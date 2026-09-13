#include "gateway.hpp"

#include <algorithm>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <utility>

#include "../domain/identifiers.hpp"

namespace nexweave::gateway {
namespace {

using domain::ErrorCode;
using domain::OperationResult;

// 本入口交付的数据事件固定的回答代际。它不代表会话内部的代际切换：事件只属于被监督的那
// 一次会话，其身份由 work_id 与 session_id 承载；取消与重开由会话内部推进，不由入口表达。
// 取值 1 而不是 0，使“有代际”与“未绑定代际”在证据里可以区分。
constexpr std::uint64_t kSessionGeneration = 1;

// 稳定事实行：空格分隔的 key=value，键顺序由调用点固定，值不含空格。
// 之所以要有固定形状：v1 的 ControlResponse 只有错误语义，没有状态载荷字段；本入口把控制
// 事实以可解析的一行交付，而不是把自由文本塞进 message。把它提升为结构化字段需要控制面
// 契约的新版本，属于版本化决策，本实现不私改 v1 的字段集合。
std::string facts(std::initializer_list<std::pair<const char*, std::string>> items) {
  std::string out;
  for (const auto& item : items) {
    if (!out.empty()) {
      out.push_back(' ');
    }
    out.append(item.first);
    out.push_back('=');
    out.append(item.second);
  }
  return out;
}

std::string flag(bool value) {
  return value ? "1" : "0";
}

OperationResult ok_with_fact_line(std::string message) {
  // 刻意把事实行放进 message：v1 的 ControlResponse 没有状态载荷字段。这是对既有契约的迁就，
  // 不是把 message 当通用数据通道——事实行的语法在头文件里作为契约写明，且只出现在本入口
  // 自己产生的响应上；分派前的拒绝仍然沿用协议层的自由文本诊断。
  OperationResult result = OperationResult::success();
  result.error.message = std::move(message);
  return result;
}

protocol::ControlResponse make_response(std::string request_id, OperationResult result,
                                        bool replayed = false) {
  protocol::ControlResponse response;
  response.request_id = std::move(request_id);
  response.result = std::move(result);
  response.replayed = replayed;
  return response;
}

// 从无法解码的帧里尽力取出 request_id。只有它合法时，v1 才允许我们回一条可寻址的响应；
// 取不到就意味着这次输入没有答复对象。解析失败（坏 JSON）与字段缺失都返回空串。
std::string attributable_request_id(const std::string& frame) {
  const nlohmann::json json = nlohmann::json::parse(frame.begin(), frame.end(), nullptr, false);
  if (json.is_discarded() || !json.is_object()) {
    return std::string();
  }
  const auto found = json.find("request_id");
  if (found == json.end() || !found->is_string()) {
    return std::string();
  }
  const std::string value = found->get<std::string>();
  return domain::is_valid_request_id(value) ? value : std::string();
}

// 一轮的局部失败。它与会话级终态是两件事：单轮失败只说明那一轮没成，会话在常驻输入下仍然
// 可以继续；把它升级成会话失败会掩盖“被打断的一轮”与“整个会话跑不下去”的区别。
domain::Error turn_error(const runtime::SessionTurnResult& turn) {
  if (!turn.error.ok()) {
    return turn.error;
  }
  if (turn.cancelled) {
    return domain::Error{ErrorCode::kCancelled, "该轮已停止"};
  }
  if (turn.backpressure) {
    return domain::Error{ErrorCode::kBackendFailure, "该轮因播放缓冲达到上限而失败"};
  }
  return domain::Error{};
}

// 会话终态的判定顺序与监督器的记账顺序一致，使“事件里的结论”和“监督器计数”不会互相矛盾：
// 建立失败最优先（会话根本没跑），其次是清理失败（资源状态不明，监督器也把它记成失败），
// 然后是取消，最后才轮到执行失败与成功。
domain::Error terminal_error(const runtime::SupervisorStatus& snapshot,
                             std::uint64_t cancelled_before) {
  if (!snapshot.last_start_error.ok()) {
    return snapshot.last_start_error;
  }
  if (!snapshot.last_cleanup_error.ok()) {
    return snapshot.last_cleanup_error;
  }
  if (snapshot.sessions_cancelled > cancelled_before) {
    return domain::Error{ErrorCode::kCancelled, "会话已按控制请求停止"};
  }
  if (!snapshot.last_run_error.ok()) {
    return snapshot.last_run_error;
  }
  return domain::Error{};
}

// 从待发队列头部取走最多 budget 字节并追加到 out。
std::size_t drain_queue(std::string& pending, std::string& out, std::size_t budget) {
  const std::size_t take = std::min(budget, pending.size());
  out.append(pending, 0, take);
  pending.erase(0, take);
  return take;
}

}  // namespace

const char* to_string(GatewayCloseReason reason) noexcept {
  switch (reason) {
    case GatewayCloseReason::kNone:
      return "none";
    case GatewayCloseReason::kPeerClosed:
      return "peer_closed";
    case GatewayCloseReason::kProtocolViolation:
      return "protocol_violation";
    case GatewayCloseReason::kOversizedFrame:
      return "oversized_frame";
    case GatewayCloseReason::kSlowClient:
      return "slow_client";
    case GatewayCloseReason::kExited:
      return "exited";
  }
  return "";
}

domain::OperationResult validate_gateway_config(const GatewayConfig& config) {
  if (config.max_connections == 0 || config.max_frame_bytes == 0 ||
      config.max_pending_control_bytes == 0 || config.max_pending_data_bytes == 0 ||
      config.max_idempotency_records == 0) {
    // 容量为 0 不是“不限制”，而是“一条都放不下”：它会让每条连接在第一次发送时就被判定为
    // 慢客户端。拒绝这种配置而不是替调用方猜意图。
    return OperationResult::failure(ErrorCode::kInvalidInput, "入口容量字段必须为正");
  }
  if (config.cancel_wait_budget.count() < 0 || config.exit_wait_budget.count() < 0) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "入口等待预算不能为负");
  }
  return OperationResult::success();
}

