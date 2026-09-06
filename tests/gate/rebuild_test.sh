#!/usr/bin/env bash
# 保护源码变化必须先重建的不变量：先构建成功样例，再变异为编译错误，test.sh 必须失败。
# 只在传入的构建目录创建 mktemp 夹具；当前 shell 创建/使用/释放它，退出或信号时 trap 清理。
# 不修改产品源码，不后台启动进程；CTest 60 秒超时是外部兜底，正常路径每步同步退出。
set -euo pipefail
ROOT="$(cd "${BASH_SOURCE[0]%/*}/../.." && pwd)"
fixture="$(mktemp -d "${1:?需要构建目录}/gate-rebuild.XXXXXX")"
trap 'rm -rf -- "$fixture"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir "$fixture/scripts"
cp "$ROOT/scripts/build.sh" "$ROOT/scripts/test.sh" "$fixture/scripts/"
cat > "$fixture/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.16)
project(gate_fixture LANGUAGES CXX)
enable_testing()
add_executable(probe main.cpp)
add_test(NAME probe COMMAND probe)
CMAKE
printf 'int main() { return 0; }\n' > "$fixture/main.cpp"
export BUILD_DIR="$fixture/build"
bash "$fixture/scripts/test.sh" > "$fixture/initial.log" 2>&1 || {
  cat "$fixture/initial.log"
  exit 1
}
# 用固定不同 mtime 排除低精度文件系统的同秒比较干扰，失败点来自源码依赖而非 CMake 变更。
printf '#error NEXWEAVE_GATE_STALE_SOURCE\n' > "$fixture/main.cpp"
touch -d '2030-01-01 00:00:00 UTC' "$fixture/main.cpp"
if bash "$fixture/scripts/test.sh" > "$fixture/changed.log" 2>&1; then
  cat "$fixture/changed.log"
  echo "FAIL: 源码已无法编译，test.sh 仍返回成功。"
  exit 1
fi
if ! grep -q NEXWEAVE_GATE_STALE_SOURCE "$fixture/changed.log"; then
  cat "$fixture/changed.log"
  echo "FAIL: 非零退出并非来自修改后的源码。"
  exit 1
fi
echo "gate_rebuild: 源码变更触发重建，编译失败阻止旧测试运行。"
