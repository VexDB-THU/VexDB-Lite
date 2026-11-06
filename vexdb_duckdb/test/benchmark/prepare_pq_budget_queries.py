"""Prepare two new disjoint query cohorts for one already-frozen PQ budget."""
import argparse
import hashlib
import json
import subprocess
import time
from pathlib import Path

from run_pq_budget_validation import ROOT, save, sha


def prepare(run):
    protocol = json.loads((run / "protocol.json").read_text())
    frozen = json.loads((run / "freeze.json").read_text())
    if sha(run / "protocol.json") != frozen["protocol_sha256"] or time.time() >= protocol["deadline_epoch"]:
        raise RuntimeError("unfrozen or expired study")
    for name, digest in frozen["inputs"].items():
        if sha(ROOT / name) != digest:
            raise RuntimeError("frozen method input changed: " + name)
    import numpy as np
    import pyarrow as pa
    import pyarrow.parquet as pq
    payload = subprocess.run(["curl", "--fail", "--silent", "--show-error", "--location", "--retry", "2",
                              "--max-time", "120", protocol["source_url"]], capture_output=True, check=True).stdout
    if hashlib.sha256(payload).hexdigest() != protocol["source_sha256"]:
        raise RuntimeError("pinned public shard hash mismatch")
    table = pq.read_table(pa.BufferReader(payload), columns=["text-embedding-3-large-1536-embedding"])
    values = table.column(0).combine_chunks()
    if not np.all(np.asarray(values.value_lengths()) == 1536):
        raise RuntimeError("wrong vector dimension")
    matrix = np.asarray(values.values, dtype=np.float32).reshape(len(values), 1536).copy()
    norms = np.linalg.norm(matrix, axis=1, keepdims=True)
    if not np.isfinite(matrix).all() or np.any(norms <= 0):
        raise RuntimeError("invalid source vectors")
    matrix /= norms
    base_file = ROOT / "build/product-tests/quantization-product-20261027/data/dbpedia/base.f32"
    base = np.memmap(base_file, dtype="<f4", mode="r", shape=(100000, 1536))
    previous = np.load(ROOT / "build/research/quantization/dbpedia100k/queries.npy")
    failed = ROOT / protocol["parent_failed_run"] / "new-queries"
    old_manifest = json.loads((failed / "manifest.json").read_text())
    old_queries = np.fromfile(failed / "queries.f32", dtype="<f4").reshape(512, 1536)
    excluded_rows = set(old_manifest["source_rows"])
    row_sha = lambda row: hashlib.sha256(np.asarray(row, dtype="<f4").tobytes()).digest()
    excluded = {row_sha(row) for row in base}
    excluded.update(row_sha(row) for row in previous)
    excluded.update(row_sha(row) for row in old_queries)
    chosen, seen = [], set()
    for index in np.random.default_rng(20261102).permutation(len(matrix)):
        digest = row_sha(matrix[index])
        if int(index) not in excluded_rows and digest not in excluded and digest not in seen:
            chosen.append(int(index))
            seen.add(digest)
        if len(chosen) == 2048:
            break
    if len(chosen) != 2048:
        raise RuntimeError("insufficient unused source rows")
    center = np.asarray(base, dtype=np.float64).mean(axis=0)
    x = np.asarray(base, dtype=np.float64) - center
    xn = np.einsum("ij,ij->i", x, x)
    for cohort, selected in (("a", chosen[:1024]), ("b", chosen[1024:])):
        out = run / "queries" / cohort
        out.mkdir(parents=True, exist_ok=False)
        queries = np.asarray(matrix[selected], dtype="<f4")
        queries.tofile(out / "queries.f32")
        truth, distances, gaps = [], [], []
        for start in range(0, 1024, 16):
            q = queries[start:start + 16].astype(np.float64) - center
            scores = xn[None, :] + np.einsum("ij,ij->i", q, q)[:, None] - 2 * q @ x.T
            for local, score in enumerate(scores):
                top = np.argpartition(score, 128)[:128]
                delta = np.asarray(base[top], dtype=np.float64) - queries[start + local].astype(np.float64)
                direct = np.einsum("ij,ij->i", delta, delta)
                order = np.lexsort((top, direct))
                a = top[np.lexsort((top, score[top]))[:11]]
                b = top[order[:11]]
                if not np.array_equal(a, b):
                    raise RuntimeError("float64 norm-dot / direct squared-L2 truth disagreement")
                truth.append(b[:10])
                distances.append(direct[order[:10]])
                gaps.append(float(direct[order[10]] - direct[order[9]]))
            if start % 128 == 0:
                print("cohort", cohort, "truth", min(start + 16, 1024), flush=True)
        np.asarray(truth, dtype="<i4").tofile(out / "truth-100000.i32")
        np.asarray(distances, dtype="<f8").tofile(out / "truth-distances.f64")
        save(out / "manifest.json", {"prepared_epoch": time.time(), "freeze_sha256": sha(run / "freeze.json"),
             "cohort": cohort, "count": 1024, "dimension": 1536, "seed": 20261102,
             "source_sha256": protocol["source_sha256"], "source_rows": selected,
             "excluded_previous_failed512_source_rows_and_vector_hashes": True,
             "excluded_base_and_historical_queries_by_hash": True, "old_two_256_holdout_files_read": False,
             "cohort_a_b_disjoint_by_row_and_vector_hash": True,
             "truth": "float64 full100K norm-dot plus every query top128 direct-L2 audit, distance then ID",
             "minimum_top10_boundary_gap": min(gaps), "base_sha256": sha(base_file),
             "scope": "unseen corpus-vector proxies, not customer questions; same fixed graph seed",
             "files": {f.name: sha(f) for f in out.iterdir() if f.is_file()}})
        print("cohort", cohort, "prepared1024 new queries; no ANN quality evaluated", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", required=True, type=Path)
    prepare(parser.parse_args().run.resolve())
