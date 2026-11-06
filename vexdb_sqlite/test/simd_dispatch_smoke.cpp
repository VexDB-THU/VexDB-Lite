#include <cstdio>

#include "distance/core/distance.h"
#include "distance/core/distance_utils_core.h"

namespace {

#define CHECK_KERNEL(field, expected)                                                   \
    do {                                                                                \
        if (kernels.field != ann_helper::expected) {                                    \
            std::fprintf(stderr, "RaBitQ SIMD dispatch mismatch: %s\n", #field);         \
            return 1;                                                                   \
        }                                                                               \
    } while (false)

#define CHECK_ARCH(prefix)                           \
    CHECK_KERNEL(flip_sign, prefix##_flip_sign);     \
    CHECK_KERNEL(kacs_walk, prefix##_kacs_walk);     \
    CHECK_KERNEL(warmup_ip_x0_q, prefix##_warmup_ip_x0_q); \
    CHECK_KERNEL(ip_fxi, prefix##_ip_fxi);           \
    CHECK_KERNEL(mask_ip_x0_q, prefix##_mask_ip_x0_q)

} // namespace

int main()
{
    const Arch arch = ann_helper::get_best_arch();
    const auto kernels = ann_helper::get_rabitq_kernels();
    switch (arch) {
#if COMPILER_SUPPORT_NEONV8
    case Arch::NEONV8:
        CHECK_ARCH(neonv8);
        break;
#endif
#if COMPILER_SUPPORT_SVEV8
    case Arch::SVEV8:
        CHECK_ARCH(svev8);
        break;
#endif
#if COMPILER_SUPPORT_SVE2V8
    case Arch::SVE2V8:
        CHECK_ARCH(sve2v8);
        break;
#endif
#if COMPILER_SUPPORT_NEONV9
    case Arch::NEONV9:
        CHECK_ARCH(neonv9);
        break;
#endif
#if COMPILER_SUPPORT_SVEV9
    case Arch::SVEV9:
        CHECK_ARCH(svev9);
        break;
#endif
#if COMPILER_SUPPORT_SVE2V9
    case Arch::SVE2V9:
        CHECK_ARCH(sve2v9);
        break;
#endif
#if COMPILER_SUPPORT_SMEV9
    case Arch::SMEV9:
        CHECK_ARCH(smev9);
        break;
#endif
#if COMPILER_SUPPORT_SME2V9
    case Arch::SME2V9:
        CHECK_ARCH(sme2v9);
        break;
#endif
#if COMPILER_SUPPORT_SSE
    case Arch::SSE:
        CHECK_ARCH(sse);
        break;
#endif
#if COMPILER_SUPPORT_AVX
    case Arch::AVX:
        CHECK_ARCH(avx);
        break;
#endif
#if COMPILER_SUPPORT_AVX512_EXTEND
    case Arch::AVX512:
        CHECK_ARCH(avx512);
        break;
#endif
    case Arch::GENERAL:
        CHECK_ARCH(genernal);
        break;
    }

    std::printf("RaBitQ SIMD dispatch smoke passed (arch=%u)\n",
                static_cast<unsigned>(arch));
    return 0;
}
