"""Exercise repeated parameterized ANN rescans and filtered retries in private PG.

A LATERAL scan reuses one index scan descriptor across different query vectors.
The test asserts a native index plan and checks against exact filtered SQL.
"""
import argparse
import array
import math
import json
import os
import pwd
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('extension', type=Path)
    parser.add_argument('--pg-config', default='pg_config')
    parser.add_argument('--probe', type=Path)
    parser.add_argument('--rows', type=int, default=2048)
    parser.add_argument('--dimension', type=int, default=32)
    parser.add_argument('--probe-count', type=int, default=32)
    parser.add_argument('--ef', type=int, default=2048)
    args = parser.parse_args()
    if not (1 <= args.dimension <= 65535 and 1 <= args.ef <= 65535 and
            args.rows >= args.probe_count + 10 and args.probe_count > 0):
        parser.error('invalid fixture dimensions, rows, probes or ef')
    module = args.extension.resolve(strict=True)
    bindir = Path(subprocess.check_output([args.pg_config, '--bindir'], text=True).strip())
    schema = Path(__file__).resolve().parents[1] / 'sql/vexdb_lite--1.2.sql'
    with tempfile.TemporaryDirectory(prefix='vexdb-rescan-') as directory:
        root = Path(directory)
        data = root / 'data'
        started = False
        records = []

        def run(label, command, check=True):
            result = subprocess.run(list(map(str, command)), text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    timeout=180)
            records.append(dict(name=label, code=result.returncode, output=result.stdout))
            if check and result.returncode:
                raise RuntimeError(label + '\n' + result.stdout)
            return result.stdout

        def sql(label, query):
            path = root / (label + '.sql')
            path.write_text(query)
            return run(label, [bindir / 'psql', '-h', root, '-p', '55446', '-U',
                               pwd.getpwuid(os.geteuid()).pw_name, '-d', 'postgres',
                               '-Atq', '-v', 'ON_ERROR_STOP=1', '-f', path])

        query = """SELECT q.id AS qid, array_agg(s.id ORDER BY s.id) AS ids
FROM probes q CROSS JOIN LATERAL (
 SELECT v.id FROM v WHERE v.bucket=q.bucket
 ORDER BY v.embedding <-> q.embedding LIMIT 10
) s GROUP BY q.id ORDER BY q.id"""
        try:
            run('init', [bindir / 'initdb', '-D', data, '--no-locale', '-A', 'trust'])
            with (data / 'postgresql.conf').open('a') as f:
                f.write("\nlisten_addresses=''\nshared_preload_libraries='" + str(module) +
                        "'\nshared_buffers='64MB'\nvexdb.vector_buffers=131072\n"
                        "maintenance_work_mem='1GB'\nautovacuum=off\n")
            run('start', [bindir / 'pg_ctl', '-D', data, '-l', root / 'server.log',
                          '-o', '-p 55446 -c unix_socket_directories=' + str(root), '-w', 'start'])
            started = True
            source = '\n'.join(line for line in schema.read_text().splitlines()
                               if not line.startswith('\\echo') and
                               not line.startswith('SELECT pg_catalog.pg_extension_config_dump('))
            sql('schema', source.replace('MODULE_PATHNAME', str(module)))
            sql('fixture', f"""CREATE TABLE v(id int PRIMARY KEY, embedding floatvector({args.dimension}), bucket int);
INSERT INTO v SELECT i, ('['||string_agg(sin(i::float8*d*.017)::text,',' ORDER BY d)||']')::floatvector,i%17
 FROM generate_series(1,{args.rows})i CROSS JOIN generate_series(1,{args.dimension})d GROUP BY i;
CREATE INDEX v_ann ON v USING vexdb_graph(embedding floatvector_l2_ops)
 WITH(m=8,ef_construction=64,parallel_workers=0);
CREATE TABLE probes AS SELECT * FROM v WHERE id BETWEEN 11 AND {10+args.probe_count};
ANALYZE v; ANALYZE probes;
SELECT vexdb_build_signature('v_ann',20261030);
""")
            # High ef is deliberate here: this is a descriptor-lifecycle test,
            # not a benchmark or a way of meeting the product recall target.
            for mode in ['off', 'on']:
                settings = 'SET vexdb.signature_rescue=' + mode + ';SET vexdb.ef_search='+str(args.ef)+';'
                plan = json.loads(sql('plan-' + mode, settings +
                    'SET enable_seqscan=off;SET enable_bitmapscan=off;SET enable_sort=off;'
                    'SET enable_memoize=off;EXPLAIN (FORMAT JSON) ' + query))
                def index_names(value):
                    if isinstance(value, dict):
                        if 'Index Name' in value:
                            yield value['Index Name']
                        for child in value.values():
                            yield from index_names(child)
                    elif isinstance(value, list):
                        for child in value:
                            yield from index_names(child)
                assert 'v_ann' in list(index_names(plan)), plan
                result = sql('rescan-' + mode, settings + f"""
SET enable_indexscan=off;SET enable_bitmapscan=off;
CREATE TEMP TABLE expected AS """ + query + ";" + """
SET enable_indexscan=on;SET enable_seqscan=off;SET enable_sort=off;SET enable_memoize=off;
CREATE TEMP TABLE actual AS """ + query + ";" + f"""
DO $$BEGIN
 IF (SELECT count(*) FROM actual)<>{args.probe_count} OR EXISTS(
   (SELECT * FROM actual EXCEPT SELECT * FROM expected)
   UNION ALL (SELECT * FROM expected EXCEPT SELECT * FROM actual))
 THEN RAISE EXCEPTION 'parameterized rescan differs from exact filtered SQL';END IF;
END$$;SELECT count(*) FROM actual;
""")
                assert result.strip() == str(args.probe_count), result
            if args.probe:
                sql('body', "ALTER TABLE v ADD COLUMN body text; UPDATE v SET body=repeat('proxy-payload-'||id||' ',32);")
                vectors = root / 'queries.f32'
                array.array('f', [math.sin(i*d*.017) for i in [17,31] for d in range(1,args.dimension+1)]).tofile(vectors.open('wb'))
                config = dict(engine='pg', table='v', dim=args.dimension, ef=args.ef,
                    signature_rescue='off', force_generic=1, prefer_index=1,
                    task='quality', mode='ann', query_count=2, queries=vectors,
                    conninfo='host='+str(root)+' port=55446 dbname=postgres user='+pwd.getpwuid(os.geteuid()).pw_name,
                    out=root/'native-plan')
                path = root/'probe.cfg'
                path.write_text(''.join(k+'='+str(v)+'\n' for k,v in config.items()))
                run('native-probe', [args.probe.resolve(), path])
                plan = (root/'native-plan/plan.txt').read_text()
                assert 'v_ann' in plan and '$1' in plan, plan
            print(json.dumps(dict(status='pass', rows=args.rows, dimension=args.dimension, ef=args.ef, probes=args.probe_count, modes=['off', 'on']), indent=2))
        except BaseException:
            print(json.dumps(dict(status='failed', records=records,
                                  server_log=(root / 'server.log').read_text()
                                  if (root / 'server.log').exists() else ''), indent=2), flush=True)
            raise
        finally:
            if started:
                run('stop', [bindir / 'pg_ctl', '-D', data, '-m', 'immediate', '-w', 'stop'], False)


if __name__ == '__main__':
    main()
