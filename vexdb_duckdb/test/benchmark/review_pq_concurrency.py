"""Check admission, waiting, execution caps and ID equivalence in queue diagnostics."""
import argparse
import csv
import hashlib
import json
import math
import time
from collections import Counter, defaultdict
from fractions import Fraction
from pathlib import Path

import numpy as np


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def percentile(values, fraction):
    return sorted(values)[max(0, math.ceil(len(values) * fraction) - 1)]


def review(run):
    out = run / "concurrency-review"
    if out.exists():
        raise RuntimeError("review already exists")
    protocol = json.loads((run / "latency-protocol.json").read_text())
    base_review = json.loads((run / "latency-review/review.json").read_text())
    if not base_review["all_inputs_outputs_verified"]:
        raise RuntimeError("base clock/ID/hash review missing")
    for path, digest in protocol["inputs"].items():
        if sha(Path(path)) != digest:
            raise RuntimeError("changed frozen input")
    truth = np.load(protocol["oracle_path"])["none_top_ids"]
    references, summaries, hashes = {}, [], {}
    for job in protocol["clean"]:
        folder = run / job["name"]
        native = {row["point"]: row for row in csv.DictReader((folder / "points.csv").open())}
        for line in Path(job["points"]).read_text().splitlines():
            fields = line.split()
            name, mode, clients, requests, limit = fields[0], fields[1], int(fields[6]), int(fields[8]), int(fields[9])
            result = native[name]
            gate = "execution_limit" in result
            threads = int(result["executors"])
            if threads != (clients if gate else limit) or (gate and int(result["execution_limit"]) != limit):
                raise RuntimeError("worker/execution limit mismatch")
            path = folder / (name + ".timing.csv")
            hashes[str(path.relative_to(run))] = sha(path)
            rows = list(csv.DictReader(path.open()))
            if len(rows) != clients * requests:
                raise RuntimeError("dropped/rejected/extra requests")
            coverage, query_counts = set(), Counter()
            executions, client_rows, events = defaultdict(list), defaultdict(list), []
            waits, elapsed, cpus, hits = [], [], [], 0
            for row in rows:
                client, ordinal, q, executor = (int(row[k]) for k in ("worker", "ordinal", "query", "executor"))
                submitted, began, ended, cpu = (int(row[k]) for k in ("begin_ns", "execute_start_ns", "end_ns", "thread_cpu_ns"))
                if not (0 <= client < clients and 0 <= ordinal < requests and 0 <= executor < threads and
                        0 <= q < len(truth) and 0 <= submitted <= began < ended <= int(result["wall_ns"]) and cpu > 0):
                    raise RuntimeError("invalid admission/execution timestamp")
                if (client, ordinal) in coverage:
                    raise RuntimeError("duplicate admitted request")
                coverage.add((client, ordinal))
                query_counts[q] += 1
                ids = tuple(sorted(map(int, row["ids"].split())))
                if len(ids) != 10 or len(set(ids)) != 10:
                    raise RuntimeError("invalid returned list")
                key = (mode, fields[2], fields[3], q)
                if references.setdefault(key, ids) != ids:
                    raise RuntimeError("returned ID set changed with concurrency control")
                hits += len(set(ids) & set(map(int, truth[q])))
                executions[executor].append((began, ended))
                client_rows[client].append((ordinal, submitted, ended))
                events += [(began, 1), (ended, -1)]
                waits.append((began - submitted) / 1e6)
                elapsed.append((ended - submitted) / 1e6)
                cpus.append(cpu / 1e6)
            if coverage != {(c, i) for c in range(clients) for i in range(requests)} or query_counts != Counter({q: clients for q in range(512)}):
                raise RuntimeError("incomplete/unbalanced submitted load")
            for intervals in executions.values():
                ordered = sorted(intervals)
                if any(a[1] > b[0] for a, b in zip(ordered, ordered[1:])):
                    raise RuntimeError("one executor ran overlapping SQL")
            for values in client_rows.values():
                ordered = sorted(values)
                if any(a[2] > b[1] for a, b in zip(ordered, ordered[1:])):
                    raise RuntimeError("logical client submitted overlapping requests")
            active, peak = 0, 0
            for _, change in sorted(events):
                active += change
                peak = max(peak, active)
            if active or peak > limit:
                raise RuntimeError("execution cap violated")
            p95 = percentile(elapsed, .95)
            if abs(p95 - float(result["p95_ms"])) > 1e-6:
                raise RuntimeError("P95 omits/miscounts waiting")
            summaries.append({"job": job["name"], "point": name, "mode": mode, "kind": "gate" if gate else "pool",
                              "logical_clients": clients, "worker_threads": threads, "retained_connections": clients if gate else threads,
                              "execution_limit": limit, "peak_native_executions": peak, "calls": len(rows),
                              "qps": float(result["qps"]), "p95_including_wait_ms": p95, "p95_wait_ms": percentile(waits, .95),
                              "mean_thread_cpu_ms": sum(cpus) / len(rows), "strict_recall": hits / (10 * len(rows)),
                              "passed_recall": Fraction(hits, 10 * len(rows)) >= Fraction(999, 1000)})
    out.mkdir()
    with (out / "points.csv").open("x") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(summaries[0]))
        writer.writeheader()
        writer.writerows(summaries)
    result = {"epoch": time.time(), "all_admitted_requests_verified": True, "execution_caps_verified": True,
              "waiting_included_in_p95": True, "returned_ids_identical_across_limits_and_rounds": True,
              "passed_recall_every_point": all(row["passed_recall"] for row in summaries),
              "calls": sum(row["calls"] for row in summaries), "rows": summaries, "native_hashes": hashes,
              "reviewer_sha256": sha(Path(__file__)),
              "scope": "Pool changes connection reuse too; gate preserves64 sessions/threads. Both are diagnostics, no production admission behavior shipped. Closed-loop local load, all512 queries/64 clients balanced, wait+execution included."}
    (out / "review.json").write_text(json.dumps(result, indent=2))
    print("PASS admitted requests, real execution caps, queue-inclusive P95 and ID equivalence:", result["calls"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", required=True, type=Path)
    review(parser.parse_args().run.resolve())
