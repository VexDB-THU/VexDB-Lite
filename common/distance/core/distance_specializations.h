#ifndef VEXDB_DISTANCE_SPECIALIZATIONS_H
#define VEXDB_DISTANCE_SPECIALIZATIONS_H

#include <boost/preprocessor/seq/for_each_product.hpp>
#include <boost/preprocessor/seq/elem.hpp>

#define VEX_NATIVE_DISTANCE_METRICS \
    (Metric::L2)(Metric::INNER_PRODUCT)(Metric::COSINE)(Metric::SPHERICAL)(Metric::L2_SQRT)(Metric::L2_NORM)
#define VEX_NATIVE_DISTANCE_DTYPES (DistPrecisionType::FLOAT)(DistPrecisionType::INT8)
#define VEX_NATIVE_DISTANCE_REMAINDERS \
    (RemainderSituation::Unknown)(RemainderSituation::NoPartial)(RemainderSituation::NoTail)
#define VEX_NATIVE_DISTANCE_ALIGNED (true)(false)
#define VEX_NATIVE_DISTANCE_TYPE(product) \
    Distancer<Arch::BOOST_PP_SEQ_ELEM(0, product), BOOST_PP_SEQ_ELEM(1, product), \
              BOOST_PP_SEQ_ELEM(2, product), BOOST_PP_SEQ_ELEM(3, product), \
              BOOST_PP_SEQ_ELEM(4, product)>
#define VEX_DECLARE_NATIVE_DISTANCE(r, product) \
    template <> float VEX_NATIVE_DISTANCE_TYPE(product)::get_distance_single( \
        const void *, const void *, uint16); \
    template <> void VEX_NATIVE_DISTANCE_TYPE(product)::get_distance_batch2( \
        const void *, void *const *, uint16, uint16, float *);

BOOST_PP_SEQ_FOR_EACH_PRODUCT(VEX_DECLARE_NATIVE_DISTANCE, \
    (DISTANCER_ISAS)(VEX_NATIVE_DISTANCE_METRICS)(VEX_NATIVE_DISTANCE_DTYPES) \
    (VEX_NATIVE_DISTANCE_REMAINDERS)(VEX_NATIVE_DISTANCE_ALIGNED))

#undef VEX_DECLARE_NATIVE_DISTANCE
#undef VEX_NATIVE_DISTANCE_TYPE
#undef VEX_NATIVE_DISTANCE_ALIGNED
#undef VEX_NATIVE_DISTANCE_REMAINDERS
#undef VEX_NATIVE_DISTANCE_DTYPES
#undef VEX_NATIVE_DISTANCE_METRICS
#endif
