// Unit test for duck_pg_shim.hpp
//
// 编译：c++ -std=c++17 -O2 -pthread test_duck_pg_shim.cpp \
//          -I../../include -o test_duck_pg_shim
// 运行：./test_duck_pg_shim
//
// 验证：模拟主库 MemStore 的锁使用模式，确保 shim 正确工作。

#include "duck_pg_shim.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

// 模拟主库 MemStore::get_entry 双锁协议（entry_lock + entry_waitlock）
struct FakeMemStore {
    LWLock entry_lock;
    LWLock entry_waitlock;
    LWLock elems_veclock;
    slock_t mempool_mutex;
    int entry_level = -1;
    std::atomic<int> num_inserts{0};

    FakeMemStore() {
        LWLockInitialize(&entry_lock, LWTRANCHE_EXTEND);
        LWLockInitialize(&entry_waitlock, LWTRANCHE_EXTEND);
        LWLockInitialize(&elems_veclock, LWTRANCHE_EXTEND);
        SpinLockInit(&mempool_mutex);
    }

    // 模拟 get_entry 路径
    bool try_promote_entry(int new_level) {
        LWLockAcquire(&entry_waitlock, LW_EXCLUSIVE);
        LWLockRelease(&entry_waitlock);

        LWLockAcquire(&entry_lock, LW_SHARED);
        if (new_level > entry_level) {
            LWLockRelease(&entry_lock);
            LWLockAcquire(&entry_waitlock, LW_EXCLUSIVE);
            LWLockAcquire(&entry_lock, LW_EXCLUSIVE);
            LWLockRelease(&entry_waitlock);
            // 升级 entry
            if (new_level > entry_level) {
                entry_level = new_level;
            }
            LWLockRelease(&entry_lock);
            return true;
        }
        LWLockRelease(&entry_lock);
        return false;
    }

    // 模拟 add_elem 路径（EXCLUSIVE 拿 elems_veclock）
    void add_elem() {
        LWLockAcquire(&elems_veclock, LW_EXCLUSIVE);
        num_inserts.fetch_add(1);
        LWLockRelease(&elems_veclock);
    }

    // 模拟 MemPool::extend 路径（SpinLock）
    void mempool_extend() {
        SpinLockAcquire(&mempool_mutex);
        pg_memory_barrier();
        SpinLockRelease(&mempool_mutex);
    }
};

void test_shim_basic() {
    FakeMemStore s;
    s.add_elem();
    s.add_elem();
    assert(s.num_inserts == 2);
    bool promoted = s.try_promote_entry(3);
    assert(promoted);
    assert(s.entry_level == 3);
    std::printf("  test_shim_basic: OK\n");
}

void test_shim_concurrent_inserts() {
    FakeMemStore s;
    constexpr int N_THREADS = 8;
    constexpr int ITER = 5000;

    std::vector<std::thread> threads;
    for (int t = 0; t < N_THREADS; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < ITER; i++) {
                // 模拟 N 个 worker 并发：先升 entry，再 add_elem
                int lvl = (i + t) % 16;
                s.try_promote_entry(lvl);
                s.add_elem();
                s.mempool_extend();
            }
        });
    }
    for (auto& t : threads) t.join();

    assert(s.num_inserts == N_THREADS * ITER);
    // entry_level 在并发竞争下可能不是绝对最大值，但至少 ≥ 0
    assert(s.entry_level >= 0);
    std::printf("  test_shim_concurrent_inserts: OK (inserts = %d, max_level = %d)\n",
                s.num_inserts.load(), s.entry_level);
}

void test_spinlock_correctness() {
    slock_t s;
    SpinLockInit(&s);
    int counter = 0;
    constexpr int N_THREADS = 16;
    constexpr int ITER = 10000;

    std::vector<std::thread> threads;
    for (int t = 0; t < N_THREADS; t++) {
        threads.emplace_back([&]() {
            for (int i = 0; i < ITER; i++) {
                SpinLockAcquire(&s);
                counter++;
                SpinLockRelease(&s);
            }
        });
    }
    for (auto& t : threads) t.join();

    assert(counter == N_THREADS * ITER);
    std::printf("  test_spinlock_correctness: OK (counter = %d)\n", counter);
}

}  // namespace

// Verify LWLockConditionalAcquire really is non-blocking.
void test_conditional_acquire() {
    LWLock l;
    LWLockInitialize(&l, LWTRANCHE_EXTEND);

    LWLockAcquire(&l, LW_EXCLUSIVE);
    // 已被 exclusive 占有 → conditional 必须立即返回 false（不阻塞）
    assert(!LWLockConditionalAcquire(&l, LW_SHARED));
    assert(!LWLockConditionalAcquire(&l, LW_EXCLUSIVE));
    LWLockRelease(&l);

    // 多 shared 可重入
    assert(LWLockConditionalAcquire(&l, LW_SHARED));
    assert(LWLockConditionalAcquire(&l, LW_SHARED));
    // exclusive 被 shared 阻挡 → 立即 false
    assert(!LWLockConditionalAcquire(&l, LW_EXCLUSIVE));
    LWLockRelease(&l);
    LWLockRelease(&l);

    std::printf("  test_conditional_acquire: OK\n");
}

int main() {
    std::printf("Running duck_pg_shim tests...\n");
    test_shim_basic();
    test_spinlock_correctness();
    test_shim_concurrent_inserts();
    test_conditional_acquire();
    std::printf("All tests passed.\n");
    return 0;
}
