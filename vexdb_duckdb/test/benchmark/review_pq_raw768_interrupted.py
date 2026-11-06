"""Audit a disk-interrupted extra baseline without hiding missing timings."""
import argparse
import csv
import json
import os
import shutil
import struct
import time
from pathlib import Path

from run_pq_budget_validation import ROOT, save, sha, verify
from review_text_concurrency import audit_round, expected_points


def review(parent, run, out):
    state = json.loads((run / "state.json").read_text())
    if state["status"] != "failed" or state.get("active_pid"):
        raise RuntimeError("requires a terminated, explicitly failed supplement")
    for job in state["jobs"]:
        try:
            os.kill(job["pid"], 0)
        except ProcessLookupError:
            continue
        raise RuntimeError("a recorded native PID still exists; check ownership first")
    frozen = json.loads((run / "inputs.json").read_text())
    verify(frozen)
    primary = json.loads((parent / "review.json").read_text())
    protocol = json.loads((run / "protocol.json").read_text())
    out.mkdir(exist_ok=False)
    inputs = dict(frozen["inputs"])
    for path in [Path(__file__).resolve(), *sorted(run.rglob("*"))]:
        if path.is_file():
            inputs[str(path.relative_to(ROOT))] = sha(path)
    save(out / "audit-inputs.json", {"frozen_epoch": time.time(), "inputs": inputs})
    qualifiers, points, query_rows, checks = {}, [], [], []
    sql_lists = 0
    for cohort in ("a", "b"):
        truth = struct.unpack("<10240i", (parent / "queries" / cohort / "truth-100000.i32").read_bytes())
        baseline = {"point": "raw-ef768", "mode": "off", "ef": 768, "factor": 1}
        p = {"rows": 100000, "queries": 1024, "repeats_per_worker": 1,
             "methods": [baseline], "connections": [1]}
        canonical = {}
        qualified, _ = audit_round(run / cohort / "qualifier", p, expected_points(p, 1), truth, canonical)
        q = qualified[0]
        qualifiers[cohort] = {"hits": q["hits"], "denominator": 10240,
                              "recall": q["recall"], "qualified": q["hits"] * 1000 >= 999 * 10240,
                              "ids_sha256": q["canonical_ids_sha256"]}
        sql_lists += q["calls"]
        if not qualifiers[cohort]["qualified"]:
            continue
        p.update(methods=[baseline, protocol["candidate"]], connections=[16, 64])
        for number, connections in ((1, [16, 64]), (2, [64])):
            current = dict(p, connections=connections)
            # Round2 has only these two fully saved points. The interrupted
            # 16-client point is never completed or inferred by this reviewer.
            audited, per_query = audit_round(run / cohort / f"round{number}", current,
                                              expected_points(current, number), truth, canonical)
            if number == 1:
                for name, digest in json.loads((run / cohort / "round1-hashes.json").read_text()).items():
                    if sha(run / cohort / name) != digest:
                        raise RuntimeError("completed first-round result changed")
            points.extend(dict(x, cohort=cohort, round=number,
                               original_round_status="complete" if number == 1 else "interrupted; complete64 points only")
                          for x in audited)
            query_rows.extend(dict(x, cohort=cohort, round=number) for x in per_query)
            sql_lists += sum(x["calls"] for x in audited)
            for c in connections:
                same = [x for x in audited if x["connections"] == c]
                raw, = [x for x in same if x["mode"] == "off"]
                candidate, = [x for x in same if x["mode"] == "pq_graph"]
                old, = [x for x in primary["points"] if x["cohort"] == cohort and x["round"] == number
                        and x["connections"] == c and x["mode"] == "pq_graph"]
                if candidate["canonical_ids_sha256"] != old["canonical_ids_sha256"]:
                    raise RuntimeError("candidate IDs differ from the unchanged primary method")
                gates = {"candidate_unchanged": True,
                         "both_recall_ge_99_9": all(x["hits"] * 1000 >= 999 * x["calls"] * 10 for x in same),
                         "stable_and_occupied": all(x["returned_set_variations"] == 0 and x["occupancy_ok"] for x in same),
                         "qps_ge_1_10": candidate["qps"] >= 1.1 * raw["qps"],
                         "p95_le_0_90": candidate["p95_ms"] <= .9 * raw["p95_ms"]}
                checks.append({"cohort": cohort, "round": number, "connections": c, "checks": gates,
                               "qps_ratio": candidate["qps"] / raw["qps"],
                               "p95_ratio": candidate["p95_ms"] / raw["p95_ms"], "passed": all(gates.values())})
    verify({"inputs": inputs})
    missing = [{"cohort": "b", "round": 2, "connections": 16, "methods": ["raw-ef768", "pq-ef1536-r16"],
                "reason": "native process stopped at4GiB disk guard before either16-client point completed"}]
    result = {"reviewed_epoch": time.time(), "parent_primary_goal_passed": primary["goal_passed"],
              "qualifiers": qualifiers, "points": points, "checks": checks, "missing": missing,
              "sql_lists_recomputed": sql_lists, "completed_matched_pairs": len(checks),
              "failed_matched_pairs": sum(not x["passed"] for x in checks),
              "all_available_extra_comparisons_passed": all(x["passed"] for x in checks),
              "supplement_complete": False, "overall_goal_including_extra_control": False,
              "source_state": state["status"], "original_deadline_epoch": protocol["deadline_epoch"],
              "free_disk_bytes_at_review": shutil.disk_usage(run).free,
              "scope": "Independent review only. Keep source failure and incomplete requests; no new native runs, tuning, deadline extension or result rewriting."}
    save(out / "review.json", result)
    for name, rows in (("summary.csv", points), ("per-query.csv", query_rows)):
        with (out / name).open("x") as f:
            fields = [k for k in rows[0] if k != "variation_examples"]
            writer = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
            writer.writeheader()
            writer.writerows(rows)
    print("reviewed", sql_lists, "saved SQL lists; failed pairs", result["failed_matched_pairs"],
          "; missing16-client pair preserved; overall goal false")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--parent", type=Path, required=True)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    review(args.parent.resolve(), args.run.resolve(), args.out.resolve())
