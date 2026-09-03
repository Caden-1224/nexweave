// NexWeave 后端能力契约：核心运行时只依赖这些标准库值对象。
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "audio_frame.hpp"
#include "error.hpp"
namespace nexweave::capability {
enum class TextEventKind { kPartial, kFinal, kToken, kDone, kError };
struct TextEvent { TextEventKind kind; std::string text; domain::Error error{}; };
using TextEventCallback = std::function<void(const TextEvent&)>;
using AudioEventCallback = std::function<void(const domain::AudioFrame&)>;
// 回调默认在调用线程同步执行；实现若使用后台线程，必须保证对象销毁前线程已停止。
// cancel 的线性化点由实现定义，但从该点起不得再提交事件；并发调用需由实现提供同步。
// 流式 ASR：每次会话先设置回调，再按顺序提交合法音频帧；输入必须通过
// validate_audio_frame，is_last=true 后实现应发送 final 并收敛到空闲。接口不拥有帧引用，
// 可同步调用；cancel 必须幂等、无阻塞地停止回调，SDK/设备资源由实现释放。
class IAsr { public: virtual ~IAsr() = default; virtual domain::OperationResult set_callback(TextEventCallback) = 0; virtual domain::OperationResult feed(const domain::AudioFrame&, bool is_last) = 0; virtual domain::OperationResult cancel() noexcept = 0; };
struct RetrievedChunk { std::string id; std::string text; double score = 0.0; };
// 只读同步检索；top_k 为零返回空列表，相同输入必须保持确定性顺序。失败通过
// Result 错误码返回，查询字符串由调用方拥有，接口不创建跨调用资源。
class IRag { public: virtual ~IRag() = default; virtual domain::Result<std::vector<RetrievedChunk>> retrieve(const std::string&, std::size_t) = 0; };
// 流式文本生成，成功时回调 token* 后回调一次 done；空 prompt 按实现约定返回
// invalid_input。回调顺序是单次 generate 的状态不变量；cancel 后丢弃迟到 token。
class ILlm { public: virtual ~ILlm() = default; virtual domain::OperationResult set_callback(TextEventCallback) = 0; virtual domain::OperationResult generate(const std::string&) = 0; virtual domain::OperationResult cancel() noexcept = 0; };
// 流式 TTS；空文本返回 invalid_input，不产生伪造 PCM。每个回调帧必须满足 16 kHz、
// 单声道、S16_LE、20 ms 音频合同；实现负责模型句柄和取消后的失败清理。
class ITts { public: virtual ~ITts() = default; virtual domain::OperationResult set_callback(AudioEventCallback) = 0; virtual domain::OperationResult synthesize(const std::string&) = 0; virtual domain::OperationResult cancel() noexcept = 0; };
// 音频输入的设备句柄由实现拥有，read 只返回值对象，调用方无需管理资源。open/close
// 成对且可重复关闭；read 未打开或设备超时返回结构化错误，阻塞与重试策略由实现声明。
class IAudioSource { public: virtual ~IAudioSource() = default; virtual domain::OperationResult open() = 0; virtual domain::Result<domain::AudioFrame> read() = 0; virtual domain::OperationResult close() noexcept = 0; };
// 音频输出按固定 20 ms 帧消费；未 open 或非法帧必须返回结构化失败。close 负责刷新并
// 释放设备/文件资源且可重复调用；接口不暴露目标名称，线程安全由具体实现保证。
class IAudioSink { public: virtual ~IAudioSink() = default; virtual domain::OperationResult open() = 0; virtual domain::OperationResult write(const domain::AudioFrame&) = 0; virtual domain::OperationResult close() noexcept = 0; };
} // namespace nexweave::capability
