# NexWeave —— 边缘智能系统底座

> 设备侧的智能任务执行框架：可观测、可取消、可恢复，后端可替换。

## 项目简介

NexWeave 部署在资源受限的边缘设备上，把任务生命周期、会话编排、流式事件、
取消、故障恢复、资源约束和证据链收敛到统一的领域契约之后，业务代码不必
直接面对模型 SDK、进程通信、超时与设备错误。

v1.0 的第一条完整业务链路是完全离线的语音交互：音频输入 → ASR → RAG 路由
→ 可选 LLM 推理 → 离线 TTS → PCM / ALSA 音频输出。语音是第一个垂直应用，
后续的机器人、视觉等设备能力在同样的契约上扩展。

核心模块只依赖标准库与本仓库的领域、协议契约；通信、音频设备、ASR、LLM、
TTS 与输出设备都通过适配器接入，因此可以在没有 NPU、模型和声卡的 WSL
环境中完成构建与确定性回归。

## 当前状态

骨架阶段：仓库布局、CMake 工程、构建与测试门禁已就绪；语音链路与各能力
模块按契约、Fake 链路、多进程、真实适配器、板端闭环的顺序逐步启用。

## 快速开始

要求：WSL / Linux，CMake ≥ 3.16，C++17 编译器。

```bash
./scripts/build.sh   # 配置与编译，产物在 build/
./scripts/test.sh    # 运行全部测试
```

`BUILD_DIR` 可覆盖构建目录，`BUILD_TYPE` 可切换构建类型（默认 Release）。

## 目录布局

核心模块只依赖领域与协议契约，适配器与部署形态由外层组装。

```text
core/domain/        领域值：标识符、版本、音频帧合同
core/protocol/      协议：控制面 JSON RPC、数据面事件
core/runtime/       任务编排：Session、Supervisor、取消与代际
core/backend/       后端能力契约与适配器接口
core/observability/ 可观测性：指标、事件、实验记录
adapters/           设备/模型适配器
apps/               应用入口
profiles/           部署形态：mock / linux / rk3576
tests/              分层测试：unit 到 stability
scripts/            构建与测试门禁
third_party/        第三方许可证材料
```

空目录不代表完成：各目录在拥有真实接口、实现或验证内容后启用。

## 许可证

自有实现以 MIT 许可证发布（见 LICENSE）；第三方依赖的许可证登记见 NOTICE。
