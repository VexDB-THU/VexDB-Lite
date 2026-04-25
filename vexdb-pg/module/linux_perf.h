/**
 * Copyright ...
 * C++ wrapper for linux perf_event API
 */

#ifndef LINUX_PERF_H
#define LINUX_PERF_H

#if __cplusplus < 201703L
#if __cplusplus < 201402L
#pragma GCC message "WARNING: linux_perf.h does not guarantee to be compiled with c++14"
#else
static_assert(false, "linux_perf.h must be compiled with c++17 or greater");
#endif
#endif /* c++ 17 or greater */

#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <asm/unistd.h>
#include <linux/perf_event.h>
#include <cerrno>
#include <cstring>
#include <array>
#include <type_traits>

#include "c.h"
#include <vtl/expr_helper>

#include "linux_perf_def.h"

template <PerfEventType Type>
struct perf_event_traits {
    static constexpr unsigned code = [] {
        if constexpr (ENABLE_HARDWARE_COUNTERS) {
            switch (Type) {
            case PerfEventType::CPU_CYCLE: return PERF_COUNT_HW_CPU_CYCLES;
            case PerfEventType::INSTR_COUNT: return PERF_COUNT_HW_INSTRUCTIONS;
            case PerfEventType::CACHE_MISS: return PERF_COUNT_HW_CACHE_MISSES;
            case PerfEventType::CACHE_HIT: return PERF_COUNT_HW_CACHE_REFERENCES;
            default: return -1;
            }
        } else {
            switch (Type) {
            case PerfEventType::CPU_CYCLE: return PERF_COUNT_SW_CPU_CLOCK;
            case PerfEventType::INSTR_COUNT: return PERF_COUNT_SW_CPU_CLOCK;
            case PerfEventType::CACHE_MISS:
            case PerfEventType::CACHE_HIT:
            default: return -1;
            }
        }
    }();
    static constexpr unsigned type = [] {
        if constexpr (ENABLE_HARDWARE_COUNTERS) {
            return PERF_TYPE_HARDWARE;
        } else {
            return PERF_TYPE_SOFTWARE;
        }
    }();
    static constexpr size_t index = [] {
        if constexpr (ENABLE_HARDWARE_COUNTERS) {
            return static_cast<size_t>(Type);
        } else {
            switch (Type) {
            case PerfEventType::CPU_CYCLE:
                return 0;
            case PerfEventType::INSTR_COUNT:
                return 1;
            default:
                return PERF_EVENT_TYPE_COUNT;
            }
        }
    }();
};

template <PerfEventType... Ts>
struct PerfEventList {
    template <PerfEventType T>
    using prepend = PerfEventList<T, Ts...>;

    template <template <PerfEventType...> class T>
    using apply = T<Ts...>;
};

template <PerfEventType... Ts>
struct SupportedPerf;

template <>
struct SupportedPerf<> {
    using type = PerfEventList<>;
};

template <PerfEventType First, PerfEventType... Rest>
struct SupportedPerf<First, Rest...> {
private:
    using tail = typename SupportedPerf<Rest...>::type;
public:
    using type = std::conditional_t<
        (perf_event_traits<First>::code != static_cast<unsigned>(-1)),
        typename tail::template prepend<First>,
        tail>;
};

template <typename List>
struct PerfEventListToSet;

template <PerfEventType... Ts>
struct PerfEventListToSet<PerfEventList<Ts...>> {
    using type = perf_event_set<Ts...>;
};

struct perf_event_set_base : public BaseObject {
    virtual ~perf_event_set_base() {}
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void destroy() = 0;
    virtual void extract_counters(size_t out[PERF_EVENT_TYPE_COUNT]) const = 0;
};

template <PerfEventType... Events>
struct perf_event_set : perf_event_set_base {
    template <PerfEventType... OtherEvents>
    friend class perf_event_set;
private:
    int perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
                        int cpu, int group_fd, unsigned long flags)
    {
        return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
    }
public:
    perf_event_set()
    {
        ann_helper::unroll<event_size>([&](auto i) {
            _counters[i] = 0;
            _fds[i] = -1;
        });

        struct perf_event_attr pe;
        memset(&pe, 0, sizeof(struct perf_event_attr));
        pe.size = sizeof(struct perf_event_attr);
        pe.disabled = 1;
        pe.exclude_kernel = 1;
        pe.exclude_hv = 1;

        constexpr PerfEventType events_arr[] = {Events...};
        ann_helper::unroll<event_size>([&](auto i) {
            auto evt = events_arr[i];
            unsigned code = perf_event_traits<evt>::code;
            if (code == -1) {
                return;
            }
            pe.type = perf_event_traits<evt>::type;
            pe.config = code;
            _fds[i] = perf_event_open(&pe, -1, 0, -1, 0);
            if (_fds[i] == -1) {
                ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("Failed to open perf event %u: %s\n",
                           static_cast<unsigned>(evt), strerror(errno))));
            }
        });
    }

    void destroy()
    {
        constexpr PerfEventType events_arr[] = {Events...};
        ann_helper::unroll<event_size>([&](auto i) {
            if (_fds[i] >= 0) {
                close(_fds[i]);
                _fds[i] = -1;
            }
        });
    }

    void start()
    {
        constexpr PerfEventType events_arr[] = {Events...};
        ann_helper::unroll<event_size>([&](auto i) {
            if (_fds[i] >= 0) {
                ioctl(_fds[i], PERF_EVENT_IOC_RESET, 0);
                ioctl(_fds[i], PERF_EVENT_IOC_ENABLE, 0);
                read_counter(i);
            }
        });
    }

    void stop()
    {
        constexpr PerfEventType events_arr[] = {Events...};
        ann_helper::unroll<event_size>([&](auto i) {
            if (_fds[i] >= 0) {
                ioctl(_fds[i], PERF_EVENT_IOC_DISABLE, 0);
                read_counter(i);
            }
        });
    }

    void extract_counters(size_t out[PERF_EVENT_TYPE_COUNT]) const override
    {
        constexpr PerfEventType events_arr[] = {Events...};
        ann_helper::unroll<event_size>([&](auto i) {
            constexpr auto evt = events_arr[i];
            constexpr auto idx = perf_event_traits<evt>::index;
            if constexpr (perf_event_traits<evt>::code != static_cast<unsigned>(-1)) {
                if (idx < PERF_EVENT_TYPE_COUNT) {
                    out[idx] += _counters[i];
                }
            }
            _counters[i] = 0;
        });
    }

private:
    static constexpr size_t event_size = sizeof...(Events);
    int _fds[event_size];
    size_t _counters[event_size];

    void read_counter(size_t idx)
    {
        if (_fds[idx] < 0) {
            return;
        }
        size_t value;
        ssize_t ret = read(_fds[idx], &value, sizeof(value));
        if (ret >= 0) {
            _counters[idx] += value;
        }
    }
};

#endif /* LINUX_PERF_H */
