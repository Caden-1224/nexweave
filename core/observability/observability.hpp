#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include "../domain/error.hpp"
namespace nexweave::observability {
// 运行清单把一次运行与源码、环境、配置和输入绑定；字段由启动方填写，序列化不访问外部资源。
// 所有字符串必须非空且 version=1；时间采用 ISO-8601 文本。该值对象可跨线程只读共享，写入由调用方同步。
struct RunManifest { std::uint32_t version=1; std::string run_id,git_commit,compiler,cmake,runtime,driver,model,config_hash,input_hash,device,profile,command,start_time,end_time; };
// 事件是离散可观察事实，sequence 在同一 request/session/generation 内单调递增；attributes 仅承载稳定文本。
struct ObservationEvent { std::uint32_t version=1; std::string request_id,session_id; std::uint64_t generation=0,sequence=0,timestamp_ms=0; std::string name; std::map<std::string,std::string> attributes; };
// 指标记录一个数值样本；unit 明确量纲，value 必须为有限值。记录不拥有计时器或文件，写入者负责持久化。
struct MetricRecord { std::uint32_t version=1; std::string request_id,session_id; std::uint64_t generation=0,timestamp_ms=0; std::string name,unit; double value=0.0; };
domain::OperationResult validate_manifest(const RunManifest&) noexcept;
domain::OperationResult validate_event(const ObservationEvent&) noexcept;
domain::OperationResult validate_metric(const MetricRecord&) noexcept;
domain::Result<std::string> encode_manifest(const RunManifest&);
domain::Result<RunManifest> decode_manifest(std::string_view);
domain::Result<std::string> encode_event(const ObservationEvent&);
domain::Result<ObservationEvent> decode_event(std::string_view);
domain::Result<std::string> encode_metric(const MetricRecord&);
domain::Result<MetricRecord> decode_metric(std::string_view);
}
