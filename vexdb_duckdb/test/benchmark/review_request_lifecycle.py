#!/usr/bin/env python3
"""Independently review fixed arrivals, every outcome, clocks and actual IDs."""
import argparse
import collections
import csv
import hashlib
import json
import math
from pathlib import Path


def review(config_path, truth_path, result_dir=None):
    cfg = dict(x.split("=", 1) for x in config_path.read_text().splitlines() if "=" in x)
    out = result_dir if result_dir is not None else Path(cfg["out"])
    with (out / "requests.tsv").open() as stream:
        rows = list(csv.DictReader(stream, delimiter="\t"))
    offered = math.ceil(float(cfg["rate"]) * int(cfg["seconds"]))
    assert len(rows) == offered
    truth = json.loads(truth_path.read_text())
    cohort = "catalog-new" if cfg["shape"] == "catalog" else "text-new"
    prefix = {"catalog": "catalog-available-price", "broad": "broad-acl", "tenant": "tenant-acl"}[cfg["shape"]]
    outcomes = collections.Counter()
    per_query = collections.defaultdict(lambda: [0, 0])
    completed, terminal = [], []
    hits = 0
    for ordinal, row in enumerate(rows):
        assert int(row["ordinal"]) == ordinal
        query = ordinal % int(cfg["query_count"])
        assert int(row["query"]) == query
        scheduled, begin, execute, executed, ended = [int(row[k]) for k in
            ["scheduled_ns", "bind_begin_ns", "execute_begin_ns", "execute_end_ns", "read_end_ns"]]
        assert scheduled == int(ordinal * 1e9 / float(cfg["rate"]))
        assert 0 <= scheduled < int(cfg["seconds"]) * 10**9
        status = row["status"]
        assert status in {"ok", "ok_late", "expired_before_start", "not_started_drain_limit", "error", "error_drain_limit"}
        assert begin >= scheduled and ended >= begin
        assert 0 <= int(row["client"]) < int(cfg["clients"])
        outcomes[status] += 1
        terminal.append((ended - scheduled) / 1e6)
        if status.startswith("ok"):
            assert begin <= execute <= executed <= ended
            assert int(row["thread_cpu_ns"]) >= 0
            late = ended - scheduled > int(cfg["request_budget_ms"]) * 1000000
            assert (status == "ok_late") == late
            ids = list(map(int, row["ids"].split()))
            assert len(ids) == len(set(ids)) == 10
            wanted = truth[cohort][prefix + ":" + str(query)]["ids"]
            found = len(set(ids) & set(wanted))
            hits += found
            per_query[query][0] += found
            per_query[query][1] += 10
            if cfg["profile"] == "0":
                assert int(row["operators"]) == 0
            elif cfg["shape"] == "broad":
                assert int(row["operators"]) > 0
                assert int(row["operator_ns"]) <= executed - execute
            completed.append(row)
        elif status == "expired_before_start":
            assert begin - scheduled > int(cfg["request_budget_ms"]) * 1000000
            assert execute == executed == 0 and not row["ids"]
    summary = json.loads((out / "summary.json").read_text())
    assert summary["offered"] == offered and summary["completed"] == len(completed)
    assert summary["expired_before_start"] == outcomes["expired_before_start"]
    assert summary["errors_or_not_started"] == sum(v for k, v in outcomes.items() if k not in {"ok", "ok_late", "expired_before_start"})
    percentile = lambda values, p: sorted(values)[math.ceil(len(values) * p) - 1] if values else None
    full = [(int(r["read_end_ns"]) - int(r["scheduled_ns"])) / 1e6 for r in completed]
    service = [(int(r["read_end_ns"]) - int(r["bind_begin_ns"])) / 1e6 for r in completed]
    admission = [(int(r["bind_begin_ns"]) - int(r["scheduled_ns"])) / 1e6 for r in completed]
    window = int(cfg["seconds"]) * 10**9
    events = collections.Counter()
    for row in rows:
        events[int(row["scheduled_ns"])] += 1
        events[int(row["read_end_ns"])] -= 1
    outstanding = peak = 0
    for timestamp in sorted(events):
        outstanding += events[timestamp]
        assert outstanding >= 0
        peak = max(peak, outstanding)
    assert outstanding == 0
    return dict(scope="fixed-arrival diagnostic on exposed queries, not a speedup or production SLA claim",
                offered=offered, outcomes=dict(outcomes), completed_recall=hits/(len(completed)*10) if completed else None,
                minimum_completed_query_recall=min((h/n for h,n in per_query.values()), default=None),
                completed_quality_passed=bool(completed) and hits/(len(completed)*10) >= .999,
                completed_query_coverage=len(per_query), planned_query_pool=int(cfg["query_count"]),
                all_offered_completed_within_budget=outcomes["ok"] == offered,
                speedup_claim_allowed=False,
                offered_neighbor_delivery_ratio=hits/(offered*10),
                terminal_request_p95_ms=percentile(terminal,.95), completed_request_p95_ms=percentile(full,.95),
                completed_service_p95_ms=percentile(service,.95), completed_admission_p95_ms=percentile(admission,.95),
                deadline_miss_fraction=(offered-outcomes["ok"])/offered,
                offered_qps=offered/int(cfg["seconds"]),
                started_during_offering_qps=sum(0 < int(r["execute_begin_ns"]) < window for r in rows)/int(cfg["seconds"]),
                completed_during_offering_qps=sum(int(r["read_end_ns"]) <= window for r in completed)/int(cfg["seconds"]),
                completed_qps_including_drain=len(completed)*1e9/max(window,max(int(r["read_end_ns"]) for r in rows)),
                peak_outstanding_including_not_submitted=peak,
                completed_after_offering_window=sum(int(r["read_end_ns"]) > int(cfg["seconds"])*10**9 for r in completed),
                source_hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [Path(__file__),config_path,truth_path,out/"requests.tsv",out/"summary.json"]})


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--truth", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--result-dir", type=Path, help="local copy of results; keep original config unchanged")
    args = parser.parse_args()
    if args.out.exists():
        raise SystemExit("refusing to overwrite review")
    result = review(args.config, args.truth, args.result_dir)
    args.out.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))
