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

# 每次先执行增量构建，让构建系统按完整依赖判断 .cpp/.hpp 是否变化。
# 不能只比较 CMakeLists 的时间；构建失败立即退出，禁止运行上一次成功的旧测试程序。
"${REPO_ROOT}/scripts/build.sh"
echo "==> 运行测试: ${BUILD_DIR}"
ctest --test-dir "$BUILD_DIR" --output-on-failure
