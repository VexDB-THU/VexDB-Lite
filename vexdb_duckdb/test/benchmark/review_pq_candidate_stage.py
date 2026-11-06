"""Audit actual SQL IDs/timing, then apply the preregistered candidate gates."""
import argparse
import csv
import json
import math
import struct
import time
from pathlib import Path

from run_pq_candidate_stage import ROOT, save, sha, verify, state_write
from review_text_concurrency import audit_round, expected_points


def review(run, stage):
    out = run / stage
    p = verify(run, stage)
    if json.loads((out / "state.json").read_text())["status"] != "complete":
        raise RuntimeError("incomplete timed stage")
    truth = struct.unpack("<" + str(p["queries"] * 10) + "i",
                          (ROOT / p["data_dir"] / "truth-100000.i32").read_bytes())
    all_points, all_queries, canonical = [], [], {}
    for number in (1, 2):
        for name, digest in json.loads((out / f"round{number}-hashes.json").read_text()).items():
            if sha(out / name) != digest:
                raise RuntimeError("completed result changed: " + name)
        points, queries = audit_round(out / f"round{number}", p, expected_points(p, number), truth, canonical)
        all_points += [dict(x, round=number) for x in points]
        all_queries += [dict(x, round=number) for x in queries]
    if sum(x["calls"] for x in all_points) != p["expected_sql_calls"]:
        raise RuntimeError("incomplete SQL coverage")
    checks, candidate_results = [], []
    for name in p["candidates"]:
        candidate_points = [x for x in all_points if x["point"] == name]
        quality = all(x["hits"] * (10000 if stage == "dev" else 1000) >=
                      (9995 if stage == "dev" else 999) * x["calls"] * 10 for x in candidate_points)
        stable = all(x["returned_set_variations"] == 0 and x["occupancy_ok"] for x in candidate_points)
        ratios, compared = [], []
        for number in (1, 2):
            for c in (16, 64):
                candidates = [x for x in candidate_points if x["round"] == number and x["connections"] == c]
                candidate, = candidates
                raw = [x for x in all_points if x["mode"] == "off" and x["round"] == number and x["connections"] == c]
                qualified = [x for x in raw if x["hits"] * 1000 >= 999 * x["calls"] * 10]
                if not qualified:
                    checks.append({"point": name, "round": number, "connections": c,
                                   "passed": False, "reason": "no raw control reaches 99.9%"})
                for baseline in qualified:
                    qps_ratio, p95_ratio = candidate["qps"] / baseline["qps"], candidate["p95_ms"] / baseline["p95_ms"]
                    passed = qps_ratio >= 1.1 and p95_ratio <= .9
                    checks.append({"point": name, "round": number, "connections": c,
                                   "raw": baseline["point"], "qps_ratio": qps_ratio, "p95_ratio": p95_ratio,
                                   "raw_recall": baseline["recall"], "candidate_recall": candidate["recall"], "passed": passed})
                    ratios.append(qps_ratio)
                    compared.append(baseline["point"])
        own = [x for x in checks if x["point"] == name]
        passed = quality and stable and len(own) >= 4 and all(x["passed"] for x in own)
        candidate_results.append({"point": name, "quality_passed": quality, "stable_and_occupied": stable,
                                  "speed_passed": all(x["passed"] for x in own), "passed": passed,
                                  "geomean_qps_ratio": math.exp(sum(map(math.log, ratios)) / len(ratios)) if ratios else 0,
                                  "mean_codes": sum(x["mean_codes"] for x in candidate_points) / len(candidate_points),
                                  "raw_controls_compared": sorted(set(compared))})
    result = {"reviewed_epoch": time.time(), "stage": stage, "protocol_sha256": sha(out / "protocol.json"),
              "timed_sql_lists_recomputed": p["expected_sql_calls"], "points": all_points,
              "all_returned_sets_stable": all(x["returned_set_variations"] == 0 for x in all_points),
              "all_occupancy_checks_passed": all(x["occupancy_ok"] for x in all_points),
              "checks": checks, "candidates": candidate_results}
    save(out / "review.json", result)
    for name, rows in (("summary.csv", all_points), ("per-query.csv", all_queries)):
        fields = [k for k in rows[0] if k != "variation_examples"]
        with (out / name).open("x") as stream:
            writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
            writer.writeheader()
            writer.writerows(rows)
    if stage == "dev":
        passing = [x for x in candidate_results if x["passed"]]
        best = min(passing, key=lambda x: (-x["geomean_qps_ratio"], x["mean_codes"], x["point"])) if passing else None
        chosen = next(x for x in p["methods"] if x["point"] == best["point"]) if best else None
        save(run / "selection.json", {"frozen_epoch": time.time(), "passed": bool(best), "method": chosen,
                                      "binary_sha256": sha(run / "benchmark"), "dev_review_sha256": sha(out / "review.json"),
                                      "stage_protocol_sha256": sha(out / "protocol.json"),
                                      "top_protocol_sha256": sha(run / "protocol.json"),
                                      "reason": "predeclared development recall and both-round 16/64 speed gates",
                                      "new_query_quality_read": False})
    state_write(run / "state.json", {"stage": stage + "_reviewed", "status": "active" if stage == "dev" else "complete",
                                     "research_goal_achieved": stage == "validation" and all(x["passed"] for x in candidate_results),
                                     "updated_epoch": time.time()})
    print(json.dumps(candidate_results, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--stage", choices=("dev", "validation"), required=True)
    args = parser.parse_args()
    review(args.run.resolve(), args.stage)
