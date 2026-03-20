#!/bin/bash
# VexDB-Lite wheel build script
# Builds a DuckDB wheel with VEX extension statically linked
#
# Prerequisites:
#   pip install scikit-build-core pybind11 setuptools_scm ninja
#
# Usage:
#   ./packaging/build_wheel.sh [output_dir]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DUCKDB_DIR="$PROJECT_DIR"
# Resolve to absolute path to survive cd's later
OUTPUT_DIR="$(cd "$(dirname "${1:-$PROJECT_DIR/dist}")" 2>/dev/null && pwd)/$(basename "${1:-$PROJECT_DIR/dist}")"
mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"
DUCKDB_VERSION="1.5.0"

# Use a unique temp dir to avoid conflicts in CI or multi-user environments
DUCKDB_PY_SRC="$(mktemp -d "${TMPDIR:-/tmp}/vexdb-build.XXXXXX")"
cleanup() { rm -rf "$DUCKDB_PY_SRC"; }
trap cleanup EXIT

echo "=== VexDB-Lite Wheel Builder ==="
echo "DuckDB version: $DUCKDB_VERSION"
echo "Build dir: $DUCKDB_PY_SRC"
echo "Output: $OUTPUT_DIR"

# Step 1: Download DuckDB Python source
echo ">>> Downloading DuckDB $DUCKDB_VERSION source..."
pip3 download "duckdb==$DUCKDB_VERSION" --no-binary :all: -d "$DUCKDB_PY_SRC/download"
cd "$DUCKDB_PY_SRC"
tar xzf "download/duckdb-$DUCKDB_VERSION.tar.gz"

BUILD_DIR="$DUCKDB_PY_SRC/duckdb-$DUCKDB_VERSION"

# Step 2: Copy VEX extension
echo ">>> Copying VEX extension..."
cp -r "$DUCKDB_DIR/extension/vex" "$BUILD_DIR/external/duckdb/extension/vex"

# Step 3: Copy physical_create_graph_index
cp "$DUCKDB_DIR/src/execution/operator/schema/physical_create_graph_index.cpp" \
   "$BUILD_DIR/external/duckdb/src/execution/operator/schema/"
cp "$DUCKDB_DIR/src/include/duckdb/execution/operator/schema/physical_create_graph_index.hpp" \
   "$BUILD_DIR/external/duckdb/src/include/duckdb/execution/operator/schema/"

# Add to CMakeLists if not already there (cross-platform sed)
SCHEMA_CMAKE="$BUILD_DIR/external/duckdb/src/execution/operator/schema/CMakeLists.txt"
if ! grep -q "physical_create_graph_index" "$SCHEMA_CMAKE"; then
    if [[ "$(uname)" == "Darwin" ]]; then
        sed -i '' 's/physical_create_index\.cpp/physical_create_index.cpp\n  physical_create_graph_index.cpp/' "$SCHEMA_CMAKE"
    else
        sed -i 's/physical_create_index\.cpp/physical_create_index.cpp\n  physical_create_graph_index.cpp/' "$SCHEMA_CMAKE"
    fi
fi

# Step 4: Add VEX to extension config
if ! grep -q "duckdb_extension_load(vex)" "$BUILD_DIR/external/duckdb/extension/extension_config.cmake"; then
    echo 'duckdb_extension_load(vex)' >> "$BUILD_DIR/external/duckdb/extension/extension_config.cmake"
fi

# Step 5: Platform-specific build settings
CMAKE_EXTRA=""
if [[ "$(uname)" == "Darwin" ]]; then
    ARCH=$(python3 -c "import platform; print(platform.machine())")
    export MACOSX_DEPLOYMENT_TARGET=11.0
    CMAKE_EXTRA="-DCMAKE_OSX_ARCHITECTURES=$ARCH -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0"
fi

# Add VEX to the extension build list
export CMAKE_ARGS="${CMAKE_EXTRA} -DBUILD_EXTENSIONS='core_functions;parquet;icu;json;vex'"

# Step 6: Build wheel
echo ">>> Building wheel..."
cd "$BUILD_DIR"
mkdir -p "$OUTPUT_DIR"

# Use all available CPU cores for parallel compilation
if [[ "$(uname)" == "Darwin" ]]; then
    export CMAKE_BUILD_PARALLEL_LEVEL=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
else
    export CMAKE_BUILD_PARALLEL_LEVEL=$(nproc 2>/dev/null || echo 4)
fi
echo "    Parallel jobs: $CMAKE_BUILD_PARALLEL_LEVEL"

pip3 wheel . --no-build-isolation --no-deps -w "$OUTPUT_DIR"

# Step 7: Patch libc++ linking on macOS (only needed for macOS 26+ / Apple Clang 17+)
# Skip in CI environments where system libc++ is fine
if [[ "$(uname)" == "Darwin" && -z "${CI:-}" ]]; then
    WHEEL_FILE=$(ls -t "$OUTPUT_DIR"/duckdb-*.whl | head -1)
    echo ">>> Patching wheel for libc++ compatibility..."

    PATCH_DIR="$(mktemp -d)"
    cd "$PATCH_DIR"
    unzip -q "$WHEEL_FILE"

    SO_FILE=$(find . -name "_duckdb*.so" | head -1)
    if [ -n "$SO_FILE" ] && otool -L "$SO_FILE" | grep -q "/usr/lib/libc++.1.dylib"; then
        install_name_tool -change /usr/lib/libc++.1.dylib @rpath/libc++.1.dylib "$SO_FILE" 2>/dev/null || true
        echo ">>> Patched: $SO_FILE"

        # Repack wheel
        rm "$WHEEL_FILE"
        zip -q -r "$WHEEL_FILE" .
        echo ">>> Repacked: $(basename "$WHEEL_FILE")"
    fi

    rm -rf "$PATCH_DIR"
fi

echo ""
echo "=== Build complete ==="
ls -lh "$OUTPUT_DIR"/duckdb-*.whl
echo ""
echo "Install with:"
echo "  pip3 install \"$OUTPUT_DIR\"/duckdb-*.whl --force-reinstall --no-deps"
