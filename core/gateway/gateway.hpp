// NexWeave 进程内 Gateway（请求入口）：把外部的控制请求与数据事件接到设备运行时上。
//
// 它解决什么问题
// --------------
// Supervisor 回答“现在有没有会话、能不能再开一个、用户喊停之后停干净没有”，但它只提供
// 进程内的函数调用。外部客户端说的是另一套东西：一行一行的 JSON、可能被任意切分的字节流、
// 会中途断开的连接。本类就是把这两者接起来的唯一一层——解析、校验、路由、幂等、有界发送，
// 并把会话结果转成数据面事件。
//
// 本入口不做的事：不跑会话、不做检索与推理、不开 socket、不做重采样或设备访问。会话本身由
// Supervisor 与它背后的会话拥有者执行；传输层（真实 TCP、ZeroMQ 或多进程转发）由外层实现，
// 它只负责把字节交给 feed() 并把 flush() 交出的字节写出去。
//
// 四个控制操作
// ------------
//   start  受理一次创建。槽位空闲时建立一个会话并**立即**返回受理响应；会话此后由监督器的
//          工作线程执行，本入口不等待推理结束。
//   query  取一次状态快照，立即返回。
//   cancel 请求取消当前会话，默认只受理不等待清理；清理是否完成由 message 与随后的终态事件
//          分别回答。
//   exit   退出设备运行时：受理停止、等待清理、把监督器推进到已退出，然后关闭全部连接。
//
// 受理响应与执行终态是两件事
// --------------------------
// start 的响应只说明“会话被受理了”；这一次会话最终成功、失败还是被取消，由随后交付的
// 数据面终态事件（done/error，end=true）回答。取消响应同理：它报告的是受理与清理快照，
// 而不是“设备已经静默”。规格要求这两类事实分别观测，本入口因此不在一个响应里同时承诺
// 它们，也不允许调用方把响应 ok 当成执行已经完成。
//
// 状态载荷的交付方式（v1 限制）
// ----------------------------
// v1 的 ControlResponse 只有版本、请求标识、ok、错误码、诊断文本与重放标记六个字段，没有
// 承载状态载荷的位置。本入口因此把控制事实以**事实行**交付：空格分隔的 `key=value`，键顺序
// 由实现固定，值不含空格（标识与状态名的合法字符集都不含空格）。这是对 v1 的迁就而不是
// 设计偏好：把状态提升为结构化字段需要控制面契约的新版本，属于版本化决策，本入口不私自
// 扩展 v1 的字段集合。自由文本只出现在分派前的拒绝响应里（沿用协议层的诊断）。
//
// 请求预算（deadline）的行为
// --------------------------
// ControlRequest.deadline 是接收方的处理预算。本入口没有排队环节——请求到达即被处理——因此
// 它不用于中断已经开始的工作，而是用来收紧本入口自己造成的等待：cancel 与 exit 的实际等待
// 取“配置预算”与“请求预算”的较小者，因此一次配置过大的等待不会超出客户端声明的预算。
// start 是例外：建立就绪的等待上界由监督器的启动预算决定，本入口既不修改它，也不会在预算
// 用尽后把已经受理的会话丢掉——受理之后必须把清理走完，丢弃会让资源状态不可解释。因此
// 建立等待可能长于某个请求自己的预算，这一点是显式取舍，不是遗漏。
//
// 与推理并发
// ----------
// 会话在监督器的工作线程上执行，本入口的任何操作都不会等待整段推理：start 只等待“建立
// 就绪”（上界由监督器的启动预算决定），query 只取一次快照，cancel 默认零等待，只有 exit
// 会按预算等待清理。因此一次生成未结束时，另一个控制请求照常被处理。
//
// 数据面交付的粒度（本实现的边界）
// --------------------------------
// 监督器只暴露**已定局**的会话结论（三段错误、计数、运行记录），没有逐轮回调接缝，因此本
// 入口在一次会话完全收敛之后成批交付该次会话的事件与终态，而不是逐 token 推送。事件按轮次
// 一条一条给出（token 与局部 error），终态单独一条。逐段推送需要会话层的事件回调接缝，
// 属于后续任务；本实现不假装已经做到。
//
// 连接关闭策略（显式，不含糊）
// ----------------------------
//   - 对端断开：本连接所属的在途会话被**受理取消**（它的输出已经没有接收者），随后该连接
//     关闭。清理是否完成仍由监督器回答，本入口不用“连接没了”冒充“设备静默了”。这条策略
//     与“谁先发起关闭”无关：服务端因为慢客户端或超长帧关闭连接之后，传输层报来的对端断开
//     同样会受理取消，否则会话会带着一个永远没人读取的输出继续占用设备槽位。
//   - 无法归属的非法输入（坏 JSON、request_id 缺失或非法）：v1 不允许发送没有 request_id
//     的响应，因此没有可寻址的答复对象；记录原因并关闭连接，不伪造一个身份去回复。
//   - 可归属的非法输入（能取出合法 request_id）：回结构化错误响应，连接保持可用。
//   - 单帧超长：与该超长帧同批到达的输入整体作废（无法保证批内先后关系），记录原因并关闭。
//     更一般地，任何一帧导致连接关闭之后，本批次剩余的帧都不再处理：连接已经不存在，继续
//     执行它们会让“哪些请求真的生效了”不可解释。
//   - 慢客户端：待发缓冲达到声明上限即关闭连接。本入口不静默丢弃已经产生的输出——那会让
//     客户端收到一段看不出缺口的流；把会话背压回推理侧需要端到端的队列策略，属于后续任务。
//   - 关闭时仍未成帧的尾部字节会被丢弃并计入账目：一条流在中途断掉时，“有多少输入没有构成
//     请求”是可审计的事实，而不是静默消失的字节。
//   - exit：先答复退出结果，再关闭全部连接。已经产生的待发字节仍然可以 flush() 取走，
//     因此退出响应不会被“关闭”吞掉。
//
// 幂等
// ----
// request_id 在同一个入口实例内唯一标识一次逻辑操作。重复到达时：字段一致且原操作仍在执行
// 返回 kAlreadyCompleted；字段一致且原操作已完成，重放原响应并置 replayed=true；字段冲突
// 返回 kInvalidInput。三条判定都复用协议层的校验，不在本类里另写一份比较逻辑。
//
// 记录容量是**硬上限**：达到上限后拒绝新的 request_id（kBackendFailure），而不是淘汰旧记录。
// 理由是淘汰会立刻破坏幂等本身——被淘汰的请求再来一次就与全新请求无法区分，于是“重试不重复
// 启动任务”不再成立。宁可明确拒绝新工作，也不接受一次可能重复执行的操作；这与规格里“资源
// 不可用时明确拒绝新工作”一致。达到上限属于运行实例的容量事件，需要更长记忆时应提高上限或
// 由后续的持久化幂等存储替换，而不是在本入口里悄悄降低保证。
//
// exit 的特殊之处：运行时确实退出时响应为成功，随后全部连接被关闭；如果等待清理超时，它
// 同样是一次**已完成**的操作结果（kTimeout），因此重试同一个 request_id 只会重放这次超时。
// 要继续等待必须使用新的 request_id——这与“重放已定局结果”的幂等语义一致，也避免同一身份
// 既表示“已超时”又表示“又等了一次”。
//
// 线程安全
// --------
// 本类**不是**线程安全的：全部入口必须由同一个线程（通常是传输层的事件循环线程）串行调用。
// 之所以不给它加锁：加锁只能掩盖“调用方用多个线程驱动同一条连接”的错误，却把监督器的阻塞
// 入口（建立等待）拖进锁内，使一次建立等待阻塞所有连接。会话执行本身在监督器的工作线程上，
// 与这里无关。
//
// 资源归属
// --------
// 本类创建：每连接一个解帧器、一份连接账目与两条有界发送队列（控制响应、数据事件）、一张
// 幂等记录表。它不创建线程、文件、socket 或设备句柄；supervisor 与 run_source 按借用保存，
// 必须比本对象活得久。发送队列的字节由调用方通过 flush() 取走，本类不负责写入任何设备。
// 已关闭且已排空的连接在下一次 open_connection() 时被回收，因此连接记录不会无界累积。
//
// 析构不做收尾动作：不排空待发字节、不受理取消、不等待在途会话。调用方应当在销毁本对象
// 之前把该取的响应取走、把运行时退出（exit）或由监督器自行收敛；析构之后再取字节已经
// 没有对象可问。在途会话不受本对象销毁影响——它由监督器拥有，会继续运行到收敛。
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "../app/session_app_owner.hpp"
#include "../domain/error.hpp"
#include "../protocol/control_rpc.hpp"
#include "../protocol/data_event.hpp"
#include "../runtime/supervisor.hpp"
#include "control_route.hpp"
#include "ndjson_framer.hpp"

