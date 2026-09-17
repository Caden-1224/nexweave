// 远端会话服务端：把独立的 SessionApp 接到数据面 PAIR 通道上。
//
// 职责与边界
// ----------
// 本类运行在独立进程内，持有一个已经组装好的 SessionApp 和一个队列音频源。它只负责：
//   - 绑定数据面端点并完成显式 ready 握手；
//   - 从 InputStreamEvent 流中取出 start/frame/end/cancel，把它们翻译成音频源的 push/
//     end_input/cancel；
//   - 在会话工作线程上运行 SessionApp，并把每轮结果转成 DataEvent 发回对端。
// 它不解析 NDJSON、不实现控制 RPC、不访问模型 SDK 或设备。传输类型只出现在实现文件里，
// 公共头文件只暴露领域值和显式配置。
//
// 线程与所有权
// ------------
// app 与 input 是借用引用，必须比本对象活得久。bind()/serve() 由同一个调用线程串行调用；
// serve() 内部创建恰好一个会话工作线程调用 app.run()，ZeroMQ 数据通道只由 serve() 所在
// 线程收发。观察者回调在会话工作线程上同步执行，只把事件投入互斥量保护的出站队列，绝不
// 触碰 socket。退出时先停止输入并请求会话收敛，再 join 工作线程，最后处理残留出站事件。
//
// 取消语义
// --------
// kCancel 输入事件表示“输入流终止并取消当前会话”：服务端先调用 app.cancel_turn() 封锁
// 旧输出，再取消音频源、请求运行停止，使阻塞在空队列上的会话线程在有限步内返回。已经交付
// 的帧不撤回；退出后不再发出新轮次事件。正常 kEnd 只结束输入，不取消已经开始的轮次。
//
// 资源与失败收敛
// --------------
// 服务端创建：一个 ZeroMQ 数据通道、一个会话工作线程、一个有界出站事件队列。音频源与
// SessionApp 由调用方拥有。连接失败、ready 超时、输出队列满、发送失败或接收错误都会写入
// 结构化错误，唤醒等待方并走统一停止路径；重复 request_stop 幂等。
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "../app/session_app.hpp"
#include "../runtime/queued_audio_source.hpp"
#include "../domain/error.hpp"
#include "../protocol/data_event.hpp"
#include "zmq_data.hpp"

namespace nexweave::transport {

// 远端会话服务端配置。全部容量与预算显式可配；非法值构造时整份回退默认值。
struct RemoteSessionServerConfig {
  // 数据面 PAIR socket 绑定端点。tcp://127.0.0.1:* 让系统分配端口，便于测试夹具写
  // 端点文件；真实部署应显式指定稳定端点。
  std::string endpoint = "tcp://127.0.0.1:*";
  // 数据面传输级配置，透传给 ZmqDataChannel。
  ZmqDataConfig data{};
  // bind() 成功后的实际端点返回给调用方；ready 握手超时表示对端没有在预算内连接。
  std::chrono::milliseconds ready_timeout{5000};
  // 没有输入事件时的轮询粒度，也是 request_stop 的收敛上界量级。
  std::chrono::milliseconds receive_poll_interval{10};
  // 尚未发送的数据事件上限（条）。观察者不阻塞，队列满即进入显式失败收敛，避免会话线程
  // 被慢消费者永久拖住；统一应用队列策略由上层配置继续扩展。
  std::size_t output_queue_capacity = 1024;
};

// 配置校验：端点非空、预算与队列容量为正。只读，不创建 socket/线程。
domain::OperationResult validate_remote_session_server_config(
    const RemoteSessionServerConfig& config);

// 服务端只读账目快照。计数只增不减，用于核对输入帧、输出事件与失败路径。
struct RemoteSessionServerStats {
  std::uint64_t received_input_events = 0;
  std::uint64_t received_input_frames = 0;
  std::uint64_t sent_output_events = 0;
  std::uint64_t input_rejected = 0;
  std::uint64_t output_queue_overflows = 0;
  std::size_t output_queue_capacity = 0;
  std::size_t output_queue_peak = 0;
  std::uint64_t send_failures = 0;
  std::uint64_t receive_timeouts = 0;
};

class RemoteSessionServer final {
 public:
  // 借用 app 与 input：它们必须比本对象活得久，且 input 必须已经与 app 的常驻输入
  // 组装在同一个 SessionApp 配置中。构造不绑定 socket、不创建线程。
  RemoteSessionServer(runtime::SessionApp& app, runtime::QueuedAudioSource& input,
                      RemoteSessionServerConfig config = {});
  ~RemoteSessionServer();

  RemoteSessionServer(const RemoteSessionServer&) = delete;
  RemoteSessionServer& operator=(const RemoteSessionServer&) = delete;

  // 绑定数据面 PAIR socket。成功返回后 bound_endpoint() 可用，但 ready 握手与业务尚未
  // 开始；调用方应在写端点文件、通知父进程就绪之后调用 serve()。重复 bind 返回
  // kAlreadyCompleted；失败不保留可用的监听端点。
  domain::OperationResult bind();

  // 等待对端连接并运行会话直到输入结束、取消或错误收敛。必须在 bind() 之后调用一次；
  // 内部创建会话工作线程，返回前 join 并冲刷已排队输出。不可重入。
  domain::OperationResult serve();

  // 请求停止服务端：只置原子标志，不触碰 socket、不阻塞、不分配。serve() 会在下一次
  // 轮询边界取消音频源并请求会话停止，因此调用方可从监控线程或信号处理后的普通线程调用。
  void request_stop() noexcept;

  // bind() 成功后的实际端点；未绑定或已关闭时为空串。返回副本。
  std::string bound_endpoint() const;

  // 账目快照；必须在 bind()/serve() 同一线程或 serve 返回后调用，内部只做互斥量保护快照。
  RemoteSessionServerStats stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace nexweave::transport
