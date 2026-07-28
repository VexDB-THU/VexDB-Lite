#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
CONTAINER="${VEXDB_PG_CONTAINER:-vexdb_pg19-test}"
DATABASE="${VEXDB_PG_DATABASE:-test}"
DSN="${VEXDB_PG_DSN:-postgresql://postgres@127.0.0.1:5433/test}"
CLI="${VEXDB_PG_CLI:-$ROOT/vexdb_sqlite/build/vexdb}"
RG="${VEXDB_PG_RG:-$(command -v rg || true)}"
WORKSPACE="${VEXDB_PG_GREP_WORKSPACE:-pg-grep-performance}"
FILE_COUNT="${VEXDB_PG_GREP_FILES:-5000}"
CONTENT_BYTES="${VEXDB_PG_GREP_CONTENT_BYTES:-2048}"
ITERATIONS="${VEXDB_PG_GREP_ITERATIONS:-5}"
MATCH_EVERY="${VEXDB_PG_GREP_MATCH_EVERY:-1000}"
PATTERN="${VEXDB_PG_GREP_PATTERN:-rare-vexfs-token-2026}"
KEEP_WORKSPACE="${VEXFS_KEEP_WORKSPACE:-0}"
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)"
OUTPUT_DIR="${VEXFS_EVAL_OUTPUT_DIR:-$ROOT/vexdb_sqlite/build/eval/vexfs-pg-grep/$RUN_ID}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/vexfs-pg-grep-perf.XXXXXX")"
MOUNT_POINT="$TMP_DIR/mount"
MOUNTED=false

for numeric in "$FILE_COUNT" "$CONTENT_BYTES" "$ITERATIONS" "$MATCH_EVERY"; do
    [[ "$numeric" =~ ^[0-9]+$ ]] || { echo "性能参数必须是正整数" >&2; exit 2; }
done
(( FILE_COUNT >= 100 && FILE_COUNT <= 10000 )) || {
    echo "VEXDB_PG_GREP_FILES 必须在 100..10000" >&2; exit 2;
}
(( CONTENT_BYTES >= 128 && CONTENT_BYTES <= 8192 )) || {
    echo "VEXDB_PG_GREP_CONTENT_BYTES 必须在 128..8192" >&2; exit 2;
}
(( ITERATIONS >= 3 && ITERATIONS <= 9 )) || {
    echo "VEXDB_PG_GREP_ITERATIONS 必须在 3..9" >&2; exit 2;
}
(( MATCH_EVERY >= 1 && MATCH_EVERY <= FILE_COUNT )) || {
    echo "VEXDB_PG_GREP_MATCH_EVERY 必须在 1..FILE_COUNT" >&2; exit 2;
}
case "$WORKSPACE" in
    ''|*[!A-Za-z0-9_-]*) echo "workspace 只能包含字母、数字、下划线和连字符" >&2; exit 2 ;;
esac
case "$PATTERN" in
    ''|*[!A-Za-z0-9_-]*) echo "pattern 只能包含字母、数字、下划线和连字符" >&2; exit 2 ;;
esac
[ -x "$CLI" ] || { echo "找不到 vexdb CLI：$CLI" >&2; exit 2; }
[ -x "$RG" ] || { echo "找不到 rg：$RG" >&2; exit 2; }
mkdir -p "$MOUNT_POINT" "$OUTPUT_DIR"

vexfs() {
    "$CLI" fs --backend pg --dsn "$DSN" --workspace "$WORKSPACE" "$@"
}

drop_workspace() {
    docker exec "$CONTAINER" psql -d "$DATABASE" -X -q -v ON_ERROR_STOP=1 \
        -c "SELECT vexfs_workspace_drop('$WORKSPACE', true);" >/dev/null 2>&1 || true
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    cd /
    if [ "$MOUNTED" = true ]; then
        vexfs unmount --force "$MOUNT_POINT" >/dev/null 2>&1 || true
    fi
    if [ "$KEEP_WORKSPACE" != 1 ]; then
        drop_workspace
    fi
    rm -rf "$TMP_DIR"
    exit "$status"
}
trap cleanup EXIT INT TERM

median() {
    sort -n "$1" | awk '
        { value[NR]=$1 }
        END {
          if (NR % 2) result=value[(NR+1)/2];
          else result=(value[NR/2]+value[NR/2+1])/2;
          printf "%.3f", result;
        }'
}

