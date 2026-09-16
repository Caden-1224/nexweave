// 控制路由接缝：把 Gateway 解析出的控制请求送到本地监督器之外的目的地。
//
// 职责与适用范围
// --------------
// `Gateway` 负责外部字节流的解帧、协议校验、请求幂等、连接账目与有界发送；这些职责不应
// 因为监督器位于另一个进程而重写一遍。本接口把“控制请求最终由谁执行”抽出来：进程内模式
// 由 `Supervisor` 直接执行，多进程模式由一个实现本接口的传输适配器转发。
//
// 外部协议不变
// ------------
// 接口收发的是既有 `protocol::ControlRequest` / `ControlResponse`，不新增字段、不改变版本，
// 也不把 ZeroMQ、进程句柄或 socket 类型暴露给 Session。调用方看到的仍是同一套控制语义：
// 一个请求返回一个响应；失败以结构化 `domain::Error` 报告。
//
// 数据事件
// --------
// 远端执行体的数据面事件通过 `poll_events()` 回到 `Gateway`，由 Gateway 继续使用自己的
// 每连接有界队列和终态规则交付。这样“执行拆分”不会改变外部事件格式；当前控制面适配器
// 可以暂时返回 0 条事件，事件传输由后续多进程 Session 适配接入。
//
// 线程与所有权
// ------------
// 实现必须允许 `Gateway` 的驱动线程串行调用。`call()` 允许阻塞，但阻塞时间必须受请求自带
// deadline 与实现配置共同约束；`poll_events()` 必须是非阻塞快照。实现不拥有 Gateway 的
// 连接、队列或请求记录，返回的响应与事件都是调用方拥有的值对象。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../domain/error.hpp"
#include "../protocol/control_rpc.hpp"
#include "../protocol/data_event.hpp"

namespace nexweave::gateway {

// 控制路由的归属标识。数值与 Gateway::ConnectionId 映射，但这里不直接依赖 Gateway 头文件，
// 避免接缝实现反向包含入口层。
using ControlRouteOwner = std::uint64_t;

// 控制路由接口。
//
// 错误语义：`call()` 返回失败时，Gateway 会把同一个 request_id 与失败错误码编码成外部
// ControlResponse；实现不需要自己构造外部响应，也不要吞掉错误。连接失败、deadline 超时、
// 远端不可用分别使用既有错误码，不用自由文本替代机器可读结论。
//
// 幂等：Gateway 在本地仍保留 request_id 记录；实现必须保持远端自身的幂等语义，使
// “已定局响应重放”和“执行中拒绝重复”在跨进程后仍然成立。
//
// 资源与超时：实现自行创建并释放传输资源，不得把资源生命周期转嫁给 Gateway；`call()`
// 返回后不得继续持有传入 request 的引用。
class IControlRoute {
 public:
  virtual ~IControlRoute() = default;

  // 执行一次控制请求。返回成功表示远端返回了合法 ControlResponse；返回失败表示本次转发
  // 没有取得可交付响应。request_id 必须原样保留在响应的归属中。
  virtual domain::Result<protocol::ControlResponse> call(
      const protocol::ControlRequest& request) = 0;

  // 取出归属到 owner 的已到达数据事件，最多 max_events 条，追加到 out。
  // 非阻塞；没有事件时返回 0。out 的内容由调用方拥有，实现不得在返回后继续修改。
  virtual std::size_t poll_events(ControlRouteOwner owner,
                                  std::vector<protocol::DataEvent>& out,
                                  std::size_t max_events) = 0;
};

}  // namespace nexweave::gateway
