# Cohere-1M PG 并行构建远程复测

日期：2026-07-24
分支：`feature/pg-rabitq`

## 2026-07-26 更正：recall 根因不是并行冷启动

后续对照推翻了本文早期“并行冷启动导致弱图，需要 76,800 个串行种子”的结论。
决定性证据是：同一 Cohere-100K 在 **0 worker 串行直接磁盘** 下 recall 也只有
0.973，而完整 MemStore 是 0.996。因此问题属于 DiskStore 路径，与 worker 是否并行
无关。

根因位于反向边裁剪：候选保存的是新节点向量，却错误沿用了旧反向边目标
`self.id`。DiskStore 会按两个节点 ID 缓存距离，于是已经缓存的“旧节点到旧邻居”距离
会被当成“新节点到旧邻居”距离复用。MemStore 没有启用这份成对距离缓存，所以一直
保持正常。

修复后，同一 Cohere-100K、768 维、`m=24`、`ef_construction=200`、
`ef_search=200`、3 worker、256MB 的结果为：

- 只保留受内存限制的 20,480 个旧串行种子作第一轮验证：recall@10 **0.994**，
  构建 1120.630 秒，峰值 840,119,091 bytes；
- 旧同口径直接磁盘结果约 0.980，完整 MemStore 控制组 0.996；
- 质量闸门仍为 recall@10 >= 0.99，正常范围是 0.996 左右；
- 二进制 SHA-256：
  `59711fcd44852b02d717449bd8c0fcc1f03ba38e400f0d31476d9bed37f31950`；
- 原始结果：
  `current-20260725-qualityfix/results/subset-plain-w3-cache-key-fix.json`。

最终代码已经删除维度感知大种子：低于 1GB 时从第一条数据直接启动 leader + worker
磁盘构建。远程完全无种子 Cohere-100K 和 Cohere-1M 最终结果将在本节继续补齐；在它们
通过前，不把本次修复标为规模验收完成。下文关于“大种子是最终方案”的内容保留为
排查历史，不再代表最终设计。

旧阶段结论（已被上方更正）：细粒度锁已经让 15 workers 真正并行，plain 和 RaBitQ compact 都能完成
100 万行构建。第一轮远程包因 worker 未做 cosine 归一化，recall 只有约 0.339；
统一预处理后仍只有 0.9686/0.9397，进一步定位到“空图直接并行磁盘插入”的冷启动
拓扑质量。维度感知的有界稳定种子已把 Cohere-100K/768d、3 worker 的 plain recall
从 0.974、0.988 提高到 0.991，并且保留真实并行磁盘尾段；正常完整内存基线是
0.996。Cohere-1M/15 worker 最终规模复测正在使用同一二进制执行。

## 环境和可复现材料

- 测试机：Linux x86_64，共享服务器。
- 容器：PostgreSQL 17.9，15 CPU，20GiB RAM，8GiB `/dev/shm`。
- 镜像：`vexdb_pg17_cohere:feature-pg-rabitq-finelocks-20260724`，镜像 ID
  `bee54c05b37d`。
- 源码基线：`731539a8fba18726272b2fceb2891dbc5d74d855` 加当前工作树；上传包
  SHA-256 为 `312be79ac918c1370648829f0d392ff8f1f7d9a53bdaad1d5bc1c8f18feafbd0`。
- PG/common 差异指纹：
  `b82abefc36762d15c47b565a5016a989a78df968dd799d053f3e86d07d0bd878`。
- 数据：Cohere-1M，1,000,000 x 768 训练向量、1,000 条查询、cosine；文件
  3,079,074,048 bytes，SHA-256
  `b4b1d982f27be55e55d82b28241ac2c7cc900f3cf7edefeb7dd83aafabb9154`。
- 原始结果：`current-20260724/results/full-15workers.json`；完整日志：
  `current-20260724/logs/full-15workers.log`。

数据导入用时 107.934 秒。表本体 4,194,508,800 bytes，总大小
4,216,995,840 bytes。查询计划明确使用 `cohere_idx`，不是顺序扫描。

## 测试参数

