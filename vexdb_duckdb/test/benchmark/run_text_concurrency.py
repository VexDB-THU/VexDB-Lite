"""Frozen 100K-text product test; no parameter selection from new results."""
import argparse
import csv
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


def state_write(run, state):
    state["updated_epoch"] = time.time()
    temporary = run / "state.tmp"
    temporary.write_text(json.dumps(state, indent=2))
    temporary.replace(run / "state.json")


def initialize(run):
    if (run / "protocol.json").exists():
        raise RuntimeError("protocol already exists")
    if shutil.disk_usage(run).free < 8 * 2**30:
        raise RuntimeError("need at least 8 GiB free before constructing this fixture")
    prior = ROOT / "build/product-tests/pq-graph-20261027-v3"
    previous = json.loads((prior / "protocol.json").read_text())
    sources = set(previous["sources"])
    sources.update(("vexdb_duckdb/test/benchmark/vex_text_concurrency_benchmark.cpp",
                    "vexdb_duckdb/test/benchmark/run_text_concurrency.py"))
    for name in previous["sources"]:
        if sha(ROOT / name) != previous["inputs"][name]:
            raise RuntimeError("previous product source changed: " + name)
    for name in sorted(sources):
        dest = run / "frozen-source" / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes((ROOT / name).read_bytes())
    methods = [
        {"point": f"raw-ef{ef}", "mode": "off", "ef": ef, "factor": 1} for ef in (40, 128, 256, 512)
    ] + [
        {"point": f"pq-ef{ef}-r16", "mode": "pq_graph", "ef": ef, "factor": 16} for ef in (256, 512)
    ] + [{"point": "full-r16", "mode": "pq_only", "ef": 128, "factor": 16}]
    connections = [1, 8, 16, 32, 64]
    points = [dict(m, connections=c) for c in connections for m in methods]
    configs = []
    for round_id, order in ((1, points), (2, points[::-1])):
        config = run / f"round{round_id}.points"
        config.write_text("".join(f"{p['point']} {p['mode']} {p['ef']} {p['factor']} {p['connections']}\n" for p in order))
        configs.append(config.relative_to(ROOT))
    data = ROOT / "build/product-tests/quantization-product-20261027/data/dbpedia"
    manifest = json.loads((data / "manifest.json").read_text())
    data_files = [data / name for name in ("base.f32", "queries.f32", "truth-100000.i32")]
    for path in data_files:
        if sha(path) != manifest["files"][path.name]["sha256"]:
            raise RuntimeError("prepared dataset checksum changed")
    files = {Path(p) for p in sources} | set(configs)
    files.update(path.relative_to(ROOT) for path in data_files)
    files.update((data.relative_to(ROOT) / "manifest.json", prior.relative_to(ROOT) / "quality-selection.json",
                  Path("build/product-tests/pq-integration-20261027/duckdb-sdk/libduckdb.dylib")))
    files.update(run.relative_to(ROOT) / name for name in ("benchmark", "vexdb_lite.duckdb_extension", "compile-command.json"))
    registered = time.time()
    save(run / "protocol.json", {
        "registered_epoch": registered, "deadline_epoch": registered + 7200,
        "build_timeout_seconds": 3600, "round_timeout_seconds": 1800, "disk_floor_bytes": 4 * 2**30,
        "reason": "User requested 100K text and high-concurrency comparison with original raw-vector graph search. Separate experiment; never resumes or extends the old overnight window.",
        "platform": platform.platform(), "hardware": {"physical_cores": 12, "logical_cpus": 12, "ram_bytes": 68719476736, "source": "read-only sysctl, verified before registration"},
        "rows": 100000, "dim": 1536, "queries": 200, "repeats_per_worker": 3,
        "connections": connections, "rounds": 2, "methods": methods,
        "expected_sql_calls": 2 * sum(connections) * len(methods) * 200 * 3,
        "primary_candidate": "pq-ef256-r16", "secondary_fixed_candidate": "pq-ef512-r16",
        "quality": "Fixed methods only, no tuning after 100K results. Report all Recall@10, per-query hit counts, and equal-quality curves at 95/99/99.9%. Primary retains >=95% recall and loses at most 4/2000 hits versus same-refine full PQ (0.2pp). Native code evaluations should average <=50% of full scan. Speed comparison must state recall and contrast with the raw graph as well as full PQ.",
        "concurrency": "Every client runs all 200 queries each pass; three independently shuffled passes, one outstanding SQL request per client. 32/64 clients oversubscribe 12 logical CPUs. Each SQL connection sets threads=1. Report CPU-core use, average occupied clients and overlap; average occupancy <80% of requested clients is flagged, never silently discarded.",
        "timing": "Native prepared SQL execution, materialization and ID extraction; Value construction, Prepare, warmup and order generation outside timers. Nanosecond request boundaries and global wall saved. QPS includes call recording/worker drain. Two fresh read-only query processes; second reverses the entire point order. This is a closed-loop local SDK workload, not open-loop network arrival latency.",
        "index": "One full PQ index, m=16 ef_construction=100 threads=1, default pq_m=384, raw vectors retained. All modes use identical graph/model/data. off is the original raw-vector graph search algorithm on that same index; no memory-saving claim against a separate PQ-free index. Build reuses the frozen product builder; its 18 single-query preparation probes are excluded from concurrency results.",
        "data_scope": "Existing previously used 200 dev proxy text queries, existing audited float64 exhaustive truth. Original two 256-query overnight holdouts remain unopened. One graph build seed only; no fresh-user-query or multi-seed claim.",
        "analysis": "Independent reviewer is frozen in a separate manifest before review; it cannot change this grid or method settings. No production code, commit, push or release required.",
        "sources": sorted(sources), "data_dir": str(data.relative_to(ROOT)),
        "inputs": {str(p): sha(ROOT / p) for p in sorted(files)},
    })
    state_write(run, {"status": "registered", "jobs": [], "controller_pid": None, "active_pid": None})
    print("registered:", 2 * sum(connections) * len(methods) * 200 * 3, "timed SQL requests", flush=True)


