#!/usr/bin/env python3
"""Exercise PostgreSQL graph-index scans after the vector cache is full.

This is intentionally query-only: it reuses an existing table, query table,
and index so the same physical index can be compared before and after a vector
buffer manager change.
"""

import argparse
import random
import statistics
import threading
import time
from concurrent.futures import ThreadPoolExecutor

import psycopg2


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5432)
    parser.add_argument("--database", default="test")
    parser.add_argument("--user", default="postgres")
    parser.add_argument("--data-table", default="cohere_data")
    parser.add_argument("--query-table", default="cohere_queries")
    parser.add_argument("--index-name", default="cohere_idx")
    parser.add_argument("--distance-op", default="<=>", choices=("<->", "<#>", "<=>"))
    parser.add_argument("--query-count", type=int, default=10000)
    parser.add_argument("--query-cardinality", type=int, default=10000)
    parser.add_argument("--warmup", type=int, default=1000)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--concurrency", default="1,5,10")
    parser.add_argument("--ef-search", type=int, default=200)
    parser.add_argument("--k", type=int, default=10)
    parser.add_argument("--statement-timeout-ms", type=int, default=30000)
    return parser.parse_args()


class Runner:
    def __init__(self, args):
        self.args = args
        self.local = threading.local()
        self.connections = []
        self.connections_lock = threading.Lock()
        self.sql = (
            "SELECT id FROM {data} ORDER BY vec {op} "
            "(SELECT vec FROM {queries} WHERE qid=%s) LIMIT %s"
        ).format(
            data=args.data_table,
            op=args.distance_op,
            queries=args.query_table,
        )

    def close(self):
        with self.connections_lock:
            for connection in self.connections:
                connection.close()
            self.connections.clear()
        self.local = threading.local()

    def cursor(self):
        cursor = getattr(self.local, "cursor", None)
        if cursor is not None:
            return cursor
        connection = psycopg2.connect(
            host=self.args.host,
            port=self.args.port,
            dbname=self.args.database,
            user=self.args.user,
            connect_timeout=10,
        )
        connection.autocommit = True
        cursor = connection.cursor()
        cursor.execute("SET enable_seqscan=off")
        cursor.execute("SET vexdb.ef_search=%s", (self.args.ef_search,))
        cursor.execute("SET statement_timeout=%s", (self.args.statement_timeout_ms,))
        self.local.cursor = cursor
        with self.connections_lock:
            self.connections.append(connection)
        return cursor

    def search(self, qid):
        cursor = self.cursor()
        cursor.execute(self.sql, (qid, self.args.k))
        rows = cursor.fetchall()
        if len(rows) != self.args.k:
            raise RuntimeError("query {} returned {} rows".format(qid, len(rows)))
        return rows

    def validate_index(self):
        connection = psycopg2.connect(
            host=self.args.host,
            port=self.args.port,
            dbname=self.args.database,
            user=self.args.user,
            connect_timeout=10,
        )
        connection.autocommit = True
        try:
            cursor = connection.cursor()
            cursor.execute(
                "SELECT indisvalid, indisready FROM pg_index "
                "WHERE indexrelid=to_regclass(%s)",
                (self.args.index_name,),
            )
            state = cursor.fetchone()
            if state != (True, True):
                raise RuntimeError(
                    "index {} is not valid and ready: {}".format(
                        self.args.index_name, state
                    )
                )
            cursor.execute("SET enable_seqscan=off")
            cursor.execute("SET vexdb.ef_search=%s", (self.args.ef_search,))
            cursor.execute("EXPLAIN (COSTS OFF) " + self.sql, (0, self.args.k))
            plan = "\n".join(row[0] for row in cursor.fetchall())
            if self.args.index_name not in plan:
                raise RuntimeError("expected index is not used:\n" + plan)
            print(plan, flush=True)
        finally:
            connection.close()

    def run_once(self, concurrency, qids):
        self.close()
        warmup = qids[: min(self.args.warmup, len(qids))]
        with ThreadPoolExecutor(max_workers=concurrency) as pool:
            list(pool.map(self.search, warmup))
            started = time.perf_counter()
            list(pool.map(self.search, qids))
        elapsed = time.perf_counter() - started
        self.close()
        return len(qids) / elapsed, elapsed


def main():
    args = parse_args()
    if args.query_count <= 0 or args.query_cardinality <= 0:
        raise SystemExit("query counts must be positive")
    rng = random.Random(20260725)
    qids = [i % args.query_cardinality for i in range(args.query_count)]
    rng.shuffle(qids)
    runner = Runner(args)
    try:
        runner.validate_index()
        for concurrency in (int(item) for item in args.concurrency.split(",") if item):
            values = []
            for repetition in range(1, args.repetitions + 1):
                qps, elapsed = runner.run_once(concurrency, qids)
                values.append(qps)
                print(
                    "concurrency={} repetition={} qps={:.3f} seconds={:.3f}".format(
                        concurrency, repetition, qps, elapsed
                    ),
                    flush=True,
                )
            print(
                "concurrency={} median_qps={:.3f}".format(
                    concurrency, statistics.median(values)
                ),
                flush=True,
            )
    finally:
        runner.close()


if __name__ == "__main__":
    main()
