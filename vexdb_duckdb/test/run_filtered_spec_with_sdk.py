"""Run the filtered-search YAML assertions using the same native DuckDB SDK.

This deliberately supports statement/query/expect steps only, and refuses
unsupported directives. It does not replace the full Catch2 spec runner.
"""
import argparse
import ctypes as c
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/spec/_lib"))
import render
import yaml


# duckdb_result layout and all function signatures are from the SDK duckdb.h.
class Result(c.Structure):
    _fields_ = [("deprecated_column_count", c.c_uint64), ("deprecated_row_count", c.c_uint64),
                ("deprecated_rows_changed", c.c_uint64), ("deprecated_columns", c.c_void_p),
                ("deprecated_error_message", c.c_void_p), ("internal_data", c.c_void_p)]


class Engine:
    def __init__(self, library):
        # The C++ extension resolves host DuckDB symbols at dlopen time.
        # ctypes defaults to RTLD_LOCAL, which hides those symbols on macOS.
        self.lib = c.CDLL(str(library.resolve()), mode=c.RTLD_GLOBAL)
        signatures = {
            "duckdb_create_config": ([c.POINTER(c.c_void_p)], c.c_int),
            "duckdb_set_config": ([c.c_void_p, c.c_char_p, c.c_char_p], c.c_int),
            "duckdb_destroy_config": ([c.POINTER(c.c_void_p)], None),
            "duckdb_open_ext": ([c.c_char_p, c.POINTER(c.c_void_p), c.c_void_p, c.POINTER(c.c_void_p)], c.c_int),
            "duckdb_connect": ([c.c_void_p, c.POINTER(c.c_void_p)], c.c_int),
            "duckdb_disconnect": ([c.POINTER(c.c_void_p)], None),
            "duckdb_close": ([c.POINTER(c.c_void_p)], None),
            "duckdb_query": ([c.c_void_p, c.c_char_p, c.POINTER(Result)], c.c_int),
            "duckdb_destroy_result": ([c.POINTER(Result)], None),
            "duckdb_result_error": ([c.POINTER(Result)], c.c_char_p),
            "duckdb_row_count": ([c.POINTER(Result)], c.c_uint64),
            "duckdb_column_count": ([c.POINTER(Result)], c.c_uint64),
            "duckdb_value_is_null": ([c.POINTER(Result), c.c_uint64, c.c_uint64], c.c_bool),
            "duckdb_value_varchar": ([c.POINTER(Result), c.c_uint64, c.c_uint64], c.c_void_p),
            "duckdb_free": ([c.c_void_p], None),
            "duckdb_library_version": ([], c.c_char_p),
        }
        for name, (args, restype) in signatures.items():
            fn = getattr(self.lib, name)
            fn.argtypes, fn.restype = args, restype
        self.database, self.connection, config, error = (c.c_void_p() for _ in range(4))
        assert self.lib.duckdb_create_config(c.byref(config)) == 0
        try:
            assert self.lib.duckdb_set_config(config, b"allow_unsigned_extensions", b"true") == 0
            if self.lib.duckdb_open_ext(None, c.byref(self.database), config, c.byref(error)):
                message = c.string_at(error).decode() if error.value else "open failed"
                self.lib.duckdb_free(error)
                raise RuntimeError(message)
        finally:
            self.lib.duckdb_destroy_config(c.byref(config))
        if self.lib.duckdb_connect(self.database, c.byref(self.connection)):
            self.lib.duckdb_close(c.byref(self.database))
            raise RuntimeError("connect failed")

    def execute(self, sql):
        result = Result()
        try:
            if self.lib.duckdb_query(self.connection, sql.encode(), c.byref(result)):
                raise RuntimeError(sql + "\n" + self.lib.duckdb_result_error(c.byref(result)).decode())
            rows = []
            for row in range(self.lib.duckdb_row_count(c.byref(result))):
                values = []
                for col in range(self.lib.duckdb_column_count(c.byref(result))):
                    if self.lib.duckdb_value_is_null(c.byref(result), col, row):
                        values.append(None)
                        continue
                    value = self.lib.duckdb_value_varchar(c.byref(result), col, row)
                    try:
                        values.append(c.string_at(value).decode())
                    finally:
                        self.lib.duckdb_free(value)
                rows.append(values)
            return rows
        finally:
            self.lib.duckdb_destroy_result(c.byref(result))

    def close(self):
        self.lib.duckdb_disconnect(c.byref(self.connection))
        self.lib.duckdb_close(c.byref(self.database))