namespace nexweave::gateway {

// 连接标识。0 保留为“无效连接”，因此新连接的编号从 1 开始，且在同一入口实例内不复用：
// 一个过期的连接标识不会突然指到另一条连接上。
using ConnectionId = std::uint64_t;
inline constexpr ConnectionId kInvalidConnectionId = 0;

// 连接被关闭的原因。它用于运行证据与调用方决策，不参与线协议。
enum class GatewayCloseReason : std::uint8_t {
  // 仍然打开。
  kNone = 0,
  // 对端断开（或传输层报告连接已失效）。这是唯一会触发“取消在途会话”的原因。
  kPeerClosed = 1,
  // 无法归属到请求的非法输入：坏 JSON、request_id 缺失或非法。
  kProtocolViolation = 2,
  // 单帧超过声明上限。
  kOversizedFrame = 3,
  // 待发缓冲达到上限：客户端消费速度跟不上服务端产出速度。
  kSlowClient = 4,
  // 收到 exit 且运行时确实已退出。
  kExited = 5
};

// 连接关闭原因的机器可读名称，用于日志与测试断言。未知值返回空串。
// 纯函数：不读时钟、不分配、不访问连接或任何外部资源，可从任意线程并发调用。
const char* to_string(GatewayCloseReason reason) noexcept;

// 入口配置。全部字段都是显式策略：默认值适用于进程内 Mock 演示，调用方可以覆盖。
struct GatewayConfig {
  // 同时可打开的连接数上限（不含已关闭但尚未回收的记录）。达到上限时 open_connection()
  // 返回 kInvalidConnectionId，而不是无限接受连接。
  std::size_t max_connections = 16;
  // 单帧内容上限（字节，不含换行符），透传给解帧器。超过即关闭连接。
  std::size_t max_frame_bytes = kDefaultMaxFrameBytes;
  // 控制响应待发上限（字节）。控制响应绝不被丢弃，因此它放不下时关闭连接。
  std::size_t max_pending_control_bytes = 64U * 1024U;
  // 数据事件待发上限（字节）。它是“慢客户端不会无限占用缓冲”的直接机制。
  std::size_t max_pending_data_bytes = 512U * 1024U;
  // 幂等记录容量上限（条）。达到上限后拒绝新的 request_id，见类注释。
  std::size_t max_idempotency_records = 256;
  // 路由模式下单次 poll_events() 最多取回的数据事件数。它限制一次外部驱动循环的工作量，
  // 防止远端一次积压大量事件时把本地单线程入口长时间占住；必须为正。
  std::size_t max_routed_events_per_poll = 64;
  // 路由模式下的转发工作线程数。每条控制请求在独立工作线程上调用 IControlRoute，因此一个
  // 慢转发不会阻塞同一入口的其他控制操作；必须为正。工作线程共享同一个路由对象，因此
  // IControlRoute::call() 必须允许并发调用。
  std::size_t max_route_workers = 4;
  // cancel 的等待清理预算默认值。0 表示只受理、不等待：取消响应只报告受理与快照，
  // 清理完成由终态事件回答。取值非负；请求自带的 deadline 会进一步收紧它。
  std::chrono::milliseconds cancel_wait_budget{0};
  // exit 的等待清理预算默认值。退出是终态操作，因此默认等到清理结束（或超时）为止，
  // 使“退出”本身有一个明确结果，而不是留下一个说不清的中间态。
  std::chrono::milliseconds exit_wait_budget{5000};
};

// 配置校验：只读配置，不创建连接、不访问外部资源、不阻塞、不分配失败以外的异常。容量字段
// 必须为正（容量为 0 的连接没有任何可发送的余地，只会立刻关闭），预算不得为负。失败返回
// kInvalidInput，调用方应在构造入口之前把它当成配置错误处理；构造本身不抛业务异常，非法
// 配置整份回退到默认值。可从任意线程并发调用。
domain::OperationResult validate_gateway_config(const GatewayConfig& config);

// 单条连接的状态与账目快照。已关闭但未被回收的连接仍可查询，因此“为什么关的”可被审计。
// 单条连接的账目。它既是 `connection_stats()` 的返回类型，也是连接对象的内部记账结构：
// 只有一份字段定义，不会出现“对外报的账”和“内部记的账”两套。
// 已关闭但未被回收的连接仍可查询，因此“为什么关的”可被审计。
struct GatewayConnectionStats {
  bool known = false;
  bool open = false;
  GatewayCloseReason close_reason = GatewayCloseReason::kNone;
  // 已成功解帧并交给处理的帧数（含被拒绝的请求）。
  std::uint64_t frames_received = 0;
  std::uint64_t responses_sent = 0;
  std::uint64_t events_sent = 0;
  // 当前待发字节数。它与 flush() 一起构成慢客户端判定的依据。
  std::size_t pending_control_bytes = 0;
  std::size_t pending_data_bytes = 0;
  // 已被 flush() 取走的字节数。
  std::uint64_t bytes_written = 0;
  // 连接关闭时仍未成帧的尾部字节数。输入在中途断掉时，这些字节没有构成任何请求，既不能
  // 当成帧处理也不该静默消失，因此单独记账。
  std::size_t discarded_partial_bytes = 0;
};

// 入口级状态快照。字段全部来自调用瞬间的一致读取，调用方拥有副本。
// 它把“入口看到的账目”和“监督器看到的账目”放在一起，便于核对忙碌拒绝、断连取消与
// 事件交付是否与真实创建的会话一致。
struct GatewayStatus {
  bool closed = false;
  std::size_t open_connections = 0;
  bool session_in_flight = false;
  std::uint64_t in_flight_session_sequence = 0;
  std::string in_flight_work_id;
  std::string in_flight_session_id;
  std::string in_flight_request_id;

