"""Real SQL: PQ refinement retains TOAST, expression and vacuum semantics."""
import argparse,json,subprocess

p=argparse.ArgumentParser();p.add_argument('--psql',default='psql');p.add_argument('--host',required=True);p.add_argument('--port',required=True);p.add_argument('--user',default='bench');p.add_argument('--database',default='postgres');a=p.parse_args()
base=[a.psql,'-h',a.host,'-p',a.port,'-U',a.user,'-d',a.database,'-Atq','-v','ON_ERROR_STOP=1']
def sql(text):
    z=subprocess.run(base+['-c',text],text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=180)
    if z.returncode:raise RuntimeError(z.stderr)
    return z.stdout.strip()

schema='vex_refinement_io_test'
sql('CREATE SCHEMA '+schema)
try:
    sql("CREATE FUNCTION "+schema+".identity_vector(v floatvector) RETURNS floatvector LANGUAGE plpgsql IMMUTABLE STRICT AS $$BEGIN RETURN v; END$$;"
        "CREATE TABLE "+schema+".items(id integer PRIMARY KEY,embedding floatvector(768),body text);"
        "INSERT INTO "+schema+".items SELECT i,(ARRAY(SELECT (sin(i*.173+j*.019)+cos(i*.037-j*.023))::real FROM generate_series(1,768) j))::floatvector(768),repeat('row-'||i||' ',80) FROM generate_series(1,512) i ORDER BY md5(i::text);"
        "ALTER TABLE "+schema+".items ALTER COLUMN embedding SET STORAGE EXTERNAL;"
        "UPDATE "+schema+".items SET embedding=embedding;"
        "CREATE INDEX plain_refine ON "+schema+".items USING vexdb_graph(embedding floatvector_l2_ops) WITH(quantizer='pq',pq_m=96,m=16,ef_construction=200);"
        "CREATE INDEX expression_refine ON "+schema+".items USING vexdb_graph(("+schema+".identity_vector(embedding)::floatvector(768)) floatvector_l2_ops) WITH(quantizer='pq',pq_m=96,m=16,ef_construction=200);")
    settings="SET max_parallel_workers_per_gather=0;SET vexdb.ef_search=1024;"
    checked=0
    for phase in ('before','after_vacuum'):
        if phase=='after_vacuum':
            sql('DELETE FROM '+schema+'.items WHERE id%7=0')
            sql('VACUUM (ANALYZE) '+schema+'.items')
        for expression,index in [('embedding','plain_refine'),(schema+'.identity_vector(embedding)::floatvector(768)','expression_refine')]:
            for ident in (3,17,255,509):
                vector=sql('SELECT embedding::text FROM '+schema+'.items WHERE id='+str(ident))
                assert vector and "'" not in vector
                query='SELECT id,body FROM '+schema+".items ORDER BY "+expression+" <-> '"+vector+"'::floatvector LIMIT 10"
                plan=sql(settings+'SET enable_seqscan=off;SET enable_bitmapscan=off;EXPLAIN (FORMAT JSON) '+query)
                assert index in plan,(phase,index,plan)
                actual=sql(settings+'SET enable_seqscan=off;SET enable_bitmapscan=off;SELECT json_agg(x) FROM ('+query+') x')
                expected=sql(settings+'SET enable_indexscan=off;SET enable_indexonlyscan=off;SET enable_bitmapscan=off;SELECT json_agg(x) FROM ('+query+') x')
                assert json.loads(actual)==json.loads(expected),(phase,index,ident,actual,expected)
                checked+=1
    print('PASS refinement IO order: %s actual SQL top-10/body comparisons; external TOAST vectors, plain/expression indexes, deletes and VACUUM'%checked)
finally:
    sql('DROP SCHEMA '+schema+' CASCADE')
