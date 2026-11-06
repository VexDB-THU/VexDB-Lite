"""Bounded read-only comparison on saved product indexes and existing dev queries."""
import argparse
import csv
import hashlib
import json
import math
import statistics
import struct
import subprocess
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def sha(path):
    with Path(path).open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def save(path, value):
    with path.open("x") as f:
        json.dump(value, f, indent=2)


def rows(path):
    with path.open() as f:
        return list(csv.DictReader(f))


def now():
    return datetime.now(timezone.utc).isoformat()


def verify(run):
    protocol = json.loads((run / "protocol.json").read_text())
    for path, digest in protocol["inputs"].items():
        if sha(ROOT / path) != digest:
            raise RuntimeError("frozen input changed: " + path)
    return protocol


def initialize(run):
    if (run / "protocol.json").exists():
        raise RuntimeError("protocol already exists")
    cases = [
        {"name": "sift-100000", "data": "sift", "rows": 100000, "dim": 128,
         "db": str(run.relative_to(ROOT) / "fixtures/sift-100000/vectors.duckdb"), "primary": True},
        {"name": "dbpedia-10000", "data": "dbpedia", "rows": 10000, "dim": 1536,
         "db": str(run.relative_to(ROOT) / "fixtures/dbpedia-10000/vectors.duckdb"), "primary": True},
        {"name": "sift-10000", "data": "sift", "rows": 10000, "dim": 128,
         "db": str(run.relative_to(ROOT) / "fixtures/sift-10000/vectors.duckdb"), "primary": False},
    ]
    sources = set()
    for record in (run / "objects").glob("*.json"):
        sources.add(Path(json.loads(record.read_text())["source"]))
    sources.update(p.relative_to(ROOT) for p in (ROOT / "vexdb_duckdb/include").rglob("*") if p.is_file())
    sources.update(p.relative_to(ROOT) for p in (ROOT / "common").rglob("*") if p.is_file())
    sources.update(Path(p) for p in (
        "common/include/graph_index/graph_index_algorithm.h", "common/quantizer/product_quantizer.h",
        "vexdb_duckdb/test/benchmark/vex_quantization_product_benchmark.cpp",
        "vexdb_duckdb/test/benchmark/vex_pq_graph_benchmark.cpp",
        "vexdb_duckdb/test/benchmark/run_pq_graph.py"))
    snapshot = run / "frozen-source"
    for path in sorted(sources):
        dest = snapshot / path
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes((ROOT / path).read_bytes())
    files = set(sources) | {run.relative_to(ROOT) / "benchmark", run.relative_to(ROOT) / "vexdb_lite.duckdb_extension"}
    files.update(run.relative_to(ROOT) / p for p in ("fixture-build-records.json", "model-parity.json", "prepare_fixtures.py"))
    for case in cases:
        data = Path("build/product-tests/quantization-product-20261027/data") / case["data"]
        case["data_dir"] = str(data)
        files.update([Path(case["db"]), data / "queries.f32", data / f"truth-{case['rows']}.i32"])
    save(run / "protocol.json", {
        "registered_at": now(), "cases": cases, "queries": {"fit": [0, 100], "validation": [100, 100]},
        "note": "Both slices are previously used development queries, not fresh held-out queries. No overnight holdout is opened.",
        "grid": {"ef": [64, 128, 256, 512], "factor": [4, 16]},
        "selection": "One common ef/factor. On both primary fit cases: >=950/1000 hits, loss <=2/1000 versus same-factor full PQ, mean code distance evaluations <=50% of row count, and P95 <=90% of full PQ. Choose smallest geometric mean P95 ratio, then smaller ef/factor. No parameter changes after validation.",
        "validation": "Frozen candidate, same-factor full PQ and raw graph ef=128/512; 1/8 connections; three passes; two independent processes per case in reversed point order. Quality/visit gates apply per case; speed gate must hold in both rounds at each concurrency. Raw graph is a strong control, not the selection baseline.",
        "memory": "Saved table, raw vector tier, graph and PQ remain charged. Node-code slot cache is an additional 8 bytes per node plus its object; it does not reduce total index memory. RSS includes the whole reopened process and is order-dependent.",
        "boundary": "macOS ARM64 DuckDB 1.5.2 SDK; freshly rebuilt fixtures to correct reserved-node persistence; same frozen training/codebooks verified separately; one graph seed; 100K text absent; code counters are a separate untimed native audit, never ANN QPS.",
        "sources": [str(p) for p in sorted(sources)],
        "inputs": {str(p): sha(ROOT / p) for p in sorted(files)},
    })
    print("registered protocol", flush=True)