  std::uint64_t connections_opened = 0;
  std::uint64_t connections_reaped = 0;
  std::uint64_t requests_accepted = 0;
  std::uint64_t requests_rejected = 0;
  std::uint64_t requests_replayed = 0;
  std::uint64_t idempotency_rejections = 0;
  std::uint64_t malformed_frames = 0;
  std::uint64_t oversized_frames = 0;
  std::uint64_t slow_client_closes = 0;
  std::uint64_t protocol_violation_closes = 0;
  std::uint64_t control_responses = 0;
  std::uint64_t data_events = 0;
  // 终态事件没有接收者（发起会话的连接已经断开）而丢弃的次数。本入口不缓存失去连接的输出，
  // 这类丢弃必须被记账，否则“事件少了”会被误当成“会话没产生事件”。
  std::uint64_t events_dropped_without_receiver = 0;
  // 事件已构造但无法编码（例如文本不是合法 UTF-8）而未能交付的次数。
  std::uint64_t encode_failures = 0;
  std::uint64_t disconnects_cancelling_session = 0;
  // 已处理的退出请求次数。它与“运行时确实退出”不是同一件事：一次超时之后客户端可以用新的
  // request_id 再退一次，因此这个计数可以大于 1，而 closed 只会从假变成真一次。
  std::uint64_t exit_requests = 0;
  // 全部连接在关闭时丢弃的未成帧尾部字节总数。已关闭且已被回收的连接不再可查，因此这里保留
  // 一个累计值，使“输入断在半帧上”不会随着连接记录一起消失。
  std::size_t discarded_partial_bytes = 0;

