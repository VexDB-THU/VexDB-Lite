#!/bin/bash
# 准备官方 SQLite amalgamation，并用固定 SHA-256 防止下载内容漂移。
#
#   bash vendor_sqlite.sh --ensure  # 已存在则校验，缺失则下载
#   bash vendor_sqlite.sh --check   # 只校验，不访问网络
set -euo pipefail

MODE="${1:---ensure}"
if [ "$#" -gt 1 ]; then
    echo "用法：bash vendor_sqlite.sh [--ensure|--check]" >&2
    exit 2
fi
case "$MODE" in
    --ensure|--check) ;;
    *) echo "用法：bash vendor_sqlite.sh [--ensure|--check]" >&2; exit 2 ;;
esac

# 版本基线 >= 3.38.0（xBestIndex 感知 LIMIT/IN 约束的最低版本）。
# 默认固定 3.45.3。改版本时必须同时提供三个文件的 SHA-256。
DEFAULT_SQLITE_YEAR=2024
DEFAULT_SQLITE_AMALG_VERSION=3450300
SQLITE_YEAR="${SQLITE_YEAR:-$DEFAULT_SQLITE_YEAR}"
SQLITE_AMALG_VERSION="${SQLITE_AMALG_VERSION:-$DEFAULT_SQLITE_AMALG_VERSION}"
SQLITE3_C_SHA256="${SQLITE3_C_SHA256:-}"
SQLITE3_H_SHA256="${SQLITE3_H_SHA256:-}"
SQLITE3EXT_H_SHA256="${SQLITE3EXT_H_SHA256:-}"

if [ "$SQLITE_YEAR:$SQLITE_AMALG_VERSION" = \
        "$DEFAULT_SQLITE_YEAR:$DEFAULT_SQLITE_AMALG_VERSION" ]; then
    SQLITE3_C_SHA256="${SQLITE3_C_SHA256:-9ca336fbcbff9f1d78b4f45b6a19583fcc097192310dd2f5f6cd43b9a33d7d69}"
    SQLITE3_H_SHA256="${SQLITE3_H_SHA256:-882ad3c0448d0324fb3a6b1a85333a9173d539ac669c9972ae1f03722ff86282}"
    SQLITE3EXT_H_SHA256="${SQLITE3EXT_H_SHA256:-b184dd1586d935133d37ad76fa353faf0a1021ff2fdedeedcc3498fff74bbb94}"
fi
if [ -z "$SQLITE3_C_SHA256" ] || [ -z "$SQLITE3_H_SHA256" ] || \
        [ -z "$SQLITE3EXT_H_SHA256" ]; then
    echo "非默认 SQLite 版本必须提供 SQLITE3_C_SHA256、SQLITE3_H_SHA256 和 SQLITE3EXT_H_SHA256" >&2
    exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DIR="${SQLITE_VENDOR_DIR:-$SCRIPT_DIR/third_party/sqlite}"
URL="https://www.sqlite.org/${SQLITE_YEAR}/sqlite-amalgamation-${SQLITE_AMALG_VERSION}.zip"

sha256_file() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    elif command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        echo "找不到 shasum 或 sha256sum" >&2
        return 1
    fi
}

verify_file() {
    local path="$1"
    local expected="$2"
    local actual
    [ -f "$path" ] || {
        echo "缺少 SQLite 文件：$path" >&2
        return 1
    }
    actual="$(sha256_file "$path")"
    if [ "$actual" != "$expected" ]; then
        echo "SQLite 文件 SHA-256 不匹配：$path" >&2
        echo "期望：$expected" >&2
        echo "实际：$actual" >&2
        return 1
    fi
}

verify_dir() {
    local dir="$1"
    verify_file "$dir/sqlite3.c" "$SQLITE3_C_SHA256"
    verify_file "$dir/sqlite3.h" "$SQLITE3_H_SHA256"
    verify_file "$dir/sqlite3ext.h" "$SQLITE3EXT_H_SHA256"
}

if [ -f "$DIR/sqlite3.c" ] && [ -f "$DIR/sqlite3.h" ] && \
        [ -f "$DIR/sqlite3ext.h" ]; then
    verify_dir "$DIR"
    echo "[vendor] SQLite ${SQLITE_AMALG_VERSION} 已校验 -> $DIR"
    exit 0
fi

if [ "$MODE" = --check ]; then
    echo "SQLite amalgamation 不完整：$DIR" >&2
    exit 1
fi

command -v curl >/dev/null 2>&1 || { echo "找不到 curl" >&2; exit 1; }
command -v unzip >/dev/null 2>&1 || { echo "找不到 unzip" >&2; exit 1; }

TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/vexdb-sqlite.XXXXXX")"
trap 'rm -rf "$TMP_ROOT"' EXIT
ARCHIVE="$TMP_ROOT/sqlite-amalgamation.zip"
EXTRACTED="$TMP_ROOT/extracted"
mkdir -p "$EXTRACTED"

echo "[vendor] 下载 $URL"
curl --fail --location --silent --show-error --retry 2 --max-time 60 \
    --output "$ARCHIVE" "$URL"
unzip -q -j "$ARCHIVE" \
    '*/sqlite3.c' '*/sqlite3.h' '*/sqlite3ext.h' \
    -d "$EXTRACTED"
verify_dir "$EXTRACTED"

mkdir -p "$DIR"
install -m 0644 "$EXTRACTED/sqlite3.c" "$DIR/sqlite3.c"
install -m 0644 "$EXTRACTED/sqlite3.h" "$DIR/sqlite3.h"
install -m 0644 "$EXTRACTED/sqlite3ext.h" "$DIR/sqlite3ext.h"
verify_dir "$DIR"

echo "[vendor] SQLite ${SQLITE_AMALG_VERSION} 下载并校验完成 -> $DIR"
grep -m1 'SQLITE_VERSION ' "$DIR/sqlite3.h" || true
