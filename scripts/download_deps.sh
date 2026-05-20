#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LIB_DIR="$SCRIPT_DIR/../web/lib"

mkdir -p "$LIB_DIR"

echo "==> 下载前端依赖到 web/lib/ ..."

# marked.js - Markdown parser
curl -fsSLo "$LIB_DIR/marked.min.js" \
  "https://cdn.jsdelivr.net/npm/marked/marked.min.js"
echo "  ✓ marked.min.js"

# github-markdown-css - light theme
curl -fsSLo "$LIB_DIR/github-markdown-light.css" \
  "https://cdn.jsdelivr.net/npm/github-markdown-css/github-markdown-light.css"
echo "  ✓ github-markdown-light.css"

# github-markdown-css - dark theme
curl -fsSLo "$LIB_DIR/github-markdown-dark.css" \
  "https://cdn.jsdelivr.net/npm/github-markdown-css/github-markdown-dark.css"
echo "  ✓ github-markdown-dark.css"

# mermaid.js - Diagram rendering
curl -fsSLo "$LIB_DIR/mermaid.min.js" \
  "https://cdn.jsdelivr.net/npm/mermaid/dist/mermaid.min.js"
echo "  ✓ mermaid.min.js"

# katex - Math formula rendering
curl -fsSLo "$LIB_DIR/katex.min.js" \
  "https://cdn.jsdelivr.net/npm/katex/dist/katex.min.js"
echo "  ✓ katex.min.js"

curl -fsSLo "$LIB_DIR/katex.min.css" \
  "https://cdn.jsdelivr.net/npm/katex/dist/katex.min.css"
echo "  ✓ katex.min.css"

echo ""
echo "依赖下载完成。文件列表:"
ls -lh "$LIB_DIR"
