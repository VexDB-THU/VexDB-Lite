#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build/standalone}"
CORE_BUILD_DIR="${CORE_BUILD_DIR:-$ROOT_DIR/build/core}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

DUCKDB_SOURCE_DIR="${DUCKDB_SOURCE_DIR:-}"
DUCKDB_VERSION_TAG="${DUCKDB_VERSION_TAG:-}"

usage() {
  cat <<USAGE
Usage:
  DUCKDB_SOURCE_DIR=/path/to/duckdb ./build.sh [configure|build|all|core|clean] [Debug|Release]

Environment:
  DUCKDB_SOURCE_DIR   Absolute path to DuckDB source checkout (required for duckdb targets)
  DUCKDB_VERSION_TAG  Override extension ABI version tag (e.g. v1.4.4)
  BUILD_DIR           DuckDB standalone build directory (default: $BUILD_DIR)
  CORE_BUILD_DIR      libvex-core build directory (default: $CORE_BUILD_DIR)
  JOBS                Parallel build jobs (default: CPU count)

Examples:
  DUCKDB_SOURCE_DIR=~/Work/duckdb ./build.sh all Release
  DUCKDB_SOURCE_DIR=~/Work/duckdb ./build.sh build
  ./build.sh core Release
USAGE
}

ACTION="${1:-all}"
if [[ "${2:-}" != "" ]]; then
  BUILD_TYPE="$2"
fi

if [[ "$ACTION" == "clean" ]]; then
  rm -rf "$BUILD_DIR" "$CORE_BUILD_DIR"
  echo "Cleaned: $BUILD_DIR"
  echo "Cleaned: $CORE_BUILD_DIR"
  exit 0
fi

configure_duckdb() {
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

build_duckdb() {
  cmake --build "$BUILD_DIR" --target vex_loadable_extension -j "$JOBS"
  local ext
  ext="$(find "$BUILD_DIR" -name 'vex.duckdb_extension' | head -n 1 || true)"
  if [[ -n "$ext" ]]; then
    echo "Built extension: $ext"
  else
    echo "Build finished, but vex.duckdb_extension path was not found automatically."
  fi
}

configure_core() {
  cmake -S "$ROOT_DIR/libvex-core" -B "$CORE_BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
}

build_core() {
  cmake --build "$CORE_BUILD_DIR" -j "$JOBS"
  local core_lib
  core_lib="$(find "$CORE_BUILD_DIR" -name 'libvex_core.a' | head -n 1 || true)"
  if [[ -n "$core_lib" ]]; then
    echo "Built core library: $core_lib"
  else
    echo "Core build finished, but libvex_core.a path was not found automatically."
  fi
}

case "$ACTION" in
  configure)
    if [[ -z "$DUCKDB_SOURCE_DIR" ]]; then
      echo "Error: DUCKDB_SOURCE_DIR is required for 'configure'."
      usage
      exit 1
    fi
    configure_duckdb
    ;;
  build)
    if [[ -z "$DUCKDB_SOURCE_DIR" ]]; then
      echo "Error: DUCKDB_SOURCE_DIR is required for 'build'."
      usage
      exit 1
    fi
    configure_duckdb
    build_duckdb
    ;;
  all)
    if [[ -z "$DUCKDB_SOURCE_DIR" ]]; then
      echo "Error: DUCKDB_SOURCE_DIR is required for 'all'."
      usage
      exit 1
    fi
    configure_duckdb
    build_duckdb
    ;;
  core)
    configure_core
    build_core
    ;;
  *)
    usage
    exit 1
    ;;
esac
