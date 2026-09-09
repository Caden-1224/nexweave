#include "fake_tts.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace nexweave::backend {

namespace {
// 按确定性规则计算起始相位：UTF-8 字节和模波形周期。unsigned char 保证
// 高字节（>127）不会被符号扩展为负值，同一文本在不同实现上得到同一相位。
std::size_t initial_phase(const std::string& text) noexcept {
  std::size_t byte_sum = 0;
  for (unsigned char byte : text) {
    byte_sum += static_cast<std::size_t>(byte);
  }
  return byte_sum % kFakeTtsCycleSamples;
}
}  // namespace

domain::OperationResult FakeTts::set_callback(capability::AudioEventCallback callback) {
  // 所有检查先于替换：失败注册不能清除旧回调、也不能解除既有取消封锁。
  if (!callback) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
  }
  callback_ = std::move(callback);
  cancelled_.store(false);
  return domain::OperationResult::success();
}

domain::OperationResult FakeTts::synthesize(const std::string& text) {
  // 取消优先于输入校验：取消后的坏文本必须报 kCancelled 而不是 kInvalidInput，
  // 否则坏文本会掩盖“本轮已经失效”这一更早发生的事实（与 FakeAsr 同序）。
  if (cancelled_.load()) {
    return domain::OperationResult::failure(domain::ErrorCode::kCancelled);
  }
  if (!callback_ || text.empty()) {
    return domain::OperationResult::failure(domain::ErrorCode::kInvalidInput);
  }

  // 帧数 = ⌈字节数 / 每帧字节数⌉：非空文本至少一帧，长度只决定帧数步长。
  const std::size_t frame_count =
      (text.size() + kFakeTtsBytesPerFrame - 1) / kFakeTtsBytesPerFrame;
  const std::size_t phase0 = initial_phase(text);

  try {
    for (std::size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
      // 每帧交付前检查取消：这是并发 cancel 的线性化点之一。取消在检查前
      // 到达则本帧不再构造与交付；取消恰在检查之后、回调结束前到达时，本帧
      // 仍会完成交付（并发窗口，无法撤回），取消从下一帧的检查开始生效。
      if (cancelled_.load()) {
        return domain::OperationResult::failure(domain::ErrorCode::kCancelled);
      }

      // 每次合成从全局样本 0 重新计数，同文本可重复得到完全一致的 PCM；
      // 帧在栈上独立构造，回调期间引用有效，返回后由析构释放，资源上界
      // 恒为一帧。元数据使用默认值即 v1 契约，不产生短帧或补零尾帧。
      domain::AudioFrame frame;
      frame.samples.resize(domain::kAudioFrameSamples);
      for (std::size_t sample = 0; sample < domain::kAudioFrameSamples; ++sample) {
        const std::size_t global_sample = frame_index * domain::kAudioFrameSamples + sample;
        const std::size_t position_in_cycle = (phase0 + global_sample) % kFakeTtsCycleSamples;
        frame.samples[sample] = position_in_cycle < kFakeTtsCycleSamples / 2
                                    ? kFakeTtsSquareAmplitude
                                    : -kFakeTtsSquareAmplitude;
      }
      callback_(frame);
    }

    // 结束边界：全部帧已交付但尚未返回时取消到达，仍收敛为 kCancelled，
    // 不能把“帧已发完”误报为成功终态；整段音频已经交付，外层不得把该结果
    // 当作可重试失败整段重放。已交付的帧无法撤回，取消只禁止后续新帧开始
    // 交付以及开启新轮次。
    if (cancelled_.load()) {
      return domain::OperationResult::failure(domain::ErrorCode::kCancelled);
    }
    return domain::OperationResult::success();
  } catch (...) {
    // 回调或帧构造抛出异常：置位取消封锁本轮，避免重试产生重复交付；
    // 无输出队列或后台资源需要等待，重新注册回调后才恢复。原异常保留给
    // 调用方定位，接收方自行清理可能已收到的帧。
    cancelled_.store(true);
    throw;
  }
}

domain::OperationResult FakeTts::cancel() noexcept {
  // 原子置位即为串行与并发两种场景下的统一取消请求；无分配、不抛异常。
  // 生效窗口（至多再交付一帧）由 synthesize 内的检查点决定，见头文件注释。
  cancelled_.store(true);
  return domain::OperationResult::success();
}

}  // namespace nexweave::backend
