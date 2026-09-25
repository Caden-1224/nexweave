// RKLLM 文本适配器实现：创建真实 runtime 并交给可测试的文本核心。
//
// runtime 负责 rkllm_init / rkllm_run_async / rkllm_abort / rkllm_destroy；
// 核心负责 ILlm 队列、事件、取消和有限等待。供应方回调只把状态和文本拷贝到
// 受控回调，不在供应方线程执行慢消费者。

#include "rkllm_text_adapter.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>

#include "rkllm.h"

#include "rkllm_text_core.hpp"

namespace nexweave::backend {
namespace {

domain::OperationResult require_positive(std::chrono::milliseconds value,
                                         const char* name) {
  if (value.count() <= 0) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            std::string(name) + " 必须为正");
  }
  return domain::OperationResult::success();
}

bool file_readable(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  return input.good();
}

class RealRkllmRuntime final : public rkllm_detail::IRkllmRuntime {
 public:
  static domain::Result<std::unique_ptr<RealRkllmRuntime>> create(
      const RkllmTextConfig& config) {
    if (!file_readable(config.model_path)) {
      return domain::Result<std::unique_ptr<RealRkllmRuntime>>::failure(
          domain::ErrorCode::kInvalidInput, "RKLLM 模型文件不可读");
    }
    auto runtime = std::unique_ptr<RealRkllmRuntime>(
        new RealRkllmRuntime(config));
    const auto initialized = runtime->initialize();
    if (!initialized.ok()) {
      return domain::Result<std::unique_ptr<RealRkllmRuntime>>::failure(
          initialized.error.code, initialized.error.message);
    }
    return domain::Result<std::unique_ptr<RealRkllmRuntime>>::success(
        std::move(runtime));
  }

  ~RealRkllmRuntime() override {
    (void)close(config_.shutdown_wait);
  }

  domain::OperationResult start_async(
      const std::string& prompt,
      rkllm_detail::VendorCallback callback) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) {
        return domain::OperationResult::failure(domain::ErrorCode::kDeviceFailure,
                                                "RKLLM runtime 已关闭");
      }
      if (running_.load()) {
        return domain::OperationResult::failure(domain::ErrorCode::kBusy,
                                                "RKLLM runtime 已在运行");
      }
      callback_ = std::move(callback);
      abort_requested_ = false;
      running_.store(true);
    }

    RKLLMInput input = {};
    input.role = "user";
    input.enable_thinking = config_.enable_thinking;
    input.input_type = RKLLM_INPUT_PROMPT;
    input.prompt_input = prompt.c_str();

    RKLLMInferParam infer = {};
    infer.mode = RKLLM_INFER_GENERATE;
    infer.keep_history = 0;
    infer.max_new_tokens = config_.max_new_tokens;

    const int result =
        rkllm_run_async(handle_, &input, &infer, static_cast<void*>(this));
    if (result != 0) {
      std::lock_guard<std::mutex> lock(mutex_);
      callback_ = {};
      running_.store(false);
      cv_.notify_all();
      return domain::OperationResult::failure(
          domain::ErrorCode::kBackendFailure,
          "RKLLM run_async 启动失败，错误码 " + std::to_string(result));
    }
    return domain::OperationResult::success();
  }

  void abort() noexcept override {
    LLMHandle handle = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      handle = handle_;
    }
    if (handle != nullptr) {
      (void)rkllm_abort(handle);
    }
  }

  bool is_running() const noexcept override { return running_.load(); }

  bool wait_until_stopped(std::chrono::milliseconds timeout) override {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, timeout, [this] {
      return !running_.load() && callbacks_in_flight_.load() == 0;
    });
    return !running_.load() && callbacks_in_flight_.load() == 0;
  }

  bool close(std::chrono::milliseconds wait) noexcept override {
    abort();
    if (!wait_until_stopped(wait)) {
      return false;
    }
    LLMHandle handle = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) {
        return true;
      }
      closed_ = true;
      handle = handle_;
      handle_ = nullptr;
    }
    if (handle == nullptr) {
      return true;
    }
    const int result = rkllm_destroy(handle);
    return result == 0;
  }

 private:
  explicit RealRkllmRuntime(const RkllmTextConfig& config)
      : config_(config) {}

  domain::OperationResult initialize() {
    RKLLMParam param = rkllm_createDefaultParam();
    param.model_path = config_.model_path.c_str();
    param.max_context_len = config_.max_context_len;
    param.max_new_tokens = config_.max_new_tokens;
    param.top_k = config_.top_k;
    param.top_p = config_.top_p;
    param.temperature = config_.temperature;
    param.repeat_penalty = config_.repeat_penalty;
    param.frequency_penalty = config_.frequency_penalty;
    param.presence_penalty = config_.presence_penalty;
    param.skip_special_token = config_.skip_special_token;
    param.ignore_eos_token = config_.ignore_eos_token;
    param.is_async = true;
    param.extend_param.base_domain_id = 0;
    param.extend_param.embed_flash = 1;

    RKLLMCallback callback = {};
    callback.result_callback = &RealRkllmRuntime::on_vendor_result;
    callback.result_userdata = static_cast<void*>(this);

    LLMHandle handle = nullptr;
    const int result = rkllm_init(&handle, &param, &callback);
    if (result != 0 || handle == nullptr) {
      if (handle != nullptr) {
        (void)rkllm_destroy(handle);
      }
      return domain::OperationResult::failure(
          domain::ErrorCode::kBackendFailure,
          "RKLLM init 失败，错误码 " + std::to_string(result));
    }
    handle_ = handle;
    return domain::OperationResult::success();
  }

  static int on_vendor_result(RKLLMResult* result, void* userdata,
                              LLMCallState state) {
    auto* self = static_cast<RealRkllmRuntime*>(userdata);
    if (self == nullptr) {
      return 0;
    }
    rkllm_detail::VendorCallResult event;
    if (state == RKLLM_RUN_NORMAL) {
      event.state = rkllm_detail::VendorCallState::kNormal;
    } else if (state == RKLLM_RUN_WAITING) {
      event.state = rkllm_detail::VendorCallState::kWaiting;
    } else if (state == RKLLM_RUN_FINISH) {
      event.state = rkllm_detail::VendorCallState::kFinished;
    } else {
      event.state = rkllm_detail::VendorCallState::kError;
    }
    if (result != nullptr && result->text != nullptr &&
        event.state != rkllm_detail::VendorCallState::kFinished) {
      event.text = result->text;
    }

    rkllm_detail::VendorCallback callback;
    bool invoke_callback = false;
    const bool terminal =
        event.state == rkllm_detail::VendorCallState::kFinished ||
        event.state == rkllm_detail::VendorCallState::kError;
    {
      std::lock_guard<std::mutex> lock(self->mutex_);
      callback = self->callback_;
      if (terminal) {
        self->running_.store(false);
        self->callback_ = {};
      }
      if (self->closed_) {
        callback = {};
      }
      if (callback) {
        // 必须在同一把锁内先登记在途回调，避免 stop 等待看到 running=false
        // 与回调尚未开始的窗口后提前销毁模型句柄。
        self->callbacks_in_flight_.fetch_add(1);
        invoke_callback = true;
      }
    }
    if (terminal) {
      self->cv_.notify_all();
    }
    if (invoke_callback) {
      callback(std::move(event));
      self->callbacks_in_flight_.fetch_sub(1);
      self->cv_.notify_all();
    }
    return 0;
  }

  RkllmTextConfig config_;
  LLMHandle handle_ = nullptr;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  rkllm_detail::VendorCallback callback_;
  std::atomic<bool> running_{false};
  std::atomic<int> callbacks_in_flight_{0};
  bool abort_requested_ = false;
  bool closed_ = false;
};

}  // namespace

