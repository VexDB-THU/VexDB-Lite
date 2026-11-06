"""Freeze and run a bounded comparison of the constant-array distance path.

Uses the existing native filtered-SQL harness without changing its timing or
quality collection. This is a new study, never a resumption of old queues.
"""
import argparse
import json
import os
import platform
import shutil
import time
from pathlib import Path

from run_filtered_search import ROOT, alive, job, save, sha, update, verify


def init(run):
    if (run / "protocol.json").exists():
        raise RuntimeError("already registered")
    if shutil.disk_usage(run).free < 4 * 2**30:
        raise RuntimeError("need 4 GiB free")
    # All three fixtures already exist. No training, regeneration or tuning.
    prior = ROOT / "build/product-tests/pq-filter-generalization-20261027"
    shutil.copyfile(prior / "benchmark", run / "benchmark")
    (run / "benchmark").chmod(0o755)
    public = "build/product-tests/quantization-product-20261027/data"
    datasets = [
        {"name": "text", "rows": 100000, "dim": 1536,
         "data": public + "/dbpedia",
         "database": "build/product-tests/text-100k-concurrency-20261027/fixture/vectors.duckdb"},
        {"name": "sift", "rows": 100000, "dim": 128,
         "data": public + "/sift",
         "database": "build/product-tests/pq-graph-20261027-v3/fixtures/sift-100000/vectors.duckdb"},
        {"name": "catalog", "rows": 20000, "dim": 64,
         "data": str((prior / "catalog-data").relative_to(ROOT)),
         "database": str((prior / "catalog-fixture/vectors.duckdb").relative_to(ROOT))},
    ]
    points, configs = [], []
    for connections in (1, 16, 64):
        for filt in ("half", "one-percent", "hundredth-percent"):
            points.append(f"exact-{filt}-c{connections} pq_graph 1536 16 0 {filt} {connections} 0\n")
        points.append(f"raw-none-c{connections} off 256 1 0 none {connections} 1\n")
        points.append(f"pq-none-c{connections} pq_graph 1536 16 0 none {connections} 1\n")
    for dataset in datasets:
        dataset["filters"] = ["none", "half", "one-percent", "hundredth-percent"]
        for version in ("baseline", "new"):
            for round_id in (1, 2):
                path = run / f"{dataset['name']}-{version}-r{round_id}.points"
                with path.open("x") as f:
                    f.write("".join(points if round_id == 1 else points[::-1]))
                configs.append(path)

    names = set(json.loads((prior / "baseline-source-manifest.json").read_text()))
    names.update([
        "vexdb_duckdb/test/benchmark/run_distance_constant_reuse.py",
        "vexdb_duckdb/test/benchmark/run_filtered_search.py",
        "vexdb_duckdb/test/benchmark/review_filtered_search.py",
        "vexdb_duckdb/test/benchmark/vex_filtered_search_benchmark.cpp",
        "vexdb_duckdb/test/benchmark/vex_quantization_product_benchmark.cpp",
        "vexdb_duckdb/test/distance_constant_reuse_regression.py",
        "tests/spec/duckdb/functions/distance_constant_reuse.yaml",
    ])
    inputs = {}
    for name in sorted(names):
        dest = run / "frozen-source" / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / name, dest)
        inputs[name] = sha(ROOT / name)
        inputs[str(dest.relative_to(ROOT))] = sha(dest)
    paths = configs + [run / "benchmark", run / "vexdb_lite.duckdb_extension",
        run / "baseline/vexdb_lite.duckdb_extension", run / "distance_functions.before.cpp",
        run / "extension-build.json", run / "build_extension.py", run / "preparation.json",
        run / "scalar-parity.json",
        ROOT / "docs/plans/2026-10-28_distance-constant-reuse-validation.md",
        ROOT / "build/product-tests/pq-integration-20261027/duckdb-sdk/libduckdb.dylib",
        ROOT / "build/product-tests/pq-integration-20261027/duckdb-sdk/duckdb.hpp"]
    for d in datasets:
        paths += [ROOT / d["data"] / name for name in ("base.f32", "queries.f32", "manifest.json")]
        paths.append(ROOT / d["database"])
    for path in paths:
        inputs[str(path.relative_to(ROOT))] = sha(path)
    registered = time.time()
    save(run / "protocol.json", {
        "registered_epoch": registered, "deadline_epoch": registered + 3600,
        "disk_floor_bytes": 4 * 2**30, "job_timeout_seconds": 1200,
        "datasets": datasets, "quality_queries": 128, "timed_queries_per_worker": 32,
        "connections": [1, 16, 64], "rounds": 2, "expected_timed_calls": 155520,
        "platform": platform.platform(), "base_commit": "a614157cb2",
        "scope": "Constant-query array reuse only. Existing used public proxy queries and explicitly synthetic catalog; unchanged saved indexes, raw vector bytes, SQL, filters and ANN settings. No overnight holdout or old deadline reuse.",
        "quality": "Independent float64 exhaustive qualifying-subset L2. Strict ID Recall@min(10,eligible) primary; filtered quality >=99.9%, no underfill or illegal IDs, baseline 100% must not decline. Compare every actual old/new per-query ID set, including controls and changing parameters.",
        "timing": "Unchanged native prepared SQL execute/materialize/fetch/duplicate validation. One outstanding query per connection, 32 shuffled requests per worker, same fixed queries. Warmup/prepare excluded. threads=1; local closed-loop SDK, not network QPS. Reverse points and dataset/version process order in round 2. Preserve low occupancy and short runs.",
        "primary_speed_gate": {"dataset": "text", "filter": "half", "connections": [16, 64],
            "rounds": [1, 2], "min_qps_ratio": 1.2, "max_p95_ratio": 0.8,
            "rule": "Each of all four matched points must satisfy both thresholds, with unchanged quality. Report every control and slower point; no post-result tuning."},
        "inputs": inputs, "sources": sorted(names),
    })
    update(run, {"status": "registered", "jobs": [], "controller_pid": None, "active_pid": None})
    print("REGISTERED 180 points, 155520 timed SQL requests; new one-hour window", flush=True)


