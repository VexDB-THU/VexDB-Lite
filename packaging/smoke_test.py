"""VexDB-Lite wheel smoke test."""
import duckdb

con = duckdb.connect()
print(f"DuckDB {duckdb.__version__}")

# VEX extension loaded
r = con.execute("SELECT loaded FROM duckdb_extensions() WHERE extension_name='vex'").fetchone()
assert r and r[0], "VEX extension not loaded!"
print("VEX extension: loaded")

# Distance function
r = con.execute("SELECT l2_distance([1,0,0]::FLOAT[3], [0,1,0]::FLOAT[3])").fetchone()
assert abs(r[0] - 1.4142135623730951) < 1e-6
print("l2_distance: OK")

# HNSW index + ANN search
con.execute("CREATE TABLE t (id INT, v FLOAT[3])")
con.execute("INSERT INTO t VALUES (1,[1,0,0]),(2,[0,1,0]),(3,[0,0,1])")
con.execute("CREATE INDEX idx ON t USING GRAPH_INDEX (v) WITH (metric='l2')")
r = con.execute("SELECT id FROM t ORDER BY l2_distance(v,[1,0,0]::FLOAT[3]) LIMIT 1").fetchone()
assert r[0] == 1
print("GRAPH_INDEX + ANN: OK")

print("All smoke tests passed!")
