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
VEXDB="$(cd "$(dirname "$VEXDB")" && pwd)/$(basename "$VEXDB")"

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
XATTR_SUPPORTED=0

mkdir -p "$TEST_HOME" "$MOUNT_POINT"

fs() {
    HOME="$TEST_HOME" "$VEXDB" fs --db "$DB" --workspace "$WORKSPACE" "$@"
}

run_in_project() {
    (
        cd "$MOUNT_POINT/project"
        HOME="$TEST_HOME" "$VEXDB" fs --db "$DB" --workspace "$WORKSPACE" "$@"
    )
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

if /usr/bin/xattr -w user.vexdb.nfs-eval yes \
        "$MOUNT_POINT/project/src/data.txt" 2>/dev/null; then
    XATTR_SUPPORTED=1
    equal yes "$(/usr/bin/xattr -p user.vexdb.nfs-eval "$MOUNT_POINT/project/src/data.txt")" \
        "扩展属性"
fi

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

# Agent 命令包装器必须先创建一致快照，原样传递参数和环境，并能用运行前
# 快照恢复。生命周期事件写 stderr，不能污染子进程 stdout。
RUN_SUCCESS_OUT="$ROOT/run-success.out"
RUN_SUCCESS_ERR="$ROOT/run-success.err"
set +e
run_in_project --json run --snapshot-before --snapshot-after-success -- \
    /bin/sh -c \
    '[ -n "$VEXFS_RUN_ID" ] && [ -n "$VEXFS_SNAPSHOT_BEFORE" ] && [ "$1" = "--json" ] && printf "agent-success\n" > agent-run.txt && printf "child-stdout\n"' \
    vexfs-child --json >"$RUN_SUCCESS_OUT" 2>"$RUN_SUCCESS_ERR"
RUN_SUCCESS_STATUS=$?
set -e
equal 0 "$RUN_SUCCESS_STATUS" "Agent 成功命令退出码"
equal child-stdout "$(cat "$RUN_SUCCESS_OUT")" "Agent 子进程 stdout"
equal agent-success "$(cat "$MOUNT_POINT/project/agent-run.txt")" "Agent 子进程写入"
RUN_SUCCESS_META="$(/usr/bin/python3 - "$RUN_SUCCESS_ERR" <<'PY'
import json
import sys

events = []
for line in open(sys.argv[1], encoding="utf-8"):
    if line.startswith("{"):
        events.append(json.loads(line))
before = next(event for event in events if event["event"] == "snapshot_before")
after = next(event for event in events if event["event"] == "snapshot_after")
finished = next(event for event in events if event["event"] == "run_exit")
assert before["run_id"] == after["run_id"] == finished["run_id"]
assert before["command"] == "sh"
assert before["snapshot"].endswith("-before")
assert after["snapshot"].endswith("-after")
assert finished["exit_code"] == 0
assert "agent-run.txt" not in before["restore_command"]
print(before["snapshot"], after["snapshot"])
PY
)"
RUN_SUCCESS_BEFORE="${RUN_SUCCESS_META%% *}"
RUN_SUCCESS_AFTER="${RUN_SUCCESS_META#* }"
SNAPSHOT_LIST="$(fs --json snapshot list)"
printf '%s' "$SNAPSHOT_LIST" | /usr/bin/python3 -c \
    'import json,sys; rows={row["name"]:row for row in json.load(sys.stdin)}; assert all(rows[name]["type"]=="agent" for name in sys.argv[1:])' \
    "$RUN_SUCCESS_BEFORE" "$RUN_SUCCESS_AFTER"
CHECKS=$((CHECKS + 9))

fs snapshot restore "$RUN_SUCCESS_BEFORE" >/dev/null
check test ! -e "$MOUNT_POINT/project/agent-run.txt"
equal '' "$(git -C "$MOUNT_POINT/project" status --porcelain)" "Agent 成功任务恢复"

