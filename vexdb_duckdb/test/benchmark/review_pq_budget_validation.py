"""Independently recompute both fixed-cohort SQL tests and matched-quality gates."""
import argparse
import csv
import json
import struct
import time
from pathlib import Path

from run_pq_budget_validation import ROOT, save, sha, verify
from review_text_concurrency import audit_round, expected_points


def review(run):
    root_protocol = json.loads((run / "protocol.json").read_text())
    frozen = json.loads((run / "freeze.json").read_text())
    verify(frozen)
    if json.loads((run / "state.json").read_text())["status"] != "complete":
        raise RuntimeError("keep incomplete tests incomplete")
    points, queries, gates = [], [], []
    sets = []
    for cohort in ("a", "b"):
        out = run / cohort
        p = json.loads((out / "protocol.json").read_text())
        verify({"inputs": p["data_sha256"]})
        data = ROOT / p["data_dir"]
        manifest = json.loads((data / "manifest.json").read_text())
        sets.append(set(manifest["source_rows"]))
        if manifest["freeze_sha256"] != sha(run / "freeze.json"):
            raise RuntimeError("query cohort not bound to the method freeze")
        truth = struct.unpack("<10240i", (data / "truth-100000.i32").read_bytes())
        canonical, own = {}, []
        for number in (1, 2):
            for name, digest in json.loads((out / f"round{number}-hashes.json").read_text()).items():
                if sha(out / name) != digest:
                    raise RuntimeError("result hash changed: " + name)
            audited, query_rows = audit_round(out / f"round{number}", p, expected_points(p, number), truth, canonical)
            own += [dict(x, cohort=cohort, round=number) for x in audited]
            queries += [dict(x, cohort=cohort, round=number) for x in query_rows]
        if sum(x["calls"] for x in own) != p["expected_sql_calls"]:
            raise RuntimeError("wrong cohort call coverage")
        for number in (1, 2):
            for connections in p["connections"]:
                same = [x for x in own if x["round"] == number and x["connections"] == connections]
                candidate, = [x for x in same if x["mode"] == "pq_graph"]
                raw = [x for x in same if x["mode"] == "off" and x["hits"] * 1000 >= 999 * x["calls"] * 10]
                checks = {"recall_ge_99_9": candidate["hits"] * 1000 >= 999 * candidate["calls"] * 10,
                          "occupied": all(x["occupancy_ok"] for x in same),
                          "stable": all(x["returned_set_variations"] == 0 for x in same),
                          "qualified_raw_exists": bool(raw),
                          "qps_ge_1_10": bool(raw) and all(candidate["qps"] >= 1.1 * x["qps"] for x in raw),
                          "p95_le_0_90": bool(raw) and all(candidate["p95_ms"] <= .9 * x["p95_ms"] for x in raw)}
                comparisons = [{"raw": x["point"], "raw_recall": x["recall"],
                                "candidate_recall": candidate["recall"],
                                "qps_ratio": candidate["qps"] / x["qps"],
                                "p95_ratio": candidate["p95_ms"] / x["p95_ms"]} for x in raw]
                gates.append({"cohort": cohort, "round": number, "connections": connections,
                              "checks": checks, "comparisons": comparisons, "passed": all(checks.values())})
        points += own
    if sets[0] & sets[1]:
        raise RuntimeError("query source rows overlap between cohorts")
    passed = all(g["passed"] for g in gates)
    result = {"reviewed_epoch": time.time(), "protocol_sha256": sha(run / "protocol.json"),
              "freeze_sha256": sha(run / "freeze.json"), "timed_sql_lists_recomputed": sum(x["calls"] for x in points),
              "query_count_per_cohort": 1024, "cohorts": 2, "points": points, "gates": gates,
              "all_returned_sets_stable": all(x["returned_set_variations"] == 0 for x in points),
              "all_occupancy_checks_passed": all(x["occupancy_ok"] for x in points), "goal_passed": passed,
              "scope": "new corpus-vector proxy queries; same graph seed, shared local host; no universal recall guarantee"}
    save(run / "review.json", result)
    for name, rows in (("summary.csv", points), ("per-query.csv", queries)):
        fields = [k for k in rows[0] if k != "variation_examples"]
        with (run / name).open("x") as stream:
            writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
            writer.writeheader()
            writer.writerows(rows)
    print("independently recomputed", result["timed_sql_lists_recomputed"], "SQL lists; two-cohort goal:", passed)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", required=True, type=Path)
    review(parser.parse_args().run.resolve())
