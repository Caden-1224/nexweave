// RKLLM 文本推理适配器的公共构造入口。
//
// Session 只看到 ILlm：正常 token 流、一次 done、错误事件、取消与下一轮边界。
// RKLLM 句柄、供应方回调线程和库路径都留在适配器实现内。模型路径由调用方注入，
// 不依赖当前工作目录，也不把厂商类型泄漏到核心。
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "../../capability/backend.hpp"

namespace nexweave::backend {

// RKLLM 文本适配器配置。模型参数、采样参数和缓冲区/等待上限都必须显式给出正值；
// factory 会先校验再加载模型。
struct RkllmTextConfig {
  // .rkllm 模型文件路径。
  std::string model_path;
  // 模型上下文窗口与单轮新增 token 上限。
  int max_context_len = 4096;
  int max_new_tokens = 128;
  // 采样参数。top_k <= 0 表示使用供应方默认。
  int top_k = 1;
  float top_p = 0.95F;
  float temperature = 0.8F;
  float repeat_penalty = 1.1F;
  float frequency_penalty = 0.0F;
  float presence_penalty = 0.0F;
  // 是否跳过特殊 token；是否忽略 EOS；是否开启 Qwen thinking。
  bool skip_special_token = true;
  bool ignore_eos_token = false;
  bool enable_thinking = false;
  // 适配器队列与输入/输出边界。
  std::size_t max_prompt_bytes = 16U * 1024U;
  std::size_t max_output_bytes = 1024U * 1024U;
  std::size_t max_pending_tokens = 256U;
  // generate 最长等待、取消/失败后等待供应方停止的时间。
  std::chrono::milliseconds generation_wait{120000};
  std::chrono::milliseconds stop_wait{5000};
  // 析构时等待供应方停止并释放句柄的最长时间。
  std::chrono::milliseconds shutdown_wait{5000};
};

// 创建真实 RKLLM 文本适配器。模型/运行库失败返回结构化错误；成功后仍需
// set_callback 才能 generate。返回值以 ILlm 暴露，同时实现 IGenerationProbe，
// 因此 Session 可以在运行时挂接生成进度观察。
domain::Result<std::unique_ptr<capability::ILlm>> create_rkllm_text_adapter(
    const RkllmTextConfig& config);

}  // namespace nexweave::backend