# 非零退出码必须原样返回，且不能创建 after-success 快照；运行前快照仍可恢复。
RUN_FAILURE_ERR="$ROOT/run-failure.err"
set +e
run_in_project --json run --snapshot-before --snapshot-after-success -- \
    /bin/sh -c 'printf "failed-change\n" > failed-run.txt; exit 23' \
    >"$ROOT/run-failure.out" 2>"$RUN_FAILURE_ERR"
RUN_FAILURE_STATUS=$?
set -e
equal 23 "$RUN_FAILURE_STATUS" "Agent 失败命令退出码"
equal failed-change "$(cat "$MOUNT_POINT/project/failed-run.txt")" "Agent 失败命令写入"
RUN_FAILURE_BEFORE="$(/usr/bin/python3 - "$RUN_FAILURE_ERR" <<'PY'
import json
import sys

events = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")
          if line.startswith("{")]
before = next(event for event in events if event["event"] == "snapshot_before")
assert not any(event["event"] == "snapshot_after" for event in events)
assert next(event for event in events if event["event"] == "run_exit")["exit_code"] == 23
print(before["snapshot"])
PY
)"
fs snapshot restore "$RUN_FAILURE_BEFORE" >/dev/null
check test ! -e "$MOUNT_POINT/project/failed-run.txt"
CHECKS=$((CHECKS + 3))

# Ctrl-C 由包装器转发给子进程，返回 shell 通用的 130；before 快照先于
# 子进程启动，因此中断不会丢掉恢复点。
RUN_SIGNAL_ERR="$ROOT/run-signal.err"
(
    cd "$MOUNT_POINT/project"
    exec env HOME="$TEST_HOME" "$VEXDB" fs --db "$DB" --workspace "$WORKSPACE" \
        --json run --snapshot-before -- /bin/sleep 30
) >"$ROOT/run-signal.out" 2>"$RUN_SIGNAL_ERR" &
RUN_SIGNAL_PID=$!
for _ in $(seq 1 200); do
    if grep -q '"event":"snapshot_before"' "$RUN_SIGNAL_ERR" 2>/dev/null; then break; fi
    sleep 0.025
done
check grep -q '"event":"snapshot_before"' "$RUN_SIGNAL_ERR"
kill -INT "$RUN_SIGNAL_PID"
set +e
wait "$RUN_SIGNAL_PID"
RUN_SIGNAL_STATUS=$?
set -e
equal 130 "$RUN_SIGNAL_STATUS" "Agent Ctrl-C 退出码"
/usr/bin/python3 - "$RUN_SIGNAL_ERR" <<'PY'
import json
import sys

events = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")
          if line.startswith("{")]
assert next(event for event in events if event["event"] == "run_exit")["exit_code"] == 130
assert any(event["event"] == "snapshot_before" for event in events)
PY
CHECKS=$((CHECKS + 2))

AGENT_START_MS="$(/usr/bin/python3 -c 'import time; print(int(time.time()*1000))')"
run_in_project --json run --snapshot-before -- /usr/bin/true \
    >"$ROOT/run-startup.out" 2>"$ROOT/run-startup.err"
AGENT_END_MS="$(/usr/bin/python3 -c 'import time; print(int(time.time()*1000))')"
AGENT_STARTUP_MS=$((AGENT_END_MS - AGENT_START_MS))
[ "$AGENT_STARTUP_MS" -le 5000 ] || {
    echo "Agent 运行前 checkpoint 超过 5 秒：${AGENT_STARTUP_MS}ms" >&2
    exit 1
}
CHECKS=$((CHECKS + 1))

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
if [ "$XATTR_SUPPORTED" = 1 ]; then
    equal yes "$(/usr/bin/xattr -p user.vexdb.nfs-eval "$MOUNT_POINT/project/src/data.txt")" \
        "重挂载扩展属性"
fi
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
echo "agent_snapshot_before_ms=$AGENT_STARTUP_MS"
if [ "$XATTR_SUPPORTED" = 0 ]; then
    echo 'limitations=["macos-nfsv3-mounted-xattr"]'
fi
