#!/usr/bin/env bash
set -euo pipefail

PHASE="${VEXFS_NODE_PHASE:?VEXFS_NODE_PHASE is required}"
CLI="${VEXFS_NODE_CLI:?VEXFS_NODE_CLI is required}"
DSN="${VEXFS_NODE_DSN:?VEXFS_NODE_DSN is required}"
WORKSPACE="${VEXFS_NODE_WORKSPACE:-pg-two-mac-opencode}"
OPENCODE="${VEXFS_NODE_OPENCODE:?VEXFS_NODE_OPENCODE is required}"
MODEL="${VEXFS_EVAL_OPENCODE_MODEL:-openai/gpt-5.4-mini}"
PYTHON="${VEXFS_NODE_PYTHON:-/usr/bin/python3}"
GIT="${VEXFS_NODE_GIT:-/usr/bin/git}"
MOUNT_POINT="$(mktemp -d "${TMPDIR:-/tmp}/vexfs-pg-opencode-mount.XXXXXX")"
PROJECT="$MOUNT_POINT/agent-project"
OPENCODE_LOG="$(mktemp "${TMPDIR:-/tmp}/vexfs-pg-opencode-log.XXXXXX")"
MOUNTED=false
CHECKS=0

[ "$(uname -s)" = Darwin ] || { echo "该节点测试只支持 macOS" >&2; exit 2; }
[ -x "$CLI" ] || { echo "找不到 vexdb CLI：$CLI" >&2; exit 2; }
[ -x "$OPENCODE" ] || { echo "找不到 OpenCode：$OPENCODE" >&2; exit 2; }
[ -x "$PYTHON" ] || { echo "找不到 Python：$PYTHON" >&2; exit 2; }
[ -x "$GIT" ] || { echo "找不到 Git：$GIT" >&2; exit 2; }

vexfs() {
    "$CLI" fs --backend pg --dsn "$DSN" --workspace "$WORKSPACE" "$@"
}

pass() {
    CHECKS=$((CHECKS + 1))
}

fail() {
    echo "$1" >&2
    exit 1
}

equals() {
    local actual="$1" expected="$2" description="$3"
    [ "$actual" = "$expected" ] || \
        fail "$description：期望 [$expected]，实际 [$actual]"
    pass
}

contains() {
    local actual="$1" expected="$2" description="$3"
    case "$actual" in
        *"$expected"*) pass ;;
        *) fail "$description：缺少 [$expected]，实际 [$actual]" ;;
    esac
}

mount_workspace() {
    vexfs mount "$MOUNT_POINT" >/dev/null
    MOUNTED=true
    contains "$(vexfs --json mount status "$MOUNT_POINT")" \
        "\"workspace\":\"$WORKSPACE\"" "FSKit 挂载状态"
}

unmount_workspace() {
    if [ "$MOUNTED" = true ]; then
        cd /
        vexfs unmount "$MOUNT_POINT" >/dev/null
        MOUNTED=false
    fi
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    cd /
    if [ "$MOUNTED" = true ]; then
        vexfs unmount --force "$MOUNT_POINT" >/dev/null 2>&1 || true
    fi
    rm -f "$OPENCODE_LOG"
    rmdir "$MOUNT_POINT" >/dev/null 2>&1 || true
    exit "$status"
}
trap cleanup EXIT INT TERM

approval_argument() {
    local help
    help="$("$OPENCODE" run --help 2>&1)"
    if [[ "$help" == *"--dangerously-skip-permissions"* ]]; then
        printf '%s\n' --dangerously-skip-permissions
    elif [[ "$help" == *"--auto"* ]]; then
        printf '%s\n' --auto
    else
        fail "当前 OpenCode 没有无人值守授权参数"
    fi
}

run_opencode() {
    local prompt="$1" approval started ended
    approval="$(approval_argument)"
    started="$(date +%s)"
    /usr/bin/perl -e 'alarm 900; exec @ARGV' \
        "$OPENCODE" run "$approval" --pure --format json --model "$MODEL" \
        --dir "$PROJECT" "$prompt" >"$OPENCODE_LOG" 2>&1 || {
            tail -80 "$OPENCODE_LOG" >&2
            fail "OpenCode 执行失败"
        }
    ended="$(date +%s)"
    [ -s "$OPENCODE_LOG" ] || fail "OpenCode 没有返回执行事件"
    pass
    printf 'OPENCODE_METRIC phase=%s version=%s model=%s seconds=%s\n' \
        "$PHASE" "$("$OPENCODE" --version 2>/dev/null | tail -1)" "$MODEL" \
        "$((ended - started))"
}

assert_mac1_state() {
    "$PYTHON" -m unittest -v test_calc.py >/dev/null
    pass
    ! grep -q NotImplementedError calc.py || fail "multiply 仍未实现"
    pass
}

case "$PHASE" in
    mac1-create)
        vexfs setup >/dev/null
        mount_workspace
        mkdir -p "$PROJECT"
        cd "$PROJECT"
        cat >calc.py <<'PY'
def add(left, right):
    return left + right


def multiply(left, right):
    """Return left multiplied by right."""
    raise NotImplementedError("OpenCode must implement multiply")
PY
        cat >test_calc.py <<'PY'
import unittest

from calc import add, multiply


class CalcTest(unittest.TestCase):
    def test_add(self):
        self.assertEqual(add(2, 5), 7)

    def test_multiply(self):
        self.assertEqual(multiply(6, 7), 42)
        self.assertEqual(multiply(-3, 4), -12)
        self.assertEqual(multiply(10, 0), 0)


