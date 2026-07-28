#!/bin/bash
set -euo pipefail

# 真实 macOS NFS 挂载回归。默认只创建 1000 个小文件，避免压满本机内存。
# usage: bash run_macos_nfs_mount.sh [vexdb]

VEXDB="${1:-${VEXDB_BIN:-}}"
if [ -z "$VEXDB" ]; then
    REPO_ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
    VEXDB="$REPO_ROOT/vexdb_sqlite/build-nfs-dev/vexdb"
fi
[ -x "$VEXDB" ] || { echo "找不到 vexdb：$VEXDB" >&2; exit 1; }

FILE_COUNT="${VEXDB_NFS_FILE_COUNT:-1000}"
case "$FILE_COUNT" in
    ''|*[!0-9]*) echo "VEXDB_NFS_FILE_COUNT 必须是正整数" >&2; exit 2 ;;
esac
[ "$FILE_COUNT" -gt 0 ] && [ "$FILE_COUNT" -le 10000 ] || {
    echo "VEXDB_NFS_FILE_COUNT 必须在 1 到 10000 之间" >&2
    exit 2
}

ROOT="$(mktemp -d -t vexdb-nfs-real-eval)"
TEST_HOME="$ROOT/home"
DB="$ROOT/workspace.sqlite3"
MOUNT_POINT="$ROOT/mnt"
WORKSPACE=nfs-real
MOUNTED=0
CHECKS=0

mkdir -p "$TEST_HOME" "$MOUNT_POINT"

fs() {
    HOME="$TEST_HOME" "$VEXDB" fs --db "$DB" --workspace "$WORKSPACE" "$@"
}

check() {
    CHECKS=$((CHECKS + 1))
    "$@"
}

equal() {
    local expected="$1" actual="$2" message="$3"
    [ "$expected" = "$actual" ] || {
        echo "${message}：期望 '$expected'，实际 '$actual'" >&2
        exit 1
    }
    CHECKS=$((CHECKS + 1))
}

unmount_now() {
    fs unmount "$MOUNT_POINT"
    MOUNTED=0
}

cleanup() {
    if [ "$MOUNTED" = 1 ]; then
        fs unmount --force "$MOUNT_POINT" >/dev/null 2>&1 || true
    fi
    if [ "${VEXDB_KEEP_NFS_EVAL:-0}" = 1 ]; then
        echo "保留测试目录：$ROOT" >&2
    else
        rm -rf "$ROOT"
    fi
}
trap cleanup EXIT

fs setup >/dev/null
DOCTOR="$(fs --json doctor)"
printf '%s' "$DOCTOR" | /usr/bin/python3 -c \
    'import json,sys; v=json.load(sys.stdin); assert v["mount_driver"]=="NFSv3"; assert v["mount_ready"] is True; assert v["database"]["schema_ready"] is True'
CHECKS=$((CHECKS + 3))

fs mount "$MOUNT_POINT"
MOUNTED=1
STATUS="$(fs --json mount status "$MOUNT_POINT")"
printf '%s' "$STATUS" | /usr/bin/python3 -c \
    'import json,sys; v=json.load(sys.stdin); assert len(v)==1; assert v[0]["source"]=="127.0.0.1:/"; assert v[0]["type"]=="nfs"'
CHECKS=$((CHECKS + 3))

mkdir -p "$MOUNT_POINT/project/src"
printf 'base\n' > "$MOUNT_POINT/project/src/data.txt"
ln "$MOUNT_POINT/project/src/data.txt" "$MOUNT_POINT/project/src/data-hard.txt"
ln -s data.txt "$MOUNT_POINT/project/src/data-link.txt"
printf '%s\n' '-hard' >> "$MOUNT_POINT/project/src/data-hard.txt"
equal "$(stat -f %i "$MOUNT_POINT/project/src/data.txt")" \
    "$(stat -f %i "$MOUNT_POINT/project/src/data-hard.txt")" "hardlink inode"
equal 2 "$(stat -f %l "$MOUNT_POINT/project/src/data.txt")" "hardlink 数量"
equal data.txt "$(readlink "$MOUNT_POINT/project/src/data-link.txt")" "symlink target"
equal 'base
-hard' "$(cat "$MOUNT_POINT/project/src/data.txt")" "hardlink 共享内容"

printf '#!/bin/sh\nprintf "nfs-ok\\n"\n' > "$MOUNT_POINT/project/run.sh"
chmod 0755 "$MOUNT_POINT/project/run.sh"
equal nfs-ok "$($MOUNT_POINT/project/run.sh)" "可执行权限"

/usr/bin/xattr -w user.vexdb.nfs-eval yes "$MOUNT_POINT/project/src/data.txt"
equal yes "$(/usr/bin/xattr -p user.vexdb.nfs-eval "$MOUNT_POINT/project/src/data.txt")" \
    "扩展属性"

/usr/bin/python3 - "$MOUNT_POINT/project/process.lock" <<'PY'
import fcntl
import multiprocessing
import os
import sys

