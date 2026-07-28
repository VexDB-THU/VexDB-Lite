#!/usr/bin/env python3
import json
import os
import resource
import sqlite3
import sys
import time


def fail(message: str) -> None:
    raise SystemExit(f"VEXFS SNAPSHOT POLICY SQLITE PERFORMANCE: FAIL: {message}")


if len(sys.argv) != 8:
    fail("usage: extension db workspace count agent_keep safety_keep max_query_ms")

extension, database, workspace = sys.argv[1:4]
count = int(sys.argv[4])
agent_keep = int(sys.argv[5])
safety_keep = int(sys.argv[6])
max_query_ms = int(sys.argv[7])

connection = sqlite3.connect(database)
connection.enable_load_extension(True)
connection.load_extension(extension)
connection.execute("SELECT vexfs_init()")
connection.execute("SELECT vexfs_workspace_create(?)", (workspace,))
workspace_id, head_commit = connection.execute(
    "SELECT id,head_commit FROM _vexfs_workspaces WHERE name=?", (workspace,)
).fetchone()

insert = """
INSERT INTO _vexfs_snapshots(
    workspace_id,name,snapshot_type,commit_id,created_at)
VALUES(?,?,?,?,?)
"""
old_created_at = 1_700_000_000_000
connection.executemany(
    insert,
    (
        (
            workspace_id,
            f"{'agent' if index % 2 == 0 else 'safety'}-{index:06d}",
            "agent" if index % 2 == 0 else "safety",
            head_commit,
            old_created_at + index,
        )
        for index in range(count)
    ),
)
connection.execute(
    insert,
    (workspace_id, "manual-keep", "manual", head_commit, old_created_at - 1),
)
connection.commit()
connection.execute(
    "SELECT vexfs_snapshot_policy_set(?,?,?,0)",
    (workspace, agent_keep, safety_keep),
).fetchone()


def timed_json(sql: str) -> tuple[dict, int]:
    started = time.monotonic_ns()
    value = json.loads(connection.execute(sql, (workspace,)).fetchone()[0])
    elapsed_ms = (time.monotonic_ns() - started) // 1_000_000
    if elapsed_ms > max_query_ms:
        fail(f"query exceeded {max_query_ms} ms: {elapsed_ms} ms")
    return value, elapsed_ms


policy, policy_ms = timed_json("SELECT vexfs_snapshot_policy_get(?)")
dry_run, dry_run_ms = timed_json("SELECT vexfs_snapshot_prune(?,1)")
pruned, prune_ms = timed_json("SELECT vexfs_snapshot_prune(?,0)")

expected_candidates = count - agent_keep - safety_keep
if policy["snapshots"]["total"] != count + 1:
    fail("policy count is incorrect")
if dry_run["candidate_count"] != expected_candidates or dry_run["deleted"] != 0:
    fail("dry-run candidate count is incorrect")
if len(dry_run["candidates"]) != min(100, expected_candidates):
    fail("dry-run sample is not bounded to 100 rows")
if dry_run["truncated"] is not (expected_candidates > 100):
    fail("dry-run truncated flag is incorrect")
if pruned["deleted"] != expected_candidates:
    fail("prune deleted count is incorrect")
remaining = connection.execute(
    """
    SELECT count(*),
           count(*) FILTER(WHERE snapshot_type='manual'),
           count(*) FILTER(WHERE snapshot_type='agent'),
           count(*) FILTER(WHERE snapshot_type='safety')
      FROM _vexfs_snapshots WHERE workspace_id=?
    """,
    (workspace_id,),
).fetchone()
if remaining != (agent_keep + safety_keep + 1, 1, agent_keep, safety_keep):
    fail(f"remaining snapshot classes are incorrect: {remaining}")

max_rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
if sys.platform != "darwin":
    max_rss *= 1024
connection.close()
os.unlink(database)
print(f"{policy_ms}|{dry_run_ms}|{prune_ms}|{max_rss}|{expected_candidates}")
