/*
 * architecture_minimal.cpp - Minimal CPU architecture detection
 * No dependencies on halfutils or complex headers
 */

#include "pg_compat.h"
#include "distance/distance.h"
#include "distance/architecture_macro.h"

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
        default:
            return false;
    }
}

Arch ann_helper::get_best_arch()
{
    return get_best_arch(Metric::L2, DistPrecisionType::FLOAT, 0);
}
