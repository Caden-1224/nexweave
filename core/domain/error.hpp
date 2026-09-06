// 统一错误与结果值对象，适用于同步返回及协议适配；不拥有文件、线程、socket 或设备。
// C++17。只读值可并发共享，修改须由调用方同步；文本/载荷复制的分配异常向上传播，
// 部分构造由标准库清理。枚举数值是 v1 线协议的一部分，已有值不得重排或复用。
#pragma once
#include <optional>
#include <string>
#include <utility>

namespace nexweave::domain {
enum class ErrorCode {
  kNone = 0,
  kInvalidInput = 1,
  kMissingField = 2,
  kAlreadyCompleted = 3,
  kCancelled = 4,
  kTimeout = 5,
  kBackendFailure = 6,
  kDeviceFailure = 7
};

// 接受且仅接受上面定义的错误码；未知强制转换值返回 false，无分配、无状态修改。
bool is_valid_error_code(ErrorCode code) noexcept;
enum class ErrorDisposition {
  kSuccess,
  kRetryable,
  kCancelled,
  kPermanent
};

// code 用于机器决策，message 仅用于诊断。超时/后端/设备失败允许调用方决定重试，
// 不代表重试一定成功或可跨代重放；取消必须停止当前代，未知错误按不可恢复处理。
struct Error {
  ErrorCode code = ErrorCode::kNone;
  std::string message;
  bool ok() const noexcept {
    return code == ErrorCode::kNone;
  }
  bool cancelled() const noexcept {
    return code == ErrorCode::kCancelled;
  }
  ErrorDisposition disposition() const noexcept;
  bool retryable() const noexcept {
    return disposition() == ErrorDisposition::kRetryable;
  }
};

// 无载荷操作结果。success 表示操作契约已满足；failure 禁止把 kNone/未知码伪装成失败，
// 两者归一为 kInvalidInput。工厂不等待或清理外部资源，外部生命周期由操作实现负责。
struct OperationResult {
  Error error{};
  bool ok() const noexcept {
    return error.ok();
  }
  static OperationResult success() {
    return {};
  }
  static OperationResult failure(ErrorCode code, std::string message = {});
};

// 带值结果。默认对象不成功；success 移入自有 T，failure 不含值。T 必须可移动构造；
// T 的复制/移动异常向上传播。公开字段修改后仍须检查 ok()，禁止消费失败结果的 value。
// optional 只表达值的有无，不拥有外部取消状态；T 自己管理其资源并遵守析构约定。
template <typename T>
struct Result {
  std::optional<T> value;
  Error error{};
  bool ok() const noexcept {
    return error.ok() && value.has_value();
  }
  static Result success(T result) {
    Result output;
    output.value.emplace(std::move(result));
    return output;
  }
  static Result failure(ErrorCode code, std::string message = {}) {
    Result output;
    output.error = OperationResult::failure(code, std::move(message)).error;
    return output;
  }
};
}  // namespace nexweave::domain
