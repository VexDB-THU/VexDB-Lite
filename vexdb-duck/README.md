# vexdb-duck

DuckDB `GRAPH_INDEX` extension scaffold for `pg_vexdb` algorithm reuse.

Current scope:

- DuckDB extension entrypoint
- `GRAPH_INDEX` type registration
- custom create-index / optimizer / scan skeleton
- no duplicated HNSW core from `VexDB-Lite`

Planned next:

- wire `GraphIndex` build/search to shared `pg_vexdb` algorithms
- add `l2_distance` and ANN query path
- run SIFT benchmark
