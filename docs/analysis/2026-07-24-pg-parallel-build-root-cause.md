# PostgreSQL 图索引并行构建根因、修复与验证

日期：2026-07-24
分支：`feature/pg-rabitq`
状态：缓存键根因已修复；本地全量和 Cohere-100K 第一轮远程质量闸门通过

## 2026-07-26 根因更正

此前把 0.9686/0.9797 的剩余召回损失归因于“多个 worker 从空图开始，导致早期拓扑
弱化”。这个判断现在已被串行直接磁盘对照否定：Cohere-100K、0 worker、从第一条
数据直接写 DiskStore 的 recall@10 只有 0.973，而相同 common 算法的完整 MemStore
为 0.996。并行不是必要条件，DiskStore 才是共同变量。

真正错误在 `GraphIndexAlgorithm::select_neighbors()` 的反向边裁剪候选：

```text
候选向量 = new_point_id 的向量
旧候选 ID = self.id
DiskStore 距离缓存键 = (候选 ID, 邻居 ID)
```

向量和 ID 不属于同一个节点，导致 `(self.id, neighbor.id)` 的旧距离命中缓存后，被
错误复用于新节点。修复让 DiskStore 候选使用 `new_point_id`；MemStore 仍保留
`self.id` 作为统计位标记，因为它没有启用成对距离缓存。

第一轮远程验证在 Cohere-100K、3 worker、20,480 个受内存限制的旧种子上把 recall
恢复到 0.994（旧约 0.980，正常完整内存 0.996），证明缓存键修复命中了真实问题。
因此最终实现删除 76,800 个维度感知串行种子，恢复低内存下从第一条数据直接磁盘
并行；本地 100K/32d 三轮结果为 1 worker 10.586s、3 worker 5.804s、1.824x，六次
recall@10 均为 1.0，3 worker 构建内存增量峰值中位数 43.9MiB。

下文“并行冷启动为何降低 recall”和大种子设计仅作为排查历史，不再代表最终根因或
最终架构。远程完全无种子 100K 与 1M 是删除该补丁的最终规模闸门。

## 旧阶段结论（已被更正）

PG 磁盘建图的多 worker 不加速，根因不是 PostgreSQL 没启动 worker，也不是 plain、
PQ 或 RaBitQ 中某个算法单独变慢。旧实现用一把 `graph_build_insert` 排他 LWLock
包住完整的 `GraphIndexAlgorithm::insert()`。leader 和所有 worker 虽然并行扫描表，
真正插图时仍然排队，实际只使用约一个 CPU 核。

这把锁也不能直接删除。PostgreSQL `ParallelContext` 的 sibling worker 加入同一个
heavyweight lock group，原有 `LockPage` 和 relation extension lock 不能完成 sibling
之间需要的互斥；同时邻接表更新是“读旧值、裁剪、写回”的复合操作。直接无锁曾稳定
产生非法 vector loc。

最终修复是保留 PG 原生并行框架，建立 Lite 自己的细粒度构建协议。100k/32d plain
直接磁盘构建中，3 workers 相对 1 worker 的中位加速从 `1.002x` 提升到 `1.818x`，
构建时间降低 44.99%，3 workers 中位 CPU 达 391%，所有构建和受影响功能回归通过。

大规模 Cohere 复测又暴露了第二个独立问题：细锁保证了并发正确性，却不能保证多个
worker 从空图开始并发插入时的拓扑质量。统一 cosine 预处理后，Cohere-1M plain
仍只有 0.9686；把 `ef_search` 提到 2048、再做精确重排也无法找回缺失近邻，证明问题
在图候选覆盖而不是查询排序。完整内存图控制组在 Cohere-100K 上达到 0.996，而固定
8K、51.2K 种子的直接磁盘并行分别只有 0.974、0.988。最终用维度感知、受内存上限
约束的稳定种子，在 76.8K 后再启动 3 个 worker，recall 恢复到 0.991 并通过 0.99
闸门。

## 旧假设：并行冷启动为何降低 recall（后续已否定）

