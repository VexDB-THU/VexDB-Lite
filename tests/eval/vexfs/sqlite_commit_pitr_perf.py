#!/usr/bin/env python3
import json
import os
import resource
import sqlite3
import sys
import time


def elapsed_ms(started: int) -> int:
    return (time.monotonic_ns() - started) // 1_000_000


def main() -> int:
    if len(sys.argv) != 9:
        raise SystemExit(
            "usage: sqlite_commit_pitr_perf.py EXTENSION DB WORKSPACE FILES "
            "LIMIT MAX_QUERY_MS MAX_SNAPSHOT_MS MAX_RSS")
    extension, database, workspace = sys.argv[1:4]
    files, limit, max_query_ms, max_snapshot_ms, max_rss = map(int, sys.argv[4:9])

    connection = sqlite3.connect(database)
    connection.enable_load_extension(True)
    connection.load_extension(extension)
    connection.enable_load_extension(False)
    connection.execute("PRAGMA journal_mode=WAL")
    connection.execute("SELECT vexfs_init()")
    connection.execute("SELECT vexfs_workspace_create(?)", (workspace,))
    connection.execute("SELECT vexfs_mkdir(?, '/src')", (workspace,))
    workspace_id, root_inode, _ = connection.execute(
        "SELECT id,root_inode,head_commit FROM _vexfs_workspaces WHERE name=?",
        (workspace,),
    ).fetchone()
    src_inode = connection.execute(
        "SELECT inode_id FROM _vexfs_dentries "
        "WHERE workspace_id=? AND parent_inode=? AND name='src'",
        (workspace_id, root_inode),
    ).fetchone()[0]
    connection.execute(
        "SELECT vexfs_write(?, '/src/.empty-seed', ?)", (workspace, b""))
    seed_inode = connection.execute(
        "SELECT inode_id FROM _vexfs_dentries "
        "WHERE workspace_id=? AND parent_inode=? AND name='.empty-seed'",
        (workspace_id, src_inode),
    ).fetchone()[0]
    manifest_id, empty_checksum = connection.execute(
        "SELECT manifest_id,checksum FROM _vexfs_file_versions "
        "WHERE inode_id=? AND version_no=1",
        (seed_inode,),
    ).fetchone()
    connection.execute(
        "SELECT vexfs_remove(?, '/src/.empty-seed', 0)", (workspace,))
    baseline_commit = connection.execute(
        "SELECT head_commit FROM _vexfs_workspaces WHERE id=?", (workspace_id,)
    ).fetchone()[0]
    first_inode = connection.execute(
        "SELECT COALESCE(MAX(id),0)+1 FROM _vexfs_inodes"
    ).fetchone()[0]
    timestamp = int(time.time() * 1000)

    with connection:
        cursor = connection.execute(
            "INSERT INTO _vexfs_commits("
            "workspace_id,parent_commit,message,path,actor,created_at) "
            "VALUES(?,?,'eval bulk files','/src','local',?)",
            (workspace_id, baseline_commit, timestamp),
        )
        target_commit = cursor.lastrowid

        def rows():
            for index in range(files):
                yield first_inode + index, f"file-{index:06d}.txt"

        connection.executemany(
            "INSERT INTO _vexfs_inodes("
            "id,workspace_id,kind,mode,size,current_version,created_at,accessed_at,"
            "updated_at,changed_at) VALUES(?,?,'file',420,0,1,?,?,?,?)",
            ((inode, workspace_id, timestamp, timestamp, timestamp, timestamp)
             for inode, _ in rows()),
        )
        connection.executemany(
            "INSERT INTO _vexfs_dentries(workspace_id,parent_inode,name,inode_id) "
            "VALUES(?,?,?,?)",
            ((workspace_id, src_inode, name, inode) for inode, name in rows()),
        )
        connection.executemany(
            "INSERT INTO _vexfs_file_versions("
            "inode_id,version_no,commit_id,manifest_id,size,checksum,created_at) "
            "VALUES(?,1,?,?,0,?,?)",
            ((inode, target_commit, manifest_id, empty_checksum, timestamp)
             for inode, _ in rows()),
        )
        connection.executemany(
            "INSERT INTO _vexfs_inode_states("
            "workspace_id,inode_id,commit_id,kind,mode,owner_principal,uid,gid,size,"
            "current_version,created_at,accessed_at,updated_at,changed_at,deleted_at) "
            "VALUES(?,?,?,'file',420,'local',0,0,0,1,?,?,?,?,NULL)",
            ((workspace_id, inode, target_commit, timestamp, timestamp, timestamp, timestamp)
             for inode, _ in rows()),
        )
        connection.executemany(
            "INSERT INTO _vexfs_dentry_states("
            "workspace_id,parent_inode,name,commit_id,inode_id,deleted) "
            "VALUES(?,?,?,?,?,0)",
            ((workspace_id, src_inode, name, target_commit, inode)
             for inode, name in rows()),
        )
        connection.execute(
            "UPDATE _vexfs_workspaces SET head_commit=? WHERE id=?",
            (target_commit, workspace_id),
        )

    def page(sql: str, parameters: tuple):
        started = time.monotonic_ns()
        payload = connection.execute(sql, parameters).fetchone()[0]
        duration = elapsed_ms(started)
        return duration, json.loads(payload)

    show_head_ms, show_head = page(
        "SELECT vexfs_workspace_show_commit(?,?,?,?)",
        (workspace, target_commit, "", limit),
    )
    deep_after = f"/src/file-{files - limit - 1:06d}.txt"
    show_deep_ms, show_deep = page(
        "SELECT vexfs_workspace_show_commit(?,?,?,?)",
        (workspace, target_commit, deep_after, limit),
    )
    diff_head_ms, diff_head = page(
        "SELECT vexfs_workspace_diff_commits(?,?,?,?,?)",
        (workspace, baseline_commit, target_commit, "", limit),
    )
    diff_deep_ms, diff_deep = page(
        "SELECT vexfs_workspace_diff_commits(?,?,?,?,?)",
        (workspace, baseline_commit, target_commit, deep_after, limit),
    )
    snapshot_started = time.monotonic_ns()
    snapshot = json.loads(connection.execute(
        "SELECT vexfs_snapshot_create_at_time(?,?,?,?)",
        (workspace, "eval-by-time", "2099-01-01T00:00:00Z", "manual"),
    ).fetchone()[0])
    snapshot_ms = elapsed_ms(snapshot_started)

    assert show_head["commit"] == target_commit
    assert len(show_head["entries"]) == limit
    assert show_head["next_after"] is not None
    assert len(show_deep["entries"]) == limit
    assert show_deep["entries"][0]["path"] > deep_after
    assert show_deep["next_after"] is None
    assert len(diff_head["changes"]) == limit
    assert diff_head["next_after"] is not None
    assert len(diff_deep["changes"]) == limit
    assert diff_deep["changes"][0]["path"] > deep_after
    assert diff_deep["next_after"] is None
    assert snapshot["commit"] == target_commit

    query_values = [show_head_ms, show_deep_ms, diff_head_ms, diff_deep_ms]
    if max(query_values) > max_query_ms:
        raise RuntimeError(
            "SQLite commit PITR query timeout: " +
            ",".join(str(value) for value in query_values))
    if snapshot_ms > max_snapshot_ms:
        raise RuntimeError(
            f"SQLite historical snapshot timeout: {snapshot_ms}ms")
    rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    if sys.platform != "darwin":
        rss *= 1024
    if rss > max_rss:
        raise RuntimeError(f"SQLite commit PITR RSS {rss} exceeds {max_rss}")
    database_bytes = os.path.getsize(database)
    print("|".join(map(str, [
        show_head_ms, show_deep_ms, diff_head_ms, diff_deep_ms,
        snapshot_ms, rss, database_bytes, target_commit,
    ])))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