measure_cli_grep() {
    local phase="$1" expected_index="$2"
    local times="$TMP_DIR/$phase-times.txt"
    local iteration started ended elapsed output
    : >"$times"
    for iteration in $(seq 1 "$ITERATIONS"); do
        output="$TMP_DIR/$phase-$iteration.json"
        started="$(/usr/bin/perl -MTime::HiRes=time -e 'printf "%.9f", time')"
        /usr/bin/perl -e 'alarm 300; exec @ARGV' \
            "$CLI" fs --backend pg --dsn "$DSN" --workspace "$WORKSPACE" \
            --json grep "$PATTERN" /corpus >"$output"
        ended="$(/usr/bin/perl -MTime::HiRes=time -e 'printf "%.9f", time')"
        elapsed="$(awk -v start="$started" -v finish="$ended" \
            'BEGIN { printf "%.3f", (finish-start)*1000 }')"
        printf '%s\n' "$elapsed" >>"$times"
        grep -Fq "\"index_used\":$expected_index" "$output" || {
            echo "$phase 没有按预期报告 index_used=$expected_index" >&2; exit 1;
        }
        grep -Fq "\"match_count\":$EXPECTED_MATCHES" "$output" || {
            echo "$phase 匹配数错误" >&2; sed -n '1p' "$output" >&2; exit 1;
        }
    done
    median "$times"
}

measure_native_rg() {
    local times="$TMP_DIR/native-times.txt" iteration started ended elapsed output count
    : >"$times"
    for iteration in $(seq 1 "$ITERATIONS"); do
        output="$TMP_DIR/native-$iteration.out"
        started="$(/usr/bin/perl -MTime::HiRes=time -e 'printf "%.9f", time')"
        /usr/bin/perl -e 'alarm 300; exec @ARGV' \
            "$RG" --threads 1 --no-ignore --fixed-strings --files-with-matches \
            "$PATTERN" "$MOUNT_POINT/corpus" >"$output"
        ended="$(/usr/bin/perl -MTime::HiRes=time -e 'printf "%.9f", time')"
        elapsed="$(awk -v start="$started" -v finish="$ended" \
            'BEGIN { printf "%.3f", (finish-start)*1000 }')"
        printf '%s\n' "$elapsed" >>"$times"
        count="$(wc -l <"$output" | tr -d ' ')"
        [ "$count" = "$EXPECTED_MATCHES" ] || {
            echo "原生 rg 匹配文件数错误：期望 $EXPECTED_MATCHES，实际 $count" >&2; exit 1;
        }
    done
    median "$times"
}

docker inspect "$CONTAINER" >/dev/null
MEMORY_LIMIT="$(docker inspect "$CONTAINER" --format '{{.HostConfig.Memory}}')"
if (( MEMORY_LIMIT <= 0 || MEMORY_LIMIT > 1073741824 )); then
    echo "PG grep 性能测试要求容器 memory.max 在 1 GiB 以内，实际 $MEMORY_LIMIT" >&2
    exit 1
fi
OOM_BEFORE="$(docker exec "$CONTAINER" sh -lc \
    "awk '\$1==\"oom_kill\"{print \$2}' /sys/fs/cgroup/memory.events")"
drop_workspace
docker exec "$CONTAINER" psql -d "$DATABASE" -X -q -v ON_ERROR_STOP=1 \
    -c "CREATE EXTENSION IF NOT EXISTS pg_trgm;" >/dev/null

CREATE_RESULT="$(docker exec -i "$CONTAINER" psql -d "$DATABASE" -X -q -t -A -F '|' \
    -v ON_ERROR_STOP=1 -v file_count="$FILE_COUNT" -v content_bytes="$CONTENT_BYTES" \
    -v match_every="$MATCH_EVERY" -v pattern="$PATTERN" -v workspace="$WORKSPACE" \
    <<'SQL' | tail -n 1
SELECT set_config('vexfs.perf_files', :'file_count', false);
SELECT set_config('vexfs.perf_content_bytes', :'content_bytes', false);
SELECT set_config('vexfs.perf_match_every', :'match_every', false);
SELECT set_config('vexfs.perf_pattern', :'pattern', false);
SELECT set_config('vexfs.perf_workspace', :'workspace', false);
CREATE TEMP TABLE vexfs_grep_build(elapsed_ms double precision) ON COMMIT PRESERVE ROWS;
DO $$
DECLARE
    v_files integer := current_setting('vexfs.perf_files')::integer;
    v_bytes integer := current_setting('vexfs.perf_content_bytes')::integer;
    v_every integer := current_setting('vexfs.perf_match_every')::integer;
    v_pattern text := current_setting('vexfs.perf_pattern');
    v_workspace text := current_setting('vexfs.perf_workspace');
    v_filler text;
    v_content text;
    v_started timestamptz;
