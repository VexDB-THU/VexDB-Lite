#!/bin/bash
set -euo pipefail

VEXFS="${1:?usage: vexfs_cli_smoke.sh /path/to/vexfs}"
TMP_DIR="$(mktemp -d -t vexfs-cli-smoke)"
DB="$TMP_DIR/vexfs.sqlite3"
cleanup() {
    rm -f "$DB" "$DB-wal" "$DB-shm"
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

"$VEXFS" --db "$DB" --workspace smoke setup >/dev/null
"$VEXFS" --db "$DB" --workspace smoke mkdir /agent
printf 'hello from cli' | "$VEXFS" --db "$DB" --workspace smoke write /agent/task.txt >/dev/null
printf 'alpha' | "$VEXFS" --db "$DB" --workspace smoke write /agent/version.txt >/dev/null
printf 'beta' | "$VEXFS" --db "$DB" --workspace smoke write /agent/version.txt >/dev/null

CONTENT="$("$VEXFS" --db "$DB" --workspace smoke cat /agent/task.txt)"
[ "$CONTENT" = "hello from cli" ]

LIST="$("$VEXFS" --db "$DB" --workspace smoke --json ls /agent)"
printf '%s' "$LIST" | /usr/bin/python3 -c \
    'import json,sys; rows=json.load(sys.stdin); assert rows[0]["name"] == "task.txt"'

HISTORY="$("$VEXFS" --db "$DB" --workspace smoke --json history /agent/version.txt)"
printf '%s' "$HISTORY" | /usr/bin/python3 -c \
    'import json,sys; page=json.load(sys.stdin); rows=page["entries"]; assert [row["version"] for row in rows] == [2,1]; assert rows[0]["current"] is True; assert page["next_before"] is None'
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

set +e
DOCTOR="$("$VEXFS" --db "$DB" --workspace smoke --json doctor)"
DOCTOR_STATUS=$?
set -e
[ "$DOCTOR_STATUS" -eq 0 ] || [ "$DOCTOR_STATUS" -eq 1 ]
printf '%s' "$DOCTOR" | /usr/bin/python3 -c \
    'import json,sys; value=json.load(sys.stdin); assert value["database"]["contract_version"] == "0.3.0"'

MOUNTS="$("$VEXFS" --json mount status)"
printf '%s' "$MOUNTS" | /usr/bin/python3 -c 'import json,sys; assert isinstance(json.load(sys.stdin), list)'

echo "VEXFS CLI SMOKE: PASS"