def verify(run):
    protocol = json.loads((run / "protocol.json").read_text())
    for name, digest in protocol["inputs"].items():
        if sha(ROOT / name) != digest:
            raise RuntimeError("frozen input changed: " + name)
    return protocol


def alive(pid):
    if not pid:
        return False
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


def job(run, state, protocol, name, command, seconds):
    if any(j["name"] == name for j in state["jobs"]):
        raise RuntimeError("refusing to overwrite a prior job: " + name)
    log_path = run / (name + ".log")
    record = {"name": name, "command": list(map(str, command)), "status": "running", "started_epoch": time.time()}
    state["jobs"].append(record)
    deadline = min(protocol["deadline_epoch"], time.time() + seconds)
    with log_path.open("x") as log:
        child = subprocess.Popen(record["command"], stdout=log, stderr=subprocess.STDOUT)
        record["pid"] = child.pid
        state.update(status=name, active_pid=child.pid)
        state_write(run, state)
        print("START", name, "pid", child.pid, flush=True)
        failure = None
        while child.poll() is None:
            free = shutil.disk_usage(run).free
            if time.time() >= deadline:
                failure = "registered timeout"
            elif free < protocol["disk_floor_bytes"]:
                failure = "disk free fell below registered 4 GiB floor"
            if failure:
                child.terminate()
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
                break
            state["last_disk_free_bytes"] = free
            state_write(run, state)
            time.sleep(5)
    record.update(status="complete" if not failure and child.returncode == 0 else "failed",
                  exit_code=child.returncode, finished_epoch=time.time(), reason=failure,
                  log_sha256=sha(log_path))
    state["active_pid"] = None
    state_write(run, state)
    if record["status"] != "complete":
        raise RuntimeError(name + ": " + str(failure or f"native exit {child.returncode}"))
    print("DONE", name, round(record["finished_epoch"] - record["started_epoch"], 1), "seconds", flush=True)


def execute(run):
    protocol = verify(run)
    state = json.loads((run / "state.json").read_text())
    if state["status"] == "complete":
        print("queue already complete")
        return
    if alive(state.get("controller_pid")) or alive(state.get("active_pid")):
        raise RuntimeError("registered controller/worker still alive; no duplicate launched")
    if state["jobs"]:
        raise RuntimeError("interrupted or failed run requires explicit audited recovery; original outputs retained")
    if time.time() >= protocol["deadline_epoch"]:
        raise RuntimeError("registered deadline expired")
    state["controller_pid"] = os.getpid()
    state_write(run, state)
    binary, extension = run / "benchmark", run / "vexdb_lite.duckdb_extension"
    data, fixture = ROOT / protocol["data_dir"], run / "fixture"
    try:
        job(run, state, protocol, "build", [binary, "build", extension, data, fixture], protocol["build_timeout_seconds"])
        verify(run)
        database = fixture / "vectors.duckdb"
        save(run / "fixture.json", {"database": str(database.relative_to(ROOT)), "sha256": sha(database), "bytes": database.stat().st_size})
        for round_id in (1, 2):
            verify(run)
            fixture_record = json.loads((run / "fixture.json").read_text())
            if sha(database) != fixture_record["sha256"]:
                raise RuntimeError("saved fixture changed")
            out = run / f"round{round_id}"
            command = [binary, "run", extension, database, data, "100000", "1536", "200", "3", run / f"round{round_id}.points", out]
            job(run, state, protocol, f"round{round_id}", command, protocol["round_timeout_seconds"])
            save(run / f"round{round_id}-hashes.json", {str(p.relative_to(run)): sha(p) for p in out.rglob("*") if p.is_file()})
        verify(run)
        if sha(database) != json.loads((run / "fixture.json").read_text())["sha256"]:
            raise RuntimeError("read-only query changed database")
        state.update(status="complete", active_pid=None, controller_pid=None)
        state_write(run, state)
    except BaseException as e:
        state.update(status="failed", error=str(e), controller_pid=None)
        state_write(run, state)
        raise


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("init", "run", "verify"))
    parser.add_argument("--run", required=True, type=Path)
    args = parser.parse_args()
    {"init": initialize, "run": execute, "verify": verify}[args.action](args.run.resolve())
