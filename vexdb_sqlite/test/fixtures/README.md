# SQLite persistence fixtures

`sqlite_quantizer_legacy_v3.db.gz` was generated with VexDB commit
`731539a8fba18726272b2fceb2891dbc5d74d855`, whose graph metadata writer used
format v3. It contains compact PQ and compact RaBitQ virtual tables.

Uncompressed SHA-256:

```text
7e538a216e3827880e3fe55c1809031cb28e787d3aa74c4825f610bbaa2faeb7
```

To reproduce it, build the old SQLite extension and run:

```bash
python3 vexdb_sqlite/test/create_legacy_quantizer_fixture.py \
  /absolute/path/to/old/vexdb_lite.dylib \
  /tmp/sqlite_quantizer_legacy_v3.db
gzip -9 /tmp/sqlite_quantizer_legacy_v3.db
```

`legacy_fixture_test.py` opens the fixed v3 file with current code, searches
both quantizers, performs updates that migrate metadata to v4, and reopens it.
