"""Validate filtered exact routing and OFFSET through real SQLite SQL.

Usage: python3 filtered_routing_regression.py /absolute/path/extension
The authorizer and SQL trace distinguish a cold metadata probe from graph loading.
This is a semantics/path regression; ANN recall and speed require native benchmarks.
"""
import array
from pathlib import Path
import sqlite3
import tempfile
import sys
import math
import json


def connect(path, extension):
    cx = sqlite3.connect(path)
    cx.enable_load_extension(True)
    cx.load_extension(str(extension))
    cx.enable_load_extension(False)
    return cx


def run(extension):
    n, dim = 4000, 16
    vecs = [array.array('f', (math.sin(i * (j + 1) * .013) + i * .001
                            for j in range(dim))).tobytes() for i in range(n)]
    query = array.array('f', (x + .017 for x in array.array('f', vecs[313]))).tobytes()
    with tempfile.TemporaryDirectory(prefix='vexdb-filter-route-') as directory:
        path = Path(directory) / 'vectors.db'
        with connect(path, extension) as cx:
            cx.execute('CREATE VIRTUAL TABLE vectors USING GRAPH_INDEX('
                       'embedding FLOAT[16],bucket INTEGER,band INTEGER,price REAL,'
                       'm=8,ef_construction=64,ef_search=64,brute_force_threshold=0)')
            # This test exercises the opt-in cost-based filter route.
            cx.execute("INSERT INTO vectors(vectors) VALUES('filtered_search_mode=auto')")
            cx.executemany('INSERT INTO vectors(rowid,embedding,bucket,band,price) '
                           'VALUES(?,?,?,?,?)',
                           ((i, vecs[i], i % 47, i % 337, None if i % 29 == 0 else i % 101)
                            for i in range(n)))
            # Build/persist the baseline graph before testing cold read connections.
            cx.execute('SELECT rowid FROM vectors WHERE embedding MATCH ? AND k=10',
                       (query,)).fetchall()
            cx.commit()
        cases = [('small', 'band=3', ()),
                 ('strong', 'bucket=3', ()),
                 ('combined', 'bucket=3 AND price>=20 AND price<90', ()),
                 ('empty', 'bucket=-1', ()),
                 ('null-equality', 'price=?', (None,))]
        for label, predicate, args in cases:
            for limit, offset in [(10, 0), (5, 3), (3, 20), (5, 5000)]:
                with connect(path, extension) as cx:
                    gold = cx.execute('SELECT rowid FROM vectors_vectors WHERE '+predicate+
                                      ' ORDER BY vexdb_l2_distance(vec,?) LIMIT ? OFFSET ?',
                                      (*args, query, limit, offset)).fetchall()
                    actual = cx.execute('SELECT rowid FROM vectors WHERE embedding MATCH ? AND '+
                                        predicate+' ORDER BY distance LIMIT ? OFFSET ?',
                                        (query, *args, limit, offset)).fetchall()
                    assert actual == gold, (label, limit, offset, actual, gold)
        # A cold exact route may read the small graph header for node count,
        # but must not load vector/adjacency segments or traverse the graph.
        with connect(path, extension) as cx:
            reads = []
            trace = []
            cx.set_trace_callback(trace.append)
            def authorizer(action, table, column, database, trigger):
                if action == sqlite3.SQLITE_READ and table == 'vectors_graph':
                    reads.append((table, column))
                return sqlite3.SQLITE_OK
            cx.set_authorizer(authorizer)
            rows = cx.execute('SELECT rowid FROM vectors WHERE embedding MATCH ? '
                              'AND bucket=3 AND k=10', (query,)).fetchall()
            assert len(rows) == 10
            segment_reads = [sql for sql in trace if 'vectors_graph' in sql and 'SELECT data' in sql]
            assert len(segment_reads) <= 1, ('strong filter loaded graph segments', segment_reads)
            info = json.loads(cx.execute('SELECT vexdb_search_info(embedding) FROM vectors LIMIT 1').fetchone()[0])
            eligible = cx.execute('SELECT count(*) FROM vectors_vectors WHERE bucket=3').fetchone()[0]
            assert info['route'] == 'exact_filtered' and info['distance_calls'] == eligible, info
        # Keep an ordinary broad query on the graph path.
        with connect(path, extension) as cx:
            reads = []
            def authorizer(action, table, column, database, trigger):
                if action == sqlite3.SQLITE_READ and table == 'vectors_graph':
                    reads.append((table, column))
                return sqlite3.SQLITE_OK
            cx.set_authorizer(authorizer)
            rows = cx.execute('SELECT rowid FROM vectors WHERE embedding MATCH ? '
                              'AND price>=0 AND k=10', (query,)).fetchall()
            assert len(rows) == 10 and reads
        # Scalar updates and rollback must use the current predicate snapshot.
        with connect(path, extension) as cx:
            sql = 'SELECT rowid FROM vectors WHERE embedding MATCH ? AND bucket=3 AND k=10'
            baseline = cx.execute(sql, (query,)).fetchall()
            cx.execute('BEGIN')
            cx.execute('UPDATE vectors SET bucket=3 WHERE rowid=313')
            assert cx.execute(sql, (query,)).fetchone()[0] == 313
            cx.rollback()
            assert cx.execute(sql, (query,)).fetchall() == baseline
    print('Filtered SQL, OFFSET, empty/NULL, cold route and rollback passed')


if __name__ == '__main__':
    run(Path(sys.argv[1]).resolve(strict=True))