path = sys.argv[1]
open(path, "wb").write(b"lock\n")
multiprocessing.set_start_method("fork")


def acquire(handle, kind, nonblocking=False):
    flag = fcntl.LOCK_EX | (fcntl.LOCK_NB if nonblocking else 0)
    if kind == "flock":
        fcntl.flock(handle, flag)
    else:
        fcntl.lockf(handle, flag, 1, 0, os.SEEK_SET)


def hold(kind, ready, release):
    with open(path, "r+b", buffering=0) as handle:
        acquire(handle, kind)
        ready.set()
        release.wait(5)


for kind in ("flock", "fcntl"):
    ready = multiprocessing.Event()
    release = multiprocessing.Event()
    process = multiprocessing.Process(target=hold, args=(kind, ready, release))
    process.start()
    assert ready.wait(5), f"{kind} holder did not acquire lock"
    blocked = False
    with open(path, "r+b", buffering=0) as handle:
        try:
            acquire(handle, kind, nonblocking=True)
        except BlockingIOError:
            blocked = True
    release.set()
    process.join(5)
    assert process.exitcode == 0, f"{kind} holder failed: {process.exitcode}"
    assert blocked, f"{kind} allowed two exclusive holders"
PY
CHECKS=$((CHECKS + 2))

mkdir -p "$MOUNT_POINT/project/node"
printf '%s\n' \
    '{"name":"vexfs-nfs-eval","version":"1.0.0","private":true,"scripts":{"test":"node index.js"}}' \
    > "$MOUNT_POINT/project/node/package.json"
printf '%s\n' 'console.log("npm-ok")' > "$MOUNT_POINT/project/node/index.js"
HOME="$TEST_HOME" npm --prefix "$MOUNT_POINT/project/node" install \
    --package-lock-only --ignore-scripts --offline --no-audit --no-fund >/dev/null
equal npm-ok "$(HOME="$TEST_HOME" npm --prefix "$MOUNT_POINT/project/node" test --silent)" \
    "npm 项目"

cargo init --quiet --bin --vcs none "$MOUNT_POINT/project/rust"
printf '%s\n' 'fn main() { println!("cargo-ok"); }' \
    > "$MOUNT_POINT/project/rust/src/main.rs"
equal cargo-ok "$(cargo run --quiet --offline \
    --manifest-path "$MOUNT_POINT/project/rust/Cargo.toml")" "Cargo 项目"
printf '%s\n' '/rust/target/' > "$MOUNT_POINT/project/.gitignore"

printf 'old\n' > "$MOUNT_POINT/project/atomic.txt"
printf 'new\n' > "$MOUNT_POINT/project/atomic.next"
mv -f "$MOUNT_POINT/project/atomic.next" "$MOUNT_POINT/project/atomic.txt"
equal new "$(cat "$MOUNT_POINT/project/atomic.txt")" "原子替换"

/usr/bin/python3 -c \
    'import os,sys; p=sys.argv[1]; f=open(p,"ab",buffering=0); f.write(b"fsync\n"); os.fsync(f.fileno()); f.close()' \
    "$MOUNT_POINT/project/src/data.txt"
printf 'open-unlink\n' > "$MOUNT_POINT/project/open-unlink.txt"
/usr/bin/python3 -c \
    'import os,sys; p=sys.argv[1]; f=open(p,"rb"); os.unlink(p); assert f.read()==b"open-unlink\n"; f.close()' \
    "$MOUNT_POINT/project/open-unlink.txt"
CHECKS=$((CHECKS + 2))

git -C "$MOUNT_POINT/project" init -q
git -C "$MOUNT_POINT/project" config user.name VexFS-NFS-Eval
git -C "$MOUNT_POINT/project" config user.email vexfs-nfs@example.invalid
git -C "$MOUNT_POINT/project" add .
git -C "$MOUNT_POINT/project" commit -qm baseline
equal '' "$(git -C "$MOUNT_POINT/project" status --porcelain)" "Git 工作区"

PERF_JSON="$(/usr/bin/python3 - "$MOUNT_POINT/perf" "$FILE_COUNT" <<'PY'
import json
import pathlib
import sys
import time

root = pathlib.Path(sys.argv[1])
count = int(sys.argv[2])
root.mkdir()
started = time.perf_counter()
for index in range(count):
    (root / f"f-{index:05d}.txt").write_text("x\n", encoding="utf-8")
seconds = time.perf_counter() - started
print(json.dumps({"files": count, "seconds": round(seconds, 6),
                  "files_per_second": round(count / max(seconds, 1e-9), 3)}))
PY
)"
printf '%s' "$PERF_JSON" | /usr/bin/python3 -c \
    'import json,sys; v=json.load(sys.stdin); assert v["files"]>0; assert v["files_per_second"]>0'
CHECKS=$((CHECKS + 2))

equal '' "$(find "$MOUNT_POINT" -name '._*' -print -quit)" "挂载树不能出现 AppleDouble"
unmount_now

