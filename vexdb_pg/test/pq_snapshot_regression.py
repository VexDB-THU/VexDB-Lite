"""PQ refinement must use the executor snapshot across committed/aborted writes."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import uuid

p = argparse.ArgumentParser()
p.add_argument('--psql', default='psql')
p.add_argument('--host', required=True)
p.add_argument('--port', required=True)
p.add_argument('--user', default='postgres')
p.add_argument('--database', default='test')
p.add_argument('--out', type=Path)
a = p.parse_args()
out = a.out or Path(tempfile.mkdtemp(prefix='vexdb-pq-snapshot-'))
if a.out:
    out.mkdir(parents=True, exist_ok=False)
base = [a.psql, '-X', '-h', a.host, '-p', a.port, '-U', a.user,
        '-d', a.database, '-Atq', '-v', 'ON_ERROR_STOP=1']
state = {'status': 'running', 'started': time.time(), 'cases': []}
schema = 'vex_pq_snapshot_test_' + uuid.uuid4().hex[:12]
created_schema = False
state['schema'] = schema


def save():
    (out / 'state.json').write_text(json.dumps(state, indent=2))


class Session:
    def __init__(self):
        self.proc = subprocess.Popen(base, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True, bufsize=1,
                                     env=dict(os.environ, PGAPPNAME='vex_pq_snapshot_regression'))
        self.ordinal = 0

    def sql(self, text):
        self.ordinal += 1
        marker = 'VEX_SNAPSHOT_DONE_%s_%s' % (self.proc.pid, self.ordinal)
        self.proc.stdin.write(text + ';\n\\echo ' + marker + '\n')
        self.proc.stdin.flush()
        lines = []
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError('private snapshot client exited: ' + '\n'.join(lines))
            if line.strip() == marker:
                return '\n'.join(lines).strip()
            lines.append(line.rstrip())

    def close(self):
        if self.proc.poll() is None:
            try:
                self.proc.stdin.write('ROLLBACK;\n\\q\n')
                self.proc.stdin.flush()
                self.proc.wait(timeout=5)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                self.proc.kill()
                self.proc.wait()


settings = ("SET statement_timeout='20s';SET max_parallel_workers_per_gather=0;"
            "SET vexdb.ef_search=1024;SET vexdb.signature_rescue=off;")
exact = 'SET enable_indexscan=off;SET enable_indexonlyscan=off;SET enable_bitmapscan=off;'
ann = 'SET enable_indexscan=on;SET enable_seqscan=off;SET enable_bitmapscan=off;'
reader = writer = None
save()


def check(name, expected, actual):
    passed = (len(actual) == 10 and len(set(actual)) == 10 and set(actual) == set(expected))
    state['cases'].append(dict(name=name, expected=expected, actual=actual, passed=passed))
    save()
    assert passed, (name, expected, actual)


try:
    writer, reader = Session(), Session()
    writer.sql(settings)
    reader.sql(settings)
    writer.sql('CREATE SCHEMA ' + schema)
    created_schema = True
    writer.sql('CREATE TABLE ' + schema +
               '.items(id integer PRIMARY KEY,v floatvector(32));INSERT INTO ' + schema +
               '.items SELECT i,(ARRAY(SELECT sin(i*j*.017)::real FROM generate_series(1,32) j))'
               '::floatvector(32) FROM generate_series(1,512) i;CREATE INDEX vector_ann ON ' +
               schema + ".items USING vexdb_graph(v floatvector_l2_ops) WITH("
               "m=16,ef_construction=200,parallel_workers=0,quantizer='pq',pq_m=4)")
    vector = writer.sql('SELECT v::text FROM ' + schema + '.items WHERE id=17')
    assert vector and "'" not in vector
    query = 'SELECT id FROM ' + schema + ".items ORDER BY v <-> '" + vector + \
            "'::floatvector LIMIT 10"
    wrapped = 'SELECT json_agg(x.id) FROM (' + query + ') x'
    plan = reader.sql(settings + ann + 'EXPLAIN (FORMAT JSON) ' + query)
    assert 'vector_ann' in plan, plan
    (out / 'plan.json').write_text(plan)
    reader.sql('BEGIN ISOLATION LEVEL REPEATABLE READ')
    expected = json.loads(reader.sql(exact + wrapped))
    check('before_write', expected, json.loads(reader.sql(ann + wrapped)))
    writer.sql('UPDATE ' + schema + '.items SET v=(ARRAY(SELECT '
               '(100+sin(17*j*.017))::real FROM generate_series(1,32) j))'
               '::floatvector(32) WHERE id=17')
    check('old_snapshot_after_commit', expected, json.loads(reader.sql(ann + wrapped)))
    reader.sql('ROLLBACK')
    new_expected = json.loads(writer.sql(exact + wrapped))
    check('new_snapshot_after_commit', new_expected, json.loads(writer.sql(ann + wrapped)))
    reader.sql('BEGIN ISOLATION LEVEL REPEATABLE READ')
    expected = json.loads(reader.sql(exact + wrapped))
    writer.sql('BEGIN;UPDATE ' + schema + '.items SET v=(ARRAY(SELECT '
               '(200+sin(i*.037))::real FROM generate_series(1,32) i))'
               '::floatvector(32) WHERE id=' + str(expected[0]))
    check('old_snapshot_uncommitted_writer', expected, json.loads(reader.sql(ann + wrapped)))
    writer.sql('ROLLBACK')
    check('old_snapshot_after_rollback', expected, json.loads(reader.sql(ann + wrapped)))
    reader.sql('ROLLBACK')
    state.update(status='passed', ended=time.time())
    save()
    print('PASS PQ scan snapshot: 5 real SQL checks, committed and rolled-back writes; ' + str(out))
except BaseException as e:
    state.update(status='failed', error=repr(e), ended=time.time())
    save()
    raise
finally:
    for session in (reader, writer):
        if session:
            session.close()
    if created_schema:
        subprocess.run(base + ['-c', 'DROP SCHEMA IF EXISTS ' + schema + ' CASCADE'],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30, check=True)
