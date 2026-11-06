#!/usr/bin/env python3
"""Bounded, sequential SQL benchmark; preserve inputs, failures and raw returned IDs."""
import argparse
import csv
import hashlib
import json
import math
import platform
from pathlib import Path
import shutil
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[3]


def sha(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(8 << 20), b""):
            h.update(block)
    return h.hexdigest()


def write(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def read_metadata(path):
    with path.open() as stream:
        return dict(csv.reader(stream))


def archive_sources(run, hashes):
    entries = {}
    for source, expected in hashes.items():
        path = Path(source)
        if path.suffix not in (".py", ".cpp", ".hpp", ".h"):
            continue
        target = run / "frozen-source" / path.relative_to(ROOT)
        target.parent.mkdir(parents=True, exist_ok=True)
        assert sha(path) == expected
        shutil.copy2(path, target)
        entries[source] = {"path": str(target.relative_to(run)), "sha256": expected}
    write(run / "frozen-source-manifest.json", {"recorded_epoch": time.time(), "sources": entries})


def verify_inputs(run, protocol):
    # Code may legitimately evolve after a completed baseline. Audit the
    # archived version against its original registered hash; never require a
    # checkout reset just to review historical measurements. Data/binaries
    # without a snapshot must still match at their original paths.
    archive = run / "frozen-source-manifest.json"
    entries = json.loads(archive.read_text())["sources"] if archive.exists() else {}
    archived = []
    for name, expected in protocol["frozen_sha256"].items():
        if name in entries:
            record = entries[name]
            assert record["sha256"] == expected
            assert sha(run / record["path"]) == expected, name
            archived.append(name)
        else:
            assert sha(Path(name)) == expected, name
    return archived


def prepare(run):
    import numpy as np
    for dataset in ("sift", "dbpedia"):
        source = ROOT / "build/research/quantization" / ("graph-data-" + dataset)
        out = run / "data" / dataset
        manifest = json.loads((source / "manifest.json").read_text())
        source_hashes = {}
        for name in ("base.npy", "queries.npy", "exact_scores.npy"):
            source_hashes[name] = sha(source / name)
            assert source_hashes[name] == manifest["files"][name]["sha256"], name
        if out.exists():
            existing = json.loads((out / "manifest.json").read_text())
            assert existing["source_manifest_sha256"] == sha(source / "manifest.json")
            for name, record in existing["files"].items():
                assert sha(out / name) == record["sha256"], name
            continue
        out.mkdir(parents=True)
        base = np.load(source / "base.npy", mmap_mode="r")
        queries = np.load(source / "queries.npy", mmap_mode="r")
        scores = np.load(source / "exact_scores.npy", mmap_mode="r")
        np.asarray(base, dtype="<f4").tofile(out / "base.f32")
        np.asarray(queries, dtype="<f4").tofile(out / "queries.f32")
        for count in (512, 10000, 100000):
            ids = np.arange(count)
            truth = np.array([np.lexsort((ids, scores[q, :count]))[:10] for q in range(len(queries))], dtype="<i4")
            truth.tofile(out / f"truth-{count}.i32")
        write(out / "manifest.json", {
            "dataset": dataset, "dim": base.shape[1], "rows": len(base), "queries": len(queries),
            "metric": "L2 on original prepared float32; text already normalized",
            "query_status": "same 200 already-used development queries; fresh holdout untouched",
            "truth": "audited cached exhaustive float64 scores for oracle only; subset prefix, distance then row ID ties",
            "source": str(source), "source_manifest_sha256": sha(source / "manifest.json"),
            "files": {p.name: {"bytes": p.stat().st_size, "sha256": sha(p)} for p in out.iterdir()},
        })


def run_queue(args):
    run = args.run.resolve()
    protocol_path = run / "protocol.json"
    state_path = run / "state.json"
    source_files = [Path(__file__).resolve(), ROOT / "vexdb_duckdb/test/benchmark/vex_quantization_product_benchmark.cpp"]
    # Include every compiled product source and the synchronization headers.
    verified = json.loads((ROOT / "build/product-tests/pq-integration-20261027/verification.json").read_text())
    source_files += [ROOT / r["source"] for r in verified["compiled_sources"]]
    source_files += [ROOT / p for p in verified["sources"] if "/include/" in p]
    source_files += sorted((ROOT / "vexdb_duckdb/include").rglob("*.hpp"))
    hashes = {str(p): sha(p) for p in source_files}
    hashes[str(args.binary.resolve())] = sha(args.binary)
    hashes[str(args.extension.resolve())] = sha(args.extension)
    library = ROOT / "build/product-tests/pq-integration-20261027/duckdb-sdk/libduckdb.dylib"
    hashes[str(library)] = sha(library)
    for dataset in ("sift", "dbpedia"):
        manifest_path = run / "data" / dataset / "manifest.json"
        manifest = json.loads(manifest_path.read_text())
        hashes[str(manifest_path)] = sha(manifest_path)
        for name, record in manifest["files"].items():
            p = manifest_path.parent / name
            assert sha(p) == record["sha256"], str(p)
            hashes[str(p)] = record["sha256"]
    if protocol_path.exists():
        protocol = json.loads(protocol_path.read_text())
        assert protocol["frozen_sha256"] == hashes, "sources, binary or data changed; use a new run"
        state = json.loads(state_path.read_text())
        assert state.get("pid") == 0 or not process_alive(state["pid"]), "coordinator is already running"
        for job in state["jobs"]:
            if job["status"] == "running":
                assert not process_alive(job["worker_pid"]), "a prior worker is still running"
                job.update(status="interrupted_preserved", reason="coordinator stopped; partial output retained")
    else:
        jobs = [{"id": f"{dataset}-{count}-{kind}", "dataset": dataset, "rows": count,
                 "dim": 128 if dataset == "sift" else 1536, "kind": kind, "status": "pending"}
                for count in (10000, 100000) for dataset in ("sift", "dbpedia") for kind in ("raw", "pq", "compact")]
        protocol = {
            "created_epoch": time.time(), "deadline_epoch": time.time() + args.minutes * 60,
            "platform": platform.platform(), "machine": platform.machine(), "duckdb_version": "v1.5.2",
            "frozen_sha256": hashes, "queries": 200, "repeats": 3, "connections": [1, 8],
            "raw_ef": [40, 128, 512], "pq_refine_factors": [1, 4, 16], "pq_m": "product default (4 dims/subcode when divisible)",
            "build": "m=16 ef_construction=100 build_threads=1; one build per fresh process",
            "pq_training": "product default parallel subquantizers (outer tasks >64 run serial); graph build thread setting does not limit PQ threads",
            "metric": "L2; text inputs already normalized", "sql": "prepared parameterized SELECT id ORDER BY l2_distance LIMIT 10",
            "timing": "steady_clock execute, materialization and ID extraction; Value arrays and Prepare outside per-call clock; QPS includes loop/output collection",
            "oracle": "cached exhaustive scores used only for ground truth, never native timing",
            "memory": "live process RSS includes table/allocator/host; file sizes are whole DB and observed index-build delta; allocator_bytes is partial",
            "quality_gate": "95% Recall@10 on these development queries; no new-query or production claim",
            "case_timeout_seconds": args.timeout, "disk_floor_bytes": 4 << 30,
        }
        write(protocol_path, protocol)
        archive_sources(run, hashes)
        state = {"status": "running", "jobs": jobs}
    import os
    state["pid"] = os.getpid()
    write(state_path, state)
    try:
        for job in state["jobs"]:
            if job["status"] != "pending":
                continue
            remaining = protocol["deadline_epoch"] - time.time()
            if remaining < 30 or shutil.disk_usage(run).free < protocol["disk_floor_bytes"]:
                state["status"] = "bounded_stop"
                break
            output = run / "cases" / job["id"]
            output.parent.mkdir(exist_ok=True)
            if output.exists():
                job.update(status="interrupted_preserved", reason="prior output exists; not overwritten")
                write(state_path, state)
                continue
            command = [str(args.binary.resolve()), str(args.extension.resolve()), str(run / "data" / job["dataset"]),
                       str(job["rows"]), str(job["dim"]), "200", job["kind"], str(output)]
            job.update(status="running", started_epoch=time.time(), command=command)
            write(state_path, state)
            print("START", job["id"], flush=True)
            with (run / (job["id"] + ".log")).open("x") as log:
                worker = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
                job["worker_pid"] = worker.pid
                write(state_path, state)
                try:
                    code = worker.wait(timeout=min(protocol["case_timeout_seconds"], remaining))
                    job["status"] = "complete" if code == 0 else "failed"
                    job["exit_code"] = code
                except subprocess.TimeoutExpired:
                    worker.terminate()
                    try:
                        worker.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        worker.kill()
                        worker.wait()
                    job.update(status="timeout", exit_code=worker.returncode)
            job["seconds"] = time.time() - job["started_epoch"]
            job["result_sha256"] = {str(p.relative_to(run)): sha(p) for p in output.glob("*") if p.is_file() and p.suffix != ".duckdb"}
            write(state_path, state)
            print(job["status"].upper(), job["id"], round(job["seconds"], 2), flush=True)
        else:
            state["status"] = "queue_finished"
    finally:
        state["pid"] = 0
        write(state_path, state)


def process_alive(pid):
    import os
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


def summarize(run):
    protocol = json.loads((run / "protocol.json").read_text())
    archived = verify_inputs(run, protocol)
    state = json.loads((run / "state.json").read_text())
    records = []
    for job in state["jobs"]:
        if job["status"] != "complete":
            continue
        for name, expected in job["result_sha256"].items():
            assert sha(run / name) == expected, name
        output = run / "cases" / job["id"]
        meta = read_metadata(output / "metadata.csv")
        data = (run / "data" / job["dataset"] / f"truth-{job['rows']}.i32").read_bytes()
        truth = struct.unpack("<" + "i" * (len(data) // 4), data)
        with (output / "points.csv").open() as stream:
            points = list(csv.DictReader(stream))
        assert len(points) == (2 if job["kind"] == "compact" else 6), "missing measurement point"
        for point in points:
            connections = int(point["connections"])
            with (output / f"{point['point']}-c{connections}.csv").open() as stream:
                samples = list(csv.DictReader(stream))
            seen = set()
            latencies, hits = [], 0
            for sample in samples:
                query, repeat = int(sample["query"]), int(sample["repeat"])
                assert 0 <= query < 200 and 0 <= repeat < 3
                assert (repeat, query) not in seen
                seen.add((repeat, query))
                ids = list(map(int, sample["ids"].split()))
                assert len(ids) == len(set(ids)) == 10 and min(ids) >= 0 and max(ids) < job["rows"]
                found = sum(i in truth[query * 10:query * 10 + 10] for i in ids)
                assert found == int(sample["hits"])
                hits += found
                latencies.append(float(sample["ms"]))
            assert len(seen) == 600
            recall = hits / 6000
            latencies.sort()
            values = {"recall10": recall, "qps": len(samples) * 1000 / float(point["wall_ms"]),
                      "p50_ms": latencies[math.ceil(.5 * len(latencies)) - 1], "p95_ms": latencies[math.ceil(.95 * len(latencies)) - 1]}
            for name, value in values.items():
                assert math.isclose(value, float(point[name]), rel_tol=1e-8, abs_tol=1e-10), name
            records.append({"dataset": job["dataset"], "rows": job["rows"], "kind": job["kind"], **point,
                            "table_file_bytes": meta["table_file_bytes"], "indexed_file_bytes": meta["indexed_file_bytes"],
                            "build_ms": meta["build_ms"], "peak_rss_bytes": meta["peak_rss_bytes"],
                            "pq_code_bytes": meta["pq_code_bytes"], "pq_codebook_bytes": meta["pq_codebook_bytes"],
                            "pq_m": meta["pq_m"], "passes_95": recall >= .95})
    if records:
        with (run / "summary.csv").open("w") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(records[0]))
            writer.writeheader()
            writer.writerows(records)
    write(run / "review.json", {"status": "verified", "complete_cases": sum(j["status"] == "complete" for j in state["jobs"]),
          "points": len(records), "returned_top10_lists": len(records) * 600, "verified_archived_sources": archived,
          "noncomplete": [{"id": j["id"], "status": j["status"]} for j in state["jobs"] if j["status"] != "complete"],
          "scope": "one native build per case; 200 already-used development queries; no holdout or independent graph seed acceptance"})
    print(json.dumps({"complete_cases": sum(j["status"] == "complete" for j in state["jobs"]), "points": len(records)}, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["prepare", "run", "summarize"])
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--extension", type=Path)
    parser.add_argument("--minutes", type=int, default=45)
    parser.add_argument("--timeout", type=int, default=900)
    args = parser.parse_args()
    args.run.mkdir(parents=True, exist_ok=True)
    if args.action == "prepare":
        prepare(args.run)
    elif args.action == "run":
        assert args.binary and args.extension
        run_queue(args)
    else:
        summarize(args.run)


if __name__ == "__main__":
    main()
