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

add_library(nexweave_warnings INTERFACE)
target_compile_options(nexweave_warnings INTERFACE
  -Wall -Wextra -Wpedantic
  $<$<BOOL:${NEXWEAVE_WARNINGS_AS_ERRORS}>:-Werror>
)

set(NEXWEAVE_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
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
  core/backend/fake_audio.cpp
  core/backend/fake_asr.cpp
  core/backend/fake_rag.cpp
  core/backend/fake_llm.cpp
  core/backend/fake_tts.cpp
  core/runtime/interaction_contract.cpp
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

enable_testing()
# 所有测试通过同一配置入口注册，新增目标不会漏掉警告门禁。
function(nexweave_test name source label)
  add_executable(nexweave_${name}_test ${source})
  target_link_libraries(nexweave_${name}_test PRIVATE
    nexweave_core_domain nexweave_warnings nlohmann_json::nlohmann_json)
  target_include_directories(nexweave_${name}_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core/capability
    ${CMAKE_CURRENT_SOURCE_DIR}/core/backend
    ${CMAKE_CURRENT_SOURCE_DIR}/core/observability)
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
nexweave_test(fake_audio tests/unit/fake_audio_test.cpp unit)
nexweave_test(fake_asr tests/unit/fake_asr_test.cpp unit)
nexweave_test(fake_rag tests/unit/fake_rag_test.cpp unit)
nexweave_test(fake_llm tests/unit/fake_llm_test.cpp unit)
nexweave_test(fake_tts tests/unit/fake_tts_test.cpp unit)
nexweave_test(interaction_contract tests/unit/interaction_contract_test.cpp unit)
# 同一公共能力测试同时进入 contract 门禁，避免只测最小 Fixture 而漏掉实际 Fake。
set_tests_properties(fake_asr_unit PROPERTIES LABELS "unit;contract")
nexweave_test(protocol_boundaries tests/unit/protocol_boundaries_test.cpp unit)

# 门禁测试只在构建目录创建独立夹具，源码从不被改动；失败也以 trap 清理临时资源。
add_test(NAME gate_scripts_failure COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/gate/gate_test.sh)
add_test(NAME gate_rebuild COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/gate/rebuild_test.sh
  ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(gate_scripts_failure gate_rebuild PROPERTIES LABELS "gate" TIMEOUT 60)
