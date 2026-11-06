"""Run a frozen, bounded candidate-acquisition SQL stage on the saved 100K index."""
import argparse
import csv
import hashlib
import json
import os
import shutil
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent


def sha(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def save(path, value):
    with path.open("x") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2)


def state_write(path, value):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, indent=2))
    temporary.replace(path)


def verify(run, stage):
    p = json.loads((run / stage / "protocol.json").read_text())
    for name, digest in p["inputs"].items():
        if sha(ROOT / name) != digest:
            raise RuntimeError("registered stage input changed: " + name)
    return p


def initialize(run, stage):
    top = json.loads((run / "protocol.json").read_text())
    if time.time() >= top["deadline_epoch"]:
        raise RuntimeError("original finite deadline expired")
    prior = ROOT / top["previous_run"]
    for name, digest in json.loads((run / "build-source-manifest.json").read_text())["inputs"].items():
        if sha(ROOT / name) != digest:
            raise RuntimeError("baseline product source changed: " + name)
    if stage == "dev":
        data = ROOT / "build/product-tests/quantization-product-20261027/data/dbpedia"
        methods = top["methods"]
        candidates = [x["point"] for x in methods]
        methods = [{"point":"raw-ef512","mode":"off","ef":512,"factor":1}, *methods]
        nq = 200
    else:
        selection = json.loads((run / "selection.json").read_text())
        if not selection["passed"]:
            raise RuntimeError("secondary development failed; fresh queries remain unused")
        if sha(run / "benchmark") != selection["binary_sha256"]:
            raise RuntimeError("frozen selected binary changed")
        methods = [{"point":"raw-ef"+str(ef),"mode":"off","ef":ef,"factor":1}
                   for ef in top["validation_controls"]]
        methods.append(selection["method"])
        candidates = [selection["method"]["point"]]
        data = run / "new-queries"
        nq = 512
    out = run / stage
    out.mkdir(exist_ok=False)
    connections = [1, 16, 64]
    points = [dict(m, connections=c) for c in connections for m in methods]
    files = {Path(__file__), HERE / "review_pq_999_recovery_stage.py", HERE / "prepare_pq_999_memory_queries.py",
             HERE / "vex_pq_norm_benchmark.cpp", HERE / "vex_text_concurrency_benchmark.cpp",
             HERE / "vex_quantization_product_benchmark.cpp", HERE / "review_text_concurrency.py", HERE / "review_pq_norm_results.py",
             HERE / "run_text_concurrency.py", run / "protocol.json", run / "benchmark",
             ROOT / top["extension_run"] / "vexdb_lite.duckdb_extension", prior / "fixture/vectors.duckdb",
             ROOT / "build/product-tests/pq-integration-20261027/duckdb-sdk/libduckdb.dylib",
             data / "queries.f32", data / "truth-100000.i32"}
    files.update(ROOT / name for name in json.loads((run / "build-source-manifest.json").read_text())["inputs"])
    if stage == "validation":
        files.update((run / "selection.json", data / "manifest.json"))
    for number, order in ((1, points), (2, points[::-1])):
        f = out / f"round{number}.points"
        f.write_text("".join(f"{x['point']} {x['mode']} {x['ef']} {x['factor']} {x['connections']}\n" for x in order))
        files.add(f)
    p = {"registered_epoch": time.time(), "deadline_epoch": min(top["deadline_epoch"], time.time() + 1800),
         "rows": 100000, "dim": 1536, "queries": nq, "repeats_per_worker": 3 if stage == "dev" else 1,
         "connections": connections, "methods": methods, "candidates": candidates,
         "expected_sql_calls": 2 * sum(connections) * len(methods) * nq * (3 if stage == "dev" else 1),
         "data_dir": str(data.relative_to(ROOT)), "stage": stage,
         "inputs": {str(f.relative_to(ROOT)): sha(f) for f in sorted(files)}}
    for f in files:
        if f.suffix in (".py", ".cpp", ".hpp", ".h"):
            dest = out / "frozen-source" / f.relative_to(ROOT)
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, dest)
    save(out / "protocol.json", p)
    state_write(out / "state.json", {"status": "registered", "jobs": [], "controller_pid": None, "active_pid": None})


def execute(run, stage):
    out = run / stage
    if not out.exists():
        initialize(run, stage)
    p = verify(run, stage)
    state = json.loads((out / "state.json").read_text())
    if state["status"] != "registered" or state["jobs"]:
        raise RuntimeError("stage already used; retain existing results, do not launch duplicates")
    state["controller_pid"] = os.getpid()
    previous = ROOT / json.loads((run / "protocol.json").read_text())["previous_run"]
    try:
        for number in (1, 2):
            verify(run, stage)
            if time.time() >= p["deadline_epoch"]:
                raise RuntimeError("registered stage deadline expired")
            command = [run / "benchmark", "run", ROOT / json.loads((run / "protocol.json").read_text())["extension_run"] / "vexdb_lite.duckdb_extension",
                       previous / "fixture/vectors.duckdb", ROOT / p["data_dir"], "100000", "1536",
                       str(p["queries"]), str(p["repeats_per_worker"]), out / f"round{number}.points", out / f"round{number}"]
            job = {"round": number, "command": list(map(str, command)), "status": "running", "started_epoch": time.time()}
            state["jobs"].append(job)
            with (out / f"round{number}.log").open("x") as log:
                child = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
                job["pid"] = child.pid
                state.update(status="running", active_pid=child.pid)
                state_write(out / "state.json", state)
                print("START", stage, number, "pid", child.pid, flush=True)
                failure = None
                while child.poll() is None:
                    if time.time() >= p["deadline_epoch"]:
                        failure = "registered timeout"
                    elif shutil.disk_usage(run).free < 4 * 2**30:
                        failure = "free disk below 4 GiB"
                    if failure:
                        child.terminate()
                        try:
                            child.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            child.kill()
                            child.wait()
                        break
                    time.sleep(5)
            job.update(status="complete" if child.returncode == 0 and not failure else "failed",
                       exit_code=child.returncode, reason=failure, finished_epoch=time.time())
            state["active_pid"] = None
            state_write(out / "state.json", state)
            if job["status"] != "complete":
                raise RuntimeError(str(job))
            compressed = []
            for file in (out / f"round{number}").glob("*.csv"):
                if file.stat().st_size < 1024*1024:
                    continue
                digest = sha(file)
                temporary = file.with_suffix(".compressed-temp")
                before = file.stat().st_blocks*512
                subprocess.run(["ditto","--hfsCompression",str(file),str(temporary)],check=True)
                if sha(temporary) != digest:
                    raise RuntimeError("native compression changed result bytes")
                if temporary.stat().st_blocks*512 < before:
                    os.replace(temporary,file)
                else:
                    temporary.unlink()
                compressed.append({"file":file.name,"sha256":digest,"physical_before":before,
                                   "physical_after":file.stat().st_blocks*512})
            save(out / f"round{number}-storage.json",compressed)
            save(out / f"round{number}-hashes.json", {str(f.relative_to(out)): sha(f)
                 for f in sorted((out / f"round{number}").rglob("*")) if f.is_file()})
            print("DONE", stage, number, flush=True)
        verify(run, stage)
        state.update(status="complete", controller_pid=None, active_pid=None)
        state_write(out / "state.json", state)
    except BaseException as error:
        state.update(status="failed", controller_pid=None, error=str(error))
        state_write(out / "state.json", state)
        raise


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--stage", choices=("dev", "validation"), required=True)
    args = parser.parse_args()
    execute(args.run.resolve(), args.stage)
