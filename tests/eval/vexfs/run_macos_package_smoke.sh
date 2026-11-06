#!/bin/bash
set -euo pipefail

# 在没有源码和 Xcode 构建目录的 macOS 上验证已安装发行包。
# usage: bash run_macos_package_smoke.sh [installed-vexdb]

VEXDB="${1:-$HOME/.local/bin/vexdb}"
[ -x "$VEXDB" ] || { echo "找不到已安装 vexdb：$VEXDB" >&2; exit 1; }

ROOT="$(mktemp -d -t vexdb-lite-macos-package-smoke)"
DB="$ROOT/workspace.sqlite3"
RESTORED_DB="$ROOT/restored.sqlite3"
ARCHIVE="$ROOT/workspace.vexfs"
MOUNT_POINT="$ROOT/mnt"
WORKSPACE=clean-install
CHECKS=0
EXPECTED_VERSION="${VEXDB_LITE_EXPECTED_VERSION:-}"
PAYLOAD="vexdb package smoke"
MOUNTED=0

fs() {
    "$VEXDB" fs --db "$DB" --workspace "$WORKSPACE" "$@"
}

check_equal() {
    local expected="$1"
    local actual="$2"
    local message="$3"
    if [ "$actual" != "$expected" ]; then
        echo "$message：期望 '$expected'，实际 '$actual'" >&2
        exit 1
    fi
    CHECKS=$((CHECKS + 1))
}

check_file() {
    [ -f "$1" ] || { echo "缺少文件：$1" >&2; exit 1; }
    CHECKS=$((CHECKS + 1))
}

wait_for_publish() {
    local attempt state
    attempt=0
    while [ "$attempt" -lt 100 ]; do
        state="$(fs --json doctor)"
        if printf '%s' "$state" | /usr/bin/python3 -c \
                'import json,sys; value=json.load(sys.stdin)["database"]; raise SystemExit(not (value["pending_handles"] == 0 and value["staging_bytes"] == 0))'; then
            return 0
        fi
        attempt=$((attempt + 1))
        sleep 0.1
    done
    echo "等待 NFS 后台发布超时：$state" >&2
    return 1
}

cleanup() {
    if [ "$MOUNTED" = 1 ]; then
        fs unmount --force "$MOUNT_POINT" >/dev/null 2>&1 || true
    fi
    if [ "${VEXDB_LITE_KEEP_SMOKE:-0}" = 1 ]; then
        echo "保留测试目录：$ROOT" >&2
    else
        rm -rf "$ROOT"
    fi
}
trap cleanup EXIT

check_equal 0.9.0 "$($VEXDB :memory: 'SELECT vexfs_contract_version();')" "文件合同错误"
VERSION_OUTPUT="$("$VEXDB" --version)"
if [ -n "$EXPECTED_VERSION" ]; then
    printf '%s\n' "$VERSION_OUTPUT" | grep -Fq "$EXPECTED_VERSION"
else
    printf '%s\n' "$VERSION_OUTPUT" | grep -Eq 'vexdb-lite 0\.1\.0-preview\.[0-9]+'
fi
CHECKS=$((CHECKS + 1))

mkdir -p "$MOUNT_POINT"
fs setup >/dev/null
DOCTOR="$(fs --json doctor)"
printf '%s' "$DOCTOR" | /usr/bin/python3 -c \
    'import json,sys; value=json.load(sys.stdin); assert value["mount_ready"] is True; assert value["mount_driver"] == "NFSv3"; assert value["database"]["schema_version"] == "0.9.0"'
MOUNT_DRIVER="$(printf '%s' "$DOCTOR" | /usr/bin/python3 -c \
    'import json,sys; print(json.load(sys.stdin)["mount_driver"])')"
CHECKS=$((CHECKS + 3))

fs mount "$MOUNT_POINT"
MOUNTED=1
mkdir -p "$MOUNT_POINT/project/src"
printf '%s\n' "$PAYLOAD" > "$MOUNT_POINT/project/src/message.txt"
check_equal "$PAYLOAD" "$(cat "$MOUNT_POINT/project/src/message.txt")" "cat 内容错误"
grep -q "$PAYLOAD" "$MOUNT_POINT/project/src/message.txt"
CHECKS=$((CHECKS + 1))

cp "$MOUNT_POINT/project/src/message.txt" "$MOUNT_POINT/project/src/copy.txt"
mv "$MOUNT_POINT/project/src/copy.txt" "$MOUNT_POINT/project/src/moved.txt"
check_file "$MOUNT_POINT/project/src/moved.txt"
ln "$MOUNT_POINT/project/src/message.txt" "$MOUNT_POINT/project/src/message-hard.txt"
ln -s message.txt "$MOUNT_POINT/project/src/message-link.txt"
check_equal "$PAYLOAD" "$(cat "$MOUNT_POINT/project/src/message-hard.txt")" "hardlink 内容错误"
check_equal "$PAYLOAD" "$(cat "$MOUNT_POINT/project/src/message-link.txt")" "symlink 内容错误"

printf '#!/bin/sh\nprintf "agent\\n"\n' > "$MOUNT_POINT/project/run.sh"
chmod 0755 "$MOUNT_POINT/project/run.sh"
check_equal agent "$($MOUNT_POINT/project/run.sh)" "可执行权限错误"

