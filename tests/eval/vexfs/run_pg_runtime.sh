#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
CLI="${VEXDB_PG_CLI:-$ROOT/vexdb_sqlite/build/vexdb}"
CONTAINER="${VEXDB_PG_CONTAINER:-vexdb_pg19-test}"
DATABASE="${VEXDB_PG_DATABASE:-test}"
DSN="${VEXDB_PG_DSN:-postgresql://postgres@127.0.0.1:5433/test}"
WORKSPACE="${VEXDB_PG_RUNTIME_WORKSPACE:-pg-runtime-eval}"
IMPORT_WORKSPACE="${WORKSPACE}-import"
CHECKS=0
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/vexfs-pg-runtime.XXXXXX")"

case "$WORKSPACE" in
    ''|*[!A-Za-z0-9_-]*) echo "workspace 只能包含字母、数字、下划线和连字符" >&2; exit 2 ;;
esac
[ -x "$CLI" ] || { echo "找不到 vexdb CLI：$CLI" >&2; exit 1; }

# 开发构建可能使用非系统 libpq；交付包不应依赖这个变量。
if [ -n "${VEXDB_PG_LIB_DIR:-}" ]; then
    export DYLD_LIBRARY_PATH="$VEXDB_PG_LIB_DIR${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
    export LD_LIBRARY_PATH="$VEXDB_PG_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

cleanup_workspace() {
    local name="$1"
    docker exec "$CONTAINER" psql -d "$DATABASE" -X -q -v ON_ERROR_STOP=1 \
        -c "SELECT vexfs_workspace_drop('$name', true);" >/dev/null 2>&1 || true
}

cleanup() {
    local status=$?
    trap - EXIT
    cleanup_workspace "$WORKSPACE"
    cleanup_workspace "$IMPORT_WORKSPACE"
    rm -rf "$TMP_DIR"
    exit "$status"
}
trap cleanup EXIT

vexfs() {
    "$CLI" fs --backend pg --dsn "$DSN" --workspace "$WORKSPACE" "$@"
}

vexfs_import() {
    "$CLI" fs --backend pg --dsn "$DSN" --workspace "$IMPORT_WORKSPACE" "$@"
}

contains() {
    local file="$1" value="$2" description="$3"
    if ! grep -Fq "$value" "$file"; then
        echo "${description}：缺少 $value" >&2
        sed -n '1,80p' "$file" >&2
        exit 1
    fi
    CHECKS=$((CHECKS + 1))
}

equals() {
    local actual="$1" expected="$2" description="$3"
    if [ "$actual" != "$expected" ]; then
        echo "${description}：期望 [$expected]，实际 [$actual]" >&2
        exit 1
    fi
    CHECKS=$((CHECKS + 1))
}

cleanup_workspace "$WORKSPACE"
cleanup_workspace "$IMPORT_WORKSPACE"
docker exec "$CONTAINER" psql -d "$DATABASE" -X -q -v ON_ERROR_STOP=1 \
    -c "CREATE EXTENSION IF NOT EXISTS pg_trgm;" >/dev/null

vexfs setup >"$TMP_DIR/setup.out"
contains "$TMP_DIR/setup.out" "postgresql [$WORKSPACE]" "setup backend"

vexfs mkdir /project/src
printf 'alpha\nbeta alpha\n' >"$TMP_DIR/v1.txt"
printf 'gamma\nbeta alpha\n' >"$TMP_DIR/v2.txt"
vexfs write /project/src/main.txt "$TMP_DIR/v1.txt" >"$TMP_DIR/write-v1.out"
vexfs write /project/src/main.txt "$TMP_DIR/v2.txt" >"$TMP_DIR/write-v2.out"
equals "$(vexfs cat /project/src/main.txt)" "$(cat "$TMP_DIR/v2.txt")" "cat current"
CHECKS=$((CHECKS + 2))

vexfs ls /project/src --json >"$TMP_DIR/ls.json"
contains "$TMP_DIR/ls.json" '"name":"main.txt"' "ls file"
contains "$TMP_DIR/ls.json" '"kind":"file"' "ls metadata"
vexfs stat /project/src/main.txt >"$TMP_DIR/stat.json"
contains "$TMP_DIR/stat.json" '"version":2' "stat version"

vexfs grep -n alpha /project >"$TMP_DIR/grep.out"
contains "$TMP_DIR/grep.out" '/project/src/main.txt:2:beta alpha' "grep line"
vexfs history /project/src/main.txt --json >"$TMP_DIR/history.json"
contains "$TMP_DIR/history.json" '"version":2' "history v2"
contains "$TMP_DIR/history.json" '"version":1' "history v1"
equals "$(vexfs show /project/src/main.txt --version 1)" "$(cat "$TMP_DIR/v1.txt")" \
    "show historical version"

