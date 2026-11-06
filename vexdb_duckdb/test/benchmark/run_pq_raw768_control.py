"""Posthoc raw768 control, with the main PQ candidate and query sets unchanged."""
import argparse
import json
import os
import struct
import time
from pathlib import Path

from run_pq_budget_validation import ROOT, save, sha, verify
from run_text_concurrency import job, state_write
from review_text_concurrency import audit_round, expected_points


def run_control(parent, run):
    parent_protocol = json.loads((parent / "protocol.json").read_text())
    primary = json.loads((parent / "review.json").read_text())
    if json.loads((parent / "state.json").read_text())["status"] != "complete":
        raise RuntimeError("never overlap main native query timings")
    plan = json.loads((parent / "supplemental-raw768-plan.json").read_text())
    run.mkdir(exist_ok=False)
    protocol = {"registered_epoch": time.time(), "deadline_epoch": time.time() + plan["window_seconds_from_launch"],
                "disk_floor_bytes": 4 * 2**30, "parent": str(parent.relative_to(ROOT)),
                "parent_review_sha256": sha(parent / "review.json"), "plan_sha256": sha(parent / "supplemental-raw768-plan.json"),
                "kind": "posthoc raw768 baseline only; no parameter tuning or new-query claim",
                "candidate": parent_protocol["candidate"], "raw_ef": 768, "connections": [16, 64]}
    save(run / "protocol.json", protocol)
    files = {Path(__file__).resolve(), run / "protocol.json", ROOT / parent_protocol["database"], parent / "benchmark",
             parent / "freeze.json", parent / "review.json", parent / "supplemental-raw768-plan.json",
             ROOT / parent_protocol["extension_run"] / "vexdb_lite.duckdb_extension"}
    for cohort in ("a", "b"):
        files.update(f for f in (parent / "queries" / cohort).iterdir() if f.is_file())
    frozen = {"inputs": {str(f.relative_to(ROOT)): sha(f) for f in files}}
    frozen["inputs"].update(json.loads((parent / "freeze.json").read_text())["inputs"])
    save(run / "inputs.json", frozen)
    state = {"status": "running", "jobs": [], "controller_pid": os.getpid(), "active_pid": None}
    state_write(run, state)
    qualifiers, points, checks, calls = {}, [], [], 0
    try:
        for cohort in ("a", "b"):
            verify(frozen)
            out = run / cohort
            out.mkdir()
            data = parent / "queries" / cohort
            truth = struct.unpack("<10240i", (data / "truth-100000.i32").read_bytes())
            baseline = {"point": "raw-ef768", "mode": "off", "ef": 768, "factor": 1}
            config = out / "qualifier.points"
            config.write_text("raw-ef768 off 768 1 1\n")

            def command(config, destination):
                return [parent / "benchmark", "run", ROOT / parent_protocol["extension_run"] / "vexdb_lite.duckdb_extension",
                        ROOT / parent_protocol["database"], data, "100000", "1536", "1024", "1", config, destination]

            job(run, state, protocol, cohort + "-qualifier", command(config, out / "qualifier"), 300)
            qp = {"rows": 100000, "queries": 1024, "repeats_per_worker": 1,
                  "methods": [baseline], "connections": [1]}
            canonical = {}
            q, _ = audit_round(out / "qualifier", qp, expected_points(qp, 1), truth, canonical)
            q = q[0]
            calls += q["calls"]
            eligible = q["hits"] * 1000 >= 999 * q["calls"] * 10
            qualifiers[cohort] = {"recall": q["recall"], "hits": q["hits"], "denominator": 10240,
                                  "qualified": eligible, "ids_sha256": q["canonical_ids_sha256"],
                                  "scope": "full-cohort quality qualification; no speed ranking from this single-client check"}
            save(out / "qualifier-review.json", qualifiers[cohort])
            if not eligible:
                print("SKIP matched-quality speed for", cohort, "raw768 recall", q["recall"], flush=True)
                continue
            methods = [baseline, protocol["candidate"]]
            p = {"rows": 100000, "queries": 1024, "repeats_per_worker": 1,
                 "methods": methods, "connections": [16, 64]}
            save(out / "protocol.json", p)
            own = []
            for number in (1, 2):
                verify(frozen)
                expected = expected_points(p, number)
                config = out / f"round{number}.points"
                config.write_text("".join(f"{x['point']} {x['mode']} {x['ef']} {x['factor']} {x['connections']}\n" for x in expected))
                job(run, state, protocol, cohort + "-round" + str(number), command(config, out / f"round{number}"), 1200)
                # Audit only after this native process exits; never compete
                # with its timing threads. Preserve original result bytes.
                audited, _ = audit_round(out / f"round{number}", p, expected, truth, canonical)
                own += [dict(x, cohort=cohort, round=number) for x in audited]
                save(out / f"round{number}-hashes.json", {str(f.relative_to(out)): sha(f)
                     for f in sorted((out / f"round{number}").rglob("*")) if f.is_file()})
            for number in (1, 2):
                for c in (16, 64):
                    same = [x for x in own if x["round"] == number and x["connections"] == c]
                    raw, = [x for x in same if x["mode"] == "off"]
                    candidate, = [x for x in same if x["mode"] == "pq_graph"]
                    old, = [x for x in primary["points"] if x["cohort"] == cohort and x["round"] == number and
                            x["connections"] == c and x["mode"] == "pq_graph"]
                    if candidate["canonical_ids_sha256"] != old["canonical_ids_sha256"]:
                        raise RuntimeError("candidate IDs changed from the primary fixed method")
                    gates = {"same_candidate": True,
                             "raw_and_candidate_ge_99_9": all(x["hits"] * 1000 >= 999 * x["calls"] * 10 for x in same),
                             "stable_and_occupied": all(x["occupancy_ok"] and x["returned_set_variations"] == 0 for x in same),
                             "qps_ge_1_10": candidate["qps"] >= 1.1 * raw["qps"],
                             "p95_le_0_90": candidate["p95_ms"] <= .9 * raw["p95_ms"]}
                    checks.append({"cohort": cohort, "round": number, "connections": c, "checks": gates,
                                   "qps_ratio": candidate["qps"] / raw["qps"],
                                   "p95_ratio": candidate["p95_ms"] / raw["p95_ms"], "passed": all(gates.values())})
            calls += sum(x["calls"] for x in own)
            points += own
        verify(frozen)
        result = {"reviewed_epoch": time.time(), "kind": protocol["kind"], "parent_primary_goal_passed": primary["goal_passed"],
                  "qualifiers": qualifiers, "points": points, "checks": checks, "sql_lists_recomputed": calls,
                  "all_applicable_extra_comparisons_passed": all(x["passed"] for x in checks),
                  "comparable_cohorts": [k for k, v in qualifiers.items() if v["qualified"]],
                  "overall_goal_including_extra_control": primary["goal_passed"] and all(x["passed"] for x in checks),
                  "protocol_sha256": sha(run / "protocol.json"), "parent_review_sha256": sha(parent / "review.json")}
        save(run / "review.json", result)
        state.update(status="complete", active_pid=None, controller_pid=None)
        state_write(run, state)
        print("posthoc raw768 control complete; all applicable points passed:", result["all_applicable_extra_comparisons_passed"], flush=True)
    except BaseException as error:
        state.update(status="failed", controller_pid=None, error=str(error))
        state_write(run, state)
        raise


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--parent", required=True, type=Path)
    parser.add_argument("--run", required=True, type=Path)
    args = parser.parse_args()
    run_control(args.parent.resolve(), args.run.resolve())
