// RKLLM 文本适配器核心实现。供应商类型不在本文件出现；这里只做队列、取消、
// 事件投递和生命周期收敛。

#include "rkllm_text_core.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

namespace nexweave::backend {
namespace {

domain::OperationResult validate_stream_config(
    const rkllm_detail::RkllmStreamConfig& config) {
  if (config.max_prompt_bytes == 0 || config.max_output_bytes == 0 ||
      config.max_pending_tokens == 0) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "RKLLM 文本容量不能为零");
  }
  if (config.generation_wait.count() <= 0 || config.stop_wait.count() <= 0) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "RKLLM 文本等待时间必须为正");
  }
  return domain::OperationResult::success();
}

}  // namespace

struct RkllmTextCore::Impl {
  struct Item {
    std::uint64_t round = 0;
    rkllm_detail::VendorCallResult result;
  };

  Impl(std::unique_ptr<rkllm_detail::IRkllmRuntime> runtime,
       rkllm_detail::RkllmStreamConfig config)
      : runtime_(std::move(runtime)), config_(config) {}

  ~Impl() {
    cancelled_.store(true);
    cv_.notify_all();
    if (runtime_ != nullptr) {
      runtime_->abort();
      (void)runtime_->close(config_.stop_wait);
    }
  }

  bool should_accept_vendor_item(std::uint64_t round) {
    return generation_active_ && round == active_round_;
  }

