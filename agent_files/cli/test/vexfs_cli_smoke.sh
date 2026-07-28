#!/bin/bash
set -euo pipefail

VEXFS="${1:?usage: vexfs_cli_smoke.sh /path/to/vexfs}"
TMP_DIR="$(mktemp -d -t vexfs-cli-smoke)"
DB="$TMP_DIR/vexfs.sqlite3"
DEST_DB="$TMP_DIR/restored.sqlite3"
ARCHIVE="$TMP_DIR/workspace.vexfs"
cleanup() {
    rm -f "$DB" "$DB-wal" "$DB-shm" "$DEST_DB" "$DEST_DB-wal" "$DEST_DB-shm" "$ARCHIVE"
    rmdir "$TMP_DIR" 2>/dev/null || true
}
trap cleanup EXIT

set +e
MISSING_DOCTOR="$("$VEXFS" --db "$DB" --workspace smoke --json doctor)"
MISSING_STATUS=$?
set -e
[ "$MISSING_STATUS" -eq 1 ]
[ ! -e "$DB" ]
printf '%s' "$MISSING_DOCTOR" | /usr/bin/python3 -c \
    'import json,sys; value=json.load(sys.stdin); assert "error" in value["database"]'
set +e
"$VEXFS" --db "$DB" --workspace smoke --json check >/dev/null 2>&1
MISSING_CHECK_STATUS=$?
set -e
[ "$MISSING_CHECK_STATUS" -eq 7 ]
[ ! -e "$DB" ]

"$VEXFS" --db "$DB" --workspace smoke setup >/dev/null
"$VEXFS" --db "$DB" --workspace smoke mkdir /agent
printf 'hello from cli' | "$VEXFS" --db "$DB" --workspace smoke write /agent/task.txt >/dev/null
printf 'alpha' | "$VEXFS" --db "$DB" --workspace smoke write /agent/version.txt >/dev/null
printf 'beta' | "$VEXFS" --db "$DB" --workspace smoke write /agent/version.txt >/dev/null

CONTENT="$("$VEXFS" --db "$DB" --workspace smoke cat /agent/task.txt)"
[ "$CONTENT" = "hello from cli" ]

"$VEXFS" --db "$DB" --workspace smoke ln /agent/task.txt /agent/task-copy.txt
LINK_STAT="$("$VEXFS" --db "$DB" --workspace smoke stat /agent/task-copy.txt)"
printf '%s' "$LINK_STAT" | /usr/bin/python3 -c \
    'import json,sys; value=json.load(sys.stdin); assert value["link_count"] == 2'
"$VEXFS" --db "$DB" --workspace smoke chown -:1234 /agent/task.txt
printf '%s' "$("$VEXFS" --db "$DB" --workspace smoke stat /agent/task.txt)" | /usr/bin/python3 -c \
    'import json,sys; assert json.load(sys.stdin)["gid"] == 1234'
printf '[{"principal":"alice","effect":"allow","permissions":"read,write","inherit":1}]' |
    "$VEXFS" --db "$DB" --workspace smoke setfacl /agent/task.txt
ACL="$("$VEXFS" --db "$DB" --workspace smoke getfacl /agent/task.txt)"
printf '%s' "$ACL" | /usr/bin/python3 -c \
    'import json,sys; value=json.load(sys.stdin); assert value[0]["principal"] == "alice"'

LIST="$("$VEXFS" --db "$DB" --workspace smoke --json ls /agent)"
printf '%s' "$LIST" | /usr/bin/python3 -c \
    'import json,sys; rows=json.load(sys.stdin); assert any(row["name"] == "task.txt" for row in rows)'

HISTORY="$("$VEXFS" --db "$DB" --workspace smoke --json history /agent/version.txt)"
printf '%s' "$HISTORY" | /usr/bin/python3 -c \
    'import json,sys; page=json.load(sys.stdin); rows=page["entries"]; assert [row["version"] for row in rows] == [2,1]; assert all(len(row["checksum"]) == 64 for row in rows); assert rows[0]["current"] is True; assert page["next_before"] is None'
[ "$("$VEXFS" --db "$DB" --workspace smoke stat /agent/version.txt | /usr/bin/python3 -c 'import json,sys; print(json.load(sys.stdin)["checksum"])')" = "4bb4fbcc335013b6f0e731486a8c27b113d6730fcfb7900dc59e1949d93558c6" ]
[ "$("$VEXFS" --db "$DB" --workspace smoke show /agent/version.txt --version 1)" = "alpha" ]
set +e
DIFF="$("$VEXFS" --db "$DB" --workspace smoke diff /agent/version.txt --from 1 --to 2)"
DIFF_STATUS=$?
set -e
[ "$DIFF_STATUS" -eq 1 ]
printf '%s' "$DIFF" | grep -q -- '-alpha'
set +e
"$VEXFS" --db "$DB" --workspace smoke diff /agent/version.txt --from 99 --to 2 >/dev/null 2>&1
MISSING_VERSION_STATUS=$?
set -e
[ "$MISSING_VERSION_STATUS" -eq 3 ]
"$VEXFS" --db "$DB" --workspace smoke restore /agent/version.txt --version 1 --dry-run >/dev/null
[ "$("$VEXFS" --db "$DB" --workspace smoke stat /agent/version.txt | /usr/bin/python3 -c 'import json,sys; print(json.load(sys.stdin)["version"])')" = "2" ]
[ "$("$VEXFS" --db "$DB" --workspace smoke restore /agent/version.txt --version 1)" = "3" ]
[ "$("$VEXFS" --db "$DB" --workspace smoke cat /agent/version.txt)" = "alpha" ]