Gateway::Gateway(runtime::Supervisor& supervisor, runtime::ISessionRunSource& run_source,
                 GatewayConfig config)
    : supervisor_(supervisor), run_source_(run_source), config_(config) {
  // 与监督器配置同一约定：构造不抛业务异常，非法配置整份回退到默认值。希望把配置错误当
  // 错误处理的调用方应先调用 validate_gateway_config()。
  if (!validate_gateway_config(config_).ok()) {
    config_ = GatewayConfig{};
  }
}

Gateway::~Gateway() = default;

Gateway::Connection* Gateway::find_connection(ConnectionId id) {
  const auto found = connections_.find(id);
  return found == connections_.end() ? nullptr : &found->second;
}

const Gateway::Connection* Gateway::find_connection(ConnectionId id) const {
  const auto found = connections_.find(id);
  return found == connections_.end() ? nullptr : &found->second;
}

std::size_t Gateway::open_connection_count() const {
  std::size_t count = 0;
  for (const auto& entry : connections_) {
    if (entry.second.ledger.open) {
      ++count;
    }
  }
  return count;
}

std::size_t Gateway::reap_finished_connections() {
  std::size_t reaped = 0;
  auto entry = connections_.begin();
  while (entry != connections_.end()) {
    const Connection& connection = entry->second;
    // 回收条件是“已经关闭**且**待发字节已经排空”：只要还有没被取走的响应，记录就要留着，
    // 否则退出响应会随着记录一起消失。
    if (!connection.ledger.open && connection.pending_control.empty() &&
        connection.pending_data.empty()) {
      entry = connections_.erase(entry);
      ++reaped;
    } else {
      ++entry;
    }
  }
  totals_.connections_reaped += reaped;
  return reaped;
}

ConnectionId Gateway::open_connection() {
  if (closed_) {
    return kInvalidConnectionId;
  }
  reap_finished_connections();
  if (open_connection_count() >= config_.max_connections) {
    return kInvalidConnectionId;
  }
  ConnectionId id = next_connection_id_++;
  connections_.emplace(id, Connection(id, config_.max_frame_bytes));
  ++totals_.connections_opened;
  return id;
}

