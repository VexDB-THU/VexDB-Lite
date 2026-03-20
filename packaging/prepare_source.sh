#!/bin/bash
# Prepare DuckDB source with VEX extension injected.
# Used by cibuildwheel's BEFORE_BUILD to set up the source tree.
#
# Usage: bash prepare_source.sh <project_root>

set -e

PROJECT_DIR="${1:-.}"
DUCKDB_DIR="$PROJECT_DIR"
DUCKDB_VERSION="1.5.0"

# Download and extract DuckDB sdist into a well-known location
WORK_DIR="/tmp/vexdb-sdist"
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"

pip download "duckdb==$DUCKDB_VERSION" --no-binary :all: -d "$WORK_DIR/download"
cd "$WORK_DIR"
tar xzf "download/duckdb-$DUCKDB_VERSION.tar.gz"

BUILD_DIR="$WORK_DIR/duckdb-$DUCKDB_VERSION"

# Inject VEX extension
cp -r "$DUCKDB_DIR/extension/vex" "$BUILD_DIR/external/duckdb/extension/vex"

# Inject physical_create_graph_index
cp "$DUCKDB_DIR/src/execution/operator/schema/physical_create_graph_index.cpp" \
   "$BUILD_DIR/external/duckdb/src/execution/operator/schema/"
cp "$DUCKDB_DIR/src/include/duckdb/execution/operator/schema/physical_create_graph_index.hpp" \
   "$BUILD_DIR/external/duckdb/src/include/duckdb/execution/operator/schema/"

SCHEMA_CMAKE="$BUILD_DIR/external/duckdb/src/execution/operator/schema/CMakeLists.txt"
if ! grep -q "physical_create_graph_index" "$SCHEMA_CMAKE"; then
    sed -i 's/physical_create_index\.cpp/physical_create_index.cpp\n  physical_create_graph_index.cpp/' "$SCHEMA_CMAKE"
fi

# Add VEX to extension config
if ! grep -q "duckdb_extension_load(vex)" "$BUILD_DIR/external/duckdb/extension/extension_config.cmake"; then
    echo 'duckdb_extension_load(vex)' >> "$BUILD_DIR/external/duckdb/extension/extension_config.cmake"
fi

echo "VEX source prepared at: $BUILD_DIR"
