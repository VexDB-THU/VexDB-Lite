#!/usr/bin/env python3
"""Benchmark PostgreSQL plain graph direct-to-disk worker scaling.

The default expectation verifies the fine-grained parallel build protocol:

    python3 vexdb_pg/test/plain_parallel_disk_benchmark.py

To reproduce the behavior of an older build with the full insertion lock:

    python3 vexdb_pg/test/plain_parallel_disk_benchmark.py --expect serialized

Environment defaults:
    PG_BENCH_CONTAINER=vexdb_pg19-test
    PG_BENCH_ROWS=100000
    PG_BENCH_RUNS=3
    PG_BENCH_DIM=32
    PG_BENCH_WORK_MEM=512MB
    PG_BENCH_MIN_RECALL=0.90
"""

import argparse
import csv
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
import tempfile
import threading
import time


def parse_size_bytes(value):
    match = re.match(r"^\s*([0-9.]+)\s*([KMGT]?i?B)\s*$", value)
    if not match:
        return 0
    number = float(match.group(1))
    unit = match.group(2)
    factors = {
        "B": 1,
        "kB": 1000,
        "KB": 1000,
        "KiB": 1024,
        "MB": 1000 ** 2,
        "MiB": 1024 ** 2,
        "GB": 1000 ** 3,
        "GiB": 1024 ** 3,
        "TB": 1000 ** 4,
        "TiB": 1024 ** 4,
    }
    return int(number * factors.get(unit, 1))


def command(args, check=True, capture=True, stdin=None):
    result = subprocess.run(
        args,
        input=stdin,
        universal_newlines=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
    )
    if check and result.returncode != 0:
        if result.stdout:
            sys.stderr.write(result.stdout)
        if result.stderr:
            sys.stderr.write(result.stderr)
        raise RuntimeError("command failed: {}".format(" ".join(args)))
    return result