void Gateway::mark_closed(Connection& connection, GatewayCloseReason reason) {
  if (!connection.ledger.open) {
    // 首个原因被保留：一次关闭只有一个原因，后来的调用不得改写它，否则证据里会留下
    // “先因为超长帧关闭、又变成对端断开”这种自相矛盾的记录。
    return;
  }
  connection.ledger.open = false;
  connection.ledger.close_reason = reason;
  // 关闭时仍未成帧的尾部字节：它们没有构成任何请求，既不能当帧处理，也不该静默消失。
  const std::size_t partial = connection.framer.partial_bytes();
  connection.ledger.discarded_partial_bytes = partial;
  totals_.discarded_partial_bytes += partial;
  if (reason == GatewayCloseReason::kSlowClient) {
    ++totals_.slow_client_closes;
  } else if (reason == GatewayCloseReason::kProtocolViolation) {
    ++totals_.protocol_violation_closes;
  }
}

void Gateway::close_connection(ConnectionId id, GatewayCloseReason reason) {
  Connection* connection = find_connection(id);
  if (connection == nullptr) {
    return;
  }
  // 断连策略与“这条连接此前是否已被服务端关闭”无关。慢客户端、超长帧与无法归属的非法输入
  // 都会由服务端先关闭连接；如果那时不评估这条策略，会话就会带着一个永远没人读取的输出继续
  // 占用设备唯一的槽位，直到它自己收敛。
  if (reason == GatewayCloseReason::kPeerClosed && inflight_.has_value() &&
      inflight_->owner == id) {
    // 断连即受理取消：这次会话的输出已经没有接收者，继续跑下去只会占用设备唯一的槽位。
    // 只受理、不等待——断开是传输层的事件，不该阻塞在清理上；清理结果仍由监督器回答。
    const auto outcome =
        supervisor_.cancel(inflight_->session_sequence, config_.cancel_wait_budget);
    if (outcome.accepted) {
      ++totals_.disconnects_cancelling_session;
    }
    // 会话照常收敛，但不再有投递目标；终态因此计入“没有接收者”的账目。
    inflight_->owner = kInvalidConnectionId;
  }
  mark_closed(*connection, reason);
}

bool Gateway::feed(ConnectionId id, std::string_view bytes) {
  Connection* connection = find_connection(id);
  if (connection == nullptr || !connection->ledger.open) {
    return false;
  }
  if (closed_) {
    // 运行时已经退出。退出会关闭全部连接并拒绝新连接，因此这里只是不变量兜底。
    mark_closed(*connection, GatewayCloseReason::kExited);
    return false;
  }

  std::vector<std::string> frames;
  const FrameVerdict verdict = connection->framer.feed(bytes, frames);
  if (verdict == FrameVerdict::kOversized) {
    // 本批输入整体作废：超长帧之后的字节归属不明，先执行批内请求再退出会让“哪些请求真的
    // 生效了”无法解释。因此一帧都不处理，只记录并关闭。
    ++totals_.oversized_frames;
    mark_closed(*connection, GatewayCloseReason::kOversizedFrame);
    return false;
  }
  for (std::size_t index = 0; index < frames.size(); ++index) {
    handle_frame(*connection, frames.at(index));
    if (!connection->ledger.open) {
      return false;
    }
  }
  return true;
}

std::size_t Gateway::flush(ConnectionId id, std::string& out, std::size_t budget) {
  Connection* connection = find_connection(id);
  if (connection == nullptr || budget == 0) {
    return 0;
  }
  std::size_t taken = drain_queue(connection->pending_control, out, budget);
  if (taken < budget) {
    taken += drain_queue(connection->pending_data, out, budget - taken);
  }
  connection->ledger.bytes_written += taken;
  return taken;
}

void Gateway::handle_frame(Connection& connection, const std::string& frame) {
  ++connection.ledger.frames_received;
  const auto decoded = protocol::decode_request(frame);
  if (!decoded.ok()) {
    handle_undecodable(connection, frame, decoded.error);
    return;
  }
  handle_request(connection, *decoded.value);
}

