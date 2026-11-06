# PostgreSQL 图索引并行磁盘构建设计

日期：2026-07-24
分支：`feature/pg-rabitq`
状态：最终方案已改为细粒度锁 + 正确距离缓存键；无大串行种子

## 2026-07-26 最终架构更正

维度感知的 76,800 节点串行种子不是最终方案。它提高了 recall，但同时减少真正的
磁盘并行数据，掩盖了 DiskStore 的实际错误。串行直接磁盘也只有 0.973 的对照证明，
问题不依赖并行冷启动。

最终架构是：

```text
maintenance_work_mem < 1GB
  -> 从第一条数据创建 DiskStore
  -> 立即启动 PG leader + workers
  -> 细粒度锁保护 entry / storage / extension / point
  -> 反向边裁剪中的新向量使用 new_point_id 作为距离缓存身份
```

`maintenance_work_mem >= 1GB` 仍保留原有 MemStore 构建；如果图超过内存预算，完整
flush 后再启动磁盘 worker。两条路径现在应当在同参数下得到接近的图质量，不能再用
增大串行种子掩盖两者差异。

本地最终版 100K/32d、1/3 worker 交错三轮：中位 10.586s 和 5.804s，加速 1.824x；
六次 recall@10 均为 1.0；3 worker 内存增量峰值中位数 43.9MiB；综合用例覆盖
plain/cosine、RaBitQ compact 和重启并通过。远程第一轮缓存键验证（仍带 20,480 个
旧种子）在 Cohere-100K 上达到 0.994；完全无种子 100K/1M 结果是最终规模闸门。

下文关于大种子的段落保留为方案演进记录，不再代表当前实现。

## 旧阶段结论（已被更正）

多 worker 不加速不是调度问题，也不是 RaBitQ 或 PQ 单独的问题。PG 的 leader 和
parallel worker 已经同时扫描表，但 plain、PQ、RaBitQ 最终都进入同一条磁盘插图路径，
并在完整的 `GraphIndexAlgorithm::insert()` 外争抢同一把
`graph_build_insert` 排他锁。100k/32d plain 基准中，3 workers 相对 1 worker 只有
`1.002x`，CPU 仍约一个核，平均有 2.6 个进程等待这把锁。

最终保留 PostgreSQL `ParallelContext`，补齐 Lite 自己的构建期并发协议：

1. 用真正能在 sibling worker 之间冲突的 named LWLock，分别保护 entry、entry
   写者排队、DiskVector 元数据、批量扩页和邻接表的复合读改写。
2. 在 `DiskVector` 外层用短 storage 锁串行化 ID、容量和元数据发布，避免把不可重入
   的 LWLock 直接塞进现有嵌套 reserve 流程。
3. 保留当前“节点内容先完成、拓扑后发布”的顺序，按 base/upper layer 分层发布；
   第一版不引入按节点 READY bitmap。
4. 正确性、压力和跨引擎回归通过后，移除完整插入锁。

这条方案复用现有 worker、DSM、buffer content lock、向量文件写锁和算法层
`lock_point()` 接口，改动集中，风险明显小于照搬 openGauss worker 框架。

细粒度锁只解决“并发时不损坏”和“多核能工作”，不自动保证空图冷启动的图质量。
生产路径因此还增加一个质量不变量：多个 worker 不能同时向空图建立最初骨架。低于
1GB 且启用并行构建时，leader 先建立维度感知、内存有上限的稳定种子，落盘后才启动
PG worker。种子不是固定比例，也不是固定 8K：目标随维度、`m`、
`ef_construction` 和 builder 数变化，并受 maintenance memory 上限约束。

```text
leader shared table scan
  -> bounded MemStore seed
  -> flush complete graph seed to DiskStore
  -> LaunchParallelWorkers
  -> leader + workers consume remaining shared scan
  -> fine-grained concurrent DiskStore insertion
```

