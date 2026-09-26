#include "rk3576_profile.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nexweave::app {

namespace {

using domain::ErrorCode;
using domain::OperationResult;
using domain::Result;

domain::Error MakeError(ErrorCode code, std::string message) {
  return OperationResult::failure(code, std::move(message)).error;
}

// 把配置展开成依赖邻接表并检测结构性错误。输出 start_order 时，音频拥有者必须排在最前；
// 这样适配器节点只能在拥有者显式就绪后启动，从进程启动顺序上阻止两个进程同时争抢设备。
domain::OperationResult BuildStartOrder(const Rk3576ProfileConfig& config,
                                        std::vector<std::size_t>* start_order,
                                        std::size_t* owner_index,
                                        bool* owner_configured) {
  if (start_order == nullptr || owner_index == nullptr || owner_configured == nullptr) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "启动顺序输出参数不能为空");
  }
  if (config.processes.empty()) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "部署配置至少需要一个进程");
  }

  const std::size_t process_count = config.processes.size();
  std::unordered_map<std::string, std::size_t> name_to_index;
  name_to_index.reserve(process_count);
  for (std::size_t index = 0; index < process_count; ++index) {
    const Rk3576ProcessSpec& spec = config.processes[index];
    if (spec.name.empty()) {
      return OperationResult::failure(ErrorCode::kInvalidInput, "部署进程角色名不能为空");
    }
    if (name_to_index.find(spec.name) != name_to_index.end()) {
      return OperationResult::failure(ErrorCode::kInvalidInput,
                                      "部署进程角色名必须唯一: " + spec.name);
    }
    name_to_index.emplace(spec.name, index);

    const OperationResult process_valid =
        runtime::validate_child_process_spec(spec.process);
    if (!process_valid.ok()) {
      return process_valid;
    }
    const OperationResult lifecycle_valid =
        runtime::validate_child_process_config(spec.lifecycle);
    if (!lifecycle_valid.ok()) {
      return lifecycle_valid;
    }
    if (spec.owns_audio_device && spec.uses_audio_adapter) {
      return OperationResult::failure(ErrorCode::kInvalidInput,
                                      "音频设备拥有者不能同时声明为适配器消费者");
    }
  }

  std::size_t found_owner_index = 0;
  bool found_owner = false;
  for (std::size_t index = 0; index < process_count; ++index) {
    if (!config.processes[index].owns_audio_device) {
      continue;
    }
    if (found_owner) {
      return OperationResult::failure(ErrorCode::kInvalidInput,
                                      "部署配置只能有一个音频设备拥有者");
    }
    found_owner = true;
    found_owner_index = index;
  }
  if (!found_owner) {
    return OperationResult::failure(ErrorCode::kInvalidInput,
                                    "部署配置必须声明一个音频设备拥有者");
  }
  if (!config.processes[found_owner_index].start_after.empty()) {
    return OperationResult::failure(ErrorCode::kInvalidInput,
                                    "音频设备拥有者不能依赖其他节点");
  }

  std::vector<std::vector<std::size_t>> dependencies(process_count);
  for (std::size_t index = 0; index < process_count; ++index) {
    const Rk3576ProcessSpec& spec = config.processes[index];
    dependencies[index].reserve(spec.start_after.size());
    for (const std::string& dependency_name : spec.start_after) {
      const auto found = name_to_index.find(dependency_name);
      if (found == name_to_index.end()) {
        return OperationResult::failure(
            ErrorCode::kInvalidInput,
            "启动依赖不存在: " + spec.name + " -> " + dependency_name);
      }
      if (found->second == index) {
        return OperationResult::failure(ErrorCode::kInvalidInput,
                                        "启动依赖不能自引用: " + spec.name);
      }
      dependencies[index].push_back(found->second);
    }
  }

  std::vector<int> visit_state(process_count, 0);
  bool cycle_detected = false;
  std::vector<std::size_t> order;
  order.reserve(process_count);
  std::function<void(std::size_t)> visit = [&](std::size_t index) {
    if (visit_state[index] == 1) {
      cycle_detected = true;
      return;
    }
    if (visit_state[index] == 2) {
      return;
    }
    visit_state[index] = 1;
    for (const std::size_t dependency : dependencies[index]) {
      visit(dependency);
    }
    visit_state[index] = 2;
    order.push_back(index);
  };

  // 先访问拥有者，保证没有依赖的拥有者出现在所有独立节点之前；随后按配置顺序展开其余边。
  visit(found_owner_index);
  for (std::size_t index = 0; index < process_count; ++index) {
    if (visit_state[index] == 0) {
      visit(index);
    }
  }
  if (cycle_detected || order.size() != process_count) {
    return OperationResult::failure(ErrorCode::kInvalidInput, "启动依赖不能形成环");
  }

  std::vector<bool> reachable(process_count, false);
  std::function<bool(std::size_t, std::size_t)> has_ancestor =
      [&](std::size_t node, std::size_t ancestor) -> bool {
    for (const std::size_t dependency : dependencies[node]) {
      if (dependency == ancestor) {
        return true;
      }
      if (!reachable[dependency]) {
        reachable[dependency] = true;
        if (has_ancestor(dependency, ancestor)) {
          return true;
        }
      }
    }
    return false;
  };
  for (std::size_t index = 0; index < process_count; ++index) {
    if (!config.processes[index].uses_audio_adapter) {
      continue;
    }
    std::fill(reachable.begin(), reachable.end(), false);
    if (index == found_owner_index || !has_ancestor(index, found_owner_index)) {
      return OperationResult::failure(
          ErrorCode::kInvalidInput,
          "音频适配器节点必须在传递依赖上位于音频拥有者之后: " +
              config.processes[index].name);
    }
  }

  *start_order = std::move(order);
  *owner_index = found_owner_index;
  *owner_configured = true;
  return OperationResult::success();
}