HNSW 类图的早期节点承担长期入口和高层骨架。空图阶段同时有多个 builder 基于快速
变化的 entry 和邻接快照插入，即使所有细粒度锁都保证页、ID 和邻接表没有损坏，早期
骨架仍会比单序列构建更弱。后续节点只能连接到已有图，增加 `ef_search` 只能更充分地
遍历这张弱图，不能补回从未形成的关键边。

修复把“存储正确性”和“图质量稳定性”分开处理：

1. leader 单线程建立并落盘一个稳定种子；
2. worker 只在种子完成后加入已经创建的 parallel table scan；
3. 种子数按 `ef_construction * builder_count * construction_windows` 计算；
4. `construction_windows=max(8, ceil(dimension/m)*3)`，避免固定 8K 对高维数据过小；
5. 种子内存最多使用 `maintenance_work_mem - 256MB`，低内存时只用四分之一，仍然
   保证超大图最终进入磁盘并行路径。

这不是把并行关掉。Cohere-100K 最终版在约 76,800 节点后启动 3 个 PG worker，CPU
从约 101% 升到约 402%，剩余约 23% 数据仍是真实并行磁盘插入；峰值内存约 799.6MiB，
相对容器基线增加约 594MiB。

## 旧实现为何串行

旧路径是：

```text
并行 heap scan
  -> detoast / prepare
  -> graph_build_insert EXCLUSIVE
       -> 搜索候选邻居
       -> 分配 base/upper ID
       -> 扩展磁盘容器
       -> 写节点、邻接表和 vector/code
       -> 更新反向边
       -> 更新 entry
  -> unlock
```

100k/32d plain 的旧基准：

| 指标 | 1 worker | 3 workers |
|---|---:|---:|
| 中位时间 | 19.853s | 19.815s |
| speedup | - | 1.002x |
| 中位 CPU | 100.262% | 101.445% |
| `graph_build_insert` 平均等待进程 | 0.900 | 2.600 |

plain 的行准备很轻，绝大多数时间都在完整插入锁内。PQ/RaBitQ 原来还把
`distancer.process()` 放在这把锁内，可并行的量化计算也被串行化。

## 为什么普通 graph_index 以前看起来没问题

旧测试大多没有强制直接磁盘构建：小数据或足够大的 `maintenance_work_mem` 会先在
leader 的 MemStore 中建图，PG parallel worker 不会并发修改 DiskStore。真正从第一行
进入磁盘路径后，plain、PQ、RaBitQ 都经过同一个 `insert_on_disk()`，也都受同一把
完整锁保护，因此表现为“不会崩，但 worker 不加速”。

新增 `graph_index_plain_parallel_disk.yaml` 把 `maintenance_work_mem` 固定为 512MB，
分别覆盖 1/3 workers、节点数、索引 valid/ready、聚合 recall 和 checkpoint/restart。
临时仅删除完整锁的镜像连续 5/5 出现非法 loc，证明这是共享 DiskStore 发布问题，
不是 RaBitQ 独有问题。

## 修复协议

### 1. entry 和写者排队

`graph_build_entry` 使用 shared/exclusive 模式保护构建期 entry 快照和更新。只加这把锁
后，高层 entry 升级者仍可能被连续 shared 读者饿死：真正 exclusive 持锁不到 1ms，
等待却可达到数秒。

新增 `graph_build_entry_wait` 闸门：升级者先阻止新读者通过，再等待 entry exclusive；
拿到 exclusive 后立即放开闸门。多个 worker 基于旧 entry level 请求升级时，后到者会
重新检查最新 level；如果已经不需要升级，就降回 shared 路径，避免高层插入被重复
串行化。

### 2. ID、容量和元数据

`graph_build_storage` 只覆盖 ID 分配、DiskVector append/extend 和 metapage 数量发布。
节点内容写入、搜索和距离计算都在锁外。

没有把不可重入的 LWLock 直接放进 `DiskVector::append()->reserve()` 的嵌套调用；而是
用一层短外锁保证 sibling worker 不会同时基于旧容量发布冲突的 ID 和 page group。

