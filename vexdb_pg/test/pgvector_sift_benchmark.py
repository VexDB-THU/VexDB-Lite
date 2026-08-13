#!/usr/bin/env python3
"""Compare VexDB plain graph with pgvector HNSW on a SIFT subset.

The script expects both extensions to be loaded in the target PostgreSQL
database.  It uses the first N training vectors and the first Q test vectors
from the ann-benchmarks SIFT file, computes exact top-10 truth for that subset,
and measures build time, WAL bytes, index size, recall and single-client query
latency.

Example:
    PYTHONPATH=/tmp/vexdb-bench-py python3 \
      vexdb_pg/test/pgvector_sift_benchmark.py \
      --dataset /tmp/sift-128-euclidean.hdf5 \
      --host /tmp/vexdb-pgbench/socket --port 56534
"""

from __future__ import annotations

import argparse
import csv
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
import tempfile
import time

import h5py
import numpy as np


def run(command: list[str], *, env: dict[str, str] | None = None,
        stdin: str | None = None) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        input=stdin,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
    )
    if result.returncode != 0:
        if result.stdout:
            sys.stderr.write(result.stdout)
        if result.stderr:
            sys.stderr.write(result.stderr)
        raise RuntimeError("command failed: " + " ".join(command))
    return result


class Benchmark:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.psql_base = [
            args.psql, "-X", "-v", "ON_ERROR_STOP=1", "-h", args.host,
            "-p", str(args.port), "-d", args.database,
        ]
        self.pgbench_base = [
            args.pgbench, "-n", "-h", args.host, "-p", str(args.port),
            "-c", "1", "-j", "1", args.database,
        ]
        self.results: dict[str, object] = {
            "dataset": str(Path(args.dataset).resolve()),
            "rows": args.rows,
            "queries": args.queries,
            "dimensions": 128,
            "top_k": 10,
            "m": args.m,
            "ef_construction": args.ef_construction,
            "ef_search": args.ef_search,
            "vex_quantizer": args.vex_quantizer,
            "vex_memory_mode": args.vex_memory_mode,
            "pq_m": args.pq_m,
            "query_runs": args.query_runs,
            "transactions_per_run": args.transactions,
            "engines": {},
        }

    def psql(self, sql: str, *, tuples: bool = False) -> str:
        command = list(self.psql_base)
        if tuples:
            command += ["-q", "-t", "-A", "-F", "|"]
        result = run(command + ["-c", sql])
        return result.stdout.strip()

    def load_dataset(self) -> None:
        print("Loading SIFT data and computing exact truth", flush=True)
        with h5py.File(self.args.dataset, "r") as source:
            train = np.asarray(source["train"][:self.args.rows], dtype=np.float32)
            queries = np.asarray(source["test"][:self.args.queries], dtype=np.float32)
        if train.shape[1] != 128 or queries.shape[1] != 128:
            raise RuntimeError("expected 128-dimensional SIFT vectors")

        train_norm = np.einsum("ij,ij->i", train, train)
        truth: list[tuple[int, int]] = []
        for begin in range(0, len(queries), 10):
            batch = queries[begin:begin + 10]
            query_norm = np.einsum("ij,ij->i", batch, batch)[:, None]
            distances = query_norm + train_norm[None, :] - 2.0 * batch @ train.T
            nearest = np.argpartition(distances, 9, axis=1)[:, :10]
            for row, ids in enumerate(nearest):
                ordered = ids[np.lexsort((ids, distances[row, ids]))]
                truth.extend((begin + row + 1, int(value) + 1) for value in ordered)

        self.psql("""
            DROP TABLE IF EXISTS sift_bench_truth;
            DROP TABLE IF EXISTS sift_bench_queries;
            DROP TABLE IF EXISTS sift_bench_pgvector;
            DROP TABLE IF EXISTS sift_bench_vex;
            DROP TABLE IF EXISTS sift_bench_source;
            CREATE UNLOGGED TABLE sift_bench_source(id int PRIMARY KEY, raw real[] NOT NULL);
        """)
        self.copy_vectors("sift_bench_source", train)
        self.psql("""
            CREATE TABLE sift_bench_vex AS
              SELECT id, raw::floatvector(128) AS v FROM sift_bench_source;
            ALTER TABLE sift_bench_vex ADD PRIMARY KEY(id);
            CREATE TABLE sift_bench_pgvector AS
              SELECT id, raw::vector(128) AS v FROM sift_bench_source;
            ALTER TABLE sift_bench_pgvector ADD PRIMARY KEY(id);
            CREATE TABLE sift_bench_queries(qid int PRIMARY KEY, raw real[] NOT NULL);
            CREATE TABLE sift_bench_truth(qid int NOT NULL, id int NOT NULL,
                                          PRIMARY KEY(qid, id));
        """)
        self.copy_vectors("sift_bench_queries", queries, id_name="qid")
        self.copy_truth(truth)
        self.psql("""
            ALTER TABLE sift_bench_queries ADD COLUMN fv floatvector(128);
            ALTER TABLE sift_bench_queries ADD COLUMN pv vector(128);
            UPDATE sift_bench_queries SET fv=raw::floatvector(128), pv=raw::vector(128);
            ALTER TABLE sift_bench_queries ALTER COLUMN fv SET NOT NULL;
            ALTER TABLE sift_bench_queries ALTER COLUMN pv SET NOT NULL;
            ANALYZE sift_bench_vex;
            ANALYZE sift_bench_pgvector;
            ANALYZE sift_bench_queries;
        """)

    def copy_vectors(self, table: str, values: np.ndarray,
                     *, id_name: str = "id") -> None:
        command = self.psql_base + [
            "-c", f"COPY {table}({id_name},raw) FROM STDIN WITH (FORMAT csv)"
        ]
        process = subprocess.Popen(
            command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True,
        )
        assert process.stdin is not None
        writer = csv.writer(process.stdin, lineterminator="\n")
        for row_id, vector in enumerate(values, 1):
            array = "{" + ",".join(format(float(item), ".8g") for item in vector) + "}"
            writer.writerow((row_id, array))
        process.stdin.close()
        stdout = process.stdout.read() if process.stdout else ""
        stderr = process.stderr.read() if process.stderr else ""
        code = process.wait()
        if code != 0:
            raise RuntimeError(f"COPY {table} failed: {stdout}\n{stderr}")

    def copy_truth(self, truth: list[tuple[int, int]]) -> None:
        command = self.psql_base + [
            "-c", "COPY sift_bench_truth(qid,id) FROM STDIN WITH (FORMAT csv)"
        ]
        process = subprocess.Popen(
            command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True,
        )
        assert process.stdin is not None
        writer = csv.writer(process.stdin, lineterminator="\n")
        writer.writerows(truth)
        process.stdin.close()
        stdout = process.stdout.read() if process.stdout else ""
        stderr = process.stderr.read() if process.stderr else ""
        code = process.wait()
        if code != 0:
            raise RuntimeError(f"COPY truth failed: {stdout}\n{stderr}")

    def build_index(self, engine: str) -> dict[str, object]:
        if engine == "vexdb":
            index = "sift_bench_vex_idx"
            table = "sift_bench_vex"
            work_mem = self.args.vex_work_mem
            options = [
                f"m={self.args.m}",
                f"ef_construction={self.args.ef_construction}",
                "parallel_workers=0",
            ]
            if self.args.vex_quantizer == "pq":
                options.extend(("quantizer=pq", f"pq_m={self.args.pq_m}",
                                f"memory_mode={self.args.vex_memory_mode}"))
            elif self.args.vex_quantizer == "rabitq":
                options.extend(("quantizer=rabitq",
                                f"memory_mode={self.args.vex_memory_mode}"))
            create = (
                f"CREATE INDEX {index} ON {table} USING vexdb_graph "
                f"(v floatvector_l2_ops) WITH ({','.join(options)})"
            )
        else:
            index = "sift_bench_pgvector_idx"
            table = "sift_bench_pgvector"
            work_mem = self.args.pgvector_work_mem
            create = (
                f"CREATE INDEX {index} ON {table} USING hnsw "
                f"(v vector_l2_ops) WITH (m={self.args.m}, "
                f"ef_construction={self.args.ef_construction})"
            )
        self.psql(f"DROP INDEX IF EXISTS {index}")
        before = self.psql("SELECT pg_current_wal_insert_lsn()", tuples=True).splitlines()[-1]
        started = time.monotonic()
        self.psql(
            f"SET max_parallel_maintenance_workers=0; "
            f"SET maintenance_work_mem='{work_mem}'; {create}"
        )
        build_seconds = time.monotonic() - started
        row = self.psql(
            f"SELECT pg_relation_size('{index}'), "
            f"pg_relation_size('{index}','vm'), pg_total_relation_size('{index}'), "
            f"pg_wal_lsn_diff(pg_current_wal_insert_lsn(),'{before}')::bigint",
            tuples=True,
        ).splitlines()[-1]
        main_bytes, vector_fork_bytes, total_bytes, wal_bytes = map(int, row.split("|"))
        plan = self.psql(
            ("SET enable_seqscan=off; EXPLAIN (COSTS OFF) SELECT id FROM "
             f"{table} ORDER BY v <-> (SELECT "
             f"{'fv' if engine == 'vexdb' else 'pv'} FROM sift_bench_queries "
             "WHERE qid=1) LIMIT 10")
        )
        if index not in plan:
            raise RuntimeError(f"planner did not use {index}:\n{plan}")
        return {
            "index": index,
            "build_seconds": build_seconds,
            "main_fork_bytes": main_bytes,
            "vector_fork_bytes": vector_fork_bytes,
            "total_index_bytes": total_bytes,
            "wal_bytes": wal_bytes,
            "points_per_second": self.args.rows / build_seconds,
            "operating_points": [],
        }

    def recall(self, engine: str, ef: int) -> float:
        table = "sift_bench_vex" if engine == "vexdb" else "sift_bench_pgvector"
        vector = "fv" if engine == "vexdb" else "pv"
        setting = "vexdb.ef_search" if engine == "vexdb" else "hnsw.ef_search"
        value = self.psql(f"""
            BEGIN;
            SET LOCAL enable_seqscan=off;
            SET LOCAL max_parallel_workers_per_gather=0;
            SET LOCAL {setting}={ef};
            CREATE TEMP TABLE sift_bench_ann ON COMMIT DROP AS
            SELECT q.qid, ann.id
            FROM sift_bench_queries q
            CROSS JOIN LATERAL (
              SELECT d.id FROM {table} d ORDER BY d.v <-> q.{vector} LIMIT 10
            ) ann;
            SELECT count(t.id)::double precision / {self.args.queries * 10}.0
            FROM sift_bench_ann a LEFT JOIN sift_bench_truth t USING(qid,id);
            ROLLBACK;
        """, tuples=True)
        numbers = [line for line in value.splitlines() if re.fullmatch(r"[0-9.]+", line)]
        if not numbers:
            raise RuntimeError("could not parse recall output: " + value)
        return float(numbers[-1])

    def latency(self, engine: str, ef: int) -> dict[str, float | list[float]]:
        table = "sift_bench_vex" if engine == "vexdb" else "sift_bench_pgvector"
        vector = "fv" if engine == "vexdb" else "pv"
        setting = "vexdb.ef_search" if engine == "vexdb" else "hnsw.ef_search"
        script = (
            f"\\set qid random(1, {self.args.queries})\n"
            f"SELECT id FROM {table} ORDER BY v <-> "
            f"(SELECT {vector} FROM sift_bench_queries WHERE qid=:qid) LIMIT 10;\n"
        )
        with tempfile.NamedTemporaryFile("w", suffix=".sql", delete=False) as target:
            target.write(script)
            path = target.name
        env = os.environ.copy()
        env["PGOPTIONS"] = (
            f"-c {setting}={ef} -c enable_seqscan=off "
            "-c max_parallel_workers_per_gather=0"
        )
        try:
            run(self.pgbench_base + ["-t", "100", "-f", path], env=env)
            latencies: list[float] = []
            tps_values: list[float] = []
            for _ in range(self.args.query_runs):
                output = run(
                    self.pgbench_base + ["-t", str(self.args.transactions), "-f", path],
                    env=env,
                ).stdout
                latency_match = re.search(r"latency average = ([0-9.]+) ms", output)
                tps_match = re.search(r"tps = ([0-9.]+) \(without initial connection time\)", output)
                if not latency_match or not tps_match:
                    raise RuntimeError("could not parse pgbench output:\n" + output)
                latencies.append(float(latency_match.group(1)))
                tps_values.append(float(tps_match.group(1)))
            return {
                "latency_ms_median": statistics.median(latencies),
                "qps_median": statistics.median(tps_values),
                "latency_ms_runs": latencies,
                "qps_runs": tps_values,
            }
        finally:
            Path(path).unlink(missing_ok=True)

    def execute(self) -> dict[str, object]:
        self.load_dataset()
        engines = self.results["engines"]
        assert isinstance(engines, dict)
        for engine in ("vexdb", "pgvector"):
            print(f"Building {engine} index", flush=True)
            engines[engine] = self.build_index(engine)

        # Alternate the engine measured first at each operating point.  This
        # avoids giving either extension all of the cooler or warmer runs.
        for point_index, ef in enumerate(self.args.ef_search):
            order = ("vexdb", "pgvector") if point_index % 2 == 0 else (
                "pgvector", "vexdb")
            for engine in order:
                print(f"Measuring {engine}, ef_search={ef}", flush=True)
                point: dict[str, object] = {"ef_search": ef, "recall_at_10": self.recall(engine, ef)}
                point.update(self.latency(engine, ef))
                engine_result = engines[engine]
                assert isinstance(engine_result, dict)
                points = engine_result["operating_points"]
                assert isinstance(points, list)
                points.append(point)
        return self.results


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--database", default="postgres")
    parser.add_argument("--psql", default="psql")
    parser.add_argument("--pgbench", default="pgbench")
    parser.add_argument("--rows", type=int, default=100000)
    parser.add_argument("--queries", type=int, default=100)
    parser.add_argument("--m", type=int, default=16)
    parser.add_argument("--ef-construction", type=int, default=64)
    parser.add_argument("--ef-search", type=int, nargs="+", default=[40, 64, 100, 200])
    parser.add_argument("--query-runs", type=int, default=3)
    parser.add_argument("--transactions", type=int, default=1000)
    parser.add_argument("--vex-work-mem", default="2GB")
    parser.add_argument("--vex-quantizer", choices=("plain", "pq", "rabitq"),
                        default="plain")
    parser.add_argument("--vex-memory-mode", choices=("full", "compact"),
                        default="compact")
    parser.add_argument("--pq-m", type=int, default=16)
    parser.add_argument("--pgvector-work-mem", default="2GB")
    parser.add_argument("--output")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    result = Benchmark(args).execute()
    rendered = json.dumps(result, indent=2, sort_keys=True)
    print(rendered)
    if args.output:
        output = Path(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(rendered + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
