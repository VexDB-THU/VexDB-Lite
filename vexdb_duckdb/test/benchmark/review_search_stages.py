#!/usr/bin/env python3
"""Recompute quality, clocks and stage summaries from immutable SQL records."""
import argparse
import collections
import csv
import hashlib
import json
import math
from pathlib import Path


def percentile(values, fraction):
    return sorted(values)[math.ceil(fraction * len(values)) - 1]


def review(evidence, truth_file, out, phase):
    manifest_file = evidence / "final-evidence-manifest.json"
    if not manifest_file.exists():
        manifest_file = evidence / "evidence-manifest.json"
    manifest = json.loads(manifest_file.read_text())
    for name, digest in manifest.items():
        assert hashlib.sha256((evidence / name).read_bytes()).hexdigest() == digest, name
    root = evidence / phase
    state = json.loads((root / "state.json").read_text())
    protocol = json.loads((root / "protocol.json").read_text())
    truth = json.loads(truth_file.read_text())
    assert state["status"] == "complete"
    assert state["ended"] < protocol["deadline_epoch"]
    assert [j["name"] for j in state["jobs"]] == [j["name"] for j in protocol["jobs"]]
    points, total, total_hits = [], 0, 0
    work_counts = collections.defaultdict(set)
    stages = ["prepare_ns", "prefilter_ns", "queue_wait_ns", "mask_ns", "search_ns",
              "visibility_ns", "exact_ns", "fetch_ns", "output_ns", "graph_lock_ns",
              "pq_prepare_ns", "pq_navigation_ns", "pq_distance_ns", "pq_refine_ns", "raw_navigation_ns"]
    for job in state["jobs"]:
        assert job["status"] == "complete" and job["exit_code"] == 0
        rows = list(csv.DictReader((root / job["name"] / "requests.csv").open()))
        summary = json.loads((root / job["name"] / "summary.json").read_text())
        cohort = "catalog-new" if job["shape"] == "catalog" else "text-new"
        prefix = {"broad": "broad-acl", "tenant": "tenant-acl", "catalog": "catalog-available-price"}[job["shape"]]
        counts = collections.defaultdict(collections.Counter)
        query_hits = collections.Counter()
        query_counts = collections.Counter()
        latencies, hits = [], 0
        for row in rows:
            client, ordinal, query = (int(row[n]) for n in ("client", "ordinal", "query"))
            assert query == (ordinal + client) % 8
            counts[client][query] += 1
            ids = list(map(int, row["ids"].split()))
            assert len(ids) == len(set(ids)) == 10
            matched = len(set(ids) & set(truth[cohort][prefix + ":" + str(query)]["ids"]))
            query_hits[query] += matched
            query_counts[query] += 1
            hits += matched
            elapsed = int(row["end_ns"]) - int(row["begin_ns"])
            assert elapsed > 0 and int(row["thread_cpu_ns"]) >= 0
            assert int(row["failed"]) == 0
            assert int(row["operator_ns"]) <= elapsed
            assert int(row["search_ns"]) <= int(row["operator_ns"])
            assert int(row["retry_search_ns"]) <= int(row["search_ns"])
            assert int(row["pq_distance_ns"]) <= int(row["search_ns"])
            profiled = job["enabled"] and job["shape"] != "tenant"
            assert int(row["operators"]) == int(bool(profiled))
            if profiled:
                assert int(row["output_rows"]) == 10
                assert int(row["search_calls"]) == 1
                assert int(row["retry_calls"]) == int(row["snapshot_rejected"]) == 0
                assert int(row["metadata_rows"]) == (20000 if cohort == "catalog-new" else 100000)
                assert int(row["routes"]) == (1 if cohort == "catalog-new" else 2)
                if cohort == "text-new":
                    assert int(row["graph_candidates"]) == int(row["raw_vectors_refined"]) == 1536
                    assert int(row["allowed_rows"]) == 50000
                work_counts[(job["shape"], query)].add(tuple(int(row[k]) for k in (
                    "search_calls", "pq_codes_scored", "graph_candidates", "raw_vectors_refined", "allowed_rows")))
            latencies.append(elapsed)
        assert len(counts) == job["clients"]
        assert all(len(c) == 8 and len(set(c.values())) == 1 for c in counts.values())
        assert len(rows) == summary["calls"]
        assert abs(summary["qps"] - len(rows) * 1e9 / summary["wall_ns"]) < 1e-8
        p95 = percentile(latencies, .95)
        assert abs(summary["p95_ms"] - p95 / 1e6) < 1e-8
        recall = hits / (10 * len(rows))
        by_query = {str(q): query_hits[q] / (query_counts[q] * 10) for q in query_counts}
        assert recall >= protocol["recall_min"] and min(by_query.values()) >= protocol["recall_min"]
        tail = [r for r, t in zip(rows, latencies) if t >= p95]
        tail_wall = sum(int(r["end_ns"]) - int(r["begin_ns"]) for r in tail)
        point = {k: job[k] for k in ("name", "shape", "mode", "clients", "enabled", "cap")}
        point.update(summary=summary, recall=recall, recall_by_query=by_query,
                     mean_active_queries=sum(latencies) / summary["wall_ns"],
                     requests_started_after_nominal_window_pct=sum(int(r["begin_ns"]) > protocol["seconds"] * 1e9 for r in rows) / len(rows) * 100,
                     tail_ended_after_nominal_window_pct=sum(int(r["end_ns"]) > protocol["seconds"] * 1e9 for r in tail) / len(tail) * 100,
                     mean_cpu_ms=sum(int(r["thread_cpu_ns"]) for r in rows) / len(rows) / 1e6,
                     thread_cpu_seconds_per_window_second=sum(int(r["thread_cpu_ns"]) for r in rows) / summary["wall_ns"],
                     mean_stage_ms={k: sum(int(r[k]) for r in rows) / len(rows) / 1e6 for k in stages},
                     tail_stage_share_pct={k: sum(int(r[k]) for r in tail) / tail_wall * 100 for k in stages},
                     tail_outside_operator_pct=sum(int(r["end_ns"]) - int(r["begin_ns"]) - int(r["operator_ns"]) for r in tail) / tail_wall * 100,
                     mean_codes=sum(int(r["pq_codes_scored"]) for r in rows) / len(rows),
                     search_calls=sum(int(r["search_calls"]) for r in rows),
                     retry_calls=sum(int(r["retry_calls"]) for r in rows),
                     exact_fallback_calls=sum(int(r["exact_fallback_calls"]) for r in rows))
        points.append(point)
        total += len(rows)
        total_hits += hits
    lookup = {p["name"]: p for p in points}
    aa = []
    for a, b in [("aa-pq64-a1", "aa-pq64-b1"), ("aa-pq64-a2", "aa-pq64-b2")]:
        x, y = lookup[a]["summary"], lookup[b]["summary"]
        ratio = y["qps"] / x["qps"]
        aa.append(dict(a=a, b=b, qps_ratio=ratio, p95_ratio=y["p95_ms"] / x["p95_ms"], passed=.97 <= ratio <= 1.03))
    result = dict(phase=phase, verified_evidence_files=len(manifest), timed_sql=total, recall=total_hits / (total * 10),
                  per_query_work_counts_stable=all(len(v) == 1 for v in work_counts.values()),
                  aa=aa, throughput_aa_passed=all(a["passed"] for a in aa),
                  speedup_claim_allowed=False, points=points,
                  scope="exposed8query regression; diagnostic closed-loop load; profiled wall-clock substages overlap; raw warmup IDs not retained")
    out.mkdir(exist_ok=False, parents=True)
    (out / "review.json").write_text(json.dumps(result, indent=2) + "\n")
    with (out / "points.csv").open("w") as f:
        writer = csv.writer(f)
        writer.writerow(["point", "clients", "profile", "qps", "p95_ms", "mean_cpu_ms", "recall", "retry_calls"])
        for p in points:
            writer.writerow([p["name"], p["clients"], p["enabled"], p["summary"]["qps"], p["summary"]["p95_ms"], p["mean_cpu_ms"], p["recall"], p["retry_calls"]])
    print(json.dumps({k: result[k] for k in ("verified_evidence_files", "timed_sql", "recall", "aa", "speedup_claim_allowed")}))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--evidence", type=Path, required=True)
    ap.add_argument("--truth", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--phase", default="diagnostic-v1")
    args = ap.parse_args()
    review(args.evidence, args.truth, args.out, args.phase)
