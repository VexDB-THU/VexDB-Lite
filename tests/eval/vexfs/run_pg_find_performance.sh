#!/usr/bin/env bash
set -euo pipefail

CONTAINER="${VEXDB_PG_CONTAINER:-vexdb_pg19-test}"
DATABASE="${VEXDB_PG_DATABASE:-test}"
FILES="${1:-10000}"
WORKSPACE="pg-find-performance"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/vexfs-pg-find.XXXXXX")"

case "$FILES" in
    10000) BUDGET_MS=5000 ;;
    100000) BUDGET_MS=30000 ;;
    *) echo "file count must be 10000 or 100000" >&2; exit 2 ;;
esac

cleanup() {
    docker exec "$CONTAINER" psql -d "$DATABASE" -X -q -v ON_ERROR_STOP=1 \
        -c "SELECT vexfs_workspace_drop('$WORKSPACE', true);" >/dev/null 2>&1 || true
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

docker inspect "$CONTAINER" >/dev/null
SEED_STARTED_NS="$(python3 -c 'import time; print(int(time.monotonic() * 1000000000))')"
docker exec -i "$CONTAINER" psql -d "$DATABASE" -X -q -v ON_ERROR_STOP=1 <<SQL >/dev/null
SELECT vexfs_workspace_drop('$WORKSPACE', true);
SELECT vexfs_workspace_create('$WORKSPACE');
SELECT vexfs_mkdir('$WORKSPACE', '/files');

-- This benchmark measures the database-side find query, not the public
-- per-file mutation path. Build a valid live inode/dentry tree in one bulk
-- transaction so a 100k query benchmark does not spend minutes generating
-- an equally large commit and manifest history.
BEGIN;
WITH target AS (
    SELECT w.workspace_id,
           w.owner_oid,
           w.owner_role,
           d.inode_id AS parent_inode
      FROM _vexfs.workspaces AS w
      JOIN _vexfs.dentries AS d
        ON d.workspace_id = w.workspace_id
      JOIN _vexfs.inodes AS root
        ON root.inode_id = d.parent_inode
       AND root.inode_id = w.root_inode
     WHERE w.name = '$WORKSPACE'
       AND d.name = 'files'
), inserted AS (
    INSERT INTO _vexfs.inodes(
        workspace_id, kind, mode, owner_oid, owner_role,
        owner_principal, current_version)
    SELECT target.workspace_id,
           'file',
           420,
           target.owner_oid,
           target.owner_role,
           target.owner_role::text,
           1
      FROM target
      CROSS JOIN generate_series(1, $FILES)
    RETURNING workspace_id, inode_id
), numbered AS (
    SELECT workspace_id,
           inode_id,
           row_number() OVER (ORDER BY inode_id) - 1 AS item
      FROM inserted
)
INSERT INTO _vexfs.dentries(workspace_id, parent_inode, name, inode_id)
SELECT numbered.workspace_id,
       target.parent_inode,
       'f' || lpad(numbered.item::text, 6, '0') || '.txt',
       numbered.inode_id
  FROM numbered
  JOIN target ON target.workspace_id = numbered.workspace_id;

UPDATE _vexfs.workspaces
   SET live_files = $FILES
 WHERE name = '$WORKSPACE';
COMMIT;
SQL
SEED_FINISHED_NS="$(python3 -c 'import time; print(int(time.monotonic() * 1000000000))')"
SEED_MS="$(python3 - "$SEED_STARTED_NS" "$SEED_FINISHED_NS" <<'PY'
import sys
print((int(sys.argv[2]) - int(sys.argv[1])) / 1_000_000)
PY
)"

explain_ms() {
    local sql="$1"
    docker exec "$CONTAINER" psql -d "$DATABASE" -X -q -t -A \
        -v ON_ERROR_STOP=1 -c "EXPLAIN (ANALYZE, FORMAT JSON) $sql" \
        | python3 -c 'import json,sys; print(json.load(sys.stdin)[0]["Execution Time"])'
}

FIRST_SQL="SELECT vexfs_find('$WORKSPACE','/files','*.txt','file',0,0,NULL,NULL,NULL,100)"
SECOND_SQL="SELECT vexfs_find('$WORKSPACE','/files','*.txt','file',0,0,NULL,NULL,'/files/f000099.txt',100)"
LAST_NAME="f$(printf '%06d' $((FILES - 1))).txt"
SELECTIVE_SQL="SELECT vexfs_find('$WORKSPACE','/files','$LAST_NAME','file',0,0,NULL,NULL,NULL,10)"

docker exec "$CONTAINER" psql -d "$DATABASE" -X -q -t -A -v ON_ERROR_STOP=1 \
    -c "$FIRST_SQL" >"$TMP_DIR/first.json"
docker exec "$CONTAINER" psql -d "$DATABASE" -X -q -t -A -v ON_ERROR_STOP=1 \
    -c "$SECOND_SQL" >"$TMP_DIR/second.json"
docker exec "$CONTAINER" psql -d "$DATABASE" -X -q -t -A -v ON_ERROR_STOP=1 \
    -c "$SELECTIVE_SQL" >"$TMP_DIR/selective.json"

python3 - "$TMP_DIR" "$LAST_NAME" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
last_name = sys.argv[2]
first = json.loads((root / "first.json").read_text())
second = json.loads((root / "second.json").read_text())
selective = json.loads((root / "selective.json").read_text())
assert len(first["entries"]) == 100
assert first["entries"][0]["path"] == "/files/f000000.txt"
assert first["next_cursor"] == "/files/f000099.txt"
assert len(second["entries"]) == 100
assert second["entries"][0]["path"] == "/files/f000100.txt"
assert len(selective["entries"]) == 1
assert selective["entries"][0]["name"] == last_name
PY

FIRST_MS="$(explain_ms "$FIRST_SQL")"
SECOND_MS="$(explain_ms "$SECOND_SQL")"
SELECTIVE_MS="$(explain_ms "$SELECTIVE_SQL")"

python3 - "$FILES" "$BUDGET_MS" "$SEED_MS" "$FIRST_MS" "$SECOND_MS" "$SELECTIVE_MS" <<'PY'
import json
import sys

files = int(sys.argv[1])
budget = float(sys.argv[2])
seed, first, second, selective = map(float, sys.argv[3:])
for label, value in (("first_page", first), ("second_page", second),
                     ("selective", selective)):
    if value > budget:
        raise SystemExit(f"PG find {label} exceeds budget: {value:.3f} > {budget:.3f} ms")
print(json.dumps({
    "files": files,
    "fixture_seed_ms": seed,
    "find_first_page_ms": first,
    "find_second_page_ms": second,
    "find_selective_ms": selective,
    "budget_ms": budget,
}, separators=(",", ":")))
PY
