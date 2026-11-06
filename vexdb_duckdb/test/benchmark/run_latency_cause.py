"""Bounded profiling and interleaved native SQL controls, never concurrent jobs."""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import time
from pathlib import Path

from run_filtered_search import ROOT, alive, save, sha, update


def snapshot(path):
    values = {}
    for name, args in {"vm": ["vm_stat"], "swap": ["sysctl", "vm.swapusage"],
                       "process_cpu": ["ps", "-axo", "pid,ppid,%cpu,rss,comm"]}.items():
        x = subprocess.run(args, capture_output=True, text=True)
        values[name] = {"code": x.returncode, "stdout": x.stdout, "stderr": x.stderr}
    path.write_text(json.dumps(values, indent=2))


def init(run):
    if (run/"latency-protocol.json").exists():
        raise RuntimeError("already registered")
    (run/"original").mkdir()
    shutil.copyfile(ROOT/"build/product-tests/distance-constant-reuse-20261028/baseline/vexdb_lite.duckdb_extension",
                    run/"original/vexdb_lite.duckdb_extension")
    versions = {"original": str(run/"original/vexdb_lite.duckdb_extension"),
                "reuse": str(run/"baseline/vexdb_lite.duckdb_extension"),
                "candidate": str(run/"candidate/vexdb_lite.duckdb_extension")}
    clean, profile = [], []
    points = []
    for c in (16,64):
        points += [f"exact-c{c} pq_graph 1536 16 0 half {c} 0 8\n",
                   f"pq-c{c} pq_graph 1536 16 0 none {c} 1 128\n"]
    for round_id in (1,2):
        for version in (["original","reuse","candidate"] if round_id==1 else ["candidate","reuse","original"]):
            name=f"clean-{version}-r{round_id}"
            path=run/(name+".points")
            path.write_text("".join(points if round_id==1 else points[::-1]))
            clean.append({"name":name,"version":version,"points":str(path)})
    for i in (1,2):
        name=f"aa-original-{i}";path=run/(name+".points");path.write_text("".join(points))
        clean.append({"name":name,"version":"original","points":str(path)})
    for version in ("original","reuse"):
        for filt,requests,scan in (("none",256,1),("half",8,0)):
            name=f"profile-{version}-{filt}";path=run/(name+".points")
            path.write_text(f"trace pq_graph 1536 16 0 {filt} 64 {scan} {requests}\n")
            profile.append({"name":name,"version":version,"points":str(path)})
    files=[Path(__file__),ROOT/"vexdb_duckdb/test/benchmark/trace_search_latency.cpp",run/"trace-latency"]
    files += list(map(Path,versions.values()))+[Path(x["points"]) for x in clean+profile]
    db=ROOT/"build/product-tests/text-100k-concurrency-20261027/fixture/vectors.duckdb"
    data=ROOT/"build/product-tests/quantization-product-20261027/data/dbpedia"
    files += [db,data/"base.f32",data/"queries.f32"]
    save(run/"latency-protocol.json",{"registered_epoch":time.time(),"deadline_epoch":time.time()+2700,
        "disk_floor_bytes":4*2**30,"job_timeout":300,"versions":versions,"profile":profile,"clean":clean,
        "database":str(db),"data":str(data),"dim":1536,"expected_profile_calls":33792,"expected_clean_calls":87040,
        "scope":"Profiling not clean throughput. Clean rounds reverse workload/version order; A/A identical-binary controls separately retained. Closed-loop local bursts, not production capacity. Per-request thread CPU, process faults/switches, OS swap and stacks distinguish code cost from off-CPU waiting; cannot reconstruct unrecorded past host state.",
        "inputs":{str(p):sha(p) for p in files}})


def execute(run,phase):
    protocol=json.loads((run/"latency-protocol.json").read_text())
    for name,digest in protocol["inputs"].items():
        if sha(Path(name))!=digest: raise RuntimeError("changed input: "+name)
    statefile=run/(phase+"-state.json")
    if statefile.exists():raise RuntimeError("prior phase exists; no blind resume")
    for other in ("profile","clean"):
        p=run/(other+"-state.json")
        if p.exists():
            s=json.loads(p.read_text())
            if alive(s.get("controller_pid")) or alive(s.get("active_pid")):raise RuntimeError("other phase active")
    state={"status":"running","controller_pid":os.getpid(),"active_pid":None,"jobs":[]}
    def write():
        temp=statefile.with_suffix(".tmp");temp.write_text(json.dumps(state,indent=2));temp.replace(statefile)
    write()
    try:
        for job in protocol[phase]:
            name=job["name"];out=run/name
            if out.exists():raise RuntimeError("output already exists")
            command=[str(run/"trace-latency"),protocol["versions"][job["version"]],protocol["database"],
                     protocol["data"],str(protocol["dim"]),job["points"],str(out)]
            snapshot(run/(name+"-before.json"))
            record={**job,"command":command,"started_epoch":time.time(),"status":"running"}
            state["jobs"].append(record)
            with (run/(name+".log")).open("x") as log:
                child=subprocess.Popen(command,stdout=log,stderr=subprocess.STDOUT)
                state["active_pid"]=child.pid;record["pid"]=child.pid;write()
                sampled=False
                print("START",name,child.pid,flush=True)
                while child.poll() is None:
                    if time.time()>min(protocol["deadline_epoch"],record["started_epoch"]+protocol["job_timeout"]) or shutil.disk_usage(run).free<protocol["disk_floor_bytes"]:
                        child.terminate();child.wait(timeout=10);raise RuntimeError("deadline/disk guard")
                    if phase=="profile" and not sampled and "TIMING " in (run/(name+".log")).read_text():
                        sampled=True
                        cmd=["sample",str(child.pid),"3","1","-file",str(run/(name+".sample.txt"))]
                        x=subprocess.run(cmd,capture_output=True,text=True,timeout=30)
                        record["sampler"]={"command":cmd,"code":x.returncode,"stdout":x.stdout,"stderr":x.stderr}
                    time.sleep(.2 if phase=="profile" else 1)
            snapshot(run/(name+"-after.json"))
            record.update(status="complete" if child.returncode==0 else "failed",exit_code=child.returncode,
                          finished_epoch=time.time())
            state["active_pid"]=None;write()
            if child.returncode:raise RuntimeError("native job failed: "+name)
            save(run/(name+"-hashes.json"),{str(p.relative_to(run)):sha(p) for p in out.rglob("*") if p.is_file()})
            print("DONE",name,flush=True)
        for name,digest in protocol["inputs"].items():
            if sha(Path(name))!=digest:raise RuntimeError("changed input: "+name)
        state.update(status="complete",controller_pid=None);write()
    except BaseException as e:
        if state.get("active_pid") and alive(state["active_pid"]):
            os.kill(state["active_pid"],15)
        state.update(status="failed",controller_pid=None,active_pid=None,error=str(e));write();raise


if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("action",choices=("init","profile","clean"));p.add_argument("--run",required=True,type=Path)
    a=p.parse_args();(init(a.run.resolve()) if a.action=="init" else execute(a.run.resolve(),a.action))
