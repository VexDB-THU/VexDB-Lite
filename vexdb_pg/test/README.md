# PG PQ 回归

抽样单测不需要运行数据库：

```sh
c++ -std=c++17 -O2 -I vexdb_pg/include \
  vexdb_pg/test/pq_training_sample_test.cpp -o /tmp/vexdb-pq-training-test
/tmp/vexdb-pq-training-test
```

不要加`-DNDEBUG`，此单测使用assert。它覆盖内存容量、边界和有序输入下的抽样分布，不用固定某一批随机行号作为答案。

在已经预加载并安装本次扩展的独立测试数据库中运行SQL回归：

```sh
psql -v ON_ERROR_STOP=1 -f vexdb_pg/test/pq_sampling_regression.sql
```

测试事务最终回滚，覆盖未ANALYZE、有序/打乱写入、NULL、嵌套函数、64维切成32维及类型转换表达式索引。PG只在维护内存超过1GiB的内存构建路径启用PQ，因此SQL测试设2GiB，并显式断言两个索引的PQ都在工作。64维、2,048条非NULL行时，各应报告`sampled 2048 of 2048 eligible vectors`。抽样容量不足以容纳全表的分布检查由单测及100K原生回归另行覆盖。宽搜索检查不作为查询速度或任意数据99.9%召回的保证。

修复抽样不会改变已训练的码本。已有受影响的PQ索引须在维护窗口执行`REINDEX INDEX 索引名`，再做自己的质量和速度验收。产品默认搜索宽度没有因为测试而改变。


`heap_vector_reader_test.cpp`是仅用于测试的PG可加载模块，不进入产品扩展。它调用实际的表达式读取接口，逐行把结果与堆表原向量的坐标切片比较，并检查空缓存槽与物理槽0:0的状态；可独立于近似图召回检查表达式值。原生验收目录保留编译参数、模块哈希及注入这项检查的SQL。测试函数`test_heap_index_vector_reader(regclass, integer, integer, integer)`的参数依次为图索引、原向量列号、从0开始的切片起点、维度，返回实际比较的非NULL行数。必须在独立测试数据库中使用，本次64维和64→32维各比较2048行。


编译PG可加载测试模块时，必须使用产品构建的头文件顺序和类布局选项：`thirdparties/`在完整Boost fallback之前，并与产品一致地使用`PG_EXTENSION`、`PG_VEXDB_TARGET_PG`、`HAVE_CXX_TYPEOF_UNQUAL`、`NDEBUG`和`-fno-exceptions -fno-rtti`。上面的抽样独立单测仍不要使用NDEBUG，两者不同。不要只用fallback Boost编译一个访问产品缓存结构的测试模块：不同Boost版本的内部结构不兼容，检查本身可能崩溃。本次原生验收保留`helper-abi-build.json`及正确模块哈希。

## 候选补漏原生诊断（未接入产品）

`graph_completion_study.cpp`构建为独立测试模块，不由扩展安装脚本注册。`test_graph_completion_batch(regclass,floatvector[],integer,integer,integer,real)`仅支持U32、FLOAT、L2原图，参数依次为索引、查询数组、原宽度、正向额外估距百分比（0～10）、距离参考位置和相对距离界限；反向预算为10%减正向预算。它保留原候选及本次分数，以有限预算检查原边界和反向连边。所有分配通过PG memory context，检查中断，结束显式清理。

函数使用关系ShareLock保护诊断快照；一次调用扫描整张图、在语句内建立邻接快照和反向结构，输出图节点ID与阶段CPU。必须通过相应heap TID映射再核对表主键。**这是冻结图诊断，不是产品ANN SQL、连接间缓存或并发写入方案；批量CPU也不能用来报告ANN QPS/P95。** 原生SQL耗时、模型初始化及事务代价另测。

2026-10-30固定共同参数复核：128条单批次触发120秒超时；阶段日志版按8条分批完成128条，原搜索99.765625%，补漏99.843750%，质量未通过。未含结构初始化的补漏CPU约增加11.66%，也未通过建议成本门槛。两次副本、批次和缓存状态不同，不认定超时根因已经解决。当前源码对应归档的阶段日志版。源码、构建命令、模型映射、失败、返回ID和审查器见`docs/reports/2026-10-30_candidate-completion-results.md`及其证据附件。