void Gateway::handle_undecodable(Connection& connection, const std::string& frame,
                                 const domain::Error& error) {
  const std::string request_id = attributable_request_id(frame);
  if (request_id.empty()) {
    // v1 不允许发送没有 request_id 的响应：这次输入没有可寻址的答复对象，因此记录原因并
    // 关闭连接，而不是伪造一个身份去回复——伪造的身份会污染客户端的幂等与重放判定。
    ++totals_.malformed_frames;
    mark_closed(connection, GatewayCloseReason::kProtocolViolation);
    return;
  }
  // 能归属到请求的非法输入得到结构化错误，且连接保持可用：参数写错是客户端的正常错误，
  // 不该让整条连接失效。
  ++totals_.requests_rejected;
  enqueue_control(connection,
                  make_response(request_id, OperationResult::failure(error.code, error.message)));
}

void Gateway::handle_request(Connection& connection, const protocol::ControlRequest& request) {
  const auto existing = requests_.find(request.request_id);
  if (existing != requests_.end()) {
    // 重复请求的三种情形由协议层的校验给出结论，本类不另写一份字段比较。
    const auto retry = protocol::validate_retry(existing->second.request, request);
    if (!retry.ok()) {
      ++totals_.requests_rejected;
      const auto rejected = OperationResult::failure(retry.error.code, retry.error.message);
      enqueue_control(connection, make_response(request.request_id, rejected));
      return;
    }
    if (!existing->second.completed) {
      ++totals_.requests_rejected;
      enqueue_control(connection,
                      make_response(request.request_id,
                                    OperationResult::failure(ErrorCode::kAlreadyCompleted,
                                                             "同一 request_id 正在执行")));
      return;
    }
    const auto replayed =
        protocol::replay_response(existing->second.request, request, existing->second.response);
    if (!replayed.ok()) {
      ++totals_.requests_rejected;
      enqueue_control(connection,
                      make_response(request.request_id,
                                    OperationResult::failure(replayed.error.code,
                                                             replayed.error.message)));
      return;
    }
    ++totals_.requests_replayed;
    enqueue_control(connection, *replayed.value);
    return;
  }

  if (requests_.size() >= config_.max_idempotency_records) {
    // 记录容量是显式上限。淘汰旧记录会让一次重试被当成新请求，从而重复启动任务——那正是
    // 幂等语义要防的事，因此这里拒绝新的身份，而不是腾地方。
    ++totals_.idempotency_rejections;
    ++totals_.requests_rejected;
    enqueue_control(connection,
                    make_response(request.request_id,
                                  OperationResult::failure(ErrorCode::kBackendFailure,
                                                           "幂等记录容量已满，拒绝受理新请求")));
    return;
  }

  RequestRecord record;
  record.request = request;
  // map 的引用在插入后保持有效，因此下面的执行函数可以安全地长期持有这条记录。
  RequestRecord& stored = requests_.emplace(request.request_id, std::move(record)).first->second;
  if (request.operation == "start") {
    execute_start(connection, request, stored);
  } else if (request.operation == "query") {
    execute_query(connection, request, stored);
  } else if (request.operation == "cancel") {
    execute_cancel(connection, request, stored);
  } else {
    execute_exit(connection, request, stored);
  }
}