  void enqueue_vendor_item(std::uint64_t round,
                           rkllm_detail::VendorCallResult result) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!should_accept_vendor_item(round)) {
        return;
      }
      if (queue_.size() >= config_.max_pending_tokens) {
        queue_overflow_ = true;
      } else {
        try {
          queue_.push_back(Item{round, std::move(result)});
        } catch (...) {
          // 分配失败不能在供应方 C 回调里继续传播；转为有界失败，由泵收敛。
          queue_overflow_ = true;
        }
      }
    }
    cv_.notify_all();
  }

  domain::OperationResult emit_token(const std::string& text,
                                     const capability::TextEventCallback& callback,
                                     capability::IGenerationProbe* probe) {
    try {
      callback(capability::TextEvent{capability::TextEventKind::kToken, text, {}});
    } catch (...) {
      if (probe != nullptr) {
        probe->on_generation_failed("RKLLM token 回调抛出异常");
      }
      return domain::OperationResult::failure(domain::ErrorCode::kBackendFailure,
                                              "RKLLM token 回调抛出异常");
    }
    if (probe != nullptr) {
      probe->on_token_delivered(text);
    }
    return domain::OperationResult::success();
  }

  domain::OperationResult emit_done(const capability::TextEventCallback& callback,
                                    capability::IGenerationProbe* probe) {
    try {
      callback(capability::TextEvent{capability::TextEventKind::kDone, "", {}});
    } catch (...) {
      if (probe != nullptr) {
        probe->on_generation_failed("RKLLM done 回调抛出异常");
      }
      return domain::OperationResult::failure(domain::ErrorCode::kBackendFailure,
                                              "RKLLM done 回调抛出异常");
    }
    if (probe != nullptr) {
      probe->on_generation_completed();
    }
    return domain::OperationResult::success();
  }

  domain::OperationResult emit_error_event(const domain::Error& error,
                                           const capability::TextEventCallback& callback,
                                           capability::IGenerationProbe* probe) {
    try {
      callback(capability::TextEvent{capability::TextEventKind::kError, "", error});
    } catch (...) {
      if (probe != nullptr) {
        probe->on_generation_failed("RKLLM error 回调抛出异常");
      }
      return domain::OperationResult::failure(domain::ErrorCode::kBackendFailure,
                                              "RKLLM error 回调抛出异常");
    }
    if (probe != nullptr) {
      probe->on_generation_failed(error.message);
    }
    return domain::OperationResult::failure(error.code, error.message);
  }

  domain::OperationResult finish_round() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      generation_active_ = false;
      active_round_ = 0;
      queue_.clear();
    }
    // 无论本轮是否成功，都等供应方停止并完成回调回收；这样下一轮不会与
    // 上一轮仍在退出的 worker/回调重叠。先 abort 是尽力而为，等待才是线性化点。
    if (runtime_->is_running()) {
      runtime_->abort();
    }
    if (!runtime_->wait_until_stopped(config_.stop_wait)) {
      std::lock_guard<std::mutex> lock(mutex_);
      unusable_ = true;
      return domain::OperationResult::failure(
          domain::ErrorCode::kTimeout, "RKLLM 停止等待超时，runtime 已隔离");
    }
    return domain::OperationResult::success();
  }

  domain::OperationResult pump(std::uint64_t round,
                               const capability::TextEventCallback& callback,
                               capability::IGenerationProbe* probe) {
    std::string pending_waiting;
    std::size_t output_bytes = 0;
    bool failed_reported = false;

    while (true) {
      Item item;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto ready = [this, round] {
          return cancelled_.load() || queue_overflow_ || stop_delivery_ ||
                 (!queue_.empty() && queue_.front().round == round);
        };
        if (!cv_.wait_for(lock, config_.generation_wait, ready)) {
          if (probe != nullptr) {
            probe->on_generation_failed("RKLLM 生成等待超时");
          }
          return domain::OperationResult::failure(domain::ErrorCode::kTimeout,
                                                  "RKLLM 生成等待超时");
        }
        if (cancelled_.load()) {
          if (probe != nullptr) {
            probe->on_generation_failed("RKLLM 生成被取消");
          }
          return domain::OperationResult::failure(domain::ErrorCode::kCancelled,
                                                  "RKLLM 生成被取消");
        }
        if (queue_overflow_) {
          if (probe != nullptr) {
            probe->on_generation_failed("RKLLM 待投递队列已满");
          }
          return domain::OperationResult::failure(
              domain::ErrorCode::kBackendFailure, "RKLLM 待投递队列已满");
        }
        if (stop_delivery_) {
          if (probe != nullptr) {
            probe->on_generation_failed(stop_reason_);
          }
          return domain::OperationResult::failure(domain::ErrorCode::kBackendFailure,
                                                  stop_reason_);
        }
        if (queue_.empty()) {
          continue;
        }
        item = std::move(queue_.front());
        queue_.pop_front();
      }

      if (item.round != round) {
        continue;
      }
      switch (item.result.state) {
        case rkllm_detail::VendorCallState::kWaiting:
          pending_waiting += item.result.text;
          break;
        case rkllm_detail::VendorCallState::kNormal: {
          std::string text = pending_waiting + item.result.text;
          pending_waiting.clear();
          if (text.empty()) {
            break;
          }
          output_bytes += text.size();
          if (output_bytes > config_.max_output_bytes) {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_delivery_ = true;
            stop_reason_ = "RKLLM 输出超过容量上限";
            cv_.notify_all();
            break;
          }
          const auto emitted = emit_token(text, callback, probe);
          if (!emitted.ok()) {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_delivery_ = true;
            stop_reason_ = emitted.error.message;
            cv_.notify_all();
            return emitted;
          }
          break;
        }
        case rkllm_detail::VendorCallState::kFinished: {
          if (!pending_waiting.empty()) {
            const auto emitted = emit_token(pending_waiting, callback, probe);
            if (!emitted.ok()) {
              return emitted;
            }
            pending_waiting.clear();
          }
          return emit_done(callback, probe);
        }
        case rkllm_detail::VendorCallState::kError: {
          if (failed_reported) {
            return domain::OperationResult::failure(domain::ErrorCode::kBackendFailure,
                                                    "RKLLM 生成错误");
          }
          failed_reported = true;
          const std::string message =
              item.result.text.empty() ? "RKLLM 生成错误" : item.result.text;
          return emit_error_event(
              domain::Error{domain::ErrorCode::kBackendFailure, message}, callback, probe);
        }
      }
    }
  }

  std::unique_ptr<rkllm_detail::IRkllmRuntime> runtime_;
  rkllm_detail::RkllmStreamConfig config_;

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Item> queue_;
  capability::TextEventCallback callback_;
  capability::IGenerationProbe* probe_ = nullptr;
  std::atomic<bool> cancelled_{false};
  bool unusable_ = false;
  bool generation_active_ = false;
  std::uint64_t generation_ = 0;
  std::uint64_t active_round_ = 0;
  bool queue_overflow_ = false;
  bool stop_delivery_ = false;
  std::string stop_reason_;
};

