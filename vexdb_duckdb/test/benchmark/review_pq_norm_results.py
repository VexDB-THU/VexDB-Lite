"""Recompute concurrent product results from real returned IDs and timestamps."""
import argparse
import csv
import hashlib
import json
import math
import shutil
import struct
import time
from pathlib import Path

from run_text_concurrency import ROOT, save, sha, verify


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def near(a, b, label):
    require(abs(a - b) <= max(1e-8, abs(a) * 1e-11), label + " arithmetic mismatch")


def csv_rows(path):
    with path.open() as stream:
        yield from csv.DictReader(stream)


def quantile(values, fraction):
    return values[math.ceil(len(values) * fraction) - 1]


def expected_points(protocol, round_id):
    points = [dict(method, connections=c) for c in protocol["connections"] for method in protocol["methods"]]
    return points if round_id == 1 else points[::-1]


def audit_round(directory, protocol, point_config, truth, canonical):
    nq, repeats, n = protocol["queries"], protocol["repeats_per_worker"], protocol["rows"]
    native = {}
    for row in csv_rows(directory / "counts.csv"):
        key = row["point"], int(row["query"])
        require(key not in native and 0 <= key[1] < nq, "duplicate native audit or query out of range")
        ids = list(map(int, row["ids"].split()))
        require(len(ids) == 10 and len(set(ids)) == 10, "invalid native return list")
        native[key] = (int(row["codes_scored"]), int(row["raw_vectors_refined"]), frozenset(ids))
    pq_names = {p["point"] for p in point_config if p["mode"] != "off"}
    require(set(native) == {(name, q) for name in pq_names for q in range(nq)}, "missing native query audit")
    points = list(csv_rows(directory / "points.csv"))
    require(len(points) == len(point_config), "missing point summaries")
    reviewed, query_rows = [], []
    for summary, expected in zip(points, point_config):
        point, c = summary["point"], int(summary["connections"])
        require(point == expected["point"] and c == expected["connections"] and summary["mode"] == expected["mode"] and
                int(summary["ef"]) == expected["ef"] and float(summary["factor"]) == expected["factor"], "point order or parameters changed")
        name, calls, wall_ns = point + "-c" + str(c), nq * repeats * c, int(summary["wall_ns"])
        plan = (directory / (name + ".explain.txt")).read_text()
        algorithm = {"off": "hnsw", "pq_only": "pq-full-scan", "pq_graph": "pq-graph", "pq_graph_norm": "pq-graph-norm"}[expected["mode"]]
        require("VEXDB_INDEX_SCAN" in plan and algorithm in plan, "query did not use the registered index path")
        hits, count, variations = 0, 0, 0
        seen, latencies, variant_examples = set(), [], []
        firsts, lasts, worker_calls = {}, {}, [0] * c
        per_query = [{"hits": 0, "calls": 0, "min": 10, "max": 0} for _ in range(nq)]
        for row in csv_rows(directory / (name + ".csv")):
            worker, repeat, q = int(row["worker"]), int(row["repeat"]), int(row["query"])
            require(0 <= worker < c and 0 <= repeat < repeats and 0 <= q < nq, "sample coordinate out of bounds")
            token = (worker * repeats + repeat) * nq + q
            require(token not in seen, "duplicate SQL sample")
            seen.add(token)
            begin, end = int(row["begin_ns"]), int(row["end_ns"])
            require(0 <= begin < end <= wall_ns and begin >= lasts.get(worker, 0), "invalid request timeline")
            firsts.setdefault(worker, begin)
            lasts[worker] = end
            worker_calls[worker] += 1
            ms = (end - begin) / 1e6
            near(ms, float(row["ms"]), "request latency")
            latencies.append(ms)
            ids = list(map(int, row["ids"].split()))
            require(len(ids) == 10 and len(set(ids)) == 10 and all(0 <= i < n for i in ids), "invalid returned IDs")
            ids_set = frozenset(ids)
            correct = set(truth[q * 10:q * 10 + 10])
            hit = len(ids_set & correct)
            require(hit == int(row["hits"]), "stored hits disagree with real returned IDs")
            if expected["mode"] != "off":
                require(ids_set == native[point, q][2], "native count audit and SQL IDs disagree")
            key = point, q
            if key in canonical and canonical[key] != ids_set:
                variations += 1
                if len(variant_examples) < 10:
                    variant_examples.append({"worker": worker, "repeat": repeat, "query": q, "ids": ids})
            else:
                canonical.setdefault(key, ids_set)
            item = per_query[q]
            item["hits"] += hit
            item["calls"] += 1
            item["min"], item["max"] = min(item["min"], hit), max(item["max"], hit)
            hits += hit
            count += 1
        require(count == calls == len(seen) and all(x == nq * repeats for x in worker_calls), "missing worker requests")
        require(all(x["calls"] == repeats * c for x in per_query), "unequal query weights")
        workers = list(csv_rows(directory / (name + "-workers.csv")))
        require(len(workers) == c, "missing worker timeline")
        for worker, row in enumerate(workers):
            require(int(row["worker"]) == worker and int(row["calls"]) == worker_calls[worker] and
                    int(row["first_begin_ns"]) == firsts[worker] and int(row["last_end_ns"]) == lasts[worker], "worker and request timelines disagree")
        active = sum(lasts[w] - firsts[w] for w in range(c)) / wall_ns
        overlap = min(lasts.values()) - max(firsts.values())
        require(overlap > 0, "no simultaneous client overlap")
        near(active, float(summary["mean_active_clients"]), "occupied client count")
        require(overlap == int(summary["overlap_ns"]), "overlap arithmetic mismatch")
        recall, qps = hits / (calls * 10), calls * 1e9 / wall_ns
        latencies.sort()
        metrics = {"recall": recall, "qps": qps, "p50_ms": quantile(latencies, .5),
                   "p95_ms": quantile(latencies, .95), "p99_ms": quantile(latencies, .99)}
        for key, value in metrics.items():
            near(value, float(summary["recall10" if key == "recall" else key]), key)
        near(float(summary["cpu_ms"]) / (wall_ns / 1e6), float(summary["cpu_cores"]), "CPU-core use")
        require(int(summary["calls"]) == calls, "summary request count mismatch")
        codes = [native[point, q][0] for q in range(nq)] if expected["mode"] != "off" else []
        refined = [native[point, q][1] for q in range(nq)] if codes else []
        if expected["mode"] == "pq_only":
            require(all(x == n for x in codes), "full scan count disagrees with row count")
        if refined:
            require(all(x == 160 for x in refined), "different exact-refinement budget")
        signature = hashlib.sha256(b"".join(struct.pack("<10i", *sorted(canonical[point, q])) for q in range(nq))).hexdigest()
        reviewed.append({"point": point, "mode": expected["mode"], "ef": expected["ef"], "factor": expected["factor"],
                         "connections": c, "calls": calls, "hits": hits, **metrics,
                         "mean_codes": sum(codes) / nq if codes else None, "max_codes": max(codes) if codes else None,
                         "mean_refined": sum(refined) / nq if refined else None,
                         "cpu_cores": float(summary["cpu_cores"]), "mean_active_clients": active,
                         "occupancy_ok": active >= .8 * c, "overlap_ms": overlap / 1e6,
                         "request_window_qps": calls * 1e9 / (max(lasts.values()) - min(firsts.values())),
                         "rss_before": int(summary["rss_before"]), "rss_after": int(summary["rss_after"]),
                         "major_faults": int(summary["major_faults"]), "minor_faults": int(summary["minor_faults"]),
                         "first_ms": float(summary["first_ms"]), "returned_set_variations": variations,
                         "variation_examples": variant_examples, "canonical_ids_sha256": signature,
                         "min_query_recall": min(x["hits"] / (x["calls"] * 10) for x in per_query),
                         "perfect_query_fraction": sum(x["min"] == 10 for x in per_query) / nq})
        query_rows.extend({"point": point, "connections": c, "query": q, "calls": x["calls"],
                           "mean_hits": x["hits"] / x["calls"], "min_hits": x["min"], "max_hits": x["max"]}
                          for q, x in enumerate(per_query))
    return reviewed, query_rows
