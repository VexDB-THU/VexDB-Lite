#include <boost/preprocessor/seq/transform.hpp>
#include <boost/preprocessor/seq/for_each_product.hpp>

#include "distance/distance_utils.h"
#include "halfvec.h"
#include "halfutils.h"

class GeneralDistancePatcher {
public:
    template <DistPrecisionType dt>
    static constexpr RemainderSituation get_remainder_situation(uint16 dim)
        { return RemainderSituation::Unknown; }

private:
    template <Metric m, DistPrecisionType dt, RemainderSituation rs, bool aligned>
    struct GeneralPolicy;

    template <TransformOp op, DistPrecisionType dt, RemainderSituation rs, bool aligned>
    struct GeneralTransformPolicy;

    template <RemainderSituation rs, bool aligned>
    struct GeneralPolicyBase {
        using VecT = void;
        using AccT = void;
        static constexpr bool use_asm_code = false;
        static constexpr bool use_custom_code = false;
        static constexpr bool is_aligned = aligned;
        static constexpr uint16 k = 1;
        static constexpr uint16 k_per_iter = 1;
        static constexpr RemainderSituation RS = rs;
    };

    template <Metric m, RemainderSituation rs, bool aligned>
    struct GeneralPolicy<m, DistPrecisionType::FLOAT, rs, aligned>
        : public GeneralPolicyBase<rs, aligned> {
        using PlainT = float;
        using IntmT = float;
        static INLINE_PROP float transform(float v) { return v; }
    };

    template <Metric m, RemainderSituation rs, bool aligned>
    struct GeneralPolicy<m, DistPrecisionType::HALF, rs, aligned>
        : public GeneralPolicyBase<rs, aligned> {
        using PlainT = half;
        using IntmT = float;
        static INLINE_PROP float transform(half v) { return half_to_float(v); }
    };

    template <Metric m, RemainderSituation rs, bool aligned>
    struct GeneralPolicy<m, DistPrecisionType::INT8, rs, aligned>
        : public GeneralPolicyBase<rs, aligned> {
        using PlainT = int8;
        using IntmT = int8;
        static INLINE_PROP int8 transform(int8 v) { return v; }
    };

public:
    template <Metric m, DistPrecisionType dt, RemainderSituation rs, bool aligned>
    using Distancer = DistanceDispatcher<GeneralPolicy, m, dt, rs, aligned>;

private:
    template <RemainderSituation rs, bool aligned>
    struct GeneralTransformPolicyBase {
        using VecT = float;
        using IntmT = float;
        static constexpr uint16 k = 1;
        static constexpr uint16 k_per_iter = 1;
        static constexpr bool use_custom_code = false;
        static constexpr bool is_aligned = aligned;
        static constexpr RemainderSituation RS = rs;

        static INLINE_PROP float add(float a, float b) { return a + b; }
        static INLINE_PROP float sub(float a, float b) { return a - b; }
        static INLINE_PROP float mul(float a, float b) { return a * b; }
        static INLINE_PROP float div(float a, float b) { return a / b; }
    };

    template <TransformOp op, RemainderSituation rs, bool aligned>
    struct GeneralTransformPolicy<op, DistPrecisionType::FLOAT, rs, aligned>
        : public GeneralTransformPolicyBase<rs, aligned> {
        using PlainT = float;
        static INLINE_PROP float to_interm(float v) { return v; }
        static INLINE_PROP float from_interm(float v) { return v; }
    };

    template <TransformOp op, RemainderSituation rs, bool aligned>
    struct GeneralTransformPolicy<op, DistPrecisionType::HALF, rs, aligned>
        : public GeneralTransformPolicyBase<rs, aligned> {
        using PlainT = half;
        static INLINE_PROP float to_interm(half v) { return half_to_float(v); }
        static INLINE_PROP float to_interm(float v) { return v; }
        static INLINE_PROP half from_interm(float v) { return float_to_half(v); }
    };

    template <TransformOp op, RemainderSituation rs, bool aligned>
    struct GeneralTransformPolicy<op, DistPrecisionType::INT8, rs, aligned>
        : public GeneralTransformPolicyBase<rs, aligned> {
        using PlainT = int16;
        static INLINE_PROP float to_interm(int16 v) { return (float)v; }
        static INLINE_PROP float to_interm(float v) { return v; }
        static INLINE_PROP int16 from_interm(float v) { return (int16)v; }
    };

public:
    template <TransformOp op, DistPrecisionType dt, RemainderSituation rs, bool aligned>
    using Transformer = TransformDispatcher<GeneralTransformPolicy, op, dt, rs, aligned>;
};

#define PatcherName GeneralDistancePatcher
#define REMAINDER_SITUATION_ENUM_SEQ (RemainderSituation::Unknown)
#define CUR_ARCH Arch::GENERAL
#include "distance/distance.templ"
#undef CUR_ARCH
#undef REMAINDER_SITUATION_ENUM_SEQ
#undef PatcherName
