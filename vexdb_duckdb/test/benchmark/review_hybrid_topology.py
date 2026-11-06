"""Read-only directed graph connectivity and retained-miss neighborhood audit."""
import argparse
import csv
import json
from collections import deque
from pathlib import Path

from review_hybrid_candidate_diagnostic import sha


def analyze(run, candidates, previous, out):
    if out.exists():
        raise RuntimeError("refusing to overwrite evidence")
    protocol = json.loads((run / "protocol.json").read_text())
    assert json.loads((run / "state.json").read_text())["inputs_unchanged"]
    for path, digest in protocol["sha256"].items():
        if sha(path) != digest:
            root = Path(__file__).resolve().parents[3]
            frozen = run / "source" / Path(path).relative_to(root)
            assert frozen.is_file() and sha(frozen) == digest, "input/frozen source changed: " + path
    for path, digest in json.loads((run / "result-hashes.json").read_text()).items():
        assert sha(run / path) == digest
    reports, misses = [], []
    for cohort in protocol["cohorts"]:
        name = cohort["name"]
        meta = json.loads((run / name / "metadata.json").read_text()); n = meta["nodes"]
        adj, rev, node_rows, row_node = [[] for _ in range(n)], [[] for _ in range(n)], [[] for _ in range(n)], {}
        for row in csv.DictReader((run / name / "nodes.csv").open()):
            node = int(row["node"]); node_rows[node] = list(map(int, row["row_ids"].split()))
            for rid in node_rows[node]:
                assert rid not in row_node
                row_node[rid] = node
        edges = 0
        for row in csv.DictReader((run / name / "edges.csv").open()):
            a, b = int(row["source"]), int(row["target"])
            assert 0 <= a < n and 0 <= b < n
            adj[a].append(b); rev[b].append(a); edges += 1
        seen = bytearray(n); queue = deque([meta["entry"]]); seen[meta["entry"]] = 1
        while queue:
            for w in adj[queue.popleft()]:
                if not seen[w]: seen[w] = 1; queue.append(w)
        reachable = sum(seen)
        # Iterative Kosaraju avoids recursion depth limits and external libraries.
        visited = bytearray(n); order = []
        for start in range(n):
            if visited[start]: continue
            stack = [(start, 0)]; visited[start] = 1
            while stack:
                v, i = stack[-1]
                if i == len(adj[v]): order.append(v); stack.pop(); continue
                stack[-1] = (v, i + 1); w = adj[v][i]
                if not visited[w]: visited[w] = 1; stack.append((w, 0))
        component = [-1] * n; sizes = []
        for start in reversed(order):
            if component[start] != -1: continue
            c = len(sizes); stack = [start]; component[start] = c; count = 0
            while stack:
                v = stack.pop(); count += 1
                for w in rev[v]:
                    if component[w] == -1: component[w] = c; stack.append(w)
            sizes.append(count)
        incoming = [False] * len(sizes)
        for v in range(n):
            for w in adj[v]:
                if component[v] != component[w]: incoming[component[w]] = True
        sources = {i for i, has_incoming in enumerate(incoming) if not has_incoming}
        reports.append({"cohort": name, **meta, "edges": edges, "zero_indegree_nodes": sum(not x for x in rev),
                        "entry_reachable": reachable, "strong_components": len(sizes),
                        "source_components": len(sources), "source_component_sizes": sorted(sizes[i] for i in sources),
                        "largest_components": sorted(sizes, reverse=True)[:12]})
        original = [r for r in csv.DictReader((previous / name / "requests.csv").open())
                    if r["case"] == "plain_ids" and r["limit"] == "0" and int(r["hits"]) < 10]
        pools = {(r["point"], int(r["query"])): set(map(int, r["ids"].split()))
                 for r in csv.DictReader((candidates / name / "ids.csv").open())
                 if r["point"] in ("raw-pool1536", "pq-pool1536")}
        for row in original:
            q = int(row["query"]); got = set(map(int, row["ids"].split())); truth = set(map(int, row["truth"].split()))
            mode = "raw" if row["mode"] == "off" else "pq"
            pool = {row_node[rid] for rid in pools[mode + "-pool1536", q]}
            anchors = {row_node[rid] for rid in got}
            for rid in sorted(truth - got):
                sid = row_node[rid]
                misses.append({"cohort": name, "mode": mode, "query": q, "row": rid, "node": sid,
                               "component": component[sid], "component_size": sizes[component[sid]],
                               "indegree": len(rev[sid]), "outdegree": len(adj[sid]),
                               "outgoing_to_pool": len(set(adj[sid]) & pool),
                               "incoming_from_pool": len(set(rev[sid]) & pool),
                               "outgoing_to_top10": len(set(adj[sid]) & anchors),
                               "incoming_from_top10": len(set(rev[sid]) & anchors)})
        print(name, reports[-1], flush=True)
    out.mkdir(parents=True)
    (out / "review.json").write_text(json.dumps({"hashes_passed": True, "graphs": reports,
             "scope": "Frozen base-layer directed edges; upper-layer paths can reach other base components. No product or timing change."}, indent=2))
    with (out / "miss-neighborhoods.csv").open("w") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(misses[0])); writer.writeheader(); writer.writerows(misses)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("run", "candidates", "previous", "out"): parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    analyze(args.run.resolve(), args.candidates.resolve(), args.previous.resolve(), args.out.resolve())
