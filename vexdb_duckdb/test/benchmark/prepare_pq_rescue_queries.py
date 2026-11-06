"""Prepare a separately sourced 512-query check only after method freezing."""
import argparse
import hashlib
import json
import time
from pathlib import Path

from run_pq_candidate_stage import ROOT, save, sha, verify


def prepare(run):
    selection = json.loads((run / "selection.json").read_text())
    if not selection["passed"] or selection["new_query_quality_read"]:
        raise RuntimeError("no eligible frozen candidate")
    verify(run, "dev")
    if sha(run / "dev/review.json") != selection["dev_review_sha256"]:
        raise RuntimeError("development evidence changed")
    if time.time() >= json.loads((run / "protocol.json").read_text())["deadline_epoch"]:
        raise RuntimeError("original deadline expired")
    import numpy as np
    import pyarrow.parquet as pq
    source = run / "source-shard-00003.parquet"
    source_hash = sha(source)
    if source_hash != "5f1cbe4a09d30a14751c01be7c194daa1110b1d0bd56e448d3a4bec54c03989c":
        raise RuntimeError("public shard differs from pinned official content hash")
    out = run / "new-queries"
    out.mkdir(exist_ok=False)
    table = pq.read_table(source, columns=["text-embedding-3-large-1536-embedding"])
    values = table.column(0).combine_chunks()
    if not np.all(np.asarray(values.value_lengths()) == 1536):
        raise RuntimeError("source embedding dimensions differ")
    matrix = np.asarray(values.values, dtype=np.float32).reshape(len(values), 1536).copy()
    norms = np.linalg.norm(matrix, axis=1, keepdims=True)
    if not np.isfinite(matrix).all() or np.any(norms <= 0):
        raise RuntimeError("invalid source vectors")
    matrix /= norms
    base_path = ROOT / "build/product-tests/quantization-product-20261027/data/dbpedia/base.f32"
    base = np.memmap(base_path, dtype="<f4", mode="r", shape=(100000, 1536))
    previous = np.load(ROOT / "build/research/quantization/dbpedia100k/queries.npy")
    row_sha = lambda row: hashlib.sha256(np.asarray(row, dtype="<f4").tobytes()).digest()
    excluded = {row_sha(row) for row in base}
    excluded.update(row_sha(row) for row in previous)
    chosen, seen = [], set()
    for index in np.random.default_rng(20261101).permutation(len(matrix)):
        digest = row_sha(matrix[index])
        if digest not in excluded and digest not in seen:
            chosen.append(int(index))
            seen.add(digest)
        if len(chosen) == 512:
            break
    if len(chosen) != 512:
        raise RuntimeError("not enough new unique queries")
    queries = np.asarray(matrix[chosen], dtype="<f4")
    queries.tofile(out / "queries.f32")
    # Truth is used only by quality checks, never by ANN execution/timers.
    # Float64 norm-dot over ALL 100K vectors, followed by direct L2 audit of
    # each query's top128. Ties resolve by row ID. No truth distance cache is
    # loaded by the SQL benchmark.
    center = np.asarray(base, dtype=np.float64).mean(axis=0)
    x = np.asarray(base, dtype=np.float64) - center
    xn = np.einsum("ij,ij->i", x, x)
    truth, distances, gaps = [], [], []
    for start in range(0, 512, 16):
        q = queries[start:start + 16].astype(np.float64) - center
        scores = xn[None, :] + np.einsum("ij,ij->i", q, q)[:, None] - 2 * q @ x.T
        for local, score in enumerate(scores):
            top = np.argpartition(score, 128)[:128]
            delta = np.asarray(base[top], dtype=np.float64) - queries[start + local].astype(np.float64)
            direct = np.einsum("ij,ij->i", delta, delta)
            a = top[np.lexsort((top, score[top]))[:11]]
            order = np.lexsort((top, direct))
            b = top[order[:11]]
            if not np.array_equal(a, b):
                raise RuntimeError("norm-dot / direct L2 top11 disagreement")
            truth.append(b[:10])
            distances.append(direct[order[:10]])
            gaps.append(float(direct[order[10]] - direct[order[9]]))
        print("audited float64 truth", min(start + 16, 512), "/512", flush=True)
    np.asarray(truth, dtype="<i4").tofile(out / "truth-100000.i32")
    np.asarray(distances, dtype="<f8").tofile(out / "truth-distances.f64")
    save(out / "manifest.json", {"prepared_epoch": time.time(), "method_frozen_epoch": selection["frozen_epoch"],
        "selection_sha256": sha(run / "selection.json"), "source_sha256": source_hash,
        "source_path": "data/train-00003-of-00026.parquet", "revision": "4a9731217921bc476a0f03544f11f22ae4903fa5",
        "source_rows": chosen, "query_count": 512, "dim": 1536, "seed": 20261101,
        "excluded_base_and_historical_query_hashes": True,
        "original_two_holdout_files_read": False,
        "old_holdout_source_shards": [0,1,2], "new_source_shard": 3,
        "scope": "new corpus-vector proxy queries, not real user questions; same unchanged graph seed",
        "truth": "float64 exhaustive norm-dot; all query top128 direct squared-L2 audit; distance then ID",
        "minimum_top10_boundary_gap": min(gaps), "base_sha256": sha(base_path),
        "files": {f.name: sha(f) for f in out.iterdir() if f.is_file()}})


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", required=True, type=Path)
    prepare(parser.parse_args().run.resolve())