class Benchmark:
    def __init__(self, args):
        self.container = args.container
        self.rows = args.rows
        self.runs = args.runs
        self.dim = args.dim
        self.work_mem = args.work_mem
        self.expect = args.expect
        self.min_recall = args.min_recall
        self.sample_interval = args.sample_interval
        self.results = []

        root = Path(__file__).resolve().parents[2]
        self.out_dir = root / "build" / "bench" / "pg_plain_parallel_disk"
        self.out_dir.mkdir(parents=True, exist_ok=True)
        run_id = time.strftime("%Y%m%d-%H%M%S")
        self.csv_path = self.out_dir / (run_id + ".csv")
        self.summary_path = self.out_dir / (run_id + ".summary.txt")

    def docker(self, *args, **kwargs):
        return command(["docker"] + list(args), **kwargs)

    def psql(self, sql, check=True):
        result = self.docker(
            "exec",
            self.container,
            "psql",
            "-d",
            "test",
            "-X",
            "-q",
            "-t",
            "-A",
            "-F",
            "|",
            "-v",
            "ON_ERROR_STOP=1",
            "-c",
            sql,
            check=check,
        )
        return result

    def prepare(self):
        command(["docker", "info"])
        inspect = self.docker("inspect", "-f", "{{.State.Running}}", self.container)
        if inspect.stdout.strip() != "true":
            self.docker("start", self.container)

        print(
            "Preparing deterministic dataset: rows={} dim={} work_mem={}".format(
                self.rows, self.dim, self.work_mem
            ),
            flush=True,
        )
        setup_sql = """
        DROP SCHEMA IF EXISTS plain_parallel_bench CASCADE;
        CREATE SCHEMA plain_parallel_bench;
        SET search_path = plain_parallel_bench, public;

        CREATE FUNCTION bench_vec(i int) RETURNS floatvector
        LANGUAGE SQL IMMUTABLE STRICT AS $$
          SELECT array_agg(
            (sin(i * 0.013 + j * 0.17) +
             cos(i * 0.007 * (j + 1)) +
             ((i % 97) * (j + 1)) * 0.00001)::float4
            ORDER BY j
          )::floatvector
          FROM generate_series(0, {dim_minus_one}) AS j
        $$;

        CREATE TABLE bench_data (
          id integer PRIMARY KEY,
          vec floatvector({dim}) NOT NULL
        );

        INSERT INTO bench_data
        SELECT i, bench_vec(i)
        FROM generate_series(1, {rows}) AS i;

        ANALYZE bench_data;

        CREATE TABLE bench_queries AS
        SELECT id AS qid, vec
        FROM bench_data
        WHERE id IN ({query_ids});

        -- Materialize exact truth before the ANN index exists.  The old
        -- benchmark only checked that an index scan returned ten rows, which
        -- allowed a structurally valid but low-quality graph to pass.
        CREATE TABLE bench_truth AS
        SELECT q.qid, exact.id
        FROM bench_queries q
        CROSS JOIN LATERAL (
          SELECT d.id
          FROM bench_data d
          ORDER BY d.vec <-> q.vec, d.id
          LIMIT 10
        ) exact;
        """.format(
            dim_minus_one=self.dim - 1,
            dim=self.dim,
            rows=self.rows,
            query_ids=", ".join(str(value) for value in self.query_ids()),
        )
        self.psql(setup_sql)

    def query_ids(self):
        candidates = (137, 1021, 2345, 4093, 5879, 7331, 9109, 17777)
        selected = [value for value in candidates if value <= self.rows]
        if not selected:
            selected = [self.rows]
        return selected

    def sample_state(self):
        state_sql = """
        SELECT
          count(*) FILTER (WHERE wait_event = 'graph_build_entry'),
          count(*) FILTER (WHERE wait_event = 'graph_build_entry_wait'),
          count(*) FILTER (WHERE wait_event = 'graph_build_storage'),
          count(*) FILTER (WHERE wait_event = 'graph_build_extension'),
          count(*) FILTER (WHERE wait_event = 'graph_build_point'),
          count(*) FILTER (WHERE backend_type = 'parallel worker')
        FROM pg_stat_activity
        WHERE pid <> pg_backend_pid()
          AND (leader_pid IS NOT NULL OR query LIKE '%%CREATE INDEX bench_idx%%');
        """
        state = self.psql(state_sql, check=False)
        state_line = ""
        if state.returncode == 0:
            lines = [line for line in state.stdout.splitlines() if line.strip()]
            if lines:
                state_line = lines[-1]
        try:
            values = [int(value) for value in state_line.split("|")]
            if len(values) != 6:
                raise ValueError("unexpected pg_stat_activity sample")
        except (ValueError, TypeError):
            values = [0] * 6

        stats = self.docker(
            "stats",
            "--no-stream",
            "--format",
            "{{.CPUPerc}}|{{.MemUsage}}",
            self.container,
            check=False,
        )
        try:
            cpu_text, memory_text = stats.stdout.strip().split("|", 1)
            cpu = float(cpu_text.rstrip("%"))
            memory = parse_size_bytes(memory_text.split("/", 1)[0])
        except (ValueError, TypeError):
            cpu = 0.0
            memory = 0
        return {
            "entry": values[0],
            "entry_wait": values[1],
            "storage": values[2],
            "extension": values[3],
            "point": values[4],
            "workers": values[5],
            "cpu": cpu,
            "memory": memory,
        }

    def validate(self):
        validation_sql = """
        SET search_path=plain_parallel_bench,public;
        SET enable_seqscan=off;
        SET vexdb.ef_search=200;
        WITH ann AS MATERIALIZED (
          SELECT q.qid, found.id
          FROM bench_queries q
          CROSS JOIN LATERAL (
            SELECT d.id
            FROM bench_data d
            ORDER BY d.vec <-> q.vec
            LIMIT 10
          ) found
        ), quality AS (
          SELECT count(t.id)::double precision /
                 ((SELECT count(*) FROM bench_queries) * 10) AS recall
          FROM ann a
          LEFT JOIN bench_truth t USING (qid, id)
        )
        SELECT i.indisvalid,
               i.indisready,
               (SELECT content::text::bigint
                FROM index_inspect('plain_parallel_bench.bench_idx')
                WHERE attribute='Base Container Number of Entries'),
               (SELECT count(*) FROM ann),
               round((SELECT recall FROM quality)::numeric, 6)
        FROM pg_index i
        WHERE i.indexrelid='plain_parallel_bench.bench_idx'::regclass;
        """
        result = self.psql(validation_sql)
        lines = [line for line in result.stdout.splitlines() if line.strip()]
        validation = lines[-1] if lines else ""
        parts = validation.split("|")
        try:
            recall = float(parts[4])
        except (IndexError, ValueError):
            recall = 0.0
        return validation, recall

    def run_once(self, workers, run_no, log_dir):
        label = "w{}-r{}".format(workers, run_no)
        self.psql(
            "DROP INDEX IF EXISTS plain_parallel_bench.bench_idx; CHECKPOINT;"
        )
        # Docker's absolute memory usage includes PostgreSQL shared memory and
        # filesystem cache left by earlier rounds.  Compare the build peak to
        # the immediately preceding per-round baseline so alternating run order
        # cannot make one worker count look cheaper merely because it ran first.
        baseline_memory = self.sample_state()["memory"]
        build_sql = (
            "SET search_path=plain_parallel_bench,public; "
            "SET maintenance_work_mem='{work_mem}'; "
            "SET max_parallel_workers=8; "
            "SET max_parallel_maintenance_workers=3; "
            "CREATE INDEX bench_idx ON bench_data "
            "USING vexdb_graph (vec floatvector_l2_ops) "
            "WITH (m=16, ef_construction=100, parallel_workers={workers})"
        ).format(work_mem=self.work_mem, workers=workers)

        log_path = Path(log_dir) / (label + ".log")
        start = time.monotonic()
        with log_path.open("w") as log_file:
            process = subprocess.Popen(
                [
                    "docker",
                    "exec",
                    self.container,
                    "psql",
                    "-d",
                    "test",
                    "-X",
                    "-q",
                    "-v",
                    "ON_ERROR_STOP=1",
                    "-c",
                    build_sql,
                ],
                stdout=log_file,
                stderr=subprocess.STDOUT,
                universal_newlines=True,
            )

            waiter_samples = {
                "entry": [],
                "entry_wait": [],
                "storage": [],
                "extension": [],
                "point": [],
            }
            parallel_workers = []
            cpu_samples = []
            memory_samples = []
            stop_sampling = threading.Event()

            def sample_until_done():
                """Observe the build without using polling as the wall-clock timer.

                docker stats can take seconds to return.  The old synchronous loop
                only noticed process completion after that call, quantizing short
                builds into sampling-sized buckets.  The main thread now waits for
                psql directly; this thread is observability only.  A sample that
                finishes after psql exits is discarded so terminal idle CPU does
                not depress the average for slower runs.
                """
                while not stop_sampling.is_set():
                    sample_started = time.monotonic()
                    sample = self.sample_state()
                    if stop_sampling.is_set():
                        break
                    for lock_name in waiter_samples:
                        waiter_samples[lock_name].append(sample[lock_name])
                    parallel_workers.append(sample["workers"])
                    cpu_samples.append(sample["cpu"])
                    memory_samples.append(sample["memory"])
                    total_waiters = sum(sample[name] for name in waiter_samples)
                    print(
                        "progress run={} sample={} waiters={} "
                        "entry={} entry_wait={} storage={} extension={} point={} "
                        "workers={} cpu={:.2f}% memory_mib={:.1f} memory_delta_mib={:.1f}".format(
                            label,
                            len(cpu_samples),
                            total_waiters,
                            sample["entry"],
                            sample["entry_wait"],
                            sample["storage"],
                            sample["extension"],
                            sample["point"],
                            sample["workers"],
                            sample["cpu"],
                            sample["memory"] / (1024.0 * 1024.0),
                            max(0, sample["memory"] - baseline_memory) / (1024.0 * 1024.0),
                        ),
                        flush=True,
                    )
                    remaining = self.sample_interval - (time.monotonic() - sample_started)
                    if remaining > 0:
                        stop_sampling.wait(remaining)

            sampler = threading.Thread(
                target=sample_until_done,
                name="pg-build-observer",
                daemon=True,
            )
            sampler.start()
            status = process.wait()
            elapsed = time.monotonic() - start
            stop_sampling.set()
            sampler.join()
        validation = "build_failed"
        recall = 0.0
        if status == 0:
            validation, recall = self.validate()
            expected_ann_rows = len(self.query_ids()) * 10
            if validation.split("|")[:4] != [
                "t", "t", str(self.rows), str(expected_ann_rows)
            ]:
                status = 4
            elif recall < self.min_recall:
                status = 5

        result = {
            "run": run_no,
            "workers": workers,
            "status": status,
            "elapsed_s": round(elapsed, 3),
            "samples": len(cpu_samples),
            "avg_lock_waiters": round(statistics.mean([
                sum(waiter_samples[name][i] for name in waiter_samples)
                for i in range(len(cpu_samples))
            ]), 3) if cpu_samples else 0.0,
            "max_lock_waiters": max([
                sum(waiter_samples[name][i] for name in waiter_samples)
                for i in range(len(cpu_samples))
            ]) if cpu_samples else 0,
            "max_parallel_workers": max(parallel_workers) if parallel_workers else 0,
            "avg_cpu_pct": (
                round(statistics.mean(cpu_samples), 3) if cpu_samples else 0.0
            ),
            "max_cpu_pct": round(max(cpu_samples), 3) if cpu_samples else 0.0,
            "baseline_memory_mib": round(
                baseline_memory / (1024.0 * 1024.0), 3
            ),
            "peak_memory_mib": round(
                max(memory_samples) / (1024.0 * 1024.0), 3
            ) if memory_samples else 0.0,
            "peak_memory_delta_mib": round(
                max(0, max(memory_samples) - baseline_memory) / (1024.0 * 1024.0), 3
            ) if memory_samples else 0.0,
            "validation": validation,
            "recall_at_10": recall,
        }
        for lock_name, samples in waiter_samples.items():
            result["avg_{}_waiters".format(lock_name)] = (
                round(statistics.mean(samples), 3) if samples else 0.0
            )
            result["max_{}_waiters".format(lock_name)] = max(samples) if samples else 0
        self.results.append(result)
        print(
            "RESULT run={run} workers={workers} status={status} "
            "elapsed_s={elapsed_s:.3f} avg_waiters={avg_lock_waiters:.3f} "
            "max_waiters={max_lock_waiters} avg_cpu={avg_cpu_pct:.3f}% "
            "max_cpu={max_cpu_pct:.3f}% validation={validation}".format(**result),
            flush=True,
        )

        if status != 0:
            sys.stderr.write(log_path.read_text())
            raise RuntimeError("{} failed with status {}".format(label, status))
        self.psql("DROP INDEX plain_parallel_bench.bench_idx;")

    def write_csv(self):
        columns = [
            "run",
            "workers",
            "status",
            "elapsed_s",
            "samples",
            "avg_lock_waiters",
            "max_lock_waiters",
            "avg_entry_waiters",
            "max_entry_waiters",
            "avg_entry_wait_waiters",
            "max_entry_wait_waiters",
            "avg_storage_waiters",
            "max_storage_waiters",
            "avg_extension_waiters",
            "max_extension_waiters",
            "avg_point_waiters",
            "max_point_waiters",
            "max_parallel_workers",
            "avg_cpu_pct",
            "max_cpu_pct",
            "baseline_memory_mib",
            "peak_memory_mib",
            "peak_memory_delta_mib",
            "validation",
            "recall_at_10",
        ]
        with self.csv_path.open("w", newline="") as file_obj:
            writer = csv.DictWriter(file_obj, fieldnames=columns)
            writer.writeheader()
            writer.writerows(self.results)

    def median(self, workers, key):
        values = [row[key] for row in self.results if row["workers"] == workers]
        return float(statistics.median(values))

    def summarize(self):
        self.write_csv()
        w1_elapsed = self.median(1, "elapsed_s")
        w3_elapsed = self.median(3, "elapsed_s")
        w1_cpu = self.median(1, "avg_cpu_pct")
        w3_cpu = self.median(3, "avg_cpu_pct")
        w1_waiters = self.median(1, "avg_lock_waiters")
        w3_waiters = self.median(3, "avg_lock_waiters")
        w1_memory = self.median(1, "peak_memory_mib")
        w3_memory = self.median(3, "peak_memory_mib")
        w1_memory_delta = self.median(1, "peak_memory_delta_mib")
        w3_memory_delta = self.median(3, "peak_memory_delta_mib")
        w1_recall = self.median(1, "recall_at_10")
        w3_recall = self.median(3, "recall_at_10")
        w1_success = sum(
            1 for row in self.results if row["workers"] == 1 and row["status"] == 0
        )
        w3_success = sum(
            1 for row in self.results if row["workers"] == 3 and row["status"] == 0
        )
        speedup = w1_elapsed / w3_elapsed
        gain_pct = (w1_elapsed - w3_elapsed) * 100.0 / w1_elapsed
        all_valid = w1_success == self.runs and w3_success == self.runs

        lines = [
            "PG plain parallel disk benchmark",
            "rows={} dim={} runs={} work_mem={}".format(
                self.rows, self.dim, self.runs, self.work_mem
            ),
            "w1_median_elapsed_s={:.3f}".format(w1_elapsed),
            "w3_median_elapsed_s={:.3f}".format(w3_elapsed),
            "w3_speedup={:.3f}x".format(speedup),
            "w3_gain_pct={:.2f}%".format(gain_pct),
            "w1_median_cpu_pct={:.3f}".format(w1_cpu),
            "w3_median_cpu_pct={:.3f}".format(w3_cpu),
            "w1_median_lock_waiters={:.3f}".format(w1_waiters),
            "w3_median_lock_waiters={:.3f}".format(w3_waiters),
            "w1_median_peak_memory_mib={:.3f}".format(w1_memory),
            "w3_median_peak_memory_mib={:.3f}".format(w3_memory),
            "w1_median_peak_memory_delta_mib={:.3f}".format(w1_memory_delta),
            "w3_median_peak_memory_delta_mib={:.3f}".format(w3_memory_delta),
            "w1_median_recall_at_10={:.6f}".format(w1_recall),
            "w3_median_recall_at_10={:.6f}".format(w3_recall),
            "min_recall_at_10={:.6f}".format(self.min_recall),
            "w1_success={}/{}".format(w1_success, self.runs),
            "w3_success={}/{}".format(w3_success, self.runs),
            "csv={}".format(self.csv_path),
        ]

        if self.expect == "serialized":
            passed = (
                all_valid
                and speedup <= 1.15
                and w3_cpu <= 150.0
                and w3_waiters >= 1.0
            )
            verdict = (
                "SERIALIZATION_REPRODUCED"
                if passed
                else "SERIALIZATION_NOT_REPRODUCED"
            )
        elif self.expect == "optimized":
            passed = (
                all_valid
                and speedup >= 1.20
                and w3_cpu >= 180.0
            )
            verdict = "OPTIMIZATION_VERIFIED" if passed else "OPTIMIZATION_NOT_VERIFIED"
        else:
            passed = all_valid
            verdict = "MEASURED" if passed else "VALIDATION_FAILED"

        lines.append("VERDICT=" + verdict)
        report = "\n".join(lines) + "\n"
        self.summary_path.write_text(report)
        print(report, end="", flush=True)
        return passed

    def cleanup(self):
        try:
            self.psql("DROP SCHEMA IF EXISTS plain_parallel_bench CASCADE;", check=False)
        except Exception:
            pass

    def execute(self):
        self.prepare()
        try:
            with tempfile.TemporaryDirectory(
                prefix="vexdb-plain-bench-", dir="/private/tmp"
            ) as log_dir:
                for run_no in range(1, self.runs + 1):
                    order = (1, 3) if run_no % 2 == 1 else (3, 1)
                    for workers in order:
                        self.run_once(workers, run_no, log_dir)
            return self.summarize()
        finally:
            self.cleanup()


