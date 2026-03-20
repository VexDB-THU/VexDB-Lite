#!/bin/bash
# VexDB-Lite wheel build script for Windows (Git Bash / MSYS2)
#
# Usage: bash build_wheel_windows.sh [output_dir]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DUCKDB_DIR="$PROJECT_DIR"
OUTPUT_DIR_RAW="${1:-$PROJECT_DIR/dist}"
mkdir -p "$OUTPUT_DIR_RAW"
OUTPUT_DIR="$(cd "$OUTPUT_DIR_RAW" && pwd)"
DUCKDB_VERSION="1.5.0"

DUCKDB_PY_SRC="$(mktemp -d)"
cleanup() { rm -rf "$DUCKDB_PY_SRC"; }
trap cleanup EXIT

echo "=== VexDB-Lite Wheel Builder (Windows) ==="
echo "DuckDB version: $DUCKDB_VERSION"

# Step 1: Download DuckDB Python source
pip download "duckdb==$DUCKDB_VERSION" --no-binary :all: -d "$DUCKDB_PY_SRC/download"
cd "$DUCKDB_PY_SRC"
tar xzf "download/duckdb-$DUCKDB_VERSION.tar.gz"

BUILD_DIR="$DUCKDB_PY_SRC/duckdb-$DUCKDB_VERSION"

# Step 2: Inject VEX extension
cp -r "$DUCKDB_DIR/extension/vex" "$BUILD_DIR/external/duckdb/extension/vex"

# Step 3: Inject physical_create_graph_index
cp "$DUCKDB_DIR/src/execution/operator/schema/physical_create_graph_index.cpp" \
   "$BUILD_DIR/external/duckdb/src/execution/operator/schema/"
cp "$DUCKDB_DIR/src/include/duckdb/execution/operator/schema/physical_create_graph_index.hpp" \
   "$BUILD_DIR/external/duckdb/src/include/duckdb/execution/operator/schema/"

SCHEMA_CMAKE="$BUILD_DIR/external/duckdb/src/execution/operator/schema/CMakeLists.txt"
if ! grep -q "physical_create_graph_index" "$SCHEMA_CMAKE"; then
    sed -i 's/physical_create_index\.cpp/physical_create_index.cpp\n  physical_create_graph_index.cpp/' "$SCHEMA_CMAKE"
fi

# Step 4: Add VEX to extension config
if ! grep -q "duckdb_extension_load(vex)" "$BUILD_DIR/external/duckdb/extension/extension_config.cmake"; then
    echo 'duckdb_extension_load(vex)' >> "$BUILD_DIR/external/duckdb/extension/extension_config.cmake"
fi

# Step 5: Build
export CMAKE_ARGS="-DBUILD_EXTENSIONS='core_functions;parquet;icu;json;vex'"
export CMAKE_BUILD_PARALLEL_LEVEL=${NUMBER_OF_PROCESSORS:-4}

cd "$BUILD_DIR"
mkdir -p "$OUTPUT_DIR"

echo ">>> Building wheel (parallel: $CMAKE_BUILD_PARALLEL_LEVEL)..."
pip wheel . --no-build-isolation --no-deps -w "$OUTPUT_DIR"

echo ""
echo "=== Build complete ==="
ls -lh "$OUTPUT_DIR"/duckdb-*.whl
