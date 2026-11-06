"""A separately registered higher-recall diagnostic, frozen before validation."""
import argparse
import json
import subprocess
from pathlib import Path

from run_pq_graph import ROOT, audit, now, save, sha, verify


def register(run):
    protocol = verify(run)
    if (run / "validation").exists():
        raise RuntimeError("must register before inspecting the primary validation slice")
    selection = json.loads((run / "selection.json").read_text())
    candidates = [c for c in selection["all_candidates"] if c["passed"]]
    if not candidates:
        raise RuntimeError("no candidate passed the original fit gates")
    # Quality first, using only the original fit evidence. Ties prefer speed.
    chosen = max(candidates, key=lambda c: (
        min(v["candidate"]["recall"] for v in c["cases"].values()),
        sum(v["candidate"]["recall"] for v in c["cases"].values()),
        -c["score"], -c["ef"], -c["factor"]))
    save(run / "quality-selection.json", {
        "registered_at": now(), "label": "separate higher-recall diagnostic; never changes primary selection or its verdict",
        "reason": "The primary fit winner sits exactly at the 0.2 percentage-point loss boundary on both cases. Check a quality-first choice as a separate pre-validation diagnostic.",
        "rule": "Among original fit gate passers, maximize worst-case fit recall, then sum recall, then minimize original speed score/ef/factor. Same frozen gates on validation; two candidates are being checked on the same previously used dev slice, not independent holdouts.",
        "primary_sha256": sha(run / "selection.json"), "script_sha256": sha(Path(__file__)),
        "ef": chosen["ef"], "factor": chosen["factor"], "fit_evidence": chosen,
        "protocol_sha256": sha(run / "protocol.json"),
    })
    (run / "quality-selection.sha256").write_text(sha(run / "quality-selection.json") + "\n")
    print("registered quality choice", chosen["ef"], chosen["factor"], flush=True)


def check_selection(run):
    p = json.loads((run / "quality-selection.json").read_text())
    if sha(run / "quality-selection.json") != (run / "quality-selection.sha256").read_text().strip():
        raise RuntimeError("quality choice changed")
    if sha(Path(__file__)) != p["script_sha256"] or sha(run / "selection.json") != p["primary_sha256"] or sha(run / "protocol.json") != p["protocol_sha256"]:
        raise RuntimeError("registered source or primary protocol changed")
    return p


def execute(run):
    protocol, choice = verify(run), check_selection(run)
    ef, factor = choice["ef"], choice["factor"]
    points = [(point, mode, width, r, c) for c in (1, 8) for point, mode, width, r in (
        (f"graph-ef{ef}-r{factor}", "pq_graph", ef, factor), (f"full-r{factor}", "pq_only", 128, factor),
        ("raw-ef128", "off", 128, 1), ("raw-ef512", "off", 512, 1))]
    phase = "quality-validation"
    base = run / phase
    base.mkdir(exist_ok=False)
    for case in protocol["cases"]:
        for round_id in (1, 2):
            name = case["name"] + f"-round{round_id}"
            config = base / (name + ".points")
            config.write_text("".join(" ".join(map(str, p)) + "\n" for p in (points if round_id == 1 else points[::-1])))
            offset, count = protocol["queries"]["validation"]
            cmd = [str(run / "benchmark"), str(run / "vexdb_lite.duckdb_extension"), str(ROOT / case["db"]),
                   str(ROOT / case["data_dir"]), str(case["rows"]), str(case["dim"]), str(offset), str(count),
                   str(config), str(base / name)]
            print("START quality diagnostic", name, flush=True)
            with (base / (name + ".log")).open("x") as log:
                subprocess.run(cmd, check=True, stdout=log, stderr=subprocess.STDOUT)
            print("DONE quality diagnostic", name, flush=True)
    verify(run)
    save(run / "quality-validation-audit.json", {"audited_at": now(), "results": audit(run, protocol, phase)})
    save(run / "quality-validation-hashes.json", {str(p.relative_to(run)): sha(p) for p in base.rglob("*") if p.is_file()})


def review(run):
    protocol = verify(run)
    check_selection(run)
    for path, digest in json.loads((run / "quality-validation-hashes.json").read_text()).items():
        if sha(run / path) != digest:
            raise RuntimeError("quality diagnostic result changed")
    checked = audit(run, protocol, "quality-validation")
    print("quality diagnostic independently audited", sum(p["calls"] for c in checked.values() for p in c.values()), "SQL lists; hashes verified")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("register", "run", "review"))
    parser.add_argument("--run", type=Path, required=True)
    args = parser.parse_args()
    {"register": register, "run": execute, "review": review}[args.action](args.run.resolve())
