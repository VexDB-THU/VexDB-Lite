"""PQ graph candidates must survive to exact reranking on clustered inputs."""
import argparse
import json
import math
from pathlib import Path

from run_filtered_spec_with_sdk import Engine


def run(library, extension, out):
    if out.exists():
        raise RuntimeError("output already exists")
    out.mkdir(parents=True)
    engine = Engine(library)
    cases = []
    try:
        engine.execute("LOAD '" + str(extension.resolve()).replace("'", "''") + "'")
        engine.execute("SET threads=1")
        engine.execute("CREATE TABLE vectors AS SELECT i::INTEGER id, "
                       "list((floor(i/512)*2+.3*(sin(i*17.13+d*31.77)+cos(i*7.71+d*11.7)))::FLOAT "
                       "ORDER BY d)::FLOAT[64] vec FROM range(4096)t(i),range(64)s(d) GROUP BY i")
        engine.execute("CREATE INDEX vector_idx ON vectors USING GRAPH_INDEX(vec) "
                       "WITH(metric='l2',quantizer='pq',pq_m=16,m=16,ef_construction=100,threads=1)")
        engine.execute("SET vexdb_pq_search_mode='pq_graph'")
        engine.execute("SET vexdb_ef_search=1024")
        engine.execute("SET vexdb_pq_refine_k_factor=2")
        for i in range(16):
            query = [2*(i%8)+.3*(math.sin(i*91.13+d*11.71)+math.cos(i*71.3+d*18.11)) for d in range(64)]
            sql = "SELECT id FROM vectors ORDER BY l2_distance(vec,[" + ",".join(map(repr,query)) + "]::FLOAT[64]) LIMIT 10"
            engine.execute("SET vexdb_brute_force_threshold=1000000")
            expected = engine.execute(sql)
            engine.execute("SET vexdb_brute_force_threshold=0")
            plan = engine.execute("EXPLAIN " + sql)
            if not any("VEXDB_INDEX_SCAN" in (cell or "") for row in plan for cell in row):
                raise RuntimeError("index was not used")
            actual = engine.execute(sql)
            a, b = {int(r[0]) for r in actual}, {int(r[0]) for r in expected}
            cases.append({"query":i,"expected":sorted(b),"actual":sorted(a),
                          "passed":len(actual)==10 and len(a)==10 and a==b})
        passed = all(c["passed"] for c in cases)
        (out/"result.json").write_text(json.dumps({"passed":passed,"cases":cases,
            "scope":"Synthetic regression against independently executed exhaustive SQL; no expected neighbor IDs embedded."},indent=2))
        if not passed:
            raise RuntimeError(f"{sum(not c['passed'] for c in cases)}/{len(cases)} queries lost exact neighbors")
        print("PASS",len(cases),"clustered-query results match exhaustive SQL")
    finally:
        engine.close()


if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--library",required=True,type=Path);p.add_argument("--extension",required=True,type=Path)
    p.add_argument("--out",required=True,type=Path);a=p.parse_args();run(a.library,a.extension,a.out)
