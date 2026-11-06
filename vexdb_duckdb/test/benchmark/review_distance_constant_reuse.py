"""Apply the registered speed gate after independent filtered-ID review.

Read-only with respect to the native queue and prior review. Retain all pairs,
including regressions and low occupancy; do not select new configurations.
"""
import argparse
import csv
import json
import time
from pathlib import Path

from review_filtered_search import csv_rows, sha


def review(run, out):
    if out.exists():
        raise RuntimeError("refusing to overwrite prior comparison")
    state = json.loads((run / "state.json").read_text())
    if state.get("controller_pid") or state.get("active_pid"):
        raise RuntimeError("native timing must finish first")
    protocol = json.loads((run / "protocol.json").read_text())
    audit = json.loads((run / "review/review.json").read_text())
    points = csv_rows(run / "review/timed-points.csv")
    quality = csv_rows(run / "review/quality.csv")
    keyed = {(r["dataset"], r["round"], r["point"], r["version"]): r for r in points}
    pairs, missing, changes = [], [], []
    compared = timed_lists = quality_lists = 0
    for dataset in protocol["datasets"]:
        for round_id in (1, 2):
            name = f"{dataset['name']}-baseline-r{round_id}"
            for line in (run / (name + ".points")).read_text().splitlines():
                point = line.split()[0]
                keys = [(dataset["name"], str(round_id), point, v) for v in ("baseline", "new")]
                if any(k not in keyed for k in keys):
                    missing.append(name + "/" + point)
                    continue
                old, new = (keyed[k] for k in keys)
                pairs.append({
                    "dataset": dataset["name"], "round": round_id, "point": point,
                    "method": old["method"], "filter": old["filter"],
                    "connections": int(old["connections"]),
                    "baseline_qps": float(old["qps"]), "new_qps": float(new["qps"]),
                    "qps_ratio": float(new["qps"]) / float(old["qps"]),
                    "baseline_p95_ms": float(old["p95_ms"]), "new_p95_ms": float(new["p95_ms"]),
                    "p95_ratio": float(new["p95_ms"]) / float(old["p95_ms"]),
                    "baseline_recall": float(old["strict_recall"]), "new_recall": float(new["strict_recall"]),
                    "baseline_occupied": float(old["mean_active_clients"]),
                    "new_occupied": float(new["mean_active_clients"]),
                })
                for suffix in ("timing.csv", "quality.csv"):
                    paths = [run / f"{dataset['name']}-{v}-r{round_id}" / (point + "." + suffix)
                             for v in ("baseline", "new")]
                    if not any(p.exists() for p in paths):
                        continue  # quality is collected once per configuration
                    if not all(p.exists() for p in paths):
                        missing.append(str(paths[0]) + ": unmatched list file")
                        continue
                    def read(path):
                        rows = csv_rows(path)
                        return {((int(r["worker"]), int(r["query"])) if "worker" in r else (int(r["query"]),)):
                                tuple(sorted(map(int, r["ids"].split()))) for r in rows}
                    a, b = map(read, paths)
                    if a.keys() != b.keys():
                        missing.append(str(paths[0]) + ": mismatched query keys")
                        continue
                    compared += len(a)
                    if suffix == "timing.csv":
                        timed_lists += len(a) * 2
                    else:
                        quality_lists += len(a) * 2
                    for key in a:
                        if a[key] != b[key]:
                            changes.append({"dataset": dataset["name"], "round": round_id, "point": point,
                                "kind": suffix, "query_key": key, "baseline": a[key], "new": b[key]})
    gate = protocol["primary_speed_gate"]
    primary = [r for r in pairs if r["dataset"] == gate["dataset"] and r["filter"] == gate["filter"]
               and r["connections"] in gate["connections"] and r["round"] in gate["rounds"]]
    failures = [r for r in primary if r["qps_ratio"] < gate["min_qps_ratio"] or r["p95_ratio"] > gate["max_p95_ratio"]]
    filtered = [r for r in points + quality if r["filter"] != "none"]
    invalid = [r for r in filtered if float(r["strict_recall"]) < .999 or int(r["underfilled_calls"]) or int(r["illegal_ids"])]
    complete = (state["status"] == "complete" and not audit["missing"] and not missing and
                audit["observed_timed_calls"] == protocol["expected_timed_calls"] and
                timed_lists == protocol["expected_timed_calls"])
    out.mkdir(parents=True)
    if pairs:
        with (out / "pairs.csv").open("x") as f:
            writer = csv.DictWriter(f, fieldnames=list(pairs[0]))
            writer.writeheader()
            writer.writerows(pairs)
    (out / "changed-ids.json").write_text(json.dumps(changes, indent=2))
    result = {"reviewed_epoch": time.time(), "complete": complete,
        "pairs": len(pairs), "compared_list_pairs": compared,
        "timed_lists": timed_lists, "quality_lists": quality_lists,
        "changed_id_sets": len(changes), "missing": missing,
        "all_matched_ids": not changes and not missing and complete,
        "filtered_invalid_points_or_sets": len(invalid),
        "all_filtered_quality_pass": bool(filtered) and not invalid and complete,
        "primary_points": primary, "primary_speed_failures": failures,
        "primary_gate_pass": (complete and len(primary) == 4 and not failures and not invalid and not changes),
        "occupancy_flags": audit["flags"],
        "scope": "All actual per-worker/query ID sets compared; order of equal-distance IDs is ignored. Speed gate copied from pre-timing protocol. Short closed-loop local runs and shared-host variation remain limitations.",
        "inputs": {str(p.relative_to(run)): sha(p) for p in [run / "protocol.json", run / "review/review.json",
            run / "review/timed-points.csv", run / "review/quality.csv"]},
        "reviewer_sha256": sha(Path(__file__))}
    (out / "comparison.json").write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    review(args.run.resolve(), args.out.resolve())
