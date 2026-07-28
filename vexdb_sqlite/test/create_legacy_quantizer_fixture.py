#!/usr/bin/env python3
"""Create a SQLite graph-format v3 PQ/RaBitQ compatibility fixture."""

from __future__ import annotations

import json
import math
import os
import sqlite3
import struct
import sys
from pathlib import Path


def vector(i: int) -> list[float]:
    return [
        math.sin(i * 0.11), math.cos(i * 0.13),
        math.sin(i * 0.17), math.cos(i * 0.19),
        math.sin(i * 0.23), math.cos(i * 0.29),
        math.sin(i * 0.31), math.cos(i * 0.37),
    ]


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: create_legacy_quantizer_fixture.py EXTENSION OUTPUT_DB", file=sys.stderr)
        return 2
    extension = Path(sys.argv[1]).resolve()
    output = Path(sys.argv[2]).resolve()
    output.unlink(missing_ok=True)

    con = sqlite3.connect(output)
    con.enable_load_extension(True)
    con.execute("SELECT load_extension(?, 'sqlite3_vexdblite_init')", (str(extension),))
    common = (
        "v FLOAT[8], metric=l2, m=16, ef_construction=128, ef_search=160, "
        "brute_force_threshold=0, memory_mode=compact"
    )
    con.execute(
        f"CREATE VIRTUAL TABLE legacy_pq USING GRAPH_INDEX({common}, quantizer=pq, pq_m=4)"
    )
    con.execute(
        f"CREATE VIRTUAL TABLE legacy_rq USING GRAPH_INDEX({common}, quantizer=rabitq)"
    )
    rows = [(i + 1, json.dumps(vector(i), separators=(",", ":"))) for i in range(512)]
    with con:
        con.executemany("INSERT INTO legacy_pq(rowid, v) VALUES (?, ?)", rows)
        con.executemany("INSERT INTO legacy_rq(rowid, v) VALUES (?, ?)", rows)

    query = json.dumps(vector(17), separators=(",", ":"))
    for table in ("legacy_pq", "legacy_rq"):
        result = con.execute(
            f"SELECT rowid FROM {table} WHERE v MATCH ? AND k=10", (query,)
        ).fetchall()
        if len(result) != 10:
            raise RuntimeError(f"{table}: graph build returned {len(result)} rows")

    # A post-build write makes xSync persist the already-built graph and codes.
    with con:
        extra = json.dumps(vector(9000), separators=(",", ":"))
        con.execute("INSERT INTO legacy_pq(rowid, v) VALUES (9001, ?)", (extra,))
        con.execute("INSERT INTO legacy_rq(rowid, v) VALUES (9001, ?)", (extra,))

    for table in ("legacy_pq", "legacy_rq"):
        meta = con.execute(
            f"SELECT data FROM {table}_graph WHERE kind=0 AND seg=0"
        ).fetchone()
        if meta is None or len(meta[0]) < 8:
            raise RuntimeError(f"{table}: graph meta is missing")
        version = struct.unpack_from("<I", meta[0], 4)[0]
        if version != 3:
            raise RuntimeError(f"{table}: expected graph v3, got v{version}")
    con.execute("PRAGMA wal_checkpoint(TRUNCATE)").fetchall()
    con.close()
    os.chmod(output, 0o644)
    print(f"legacy SQLite graph v3 fixture written: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
