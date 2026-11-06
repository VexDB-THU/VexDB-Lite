"""Independently verify native SQL windows and preregistered paired controls."""
import argparse,csv,hashlib,json,math
from pathlib import Path
from collections import Counter
from review_filtered_hybrid_benchmark import percentile,allowed_stored

def sha(path):
 h=hashlib.sha256()
 with Path(path).open('rb') as f:
  for b in iter(lambda:f.read(1024*1024),b''):h.update(b)
 return h.hexdigest()
def review(run,out):
 assert not out.exists();p=json.loads((run/'protocol.json').read_text());s=json.loads((run/'state.json').read_text())
 assert s['status']=='complete' and s['inputs_unchanged'] and s['active_pid'] is None
 assert s['ended_epoch']<=p['deadline_epoch'] and len(s['jobs'])==len(p['jobs'])
 assert all(sha(k)==v for k,v in p['sha256'].items())
 assert all(sha(run/k)==v for k,v in json.loads((run/'result-hashes.json').read_text()).items())
 truth=json.loads((Path(p['preparation'])/'truth.json').read_text());points=[];warm=[];stable_ids={}
 for job in s['jobs']:
  assert job['status']=='complete' and job['exit_code']==0
  w=next(w for w in p['works'] if w['name']==job['workload']);metadata={int(r['id']):r for r in csv.DictReader(Path(w['metadata']).open())}
  expected_points={r.split()[0]:r.split() for r in Path(job['points']).read_text().splitlines()}
  summaries=list(csv.DictReader((run/job['name']/'points.csv').open()));assert set(x['point'] for x in summaries)==set(expected_points)
  for summary in summaries:
   point=summary['point'];spec=expected_points[point];clients=int(spec[5]);seconds=int(spec[6]);rows=list(csv.DictReader((run/job['name']/(point+'.timing.csv')).open()))
   times={};mix=Counter();latencies=[];cpu=0;hits=0;count=0;last_end=0
   for r in rows:
    c,q=int(r['client']),int(r['query']);begin,end=int(r['begin_ns']),int(r['end_ns']);used=int(r['thread_cpu_ns'])
    assert 0<=c<clients and 0<=q<8 and 0<=begin<end<=int(summary['wall_ns']) and 0<used<=(end-begin)*1.05
    times.setdefault(c,[]).append((int(r['ordinal']),begin,end));mix[c,q]+=1;latencies.append((end-begin)/1e6);cpu+=used;last_end=max(last_end,end)
    ids=list(map(int,r['ids'].split()));expected=truth[w['source']][w['profile']+':'+str(q)]['ids'];assert len(ids)==len(set(ids))==10
    assert all(allowed_stored(w['profile'],metadata[rid],q) for rid in ids)
    assert int(r['payload_bytes'])==sum(int(metadata[rid]['body_bytes']) for rid in ids)
    hits+=len(set(ids)&set(expected));count+=len(expected)
    key=(w['name'],job['version'],q)
    if key in stable_ids:assert stable_ids[key]==tuple(ids),'version changed returned ID/order'
    else:stable_ids[key]=tuple(ids)
   assert len(rows)==int(summary['calls']) and set(times)==set(range(clients));occupied=0;latest=0;earliest=int(summary['wall_ns'])
   for c,ts in times.items():
    assert [t[0] for t in ts]==list(range(len(ts)))
    assert all(a[2]<=b[1] for a,b in zip(ts,ts[1:]));assert len({mix[c,q] for q in range(8)})==1
    assert ts[-1][2]>=seconds*1e9
    occupied+=ts[-1][2]-ts[0][1];latest=max(latest,ts[0][1]);earliest=min(earliest,ts[-1][2])
   wall=int(summary['wall_ns']);qps=len(rows)*1e9/wall
   assert math.isclose(qps,float(summary['qps']),rel_tol=1e-10)
   for label,f in [('p50_ms',.5),('p95_ms',.95),('p99_ms',.99)]:assert math.isclose(percentile(latencies[:],f),float(summary[label]),rel_tol=1e-8,abs_tol=1e-6)
   assert math.isclose(occupied/wall,float(summary['mean_active_clients']),rel_tol=1e-10)
   assert earliest-latest==int(summary['overlap_ns'])
   assert cpu/1e6 <= float(summary['process_cpu_ms'])*1.1
   item=dict(workload=w['name'],version=job['version'],round=job['round'],clients=clients,**summary,recall=hits/count,quality_passed=hits/count>=.999,occupancy_passed=occupied/wall>=.8*clients,cpu_cores=float(summary['process_cpu_ms'])*1e6/wall,cpu_p95_ms=percentile([int(r['thread_cpu_ns'])/1e6 for r in rows],.95),route=(run/job['name']/(point+'.route.txt')).read_text().strip())
   (warm if point.startswith('warm') else points).append(item)
 look={(r['workload'],r['version'],r['round'],r['clients']):r for r in points};controls=[]
 for w in p['works']:
  for c in (1,64):
   get=lambda v,r:look[w['name'],v,r,c]
   aa=[float(get('baseline-aa',r)['qps'])/float(get('baseline',r)['qps']) for r in (1,2)]
   gain=[float(get('candidate',r)['qps'])/float(get('baseline',r)['qps']) for r in (1,2)]
   tail=[float(get('candidate',r)['p95_ms'])/float(get('baseline',r)['p95_ms']) for r in (1,2)]
   off=[float(get('new-off',r)['qps'])/float(get('baseline',r)['qps']) for r in (1,2)]
   stable=all(.97<=x<=1.03 for x in aa);quality=all(get(v,r)['quality_passed'] for r in (1,2) for v in p['versions']);occupancy=all(get(v,r)['occupancy_passed'] for r in (1,2) for v in p['versions'])
   native=get('baseline',1)['route']=='native-scalar';default=(math.prod(off)**.5>=.97) if native else None
   controls.append(dict(workload=w['name'],clients=c,aa_r1=aa[0],aa_r2=aa[1],aa_passed=stable,qps_r1=gain[0],qps_r2=gain[1],qps_geomean=math.prod(gain)**.5,p95_r1=tail[0],p95_r2=tail[1],off_geomean=math.prod(off)**.5,default_gate_passed=default,quality_passed=quality,occupancy_passed=occupancy,benefit_gate_passed=stable and quality and occupancy and all(x>=1.1 for x in gain) and all(x<=1 for x in tail) and default is not False))
 out.mkdir()
 for name,rows in [('points',points),('warmups',warm),('controls',controls)]:
  with (out/(name+'.csv')).open('w') as f:wr=csv.DictWriter(f,fieldnames=rows[0]);wr.writeheader();wr.writerows(rows)
 result=dict(jobs=len(s['jobs']),points=len(points),timed_calls=sum(int(r['calls']) for r in points),warmup_points=len(warm),warmup_calls=sum(int(r['calls']) for r in warm),hashes_deadline_clock_mix_payload_passed=True,all_recall_passed=all(r['quality_passed'] for r in points+warm),all_benefit_gates_passed=all(r['benefit_gate_passed'] for r in controls),controls=controls,scope='Native prepared SQL incl result bodies, stored metadata, fixed fresh seed first eight. Closed-loop saturated retrieval, local ARM64; no production capacity or universal recall claim. A/A failures retained, no normalization.')
 (out/'review.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
if __name__=='__main__':
 a=argparse.ArgumentParser();a.add_argument('--run',required=True,type=Path);a.add_argument('--out',required=True,type=Path);n=a.parse_args();review(n.run,n.out)