- `m=24`、`ef_construction=200`、`parallel_workers=15`。
- 直接磁盘构建：`maintenance_work_mem=1000MB`。
- 查询：`ef_search=200`、`topk=10`。
- 并发：1、5、10、15、20、25、30。
- 每个并发点执行 10,000 次查询，重复 3 轮，取中位 QPS。

## 完整结果

| 指标 | plain | RaBitQ compact |
|---|---:|---:|
| 构建时间 | 2998.124 s | 2932.909 s |
| PG relation | 352,632,832 B | 352,739,328 B |
| 含向量文件的物理总量 | 3,424,419,840 B | 1,248,739,328 B |
| 峰值容器内存 | 约 7.87 GiB | 约 8.68 GiB |
| recall@10 | 0.3395 | 0.3393 |

RaBitQ compact 的物理总量比 plain 少 63.5%。两个模式是顺序执行，后一个模式
继承了前一个模式的 page cache 和 allocator 状态，因此 7.87/8.68 GiB 不能直接当成
算法净内存对比；从各自采样基线到峰值的增量约为 3.16GiB 和 1.87GiB，也仍包含
数据库缓存和构建临时内存。

| 并发 | plain QPS | RaBitQ QPS | RaBitQ/plain |
|---:|---:|---:|---:|
| 1 | 83.092 | 55.394 | 66.7% |
| 5 | 400.034 | 255.815 | 63.9% |
| 10 | 720.410 | 445.577 | 61.9% |
| 15 | 775.276 | 518.747 | 66.9% |
| 20 | 858.001 | 558.971 | 65.1% |
| 25 | 878.163 | 583.155 | 66.4% |
| 30 | 869.124 | 594.850 | 68.4% |

这些 QPS 的图本身 recall 不合格，只能说明在错误索引上的执行开销，不能与主库
截图或正确版本做性能验收。

## 与主库截图的关系

主库同参数截图中的构建时间为 plain 536.08 秒、RaBitQ 597.58 秒，recall 分别为
0.997、0.996。当前远程机上的完成时间分别慢 5.59 倍和 4.91 倍，但两边硬件、磁盘、
缓存和实现都不同，不能把倍数直接当成算法差距。当前结果能确定的是：

1. 删除完整插入全局锁后不再只有约 1 个核工作。构建期间通常使用 12--14.7 个核，
   CPU 峰值 1477.98%。
2. 大多数时刻 15 个 builder 都不等待；偶尔只有 1--3 个短暂等待
   `graph_build_point`，另有短暂 `DataFileRead`，没有再次形成全局排队。
3. 两个索引均完整构建，没有崩溃或 OOM。因此细粒度锁解决了“workers 不加速”的
   架构问题，但还不能据这轮错误 recall 判断最终性能是否达到主库水平。

## recall 根因

leader 的构建回调一直通过 `read_vec()` 完成 detoast、对齐和 cosine 单位化；PG
parallel worker 却直接把 `DatumGetVector()` 返回的原始向量送入 distancer 和图插入。
Cohere 原始向量并非单位长度，15 workers 写入的绝大多数节点保持原长度，leader
写入的少数节点被单位化，最终图混用了两种表示。查询端按单位化 cosine/IP 遍历，
所以 plain 和 RaBitQ 同时降到约 0.339；两者几乎相同也说明问题位于共同的图构建
输入，而不是 RaBitQ 编码本身。

修复把向量读取和预处理放入 leader/worker 共用的 `BuildCallbackDataBase`：两条路径
现在统一 detoast、64 字节对齐、cosine 单位化和释放；worker 的 quantizer code 也从
同一个单位化向量计算。

## 修复后的本地验证

- 新增直接磁盘综合回归：20,000 行、32 维、长度变化 1--3101 倍；覆盖 1 worker、
  3 workers、重启后读取、plain cosine 和 RaBitQ compact cosine。
- 改进后的均匀方向数据上，plain 重复 5 次均为 80/80 命中，RaBitQ 重复 3 次均为
  80/80；正式门槛为至少 78/80、8 条查询全部命中自身、零条完全漏召回。
- 综合回归 1/1 通过；全部并行构建 13/13、RaBitQ 6/6、PQ 13/13 通过。
- PostgreSQL 19 全量实际执行 89/89 通过，覆盖 80 万行 x 768 维的约 3.2GB flush、
  WAL/重启、事务、增删改、plain、PQ、RaBitQ 和并行磁盘构建。