vexfs ln /project/src/main.txt /project/src/hardlink.txt
equals "$(vexfs cat /project/src/hardlink.txt)" "$(cat "$TMP_DIR/v2.txt")" "hardlink content"
vexfs chown 501:20 /project/src/main.txt
vexfs stat /project/src/main.txt >"$TMP_DIR/chown.json"
contains "$TMP_DIR/chown.json" '"uid":501' "stored uid"
contains "$TMP_DIR/chown.json" '"gid":20' "stored gid"

printf '%s\n' '[{"principal":"postgres","effect":"allow","permissions":"read,write,execute,metadata","inherit":1}]' \
    >"$TMP_DIR/acl.json"
vexfs setfacl /project/src/main.txt "$TMP_DIR/acl.json"
vexfs getfacl /project/src/main.txt >"$TMP_DIR/getfacl.json"
contains "$TMP_DIR/getfacl.json" '"principal":"postgres"' "ACL principal"
contains "$TMP_DIR/getfacl.json" '"permissions":"read,write,execute,metadata"' "ACL permissions"

vexfs index status >"$TMP_DIR/index.json"
contains "$TMP_DIR/index.json" '"backend":"pg-trgm"' "PG trigram capability"
contains "$TMP_DIR/index.json" '"available":true' "PG trigram available"
contains "$TMP_DIR/index.json" '"enabled":false' "PG trigram default disabled"
vexfs index enable >"$TMP_DIR/index-enabled.json"
contains "$TMP_DIR/index-enabled.json" '"enabled":true' "PG trigram enabled"
contains "$TMP_DIR/index-enabled.json" '"dirty":false' "PG trigram clean"
vexfs --json grep alpha /project >"$TMP_DIR/indexed-grep.json"
contains "$TMP_DIR/indexed-grep.json" '"index_used":true' "PG grep uses trigram index"
vexfs quota set --max-bytes 1048576 --max-files 100 --max-file-bytes 262144 \
    >"$TMP_DIR/quota.json"
contains "$TMP_DIR/quota.json" '"max_bytes":1048576' "quota max bytes"
vexfs retention set --keep-versions 10 --keep-days 30 >"$TMP_DIR/retention.json"
contains "$TMP_DIR/retention.json" '"keep_versions":10' "retention versions"

vexfs snapshot create baseline >"$TMP_DIR/snapshot-create.out"
vexfs snapshot list >"$TMP_DIR/snapshot-list.out"
contains "$TMP_DIR/snapshot-list.out" "baseline" "snapshot list"
vexfs snapshot show baseline >"$TMP_DIR/snapshot-show.json"
contains "$TMP_DIR/snapshot-show.json" '"path":"/project/src/main.txt"' "snapshot tree"
printf 'after snapshot\n' >"$TMP_DIR/after.txt"
vexfs write /project/src/main.txt "$TMP_DIR/after.txt" >/dev/null
set +e
vexfs snapshot diff baseline >"$TMP_DIR/snapshot-diff.json"
diff_status=$?
set -e
equals "$diff_status" "1" "snapshot diff changed status"
contains "$TMP_DIR/snapshot-diff.json" '"path":"/project/src/main.txt"' "snapshot diff"
vexfs snapshot restore baseline >/dev/null
equals "$(vexfs cat /project/src/main.txt)" "$(cat "$TMP_DIR/v2.txt")" "snapshot restore"
vexfs --json grep alpha /project >"$TMP_DIR/restored-indexed-grep.json"
contains "$TMP_DIR/restored-indexed-grep.json" '"index_used":true' \
    "snapshot restore keeps PG trigram index current"

vexfs check >"$TMP_DIR/check.out"
contains "$TMP_DIR/check.out" "OK workspace=$WORKSPACE" "deep check"
vexfs gc --batch 100 >"$TMP_DIR/gc.json"
contains "$TMP_DIR/gc.json" '"deleted_versions"' "bounded GC"

vexfs export --output "$TMP_DIR/workspace.vexfs" >/dev/null
vexfs archive verify "$TMP_DIR/workspace.vexfs" >"$TMP_DIR/archive-verify.out"
contains "$TMP_DIR/archive-verify.out" '"ok":true' "archive verify"
vexfs_import import "$TMP_DIR/workspace.vexfs" >/dev/null
equals "$(vexfs_import cat /project/src/main.txt)" "$(cat "$TMP_DIR/v2.txt")" \
    "PG archive import"
vexfs_import check >"$TMP_DIR/import-check.out"
contains "$TMP_DIR/import-check.out" "OK workspace=$IMPORT_WORKSPACE" "import deep check"

vexfs descriptor "$TMP_DIR/workspace.json" >/dev/null
contains "$TMP_DIR/workspace.json" '"backend": "postgresql"' "descriptor backend"
contains "$TMP_DIR/workspace.json" "\"workspace\": \"$WORKSPACE\"" "descriptor workspace"
mode="$(stat -f '%Lp' "$TMP_DIR/workspace.json" 2>/dev/null || stat -c '%a' "$TMP_DIR/workspace.json")"
equals "$mode" "600" "descriptor permissions"

echo "VEXFS PG RUNTIME: PASS ($CHECKS checks)"
