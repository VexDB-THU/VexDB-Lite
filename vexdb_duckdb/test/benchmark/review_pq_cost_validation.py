"""Recompute frozen PQ validation recall from actual native returned IDs."""
import argparse
import csv
import hashlib
import json
import math
import time
from fractions import Fraction
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[3]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def review(run):
    out = run / "validation-review"
    if out.exists():
        raise RuntimeError("review already exists")
    frozen = json.loads((run / "validation-freeze.json").read_text())
    execution = json.loads((run / "validation-execution.json").read_text())
    if sha(run / "validation-freeze.json") != execution["freeze_sha256"]:
        raise RuntimeError("changed method")
    for path, digest in frozen["inputs"].items():
        if sha(ROOT / path) != digest:
            raise RuntimeError("changed frozen input: " + path)
    if sha(run / "validation-quality.points") != execution["points_sha256"] or sha(run / "audit-adc") != execution["auditor_sha256"]:
        raise RuntimeError("changed native execution")
    expected = {line.split()[0] for line in (run / "validation-quality.points").read_text().splitlines()}
    summaries, inputs, per_cohort = [], {}, {}
    for name, spec in frozen["cohorts"].items():
        data = run / "validation" / name
        if sha(data / "manifest.json") != execution["cohort_manifests"][name]:
            raise RuntimeError("changed query manifest")
        manifest = json.loads((data / "manifest.json").read_text())
        for file, digest in manifest["files"].items():
            if sha(data / file) != digest:
                raise RuntimeError("changed query input")
        truth = np.fromfile(data / "truth.i32", dtype="<i4").reshape(spec["count"], 10)
        result = run / "validation-results" / name / "ids.csv"
        inputs[str(result.relative_to(run))] = sha(result)
        points = {}
        for row in csv.DictReader(result.open()):
            points.setdefault(row["point"], []).append(row)
        if set(points) != expected:
            raise RuntimeError("missing/extra points")
        per_cohort[name] = {}
        for point, rows in points.items():
            if len(rows) != spec["count"] or {int(row["query"]) for row in rows} != set(range(spec["count"])):
                raise RuntimeError("incomplete query coverage")
            hits, codes, refines = 0, [], []
            for row in rows:
                ids = list(map(int, row["ids"].split()))
                distances = list(map(float, row["distances"].split()))
                if (len(ids) != 10 or len(set(ids)) != 10 or any(not 0 <= x < spec["rows"] for x in ids)
                        or len(distances) != 10 or not all(map(math.isfinite, distances))):
                    raise RuntimeError("invalid result")
                hits += len(set(ids) & set(map(int, truth[int(row["query"])])))
                codes.append(int(row["codes_scored"]))
                refines.append(int(row["raw_vectors_refined"]))
            passed = Fraction(hits, 10 * spec["count"]) >= Fraction(999, 1000)
            record = {"cohort": name, "point": point, "hits": hits, "slots": 10 * spec["count"],
                      "strict_recall": hits / (10 * spec["count"]), "passed": passed,
                      "mean_codes_scored": sum(codes) / len(rows), "mean_raw_vectors_refined": sum(refines) / len(rows),
                      "scope": spec["scope"]}
            summaries.append(record)
            per_cohort[name][point] = record
    out.mkdir()
    with (out / "points.csv").open("x") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(summaries[0]))
        writer.writeheader()
        writer.writerows(summaries)
    result = {"epoch": time.time(), "candidate_ef": frozen["candidate"]["ef"],
              "candidate_passed_every_cohort": all(per_cohort[name]["candidate"]["passed"] for name in per_cohort),
              "cohorts": per_cohort, "native_result_hashes": inputs, "all_frozen_inputs_verified": True,
              "reviewer_sha256": sha(Path(__file__)),
              "scope": "Candidate fixed before new query generation; no tuning after quality. SIFT is a perturbation compatibility check; public text is a corpus-vector proxy. Raw controls are graded at the same99.9% gate, no conservative dev threshold used to hide faster eligible controls."}
    (out / "review.json").write_text(json.dumps(result, indent=2))
    for row in summaries:
        print(row["cohort"], row["point"], row["hits"], "/", row["slots"], f'{100 * row["strict_recall"]:.6f}%', flush=True)
    print("candidate passed every cohort:", result["candidate_passed_every_cohort"], flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", type=Path, required=True)
    review(parser.parse_args().run.resolve())