## 尚未完成的最终闸门

修复后的源码和二进制已经完成同口径 Cohere-1M 复测。当前闸门状态为：

1. plain 和 RaBitQ recall 至少 0.99：**未通过**，分别为 0.9686 和 0.9397；
2. 重新记录构建耗时、15 workers CPU、锁等待、索引大小和峰值内存：**已完成**；
3. recall 合格后再接受 1/5/10/15/20/25/30 并发 QPS：**未执行**；
4. 补 1/3/7/15 workers 构建矩阵：**待完成**。

当前判断是：并行架构修复已被远程 CPU 和锁等待数据证明有效，cosine 归一化根因也已
修复；但 15-worker 图质量仍不达标，远程性能验收未通过。

## 修复后二进制远程复测

最新源码包 SHA-256 为
`3c174cd5df112274ba0f1572dd6cdf58e5ccf52ad36900af5c475f74107aa62a`，
PG/common 差异指纹为
`bf427ad55fd25f1b57a238b793a45cb1fe55fbc418bdda65e410d0ab04c8ad7e`。
远程 PG17 Release 二进制 SHA-256 从旧版
`f1ad7566849cfdd55893f25ef73dd72492540eff9a64917902f6756c9deb9f20`
变为新版
`a6c80b31111d62f1f6c51097a1672643f8fdd95d1f3ccafb8c6bf32233c6e524`。
数据库经 WAL 恢复后确认加载新版二进制，Cohere 表和 1,000 条查询保持不变。

| 指标 | 修复后 plain | 修复后 RaBitQ compact |
|---|---:|---:|
| 构建时间 | 2794.632 s | 2293.456 s |
| recall@10，ef_search=200 | 0.9686 | 0.9397 |
| PG relation | 352,632,832 B | 352,739,328 B |
| 含向量文件的物理总量 | 3,424,391,168 B | 1,248,739,328 B |
| 容器内存峰值 | 约 5.80 GiB | 约 5.50 GiB |

与修复前错误索引相比，plain recall 增加 0.6291，RaBitQ 增加 0.6004，证明
leader/worker 统一 cosine 归一化是正确修复。构建时间也分别缩短约 6.8% 和 21.8%。
两种查询计划都明确使用 `cohere_idx`。

但是两者仍低于 0.99 闸门。RaBitQ 搜索宽度扫描结果如下：

| ef_search | recall@10 | 1,000 queries |
|---:|---:|---:|
| 200 | 0.9397 | 基准轮 |
| 400 | 0.9566 | 25.409 s |
| 800 | 0.9659 | 44.200 s |
| 1600 | 0.9709 | 80.850 s |
| 2048 | 0.9720 | 98.310 s |

继续扩大搜索宽度已经接近平台。为区分近似排序和图候选覆盖问题，又对候选做原始
向量精确 cosine 重排：

- `ef_search=200` 时，精排前 20/50/100/200 个候选均为 0.9417；
- `ef_search=2048` 时，精排前 100/500/1000/2048 个候选均为 0.9743。

SQL `EXPLAIN ANALYZE` 确认 `LIMIT 200` 真实返回 200 行，不存在内部 top-k 截断。
因此剩余约 2.6% 的真实近邻没有进入 2048 个已访问候选，问题发生在图遍历/图质量，
不能只靠查询端精排解决。plain 在相同 15-worker 图上的 0.9686 也低于主库，说明下一步
应优先比较同一数据集的 1/3/7/15 workers plain recall，确认细粒度并发插入是否降低
图质量；然后再单独评估 RaBitQ 近似距离带来的额外损失。

原始结果保存在远程：

- `current-20260724-recallfix/results/plain-recall.json`
- `current-20260724-recallfix/results/rabitq-recall.json`
- `current-20260724-recallfix/results/rabitq-ef-sweep.json`
- `current-20260724-recallfix/results/rabitq-refine-sweep.json`
- `current-20260724-recallfix/results/rabitq-refine-ef2048.json`

由于 recall 闸门没有通过，本轮没有继续执行并发 QPS；之前错误索引上的 QPS 仍只可
用于定位开销，不能作为性能结论。

