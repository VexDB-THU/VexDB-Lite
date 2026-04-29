# DuckDB Search Path Report

**Date:** 2026-04-29  
**Repo:** `/Users/sunji/Work/PG_VEXDB`  
**Scope:** `vexdb-duck` 当前查询路径是否从磁盘读取，还是纯内存读取

## 结论

当前 `vexdb-duck` 的向量索引查询路径是：

1. **图索引搜索本身是纯内存读取**
2. **搜索完成后的结果行回表是通过 DuckDB 表存储读取**

所以更准确地说：

- **ANN graph search**：纯内存
- **base table row fetch**：走 DuckDB 存储层 `Fetch(...)`

它不是 PostgreSQL 那种从磁盘页或独立索引文件中读取图结构，也不是当前 DuckDB 中“索引在磁盘、查询时按需映射”的方案。

## 当前方案概述

当前 DuckDB 实现没有把图索引持久化到独立磁盘结构中，也没有实现 `Serialize/Deserialize` 索引状态恢复。  
索引是在 `CREATE INDEX` 时构建为进程内内存对象，查询时直接使用这份内存对象进行 ANN 搜索。

核心含义：

- `CREATE INDEX` 后，同一进程内可直接查询
- 查询时不会从磁盘重建图，也不会从索引文件读取邻接表/向量
- 如果进程结束或重新打开数据库，当前版本并不保证能恢复这份图索引状态

## 证据链

### 1. `BuildBulk(...)` 直接把图建到内存 `runtime_` 里

实现位置：

- [vexdb-duck/index/graph_index.cpp](/Users/sunji/Work/PG_VEXDB/vexdb-duck/index/graph_index.cpp:1)

当前 `GraphIndex` 在 `BuildBulk(...)` 中做了这些事：

- 把输入向量保存在 `vectors_`
- 把输入行号保存在 `row_ids_`
- 创建 `runtime_ = make_uniq<GraphIndexRuntimeState>(...)`
- `runtime_->store` 是 Duck 版 `MemStore`
- 然后调用共享算法 `GraphIndexAlgorithm<DuckStore, DuckDistancer>::insert(...)`

这说明图结构是在 `runtime_->store` 里直接生成的，而不是写到磁盘索引页之后再读取。

### 2. `SearchANN(...)` 直接搜索 `runtime_->store`

实现位置：

- [vexdb-duck/index/graph_index.cpp](/Users/sunji/Work/PG_VEXDB/vexdb-duck/index/graph_index.cpp:1)

当前 `SearchANN(...)` 路径：

1. 检查 `runtime_` 是否存在
2. 取出 `runtime_->store`
3. 构造共享算法对象
4. 调用 `algo.search(...)`

这里没有任何：

- `Deserialize`
- `IndexStorageInfo`
- `FileHandle`
- `DuckDB` block manager 读取邻接结构
- 索引文件读取

所以 ANN 搜索本身明确是内存态运行。

### 3. Duck 版 `MemStore` 的向量和邻接都在 `std::vector` 中

实现位置：

- [vexdb-duck/include/vex_graph_index_depend_duck.hpp](/Users/sunji/Work/PG_VEXDB/vexdb-duck/include/vex_graph_index_depend_duck.hpp:1)

当前 Duck 版 `MemStore` 持有的核心状态包括：

- `std::vector<point_type> elems`
- `std::vector<std::vector<char>> vectors`
- `std::vector<BasePointRec> base_points`
- `std::vector<UpperPointRec> upper_points`
- `GraphIndexEntryInfo entry_info`

其中：

- `vectors` 保存原始向量
- `base_points` 保存 base layer 邻接
- `upper_points` 保存 upper layer 邻接

这些都是常规进程内内存容器，没有任何磁盘读接口。

### 4. 查询执行器先 ANN，再回表

实现位置：

- [vexdb-duck/optimizer/vex_physical_index_scan.cpp](/Users/sunji/Work/PG_VEXDB/vexdb-duck/optimizer/vex_physical_index_scan.cpp:1)
- [vexdb-duck/include/vex_fetch_utils.hpp](/Users/sunji/Work/PG_VEXDB/vexdb-duck/include/vex_fetch_utils.hpp:1)

当前 `PhysicalVexIndexScan::Execute(...)` 做的是：

1. 计算 query vector
2. 调用 `graph_index.SearchANN(...)`
3. 得到 `vector<row_t>` 和距离
4. 再调用 `FetchRowsByRowIds(...)`
5. `FetchRowsByRowIds(...)` 内部调用 `storage.Fetch(...)`

所以“查询”被拆成两段：

- 第一段：索引搜索，纯内存
- 第二段：结果行读取，DuckDB 存储读取

## 读取路径分解

### A. 向量搜索阶段

数据来源：

- `runtime_->store.vectors`
- `runtime_->store.base_points`
- `runtime_->store.upper_points`

读取方式：

- 直接从内存容器取数据
- 通过共享算法访问邻接和向量

当前不是：

- 从 DuckDB table 重新取原始向量后 brute-force
- 从磁盘索引结构按需读取节点

### B. 回表阶段

数据来源：

- DuckDB `DataTable`

读取方式：

- `DuckTableEntry::GetStorage()`
- `storage.Fetch(transaction, fetch_chunk, fetch_col_ids, row_id_vec, batch, fetch_state)`

因此回表本身属于 DuckDB 的正常存储读取路径。  
这里是否真正触磁盘，取决于 DuckDB 自己的数据页是否已在缓存中，但这已经不是 `vexdb-duck` 的索引图读取逻辑了。

## 当前不是哪种方案

当前实现**不是**下面这些方案：

### 1. 不是“索引在磁盘，查询时按需读取索引节点”

原因：

- 没有 DuckDB 专用索引页格式
- 没有节点级 block 读取代码
- 没有 `Serialize/Deserialize` 索引恢复路径

### 2. 不是“查询时从表里把所有向量读出来再做 brute-force”

原因：

- `SearchANN(...)` 直接使用 `runtime_->store`
- 共享算法运行在 Duck 版内存图结构上

### 3. 不是 PostgreSQL 版 `_vec` 文件方案

原因：

- DuckDB 分支没有 `vector_smgr`
- 没有 `_vec` side file
- 没有 PG `Relation/Buffer/Page` I/O

## 当前设计的优缺点

### 优点

- 实现简单，路径短
- 共享算法接入阻力小
- 查询阶段延迟低，因为图结构已在内存中
- 便于快速验证 `pg_vexdb` 算法迁移是否正确

### 缺点

- 没有索引持久化恢复
- 进程重启后索引状态不可依赖
- 内存占用随图和向量规模线性增长
- 当前更像“内存型索引插件”，还不是完整的磁盘索引插件

## 对你问题的直接回答

如果只问：

> 现在 duckdb 的 search 是从磁盘读取的，还是纯内存读取？

答案是：

> **ANN 搜索本身是纯内存读取。**

如果问得更完整一点：

> 整个查询流程是纯内存还是会读磁盘？

答案是：

> **图索引搜索是纯内存；搜索后的结果行回表通过 DuckDB 存储层读取。**

## 后续如果要做“磁盘型搜索”需要什么

要把当前方案改成“查询时可从磁盘恢复/读取索引”，至少需要补这些能力：

1. `GraphIndex` 的 `SerializeToDisk / SerializeToWAL`
2. 索引 reopen 后的 `Deserialize`
3. DuckDB 侧持久化图结构格式
4. 查询阶段从持久化结构恢复或按需读取节点/向量
5. 明确索引内存缓存策略

当前版本还没有这些。