BEGIN
    PERFORM vexfs_workspace_create(v_workspace);
    PERFORM vexfs_mkdir(v_workspace, '/corpus');
    v_filler := repeat('alpha beta gamma delta epsilon ', (v_bytes / 31) + 2);
    v_started := clock_timestamp();
    FOR i IN 1..v_files LOOP
        v_content := left(v_filler, v_bytes - 17) || ' ' || lpad(i::text, 16, '0');
        IF i % v_every = 0 THEN
            v_content := overlay(v_content PLACING v_pattern FROM 33);
        END IF;
        PERFORM vexfs_write(
            v_workspace, '/corpus/file-' || lpad(i::text, 6, '0') || '.txt',
            convert_to(v_content, 'UTF8'));
    END LOOP;
    INSERT INTO vexfs_grep_build VALUES (
        extract(epoch FROM clock_timestamp() - v_started) * 1000.0);
END $$;
SELECT round(build.elapsed_ms::numeric,3),
       (vexfs_workspace_stat(:'workspace')->>'head_commit')::bigint,
       (SELECT count(*) FROM vexfs_list(:'workspace','/corpus'))
  FROM vexfs_grep_build AS build;
SQL
)"
IFS='|' read -r CREATE_MS HEAD FILES <<<"$CREATE_RESULT"
EXPECTED_HEAD=$((FILE_COUNT + 2))
EXPECTED_MATCHES=$((FILE_COUNT / MATCH_EVERY))
[ "$HEAD" = "$EXPECTED_HEAD" ] && [ "$FILES" = "$FILE_COUNT" ] && \
    [ "$EXPECTED_MATCHES" -gt 0 ] || {
        echo "PG grep 数据集状态错误：$CREATE_RESULT" >&2; exit 1;
    }

vexfs index disable >/dev/null
vexfs --json grep "$PATTERN" /corpus >/dev/null
SCAN_MEDIAN_MS="$(measure_cli_grep scan false)"

BUILD_STARTED="$(/usr/bin/perl -MTime::HiRes=time -e 'printf "%.9f", time')"
vexfs index enable >"$TMP_DIR/index-enable.json"
BUILD_ENDED="$(/usr/bin/perl -MTime::HiRes=time -e 'printf "%.9f", time')"
INDEX_BUILD_MS="$(awk -v start="$BUILD_STARTED" -v finish="$BUILD_ENDED" \
    'BEGIN { printf "%.3f", (finish-start)*1000 }')"
grep -Fq '"enabled":true' "$TMP_DIR/index-enable.json"
grep -Fq '"dirty":false' "$TMP_DIR/index-enable.json"
grep -Fq "\"indexed_files\":$FILE_COUNT" "$TMP_DIR/index-enable.json"

PLAN="$(docker exec "$CONTAINER" psql -d "$DATABASE" -X -q -t -A \
    -v ON_ERROR_STOP=1 -c \
    "SET enable_seqscan=off;
     EXPLAIN (COSTS OFF) SELECT inode_id FROM _vexfs.grep_documents
      WHERE content LIKE '%$PATTERN%';
     RESET enable_seqscan;")"
printf '%s\n' "$PLAN" >"$OUTPUT_DIR/pg-plan.txt"
grep -Fq 'vexfs_grep_documents_content_trgm_idx' "$OUTPUT_DIR/pg-plan.txt" || {
    echo "PostgreSQL 计划没有使用 trigram GIN" >&2
    cat "$OUTPUT_DIR/pg-plan.txt" >&2
    exit 1
}

vexfs --json grep "$PATTERN" /corpus >/dev/null
INDEX_MEDIAN_MS="$(measure_cli_grep index true)"

vexfs mount "$MOUNT_POINT" >/dev/null
MOUNTED=true
"$RG" --threads 1 --no-ignore --fixed-strings --files-with-matches \
    "$PATTERN" "$MOUNT_POINT/corpus" >/dev/null
NATIVE_MEDIAN_MS="$(measure_native_rg)"
cd /
vexfs unmount "$MOUNT_POINT" >/dev/null
MOUNTED=false

SCAN_SPEEDUP="$(awk -v scan="$SCAN_MEDIAN_MS" -v idx="$INDEX_MEDIAN_MS" \
    'BEGIN { printf "%.3f", scan/idx }')"