## 2026-07-25：冷启动图质量尝试（后续已撤销）

### 对照实验

在同一台远程服务器、同一份 Cohere-100K/768d 和同样的 `m=24`、
`ef_construction=200`、`ef_search=200` 上做了三组对照：

| 构建方式 | 串行稳定种子 | 真实磁盘并行尾段 | recall@10 |
|---|---:|---:|---:|
| 旧直接磁盘并行 | 0 | 100% | 低于修复后结果 |
| 第一版有界种子 | 8,192 | 约 92% | 0.974 |
| 第二版有界种子 | 51,200 | 约 49% | 0.988 |
| 最终维度感知种子 | 76,800 | 约 23% | 0.991 |
| 完整内存图控制组 | 约 100,000 | 0 | 0.996 |

完整内存控制组使用完全相同的 common 图算法和查询路径，说明“正常应该是多少”不是
凭主库截图猜测：这个 100K 控制集本身的正常值就是 0.996；主库 Cohere-1M 截图中的
plain 0.997、RaBitQ 0.996 与它一致。当前验收线定为 `recall@10 >= 0.99`，既能容纳
随机图的正常波动，也能拦住 0.9686/0.9397 这类真实退化。

最终策略不再让多个 PG worker 从空图同时插入。低于 1GB 且启用并行 worker 时，
leader 先在 MemStore 建一个有界种子并落盘，再让 worker 加入同一个 parallel scan。
种子目标随 `dimension / m`、`ef_construction` 和 builder 数增长，Cohere-768d/m=24
使用每个 builder 96 个 construction window；同时用
`maintenance_work_mem - 256MB` 限制上界，防止方案退化为不受限的全内存构建。

Cohere-100K 最终实测：

- 构建时间 894.297 秒；
- recall@10 为 0.991，通过 0.99 闸门；
- 3 个 worker 在约 76,800 个种子节点后真实启动，CPU 从约 101% 升到约 402%；
- 容器内存基线 215,272,652 B，峰值 838,441,369 B，增量约 594MiB；
- 峰值只占 8GiB 容器限制的约 9.8%，没有 OOM 或无界增长；
- 查询计划明确使用 `cohere_idx`。

第一版 51,200 种子得到 0.988，没有被当作完成，而是继续提高到 76,800 后才通过
闸门。原始结果保存在远程：

- `current-20260725-qualityfix/results/subset-plain-w3-qualityfix2.json`
- `current-20260725-qualityfix/results/subset-plain-w3-qualityfix3.json`
- `current-20260725-qualityfix/results/subset-plain-memory-w3-recall.json`

## 2026-07-26：最终根因修复（取代串行种子方案）

上一节的 76,800 行串行种子只是定位阶段的临时绕过方案，最终源码已撤销。真正根因在
DiskStore 反向边裁剪的距离缓存：候选保存的是新节点向量，缓存 ID 却错误使用旧邻居
ID，导致旧距离被当成新节点距离复用。MemStore 不启用这组 pair-distance cache，因而
完整内存控制组一直正常。修复后，候选向量和缓存 ID 都使用 `new_point_id`。

无串行种子、从第一条记录起直接磁盘并行的 Cohere-100K plain 结果：

- recall@10 为 0.994，恢复到完整内存控制组 0.996 的正常波动范围；
- 构建时间 1,212.292 秒，leader 加 3 workers 从开始就约占用 400% CPU；
- 峰值容器内存 712,087,961 B，约 679 MiB；
- 远程原始结果：`results/subset-plain-w3-cache-key-no-seed.json`。

这个修复也暴露了 compact 的第二个独立问题：直接用 RaBitQ 近似距离建图虽然只需
175.254 秒，但 recall@10 只有 0.944。它不是合理的性能提升，而是建图质量损失。
最终 compact 流程改为：先训练量化器但不启用，使用原始向量和精确距离完成图构建；
全部 worker 结束后，再批量把独立向量文件原地编码为 PQ/RaBitQ code、截短文件，最后
才发布量化器元数据。这样 compact 只改变最终存储和查询距离，不改变图拓扑质量。

本地 20,000 行、32 维、5 轮中位数：

