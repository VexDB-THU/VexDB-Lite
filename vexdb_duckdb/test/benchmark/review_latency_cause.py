"""Independently validate native latency traces and summarize sample evidence."""
import argparse
import csv
import json
import re
import time
from pathlib import Path

import numpy as np
from review_filtered_search import csv_rows, percentile, sha


def sample_summary(path):
    source = path.read_text()
    graph, collapsed = source.split("Sort by top of stack", 1)
    roots = sum(map(int,re.findall(r"^\s+(\d+) Thread_",graph,re.M)))
    ancestors = []
    allocator_wait = 0
    for line in graph.splitlines():
        m = re.match(r"^(\D*?)(\d+) (.*?)  \(in ",line)
        if not m:
            continue
        depth,count,symbol = len(m[1]),int(m[2]),m[3]
        while ancestors and ancestors[-1][0]>=depth:
            ancestors.pop()
        if symbol=="__psynch_mutexwait" and any("FixedSizeAllocator::Get(" in x[1] for x in ancestors):
            allocator_wait += count
        ancestors.append((depth,symbol))
    exclusive = []
    for line in collapsed.split("Binary Images:",1)[0].splitlines():
        m = re.match(r"\s*(.*?)\s+\(in [^)]+\)\s+(\d+)\s*$",line)
        if m:
            exclusive.append({"symbol":m[1],"samples":int(m[2])})
    return {"path":str(path),"sha256":sha(path),"all_thread_sample_observations":roots,
        "allocator_mutex_wait_observations":allocator_wait,
        "allocator_wait_fraction_of_all_threads":allocator_wait/roots if roots else None,
        "top_exclusive":exclusive[:20],
        "scope":"Sample observations include runnable/off-CPU threads, not CPU percentages. Inclusive ancestors identify allocator mutex waits; top-exclusive counts are separate and must not be summed as nested inclusive time."}


def review(run):
    out=run/"latency-review"
    if out.exists():raise RuntimeError("review already exists")
    p=json.loads((run/"latency-protocol.json").read_text())
    for name,digest in p["inputs"].items():
        if sha(Path(name))!=digest:raise RuntimeError("changed frozen input: "+name)
    truths=np.load(p.get("oracle_path", str(run.parent/"distance-constant-reuse-20261028/review/text-oracle.npz")))
    corpus_rows=p.get("corpus_rows",100000)
    summaries=[];flags=[];total=0;quality_bad=[]
    for phase in ("profile","clean"):
        s=json.loads((run/(phase+"-state.json")).read_text())
        if s["status"]!="complete" or s.get("active_pid") or s.get("controller_pid"):
            raise RuntimeError("phase not stopped/complete")
        phase_calls=0
        for job in p[phase]:
            name=job["name"];directory=run/name
            hashes=json.loads((run/(name+"-hashes.json")).read_text())
            for path,digest in hashes.items():
                if sha(run/path)!=digest:raise RuntimeError("changed native output")
            native={r["point"]:r for r in csv_rows(directory/"points.csv")}
            for line in Path(job["points"]).read_text().splitlines():
                fields=line.split();point=fields[0];filt=fields[5];c=int(fields[6]);requests=int(fields[8])
                result=native[point];rows=csv_rows(directory/(point+".timing.csv"))
                if len(rows)!=c*requests or {(int(r["worker"]),int(r["ordinal"])) for r in rows}!={(w,i) for w in range(c) for i in range(requests)}:
                    raise RuntimeError("incomplete request coverage")
                wall=int(result["wall_ns"]);latencies=[];cpus=[];hits=0;truth=truths[filt+"_top_ids"]
                for row in rows:
                    begin,end,cpu=(int(row[k]) for k in ("begin_ns","end_ns","thread_cpu_ns"))
                    if not 0<=begin<=end<=wall or cpu<=0:raise RuntimeError("invalid clock")
                    latencies.append((end-begin)/1e6);cpus.append(cpu/1e6)
                    q=int(row["query"]);ids=list(map(int,row["ids"].split()))
                    if not 0<=q<128 or len(ids)!=10 or len(set(ids))!=10 or any(not 0<=x<corpus_rows or (filt=="half" and x%2) for x in ids):
                        raise RuntimeError("illegal result")
                    hits+=len(set(ids)&set(truth[q]))
                if abs(len(rows)*1e9/wall-float(result["qps"]))>1e-6:raise RuntimeError("QPS mismatch")
                for f,label in ((.5,"p50_ms"),(.95,"p95_ms"),(.99,"p99_ms")):
                    if abs(percentile(latencies,f)-float(result[label]))>1e-6:raise RuntimeError("latency mismatch")
                for f,label in ((.5,"cpu_p50_ms"),(.95,"cpu_p95_ms")):
                    if abs(percentile(cpus,f)-float(result[label]))>1e-6:raise RuntimeError("CPU mismatch")
                if float(result["mean_active_clients"])<.8*c:flags.append(name+"/"+point+": occupancy below80%")
                if c>1 and int(result["overlap_ns"])<=0:flags.append(name+"/"+point+": no full overlap")
                if hits!=10*len(rows):quality_bad.append(name+"/"+point)
                summaries.append({"phase":phase,"job":name,"version":job["version"],"filter":filt,"connections":c,
                    **result,"strict_recall":hits/(10*len(rows)),"mean_thread_cpu_ms":sum(cpus)/len(cpus),
                    "mean_request_wall_ms":sum(latencies)/len(latencies),"off_cpu_fraction":1-sum(cpus)/sum(latencies)})
                phase_calls+=len(rows)
        if phase_calls!=p["expected_"+phase+"_calls"]:raise RuntimeError("total request mismatch")
        total+=phase_calls
    out.mkdir()
    with (out/"points.csv").open("x") as f:
        w=csv.DictWriter(f,fieldnames=list(summaries[0]));w.writeheader();w.writerows(summaries)
    sampled=[sample_summary(run/(job["name"]+".sample.txt")) for job in p["profile"]]
    (out/"samples.json").write_text(json.dumps(sampled,indent=2))
    result={"epoch":time.time(),"calls":total,"clean_calls":p["expected_clean_calls"],"profile_calls":p["expected_profile_calls"],
        "points":len(summaries),"quality_bad":quality_bad,"flags":flags,"all_inputs_outputs_verified":True,
        "reviewer_sha256":sha(Path(__file__)),"scope":p["scope"]}
    (out/"review.json").write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))


if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("--run",required=True,type=Path);review(p.parse_args().run.resolve())
