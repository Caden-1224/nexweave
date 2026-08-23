#!/usr/bin/env bash
# 构建门禁脚本。
#
# 职责：以可重复的方式完成配置与编译，作为仓库的统一构建入口。
# 输入前提：仓库根目录存在顶层 CMakeLists.txt；环境可访问 cmake 与 C++ 编译器。
# 输出后置：成功时在 BUILD_DIR（默认 build/）下产生可执行测试与库产物；
#           失败时以非零码退出，并在 stderr 给出可读原因（缺工具/配置错误/编译错误）。
# 可重入：脚本可以重复执行；重复运行不改变产物语义，测试结果一致。
# 用法：BUILD_DIR=/自定义路径 ./scripts/build.sh 可覆盖构建目录；
#       BUILD_TYPE=Debug ./scripts/build.sh 可切换构建类型（默认 Release）。

set -euo pipefail

# 定位仓库根目录：本脚本位于 <root>/scripts/ 下，取其父目录的父目录。
# 用参数展开代替 dirname 命令：工具预检之前不能依赖任何外部命令，
# 否则在 PATH 被清空的环境下会先产生噪音错误。
REPO_ROOT="$(cd "${BASH_SOURCE[0]%/*}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

# 必需工具预检：任何一项缺失都提前失败并给出安装提示，
# 避免半途配置失败后留下难以理解的错误堆栈。
missing=0
for tool in cmake g++ make; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "错误：缺少必需工具 '${tool}'。" >&2
    echo "       Ubuntu/Debian 可执行: sudo apt install ${tool}" >&2
    missing=1
  fi
done
if [ "$missing" -ne 0 ]; then
  exit 1
fi

echo "==> 配置: cmake -S ${REPO_ROOT} -B ${BUILD_DIR} (${BUILD_TYPE})"
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"

echo "==> 编译: ${BUILD_DIR}"
cmake --build "$BUILD_DIR"

echo "==> 构建完成: ${BUILD_DIR}"
