"""Recompute business SQL quality, timing, uniform coverage and paired controls."""
import argparse
from collections import Counter
import csv
from fractions import Fraction
import json
import math
from pathlib import Path

from run_filtered_hybrid_benchmark import sha


def percentile(values, fraction):
    values.sort()
    return values[max(0, math.ceil(len(values) * fraction) - 1)]


def allowed(profile, row, q):
    if profile == "broad-acl": return row % 2 == 0
    if profile == "tenant-acl": return row % 100 == q % 100 and row % 7 != 0
    if profile == "catalog-available-price": return row % 5 != 0 and 1000 + (q % 5) * 100 <= 100 + row % 10000 <= 9000
    raise ValueError(profile)


def allowed_stored(profile, metadata, q):
    # Actual stored columns from a frozen pre-timing read; no inference from ID.
    if profile == "broad-acl": return metadata["is_active"] == "1"
    if profile == "tenant-acl": return metadata["tenant_id"] == str(q % 100) and metadata["is_public"] == "1"
    if profile == "catalog-available-price":
        price = int(metadata["price_cents"]) if metadata["price_cents"] else None
        return metadata["in_stock"] == "1" and price is not None and 1000 + (q % 5) * 100 <= price <= 9000
    raise ValueError(profile)


def review(run, out):
    if out.exists(): raise RuntimeError("refusing to overwrite review")
    protocol = json.loads((run / "protocol.json").read_text())
    state = json.loads((run / "state.json").read_text())
    if state["status"] != "complete": raise RuntimeError("native run is incomplete; preserve partial evidence")
    assert state["inputs_unchanged"] and not state["active_pid"]
    assert len(state["jobs"]) == len(protocol["jobs"])
    root = Path(__file__).resolve().parents[3]
    for path, digest in protocol["sha256"].items():
        if sha(path) != digest:
            frozen = run / "frozen-source" / Path(path).relative_to(root)
            assert frozen.is_file() and sha(frozen) == digest, "changed input/frozen source: " + path
    truth = json.loads((run.parent / "performance-preparation/truth.json").read_text())
    metadata = {}
    for work in protocol["workloads"]:
        if "metadata" in work and work["source"] not in metadata:
            metadata[work["source"]] = {int(r["id"]): r for r in csv.DictReader(Path(work["metadata"]).open())}
    points, lookup, all_ids, total_calls, warmups = [], {}, {}, 0, []
    warmup_specs = protocol.get("warmup_points", {})
    for job in state["jobs"]:
        assert job["status"] == "complete" and job["exit_code"] == 0 and job["finished_epoch"] <= protocol["deadline_epoch"]
        for path, digest in json.loads((run / (job["name"] + "-hashes.json")).read_text()).items():
            assert sha(run / path) == digest, "changed result: " + path
        work = next(w for w in protocol["workloads"] if w["name"] == job["workload"])
        vector_rows = Path(work["base"]).stat().st_size // (4 * work["dim"])
        directory = run / job["name"]
        summaries = list(csv.DictReader((directory / "points.csv").open()))
        assert {p["point"] for p in summaries} == {"c" + str(c) for c in protocol.get("clients", (1, 16, 64))} | set(warmup_specs)
        for summary in summaries:
            is_warmup = summary["point"] in warmup_specs
            clients = warmup_specs[summary["point"]]["clients"] if is_warmup else int(summary["point"][1:])
            rows = list(csv.DictReader((directory / (summary["point"] + ".timing.csv")).open()))
            wall = int(summary["wall_ns"])
            assert len(rows) == int(summary["calls"])
            assert wall >= (warmup_specs[summary["point"]]["min_seconds"] if is_warmup else 5) * 10**9
            coverage, latencies, hits, denominator, worker_times = Counter(), [], 0, 0, {}
            for r in rows:
                client, ordinal, q = int(r["client"]), int(r["ordinal"]), int(r["query"])
                begin, end = int(r["begin_ns"]), int(r["end_ns"])
                assert 0 <= client < clients and 0 <= q < 8 and 0 <= begin <= end <= wall
                assert 0 <= int(r["thread_cpu_ns"]) <= end - begin
                if client not in worker_times: worker_times[client] = [begin, end, 0]
                assert ordinal == worker_times[client][2]
                worker_times[client][1], worker_times[client][2] = end, ordinal + 1
                ids = list(map(int, r["ids"].split()))
                stored = metadata.get(work["source"])
                assert len(set(ids)) == len(ids) == 10 and all(0 <= i < vector_rows and
                    (allowed_stored(work["profile"], stored[i], q) if stored else allowed(work["profile"], i, q)) for i in ids)
                expected_bytes = sum(int(stored[i]["body_bytes"]) if stored else 32 * len(f"proxy-payload-{i} ") for i in ids)
                assert int(r["payload_bytes"]) == expected_bytes
                expected = truth[work["source"]][work["profile"] + ":" + str(q)]["ids"]
                hits += len(set(ids) & set(expected)); denominator += len(expected)
                key = work["name"], job["version"], q
                if key in all_ids: assert all_ids[key] == set(ids), "concurrency/round changed IDs"
                else: all_ids[key] = set(ids)
                if job["version"] in ("legacy", "legacy-aa", "exact"): assert set(ids) == set(expected)
                coverage[client, q] += 1; latencies.append((end - begin) / 1e6)
            assert set(worker_times) == set(range(clients))
            for client in range(clients):
                counts = [coverage[client, q] for q in range(8)]
                assert min(counts) > 0 and len(set(counts)) == 1, "incomplete/nonuniform query cycle"
            occupied = sum(v[1] - v[0] for v in worker_times.values()) / wall
            overlap = min(v[1] for v in worker_times.values()) - max(v[0] for v in worker_times.values())
            assert overlap > 0
            qps = len(rows) * 1e9 / wall
            assert math.isclose(qps, float(summary["qps"]), rel_tol=1e-10)
            recall = Fraction(hits, denominator)
            point = {"workload": work["name"], "source": work["source"], "profile": work["profile"],
                     "version": job["version"], "round": job["round"], "clients": clients,
                     "calls": len(rows), "wall_seconds": wall / 1e9, "qps": qps,
                     "p50_ms": percentile(latencies.copy(), .5), "p95_ms": percentile(latencies.copy(), .95),
                     "p99_ms": percentile(latencies.copy(), .99), "hits": hits, "expected": denominator,
                     "recall": float(recall), "quality_passed": recall >= Fraction(999, 1000),
                     "mean_active_clients": occupied, "occupancy_passed": occupied >= clients * .8,
                     "process_cpu_cores": float(summary["process_cpu_ms"]) * 1e6 / wall,
                     "rss_after": int(summary["rss_after"])}
            for metric in ("p50_ms", "p95_ms", "p99_ms", "mean_active_clients"):
                assert math.isclose(point[metric], float(summary[metric]), rel_tol=1e-9), metric
            if is_warmup:
                warmups.append(point)
            else:
                points.append(point); lookup[work["name"], clients, job["version"], job["round"]] = point
                total_calls += len(rows)
    controls = []
    for work in protocol["workloads"]:
        for clients in protocol.get("clients", (1, 16, 64)):
            def get(version, round_id): return lookup[work["name"], clients, version, round_id]
            aa = [get("legacy-aa", r)["qps"] / get("legacy", r)["qps"] for r in (1, 2)]
            default = [get("exact", r)["qps"] / get("legacy", r)["qps"] for r in (1, 2)]
            for version in ("pq", "raw"):
                ratios = [get(version, r)["qps"] / get("exact", r)["qps"] for r in (1, 2)]
                tails = [get(version, r)["p95_ms"] / get("exact", r)["p95_ms"] for r in (1, 2)]
                quality = all(get(version, r)["quality_passed"] for r in (1, 2))
                occupancy = all(get(v, r)["occupancy_passed"] for r in (1, 2) for v in ("legacy", "legacy-aa", "exact", version))
                stable = all(.97 <= x <= 1.03 for x in aa)
                default_ok = math.prod(default) ** .5 >= .97
                controls.append({"workload": work["name"], "clients": clients, "version": version,
                                 "aa_r1": aa[0], "aa_r2": aa[1], "aa_passed": stable,
                                 "default_r1": default[0], "default_r2": default[1], "default_geomean": math.prod(default) ** .5,
                                 "default_passed": default_ok, "qps_r1": ratios[0], "qps_r2": ratios[1],
                                 "qps_geomean": math.prod(ratios) ** .5, "p95_r1": tails[0], "p95_r2": tails[1],
                                 "quality_passed": quality, "occupancy_passed": occupancy,
                                 "benefit_gate_passed": stable and default_ok and quality and occupancy and all(x >= 1.1 for x in ratios) and all(x <= 1 for x in tails)})
    out.mkdir(parents=True)
    for name, data in (("points.csv", points), ("controls.csv", controls)):
        with (out / name).open("w") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(data[0])); writer.writeheader(); writer.writerows(data)
    if warmups:
        with (out / "warmups.csv").open("w") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(warmups[0])); writer.writeheader(); writer.writerows(warmups)
    result = {"native_jobs": len(state["jobs"]), "points": len(points), "calls": total_calls,
              "hashes_coverage_and_clock_passed": True, "all_quality_passed": all(p["quality_passed"] for p in points + warmups),
              "all_benefit_gates_passed": all(c["benefit_gate_passed"] for c in controls),
              "scope": "Local prepared business SQL with proxy bodies/metadata and eight registered query vectors, saturated closed-loop load. Combine with separately registered quality/controls evidence; not production capacity."}
    if warmups:
        result.update(warmup_points=len(warmups), warmup_calls=sum(p["calls"] for p in warmups))
    (out / "review.json").write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", required=True, type=Path); parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args(); review(args.run.resolve(), args.out.resolve())
