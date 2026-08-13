#!/usr/bin/env python3
"""Measure PQ/RaBitQ recall as the PostgreSQL partition count grows."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import time
from typing import Any


def run(command: list[str], *, env: dict[str, str] | None = None) -> str:
    result = subprocess.run(
        command,
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
    return result.stdout.strip()


class PartitionRecallBenchmark:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.env = os.environ.copy()
        self.env["LC_ALL"] = "C"
        self.psql_base = [
            args.psql,
            "-X",
            "-q",
            "-t",
            "-A",
            "-F",
            "|",
            "-v",
            "ON_ERROR_STOP=1",
            "-h",
            args.host,
            "-p",
            str(args.port),
            "-d",
            args.database,
        ]
        self.results: dict[str, Any] = {
            "config": {
                "cases": args.case,
                "queries": args.queries,
                "dimensions": args.dimensions,
                "top_k": 10,
                "runs": args.runs,
                "ef_search": args.ef_search,
                "quantizers": args.quantizer,
                "m": args.m,
                "ef_construction": args.ef_construction,
                "pq_m": args.pq_m,
                "min_recall": args.min_recall,
            },
            "environment": {},
            "runs": [],
            "summary": [],
        }

    def psql(self, sql: str) -> str:
        return run(self.psql_base + ["-c", sql], env=self.env)

    @staticmethod
    def parse_cases(values: list[str]) -> list[tuple[int, list[int]]]:
        parsed: list[tuple[int, list[int]]] = []
        for value in values:
            row_text, separator, partition_text = value.partition(":")
            if not separator:
                raise ValueError("case must use ROWS:PARTITIONS, for example 100000:1,2,5,10")
            rows = int(row_text)
            partitions = [int(item) for item in partition_text.split(",")]
            if rows <= 0 or not partitions or any(item <= 0 for item in partitions):
                raise ValueError("case rows and partitions must be positive")
            if any(rows % item != 0 for item in partitions):
                raise ValueError("each partition count must divide rows exactly")
            if any(rows // item < 10000 for item in partitions):
                raise ValueError("every leaf needs at least 10000 rows to activate RaBitQ")
            parsed.append((rows, partitions))
        return parsed

    def create_vector_function(self) -> None:
        components = []
        for dimension in range(self.args.dimensions):
            components.append(
                "(sin(i * 0.013 + {j} * 0.17) + "
                "cos(i * 0.007 * ({jp1})) + "
                "((floor(i)::bigint % 97) * ({jp1})) * 0.00001)::real".format(
                    j=dimension,
                    jp1=dimension + 1,
                )
            )
        array_sql = ",\n".join(components)
        self.psql(
            "DROP TABLE IF EXISTS vex_part_eval_ann;"
            "DROP TABLE IF EXISTS vex_part_eval_truth;"
            "DROP TABLE IF EXISTS vex_part_eval_queries;"
            "DROP TABLE IF EXISTS vex_part_eval_data CASCADE;"
            "DROP TABLE IF EXISTS vex_part_eval_source;"
            "DROP FUNCTION IF EXISTS __vex_part_eval_vec(double precision);"
            "CREATE FUNCTION __vex_part_eval_vec(i double precision) RETURNS floatvector "
            "LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE AS $$ "
            f"SELECT ARRAY[{array_sql}]::floatvector $$;"
        )

    def prepare_source(self, rows: int) -> None:
        print(f"Preparing deterministic source rows={rows}", flush=True)
        self.psql(
            "DROP TABLE IF EXISTS vex_part_eval_truth;"
            "DROP TABLE IF EXISTS vex_part_eval_queries;"
            "DROP TABLE IF EXISTS vex_part_eval_data CASCADE;"
            "DROP TABLE IF EXISTS vex_part_eval_source;"
            "CREATE UNLOGGED TABLE vex_part_eval_source "
            "(id int PRIMARY KEY, vec floatvector({dim}) NOT NULL);"
            "INSERT INTO vex_part_eval_source "
            "SELECT i, __vex_part_eval_vec(i::double precision) "
            "FROM generate_series(1, {rows}) AS i;"
            "ANALYZE vex_part_eval_source;"
            "CREATE TABLE vex_part_eval_queries "
            "(qid int PRIMARY KEY, vec floatvector({dim}) NOT NULL);"
            "INSERT INTO vex_part_eval_queries "
            "SELECT q, __vex_part_eval_vec("
            "  (q::double precision * {rows} / ({queries} + 1)) + 0.37) "
            "FROM generate_series(1, {queries}) AS q;"
            "CREATE TABLE vex_part_eval_truth AS "
            "SELECT q.qid, exact.id "
            "FROM vex_part_eval_queries q "
            "CROSS JOIN LATERAL ("
            "  SELECT d.id FROM vex_part_eval_source d "
            "  ORDER BY l2_distance(d.vec, q.vec), d.id LIMIT 10"
            ") exact;"
            "CREATE INDEX ON vex_part_eval_truth(qid, id);".format(
                dim=self.args.dimensions,
                rows=rows,
                queries=self.args.queries,
            )
        )

    def prepare_partitions(self, rows: int, partitions: int) -> None:
        leaf_rows = rows // partitions
        statements = [
            "DROP TABLE IF EXISTS vex_part_eval_data CASCADE",
            "CREATE TABLE vex_part_eval_data "
            f"(id int, bucket int, vec floatvector({self.args.dimensions}) NOT NULL) "
            "PARTITION BY RANGE (bucket)",
        ]
        for leaf in range(partitions):
            lower = leaf * leaf_rows + 1
            upper = (leaf + 1) * leaf_rows + 1
            statements.append(
                f"CREATE TABLE vex_part_eval_data_{leaf} "
                "PARTITION OF vex_part_eval_data "
                f"FOR VALUES FROM ({lower}) TO ({upper})"
            )
        statements.extend(
            (
                "INSERT INTO vex_part_eval_data "
                "SELECT id, id, vec FROM vex_part_eval_source",
                "ANALYZE vex_part_eval_data",
            )
        )
        print(
            f"Loading rows={rows}, partitions={partitions}, rows_per_leaf={leaf_rows}",
            flush=True,
        )
        self.psql(";".join(statements) + ";")

    def build_index(self, quantizer: str) -> tuple[float, int, int, bool]:
        options = [
            f"m={self.args.m}",
            f"ef_construction={self.args.ef_construction}",
            "parallel_workers=0",
            f"quantizer={quantizer}",
            "memory_mode=compact",
        ]
        expected_name = "PQ" if quantizer == "pq" else "RaBitQ"
        if quantizer == "pq":
            options.append(f"pq_m={self.args.pq_m}")
        started = time.monotonic()
        self.psql(
            "SET maintenance_work_mem='2GB';"
            "SET max_parallel_maintenance_workers=0;"
            "CREATE INDEX vex_part_eval_idx ON vex_part_eval_data "
            "USING vexdb_graph (vec floatvector_l2_ops) "
            f"WITH ({','.join(options)});"
        )
        build_seconds = time.monotonic() - started
        active_row = self.psql(
            "SELECT count(DISTINCT child.oid), "
            "count(DISTINCT child.oid) FILTER (WHERE info.content::text="
            f"'{expected_name}') "
            "FROM pg_inherits i "
            "JOIN pg_class child ON child.oid=i.inhrelid "
            "CROSS JOIN LATERAL index_inspect(child.oid::regclass::text) info "
            "WHERE i.inhparent='vex_part_eval_idx'::regclass "
            "AND info.attribute='Working Quantizer';"
        ).splitlines()[-1]
        leaf_indexes, active_indexes = map(int, active_row.split("|"))
        size_bytes = int(
            self.psql(
                "SELECT coalesce(sum(pg_total_relation_size(relid)),0)::bigint "
                "FROM pg_partition_tree('vex_part_eval_idx') WHERE isleaf;"
            ).splitlines()[-1]
        )
        plan = self.psql(
            "SET enable_seqscan=off;"
            "EXPLAIN (COSTS OFF) SELECT id FROM vex_part_eval_data "
            "ORDER BY vec <-> (SELECT vec FROM vex_part_eval_queries WHERE qid=1) "
            "LIMIT 10;"
        )
        return build_seconds, size_bytes, active_indexes, "Index Scan using" in plan

    def measure_recall(self, ef_search: int) -> dict[str, Any]:
        started = time.monotonic()
        output = self.psql(
            "BEGIN;"
            "SET LOCAL enable_seqscan=off;"
            "SET LOCAL max_parallel_workers_per_gather=0;"
            f"SET LOCAL vexdb.ef_search={ef_search};"
            "CREATE TEMP TABLE vex_part_eval_ann ON COMMIT DROP AS "
            "SELECT q.qid, ann.id "
            "FROM vex_part_eval_queries q "
            "CROSS JOIN LATERAL ("
            "  SELECT d.id FROM vex_part_eval_data d "
            "  ORDER BY d.vec <-> q.vec LIMIT 10"
            ") ann;"
            "WITH per_query AS ("
            "  SELECT a.qid, count(t.id) AS hits "
            "  FROM vex_part_eval_ann a "
            "  LEFT JOIN vex_part_eval_truth t USING(qid,id) "
            "  GROUP BY a.qid"
            ") "
            "SELECT sum(hits), min(hits), "
            "count(*) FILTER (WHERE hits=0), count(*) "
            "FROM per_query;"
            "ROLLBACK;"
        )
        query_seconds = time.monotonic() - started
        aggregate = [line for line in output.splitlines() if line.count("|") == 3][-1]
        total_hits, min_hits, zero_queries, query_count = map(int, aggregate.split("|"))
        return {
            "ef_search": ef_search,
            "recall_at_10": total_hits / (query_count * 10),
            "min_hits_per_query": min_hits,
            "zero_hit_queries": zero_queries,
            "query_seconds": query_seconds,
        }

    def run_case(self, rows: int, partitions: int) -> None:
        self.prepare_partitions(rows, partitions)
        for run_number in range(1, self.args.runs + 1):
            quantizers = list(self.args.quantizer)
            if run_number % 2 == 0:
                quantizers.reverse()
            for quantizer in quantizers:
                print(
                    f"Building rows={rows}, partitions={partitions}, "
                    f"run={run_number}, quantizer={quantizer}",
                    flush=True,
                )
                build_seconds, size_bytes, active_indexes, uses_index = self.build_index(
                    quantizer
                )
                points = []
                for ef_search in self.args.ef_search:
                    point = self.measure_recall(ef_search)
                    points.append(point)
                    print(
                        f"  ef={ef_search} recall={point['recall_at_10']:.4f} "
                        f"min_hits={point['min_hits_per_query']} "
                        f"zero={point['zero_hit_queries']}",
                        flush=True,
                    )
                record = {
                    "rows": rows,
                    "partitions": partitions,
                    "rows_per_leaf": rows // partitions,
                    "run": run_number,
                    "quantizer": quantizer,
                    "build_seconds": build_seconds,
                    "total_leaf_index_bytes": size_bytes,
                    "active_leaf_indexes": active_indexes,
                    "uses_index": uses_index,
                    "operating_points": points,
                }
                self.results["runs"].append(record)
                self.psql("DROP INDEX vex_part_eval_idx;")

    def summarize(self) -> None:
        detailed = self.results["runs"]
        summary = self.results["summary"]
        cases = sorted({(item["rows"], item["partitions"]) for item in detailed})
        for rows, partitions in cases:
            for quantizer in self.args.quantizer:
                records = [
                    item
                    for item in detailed
                    if item["rows"] == rows
                    and item["partitions"] == partitions
                    and item["quantizer"] == quantizer
                ]
                for ef_search in self.args.ef_search:
                    points = [
                        point
                        for item in records
                        for point in item["operating_points"]
                        if point["ef_search"] == ef_search
                    ]
                    summary.append(
                        {
                            "rows": rows,
                            "partitions": partitions,
                            "rows_per_leaf": rows // partitions,
                            "quantizer": quantizer,
                            "ef_search": ef_search,
                            "recall_min": min(point["recall_at_10"] for point in points),
                            "recall_median": statistics.median(
                                point["recall_at_10"] for point in points
                            ),
                            "recall_max": max(point["recall_at_10"] for point in points),
                            "min_hits_per_query": min(
                                point["min_hits_per_query"] for point in points
                            ),
                            "max_zero_hit_queries": max(
                                point["zero_hit_queries"] for point in points
                            ),
                            "build_seconds_median": statistics.median(
                                item["build_seconds"] for item in records
                            ),
                            "all_leaf_quantizers_active": all(
                                item["active_leaf_indexes"] == partitions for item in records
                            ),
                            "all_plans_use_index": all(item["uses_index"] for item in records),
                        }
                    )

    def execute(self) -> dict[str, Any]:
        self.results["environment"] = {
            "postgresql": self.psql("SHOW server_version;"),
            "extension": self.psql(
                "SELECT extversion FROM pg_extension WHERE extname='vexdb_lite';"
            ),
        }
        self.create_vector_function()
        for rows, partitions_list in self.parse_cases(self.args.case):
            self.prepare_source(rows)
            for partitions in partitions_list:
                self.run_case(rows, partitions)
        self.summarize()
        return self.results


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--database", default="postgres")
    parser.add_argument("--psql", default="psql")
    parser.add_argument(
        "--case",
        action="append",
        default=[],
        help="ROWS:comma-separated partition counts; may be repeated",
    )
    parser.add_argument("--queries", type=int, default=20)
    parser.add_argument("--dimensions", type=int, default=32)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--ef-search", type=int, nargs="+", default=[40, 64, 100, 200])
    parser.add_argument("--quantizer", choices=("pq", "rabitq"), nargs="+", default=["pq", "rabitq"])
    parser.add_argument("--m", type=int, default=16)
    parser.add_argument("--ef-construction", type=int, default=100)
    parser.add_argument("--pq-m", type=int, default=16)
    parser.add_argument("--min-recall", type=float, default=0.80)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    if not args.case:
        args.case = ["100000:1,2,5,10"]
    if args.queries <= 0 or args.dimensions <= 0 or args.runs <= 0:
        parser.error("queries, dimensions and runs must be positive")
    if args.dimensions % args.pq_m != 0:
        parser.error("dimensions must be divisible by pq-m")
    if not 0.0 <= args.min_recall <= 1.0:
        parser.error("min-recall must be between 0 and 1")
    return args


def main() -> int:
    args = parse_args()
    benchmark = PartitionRecallBenchmark(args)
    result = benchmark.execute()
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    failures = []
    for item in result["summary"]:
        print(
            "{rows}|{partitions}|{quantizer}|{ef_search}|{recall_min:.4f}|"
            "{recall_median:.4f}|{recall_max:.4f}|{min_hits_per_query}|"
            "{max_zero_hit_queries}".format(**item)
        )
        if (
            item["recall_min"] < args.min_recall
            or item["max_zero_hit_queries"] > 0
            or not item["all_leaf_quantizers_active"]
            or not item["all_plans_use_index"]
        ):
            failures.append(item)
    if failures:
        sys.stderr.write(f"partition recall gate failed for {len(failures)} point(s)\n")
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
