// RK3576 板端真实 RKLLM 文本适配器验收夹具。
//
// 通过公共 ILlm 驱动适配器：固定 prompt 收集 token/done/error，覆盖正常生成、
// 生成中取消、回调异常、待投递队列满、超时和无效模型路径。模型路径由命令行注入。
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "rkllm_text_adapter.hpp"

using namespace nexweave;

namespace {

int g_failures = 0;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      ++g_failures;                                                             \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " << #condition \
                << std::endl;                                                   \
    }                                                                           \
  } while (false)

constexpr char kSmokePrompt[] = "你好，请用一句话介绍你自己。";
constexpr char kLongPrompt[] = "请从一数到二十，用逗号分隔。";

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct EventLog {
  std::vector<capability::TextEvent> events;
  std::int64_t first_token_ms = -1;
  std::int64_t done_ms = -1;
};

bool HasDone(const EventLog& log) {
  for (const auto& event : log.events) {
    if (event.kind == capability::TextEventKind::kDone) {
      return true;
    }
  }
  return false;
}

std::string FirstChars(const std::string& text, std::size_t count) {
  return text.size() <= count ? text : text.substr(0, count) + "...";
}

bool RunSuccessOnce(capability::ILlm& llm, const std::string& prompt,
                    bool print) {
  EventLog log;
  const auto started = now_ms();
  const auto registered = llm.set_callback([&](const capability::TextEvent& event) {
    log.events.push_back(event);
    if (event.kind == capability::TextEventKind::kToken &&
        log.first_token_ms < 0) {
      log.first_token_ms = now_ms();
    }
    if (event.kind == capability::TextEventKind::kDone) {
      log.done_ms = now_ms();
    }
  });
  if (!registered.ok()) {
    std::cerr << "set_callback 失败 code=" << static_cast<int>(registered.error.code)
              << std::endl;
    return false;
  }
  const auto generated = llm.generate(prompt);
  if (!generated.ok()) {
    std::cerr << "generate 失败 code=" << static_cast<int>(generated.error.code)
              << " message=" << generated.error.message << std::endl;
    return false;
  }

  std::size_t tokens = 0;
  std::size_t errors = 0;
  std::string full_text;
  for (const auto& event : log.events) {
    if (event.kind == capability::TextEventKind::kToken) {
      ++tokens;
      full_text += event.text;
    } else if (event.kind == capability::TextEventKind::kError) {
      ++errors;
    }
  }
  CHECK(tokens >= 1);
  CHECK(errors == 0);
  CHECK(HasDone(log));
  CHECK(log.first_token_ms >= started);
  CHECK(log.done_ms >= log.first_token_ms);
  CHECK(!full_text.empty());
  if (print) {
    std::cout << "  tokens=" << tokens
              << " first_token_ms=" << (log.first_token_ms - started)
              << " total_ms=" << (log.done_ms - started)
              << " text=\"" << FirstChars(full_text, 80) << "\"" << std::endl;
  }
  return true;
}

std::unique_ptr<capability::ILlm> CreateAdapter(
    const backend::RkllmTextConfig& config) {
  auto created = backend::create_rkllm_text_adapter(config);
  if (!created.ok()) {
    std::cerr << "创建 RKLLM 适配器失败 code="
              << static_cast<int>(created.error.code)
              << " message=" << created.error.message << std::endl;
    return nullptr;
  }
  return std::move(*created.value);
}

void RunSuccessRepeat(const backend::RkllmTextConfig& config, int repeat) {
  auto llm = CreateAdapter(config);
  if (!llm) {
    ++g_failures;
    return;
  }
  int success = 0;
  for (int index = 0; index < repeat; ++index) {
    const int before = g_failures;
    if (RunSuccessOnce(*llm, kSmokePrompt, true) && g_failures == before) {
      ++success;
    }
  }
  std::cout << "success_repeat=" << success << "/" << repeat << std::endl;
  CHECK(success == repeat);
}

void RunCancelCase(const backend::RkllmTextConfig& config) {
  auto llm = CreateAdapter(config);
  if (!llm) {
    ++g_failures;
    return;
  }
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<capability::TextEvent> events;
  bool first_token = false;
  CHECK(llm->set_callback([&](const capability::TextEvent& event) {
          std::lock_guard<std::mutex> lock(mutex);
          events.push_back(event);
          if (event.kind == capability::TextEventKind::kToken) {
            first_token = true;
            cv.notify_all();
          }
        }).ok());

  std::atomic<int> result_code{static_cast<int>(domain::ErrorCode::kNone)};
  std::thread worker([&] {
    const auto generated = llm->generate(kSmokePrompt);
    result_code.store(static_cast<int>(generated.error.code));
  });

  {
    std::unique_lock<std::mutex> lock(mutex);
    const bool started = cv.wait_for(lock, std::chrono::seconds(30),
                                     [&] { return first_token; });
    CHECK(started);
  }
  CHECK(llm->cancel().ok());
  worker.join();
  CHECK(result_code.load() == static_cast<int>(domain::ErrorCode::kCancelled));
  {
    std::lock_guard<std::mutex> lock(mutex);
    bool has_done = false;
    for (const auto& event : events) {
      has_done = has_done || event.kind == capability::TextEventKind::kDone;
    }
    CHECK(!has_done);
    std::cout << "  cancel_events=" << events.size() << " done=false"
              << std::endl;
  }

  CHECK(llm->set_callback([](const capability::TextEvent&) {}).ok());
  CHECK(RunSuccessOnce(*llm, kSmokePrompt, false));
}

void RunCallbackFailureCase(const backend::RkllmTextConfig& config) {
  auto llm = CreateAdapter(config);
  if (!llm) {
    ++g_failures;
    return;
  }
  CHECK(llm->set_callback([](const capability::TextEvent&) {
          throw std::runtime_error("硬件测试注入回调异常");
        }).ok());
  const auto generated = llm->generate(kSmokePrompt);
  CHECK(!generated.ok());
  CHECK(generated.error.code == domain::ErrorCode::kBackendFailure);

  CHECK(llm->set_callback([](const capability::TextEvent&) {}).ok());
  CHECK(RunSuccessOnce(*llm, kSmokePrompt, false));
}

void RunQueueOverflowCase(const backend::RkllmTextConfig& config) {
  backend::RkllmTextConfig overflow = config;
  overflow.max_pending_tokens = 1;
  overflow.max_new_tokens = 32;
  overflow.generation_wait = std::chrono::seconds(60);
  auto llm = CreateAdapter(overflow);
  if (!llm) {
    ++g_failures;
    return;
  }
  std::atomic<int> tokens{0};
  CHECK(llm->set_callback([&](const capability::TextEvent& event) {
          if (event.kind == capability::TextEventKind::kToken) {
            tokens.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
          }
        }).ok());
  const auto started = now_ms();
  const auto generated = llm->generate(kLongPrompt);
  const auto elapsed = now_ms() - started;
  CHECK(!generated.ok());
  CHECK(generated.error.code == domain::ErrorCode::kBackendFailure ||
        generated.error.code == domain::ErrorCode::kTimeout);
  CHECK(elapsed < 30000);
  std::cout << "  queue_overflow_code="
            << static_cast<int>(generated.error.code)
            << " elapsed_ms=" << elapsed << " delivered_tokens="
            << tokens.load() << std::endl;
  CHECK(llm->set_callback([](const capability::TextEvent&) {}).ok());
  CHECK(RunSuccessOnce(*llm, kSmokePrompt, false));
}

void RunInvalidCase(const backend::RkllmTextConfig& config) {
  backend::RkllmTextConfig invalid = config;
  invalid.model_path += ".missing";
  auto created = backend::create_rkllm_text_adapter(invalid);
  CHECK(!created.ok());
  CHECK(created.error.code == domain::ErrorCode::kInvalidInput ||
        created.error.code == domain::ErrorCode::kBackendFailure);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "用法: " << argv[0]
              << " <model.rkllm> <mode> [repeat]\n"
                 "mode: success|long|cancel|callback-failure|queue-overflow|invalid"
              << std::endl;
    return 2;
  }
  const std::string mode = argv[2];
  const int repeat = argc >= 4 ? std::max(1, std::atoi(argv[3])) : 1;
  backend::RkllmTextConfig config;
  config.model_path = argv[1];
  config.max_context_len = 4096;
  config.max_new_tokens = 64;
  config.top_k = 1;
  config.top_p = 0.95F;
  config.temperature = 0.8F;
  config.repeat_penalty = 1.1F;
  config.skip_special_token = true;
  config.enable_thinking = false;

  if (mode == "success") {
    RunSuccessRepeat(config, repeat);
  } else if (mode == "long") {
    auto llm = CreateAdapter(config);
    if (!llm) {
      ++g_failures;
    } else {
      CHECK(RunSuccessOnce(*llm, kLongPrompt, true));
    }
  } else if (mode == "cancel") {
    RunCancelCase(config);
  } else if (mode == "callback-failure") {
    RunCallbackFailureCase(config);
  } else if (mode == "queue-overflow") {
    RunQueueOverflowCase(config);
  } else if (mode == "invalid") {
    RunInvalidCase(config);
  } else {
    std::cerr << "未知模式: " << mode << std::endl;
    return 2;
  }

  if (g_failures == 0) {
    std::cout << "rkllm_hardware_test 通过" << std::endl;
    return 0;
  }
  std::cerr << "rkllm_hardware_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