  // 监督器状态快照。会话计数、三段错误与工作身份以它为准，本结构不重复一份。
  runtime::SupervisorStatus supervisor;
};

// 进程内请求入口。构造只校验配置并保存借用引用，不打开连接、不创建线程、不访问设备，
// 因此不会阻塞、不会失败（非法配置整份回退到默认值）。
//
// 借用关系：supervisor 与 run_source 必须比本对象活得久。run_source 提供“最近一次收敛会话
// 交付了什么”，生产环境由会话拥有者工厂实现；测试可以注入一个只记录结果的实现，从而在不跑
// 真实会话的前提下验收入口行为。
class Gateway final {
 public:
  Gateway(runtime::Supervisor& supervisor, runtime::ISessionRunSource& run_source,
          GatewayConfig config = {});

  // 路由模式：把解析后的控制请求交给 route 执行。它复用同一个 Gateway 的分帧、幂等、连接
  // 账目与有界发送，因此外部协议不变；route 必须比本对象活得久。事件交付通过 route 的
  // poll_events() 接缝继续使用本对象的数据队列。
  explicit Gateway(IControlRoute& route, GatewayConfig config = {});

  // 析构不排空待发字节、不受理取消、不等待在途会话（见类注释的资源归属）。不抛异常。
  ~Gateway();

