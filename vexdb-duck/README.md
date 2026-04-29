# vexdb-duck

DuckDB `GRAPH_INDEX` extension for HNSW vector similarity search.

## Features

- HNSW (Hierarchical Navigable Small World) graph index for approximate nearest neighbor search
- Persistent storage via DuckDB's FixedSizeAllocator (survives checkpoint/restart)
- L2 distance metric
- Configurable index parameters (m, ef_construction)

## Building

This extension is built as part of DuckDB's extension system.

### Prerequisites

- DuckDB source at `/home/mingwei6/workspace/duckdb_src` (or set via `DUCKDB_DIR`)
- C++17 compiler
- CMake

### Build Steps

1. Create a local extension config in DuckDB's extension directory:

```bash
cat > $DUCKDB_DIR/extension/extension_config_local.cmake << 'EOF'
duckdb_extension_load(vex
    SOURCE_DIR "/home/mingwei6/workspace/vexlite/vexdb-duck"
    INCLUDE_DIR "/home/mingwei6/workspace/vexlite/vexdb-duck/include"
)
EOF
```

2. Configure and build:

```bash
cd $DUCKDB_DIR/build
cmake .. -DOVERRIDE_GIT_DESCRIBE=v1.5.2
make vex_loadable_extension -j8
```

> Note: `-DOVERRIDE_GIT_DESCRIBE=v1.5.2` is required to match the Python duckdb package version.

### Build Artifacts

- Loadable extension: `$DUCKDB_DIR/build/extension/vex/vex.duckdb_extension`

## Usage

```python
import duckdb

# Load extension
con = duckdb.connect(config={'allow_unsigned_extensions': 'true'})
con.execute("LOAD '/path/to/vex.duckdb_extension'")

# Create table with vectors
con.execute("CREATE TABLE items (id INTEGER, vec FLOAT[128])")

# Insert vectors
con.execute("INSERT INTO items VALUES (1, [1.0, 2.0, ...]::FLOAT[128])")

# Create HNSW index
con.execute("CREATE INDEX idx_vec ON items USING GRAPH_INDEX (vec) WITH (metric='l2', m=16, ef_construction=64)")

# Query for nearest neighbors
results = con.execute("SELECT * FROM items ORDER BY vec <-> '[1.0, 2.0, ...]'::FLOAT[128] LIMIT 10").fetchall()
```

### Index Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `m` | 16 | Number of neighbors per node (higher = better recall, more memory) |
| `ef_construction` | 64 | Dynamic candidate list size during build (higher = better quality, slower build) |
| `metric` | 'l2' | Distance metric (currently only 'l2' supported) |

## Testing

Run the smoke test with SIFT data:

```bash
python3 test/smoke_sift_literal.py \
    /path/to/vex.duckdb_extension \
    /path/to/sift.csv \
    /path/to/sift.sql \
    --row-limit 500 \
    --query-limit 20
```

## Architecture

- `vex_hnsw_node.hpp`: Segment layouts for FixedSizeAllocator storage
- `vex_graph_index_depend_duck.hpp`: DuckStore (MemStore) with allocator-backed storage
- `vex_graph_index.hpp`: GraphIndex class implementing BoundIndex interface
- `graph_index.cpp`: Index build, search, and serialization logic
- `graph_index_disk.cpp`: Disk serialization via FixedSizeAllocator

## Storage

The index uses DuckDB's `FixedSizeAllocator` for persistent storage:

- **Node allocator**: Stores node headers and level-0 neighbors
- **Vector allocator**: Stores vector float data
- **Upper allocator**: Stores upper-level neighbors for multi-layer nodes

Data persists through DuckDB's checkpoint/WAL mechanism automatically.
