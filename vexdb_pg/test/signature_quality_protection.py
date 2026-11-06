"""Private PostgreSQL16+ regression for exact fallback after auxiliary invalidation.
No system installation. Requires the matching pg_config and built module.
"""
import argparse, json, os, subprocess, tempfile
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('extension',type=Path);p.add_argument('--pg-config',default='pg_config');args=p.parse_args()
module=args.extension.resolve();bindir=Path(subprocess.check_output([args.pg_config,'--bindir'],text=True).strip())
sql_source=Path(__file__).resolve().parents[1]/'sql/vexdb_lite--1.2.sql'
with tempfile.TemporaryDirectory(prefix='vexdb-guard-pg-') as temp:
 r=Path(temp);data=r/'data';started=False;records=[]
 def cmd(label,argv,ok=True):
  z=subprocess.run(list(map(str,argv)),text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=180)
  records.append({'name':label,'code':z.returncode,'output':z.stdout})
  if ok and z.returncode:raise RuntimeError(label+'\n'+z.stdout)
  return z
 def sql(label,text,ok=True):
  f=r/(label+'.sql');f.write_text(text)
  return cmd(label,[bindir/'psql','-h',r,'-p','55444','-U',os.environ.get('USER','bench'),'-d','postgres','-Atq','-v','ON_ERROR_STOP=1','-f',f],ok)
 try:
  cmd('init',[bindir/'initdb','-D',data,'--no-locale','-A','trust'])
  with (data/'postgresql.conf').open('a') as f:f.write("\nlisten_addresses=''\nshared_preload_libraries='"+str(module)+"'\nshared_buffers='64MB'\nvexdb.vector_buffers=131072\nmaintenance_work_mem='2GB'\nautovacuum=off\n")
  cmd('start',[bindir/'pg_ctl','-D',data,'-l',r/'server.log','-o','-p 55444 -c unix_socket_directories='+str(r),'-w','start']);started=True
  source=sql_source.read_text();source='\n'.join(x for x in source.splitlines() if not x.startswith('\\echo') and not x.startswith('SELECT pg_catalog.pg_extension_config_dump('));source=source.replace('MODULE_PATHNAME',str(module))
  sql('schema',source)
  setup="""CREATE TABLE v(id int PRIMARY KEY,embedding floatvector(512),bucket int);
INSERT INTO v SELECT i, ('['||string_agg(sin(i::float8*d*.017)::text,',' ORDER BY d)||']')::floatvector,i%50 FROM generate_series(1,4096)i CROSS JOIN generate_series(1,512)d GROUP BY i;
CREATE INDEX v_ann ON v USING vexdb_graph(embedding floatvector_l2_ops) WITH(m=8,ef_construction=64,parallel_workers=0);
CREATE TEMP TABLE marker(x int);
"""
  sql('fixture',setup)
  query="SELECT id FROM v WHERE bucket<20 ORDER BY embedding <-> (SELECT embedding FROM v WHERE id=17) LIMIT 10"
  # Exact truth obtained with native full scan; then require the ANN plan itself
  # to execute the exact-on-missing route at ef1, rather than accepting raw ef1.
  check="""SET enable_indexscan=off;SET enable_bitmapscan=off;
CREATE TEMP TABLE expected AS %s;
SET enable_indexscan=on;SET enable_seqscan=off;SET enable_sort=off;SET vexdb.signature_rescue=on;SET vexdb.ef_search=1;
DO $$DECLARE got int[];want int[];info json;BEGIN
 SELECT array_agg(id ORDER BY id) INTO got FROM (%s)x;SELECT array_agg(id ORDER BY id) INTO want FROM expected;
 IF got<>want THEN RAISE EXCEPTION 'fallback quality mismatch %% %%',got,want;END IF;
 info:=vexdb_last_search_info()::json;
 IF info->>'route'<>'exact_signature_fallback' THEN RAISE EXCEPTION 'wrong route %%',info;END IF;
END$$;
SELECT vexdb_signature_status('v_ann');SELECT vexdb_last_search_info();
"""%(query,query)
  assert sql('missing',check).stdout.splitlines()[0]=='missing'
  sql('build',"SELECT vexdb_build_signature('v_ann',20261102);SELECT vexdb_signature_status('v_ann');")
  assert 'ready' in records[-1]['output']
  sql('abort',"BEGIN;INSERT INTO v SELECT 9000,embedding,1 FROM v WHERE id=17;ROLLBACK;SELECT vexdb_signature_status('v_ann');")
  assert 'write_invalid' in records[-1]['output'];sql('after-abort',check)
  sql('savepoint',"SELECT vexdb_build_signature('v_ann',20261102);BEGIN;SAVEPOINT s;UPDATE v SET embedding=(SELECT embedding FROM v WHERE id=33) WHERE id=17;ROLLBACK TO s;RELEASE s;COMMIT;SELECT vexdb_signature_status('v_ann');")
  assert 'write_invalid' in records[-1]['output'];sql('after-savepoint',check)
  sql('commit-insert',"INSERT INTO v SELECT 9000,embedding,1 FROM v WHERE id=33;")
  sql('after-insert',check)
  sql('delete-vacuum',"SELECT vexdb_build_signature('v_ann',20261102);DELETE FROM v WHERE id=17;VACUUM v;SELECT vexdb_signature_status('v_ann');")
  assert 'vacuum_invalid' in records[-1]['output']
  query=query.replace('id=17','id=33');check=check.replace('id=17','id=33');sql('after-vacuum',check)
  # Existing model regeneration and actual filtered retry widths.
  sql('rebuild',"SELECT vexdb_build_signature('v_ann',20261102);")
  retry=sql('retry',"SET enable_seqscan=off;SET enable_bitmapscan=off;SET enable_sort=off;SET vexdb.signature_rescue=on;SET vexdb.ef_search=16;SELECT id FROM v WHERE bucket=1 ORDER BY embedding <-> (SELECT embedding FROM v WHERE id=33) LIMIT 10;SELECT vexdb_last_search_info();")
  info=json.loads(retry.stdout.splitlines()[-1]);assert info['route']=='graph_rescue',info
  assert info['rounds']>1 and info['graph_ef']==16*2**(info['rounds']-1),info
  assert info['rescue_refinements']<=info['distance_calls']*.025,info
  sql('invalidate-for-timeout',"INSERT INTO v SELECT 9001,embedding,1 FROM v WHERE id=33;")
  timeout=sql('timeout',"SET enable_seqscan=off;SET enable_sort=off;SET vexdb.signature_rescue=on;SET statement_timeout='1ms';"+query+';',ok=False)
  assert timeout.returncode and 'statement timeout' in timeout.stdout,timeout.stdout
  sql('after-timeout',check)
  # Default OFF remains a graph route after the same invalidation.
  off=sql('off',"SET enable_seqscan=off;SET enable_sort=off;SET vexdb.signature_rescue=off;"+query+";SELECT vexdb_last_search_info();")
  assert json.loads(off.stdout.splitlines()[-1])['route']=='graph'
  # Two real connections: a writer invalidates the shared model after the
  # reader establishes a repeatable-read snapshot. Exact fallback must respect
  # that old snapshot, then a new request must see the committed row.
  sql('snapshot-build',"SELECT vexdb_build_signature('v_ann',20261102);")
  a=subprocess.Popen(list(map(str,[bindir/'psql','-h',r,'-p','55444','-U',os.environ.get('USER','bench'),'-d','postgres','-Atq','-v','ON_ERROR_STOP=1'])),stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
  try:
   prefix,rest=check.split('SET enable_indexscan=on;',1)
   a.stdin.write('BEGIN ISOLATION LEVEL REPEATABLE READ;'+prefix+'\\echo SNAPSHOT_READY\n');a.stdin.flush()
   output=[]
   for line in a.stdout:
    output.append(line)
    if line.strip()=='SNAPSHOT_READY':break
   else:raise RuntimeError('snapshot reader did not start: '+''.join(output))
   sql('snapshot-writer',"INSERT INTO v SELECT 9100,embedding,1 FROM v WHERE id=33;")
   a.stdin.write('SET enable_indexscan=on;'+rest+'COMMIT;\n');a.stdin.close()
   output.extend(a.stdout.readlines());a.wait(timeout=30)
   records.append({'name':'concurrent-snapshot','code':a.returncode,'output':''.join(output)})
   assert a.returncode==0,''.join(output)
   assert 'exact_signature_fallback' in ''.join(output),''.join(output)
  finally:
   if a.poll() is None:a.kill();a.wait()
  sql('after-snapshot-commit',check)
  print(json.dumps({'status':'pass','records':records,'retry_info':info},indent=2))
 except BaseException:
  print(json.dumps({'status':'failed','records':records,'server_log':(r/'server.log').read_text() if (r/'server.log').exists() else ''},indent=2),flush=True)
  raise
 finally:
  if started:cmd('stop',[bindir/'pg_ctl','-D',data,'-m','immediate','-w','stop'],ok=False)