  Gateway(const Gateway&) = delete;
  Gateway& operator=(const Gateway&) = delete;

  // 打开一条连接。返回 kInvalidConnectionId 表示入口已经退出，或同时打开的连接数达到上限；
  // 两种情况都不产生任何其他副作用。达到上限前会先回收“已关闭且待发字节已排空”的连接，
  // 因此正常使用不会因为历史连接而耗尽名额。
  ConnectionId open_connection();

  // 喂入一条连接的入站字节。切分任意：半包留在解帧器里，粘包会产出多帧。
  // 返回 false 表示这条连接已经关闭。关闭可能由本次输入触发（超长帧、无法归属的非法输入、
  // 慢客户端、退出），也可能在此之前就已经发生——例如携带 exit 的这一帧本身就会被处理，
  // 然后连接才关闭。因此调用方判断“还有没有答复要取”应当看 flush()，而不是看这个返回值。
  bool feed(ConnectionId id, std::string_view bytes);

  // 取走待发字节，追加到 out，最多 budget 字节；先控制响应后数据事件。
  //
  // 为什么控制响应优先：控制响应是客户端继续推进的唯一凭据，把它排在成片的数据事件之后，
  // 就等于让一次查询/取消的可见性取决于客户端读得多快——那正是“转发层等待推理”的翻版。
  // 两个队列内部各自保持先进先出，因此同类消息不会重排；跨队列的先后不属于任何契约。
  //
  // budget 为 0 或连接不存在时返回 0。已关闭的连接的待发字节仍可被取走，使退出响应与
  // 终态事件不会被关闭动作吞掉；连接记录的回收发生在下一次 open_connection()。
  std::size_t flush(ConnectionId id, std::string& out, std::size_t budget);