/usr/bin/python3 -c \
    'from pathlib import Path; import json,sys; Path(sys.argv[1]).write_text(json.dumps({"ok": True}))' \
    "$MOUNT_POINT/project/result.json"
check_equal True "$(/usr/bin/python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["ok"])' "$MOUNT_POINT/project/result.json")" "Python 文件读写错误"

if [ "$MOUNT_DRIVER" = NFSv3 ]; then
    set +e
    XATTR_ERROR="$(/usr/bin/xattr -w user.vexdb.package-smoke yes \
        "$MOUNT_POINT/project/src/message.txt" 2>&1)"
    XATTR_STATUS=$?
    set -e
    [ "$XATTR_STATUS" -ne 0 ] || {
        echo "NFSv3 不应伪装支持 xattr" >&2
        exit 1
    }
    case "$XATTR_ERROR" in
        *'Operation not supported'*|*'Operation not permitted'*) ;;
        *) echo "NFSv3 xattr 错误不稳定：$XATTR_ERROR" >&2; exit 1 ;;
    esac
    CHECKS=$((CHECKS + 1))
    [ ! -e "$MOUNT_POINT/project/src/._message.txt" ] || {
        echo "NFSv3 xattr 失败后生成了 AppleDouble 文件" >&2
        exit 1
    }
    CHECKS=$((CHECKS + 1))
else
    /usr/bin/xattr -w user.vexdb.package-smoke yes "$MOUNT_POINT/project/src/message.txt"
    check_equal yes "$(/usr/bin/xattr -p user.vexdb.package-smoke \
        "$MOUNT_POINT/project/src/message.txt")" "xattr 错误"
fi

git -C "$MOUNT_POINT/project" init -q
git -C "$MOUNT_POINT/project" config user.name VexDB-Test
git -C "$MOUNT_POINT/project" config user.email test@vexdb.local
git -C "$MOUNT_POINT/project" add .
git -C "$MOUNT_POINT/project" commit -qm baseline
check_equal '' "$(git -C "$MOUNT_POINT/project" status --porcelain)" "Git 初始工作区不干净"

wait_for_publish
fs snapshot create remote-baseline >/dev/null
printf 'changed\n' > "$MOUNT_POINT/project/src/message.txt"
printf 'temporary\n' > "$MOUNT_POINT/project/temporary.txt"
fs snapshot restore remote-baseline >/dev/null
check_equal "$PAYLOAD" "$(cat "$MOUNT_POINT/project/src/message.txt")" "快照没有恢复旧内容"
[ ! -e "$MOUNT_POINT/project/temporary.txt" ] || { echo "快照没有删除新增文件" >&2; exit 1; }
CHECKS=$((CHECKS + 1))
check_equal '' "$(git -C "$MOUNT_POINT/project" status --porcelain)" "快照恢复后 Git 工作区不干净"

fs index enable >/dev/null
fs grep "$PAYLOAD" /project | grep -q '/project/src/message.txt'
CHECKS=$((CHECKS + 1))

fs unmount "$MOUNT_POINT"
MOUNTED=0
set +e
MOUNT_STATUS="$(fs --json mount status "$MOUNT_POINT")"
MOUNT_STATUS_CODE=$?
set -e
check_equal 1 "$MOUNT_STATUS_CODE" "未挂载状态退出码错误"
check_equal '[]' "$MOUNT_STATUS" "卸载后仍有 mount session"

CHECK="$(fs --json check)"
printf '%s' "$CHECK" | /usr/bin/python3 -c \
    'import json,sys; value=json.load(sys.stdin); assert value["ok"] is True; assert value["content_model"] == "chunked-manifest-v1"; assert value["issue_count"] == 0'
CHECKS=$((CHECKS + 3))

fs export --snapshot remote-baseline --output "$ARCHIVE" >/dev/null
"$VEXDB" fs archive verify "$ARCHIVE" | /usr/bin/python3 -c \
    'import json,sys; value=json.load(sys.stdin); assert value["ok"] is True; assert value["format_version"] == 2'
CHECKS=$((CHECKS + 2))
"$VEXDB" fs --db "$RESTORED_DB" --workspace restored import "$ARCHIVE" >/dev/null
check_equal "$PAYLOAD" \
    "$("$VEXDB" fs --db "$RESTORED_DB" --workspace restored cat /project/src/message.txt)" \
    "归档恢复内容错误"
"$VEXDB" fs --db "$RESTORED_DB" --workspace restored --json check | /usr/bin/python3 -c \
    'import json,sys; value=json.load(sys.stdin); assert value["ok"] is True; assert value["content_model"] == "chunked-manifest-v1"'
CHECKS=$((CHECKS + 2))

COUNTS="$($VEXDB "$DB" 'SELECT (SELECT count(*) FROM _vexfs_manifests) || "|" || (SELECT count(*) FROM _vexfs_chunks);')"
printf '%s' "$COUNTS" | /usr/bin/python3 -c \
    'import sys; manifests,chunks=map(int,sys.stdin.read().split("|")); assert manifests > 0; assert chunks > 0'
CHECKS=$((CHECKS + 2))

echo "VEXDB MACOS PACKAGE SMOKE: PASS ($CHECKS checks)"