def equal(actual, expected):
    if isinstance(expected, str) and expected.startswith(("<REGEX>:", "<!REGEX>:")):
        match = bool(re.fullmatch(expected.partition(":")[2], actual or "", re.S))
        return not match if expected.startswith("<!REGEX>:") else match
    return actual == (None if expected is None else str(expected))


def run(library, extension, spec_path, out):
    if out.exists():
        raise RuntimeError("refusing to overwrite evidence")
    spec = yaml.safe_load(spec_path.read_text())
    dialect = render.load_dialects()["duckdb"]
    rendered = render.emit_duckdb(spec, dialect)
    engine = Engine(library)
    checks = []
    try:
        engine.execute("LOAD '" + str(extension.resolve()).replace("'", "''") + "'")
        engine.execute("SET threads=1")
        for number, step in enumerate(spec["steps"]):
            if set(step) - {"statement", "query", "expect", "_field_types"}:
                raise RuntimeError("unsupported spec step")
            sql = render.render_template(step.get("query", step.get("statement")), dialect)
            rows = engine.execute(sql)
            if "query" in step:
                expected = step["expect"]
                if len(rows) != len(expected) or any(len(a) != len(b) or not all(equal(x, y) for x, y in zip(a, b)) for a, b in zip(rows, expected)):
                    raise RuntimeError(f"step {number} mismatch: {rows!r} != {expected!r}")
                checks.append({"step": number, "query": sql, "rows": len(rows), "passed": True})
        # Functional metric coverage, kept separate from L2 timing evidence.
        engine.execute("CREATE TABLE metric_source AS SELECT i::INTEGER id, [(i+1)::FLOAT,sin(i*.17)::FLOAT,cos(i*.11)::FLOAT,1::FLOAT]::FLOAT[4] vec FROM range(512)t(i)")
        for metric, function, order in (("cosine", "cosine_distance", "ASC"), ("ip", "inner_product", "DESC")):
            table = "metric_" + metric
            engine.execute("CREATE TABLE " + table + " AS SELECT * FROM metric_source")
            engine.execute(f"CREATE INDEX {table}_idx ON {table} USING GRAPH_INDEX(vec) WITH(metric='{metric}',quantizer='pq',pq_m=1,threads=1)")
            for mode in ("off", "pq_only", "pq_graph"):
                engine.execute(f"SET vexdb_pq_search_mode='{mode}'")
                query = f"SELECT id FROM {table} WHERE id>=490 ORDER BY {function}(vec,[1,.3,.5,.7]::FLOAT[4]) {order} LIMIT 10"
                engine.execute("SET vexdb_brute_force_threshold=1000000")
                reference = engine.execute(query)
                engine.execute("SET vexdb_brute_force_threshold=0")
                plan = engine.execute("EXPLAIN " + query)
                assert not any("VEXDB_INDEX_SCAN" in (cell or "") for row in plan for cell in row)
                actual = engine.execute(query)
                assert actual == reference and len(actual) == 10 and all(int(r[0]) >= 490 for r in actual)
                checks.append({"metric": metric, "mode": mode, "passed": True})
        out.mkdir(parents=True)
        (out / "rendered.test").write_text(rendered)
        (out / "result.json").write_text(json.dumps({"version": engine.lib.duckdb_library_version().decode(),
              "spec": str(spec_path), "scope": "Targeted YAML through native C API, plus filtered cosine/IP; not the full Catch2 suite.",
              "checks": checks, "passed": True}, indent=2))
        print("PASS", len(checks), "native assertions; YAML render and filtered cosine/IP in all three modes")
    finally:
        engine.close()


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--library", type=Path, required=True)
    p.add_argument("--extension", type=Path, required=True)
    p.add_argument("--spec", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    args = p.parse_args()
    run(args.library, args.extension, args.spec, args.out)