def execute(run):
    protocol = verify(run)
    state = json.loads((run / "state.json").read_text())
    if state["status"] == "complete":
        return
    if alive(state.get("controller_pid")) or alive(state.get("active_pid")) or state["jobs"]:
        raise RuntimeError("active/prior queue; no duplicate or blind resume")
    if time.time() >= protocol["deadline_epoch"]:
        raise RuntimeError("deadline expired")
    state["controller_pid"] = os.getpid()
    update(run, state)
    try:
        for round_id in (1, 2):
            datasets = protocol["datasets"] if round_id == 1 else protocol["datasets"][::-1]
            for dataset in datasets:
                for version in (["baseline", "new"] if round_id == 1 else ["new", "baseline"]):
                    name = f"{dataset['name']}-{version}-r{round_id}"
                    extension = run / ("baseline/vexdb_lite.duckdb_extension" if version == "baseline" else "vexdb_lite.duckdb_extension")
                    job(run, state, protocol, name, [run / "benchmark", "run", extension,
                        ROOT / dataset["database"], ROOT / dataset["data"], str(dataset["dim"]),
                        "128", run / (name + ".points"), run / name])
                    output = run / name
                    save(run / (name + "-hashes.json"), {
                        str(p.relative_to(run)): sha(p) for p in output.rglob("*") if p.is_file()})
        verify(run)
        state.update(status="complete", controller_pid=None)
        update(run, state)
    except BaseException as e:
        state.update(status="failed", error=str(e), controller_pid=None)
        update(run, state)
        raise


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("init", "run", "verify"))
    parser.add_argument("--run", required=True, type=Path)
    args = parser.parse_args()
    {"init": init, "run": execute, "verify": verify}[args.action](args.run.resolve())