if __name__ == "__main__":
    unittest.main()
PY
        cat >TASK_MAC1.md <<'MD'
Implement multiply in calc.py. Keep add unchanged and make all unit tests pass.
MD
        "$GIT" init -b main >/dev/null
        "$GIT" config user.name "VexFS Two-Mac Eval"
        "$GIT" config user.email "eval@example.invalid"
        "$GIT" add .
        "$GIT" commit -m "shared workspace fixture" >/dev/null
        run_opencode \
            "Work only in this repository. Read TASK_MAC1.md, edit only calc.py, and run /usr/bin/python3 -m unittest -v. Do not ask questions and do not commit."
        assert_mac1_state
        equals "$("$GIT" diff --name-only)" "calc.py" "第一台 OpenCode 修改范围"
        "$GIT" add calc.py
        "$GIT" commit -m "mac1 opencode" >/dev/null
        contains "$("$GIT" log -1 --format=%s)" "mac1 opencode" "第一台 Mac Git 提交"
        unmount_workspace
        vexfs snapshot create mac1-opencode >/dev/null
        contains "$(vexfs snapshot list)" "mac1-opencode" "第一台工作区快照"
        ;;

    mac2-modify)
        mount_workspace
        cd "$PROJECT"
        assert_mac1_state
        contains "$("$GIT" log --format=%s --all)" "mac1 opencode" "继承第一台 Mac Git 提交"
        cat >test_power.py <<'PY'
import unittest

from calc import power


class PowerTest(unittest.TestCase):
    def test_power(self):
        self.assertEqual(power(2, 10), 1024)
        self.assertEqual(power(5, 0), 1)
        self.assertEqual(power(-3, 3), -27)


if __name__ == "__main__":
    unittest.main()
PY
        cat >TASK_MAC2.md <<'MD'
Add power(base, exponent) to calc.py for non-negative integer exponents. Preserve existing behavior.
MD
        "$GIT" add test_power.py TASK_MAC2.md
        "$GIT" commit -m "mac2 task fixture" >/dev/null
        run_opencode \
            "This repository was created by another Mac. Read TASK_MAC2.md, edit only calc.py, and run /usr/bin/python3 -m unittest -v. Do not ask questions and do not commit."
        "$PYTHON" -m unittest -v >/dev/null
        pass
        equals "$("$GIT" diff --name-only)" "calc.py" "第二台 OpenCode 修改范围"
        contains "$(cat calc.py)" "def power" "第二台 OpenCode 实现"
        "$GIT" add calc.py
        "$GIT" commit -m "mac2 opencode" >/dev/null
        unmount_workspace
        vexfs snapshot create mac2-opencode >/dev/null
        contains "$(vexfs snapshot list)" "mac2-opencode" "第二台工作区快照"
        ;;

    mac1-verify-restore)
        mount_workspace
        cd "$PROJECT"
        "$PYTHON" -m unittest -v >/dev/null
        pass
        contains "$("$GIT" log --format=%s --all)" "mac1 opencode" "继承第一台提交"
        contains "$("$GIT" log --format=%s --all)" "mac2 opencode" "继承第二台提交"
        contains "$(cat calc.py)" "def multiply" "继承 multiply"
        contains "$(cat calc.py)" "def power" "继承 power"
        native_grep="$(grep -n 'def power' calc.py)"
        contains "$native_grep" "def power" "挂载目录原生 grep"
        unmount_workspace

        snapshots="$(vexfs snapshot list)"
        contains "$snapshots" "mac1-opencode" "版本列表包含第一台快照"
        contains "$snapshots" "mac2-opencode" "版本列表包含第二台快照"
        diff_status=0
        diff_output="$(vexfs snapshot diff mac1-opencode --to mac2-opencode)" || diff_status=$?
        equals "$diff_status" "1" "两个 workspace 版本存在差异"
        contains "$diff_output" "calc.py" "两个 workspace 版本的差异"
        history_json="$(vexfs --json history /agent-project/calc.py --limit 100)"
        history_count="$(printf '%s' "$history_json" | "$PYTHON" -c 'import json,sys; print(len(json.load(sys.stdin)))')"
        [ "$history_count" -ge 1 ] || fail "calc.py 没有可查看的历史版本"
        pass
        db_grep="$(vexfs --json grep power /agent-project)"
        contains "$db_grep" '"match_count":' "数据库 grep"
        contains "$db_grep" '"index_used":false' "PG grep 如实报告未用索引"
        index_status="$(vexfs index status)"
        contains "$index_status" '"available":false' "PG 索引能力边界"

        vexfs snapshot restore mac1-opencode >/dev/null
        mount_workspace
        cd "$PROJECT"
        "$PYTHON" -m unittest -v test_calc.py >/dev/null
        pass
        [ ! -e test_power.py ] || fail "一键恢复后仍存在第二台测试文件"
        pass
        ! grep -q 'def power' calc.py || fail "一键恢复后仍存在第二台实现"
        pass
        equals "$("$GIT" log -1 --format=%s)" "mac1 opencode" "一键恢复后的 Git workspace"
        unmount_workspace
        contains "$(vexfs check)" "OK workspace=$WORKSPACE" "恢复后 deep check"
        printf 'VERSION_METRIC workspace=%s snapshots=2 calc_history=%s grep_index_available=false\n' \
            "$WORKSPACE" "$history_count"
        ;;

    *)
        fail "未知节点阶段：$PHASE"
        ;;
esac

echo "VEXFS PG TWO-MAC OPENCODE NODE: PASS (phase=$PHASE, $CHECKS checks)"
