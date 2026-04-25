/**
 * Copyright ...
 * Inherit from this class to check resource leaks.
 */

#ifndef DISKANN_UTILS_LEAK_CHECKER_H
#define DISKANN_UTILS_LEAK_CHECKER_H

#include "c.h"

#define USE_LEAK_CHECKER false

namespace ann_helper {
#if USE_LEAK_CHECKER
class LeakChecker {
public:
    LeakChecker() = default;
    LeakChecker(const LeakChecker &other) = default;
    LeakChecker(LeakChecker &&other) : _count(other._count) { other._count = 0; }
    LeakChecker &operator=(const LeakChecker &other) = default;
    LeakChecker &operator=(LeakChecker &&other)
    {
        _count = other._count;
        other._count = 0;
        return *this;
    }
    void swap(LeakChecker &other) { std::swap(_count, other._count); }
    virtual ~LeakChecker()
    {
        Assert(_count == 0);
        if (_count != 0) {
            ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                errmsg("Memory leak detected: %d", _count)));
        }
    }
    void destroy() { --_count; }
private:
    int _count{1};
};
#else
class LeakChecker {
public:
    constexpr static void destroy() {}
};
#endif /* USE_LEAK_CHECKER */
} /* namespace ann_helper */

#endif /* DISKANN_UTILS_LEAK_CHECKER_H */
