#!/bin/bash
set -euo pipefail

DEMO_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$DEMO_DIR/build"
DIST_DIR="$DEMO_DIR/dist"
EMCMAKE="$(command -v emcmake || true)"
CMAKE_BIN="$(command -v cmake || true)"

if [ -z "$EMCMAKE" ] || [ -z "$CMAKE_BIN" ]; then
    echo "缺少 Emscripten 或 CMake。macOS 可运行: brew install emscripten cmake" >&2
    exit 1
fi

# Homebrew 的 Emscripten 包自带 LLVM/Binaryen，但在 PATH 中存在旧 Python 时
# 首次自动探测可能写出错误路径。Homebrew 环境显式使用 formula 内工具链；
# 其他系统沿用已激活的 emsdk 环境。
if command -v brew >/dev/null 2>&1; then
    EMSCRIPTEN_ROOT="$(brew --prefix emscripten)/libexec"
    BREW_PYTHON_PREFIX="$(brew --prefix python@3.14 2>/dev/null || true)"
    BREW_NODE_PREFIX="$(brew --prefix node 2>/dev/null || true)"
    [ -n "$BREW_PYTHON_PREFIX" ] && export EMSDK_PYTHON="$BREW_PYTHON_PREFIX/bin/python3.14"
    [ -n "$BREW_NODE_PREFIX" ] && export EM_NODE_JS="$BREW_NODE_PREFIX/bin/node"
    export EM_LLVM_ROOT="$EMSCRIPTEN_ROOT/llvm/bin"
    export EM_BINARYEN_ROOT="$EMSCRIPTEN_ROOT/binaryen"
fi
export EM_CACHE="$BUILD_DIR/emscripten-cache"
export CCACHE_TEMPDIR="$BUILD_DIR/ccache-tmp"
mkdir -p "$EM_CACHE" "$CCACHE_TEMPDIR"

BOOST_INC="${VEXDB_BOOST_INC:-}"
if [ -z "$BOOST_INC" ] && command -v brew >/dev/null 2>&1; then
    BOOST_INC="$(brew --prefix boost)/include"
fi
[ -n "$BOOST_INC" ] || {
    echo "找不到 Boost 头文件。请设置 VEXDB_BOOST_INC=/path/to/boost/include" >&2
    exit 1
}

"$EMCMAKE" "$CMAKE_BIN" -S "$DEMO_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DVEXDB_BOOST_INC="$BOOST_INC"
"$CMAKE_BIN" --build "$BUILD_DIR" --target vexdb_wasm -j 4

mkdir -p "$DIST_DIR"
cp "$BUILD_DIR/vexdb-wasm.js" "$DIST_DIR/"
cp "$BUILD_DIR/vexdb-wasm.wasm" "$DIST_DIR/"
cp "$DEMO_DIR/../ios/VexDBLiteDemo/Resources/embedding-demo-vectors.json" "$DIST_DIR/"
cp "$DEMO_DIR/../ios/VexDBLiteDemo/Resources/embedding-demo.txt" "$DIST_DIR/"
node "$DEMO_DIR/prepare-image-assets.mjs" \
    "$DEMO_DIR/../ios/VexDBLiteDemo/Resources" \
    "$DIST_DIR"
node "$DEMO_DIR/bundle.mjs"

echo "WASM 演示已生成: $DIST_DIR"
