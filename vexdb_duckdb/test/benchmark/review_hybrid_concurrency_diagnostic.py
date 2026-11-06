"""Read-only diagnostic review; quality is recomputed from returned IDs."""
import argparse,csv,hashlib,json,re,statistics
from collections import Counter
from pathlib import Path

def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def pct(xs,p):return sorted(xs)[max(0,int(len(xs)*p)-1)]
def main(run):
 protocol=json.loads((run/'protocol.json').read_text());state=json.loads((run/'state.json').read_text());assert state['status']=='complete'
 assert all(sha(p)==v for p,v in protocol['sha256'].items());assert state['ended']<=protocol['deadline_epoch']
 root=run.parents[3];h=root/'build/product-tests/hybrid-recall-20261029'
 truths=[json.loads((h/'performance-preparation/truth.json').read_text()),json.loads((h/'raw128-control/performance-preparation/truth.json').read_text())]
 points=[];resource=[];locks={}
 for w in protocol['works']:
  truth=next(t[w['source']] for t in truths if w['source'] in t)
  for p in csv.DictReader((run/w['name']/'points.csv').open()):
   rows=list(csv.DictReader((run/w['name']/(p['point']+'.timing.csv')).open()));assert len(rows)==int(p['calls'])
   hits=0;mix=Counter();wall=[];cpus=[];reads=[];executions=[]
   for r in rows:
    q=int(r['query']);ids=list(map(int,r['ids'].split()));expected=truth[w['profile']+':'+str(q)]['ids']
    assert len(ids)==len(set(ids))==len(expected)==10
    hits+=len(set(ids)&set(expected));mix[int(r['client']),q]+=1
    ns=int(r['end_ns'])-int(r['begin_ns']);assert 0<int(r['thread_cpu_ns'])<=ns*1.05
    assert int(r['execute_ns'])+int(r['read_ns'])<=ns
    wall.append(ns/1e6);cpus.append(int(r['thread_cpu_ns'])/1e6);reads.append(int(r['read_ns'])/1e6);executions.append(int(r['execute_ns'])/1e6)
   for client in {c for c,q in mix}:assert len({mix[client,q] for q in range(8)})==1
   item=dict(workload=w['name'],**p,recall=hits/(10*len(rows)),cpu_p50_ms=pct(cpus,.5),cpu_p95_ms=pct(cpus,.95),read_p95_ms=pct(reads,.95),read_wall_fraction=sum(reads)/sum(wall),execute_wall_fraction=sum(executions)/sum(wall),cpu_cores=float(p['process_cpu_ms'])*1e6/int(p['wall_ns']))
   assert item['recall']>=.999
   points.append(item)
  resources=[json.loads(s) for s in (run/(w['name']+'.resources.jsonl')).read_text().splitlines()]
  def counter(r,k):
   m=re.search(re.escape(k)+r':\s+(\d+)',r['vm']);return int(m.group(1)) if m else None
  resource.append(dict(workload=w['name'],system_swapins_delta=counter(resources[-1],'Swapins')-counter(resources[0],'Swapins'),system_swapouts_delta=counter(resources[-1],'Swapouts')-counter(resources[0],'Swapouts'),note='System-wide cumulative counter changes across whole job, not process attribution.'))
  lines=(run/(w['name']+'.sample.txt')).read_text().splitlines();groups=Counter()
  for i,line in enumerate(lines):
   m=re.search(r'(\d+) __psynch_mutexwait ',line)
   if not m:continue
   parent=' '.join(lines[max(0,i-10):i]);n=int(m.group(1))
   label=next((label for token,label in [('StandardBufferManager::Pin','buffer-pin'),('StandardBufferManager::Unpin','buffer-unpin'),('DuckTransactionManager::StartTransaction','transaction-start'),('VexSearchQueue','admission'),('ReverseGraph','reverse-cache'),('FixedSizeAllocator','graph-allocator'),('GetVectorScanType','column-scan-state')] if token in parent),'other')
   groups[label]+=n
  locks[w['name']]=dict(groups)
 out=run.parent/'diagnostic-review';out.mkdir(exist_ok=False)
 with (out/'points.csv').open('w') as f:wr=csv.DictWriter(f,fieldnames=points[0]);wr.writeheader();wr.writerows(points)
 (out/'review.json').write_text(json.dumps(dict(points=len(points),calls=sum(int(p['calls']) for p in points),all_recall_passed=True,hashes_deadline_mix_clocks_passed=True,resources=resource,wall_stack_mutex_leaf_samples=locks,scope='Initial sample64 includes profiling. Ladder has resource polling. No unprofiled performance gate claimed; counters do not establish external scheduling causality.'),indent=2))
 print(json.dumps(dict(points=len(points),calls=sum(int(p['calls']) for p in points),all_recall_passed=True,resources=resource,locks=locks),indent=2))
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--run',type=Path,required=True);main(p.parse_args().run)
