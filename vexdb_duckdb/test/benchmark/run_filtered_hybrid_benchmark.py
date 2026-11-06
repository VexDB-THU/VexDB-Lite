"""Bounded, serial, frozen business SQL comparison. Never extends a run."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[3]


def sha(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def save(path, value):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, indent=2))
    temporary.replace(path)


def initialize(run):
    if run.exists():
        raise RuntimeError("run already exists")
    run.mkdir(parents=True)
    parent = run.parent
    cohorts = json.loads((parent / "performance-preparation/cohorts.json").read_text())
    versions = {
        "legacy": {"extension": str(ROOT / "build/product-tests/pq-admission-fast-20261028/product/vexdb_lite.duckdb_extension"), "filter_mode": "legacy", "pq_mode": "pq_graph"},
        "exact": {"extension": str(parent / "product-v4/vexdb_lite.duckdb_extension"), "filter_mode": "exact", "pq_mode": "pq_graph"},
        "pq": {"extension": str(parent / "product-v4/vexdb_lite.duckdb_extension"), "filter_mode": "auto", "pq_mode": "pq_graph"},
        "raw": {"extension": str(parent / "product-v4/vexdb_lite.duckdb_extension"), "filter_mode": "auto", "pq_mode": "off"},
    }
    versions["legacy-aa"] = dict(versions["legacy"])
    workloads = []
    for name, source, profile in (
        ("text-broad", "text-public", "broad-acl"),
        ("text-tenant", "text-public", "tenant-acl"),
        ("catalog-matched", "catalog-matched", "catalog-available-price"),
        ("catalog-mixed", "catalog-mixed", "catalog-available-price"),
    ):
        c = next(c for c in cohorts if c["name"] == source)
        workloads.append({"name": name, "source": source, "database": c["database"],
                          "queries": c["queries"], "base": c["base"], "dim": c["dim"], "profile": profile})
    jobs = []
    for work in workloads:
        for round_id, order in ((1, ["legacy", "exact", "pq", "raw", "legacy-aa"]),
                                (2, ["legacy-aa", "raw", "pq", "exact", "legacy"])):
            for version in order:
                name = f"{work['name']}-{version}-r{round_id}"
                path = run / (name + ".points")
                setting = versions[version]
                clients = (1, 16, 64) if round_id == 1 else (64, 16, 1)
                path.write_text("".join(f"c{c} {setting['filter_mode']} {setting['pq_mode']} 1536 16 {c} 5 {work['profile']}\n" for c in clients))
                jobs.append({"name": name, "workload": work["name"], "round": round_id,
                             "version": version, "points": str(path)})
    inputs = [Path(__file__), ROOT / "vexdb_duckdb/test/benchmark/benchmark_filtered_hybrid.cpp",
              ROOT / "vexdb_duckdb/test/benchmark/vex_quantization_product_benchmark.cpp",
              parent / "benchmark-filtered-hybrid-final", parent / "performance-preparation/truth.json",
              parent / "performance-preparation/cohorts.json",
              ROOT / "build/product-tests/pq-integration-20261027/duckdb-sdk/libduckdb.dylib"]
    inputs += [Path(v["extension"]) for v in versions.values()]
    inputs += [Path(j["points"]) for j in jobs]
    inputs += [Path(w[k]) for w in workloads for k in ("database", "queries", "base")]
    for prefix in (ROOT / "common", ROOT / "vexdb_duckdb"):
        inputs += [p for p in prefix.rglob("*") if p.is_file() and not any(x in ("test", "build", "third_party") for x in p.parts)
                   and p.suffix not in (".md", ".py", ".json")]
    protocol = {
        "registered_epoch": time.time(), "deadline_epoch": time.time() + 1800,
        "disk_floor_bytes": 4 * 2**30, "job_timeout_seconds": 180,
        "binary": str(parent / "benchmark-filtered-hybrid-final"), "versions": versions,
        "workloads": workloads, "jobs": jobs,
        "load": "Eight exposed query vectors per cohort; every client executes complete uniform 8-query cycles for >=5 seconds. Counts may differ by capacity, mixes stay identical. Per-session threads=1; clients1/16/64; connection reuse and native prepared SQL returning ID and body. Includes result materialization and copy, excludes prepare/warmup, no network/embedding/generation. Closed-loop retrieval load, not production capacity.",
        "quality": "Independent precomputed float64 truth for every profile/query; real returned IDs each request, no duplicates/illegal/missing rows; every point recall>=.999. Synthetic metadata and bodies, no customer traffic.",
        "controls": "Reverse second round; same old binary A/A. AA each-round QPS ratio in [.97,1.03]. New exact/legacy paired geometric QPS>=.97. Candidate/exact gains considered only with quality and stable AA; require >=10% QPS gain and no P95 regression in both rounds. Do not normalize candidate failures away.",
        "boundaries": "Fresh synthetic query validation already failed 99.9% on some broad/unfiltered groups and is retained. These timings do not override that failure or justify default-on. No compiles, profiling, disassembly or truth recomputation during timing.",
        "sha256": {str(p.resolve()): sha(p) for p in dict.fromkeys(inputs)},
    }
    save(run / "protocol.json", protocol)
    print("registered", len(jobs), "serial native jobs", flush=True)


def execute(run):
    protocol = json.loads((run / "protocol.json").read_text())
    if (run / "state.json").exists():
        raise RuntimeError("prior execution exists; no blind restart")
    for path, digest in protocol["sha256"].items():
        if sha(path) != digest:
            raise RuntimeError("changed frozen input: " + path)
    state = {"status": "running", "controller_pid": os.getpid(), "active_pid": None, "jobs": []}
    save(run / "state.json", state)
    child = None
    try:
        for spec in protocol["jobs"]:
            if time.time() >= protocol["deadline_epoch"] or shutil.disk_usage(run).free < protocol["disk_floor_bytes"]:
                raise RuntimeError("deadline/disk guard")
            work = next(w for w in protocol["workloads"] if w["name"] == spec["workload"])
            out = run / spec["name"]
            if out.exists():
                raise RuntimeError("output already exists")
            cmd = [protocol["binary"], protocol["versions"][spec["version"]]["extension"], work["database"],
                   str(Path(work["queries"]).parent), str(work["dim"]), spec["points"], str(out)]
            if work.get("metadata_source") == "stored": cmd.append("stored")
            if protocol["versions"][spec["version"]].get("bidirectional"):
                if work.get("metadata_source") != "stored": raise RuntimeError("reverse timing requires stored metadata")
                cmd.append("bidirectional")
            record = dict(spec, command=cmd, status="running", started_epoch=time.time())
            state["jobs"].append(record)
            with (run / (spec["name"] + ".log")).open("x") as stream:
                child = subprocess.Popen(cmd, stdout=stream, stderr=subprocess.STDOUT)
                state["active_pid"] = child.pid
                record["pid"] = child.pid
                save(run / "state.json", state)
                print("START", spec["name"], child.pid, flush=True)
                while child.poll() is None:
                    if time.time() >= min(protocol["deadline_epoch"], record["started_epoch"] + protocol["job_timeout_seconds"]) or shutil.disk_usage(run).free < protocol["disk_floor_bytes"]:
                        child.terminate()
                        try: child.wait(timeout=5)
                        except subprocess.TimeoutExpired: child.kill(); child.wait()
                        raise RuntimeError("native deadline/disk guard")
                    time.sleep(.5)
            record.update(exit_code=child.returncode, finished_epoch=time.time(), status="complete" if child.returncode == 0 else "failed")
            state["active_pid"] = None
            save(run / "state.json", state)
            save(run / (spec["name"] + "-hashes.json"), {str(p.relative_to(run)): sha(p) for p in out.rglob("*") if p.is_file()})
            print("DONE", spec["name"], child.returncode, flush=True)
            if child.returncode:
                raise RuntimeError("native job failed: " + spec["name"])
        for path, digest in protocol["sha256"].items():
            if sha(path) != digest:
                raise RuntimeError("frozen input changed during timing: " + path)
        state.update(status="complete", controller_pid=None, active_pid=None, finished_epoch=time.time(), inputs_unchanged=True)
        save(run / "state.json", state)
    except BaseException as error:
        if child and child.poll() is None:
            child.terminate()
            try: child.wait(timeout=5)
            except subprocess.TimeoutExpired: child.kill(); child.wait()
        state.update(status="failed", controller_pid=None, active_pid=None, error=str(error), finished_epoch=time.time())
        if state["jobs"] and state["jobs"][-1]["status"] == "running": state["jobs"][-1]["status"] = "failed"
        save(run / "state.json", state)
        raise


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("init", "run"))
    parser.add_argument("--run", type=Path, required=True)
    args = parser.parse_args()
    (initialize if args.action == "init" else execute)(args.run.resolve())