domain::Result<std::unique_ptr<capability::ILlm>> create_rkllm_text_adapter(
    const RkllmTextConfig& config) {
  if (config.model_path.empty()) {
    return domain::Result<std::unique_ptr<capability::ILlm>>::failure(
        domain::ErrorCode::kInvalidInput, "RKLLM 模型路径为空");
  }
  if (config.max_context_len <= 0 || config.max_new_tokens <= 0) {
    return domain::Result<std::unique_ptr<capability::ILlm>>::failure(
        domain::ErrorCode::kInvalidInput, "RKLLM 上下文/token 上限非法");
  }
  if (config.top_p < 0.0F || config.top_p > 1.0F ||
      config.temperature < 0.0F || config.repeat_penalty < 0.0F ||
      config.frequency_penalty < 0.0F || config.presence_penalty < 0.0F) {
    return domain::Result<std::unique_ptr<capability::ILlm>>::failure(
        domain::ErrorCode::kInvalidInput, "RKLLM 采样参数非法");
  }
  const auto generation_ok = require_positive(config.generation_wait, "generation_wait");
  if (!generation_ok.ok()) {
    return domain::Result<std::unique_ptr<capability::ILlm>>::failure(
        generation_ok.error.code, generation_ok.error.message);
  }
  const auto stop_ok = require_positive(config.stop_wait, "stop_wait");
  if (!stop_ok.ok()) {
    return domain::Result<std::unique_ptr<capability::ILlm>>::failure(
        stop_ok.error.code, stop_ok.error.message);
  }
  const auto shutdown_ok = require_positive(config.shutdown_wait, "shutdown_wait");
  if (!shutdown_ok.ok()) {
    return domain::Result<std::unique_ptr<capability::ILlm>>::failure(
        shutdown_ok.error.code, shutdown_ok.error.message);
  }

  auto runtime = RealRkllmRuntime::create(config);
  if (!runtime.ok()) {
    return domain::Result<std::unique_ptr<capability::ILlm>>::failure(
        runtime.error.code, runtime.error.message);
  }

  rkllm_detail::RkllmStreamConfig stream_config;
  stream_config.max_prompt_bytes = config.max_prompt_bytes;
  stream_config.max_output_bytes = config.max_output_bytes;
  stream_config.max_pending_tokens = config.max_pending_tokens;
  stream_config.generation_wait = config.generation_wait;
  stream_config.stop_wait = config.stop_wait;

  auto core = RkllmTextCore::create(std::move(*runtime.value), stream_config);
  if (!core.ok()) {
    return domain::Result<std::unique_ptr<capability::ILlm>>::failure(
        core.error.code, core.error.message);
  }
  return domain::Result<std::unique_ptr<capability::ILlm>>::success(
      std::move(*core.value));
}

}  // namespace nexweave::backend