RkllmTextCore::RkllmTextCore(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

RkllmTextCore::~RkllmTextCore() = default;

domain::Result<std::unique_ptr<RkllmTextCore>> RkllmTextCore::create(
    std::unique_ptr<rkllm_detail::IRkllmRuntime> runtime,
    rkllm_detail::RkllmStreamConfig config) {
  if (runtime == nullptr) {
    return domain::Result<std::unique_ptr<RkllmTextCore>>::failure(
        domain::ErrorCode::kInvalidInput, "RKLLM runtime 为空");
  }
  const auto config_ok = validate_stream_config(config);
  if (!config_ok.ok()) {
    return domain::Result<std::unique_ptr<RkllmTextCore>>::failure(
        config_ok.error.code, config_ok.error.message);
  }
  return domain::Result<std::unique_ptr<RkllmTextCore>>::success(
      std::unique_ptr<RkllmTextCore>(
          new RkllmTextCore(std::make_unique<Impl>(std::move(runtime), config))));
}

domain::OperationResult RkllmTextCore::set_callback(
    capability::TextEventCallback callback) {
  if (!callback) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                            "RKLLM 文本回调为空");
  }
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  if (impl_->generation_active_) {
    return domain::OperationResult::failure(domain::ErrorCode::kBusy,
                                            "RKLLM 文本生成仍在进行");
  }
  if (impl_->unusable_) {
    return domain::OperationResult::failure(domain::ErrorCode::kDeviceFailure,
                                            "RKLLM runtime 已不可用");
  }
  impl_->callback_ = std::move(callback);
  impl_->queue_.clear();
  impl_->queue_overflow_ = false;
  impl_->stop_delivery_ = false;
  impl_->stop_reason_.clear();
  impl_->cancelled_.store(false);
  return domain::OperationResult::success();
}

domain::OperationResult RkllmTextCore::generate(const std::string& prompt) {
  if (impl_->cancelled_.load()) {
    return domain::OperationResult::failure(domain::ErrorCode::kCancelled);
  }

  capability::TextEventCallback callback;
  capability::IGenerationProbe* probe = nullptr;
  std::uint64_t round = 0;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    if (impl_->generation_active_) {
      return domain::OperationResult::failure(domain::ErrorCode::kBusy,
                                              "RKLLM 文本生成仍在进行");
    }
    if (impl_->unusable_) {
      return domain::OperationResult::failure(domain::ErrorCode::kDeviceFailure,
                                              "RKLLM runtime 已不可用");
    }
    if (!impl_->callback_) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                              "RKLLM 文本回调未注册");
    }
    if (prompt.empty()) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                              "RKLLM prompt 为空");
    }
    if (prompt.size() > impl_->config_.max_prompt_bytes) {
      return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput,
                                              "RKLLM prompt 超过容量上限");
    }
    callback = impl_->callback_;
    probe = impl_->probe_;
    impl_->generation_active_ = true;
    round = ++impl_->generation_;
    impl_->active_round_ = round;
    impl_->queue_.clear();
    impl_->queue_overflow_ = false;
    impl_->stop_delivery_ = false;
    impl_->stop_reason_.clear();
  }

  auto vendor_callback = [this, round](rkllm_detail::VendorCallResult result) {
    impl_->enqueue_vendor_item(round, std::move(result));
  };
  const auto started =
      impl_->runtime_->start_async(prompt, std::move(vendor_callback));
  if (!started.ok()) {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->generation_active_ = false;
    impl_->active_round_ = 0;
    if (probe != nullptr) {
      probe->on_generation_failed(started.error.message);
    }
    return started;
  }

  if (probe != nullptr) {
    probe->on_generation_started();
  }

  domain::OperationResult pumped;
  try {
    pumped = impl_->pump(round, callback, probe);
  } catch (...) {
    // 泵内分配异常或用户回调异常若向上传播，仍必须先收拢供应方与队列。
    (void)impl_->finish_round();
    throw;
  }
  const auto stopped = impl_->finish_round();
  if (!stopped.ok()) {
    return stopped;
  }
  return pumped;
}

domain::OperationResult RkllmTextCore::cancel() noexcept {
  impl_->cancelled_.store(true);
  impl_->cv_.notify_all();
  if (impl_->runtime_ != nullptr && impl_->runtime_->is_running()) {
    impl_->runtime_->abort();
  }
  return domain::OperationResult::success();
}

void RkllmTextCore::set_progress_probe(
    capability::IGenerationProbe* probe) noexcept {
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  impl_->probe_ = probe;
}

}  // namespace nexweave::backend
