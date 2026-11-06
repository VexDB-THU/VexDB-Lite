"""Bounded, source-frozen filtered SQL comparison; never resumes old studies."""
import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def sha(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def save(path, value):
    with path.open("x") as f:
        json.dump(value, f, indent=2)


def update(run, state):
    state["updated_epoch"] = time.time()
    temp = run / "state.tmp"
    temp.write_text(json.dumps(state, indent=2))
    temp.replace(run / "state.json")


def init(run):
    import numpy as np
    if (run / "protocol.json").exists():
        raise RuntimeError("already registered")
    if shutil.disk_usage(run).free < 6 * 2**30:
        raise RuntimeError("need 6 GiB before preparing the fixture")
    data = run / "catalog-data"
    data.mkdir(exist_ok=False)
    rng = np.random.default_rng(20261027)
    # Ten product categories with correlated embeddings; all queries concern
    # category 0, while the opposed-category filter deliberately requests 9.
    base = (rng.normal(0, .3, (20000, 64)) + (np.arange(20000) // 2000)[:, None] * 2).astype("<f4")
    queries = rng.normal(0, .3, (128, 64)).astype("<f4")
    base.tofile(data / "base.f32")
    queries.tofile(data / "queries.f32")
    save(data / "manifest.json", {"rows": 20000, "dim": 64, "queries": 128,
         "scope": "Synthetic clustered catalog, not customer data; category=id//2000; independent tenant predicates=id%divisor.",
         "rng": "numpy default_rng 20261027", "numpy": np.__version__,
         "files": {p.name: sha(p) for p in data.iterdir() if p.is_file()}})
    public = ROOT / "build/product-tests/quantization-product-20261027/data"
    datasets = [
        {"name": "sift", "rows": 100000, "dim": 128, "data": str((public / "sift").relative_to(ROOT)),
         "database": "build/product-tests/pq-graph-20261027-v3/fixtures/sift-100000/vectors.duckdb"},
        {"name": "text", "rows": 100000, "dim": 1536, "data": str((public / "dbpedia").relative_to(ROOT)),
         "database": "build/product-tests/text-100k-concurrency-20261027/fixture/vectors.duckdb"},
        {"name": "catalog", "rows": 20000, "dim": 64, "data": str(data.relative_to(ROOT)),
         "database": str((run / "catalog-fixture/vectors.duckdb").relative_to(ROOT))},
    ]
    common_filters = ["none", "half", "one-percent", "tenth-percent", "hundredth-percent"]
    configs = []
    total_calls = 0
    for dataset in datasets:
        filters = common_filters + (["opposed-category"] if dataset["name"] == "catalog" else [])
        dataset["filters"] = filters
        for version in ("baseline", "new"):
            methods = [("raw", "off", 256, 1, 0), ("pq", "pq_graph", 1536, 16, 0),
                       ("exact", "off", 256, 1, 1000000)] if version == "baseline" else [("pq", "pq_graph", 1536, 16, 0)]
            points = []
            for connections in (1, 16, 64):
                for filt in filters:
                    selected = methods + ([("raw", "off", 256, 1, 0)] if version == "new" and filt == "none" else [])
                    for method, mode, ef, factor, threshold in selected:
                        expected = 0 if threshold or (version == "new" and filt != "none") else (1 if filt == "none" else -1)
                        name = f"{method}-{filt}-c{connections}"
                        points.append(f"{name} {mode} {ef} {factor} {threshold} {filt} {connections} {expected}\n")
                        total_calls += 2 * 32 * connections
            for round_id in (1, 2):
                path = run / f"{dataset['name']}-{version}-r{round_id}.points"
                path.write_text("".join(points if round_id == 1 else points[::-1]))
                configs.append(path)
    names = set(json.loads((run / "baseline-source-manifest.json").read_text()))
    names.update([
        "vexdb_duckdb/test/benchmark/vex_filtered_search_benchmark.cpp",
        "vexdb_duckdb/test/benchmark/vex_quantization_product_benchmark.cpp",
        "vexdb_duckdb/test/benchmark/run_filtered_search.py",
        "vexdb_duckdb/test/benchmark/review_filtered_search.py",
        "vexdb_duckdb/test/filtered_search_integration_test.cpp",
        "vexdb_duckdb/test/pq_center_policy_test.cpp",
    ])
    for name in names:
        dest = run / "frozen-source" / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / name, dest)
    inputs = {name: sha(ROOT / name) for name in sorted(names)}
    for d in datasets:
        for name in ("base.f32", "queries.f32", "manifest.json"):
            p = ROOT / d["data"] / name
            inputs[str(p.relative_to(ROOT))] = sha(p)
        if d["name"] != "catalog":
            p = ROOT / d["database"]
            inputs[d["database"]] = sha(p)
    for p in configs + [run / "benchmark", run / "vexdb_lite.duckdb_extension", run / "baseline.duckdb_extension",
                         ROOT / "build/product-tests/pq-integration-20261027/duckdb-sdk/libduckdb.dylib"]:
        inputs[str(p.relative_to(ROOT))] = sha(p)
    registered = time.time()
    save(run / "protocol.json", {
        "registered_epoch": registered, "deadline_epoch": registered + 5400,
        "disk_floor_bytes": 4 * 2**30, "job_timeout_seconds": 1200,
        "datasets": datasets, "quality_queries": 128, "timed_queries_per_worker": 32,
        "connections": [1, 16, 64], "rounds": 2, "expected_timed_calls": total_calls,
        "platform": platform.platform(),
        "scope": "User-authorized filtered search fixes. Existing used public proxy queries plus explicitly synthetic catalog, one saved graph per workload. No overnight/fresh holdout reuse or deadline extension.",
        "quality": "Independent float64 exhaustive L2 on the qualifying subset; save strict ID Recall@min(10,eligible) and distance-boundary recall separately. Record underfill and illegal IDs. New filtered path must return all available k and reach >=99.9% strict recall. No tuning from these results.",
        "timing": "Native prepared SQL execute/materialize/fetch/duplicate validation, one outstanding request per connection. Each worker runs the same first 32 queries in fixed independently shuffled order. Warmup/prepare excluded. Global wall, every request ID and ns timestamp retained. Two full reversed point rounds and reversed process order in round 2. threads=1 per connection; closed-loop local SDK, not network throughput. Short one-client points and shared host variability must be disclosed.",
        "comparison": "Old raw graph ef256, old PQ graph ef1536 refine16, old exact with optimizer brute-force threshold1M; new filtered exact with PQgraph setting, same SQL/index/bytes. Unfiltered old/new raw and PQ are controls. Fast low-quality/underfilled results cannot be called a speedup; compare matched-quality exact too.",
        "oracle": "No product IDs, distance caches or SQL used as truth. Direct float64 base norms and matrix products for oracle only, never ANN timing. Equal-distance top-k boundary uses a separately disclosed 1e-8*max(1,kth_squared_L2) tolerance; strict ID metric remains primary.",
        "inputs": inputs, "sources": sorted(names),
    })
    update(run, {"status": "registered", "jobs": [], "controller_pid": None, "active_pid": None})
    print("registered", total_calls, "timed SQL requests", flush=True)


def verify(run):
    p = json.loads((run / "protocol.json").read_text())
    for name, digest in p["inputs"].items():
        if sha(ROOT / name) != digest:
            raise RuntimeError("frozen input changed: " + name)
    return p


def alive(pid):
    if not pid:
        return False
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


def job(run, state, protocol, name, command):
    if any(j["name"] == name for j in state["jobs"]):
        raise RuntimeError("prior job exists")
    record = {"name": name, "command": list(map(str, command)), "status": "running", "started_epoch": time.time()}
    state["jobs"].append(record)
    end = min(protocol["deadline_epoch"], time.time() + protocol["job_timeout_seconds"])
    failure = None
    with (run / (name + ".log")).open("x") as log:
        child = subprocess.Popen(record["command"], stdout=log, stderr=subprocess.STDOUT)
        record["pid"] = child.pid
        state.update(status=name, active_pid=child.pid)
        update(run, state)
        print("START", name, child.pid, flush=True)
        while child.poll() is None:
            free = shutil.disk_usage(run).free
            if time.time() >= end:
                failure = "registered timeout"
            elif free < protocol["disk_floor_bytes"]:
                failure = "disk floor"
            if failure:
                child.terminate()
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
                break
            state["disk_free_bytes"] = free
            update(run, state)
            time.sleep(3)
    record.update(status="complete" if not failure and child.returncode == 0 else "failed",
                  exit_code=child.returncode, reason=failure, finished_epoch=time.time(),
                  log_sha256=sha(run / (name + ".log")))
    state["active_pid"] = None
    update(run, state)
    if record["status"] != "complete":
        raise RuntimeError(name + ": " + str(failure or child.returncode))
    print("DONE", name, round(record["finished_epoch"] - record["started_epoch"], 1), flush=True)


def execute(run):
    p = verify(run)
    state = json.loads((run / "state.json").read_text())
    if state["status"] == "complete":
        return
    if alive(state.get("controller_pid")) or alive(state.get("active_pid")) or state["jobs"]:
        raise RuntimeError("active or prior run: no duplicate or blind resume")
    if time.time() >= p["deadline_epoch"]:
        raise RuntimeError("deadline expired")
    state["controller_pid"] = os.getpid()
    update(run, state)
    try:
        catalog = p["datasets"][2]
        job(run, state, p, "catalog-build", [run / "benchmark", "build", run / "vexdb_lite.duckdb_extension",
            ROOT / catalog["data"], "20000", "64", run / "catalog-fixture"])
        db = ROOT / catalog["database"]
        save(run / "catalog-fixture.json", {"sha256": sha(db), "bytes": db.stat().st_size})
        for round_id in (1, 2):
            datasets = p["datasets"] if round_id == 1 else p["datasets"][::-1]
            for dataset in datasets:
                for version in (["baseline", "new"] if round_id == 1 else ["new", "baseline"]):
                    # Avoid repeatedly hashing gigabytes during native timing.
                    # All inputs are verified before and after the whole queue.
                    if time.time() >= p["deadline_epoch"]:
                        raise RuntimeError("deadline expired")
                    name = f"{dataset['name']}-{version}-r{round_id}"
                    extension = run / ("baseline.duckdb_extension" if version == "baseline" else "vexdb_lite.duckdb_extension")
                    job(run, state, p, name, [run / "benchmark", "run", extension, ROOT / dataset["database"],
                        ROOT / dataset["data"], str(dataset["dim"]), "128", run / (name + ".points"), run / name])
                    output = run / name
                    save(run / (name + "-hashes.json"), {str(f.relative_to(run)): sha(f) for f in output.rglob("*") if f.is_file()})
        verify(run)
        if sha(db) != json.loads((run / "catalog-fixture.json").read_text())["sha256"]:
            raise RuntimeError("read-only catalog changed")
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
