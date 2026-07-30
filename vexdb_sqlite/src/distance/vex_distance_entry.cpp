// 距离入口实现：把 common DispatchRunner（重模板，编译慢）隔离在本翻译单元，
// vtab/UDF 只 include 轻头 vex_distance_entry.h。
#include "vex_distance_entry.h"

#include "distance/core/distance.h"

#include <cmath>

namespace {

// 复用 common 中已经实例化的 dispatcher。不要在本翻译单元重复实例化，
// Windows COFF 链接器不会像 ELF/Mach-O 一样合并这些模板符号。
ann_helper::distance_func GetRawDistanceFunc(Metric metric) {
    // dim=1 选择 unaligned/unknown remainder 版本，返回的函数可处理实际 dim。
    return ann_helper::get_general_distance_func(metric, 1);
}

float ComputeL2(const float *x, const float *y, uint16_t dim) {
    static const auto raw = GetRawDistanceFunc(Metric::L2);  // 返回平方距离
    return std::sqrt(raw(x, y, dim));
}

float ComputeCosine(const float *x, const float *y, uint16_t dim) {
    static const auto raw = GetRawDistanceFunc(Metric::INNER_PRODUCT);  // 返回 -dot
    float norm_x = 0.0f;
    float norm_y = 0.0f;
    for (uint16_t i = 0; i < dim; ++i) {
        norm_x += x[i] * x[i];
        norm_y += y[i] * y[i];
    }
    if (norm_x == 0.0f || norm_y == 0.0f) return 2.0f;
    return 1.0f + raw(x, y, dim) / std::sqrt(norm_x * norm_y);
}

float ComputeNegIP(const float *x, const float *y, uint16_t dim) {
    static const auto raw = GetRawDistanceFunc(Metric::INNER_PRODUCT);  // 返回 -ip
    return raw(x, y, dim);
}

}  // namespace

namespace vexdb_sqlite {

DistanceFn GetDistanceFn(VexMetric metric) {
    switch (metric) {
    case VexMetric::L2: return ComputeL2;
    case VexMetric::COSINE: return ComputeCosine;
    case VexMetric::INNER_PRODUCT: return ComputeNegIP;
    }
    return ComputeL2;
}

}  // namespace vexdb_sqlite
