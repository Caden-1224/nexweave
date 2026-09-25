cmake_minimum_required(VERSION 3.16)
project(nexweave VERSION 0.1.0 LANGUAGES CXX)

# 核心与测试统一使用 C++17；版本头由 project 生成并由单元测试核对，避免手工漂移。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
option(NEXWEAVE_WARNINGS_AS_ERRORS "将全部 C++ 目标的编译器警告视为错误" OFF)

# JSON 为实际编译依赖，必须在配置阶段发现；不让新环境直到编译源文件时才报缺头文件。
find_package(nlohmann_json 3.10 CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
  message(FATAL_ERROR
    "缺少 nlohmann-json >= 3.10。Ubuntu/Debian: sudo apt install nlohmann-json3-dev；"
    "自定义安装请设置 nlohmann_json_DIR 或 CMAKE_PREFIX_PATH。")
endif()
find_package(Threads REQUIRED)

# ZeroMQ 控制传输依赖：libzmq3-dev 提供 zmq.h/zmq.hpp 与动态库，但发行版没有稳定 CMake
# config 或 pkg-config 文件，因此在配置阶段直接探测头与库，并在缺失时给出安装命令。
find_path(NEXWEAVE_ZMQ_INCLUDE_DIR NAMES zmq.h)
find_path(NEXWEAVE_CPPZMQ_INCLUDE_DIR NAMES zmq.hpp)
find_library(NEXWEAVE_ZMQ_LIBRARY NAMES zmq)
if(NOT NEXWEAVE_ZMQ_INCLUDE_DIR OR NOT NEXWEAVE_CPPZMQ_INCLUDE_DIR OR
   NOT NEXWEAVE_ZMQ_LIBRARY)
  message(FATAL_ERROR
    "缺少 ZeroMQ 开发文件（需要 zmq.h、zmq.hpp 与 libzmq）。"
    "Ubuntu/Debian: sudo apt install libzmq3-dev；"
    "自定义安装请设置 NEXWEAVE_ZMQ_INCLUDE_DIR、NEXWEAVE_CPPZMQ_INCLUDE_DIR "
    "或 NEXWEAVE_ZMQ_LIBRARY。")
endif()

add_library(nexweave_warnings INTERFACE)
target_compile_options(nexweave_warnings INTERFACE
  -Wall -Wextra -Wpedantic
  $<$<BOOL:${NEXWEAVE_WARNINGS_AS_ERRORS}>:-Werror>
)

set(NEXWEAVE_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
# 构建时采集 Git 提交：运行证据里的“版本”必须能追溯到具体源码状态。取不到时写成
# unknown 而不是让配置失败——从源码压缩包构建是受支持的方式，那条路径上没有 .git。
execute_process(
  COMMAND git rev-parse HEAD
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE NEXWEAVE_GIT_COMMIT
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
  RESULT_VARIABLE NEXWEAVE_GIT_RESULT
)
if(NOT NEXWEAVE_GIT_RESULT EQUAL 0 OR NEXWEAVE_GIT_COMMIT STREQUAL "")
  set(NEXWEAVE_GIT_COMMIT "unknown")
endif()
configure_file(cmake/nexweave_version_from_cmake.h.in
  ${NEXWEAVE_GENERATED_DIR}/nexweave_version_from_cmake.h @ONLY)

add_library(nexweave_core_domain STATIC
  core/domain/version.cpp
  core/domain/identifiers.cpp
  core/domain/audio_frame.cpp
  core/domain/error.cpp
  core/runtime/session_state_machine.cpp
  core/protocol/control_rpc.cpp
  core/protocol/data_event.cpp
  core/observability/observability.cpp
  core/observability/run_evidence.cpp
  core/backend/fake_audio.cpp
  core/backend/fake_asr.cpp
  core/backend/fake_rag.cpp
  core/backend/bm25_rag.cpp
  core/backend/fake_llm.cpp
  core/backend/fake_tts.cpp
  core/backend/zipformer_stream.cpp
  core/backend/rkllm/rkllm_text_core.cpp
  core/runtime/interaction_contract.cpp
  core/runtime/session_runtime.cpp
  core/runtime/text_segmentation.cpp
  core/runtime/speech_segmentation.cpp
  core/runtime/resident_audio_input.cpp
  core/runtime/queued_audio_source.cpp
  core/runtime/supervisor.cpp
  core/runtime/child_process.cpp
  core/runtime/child_process_owner.cpp
)
target_include_directories(nexweave_core_domain PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/core/domain
  ${CMAKE_CURRENT_SOURCE_DIR}/core/runtime
  ${NEXWEAVE_GENERATED_DIR}
)
target_link_libraries(nexweave_core_domain
  PRIVATE nexweave_warnings nlohmann_json::nlohmann_json
  PUBLIC Threads::Threads
)

# 应用层：把能力夹具、会话编排与输入生产者组装成可运行对象。它与核心库分开编译，
# 因此“应用入口”不进入核心依赖，核心也不会反向依赖命令行。
add_library(nexweave_app STATIC
  core/app/session_app.cpp
  core/app/session_app_owner.cpp
)
target_include_directories(nexweave_app PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/core/app
  ${CMAKE_CURRENT_SOURCE_DIR}/core/capability
  ${CMAKE_CURRENT_SOURCE_DIR}/core/backend
)
target_link_libraries(nexweave_app
  PUBLIC nexweave_core_domain
  PRIVATE nexweave_warnings nlohmann_json::nlohmann_json
)

# 请求入口层：把外部控制请求与数据事件接到监督器上。它依赖应用层（会话组装结果）与核心层
# （监督器、协议），因此单独成一个目标：命令行入口与后续传输适配器共用同一份请求处理逻辑，
# 不必各自复制一份控制路由。
add_library(nexweave_gateway STATIC
  core/gateway/ndjson_framer.cpp
  core/gateway/gateway.cpp
)
target_include_directories(nexweave_gateway PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/core/gateway
)
target_link_libraries(nexweave_gateway
  PUBLIC nexweave_app
  PRIVATE nexweave_warnings nlohmann_json::nlohmann_json
)

# ZeroMQ 传输适配器：控制面使用 REQ/REP 接入进程内 Gateway，数据面使用 PAIR multipart
# 搬运输入音频与输出事件。Context/socket 类型只在实现文件里出现，公共头文件只暴露
# NexWeave 值对象，避免传输细节进入 Session。
add_library(nexweave_transport STATIC
  core/transport/zmq_control.cpp
  core/transport/zmq_data.cpp
  core/transport/zmq_gateway_route.cpp
  core/transport/remote_session_server.cpp
  core/transport/remote_session_proxy.cpp
  core/transport/remote_session_route.cpp
)
target_include_directories(nexweave_transport PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/core/transport
)
target_include_directories(nexweave_transport PRIVATE
  ${NEXWEAVE_ZMQ_INCLUDE_DIR}
  ${NEXWEAVE_CPPZMQ_INCLUDE_DIR}
)
target_link_libraries(nexweave_transport
  PUBLIC nexweave_gateway
  PRIVATE nexweave_warnings ${NEXWEAVE_ZMQ_LIBRARY}
)

# 部署 profile 层：把进程拓扑、生命周期与数据面组装成可复用的上层契约。它依赖传输层，
# 但不反向进入传输或 Session；后续 Linux/RK3576 部署入口链接本目标即可复用启停语义。
add_library(nexweave_profile STATIC
  core/profile/linux_profile.cpp
)
target_include_directories(nexweave_profile PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/core/profile
)
target_link_libraries(nexweave_profile
  PUBLIC nexweave_transport
  PRIVATE nexweave_warnings
)

# 子进程生命周期测试夹具：它只调用 notify_parent_ready() 并等待信号，供集成测试观察
# 启动就绪、SIGTERM/SIGKILL 升级和退出归档。它不进入产品安装目标。
add_executable(nexweave_child_process_helper tests/support/child_process_helper.cpp)
target_link_libraries(nexweave_child_process_helper PRIVATE
  nexweave_core_domain nexweave_warnings)

# 多进程 Gateway 路由测试夹具：在独立进程中运行真实 Gateway + Supervisor，并通过
# notify_parent_ready() 报告控制端点已绑定。
add_executable(nexweave_remote_gateway_fixture tests/support/remote_gateway_fixture.cpp)
target_link_libraries(nexweave_remote_gateway_fixture PRIVATE
  nexweave_transport nexweave_warnings)

# 多进程 Session 测试夹具：在独立进程中运行真实 SessionApp 与数据面服务端。
add_executable(nexweave_remote_session_fixture tests/support/remote_session_fixture.cpp)
target_link_libraries(nexweave_remote_session_fixture PRIVATE
  nexweave_transport nexweave_warnings)

# 数据面脚本夹具：阻塞输入队列以验证满队列取消，并为旧代际输出过滤提供可控对端。
add_executable(nexweave_remote_scripted_fixture tests/support/remote_scripted_fixture.cpp)
target_link_libraries(nexweave_remote_scripted_fixture PRIVATE
  nexweave_transport nexweave_warnings)

enable_testing()
# 所有测试通过同一配置入口注册，新增目标不会漏掉警告门禁。
function(nexweave_test name source label)
  add_executable(nexweave_${name}_test ${source})
  target_link_libraries(nexweave_${name}_test PRIVATE
    nexweave_core_domain nexweave_warnings nlohmann_json::nlohmann_json)
  target_include_directories(nexweave_${name}_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core/capability
    ${CMAKE_CURRENT_SOURCE_DIR}/core/backend
    ${CMAKE_CURRENT_SOURCE_DIR}/core/observability
    ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  add_test(NAME ${name}_${label} COMMAND nexweave_${name}_test)
  set_tests_properties(${name}_${label} PROPERTIES LABELS "${label}")
endfunction()

nexweave_test(version tests/unit/version_test.cpp unit)
nexweave_test(identifiers tests/unit/identifiers_test.cpp unit)
nexweave_test(audio_frame tests/unit/audio_frame_test.cpp unit)
nexweave_test(error tests/unit/error_test.cpp unit)
nexweave_test(backend tests/contract/backend_contract_test.cpp contract)
nexweave_test(session_state_machine tests/unit/session_state_machine_test.cpp unit)
nexweave_test(control_rpc tests/unit/control_rpc_test.cpp unit)
nexweave_test(data_event tests/unit/data_event_test.cpp unit)
nexweave_test(observability tests/unit/observability_test.cpp unit)
# 运行证据记录器：用注入的手动时钟验收步数分配、首次出现语义与未测量标注。
nexweave_test(run_evidence tests/unit/run_evidence_test.cpp unit)
nexweave_test(fake_audio tests/unit/fake_audio_test.cpp unit)
# 队列音频源边界：满队列拒绝、自然结束保留帧、取消唤醒阻塞读取。
nexweave_test(queued_audio_source tests/unit/queued_audio_source_test.cpp unit)
# 统一队列语义：容量、拒绝、超时、关闭、清空与峰值。
nexweave_test(bounded_queue tests/unit/bounded_queue_test.cpp unit)
# 终态缓存：容量淘汰、保留期限与显式过期查询。
nexweave_test(terminal_cache tests/unit/terminal_cache_test.cpp unit)
nexweave_test(fake_asr tests/unit/fake_asr_test.cpp unit)
nexweave_test(zipformer_stream tests/unit/zipformer_stream_test.cpp unit)
nexweave_test(rkllm_text_core tests/unit/rkllm_text_core_test.cpp unit)
nexweave_test(fake_rag tests/unit/fake_rag_test.cpp unit)
nexweave_test(bm25_rag tests/unit/bm25_rag_test.cpp unit)
nexweave_test(fake_llm tests/unit/fake_llm_test.cpp unit)
nexweave_test(fake_tts tests/unit/fake_tts_test.cpp unit)
nexweave_test(playback_clock tests/unit/playback_clock_test.cpp unit)
nexweave_test(interaction_contract tests/unit/interaction_contract_test.cpp unit)
nexweave_test(session_l0_l1 tests/integration/session_l0_l1_test.cpp integration)
# 真实检索路由校准确认：加载固定知识库与校准/验收集，阈值和上下文预算都来自显式配置。
nexweave_test(rag_calibration tests/integration/rag_routing_calibration_test.cpp integration)
target_compile_definitions(nexweave_rag_calibration_test PRIVATE
  NEXWEAVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
nexweave_test(session_l2_l3 tests/integration/session_l2_l3_test.cpp integration)
nexweave_test(session_cancel tests/integration/session_cancel_test.cpp integration)
nexweave_test(speech_segmentation tests/unit/speech_segmentation_test.cpp unit)
nexweave_test(text_segmentation tests/unit/text_segmentation_test.cpp unit)
nexweave_test(resident_input tests/integration/resident_input_session_test.cpp integration)
# 该目标包含真实线程交错：条件变量漏唤醒会表现为挂起，超时让它在 60 秒内变成明确失败。
set_tests_properties(resident_input_integration PROPERTIES TIMEOUT 60)
# 同一公共能力测试同时进入 contract 门禁，避免只测最小 Fixture 而漏掉实际 Fake。
set_tests_properties(fake_asr_unit fake_tts_unit fake_audio_unit
  PROPERTIES LABELS "unit;contract")
nexweave_test(session_app tests/integration/session_app_test.cpp integration)
# 应用测试同时需要应用层与核心层：它既验收组装结果，也比对会话层的公开证据。
target_link_libraries(nexweave_session_app_test PRIVATE nexweave_app)
target_include_directories(nexweave_session_app_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/core/app)
nexweave_test(supervisor_session_app tests/integration/supervisor_session_app_test.cpp integration)
# 该目标同时需要应用层与核心层：它验收的是把会话应用接到监督器接缝上之后的行为。
target_link_libraries(nexweave_supervisor_session_app_test PRIVATE nexweave_app)
target_include_directories(nexweave_supervisor_session_app_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/core/app)
set_tests_properties(supervisor_session_app_integration PROPERTIES TIMEOUT 60)
nexweave_test(supervisor_lifecycle tests/integration/supervisor_lifecycle_test.cpp integration)
nexweave_test(child_process_contract tests/unit/child_process_contract_test.cpp unit)
nexweave_test(child_process_lifecycle tests/integration/child_process_lifecycle_test.cpp integration)
target_compile_definitions(nexweave_child_process_lifecycle_test PRIVATE
  NEXWEAVE_CHILD_PROCESS_HELPER="$<TARGET_FILE:nexweave_child_process_helper>")
# 测试包含真实 fork/exec、信号和 waitpid；超时让夹具挂起变成明确失败。
set_tests_properties(child_process_lifecycle_integration PROPERTIES TIMEOUT 60)
nexweave_test(child_process_owner tests/integration/child_process_owner_test.cpp integration)
target_compile_definitions(nexweave_child_process_owner_test PRIVATE
  NEXWEAVE_CHILD_PROCESS_HELPER="$<TARGET_FILE:nexweave_child_process_helper>")
# 拥有者测试包含真实 Supervisor 工作线程、子进程和信号升级，超时用于暴露挂起。
set_tests_properties(child_process_owner_integration PROPERTIES TIMEOUT 60)
# 该目标包含真实线程交错：闸门不放行会表现为挂起，超时让它在 60 秒内变成明确失败。
set_tests_properties(supervisor_lifecycle_integration PROPERTIES TIMEOUT 60)
nexweave_test(protocol_boundaries tests/unit/protocol_boundaries_test.cpp unit)
nexweave_test(ndjson_framer tests/unit/ndjson_framer_test.cpp unit)
# 解帧是纯值逻辑，但实现位于入口库内，因此测试同时链接入口层，避免出现“只有测试里能用”的副本。
target_link_libraries(nexweave_ndjson_framer_test PRIVATE nexweave_gateway)
# profile 层把能力夹具、会话应用、监督器与请求入口组装成"一条命令跑一个场景"。它同时依赖
# 应用层与请求入口层，因此放在 core/profile 而不是 core/app——否则"应用层不依赖请求入口层"
# 这条分层约束会被一次文件摆放反过来，后续任何人都可能顺着 include 路径把它变成真依赖。
nexweave_test(mock_profile tests/integration/mock_profile_test.cpp integration)
target_sources(nexweave_mock_profile_test PRIVATE core/profile/mock_profile.cpp)
target_link_libraries(nexweave_mock_profile_test PRIVATE nexweave_gateway)
target_include_directories(nexweave_mock_profile_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/core/profile
  ${CMAKE_CURRENT_SOURCE_DIR}/core/observability)
# 场景包含真实线程交错（会话在监督器工作线程上执行）；等满即说明实现有缺陷，
# 超时让它在 60 秒内变成明确失败而不是挂起。
set_tests_properties(mock_profile_integration PROPERTIES TIMEOUT 60)
# 运行证据：四条进程内路径的场景各跑一次真实 Mock 运行，验收五份产物、里程碑与取消阶段。
nexweave_test(mock_profile_evidence tests/integration/mock_profile_evidence_test.cpp integration)
target_sources(nexweave_mock_profile_evidence_test PRIVATE core/profile/mock_profile.cpp)
target_link_libraries(nexweave_mock_profile_evidence_test PRIVATE nexweave_gateway)
target_include_directories(nexweave_mock_profile_evidence_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/core/profile
  ${CMAKE_CURRENT_SOURCE_DIR}/core/observability)
# 场景包含真实线程交错（会话在监督器工作线程上执行，取消由设备回调发起）。
set_tests_properties(mock_profile_evidence_integration PROPERTIES TIMEOUT 60)
nexweave_test(gateway_entry tests/integration/gateway_entry_test.cpp integration)
# 入口测试同时需要入口层、会话应用与核心层：它验收的是把真实会话接到外部请求入口之后的
# 行为，并包含真实线程交错（会话在工作线程上执行而控制请求在调用线程上处理）。
target_link_libraries(nexweave_gateway_entry_test PRIVATE nexweave_gateway)
# 闸门不放行或事件交付漏唤醒会表现为挂起，超时让它在 60 秒内变成明确失败。
set_tests_properties(gateway_entry_integration PROPERTIES TIMEOUT 60)

# ZeroMQ 控制面集成测试：真实 socket、真实 Gateway 与阻塞会话夹具共同验证长执行期间的
# 控制可达性、幂等和启停清理。
nexweave_test(zmq_control tests/integration/zmq_control_test.cpp integration)
target_link_libraries(nexweave_zmq_control_test PRIVATE nexweave_transport)
set_tests_properties(zmq_control_integration PROPERTIES TIMEOUT 60)

# 多进程 Gateway 控制路由：本地 Gateway 通过 ZeroMQ 路由到独立进程中的 Gateway+Supervisor，
# 验证推理阻塞时控制可达、取消收敛、远端不可用结构化错误与进程退出后的明确失败。
nexweave_test(gateway_route tests/integration/gateway_route_test.cpp integration)
target_link_libraries(nexweave_gateway_route_test PRIVATE nexweave_transport)
target_compile_definitions(nexweave_gateway_route_test PRIVATE
  NEXWEAVE_REMOTE_GATEWAY_FIXTURE="$<TARGET_FILE:nexweave_remote_gateway_fixture>"
  NEXWEAVE_REMOTE_SESSION_FIXTURE="$<TARGET_FILE:nexweave_remote_session_fixture>"
  NEXWEAVE_REMOTE_SCRIPTED_FIXTURE="$<TARGET_FILE:nexweave_remote_scripted_fixture>")
# 夹具包含真实线程、ZeroMQ socket 与独立子进程，挂起时必须在 60 秒内变成明确失败。
set_tests_properties(gateway_route_integration PROPERTIES TIMEOUT 60)

# ZeroMQ 数据面集成测试：真实 PAIR socket、multipart 编解码、输入流顺序和事件终态。
nexweave_test(zmq_data tests/integration/zmq_data_test.cpp integration)
target_link_libraries(nexweave_zmq_data_test PRIVATE nexweave_transport)
set_tests_properties(zmq_data_integration PROPERTIES TIMEOUT 60)

# 多进程 Session 适配：真实子进程、持续输入上行、输出事件接收、取消与重复启停。
nexweave_test(remote_session tests/integration/remote_session_test.cpp integration)
target_link_libraries(nexweave_remote_session_test PRIVATE nexweave_transport)
target_compile_definitions(nexweave_remote_session_test PRIVATE
  NEXWEAVE_REMOTE_SESSION_FIXTURE="$<TARGET_FILE:nexweave_remote_session_fixture>"
  NEXWEAVE_REMOTE_SCRIPTED_FIXTURE="$<TARGET_FILE:nexweave_remote_scripted_fixture>")
set_tests_properties(remote_session_integration PROPERTIES TIMEOUT 60)

# Linux profile 生命周期：真实子进程与 PAIR 数据面，覆盖重复启停、重启、取消过滤、
# 启动失败回滚和满输出队列收敛；超时让夹具挂起变成明确失败。
nexweave_test(linux_profile tests/integration/linux_profile_lifecycle_test.cpp integration)
target_link_libraries(nexweave_linux_profile_test PRIVATE nexweave_profile)
target_compile_definitions(nexweave_linux_profile_test PRIVATE
  NEXWEAVE_REMOTE_SESSION_FIXTURE="$<TARGET_FILE:nexweave_remote_session_fixture>"
  NEXWEAVE_REMOTE_SCRIPTED_FIXTURE="$<TARGET_FILE:nexweave_remote_scripted_fixture>")
set_tests_properties(linux_profile_integration PROPERTIES TIMEOUT 60)

# 单进程/多进程对照：同一 Fake Session 夹具分别在同进程与 LinuxProfile 子进程中运行，
# 输出原始延迟/RSS/队列样本与汇总 JSON；测试只断言成功收敛和输出一致，不把 Fake 耗时当性能。
nexweave_test(linux_profile_comparison tests/integration/linux_profile_comparison_test.cpp integration)
target_link_libraries(nexweave_linux_profile_comparison_test PRIVATE nexweave_profile)
target_include_directories(nexweave_linux_profile_comparison_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
target_compile_definitions(nexweave_linux_profile_comparison_test PRIVATE
  NEXWEAVE_REMOTE_SESSION_FIXTURE="$<TARGET_FILE:nexweave_remote_session_fixture>")
set_tests_properties(linux_profile_comparison_integration PROPERTIES TIMEOUT 120)

# 单进程 Mock 应用入口：与测试目标共用同一套警告门禁与依赖，避免“只在测试里能跑”。
add_executable(nexweave_session_app apps/session_app/main.cpp)
target_link_libraries(nexweave_session_app PRIVATE
  nexweave_app nexweave_warnings nlohmann_json::nlohmann_json)
target_include_directories(nexweave_session_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/core/capability
  ${CMAKE_CURRENT_SOURCE_DIR}/core/backend
  ${CMAKE_CURRENT_SOURCE_DIR}/core/observability)

# Mock profile 命令行入口：一条命令跑一个可复现场景，并自带证据与清理账目。
add_executable(nexweave_mock_profile
  apps/mock_profile/main.cpp
  core/profile/mock_profile.cpp)
target_link_libraries(nexweave_mock_profile PRIVATE
  nexweave_gateway nexweave_warnings nlohmann_json::nlohmann_json)
target_include_directories(nexweave_mock_profile PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/core/profile
  ${CMAKE_CURRENT_SOURCE_DIR}/core/capability
  ${CMAKE_CURRENT_SOURCE_DIR}/core/backend
  ${CMAKE_CURRENT_SOURCE_DIR}/core/observability)

# 命令行门禁：验收参数错误、三种输入模式可运行、确定性一致与信号退出。
# 它只调用已经构建好的可执行文件，不修改源码，也不后台启动进程。
add_test(NAME session_app_cli
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/gate/session_app_cli_test.sh
          $<TARGET_FILE:nexweave_session_app>)
set_tests_properties(session_app_cli PROPERTIES LABELS "gate" TIMEOUT 60)

# 门禁测试只在构建目录创建独立夹具，源码从不被改动；失败也以 trap 清理临时资源。
# Mock profile 门禁：验收四个场景可显式选择、退出码语义、产物无残留与无硬件依赖。
# 它只调用已经构建好的可执行文件，不修改源码，也不后台启动进程。
add_test(NAME mock_profile_cli
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/gate/mock_profile_cli_test.sh
          $<TARGET_FILE:nexweave_mock_profile>)
set_tests_properties(mock_profile_cli PROPERTIES LABELS "gate" TIMEOUT 60)

add_test(NAME gate_scripts_failure COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/gate/gate_test.sh)
add_test(NAME gate_rebuild COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/gate/rebuild_test.sh
  ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(gate_scripts_failure gate_rebuild PROPERTIES LABELS "gate" TIMEOUT 60)

# 可选 RKNN Zipformer 适配器构建：默认关闭，WSL Fake 回归不依赖 NPU SDK。
# 打开后要求显式提供 NEXWEAVE_RKNN_ROOT，其中 include/ 放 rknn_api.h 与
# kaldi-native-fbank 头，lib/ 放 librknnrt 与 libkaldi-native-fbank-core。
# 第三方库不提交到本仓库；板端证据记录实际使用的头、库版本与路径。
option(NEXWEAVE_ENABLE_RKNN "构建 RKNN Zipformer ASR 适配器" OFF)
set(NEXWEAVE_RKNN_ROOT "" CACHE PATH "RKNN 与 kaldi-native-fbank 依赖根目录")
if(NEXWEAVE_ENABLE_RKNN)
  find_path(NEXWEAVE_RKNN_INCLUDE_DIR
    NAMES rknn_api.h
    HINTS "${NEXWEAVE_RKNN_ROOT}/include")
  find_library(NEXWEAVE_RKNN_LIBRARY
    NAMES rknnrt
    HINTS "${NEXWEAVE_RKNN_ROOT}/lib")
  find_path(NEXWEAVE_KNF_INCLUDE_DIR
    NAMES kaldi-native-fbank/csrc/online-feature.h
    HINTS "${NEXWEAVE_RKNN_ROOT}/include")
  find_library(NEXWEAVE_KNF_LIBRARY
    NAMES kaldi-native-fbank-core
    HINTS "${NEXWEAVE_RKNN_ROOT}/lib")
  if(NOT NEXWEAVE_RKNN_INCLUDE_DIR OR NOT NEXWEAVE_RKNN_LIBRARY OR
     NOT NEXWEAVE_KNF_INCLUDE_DIR OR NOT NEXWEAVE_KNF_LIBRARY)
    message(FATAL_ERROR
      "NEXWEAVE_ENABLE_RKNN=ON 需要 NEXWEAVE_RKNN_ROOT/include 与 "
      "NEXWEAVE_RKNN_ROOT/lib 下同时提供 rknn_api.h、librknnrt 与 "
      "kaldi-native-fbank 头/静态库。")
  endif()

  add_library(nexweave_rknn_zipformer STATIC
    core/backend/rknn/kaldi_online_fbank.cpp
    core/backend/rknn/rknn_zipformer_engine.cpp
    core/backend/rknn/rknn_zipformer_asr.cpp
  )
  target_include_directories(nexweave_rknn_zipformer
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core/backend/rknn
    PRIVATE
      ${NEXWEAVE_RKNN_INCLUDE_DIR}
      ${NEXWEAVE_KNF_INCLUDE_DIR}
  )
  target_link_libraries(nexweave_rknn_zipformer
    PUBLIC nexweave_core_domain
    PRIVATE
      nexweave_warnings
      ${NEXWEAVE_RKNN_LIBRARY}
      ${NEXWEAVE_KNF_LIBRARY}
  )

  # 板端硬件测试可执行文件：模型、词表和 WAV 由命令行注入，不在 CMake 配置期硬编码。
  add_executable(nexweave_rknn_zipformer_hardware_test
    tests/hardware/rknn_zipformer_hardware_test.cpp)
  target_link_libraries(nexweave_rknn_zipformer_hardware_test PRIVATE
    nexweave_rknn_zipformer
    nexweave_warnings
  )
  target_include_directories(nexweave_rknn_zipformer_hardware_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core/capability
    ${CMAKE_CURRENT_SOURCE_DIR}/core/backend
    ${CMAKE_CURRENT_SOURCE_DIR}/core/domain
  )
  # 板端预编译的 kaldi-native-fbank 静态库未带 PIC；硬件测试单独使用非 PIE 链接，
  # 避免把该第三方库的构建方式强加给核心库或其它测试目标。
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(nexweave_rknn_zipformer_hardware_test PRIVATE -no-pie)
  endif()
endif()

# 可选 RKLLM 文本适配器构建：默认关闭，WSL Fake 回归不依赖 NPU/SDK。
# 打开后要求显式提供 NEXWEAVE_RKLLM_ROOT，其中 include/ 放 rkllm.h，
# lib/ 放 librkllmrt.so。模型文件由运行时配置注入，不提交到仓库。
option(NEXWEAVE_ENABLE_RKLLM "构建 RKLLM 文本推理适配器" OFF)
set(NEXWEAVE_RKLLM_ROOT "" CACHE PATH "RKLLM Runtime 依赖根目录")
if(NEXWEAVE_ENABLE_RKLLM)
  find_path(NEXWEAVE_RKLLM_INCLUDE_DIR
    NAMES rkllm.h
    HINTS "${NEXWEAVE_RKLLM_ROOT}/include")
  find_library(NEXWEAVE_RKLLM_LIBRARY
    NAMES rkllmrt
    HINTS "${NEXWEAVE_RKLLM_ROOT}/lib")
  if(NOT NEXWEAVE_RKLLM_INCLUDE_DIR OR NOT NEXWEAVE_RKLLM_LIBRARY)
    message(FATAL_ERROR
      "NEXWEAVE_ENABLE_RKLLM=ON 需要 NEXWEAVE_RKLLM_ROOT/include/rkllm.h "
      "与 NEXWEAVE_RKLLM_ROOT/lib/librkllmrt.so。")
  endif()

  add_library(nexweave_rkllm_text STATIC
    core/backend/rkllm/rkllm_text_adapter.cpp
  )
  target_include_directories(nexweave_rkllm_text
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core/backend/rkllm
    PRIVATE ${NEXWEAVE_RKLLM_INCLUDE_DIR}
  )
  target_link_libraries(nexweave_rkllm_text
    PUBLIC nexweave_core_domain
    PRIVATE
      nexweave_warnings
      ${NEXWEAVE_RKLLM_LIBRARY}
  )

  add_executable(nexweave_rkllm_hardware_test
    tests/hardware/rkllm_hardware_test.cpp)
  target_link_libraries(nexweave_rkllm_hardware_test PRIVATE
    nexweave_rkllm_text
    nexweave_warnings
  )
  target_include_directories(nexweave_rkllm_hardware_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core/capability
    ${CMAKE_CURRENT_SOURCE_DIR}/core/backend
    ${CMAKE_CURRENT_SOURCE_DIR}/core/backend/rkllm
    ${CMAKE_CURRENT_SOURCE_DIR}/core/domain
  )
endif()
