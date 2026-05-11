// duck_pg_shim.hpp - PostgreSQL lock primitive shim for the duck build path.
//
// Main library (include/graph_index/) uses PG-style LWLock / slock_t /
// pg_memory_barrier. The duck build provides std::atomic-based equivalents
// here so main code compiles without #ifdef.
//
// Not safe across processes — duck single-process thread model only.

#pragma once

#include "vex_simple_rwlock.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>

// ============================================================
// PG kernel typedefs / macros
// ============================================================

using Size = std::size_t;

#ifndef MAXALIGN
// PG uses 8-byte alignment by default on x86_64.
#define MAXIMUM_ALIGNOF 8
#define MAXALIGN(LEN) (((std::size_t)(LEN) + (MAXIMUM_ALIGNOF - 1)) & ~((std::size_t)(MAXIMUM_ALIGNOF - 1)))
#endif

#ifndef TYPEALIGN
#define TYPEALIGN(ALIGNVAL, LEN) (((std::size_t)(LEN) + ((ALIGNVAL) - 1)) & ~((std::size_t)((ALIGNVAL) - 1)))
#endif

// PG-style assertion. Disabled in release like PG's Assert.
#ifndef Assert
#ifdef USE_ASSERT_CHECKING
#define Assert(cond) do { if (!(cond)) { std::fprintf(stderr, "Assert failed: %s\n", #cond); std::abort(); } } while (0)
#else
#define Assert(cond) ((void)0)
#endif
#endif

#ifndef Assume
#define Assume(cond) ((void)0)
#endif

// MemoryContext / palloc / palloc0 / pfree / MemoryContextSwitchTo are
// provided by vtl/allocator (duck path block at the top of that header).
// Including vtl/allocator here would create a dependency cycle, so callers
// that need these symbols should already pull in vtl/allocator (vex_graph
// _index_depend_duck.hpp does via vtl/bitvector → vtl/allocator).

inline void* MemoryContextAlloc(void* /*ctx*/, Size size) {
    // vtl/allocator's palloc returns malloc; replicate.
    void* p = std::malloc(size);
    if (!p) {
        throw std::bad_alloc();
    }
    return p;
}

// ============================================================
// elog / ereport
//
// Duck has no PG error reporting; map to std::runtime_error / fprintf.
// ============================================================

enum {
    DEBUG5 = 10, DEBUG4 = 11, DEBUG3 = 12, DEBUG2 = 13, DEBUG1 = 14,
    LOG = 15, INFO = 17, NOTICE = 18, WARNING = 19, ERROR = 20, FATAL = 21, PANIC = 22,
};

#ifndef elog
#define elog(level, ...) \
    do { \
        if ((level) >= ERROR) { \
            char _buf[512]; \
            std::snprintf(_buf, sizeof(_buf), __VA_ARGS__); \
            throw std::runtime_error(_buf); \
        } else { \
            std::fprintf(stderr, "[vex-duck] " __VA_ARGS__); \
            std::fprintf(stderr, "\n"); \
        } \
    } while (0)
#endif

// ============================================================
// LWLock
// ============================================================

using LWLock = vex_duck::SimpleRWLock;

// Cache-line aligned to avoid false sharing when stored in arrays.
struct alignas(64) LWLockPadded {
    LWLock lock;
};

enum LWLockMode {
    LW_SHARED,
    LW_EXCLUSIVE,
};

// PG tranche IDs are perf-monitoring categories shown in pg_stat_activity.
// The duck path ignores them, but the symbol must exist for main-lib callers
// to compile.
constexpr int LWTRANCHE_EXTEND = 0;

inline void LWLockAcquire(LWLock* l, LWLockMode mode) noexcept {
    if (mode == LW_SHARED) {
        l->lock_shared();
    } else {
        l->lock();
    }
}

// SimpleRWLock infers shared/exclusive from atomic state — no mode arg needed.
inline void LWLockRelease(LWLock* l) noexcept {
    l->unlock();
}

inline void LWLockInitialize(LWLock* l, int /*tranche*/) noexcept {
    // PG resets in place; mirror that with destroy + placement-new so callers
    // can re-initialize an already-constructed lock.
    l->~LWLock();
    new (l) LWLock();
}

inline bool LWLockConditionalAcquire(LWLock* l, LWLockMode mode) noexcept {
    return mode == LW_SHARED ? l->try_lock_shared() : l->try_lock();
}

// ============================================================
// SpinLock (slock_t)
// ============================================================

using slock_t = std::atomic_flag;

inline void SpinLockInit(slock_t* s) noexcept {
    s->clear(std::memory_order_relaxed);
}

inline void SpinLockAcquire(slock_t* s) noexcept {
    int spin = 0;
    while (s->test_and_set(std::memory_order_acquire)) {
        vex_duck::detail::backoff(spin);
    }
}

inline void SpinLockRelease(slock_t* s) noexcept {
    s->clear(std::memory_order_release);
}

// ============================================================
// Memory barriers
// ============================================================

inline void pg_memory_barrier() noexcept {
    std::atomic_thread_fence(std::memory_order_acq_rel);
}

inline void pg_compiler_barrier() noexcept {
    std::atomic_signal_fence(std::memory_order_acq_rel);
}

inline void pg_read_barrier() noexcept {
    std::atomic_thread_fence(std::memory_order_acquire);
}

inline void pg_write_barrier() noexcept {
    std::atomic_thread_fence(std::memory_order_release);
}
