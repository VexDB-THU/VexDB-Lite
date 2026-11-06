"""Exercise bounded SQL construction, persistence and rollback with a loadable extension.

Usage: python3 bounded_build_regression.py /absolute/path/vexdb_lite.so
This checks storage and transaction invariants, not universal ANN recall or speed.
Directed approximate graphs can have unreachable nodes even with a wide search.
The independent benchmark must still pass its own 99.9% quality gate.
"""
import array
from contextlib import contextmanager
import hashlib
import math
from pathlib import Path
import sqlite3
import sys
import tempfile


@contextmanager
def connect(path, extension, readonly=False):
    cx = sqlite3.connect('file:' + str(path) + ('?mode=ro' if readonly else ''), uri=True)
    cx.enable_load_extension(True)
    cx.load_extension(str(extension))
    cx.enable_load_extension(False)
    try:
        with cx:
            yield cx
    finally:
        # sqlite3.Connection's context manager commits/rolls back but does not
        # close it. Close the writer before measuring read-only file changes,
        # otherwise its later destruction can checkpoint WAL during that check.
        cx.close()


def run(extension):
    vectors = [array.array('f', (math.sin(i * (j + 1) * .017) for j in range(64))).tobytes()
               for i in range(5000)]
    with tempfile.TemporaryDirectory(prefix='vexdb-bounded-build-') as directory:
        path = Path(directory) / 'test.db'
        with connect(path, extension) as cx:
            cx.execute('PRAGMA journal_mode=WAL')
            cx.execute('CREATE VIRTUAL TABLE configured USING GRAPH_INDEX('
                       'embedding FLOAT[2],graph_memory_limit=262144,'
                       'graph_build_memory_limit=2097152)')
            assert cx.execute("SELECT value FROM configured_config WHERE key='graph_build_memory_limit'").fetchone()[0] == '2097152'
            cx.execute('DROP TABLE configured')
            cx.execute('CREATE VIRTUAL TABLE vectors USING GRAPH_INDEX('
                       'embedding FLOAT[64],tag INTEGER,metric=l2,m=8,'
                       'ef_construction=64,ef_search=8192,graph_memory_limit=262144)')
            cx.execute('BEGIN IMMEDIATE')
            cx.executemany('INSERT INTO vectors(rowid,embedding,tag) VALUES(?,?,?)',
                           ((i + 1, vector, i % 7) for i, vector in enumerate(vectors)))
            cx.commit()  # Forces the bounded memory -> disk construction path.
            assert cx.execute('SELECT count(*) FROM vectors_graph WHERE kind>0').fetchone()[0] > 0
            assert cx.execute('PRAGMA quick_check').fetchone()[0] == 'ok'
            cx.execute('PRAGMA wal_checkpoint(TRUNCATE)')
            assert cx.execute("SELECT value FROM vectors_config WHERE key='graph_build_memory_limit'").fetchone()[0] == '0'
            graph = cx.execute('SELECT kind,seg,data FROM vectors_graph ORDER BY kind,seg').fetchall()
            cx.execute("INSERT INTO vectors(vectors) VALUES('graph_build_memory_limit=2097152')")
            cx.commit()
            assert cx.execute('SELECT kind,seg,data FROM vectors_graph ORDER BY kind,seg').fetchall() == graph
            assert cx.execute("SELECT value FROM vectors_config WHERE key='graph_memory_limit'").fetchone()[0] == '262144'
            cx.execute('BEGIN IMMEDIATE')
            cx.execute("INSERT INTO vectors(vectors) VALUES('graph_build_memory_limit=4194304')")
            cx.rollback()
            assert cx.execute("SELECT value FROM vectors_config WHERE key='graph_build_memory_limit'").fetchone()[0] == '2097152'
            cx.execute('BEGIN IMMEDIATE')
            cx.execute('SAVEPOINT budget')
            cx.execute("INSERT INTO vectors(vectors) VALUES('graph_build_memory_limit=4194304')")
            cx.execute('ROLLBACK TO budget')
            cx.execute('RELEASE budget')
            cx.commit()
            assert cx.execute("SELECT value FROM vectors_config WHERE key='graph_build_memory_limit'").fetchone()[0] == '2097152'
            for bad in ['-1', '1k', '9223372036854775808']:
                try:
                    cx.execute("INSERT INTO vectors(vectors) VALUES(?)", ('graph_build_memory_limit=' + bad,))
                    raise AssertionError('invalid budget accepted')
                except sqlite3.OperationalError:
                    cx.rollback()
            assert cx.execute('SELECT kind,seg,data FROM vectors_graph ORDER BY kind,seg').fetchall() == graph
            baselines = {}
            for qi in [17, 59, 103, 997, 4093]:
                for predicate in ['', ' AND tag=3']:
                    baselines[qi, predicate] = cx.execute(
                        'SELECT rowid FROM vectors WHERE embedding MATCH ? AND k=10' + predicate,
                        (vectors[qi],)).fetchall()
        before = hashlib.sha256(path.read_bytes()).hexdigest()
        with connect(path, extension, readonly=True) as cx:
            assert cx.execute("SELECT value FROM vectors_config WHERE key='graph_build_memory_limit'").fetchone()[0] == '2097152'
            for qi in [17, 59, 103, 997, 4093]:
                for predicate in ['', ' AND tag=3']:
                    rows = cx.execute(
                        'SELECT rowid FROM vectors WHERE embedding MATCH ? AND k=10' + predicate,
                        (vectors[qi],)).fetchall()
                    assert len(rows) == 10 and len({x[0] for x in rows}) == 10
                    assert rows == baselines[qi, predicate]
                    for (rowid,) in rows:
                        vector = cx.execute('SELECT embedding FROM vectors WHERE rowid=?',
                                            (rowid,)).fetchone()[0]
                        assert vector == vectors[rowid - 1]
                        assert not predicate or (rowid - 1) % 7 == 3
        assert hashlib.sha256(path.read_bytes()).hexdigest() == before
        with connect(path, extension) as cx:
            query = 'SELECT rowid FROM vectors WHERE embedding MATCH ? AND k=10'
            expected = cx.execute(query, (vectors[103],)).fetchall()
            cx.execute('BEGIN IMMEDIATE')
            cx.execute('DELETE FROM vectors WHERE rowid=104')
            cx.execute('INSERT INTO vectors(rowid,embedding,tag) VALUES(6000,?,3)', (vectors[103],))
            cx.rollback()
            assert cx.execute(query, (vectors[103],)).fetchall() == expected
            cx.execute('CREATE TABLE audit(value INTEGER)')
            cx.execute('INSERT INTO audit VALUES(1)')
            cx.commit()
            # An idle query must not keep a BLOB handle/read transaction alive.
            with sqlite3.connect(path) as writer:
                writer.execute('INSERT INTO audit VALUES(2)')
                writer.commit()
                busy, _, _ = writer.execute('PRAGMA wal_checkpoint(TRUNCATE)').fetchone()
                assert busy == 0
        print('Bounded SQL build: persisted segments, read-only reopen, '
              'correct vectors, filter, stable results, rollback and WAL release passed')


if __name__ == '__main__':
    run(Path(sys.argv[1]).resolve(strict=True))
