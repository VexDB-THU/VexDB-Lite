# DuckDB persistence fixtures

`duckdb_quantizer_legacy_v1_v2.db.gz` was generated with DuckDB v1.5.2 and
VexDB commit `731539a8fba18726272b2fceb2891dbc5d74d855`, before the versioned PQ and
RaBitQ persistence changes.

Uncompressed SHA-256:

```text
c3e8f9c7a13f6e5fd3b52276ad460c8bac76b964eedbd2b8af5f84e5adb708a8
```

The generator is `vexdb_duckdb/test/create_legacy_quantizer_fixture.cpp`.
Build it against the old extension revision, create the database, then gzip it:

```bash
bash build_duck.sh bin
build/duck/v1.5.2/bin/create_legacy_quantizer_fixture \
  /absolute/path/to/old/vexdb_lite.duckdb_extension \
  /tmp/duckdb_quantizer_legacy_v1_v2.db
gzip -9 /tmp/duckdb_quantizer_legacy_v1_v2.db
```

The compatibility spec opens this fixed file with the current extension. This
prevents a test from accidentally writing and reading only the newest format.