def audit(run, protocol, phase):
    result = {}
    for directory in sorted(p for p in (run / phase).glob("*") if p.is_dir()):
        case_name = directory.name.split("-round")[0]
        case = next(c for c in protocol["cases"] if c["name"] == case_name)
        offset, count = protocol["queries"]["fit" if phase == "fit" else "validation"]
        truth_file = ROOT / case["data_dir"] / f"truth-{case['rows']}.i32"
        truth = struct.unpack("<2000i", truth_file.read_bytes())
        native = {}
        for r in rows(directory / "counts.csv"):
            key = r["point"], int(r["query"])
            entry = (int(r["codes_scored"]), int(r["raw_vectors_refined"]), set(map(int, r["ids"].split())))
            if key in native and native[key] != entry:
                raise RuntimeError("native audit changed between connections")
            native[key] = entry
        points = {}
        for summary in rows(directory / "points.csv"):
            point, connections = summary["point"], int(summary["connections"])
            raw = rows(directory / f"{point}-c{connections}.csv")
            if len(raw) != count * 3:
                raise RuntimeError("missing queries")
            hits, latencies, seen, visits, refined = 0, [], set(), [], []
            for r in raw:
                q, repeat = int(r["query"]), int(r["repeat"])
                if not 0 <= q < count or (repeat, q) in seen or repeat not in (0, 1, 2):
                    raise RuntimeError("duplicate or invalid sample")
                seen.add((repeat, q))
                ids = list(map(int, r["ids"].split()))
                if len(ids) != 10 or len(set(ids)) != 10 or any(i < 0 or i >= case["rows"] for i in ids):
                    raise RuntimeError("invalid returned IDs")
                correct = set(truth[(offset + q) * 10:(offset + q + 1) * 10])
                hit = len(set(ids) & correct)
                if hit != int(r["hits"]):
                    raise RuntimeError("stored recall mismatch")
                hits += hit
                latencies.append(float(r["ms"]))
                if (point, q) in native:
                    v, f, actual = native[point, q]
                    if set(ids) != actual:
                        raise RuntimeError("SQL and native audit IDs differ")
                    visits.append(v)
                    refined.append(f)
                elif not point.startswith("raw-"):
                    raise RuntimeError("missing native distance counts")
            p95 = sorted(latencies)[math.ceil(.95 * len(latencies)) - 1]
            recall = hits / (count * 3 * 10)
            if abs(recall - float(summary["recall10"])) > 1e-10 or abs(p95 - float(summary["p95_ms"])) > 1e-8:
                raise RuntimeError("independent statistic mismatch")
            if abs(float(summary["qps"]) - len(raw) * 1000 / float(summary["wall_ms"])) > 1e-5:
                raise RuntimeError("QPS arithmetic mismatch")
            points[f"{point}-c{connections}"] = {
                "recall": recall, "hits": hits // 3, "p95_ms": p95, "p50_ms": statistics.median(latencies),
                "qps": float(summary["qps"]), "calls": len(raw), "first_ms": float(summary["first_ms"]),
                "rss_warm": int(summary["rss_warm"]),
                "mean_codes": statistics.mean(visits) if visits else None,
                "max_codes": max(visits) if visits else None,
                "mean_refined": statistics.mean(refined) if refined else None,
            }
        result[directory.name] = points
    return result


