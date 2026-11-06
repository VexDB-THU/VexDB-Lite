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
    for name, digest in top["baseline_source_sha256"].items():
        if sha(ROOT / name) != digest:
            raise RuntimeError("baseline product source changed: " + name)
    if stage == "dev":
        # The registered 99.95% development margin determines eligibility.
        # Independently recompute it from actual native returned IDs.
        data = ROOT / "build/product-tests/quantization-product-20261027/data/dbpedia"
        import struct
        truth = struct.unpack("<2000i", (data / "truth-100000.i32").read_bytes())
        native = {}
        with (run / "probe-results/ids.csv").open() as stream:
            for row in csv.DictReader(stream):
                name, q = row["point"], int(row["query"])
                ids = list(map(int, row["ids"].split()))
                if len(ids) != 10 or len(set(ids)) != 10 or not 0 <= q < 200:
                    raise RuntimeError("invalid native diagnostic IDs")
                native.setdefault(name, {})[q] = len(set(ids) & set(truth[q * 10:q * 10 + 10]))
        hits = {name: sum(values.values()) for name, values in native.items()}
        if any(len(values) != 200 for values in native.values()):
            raise RuntimeError("incomplete native diagnostic")
        methods = [{"point": "raw-ef512", "mode": "off", "ef": 512, "factor": 1}]
        candidates = []
        for ef in (768, 1024, 1536):
            if hits["pq-ef" + str(ef)] >= 1999:
                name = "pq-ef" + str(ef) + "-r16"
                candidates.append(name)
                methods.append({"point": name, "mode": "pq_graph", "ef": ef, "factor": 16})
        nq = 200
        save(run / "probe-recall-review.json", {"hits_by_method": hits, "denominator": 2000,
                                               "ids_sha256": sha(run / "probe-results/ids.csv"),
                                               "eligible_existing_pq": candidates,
                                               "cached_union_not_sql_qps": True})
    else:
        selection = json.loads((run / "selection.json").read_text())
        if not selection["passed"]:
            raise RuntimeError("development selection failed; fresh queries remain unused")
        if sha(run / "benchmark") != selection["binary_sha256"]:
            raise RuntimeError("frozen selected binary changed")
        methods = [{"point": "raw-ef512", "mode": "off", "ef": 512, "factor": 1},
                   {"point": "raw-ef768", "mode": "off", "ef": 768, "factor": 1},
                   selection["method"]]
        candidates = [selection["method"]["point"]]
        data = run / "new-queries"
        nq = 512
    out = run / stage
    out.mkdir(exist_ok=False)
    connections = [1, 16, 64]
    points = [dict(m, connections=c) for c in connections for m in methods]
    files = {Path(__file__), HERE / "review_pq_candidate_stage.py", HERE / "prepare_pq_rescue_queries.py",
             HERE / "vex_pq_rescue_benchmark.cpp", HERE / "vex_text_concurrency_benchmark.cpp",
             HERE / "vex_quantization_product_benchmark.cpp", HERE / "review_text_concurrency.py",
             HERE / "run_text_concurrency.py", run / "protocol.json", run / "benchmark",
             prior / "vexdb_lite.duckdb_extension", prior / "fixture/vectors.duckdb",
             ROOT / "build/product-tests/pq-integration-20261027/duckdb-sdk/libduckdb.dylib",
             data / "queries.f32", data / "truth-100000.i32"}
    files.update(ROOT / name for name in top["baseline_source_sha256"])
    if stage == "validation":
        files.update((run / "selection.json", data / "manifest.json"))
    for number, order in ((1, points), (2, points[::-1])):
        f = out / f"round{number}.points"
        f.write_text("".join(f"{x['point']} {x['mode']} {x['ef']} {x['factor']} {x['connections']}\n" for x in order))
        files.add(f)
    p = {"registered_epoch": time.time(), "deadline_epoch": min(top["deadline_epoch"], time.time() + 1800),
         "rows": 100000, "dim": 1536, "queries": nq, "repeats_per_worker": 3,
         "connections": connections, "methods": methods, "candidates": candidates,
         "expected_sql_calls": 2 * sum(connections) * len(methods) * nq * 3,
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
            command = [run / "benchmark", "run", previous / "vexdb_lite.duckdb_extension",
                       previous / "fixture/vectors.duckdb", ROOT / p["data_dir"], "100000", "1536",
                       str(p["queries"]), "3", out / f"round{number}.points", out / f"round{number}"]
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