### 3. 连续扩页

`graph_build_extension` 包住一整批 `P_NEW` 和 PlainStore 链接更新。它补足 sibling
worker 共享 heavyweight lock group 时 `LockRelationForExtension` 不能互斥的问题，
保证 DiskVector 需要的 page group 仍然连续。

### 4. 邻接表复合更新

`graph_build_point` 按索引、层和 point ID 哈希到 4096 个分片。构建期搜索复制一个
邻接表时拿 shared，反向边的“读、裁剪、写”全程拿 exclusive。分片碰撞只增加短暂
等待，不影响正确性。

这些 point 锁只在 `parallel_build=true` 的 DiskStore 中启用，普通查询不增加逐节点
LWLock 成本。

### 5. vector/code 和随机层级

新节点的 element、base point、raw vector 或 quantized code 全部写完后，才从旧节点
发布指向它的反向边；upper point 同样先完整写入，entry 最后发布。独立 `_vec` 文件
继续使用已有 aligned-block 写锁，未增加全局 vector 锁。

曾测试过覆盖完整 vector 读写的粗锁，它没有改善 recall，反而形成新瓶颈，因此没有
保留。PG worker 的层级随机种子改为混入 `MyProcPid`，避免多个进程使用相同随机流；
DuckDB 和 SQLite 仍使用原来的线程 ID 种子。

## 性能和内存结果

环境：Docker PG19、4 CPU、100,000 行、32 维、L2、`m=16`、
`ef_construction=100`、`maintenance_work_mem=512MB`。1/3 workers 交错各跑五轮。
主线程直接等待 `CREATE INDEX`，后台线程独立采样；这修复了旧脚本被同步
`docker stats` 采样周期抬高或压低短构建时间的问题。

| 指标 | 1 worker | 3 workers |
|---|---:|---:|
| 中位构建时间 | 12.109s | 6.661s |
| speedup | - | 1.818x |
| 时间降低 | - | 44.99% |
| 中位 CPU | 197.852% | 390.983% |
| 中位采样锁等待进程 | 0 | 0 |
| 每轮构建内存增量峰值的中位数 | 18.5 MiB | 22.6 MiB |
| 成功 | 5/5 | 5/5 |

每次都验证 `indisvalid=true`、`indisready=true`、base entries=100000、top-10 返回
10 行。基准输出：
`build/bench/pg_plain_parallel_disk/20260724-141731.csv`，结论为
`VERDICT=OPTIMIZATION_VERIFIED`。

内存采用“每轮开始前容器基线到构建期峰值”的增量。容器绝对内存包含 PostgreSQL
共享内存和前几轮留下的文件页缓存，会随运行顺序累积，不能直接比较。新的 named
LWLock 分片是固定大小，约束不随向量数增长；100k 基准中 3 workers 没有增加构建内存
增量。五轮 3 workers 有一轮出现 103.8 MiB 增量，虽然中位数为 22.6 MiB，仍需在
隔离进程和大数据环境中解释该长尾。它也不能代替 Cohere-1M 的长期 RSS、page cache
和索引文件规模测试。

## 受影响功能回归

- PG19：完整 spec `89/89`；覆盖 plain、PQ compact/parallel/disk/restart/incremental、
  RaBitQ parallel/disk/compact/restart/DML/recall、corruption、delete、WAL、transaction，
  以及 800k 行 768 维 flush nbytes overflow。
- plain 直接磁盘并行新用例：修复实现先通过，再连续 `5/5` 通过。
- 并发压力：0/1/3/7 workers 多轮构建都保持 20k entries、valid/ready，无 crash、
  assertion 或非法 loc；聚合 recall 闸门稳定通过。
- 失败清理：100 万行 plain、3 workers 直接磁盘构建由 `pg_cancel_backend` 主动取消；
  客户端正确失败，索引对象、parallel worker 和锁等待全部清理；随后同表立即重建
  成功，索引 valid/ready、base entries=1000000、top-10 返回 10 行。
