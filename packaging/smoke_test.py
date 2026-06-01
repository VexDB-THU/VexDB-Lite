from __future__ import annotations

import math

import vexdb_lite


def _assert_close(actual: float, expected: float, *, tolerance: float = 1e-5) -> None:
    if not math.isclose(actual, expected, rel_tol=tolerance, abs_tol=tolerance):
        raise AssertionError(f"expected {expected}, got {actual}")


def main() -> None:
    con = vexdb_lite.connect()

    version = con.execute("SELECT vex_version()").fetchone()[0]
    if "vexdb_vector duck extension" not in version:
        raise AssertionError(f"unexpected vex_version(): {version!r}")

    _assert_close(
        con.execute("SELECT l2_distance([1,0,0]::FLOAT[3], [0,1,0]::FLOAT[3])").fetchone()[0],
        math.sqrt(2),
    )
    _assert_close(
        con.execute("SELECT cosine_distance([1,0,0]::FLOAT[3], [2,0,0]::FLOAT[3])").fetchone()[0],
        0.0,
    )

    con.execute("CREATE TABLE items (id INTEGER, category VARCHAR, vec FLOAT[3])")
    con.execute(
        """
        INSERT INTO items VALUES
            (1, 'book', [1.0, 0.0, 0.0]::FLOAT[3]),
            (2, 'book', [0.9, 0.1, 0.0]::FLOAT[3]),
            (3, 'image', [0.0, 1.0, 0.0]::FLOAT[3]),
            (4, 'image', [0.0, 0.0, 1.0]::FLOAT[3])
        """
    )
    con.execute("CREATE INDEX idx_items_vec ON items USING GRAPH_INDEX (vec, category) WITH (m = 8)")
    con.execute("SET vex_brute_force_threshold = 0")

    plan = con.execute(
        """
        EXPLAIN SELECT id
        FROM items
        WHERE category = 'book'
        ORDER BY l2_distance(vec, [1.0, 0.0, 0.0]::FLOAT[3])
        LIMIT 2
        """
    ).fetchall()
    if "VEX_INDEX_SCAN" not in "\n".join(str(row) for row in plan):
        raise AssertionError(f"expected VEX_INDEX_SCAN in plan, got: {plan!r}")

    rows = con.execute(
        """
        SELECT id
        FROM items
        WHERE category = 'book'
        ORDER BY l2_distance(vec, [1.0, 0.0, 0.0]::FLOAT[3])
        LIMIT 2
        """
    ).fetchall()
    if rows != [(1,), (2,)]:
        raise AssertionError(f"unexpected nearest-neighbor order: {rows!r}")

    print("VexDB-Lite Python smoke test PASSED")


if __name__ == "__main__":
    main()
