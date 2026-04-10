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

# Add VEX to extension config
if ! grep -q "duckdb_extension_load(vex)" "$BUILD_DIR/external/duckdb/extension/extension_config.cmake"; then
    echo 'duckdb_extension_load(vex)' >> "$BUILD_DIR/external/duckdb/extension/extension_config.cmake"
fi

echo "VEX source prepared at: $BUILD_DIR"