def execute_phase(run, phase):
    protocol = verify(run)
    base = run / phase
    base.mkdir(exist_ok=False)
    if phase == "fit":
        points = [(f"full-r{r}", "pq_only", 128, r, 1) for r in (4, 16)]
        points += [(f"graph-ef{ef}-r{r}", "pq_graph", ef, r, 1) for r in (4, 16) for ef in (64, 128, 256, 512)]
        points += [(f"raw-ef{ef}", "off", ef, 1, 1) for ef in (128, 512)]
        rounds = 1
    else:
        selection = json.loads((run / "selection.json").read_text())
        if sha(run / "selection.json") != (run / "selection.sha256").read_text().strip():
            raise RuntimeError("frozen selection changed")
        if not selection["passed"]:
            raise RuntimeError("fit gate failed; validation remains unused by this experiment")
        ef, r = selection["ef"], selection["factor"]
        points = [(point, mode, width, factor, c) for c in (1, 8) for point, mode, width, factor in (
            (f"graph-ef{ef}-r{r}", "pq_graph", ef, r), (f"full-r{r}", "pq_only", 128, r),
            ("raw-ef128", "off", 128, 1), ("raw-ef512", "off", 512, 1))]
        rounds = 2
    offset, count = protocol["queries"]["fit" if phase == "fit" else "validation"]
    for case in protocol["cases"]:
        for round_id in range(1, rounds + 1):
            name = case["name"] + (f"-round{round_id}" if phase != "fit" else "")
            config = base / (name + ".points")
            config.write_text("".join(" ".join(map(str, point)) + "\n" for point in (points if round_id == 1 else points[::-1])))
            cmd = [str(run / "benchmark"), str(run / "vexdb_lite.duckdb_extension"), str(ROOT / case["db"]),
                   str(ROOT / case["data_dir"]), str(case["rows"]), str(case["dim"]), str(offset), str(count),
                   str(config), str(base / name)]
            print("START", phase, name, flush=True)
            with (base / (name + ".log")).open("x") as log:
                subprocess.run(cmd, check=True, stdout=log, stderr=subprocess.STDOUT)
            print("DONE", phase, name, flush=True)
    verify(run)
    save(run / (phase + "-audit.json"), {"audited_at": now(), "results": audit(run, protocol, phase)})
    save(run / (phase + "-hashes.json"), {str(p.relative_to(run)): sha(p) for p in base.rglob("*") if p.is_file()})


def select(run):
    protocol = verify(run)
    fit = audit(run, protocol, "fit")
    candidates = []
    for factor in protocol["grid"]["factor"]:
        for ef in protocol["grid"]["ef"]:
            comparisons = {}
            passed = True
            ratios = []
            for case in protocol["cases"]:
                if not case["primary"]:
                    continue
                data = fit[case["name"]]
                candidate = data[f"graph-ef{ef}-r{factor}-c1"]
                control = data[f"full-r{factor}-c1"]
                ratio = candidate["p95_ms"] / control["p95_ms"]
                good = candidate["hits"] >= 950 and candidate["hits"] >= control["hits"] - 2 and candidate["mean_codes"] <= case["rows"] * .5 and ratio <= .9
                comparisons[case["name"]] = {"candidate": candidate, "control": control, "p95_ratio": ratio, "passed": good}
                passed &= good
                ratios.append(ratio)
            candidates.append({"ef": ef, "factor": factor, "passed": passed,
                               "score": math.prod(ratios) ** (1 / len(ratios)), "cases": comparisons})
    eligible = [c for c in candidates if c["passed"]]
    chosen = min(eligible, key=lambda c: (c["score"], c["ef"], c["factor"])) if eligible else {"passed": False}
    save(run / "selection.json", {"frozen_at": now(), **chosen, "all_candidates": candidates})
    (run / "selection.sha256").write_text(sha(run / "selection.json") + "\n")
    print(json.dumps({k: v for k, v in chosen.items() if k != "cases"}), flush=True)


def review(run):
    protocol = verify(run)
    for phase in ("fit", "validation"):
        manifest = run / (phase + "-hashes.json")
        if not manifest.exists():
            continue
        for path, digest in json.loads(manifest.read_text()).items():
            if sha(run / path) != digest:
                raise RuntimeError("result changed: " + path)
        checked = audit(run, protocol, phase)
        print(phase, "independently audited", sum(v["calls"] for points in checked.values() for v in points.values()), "SQL top-10 lists", flush=True)
    print("frozen inputs and result hashes verified", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("init", "fit", "select", "validation", "review"))
    parser.add_argument("--run", type=Path, required=True)
    args = parser.parse_args()
    run = args.run.resolve()
    {"init": initialize, "fit": lambda r: execute_phase(r, "fit"), "select": select,
     "validation": lambda r: execute_phase(r, "validation"), "review": review}[args.action](run)
