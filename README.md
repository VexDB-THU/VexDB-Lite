<div align="center">
  <h1>VexDB-Lite</h1>
  <p><strong>Lightweight High-Performance Vector Database</strong></p>
  <p>Embedded vector database with GRAPH_INDEX — an enhanced HNSW engine featuring parallel build, vector dedup, PQ quantization, and SIMD-accelerated distance computation</p>
  <p><a href="README.md">English</a> | <a href="README_zh.md">中文</a></p>
</div>

---

## Overview

VexDB-Lite is the lightweight edition of the [VexDB](https://vexdb.com/) vector database family, designed for embedded and edge deployment scenarios. It brings VexDB's core vector search capabilities to a columnar analytics engine, providing a full SQL interface that makes vector search as simple as a regular query.

### VexDB vs VexDB-Lite

| | VexDB | VexDB-Lite |
|---|---|---|
| **Purpose** | Enterprise distributed vector database | Lightweight embedded vector database |
| **Storage Engine** | openGauss row-store engine | DuckDB columnar analytics engine |
| **Index Algorithm** | HNSW (disk-based page storage, parallel Vacuum) | HNSW (in-memory graph, FixedSizeAllocator persistence) |
| **Graph Repair** | Full `RepairGraphElement` with parallel Workers | Synchronous delete + repair (update entry → clean neighbors → repair connections) |
| **Quantizer** | PQ / RaBitQ / adaptive online update | Product Quantization (PQ) |
| **Deployment** | Client/Server, clustered | In-process embedded, single-file database |
| **Hybrid Index** | Partitioned tables + HNSW | HYBRID_INDEX (in-memory partitioned HNSW) |
| **Scale** | Billions of vectors | Millions of vectors |
| **Use Cases** | Production, enterprise RAG, large-scale recommendation | Edge devices, mobile, prototyping, dev/test |

VexDB-Lite's GRAPH_INDEX core algorithms (multi-layer graph search, neighbor selection, graph repair) are derived from VexDB's GRAPH_INDEX implementation, adapted for embedded scenarios:
- **In-memory graph structure** replaces VexDB's disk page storage, reducing I/O overhead
- **FixedSizeAllocator serialization** replaces VexDB's WAL/page-level persistence, simplifying the storage layer
- **Synchronous graph repair** replaces VexDB's deferred Vacuum Workers, ensuring immediate consistency
- Retains VexDB's core design: **hierarchical navigation, bidirectional connections, post-delete graph repair, level overflow protection**

### Relationship with DuckDB

VexDB-Lite is built on top of [DuckDB](https://duckdb.org/), an in-process analytical database. It extends DuckDB with vector search capabilities while fully preserving the DuckDB ecosystem:

- **Full SQL support** — all DuckDB SQL features (JOIN, CTE, window functions, etc.) work alongside vector search
- **Python binding** — install via `pip`, use with Pandas, Polars, and Arrow
- **File format compatibility** — read/write Parquet, CSV, JSON natively
- **Single-file database** — no server, no dependencies, just a `.duckdb` file

**vs DuckDB vss extension**: DuckDB's official [vss extension](https://duckdb.org/docs/extensions/vss.html) provides basic HNSW indexing. VexDB-Lite goes further with parallel build, vector dedup, PQ quantization, hybrid filtered index, synchronous graph repair, and SIMD-accelerated distance computation.

## Use Cases

- **RAG Applications** — Semantic retrieval + structured filtering in one query
- **Recommendation Systems** — Vector similarity + business attribute hybrid ranking
- **Image/Audio Retrieval** — Nearest neighbor search on high-dimensional feature vectors
- **Embedded Deployment** — Zero-dependency, single-file database for edge devices and mobile
- **Dev/Test** — Quickly validate vector search logic locally, then migrate to VexDB in production

## Features

### Native Vector Type
```sql
CREATE TABLE documents (
    id INTEGER PRIMARY KEY,
    content VARCHAR,
    embedding FLOATVECTOR(768)
);
```

### GRAPH_INDEX — Enhanced HNSW
GRAPH_INDEX is built on the HNSW (Hierarchical Navigable Small World) algorithm with key enhancements over vanilla HNSW:
- **Parallel build** — multi-threaded index construction with lock-free search and per-node spinlocks
- **Vector deduplication** — identical vectors share a single graph node, reducing memory and improving search speed
- **PQ quantization** — optional Product Quantization for lower memory footprint on large datasets
- **SIMD distance** — SSE, AVX2, ARM NEON optimized distance computation with generic fallback
- **Synchronous graph repair** — immediate connectivity repair on delete, no background vacuum needed
- **Auto brute-force fallback** — small datasets automatically use exact search for 100% recall

Supports L2, cosine, and inner product distance metrics.
```sql
-- Create index (default: L2 distance)
CREATE INDEX idx ON documents USING GRAPH_INDEX (embedding)
    WITH (m = 16, ef_construction = 64);

-- L2 distance search (automatically uses index)
SELECT id, content
FROM documents
ORDER BY l2_distance(embedding, [0.1, 0.2, ...]::FLOATVECTOR(768))
LIMIT 10;

-- Cosine distance index (auto-normalizes vectors, ideal for text semantic search)
CREATE INDEX idx_cos ON documents USING GRAPH_INDEX (embedding)
    WITH (metric = 'cosine');

SELECT id, content
FROM documents
ORDER BY cosine_distance(embedding, [0.1, 0.2, ...]::FLOATVECTOR(768))
LIMIT 10;

-- Inner product index (ideal for recommendation score ranking)
CREATE INDEX idx_ip ON documents USING GRAPH_INDEX (embedding)
    WITH (metric = 'ip');

SELECT id, content
FROM documents
ORDER BY inner_product(embedding, [0.1, 0.2, ...]::FLOATVECTOR(768)) DESC
LIMIT 10;
```

### Hybrid Filtered Index
Builds independent HNSW graphs per partition key for efficient "filter + vector search" queries.
```sql
CREATE INDEX idx ON products USING HYBRID_INDEX (embedding, category)
    WITH (metric = 'cosine');

-- Vector search within a partition
SELECT * FROM products
WHERE category = 'electronics'
ORDER BY cosine_distance(embedding, ?::FLOATVECTOR(128))
LIMIT 20;
```

### Distance Functions

| Function / Operator | Description | Order | Use Case |
|------|------|----------|----------|
| `l2_distance` / `<->` | Euclidean distance | ASC (smaller = closer) | General vector search |
| `cosine_distance` / `<=>` | Cosine distance | ASC (smaller = closer) | Text semantic similarity |
| `inner_product` | Inner product | DESC (larger = more similar) | Recommendation scoring |
| `<~>` | Negative inner product | ASC (smaller = more similar) | Operator form of inner product, `vec <~> query` equals `-inner_product(vec, query)` |

### Product Quantization (PQ)
```sql
CREATE INDEX idx ON vectors USING GRAPH_INDEX (vec)
    WITH (quantizer = 'pq', pq_m = 8);
```
PQ compresses high-dimensional vectors into compact codes, reducing memory usage and accelerating search on large datasets.

### Full Persistence Support
- **Checkpoint serialization**: indexes are persisted to disk alongside the database
- **WAL replay**: uncommitted changes are recovered via write-ahead log
- **Restart safe**: indexes are automatically restored after database restart, no rebuild needed

## Architecture

```
+---------------------------------------------------+
|                   SQL Interface                    |
|   FLOATVECTOR type - Distance functions - Optimizer|
+---------------------------------------------------+
|                   Index Layer                      |
|   GRAPH_INDEX (HNSW)  -  HYBRID_INDEX (Partitioned)|
|   Product Quantizer   -  SIMD Distance Compute     |
+---------------------------------------------------+
|                 Storage Engine                      |
|   Columnar Storage - Transactions - Checkpoint - WAL|
+---------------------------------------------------+
```

## Quick Start

### Build

```bash
# Release build (optimized, for deployment and benchmarks)
./build.sh release

# Debug build (with debug info and unit tests)
./build.sh dev

# Debug + AddressSanitizer (detect memory issues)
./build.sh dev --asan

# Build and run all vex tests
./build.sh test

# Run tests matching a specific name
./build.sh test --filter 'graph_index_dedup'

# Specify parallel build threads
./build.sh release -j4

# Clean all build directories
./build.sh clean
```

| Command | Build Type | Default Target | Output Path |
|---------|-----------|----------------|-------------|
| `dev` | Debug | unittest | `duckdb/build/debug/` |
| `dev --asan` | Debug+ASan | unittest | `duckdb/build/asan/` |
| `release` | Release | duckdb | `duckdb/build/release/` |
| `test` | Release | unittest | Builds then runs tests |

### Usage

```bash
./duckdb/build/release/duckdb
```

```sql
-- Load the VexDB vector extension
LOAD vex;

-- Create a table
CREATE TABLE items (id INTEGER PRIMARY KEY, vec FLOATVECTOR(4));

-- Create an HNSW index
CREATE INDEX idx ON items USING GRAPH_INDEX (vec);

-- Insert data
INSERT INTO items VALUES
    (1, [1.0, 0.0, 0.0, 0.0]::FLOATVECTOR(4)),
    (2, [0.0, 1.0, 0.0, 0.0]::FLOATVECTOR(4)),
    (3, [0.0, 0.0, 1.0, 0.0]::FLOATVECTOR(4));

-- Vector search
SELECT id FROM items
ORDER BY l2_distance(vec, [0.9, 0.1, 0.0, 0.0]::FLOATVECTOR(4))
LIMIT 1;
-- Result: 1
```

## Index Parameters

### GRAPH_INDEX

| Parameter | Default | Range | Description |
|-----------|---------|-------|-------------|
| `metric` | `l2` | `l2` / `cosine` / `ip` | Distance metric. `cosine` auto-normalizes vectors, `ip` for inner product (larger = more similar) |
| `m` | 16 | 2-100 | Max neighbors per layer. Higher = better index quality, more memory |
| `ef_construction` | 64 | 4-1000 | Search width during construction. Higher = slower build but better quality |
| `quantizer` | none | none/pq | Quantizer type |
| `pq_m` | auto | - | Number of PQ subspaces (dimension must be divisible) |
| `max_dedup` | 8 | 1-256 | Deduplication capacity (1 = disabled). Identical vectors share a graph node |
| `threads` | 1 | 1-N | Parallel build threads. For large datasets, set to number of CPU cores |

### HYBRID_INDEX

| Parameter | Default | Description |
|-----------|---------|-------------|
| `metric` | `l2` | Distance metric, same as GRAPH_INDEX |
| `m` | 16 | Same as GRAPH_INDEX |
| `ef_construction` | 64 | Same as GRAPH_INDEX |

## Runtime Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `vex_ef_search` | 40 | Search expansion factor. Higher = better recall, slower |
| `vex_brute_force_threshold` | 64 | When node count is below this value, use brute-force search (100% recall) |

```sql
-- Improve search quality
SET vex_ef_search = 100;

-- Force brute-force search for small tables
SET vex_brute_force_threshold = 1000;

-- Reset to defaults
RESET vex_ef_search;
RESET vex_brute_force_threshold;
```

## Project Structure

```
extension/vex/
├── include/              # Headers
│   ├── vex_graph_index.hpp
│   ├── vex_graph_index_core.hpp
│   ├── vex_hybrid_index.hpp
│   ├── vex_distance.hpp
│   ├── vex_quantizer.hpp
│   ├── vex_optimizer.hpp
│   ├── vex_types.hpp
│   └── vex_functions.hpp
├── index/                # Index implementations
│   ├── graph_index.cpp        # HNSW graph index
│   ├── graph_index_core.cpp   # Core graph algorithms
│   └── hybrid_index.cpp       # Hybrid filtered index
├── distance/             # SIMD distance computation
├── functions/            # SQL function registration
├── optimizer/            # Query optimizer (ORDER BY → index scan)
├── quantizer/            # Product Quantizer
├── types/                # FLOATVECTOR type
└── vex_extension.cpp     # Extension entry point

test/sql/vex/            # 75 test files
├── types/                # Type tests
├── functions/            # Distance function tests
└── index/                # Index tests (persistence, stress, PQ, etc.)
```

## Testing

```bash
# Build and run all vex tests
./build.sh test

# Run specific tests
./build.sh test --filter 'graph_index_dedup'

# Or invoke unittest directly
duckdb/build/release/test/unittest "test/sql/vex/*"
duckdb/build/release/test/unittest "test/sql/vex/index/graph_index_basic.test"
```

Test coverage across **75 test files** includes:
- Basic CRUD and search correctness
- HNSW index persistence and WAL replay
- Graph repair and search quality after heavy deletes
- PQ quantized search and persistence
- Hybrid index multi-partition operations
- Brute-force / graph search threshold boundaries
- Concurrent read/write, transaction rollback
- Vector deduplication
- Large-scale recall-after-delete validation
- High-dimensional vectors (128D)
- Multiple distance metrics (L2 / Cosine / Inner Product)
- Edge cases: unnormalized, zero, and reversed vectors
- Recall@10 vs brute-force quality verification

## Contributing

Contributions are welcome! Please open an issue to discuss your idea before submitting a pull request.

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE) for details.
