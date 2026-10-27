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
        algorithm = {"off": "hnsw", "pq_only": "pq-full-scan", "pq_graph": "pq-graph"}[expected["mode"]]
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


def register(run):
    protocol = verify(run)
    require(not (run / "round1-hashes.json").exists(), "register reviewer before completed benchmark results")
    source = Path(__file__).resolve()
    snapshot = run / "reviewer-source.py"
    snapshot.write_bytes(source.read_bytes())
    save(run / "reviewer-manifest.json", {"registered_epoch": time.time(), "script_sha256": sha(source),
                                          "snapshot_sha256": sha(snapshot), "protocol_sha256": sha(run / "protocol.json"),
                                          "rules": "Independent actual-ID and nanosecond recomputation; all predefined points retained; no QPS claim from native counters. Quality-frontier tables are descriptive comparisons of fixed curves, not parameter retuning."})
    print("reviewer frozen before results", flush=True)


def review(run):
    protocol = verify(run)
    manifest = json.loads((run / "reviewer-manifest.json").read_text())
    require(sha(Path(__file__)) == manifest["script_sha256"] and sha(run / "reviewer-source.py") == manifest["snapshot_sha256"] and
            sha(run / "protocol.json") == manifest["protocol_sha256"], "registered reviewer or protocol changed")
    state = json.loads((run / "state.json").read_text())
    require(state["status"] == "complete", "queue not complete; partial results must not be marked accepted")
    fixture = json.loads((run / "fixture.json").read_text())
    require(sha(ROOT / fixture["database"]) == fixture["sha256"], "saved index changed")
    data = ROOT / protocol["data_dir"]
    truth = struct.unpack("<2000i", (data / "truth-100000.i32").read_bytes())
    all_points, all_queries, canonical = [], [], {}
    for round_id in range(1, 3):
        hashes = json.loads((run / f"round{round_id}-hashes.json").read_text())
        for name, digest in hashes.items():
            require(sha(run / name) == digest, "completed result changed: " + name)
        points, query_rows = audit_round(run / f"round{round_id}", protocol, expected_points(protocol, round_id), truth, canonical)
        all_points.extend(dict(p, round=round_id) for p in points)
        all_queries.extend(dict(p, round=round_id) for p in query_rows)
    total = sum(p["calls"] for p in all_points)
    require(total == protocol["expected_sql_calls"], "total SQL coverage mismatch")
    gates, frontiers = [], []
    for round_id in (1, 2):
        for c in protocol["connections"]:
            point_map = {p["point"]: p for p in all_points if p["round"] == round_id and p["connections"] == c}
            full = point_map["full-r16"]
            for name in (protocol["primary_candidate"], protocol["secondary_fixed_candidate"]):
                candidate = point_map[name]
                # Every query has the same repetition count. Exact integer hit
                # comparisons preserve the registered 4-of-2000 loss tolerance.
                repeat_weight = protocol["repeats_per_worker"] * c
                target_hits = candidate["hits"] // repeat_weight
                control_hits = full["hits"] // repeat_weight
                checks = {"recall_ge_95": candidate["hits"] * 100 >= candidate["calls"] * 10 * 95,
                          "loss_le_0_2pp": target_hits >= control_hits - 4,
                          "codes_at_most_half": candidate["mean_codes"] <= protocol["rows"] / 2,
                          "p95_at_least_10pct_better_than_full": candidate["p95_ms"] <= .9 * full["p95_ms"],
                          "occupancy": candidate["occupancy_ok"], "stable_returned_ids": candidate["returned_set_variations"] == 0}
                gates.append({"round": round_id, "connections": c, "point": name, "gates": checks,
                              "passed": all(checks.values()), "loss_pp": 100 * (full["recall"] - candidate["recall"]),
                              "p95_ratio_to_full": candidate["p95_ms"] / full["p95_ms"]})
            for target in (.95, .99, .999):
                choices = {}
                for kind, modes in (("raw", {"off"}), ("pq_graph", {"pq_graph"})):
                    eligible = [p for p in point_map.values() if p["mode"] in modes and p["hits"] * 1000 >= round(target * 1000) * p["calls"] * 10]
                    best = min(eligible, key=lambda p: p["p95_ms"]) if eligible else None
                    choices[kind] = {k: best[k] for k in ("point", "recall", "p95_ms", "p99_ms", "qps")} if best else None
                frontiers.append({"round": round_id, "connections": c, "recall_target": target, **choices})
    result = {"audited_at_epoch": time.time(), "protocol_sha256": sha(run / "protocol.json"),
              "timed_sql_lists_recomputed": total, "all_points_present": True,
              "all_occupancy_checks_passed": all(p["occupancy_ok"] for p in all_points),
              "all_returned_sets_stable": all(p["returned_set_variations"] == 0 for p in all_points),
              "points": all_points, "candidate_gates": gates, "quality_frontiers": frontiers}
    save(run / "review.json", result)
    fields = [k for k in all_points[0] if k != "variation_examples"]
    with (run / "summary.csv").open("x") as out:
        writer = csv.DictWriter(out, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(all_points)
    with (run / "per-query.csv").open("x") as out:
        writer = csv.DictWriter(out, fieldnames=list(all_queries[0]))
        writer.writeheader()
        writer.writerows(all_queries)
    print("independently recomputed", total, "SQL lists; stable:", result["all_returned_sets_stable"],
          "occupancy:", result["all_occupancy_checks_passed"], flush=True)


def smoke_test(run):
    protocol = {"rows": 10000, "queries": 8, "repeats_per_worker": 1}
    points = [{"point": "raw-ef128", "mode": "off", "ef": 128, "factor": 1, "connections": 2},
              {"point": "pq-ef256-r16", "mode": "pq_graph", "ef": 256, "factor": 16, "connections": 2},
              {"point": "full-r16", "mode": "pq_only", "ef": 128, "factor": 16, "connections": 2}]
    truth = struct.unpack("<2000i", (ROOT / "build/product-tests/quantization-product-20261027/data/sift/truth-10000.i32").read_bytes())
    checked, _ = audit_round(run / "smoke", protocol, points, truth, {})
    require(sum(p["calls"] for p in checked) == 48, "smoke count mismatch")
    damaged = run / "reviewer-negative-control"
    shutil.copytree(run / "smoke", damaged)
    path = damaged / "raw-ef128-c2.csv"
    content = list(csv_rows(path))
    content[0]["ms"] = str(float(content[0]["ms"]) + .1)
    with path.open("w") as f:
        writer = csv.DictWriter(f, fieldnames=list(content[0]))
        writer.writeheader()
        writer.writerows(content)
    try:
        audit_round(damaged, protocol, points, truth, {})
    except RuntimeError as e:
        require("latency" in str(e), "negative control failed for an unrelated reason")
    else:
        raise RuntimeError("reviewer missed a deliberately corrupted latency")
    save(run / "reviewer-selftest.json", {"smoke_calls_recomputed": 48, "negative_control_latency_corruption_rejected": True})
    print("reviewer smoke and negative control passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("register", "review", "smoke"))
    parser.add_argument("--run", required=True, type=Path)
    args = parser.parse_args()
    {"register": register, "review": review, "smoke": smoke_test}[args.action](args.run.resolve())