domain::Error FirstError(const domain::Error& current, const domain::Error& candidate) {
  if (!current.ok()) {
    return current;
  }
  if (!candidate.ok()) {
    return candidate;
  }
  return {};
}

}  // namespace

const char* to_string(Rk3576ProfileState state) noexcept {
  switch (state) {
    case Rk3576ProfileState::kIdle:
      return "idle";
    case Rk3576ProfileState::kStarting:
      return "starting";
    case Rk3576ProfileState::kReady:
      return "ready";
    case Rk3576ProfileState::kStopping:
      return "stopping";
    case Rk3576ProfileState::kUnavailable:
      return "unavailable";
  }
  return "";
}

domain::OperationResult validate_rk3576_profile_config(const Rk3576ProfileConfig& config) {
  std::vector<std::size_t> start_order;
  std::size_t owner_index = 0;
  bool owner_configured = false;
  return BuildStartOrder(config, &start_order, &owner_index, &owner_configured);
}

struct Rk3576Profile::Node {
  explicit Node(Rk3576ProcessSpec process_spec)
      : spec(std::move(process_spec)), process(spec.lifecycle) {}

  Rk3576ProcessSpec spec;
  runtime::ChildProcess process;
  runtime::ChildProcessIdentity identity{};
  bool started = false;
  bool ready = false;
  domain::Error start_error{};
  runtime::ChildProcessExit last_exit{};
};

Rk3576Profile::Rk3576Profile(Rk3576ProfileConfig config)
    : config_(std::move(config)) {
  std::vector<std::size_t> start_order;
  std::size_t owner_index = 0;
  bool owner_configured = false;
  const OperationResult valid =
      BuildStartOrder(config_, &start_order, &owner_index, &owner_configured);
  if (!valid.ok()) {
    config_error_ = valid.error;
  } else {
    start_order_ = std::move(start_order);
    audio_owner_index_ = owner_index;
    audio_owner_configured_ = owner_configured;
  }

  nodes_.reserve(config_.processes.size());
  for (const Rk3576ProcessSpec& spec : config_.processes) {
    nodes_.push_back(std::make_unique<Node>(spec));
  }
}