SNAPSHOT_COMMIT="$("$VEXFS" --db "$DB" --workspace smoke snapshot create cli-baseline)"
[ "$SNAPSHOT_COMMIT" -gt 0 ]
WORKSPACE_LOG="$("$VEXFS" --db "$DB" --workspace smoke --json workspace log --limit 2)"
printf '%s' "$WORKSPACE_LOG" | /usr/bin/python3 -c \
    'import json,sys; page=json.load(sys.stdin); rows=page["entries"]; assert len(rows)==2; assert rows[0]["commit"] > rows[1]["commit"]; assert rows[0]["actor"]=="local"; assert rows[0]["has_snapshot"] is True; assert rows[0]["snapshots"]==["cli-baseline"]; assert page["next_before"]==rows[-1]["commit"]'
WORKSPACE_NEXT="$(printf '%s' "$WORKSPACE_LOG" | /usr/bin/python3 -c \
    'import json,sys; print(json.load(sys.stdin)["next_before"])')"
"$VEXFS" --db "$DB" --workspace smoke --json workspace log --limit 2 \
    --before "$WORKSPACE_NEXT" | /usr/bin/python3 -c \
    'import json,sys; rows=json.load(sys.stdin)["entries"]; assert rows and all(row["commit"] < int(sys.argv[1]) for row in rows)' "$WORKSPACE_NEXT"
"$VEXFS" --db "$DB" --workspace smoke workspace log --limit 1 | grep -q '^COMMIT'
printf 'changed after snapshot' | "$VEXFS" --db "$DB" --workspace smoke write /agent/task.txt >/dev/null
set +e
SNAPSHOT_DIFF="$("$VEXFS" --db "$DB" --workspace smoke snapshot diff cli-baseline)"
SNAPSHOT_DIFF_STATUS=$?
set -e
[ "$SNAPSHOT_DIFF_STATUS" -eq 1 ]
printf '%s' "$SNAPSHOT_DIFF" | /usr/bin/python3 -c \
    'import json,sys; value=json.load(sys.stdin); assert any(row["path"] == "/agent/task.txt" for row in value["changes"])'
"$VEXFS" --db "$DB" --workspace smoke snapshot restore cli-baseline --dry-run >/dev/null
"$VEXFS" --db "$DB" --workspace smoke snapshot restore cli-baseline >/dev/null
[ "$("$VEXFS" --db "$DB" --workspace smoke cat /agent/task.txt)" = "hello from cli" ]
[ "$("$VEXFS" --db "$DB" --workspace smoke cat /agent/task-copy.txt)" = "hello from cli" ]
"$VEXFS" --db "$DB" --workspace smoke snapshot diff cli-baseline >/dev/null
SNAPSHOT_LIST="$("$VEXFS" --db "$DB" --workspace smoke --json snapshot list)"
printf '%s' "$SNAPSHOT_LIST" | /usr/bin/python3 -c \
    'import json,sys; value=json.load(sys.stdin); names=[row["name"] for row in value]; assert "cli-baseline" in names; safety=[row for row in value if row["name"].startswith("vexfs-safety-smoke-")]; assert safety and all(row["type"]=="safety" for row in safety)'
"$VEXFS" --db "$DB" --workspace smoke snapshot create cli-agent-prune --type agent >/dev/null
"$VEXFS" --db "$DB" --workspace smoke snapshot policy set \
    --agent-keep 0 --safety-keep 0 --days 0 | /usr/bin/python3 -c \
    'import json,sys; value=json.load(sys.stdin); assert value["expired"]["agent"] >= 1; assert value["snapshots"]["manual"] >= 1'
"$VEXFS" --db "$DB" --workspace smoke snapshot prune --dry-run | /usr/bin/python3 -c \
    'import json,sys; value=json.load(sys.stdin); assert value["dry_run"] is True; assert value["deleted"] == 0; assert value["candidate_count"] >= 2'
"$VEXFS" --db "$DB" --workspace smoke snapshot prune | /usr/bin/python3 -c \
    'import json,sys; value=json.load(sys.stdin); assert value["deleted"] >= 2; assert value["policy"]["snapshots"]["manual"] >= 1'
"$VEXFS" --db "$DB" --workspace smoke snapshot policy set \
    --agent-keep 20 --safety-keep 10 --days 30 >/dev/null