def parse_args():
    parser = argparse.ArgumentParser(
        description="Benchmark PG plain graph direct-to-disk worker scaling"
    )
    parser.add_argument(
        "--container",
        default=os.environ.get("PG_BENCH_CONTAINER", "vexdb_pg19-test"),
    )
    parser.add_argument(
        "--rows", type=int, default=int(os.environ.get("PG_BENCH_ROWS", "100000"))
    )
    parser.add_argument(
        "--runs", type=int, default=int(os.environ.get("PG_BENCH_RUNS", "3"))
    )
    parser.add_argument(
        "--dim", type=int, default=int(os.environ.get("PG_BENCH_DIM", "32"))
    )
    parser.add_argument(
        "--work-mem",
        default=os.environ.get("PG_BENCH_WORK_MEM", "512MB"),
    )
    parser.add_argument(
        "--expect",
        choices=("serialized", "optimized", "none"),
        default=os.environ.get("PG_BENCH_EXPECT", "optimized"),
    )
    parser.add_argument(
        "--min-recall",
        type=float,
        default=float(os.environ.get("PG_BENCH_MIN_RECALL", "0.90")),
        help="minimum exact recall@10 required for every completed build",
    )
    parser.add_argument(
        "--sample-interval",
        type=float,
        default=float(os.environ.get("PG_BENCH_SAMPLE_INTERVAL", "0.5")),
        help="seconds between observability samples; timing does not depend on this interval",
    )
    args = parser.parse_args()
    if (
        args.rows < 10
        or args.runs < 1
        or args.dim < 1
        or args.sample_interval <= 0
        or not 0.0 <= args.min_recall <= 1.0
    ):
        parser.error("rows must be >= 10; runs and dim must be positive")
    if not re.match(r"^[1-9][0-9]*(kB|MB|GB)$", args.work_mem):
        parser.error("work-mem must look like 512MB or 2GB")
    return args


def main():
    args = parse_args()
    benchmark = Benchmark(args)
    try:
        passed = benchmark.execute()
    except Exception as exc:
        sys.stderr.write("ERROR: {}\n".format(exc))
        return 1
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
