"""Loader-name-only recovery. Retain original sources, failure and deadline."""
import argparse
import json
import os
import shutil
import time
from pathlib import Path

from run_filtered_search import ROOT, alive, job, save, sha, update, verify


def init(parent, run):
    p = verify(parent)
    old = json.loads((parent / "state.json").read_text())
    if alive(old.get("controller_pid")) or alive(old.get("active_pid")):
        raise RuntimeError("original process remains active")
    if old["status"] != "failed" or len(old["jobs"]) != 2 or old["jobs"][0]["status"] != "complete":
        raise RuntimeError("unexpected recovery boundary")
    log = parent / "sift-baseline-r1.log"
    if "baseline_duckdb_cpp_init" not in log.read_text() or (parent / "sift-baseline-r1/points.csv").exists():
        raise RuntimeError("not the registered loader failure or query results already exist")
    run.mkdir(exist_ok=False)
    baseline = run / "baseline/vexdb_lite.duckdb_extension"
    baseline.parent.mkdir()
    shutil.copyfile(parent / "baseline.duckdb_extension", baseline)
    copied = []
    for original in parent.glob("*.points"):
        dest = run / original.name
        shutil.copyfile(original, dest)
        copied.append(dest)
    p.update({"original_run": str(parent.relative_to(ROOT)),
              "recovery_registered_epoch": time.time(),
              "recovery_reason": "The copied baseline extension was named baseline.duckdb_extension; DuckDB inferred a nonexistent baseline_duckdb_cpp_init. Copy identical bytes under baseline/vexdb_lite.duckdb_extension. No native query/timing had run. Reuse completed synthetic fixture, keep all method/data/timing parameters and original deadline.",
              "recovery_source": str(Path(__file__).resolve().relative_to(ROOT))})
    original_fixture = json.loads((parent / "catalog-fixture.json").read_text())
    catalog_db = ROOT / p["datasets"][2]["database"]
    if sha(catalog_db) != original_fixture["sha256"]:
        raise RuntimeError("catalog fixture changed")
    p["inputs"][str(catalog_db.relative_to(ROOT))] = original_fixture["sha256"]
    for f in copied + [baseline, Path(__file__).resolve(), parent / "state.json", log]:
        p["inputs"][str(f.relative_to(ROOT))] = sha(f)
    save(run / "protocol.json", p)
    shutil.copyfile(Path(__file__), run / "recovery-source.py")
    update(run, {"status": "registered", "jobs": [], "controller_pid": None, "active_pid": None})
    print("recovery registered; unchanged deadline", p["deadline_epoch"], flush=True)


def execute(run):
    p = verify(run)
    parent = ROOT / p["original_run"]
    state = json.loads((run / "state.json").read_text())
    if state["status"] == "complete":
        return
    if state["jobs"] or alive(state.get("active_pid")) or alive(state.get("controller_pid")):
        raise RuntimeError("prior/active job; no duplicate")
    state["controller_pid"] = os.getpid()
    update(run, state)
    try:
        for round_id in (1, 2):
            datasets = p["datasets"] if round_id == 1 else p["datasets"][::-1]
            for dataset in datasets:
                for version in (["baseline", "new"] if round_id == 1 else ["new", "baseline"]):
                    if time.time() >= p["deadline_epoch"]:
                        raise RuntimeError("original deadline expired")
                    name = f"{dataset['name']}-{version}-r{round_id}"
                    extension = run / "baseline/vexdb_lite.duckdb_extension" if version == "baseline" else parent / "vexdb_lite.duckdb_extension"
                    job(run, state, p, name, [parent / "benchmark", "run", extension, ROOT / dataset["database"],
                        ROOT / dataset["data"], str(dataset["dim"]), "128", run / (name + ".points"), run / name])
                    save(run / (name + "-hashes.json"), {str(f.relative_to(run)): sha(f) for f in (run / name).rglob("*") if f.is_file()})
        verify(run)
        state.update(status="complete", controller_pid=None)
        update(run, state)
    except BaseException as e:
        state.update(status="failed", error=str(e), controller_pid=None)
        update(run, state)
        raise


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("init", "run"))
    parser.add_argument("--parent", type=Path)
    parser.add_argument("--run", required=True, type=Path)
    args = parser.parse_args()
    if args.action == "init":
        init(args.parent.resolve(), args.run.resolve())
    else:
        execute(args.run.resolve())
