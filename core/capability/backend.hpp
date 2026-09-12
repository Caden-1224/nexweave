// 后端能力接口：核心只传递 C++17 值对象，不暴露厂商 SDK、线程或设备句柄。
// 调用方拥有接口对象，适配器拥有其后台线程/设备并负责关闭；析构必须先停止回调并回收资源。
// 借用的字符串、帧、回调参数只在调用期间有效；需要异步使用时，适配器必须复制。
// 同一实例的方法默认由调用方串行调用；声明支持并发的适配器负责内部同步和线性化。
// 注册回调持有可调用对象副本，捕获引用的寿命由调用方保证；回调不得抛出或重入本对象。
// 重入的唯一例外是取消：ILlm 与 ITts 的 cancel() 允许在各自的投递回调中被调用。原因是这两个
// 能力只通过回调暴露进度，编排层在回调之外没有任何注入点，只能在回调内部打断一次正在执行的
// 同步调用；因此这两个入口必须非阻塞、不分配、不触发新回调，也不得等待那次进行中的调用返回。
// IAsr 不提供这一例外：它由调用方按帧驱动，取消可以在 feed 回调之外发起，因此要求取消不得在
// feed 回调中重入。
// 除 noexcept 方法外，分配/回调异常可向上传播；适配器不得遗留半启动的外部资源。
// 这里无隐式 deadline；阻塞设备必须在具体适配器声明有限等待和失败清理策略。
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "audio_frame.hpp"
#include "error.hpp"
#include "generation_probe.hpp"

namespace nexweave::capability {
enum class TextEventKind {
  kPartial,
  kFinal,
  kToken,
  kDone,
  kError
};
// kError 必须携带非成功错误码，其余事件 error=kNone；text 为本次增量或最终文本。
// 接收方不得保存引用，应复制需要保留的内容；事件对象自身不拥有异步线程或取消状态。
struct TextEvent {
  TextEventKind kind;
  std::string text;
  domain::Error error{};
};
using TextEventCallback = std::function<void(const TextEvent&)>;
using AudioEventCallback = std::function<void(const domain::AudioFrame&)>;

// ASR（Automatic Speech Recognition，自动语音识别）：先注册回调，再逐帧提交合法音频。
// is_last 后发送一次 final 并回到空闲；缺回调/非法帧返回结构化失败且不提交事件。
// cancel 幂等停止当前操作；线性化点后不再开始回调，适配器声明如何等待已进入的回调。
// noexcept 清理不得抛出，失败以错误码返回；对象销毁必须等待线程结束。
class IAsr {
 public:
  virtual ~IAsr() = default;
  virtual domain::OperationResult set_callback(TextEventCallback callback) = 0;
  virtual domain::OperationResult feed(const domain::AudioFrame& frame, bool is_last) = 0;
  virtual domain::OperationResult cancel() noexcept = 0;
};

// 一条检索命中：id 在同一索引版本内标识片段，text 是供回答/提示词使用的自有文本。
// score 的量纲、有效范围和排序方向由具体后端说明，不承诺跨后端可比；核心保留返回顺序，
// 不把未知分值当概率。对象无外部资源，复制拥有独立文本，修改由调用方同步。
struct RetrievedChunk {
  std::string id;
  std::string text;
  double score = 0.0;
};

// RAG（Retrieval-Augmented Generation，检索增强生成）的只读检索能力。
// top_k=0 返回成功空列表；固定索引与输入必须有确定顺序；失败无可消费值。
// 返回列表拥有文本副本，索引和外部资源由实现释放，查询引用不跨调用保留。
class IRag {
 public:
  virtual ~IRag() = default;
  virtual domain::Result<std::vector<RetrievedChunk>> retrieve(const std::string& query,
                                                               std::size_t top_k) = 0;
};

// LLM（Large Language Model，大语言模型）：非空 prompt 成功产生 token* 后一次 done。
// 空输入返回 kInvalidInput；失败用结构化错误/错误事件报告，取消后禁止提交迟到 token。
// 注册的所有权与生命周期约定同 IAsr；取消允许在 token 回调中重入（见文件头），因此实现
// 必须用原子标志或等效的无锁置位记录它，并在每个 token 交付前检查。实现不得把启动成功
// 当成异步生成已经完成。
//
// 进度观测：同时实现 IGenerationProbe 的适配器必须override本方法，把编排层挂上来的
// 探针保存下来并在 generate 期间把交付事实同步转给它；不支持观测的适配器沿用默认空
// 实现即可，因此既有适配器无需改动。探针由调用方拥有，生命周期必须覆盖“挂上到摘掉”
// 这一段；适配器只在指针非空期间报告，收到 nullptr 后不得再报告。
class ILlm {
 public:
  virtual ~ILlm() = default;
  virtual domain::OperationResult set_callback(TextEventCallback callback) = 0;
  virtual domain::OperationResult generate(const std::string& prompt) = 0;
  virtual domain::OperationResult cancel() noexcept = 0;
  virtual void set_progress_probe(IGenerationProbe* /*probe*/) noexcept {}
};

// TTS（Text-to-Speech，文本转语音）：本版 synthesize 为同步完成操作，返回前所有 PCM
// 回调已结束；成功返回表示完整合成结束，失败返回保留错误，不再有后续回调。
// 空文本 kInvalidInput；每帧遵守16kHz/单声道/S16_LE/320样本。需要异步的适配器应在外层
// 调度这个同步入口，不能在返回后偷偷继续回调；并发取消支持须在实现中明确声明。
// 取消允许在 PCM 回调中重入（见文件头），实现必须在下一帧交付前检查它；已经交付的帧
// 属于接收方，取消不撤回它们。
class ITts {
 public:
  virtual ~ITts() = default;
  virtual domain::OperationResult set_callback(AudioEventCallback callback) = 0;
  virtual domain::OperationResult synthesize(const std::string& text) = 0;
  virtual domain::OperationResult cancel() noexcept = 0;
};

// 音频源：open 建立输入轮次，read 成功返回自有完整帧，EOF 用 kAlreadyCompleted。
// 未打开读/取消 kDeviceFailure；取消后的 read 为 kCancelled。cancel 对已取消轮次幂等，
// close 幂等释放资源；必须 close/open 才开始下一轮，不能靠重复 open 清除取消状态。
// close/cancel 不得抛出；设备实现负责使阻塞 read 在声明的超时内退出、释放句柄与工作线程。
class IAudioSource {
 public:
  virtual ~IAudioSource() = default;
  virtual domain::OperationResult open() = 0;
  virtual domain::Result<domain::AudioFrame> read() = 0;
  virtual domain::OperationResult cancel() noexcept = 0;
  virtual domain::OperationResult close() noexcept = 0;
};

// 音频汇：成功 write 只消费合法完整帧，调用方返回后可复用输入；未打开/非法帧明确失败。
// cancel 停止本轮写入并丢弃尚未播放的排队数据；不能撤销设备已经播放的声音。
// close 正常结束并释放外部资源，具体实现说明刷新策略；取消后 close 不得重新播放已丢弃数据。
// Fake 的已缓存结果属于可丢弃数据，取消清空，正常 close 保留快照；重开从空轮次开始。
// 同源接口，cancel/close 幂等且不抛异常；失败由实现返回错误码并完成必要清理。
class IAudioSink {
 public:
  virtual ~IAudioSink() = default;
  virtual domain::OperationResult open() = 0;
  virtual domain::OperationResult write(const domain::AudioFrame& frame) = 0;
  virtual domain::OperationResult cancel() noexcept = 0;
  virtual domain::OperationResult close() noexcept = 0;
};
}  // namespace nexweave::capability
