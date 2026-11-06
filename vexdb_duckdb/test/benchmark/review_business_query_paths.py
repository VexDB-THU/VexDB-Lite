"""Review frozen native SQL path evidence; never infer QPS from this preflight."""
import argparse
import csv
import hashlib
import json
from collections import defaultdict
from fractions import Fraction
from pathlib import Path


def sha(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def review(run, out):
    if out.exists():
        raise RuntimeError("refusing to overwrite review evidence")
    protocol = json.loads((run / "registration.json").read_text())
    state = json.loads((run / "state.json").read_text())
    assert state["status"] == "complete" and not state["active_pid"]
    assert len(state["jobs"]) == len(protocol["cohorts"])
    for job in state["jobs"]:
        assert job["status"] == "complete" and job["exit_code"] == 0
        assert job["finished_epoch"] <= protocol["deadline_epoch"]
    for path, digest in json.loads((run / "inputs.json").read_text()).items():
        assert sha(path) == digest, "changed input: " + path

    records, groups, limit_pairs = [], defaultdict(list), defaultdict(dict)
    for cohort in protocol["cohorts"]:
        name = cohort["name"]
        for path, digest in json.loads((run / (name + "-hashes.json")).read_text()).items():
            assert sha(run / path) == digest, "changed result: " + path
        rows = list(csv.DictReader((run / name / "requests.csv").open()))
        seen, case_names = set(), set()
        for row in rows:
            for key in ("limit", "query", "index_plan", "eligible", "expected_k", "returned", "hits", "illegal", "payload_ok", "admitted_delta"):
                row[key] = int(row[key])
            row["cohort"] = name
            assert row["mode"] in protocol["modes"] and row["limit"] in protocol["limits"]
            assert 0 <= row["query"] < protocol["queries_per_cohort"]
            key = row["case"], row["mode"], row["limit"], row["query"]
            assert key not in seen, "duplicate request key"
            seen.add(key)
            case_names.add(row["case"])
            ids = [int(v) for v in row["ids"].split()]
            truth = [int(v) for v in row["truth"].split()]
            assert len(set(ids)) == len(ids) == row["returned"]
            assert len(set(truth)) == len(truth) == row["expected_k"]
            assert len(set(ids) & set(truth)) == row["hits"]
            assert row["returned"] == row["expected_k"] and row["illegal"] == 0 and row["payload_ok"] == 1
            plan = (run / name / (f"{row['case']}-{row['mode']}-{row['limit']}-q{row['query']}.plan.txt")).read_text()
            assert int("VEXDB_INDEX_SCAN" in plan) == row["index_plan"] == row["admitted_delta"]
            if not row["index_plan"]:
                assert set(ids) == set(truth), "exact SQL differs from independent full-vector truth"
            pair = name, row["case"], row["mode"], row["query"]
            limit_pairs[pair][row["limit"]] = set(ids)
            groups[name, row["case"], row["mode"], row["limit"]].append(row)
            records.append(row)
        assert len(case_names) == protocol["cases"]
        assert len(rows) == protocol["cases"] * len(protocol["modes"]) * len(protocol["limits"]) * protocol["queries_per_cohort"]
    assert len(records) == protocol["expected_requests"]
    for limits in limit_pairs.values():
        assert set(limits) == set(protocol["limits"])
        assert all(ids == limits[protocol["limits"][0]] for ids in limits.values()), "IDs changed when queue setting changed"

    points = []
    for (cohort, case, mode, limit), rows in sorted(groups.items()):
        assert len(rows) == protocol["queries_per_cohort"]
        assert len({r["index_plan"] for r in rows}) == 1
        denominator = sum(r["expected_k"] for r in rows)
        hits = sum(r["hits"] for r in rows)
        recall = Fraction(hits, denominator) if denominator else None
        points.append({"cohort": cohort, "case": case, "mode": mode, "limit": limit,
                       "requests": len(rows), "index_plan": rows[0]["index_plan"],
                       "eligible_min": min(r["eligible"] for r in rows), "eligible_max": max(r["eligible"] for r in rows),
                       "hits": hits, "expected_k_sum": denominator,
                       "recall": float(recall) if recall is not None else None,
                       "quality_gate": recall is None or recall >= Fraction(999, 1000),
                       "admitted": sum(r["admitted_delta"] for r in rows)})
    result = {"requests": len(records), "points": len(points), "queries_per_cohort": protocol["queries_per_cohort"],
              "inputs_and_results_unchanged": True, "all_jobs_within_deadline": True,
              "limit_id_equivalence": True, "permission_payload_and_result_counts_passed": True,
              "exact_points_match_independent_truth": True,
              "sampled_quality_gate_passed": all(p["quality_gate"] for p in points),
              "index_requests": sum(r["index_plan"] for r in records),
              "nonindex_requests": sum(not r["index_plan"] for r in records),
              "scope": "Native SQL path/quality preflight on existing exposed queries and explicit proxy metadata. Not new-query acceptance, concurrent load, production traffic share, or performance evidence."}
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
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    review(args.run.resolve(), args.out.resolve())
