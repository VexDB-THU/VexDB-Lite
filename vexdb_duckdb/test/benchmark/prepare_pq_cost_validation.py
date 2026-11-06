"""Prepare frozen PQ-cost validation inputs; never execute ANN or read sealed results."""
import argparse
import hashlib
import json
import shutil
import subprocess
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2))


def prepare(run):
    import numpy as np
    import pyarrow as pa
    import pyarrow.parquet as pq

    frozen = json.loads((run / "validation-freeze.json").read_text())
    for path, digest in frozen["inputs"].items():
        if sha(ROOT / path) != digest:
            raise RuntimeError("changed frozen input: " + path)
    if time.time() >= frozen["deadline_epoch"]:
        raise RuntimeError("original deadline expired")
    output = run / "validation"
    output.mkdir(exist_ok=False)
    for name, spec in frozen["cohorts"].items():
        if time.time() >= frozen["deadline_epoch"] or shutil.disk_usage(run).free < 4 * 2**30:
            raise RuntimeError("deadline/disk guard")
        out = output / name
        out.mkdir(exist_ok=False)
        rows, dim, count = spec["rows"], spec["dim"], spec["count"]
        base_path = ROOT / spec["base"]
        if sha(base_path) != spec["base_sha256"]:
            raise RuntimeError("base hash changed")
        base = np.memmap(base_path, dtype="<f4", mode="r", shape=(rows, dim))
        random = np.random.default_rng(spec["seed"])
        provenance = {}
        if spec["kind"] in ("catalog-matched", "catalog-mixed"):
            categories = (random.integers(0, 10, count) if spec["kind"] == "catalog-mixed"
                          else np.zeros(count, dtype=np.int64))
            queries = (random.normal(0, .3, (count, dim)) + 2 * categories[:, None]).astype("<f4")
            provenance["categories"] = categories.tolist()
        elif spec["kind"] == "sift-perturbed":
            selected = random.choice(rows, size=count, replace=False)
            scales = np.asarray(base[:4096], dtype=np.float64).std(axis=0)
            queries = (base[selected] + random.normal(0, 1, (count, dim)) * (.01 * scales)).astype("<f4")
            provenance["selected_base_rows"] = selected.tolist()
        elif spec["kind"] == "text-public":
            fetch = subprocess.run(["curl", "--fail", "--silent", "--show-error", "--location",
                                    "--retry", "2", "--max-time", "180", spec["source_url"]],
                                   capture_output=True, check=True)
            payload = fetch.stdout
            source_sha = hashlib.sha256(payload).hexdigest()
            table = pq.read_table(pa.BufferReader(payload), columns=[spec["column"]])
            values = table.column(0).combine_chunks()
            if not np.all(np.asarray(values.value_lengths()) == dim):
                raise RuntimeError("wrong public embedding dimension")
            matrix = np.asarray(values.values, dtype=np.float32).reshape(len(values), dim).copy()
            norms = np.linalg.norm(matrix, axis=1, keepdims=True)
            if not np.isfinite(matrix).all() or np.any(norms <= 0):
                raise RuntimeError("invalid public vectors")
            matrix /= norms
            row_sha = lambda row: hashlib.sha256(np.asarray(row, dtype="<f4").tobytes()).digest()
            excluded = {row_sha(row) for row in base}
            exclusion_files = []
            # Exclude existing product query bytes without evaluating their
            # quality. Old overnight sealed arrays
            # and truth files are outside this tree and are never opened.
            for path in sorted((ROOT / "build/product-tests").rglob("queries.f32")):
                if run in path.parents or path.stat().st_size % (dim * 4):
                    continue
                exposed = np.fromfile(path, dtype="<f4").reshape(-1, dim)
                excluded.update(row_sha(row) for row in exposed)
                exclusion_files.append({"path": str(path.relative_to(ROOT)), "sha256": sha(path)})
            selected, seen = [], set()
            for index in random.permutation(len(matrix)):
                digest = row_sha(matrix[index])
                if digest not in excluded and digest not in seen:
                    selected.append(int(index))
                    seen.add(digest)
                if len(selected) == count:
                    break
            if len(selected) != count:
                raise RuntimeError("insufficient unused public vectors")
            queries = np.asarray(matrix[selected], dtype="<f4")
            provenance.update(source_url=spec["source_url"], source_sha256=source_sha,
                              source_rows=selected, excluded_prior_product_query_files=exclusion_files,
                              excluded_base_hashes=True, sealed_overnight_arrays_read=False)
            del payload, table, values, matrix, norms
        else:
            raise RuntimeError("unknown frozen query generation")
        queries.tofile(out / "queries.f32")
        center = np.asarray(base[:4096], dtype=np.float64).mean(axis=0)
        vectors = np.asarray(base, dtype=np.float64) - center
        squared = np.einsum("ij,ij->i", vectors, vectors)
        truth, truth_distances, gaps = [], [], []
        fallback = 0
        for start in range(0, count, 16):
            if time.time() >= frozen["deadline_epoch"]:
                raise RuntimeError("deadline during independent truth")
            q = queries[start:start + 16].astype(np.float64) - center
            qn = np.einsum("ij,ij->i", q, q)
            scores = squared[None, :] + qn[:, None] - 2 * q @ vectors.T
            for local, score in enumerate(scores):
                selected = np.argpartition(score, 128)[:128]
                delta = np.asarray(base[selected], dtype=np.float64) - queries[start + local].astype(np.float64)
                direct = np.einsum("ij,ij->i", delta, delta)
                order = np.lexsort((selected, direct))
                norm_order = selected[np.lexsort((selected, score[selected]))[:11]]
                error = 16 * np.finfo(np.float64).eps * dim * (float(squared.max()) + qn[local] +
                         2 * np.sqrt(float(squared.max()) * qn[local]))
                outside = np.partition(score, 128)[128]
                if (not np.array_equal(norm_order, selected[order[:11]]) or
                        outside - score[selected[order[9]]] <= 2 * error):
                    # Preserve a fixed direct squared-L2/ID oracle even at
                    # mathematical ties or norm-dot cancellation boundaries.
                    fallback += 1
                    direct_all = np.empty(rows, dtype=np.float64)
                    for first in range(0, rows, 4096):
                        delta = np.asarray(base[first:first + 4096], dtype=np.float64) - queries[start + local]
                        direct_all[first:first + len(delta)] = np.einsum("ij,ij->i", delta, delta)
                    selected = np.arange(rows)
                    direct = direct_all
                    order = np.lexsort((selected, direct))
                truth.append(selected[order[:10]])
                truth_distances.append(direct[order[:10]])
                gaps.append(float(direct[order[10]] - direct[order[9]]))
            if start % 128 == 0:
                print(name, "independent truth", min(count, start + 16), "/", count, flush=True)
        np.asarray(truth, dtype="<i4").tofile(out / "truth.i32")
        np.asarray(truth_distances, dtype="<f8").tofile(out / "truth-distances.f64")
        save(out / "manifest.json", {"prepared_epoch": time.time(), "freeze_sha256": sha(run / "validation-freeze.json"),
             "specification": spec, "provenance": provenance, "minimum_k_boundary_gap": min(gaps),
             "truth": "Exhaustive centered float64 norm-dot; top128 direct squared-L2 audit, distance then ID; full direct fallback at disagreement/ambiguous cutoff.",
             "full_direct_fallback_queries": fallback,
             "files": {p.name: sha(p) for p in out.iterdir() if p.is_file()}})
        del base, vectors, squared
        print(name, "ready; no ANN quality evaluated", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", type=Path, required=True)
    prepare(parser.parse_args().run.resolve())