对 Cohere-768d、m=24、ef_construction=200、3 worker，最终目标是 76,800 个种子节点；
100K 控制集仍有约 23% 数据走真实磁盘并行。其 recall@10 为 0.991，完整内存控制组为
0.996；固定 8K 和 51.2K 种子则分别只有 0.974、0.988。这个对照说明种子是图质量
约束，不是为了隐藏并行性能问题。

## 实现后的结果

实现中额外发现，只有 entry shared/exclusive 锁仍会让高层 entry 升级者长期被连续
读者插队。新增 `graph_build_entry_wait` 排他闸门后，新读者不会越过正在等待的写者；
真正的 entry exclusive 持锁通常不到 1ms，不再成为长尾串行点。PG worker 的随机数
种子也混入进程 PID，避免多个进程使用相同层级随机序列。

最终锁职责如下：

| named LWLock | 分片数 | 作用范围 |
|---|---:|---|
| `graph_build_entry` | 128 | entry 快照和高层 entry 更新 |
| `graph_build_entry_wait` | 128 | 防止 entry 写者饥饿 |
| `graph_build_storage` | 128 | ID、DiskVector 容量和元数据的短更新 |
| `graph_build_extension` | 128 | 同一索引整批连续扩页 |
| `graph_build_point` | 4096 | 单节点邻接表完整读改写 |

这些锁只按固定分片预分配，不随向量数增长。point/entry/storage 锁只在 PG 并行构建
路径启用；查询路径不增加逐节点 named LWLock。独立 `_vec` 文件继续使用原有的
aligned-block 写锁，不增加覆盖完整向量写入的全局锁。

100k 行、32 维、`maintenance_work_mem=512MB`、1/3 workers 交错各跑五轮。
计时由主线程直接等待 `CREATE INDEX` 完成，状态和 Docker 指标在后台线程采样，避免
同步 `docker stats` 把短构建时间量化到采样周期：

| 指标 | 1 worker | 3 workers |
|---|---:|---:|
| 中位构建时间 | 12.109s | 6.661s |
| speedup | - | 1.818x |
| 中位容器 CPU | 197.852% | 390.983% |
| 中位锁等待进程 | 0 | 0 |
| 每轮构建内存增量峰值的中位数 | 18.5 MiB | 22.6 MiB |
| 正确构建 | 5/5 | 5/5 |

相对旧实现的 1.002x，3 workers 现在稳定使用多核并把时间降低 44.99%。内存使用必须看
“本轮构建前基线到构建期峰值”的增量；Docker 容器绝对值包含跨轮页缓存，不能直接
横向比较。3 workers 的五轮中有一轮内存增量达到 103.8 MiB，因此当前只能确认
中位数接近，不能声称多 worker 不会增加峰值内存。

功能回归：PG19 spec 89/89，新增 plain 直接磁盘并行用例连续 5/5；DuckDB spec
125/125、4660 assertions；SQLite spec 31/31，4 万向量 8 线程三轮 recall@10=1.0，
旧 v3 PQ/RaBitQ 文件兼容通过。另用 100 万行 plain 磁盘构建验证主动取消：取消后
索引对象消失、parallel worker 和 `graph_build_*` waiters 均为 0，并可立即重建出
valid/ready、100 万 entries 的索引。

## 已确认的事实

### worker 确实启动了

`prepare_parallel_context()` 创建 DSM 和 parallel scan；磁盘模式从第一行开始就调用
`LaunchParallelWorkers()`。worker 通过 `table_beginscan_parallel()` 分片扫描 heap。

问题发生在扫描之后：leader 和 worker 都把完整图插入包在同一把
`graph_build_insert` 排他锁里。

```text
heap parallel scan             可以并行
  └─ detoast / query prepare   部分可以并行
      └─ graph_build_insert    同一索引只有一把排他锁
          ├─ 搜索候选邻居
          ├─ 分配 base/upper ID
          ├─ 扩展磁盘容器
          ├─ 写 element、邻接表、vector/code
          ├─ 更新反向边
          └─ 更新 entrypoint
```

plain 的 `d.process()` 很轻，几乎全部耗时都在锁内，所以增加 worker 只增加等待。
worker callback 甚至把 PQ/RaBitQ 的 `d.process()` 也放在锁内，量化路径的可并行计算
也被串行化。

