#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
CONTAINER="${VEXDB_PG_CONTAINER:-vexdb_pg19-test}"
DATABASE="${VEXDB_PG_DATABASE:-test}"
BATCH_SIZE="${VEXFS_PG_CREATE_BATCH_SIZE:-1000}"
SCALES="${VEXFS_PG_CREATE_BATCH_SCALES:-1000 10000 100000}"
MEMORY_LIMIT_BYTES="${VEXFS_PG_CREATE_BATCH_MEMORY_LIMIT_BYTES:-1073741824}"
MIN_SPEEDUP="${VEXFS_PG_CREATE_BATCH_MIN_SPEEDUP:-2}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/vexfs-pg-create-batch.XXXXXX")"
REPORT_DIR="$ROOT/build/eval/vexfs"
REPORT="$REPORT_DIR/pg_create_batch_performance.tsv"
ACTIVE_WORKSPACES=()

cleanup() {
    local status=$?
    trap - EXIT
    local workspace
    for workspace in "${ACTIVE_WORKSPACES[@]:-}"; do
        [[ -n "$workspace" ]] || continue
        docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" -X -q \
            -c "SELECT public.vexfs_workspace_drop('$workspace', true);" \
            >/dev/null 2>&1 || true
    done
    rm -rf "$TMP"
    exit "$status"
}
trap cleanup EXIT

fail() {
    echo "VEXFS PG CREATE BATCH PERFORMANCE: FAIL: $*" >&2
    exit 1
}

find_python() {
    local candidate
    for candidate in \
        "${VEXDB_LITE_PYTHON:-}" \
        "$(command -v python3 2>/dev/null || true)" \
        /opt/homebrew/bin/python3 \
        /opt/anaconda3/bin/python3 \
        /usr/bin/python3; do
        [[ -n "$candidate" && -x "$candidate" ]] || continue
        if "$candidate" -c 'import sys; raise SystemExit(sys.version_info < (3, 8))' \
                >/dev/null 2>&1; then
            echo "$candidate"
            return 0
        fi
    done
    return 1
}

PYTHON_BIN="$(find_python)" || fail "需要 Python 3.8 或更高版本"

now_ms() {
    "$PYTHON_BIN" -c 'import time; print(time.monotonic_ns() // 1_000_000)'
}

memory_bytes() {
    docker exec "$CONTAINER" sh -c \
        'if [ -r /sys/fs/cgroup/memory.current ]; then cat /sys/fs/cgroup/memory.current; elif [ -r /sys/fs/cgroup/memory/memory.usage_in_bytes ]; then cat /sys/fs/cgroup/memory/memory.usage_in_bytes; else echo 0; fi'
}

run_guarded_sql() {
    local sql_file=$1
    local label=$2
    local error_file="$TMP/$label.stderr"
    local peak_file="$TMP/$label.peak"
    local exceeded_file="$TMP/$label.memory-exceeded"
    local baseline
    baseline="$(memory_bytes)"
    [[ "$baseline" =~ ^[0-9]+$ ]] || fail "无法读取容器内存"
    (( baseline <= MEMORY_LIMIT_BYTES )) || \
        fail "容器运行前内存 ${baseline} 已超过上限 ${MEMORY_LIMIT_BYTES}"
    echo "$baseline" > "$peak_file"

    docker exec -i "$CONTAINER" psql -U postgres -d "$DATABASE" -X -q \
        -v ON_ERROR_STOP=1 < "$sql_file" >/dev/null 2>"$error_file" &
    local load_pid=$!
    (
        local peak=$baseline
        local current
        while kill -0 "$load_pid" >/dev/null 2>&1; do
            current="$(memory_bytes 2>/dev/null || echo 0)"
            if [[ "$current" =~ ^[0-9]+$ ]] && (( current > peak )); then
                peak=$current
                echo "$peak" > "$peak_file"
            fi
            if [[ "$current" =~ ^[0-9]+$ ]] && (( current > MEMORY_LIMIT_BYTES )); then
                : > "$exceeded_file"
                kill -TERM "$load_pid" >/dev/null 2>&1 || true
                break
            fi
            sleep 0.2
        done
    ) &
    local monitor_pid=$!
    local load_status=0
    wait "$load_pid" || load_status=$?
    wait "$monitor_pid" || true
    [[ ! -e "$exceeded_file" ]] || \
        fail "$label 超过容器内存上限 ${MEMORY_LIMIT_BYTES} bytes"
    if (( load_status != 0 )); then
        sed -n '1,120p' "$error_file" >&2
        fail "$label SQL 执行失败，退出码 $load_status"
    fi
}

[[ "$BATCH_SIZE" =~ ^[0-9]+$ ]] || fail "batch size 必须是整数"
(( BATCH_SIZE >= 1 && BATCH_SIZE <= 1000 )) || fail "batch size 必须在 1..1000"
[[ "$MEMORY_LIMIT_BYTES" =~ ^[0-9]+$ ]] || fail "内存上限必须是整数"
docker inspect "$CONTAINER" >/dev/null
docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" -X -q -t -A \
    -v ON_ERROR_STOP=1 -c \
    "SELECT to_regprocedure('public.vexfs_create_batch(text,text,jsonb)') IS NOT NULL;" \
    | grep -qx t || fail "测试数据库没有 vexfs_create_batch"

