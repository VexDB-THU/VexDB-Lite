// GraphBridge —— vtab 与 GraphIndexAlgorithm 之间的桥（M3）。
//
// 轻头：算法重模板全部藏在 graph_bridge.cpp（pimpl），vtab 只见这个接口。
// 单线程（SQLite 串行模型）；M3+ 并行建图在 cpp 内扩展。
//
// 距离输出为"用户语义"（与 M2 暴力路径/标量函数严格一致，lower=closer）：
//   L2 = sqrt(raw)，COSINE = 1 + raw（query 已归一化），IP = raw（即负内积）。
#ifndef VEXDB_SQLITE_GRAPH_BRIDGE_H
#define VEXDB_SQLITE_GRAPH_BRIDGE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "quantizer_type.h"
#include "vex_distance_entry.h"

namespace vexdb_sqlite {

class GraphBridge {
public:
    GraphBridge(uint16_t dim, int m, int ef_construction, VexMetric metric,
                QuantizerType quantizer = QuantizerType::NONE, uint32_t pq_m = 0,
                bool compact_mode = false);
    GraphBridge(uint16_t dim, int m, int ef_construction, VexMetric metric,
                bool use_rabitq, bool compact_mode = false)
        : GraphBridge(dim, m, ef_construction, metric,
                      use_rabitq ? QuantizerType::RABITQ : QuantizerType::NONE,
                      0, compact_mode) {}
    ~GraphBridge();
    GraphBridge(const GraphBridge &) = delete;
    GraphBridge &operator=(const GraphBridge &) = delete;

    // 增量插入一条向量（HNSW algo.insert，真增量非重建）。
    void Insert(const float *vec, int64_t rowid);

    // 批量建图（M3+）：vecs 平铺 n×dim，rowids 长度 n。n_threads<=1 退化串行；
    // 多线程时三段式（首点串行 → 连续切片 spawn worker），调用方保证构建期独占
    // 且 vecs/rowids 在返回前有效。线程为纯计算，绝不触碰 sqlite3 句柄。
    void BuildBulk(const float *vecs, const int64_t *rowids, size_t n, int n_threads);

    // KNN：返回 (用户语义 distance, rowid) 升序，最多 k 条。
    // cosine 时内部归一化 query 副本。
    void Search(const float *query, size_t k, uint32_t ef_search,
                std::vector<std::pair<double, int64_t>> &out);

    // 带谓词 KNN（M7' 图内过滤）：filter 返回 false 的 rowid 不进结果，
    // 但其节点仍参与图遍历导航（filtered-HNSW 标准语义，连通性不受谓词影响）。
    // ef_search 由调用方按选择性补偿后传入；filter 为空等价无过滤。
    void Search(const float *query, size_t k, uint32_t ef_search,
                const std::function<bool(int64_t)> &filter,
                std::vector<std::pair<double, int64_t>> &out);

    size_t Count() const;
    QuantizerType Quantizer() const;
    bool UsesPQ() const;
    bool UsesRaBitQ() const;
    bool IsCompactMode() const;

    // ---- tid 摘除（DELETE/UPDATE 增量化，对齐 MySQL 二级索引 delete-mark+
    // insert 范式）----
    // 把 rowid 从其节点的 tids 里摘除：节点变空壳后天然零输出（查询展开
    // tids 产行）但仍参与 HNSW 导航（保连通性）。UPDATE 向量 = RemoveTid(旧)
    // + Insert(新)——同 rowid 旧空壳与新节点无二义。elems 变更随 SerializeV2
    // 持久化（无需独立 deleted 段）。返回 false=rowid 不在图中。
    // 空壳（dead）节点占 ef 槽位，Search 内按 dead 比补偿；占比超阈值时
    // 调用方应作废重建（图质量与空间回收，对应 MySQL purge）。
    bool RemoveTid(int64_t rowid);
    size_t DeadNodeCount() const;

    // ---- 格式 v2：段式（M9'，%_graph(kind, seg, data)）----
    // kind：0=meta 1=elems 2=upper 3=base 段 4=vec 段 5=量化器固定数据
    // 6=量化 code 段（记录段按 64 条切分）。read 返回 false=段不存在；
    // write 必须在宿主写事务内调用。
    using SegReadFn = std::function<bool(int kind, uint32_t seg, std::vector<char> &out)>;
    using SegWriteFn = std::function<bool(int kind, uint32_t seg, const std::vector<char> &data)>;
    // 记录粒度读（M9'b）：只读段 blob 的 [offset, offset+len) 进 dst（建议用
    // sqlite3_blob_read 增量 I/O——只触达 offset 所在页）。
    using SegRecReadFn =
        std::function<bool(int kind, uint32_t seg, size_t offset, size_t len, char *dst)>;