NATIVE_SPEEDUP="$(awk -v native="$NATIVE_MEDIAN_MS" -v idx="$INDEX_MEDIAN_MS" \
    'BEGIN { printf "%.3f", native/idx }')"
awk -v scan="$SCAN_MEDIAN_MS" -v idx="$INDEX_MEDIAN_MS" \
    'BEGIN { exit !(idx < scan) }' || {
        echo "PG trigram 没有快于 PG 扫描：index=$INDEX_MEDIAN_MS scan=$SCAN_MEDIAN_MS" >&2
        exit 1
    }
awk -v native="$NATIVE_MEDIAN_MS" -v idx="$INDEX_MEDIAN_MS" \
    'BEGIN { exit !(idx < native) }' || {
        echo "PG trigram 没有快于挂载目录原生 rg：index=$INDEX_MEDIAN_MS native=$NATIVE_MEDIAN_MS" >&2
        exit 1
    }

INDEX_BYTES="$(docker exec "$CONTAINER" psql -d "$DATABASE" -X -q -t -A \
    -v ON_ERROR_STOP=1 -c \
    "SELECT pg_total_relation_size('_vexfs.grep_documents'::regclass);")"
MEMORY_CURRENT="$(docker exec "$CONTAINER" sh -lc 'cat /sys/fs/cgroup/memory.current')"
MEMORY_PEAK="$(docker exec "$CONTAINER" sh -lc 'cat /sys/fs/cgroup/memory.peak')"
OOM_AFTER="$(docker exec "$CONTAINER" sh -lc \
    "awk '\$1==\"oom_kill\"{print \$2}' /sys/fs/cgroup/memory.events")"
[ "$OOM_AFTER" = "$OOM_BEFORE" ] || {
    echo "PG grep 性能测试触发 OOM kill：before=$OOM_BEFORE after=$OOM_AFTER" >&2; exit 1;
}

cat >"$OUTPUT_DIR/report.json" <<EOF
{"result":"PASS","workspace":"$WORKSPACE","files":$FILE_COUNT,"content_bytes":$CONTENT_BYTES,"matches":$EXPECTED_MATCHES,"iterations":$ITERATIONS,"create_ms":$CREATE_MS,"index_build_ms":$INDEX_BUILD_MS,"pg_scan_median_ms":$SCAN_MEDIAN_MS,"pg_index_median_ms":$INDEX_MEDIAN_MS,"native_rg_median_ms":$NATIVE_MEDIAN_MS,"scan_speedup":$SCAN_SPEEDUP,"native_speedup":$NATIVE_SPEEDUP,"index_bytes":$INDEX_BYTES,"memory_limit":$MEMORY_LIMIT,"memory_current":$MEMORY_CURRENT,"memory_peak":$MEMORY_PEAK,"oom_kill":$OOM_AFTER}
EOF
cat >"$OUTPUT_DIR/report.md" <<EOF
# VexFS PostgreSQL grep 性能

- 结果：PASS
- 文件：$FILE_COUNT，每个 $CONTENT_BYTES bytes，匹配文件 $EXPECTED_MATCHES
- 迭代：$ITERATIONS 次，取中位数，原生 rg 固定 1 线程
- 数据创建：$CREATE_MS ms
- PG scan：$SCAN_MEDIAN_MS ms
- PG trigram：$INDEX_MEDIAN_MS ms
- 挂载目录原生 rg：$NATIVE_MEDIAN_MS ms
- trigram 相对 PG scan：${SCAN_SPEEDUP}x
- trigram 相对原生 rg：${NATIVE_SPEEDUP}x
- 索引构建：$INDEX_BUILD_MS ms；派生表和索引：$INDEX_BYTES bytes
- 容器：memory.max=$MEMORY_LIMIT，memory.current=$MEMORY_CURRENT，memory.peak=$MEMORY_PEAK，oom_kill=$OOM_AFTER

查询计划见 \`pg-plan.txt\`，必须包含 \`vexfs_grep_documents_content_trgm_idx\`。
EOF

echo "VEXFS PG GREP PERFORMANCE: PASS (files=$FILE_COUNT scan_ms=$SCAN_MEDIAN_MS index_ms=$INDEX_MEDIAN_MS native_rg_ms=$NATIVE_MEDIAN_MS scan_speedup=${SCAN_SPEEDUP}x native_speedup=${NATIVE_SPEEDUP}x oom_kill=$OOM_AFTER)"
echo "report=$OUTPUT_DIR/report.md"
