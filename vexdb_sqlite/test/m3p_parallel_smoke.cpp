// M3+ 并行建图冒烟（直接调 GraphBridge，绕过 SQL 层专测并发正确性）。
//
// 验收（计划 M3+）：并行 recall 必须对齐单线程 baseline（防 duck 端 pw=2
// recall 暴跌 83.57% 的前科）；N=40000 > 32768（吃透多次外层扩容/快慢路径
// 切换）；并行多轮重跑（概率性 race 需重复采样）；并行图序列化 round-trip。
// 线程合法性由结构保证：BuildBulk 输入是纯内存数组，worker 不碰 sqlite3。
#include "index/graph_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

using vexdb_sqlite::GraphBridge;

namespace {

constexpr int kDim = 16;
#ifdef M3P_SMALL_N
constexpr size_t kN = M3P_SMALL_N;  // TSan 等慢速 instrument 下缩规模
#else
constexpr size_t kN = 40000;
#endif
constexpr size_t kQueries = 10;
constexpr size_t kK = 10;
constexpr int kParallelRounds = 3;

int g_fail = 0;

void check(bool ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "M3+ FAIL: %s\n", what);
        g_fail = 1;
    }
}

unsigned int g_seed = 20260610;
float frand() {
    g_seed = g_seed * 1103515245u + 12345u;
    return float((g_seed >> 8) & 0xFFFF) / 65536.0f * 2.0f - 1.0f;
}

float L2(const float *a, const float *b) {
    float s = 0;
    for (int d = 0; d < kDim; d++) {
        float diff = a[d] - b[d];
        s += diff * diff;
    }
    return std::sqrt(s);
}

// 暴力 ground truth top-k（纯内存）。
void BruteTopK(const std::vector<float> &data, const float *q, std::vector<int64_t> &ids) {
    std::vector<std::pair<float, int64_t>> all(kN);
    for (size_t i = 0; i < kN; i++) {
        all[i] = {L2(data.data() + i * kDim, q), int64_t(i + 1)};
    }
    std::partial_sort(all.begin(), all.begin() + kK, all.end());
    ids.clear();
    for (size_t i = 0; i < kK; i++) ids.push_back(all[i].second);
}

double RecallOf(GraphBridge &g, const std::vector<float> &data,
                const std::vector<std::vector<int64_t>> &truth) {
    size_t hit = 0;
    for (size_t qi = 0; qi < kQueries; qi++) {
        std::vector<std::pair<double, int64_t>> res;
        g.Search(data.data() + (qi * 977 % kN) * kDim, kK, /*ef_search=*/120, res);
        for (const auto &r : res) {
            for (int64_t t : truth[qi]) {
                if (r.second == t) { hit++; break; }
            }
        }
    }
    return double(hit) / double(kQueries * kK);
}

}  // namespace

