#!/usr/bin/env python3
"""Open, mutate, migrate, and reopen the fixed SQLite graph v3 fixture."""

from __future__ import annotations

import gzip
import json
import shutil
import sqlite3
import struct
import sys
import tempfile
from pathlib import Path


def load(con: sqlite3.Connection, extension: Path) -> None:
    con.enable_load_extension(True)
    con.execute("SELECT load_extension(?, 'sqlite3_vexdblite_init')", (str(extension),))


def query(con: sqlite3.Connection, table: str, values: list[float], k: int) -> list[int]:
    rows = con.execute(
        f"SELECT rowid FROM {table} WHERE v MATCH ? AND k=?",
        (json.dumps(values, separators=(",", ":")), k),
    ).fetchall()
    return [int(row[0]) for row in rows]


def graph_version(con: sqlite3.Connection, table: str) -> int:
    row = con.execute(
        f"SELECT data FROM {table}_graph WHERE kind=0 AND seg=0"
    ).fetchone()
    if row is None or len(row[0]) < 8:
        raise AssertionError(f"{table}: missing graph meta")
    return struct.unpack_from("<I", row[0], 4)[0]


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: legacy_fixture_test.py EXTENSION FIXTURE_GZ", file=sys.stderr)
        return 2
    extension = Path(sys.argv[1]).resolve()
    fixture = Path(sys.argv[2]).resolve()

    with tempfile.TemporaryDirectory(prefix="vexdb-sqlite-legacy-") as temp_dir:
        db_path = Path(temp_dir) / "legacy_v3.db"
        with gzip.open(fixture, "rb") as source, db_path.open("wb") as target:
            shutil.copyfileobj(source, target)

        con = sqlite3.connect(db_path)
        load(con, extension)
        for table in ("legacy_pq", "legacy_rq"):
            if graph_version(con, table) != 3:
                raise AssertionError(f"{table}: fixture is not graph v3")
            ids = query(con, table, [0.0, 1.0, 0.0, 1.0, 0.0, 1.0, 0.0, 1.0], 64)
            if len(ids) != 64 or len(set(ids)) != 64:
                raise AssertionError(f"{table}: legacy search returned duplicate/missing rows")

        with con:
            con.execute(
                "UPDATE legacy_pq SET v=? WHERE rowid=1",
                (json.dumps([100.0] * 8),),
            )
            con.execute(
                "UPDATE legacy_rq SET v=? WHERE rowid=2",
                (json.dumps([-100.0] * 8),),
            )
        # The first current-version write migrates v3 whole metadata to v4 segments.
        for table in ("legacy_pq", "legacy_rq"):
            if graph_version(con, table) != 4:
                raise AssertionError(f"{table}: graph v3 was not migrated to v4")
        con.close()

        con = sqlite3.connect(db_path)
        load(con, extension)
        for table in ("legacy_pq", "legacy_rq"):
            ids = query(con, table, [0.0, 1.0, 0.0, 1.0, 0.0, 1.0, 0.0, 1.0], 64)
            if len(ids) != 64 or len(set(ids)) != 64:
                raise AssertionError(f"{table}: migrated graph failed after reopen")
        con.close()

    print("SQLite legacy graph v3 PQ/RaBitQ fixture: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
