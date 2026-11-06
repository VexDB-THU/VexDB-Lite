"""Independently check the untimed, fixed-parameter candidate-pool diagnostic."""
import argparse
import csv
import hashlib
import json
import struct
import time
from pathlib import Path


def sha(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def rows(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def review(run):
    protocol = json.loads((run / "candidate-audit-protocol.json").read_text())
    main_protocol = json.loads((run / "protocol.json").read_text())
    build = json.loads((run / "candidate-audit-build.json").read_text())
    source = run / "candidate-audit-source.cpp"
    require(sha(source) == protocol["source_sha256"] == build["source_sha256"], "audit source changed")
    require(sha(run / "candidate-audit") == build["binary_sha256"], "audit executable changed")
    require(sha(run / "protocol.json") == protocol["protocol_sha256"], "main protocol changed")
    state = json.loads((run / "state.json").read_text())
    require(state["status"] == "complete", "timed queue incomplete")
    require(build["compiled_epoch"] >= max(j["finished_epoch"] for j in state["jobs"]),
            "diagnostic overlapped timed work")
    command = json.loads((run / "candidate-audit-command.json").read_text())
    truth_path = Path(command[3]) / "truth-100000.i32"
    truth_bytes = truth_path.read_bytes()
    require(len(truth_bytes) == 200 * 10 * 4, "truth shape mismatch")
    truth_flat = struct.unpack("<2000i", truth_bytes)
    truth = [set(truth_flat[q * 10:(q + 1) * 10]) for q in range(200)]
    native_rounds = []
    for number in (1, 2):
        native = {}
        for row in rows(run / f"round{number}" / "counts.csv"):
            key = row["point"], int(row["query"])
            require(key not in native, "duplicate formal native count")
            native[key] = (int(row["codes_scored"]), set(map(int, row["ids"].split())))
        native_rounds.append(native)
    names = {p["point"] for p in protocol["points"]}
    sums = {name: {"hits": 0, "pool_hits": 0, "missing_from_pool": 0,
                   "in_pool_not_returned": 0, "imperfect_queries": [],
                   "min_query_hits": 10, "code_sum": 0} for name in names}
    seen = set()
    pool_path = run / "candidate-pool-audit" / "pools.csv"
    for row in rows(pool_path):
        name, q = row["point"], int(row["query"])
        require(name in names and 0 <= q < 200, "unexpected pool coordinate")
        require((name, q) not in seen, "duplicate pool coordinate")
        seen.add((name, q))
        final, pool = [list(map(int, row[field].split())) for field in ("final_ids", "pool_ids")]
        require(len(final) == len(set(final)) == 10 and len(pool) == len(set(pool)) == 160,
                "invalid ID cardinality")
        require(all(0 <= i < main_protocol["rows"] for i in pool), "ID outside table")
        require(set(final) <= set(pool), "final IDs outside pool")
        codes = int(row["codes_scored"])
        for native in native_rounds:
            require(native[name, q] == (codes, set(final)), "pool audit disagrees with timed-run native audit")
        pool_hits = len(truth[q] & set(pool))
        hits = len(truth[q] & set(final))
        missing = len(truth[q] - set(pool))
        lost = len((truth[q] & set(pool)) - set(final))
        for field, actual in (("pool_hits", pool_hits), ("final_hits", hits),
                              ("missing_from_pool", missing), ("in_pool_not_returned", lost)):
            require(actual == int(row[field]), "stored diagnostic disagrees with ID sets: " + field)
        total = sums[name]
        total["hits"] += hits
        total["pool_hits"] += pool_hits
        total["missing_from_pool"] += missing
        total["in_pool_not_returned"] += lost
        total["code_sum"] += codes
        total["min_query_hits"] = min(total["min_query_hits"], hits)
        if hits != 10:
            total["imperfect_queries"].append({"query": q, "hits": hits})
    require(seen == {(name, q) for name in names for q in range(200)}, "missing pool rows")
    for total in sums.values():
        total["recall"] = total["hits"] / 2000
        total["mean_codes"] = total["code_sum"] / 200
    result = {"reviewed_epoch": time.time(), "rows_checked": len(seen),
              "scope": "Untimed fixed-parameter diagnostic; no parameter selection or throughput claim",
              "protocol_sha256": sha(run / "candidate-audit-protocol.json"),
              "source_sha256": sha(source), "reviewer_sha256": sha(Path(__file__)),
              "pools_sha256": sha(pool_path), "truth_sha256": sha(truth_path),
              "formal_review_sha256": sha(run / "review.json"), "methods": sums}
    with (run / "candidate-pool-review.json").open("x") as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2)
    print(json.dumps(sums, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", required=True, type=Path)
    review(parser.parse_args().run.resolve())
