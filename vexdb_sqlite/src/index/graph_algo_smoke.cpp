// M3 地基编译冒烟：验证 GraphIndexAlgorithm × SQLite MemStore × 距离 dispatcher
// 的整条模板链在 PG_VEXDB_TARGET_SQLITE 下可实例化（编译期验证为主，附带一个
// 最小 build/search 运行检查，由 m3 测试调用）。
//
// 桥接模式照抄 DuckDB 端 RunWithDuckAlgo（graph_index.cpp:53）。
//
// include 顺序约定（duck 同款）：宿主依赖头必须先于 algorithm.h——vtl 容器头
// 依赖宿主提供的 Assert/unlikely 宏与 ItemPointerData 类型。
#include "vex_graph_index_depend_sqlite.hpp"
#include "graph_index/graph_index_algorithm.h"

#include "distance/core/distance.h"
#include "distance/core/distance_dispatcher.h"
#include "distance/core/distance_utils_core.h"

#include <cstring>
#include <vector>

namespace vexdb_sqlite {

using SqliteStore = MemStore<uint32, GraphIndexPoint>;

namespace {

using SqMetricList = MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::COSINE>;
using SqDTypeList = DistPrecisionTypeList<DistPrecisionType::FLOAT>;

template <typename Fn>
auto RunWithAlgo(Metric metric, uint16_t dim, int ef_construction, int m, SqliteStore &store,
                 Fn &&fn) {
    return DispatchRunner<false, SqMetricList, SqDTypeList, DispatcherMode::NO_QUANT>::call(
        metric, DistPrecisionType::FLOAT, dim, QuantizerType::NONE,
        [&](auto &distancer) -> decltype(auto) {
            using DistT = std::decay_t<decltype(distancer)>;
            using AlgoT = GraphIndexAlgorithm<SqliteStore, DistT>;
            AlgoT algo(uint_fast16_t(ef_construction), uint_fast16_t(m), store, distancer);
            return fn(algo);
        });
}

}  // namespace

// 最小运行检查：N 个向量建图 + 查询自身，返回 top1 row_id 是否正确。
// （完整桥接层 M3 后续填充；本函数供 m3 smoke 调用。）
bool GraphAlgoSelfTest() {
    constexpr uint16_t kDim = 8;
    constexpr int kN = 64;
    SqliteStore store(kDim, /*m=*/8, kDim * sizeof(float));

    std::vector<std::vector<float>> data(kN);
    for (int i = 0; i < kN; i++) {
        data[i].resize(kDim);
        for (int d = 0; d < kDim; d++) {
            data[i][d] = float(i) + 0.01f * float(d);
        }
    }

    bool ok = true;
    RunWithAlgo(Metric::L2, kDim, /*efc=*/64, /*m=*/8, store, [&](auto &algo) {
        using AlgoT = std::decay_t<decltype(algo)>;
        PointExtensionContext pctx;
        for (int i = 0; i < kN; i++) {
            ItemPointerData tid{};
            tid.row_id = i + 1;
            typename AlgoT::InsertContextBase ctx(
                pctx, reinterpret_cast<const char *>(data[i].data()), &tid);
            algo.insert(ctx);
        }
        // 查询第 17 个向量自身 → top1 必须是 row_id=18
        auto res = algo.search(pctx, reinterpret_cast<const char *>(data[17].data()),
                               /*ef_search=*/40);
        ok = !res.empty() && res[0].tid.row_id == 18;
        ann_helper::optional_destroy(res);
        return 0;
    });
    return ok;
}

}  // namespace vexdb_sqlite
