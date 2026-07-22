#!/usr/bin/env python3
"""SQLite plain/PQ/RaBitQ 可重复的本地性能与召回对比。

默认工作负载与迁移报告一致：20000 行、32 维、200 个查询、k=10。
需要 numpy；扩展应使用 Release 构建，避免把 Debug 数字写进报告。
"""

from __future__ import annotations

import argparse
import json
import os
import sqlite3
import statistics
import tempfile
import time
from pathlib import Path

import numpy as np


DEFAULT_MODES = ("plain", "pq-full", "pq-compact", "rabitq-full", "rabitq-compact")


def create_args(mode: str, dim: int) -> str:
    common = (
        f"v FLOAT[{dim}], metric=l2, m=16, ef_construction=160, "
        "ef_search=160, brute_force_threshold=0"
    )
    if mode == "plain":
        return common + ", quantizer=none, memory_mode=full"
    if mode.startswith("pq-"):
        pq_m = max(1, dim // 4)
        while dim % pq_m != 0:
            pq_m -= 1
        return common + f", quantizer=pq, pq_m={pq_m}, memory_mode={mode[3:]}"
    if mode.startswith("rabitq-"):
        return common + f", quantizer=rabitq, memory_mode={mode[7:]}"
    raise ValueError(f"unknown mode: {mode}")


def vector_json(vec: np.ndarray) -> str:
    return json.dumps(vec.tolist(), separators=(",", ":"))


def exact_topk(data: np.ndarray, queries: np.ndarray, k: int) -> list[set[int]]:
    truth: list[set[int]] = []
    for query in queries:
        squared = np.sum((data - query) ** 2, axis=1)
        ids = np.argpartition(squared, k - 1)[:k]
        truth.append({int(i) + 1 for i in ids})
    return truth


def run_mode(
    extension: Path,
    mode: str,
    data: np.ndarray,
    query_json: list[str],
    truth: list[set[int]],
    k: int,
) -> dict[str, float | int | str]:
    fd, db_path = tempfile.mkstemp(prefix=f"vexdb-{mode}-", suffix=".db")
    os.close(fd)
    try:
        conn = sqlite3.connect(db_path)
        conn.enable_load_extension(True)
        conn.execute(
            "SELECT load_extension(?, 'sqlite3_vexdblite_init')", (str(extension),)
        )
        conn.execute(f"CREATE VIRTUAL TABLE idx USING GRAPH_INDEX({create_args(mode, data.shape[1])})")

        rows = ((i + 1, vector_json(vec)) for i, vec in enumerate(data))
        started = time.perf_counter()
        with conn:
            conn.executemany("INSERT INTO idx(rowid, v) VALUES (?, ?)", rows)
        load_seconds = time.perf_counter() - started

        started = time.perf_counter()
        conn.execute(
            "SELECT rowid FROM idx WHERE v MATCH ? AND k = ?", (query_json[0], k)
        ).fetchall()
        build_seconds = time.perf_counter() - started

        # 预热不计入 QPS；每个模式使用完全相同的查询顺序。
        for query in query_json[: min(20, len(query_json))]:
            conn.execute(
                "SELECT rowid FROM idx WHERE v MATCH ? AND k = ?", (query, k)
            ).fetchall()

        hit = 0
        started = time.perf_counter()
        for query, expected in zip(query_json, truth):
            rows = conn.execute(
                "SELECT rowid FROM idx WHERE v MATCH ? AND k = ?", (query, k)
            ).fetchall()
            hit += len({int(row[0]) for row in rows} & expected)
        query_seconds = time.perf_counter() - started

        # SQLite 图是按需构建的；追加一个与已有向量相同的 rowid，触发 xSync
        # 把图和量化数据落盘，但不会改变图节点/code 数量。
        started = time.perf_counter()
        with conn:
            conn.execute(
                "INSERT INTO idx(rowid, v) VALUES (?, ?)",
                (len(data) + 1, vector_json(data[0])),
            )
        persist_seconds = time.perf_counter() - started
        raw_bytes = int(
            conn.execute(
                "SELECT coalesce(sum(length(data)), 0) FROM idx_graph WHERE kind = 4"
            ).fetchone()[0]
        )
        code_bytes = int(
            conn.execute(
                "SELECT coalesce(sum(length(data)), 0) FROM idx_graph WHERE kind IN (5, 6)"
            ).fetchone()[0]
        )
        conn.execute("PRAGMA wal_checkpoint(TRUNCATE)").fetchall()
        db_bytes = os.path.getsize(db_path)
        conn.close()

        return {
            "mode": mode,
            "load_s": load_seconds,
            "build_s": build_seconds,
            "persist_s": persist_seconds,
            "qps": len(query_json) / query_seconds,
            "recall_at_k": hit / (len(query_json) * k),
            "raw_mirror_bytes": raw_bytes,
            "quantizer_bytes": code_bytes,
            "db_bytes": db_bytes,
        }
    finally:
        try:
            os.unlink(db_path)
        except FileNotFoundError:
            pass


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("extension", type=Path)
    parser.add_argument("--rows", type=int, default=20_000)
    parser.add_argument("--dim", type=int, default=32)
    parser.add_argument("--queries", type=int, default=200)
    parser.add_argument("--k", type=int, default=10)
    parser.add_argument("--seed", type=int, default=20260722)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--modes", nargs="+", default=list(DEFAULT_MODES), choices=DEFAULT_MODES)
    parser.add_argument(
        "--check",
        action="store_true",
        help="按当前合成基线检查所有模式 recall@k >= 0.95",
    )
    args = parser.parse_args()

    extension = args.extension.resolve()
    if not extension.exists():
        parser.error(f"extension does not exist: {extension}")
    if args.rows < args.k or args.queries < 1 or args.dim < 1 or args.repetitions < 1:
        parser.error("rows must be >= k, and queries/dim/repetitions must be positive")

    rng = np.random.default_rng(args.seed)
    data = rng.normal(size=(args.rows, args.dim)).astype(np.float32)
    query_ids = rng.choice(args.rows, size=args.queries, replace=args.queries > args.rows)
    queries = data[query_ids]
    query_json = [vector_json(query) for query in queries]
    truth = exact_topk(data, queries, args.k)

    runs: dict[str, list[dict[str, float | int | str]]] = {
        mode: [] for mode in args.modes
    }
    for repetition in range(args.repetitions):
        order = list(args.modes)
        if repetition % 2:
            order.reverse()
        shift = repetition % len(order)
        order = order[shift:] + order[:shift]
        print(f"run {repetition + 1}/{args.repetitions}: {' '.join(order)}")
        for mode in order:
            result = run_mode(extension, mode, data, query_json, truth, args.k)
            runs[mode].append(result)
            print(
                f"  {mode:<16} build={result['build_s']:.3f}s "
                f"qps={result['qps']:.1f} recall={result['recall_at_k']:.4f}"
            )

    results: list[dict[str, float | int | str]] = []
    for mode in args.modes:
        mode_runs = runs[mode]
        aggregate: dict[str, float | int | str] = {"mode": mode}
        for key in mode_runs[0]:
            if key == "mode":
                continue
            aggregate[key] = statistics.median(float(run[key]) for run in mode_runs)
        results.append(aggregate)

    print("median")
    print("mode             load_s  build_s persist_s       QPS  recall  raw_MiB code_MiB db_MiB")
    for result in results:
        print(
            f"{result['mode']:<16} {result['load_s']:7.3f} {result['build_s']:8.3f} "
            f"{result['persist_s']:9.3f} {result['qps']:9.1f} "
            f"{result['recall_at_k']:7.4f} "
            f"{result['raw_mirror_bytes'] / 2**20:7.2f} "
            f"{result['quantizer_bytes'] / 2**20:8.2f} "
            f"{result['db_bytes'] / 2**20:6.2f}"
        )
    plain = next((result for result in results if result["mode"] == "plain"), None)
    if plain is not None:
        for result in results:
            print(
                f"{result['mode']}/plain build_ratio="
                f"{float(result['build_s']) / float(plain['build_s']):.4f} "
                f"qps_ratio={float(result['qps']) / float(plain['qps']):.4f}"
            )
    print(
        json.dumps(
            {"repetitions": args.repetitions, "results": results},
            ensure_ascii=False,
            indent=2,
        )
    )
    if args.check:
        failed = []
        for result in results:
            floor = 0.95
            if float(result["recall_at_k"]) < floor:
                failed.append(
                    f"{result['mode']} recall {result['recall_at_k']:.4f} < {floor:.2f}"
                )
            mode = str(result["mode"])
            raw_bytes = float(result["raw_mirror_bytes"])
            code_bytes = float(result["quantizer_bytes"])
            if mode.endswith("compact") and raw_bytes != 0:
                failed.append(f"{mode} raw mirror is {raw_bytes:.0f}, expected 0")
            if mode != "plain" and code_bytes <= 0:
                failed.append(f"{mode} quantizer bytes are not positive")
        if failed:
            print("benchmark check failed: " + "; ".join(failed))
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