int main() {
    std::vector<float> data(kN * kDim);
    for (auto &v : data) v = frand();
    std::vector<int64_t> rowids(kN);
    for (size_t i = 0; i < kN; i++) rowids[i] = int64_t(i + 1);

    std::vector<std::vector<int64_t>> truth(kQueries);
    for (size_t qi = 0; qi < kQueries; qi++) {
        BruteTopK(data, data.data() + (qi * 977 % kN) * kDim, truth[qi]);
    }

    // ── 单线程 baseline ──
    double recall_serial;
    {
        GraphBridge g(kDim, /*m=*/16, /*efc=*/100, VexMetric::L2);
        g.BuildBulk(data.data(), rowids.data(), kN, /*n_threads=*/1);
        check(g.Count() == kN, "serial count == N");
        recall_serial = RecallOf(g, data, truth);
        printf("serial   recall@%zu = %.4f (N=%zu)\n", kK, recall_serial, kN);
        check(recall_serial >= 0.95, "serial recall >= 0.95");
    }

    // ── 并行 ×3 轮：recall 必须对齐 baseline（容差 0.02，防暴跌型 race） ──
    for (int round = 0; round < kParallelRounds; round++) {
        GraphBridge g(kDim, 16, 100, VexMetric::L2);
        g.BuildBulk(data.data(), rowids.data(), kN, /*n_threads=*/8);
        check(g.Count() == kN, "parallel count == N");
        double r = RecallOf(g, data, truth);
        printf("parallel recall@%zu = %.4f (round %d, 8 threads)\n", kK, r, round + 1);
        check(r >= recall_serial - 0.02, "parallel recall aligns serial baseline");

        if (round == 0) {
            // 并行图 v2 段式序列化 round-trip（内存 map 模拟 %_graph 段存储）：
            // 全内存 load 与 DiskStore 懒加载两条路径 recall 都必须不变。
            std::map<std::pair<int, uint32_t>, std::vector<char>> segs;
            bool ser_ok = g.SerializeV2([&](int kind, uint32_t seg, const std::vector<char> &d) {
                segs[{kind, seg}] = d;
                return true;
            });
            check(ser_ok, "v2 serialize ok");
            auto reader = [&](int kind, uint32_t seg, std::vector<char> &out) -> bool {
                auto it = segs.find({kind, seg});
                if (it == segs.end()) return false;
                out = it->second;
                return true;
            };
            std::string err;
            auto g2 = GraphBridge::OpenV2(reader, kDim, 16, 100, VexMetric::L2, false, err);
            check(g2 != nullptr, "v2 full load ok");
            if (g2) {
                double r2 = RecallOf(*g2, data, truth);
                check(std::fabs(r2 - r) < 1e-9, "v2 round-trip recall identical");
            }
            auto rec_reader = [&](int kind, uint32_t seg, size_t offset, size_t len,
                                  char *dst) -> bool {
                auto it = segs.find({kind, seg});
                if (it == segs.end() || offset + len > it->second.size()) return false;
                std::memcpy(dst, it->second.data() + offset, len);
                return true;
            };
            // DiskStore：预算压到最小（缓存冻结后全程记录直读），recall 仍须逐位一致
            auto g3 = GraphBridge::OpenV2Disk(reader, nullptr, rec_reader, kDim, 16, 100,
                                              VexMetric::L2, false, /*cache_budget=*/1, err);
            check(g3 != nullptr, "v2 disk open ok");
            if (g3) {
                double r3 = RecallOf(*g3, data, truth);
                check(std::fabs(r3 - r) < 1e-9, "disk-mode recall identical under min budget");
            }
            // 无 read_rec（退化整段 LRU 路径）也必须正确
            auto g4 = GraphBridge::OpenV2Disk(reader, nullptr, nullptr, kDim, 16, 100,
                                              VexMetric::L2, false, /*cache_budget=*/1, err);
            check(g4 != nullptr, "v2 disk open (no rec reader) ok");
            if (g4) {
                double r4 = RecallOf(*g4, data, truth);
                check(std::fabs(r4 - r) < 1e-9, "disk-mode (seg fallback) recall identical");
            }

            // v4 增量元数据门槛：单行 DiskStore insert 只能写一个 elems
            // 小段和有限个 upper 小段，不能退回整表 O(N) BLOB 重写。
            size_t elem_writes = 0;
            size_t upper_writes = 0;
            size_t elem_bytes = 0;
            auto bounded_writer = [&](int kind, uint32_t seg,
                                      const std::vector<char> &d) -> bool {
                segs[{kind, seg}] = d;
                if (kind == 1) {
                    elem_writes++;
                    elem_bytes += d.size();
                } else if (kind == 2) {
                    upper_writes++;
                }
                return true;
            };
            auto g5 = GraphBridge::OpenV2Disk(
                reader, bounded_writer, rec_reader, kDim, 16, 100,
                VexMetric::L2, false, /*cache_budget=*/1, err);
            check(g5 != nullptr, "v4 disk open for bounded metadata write ok");
            if (g5) {
                std::vector<float> added(kDim, 11.0f);
                g5->Insert(added.data(), 880000);
                check(g5->SerializeV2(bounded_writer),
                      "v4 bounded metadata dirty flush ok");
                check(elem_writes == 1, "single insert writes exactly one elems segment");
                check(elem_bytes <= 4096, "single insert elems write stays page-sized");
                check(upper_writes <= 32,
                      "single insert writes a bounded number of upper segments");
            }

            // RaBitQ 直接持久化测试：绕过 SQL 层的损坏后自动重建，证明
            // OpenV2/OpenV2Disk 本身能恢复量化器，并拒绝损坏的固定数据和编码。
            std::map<std::pair<int, uint32_t>, std::vector<char>> empty_rq_segs;
            GraphBridge empty_rq(kDim, 16, 100, VexMetric::L2, true);
            check(empty_rq.SerializeV2([&](int kind, uint32_t seg,
                                           const std::vector<char> &d) {
                empty_rq_segs[{kind, seg}] = d;
                return true;
            }), "empty RaBitQ serialize ok");
            auto empty_reader = [&](int kind, uint32_t seg, std::vector<char> &out) -> bool {
                auto it = empty_rq_segs.find({kind, seg});
                if (it == empty_rq_segs.end()) return false;
                out = it->second;
                return true;
            };
            auto empty_loaded = GraphBridge::OpenV2(
                empty_reader, kDim, 16, 100, VexMetric::L2, true, err);
            check(empty_loaded != nullptr && !empty_loaded->UsesRaBitQ(),
                  "empty RaBitQ round-trip waits for first insert");

            GraphBridge rq(kDim, 16, 100, VexMetric::L2, true);
            rq.BuildBulk(data.data(), rowids.data(), kN, /*n_threads=*/8);
            check(rq.UsesRaBitQ(), "RaBitQ enabled after bulk build");
            std::map<std::pair<int, uint32_t>, std::vector<char>> rq_segs;
            check(rq.SerializeV2([&](int kind, uint32_t seg, const std::vector<char> &d) {
                rq_segs[{kind, seg}] = d;
                return true;
            }), "RaBitQ serialize ok");
            size_t rq_code_segments = 0;
            for (const auto &entry : rq_segs) rq_code_segments += entry.first.first == 6;
            check(rq_code_segments > 1, "RaBitQ codes are segmented");
            auto rq_reader = [&](int kind, uint32_t seg, std::vector<char> &out) -> bool {
                auto it = rq_segs.find({kind, seg});
                if (it == rq_segs.end()) return false;
                out = it->second;
                return true;
            };
            auto rq2 = GraphBridge::OpenV2(
                rq_reader, kDim, 16, 100, VexMetric::L2, true, err);
            check(rq2 != nullptr && rq2->UsesRaBitQ(), "RaBitQ full load active");
            if (rq2) check(RecallOf(*rq2, data, truth) >= 0.90,
                           "RaBitQ full load recall >= 0.90");
            auto rq3 = GraphBridge::OpenV2Disk(
                rq_reader, nullptr, nullptr, kDim, 16, 100, VexMetric::L2, true,
                /*cache_budget=*/1, err);
            check(rq3 != nullptr && rq3->UsesRaBitQ(), "RaBitQ disk load active");
            if (rq3) {
                check(RecallOf(*rq3, data, truth) >= 0.90,
                      "RaBitQ disk load recall >= 0.90");
                check(rq3->CacheBytesUsed() <= rq3->CacheBudgetBytes(),
                      "RaBitQ code/base/vec cache stays within shared budget");
            }

            auto rq_rec_reader = [&](int kind, uint32_t seg, size_t offset, size_t len,
                                     char *dst) -> bool {
                auto it = rq_segs.find({kind, seg});
                if (it == rq_segs.end() || offset + len > it->second.size()) return false;
                std::memcpy(dst, it->second.data() + offset, len);
                return true;
            };
            auto rq_writer = [&](int kind, uint32_t seg, const std::vector<char> &d) -> bool {
                rq_segs[{kind, seg}] = d;
                return true;
            };
            auto rq4 = GraphBridge::OpenV2Disk(
                rq_reader, rq_writer, rq_rec_reader, kDim, 16, 100, VexMetric::L2, true,
                /*cache_budget=*/1, err);
            check(rq4 != nullptr && rq4->UsesRaBitQ(),
                  "RaBitQ record-read disk load active");
            if (rq4) {
                check(RecallOf(*rq4, data, truth) >= 0.90,
                      "RaBitQ record-read recall >= 0.90");
                std::vector<float> added(kDim, 9.0f);
                rq4->Insert(added.data(), 900000);
                check(rq4->Count() == kN + 1, "RaBitQ disk incremental code appended");
                std::vector<std::pair<double, int64_t>> added_result;
                rq4->Search(added.data(), 1, 160, added_result);
                check(!added_result.empty() && added_result[0].second == 900000,
                      "RaBitQ disk incremental row searchable");
                check(rq4->SerializeV2(rq_writer), "RaBitQ disk dirty code flush ok");
                check(rq4->CacheBytesUsed() <= rq4->CacheBudgetBytes(),
                      "RaBitQ dirty cache stays within shared budget");

                std::string reopen_err;
                auto rq5 = GraphBridge::OpenV2Disk(
                    rq_reader, nullptr, rq_rec_reader, kDim, 16, 100, VexMetric::L2, true,
                    /*cache_budget=*/1, reopen_err);
                check(rq5 != nullptr && rq5->UsesRaBitQ() && rq5->Count() == kN + 1,
                      "RaBitQ full last code segment reopens after insert");
            }

            auto expect_bad_rq = [&](auto mutate, const char *what) {
                auto bad = rq_segs;
                mutate(bad);
                auto bad_reader = [&](int kind, uint32_t seg, std::vector<char> &out) -> bool {
                    auto it = bad.find({kind, seg});
                    if (it == bad.end()) return false;
                    out = it->second;
                    return true;
                };
                std::string bad_err;
                auto opened = GraphBridge::OpenV2(
                    bad_reader, kDim, 16, 100, VexMetric::L2, true, bad_err);
                check(opened == nullptr && !bad_err.empty(), what);
            };
            expect_bad_rq([](auto &bad) { bad.erase({5, 0}); },
                          "RaBitQ missing fixed segment rejected");
            expect_bad_rq([](auto &bad) { bad[{6, 0}].pop_back(); },
                          "RaBitQ truncated code segment rejected");
            expect_bad_rq([](auto &bad) { bad[{5, 0}][4] = 0; },
                          "RaBitQ unsupported fixed version rejected");
            expect_bad_rq([](auto &bad) {
                bad[{6, 0}][0] = char(0xff);
                bad[{6, 0}][1] = char(0xff);
            }, "RaBitQ invalid cluster id rejected");

            // PQ 直接持久化测试：SQLite 只负责宿主存储，训练、编码和距离
            // lookup 复用 common/quantizer 与 common/distance。compact 模式下
            // 不允许出现 kind=4 原始向量镜像。
            constexpr size_t pq_n = 512;
            GraphBridge pq(kDim, 16, 100, VexMetric::L2,
                           QuantizerType::PQ, /*pq_m=*/4, /*compact=*/true);
            pq.BuildBulk(data.data(), rowids.data(), pq_n, /*n_threads=*/4);
            check(pq.UsesPQ() && pq.IsCompactMode(), "PQ compact enabled after bulk build");

            std::map<std::pair<int, uint32_t>, std::vector<char>> pq_segs;
            auto pq_writer = [&](int kind, uint32_t seg,
                                 const std::vector<char> &d) -> bool {
                pq_segs[{kind, seg}] = d;
                return true;
            };
            check(pq.SerializeV2(pq_writer), "PQ compact serialize ok");
            check(pq_segs.count({4, 0}) == 0, "PQ compact omits raw vector segments");
            check(pq_segs.count({5, 0}) == 1 && pq_segs.count({6, 0}) == 1,
                  "PQ codebook and codes persisted");

            auto pq_reader = [&](int kind, uint32_t seg, std::vector<char> &out) -> bool {
                auto it = pq_segs.find({kind, seg});
                if (it == pq_segs.end()) return false;
                out = it->second;
                return true;
            };
            auto pq_contains = [&](GraphBridge &index, const float *query,
                                   int64_t expected) -> bool {
                std::vector<std::pair<double, int64_t>> result;
                index.Search(query, 64, 160, result);
                return std::any_of(result.begin(), result.end(), [&](const auto &item) {
                    return item.second == expected;
                });
            };

            auto pq2 = GraphBridge::OpenV2(
                pq_reader, kDim, 16, 100, VexMetric::L2,
                QuantizerType::PQ, /*pq_m=*/4, /*compact=*/true, err);
            check(pq2 != nullptr && pq2->UsesPQ() && pq2->IsCompactMode(),
                  "PQ compact full load active");
            if (pq2) {
                check(pq_contains(*pq2, data.data() + 37 * kDim, rowids[37]),
                      "PQ compact full load row searchable");
            }

            auto pq_rec_reader = [&](int kind, uint32_t seg, size_t offset, size_t len,
                                     char *dst) -> bool {
                auto it = pq_segs.find({kind, seg});
                if (it == pq_segs.end() || offset + len > it->second.size()) return false;
                std::memcpy(dst, it->second.data() + offset, len);
                return true;
            };
            auto pq3 = GraphBridge::OpenV2Disk(
                pq_reader, pq_writer, pq_rec_reader, kDim, 16, 100, VexMetric::L2,
                QuantizerType::PQ, /*pq_m=*/4, /*compact=*/true,
                /*cache_budget=*/1, err);
            check(pq3 != nullptr && pq3->UsesPQ() && pq3->IsCompactMode(),
                  "PQ compact disk load active");
            if (pq3) {
                check(pq_contains(*pq3, data.data() + 73 * kDim, rowids[73]),
                      "PQ compact disk load row searchable");
                // 每个 PQ 子向量都取自训练样本，但组合成训练集中没有的新向量；
                // 这样能测到真正的增量 code append，又不会把“训练范围外极端值
                // 的量化误差”误判成持久化错误。
                std::vector<float> added(kDim);
                constexpr size_t pq_subdim = kDim / 4;
                for (size_t sub = 0; sub < 4; sub++) {
                    const float *source = data.data() + (100 + sub) * kDim;
                    std::copy(source + sub * pq_subdim,
                              source + (sub + 1) * pq_subdim,
                              added.begin() + sub * pq_subdim);
                }
                pq3->Insert(added.data(), 910000);
                check(pq3->Count() == pq_n + 1,
                      "PQ compact disk incremental code appended");
                check(pq_contains(*pq3, added.data(), 910000),
                      "PQ compact disk incremental row searchable");
                check(pq3->SerializeV2(pq_writer), "PQ compact disk dirty code flush ok");
                check(pq3->CacheBytesUsed() <= pq3->CacheBudgetBytes(),
                      "PQ code/base cache stays within shared budget");

                std::string reopen_err;
                auto pq4 = GraphBridge::OpenV2Disk(
                    pq_reader, nullptr, pq_rec_reader, kDim, 16, 100, VexMetric::L2,
                    QuantizerType::PQ, /*pq_m=*/4, /*compact=*/true,
                    /*cache_budget=*/1, reopen_err);
                check(pq4 != nullptr && pq4->UsesPQ() && pq4->Count() == pq_n + 1,
                      "PQ compact reopens after disk insert");
                if (pq4) {
                    check(pq_contains(*pq4, added.data(), 910000),
                          "reopened PQ incremental row searchable");
                }
            }

            auto expect_bad_pq = [&](auto mutate, const char *what) {
                auto bad = pq_segs;
                mutate(bad);
                auto bad_reader = [&](int kind, uint32_t seg,
                                      std::vector<char> &out) -> bool {
                    auto it = bad.find({kind, seg});
                    if (it == bad.end()) return false;
                    out = it->second;
                    return true;
                };
                std::string bad_err;
                auto opened = GraphBridge::OpenV2(
                    bad_reader, kDim, 16, 100, VexMetric::L2,
                    QuantizerType::PQ, /*pq_m=*/4, /*compact=*/true, bad_err);
                check(opened == nullptr && !bad_err.empty(), what);
            };
            expect_bad_pq([](auto &bad) { bad.erase({5, 0}); },
                          "PQ missing codebook segment rejected");
            expect_bad_pq([](auto &bad) { bad[{6, 0}].pop_back(); },
                          "PQ truncated code segment rejected");
            expect_bad_pq([](auto &bad) { bad[{5, 0}][4] = 0; },
                          "PQ unsupported fixed version rejected");

            // K-means 的 dispatcher 必须按 dsub 而不是原向量 dim 选择 tail
            // kernel。dim=6,pq_m=3 => dsub=2，300 行越过 QuickCenters 路径；
            // ASan/UBSan 构建可直接抓到选错 NoTail kernel 的越界读取。
            constexpr size_t tail_dim = 6;
            constexpr size_t tail_n = 300;
            std::vector<float> tail_data(tail_n * tail_dim);
            std::vector<int64_t> tail_rowids(tail_n);
            for (size_t i = 0; i < tail_n; i++) {
                tail_rowids[i] = int64_t(i + 1);
                for (size_t d = 0; d < tail_dim; d++) {
                    tail_data[i * tail_dim + d] =
                        float((i * (d + 3) + d * 11) % 257) / 17.0f;
                }
            }
            GraphBridge pq_tail(tail_dim, 16, 100, VexMetric::L2,
                                QuantizerType::PQ, /*pq_m=*/3, /*compact=*/true);
            pq_tail.BuildBulk(tail_data.data(), tail_rowids.data(), tail_n,
                              /*n_threads=*/4);
            std::vector<std::pair<double, int64_t>> tail_result;
            pq_tail.Search(tail_data.data() + 123 * tail_dim, 160, 160, tail_result);
            check(std::any_of(tail_result.begin(), tail_result.end(), [](const auto &item) {
                      return item.second == 124;
                  }), "PQ dsub tail-kernel training/search works");
        }
    }

    if (g_fail) {
        printf("M3+ PARALLEL SMOKE: FAIL\n");
        return 1;
    }
    printf("M3+ PARALLEL SMOKE: PASS\n");
    return 0;
}
