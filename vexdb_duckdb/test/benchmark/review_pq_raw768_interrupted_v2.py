"""Audit a disk-interrupted extra baseline without hiding missing timings."""
import argparse
import csv
import json
import io
import os
import shutil
import struct
import time
from pathlib import Path

from run_pq_budget_validation import ROOT, save, sha, verify
from review_text_concurrency import audit_round, expected_points


def complete_sql_view(parent, source, out):
    """Keep real SQL bytes; identify incomplete counters without inventing them."""
    view = out / "complete64-sql-view"
    view.mkdir()
    payload = (source / "counts.csv").read_bytes()
    if payload.endswith(b"\n"):
        raise RuntimeError("this version handles the observed truncated final counter row only")
    boundary = payload.rfind(b"\n") + 1
    prefix, tail = payload[:boundary], payload[boundary:]
    rows = list(csv.DictReader(io.StringIO(prefix.decode())))
    reference_path = parent / "b" / "round2" / "counts.csv"
    reference = list(csv.DictReader(reference_path.open()))
    if len(reference) != 1024 or len(rows) != 979:
        raise RuntimeError("counter layout differs from the recorded interruption; do not skip it")
    for q, row in enumerate(rows):
        if row != reference[q] or int(row["query"]) != q:
            raise RuntimeError("available native counter differs from the frozen method reference")
    # The original 979 complete rows and truncated tail remain untouched.
    # A separate view uses the complete main-study reference ONLY to validate
    # SQL ID parity through the unchanged auditor. It is never called a new
    # full measurement of this interrupted process's estimation work.
    (view / "actual-native-prefix.csv").write_bytes(prefix)
    (view / "actual-native-truncated-tail.bin").write_bytes(tail)
    shutil.copyfile(reference_path, view / "counts.csv")
    names = ["points.csv"]
    for point in ("pq-ef1536-r16", "raw-ef768"):
        for suffix in (".csv", "-workers.csv", ".explain.txt"):
            names.append(point + "-c64" + suffix)
    for name in names:
        shutil.copyfile(source / name, view / name)
        if sha(source / name) != sha(view / name):
            raise RuntimeError("SQL copy changed")
    note = {"scope": "Real completed64-client SQL files, byte-for-byte. Counter reference validates identical candidate IDs only; no filled-in native process measurements.",
            "original_counter_sha256": sha(source / "counts.csv"), "actual_complete_native_rows": len(rows),
            "required_native_rows": 1024, "native_counter_complete": False,
            "truncated_tail_bytes": len(tail), "reference_path": str(reference_path.relative_to(ROOT)),
            "reference_sha256": sha(reference_path),
            "copied_sql_files": {name: sha(view / name) for name in names}}
    save(view / "provenance.json", note)
    return view, note


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
    partial_native_note = None
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
            directory = run / cohort / f"round{number}"
            if number == 2:
                directory, partial_native_note = complete_sql_view(parent, directory, out)
            audited, per_query = audit_round(directory, current,
                                              expected_points(current, number), truth, canonical)
            if number == 2:
                for item in audited:
                    if item["mode"] != "off":
                        item["reference_mean_codes"] = item["mean_codes"]
                        item["mean_codes"] = item["max_codes"] = item["mean_refined"] = None
                        item["native_counter_complete_in_process"] = False
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
              "partial_native_counter_provenance": partial_native_note,
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