| 模式 | 构建时间 ms | QPS | recall@10 | 相对 plain 构建 | 相对 plain QPS |
|---|---:|---:|---:|---:|---:|
| plain | 1,588.52 | 4,689.44 | 1.0000 | 1.0000 | 1.0000 |
| PQ full | 3,843.75 | 3,559.99 | 1.0000 | 2.4197 | 0.7591 |
| PQ compact | 3,851.04 | 3,546.92 | 1.0000 | 2.4243 | 0.7564 |
| RaBitQ full | 2,192.70 | 5,034.74 | 0.9965 | 1.3803 | 1.0736 |
| RaBitQ compact | 2,207.59 | 5,140.46 | 0.9970 | 1.3897 | 1.0962 |

PG19 最终完整 spec 为 89/89，通过 plain、PQ、RaBitQ、并行磁盘、compact、低维 code
扩张、增量写入、重启、WAL 和事务回归。最终 Cohere-100K/1M 远程结果在同一节继续
补充。

跨端最终回归也使用同一份工作树重新执行：DuckDB 125/125、4660 assertions；SQLite
完整构建与冒烟通过，4 万行串行和 8 线程连续三轮 recall@10 均为 1.0，v3 PQ/RaBitQ
旧文件兼容通过，spec 31/31。SQLite Release 三轮中位数中，PQ compact recall 为
0.9990、QPS 为 plain 的 0.8312；RaBitQ compact recall 为 0.9945、QPS 为 plain 的
0.7639；两者 raw mirror 都为 0，确认 compact 没有保留原始向量副本。

Cohere-100K、768 维、3 workers 的最终 RaBitQ compact 实测也通过：

- recall@10 从旧近似建图的 0.944 恢复到 0.993，通过 0.99 门槛；
- 构建时间 1,322.808 秒，是同环境 plain 1,212.292 秒的 1.091 倍；
- 构建期 leader 加 3 workers 持续约 400% CPU，worker 结束后由 leader 完成最终编码；
- 容器绝对峰值 722,678,579 B；扣除本轮基线后的增量约 510 MiB；
- 最终物理索引 133,013,504 B，比 plain 350,502,912 B 小 62.1%；
- 原始结果：`results/subset-rabitq-w3-exact-compact.json`，测试二进制 SHA-256 为
  `d8831d2d699fd1bafc7cad1aa08b5f1ac4cc22d3e11f34fda90dca2fb7b5a6f3`。

同口径 PQ compact、`pq_m=96` 的 recall@10 为 0.988，没有按 0.99 闸门误判为通过。
对同一个已建索引做搜索宽度扫描，`ef_search=400/800/1600/2048` 分别得到
0.996/1.000/1.000/1.000。这证明精确建图拓扑正常，剩余差距来自 m=96 PQ code 的
路由精度，而不是 compact 再次损坏图。为保持与主库 `ef_search=200` 的统一口径，继续
验证 `pq_m=192`，不在实现内部暗中放大搜索宽度。

`pq_m=192` 的同机复测得到 recall@10 0.994，通过 0.99 门槛；构建时间
1,457.233 秒，比 `pq_m=96` 的 1,370.163 秒增加 6.4%。最终物理索引
63,299,584 B，`pq_m=96` 为 53,698,560 B；换取高质量路由的增量约 9.2MiB，构建期
增量内存约增加 7.7MiB。远程原始结果为
`results/subset-pq192-w3-exact-compact.json`。因此 768 维高召回配置采用 `pq_m=192`，
而不是偷偷改变统一的 `ef_search=200` 查询口径。

## 2026-07-26：Cohere-1M 最终 plain 与满缓存并发修复

最终 PG17 Release 二进制先完成无串行种子的 Cohere-1M plain 构建。参数保持
`m=24`、`ef_construction=200`、15 workers、`ef_search=200`、top-10；为了从第一条
记录进入直接磁盘路径，`maintenance_work_mem` 使用 256MB。

- 构建时间 3,806.948 秒；比修复距离缓存 ID 之前的 4,375.965 秒缩短 13.0%；
- recall@10 为 0.9939，通过 0.99 门槛，且重启后仍为 valid/ready；
- 最终物理总量 3,424,387,072 B；
- 容器基线 122,368,819 B，绝对峰值 7,474,316,836 B。峰值包含 3GB 级向量文件的
  page cache，不能当作算法堆内存；
