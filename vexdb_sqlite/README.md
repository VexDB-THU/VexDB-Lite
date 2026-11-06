# vexdb_sqlite — VexDB-Lite 的 SQLite 适配层

SQLite 版在同一个扩展中提供 VexDB 自研图索引和 VexFS 文件能力。普通表、向量索引和文件可以保存在同一个数据库中，并由 SQLite 事务和 Backup API 统一管理。

> 完整计划：`docs/plans/2026-06-10_sqlite-adapter-v1-plan.md`
> 范围调研：`docs/research/2026-06-10_sqlite-v1-scope-reinvestigation.md`

## 当前进度：M0～M5 ✅，PQ / RaBitQ 已接入共享图算法
## 统一命令

macOS 构建会生成自带 SQLite 的 `vexdb`，无需手动加载扩展：

```bash
vexdb agent.db
vexdb agent.db "SELECT vexdb_version();"
vexdb fs --db agent.db ls /
vexdb fs --db agent.db grep -n error /
vexdb fs --db agent.db index enable
vexdb fs --db agent.db check
```

`vexfs` 是同一个可执行文件的兼容入口，等价于 `vexdb fs`。

`grep` 默认通过单个数据库连接批量搜索，不需要逐个打开挂载文件。大工作区可显式开启
FTS5 trigram 索引；索引默认关闭，关闭时不增加文件写入开销。宿主 SQLite 没有 FTS5 时
仍可使用批量扫描，不会让文件不可读。

每个文件版本保存 SHA-256。`vexdb fs check` 默认只读检查目录、提交、历史、快照、暂存区
和全部内容；`vexdb fs check --quick` 跳过 BLOB 哈希，只检查结构和引用。

现有 SQLite 程序仍然可以加载 `vexdb_lite.dylib`：

```sql
.load ./vexdb_lite              -- 桌面；移动端走静态注册

SELECT vexdb_l2_distance('[1,2,3]', '[4,5,6]');      -- 4 个距离函数 + vexdb_f32/vector_to_json

CREATE VIRTUAL TABLE idx USING GRAPH_INDEX(
    embedding FLOAT[128], metric=cosine,
    quantizer=pq, pq_m=32, memory_mode=compact, m=16
);
INSERT INTO idx(rowid, embedding) VALUES (1, :blob_or_json);
SELECT rowid, distance FROM idx WHERE embedding MATCH :query AND k = 10;  -- 持久化 ANN
```

## 当前进度

| 里程碑 | 验证 | 状态 |
|---|---|---|
| M0 双形态注册链路 | `m0_static_smoke` + CLI `.load` | ✅ arm64 + x86_64 |
| M1 距离层（common SIMD dispatch） | `m1_distance_smoke` + 跨引擎 800 组对照（`m1_cross_engine_check.py`，float64 真值仲裁）+ DuckDB 回归 111 cases | ✅ |
| M2 虚拟表（shadow table 持久化 + 暴力 KNN） | `m2_vtab_smoke`（KNN 正确性/事务回滚/关库重开/错误路径） | ✅ arm64 + x86_64 |
| M3 HNSW（共享算法 × SQLite store，>64 行走图，`%_graph` blob 持久化） | `m3_hnsw_smoke`（recall@10=1.000、增量、重开 blob 还原、DELETE/ROLLBACK） | ✅ arm64 + x86_64 |
| M3+ 并行建图（rebuild 预读后多线程，publish fence，TSan 零 race） | `m3p_parallel_smoke`（N=40000、8 线程 ×3 轮 recall==串行 baseline） | ✅ |
| M4 spec 落地 | SQLite spec 32 passed / 0 failed（SQLite 3.46.0、3.53.4） | ✅ |
| PQ（共享训练、编码、ADC、SIMD） | full/compact、L2/cosine/IP、精确重排、增量和重启 | ✅ macOS arm64 |
| RaBitQ（共享量化器、图遍历、持久化） | L2/cosine/IP recall@10 均为 1.000 | ✅ macOS arm64 |
| M5 跨平台发版 | Linux、macOS、iOS XCFramework、Android、WASM | ✅ v0.0.17 |
| M4 spec 落地 | 多引擎 YAML spec | ✅ |
| M5 macOS 统一包 | `vexdb` + `vexfs` + dylib + App + FSKit | ✅ 技术预览 |

距离语义三 metric 统一 **lower = closer**（L2=sqrt、cosine=1-sim、ip=负内积），`ORDER BY distance ASC` 即最近优先。跨 ISA（NEON/SSE）允许 ~1e-6 级 float32 重排序分歧。

