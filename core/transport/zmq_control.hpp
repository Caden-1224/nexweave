// ZeroMQ 控制传输适配器：把进程内 Gateway 暴露为带 deadline 的请求/响应控制面。
//
// 职责与边界
// ----------
// 本层只负责控制面的字节搬运、连接生命周期和 ZeroMQ socket 回收；不执行会话、不做检索或
// 推理，也不解释控制载荷的业务含义。请求进入后仍由 gateway::Gateway 完成版本校验、幂等、
// deadline 收紧与结构化响应；ZeroMQ 的 context/socket 类型不会出现在本头文件中，因此不会
// 泄漏到 Session 或领域层。
//
// 线程与所有权
// ------------
// 服务端由一个专用 I/O 线程拥有并串行使用 ZeroMQ REP socket；Gateway 也只在同一个 I/O 线程
// 上被调用，满足 Gateway “不是线程安全、必须单线程驱动”的契约。会话推理仍在 Supervisor 的
// 工作线程上执行，所以 start 返回后 I/O 线程可以继续处理 query/cancel；exit 是按预算等待
// 清理的终态操作，超时由 Gateway 的结构化结果回答。socket 只能在该 I/O 线程内创建、使用和
// 销毁；stop() 只置停止标志并等待线程退出，不跨线程操作 ZeroMQ 对象。
//
// 资源与失败清理
// --------------
// 服务端创建：一个 std::thread、一个 ZeroMQ context 和一个 REP socket。线程退出前按顺序
// 关闭 Gateway 虚拟连接、socket 和 context；start() 绑定失败时返回结构化错误且不留下线程。
// 客户端创建：一个 ZeroMQ context 和一个 REQ socket；REQ 在超时后状态机会失效，因此每次
// 超时/接收失败都重建 socket，允许调用方用同一 request_id 重试；服务端的幂等记录由 Gateway
// 负责，传输层不伪造完成结果。
#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

#include "../gateway/gateway.hpp"
#include "../protocol/control_rpc.hpp"

namespace nexweave::transport {

// ZeroMQ 控制服务端配置。所有容量和等待都显式可配；非法值在构造时整份回退到默认值，并通过
// validate_zmq_control_server_config() 单独检查。默认端点绑定到本机回环的临时端口，便于测试
// 与同机部署；真实多进程部署应显式指定固定端点。
struct ZmqControlServerConfig {
  // REP socket 的绑定端点。空串非法；默认 tcp://127.0.0.1:* 由系统分配端口。
  std::string endpoint = "tcp://127.0.0.1:*";
  // ZeroMQ 接收/发送高水位（条数），必须 >0；它限制单个对端排队，而不是业务队列容量。
  std::size_t receive_high_water_mark = 32;
  std::size_t send_high_water_mark = 32;
  // 无请求时 I/O 线程的轮询间隔，也是 stop() 等待线程收敛的上界量级；必须 >0。
  std::chrono::milliseconds poll_interval{10};
  // 向客户端发送响应时的 socket 发送超时；慢消费者达到 HWM 后本端不会无限阻塞；必须 >0。
  std::chrono::milliseconds send_timeout{500};
  // start() 等待绑定完成的预算；绑定失败或超时会以结构化错误返回；必须 >0。
  std::chrono::milliseconds bind_wait_budget{1000};
  // 单次请求最多从 Gateway 取走的控制响应字节数；必须 >0，防止坏配置造成无界读取。
  std::size_t max_response_bytes = 64U * 1024U;
};

// 配置校验：只读配置，不创建 socket、不绑定端口、不启动线程。容量/预算非正返回
// kInvalidInput，成功不改变输入。
domain::OperationResult validate_zmq_control_server_config(
    const ZmqControlServerConfig& config);

// ZeroMQ 控制服务端。构造只保存 Gateway 借用引用与配置，不创建线程或 socket；start() 才
// 建立 context/socket 并启动 I/O 线程。gateway 必须比本对象活得久。
//
// 生命周期约定：
//   - start() 在未运行时启动；成功返回后 running() 为真且 bound_endpoint() 可用。
//   - start() 在运行中返回 kAlreadyCompleted，不重复创建线程。
//   - stop() 幂等；置位停止标志并 join I/O 线程。stop() 只能在非 I/O 线程调用。
//   - 析构等价于 stop()；调用方必须保证析构期间没有其他线程进入本对象。
//   - stop() 会关闭本适配器在 Gateway 中占用的虚拟连接，因此由该连接发起的在途会话按
//     “对端断开”策略受理取消；清理是否完成由调用方查询 Supervisor/Gateway 回答。
class ZmqControlServer final {
 public:
  explicit ZmqControlServer(gateway::Gateway& gateway,
                            ZmqControlServerConfig config = {});
  ~ZmqControlServer();

  ZmqControlServer(const ZmqControlServer&) = delete;
  ZmqControlServer& operator=(const ZmqControlServer&) = delete;

  // 启动 I/O 线程并绑定端点。返回失败时对象保持在未运行状态，可修正配置或端点后重试。
  domain::OperationResult start();
  // 请求停止并 join I/O 线程；重复调用成功且无副作用；不得在服务端 I/O 线程上调用。
  domain::OperationResult stop() noexcept;
  // 是否处于已绑定并运行状态。
  bool running() const noexcept;
  // 实际绑定端点；未运行时为空串。返回副本，调用方拥有。
  std::string bound_endpoint() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ZeroMQ 控制客户端配置。客户端不解释 request_id，也不缓存响应；幂等判断在服务端完成。
struct ZmqControlClientConfig {
  // 发送单个请求的 socket 超时；必须 >0。
  std::chrono::milliseconds send_timeout{500};
  // REQ socket 的接收高水位；必须 >0。
  std::size_t receive_high_water_mark = 32;
};

// 配置校验：只读配置，不创建 socket。send_timeout 非正或 receive_high_water_mark 为 0 返回
// kInvalidInput。
domain::OperationResult validate_zmq_control_client_config(
    const ZmqControlClientConfig& config);

// ZeroMQ 控制客户端。构造只保存端点，不建立网络连接；connect() 建立 REQ socket，call() 在
// 需要时自动建立连接。调用方拥有所有返回值；close() 后不能继续 call()。
//
// deadline 语义：call() 的预算取 ControlRequest.deadline。发送和接收都受同一相对起点约束；
// 一旦超时，REQ socket 会被重建，下一次调用可以重试同一 request_id，而是否重复执行由服务端
// Gateway 的幂等记录决定。客户端不把超时伪装成成功，也不替服务端判断任务是否已经受理。
class ZmqControlClient final {
 public:
  explicit ZmqControlClient(std::string endpoint,
                            ZmqControlClientConfig config = {});
  ~ZmqControlClient();

  ZmqControlClient(const ZmqControlClient&) = delete;
  ZmqControlClient& operator=(const ZmqControlClient&) = delete;

  // 显式建立/重建 REQ socket 并连接端点。重复调用会关闭旧 socket 后重连；配置错误或 ZeroMQ
  // 异常返回结构化错误。成功后 connected() 为真。
  domain::OperationResult connect();
  // 发送请求并在 request.deadline 内等待响应。失败不返回伪造响应；超时返回 kTimeout，传输
  // 层异常返回 kBackendFailure，协议解码/归属错误返回对应协议错误。
  domain::Result<protocol::ControlResponse> call(
      const protocol::ControlRequest& request);
  // 幂等关闭 socket。关闭后 connected() 为假，call() 返回 kAlreadyCompleted；本方法不抛异常。
  void close() noexcept;
  bool connected() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace nexweave::transport
