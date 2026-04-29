# vexdb-duck

DuckDB `GRAPH_INDEX` extension scaffold for `pg_vexdb` algorithm reuse.

## Building

This extension is built as part of DuckDB's extension system. It requires a DuckDB source tree.

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
cmake ..
make vex_extension -j$(nproc)           # Static library
make vex_loadable_extension -j$(nproc)  # Loadable extension
```

### Build Artifacts

- Static library: `$DUCKDB_DIR/build/extension/vex/libvex_extension.a`
- Loadable extension: `$DUCKDB_DIR/build/extension/vex/vex.duckdb_extension`

## Current Scope

- DuckDB extension entrypoint
- `GRAPH_INDEX` type registration
- custom create-index / optimizer / scan skeleton
- no duplicated HNSW core from `VexDB-Lite`

## Planned Next

- wire `GraphIndex` build/search to shared `pg_vexdb` algorithms
- add `l2_distance` and ANN query path
- run SIFT benchmark