mkdir -p "$REPORT_DIR"
printf 'mode\tfiles\tbatch_size\tbatches\telapsed_ms\tcleanup_ms\tfiles_per_second\tcommits\tchanges\tversions\tmanifests\tpeak_memory_bytes\tcheck_ok\n' > "$REPORT"

# A small old-path control proves that batching removes real per-file commit
# overhead without repeating the old design at 10k/100k scale.
LEGACY_WORKSPACE="eval-batch-legacy-$$"
ACTIVE_WORKSPACES+=("$LEGACY_WORKSPACE")
docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" -X -q \
    -v ON_ERROR_STOP=1 -c "SELECT public.vexfs_workspace_create('$LEGACY_WORKSPACE');" \
    >/dev/null
LEGACY_SQL="$TMP/legacy.sql"
{
    echo "SET work_mem='16MB'; SET temp_file_limit='512MB'; SET statement_timeout='5min';"
    for ((index=1; index<=1000; index++)); do
        printf "SELECT public.vexfs_create('%s','/legacy-%06d.txt','file',420);\n" \
            "$LEGACY_WORKSPACE" "$index"
    done
} > "$LEGACY_SQL"
LEGACY_STARTED="$(now_ms)"
run_guarded_sql "$LEGACY_SQL" legacy-1000
LEGACY_ELAPSED=$(( $(now_ms) - LEGACY_STARTED ))
LEGACY_PEAK="$(cat "$TMP/legacy-1000.peak")"
LEGACY_STATE="$(docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" \
    -X -q -t -A -F '|' -v ON_ERROR_STOP=1 -c \
    "SELECT live_files,head_commit,
            (SELECT count(*) FROM _vexfs.commit_changes c WHERE c.workspace_id=w.workspace_id),
            (public.vexfs_check('$LEGACY_WORKSPACE',0)->>'ok')::boolean
       FROM _vexfs.workspaces w WHERE name='$LEGACY_WORKSPACE';")"
IFS='|' read -r LEGACY_FILES LEGACY_COMMITS LEGACY_CHANGES LEGACY_OK <<<"$LEGACY_STATE"
[[ "$LEGACY_FILES|$LEGACY_COMMITS|$LEGACY_CHANGES|$LEGACY_OK" == "1000|1001|1001|t" ]] || \
    fail "旧路径控制组状态不正确：$LEGACY_STATE"
LEGACY_RATE="$("$PYTHON_BIN" -c \
    "print(round(1000 * 1000 / max(1, int('$LEGACY_ELAPSED')), 2))")"
LEGACY_DROP_SQL="$TMP/legacy-drop.sql"
printf "SET statement_timeout='2min'; SELECT public.vexfs_workspace_drop('%s', true);\n" \
    "$LEGACY_WORKSPACE" > "$LEGACY_DROP_SQL"
LEGACY_DROP_STARTED="$(now_ms)"
run_guarded_sql "$LEGACY_DROP_SQL" legacy-drop
LEGACY_CLEANUP=$(( $(now_ms) - LEGACY_DROP_STARTED ))
LEGACY_DROP_PEAK="$(cat "$TMP/legacy-drop.peak")"
(( LEGACY_DROP_PEAK > LEGACY_PEAK )) && LEGACY_PEAK="$LEGACY_DROP_PEAK"
(( LEGACY_CLEANUP <= 30000 )) || fail "旧路径 1000 文件清理超过 30 秒"
printf 'single\t1000\t1\t1000\t%s\t%s\t%s\t%s\t%s\t1000\t1\t%s\t%s\n' \
    "$LEGACY_ELAPSED" "$LEGACY_CLEANUP" "$LEGACY_RATE" "$LEGACY_COMMITS" \
    "$LEGACY_CHANGES" "$LEGACY_PEAK" "$LEGACY_OK" >> "$REPORT"
ACTIVE_WORKSPACES=()

