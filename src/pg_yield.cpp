/**
 * Copyright (c) 2024, openGauss Contributors
 * Copyright (c) 2024, pg_vexdb Contributors
 * 
 * A mimic to boost rw_spinlock::yield
 * Copied from openGauss src/gausskernel/storage/access/annvector/module/pg_yield.cpp
 */

#include "pg_compat.h"

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

#if __has_builtin(__builtin_ia32_pause) && !defined(__INTEL_COMPILER)
#define YIELD_PAUSE() __builtin_ia32_pause()
#elif defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
#define YIELD_PAUSE() __asm__ __volatile__( "rep; nop" : : : "memory" )
#elif defined(__GNUC__) && ((defined(__ARM_ARCH) && __ARM_ARCH >= 8) || defined(__ARM_ARCH_8A__) || defined(__aarch64__))
#define YIELD_PAUSE() __asm__ __volatile__( "yield" : : : "memory" )
#else
#define YIELD_PAUSE() ((void)0)
#endif

#if defined(_POSIX_PRIORITY_SCHEDULING) && (_POSIX_PRIORITY_SCHEDULING+0 > 0) || \
    (defined(_POSIX_THREAD_PRIORITY_SCHEDULING) && (_POSIX_THREAD_PRIORITY_SCHEDULING+0 > 0)) || \
    (defined(_XOPEN_REALTIME) && (_XOPEN_REALTIME+0 >= 0))
#include <sched.h>
#define YIELD_YIELD() sched_yield()
#else
#define YIELD_YIELD() YIELD_PAUSE()
#endif

extern "C" {

void pg_yield(unsigned int k)
{
    constexpr unsigned int sleep_every = 1024;
    k %= sleep_every;
    if (k < 5) {
        const unsigned int pause_count = 1u << k;
        for(unsigned int i = 0; i < pause_count; ++i) {
            YIELD_PAUSE();
        }
    } else if (k < sleep_every - 1) {
        YIELD_YIELD();
    } else {
        pg_usleep(1);
        CHECK_FOR_INTERRUPTS();
    }
}

} /* extern "C" */