### 当前证据

| 指标 | 1 worker | 3 workers |
|---|---:|---:|
| 100k/32d 中位构建时间 | 19.853s | 19.815s |
| speedup | - | 1.002x |
| 中位容器 CPU | 100.262% | 101.445% |
| `graph_build_insert` 平均等待进程 | 0.900 | 2.600 |
| 正确构建 | 3/3 | 3/3 |

`graph_index_plain_parallel_disk.yaml` 已证明 1/3 workers 的直接磁盘构建、节点数、
recall 和重启读取都正确；临时删除完整锁后，同一 plain 用例连续 5/5 次出现非法 loc。
因此“锁导致串行”和“锁不能直接删除”都已有稳定证据。

## 为什么 openGauss 能并行，Lite 不能直接照抄

openGauss 用 `LAUNCH_CONSUMER_WITHOUT_LOCKGROUPLEADER` 启动 worker。Lite 使用原生
PostgreSQL `ParallelContext`，parallel worker 加入 leader 的 heavyweight lock group。
同一 lock group 内，`LockPage` 和 relation extension lock 不能作为 sibling worker
之间的互斥手段。

两边的持久化布局也不同：

```text
openGauss
  完整 HNSW tuple + vector/code
      └─ 一次写入并可见
          └─ 发布反向边
              └─ 更新 entry

Lite
  base ID / elems / base point / 独立 _vec 文件 / upper ID
      └─ 分散在多个容器和文件
          └─ 分层发布反向边
              └─ 更新 entry
```

openGauss 的页锁之所以能工作，既依赖 worker 不共享 lock group，也依赖节点更接近
一次完整 tuple 提交。只复制它的局部锁范围不能保证 Lite 正确。

## 当前存储层的四个并发缺口

### 1. entry 锁对 parallel sibling 无效

`DiskStore::get_entry()` 使用 `LockPage` 的 shared/exclusive 模式。对 PG 普通独立会话
它能冲突，但对同一个 `ParallelContext` 的 sibling worker 不提供所需互斥。

后果包括空图可能被多个 worker 同时初始化，以及高层 entry 更新相互覆盖。

### 2. DiskVector 元数据发布顺序不完整

`DiskVector::append()` 当前先执行 `nitem++`，再调用 `reserve()`。`nitem` 已经可见时，
对应容量和内容可能还不存在。

更重要的是，`append()` 先拿元数据 logical lock，再调用会再次拿同一 logical lock 的
`reserve()`。heavyweight lock 对同一 lock owner 可重入；LWLock 不可重入。因此不能
把 `LockPage` 原地替换成 LWLock，必须拆出 `reserve_locked()` 一类的内部流程，保证
一层只获取一次锁。

应长期保持下面的不变量：

```text
capacity >= nitem
item_start_pages[0..npage) 全部指向已经完成初始化的连续 page group
```

### 3. 批量扩页不能保证连续

`BlockMgr::reserve_new_pages()` 用 relation extension lock 包住一批 `P_NEW`，目的是保证
返回 page range 连续。但 sibling worker 共享 lock group 时，这把锁不能阻止两批扩页
交错。

DiskVector 又按 `npage` 假定 page group 以 8、8、16、32... 的方式增长。两个 worker
基于旧 `npage` 同时计算 group size 时，即使单页扩展本身成功，也可能把错误大小的
group 发布到下一个槽位。这与“小表通过、跨容量边界后出现随机非法 loc”的实验完全
一致。

### 4. 邻接表是复合读改写，但 DiskStore 的 point lock 是空实现

反向边更新不是单次写：先读旧邻居，再做裁剪，最后写回一个槽位。buffer content lock
能避免一条记录被读到半写状态，却不能保护整个“读 -> 计算 -> 写”过程。两个 worker
可能基于同一旧值写同一个槽位，产生丢边和 recall 下降。

## 目标架构

### 锁职责必须拆开

不复用一把锁承担多个角色，避免重新形成全局串行点或自锁死。