Rk3576Profile::~Rk3576Profile() {
  (void)stop();
}

domain::OperationResult Rk3576Profile::start() {
  if (!config_error_.ok()) {
    return OperationResult{config_error_};
  }
  if (state_ == Rk3576ProfileState::kUnavailable) {
    return OperationResult::failure(ErrorCode::kBackendFailure,
                                    "RK3576 部署 profile 已经不可用，必须先完成清理");
  }
  if (state_ != Rk3576ProfileState::kIdle) {
    return OperationResult::failure(ErrorCode::kBusy,
                                    "RK3576 部署 profile 已经在运行或正在停止");
  }

  ++start_attempts_;
  state_ = Rk3576ProfileState::kStarting;
  for (const std::unique_ptr<Node>& node : nodes_) {
    node->identity = {};
    node->started = false;
    node->ready = false;
    node->start_error = {};
    node->last_exit = {};
  }

  for (const std::size_t index : start_order_) {
    Node& node = *nodes_[index];
    Result<runtime::ChildProcessIdentity> started;
    try {
      started = node.process.start(node.spec.process);
    } catch (...) {
      started = Result<runtime::ChildProcessIdentity>::failure(
          ErrorCode::kBackendFailure, "启动部署子进程时发生异常");
    }
    if (!started.ok()) {
      ++start_failures_;
      node.start_error = started.error;
      last_start_error_ = started.error;
      last_error_ = started.error;

      // 部分启动失败必须回收本轮已经就绪的节点。启动失败自身的 ChildProcess 已在内部回滚
      // 本次管道和进程；这里只处理排在它前面、已经报告过就绪的节点。回滚顺序与启动相反。
      const OperationResult rolled_back = StopStartedNodes();
      if (!rolled_back.ok()) {
        state_ = Rk3576ProfileState::kUnavailable;
        last_error_ = rolled_back.error;
        return rolled_back;
      }
      state_ = Rk3576ProfileState::kIdle;
      return OperationResult{started.error};
    }

    node.identity = started.value.value();
    node.started = true;
    node.ready = true;
    node.start_error = {};
  }

  ++starts_succeeded_;
  state_ = Rk3576ProfileState::kReady;
  last_error_ = {};
  return OperationResult::success();
}

domain::OperationResult Rk3576Profile::StopStartedNodes() noexcept {
  domain::Error first_error{};
  bool failed = false;

  for (auto iterator = start_order_.rbegin(); iterator != start_order_.rend(); ++iterator) {
    Node& node = *nodes_[*iterator];
    runtime::ChildProcessStatus snapshot;
    try {
      snapshot = node.process.status();
    } catch (...) {
      failed = true;
      first_error = FirstError(first_error, MakeError(ErrorCode::kBackendFailure,
                                                      "读取部署子进程状态时发生异常"));
      continue;
    }

    if (!node.started && !snapshot.process_alive) {
      node.identity = {};
      node.ready = false;
      continue;
    }

    try {
      const Result<runtime::ChildProcessExit> stopped = node.process.stop(node.identity);
      if (!stopped.ok()) {
        failed = true;
        first_error = FirstError(first_error, stopped.error);
      } else if (stopped.value.has_value()) {
        node.last_exit = stopped.value.value();
      }
    } catch (...) {
      failed = true;
      first_error = FirstError(first_error, MakeError(ErrorCode::kBackendFailure,
                                                      "停止部署子进程时发生异常"));
    }
    node.identity = {};
    node.started = false;
    node.ready = false;
  }

  if (failed) {
    return OperationResult{first_error};
  }
  return OperationResult::success();
}

