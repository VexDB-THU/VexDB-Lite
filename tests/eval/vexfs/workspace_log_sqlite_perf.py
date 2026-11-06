#!/usr/bin/env python3
import json
import resource
import sqlite3
import sys
import time


def main() -> int:
    if len(sys.argv) != 8:
        raise SystemExit(
            "usage: workspace_log_sqlite_perf.py EXTENSION DB WORKSPACE COUNT LIMIT MAX_MS MAX_RSS")
    extension, database, workspace = sys.argv[1:4]
    count, limit, max_ms, max_rss = map(int, sys.argv[4:8])
    connection = sqlite3.connect(database)
    connection.enable_load_extension(True)
    connection.load_extension(extension)
    connection.enable_load_extension(False)
    connection.execute("SELECT vexfs_init()")
    connection.execute("SELECT vexfs_workspace_create(?)", (workspace,))
    workspace_id = connection.execute(
        "SELECT id FROM _vexfs_workspaces WHERE name=?", (workspace,)).fetchone()[0]
    with connection:
        connection.executemany(
            "INSERT INTO _vexfs_commits("
            "id,workspace_id,parent_commit,message,path,actor,created_at) "
            "VALUES(?,?,?,?,?,?,?)",
            ((commit, workspace_id, commit - 1, "eval commit",
              f"/src/file-{commit}.txt", "local", 1700000000000 + commit)
             for commit in range(2, count + 2)),
        )
        connection.execute(
            "UPDATE _vexfs_workspaces SET head_commit=? WHERE id=?",
            (count + 1, workspace_id),
        )
        connection.execute(
            "INSERT INTO _vexfs_snapshots(workspace_id,name,commit_id) VALUES(?,?,?)",
            (workspace_id, "head-snapshot", count + 1),
        )

    def query(before: int):
        started = time.monotonic_ns()
        payload = connection.execute(
            "SELECT vexfs_workspace_log(?,?,?)", (workspace, limit, before)).fetchone()[0]
        elapsed_ms = (time.monotonic_ns() - started) // 1_000_000
        return elapsed_ms, json.loads(payload)

    head_ms, head = query(0)
    tail_cursor = limit + 2
    tail_ms, tail = query(tail_cursor)
    assert len(head["entries"]) == limit
    assert head["entries"][0]["commit"] == count + 1
    assert head["entries"][0]["snapshots"] == ["head-snapshot"]
    assert head["next_before"] == count + 2 - limit
    assert tail["entries"] and all(row["commit"] < tail_cursor for row in tail["entries"])
    if head_ms > max_ms or tail_ms > max_ms:
        raise RuntimeError(f"SQLite workspace log timeout: head={head_ms}ms tail={tail_ms}ms")
    rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    if sys.platform != "darwin":
        rss *= 1024
    if rss > max_rss:
        raise RuntimeError(f"SQLite workspace log RSS {rss} exceeds {max_rss}")
    print(f"{head_ms}|{tail_ms}|{rss}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
