# NexWeave — 边缘智能系统底座

NexWeave 为设备侧智能任务提供统一的标识、音频、错误、会话状态、取消代际和协议契约。
当前已实现这些基础模块与内存 Fake 音频输入输出，可以在无声卡、无模型的 WSL/Linux 中验证。
完整语音业务链路、多进程传输和真实设备适配尚未实现。

## 构建与测试

要求 C++17 编译器、CMake ≥ 3.16、Make、Bash 和 nlohmann-json ≥ 3.10。
Ubuntu 22.04 可安装：

```bash
sudo apt install build-essential cmake nlohmann-json3-dev
./scripts/build.sh
./scripts/test.sh
```

测试脚本每次先做增量构建，源码编译失败时不会运行旧程序。默认使用 Release；
测试检查不依赖 assert，因此 Release 和 Debug 都会执行失败断言。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DNEXWEAVE_WARNINGS_AS_ERRORS=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
BUILD_DIR=build/debug BUILD_TYPE=Debug ./scripts/test.sh
```

`BUILD_DIR` 和 `BUILD_TYPE` 同时适用于两个脚本。自定义 JSON 安装可通过 CMake 的
`nlohmann_json_DIR` 或 `CMAKE_PREFIX_PATH` 指定，缺失依赖会在配置阶段给出安装提示。
所有 C++ 目标共享警告选项；格式由 `.clang-format` 定义，建议使用 clang-format 18。

## 模块边界

- `core/domain/`：标识、生成代际、版本、PCM 帧和结构化错误。
- `core/capability/`：ASR、RAG、LLM、TTS 与可取消音频输入输出接口。
- `core/runtime/`：Session 状态迁移与旧代事件隔离。
- `core/protocol/`：控制面 JSON 编解码、重试和超时规则；数据面元数据与二进制 PCM。
- `core/backend/`：确定性 Fake 音频源与音频汇。
- `core/observability/`：运行清单、事件和指标值对象。
- `tests/`：单元测试、最小能力夹具和构建门禁测试。

核心不暴露模型 SDK 或设备句柄；JSON 是编译依赖。控制面只定义可测试的值语义，
尚无网络服务或幂等缓存。TTS 契约约定同步完成，音频取消丢弃待消费数据；
内存 Fake 的结果不能用来推断真实设备延迟、吞吐或稳定性。

## 许可证

自有实现采用 MIT（见 LICENSE）。当前第三方依赖、版本和许可证记录见 NOTICE；
本仓库不包含模型、厂商 SDK、现场录音或凭据。