"$VEXFS" --db "$DB" --workspace smoke snapshot show cli-baseline | /usr/bin/python3 -c \
    'import json,sys; value=json.load(sys.stdin); assert value["name"] == "cli-baseline"'
"$VEXFS" --db "$DB" --workspace smoke snapshot drop cli-baseline

CHECK="$("$VEXFS" --db "$DB" --workspace smoke --json check)"
printf '%s' "$CHECK" | /usr/bin/python3 -c \
    'import json,sys; value=json.load(sys.stdin); assert value["ok"] is True; assert value["mode"] == "deep"; assert value["checked"]["versions"] >= 5'
"$VEXFS" --db "$DB" --workspace smoke check --quick | grep -q '^OK '

"$VEXFS" --db "$DB" --workspace smoke quota show | /usr/bin/python3 -c \
    'import json,sys; value=json.load(sys.stdin); assert value["max_bytes"] is None; assert value["live_files"] > 0'
"$VEXFS" --db "$DB" --workspace smoke quota set \
    --max-bytes unlimited --max-files unlimited --max-file-bytes 4 >/dev/null
set +e
printf '12345' | "$VEXFS" --db "$DB" --workspace smoke write /agent/version.txt >/dev/null 2>&1
QUOTA_STATUS=$?
set -e
[ "$QUOTA_STATUS" -eq 6 ]
[ "$("$VEXFS" --db "$DB" --workspace smoke cat /agent/version.txt)" = "alpha" ]
"$VEXFS" --db "$DB" --workspace smoke quota set \
    --max-bytes unlimited --max-files unlimited --max-file-bytes unlimited >/dev/null

"$VEXFS" --db "$DB" --workspace smoke retention set --keep-versions 1 --keep-days 0 |
    /usr/bin/python3 -c 'import json,sys; assert json.load(sys.stdin)["reclaimable_versions"] > 0'
"$VEXFS" --db "$DB" --workspace smoke gc pause |
    /usr/bin/python3 -c 'import json,sys; assert json.load(sys.stdin)["gc_paused"] is True'
set +e
"$VEXFS" --db "$DB" --workspace smoke gc --batch 1 >/dev/null 2>&1
PAUSED_GC_STATUS=$?
set -e
[ "$PAUSED_GC_STATUS" -eq 7 ]
"$VEXFS" --db "$DB" --workspace smoke gc resume |
    /usr/bin/python3 -c 'import json,sys; assert json.load(sys.stdin)["gc_paused"] is False'
"$VEXFS" --db "$DB" --workspace smoke gc --batch 1 |
    /usr/bin/python3 -c 'import json,sys; value=json.load(sys.stdin); assert 0 <= value["deleted_versions"] <= 1; assert isinstance(value["has_more"], bool)'
while "$VEXFS" --db "$DB" --workspace smoke gc --batch 1000 |
    /usr/bin/python3 -c 'import json,sys; raise SystemExit(0 if json.load(sys.stdin)["has_more"] else 1)'
do :; done
"$VEXFS" --db "$DB" --workspace smoke --json check |
    /usr/bin/python3 -c 'import json,sys; assert json.load(sys.stdin)["ok"] is True'

"$VEXFS" --db "$DB" --workspace smoke export --output "$ARCHIVE" |
    /usr/bin/python3 -c 'import json,sys; value=json.load(sys.stdin); assert value["format_version"] == 2; assert len(value["package_checksum"]) == 64'
"$VEXFS" archive verify "$ARCHIVE" |
    /usr/bin/python3 -c 'import json,sys; assert json.load(sys.stdin)["ok"] is True'
"$VEXFS" --db "$DEST_DB" --workspace restored import "$ARCHIVE" >/dev/null
[ "$("$VEXFS" --db "$DEST_DB" --workspace restored cat /agent/task.txt)" = "hello from cli" ]
"$VEXFS" --db "$DEST_DB" --workspace restored --json check |
    /usr/bin/python3 -c 'import json,sys; assert json.load(sys.stdin)["ok"] is True'

set +e
DOCTOR="$("$VEXFS" --db "$DB" --workspace smoke --json doctor)"
DOCTOR_STATUS=$?
set -e
[ "$DOCTOR_STATUS" -eq 0 ] || [ "$DOCTOR_STATUS" -eq 1 ]
printf '%s' "$DOCTOR" | /usr/bin/python3 -c \
    'import json,sys; value=json.load(sys.stdin); database=value["database"]; assert database["schema_version"] == "0.9.0"; recovery=database["recovery"]; assert recovery["snapshot_count"] >= 0; assert recovery["protected_history_bytes"] >= 0; assert recovery["reclaimable_bytes"] >= 0; assert (recovery["oldest_recovery_commit"] is None) == (recovery["snapshot_count"] == 0)'

MOUNTS="$("$VEXFS" --json mount status)"
printf '%s' "$MOUNTS" | /usr/bin/python3 -c 'import json,sys; assert isinstance(json.load(sys.stdin), list)'

echo "VEXFS CLI SMOKE: PASS"
