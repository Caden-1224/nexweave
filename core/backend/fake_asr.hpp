// 确定性 ASR 适配器：以文本夹具替代模型推理，用于 C++17 Mock 能力测试。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "../capability/backend.hpp"

namespace nexweave::backend {

/**
 * FakeAsr（Fake Automatic Speech Recognition，模拟自动语音识别）把合法 PCM
 * 送帧操作映射为预设文本事件；不分析波形，也不声称识别准确率。
 *
 * 夹具是非空的累计假设列表，每项非空；允许后项修正前项而不要求前缀关系。
 * 非结束帧每次发布下一项 partial，列表耗尽后重复最后一项，游标饱和避免长流溢出；
 * 任一合法结束帧直接发布最后一项 final，再发布空文本 done，并归零进入空闲。
 * 一帧即可结束；没有提交帧时没有事件，空载荷不是结束标记，必须按音频合同拒绝。
 *
 * 对象拥有夹具与回调副本，不保存 PCM 或输出队列，不创建文件、线程、进程、
 * socket、SDK/设备句柄。输入引用只在 feed 内借用，事件引用只在回调期间有效；
 * 接收方自行复制事件，并负责清理已接收的旧 generation 数据，取消不能撤回已交付值。
 * 构造参数按值接收后移动，内存由标准库成员析构释放；复制/分配异常向上传播。
 *
 * 所有方法及析构由调用方串行调度；同实例不支持并发调用，回调不得重入或销毁
 * 本对象，也不得抛异常。回调在 feed 调用线程同步完成，无隐藏任务或延时；
 * 调用方通过送帧节奏控制进度，无内部 deadline。回调若不返回，feed 无法超时抢占，
 * 需有抢占需求时使用外层调度适配器，不能在另一线程直接调用本对象来中断。
 */
class FakeAsr final : public capability::IAsr {
 public:
  // 只保存夹具；空列表/空项延迟到 set_callback 返回 kInvalidInput，不在构造时
  // 伪造成功事件。与既有 IAsr、v1 固定 16 kHz/单声道/S16_LE/320 样本合同兼容。
  explicit FakeAsr(std::vector<std::string> hypotheses);

  // 非空回调与合法夹具才成功；成功注册是显式新轮次边界，替换旧回调、清游标及取消态。
  // 包括活动轮次在内均可重置；失败保留旧回调和状态。捕获引用必须活到替换/析构前。
  domain::OperationResult set_callback(capability::TextEventCallback callback) override;

  // 先检查取消，再检查回调与帧；分别返回 kCancelled 或 kInvalidInput，失败不发事件、
  // 不消费夹具。成功同步完成 partial 或 final/done；正常结束后可直接 feed 新一轮。
  // 分配异常若在交付前发生则保持游标；回调异常发生后封锁本轮并归零，再向上传播，
  // 避免重试产生重复 final/done；恢复必须重新注册，接收方清理可能已收到的部分事件。
  domain::OperationResult feed(const domain::AudioFrame& frame, bool is_last) override;

  // 幂等、无分配、不抛异常；设置取消态并清游标即为串行调用下的线性化点。
  // 返回后没有在途回调或待发事件；后续 feed 保持 kCancelled，只有成功注册才恢复。
  domain::OperationResult cancel() noexcept override;

 private:
  std::vector<std::string> hypotheses_;  // 构造后不修改，自有文本不引用调用方容器。
  capability::TextEventCallback callback_;  // 自有函数对象；捕获引用仍由调用方管理。
  std::size_t next_hypothesis_ = 0;  // 当前轮次下一条假设索引，饱和于最后一项。
  bool cancelled_ = false;  // cancel 后保持封锁，防止旧轮次在 feed 中自动复活。
};

}  // namespace nexweave::backend