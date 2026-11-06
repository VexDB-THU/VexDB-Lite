"""Independently recompute filtered recall and native timing from real IDs."""
import argparse
import csv
import hashlib
import json
import math
import time
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[3]


def sha(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def csv_rows(path):
    with path.open() as f:
        return list(csv.DictReader(f))


def mask_for(name, n):
    ids = np.arange(n)
    if name == "none":
        return np.ones(n, dtype=bool)
    if name == "opposed-category":
        return ids >= 18000
    divisor = {"half": 2, "one-percent": 100, "tenth-percent": 1000, "hundredth-percent": 10000}[name]
    return ids % divisor == 0


def oracle(base, queries, filt):
    eligible = mask_for(filt, len(base))
    indices = np.flatnonzero(eligible)
    subset = base if filt == "none" else base[indices]
    squared = np.einsum("ij,ij->i", subset, subset)
    scores = np.einsum("ij,ij->i", queries, queries)[:, None] + squared[None, :] - 2 * (queries @ subset.T)
    k = min(10, len(indices))
    top = []
    boundaries = []
    for values in scores:
        # Sort ties by row ID. No extension/SQL output participates in truth.
        selected = np.lexsort((indices, values))[:k]
        top.append(set(map(int, indices[selected])))
        boundaries.append(float(values[selected[-1]]) if k else math.inf)
    return {"mask": eligible, "top": top, "boundary": boundaries, "k": k, "eligible": int(eligible.sum())}


def grade(rows, truth, base, queries):
    hits = distance_hits = returned = underfill = illegal = 0
    cache = {}
    for row in rows:
        q = int(row["query"])
        if not 0 <= q < len(queries):
            raise RuntimeError("bad query ID")
        ids = [int(s) for s in row["ids"].split()]
        if len(ids) != len(set(ids)) or len(ids) > 10:
            raise RuntimeError("duplicate IDs or excessive output")
        returned += len(ids)
        underfill += len(ids) < truth["k"]
        for id_ in ids:
            if not 0 <= id_ < len(base) or not truth["mask"][id_]:
                illegal += 1
                continue
            if id_ in truth["top"][q]:
                hits += 1
                distance_hits += 1
            else:
                key = (q, id_)
                if key not in cache:
                    difference = base[id_] - queries[q]
                    cache[key] = float(difference @ difference)
                boundary = truth["boundary"][q]
                distance_hits += cache[key] <= boundary + 1e-8 * max(1., boundary)
    denominator = len(rows) * truth["k"]
    return {"calls": len(rows), "returned": returned, "hits": hits, "denominator": denominator,
            "strict_recall": hits / denominator if denominator else 1.,
            "boundary_recall": distance_hits / denominator if denominator else 1.,
            "underfilled_calls": underfill, "illegal_ids": illegal}


def percentile(values, fraction):
    return sorted(values)[max(0, min(len(values) - 1, math.ceil(len(values) * fraction) - 1))]


def review(run, out):
    if out.exists():
        raise RuntimeError("refusing to overwrite prior review")
    out.mkdir(parents=True)
    protocol = json.loads((run / "protocol.json").read_text())
    state = json.loads((run / "state.json").read_text())
    if state.get("active_pid") or state.get("controller_pid"):
        raise RuntimeError("wait for native queue to stop")
    for name, digest in protocol["inputs"].items():
        if sha(ROOT / name) != digest:
            raise RuntimeError("frozen input changed: " + name)
    for manifest in run.glob("*-hashes.json"):
        for name, digest in json.loads(manifest.read_text()).items():
            if sha(run / name) != digest:
                raise RuntimeError("completed output changed: " + name)
    results, quality_results, missing, flags = [], [], [], []
    total_calls = 0
    for dataset in protocol["datasets"]:
        print("ORACLE", dataset["name"], flush=True)
        data = ROOT / dataset["data"]
        base = np.fromfile(data / "base.f32", dtype="<f4", count=dataset["rows"] * dataset["dim"]).reshape(-1, dataset["dim"]).astype(np.float64)
        queries = np.fromfile(data / "queries.f32", dtype="<f4", count=128 * dataset["dim"]).reshape(128, dataset["dim"]).astype(np.float64)
        truths = {f: oracle(base, queries, f) for f in dataset["filters"]}
        np.savez(out / (dataset["name"] + "-oracle.npz"), **{
            f + "_top_ids": np.array([sorted(s) for s in t["top"]], dtype=np.int32)
            for f, t in truths.items()}, **{f + "_boundary": t["boundary"] for f, t in truths.items()})
        for round_id in (1, 2):
            for version in ("baseline", "new"):
                name = f"{dataset['name']}-{version}-r{round_id}"
                path = run / name
                config = []
                for line in (run / (name + ".points")).read_text().splitlines():
                    fields = line.split()
                    config.append({"point": fields[0], "method": fields[0].split("-")[0], "filter": fields[5], "connections": int(fields[6]), "expected_scan": int(fields[7])})
                summary_path = path / "points.csv"
                observed = {r["point"]: r for r in csv_rows(summary_path)} if summary_path.exists() else {}
                for p in config:
                    point = p["point"]
                    if point not in observed:
                        missing.append(name + "/" + point)
                        continue
                    s = observed[point]
                    rows = csv_rows(path / (point + ".timing.csv"))
                    expected = {(w, q) for w in range(p["connections"]) for q in range(32)}
                    if {(int(r["worker"]), int(r["query"])) for r in rows} != expected or len(rows) != len(expected):
                        raise RuntimeError("bad worker/query coverage " + name + "/" + point)
                    wall = int(s["wall_ns"])
                    latencies = []
                    for r in rows:
                        begin, end = int(r["begin_ns"]), int(r["end_ns"])
                        if not 0 <= begin <= end <= wall:
                            raise RuntimeError("invalid request timestamp")
                        latencies.append((end - begin) / 1e6)
                    qps = len(rows) * 1e9 / wall
                    if abs(qps - float(s["qps"])) > 1e-8 * max(1, qps):
                        raise RuntimeError("QPS recomputation disagrees")
                    for fraction, label in ((.5, "p50_ms"), (.95, "p95_ms"), (.99, "p99_ms")):
                        if abs(percentile(latencies, fraction) - float(s[label])) > 1e-7:
                            raise RuntimeError("latency recomputation disagrees")
                    scan = "VEXDB_INDEX_SCAN" in (path / (point + ".plan.txt")).read_text()
                    if scan != bool(int(s["index_scan"])) or (p["expected_scan"] >= 0 and scan != bool(p["expected_scan"])):
                        raise RuntimeError("query plan disagrees")
                    grade_ = grade(rows, truths[p["filter"]], base, queries)
                    meta = {"dataset": dataset["name"], "version": version, "round": round_id, **p,
                            "eligible_rows": truths[p["filter"]]["eligible"], "index_scan": scan}
                    results.append({**meta, **grade_, **{key: float(s[key]) for key in (
                        "qps", "p50_ms", "p95_ms", "p99_ms", "cpu_cores", "mean_active_clients", "overlap_ns", "rss_before", "rss_after")}})
                    total_calls += len(rows)
                    if float(s["mean_active_clients"]) < .8 * p["connections"]:
                        flags.append(name + "/" + point + ": average occupied clients below80%")
                    if p["connections"] > 1 and int(s["overlap_ns"]) <= 0:
                        flags.append(name + "/" + point + ": no all-worker overlap")
                    quality = path / (point + ".quality.csv")
                    if quality.exists():
                        qrows = csv_rows(quality)
                        if len(qrows) != 128 or {int(r["query"]) for r in qrows} != set(range(128)):
                            raise RuntimeError("incomplete quality set")
                        quality_results.append({**meta, **grade(qrows, truths[p["filter"]], base, queries)})
        del base, queries
    for name, values in (("timed-points.csv", results), ("quality.csv", quality_results)):
        if values:
            with (out / name).open("x") as f:
                w = csv.DictWriter(f, fieldnames=list(values[0]))
                w.writeheader()
                w.writerows(values)
    new_filtered = [r for r in results + quality_results if r["version"] == "new" and r["filter"] != "none"]
    quality_failures = [{k: r[k] for k in ("dataset", "round", "point", "strict_recall", "underfilled_calls", "illegal_ids")}
                        for r in new_filtered if r["strict_recall"] < .999 or r["underfilled_calls"] or r["illegal_ids"]]
    report = {"reviewed_epoch": time.time(), "queue_status": state["status"],
              "expected_timed_calls": protocol["expected_timed_calls"], "observed_timed_calls": total_calls,
              "timed_points": len(results), "quality_sets": len(quality_results),
              "quality_lists": sum(r["calls"] for r in quality_results),
              "missing": missing, "flags": flags, "new_filtered_failures": quality_failures,
              "new_filtered_quality_pass": bool(new_filtered) and not quality_failures and not missing,
              "speed_claim": "No universal speed gate. Compare matched-quality exact; low recall or underfilled ANN cannot establish a speed win.",
              "reviewer_sha256": sha(Path(__file__)), "numpy": np.__version__}
    (out / "review.json").write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2), flush=True)


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    args = p.parse_args()
    review(args.run.resolve(), args.out.resolve())
