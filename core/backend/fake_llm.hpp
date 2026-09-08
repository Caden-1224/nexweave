#pragma once
#include <string>
#include <vector>
#include "../capability/backend.hpp"
namespace nexweave::backend {
// 确定性 Fake LLM：无线程或模型句柄；非空 prompt 同步发布 token 后发布 done。
class FakeLlm final : public capability::ILlm {
 public:
  explicit FakeLlm(std::vector<std::string> tokens);
  domain::OperationResult set_callback(capability::TextEventCallback callback) override;
  domain::OperationResult generate(const std::string& prompt) override;
  domain::OperationResult cancel() noexcept override;
 private:
  std::vector<std::string> tokens_;
  capability::TextEventCallback callback_;
  bool cancelled_ = false;
};
}  // namespace nexweave::backend