- 1 并发三轮 QPS 为 68.69、69.11、69.39，中位数 69.11。

继续执行并发测试时发现独立的查询缓存问题。向量缓存装满后，5 个后端仍在前进，
但只使用约 1.3 个 CPU 核；64MB 压力配置下 QPS 仅 30.538，缓存关闭时同机约 66.33。
根因有两层：

1. 全局缓存额度已经用完且 freelist 为空时，slot 构造仍按“等待扩容”路径指数退避，
   每个冷未命中都先做无意义 sleep，返回外层后才开始 evict；
2. 构造失败写入 locmap 的值带 `invalid_mask`，不是全零 empty。旧清理条件只删除
   empty，导致同一个 invalid 项被重试 16 次、误回收其他 slot，随后绕过缓存；无效
   key 还会持续累积。

修复后，达到全局上限时立即返回到 map 锁外执行回收，并删除仍为 invalid 的目标项；
若回收连续失败，仍保留有界重试和直接磁盘读取兜底。PG17 测试二进制 SHA-256 为
`787cf4438d442597f407e3f6f603c5f85d5370d4898b89e330e49a4d1ead854b`。

64MB 满缓存、完全相同的 5 并发 3,000 查询 A/B：

| 版本 | QPS | 相对旧版 | CPU 使用 |
|---|---:|---:|---:|
| 修复前 | 30.538 | 1.00x | 约 1.3 核 |
| 仅去掉无意义等待 | 133.293 | 4.36x | 接近 5 核 |
| 完整修复 invalid 项 | 138.884 | 4.55x | 接近 5 核 |

恢复实际 2GB 配置后，正式三轮结果如下：

| 并发 | 三轮 QPS | 中位 QPS | 相对 1 并发 | 并行效率 |
|---:|---|---:|---:|---:|
| 1 | 69.013 / 68.153 / 67.847 | 68.153 | 1.00x | 100.0% |
| 5 | 325.146 / 321.767 / 324.360 | 324.360 | 4.76x | 95.2% |
| 10 | 544.643 / 582.423 / 540.145 | 544.643 | 7.99x | 79.9% |

因此这台测试机上“正常值”应按同机正确索引理解为：1/5/10 并发约
68/324/545 QPS，而不是直接照搬主库截图的 139/684/1272。后者硬件和完整运行环境
不同；本轮的关键验收是正确 recall 下仍达到 4.76x 和 7.99x 扩展。修复后重新跑完
1,000 个 Cohere 查询，recall@10 仍为 0.9939；1ms statement timeout 在 0.12 秒内
返回，数据库无遗留查询后端。

原始远程输出：

- `results/full-plain-rabitq-pq-exact-compact-256mb.json`（1M 构建、recall、早期单并发）；
- `current-20260725-vecbuf/screenlog.0`（64MB 修复前基线）；
- `current-20260725-vecbuf/fixed/screenlog.0`（只去等待）；
- `current-20260725-vecbuf/fixed2/screenlog.0`（完整修复）；
- `current-20260725-vecbuf/formal-2gb/screenlog.0`（2GB 正式三轮）。

最终修复后的完整回归：

- PostgreSQL 19 Release 全量 spec：89/89；包含 80 万行 x 768 维 flush、缓存回收、
  并行构建、WAL/重启、DML、plain、PQ、RaBitQ；
- PG 20,000 行量化器五轮中位数：plain 4,003.20 QPS/recall 1.0000，PQ full
  3,220.66/1.0000，PQ compact 3,320.88/1.0000，RaBitQ full 4,258.85/0.9970，
  RaBitQ compact 4,523.45/0.9980；全部功能和性能门槛通过；
- DuckDB 全量：125/125，4,660 assertions；
- SQLite 从当前源码完整重建并通过 M0/M1/SIMD/M2/M3，4 万行串行 recall 1.0、
  8 线程三轮 recall 都为 1.0，v3 PQ/RaBitQ 固定文件兼容通过，spec 31/31；
- `git diff --check`、benchmark Python 编译和三个测试 runner 的 `bash -n` 通过。
