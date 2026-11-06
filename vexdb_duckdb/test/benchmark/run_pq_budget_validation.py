"""Freeze and run one fixed high-recall PQ budget on two untouched cohorts."""
import argparse
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


def state_write(run, state):
    f = run / "state.tmp"
    f.write_text(json.dumps(state, indent=2))
    f.replace(run / "state.json")


def verify(record):
    for name, digest in record["inputs"].items():
        if sha(ROOT / name) != digest:
            raise RuntimeError("frozen input changed: " + name)


def freeze(run):
    p = json.loads((run / "protocol.json").read_text())
    baseline = json.loads((run / "baseline-source-manifest.json").read_text())
    for name, digest in baseline.items():
        if sha(ROOT / name) != digest:
            raise RuntimeError("restore pre-turn product sources first: " + name)
    files = {ROOT / name for name in baseline}
    files.update((run / "protocol.json", run / "benchmark", HERE / "run_pq_budget_validation.py",
                  HERE / "prepare_pq_budget_queries.py", HERE / "review_pq_budget_validation.py",
                  HERE / "vex_pq_rescue_benchmark.cpp", run / "compile-command.json",
                  HERE / "review_text_concurrency.py", HERE / "run_text_concurrency.py",
                  ROOT / p["extension_run"] / "vexdb_lite.duckdb_extension",
                  ROOT / p["database"], ROOT / "build/product-tests/pq-integration-20261027/duckdb-sdk/libduckdb.dylib"))
    record = {"frozen_epoch": time.time(), "protocol_sha256": sha(run / "protocol.json"),
              "inputs": {str(f.relative_to(ROOT)): sha(f) for f in sorted(files)}}
    for f in files:
        if f.suffix in (".py", ".cpp", ".hpp", ".h"):
            target = run / "frozen-source" / f.relative_to(ROOT)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, target)
    save(run / "freeze.json", record)
    print("single existing-PQ candidate frozen before new cohorts", flush=True)


def execute(run):
    p = json.loads((run / "protocol.json").read_text())
    frozen = json.loads((run / "freeze.json").read_text())
    verify(frozen)
    state = json.loads((run / "state.json").read_text())
    if state["status"] not in ("registered", "frozen") or state.get("jobs"):
        raise RuntimeError("run already used, no duplicate or silent restart")
    if time.time() >= p["deadline_epoch"]:
        raise RuntimeError("fixed deadline expired")
    state.update(status="running", controller_pid=os.getpid(), active_pid=None, jobs=[])
    state_write(run, state)
    try:
        for cohort in ("a", "b"):
            out = run / cohort
            out.mkdir(exist_ok=False)
            methods = [{"point": "raw-ef" + str(ef), "mode": "off", "ef": ef, "factor": 1} for ef in p["raw_ef"]]
            methods.append(p["candidate"])
            points = [dict(m, connections=c) for c in p["load"]["connections"] for m in methods]
            stage = {"rows": 100000, "dim": 1536, "queries": 1024, "repeats_per_worker": 1,
                     "connections": p["load"]["connections"], "methods": methods,
                     "expected_sql_calls": 2 * sum(p["load"]["connections"]) * len(methods) * 1024,
                     "cohort": cohort, "data_dir": str((run / "queries" / cohort).relative_to(ROOT)),
                     "freeze_sha256": sha(run / "freeze.json")}
            data = run / "queries" / cohort
            data_hashes = {str(f.relative_to(ROOT)): sha(f) for f in data.iterdir() if f.is_file()}
            stage["data_sha256"] = data_hashes
            save(out / "protocol.json", stage)
            for number, order in ((1, points), (2, points[::-1])):
                verify(frozen)
                verify({"inputs": data_hashes})
                if time.time() >= p["deadline_epoch"]:
                    raise RuntimeError("registered deadline expired")
                config = out / f"round{number}.points"
                with config.open("x") as f:
                    f.write("".join(f"{x['point']} {x['mode']} {x['ef']} {x['factor']} {x['connections']}\n" for x in order))
                command = [run / "benchmark", "run", ROOT / p["extension_run"] / "vexdb_lite.duckdb_extension",
                           ROOT / p["database"], data, "100000", "1536", "1024", "1", config, out / f"round{number}"]
                job = {"cohort": cohort, "round": number, "started_epoch": time.time(), "status": "running",
                       "command": list(map(str, command))}
                state["jobs"].append(job)
                with (out / f"round{number}.log").open("x") as log:
                    child = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
                    job["pid"] = child.pid
                    state["active_pid"] = child.pid
                    state_write(run, state)
                    print("START", cohort, number, "pid", child.pid, flush=True)
                    failure = None
                    while child.poll() is None:
                        if time.time() >= p["deadline_epoch"]:
                            failure = "registered deadline"
                        elif shutil.disk_usage(run).free < p["disk_floor_bytes"]:
                            failure = "free disk below unchanged4GiB floor"
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
                           finished_epoch=time.time(), exit_code=child.returncode, reason=failure)
                state["active_pid"] = None
                state_write(run, state)
                if job["status"] != "complete":
                    raise RuntimeError(str(job))
                # Preserve exact readable bytes while reducing completed CSV
                # allocation between timed processes, never during a query.
                storage = []
                for file in (out / f"round{number}").glob("*.csv"):
                    if file.stat().st_size < 1024 * 1024:
                        continue
                    digest, before = sha(file), file.stat().st_blocks * 512
                    temporary = file.with_suffix(".compressed-temp")
                    subprocess.run(["ditto", "--hfsCompression", str(file), str(temporary)], check=True)
                    if sha(temporary) != digest:
                        raise RuntimeError("native compression changed result bytes")
                    if temporary.stat().st_blocks * 512 < before:
                        os.replace(temporary, file)
                    else:
                        temporary.unlink()
                    storage.append({"file": file.name, "sha256": digest, "physical_before": before,
                                    "physical_after": file.stat().st_blocks * 512})
                save(out / f"round{number}-storage.json", storage)
                save(out / f"round{number}-hashes.json", {str(f.relative_to(out)): sha(f)
                     for f in sorted((out / f"round{number}").rglob("*")) if f.is_file()})
                print("DONE", cohort, number, flush=True)
        verify(frozen)
        state.update(status="complete", controller_pid=None, active_pid=None)
        state_write(run, state)
    except BaseException as error:
        state.update(status="failed", error=str(error), controller_pid=None)
        state_write(run, state)
        raise


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("freeze", "run"))
    parser.add_argument("--run", type=Path, required=True)
    args = parser.parse_args()
    (freeze if args.action == "freeze" else execute)(args.run.resolve())
