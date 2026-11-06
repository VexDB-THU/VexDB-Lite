"""Optional signature lifecycle through real SQLite SQL.

Run: python3 signature_rescue_regression.py /absolute/path/vexdb_lite.so
This verifies storage and transactions, not universal ANN recall or latency.
"""
import array,sqlite3,tempfile,math,sys,json,random,hashlib,time
from pathlib import Path
extension=Path(sys.argv[1]).resolve();result={};started=time.time()
def connect(path):
 c=sqlite3.connect(path);c.enable_load_extension(True);c.load_extension(str(extension));c.enable_load_extension(False);return c
rng=random.Random(83017);dim=512;n=1400
vectors=[array.array('f',[rng.gauss(0,1) for _ in range(dim)]).tobytes() for i in range(n)]
with tempfile.TemporaryDirectory(prefix='vexdb-signature-') as temp:
 p=Path(temp)/'test.db'
 c=connect(p);c.execute('PRAGMA journal_mode=WAL');c.execute('CREATE VIRTUAL TABLE v USING GRAPH_INDEX(embedding FLOAT[512],m=8,ef_construction=64,ef_search=128,brute_force_threshold=0,tag INTEGER)')
 c.executemany('INSERT INTO v(rowid,embedding,tag) VALUES(?,?,?)',[(i+1,x,i%3) for i,x in enumerate(vectors)]);c.commit()
 query='SELECT rowid,distance FROM v WHERE embedding MATCH ? AND k=10'
 base=c.execute(query,(vectors[16],)).fetchall();c.commit()
 c.execute("INSERT INTO v(v) VALUES('signature_build=83017')");c.commit()
 assert c.execute('SELECT count(*) FROM v_graph WHERE kind=7').fetchone()[0]==1
 codehash=lambda:hashlib.sha256(b''.join(r[0] for r in c.execute('SELECT data FROM v_graph WHERE kind in (7,8) ORDER BY kind,seg'))).hexdigest()
 original_hash=codehash();c.execute("INSERT INTO v(v) VALUES('signature_rescue=on')");c.commit()
 assert c.execute(query,(vectors[16],)).fetchall()==base
 # SAVEPOINT and full rollback restore both model and data.
 c.execute('SAVEPOINT change');c.execute('UPDATE v SET embedding=? WHERE rowid=17',(vectors[33],));c.execute('ROLLBACK TO change');c.execute('RELEASE change');c.commit()
 assert codehash()==original_hash;assert c.execute(query,(vectors[16],)).fetchall()==base
 c.execute('BEGIN IMMEDIATE');c.execute('DELETE FROM v WHERE rowid=17');c.execute('INSERT INTO v(rowid,embedding,tag) VALUES(2000,?,0)',(vectors[16],));c.rollback()
 assert codehash()==original_hash;assert c.execute(query,(vectors[16],)).fetchall()==base
 # DELETE then exact duplicate reattachment, committed append, reopen.
 c.execute('DELETE FROM v WHERE rowid=17');c.execute('INSERT INTO v(rowid,embedding,tag) VALUES(2000,?,0)',(vectors[16],));
 new=array.array('f',[100.0]*dim).tobytes();c.execute('INSERT INTO v(rowid,embedding,tag) VALUES(2001,?,1)',(new,));c.commit()
 assert c.execute(query,(vectors[16],)).fetchone()[0]==2000
 assert c.execute(query,(new,)).fetchone()[0]==2001
 c.close();c=connect(p)
 assert c.execute(query,(new,)).fetchone()[0]==2001
 assert c.execute(query,(vectors[16],)).fetchone()[0]==2000
 # Two-connection cookie invalidation and filter output.
 d=connect(p);d.execute('UPDATE v SET embedding=? WHERE rowid=2001',(vectors[44],));d.commit();d.close()
 assert c.execute(query,(vectors[44],)).fetchone()[0] in (45,2001)
 rows=c.execute(query+' AND tag=1',(vectors[44],)).fetchall();assert len(rows)==10
 assert all(c.execute('SELECT tag FROM v WHERE rowid=?',(r[0],)).fetchone()[0]==1 for r in rows)
 c.execute('BEGIN');c.execute("INSERT INTO v(v) VALUES('signature_rescue=off')");c.rollback()
 assert c.execute("SELECT value FROM v_config WHERE key='signature_rescue'").fetchone()[0]=='on'
 # Old optimization builds used kinds 5/6. Open the same model at those
 # locations, then append and reopen to verify migration keeps every code.
 legacy=Path(temp)/'legacy.db';saved=sqlite3.connect(legacy);c.backup(saved);saved.close()
 old=connect(legacy)
 old.execute('UPDATE v_graph SET kind=kind-2 WHERE kind IN (7,8)');old.commit();old.close()
 old=connect(legacy)
 assert old.execute(query,(vectors[16],)).fetchone()[0]==2000
 old.execute('INSERT INTO v(rowid,embedding,tag) VALUES(2002,?,0)',(vectors[66],));old.commit();old.close()
 old=connect(legacy)
 assert old.execute(query,(vectors[66],)).fetchone()[0] in (67,2002)
 assert old.execute('SELECT count(*) FROM v_graph WHERE kind=7').fetchone()[0]==1
 old.close()
 # Explicit opt-in on an unsupported index must fail visibly; it must not
 # silently run approximate PQ/cosine/IP without the requested protection.
 for name,options in [('unsupported_pq',",quantizer='pq',pq_m=64"),('unsupported_cos',",metric=cosine"),('unsupported_ip',",metric=ip")]:
  c.execute('CREATE VIRTUAL TABLE '+name+' USING GRAPH_INDEX(embedding FLOAT[512]'+options+')')
  try:c.execute("INSERT INTO "+name+"("+name+") VALUES('signature_rescue=on')")
  except sqlite3.DatabaseError as e:assert 'raw L2 graph' in str(e)
  else:raise AssertionError('unsupported protection accepted: '+name)
  c.execute("INSERT INTO "+name+"("+name+") VALUES('signature_rescue=off')")
 c.commit()
 result={'status':'pass','rows':n,'dim':dim,'tests':['default-off','persisted-model','mode','rollback','savepoint','delete-duplicate-reattach','append','restart','cross-connection-update','filter','legacy-signature-migration','unsupported-mode-rejected'],'seconds':time.time()-started,'extension_sha256':hashlib.sha256(extension.read_bytes()).hexdigest()}
 print(json.dumps(result,indent=2));c.close()
