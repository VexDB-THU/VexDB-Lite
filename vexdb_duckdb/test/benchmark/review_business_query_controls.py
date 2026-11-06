"""Check direct/view controls against the already audited full-vector truth."""
import argparse
import csv
import json
from collections import defaultdict
from fractions import Fraction
from pathlib import Path

from review_business_query_paths import sha


def review(run, out):
    if out.exists():
        raise RuntimeError("refusing to overwrite controls review")
    control = run / "controls"
    registration = json.loads((control / "registration.json").read_text())
    protocol = json.loads((run / "registration.json").read_text())
    assert registration["deadline_epoch"] == protocol["deadline_epoch"]
    jobs = json.loads((control / "jobs.json").read_text())
    assert len(jobs) == len(protocol["cohorts"])
    for job in jobs:
        assert job["exit_code"] == 0 and job["finished_epoch"] <= registration["deadline_epoch"]
    for path, digest in json.loads((control / "inputs.json").read_text()).items():
        assert sha(path) == digest, "changed control input: " + path
    for path, digest in json.loads((control / "hashes.json").read_text()).items():
        assert sha(control / path) == digest, "changed control output: " + path
    points, calls = [], 0
    for cohort in protocol["cohorts"]:
        name = cohort["name"]
        path = run / name / "requests.csv"
        digest = json.loads((run / (name + "-hashes.json")).read_text())[f"{name}/requests.csv"]
        assert sha(path) == digest
        truth = {int(r["query"]): set(map(int, r["truth"].split())) for r in csv.DictReader(path.open())
                 if r["case"] == "plain_ids" and r["mode"] == "off" and r["limit"] == "0"}
        rows = list(csv.DictReader((control / name / "requests.csv").open()))
        assert len(rows) == 6 * 2 * protocol["queries_per_cohort"]
        groups, seen, comparable = defaultdict(list), set(), defaultdict(dict)
        for row in rows:
            key = row["case"], row["mode"], int(row["query"])
            assert key not in seen and key[2] in truth and row["mode"] in protocol["modes"]
            seen.add(key)
            ids = set(map(int, row["ids"].split()))
            assert len(ids) == len(row["ids"].split()) == len(truth[key[2]])
            plan = (control / name / f"{key[0]}-{key[1]}-q{key[2]}.plan.txt").read_text()
            assert int("VEXDB_INDEX_SCAN" in plan) == int(row["index_plan"]) == int(row["admitted_delta"])
            hits = len(ids & truth[key[2]])
            if not int(row["index_plan"]):
                assert ids == truth[key[2]], "exact control differs from independent truth"
            groups[key[:2]].append((row, hits, len(truth[key[2]])))
            comparable[key[1], key[2]][key[0]] = ids
        for values in comparable.values():
            direct = values["direct_before_views"]
            for case in ("direct_after_view", "direct_payload_before_temp_write", "direct_after_temp_write"):
                assert values[case] == direct, "direct control IDs changed with setup"
        assert len(groups) == 12
        for (case, mode), group in sorted(groups.items()):
            assert len(group) == protocol["queries_per_cohort"]
            plans = {int(r["index_plan"]) for r, _, _ in group}
            assert len(plans) == 1
            hits, total = sum(x[1] for x in group), sum(x[2] for x in group)
            recall = Fraction(hits, total)
            points.append({"cohort": name, "case": case, "mode": mode, "requests": len(group),
                           "index_plan": plans.pop(), "hits": hits, "expected_k_sum": total,
                           "recall": float(recall), "quality_gate": recall >= Fraction(999, 1000)})
        calls += len(rows)
    assert calls == registration["expected_requests"]
    result = {"requests": calls, "points": len(points), "hashes_and_deadline_passed": True,
              "direct_setup_id_equivalence": True, "exact_controls_match_truth": True,
              "sampled_quality_gate_passed": all(p["quality_gate"] for p in points),
              "scope": "Direct versus business-view and temp-write path controls, not latency or customer coverage."}
    out.mkdir(parents=True)
    with (out / "points.csv").open("w") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(points[0]))
        writer.writeheader()
        writer.writerows(points)
    (out / "review.json").write_text(json.dumps(result, indent=2))
    (out / "reviewer-source.sha256").write_text(sha(Path(__file__)) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    review(args.run.resolve(), args.out.resolve())
