// 任务、请求、会话与生成代际的 v1 契约，纯内存值，无线程、设备、文件或时钟。
// 标识由调用方生成并保证作用域内唯一；校验只判断格式，不提供随机数或唯一性保证。
// 字符串按 ASCII 字节计长，最大128字节；不分配、不保留引用，除空 session_id 外，空/非法输入返回
// false。 只读校验可并发；Generation 的读写须由拥有它的 Session 同步，禁止无锁并发修改。
#pragma once
#include <cstdint>
#include <string_view>

namespace nexweave::domain {
// work_id 关联任务完整生命周期，格式 w- 后跟至少一位十进制数字，总长<=128。
bool is_valid_work_id(std::string_view value) noexcept;
// request_id 在接收方任务上下文内标识一次操作；1～128个 ASCII 字母、数字或 -_.。
// 重试复用同一个标识，新的逻辑操作必须使用新标识，不能依赖格式校验去重。
bool is_valid_request_id(std::string_view value) noexcept;
// session_id 关联多轮会话；允许空串表示未绑定，其余格式同 request_id。
bool is_valid_session_id(std::string_view value) noexcept;

// generation 隔离取消前后的结果，不等于协议版本。初始0，每次新请求/有效取消推进。
// 到 uint64 上限时饱和而不回绕；advance 返回旧值表示耗尽，拥有者必须停止开启新代。
// accepts 只比较数值相等，不证明事件类型/请求归属有效；不拥有外部清理或缓存。
// 所有方法 noexcept，无分配；状态只能单调递增或保持上限，销毁不需要资源清理。
class Generation {
 public:
  std::uint64_t current() const noexcept {
    return value_;
  }
  std::uint64_t advance() noexcept;
  bool accepts(std::uint64_t generation) const noexcept {
    return generation == value_;
  }

 private:
  std::uint64_t value_ = 0;
};
}  // namespace nexweave::domain