| 锁 | 推荐粒度 | 模式 | 保护内容 |
|---|---|---|---|
| `graph_build_entry` | index OID 分片 | shared/exclusive | 空图初始化、entry 快照和升级 |
| `graph_build_meta` | index OID + DiskVector meta block 分片 | exclusive | `nitem`、`npage`、`item_start_pages` |
| `graph_build_extension` | index OID 分片 | exclusive | 同一 relation 的整批连续扩页 |
| `graph_build_point` | index OID + layer + point ID 分片 | exclusive | 单节点邻接表的复合读改写 |

建议 entry/extension 使用少量分片，meta 使用中等分片，point 使用较多分片。分片碰撞
只会带来额外等待，不能影响正确性。不同 tranche 使用不同 wait event，benchmark
才能区分热 entry、扩页和热邻接点。

现有 `graph_build_insert` 完整锁只作为迁移期安全模式保留，最终优化模式不再获取它。

### 固定锁顺序

所有路径遵守同一个顺序：

```text
entry(shared/exclusive)
  -> DiskVector meta
      -> relation extension
          -> buffer content lock

entry(shared/exclusive)
  -> vector-file stripe lock

entry(shared/exclusive)
  -> one point lock
      -> buffer content lock
```

约束：

- 一次只持有一个 point lock，不同时锁多个邻居。
- 不在持有 point lock 时分配新 ID 或扩页。
- 不在持有 buffer content lock 时反向获取 meta、extension 或 point lock。
- `DiskVector` 的 public 方法最多获取一次 meta LWLock；内部用 `*_locked()` helper。
- PG 错误使用 `PG_TRY/PG_CATCH` 或 PostgreSQL 的锁清理机制，不能依赖 C++ 析构处理
  `elog(ERROR)` 的 longjmp。

### 节点按层发布

当前算法已经接近正确的发布顺序，应把它写成明确的不变量，而不是新增一套节点状态：

```text
ALLOCATED（拓扑不可达）
  ├─ 写 elem
  ├─ 写 base point 的出边
  ├─ 写 raw vector / PQ code / RaBitQ code
  └─ BASE_READY
       └─ 在旧 base point 中发布反向边
            └─ base 图可达

对每个 upper layer：
  分配 upper ID
    -> 写完整 upper point 和 lower_layer_idx
      -> 在旧 upper point 中发布反向边
        -> 该层可达

全部需要的层完成
  -> 更新 entrypoint
```

只要以下条件成立，构建期搜索不会遇到半写节点：

1. 新节点在第一条指向它的反向边发布前，base point、elem 和 vector/code 已完成。
2. upper point 在第一条指向它的 upper 反向边发布前已完成。
3. entrypoint 最后发布。
4. 反向边写入使用 buffer exclusive lock，搜索读取使用 buffer shared lock。

因此第一版不建议加入按节点 READY bitmap。它会引入按表估算和 DSM 扩容问题，而
当前拓扑发布顺序已经能表达 READY。先用 debug validator 验证“不存在指向未初始化
节点的边”；只有细锁完成后仍发现可达性漏洞，才增加显式状态。

### DiskVector 分配流程

建议把分配改为一个短临界区：

```text
acquire meta lock
  id = nitem
  if capacity < id + 1:
      acquire relation extension lock
      allocate and initialize the complete page group
      publish item_start_pages + npage under buffer lock
      release relation extension lock
  publish nitem = id + 1 under buffer lock
release meta lock
```

节点内容仍在临界区外写入。它在反向边发布前不可达，所以无需把大块 vector/code 写入
放回 meta 锁内。

base、upper、elems 使用各自 meta 锁，可以并行分配；只有真正扩展同一 relation 时才
短暂争抢 relation extension lock。

### 算法路径

```text
parallel scan
  -> detoast / normalize / d.process            [worker 本地，可并行]
  -> entry shared lock
  -> graph search                               [并行读]
  -> allocate base ID                          [短 meta/extension lock]
  -> write elem/base/vector or code             [不同 page/file 可并行]
  -> for each selected base neighbor
       point lock -> read/prune/write -> unlock [热点节点短串行]
  -> build/publish upper layers                 [同样的短锁]
  -> optional entry update                      [少量 exclusive]
  -> release entry lock
```