`quantizer=pq, pq_m=N` 开启 PQ，`quantizer=rabitq` 开启 RaBitQ，`quantizer=none` 关闭量化。PQ 的 K-means、编码、ADC 距离和 SIMD 分发直接复用 `common/`，SQLite 只实现虚拟表、事务、缓存和持久化适配。PQ 查询按 `ef_search × 1.25` 保留候选，再从 `%_vectors` 读取业务向量做精确重排；full 和 compact 使用同一搜索逻辑。

训练固定数据和每个节点的 code 会写入 `%_graph` shadow table，关库重开后直接恢复。设置 `memory_mode=compact` 后，`%_vectors` 中的业务向量仍保留，但索引 blob 不再写原始向量镜像；PQ / RaBitQ 的搜索、增量写入和重开都使用 code-aware 图路径。compact 未指定量化器时自动选择 PQ；显式 `quantizer=none` 会报错。

本地 Release 性能与召回 eval：

```bash
cmake -S vexdb_sqlite -B vexdb_sqlite/build-perf \
  -DCMAKE_BUILD_TYPE=Release -DVEXDB_SQLITE_BUILD_TESTS=OFF
cmake --build vexdb_sqlite/build-perf -j4
python3 vexdb_sqlite/test/quantizer_benchmark.py \
  vexdb_sqlite/build-perf/vexdb_lite.dylib --check
```

## 双形态分发（架构前置决策）

| 形态 | 适用 | 机制 |
|---|---|---|
| **静态注册**（默认） | 移动端 iOS/Android/WASM、可嵌入宿主 | amalgamation 静态链 + `vexdb_sqlite_register(db)` 或 `sqlite3_auto_extension`。`-DVEXDB_SQLITE_CORE=1` 直链真实 sqlite3 符号 |
| **loadable** `.so`/`.dylib` | 桌面/服务端 | 运行时 `.load ./vexdb_lite sqlite3_vexdblite_init`，经 `sqlite3ext.h` 间接表 |

> iOS 系统 libsqlite3 禁扩展加载、WASM 不支持运行时 `.load` → 移动端**只能**走静态注册。故默认形态是静态注册，loadable 仅桌面附加。

## 目录

```
vexdb_sqlite/
├── CMakeLists.txt           # 双形态产出：vexdb_lite(.so/.dylib) + vexdb_lite_static(.a)
├── vendor_sqlite.sh         # 拉取官方 SQLite amalgamation（≥3.38，默认 3.45.3）
├── include/
│   ├── vexdb_sqlite.h        # 公共入口：register / loadable init
│   ├── vexdb_sqlite_internal.h  # loadable vs core 头切换
│   └── vtab/graph_index_vtab.h
├── src/
│   ├── vexdb_sqlite_init.cpp  # 入口（两形态汇聚）+ vexdb_version()
│   └── vtab/graph_index_vtab.cpp  # GRAPH_INDEX 虚拟表模块（M0 只读骨架）
├── test/m0_static_smoke.c    # 静态注册冒烟
└── third_party/sqlite/       # vendored amalgamation（gitignore，跑 vendor_sqlite.sh 获取）
```

> 模块名 `GRAPH_INDEX` = 三端共享的索引类型名（DuckDB `TYPE_NAME` / DuckDB+PG `index_info` 报告名均为 `GRAPH_INDEX`）。

## 构建

```bash
# 推荐（需 cmake）：
bash build_sqlite.sh test     # vendor + 配置 + 编双形态 + 跑 M0 冒烟
bash build_sqlite.sh vendor   # 仅拉取 amalgamation
bash build_sqlite.sh clean

# 无 cmake 时的手动 fallback（M0 时点快照，源文件清单以 CMakeLists.txt 为准）：
cd vexdb_sqlite && bash vendor_sqlite.sh
INC="-Iinclude -Ithird_party/sqlite"
clang -O1 -c third_party/sqlite/sqlite3.c -Ithird_party/sqlite -DSQLITE_ENABLE_LOAD_EXTENSION=1 -o build/sqlite3.o
clang++ -std=c++17 -DVEXDB_SQLITE_CORE=1 $INC -c src/vexdb_sqlite_init.cpp -o build/init_core.o
clang++ -std=c++17 -DVEXDB_SQLITE_CORE=1 $INC -c src/vtab/graph_index_vtab.cpp -o build/vtab_core.o
clang -DVEXDB_SQLITE_CORE=1 $INC -c test/m0_static_smoke.c -o build/smoke.o
clang++ build/smoke.o build/init_core.o build/vtab_core.o build/sqlite3.o -lpthread -o build/m0_static_smoke
./build/m0_static_smoke
```

