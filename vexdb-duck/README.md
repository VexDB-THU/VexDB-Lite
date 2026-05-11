# vexdb-duck

`vexdb-duck` is the DuckDB side of `VexDB-Lite`.

This directory contains:

- the DuckDB extension entrypoint (`vex_extension.cpp`)
- registered scalar/table functions (`functions/`)
- the `GRAPH_INDEX` implementation (`index/`)
- optimizer and physical operators (`optimizer/`)
- benchmark and smoke scripts (`test/`)

For the up-to-date project overview, SQL examples, build steps, and benchmark results, use the repository root README files:

- [../README.md](../README.md)
- [../README.en.md](../README.en.md)

Quick reminders:

## Build

Register the extension in DuckDB's `extension_config_local.cmake`:

```cmake
duckdb_extension_load(vex
    SOURCE_DIR "/path/to/VexDB-Lite/vexdb-duck"
    INCLUDE_DIR "/path/to/VexDB-Lite/vexdb-duck/include"
)
```

Then build:

```bash
cd /path/to/duckdb/build
cmake .. -DOVERRIDE_GIT_DESCRIBE=v1.5.2
cmake --build . --target vex_loadable_extension -j$(nproc)
```

## Load

```sql
LOAD '/path/to/vex.duckdb_extension';
SELECT vex_version();
```

## Minimal SQL Example

```sql
CREATE TABLE items (id INTEGER, vec FLOAT[3]);

CREATE INDEX idx_items_vec
ON items
USING GRAPH_INDEX (vec)
WITH (metric='l2', m=16, ef_construction=64);

SELECT id
FROM items
ORDER BY l2_distance(vec, [1.0, 2.0, 3.0]::FLOAT[3])
LIMIT 10;
```
