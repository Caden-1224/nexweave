#!/usr/bin/env bash
# 测试门禁脚本。
#
# 职责：构建完成后运行全部 CTest 用例，失败时给出明确诊断（哪个测试、
#       哪些断言、退出码），作为开发门禁的可重复执行入口。
# 输入前提：scripts/build.sh 已成功执行，或本脚本会自动先构建一次。
# 输出后置：全部通过时退出码 0 并打印汇总；任一失败时退出码非零，
#           通过 --output-on-failure 直接展示失败用例输出。
# 可重入：重复执行结果一致；BUILD_DIR 与 build.sh 共用同一约定。

set -euo pipefail

# 同 build.sh：预检前不依赖外部命令，用参数展开定位仓库根目录。
REPO_ROOT="$(cd "${BASH_SOURCE[0]%/*}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build}"

if ! command -v ctest >/dev/null 2>&1; then
  echo "错误：缺少必需工具 'ctest'。" >&2
  echo "       Ubuntu/Debian 可执行: sudo apt install cmake" >&2
  exit 1
fi

# 构建目录缺失或已过期（CMakeLists.txt 比测试定义新）时，先走一次
# 构建门禁，保证测试对象与当前源码一致；只检查文件存在会让部分失败
# 或陈旧的构建目录静默跑旧产物。
if [ ! -f "$BUILD_DIR/CTestTestfile.cmake" ] || \
   [ "$REPO_ROOT/CMakeLists.txt" -nt "$BUILD_DIR/CTestTestfile.cmake" ]; then
  echo "==> 构建目录缺失或已过期，先执行构建门禁。"
  "${REPO_ROOT}/scripts/build.sh"
fi

echo "==> 运行测试: ${BUILD_DIR}"
ctest --test-dir "$BUILD_DIR" --output-on-failure
