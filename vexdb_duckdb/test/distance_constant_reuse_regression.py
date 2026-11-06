"""Real-SDK scalar regression; execute each extension in a separate process.

Writes all returned rows, not just checksums. Compare two completed outputs to
verify that constant-array reuse keeps ordinary SQL values and NULLs intact.
"""
import argparse
import hashlib
import json
from pathlib import Path

from run_filtered_spec_with_sdk import Engine


def execute(library, extension, out):
    if out.exists():
        raise RuntimeError("refusing to overwrite prior results")
    out.mkdir(parents=True)
    engine = Engine(library)
    records = []
    try:
        engine.execute("LOAD '" + str(extension.resolve()).replace("'", "''") + "'")
        engine.execute("SET threads=1")
        engine.execute("SET vexdb_brute_force_threshold=1000000")
        for dim in (1, 3, 16, 128, 1536):
            table = f"input_{dim}"
            a = f"list_transform(range({dim}),j->sin((i+.31)*(j+.7)*.013)::FLOAT)"
            b = f"list_transform(range({dim}),j->cos((i+.71)*(j+.2)*.017)::FLOAT)::FLOAT[{dim}]"
            engine.execute(f"CREATE TABLE {table} AS SELECT i::INTEGER id, (CASE WHEN i%17=0 THEN NULL ELSE {a} END)::FLOAT[{dim}] v, {b} w FROM range(4099)t(i)")
            literal = "[" + ",".join(str((j % 7 + 1) * .13) for j in range(dim)) + f"]::FLOAT[{dim}]"
            vectors = [("right", "v", literal), ("left", literal, "v"),
                       ("columns", "v", "w"), ("constants", literal, literal),
                       ("parent-null", "v", f"NULL::FLOAT[{dim}]")]
            for shape, left, right in vectors:
                for filt in ("TRUE", "id%3 IN (0,2)"):
                    sql = (f"SELECT id,l2_distance({left},{right}),inner_product({left},{right}),"
                           f"cosine_distance({left},{right}),({left}<~>{right}),array_distance({left},{right}) "
                           f"FROM {table} WHERE {filt} ORDER BY id")
                    rows = engine.execute(sql)
                    expected_ids = [i for i in range(4099) if filt == "TRUE" or i % 3 in (0, 2)]
                    if [int(r[0]) for r in rows] != expected_ids:
                        raise RuntimeError("row selection changed")
                    for row in rows:
                        if shape == "parent-null" or (shape != "constants" and int(row[0]) % 17 == 0):
                            if any(x is not None for x in row[1:]):
                                raise RuntimeError("parent NULL failed to propagate")
                        elif row[1] != row[5]:
                            raise RuntimeError("L2 alias mismatch")
                    name = f"dim{dim}-{shape}-{'all' if filt=='TRUE' else 'selected'}"
                    target = out / (name + ".json")
                    payload = json.dumps(rows, separators=(",", ":")).encode()
                    target.write_bytes(payload)
                    records.append({"name": name, "rows": len(rows), "sha256": hashlib.sha256(payload).hexdigest()})
            # Constant columns from table storage, distinct from literal folding.
            engine.execute(f"CREATE TABLE constant_{dim} AS SELECT i id,{literal} v FROM range(4099)t(i)")
            sql = f"SELECT id,l2_distance(v,{literal}),inner_product({literal},v) FROM constant_{dim} WHERE id%5!=1 ORDER BY id"
            rows = engine.execute(sql)
            if len(rows) != sum(i % 5 != 1 for i in range(4099)) or any(float(row[1]) != 0 for row in rows):
                raise RuntimeError("stored constant vector results changed")
            target = out / f"dim{dim}-stored-constant.json"
            payload = json.dumps(rows, separators=(",", ":")).encode()
            target.write_bytes(payload)
            records.append({"name": target.stem, "rows": len(rows), "sha256": hashlib.sha256(payload).hexdigest()})
        errors = []
        for sql in ("SELECT l2_distance([1,2]::FLOAT[2],[1,2,3]::FLOAT[3])",
                    "SELECT cosine_distance([1,2,3]::FLOAT[3],[1,2]::FLOAT[2])"):
            try:
                engine.execute(sql)
                raise AssertionError("dimension error was accepted")
            except RuntimeError as error:
                errors.append({"sql": sql, "error": str(error)})
        summary = {"cases": records, "errors": errors, "version": engine.lib.duckdb_library_version().decode()}
        (out / "summary.json").write_text(json.dumps(summary, indent=2))
        print("PASS", len(records), "SQL cases,", sum(r["rows"] for r in records), "returned rows,", len(errors), "dimension rejections")
    finally:
        engine.close()


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--library", type=Path, required=True)
    p.add_argument("--extension", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    args = p.parse_args()
    execute(args.library, args.extension, args.out)