FIRST_BATCH_ELAPSED=""
RAN_1000=0
for scale in $SCALES; do
    [[ "$scale" =~ ^[0-9]+$ ]] || fail "scale 必须是整数：$scale"
    (( scale >= 1 && scale <= 100000 )) || fail "scale 必须在 1..100000：$scale"
    workspace="eval-batch-${scale}-$$"
    ACTIVE_WORKSPACES+=("$workspace")
    docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" -X -q \
        -v ON_ERROR_STOP=1 -c "SELECT public.vexfs_workspace_create('$workspace');" \
        >/dev/null

    sql_file="$TMP/batch-$scale.sql"
    {
        echo "SET work_mem='16MB'; SET temp_file_limit='512MB'; SET statement_timeout='5min';"
        for ((start=1; start<=scale; start+=BATCH_SIZE)); do
            end=$((start + BATCH_SIZE - 1))
            (( end > scale )) && end=$scale
            printf "SELECT public.vexfs_create_batch('%s','/',(SELECT jsonb_agg(jsonb_build_object('name','file-'||lpad(n::text,10,'0')||'.txt') ORDER BY n) FROM generate_series(%d,%d) AS n));\n" \
                "$workspace" "$start" "$end"
        done
    } > "$sql_file"
    batches=$(( (scale + BATCH_SIZE - 1) / BATCH_SIZE ))
    started="$(now_ms)"
    run_guarded_sql "$sql_file" "batch-$scale"
    elapsed=$(( $(now_ms) - started ))
    peak="$(cat "$TMP/batch-$scale.peak")"
    if (( scale == 1000 )); then
        FIRST_BATCH_ELAPSED="$elapsed"
        RAN_1000=1
    fi

    deep=0
    (( scale <= 1000 )) && deep=1
    state="$(docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" \
        -X -q -t -A -F '|' -v ON_ERROR_STOP=1 -c \
        "SELECT w.live_files,w.head_commit,
                (SELECT count(*) FROM _vexfs.commit_changes c WHERE c.workspace_id=w.workspace_id),
                (SELECT count(*) FROM _vexfs.file_versions v WHERE v.workspace_id=w.workspace_id),
                (SELECT count(*) FROM _vexfs.manifests m WHERE m.workspace_id=w.workspace_id),
                (SELECT count(*) FROM _vexfs.audit_events a WHERE a.workspace_id=w.workspace_id AND a.operation='create_batch'),
                (public.vexfs_check('$workspace',$deep)->>'ok')::boolean
           FROM _vexfs.workspaces w WHERE name='$workspace';")"
    IFS='|' read -r files commits changes versions manifests audits check_ok <<<"$state"
    expected_commits=$((batches + 1))
    expected_changes=$((scale + 1))
    [[ "$files" == "$scale" && "$commits" == "$expected_commits" && \
       "$changes" == "$expected_changes" && "$versions" == "$scale" && \
       "$manifests" == "1" && "$audits" == "$batches" && "$check_ok" == "t" ]] || \
        fail "$scale 文件状态不正确：$state"

    case "$scale" in
        1000) budget_ms=15000 ;;
        10000) budget_ms=90000 ;;
        100000) budget_ms=600000 ;;
        *) budget_ms=600000 ;;
    esac
    (( elapsed <= budget_ms )) || fail "$scale 文件耗时 ${elapsed}ms 超过预算 ${budget_ms}ms"
    rate="$("$PYTHON_BIN" -c \
        "print(round(int('$scale') * 1000 / max(1, int('$elapsed')), 2))")"
    drop_sql="$TMP/drop-$scale.sql"
    printf "SET statement_timeout='2min'; SELECT public.vexfs_workspace_drop('%s', true);\n" \
        "$workspace" > "$drop_sql"
    drop_started="$(now_ms)"
    run_guarded_sql "$drop_sql" "drop-$scale"
    cleanup_elapsed=$(( $(now_ms) - drop_started ))
    drop_peak="$(cat "$TMP/drop-$scale.peak")"
    (( drop_peak > peak )) && peak="$drop_peak"
    case "$scale" in
        1000) cleanup_budget_ms=15000 ;;
        10000) cleanup_budget_ms=30000 ;;
        100000) cleanup_budget_ms=120000 ;;
        *) cleanup_budget_ms=120000 ;;
    esac
    (( cleanup_elapsed <= cleanup_budget_ms )) || \
        fail "$scale 文件清理耗时 ${cleanup_elapsed}ms 超过预算 ${cleanup_budget_ms}ms"
    printf 'batch\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$scale" "$BATCH_SIZE" "$batches" "$elapsed" "$cleanup_elapsed" \
        "$rate" "$commits" "$changes" "$versions" "$manifests" "$peak" \
        "$check_ok" >> "$REPORT"
    echo "batch files=$scale elapsed_ms=$elapsed cleanup_ms=$cleanup_elapsed rate=$rate peak_memory_bytes=$peak"
    ACTIVE_WORKSPACES=()
done

if (( RAN_1000 == 1 )); then
    SPEEDUP="$("$PYTHON_BIN" -c \
        "print(round(int('$LEGACY_ELAPSED') / max(1, int('$FIRST_BATCH_ELAPSED')), 2))")"
    "$PYTHON_BIN" -c \
        "raise SystemExit(0 if float('$SPEEDUP') >= float('$MIN_SPEEDUP') else 1)" || \
        fail "1000 文件只快 ${SPEEDUP}x，低于 ${MIN_SPEEDUP}x"
    echo "single_1000_ms=$LEGACY_ELAPSED batch_1000_ms=$FIRST_BATCH_ELAPSED speedup=${SPEEDUP}x"
fi

echo "VEXFS PG CREATE BATCH PERFORMANCE: PASS"
echo "report=$REPORT"
