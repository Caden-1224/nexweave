// 控制面 RPC（Remote Procedure Call，远程过程调用）的 v1 内存契约。
// 所有函数为纯值操作：不访问网络、时钟、线程或缓存；调用方拥有输入与返回值。
// 只读调用可并发，修改同一对象须外部同步；任何分配失败可抛 std::bad_alloc，
// 栈内临时资源自动清理，校验/重放失败不改变原请求、响应或外部任务。
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "../domain/error.hpp"

namespace nexweave::protocol {
struct ControlRequest {
  std::uint32_t version = 1;
  std::string request_id;
  std::string operation;
  std::string work_id;
  std::string session_id;
  std::uint64_t generation = 0;
  // 从接收方首次接收此 request_id 起算的相对处理预算，单位毫秒，必须 >0。
  // 接收方用 steady_clock 计时，包含排队和处理，不包括首次到达前的网络时间。
  // 重试沿用首次起点和预算，不能延长；跨进程只传时长，不传本机时钟时间点。
  std::chrono::milliseconds deadline{0};
};

// 同一接收方任务上下文内 request_id 唯一标识一次逻辑操作。重复请求所有字段
// 必须一致；冲突返回 kInvalidInput。执行中重复请求返回 kAlreadyCompleted，
// 不启动第二次操作；已完成时调用 replay_response 重放原成功/失败结果。
// 幂等记录由接收方创建、同步和释放，保留到任务关闭；重连不能静默丢失记录。
// 本契约不实现持久化缓存，缓存满/丢失的适配器必须拒绝重试，不能默认当新请求执行。
struct ControlResponse {
  std::uint32_t version = 1;
  std::string request_id;
  domain::OperationResult result{};
  bool replayed = false;
};

// 校验 v1、标识、start/query/cancel/exit 和正预算。可选 work/session 为空合法；
// 空或非法 request_id 返回 kMissingField，其余格式错误 kInvalidInput，非正预算 kTimeout。
// 成功不改变输入。version 和 generation 为不同概念，generation=0 可用于初始控制请求。
domain::OperationResult validate_request(const ControlRequest& request);
// 响应必须包含合法 request_id、v1 和已定义错误码；未知枚举返回 kInvalidInput。
domain::OperationResult validate_response(const ControlResponse& response);

// v1 固定字段集合，不接受未知/缺失字段；整数拒绝负数、浮点、布尔和溢出。
// 解码格式错误返回 kInvalidInput；业务校验保留上面错误码。编码非法 UTF-8 也返回
// kInvalidInput。失败无可消费值；成功往返保留全部字段，响应 ok 必须与错误码一致。
domain::Result<std::string> encode_request(const ControlRequest& request);
domain::Result<ControlRequest> decode_request(std::string_view input);
domain::Result<std::string> encode_response(const ControlResponse& response);
domain::Result<ControlResponse> decode_response(std::string_view input);

// 校验同一逻辑请求的重试；全部字段相同才成功，不比较 JSON 字段顺序。
// 先校验两个请求并保留 kMissingField/kTimeout 等原错误；两个合法请求的字段冲突才返回
// kInvalidInput。不执行任务，不拥有缓存，也不改变代际。
domain::OperationResult validate_retry(const ControlRequest& original, const ControlRequest& retry);
// 接收方提供自首次接收以来的单调时钟耗时；负耗时 kInvalidInput，elapsed>=deadline
// 返回 kTimeout（边界相等即到期）。本函数不睡眠、不取消任务；调用方负责停止接受结果，
// 推进取消代际并释放外部资源，清理完成前不得报告成功或重新启动同一请求。
domain::OperationResult check_deadline(const ControlRequest& request,
                                       std::chrono::milliseconds elapsed);
// 只重放已完成且 request_id 匹配的响应；保留原错误/文本，replayed=true。
// 先保留请求/响应校验错误；合法输入的冲突或错误关联返回 kInvalidInput。
// 执行中的请求不得传入伪造完成响应。
// 已完成结果可在预算耗尽后查询，check_deadline 仅约束未完成操作，不销毁缓存。
domain::Result<ControlResponse> replay_response(const ControlRequest& original,
                                                const ControlRequest& retry,
                                                const ControlResponse& completed);
}  // namespace nexweave::protocol
