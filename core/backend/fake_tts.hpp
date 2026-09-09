// 确定性 Fake TTS（Text-to-Speech，文本转语音）适配器声明：
// 为 C++17 Mock 阶段提供“整段文本 → 若干 20 ms PCM 帧”的同步逐帧交付，
// 用于验证流式合成进度、取消收敛、回调失败与慢消费背压，不接入真实模型。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "../capability/backend.hpp"

namespace nexweave::backend {

// 每帧（20 ms）承载的 UTF-8 输入字节数；只决定帧数步长，不代表真实语速或
// 音素时长（真实语速、重采样与尾帧语义见总规格“统一音频与尾部”，由真实
// MeloTTS 适配器与后续 Session 层交付，本 Fake 不建模）。数值是 v1 设计
// 约束，变更会同时改变全部确定性输出，需同步本文件与单元测试。
inline constexpr std::size_t kFakeTtsBytesPerFrame = 16;

// 合成波形的周期（采样数）：80 样本 @16 kHz = 5 ms，对应 200 Hz 方波。
// 320 % 80 == 0，帧边界天然落在周期边界上，跨帧相位连续且每帧自洽。
inline constexpr std::size_t kFakeTtsCycleSamples = 80;

// 方波电平（正半周），负半周为相反数；±8000 约为 -12.2 dBFS，留出削波余量。
inline constexpr std::int16_t kFakeTtsSquareAmplitude = 8000;

/**
 * FakeTts（Fake Text-to-Speech，模拟文本转语音）把非空文本按固定步长切成
 * N = ⌈文本字节数 / 16⌉ 个 320 样本帧，在 synthesize 返回前逐帧同步回调，
 * 首帧必然早于函数返回，成功返回即表示完整合成结束（没有单独的 done 事件）。
 * 合成结束不等于播放结束：播放归属与节奏由外层 Session/夹具编排，本类不涉足。
 *
 * 确定性规则：第 g 个全局样本（0 起，跨帧连续）的电平由
 * (起始相位 + g) % 80 < 40 ? +8000 : -8000 决定；起始相位 = (UTF-8 字节和) % 80。
 * 同一文本每次合成得到逐采样一致的 PCM；不同文本因长度与起始相位不同而不同。
 * 本规则是“按文本增量产生确定性 PCM”的设计约束，不是对真实语音的建模；
 * 帧元数据固定为 v1 音频契约（16 kHz/单声道/S16_LE/320 样本），不产生短帧，
 * 不需要补零尾帧；尾部补零与真实重采样语义见总规格“统一音频与尾部”，
 * 由输入侧契约与真实适配器处理。
 *
 * 对象拥有回调副本与一个原子取消标志，不保存输出队列、不创建文件、线程、
 * 进程、socket 或设备句柄，也无内部 deadline。回调在 synthesize 的调用线程
 * 同步执行，每帧只以栈上局部对象存在一次，资源上界为一帧内存；慢接收方直接
 * 拖慢 synthesize，不存在隐藏缓冲，这就是“慢消费”的明确策略与有界性。
 * 事件/帧引用只在回调期间有效，接收方必须复制需要保留的内容。
 *
 * 线程与取消约定：set_callback 与 synthesize 默认由调用方串行调用，且不得
 * 与另一个线程上正在执行的 synthesize 并发；唯一例外是 cancel()，它可随时
 * 与其他线程进行中的 synthesize 并发（内部用 std::atomic 同步）。取消的
 * 线性化点是 synthesize 内“每帧交付前”的检查与“最后一帧交付后、返回前”
 * 的检查：取消在检查前到达的帧不会再交付；已通过检查或已进入回调的那一帧
 * 至多完成本次交付，不会被撤回或抢占（并发窗口，无法撤销），取消从下一帧
 * 的检查开始生效。若取消在全部帧交付后、返回成功前到达，synthesize 收敛为
 * kCancelled——此时整段音频已经交付，调用方不得把该结果当作可重试失败而
 * 整段重放。取消后（无论是否曾并发）后续 synthesize 返回 kCancelled，只有
 * 成功 set_callback 才能开启新轮次；重复取消幂等且不抛异常、无分配。
 *
 * 错误与恢复：空文本或缺回调返回 kInvalidInput 且不发任何帧；回调或分配
 * 抛出的异常原样向上传播，同时置位取消封锁本轮，避免重试产生重复交付，
 * 接收方自行清理可能已收到的帧，重新注册回调后恢复。synthesize 的校验
 * 顺序固定为“先取消、后输入”，坏文本不能掩盖轮次已失效这一事实。
 * 析构不等待任何线程（本类不拥有线程），无资源泄漏路径。
 */
class FakeTts final : public capability::ITts {
 public:
  // 无夹具构造；注册回调后即可合成。对象无外部资源，默认析构即可。
  FakeTts() = default;

  // 非空回调才成功；成功注册是显式新轮次边界，替换旧回调并清除取消状态，
  // 包括活动轮次被取消后的封锁。失败注册保留旧回调与取消状态。
  // 捕获引用必须活到替换或析构之前；不得与进行中的 synthesize 并发调用。
  domain::OperationResult set_callback(capability::AudioEventCallback callback) override;

  // 先查取消、再查回调与空文本，分别返回 kCancelled/kInvalidInput，失败不
  // 交付任何帧。成功时按确定性规则同步逐帧回调并返回 success；返回后没有
  // 在途回调或迟到帧。并发取消的精确生效窗口见类注释与 cancel()。
  domain::OperationResult synthesize(const std::string& text) override;

  // 幂等、无分配、不抛异常；与进行中的 synthesize 并发安全，线性化点见类
  // 注释。串行使用时返回后绝无后续帧；并发使用时，若取消恰在一帧通过其
  // 交付前检查之后到达，该帧仍会完成本次交付（无法撤回），此后不再有任何
  // 新帧开始交付。已交付的帧始终归接收方所有。
  domain::OperationResult cancel() noexcept override;

 private:
  capability::AudioEventCallback callback_;  // 自有函数对象；捕获引用由调用方管理。
  std::atomic<bool> cancelled_{false};  // 唯一允许并发访问的成员；其余成员串行访问。
};

}  // namespace nexweave::backend