void Gateway::execute_start(Connection& connection, const protocol::ControlRequest& request,
                            RequestRecord& record) {
  // 先把上一次会话还没交付的结果交付掉。上一次会话必须在槽位空闲时才可能被新的创建取代，
  // 而“空闲”只说明它已经收敛、不说明它的终态已经送出去；直接覆盖在途记录会丢掉那批事件，
  // 并让那一次创建永远停在“执行中”——它的重试会一直得到 kAlreadyCompleted。
  deliver_settled();

  // 取消计数基线必须在 start() 之前取：会话可能在 start() 返回之后立刻收敛，晚取的基线会
  // 把这次会话自己的取消算进“之前”。
  const std::uint64_t cancelled_before = supervisor_.status().sessions_cancelled;

  runtime::SupervisorSessionSpec spec;
  spec.work_id = request.work_id;
  spec.session_id = request.session_id;
  spec.request_id = request.request_id;
  const auto outcome = supervisor_.start(spec);
  if (!outcome.ok()) {
    // 忙碌、已退出、槽位不可用、建立超时与拥有者的原始错误都在这里如实回给客户端。
    // 失败没有启动任何会话，因此这条请求当场定局。
    ++totals_.requests_rejected;
    const auto response = make_response(
        request.request_id, OperationResult::failure(outcome.error.code, outcome.error.message));
    record.completed = true;
    record.response = response;
    enqueue_control(connection, response);
    return;
  }

  const runtime::SupervisorStatus snapshot = supervisor_.status();
  InFlightSession session;
  session.owner = connection.id;
  session.session_sequence = snapshot.session_sequence;
  session.work_id = snapshot.work_id;
  session.session_id = snapshot.session_id;
  session.request_id = request.request_id;
  session.cancelled_before = cancelled_before;
  inflight_ = std::move(session);

  ++totals_.requests_accepted;
  const auto response = make_response(
      request.request_id,
      ok_with_fact_line(facts({{"work_id", snapshot.work_id},
                             {"session_id", snapshot.session_id},
                             {"session_sequence", std::to_string(snapshot.session_sequence)}})));
  // 刻意**不**标记为已定局：会话还在执行，此刻它的终态尚未产生。重复的同 id 请求因此在
  // 收敛之前得到 kAlreadyCompleted，收敛之后重放这条受理响应。
  record.response = response;
  record.completed = false;
  enqueue_control(connection, response);
}

void Gateway::execute_query(Connection& connection, const protocol::ControlRequest& request,
                            RequestRecord& record) {
  const runtime::SupervisorStatus snapshot = supervisor_.status();
  const auto response = make_response(
      request.request_id,
      ok_with_fact_line(facts({{"state", runtime::to_string(snapshot.state)},
                             {"session_sequence", std::to_string(snapshot.session_sequence)},
                             {"work_id", snapshot.work_id},
                             {"session_id", snapshot.session_id},
                             {"cancel_accepted", flag(snapshot.cancel_accepted)}})));
  record.completed = true;
  record.response = response;
  ++totals_.requests_accepted;
  enqueue_control(connection, response);
}

void Gateway::execute_cancel(Connection& connection, const protocol::ControlRequest& request,
                             RequestRecord& record) {
  const runtime::SupervisorStatus snapshot = supervisor_.status();
  if (!request.work_id.empty() && request.work_id != snapshot.work_id) {
    // 取消带上了目标身份时，身份不符就明确拒绝：一次针对旧会话的迟到取消不该打到当前
    // 会话上，这比“尽力而为地取消点什么”安全得多。
    ++totals_.requests_rejected;
    const auto rejected = make_response(
        request.request_id,
        OperationResult::failure(ErrorCode::kInvalidInput, "取消目标不是当前会话"));
    record.completed = true;
    record.response = rejected;
    enqueue_control(connection, rejected);
    return;
  }

  const std::uint64_t expected = inflight_.has_value() ? inflight_->session_sequence : 0;
  // 请求自带的 deadline 收紧配置里的等待预算：一次配置过大的等待不该超出客户端自己声明的
  // 预算。两个预算都非负，取较小者即可。
  const auto budget = std::min(config_.cancel_wait_budget, request.deadline);
  const auto outcome = supervisor_.cancel(expected, budget);
  const std::string message = facts({{"accepted", flag(outcome.accepted)},
                                     {"cleanup_completed", flag(outcome.cleanup_completed)},
                                     {"state", runtime::to_string(outcome.state)}});
  // 失败时用事实行替换监督器的诊断文本：受理与静默这两件事必须同时可读，而自由文本会
  // 把机器可读的部分挤掉。监督器的原始诊断仍可从状态快照取到。
  const auto response =
      outcome.error.ok()
          ? make_response(request.request_id, ok_with_fact_line(message))
          : make_response(request.request_id,
                          OperationResult::failure(outcome.error.code, message));
  record.completed = true;
  record.response = response;
  ++totals_.requests_accepted;
  enqueue_control(connection, response);
}

