#!/usr/bin/env bash
set -euo pipefail

PHASE="${VEXFS_NODE_PHASE:?VEXFS_NODE_PHASE is required}"
CLI="${VEXFS_NODE_CLI:-$HOME/.local/bin/vexdb}"
DSN="${VEXFS_NODE_DSN:?VEXFS_NODE_DSN is required}"
WORKSPACE="${VEXFS_NODE_WORKSPACE:-pg-remote-macos}"
CHECKS=0
MOUNT_POINT="$(mktemp -d "${TMPDIR:-/tmp}/vexfs-pg-remote-mount.XXXXXX")"
MOUNTED=false

[ "$(uname -s)" = Darwin ] || { echo "该节点测试只支持 macOS" >&2; exit 2; }
[ -x "$CLI" ] || { echo "找不到 vexdb CLI：$CLI" >&2; exit 2; }

vexfs() {
    "$CLI" fs --backend pg --dsn "$DSN" --workspace "$WORKSPACE" "$@"
}

pass() {
    CHECKS=$((CHECKS + 1))
}

equals() {
    local actual="$1" expected="$2" description="$3"
    if [ "$actual" != "$expected" ]; then
        echo "$description：期望 [$expected]，实际 [$actual]" >&2
        exit 1
    fi
    pass
}

contains() {
    local actual="$1" expected="$2" description="$3"
    case "$actual" in
        *"$expected"*) pass ;;
        *) echo "$description：缺少 [$expected]，实际 [$actual]" >&2; exit 1 ;;
    esac
}

assert_file() {
    [ -f "$1" ] || { echo "$2：文件不存在 $1" >&2; exit 1; }
    pass
}

assert_missing() {
    [ ! -e "$1" ] && [ ! -L "$1" ] || { echo "$2：路径仍存在 $1" >&2; exit 1; }
    pass
}

mount_workspace() {
    vexfs mount "$MOUNT_POINT" >/dev/null
    MOUNTED=true
    contains "$(vexfs --json mount status "$MOUNT_POINT")" \
        "\"workspace\":\"$WORKSPACE\"" "FSKit 挂载状态"
}

unmount_workspace() {
    if [ "$MOUNTED" = true ]; then
        vexfs unmount "$MOUNT_POINT" >/dev/null
        MOUNTED=false
    fi
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    if [ "$MOUNTED" = true ]; then
        vexfs unmount --force "$MOUNT_POINT" >/dev/null 2>&1 || true
    fi
    rmdir "$MOUNT_POINT" >/dev/null 2>&1 || true
    exit "$status"
}
trap cleanup EXIT INT TERM

verify_local_baseline() {
    assert_file "$MOUNT_POINT/shared/from-local.txt" "本机文件"
    equals "$(cat "$MOUNT_POINT/shared/from-local.txt")" "created-on-first-mac" "本机文件内容"
    equals "$(readlink "$MOUNT_POINT/shared/from-local-link")" "from-local.txt" "符号链接"
    equals "$(stat -f '%i' "$MOUNT_POINT/shared/from-local.txt")" \
        "$(stat -f '%i' "$MOUNT_POINT/shared/from-local-hard")" "hardlink inode"
    equals "$(xattr -p com.vexfs.origin "$MOUNT_POINT/shared/from-local.txt")" \
        "first-mac" "扩展属性"
    equals "$("$MOUNT_POINT/bin/from-local.sh")" "script-from-first-mac" "可执行权限"
}

