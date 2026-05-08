/*
 * architecture_minimal.cpp - Minimal CPU architecture detection
 * No dependencies on halfutils or complex headers
 */

#include "distance/core/distance.h"
#include "distance/core/architecture_macro.h"

#if defined(PG_VEXDB_TARGET_PG)
#include "pg_compat.h"
#endif

/* Declarations from distance_utils.h */
namespace ann_helper {
    bool is_arch_available(Arch arch, Metric m, DistPrecisionType dt);
    Arch get_best_arch();
}

#if COMPILER_TARGET_X86_64
#include <cpuid.h>

static bool supports_sse() {
    unsigned int eax, ebx, ecx, edx;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        return (ecx & bit_SSE4_1) && (ecx & bit_SSE4_2);
    }
    return false;
}

static bool supports_avx() {
    unsigned int eax, ebx, ecx, edx;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        if (!(ecx & bit_AVX) || !(ecx & bit_FMA)) {
            return false;
        }
    }
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        return (ebx & bit_AVX2) != 0;
    }
    return false;
}

static bool supports_avx512() {
    unsigned int eax, ebx, ecx, edx;
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        return (ebx & bit_AVX512F) && (ebx & bit_AVX512DQ) && 
               (ebx & bit_AVX512BW) && (ebx & bit_AVX512VL);
    }
    return false;
}
#endif /* x86_64 */

#if COMPILER_TARGET_ARM
static bool supports_neonv8()
{
#if COMPILER_SUPPORT_NEONV8
    return true;
#else
    return false;
#endif
}
#endif /* arm */

Arch get_best_arch(Metric m, DistPrecisionType dt, uint16 dim)
{
    (void)m;
    (void)dt;
    (void)dim;

#if COMPILER_TARGET_X86_64
    if (supports_avx512()) {
        return Arch::AVX512;
    }
    if (supports_avx()) {
        return Arch::AVX;
    }
    if (supports_sse()) {
        return Arch::SSE;
    }
#endif /* x86_64 */

#if COMPILER_TARGET_ARM
    if (supports_neonv8()) {
        return Arch::NEONV8;
    }
#endif

    return Arch::GENERAL;
}

bool ann_helper::is_arch_available(Arch arch, Metric m, DistPrecisionType dt)
{
    (void)m;
    (void)dt;
    
    switch (arch) {
        case Arch::GENERAL:
            return true;
#if COMPILER_TARGET_X86_64
        case Arch::SSE:
            return supports_sse();
        case Arch::AVX:
            return supports_avx();
        case Arch::AVX512:
            return supports_avx512();
#endif
#if COMPILER_TARGET_ARM
        case Arch::NEONV8:
            return supports_neonv8();
#endif
        default:
            return false;
    }
}

Arch ann_helper::get_best_arch()
{
    return get_best_arch(Metric::L2, DistPrecisionType::FLOAT, 0);
}
