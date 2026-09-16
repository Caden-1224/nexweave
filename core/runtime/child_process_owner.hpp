// 子进程会话拥有者：把操作系统子进程接到现有 Supervisor 的 ISessionOwner 接缝上。
//
// 职责与定位
// ----------
// Supervisor 只认识“建立、执行、清理”三个入口；本文件提供一个最小适配器，把
// ChildProcess 的生命周期映射成这三个入口。它不实现会话协议、不搬音频、不解释远端事件；
// 这些能力属于后续的多进程 Session/传输适配。当前适配器的外部行为是：
//   start()   启动子进程并等待就绪，只有就绪后才让 Supervisor 把槽位标为工作中；
//   run()     等待子进程退出；收到停止请求后负责在有限预算内完成 SIGTERM/SIGKILL 升级；
//   cleanup() 再次确保子进程已经回收，幂等。
//
// 为什么需要独立工厂
// ------------------
// Supervisor 的工厂契约要求每次会话产出全新的拥有者对象。每次 create() 都新建一个
// ChildProcess，因此子进程身份、退出归档、停止标记和管道都不会跨会话泄漏；旧会话结束后
// 旧身份无法命中新进程。
//
// 资源与线程
// ----------
// 工厂只保存配置副本，不创建子进程。每个拥有者独占一个 ChildProcess；ChildProcess 的
// fork/exec、信号、waitpid 都由拥有者对象持有，并由 Supervisor 的同一个工作线程串行调用
// start()/run()/cleanup()。唯一允许跨线程的是 request_stop()，它只向当前子进程发送
// SIGTERM，不等待回收。
//
// 已知边界
// --------
// 本适配器把“子进程存活”当作会话存活，子进程退出即会话终止。需要由父进程持续转发控制
// 请求和消费流式事件的多进程 Session 会在后续适配器中替换 run() 的驱动方式，但仍应复用
// ChildProcess 的启动、身份与停止升级语义。
#pragma once

#include <memory>

#include "../domain/error.hpp"
#include "child_process.hpp"
#include "supervisor.hpp"

namespace nexweave::runtime {

// 子进程拥有者工厂。构造时校验子进程配置；非法配置不会抛异常，而是在 create() 时返回
// 结构化错误并拒绝建立拥有者。
//
// spec 在本版是每个会话固定的启动模板：参数不包含会话身份。需要按 work_id/session_id
// 生成不同命令行的部署方式应在后续适配器中扩展工厂接口，而不是在本类里偷偷解析领域标识。
class ChildProcessOwnerFactory final : public ISessionOwnerFactory {
 public:
  ChildProcessOwnerFactory(ChildProcessSpec spec, ChildProcessConfig config = {});

  ~ChildProcessOwnerFactory() override = default;

  ChildProcessOwnerFactory(const ChildProcessOwnerFactory&) = delete;
  ChildProcessOwnerFactory& operator=(const ChildProcessOwnerFactory&) = delete;

  // 为一次会话创建全新拥有者。配置无效或创建拥有者失败时返回 nullptr，并写入 error；
  // 成功后调用方获得一个独占的子进程槽位，真正的 fork 发生在 Supervisor 工作线程调用
  // 拥有者 start() 时。
  std::shared_ptr<ISessionOwner> create(const SupervisorSessionSpec& spec,
                                        domain::Error& error) override;

 private:
  ChildProcessSpec spec_;
  ChildProcessConfig config_;
  domain::Error config_error_{};
};

}  // namespace nexweave::runtime