worker callback 的 `d.process()` 应移到图锁之前。对 plain 影响很小，对 PQ/RaBitQ 可先
释放量化计算的并行度，但这只是完整方案的一部分，不能单独宣称解决多 worker 扩展。

## 方案对比

### A. 保留 ParallelContext，补齐细粒度发布协议

- 完整度：9/10。
- 人工估算：约 1 到 2 周；Codex 辅助实现和本地验证约 1 到 2 天，完整远程矩阵另计。
- 优点：复用现有 worker 和算法，改动集中，可保留 serialized 模式回退。
- 风险：需要同时改 DiskVector 分配、entry、point 和扩页锁，不能拆成互不相关的小补丁。

**推荐 A。** 它解决实际共享状态，不改变 PG 事务模型，也不改变图算法和持久化格式。

### B. 只把预处理移出完整锁，或遇到磁盘模式强制单 worker

- 完整度：4/10。
- 人工估算：约半天；Codex 辅助约 1 小时。
- 优点：安全，PQ/RaBitQ 可能有小幅收益，避免无意义 worker。
- 缺点：plain 仍不加速，图插入仍是单核，只是诚实降级或临时优化。

### C. 模仿 openGauss，自己启动不加入 lock group 的 worker

- 完整度：8/10。
- 人工估算：约 3 到 6 周；Codex 辅助约 4 到 7 天，事务/恢复验证另计。
- 优点：更接近主库锁模型。
- 缺点：动态后台 worker 是独立会话，需要重做未提交索引的 relation 打开、snapshot、
  错误传播、取消和清理协议，风险远大于当前问题。

### D. 分片建多个图再 merge

- 完整度：6/10。
- 人工估算：约 1 到 2 个月；Codex 辅助约 1 到 2 周，算法评估另计。
- 优点：理论并行度高。
- 缺点：改变拓扑与 recall，需要新的 merge 算法，不是当前并发缺口的最小修复。

## 性能预期

不能把 3 workers 理解为必然 3 倍。去掉完整锁后仍有三个自然上限：

1. HNSW 高层和 entry 附近是热点，point lock 会有真实竞争。
2. page extension、shared buffer 和独立 `_vec` 文件会受存储与内存带宽限制。
3. 每次插入依赖已经发布的图，不能完全变成无依赖批处理。

第一道通过线沿用现有 benchmark：

- 3 workers 相对 1 worker speedup `>= 1.20x`。
- 构建期平均 CPU `>= 180%`。
- `graph_build_insert` 等待 `<= 0.25`。
- 所有构建结果 valid/ready、节点数正确、top-10 正常。

这只是“确实并行”的门槛，不是最终目标。Cohere-1M 应测 0/1/3/7/15 workers 的完整
曲线。plain 低维更容易受磁盘和热点锁限制；768 维以及 PQ/RaBitQ 有更多距离计算和
编码工作，可期待更高 CPU 利用率，但不应在真实数据前承诺线性加速。

## 内存影响

推荐方案不会把完整图搬回内存：base、upper、elem 和 vector/code 仍落盘。

- 每个 worker 只增加搜索候选、距离缓存、量化临时区等工作内存，规模主要随
  `ef_construction`、`m`、维度和量化器增长，而不是随全表节点数线性增长。
- PQ/RaBitQ 模型会在各 worker 中各有一份，需要实测 leader 和 worker 的峰值 RSS；
  这是 worker 数增加后的主要内存项之一。
- named LWLock 分片是 cluster 级固定内存，合理配置下是不到数 MB 的数量级，不是
  每个索引或每个节点一把锁。
- 不使用 READY bitmap，避免额外的 O(N) DSM 和估算偏差；如果以后必须加入，1 bit/节点
  本身不大，但动态容量和失败清理更复杂。

验收必须同时记录峰值 RSS、shared buffer 命中/读写、临时文件和索引大小，不能只看
时间与 CPU。

