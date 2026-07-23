# vexdb_sqlite — VexDB-Lite 的 SQLite 适配层

SQLite 版在同一个扩展中提供 VexDB 自研图索引和 VexFS 文件能力。普通表、向量索引和文件可以保存在同一个数据库中，并由 SQLite 事务和 Backup API 统一管理。

> 完整计划：`docs/plans/2026-06-10_sqlite-adapter-v1-plan.md`
> 范围调研：`docs/research/2026-06-10_sqlite-v1-scope-reinvestigation.md`

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

CREATE VIRTUAL TABLE idx USING GRAPH_INDEX(embedding FLOAT[128], metric=cosine, m=16);
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
| M4 spec 落地 | 多引擎 YAML spec | ✅ |
| M5 macOS 统一包 | `vexdb` + `vexfs` + dylib + App + FSKit | ✅ 技术预览 |

距离语义三 metric 统一 **lower = closer**（L2=sqrt、cosine=1-sim、ip=负内积），`ORDER BY distance ASC` 即最近优先。跨 ISA（NEON/SSE）允许 ~1e-6 级 float32 重排序分歧。

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

## 路线（详见计划文档）

- **Stage A 核心**（M0✅ → M1 距离层 → M2 暴力搜索 → M3 HNSW + M3+ 并行 → M4 spec → M5 桌面发版）
- **Stage B** HybridIndex（过滤查询）
- **Stage C** 移动端（iOS/Android，WASM 可选）
