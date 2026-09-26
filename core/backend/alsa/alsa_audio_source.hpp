// ALSA 音频输入适配器：把真实采集后端转换为统一 capability::IAudioSource。
//
// 职责
// ----
// 本类只做三件事：管理一次输入轮次的 open/close、把设备原生 PCM 显式转换为固定的
// 16 kHz/单声道/S16_LE/320 样本帧、在可诊断的预算内处理超时、取消、短读、溢出和
// 设备恢复。它不判断语音活动、不切分语音段、不做回声消除，也不拥有全双工设备；
// 那些职责属于后续的音频前端拥有者。
//
// 设备边界
// --------
// 设备名、原生采样率、原生声道、period/buffer、原生格式只存在于 PcmCaptureBackend
// 及其配置中；Session 和核心契约看不到 snd_pcm_t。适配器不硬编码任何板端设备名，
// 也不假设设备一定原生支持 16 kHz 单声道 S16_LE：实际采样率、声道和编码由后端在
// open 时回报，转换器保存跨块状态完成显式转换与重采样。
//
// 轮次与并发
// ----------
// open() 建立一次输入轮次；正常回答收尾不调用 close()，因此同一对象可以跨多个回答持续
// 采集。cancel() 只取消当前采集轮次：置取消线性化点、唤醒阻塞 read、丢弃转换缓存；
// cancel 后的 read 返回 kCancelled，必须 close() 后再 open() 才能开始下一轮，重复 open
// 不能清除取消状态。read() 与 cancel() 允许并发；open/close/recover 由适配器内部设备
// 互斥量串行化，close 会先通知后端取消，再等待设备操作退出，避免释放仍在使用的句柄。
//
// 失败与清理
// ----------
// 所有等待、重试和预算都来自 AlsaAudioSourceConfig 并经过校验。读超时返回 kTimeout；
// overrun/underrun 由后端恢复并返回 kRecovered，适配器在预算内重试；设备断开时在
// reconnect_budget 内调用后端 recover()，成功则丢弃旧转换缓存后继续，失败或预算耗尽
// 返回 kDeviceFailure。close() 幂等释放后端句柄并清空缓存；析构继续调用 close()，
// 但如果对象析构时 read 仍在另一个线程中执行，拥有者必须先退出该线程，否则属于误用。
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "../capability/backend.hpp"
#include "pcm_capture_backend.hpp"

namespace nexweave::backend {

// 适配器策略配置。设备原生参数在 PcmCaptureBackendConfig 中显式配置并校验；本结构
// 只约束上层读取操作的等待、重试和恢复预算，避免把设备参数和操作策略混成一个含义
// 模糊的结构。
struct AlsaAudioSourceConfig {
  // 一次 IAudioSource::read() 从开始到返回成功/失败的最长等待。转换器可能跨多次
  // 后端 read 才能凑齐 320 个输出样本，因此这是整个 read 调用的上限，而不是单次
  // 后端等待。必须大于 0。
  std::chrono::milliseconds read_timeout{500};

  // 一次 read 最多调用后端多少次。它防止后端连续返回空块、超时或恢复状态时形成
  // 空转；达到上限后返回 kTimeout 或 kDeviceFailure，而不是无限重试。
  std::size_t max_backend_reads_per_frame = 64;

  // 一次 read 内允许发起多少次设备重连。每次重连会调用后端 recover()，并丢弃旧
  // 转换缓存，避免把断开前后的音频拼成连续数据。0 表示不重连；上限用于防止持续
  // 断连形成无限循环。
  std::size_t reconnect_budget = 2;

