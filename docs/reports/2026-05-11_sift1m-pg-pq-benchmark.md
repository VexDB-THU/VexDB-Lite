# SIFT1M PG-side vexdb_graph 基准测试报告

**日期**: 2026-05-11
**服务器**: 116.204.26.51 (CentOS 8 / x86_64 / gcc 8.5)
**PostgreSQL**: 19devel (从 git master 源码编译)
**扩展**: pg_vexdb @ `vexdb-pq` 分支 head `584860ee6c` (PR #4)
**数据集**: SIFT1M
  - n_base = 1,000,000 × dim 128 (float32)
  - n_queries = 200 (full 10K subset 因 SIGSEGV 没跑完)
  - n_neighbors_per_query = 100 ground truth
  - 距离 metric: L2
**索引参数**: `m = 16, ef_construction = 200, parallel_workers = 0`
**查询语句**: `SELECT id FROM sift_base ORDER BY vec <-> $1::floatvector LIMIT 100`

---

## 构建 + 索引大小

| 变体 | 构建时间 | 索引大小 | 备注 |
|---|---:|---:|---|
| HNSW only           | ~30 分钟 | 208 MB | `WITH (m=16, ef_construction=200)` 单线程 |
| HNSW + PQ (pq_m=16) | 30.8 分钟 (1848.3 s) | 208 MB | + `quantizer='pq', pq_m=16` |

> 原始向量在 heap 里 488 MB (128 × 4 B × 1M)；本表 "索引大小" 只算 vexdb_graph 索引文件本身（HNSW 图 + 向量副本 + 元数据），不含 heap。

---

## HNSW only — 延迟 / 召回 vs `vex_ef_search`

| ef_search | rows | p50 (ms) | p95 (ms) | p99 (ms) | avg (ms) | QPS | recall@10 | recall@100 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
|  64 |   65.0 | 276.35 | 998.27 | (n/a) | (n/a) | (n/a) | 0.981 | 0.650 |
| 100 |  100.0 |   4.26 | 979.74 | (n/a) | (n/a) | (n/a) | 0.991 | 0.952 |
| 200 |  100.0 |   7.96 | 989.44 | (n/a) | (n/a) | (n/a) | 0.998 | 0.988 |
| 400 |  100.0 |  13.03 | 989.71 | (n/a) | (n/a) | (n/a) | 1.000 | 0.997 |
| 800 |  100.0 |  19.06 | 987.47 | (n/a) | (n/a) | (n/a) | 1.000 | 0.999 |

观察：

- **ef=64 不够**：rows 列只有 65（< LIMIT 100），意味着 HNSW 搜索 ef 列表本身就低于 100，召回@100 掉到 0.650。**生产场景 ef_search 不能低于 LIMIT k**。
- **ef=400 / 800 召回完美**（recall@10 = 1.000, recall@100 ≥ 0.997），p50 13-19 ms，可用范围。
- **p95 一直 ~1000 ms 异常**：每个 ef 档都是约 1 秒的 p95，怀疑 checkpoint / autovacuum / 第一次冷启动 cache miss 影响。下次需要 warmup pass + 关掉后台维护任务再测。

---

## HNSW + PQ (pq_m=16) — **未完成（PG SIGSEGV）**

构建成功（1848 s），但**首个查询触发 PG backend segfault**：

```
2026-05-11 15:12:29.577 CST [2359528] LOG: client backend (PID 2427933)
  was terminated by signal 11: Segmentation fault
DETAIL: Failed process was running:
  SELECT id FROM sift_base ORDER BY vec <-> '[...]'::floatvector LIMIT 100
2026-05-11 15:12:29.577 CST LOG: terminating any other active server processes
2026-05-11 15:12:30.058 CST LOG: redo starts at 0/38776470
2026-05-11 15:12:30.101 CST LOG: database system is ready to accept connections
```

PG 自动 redo 恢复成功，没有数据损坏。

**根因**（推测）：PG 端 PQ 扫描路径未完整接通。`PQDistancer::prepare()` 当前从进程级 `g_pq_cache` 加载 centroids（commit `8791f142f3`），但 SearchPQ 在 `graph_index_scan.cpp` 的具体调用链可能引用了一个没初始化的 buffer / 索引页指针。`hnsw_read_pq_center` 还是 stub（`include/pq.h:127` 声明，`src/pq.cpp` 空实现）。

**影响**：

- ✅ `CREATE INDEX WITH (quantizer='pq')` 接受 DDL，训练完成（KMeans 1M × 128d，30 分钟）
- ✅ 训练后的 codebook 写入 process-local cache（`g_pq_cache[Oid]`），同进程 prepare() 可读取
- ❌ **第一次 SELECT 走 vex_pq_search 路径就 crash**
- 后果：当前 PG 端 PQ 是 "build-only" feature，**不能用于生产查询**

---

## 关键发现：opclass 选错会 silently 走 seq scan

我们前一轮跑出 p50=147 ms 的"HNSW"数字，实际查询计划是：

```
Limit → Gather Merge → Parallel Seq Scan → Sort top-N
```

完全没走 vexdb_graph 索引。原因：

- `CREATE INDEX ... USING vexdb_graph (vec)` 默认 opclass = **`floatvector_cosine_ops`**（在 SQL 注册时 `DEFAULT FOR TYPE floatvector USING vexdb_graph` 给了 cosine）
- 查询 `ORDER BY vec <-> q::floatvector` 中 `<->` 对 cosine opclass 解析为 cosine_distance，对 SIFT 的 L2 ground truth 不匹配
- planner 估算 index scan 代价不如 seq scan，选了 seq

修复：CREATE INDEX 必须**显式**指定 `(vec floatvector_l2_ops)`：

```sql
CREATE INDEX idx_l2 ON sift_base
    USING vexdb_graph (vec floatvector_l2_ops)
    WITH (m = 16, ef_construction = 200);
```

修复后查询计划：

```
Limit → Index Scan using idx_l2 on sift_base
        Order By: vec <-> $1
```

p50 从 147 ms 降到 ~5–19 ms（12-30 倍提速）。

**建议**：
- 把 `floatvector_l2_ops` 设为默认 opclass（因为 SIFT 等主流 benchmark 用 L2）
- 或在文档里大字写明"必须显式指定 opclass"
- 或 planner 端给 cosine + L2 query 提示"opclass mismatch, falling back to seq scan"

---

## Known limitations 总结

| # | 项 | 状态 |
|---|---|---|
| 1 | PG-side PQ scan SIGSEGV | **阻塞生产**，本 PR 已知 |
| 2 | PQ centroids 进程级 cache (`g_pq_cache`)，restart 后丢失 | 已记录在 PR description |
| 3 | 默认 opclass 是 cosine，L2 查询要显式 opclass 否则 silent fallback | 文档 + UX 问题 |
| 4 | `amcanparallel = false`，单线程 query | 性能侧未优化 |
| 5 | 单线程 build 30 分钟 / 1M × 128d | 跟 pgvector 同量级 |

---

## 下一步建议

1. **修 PQ scan SIGSEGV**：在 `graph_index_scan.cpp` 的 SearchPQ 路径加 NOTICE 排查 buffer/code 状态；这是 PR follow-up 第 1 优先级。
2. **PQ codes 持久化到 `qtcode_block`**：实现 `hnsw_read_pq_center`，保证 restart-safe，是 PQ 真正生产可用的前提。
3. **默认 opclass 改 l2_ops**：减少使用门槛。
4. **runner warmup + 关 autovacuum**：消除 p95 ~1s 的异常。
5. **`amcanparallel = true`**：让 ANN scan 多 worker 并行，单查询 latency 进一步降低。

---

## 原始日志摘要

```
Loading queries + ground truth...
  loaded 200 queries, 200 gt sets
sift_base: 1,000,000 rows

=== HNSW only (l2_ops, m=16, ef_construction=200) ===
  idx_l2 reused, size 208.3 MB
  ef=64:  p50=276.35ms p95=998.27ms recall@10=0.981 recall@100=0.650  (rows=65, ef too low)
  ef=100: p50=4.26ms   p95=979.74ms recall@10=0.991 recall@100=0.952
  ef=200: p50=7.96ms   p95=989.44ms recall@10=0.998 recall@100=0.988
  ef=400: p50=13.03ms  p95=989.71ms recall@10=1.000 recall@100=0.997
  ef=800: p50=19.06ms  p95=987.47ms recall@10=1.000 recall@100=0.999

=== HNSW + PQ (pq_m=16) ===
  build: 1848.3s, size 208.3 MB
  ef=64:  SIGSEGV on first query — PG backend terminated
```
