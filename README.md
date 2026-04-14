<div align="center">
  <h1>VexDB-Lite</h1>
  <p><strong>Embedded Vector Search Engine — One File, One Database</strong></p>
  <p>No server. No network. Data stays on device.</p>
  <p><a href="README.md">English</a> | <a href="README_zh.md">中文</a></p>
</div>

---

<p align="center">
  <img src="docs/images/architecture.png" alt="Architecture" width="700">
</p>

## What is VexDB-Lite

VexDB-Lite is an embedded vector search engine. Think of it as a database that understands both traditional queries and semantic search — in one file.

Traditional databases match exact keywords. VexDB-Lite understands relationships — "fruit" and "apple" are related — because it searches vectors, not text. And you still get full SQL: JOIN, aggregation, filtering.

No Docker. No cluster. No network request.

```sql
-- Create a vector index
CREATE INDEX idx ON documents USING GRAPH_INDEX(embedding)
  WITH (metric='cosine', m=16, ef_construction=200);

-- Semantic search, one SQL query
SELECT title, content
FROM documents
ORDER BY cosine_distance(embedding, [0.1, 0.2, ...])
LIMIT 10;
```

---

## One Library, Every Platform

VexDB-Lite is not just a Python package. It's a compiled native engine that runs on iOS, Android, and browsers:

| Platform | Integration | Size |
|----------|------------|------|
| Python (Desktop/Server) | `pip install vexdb-lite` | — |
| Java | `vexdb-lite.jar` (JDBC) | 32 MB |
| iOS | `libvexdb.a` static lib | **11 MB** |
| Android | `libvexdb.a` + JNI | **11 MB** |
| Browser | WebAssembly module | **~8 MB** |
| Rust | `vexdb-lite` crate | — |
| Node.js | `vexdb-lite` npm package | — |

11 MB includes a full SQL engine + vector indexing + PQ quantization. No extension loading needed — everything is built-in.

---

## SIMD Acceleration, Everywhere

Different devices, same fast vector math:

- Intel/AMD laptops: SSE + AVX2
- Apple M series / Android: ARM NEON
- Browsers: WebAssembly SIMD128

From laptops to phones to edge devices to browsers — distance computation runs on hardware-accelerated paths.

---

## What It Can Do

**Two Index Types**

- **GRAPH_INDEX** — Graph-based ANN index for pure vector similarity search
- **HYBRID_INDEX** — Partitioned index for vector + scalar filtered search

One SQL query does both structured filtering and semantic search:

```sql
CREATE INDEX idx ON products USING HYBRID_INDEX(embedding, category);

SELECT name, price
FROM products
WHERE category = 'electronics'
ORDER BY cosine_distance(embedding, [0.1, 0.2, ...])
LIMIT 10;
```

**Three Distance Metrics** — L2, Cosine (auto-normalized), Inner Product

**PQ Quantization** — Compress vectors 4-8x with one config line:

```sql
CREATE INDEX idx ON docs USING GRAPH_INDEX(embedding)
  WITH (quantizer='pq', pq_m=8);
```

**Vector Dedup** — Duplicate vectors share graph nodes. Smaller index, faster search.

**Parallel Index Build** — Multi-threaded construction with `WITH (threads=8)`.

**Single-File Persistence** — Data + index in one `.db` file. Checkpoint, restart, auto-recovery.

---

## Use Cases

**Local RAG** — Run a knowledge base on your laptop. Document embeddings in VexDB-Lite, paired with a local LLM. Fully offline Q&A.

**Mobile Semantic Search** — Search photos by meaning, smart chat history retrieval, on-device recommendations. No network, faster UX, better privacy.

**IoT & Edge** — Anomaly detection on industrial equipment, sensor pattern matching at the edge. No cloud round-trip needed.

**Enterprise On-Prem** — For organizations with strict data security requirements. No external dependencies, simpler audit and compliance.

---

## Benchmark

<p align="center">
  <img src="docs/images/benchmark-comparison.png" alt="Benchmark" width="700">
</p>

**SIFT-1M** (1M × 128d, Apple M4 Pro, Recall ≥ 98.5%)

| Metric | VexDB-Lite | ChromaDB | LanceDB |
|--------|-----------|----------|---------|
| Data Import | 0.9s | 509s | 0.5s |
| Recall@10 | 99.7% | 99.6% | 99.1% |
| QPS | 647 | 586 | 562 |
| Avg Latency | 1.5ms | 1.7ms | 1.8ms |

<p align="center">
  <img src="docs/images/bench_curve_sift.png" alt="Recall vs QPS" width="600">
</p>

VexDB-Lite consistently occupies the upper-right corner of the Recall-QPS curve.

---

## Testing

**9 platforms, 259 test cases, 6302 assertions — all passed.**

| Platform | Cases | Assertions |
|----------|:-----:|:----------:|
| Desktop (full SQL) | 82 | 3446 |
| Python (full SQL) | 83 | 2394 |
| iOS Simulator | 16 | 108 |
| Android Emulator | 16 | 108 |
| WASM | 8 | 8 |
| Java JDBC | 13 | 13 |
| Rust | 9 | 9 |

---

## Documentation

| Platform | Guide |
|----------|-------|
| CLI | [usage_cli.md](extension/vex/docs/usage_cli.md) |
| Python | [usage_python.md](extension/vex/docs/usage_python.md) |
| Java | [usage_java.md](extension/vex/docs/usage_java.md) |
| iOS | [usage_ios.md](extension/vex/docs/usage_ios.md) |
| Android | [usage_android.md](extension/vex/docs/usage_android.md) |
| WASM | [usage_wasm.md](extension/vex/docs/usage_wasm.md) |
| Rust | [usage_rust.md](extension/vex/docs/usage_rust.md) |
| Node.js | [usage_nodejs.md](extension/vex/docs/usage_nodejs.md) |

---

## Build

```bash
# 1) Point to a separate DuckDB source checkout
export DUCKDB_SOURCE_DIR=/path/to/duckdb

# 2) (Optional) pin extension version tag to target DuckDB runtime
# export DUCKDB_VERSION_TAG=v1.4.4

# 3) Build loadable vex extension
./build.sh all Release

# 4) Output artifact (example)
# build/standalone/_duckdb/extension/vex/vex.duckdb_extension
```

Load in DuckDB:

```sql
LOAD '/absolute/path/to/vex.duckdb_extension';
SELECT vex_version();
```

---

## License

MIT

VexDB-Lite is built as an extension on top of [DuckDB](https://duckdb.org/) (MIT License).