case "$PHASE" in
    local-create)
        vexfs setup >/dev/null
        mount_workspace
        mkdir -p "$MOUNT_POINT/shared" "$MOUNT_POINT/bin"
        printf '%s\n' 'created-on-first-mac' >"$MOUNT_POINT/shared/from-local.txt"
        ln "$MOUNT_POINT/shared/from-local.txt" "$MOUNT_POINT/shared/from-local-hard"
        ln -s from-local.txt "$MOUNT_POINT/shared/from-local-link"
        printf '%s\n' '#!/bin/sh' 'printf "%s\\n" script-from-first-mac' \
            >"$MOUNT_POINT/bin/from-local.sh"
        chmod 0751 "$MOUNT_POINT/bin/from-local.sh"
        xattr -w com.vexfs.origin first-mac "$MOUNT_POINT/shared/from-local.txt"
        verify_local_baseline
        unmount_workspace
        vexfs snapshot create local-baseline >/dev/null
        contains "$(vexfs snapshot list)" "local-baseline" "首台机器快照"
        ;;
    remote-modify)
        mount_workspace
        verify_local_baseline
        printf '%s\n' 'created-on-second-mac' >"$MOUNT_POINT/shared/from-remote.tmp"
        mv "$MOUNT_POINT/shared/from-remote.tmp" "$MOUNT_POINT/shared/from-remote.txt"
        printf '%s\n' 'updated-on-second-mac' >>"$MOUNT_POINT/shared/from-local.txt"
        xattr -w com.vexfs.remote second-mac "$MOUNT_POINT/shared/from-local.txt"
        equals "$(tail -n 1 "$MOUNT_POINT/shared/from-local.txt")" \
            "updated-on-second-mac" "第二台机器追加"
        equals "$(xattr -p com.vexfs.remote "$MOUNT_POINT/shared/from-local.txt")" \
            "second-mac" "第二台机器扩展属性"
        unmount_workspace
        vexfs snapshot create remote-update >/dev/null
        contains "$(vexfs snapshot list)" "remote-update" "第二台机器快照"
        ;;
    remote-runtime-check)
        equals "$(vexfs cat /shared/from-local.txt)" \
            "created-on-first-mac" "第二台机器直接 PG runtime 读取"
        contains "$(vexfs check)" "OK workspace=$WORKSPACE" \
            "第二台机器直接 PG runtime deep check"
        ;;
    local-restore)
        mount_workspace
        assert_file "$MOUNT_POINT/shared/from-remote.txt" "远端文件回传"
        equals "$(cat "$MOUNT_POINT/shared/from-remote.txt")" \
            "created-on-second-mac" "远端文件内容"
        equals "$(tail -n 1 "$MOUNT_POINT/shared/from-local.txt")" \
            "updated-on-second-mac" "远端追加回传"
        equals "$(xattr -p com.vexfs.remote "$MOUNT_POINT/shared/from-local.txt")" \
            "second-mac" "远端 xattr 回传"
        unmount_workspace
        vexfs snapshot restore local-baseline --force-unmount >/dev/null
        mount_workspace
        verify_local_baseline
        assert_missing "$MOUNT_POINT/shared/from-remote.txt" "整树快照恢复"
        printf '%s\n' 'created-after-restore' >"$MOUNT_POINT/shared/after-restore.txt"
        assert_file "$MOUNT_POINT/shared/after-restore.txt" "恢复后新文件"
        unmount_workspace
        vexfs snapshot create local-restored >/dev/null
        contains "$(vexfs snapshot list)" "local-restored" "恢复后快照"
        ;;
    remote-final)
        mount_workspace
        verify_local_baseline
        assert_missing "$MOUNT_POINT/shared/from-remote.txt" "远端观察整树恢复"
        equals "$(cat "$MOUNT_POINT/shared/after-restore.txt")" \
            "created-after-restore" "远端观察恢复后写入"
        contains "$(find "$MOUNT_POINT/shared" -maxdepth 1 -print | sort)" \
            "after-restore.txt" "Bash find"
        contains "$(grep -R 'created-on-first-mac' "$MOUNT_POINT/shared")" \
            "created-on-first-mac" "Bash grep"
        unmount_workspace
        contains "$(vexfs snapshot list)" "local-baseline" "最终快照 local-baseline"
        contains "$(vexfs snapshot list)" "remote-update" "最终快照 remote-update"
        contains "$(vexfs snapshot list)" "local-restored" "最终快照 local-restored"
        contains "$(vexfs check)" "OK workspace=$WORKSPACE" "最终 deep check"
        ;;
    *)
        echo "未知节点阶段：$PHASE" >&2
        exit 2
        ;;
esac

echo "VEXFS PG REMOTE MACOS NODE: PASS (phase=$PHASE, $CHECKS checks)"
