#!/usr/bin/env bash
# 门禁脚本失败路径测试。
#
# 测试意图：验证 scripts/build.sh 与 scripts/test.sh 在构建工具缺失时
# 必须（1）以非零退出码失败，（2）在 stderr 输出包含工具名的可读错误。
# 保护的不变量：门禁的"可读失败"承诺——缺工具时不允许静默半途失败或
# 裸奔到晦涩的错误堆栈。成功路径由仓库自身的构建 + 全量 CTest 链路覆盖，
# 不在此重复。
# 实现说明：通过 env -i 清空 PATH 制造"工具缺失"环境；脚本内部只用
# bash 内建命令与 command -v 探测，因此该环境下行为是确定性的。

set -u

REPO_ROOT="$(cd "${BASH_SOURCE[0]%/*}/../.." && pwd)"
failures=0

# 参数：脚本路径、预期在错误消息中出现的工具名。
check_readable_missing_tool_error() {
  local script="$1"
  local tool="$2"
  local output
  local exit_code
  output="$(env -i PATH=/nonexistent /bin/bash "$script" 2>&1)"
  exit_code=$?
  if [ "$exit_code" -eq 0 ]; then
    echo "FAIL: $script 在工具缺失时退出码应为非零，实际为 0"
    failures=$((failures + 1))
    return
  fi
  if ! printf '%s' "$output" | grep -q "缺少必需工具 '${tool}'"; then
    echo "FAIL: $script 未输出针对 '${tool}' 的可读错误"
    failures=$((failures + 1))
  fi
}

# build.sh 预检 cmake、g++、make 三个工具，逐项报告。
check_readable_missing_tool_error "${REPO_ROOT}/scripts/build.sh" "cmake"
check_readable_missing_tool_error "${REPO_ROOT}/scripts/build.sh" "g++"
check_readable_missing_tool_error "${REPO_ROOT}/scripts/build.sh" "make"
# test.sh 预检 ctest。
check_readable_missing_tool_error "${REPO_ROOT}/scripts/test.sh" "ctest"

if [ "$failures" -ne 0 ]; then
  echo "gate_test: $failures check(s) failed"
  exit 1
fi
echo "gate_test: all checks passed"
exit 0