## 测试覆盖图

```text
PG PARALLEL DISK BUILD COVERAGE
================================
[★★★ 已覆盖] plain 20k/32d, 1 worker, direct disk
    valid/ready + Raw Vector + 20k entries + recall>=9/10

[★★★ 已覆盖] plain 20k/32d, 3 workers, direct disk + checkpoint/restart
    valid/ready + entries + recall

[★★★ 已覆盖] 当前 serialized 性能基线
    100k/32d, 1/3 workers x 3, elapsed + CPU + lock waiters

[★★★ 已覆盖] 临时无完整锁的反向验证
    plain 直接磁盘 5/5 出现非法 loc；证明用例能区分安全/损坏实现

[GAP] DiskVector 每个扩容边界前后并发分配
    base / upper / elems；断言 capacity>=nitem、page group 连续

[GAP] 邻接表复合读改写压力
    1/3/7 workers x 20；所有 neighbor ID 和 upper layer ID 均在范围内

[GAP] 空图和 entry 升级竞争
    同时初始化、连续高层节点、分片锁碰撞

[GAP] plain / PQ / RaBitQ 全矩阵
    full/compact、m 变化、低/高维、不同 ef_construction

[GAP] 失败恢复
    cancel leader、kill worker、PG restart、CREATE INDEX CONCURRENTLY 失败清理

[GAP] 平台和版本
    PG16/17/18/19；x86_64/aarch64；assert；可运行部分 ASAN/UBSAN

[GAP] Cohere-1M 真实规模
    0/1/3/7/15 workers；时间、CPU、各锁等待、RSS、I/O、大小、recall@10
```

修复后的测试顺序：

1. 先写 DiskVector 分配/扩容的最小并发测试，证明元数据不变量。
2. 再接入 entry 和 point lock，运行现有 plain 负例直到无完整锁也稳定通过。
3. 运行 1/3/7 workers x 20 的 plain/PQ/RaBitQ 压力和 neighbor validator。
4. 运行 cancel/kill/restart 与 WAL/CONCURRENTLY 测试。
5. PG19 本地全量 spec 后，再做 PG16-19 和双架构。
6. 最后运行相同脚本的 `--expect optimized` 和 Cohere-1M。

性能测试失败不能用降低 recall、减少 `ef_construction` 或跳过完整向量写入换取通过。

## 生产失败模式

| 失败 | 当前风险 | 设计防护 | 必须测试 |
|---|---|---|---|
| 两个 worker 同时跨 DiskVector 容量边界 | 非法 loc、损坏索引 | meta + relation extension LWLock；容量先于 nitem 发布 | 是 |
| 两个 worker 同时更新同一邻居 | 丢边、recall 下降 | point lock 包住完整读改写 | 是 |
| 多 worker 同时观察空图 | 多 root、不可达节点 | entry exclusive + 锁内重读 | 是 |
| 高层节点同时升级 entry | entry 覆盖或指向未完成层 | upper 完成后才在 entry exclusive 下发布 | 是 |
| worker 在分配后、发布边前失败 | 已分配但不可达节点 | 整个 CREATE INDEX 失败并清理；不得把索引标 valid | 是 |
| worker 在持锁时 `elog(ERROR)` | 锁残留、其他 worker 卡死 | PG 错误清理/显式 catch；leader 统一取消 | 是 |
| 锁分片碰撞过多 | 正确但不扩展 | 分开 wait event，按真实冲突调 stripe 数 | 是 |
| worker 数增加导致 RSS 过高 | OOM、系统抖动 | 记录每 worker 内存；按总预算限制并行度 | 是 |

任何非法 neighbor/loc、worker crash、索引 valid 但 validator 失败都属于硬失败；不能只靠
recall 查询偶然成功判定正确。

## 实施阶段

### 阶段 1：存储分配协议

- 增加独立 meta/extension named LWLock tranche 和 hash helper。
- 重构 `DiskVector`，消除嵌套获取同一 logical lock。
- 建立 `capacity >= nitem` 和 page group 连续性测试。
- 此时保留完整 `graph_build_insert` 锁，单独证明存储层正确。