domain::OperationResult Rk3576Profile::stop() noexcept {
  ++stop_requests_;

  bool has_live_process = false;
  for (const std::unique_ptr<Node>& node : nodes_) {
    try {
      if (node->process.status().process_alive) {
        has_live_process = true;
        break;
      }
    } catch (...) {
      has_live_process = true;
      break;
    }
  }
  if (state_ == Rk3576ProfileState::kIdle && !has_live_process) {
    ++stops_succeeded_;
    return OperationResult::success();
  }

  state_ = Rk3576ProfileState::kStopping;
  OperationResult stopped;
  try {
    stopped = StopStartedNodes();
  } catch (...) {
    stopped = OperationResult::failure(ErrorCode::kBackendFailure,
                                       "停止 RK3576 部署 profile 时发生异常");
  }
  if (!stopped.ok()) {
    ++stop_failures_;
    state_ = Rk3576ProfileState::kUnavailable;
    last_error_ = stopped.error;
    return stopped;
  }

  ++stops_succeeded_;
  state_ = Rk3576ProfileState::kIdle;
  last_error_ = {};
  return OperationResult::success();
}

domain::OperationResult Rk3576Profile::restart() {
  const OperationResult stopped = stop();
  if (!stopped.ok()) {
    return stopped;
  }
  const OperationResult started = start();
  if (started.ok()) {
    ++restarts_succeeded_;
  }
  return started;
}

bool Rk3576Profile::running() const noexcept {
  return state_ == Rk3576ProfileState::kStarting ||
         state_ == Rk3576ProfileState::kReady;
}

domain::Error Rk3576Profile::last_error() const noexcept {
  if (!last_error_.ok()) {
    return last_error_;
  }
  for (const std::unique_ptr<Node>& node : nodes_) {
    if (!node->start_error.ok()) {
      return node->start_error;
    }
  }
  return last_error_;
}

Rk3576ProfileStatus Rk3576Profile::status() const {
  Rk3576ProfileStatus output;
  output.state = state_;
  output.start_attempts = start_attempts_;
  output.starts_succeeded = starts_succeeded_;
  output.start_failures = start_failures_;
  output.stop_requests = stop_requests_;
  output.stops_succeeded = stops_succeeded_;
  output.stop_failures = stop_failures_;
  output.restarts_succeeded = restarts_succeeded_;
  output.last_start_error = last_start_error_;
  output.last_error = last_error_;

  if (audio_owner_configured_ && audio_owner_index_ < nodes_.size()) {
    output.audio_owner_name = nodes_[audio_owner_index_]->spec.name;
  }

  bool all_ready = true;
  bool owner_ready = false;
  bool has_adapter_consumer = false;
  bool all_adapters_ready = true;
  output.processes.reserve(nodes_.size());
  for (const std::unique_ptr<Node>& node : nodes_) {
    runtime::ChildProcessStatus snapshot;
    try {
      snapshot = node->process.status();
    } catch (...) {
      snapshot = {};
    }

    Rk3576ProcessStatus process_status;
    process_status.name = node->spec.name;
    process_status.owns_audio_device = node->spec.owns_audio_device;
    process_status.uses_audio_adapter = node->spec.uses_audio_adapter;
    process_status.started = node->started;
    process_status.process_state = snapshot.state;
    process_status.identity = snapshot.identity;
    process_status.native_pid = snapshot.native_pid;
    process_status.start_error = node->start_error;
    process_status.last_exit = snapshot.last_exit;
    process_status.ready = node->ready && snapshot.process_alive;
    output.processes.push_back(std::move(process_status));

    if (node->spec.owns_audio_device) {
      owner_ready = node->ready && snapshot.process_alive;
    }
    if (node->spec.uses_audio_adapter) {
      has_adapter_consumer = true;
      if (!node->ready || !snapshot.process_alive) {
        all_adapters_ready = false;
      }
    }
    if (!node->ready || !snapshot.process_alive) {
      all_ready = false;
    }
  }

  output.audio_owner_ready = owner_ready;
  output.audio_adapter_ready = has_adapter_consumer && all_adapters_ready && owner_ready;
  output.running = state_ == Rk3576ProfileState::kReady && all_ready;
  return output;
}

}  // namespace nexweave::app
