#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build/standalone}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

DUCKDB_SOURCE_DIR="${DUCKDB_SOURCE_DIR:-}"
DUCKDB_VERSION_TAG="${DUCKDB_VERSION_TAG:-}"

usage() {
  cat <<USAGE
Usage:
  DUCKDB_SOURCE_DIR=/path/to/duckdb ./build.sh [configure|build|all|clean] [Debug|Release]

Environment:
  DUCKDB_SOURCE_DIR   Absolute path to DuckDB source checkout (required)
  DUCKDB_VERSION_TAG  Override extension ABI version tag (e.g. v1.4.4)
  BUILD_DIR           Build directory (default: $BUILD_DIR)
  JOBS                Parallel build jobs (default: CPU count)

Examples:
  DUCKDB_SOURCE_DIR=~/Work/duckdb ./build.sh all Release
  DUCKDB_SOURCE_DIR=~/Work/duckdb ./build.sh build
USAGE
}

ACTION="${1:-all}"
if [[ "${2:-}" != "" ]]; then
  BUILD_TYPE="$2"
fi

if [[ "$ACTION" == "clean" ]]; then
  rm -rf "$BUILD_DIR"
  echo "Cleaned: $BUILD_DIR"
  exit 0
fi

if [[ -z "$DUCKDB_SOURCE_DIR" ]]; then
  echo "Error: DUCKDB_SOURCE_DIR is required."
  usage
  exit 1
fi

configure() {
  local cmake_args=(
    -S "$ROOT_DIR"
    -B "$BUILD_DIR"
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DDUCKDB_SOURCE_DIR="$DUCKDB_SOURCE_DIR"
  )
  if [[ -n "$DUCKDB_VERSION_TAG" ]]; then
    cmake_args+=(-DOVERRIDE_GIT_DESCRIBE="$DUCKDB_VERSION_TAG")
  fi
  cmake "${cmake_args[@]}"
}

build() {
  cmake --build "$BUILD_DIR" --target vex_loadable_extension -j "$JOBS"
  local ext
  ext="$(find "$BUILD_DIR" -name 'vex.duckdb_extension' | head -n 1 || true)"
  if [[ -n "$ext" ]]; then
    echo "Built extension: $ext"
  else
    echo "Build finished, but vex.duckdb_extension path was not found automatically."
  fi
}

case "$ACTION" in
  configure)
    configure
    ;;
  build)
    configure
    build
    ;;
  all)
    configure
    build
    ;;
  *)
    usage
    exit 1
    ;;
esac
