"""Real SQL: filtered exact routing and absent auxiliary quality protection.

Independent exact answers use Python float64 arithmetic and ordinary SQLite
metadata predicates. This is a lifecycle/semantic regression, not ANN QPS.
"""
import array, json, math, random, sqlite3, sys, tempfile
from pathlib import Path
extension=Path(sys.argv[1]).resolve();rng=random.Random(2026110204);dim=64;n=4096
vectors=[array.array('f',[rng.gauss(0,1) for _ in range(dim)]) for _ in range(n)]

def connect(p):
 c=sqlite3.connect(p);c.enable_load_extension(True);c.load_extension(str(extension));c.enable_load_extension(False)
 c.create_collation('Case, "fold',lambda a,b:(a.casefold()>b.casefold())-(a.casefold()<b.casefold()))
 return c
with tempfile.TemporaryDirectory(prefix='vexdb-filter-quality-') as d:
 p=Path(d)/'test.db';c=connect(p)
 c.execute('PRAGMA journal_mode=WAL')
 c.execute('CREATE VIRTUAL TABLE v USING GRAPH_INDEX(embedding FLOAT[64],bucket INTEGER,label TEXT,m=8,ef_construction=64,ef_search=40,brute_force_threshold=64)')
 c.execute('CREATE TABLE meta(id INTEGER PRIMARY KEY,bucket INTEGER,label TEXT)')
 rows=[(i+1,v.tobytes(),999 if i<3 else None if i%71==0 else i%50,'MiXeD' if i%2 else 'mixed') for i,v in enumerate(vectors)]
 c.executemany('INSERT INTO v(rowid,embedding,bucket,label) VALUES(?,?,?,?)',rows)
 c.executemany('INSERT INTO meta VALUES(?,?,?)',[(i,b,l) for i,_,b,l in rows]);c.commit()
 q=array.array('f',[rng.gauss(0,1) for _ in range(dim)])
 def expected(pred='',params=(),offset=0,k=10):
  allowed=[i for i, in c.execute('SELECT id FROM meta'+(' WHERE '+pred if pred else ''),params)]
  return [i for _,i in sorted((sum((float(x)-float(y))**2 for x,y in zip(q,vectors[i-1])),i) for i in allowed)[offset:offset+k]]
 def search(pred='',params=(),limit=False,offset=0,k=10):
  sql='SELECT rowid FROM v WHERE embedding MATCH ?'+('' if limit else ' AND k='+str(k))
  if pred:sql+=' AND '+pred
  if limit:sql+=' ORDER BY distance LIMIT '+str(k)+' OFFSET '+str(offset)
  return [i for i, in c.execute(sql,(q.tobytes(),*params))]
 def info():return json.loads(c.execute('SELECT vexdb_search_info(v) FROM v LIMIT 1').fetchone()[0])
 # Cost routing is opt-in; the default keeps the existing graph policy.
 assert search('bucket=?',(12,))==expected('bucket=?',(12,))
 assert info()['route']=='graph',info()
 c.execute("INSERT INTO v(v) VALUES('filtered_search_mode=auto')");c.commit()
 # >64 eligible rows used to compensate graph width by 10x; now exact set.
 cases=[('bucket=?',(12,)),('bucket=?',('12',)),('bucket>=? AND bucket<=?',(12,12)),
        ('bucket=? AND label=? COLLATE NOCASE',(12,'MIXED')),
        ('bucket=? AND label=? COLLATE "Case, ""fold"',(12,'MIXED'))]
 for pred,args in cases:
  assert search(pred,args)==expected(pred,args),(pred,search(pred,args),expected(pred,args))
  assert info()['route']=='exact_filtered',info()
 # Empty, fewer than k, NULL equality and OFFSET on small/exact paths.
 assert search('bucket=?',(None,))==[]
 assert search('bucket=?',(99,))==[]
 assert search('bucket=?',(999,))==expected('bucket=?',(999,))
 assert len(search('bucket=?',(999,)))==3
 assert search('bucket=?',(12,),True,3,5)==expected('bucket=?',(12,),3,5)
 assert search('bucket=?',(12,),True,3,20)==expected('bucket=?',(12,),3,20)
 # Explicit rescue with no model must not silently use an unqualified narrow graph.
 c.execute("INSERT INTO v(v) VALUES('ef_search=1')")
 c.execute("INSERT INTO v(v) VALUES('signature_rescue=on')");c.commit()
 assert search()==expected();assert info()['route']=='exact_signature_fallback',info()
 c.execute('SAVEPOINT s');c.execute('UPDATE v SET embedding=? WHERE rowid=2',(q.tobytes(),));c.execute('ROLLBACK TO s');c.execute('RELEASE s');c.commit()
 assert search()==expected()
 # Snapshot and cross-connection scalar writes must keep whitelist semantics.
 other=connect(p);other.execute('UPDATE v SET bucket=12 WHERE rowid=1');other.commit();other.close()
 c.execute('UPDATE meta SET bucket=12 WHERE id=1');c.commit()
 assert search('bucket=?',(12,))==expected('bucket=?',(12,))
 # WAL reader keeps its snapshot while another connection commits a vector
 # update. A fresh transaction then sees the new nearest row and cookie.
 old_expected=expected();c.execute('BEGIN');assert search()==old_expected
 other=connect(p);other.execute('UPDATE v SET embedding=? WHERE rowid=1',(q.tobytes(),));other.commit();other.close()
 assert search()==old_expected;c.commit();vectors[0]=q
 assert search()==expected();assert search()[0]==1
 # Interrupt an exact fallback then retry; partial top-k must never escape.
 steps=[0]
 def interrupt():steps[0]+=1;return int(steps[0]>2)
 c.set_progress_handler(interrupt,1)
 try:search();raise AssertionError('cancellation was not exercised')
 except sqlite3.OperationalError as e:assert 'interrupt' in str(e).lower(),str(e)
 finally:c.set_progress_handler(None,0)
 assert search()==expected()
 c.close();c=connect(p);assert search()==expected();assert info()['route']=='exact_signature_fallback'
 # Repeated vectors share graph nodes. Cost routing must count those nodes,
 # not charge one graph distance for every duplicate row.
 c.execute('CREATE VIRTUAL TABLE copies USING GRAPH_INDEX(embedding FLOAT[64],bucket INTEGER,m=8,ef_construction=64,ef_search=40,brute_force_threshold=64)')
 c.execute("INSERT INTO copies(copies) VALUES('filtered_search_mode=auto')")
 c.executemany('INSERT INTO copies(rowid,embedding,bucket) VALUES(?,?,?)',[(i+1,vectors[i%4].tobytes(),i%50) for i in range(n)])
 c.execute('SELECT rowid FROM copies WHERE embedding MATCH ? AND k=1',(q.tobytes(),)).fetchall();c.commit()
 import struct
 nodes=struct.unpack_from('<Q',c.execute('SELECT data FROM copies_graph WHERE kind=0 AND seg=0').fetchone()[0],24)[0]
 assert 4<=nodes<n//16,nodes
 result=c.execute('SELECT rowid,distance FROM copies WHERE embedding MATCH ? AND k=10 AND bucket=12',(q.tobytes(),)).fetchall()
 assert len(result)==10 and all((i-1)%50==12 and distance==0 for i,distance in result),result
 duplicate_info=json.loads(c.execute('SELECT vexdb_search_info(copies) FROM copies LIMIT 1').fetchone()[0])
 assert duplicate_info['route']=='graph',duplicate_info
 # Append after trimming: allocation capacity must not be mistaken for the
 # initialized/published node count. Also exercise an auxiliary append/reopen.
 c.execute("INSERT INTO copies(copies) VALUES('signature_build=20261102')")
 c.execute("INSERT INTO copies(copies) VALUES('signature_rescue=on')");c.commit()
 added=array.array('f',[100.0]*dim).tobytes()
 c.execute('INSERT INTO copies(rowid,embedding,bucket) VALUES(9000,?,12)',(added,));c.commit()
 assert c.execute('SELECT rowid FROM copies WHERE embedding MATCH ? AND k=1',(added,)).fetchone()[0]==9000
 c.close();c=connect(p)
 assert c.execute('SELECT rowid FROM copies WHERE embedding MATCH ? AND k=1',(added,)).fetchone()[0]==9000
 print(json.dumps({'status':'pass','rows':n,'dim':dim,'cases':len(cases),'checks':['predicate-affinity','NULL','empty','collation-NOCASE','collation-quoted-name','OFFSET-exact','missing-model-exact','savepoint','cross-connection-write','concurrent-WAL-snapshot','cancel-retry','reopen'],'duplicate_nodes':nodes,'duplicate_info':duplicate_info,'info':info()},indent=2))
 c.close()