void Gateway::execute_exit(Connection& connection, const protocol::ControlRequest& request,
                           RequestRecord& record) {
  const auto budget = std::min(config_.exit_wait_budget, request.deadline);
  const auto outcome = supervisor_.shutdown(budget);
  // “退没退”看状态，“上一次清理干不干净”看错误：两者由一个响应分别回答，不互相掩盖。
  const bool exited = outcome.state == runtime::SupervisorState::kClosed;
  const std::string message = facts({{"accepted", flag(outcome.accepted)},
                                     {"state", runtime::to_string(outcome.state)},
                                     {"cleanup", flag(outcome.error.ok())}});
  const auto response =
      exited ? make_response(request.request_id, ok_with_fact_line(message))
             : make_response(request.request_id,
                             OperationResult::failure(outcome.error.code, message));
  record.completed = true;
  record.response = response;
  ++totals_.requests_accepted;
  ++totals_.exit_requests;
  enqueue_control(connection, response);

  if (!exited) {
    // 超时说明清理还没结束，运行时**没有**退出。入口因此保持可用，客户端可以用新的
    // request_id 再调一次退出（同一个 request_id 会重放这次超时结果，见类注释）。
    return;
  }
  closed_ = true;
  // 先把还欠客户端的东西交付掉：在途会话的终态要在这里发出，而不是随着关闭一起消失。
  deliver_settled();
  for (auto& entry : connections_) {
    mark_closed(entry.second, GatewayCloseReason::kExited);
  }
}

std::size_t Gateway::deliver_settled() {
  if (!inflight_.has_value()) {
    return 0;
  }
  // 只做一次非阻塞判断：预算为 0 的等待等价于“现在收敛了没有”。它在 kQuiescing 期间为假，
  // 因此事件不会被提前交付——终态与会话证据必须来自同一个已经定局的快照。
  if (!supervisor_.wait_for_slot(std::chrono::milliseconds{0})) {
    return 0;
  }
  return emit_session_events(*inflight_);
}

std::size_t Gateway::emit_session_events(const InFlightSession& session) {
  const runtime::SupervisorStatus snapshot = supervisor_.status();
  // 先清空在途再交付：投递过程不再依赖它，因此重复交付不会重复发出同一批事件。
  inflight_.reset();

  const std::shared_ptr<const runtime::SessionAppRunRecord> record = run_source_.last_run();
  // 事件归属按会话序号核对。序号不一致说明本次会话还没有发布记录（例如建立阶段就失败了），
  // 此时一条事件都不能发——把上一次会话的文本当成本次输出，正是旧结果污染新会话的来源。
  const bool matches =
      record != nullptr && record->spec.session_sequence == session.session_sequence;

  std::size_t emitted = 0;
  // 事件序号在**本次会话内**从 1 开始编号，不延续上一次会话：交付顺序即发生顺序，而“第几条”
  // 只在一次会话内才有意义。lambda 直接读取这个计数器，因此不需要再加一个同义的参数。
  std::uint64_t sequence = 0;
  const auto emit = [&](protocol::DataEvent event) {
    event.request_id = session.request_id;
    event.session_id = session.session_id;
    event.generation = kSessionGeneration;
    event.sequence = sequence;
    return enqueue_event_for(session.owner, event);
  };

  if (matches) {
    for (const auto& turn : record->result.turns) {
      ++sequence;
      if (!turn.text.empty()) {
        protocol::DataEvent token;
        token.type = protocol::DataEventType::kToken;
        token.text = turn.text;
        emitted += emit(std::move(token));
      }
      const domain::Error local = turn_error(turn);
      if (!local.ok()) {
        // 局部失败：end 保持为假，因此它不会与终态混淆。已交付的文本仍然照常给出，
        // 因为那些文本确实产生过。
        ++sequence;
        protocol::DataEvent failure;
        failure.type = protocol::DataEventType::kError;
        failure.error_code = local.code;
        failure.message = local.message;
        failure.end = false;
        emitted += emit(std::move(failure));
      }
    }
  }

  const domain::Error terminal = terminal_error(snapshot, session.cancelled_before);
  ++sequence;
  protocol::DataEvent final_event;
  if (terminal.ok()) {
    final_event.type = protocol::DataEventType::kDone;
    final_event.message = facts({{"state", runtime::to_string(snapshot.state)}});
  } else {
    final_event.type = protocol::DataEventType::kError;
    final_event.error_code = terminal.code;
    final_event.message = terminal.message;
  }
  final_event.end = true;
  emitted += emit(std::move(final_event));

  // 会话已经定局：把这次会话的受理响应标记为已完成，使后续同 id 请求重放它而不是重新执行。
  const auto started = requests_.find(session.request_id);
  if (started != requests_.end()) {
    started->second.completed = true;
  }
  return emitted;
}

