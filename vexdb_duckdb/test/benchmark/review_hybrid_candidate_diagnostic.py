"""Independently audit retained-query candidate acquisition; no QPS claim."""
import argparse
import csv
import hashlib
import json
from collections import defaultdict
from pathlib import Path


def sha(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def review(run, previous, out):
    if out.exists():
        raise RuntimeError("refusing to overwrite evidence")
    protocol = json.loads((run / "protocol.json").read_text())
    state = json.loads((run / "state.json").read_text())
    assert state["status"] == "complete" and state["inputs_unchanged"] and not state["active_pid"]
    assert all(j["status"] == "complete" and j["finished_epoch"] <= protocol["deadline_epoch"] for j in state["jobs"])
    for path, digest in protocol["sha256"].items():
        assert sha(path) == digest, "input changed: " + path
    for path, digest in json.loads((run / "result-hashes.json").read_text()).items():
        assert sha(run / path) == digest, "output changed: " + path
    summaries, misses = [], []
    for cohort in protocol["cohorts"]:
        original = list(csv.DictReader((previous / cohort["name"] / "requests.csv").open()))
        truth = {int(r["query"]): set(map(int, r["truth"].split())) for r in original
                 if r["case"] == "plain_ids" and r["mode"] == "off" and r["limit"] == "0"}
        baseline = {(r["mode"], int(r["query"])): set(map(int, r["ids"].split())) for r in original
                    if r["case"] == "plain_ids" and r["limit"] == "0"}
        groups = defaultdict(list)
        for row in csv.DictReader((run / cohort["name"] / "ids.csv").open()):
            groups[row["point"]].append(row)
        for point, rows in groups.items():
            assert {int(r["query"]) for r in rows} == set(range(protocol["count"]))
            hit_sum, sizes, codes, refined = 0, [], [], []
            for row in rows:
                q = int(row["query"])
                ids = list(map(int, row["ids"].split()))
                assert len(set(ids)) == len(ids)
                hits = len(set(ids) & truth[q]); hit_sum += hits; sizes.append(len(ids))
                codes.append(int(row["codes_scored"])); refined.append(int(row["raw_vectors_refined"]))
                if point in ("raw-baseline", "pq-baseline"):
                    mode = "off" if point == "raw-baseline" else "pq_graph"
                    assert set(ids) == baseline[mode, q], "current baseline differs from retained SQL IDs"
                if hits < len(truth[q]):
                    misses.append({"cohort": cohort["name"], "point": point, "query": q, "hits": hits,
                                   "missing": " ".join(map(str, sorted(truth[q] - set(ids))))})
            summaries.append({"cohort": cohort["name"], "point": point, "queries": len(rows),
                              "candidate_coverage_hits": hit_sum, "expected": 10 * len(rows),
                              "candidate_coverage": hit_sum / (10 * len(rows)), "min_pool": min(sizes),
                              "max_pool": max(sizes), "mean_pq_codes": sum(codes) / len(codes),
                              "mean_raw_refined": sum(refined) / len(refined)})
    out.mkdir(parents=True)
    for name, rows in (("points.csv", summaries), ("misses.csv", misses)):
        with (out / name).open("w") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0])); writer.writeheader(); writer.writerows(rows)
    result = {"hashes_and_deadline_passed": True, "baseline_sql_ids_reproduced": True,
              "cohorts": len(protocol["cohorts"]), "points": len(summaries),
              "scope": "Retained proxies are diagnostic/development. Pool coverage is the best possible recall after reranking that pool; it is not ANN throughput or a new holdout."}
    (out / "review.json").write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--previous", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    review(args.run.resolve(), args.previous.resolve(), args.out.resolve())
