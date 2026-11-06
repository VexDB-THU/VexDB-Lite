"""Independent integer-hit review of scalar/PQ native SQL validation."""
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


def eligible(case, row, q, n):
    if case in ("plain_ids", "plain_payload", "joined_unfiltered_payload"):
        return True
    if case in ("rag_tenant_public", "rag_pagination", "joined_tenant_payload"):
        return row % 100 == q % 100 and row % 7 != 0
    if case == "rag_acl_recent":
        return row % 100 == q % 100 and row % 7 != 0 and row % 365 >= 300
    if case == "rag_broad_public":
        return row % 7 != 0
    if case == "rag_empty_tenant":
        return False
    if case == "catalog_category_stock_price":
        return row * 10 // n == (q + 9) % 10 and row % 5 != 0 and 1000 <= 100 + row % 10000 <= 9000
    if case == "catalog_sparse":
        return row >= n - 3
    raise ValueError("unregistered case " + case)


def review(phase, out):
    if out.exists():
        raise RuntimeError("refusing to overwrite review")
    protocol = json.loads((phase / "protocol.json").read_text())
    state = json.loads((phase / "state.json").read_text())
    assert state["status"] == "complete" and state["hashes_unchanged"]
    assert all(j["status"] == "complete" and j["exit_code"] == 0 and j["finished_epoch"] <= protocol["deadline_epoch"] for j in state["jobs"])
    final = (phase / "generated-inputs.json").exists()
    cohorts = json.loads((phase / "generated-inputs.json").read_text()) if final else protocol["cohorts"]
    count = protocol["count"] if final else protocol["queries_per_cohort"]
    cases = protocol.get("cases", ["plain_ids", "plain_payload", "rag_tenant_public", "rag_acl_recent",
        "rag_broad_public", "rag_empty_tenant", "catalog_category_stock_price", "catalog_sparse",
        "joined_tenant_payload", "joined_unfiltered_payload", "rag_pagination"])
    for cohort in cohorts:
        if "queries_sha256" in cohort:
            assert sha(cohort["queries"]) == cohort["queries_sha256"], "generated queries changed"
    for path, digest in protocol.get("source_sha256", {}).items():
        assert sha(phase / "source" / path) == digest, "frozen source changed: " + path
    for path, digest in protocol.get("input_sha256", protocol.get("inputs", {})).items():
        if sha(path) != digest:
            # A completed version's source may later advance. Verify its saved
            # source bytes, never accept a changed database/binary as equivalent.
            root = Path(__file__).resolve().parents[3]
            original = Path(path)
            candidates = []
            if original.is_relative_to(root):
                candidates.append(phase / "source" / original.relative_to(root))
            if original.is_relative_to(root / "build"):
                candidates.append(phase / "source" / original.relative_to(root / "build"))
            assert any(p.is_file() and sha(p) == digest for p in candidates), "input/frozen source changed: " + path
    if (phase / "result-hashes.json").exists():
        for path, digest in json.loads((phase / "result-hashes.json").read_text()).items():
            assert sha(phase / path) == digest, "result changed: " + path
    points, calls = [], 0
    for cohort in cohorts:
        n = Path(cohort["base"]).stat().st_size // (4 * cohort["dim"])
        rows = list(csv.DictReader((phase / cohort["name"] / "requests.csv").open()))
        assert len(rows) == len(cases) * 2 * 2 * count
        groups, pairs, seen = defaultdict(list), defaultdict(dict), set()
        for row in rows:
            for key in ("limit", "query", "index_plan", "eligible", "expected_k", "returned", "hits", "illegal", "payload_ok", "admitted_delta"):
                row[key] = int(row[key])
            key = row["case"], row["mode"], row["limit"], row["query"]
            assert key not in seen and 0 <= row["query"] < count
            seen.add(key)
            ids, truth = list(map(int, row["ids"].split())), list(map(int, row["truth"].split()))
            assert len(set(ids)) == len(ids) == row["returned"] == row["expected_k"] == len(set(truth)) == len(truth)
            assert all(0 <= i < n and eligible(row["case"], i, row["query"], n) for i in ids + truth)
            assert len(set(ids) & set(truth)) == row["hits"]
            assert row["illegal"] == 0 and row["payload_ok"] == 1
            assert row["admitted_delta"] == int(bool(row["index_plan"] and row["eligible"]))
            if not row["index_plan"]:
                assert set(ids) == set(truth), "exact SQL differs from independent truth"
            pairs[row["case"], row["mode"], row["query"]][row["limit"]] = set(ids)
            groups[row["case"], row["mode"], row["limit"]].append(row)
        for pair in pairs.values():
            assert set(pair) == {0, 1} and pair[0] == pair[1], "queue setting changed results"
        assert set(groups) == {(case, mode, cap) for case in cases for mode in ("off", "pq_graph") for cap in (0, 1)}
        for (case, mode, limit), group in sorted(groups.items()):
            assert len(group) == count
            hits, denominator = sum(r["hits"] for r in group), sum(r["expected_k"] for r in group)
            recall = Fraction(hits, denominator) if denominator else None
            points.append({"cohort": cohort["name"], "case": case, "mode": mode, "limit": limit,
                           "requests": len(group), "hits": hits, "expected_k_sum": denominator,
                           "recall": float(recall) if recall is not None else None,
                           "gate_passed": recall is None or recall >= Fraction(999, 1000),
                           "index_requests": sum(r["index_plan"] for r in group)})
        calls += len(rows)
    result = {"phase": phase.name, "requests": calls, "points": len(points), "queries_per_cohort": count,
              "hashes_and_deadline_passed": True, "permissions_payload_counts_passed": True,
              "queue_limit_equivalence_passed": True, "quality_gate_passed": all(p["gate_passed"] for p in points),
              "failed_points": [p for p in points if not p["gate_passed"]],
              "scope": "Full-vector float64 truth outside SQL; hits independently recomputed from returned IDs. Final proxies are synthetic blends, not customer questions. No performance claim."}
    out.mkdir(parents=True)
    with (out / "points.csv").open("w") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(points[0]))
        writer.writeheader()
        writer.writerows(points)
    (out / "review.json").write_text(json.dumps(result, indent=2))
    print(json.dumps({k: v for k, v in result.items() if k != "failed_points"}, indent=2))
    print("failed_points", len(result["failed_points"]))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--phase", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    review(args.phase.resolve(), args.out.resolve())
