// NDJSON（Newline Delimited JSON，换行分隔 JSON）的增量解帧器。
//
// 它解决什么问题
// --------------
// 传输层交给我们的是**字节流**，不是消息：一次读取可能返回半帧、数帧或零字节，发送方的一次
// 写入边界不会保留到接收方。因此“把字节流切成一条条完整 JSON 文本”必须是独立的一步，而且
// 必须能在任意切分下给出同一结果——否则后面所有协议校验都会在残缺或错位的输入上做判断。
//
// 边界规则
// --------
//   - 一帧 = 一个换行符（'\n'）之前的全部字节；换行符本身不属于帧内容。
//   - 行尾若为 '\r'（Windows 风格 '\r\n'），该 '\r' 被剥离；其余位置的 '\r' 属于帧内容。
//   - 完全为空的行、以及仅含 '\r' 的行是空行，不产生帧。只含空格的行**不是**空行：它会被
//     当作一帧交付，由上层给出结构化的非法输入结论，而不是在这一层被静默丢弃。
//   - 没有换行的尾部字节是**半包**：它们留在缓冲里等待后续输入，既不算帧也不丢失。
//
// 有界性
// ------
// 不可信客户端可以一直不发换行符，所以缓冲必须有上限。上限约束的是**单帧内容长度**（不含
// 换行符），取值含边界：恰好等于上限的帧合法，多一个字节即判定超限。一旦判定超限就立刻
// 放弃已收字节并进入“丢弃到下一个换行符”的状态，因此缓冲永远不会超过上限，也不会随着连接
// 存活时间增长。丢弃必须止于换行符：如果把超长帧的尾部当成新帧的开头，一行垃圾就会变成一串
// 伪造的请求。
//
// 职责边界
// --------
// 本类只做“按行切分”，不解析 JSON、不校验字段、不做业务判断，也不决定超限之后连接是否继续
// 可用——那是请求入口的策略。它不创建线程、文件、socket 或设备，不读时钟，不做 I/O；除
// 标准库分配失败外不抛异常。
//
// 线程安全
// --------
// 本类**不是**线程安全的：单个实例的全部方法必须由同一个线程串行调用。它没有内部同步，
// 因为它的全部状态（半包缓冲、丢弃状态、计数）都只在字节流推进的线程上变化。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nexweave::gateway {

// 单帧默认上限（字节，不含换行符）。它覆盖控制请求与 JSON 数据事件的正常体积，同时保证
// 单个客户端不能仅凭“不发换行符”就要求服务端预留大块内存。
inline constexpr std::size_t kDefaultMaxFrameBytes = 64U * 1024U;

// 一次 feed 的结论。超限会作为显式结论返回，而不是抛异常：超长帧是不可信输入下的正常
// 分支，调用方需要据此决定是否继续使用这条连接。
enum class FrameVerdict : std::uint8_t {
  // 本批输入内的完整帧已全部产出（产出帧数可以为零）。
  kOk = 0,
  // 本批输入里出现了超过上限的帧：该帧被丢弃，解帧器已在下一个换行符处重新同步，
  // 同一批次里位于其后的完整帧仍然照常产出。
  kOversized = 1
};

class NdjsonFramer final {
 public:
  // max_frame_bytes：单帧内容最大字节数（不含换行符）。取值 0 会归一为 1，因为“上限为 0”
  // 在语义上等于“拒绝一切帧”，而归一为 1 保留了“上限必须为正”这条不变量；调用方若要拒绝
  // 全部输入，应在配置校验阶段明确拒绝，而不是依赖这里的退化行为。
  explicit NdjsonFramer(std::size_t max_frame_bytes = kDefaultMaxFrameBytes);

  // 喂入任意切分的字节块，把本次产出的完整帧**追加**到 frames（不覆盖已有内容）。
  // 追加而不是覆盖：一次输入可能产出多帧，调用方通常希望在同一个容器里按顺序收集。
  // 返回 kOversized 表示本次输入中出现了超限帧；解帧器保持可用，是否继续使用这条连接由
  // 调用方决定。失败不抛出，frames 中已产出的帧在超限发生时仍然有效。
  FrameVerdict feed(std::string_view chunk, std::vector<std::string>& frames);

  // 尚未成帧的尾部字节数。丢弃模式吞掉的字节不计入：它们已经确定不属于任何帧。
  std::size_t partial_bytes() const noexcept;

  // 自复位以来判定超限的帧数，用于运行证据与连接级统计。
  std::uint64_t oversized_frames() const noexcept;

  // 回到构造状态：清空半包缓冲、丢弃状态与超限计数。它不释放容量，不涉及外部资源。
  void reset() noexcept;

 private:
  // 把一行（已去掉换行符）按边界规则交付：剥离行尾 '\r'，空行不产生帧。
  static void emit_line(std::string& line, std::vector<std::string>& frames);

  const std::size_t max_frame_bytes_;
  // 当前未成帧的尾部字节。长度永远不超过 max_frame_bytes_。
  std::string buffer_;
  // 是否处于“丢弃到下一个换行符”的状态。它与 buffer_ 互斥：丢弃期间不保留任何字节。
  bool discarding_ = false;
  std::uint64_t oversized_frames_ = 0;
};

}  // namespace nexweave::gateway