  // 关闭一条连接并释放它的输入方向。原因是幂等记录：第一次关闭的原因被保留，后续调用不再
  // 改写它。若在途会话由这条连接发起，本次关闭会**受理取消**该会话（输出已无接收者）——这条
  // 策略与“连接此前是否已被服务端关闭”无关，因此传输层随后报来的对端断开同样会生效。
  // 对端断开与传输层错误都由调用方通过本入口报告；关闭时仍未成帧的尾部字节计入账目。
  void close_connection(ConnectionId id,
                        GatewayCloseReason reason = GatewayCloseReason::kPeerClosed);

  // 交付一次已经收敛的会话的事件与终态，返回本次交付的事件数。它只做一次非阻塞的收敛判断，
  // 绝不等候推理：会话尚未收敛（或没有在途会话）时返回 0。交付内容为该次会话的轮次事件与
  // 终态（见类注释的粒度说明）。调用方通常在传输循环里按自己的节奏调用它，因此“什么时候把
  // 结果送出去”由传输层决定，而不是由本类偷偷起一个线程。
  // 创建新会话之前本类会自己先调用一次：否则上一次会话的结果会被新会话顶掉。
  std::size_t deliver_settled();

  // 连接是否仍然打开。未知连接返回 false。
  bool connection_open(ConnectionId id) const;

  // 单条连接的状态快照。未知连接返回 known=false 的默认值。
  GatewayConnectionStats connection_stats(ConnectionId id) const;

  // 入口级状态快照：连接账目、在途会话身份与监督器状态。
  GatewayStatus status() const;

 private:
  // 幂等记录：原请求（用于重试比对）+ 是否已定局 + 已定局的响应。
  struct RequestRecord {
    protocol::ControlRequest request;
    bool completed = false;
    protocol::ControlResponse response;
  };

  // 一条连接的入站解帧器、账目与两条有界待发队列。待发内容是**完整的一行**（含换行符），
  // 因此 flush() 只需按字节截断，不需要再判断消息边界。
  struct Connection {
    // 解帧器的上限是构造性质：连接一旦建立，帧长上限就不再变化，因此这里用构造而不是赋值
    // 来设置它（解帧器持有 const 上限，赋值会让“上限中途被改”成为可能）。
    // 账目里的 open 在同名统计结构中的默认值是“未知连接不可用”，而一条刚建立的连接是打开的，
    // 因此这里显式打开它——复用同一个字段定义不能顺带复用它的默认值。
    Connection(ConnectionId connection_id, std::size_t max_frame_bytes)
        : id(connection_id), framer(max_frame_bytes) {
      ledger.open = true;
    }

    ConnectionId id = kInvalidConnectionId;
    NdjsonFramer framer;
    std::string pending_control;
    std::string pending_data;
    // 连接账目。对外快照直接取自它，因此不存在第二份字段定义。
    GatewayConnectionStats ledger;
  };

  // 在途会话：本入口为它保留归属信息，使“会话归谁发起”与“它是不是被停止的”都可判定。
  // owner 为 kInvalidConnectionId 表示发起连接已经断开：会话照常收敛，但事件不再投递。
  struct InFlightSession {
    ConnectionId owner = kInvalidConnectionId;
    std::uint64_t session_sequence = 0;
    std::string work_id;
    std::string session_id;
    std::string request_id;
    // 路由模式下事件可能分多次 poll 到达；序号在本次会话内连续递增，不因分批交付而重置。
    std::uint64_t next_sequence = 0;
    // 本次会话开始之前监督器的“已取消会话数”。终态算不算取消以这个计数的增量为准，而不是
    // 以本入口自己的标志为准：退出（shutdown）同样会受理停止，而那个停止不是通过 cancel
    // 请求进来的，只看本入口的标志会把它漏掉。
    std::uint64_t cancelled_before = 0;
  };

  Connection* find_connection(ConnectionId id);
  const Connection* find_connection(ConnectionId id) const;

