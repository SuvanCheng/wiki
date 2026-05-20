#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

BIN_DIR="$SCRIPT_DIR/bin"
mkdir -p "$BIN_DIR"

# 版本号：优先用 git tag，否则用日期
if [ -n "${QA_VERSION:-}" ]; then
  VERSION="$QA_VERSION"
elif git rev-parse --is-inside-work-tree &>/dev/null; then
  VERSION="$(git describe --tags --always --dirty 2>/dev/null || echo 'dev')"
else
  VERSION="$(date +%Y.%m.%d)"
fi

LDFLAGS="-s -w -X main.version=$VERSION"

echo "========================================="
echo "  QA Wiki — 跨平台编译"
echo "  版本: $VERSION"
echo "  纯 Go SQLite，无需 CGO，无需交叉工具链"
echo "========================================="
echo ""

# 确保依赖已下载
if [ ! -f web/lib/marked.min.js ]; then
  echo "[!] 未找到前端依赖，正在自动下载..."
  bash scripts/download_deps.sh
  echo ""
fi

echo "[1/3] Windows (amd64) ..."
CGO_ENABLED=0 GOOS=windows GOARCH=amd64 go build -ldflags "$LDFLAGS" -o "$BIN_DIR/qa-wiki.exe" ./cmd/qa-wiki
echo "  -> bin/qa-wiki.exe"

echo "[2/3] macOS (Apple Silicon, arm64) ..."
CGO_ENABLED=0 GOOS=darwin GOARCH=arm64 go build -ldflags "$LDFLAGS" -o "$BIN_DIR/qa-wiki-mac-arm" ./cmd/qa-wiki
echo "  -> bin/qa-wiki-mac-arm"

echo "[3/3] Linux (amd64) ..."
CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -ldflags "$LDFLAGS" -o "$BIN_DIR/qa-wiki-linux" ./cmd/qa-wiki
echo "  -> bin/qa-wiki-linux"

echo ""
echo "[*] 编译 Export-Public 工具 (当前平台) ..."
CGO_ENABLED=0 go build -ldflags "$LDFLAGS" -o "$BIN_DIR/export-public" ./cmd/export-public
echo "  -> bin/export-public"

echo ""
echo "========================================="
echo "  编译完成！版本: $VERSION"
echo "  输出目录: bin/"
echo "========================================="
ls -lh "$BIN_DIR"