std::size_t Gateway::enqueue_event_for(ConnectionId owner, const protocol::DataEvent& event) {
  Connection* connection = find_connection(owner);
  if (connection == nullptr || !connection->ledger.open) {
    // 没有接收者：事件照常产生但不缓存、不重放，只记账。丢弃必须可见，否则“事件少了”
    // 会被误读成“会话没有产生事件”。
    ++totals_.events_dropped_without_receiver;
    return 0;
  }
  const auto encoded = protocol::encode_event_metadata(event);
  if (!encoded.ok()) {
    // 事件因此不可交付（例如文本不是合法 UTF-8）。它不是会话失败，因此不改变终态。
    ++totals_.encode_failures;
    return 0;
  }
  const std::size_t bytes = encoded.value->size() + 1;
  if (connection->pending_data.size() + bytes > config_.max_pending_data_bytes) {
    // 慢客户端：待发数据达到声明上限即关闭。这里不静默丢弃这一条再继续——那会让客户端
    // 收到一段看不出缺口的流。把会话背压回推理侧需要端到端的队列策略，属于后续任务。
    mark_closed(*connection, GatewayCloseReason::kSlowClient);
    ++totals_.events_dropped_without_receiver;
    return 0;
  }
  connection->pending_data.append(*encoded.value);
  connection->pending_data.push_back('\n');
  ++connection->ledger.events_sent;
  ++totals_.data_events;
  return 1;
}

void Gateway::enqueue_control(Connection& connection, const protocol::ControlResponse& response) {
  if (!connection.ledger.open) {
    return;
  }
  const auto encoded = protocol::encode_response(response);
  if (!encoded.ok()) {
    // 响应编码失败说明本入口构造了非法响应，属于实现缺陷；此时不能回一条半成品，只能关闭。
    mark_closed(connection, GatewayCloseReason::kProtocolViolation);
    return;
  }
  const std::size_t bytes = encoded.value->size() + 1;
  if (connection.pending_control.size() + bytes > config_.max_pending_control_bytes) {
    // 控制响应绝不被丢弃：它是客户端继续推进的唯一凭据。放不下就关闭连接，让客户端看到
    // 一次明确的终止，而不是一条被悄悄吞掉的答复。
    mark_closed(connection, GatewayCloseReason::kSlowClient);
    return;
  }
  connection.pending_control.append(*encoded.value);
  connection.pending_control.push_back('\n');
  ++connection.ledger.responses_sent;
  ++totals_.control_responses;
}

bool Gateway::connection_open(ConnectionId id) const {
  const Connection* connection = find_connection(id);
  return connection != nullptr && connection->ledger.open;
}

GatewayConnectionStats Gateway::connection_stats(ConnectionId id) const {
  const Connection* connection = find_connection(id);
  if (connection == nullptr) {
    return GatewayConnectionStats{};
  }
  // 账目直接取自连接自己持有的那一份，只补上待发字节数（它由队列长度推导，不单独记账）。
  GatewayConnectionStats stats = connection->ledger;
  stats.known = true;
  stats.pending_control_bytes = connection->pending_control.size();
  stats.pending_data_bytes = connection->pending_data.size();
  return stats;
}

GatewayStatus Gateway::status() const {
  GatewayStatus snapshot = totals_;
  snapshot.closed = closed_;
  snapshot.open_connections = open_connection_count();
  snapshot.session_in_flight = inflight_.has_value();
  if (inflight_.has_value()) {
    snapshot.in_flight_session_sequence = inflight_->session_sequence;
    snapshot.in_flight_work_id = inflight_->work_id;
    snapshot.in_flight_session_id = inflight_->session_id;
    snapshot.in_flight_request_id = inflight_->request_id;
  }
  snapshot.supervisor = supervisor_.status();
  return snapshot;
}

}  // namespace nexweave::gateway