  // 一次 read 内允许处理多少次底层 overrun/underrun 恢复。与重连预算分开，是因为
  // 溢出恢复通常不需要关闭设备；但连续溢出同样必须有上限。0 表示不允许恢复。
  std::size_t max_recoveries_per_frame = 8;
};

// 适配器只读计数。所有字段在 open() 开始时重置，close() 后保留最后一次轮次快照，
// 便于测试与运行证据核对资源释放、取消和恢复是否按预期发生。
struct AlsaAudioSourceStats {
  std::uint64_t open_attempts = 0;
  std::uint64_t open_successes = 0;
  std::uint64_t close_calls = 0;
  std::uint64_t read_calls = 0;
  std::uint64_t frames_returned = 0;
  std::uint64_t backend_reads = 0;
  std::uint64_t backend_timeouts = 0;
  std::uint64_t backend_recoveries = 0;
  std::uint64_t reconnect_attempts = 0;
  std::uint64_t reconnect_successes = 0;
  std::uint64_t device_failures = 0;
  std::uint64_t cancelled_reads = 0;
  std::uint64_t output_samples = 0;
  std::uint64_t samples_discarded_on_recovery = 0;
  std::uint32_t actual_sample_rate_hz = 0;
  std::uint16_t actual_channels = 0;
  PcmSampleFormat actual_format = PcmSampleFormat::kS16LE;
};

// 校验适配器策略配置：超时、读取次数和恢复/重连预算必须有界且为非零可工作的值。
// 该函数不校验设备参数，也不访问后端；后端配置用 validate_pcm_capture_backend_config。
domain::OperationResult validate_alsa_audio_source_config(
    const AlsaAudioSourceConfig& config);

// 真实输入适配器。构造函数接收后端所有权；适配器销毁时先 close() 后端，再释放对象。
// 后端必须由调用方在 move 前创建完成，且不能与其它拥有者共享。
//
// 状态不变量：
//   - opened_==true 且 closed_==false 时，后端句柄处于 open 状态；
//   - cancelled_==true 后，read 返回 kCancelled，必须 close() 后才能再次 open()；
//   - close() 后 opened_==false、closed_==true、取消标志清除，可以开始下一轮；
//   - 转换缓存只在 open() 成功后建立，任何 cancel/close/recover 都会显式丢弃它。
class AlsaAudioSource final : public capability::IAudioSource {
 public:
  AlsaAudioSource(AlsaAudioSourceConfig config,
                  std::unique_ptr<PcmCaptureBackend> backend);
  ~AlsaAudioSource() override;

  AlsaAudioSource(const AlsaAudioSource&) = delete;
  AlsaAudioSource& operator=(const AlsaAudioSource&) = delete;

  // 建立输入轮次：校验配置与后端所有权，调用 backend->open()，成功后清空转换缓存。
  // 首次调用成功；已打开且未 close 返回 kAlreadyCompleted；取消后未 close 返回
  // kCancelled；后端为空或配置非法返回 kInvalidInput；打开失败返回后端错误并保持
  // 未打开状态，允许调用方修正环境后重试。
  domain::OperationResult open() override;

  // 读取一帧固定 16 kHz/单声道/S16_LE/320 样本。未打开/已关闭返回 kDeviceFailure；
  // 取消后返回 kCancelled；设备读取超时返回 kTimeout；设备断开且恢复失败或预算
  // 耗尽返回 kDeviceFailure。短读、overrun、underrun 和恢复不会形成空转。
  domain::Result<domain::AudioFrame> read() override;

  // 取消当前采集轮次：置取消点、唤醒阻塞 read、丢弃未消费的转换缓存。幂等、不抛异常；
  // 已经交付给调用方的帧不撤回，但不会再返回旧缓存中的后续样本。
  domain::OperationResult cancel() noexcept override;

  // 释放后端句柄并清空转换缓存。幂等、不抛异常；若 read 正在阻塞，close 会先请求
  // 后端取消，等待设备互斥量后关闭句柄，保证阻塞 read 在适配器声明的超时内退出。
  domain::OperationResult close() noexcept override;

  // 打开成功后的设备实际格式；未打开时返回无效值。供硬件证据和测试上报实际协商结果。
  PcmCaptureFormat actual_capture_format() const noexcept;

  // 适配器计数快照。可在运行结束后读取，不改变状态。
  AlsaAudioSourceStats stats() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace nexweave::backend
