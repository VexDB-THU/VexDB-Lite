"""Generate registered synthetic/proxy queries after freezing the PQ method."""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[3]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(run):
    frozen = json.loads((run / "method-freeze.json").read_text())
    for name, digest in frozen["inputs"].items():
        if sha(ROOT / name) != digest:
            raise RuntimeError("frozen method changed: " + name)
    datasets = {
        "catalog": (20000, 64, "build/product-tests/pq-filter-generalization-20261027/catalog-data"),
        "sift": (100000, 128, "build/product-tests/quantization-product-20261027/data/sift"),
        "text": (100000, 1536, "build/product-tests/quantization-product-20261027/data/dbpedia"),
    }
    for name, specification in frozen["holdout"].items():
        dataset = name.split("-")[0]
        rows, dim, data = datasets[dataset]
        base_path = ROOT / data / "base.f32"
        base = np.memmap(base_path, dtype="<f4", mode="r", shape=(rows, dim))
        rng = np.random.default_rng(specification["seed"])
        count = specification["count"]
        selected = None
        if name.startswith("catalog"):
            categories = np.zeros(count, dtype=np.int32) if name.endswith("matched") else rng.integers(0, 10, count)
            query = (rng.normal(0, .3, (count, dim)) + 2 * categories[:, None]).astype("<f4")
        else:
            selected = rng.choice(rows, count, replace=False)
            scale = np.asarray(base[:4096], dtype=np.float64).std(axis=0)
            query = (base[selected] + rng.normal(0, 1, (count, dim)) * (.01 * scale)).astype("<f4")
            if dataset == "text":
                query /= np.linalg.norm(query, axis=1, keepdims=True)
        out = run / "holdout" / name
        out.mkdir(parents=True, exist_ok=False)
        query.tofile(out / "queries.f32")
        # Centered float64 exhaustive distance, with direct top-32 confirmation.
        center = np.asarray(base[:4096], dtype=np.float64).mean(axis=0)
        vectors = np.asarray(base, dtype=np.float64) - center
        norm = np.einsum("ij,ij->i", vectors, vectors)
        truth, gaps = [], []
        for start in range(0, count, 16):
            q = query[start:start+16].astype(np.float64) - center
            scores = norm[None,:] + np.einsum("ij,ij->i",q,q)[:,None] - 2*q@vectors.T
            for i, score in enumerate(scores):
                ids = np.argpartition(score, 32)[:32]
                delta = np.asarray(base[ids], dtype=np.float64) - query[start+i].astype(np.float64)
                direct = np.einsum("ij,ij->i", delta, delta)
                ordered = ids[np.lexsort((ids, direct))]
                centered = ids[np.lexsort((ids, score[ids]))]
                if not np.array_equal(ordered[:11], centered[:11]):
                    raise RuntimeError("direct and centered oracle disagree")
                truth.append(ordered[:10])
                ds = np.sort(direct)
                gaps.append(float(ds[10]-ds[9]))
        np.asarray(truth,dtype="<i4").tofile(out / "truth.i32")
        manifest = {"name": name, "dataset": dataset, "rows": rows, "dim": dim, "queries": count,
            "specification": specification, "base": str(base_path.relative_to(ROOT)), "base_sha256": sha(base_path),
            "selected_base_rows_for_perturbation": selected.tolist() if selected is not None else None,
            "oracle": "Independent exhaustive centered float64 L2; top32 direct audit, row ID at exact ties.",
            "minimum_k_boundary_gap": min(gaps), "method_freeze_sha256": sha(run/"method-freeze.json"),
            "files": {p.name: sha(p) for p in out.iterdir() if p.is_file()}, "numpy": np.__version__}
        (out/"manifest.json").write_text(json.dumps(manifest,indent=2))
        print("READY",name,flush=True)
        del base,vectors


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run", required=True, type=Path)
    prepare(p.parse_args().run.resolve())
