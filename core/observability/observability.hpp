// 可观测性 v1 内存记录：拥有字符串和属性副本，不打开文件、时钟、线程或设备。
// 持久化记录器由调用方创建和关闭；本层失败没有部分文件或外部状态，资源随对象释放。
// C++17，只读可并发，修改须外部同步。分配失败抛 std::bad_alloc，不伪装成业务错误。
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "../domain/error.hpp"

namespace nexweave::observability {
// 一次完整运行的清单：run_id 关联本地记录，git_commit/config_hash/input_hash 关联可复现实验。
// compiler/cmake/runtime/driver/model/device/profile/command 由启动方如实填写，无硬件项用 "none"。
// 时间由调用方提供 ISO-8601 文本；这里只校验非空，不解析日历、不推断时间先后。
struct RunManifest {
  std::uint32_t version = 1;
  std::string run_id;
  std::string git_commit;
  std::string compiler;
  std::string cmake;
  std::string runtime;
  std::string driver;
  std::string model;
  std::string config_hash;
  std::string input_hash;
  std::string device;
  std::string profile;
  std::string command;
  std::string start_time;
  std::string end_time;
};

// 离散事件；name 是非空稳定事件名，attributes 拥有额外 UTF-8 文本字段。
// sequence 应在 request/session/generation 内单调；timestamp_ms 是从运行开始的单调耗时。
// 生产方拥有时钟与序号，校验器不维护历史；0 可表示首事件/起点，接收方负责排序和代际过滤。
struct ObservationEvent {
  std::uint32_t version = 1;
  std::string request_id;
  std::string session_id;
  std::uint64_t generation = 0;
  std::uint64_t sequence = 0;
  std::uint64_t timestamp_ms = 0;
  std::string name;
  std::map<std::string, std::string> attributes;
};

// 一个带量纲的指标样本；name/unit 非空，value 为有限实数，可以为负或小数。
// timestamp_ms 与事件使用相同起点；此类型不把样本汇总成百分位，不校验量纲换算。
struct MetricRecord {
  std::uint32_t version = 1;
  std::string request_id;
  std::string session_id;
  std::uint64_t generation = 0;
  std::uint64_t timestamp_ms = 0;
  std::string name;
  std::string unit;
  double value = 0.0;
};

// 三种校验均先检查 version=1，再检查必填字段；记录不变，失败返回结构化错误。
// 空字段/非法关联标识返回 kMissingField；未知版本和非有限指标 kInvalidInput。
// session_id 可为空，request_id 必须满足领域规则；成功不表示时间/哈希已由外部证据核实。
domain::OperationResult validate_manifest(const RunManifest& manifest);
domain::OperationResult validate_event(const ObservationEvent& event);
domain::OperationResult validate_metric(const MetricRecord& metric);

// 编码前校验完整记录；非法 UTF-8 返回 kInvalidInput。解码采用 v1 固定字段集合，
// 缺失/未知字段、整数的负数/浮点/布尔/溢出返回 kInvalidInput；合法记录保留全部字段。
// 指标 value 接受 JSON 数值而不接受布尔；NaN/Inf 拒绝。失败无可消费值，不改变输入。
domain::Result<std::string> encode_manifest(const RunManifest& manifest);
domain::Result<RunManifest> decode_manifest(std::string_view input);
domain::Result<std::string> encode_event(const ObservationEvent& event);
domain::Result<ObservationEvent> decode_event(std::string_view input);
domain::Result<std::string> encode_metric(const MetricRecord& metric);
domain::Result<MetricRecord> decode_metric(std::string_view input);
}  // namespace nexweave::observability