- DuckDB：扩展编译、3 个关键 smoke，通过完整 spec `125/125`、4660 assertions，
  包含并行构建、并发读写、PQ、RaBitQ、持久化、WAL 和故障场景。
- SQLite：完整 build/test 通过；4 万向量串行和 8 线程三轮 recall@10 都为 1.0；
  v3 PQ/RaBitQ 旧文件兼容通过；spec `31/31`。
- 静态检查：`git diff --check` 通过；生产代码中不再引用 `graph_build_insert` 或旧的
  `VexGraphBuildLocks`，仅 benchmark 保留旧 wait event 用于历史版本对照。

## 仍需在发布前补的规模证据

本地结果已经证明架构问题被解决，也回归了受影响功能。但发布级性能结论还应在远程
Cohere-1M、768 维上重跑 1/3/7/15 workers，记录构建时间、CPU、细锁等待、每轮内存
增量、容器 RSS、page cache、索引大小、recall@10 和查询 QPS；并补 x86_64、PG16-18
的矩阵。这个缺口不否定本地修复，但限制了“大数据和多版本均线性扩展”的结论。

## 2026-07-26 根因补充：距离缓存 ID 与 compact 建图语义

后续串行 Cohere-100K 直接磁盘对照仍只有 0.973，证明低 recall 不是细粒度并行锁本身
造成。最终定位到 `GraphIndexAlgorithm::prune_neighbors()` 的反向边候选：候选向量取
自新节点，pair-distance cache 的 ID 却复用了旧邻居 ID。DiskStore 开启这组缓存后，
会把旧邻居距离错误命中为新节点距离。修复为缓存 ID 使用 `new_point_id` 后，不需要
任何串行种子，3 workers plain recall@10 恢复到 0.994。

量化 compact 还有一层语义问题。旧直接磁盘路径使用 PQ/RaBitQ code 的近似距离完成
图构建；RaBitQ 在 Cohere-100K 上因此出现 0.944 recall。最终实现把 compact 分为两个
阶段：

1. 训练 codebook，但构建期保持量化器未启用，worker 统一使用原始向量精确建图；
2. worker 全部结束后，按批编码独立向量文件，安全处理 code 缩小和低维扩张两种原地
   写入方向，截短文件后再原子发布量化器元数据。

PG19 最终完整 spec 89/89；PG17 Release 编译通过。本地量化性能门禁中 RaBitQ compact
recall 为 0.9970、QPS 为 plain 的 1.0962 倍；PQ compact recall 为 1.0000、QPS 为
plain 的 0.7564 倍。远程 Cohere 规模结果记录在配套测试报告。

## 2026-07-26 查询扩展性补充：满缓存未命中的状态机

构建并发修复和查询并发是两条独立路径。Cohere-1M 正确图的 recall 恢复到 0.9939
之后，查询压测暴露出 vector buffer manager 的满载状态机问题：缓存未满时是
`miss -> expand -> retry`，缓存已满时应是 `miss -> evict -> retry`，旧实现却继续先等
扩容，导致多个 backend 同时 sleep，CPU 利用率和 QPS 一起下降。

另一个关键错误是失败位置的表示：`set_empty(); set_invalid()` 最终得到
`buf_offset=invalid_mask`，所以 `empty()` 为 false。外层若只按 `empty()` 删除 locmap
项，会把这个失败项永久保留。后续重试只能反复命中它，最多回收 16 个无关 slot 后
直接读盘。正确条件是只删除仍然 `!valid()` 的值；并发线程已经发布的 valid 值不能
删除。

修复保持 map 内不做 eviction 的原有边界：slot 构造只负责快速报告“已满”，真正
`do_evict()` 仍在 concurrent map 操作返回后执行，避免持有 map 内部锁时递归进入
`erase_if()`。64MB 压力 A/B 从 30.538 提升到 138.884 QPS；2GB 正式配置达到
1/5/10 并发 68.153/324.360/544.643 QPS，即 4.76x/7.99x 扩展，同时 recall 保持
0.9939。