  // 处理一帧（已去掉换行符的完整文本）。这是从字节到语义的唯一入口。
  void handle_frame(Connection& connection, const std::string& frame);
  // 处理解码失败的帧：能取出合法 request_id 时回结构化错误并保留连接，否则关闭连接。
  void handle_undecodable(Connection& connection, const std::string& frame,
                          const domain::Error& error);
  // 处理已通过协议校验的请求：先做幂等判定，再分派到具体操作。
  void handle_request(Connection& connection, const protocol::ControlRequest& request);
  void execute_start(Connection& connection, const protocol::ControlRequest& request,
                     RequestRecord& record);
  void execute_query(Connection& connection, const protocol::ControlRequest& request,
                     RequestRecord& record);
  void execute_cancel(Connection& connection, const protocol::ControlRequest& request,
                      RequestRecord& record);
  void execute_exit(Connection& connection, const protocol::ControlRequest& request,
                    RequestRecord& record);

  // 交付一次会话的事件与终态。前置：槽位已经收敛。记录与本次会话序号不一致时不产生任何
  // 事件——把上一次会话的结果当成本次输出，正是“旧结果污染新会话”的来源。
  std::size_t emit_session_events(const InFlightSession& session);

  // 路由模式下的四个执行入口。它们复用同一个 RequestRecord 幂等表与连接队列，只把执行体
  // 换成 IControlRoute；返回值语义与本地入口一致。
  void execute_routed(Connection& connection, const protocol::ControlRequest& request,
                      RequestRecord& record);
  // 路由模式下取回远端数据事件并投入连接队列；终态事件会关闭本次在途会话。
  std::size_t deliver_routed_events();

  // 路由模式的线程池：提交、回收和分发。
  struct RouteTask {
    ConnectionId owner = kInvalidConnectionId;
    protocol::ControlRequest request;
  };
  struct RouteCompletion {
    ConnectionId owner = kInvalidConnectionId;
    protocol::ControlRequest request;
    domain::Result<protocol::ControlResponse> result;
  };
  void StartRouteWorkers();
  void StopRouteWorkers() noexcept;
  domain::OperationResult SubmitRouteTask(ConnectionId owner,
                                          const protocol::ControlRequest& request);
  void DrainRouteCompletions();
  void HandleRouteCompletion(RouteCompletion completion);

  // 标记一条连接已关闭并记录原因（已关闭时保留首个原因），并把解帧器里尚未成帧的尾部字节
  // 计入账目。它不回收记录，也不触发取消策略：那条策略属于“对端断开”，由 close_connection()
  // 判定。
  void mark_closed(Connection& connection, GatewayCloseReason reason);

  // 入队一条控制响应；放不下（达到控制缓冲上限）或编码失败时改为关闭连接。
  void enqueue_control(Connection& connection, const protocol::ControlResponse& response);
  // 把一条数据事件投递给连接；没有接收者时计入丢弃账目并返回 0。
  std::size_t enqueue_event_for(ConnectionId owner, const protocol::DataEvent& event);

  // 回收“已关闭且两条待发队列都已排空”的连接记录。返回回收条数。
  std::size_t reap_finished_connections();
  // 打开连接数（不含已关闭的记录）。
  std::size_t open_connection_count() const;

  runtime::Supervisor* supervisor_ = nullptr;
  runtime::ISessionRunSource* run_source_ = nullptr;
  IControlRoute* route_ = nullptr;
  GatewayConfig config_;
  std::vector<std::thread> route_workers_;
  std::mutex route_mutex_;
  std::condition_variable route_condition_;
  std::deque<RouteTask> route_tasks_;
  std::deque<RouteCompletion> route_completions_;
  bool route_stopping_ = false;
  domain::Error route_worker_error_{};
  std::map<std::string, RequestRecord> requests_;
  std::map<ConnectionId, Connection> connections_;
  ConnectionId next_connection_id_ = 1;
  std::optional<InFlightSession> inflight_;
  bool closed_ = false;
  // 累计账目。除动态字段（连接数、在途会话、监督器快照）外的字段在 status() 中原样带出，
  // 因此不需要为同一批计数再维护一份平行结构。
  GatewayStatus totals_;
};

}  // namespace nexweave::gateway
