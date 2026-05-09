// ARCH_FUNC_CALL: switch on Arch enum and forward `arg` to the per-isa
// SIMD-prefixed function. Used by src/distance/core/pq_dispatcher.cpp and
// (legacy duplicate) src/distance/pg/distance.cpp.
#ifndef ARCH_DISPATCH_MACROS_H
#define ARCH_DISPATCH_MACROS_H

#include "distance/core/distance.h"
#include "distance/core/architecture_macro.h"
#include "distance/core/distance_utils_core.h"

#if COMPILER_SUPPORT_NEONV8
#define ISA_FUNC_CALL_NEONV8(arg, call) \
    case Arch::NEONV8:                  \
        call(NEONV8_FUNC(arg));
#else
#define ISA_FUNC_CALL_NEONV8(arg, call)
#endif

#if COMPILER_SUPPORT_SVEV8
#define ISA_FUNC_CALL_SVEV8(arg, call) \
    case Arch::SVEV8:                  \
        call(SVEV8_FUNC(arg));
#else
#define ISA_FUNC_CALL_SVEV8(arg, call)
#endif

#if COMPILER_SUPPORT_SVE2V8
#define ISA_FUNC_CALL_SVE2V8(arg, call) \
    case Arch::SVE2V8:                  \
        call(SVE2V8_FUNC(arg));
#else
#define ISA_FUNC_CALL_SVE2V8(arg, call)
#endif

#if COMPILER_SUPPORT_NEONV9
#define ISA_FUNC_CALL_NEONV9(arg, call) \
    case Arch::NEONV9:                  \
        call(NEONV9_FUNC(arg));
#else
#define ISA_FUNC_CALL_NEONV9(arg, call)
#endif

#if COMPILER_SUPPORT_SVEV9
#define ISA_FUNC_CALL_SVEV9(arg, call) \
    case Arch::SVEV9:                  \
        call(SVEV9_FUNC(arg));
#else
#define ISA_FUNC_CALL_SVEV9(arg, call)
#endif

#if COMPILER_SUPPORT_SVE2V9
#define ISA_FUNC_CALL_SVE2V9(arg, call) \
    case Arch::SVE2V9:                  \
        call(SVE2V9_FUNC(arg));
#else
#define ISA_FUNC_CALL_SVE2V9(arg, call)
#endif

#if COMPILER_SUPPORT_SMEV9
#define ISA_FUNC_CALL_SMEV9(arg, call) \
    case Arch::SMEV9:                  \
        call(SMEV9_FUNC(arg));
#else
#define ISA_FUNC_CALL_SMEV9(arg, call)
#endif

#if COMPILER_SUPPORT_SME2V9
#define ISA_FUNC_CALL_SME2V9(arg, call) \
    case Arch::SME2V9:                  \
        call(SME2V9_FUNC(arg));
#else
#define ISA_FUNC_CALL_SME2V9(arg, call)
#endif

#if COMPILER_SUPPORT_SSE
#define ISA_FUNC_CALL_SSE(arg, call) \
    case Arch::SSE:                  \
        call(SSE_FUNC(arg));
#else
#define ISA_FUNC_CALL_SSE(arg, call)
#endif

#if COMPILER_SUPPORT_AVX
#define ISA_FUNC_CALL_AVX(arg, call) \
    case Arch::AVX:                  \
        call(AVX_FUNC(arg));
#else
#define ISA_FUNC_CALL_AVX(arg, call)
#endif

#if COMPILER_SUPPORT_AVX512_EXTEND
#define ISA_FUNC_CALL_AVX512(arg, call) \
    case Arch::AVX512:                  \
        call(AVX512_FUNC(arg));
#else
#define ISA_FUNC_CALL_AVX512(arg, call)
#endif

#define ARCH_FUNC_CALL(arch, arg, call) \
    switch (arch) {                     \
        ISA_FUNC_CALL_NEONV8(arg, call) \
        ISA_FUNC_CALL_SVEV8(arg, call)  \
        ISA_FUNC_CALL_SVE2V8(arg, call) \
        ISA_FUNC_CALL_NEONV9(arg, call) \
        ISA_FUNC_CALL_SVEV9(arg, call)  \
        ISA_FUNC_CALL_SVE2V9(arg, call) \
        ISA_FUNC_CALL_SMEV9(arg, call)  \
        ISA_FUNC_CALL_SME2V9(arg, call) \
        ISA_FUNC_CALL_SSE(arg, call)    \
        ISA_FUNC_CALL_AVX(arg, call)    \
        ISA_FUNC_CALL_AVX512(arg, call) \
        case Arch::GENERAL:             \
        default:                        \
            call(GENERAL_FUNC(arg));    \
    }

#endif