> macOS 注意：本机 anaconda clang 默认 target 是 x86_64，与系统 arm64 sqlite3 不匹配。
> CMake 已自动对齐 host 架构；手动编 loadable 时按需加 `-arch arm64`。

## 发布产物

[v0.0.17](https://github.com/VexDB-THU/VexDB-Lite/releases/tag/v0.0.17) 提供 Linux x86_64 / AArch64、macOS arm64 / x86_64、iOS XCFramework、Android arm64-v8a / x86_64 和 WASM 压缩包。本版本暂不提供 Windows 预编译包。

## 路线（详见计划文档）

- **Stage A 核心**（M0✅ → M1 距离层 → M2 暴力搜索 → M3 HNSW + M3+ 并行 → M4 spec → M5 桌面发版）
- **Stage B** HybridIndex（过滤查询）
- **Stage C** 移动端（iOS/Android，WASM 可选）

## 有界查询缓存与批量建图

服务端应复用已打开的连接和预备语句，并将连接数、索引缓存和原向量一起计入内存预算。每次请求重新打开大库、或交替切换大索引，与持久连接的延迟不同，应分别测量。[实际SQL稳定性对照](../docs/reports/2026-11-03_recall-and-pg-io-validation.md)保留了两种条件的结果；热连接稳定不能替代冷启动验收。

`graph_memory_limit`控制每个连接的图段缓存。大量连接可以使用较小的查询缓存，批量建图则可单独设置`graph_build_memory_limit`，单位均为字节。例如：

```sql
CREATE VIRTUAL TABLE vectors USING GRAPH_INDEX(
    embedding FLOAT[768], metric=l2,
    graph_memory_limit=41943040,
    graph_build_memory_limit=2147483648
);
```

这表示查询缓存40MiB，构建工作集预算2GiB。构建预算为0或省略时沿用旧规则：查询缓存的4倍。较大的构建预算会占用更多维护内存，可以减少分段反复读取，并让更多向量参与并行内存建图；这项成本需要计入建索引资源。段缓存以外的元数据、临时对象和SQLite缓存仍需额外内存，这些参数不是进程RSS硬上限。

也可以用`INSERT INTO vectors(vectors) VALUES('graph_build_memory_limit=2147483648')`调整后续构建预算。它随事务提交/回滚，不删除现有图，不改变查询缓存，也不自动重建已有索引。原有文件缺少这个配置键时继续使用旧默认。

## 可选过滤分流与辅助码补漏（开发分支）

SQLite目前没有PQ查询入口。原向量L2图可以显式试用辅助码补漏；过滤分流是另一个独立开关，默认均关闭：

```sql
BEGIN;
INSERT INTO vectors(vectors) VALUES('signature_build=42');
INSERT INTO vectors(vectors) VALUES('signature_rescue=on');
INSERT INTO vectors(vectors) VALUES('filtered_search_mode=auto');
COMMIT;

SELECT rowid, distance FROM vectors
WHERE embedding MATCH :query AND k=10 AND tenant_id=:tenant;
SELECT vexdb_search_info(vectors) FROM vectors LIMIT 1;
```

表须声明示例中的`tenant_id`元数据列。分流只按当前候选数、实际图节点数、维度和搜索预算估算工作量；候选较小时直接精算，其他情况继续图搜索。类型、NULL、排序规则与事务快照仍由SQLite约束，分页应用于最终结果。`filtered_search_mode=off`恢复旧分流策略；配置随事务提交和回滚。

启用补漏后，原向量L2图的辅助码缺失或失效时使用当前事务下的原向量精确搜索。这能避免静默退回较窄的图搜索，但可能明显变慢；重建后仍须显式生成辅助码。辅助码的逻辑大小及每连接实际分配容量都计入资源，原向量继续保留。

诊断函数参数是虚拟表的表名列，不是字符串；空表没有可供读取的行。输出包括实际路径、估距数、宽度和辅助码容量。它用于解释执行路径，不能自动证明某个近似搜索宽度在任意业务达到99.9%。已验证场景、超时与缺项见[公平对照与质量保护](../docs/reports/2026-11-02_search-fairness-and-quality-protection.md)。
