cmake_minimum_required(VERSION 3.16)

# NexWeave 边缘智能系统底座：设备侧智能任务执行框架。
# 项目版本号统一维护在 project() 的 VERSION 处；core/domain/version.hpp 中
# 的 NEXWEAVE_VERSION_* 常量必须与这里保持一致，tests/unit/version_test.cpp
# 会校验两者的一致性，防止版本漂移。
project(nexweave VERSION 0.1.0 LANGUAGES CXX)

# 目标平台为资源受限的嵌入式 Linux，构建主要在 WSL 与 Linux 上进行。
# C++17 为最低语言标准；核心模块禁止依赖 ROS2/ZeroMQ/ALSA/RKNN/RKLLM/
# sherpa-onnx/MeloTTS 类型，保证核心可移植。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  # 默认 Release：性能数据在 Release 语义下才可比较；
  # 调试构建通过 -DCMAKE_BUILD_TYPE=Debug 显式选择。
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(NEXWEAVE_WARNINGS_AS_ERRORS "将编译器警告视为错误（门禁构建建议开启）" OFF)

# 由 CMake 的 project(VERSION ...) 生成版本头文件，避免 CMake 与头文件
# 版本号手工重复维护而漂移；version_unit 测试校验生成值与 core/domain/
# version.hpp 中的常量一致。
set(NEXWEAVE_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
configure_file(
  cmake/nexweave_version_from_cmake.h.in
  ${NEXWEAVE_GENERATED_DIR}/nexweave_version_from_cmake.h
  @ONLY
)

# ---- 核心模块：core/domain（版本号领域值）----
add_library(nexweave_core_domain STATIC
  core/domain/version.cpp
  core/domain/identifiers.cpp
  core/domain/audio_frame.cpp
  core/domain/error.cpp
)
target_include_directories(nexweave_core_domain PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/core/domain
  ${NEXWEAVE_GENERATED_DIR}
)
target_compile_options(nexweave_core_domain PRIVATE
  -Wall -Wextra -Wpedantic
)
if(NEXWEAVE_WARNINGS_AS_ERRORS)
  target_compile_options(nexweave_core_domain PRIVATE -Werror)
endif()

# ---- 测试 ----
enable_testing()

add_executable(nexweave_version_test
  tests/unit/version_test.cpp
)
target_link_libraries(nexweave_version_test PRIVATE nexweave_core_domain)
target_compile_options(nexweave_version_test PRIVATE -Wall -Wextra -Wpedantic)
if(NEXWEAVE_WARNINGS_AS_ERRORS)
  target_compile_options(nexweave_version_test PRIVATE -Werror)
endif()

# 版本一致性是发布边界的不变量：CMake VERSION 与头文件常量必须一致。
add_test(NAME version_unit COMMAND nexweave_version_test)
set_tests_properties(version_unit PROPERTIES LABELS "unit")

add_executable(nexweave_identifiers_test tests/unit/identifiers_test.cpp)
target_link_libraries(nexweave_identifiers_test PRIVATE nexweave_core_domain)
target_compile_options(nexweave_identifiers_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME identifiers_unit COMMAND nexweave_identifiers_test)
set_tests_properties(identifiers_unit PROPERTIES LABELS "unit")

add_executable(nexweave_audio_frame_test tests/unit/audio_frame_test.cpp)
target_link_libraries(nexweave_audio_frame_test PRIVATE nexweave_core_domain)
target_compile_options(nexweave_audio_frame_test PRIVATE -Wall -Wextra -Wpedantic)
if(NEXWEAVE_WARNINGS_AS_ERRORS)
  target_compile_options(nexweave_audio_frame_test PRIVATE -Werror)
endif()
add_test(NAME audio_frame_unit COMMAND nexweave_audio_frame_test)
set_tests_properties(audio_frame_unit PROPERTIES LABELS "unit")
add_executable(nexweave_error_test tests/unit/error_test.cpp)
target_link_libraries(nexweave_error_test PRIVATE nexweave_core_domain)
target_compile_options(nexweave_error_test PRIVATE -Wall -Wextra -Wpedantic)
if(NEXWEAVE_WARNINGS_AS_ERRORS)
  target_compile_options(nexweave_error_test PRIVATE -Werror)
endif()
add_test(NAME error_unit COMMAND nexweave_error_test)
set_tests_properties(error_unit PROPERTIES LABELS "unit")

# 后端能力契约夹具只依赖公共值对象，验证六类接口可被同一运行时注入，
# 并覆盖 top_k=0、空文本、未打开输出和设备读失败等失败语义。
add_executable(nexweave_backend_contract_test
  tests/contract/backend_contract_test.cpp)
target_link_libraries(nexweave_backend_contract_test PRIVATE nexweave_core_domain)
target_include_directories(nexweave_backend_contract_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/core/capability)
target_compile_options(nexweave_backend_contract_test PRIVATE -Wall -Wextra -Wpedantic)
if(NEXWEAVE_WARNINGS_AS_ERRORS)
  target_compile_options(nexweave_backend_contract_test PRIVATE -Werror)
endif()
add_test(NAME backend_contract COMMAND nexweave_backend_contract_test)
set_tests_properties(backend_contract PROPERTIES LABELS "contract")

# 门禁脚本失败路径：工具缺失时必须以可读错误和非零码退出；
# 成功路径由仓库自身的构建 + 全量 CTest 链路覆盖。
add_test(NAME gate_scripts_failure COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/gate/gate_test.sh)
set_tests_properties(gate_scripts_failure PROPERTIES LABELS "gate")