### 阶段 2：图发布协议

- 实现 DiskStore entry 和 point lock。
- 明确 base/upper/entry 的发布边界与锁顺序。
- worker 的 `d.process()` 移到图锁前。
- 增加 serialized/fine 两种内部模式，便于 A/B 和紧急回退。

### 阶段 3：移除完整锁并压力验证

- fine 模式不再获取 `graph_build_insert`。
- 运行 plain/PQ/RaBitQ 1/3/7 workers x 20 和失败恢复。
- 更新 benchmark，分别统计 entry/meta/extension/point wait event。

### 阶段 4：真实规模和默认开启

- 运行 Cohere-1M 和 PG16-19、双架构矩阵。
- 达到正确性、内存和性能门槛后默认使用 fine 模式。
- serialized 模式保留至少一个发布周期，作为安全回退。

这些阶段集中修改同一套 PG build、DiskStore 和 DiskVector 路径，应顺序实施；不适合拆成
多个同时改生产代码的 worktree。测试夹具和远程 benchmark 可在存储协议稳定后并行准备。

## 已复用的能力

- PostgreSQL `ParallelContext`、DSM 和 parallel heap scan：继续使用。
- named LWLock 注册和 `VexVecWriteLocks`：沿用同一种跨 worker 锁机制。
- PostgreSQL buffer content lock：继续保证单页物理读写原子性。
- `GraphIndexAlgorithm -> store.lock_point()` 抽象：补齐 DiskStore 实现，不改算法接口。
- 当前分层写入顺序：升级为明确发布协议，不增加新的持久化格式。
- `graph_index_plain_parallel_disk.yaml` 和 `plain_parallel_disk_benchmark.py`：直接作为
  正确性基线和性能门禁。
- openGauss：只作为锁语义和发布顺序参考，不复制 worker 框架。

## 不在本次修复范围

- PG 内存构建使用 `std::thread`：PG backend 全局状态不支持，属于另一条架构路线。
- DuckDB/SQLite 并发模型：它们共享地址空间，不存在 PG lock group 问题。
- 分片建图再 merge：会改变算法拓扑和 recall，当前没有必要。
- 查询 QPS 优化：本问题是 CREATE INDEX 的构建扩展性。
- 持久化格式大改或每节点一把持久锁：改动大且没有必要。
- 第一版 READY bitmap：先证明现有“不可达直到发布”的协议；有反例再增加。

## 最终建议

选择方案 A，并严格按“存储分配 -> 图发布 -> 移除完整锁 -> 真实规模”推进。最容易犯的
错误是看到全局锁后只把它缩小：如果没有同时修复 DiskVector 的可重入锁、批量扩页和
邻接表读改写，代码可能在小表上更快，却在跨容量边界后静默损坏。

## 2026-07-26 最终架构修正

最终实现不再使用大串行种子。plain、PQ 和 RaBitQ 都可以从第一条记录开始进入直接
磁盘并行建图；DiskStore 的 pair-distance cache 必须保证“候选向量”和“候选 ID”表示
同一个节点，反向边中的新节点候选固定使用 `new_point_id`。

`memory_mode='compact'` 的职责被收窄为最终存储格式，不再允许它改变建图距离：

1. leader 训练并持有量化器，持久化 codebook，但元数据保持 disabled；
2. leader 和 workers 以 `QuantizerType::NONE` 使用原始向量精确建图；
3. workers 退出后，leader 分批原地编码向量文件；code 变小时从前向后，低维 code
   变大时从后向前，避免覆盖尚未读取的原始向量；
4. 清除原始向量缓存、把向量文件截短到 code 逻辑大小，再发布 quantizer enabled；
5. WAL 和索引 valid/ready 发布仍发生在上述步骤全部成功之后。

该顺序保证失败的 `CREATE INDEX` 不会留下可见的半量化索引，也保证重启、增量插入和
查询只会看到完整的 raw 或完整的 code 状态。