APPLEDOUBLE_DENTRIES="$($VEXDB "$DB" \
    "SELECT count(*) FROM _vexfs_dentry_states WHERE substr(name,1,2)='._';")"
equal 0 "$APPLEDOUBLE_DENTRIES" "数据库不能保存 AppleDouble dentry"
CHECK_JSON="$(fs --json check)"
printf '%s' "$CHECK_JSON" | /usr/bin/python3 -c \
    'import json,sys; v=json.load(sys.stdin); assert v["ok"] is True; assert v["issue_count"]==0'
CHECKS=$((CHECKS + 2))
fs snapshot create baseline >/dev/null
check fs history /project/src/data.txt --limit 5

fs mount "$MOUNT_POINT"
MOUNTED=1
equal "$(stat -f %i "$MOUNT_POINT/project/src/data.txt")" \
    "$(stat -f %i "$MOUNT_POINT/project/src/data-hard.txt")" "重挂载 hardlink inode"
equal 2 "$(stat -f %l "$MOUNT_POINT/project/src/data.txt")" "重挂载 hardlink 数量"
equal yes "$(/usr/bin/xattr -p user.vexdb.nfs-eval "$MOUNT_POINT/project/src/data.txt")" \
    "重挂载扩展属性"
equal '' "$(git -C "$MOUNT_POINT/project" status --porcelain)" "重挂载 Git 工作区"
printf 'changed\n' > "$MOUNT_POINT/project/src/data.txt"
printf 'temporary\n' > "$MOUNT_POINT/project/temporary.txt"
unmount_now

fs snapshot restore baseline >/dev/null
fs mount "$MOUNT_POINT"
MOUNTED=1
equal "$(stat -f %i "$MOUNT_POINT/project/src/data.txt")" \
    "$(stat -f %i "$MOUNT_POINT/project/src/data-hard.txt")" "恢复后 hardlink inode"
equal 2 "$(stat -f %l "$MOUNT_POINT/project/src/data.txt")" "恢复后 hardlink 数量"
check test ! -e "$MOUNT_POINT/project/temporary.txt"
equal '' "$(git -C "$MOUNT_POINT/project" status --porcelain)" "快照恢复后 Git 工作区"
unmount_now

# 模拟 gateway 异常退出。挂载记录必须仍能识别这块盘，强制卸载要能清理
# 进程状态，随后同一个 workspace 可以重新挂载并继续读取。
fs mount "$MOUNT_POINT"
MOUNTED=1
GATEWAY_RECORD="$(find "$TEST_HOME/Library/Application Support/VexDB-Lite/nfs-gateways" \
    -name gateway.record -type f -print -quit)"
check test -f "$GATEWAY_RECORD"
GATEWAY_PID="$(sed -n '2p' "$GATEWAY_RECORD")"
case "$GATEWAY_PID" in
    ''|*[!0-9]*) echo "无效的 NFS gateway PID：$GATEWAY_PID" >&2; exit 1 ;;
esac
check kill -9 "$GATEWAY_PID"
for _ in 1 2 3 4 5 6 7 8 9 10; do
    if ! kill -0 "$GATEWAY_PID" 2>/dev/null; then break; fi
    sleep 0.1
done
if kill -0 "$GATEWAY_PID" 2>/dev/null; then
    echo "NFS gateway 被杀后仍然存活：$GATEWAY_PID" >&2
    exit 1
fi
CHECKS=$((CHECKS + 1))
CRASH_STATUS="$(fs --json mount status "$MOUNT_POINT")"
printf '%s' "$CRASH_STATUS" | /usr/bin/python3 -c \
    'import json,sys; v=json.load(sys.stdin); assert len(v)==1; assert v[0]["type"]=="nfs"'
CHECKS=$((CHECKS + 1))
fs unmount --force "$MOUNT_POINT"
MOUNTED=0
equal '' "$(find "$TEST_HOME/Library/Application Support/VexDB-Lite/nfs-gateways" \
    -name gateway.record -type f -print -quit)" "gateway 崩溃后的状态清理"
fs mount "$MOUNT_POINT"
MOUNTED=1
equal 'base
-hard
fsync' "$(cat "$MOUNT_POINT/project/src/data.txt")" "gateway 崩溃后的重新挂载"
unmount_now

equal '[]' "$(fs --json mount status "$MOUNT_POINT")" "卸载后挂载状态"
FINAL_DOCTOR="$(fs --json doctor)"
printf '%s' "$FINAL_DOCTOR" | /usr/bin/python3 -c \
    'import json,sys; v=json.load(sys.stdin); assert v["mount_count"]==0; assert v["database"]["pending_handles"]==0; assert v["database"]["staging_bytes"]==0'
CHECKS=$((CHECKS + 3))

echo "VEXDB MACOS NFS REAL MOUNT: PASS ($CHECKS checks)"
echo "small_file_metrics=$PERF_JSON"
