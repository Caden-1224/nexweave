#!/usr/bin/env bash
# 生成 RK3576 源码部署包，并在打包前后拒绝模型、厂商运行库、动态库和敏感材料。
#
# 用途：把当前 Git 工作区中会进入部署的源码文件打成可审查压缩包，供板端 build-strict 构建。
# 边界：模型、SDK、动态库、构建缓存、日志、凭据、令牌和私钥不进入部署包；脚本只读取 Git
# 索引与未忽略文件，不修改源码，不运行构建。
#
# 用法：bash scripts/package_rk3576_deployment.sh [输出.tar.gz]
# 默认输出 build/rk3576-deployment-source.tar.gz。
set -euo pipefail

ROOT="$(cd "${BASH_SOURCE[0]%/*}/.." && pwd)"
OUTPUT="${1:-${ROOT}/build/rk3576-deployment-source.tar.gz}"
OUTPUT_DIR="$(dirname "$OUTPUT")"

if ! command -v git >/dev/null 2>&1 || ! command -v tar >/dev/null 2>&1; then
  echo "打包失败: 需要 git 与 tar" >&2
  exit 1
fi
if ! git -C "$ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "打包失败: 源码根目录不是 Git 工作区: $ROOT" >&2
  exit 1
fi

mkdir -p "$OUTPUT_DIR"
FILE_LIST="$(mktemp /tmp/nexweave-package-files.XXXXXX)"
ARCHIVE_LIST="$(mktemp /tmp/nexweave-package-archive.XXXXXX)"
trap 'rm -f "$FILE_LIST" "$ARCHIVE_LIST"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# 收集 Git 中会进入版本库的路径，并带上未忽略的新文件；构建目录、日志和编译产物由
# .gitignore 排除，不能依赖“当前目录里看起来没有”这种一次性事实。
git -C "$ROOT" ls-files --cached --others --exclude-standard -z >"$FILE_LIST"

forbidden() {
  local path="$1"
  case "$path" in
    .git/*|build/*|build-*/*|deps/*|models/*|logs/*|Testing/*)
      return 0
      ;;
    *.rkllm|*.rknn|*.onnx|*.so|*.so.*|*.dylib|*.dll|*.a|*.bin|*.o|*.exe)
      return 0
      ;;
    *.pem|*.key|*.crt|id_rsa|id_ed25519|.env|*.token|*password*|*secret*)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

count=0
while IFS= read -r -d '' path; do
  if [ -z "$path" ]; then
    continue
  fi
  if forbidden "$path"; then
    echo "部署包拒绝禁止路径: $path" >&2
    exit 1
  fi
  count=$((count + 1))
done <"$FILE_LIST"

if [ "$count" -eq 0 ]; then
  echo "打包失败: 没有可打包文件" >&2
  exit 1
fi

tar -C "$ROOT" -czf "$OUTPUT" --null -T "$FILE_LIST"
tar -tzf "$OUTPUT" >"$ARCHIVE_LIST"
while IFS= read -r path; do
  [ -n "$path" ] || continue
  if forbidden "$path"; then
    echo "部署包归档内发现禁止路径: $path" >&2
    exit 1
  fi
done <"$ARCHIVE_LIST"

echo "部署包生成成功: $OUTPUT"
echo "部署包文件数: $count"
echo "部署包 SHA256: $(sha256sum "$OUTPUT" | awk '{print $1}')"