    // 是否 DiskStore 模式（OpenV2Disk 打开；内存有界，段 LRU 懒加载）。
    bool IsDiskMode() const;
    // DiskStore 三类页（base/vec/量化 code）的统一缓存观测值；mem 模式为 0。
    size_t CacheBytesUsed() const;
    size_t CacheBudgetBytes() const;
    // DiskStore 模式专用：有未落盘的 dirty 段/常驻改动（全内存模式恒 false，
    // dirty 由 vtab 的 graph_dirty 管）。
    bool HasDirty() const;

    // 序列化写出 v2 段。两模式统一增量协议：dirty 段 UPSERT + 常驻三件套
    //（meta/elems/upper，体量小全量重写）。全内存模式在 NeedsFullRewrite()
    //（重建/BuildBulk 后首次落盘）时全量切段——调用方须先清旧段。
    // 返回 false=write 回调失败（失败段保留 dirty 供重试）。
    bool SerializeV2(const SegWriteFn &write);

    // mem 模式落盘前询问：true=调用方需先清段（InvalidatePersistedGraph）再
    // SerializeV2 全量；false=增量 UPSERT，段保持。disk 模式恒 false。
    bool NeedsFullRewrite() const;

    // 从 v2 段全量载入（全内存模式，性能形态）。无 meta 段/校验失败返回
    // nullptr 并填 err。
    static std::unique_ptr<GraphBridge> OpenV2(const SegReadFn &read, uint16_t dim, int m,
                                               int ef_construction, VexMetric metric,
                                               QuantizerType quantizer, uint32_t pq_m,
                                               bool compact_mode,
                                               std::string &err);
    static std::unique_ptr<GraphBridge> OpenV2(const SegReadFn &read, uint16_t dim, int m,
                                               int ef_construction, VexMetric metric,
                                               bool use_rabitq, bool compact_mode,
                                               std::string &err) {
        return OpenV2(read, dim, m, ef_construction, metric,
                      use_rabitq ? QuantizerType::RABITQ : QuantizerType::NONE,
                      0, compact_mode, err);
    }
    static std::unique_ptr<GraphBridge> OpenV2(const SegReadFn &read, uint16_t dim, int m,
                                               int ef_construction, VexMetric metric,
                                               bool use_rabitq, std::string &err) {
        return OpenV2(read, dim, m, ef_construction, metric, use_rabitq, false, err);
    }

    // 读持久化 meta 头的节点数（含空壳；-1=无段/不可读）。over_limit 模式
    // 分流用：实际加载体量由持久化 base_count 决定，存活行数在删除后会低估
    //（差额最大 = 重建阈值 20%），用行数判会在边界上选错模式、超限载入。
    static int64_t PeekPersistedNodeCount(const SegReadFn &read);

    // DiskStore 懒加载打开（内存有界形态）：meta/elems/upper 常驻，base/vec
    // 段进缓存直到预算线（缓存冻结策略），之后读 miss 经 read_rec 直读单
    // 记录（防 thrash）。write 用于写事务内 dirty 段 evict 写回与 xSync flush；
    // read_rec 为空时读 miss 退化整段载入+驱逐（不推荐，仅兼容）。
    static std::unique_ptr<GraphBridge> OpenV2Disk(const SegReadFn &read, const SegWriteFn &write,
                                                   const SegRecReadFn &read_rec, uint16_t dim,
                                                   int m, int ef_construction, VexMetric metric,
                                                   QuantizerType quantizer, uint32_t pq_m,
                                                   bool compact_mode,
                                                   size_t cache_budget,
                                                   std::string &err);
    static std::unique_ptr<GraphBridge> OpenV2Disk(const SegReadFn &read,
                                                   const SegWriteFn &write,
                                                   const SegRecReadFn &read_rec, uint16_t dim,
                                                   int m, int ef_construction, VexMetric metric,
                                                   bool use_rabitq, bool compact_mode,
                                                   size_t cache_budget,
                                                   std::string &err) {
        return OpenV2Disk(read, write, read_rec, dim, m, ef_construction, metric,
                          use_rabitq ? QuantizerType::RABITQ : QuantizerType::NONE,
                          0, compact_mode, cache_budget, err);
    }
    static std::unique_ptr<GraphBridge> OpenV2Disk(const SegReadFn &read,
                                                   const SegWriteFn &write,
                                                   const SegRecReadFn &read_rec, uint16_t dim,
                                                   int m, int ef_construction, VexMetric metric,
                                                   bool use_rabitq, size_t cache_budget,
                                                   std::string &err) {
        return OpenV2Disk(read, write, read_rec, dim, m, ef_construction, metric,
                          use_rabitq, false, cache_budget, err);
    }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vexdb_sqlite

#endif  // VEXDB_SQLITE_GRAPH_BRIDGE_H
