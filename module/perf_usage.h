/**
 * Copyright ...
 *
 * Set PERF_USAGE_COUNT / PERF_USAGE_TIME / PERF_USAGE_EVENT to control perf dimensions.
 *
 * Usage:
 *   1) Declare category tags in local scope:
 *        PERF_DECLARE_CATS(MyPerfCats, false, read, write, calc);
 *
 *   2) Major usage (member-style):
 *        struct Something : public PERFER(MyPerfCats)
 *        {
 *            using PerfCats = MyPerfCats;
 *            void run()
 *            {
 *                DO_PERF(read);
 *                DO_PERF(write, PerfEventType::CPU_CYCLE, PerfEventType::INSTR_COUNT);
 *                STOP_PERF(read);
 *                STOP_PERF(write);
 *                REPORT_PERF(NOTICE);
 *            }
 *        };
 *
 *   3) Compatibility usage (object-style):
 *        using MemPerf = PERFER(MyPerfCats);
 *        void run(MemPerf &perf)
 *        {
 *            DO_PERF_ON(perf, MyPerfCats, read);
 *            DO_PERF_ON(perf, MyPerfCats, write, PerfEventType::CPU_CYCLE, PerfEventType::INSTR_COUNT);
 *            STOP_PERF_ON(perf, MyPerfCats, read);
 *            STOP_PERF_ON(perf, MyPerfCats, write);
 *            REPORT_PERF_ON(perf, NOTICE);
 *        }
 *
 * Notes:
 *   - Categories are compile-time types, no dynamic registry.
 *   - Duplicate categories in PERF_DECLARE_CATS are rejected at compile time.
 *   - PerfEventType values come from annvector/module/linux_perf_def.h.
 *   - Member-style macros are the primary interface; object-style macros are compatibility helpers.
 */

#ifndef PERFUSAGE_H
#define PERFUSAGE_H

#ifndef PERF_USAGE_COUNT
#define PERF_USAGE_COUNT 0
#endif

#ifndef PERF_USAGE_TIME
#define PERF_USAGE_TIME 0
#endif

#ifndef PERF_USAGE_EVENT
#define PERF_USAGE_EVENT 0
#endif

#if PERF_USAGE_TIME
#include <chrono>
#endif
#include <type_traits>

#include <boost/preprocessor/seq/enum.hpp>
#include <boost/preprocessor/seq/for_each.hpp>
#include <boost/preprocessor/seq/transform.hpp>
#include <boost/preprocessor/cat.hpp>
#include <boost/preprocessor/stringize.hpp>
#include <boost/preprocessor/variadic/to_seq.hpp>

#if PERF_USAGE_COUNT || PERF_USAGE_TIME || PERF_USAGE_EVENT
#include <vtl/expr_helper>
#endif

#include "c.h"
#include "linux_perf_def.h"
#if PERF_USAGE_EVENT
#include "linux_perf.h"
#endif
#if PERF_USAGE_TIME
#include "timer.h"
#endif
#if PERF_USAGE_COUNT || PERF_USAGE_TIME
#include "parallel_counter.h"
#endif

namespace internal {
template <typename target, typename... tags>
struct contains_type;

template <typename target>
struct contains_type<target> : std::false_type {};

template <typename target, typename first, typename... rest>
struct contains_type<target, first, rest...>
    : std::integral_constant<bool, std::is_same<target, first>::value || contains_type<target, rest...>::value> {};

template <typename... tags>
struct unique_types;

template <>
struct unique_types<> : std::true_type {};

template <typename first, typename... rest>
struct unique_types<first, rest...>
    : std::integral_constant<bool, !contains_type<first, rest...>::value && unique_types<rest...>::value> {};

template <size_t idx, typename first, typename... rest>
struct type_at {
    using type = typename type_at<idx - 1, rest...>::type;
};

template <typename first, typename... rest>
struct type_at<0, first, rest...> {
    using type = first;
};

template <size_t idx, typename first>
struct type_at<idx, first> {
    static_assert(idx == 0, "Perf category index out of range");
    using type = first;
};

template <typename target, size_t idx, typename first, typename... rest>
struct index_of;

template <bool matched, typename target, size_t idx, typename first, typename... rest>
struct index_of_impl;

template <typename target, size_t idx, typename first, typename... rest>
struct index_of_impl<false, target, idx, first, rest...>
    : std::integral_constant<size_t, index_of<target, idx + 1, rest...>::value> {};

template <typename target, size_t idx, typename first, typename... rest>
struct index_of_impl<true, target, idx, first, rest...> : std::integral_constant<size_t, idx> {};

template <typename target, size_t idx, typename first, typename... rest>
struct index_of : index_of_impl<std::is_same<target, first>::value, target, idx, first, rest...> {};

template <typename target, size_t idx, typename first>
struct index_of<target, idx, first> {
    static_assert(std::is_same<target, first>::value, "Perf category not found in parameter pack");
    static constexpr size_t value = idx;
};

template <typename cat>
struct named_category {
    static constexpr const char *name() { return cat::name(); }
};

template <typename... cats>
struct cat_name_array {
    static const char *const values[sizeof...(cats)];
};

template <typename... cats>
const char *const cat_name_array<cats...>::values[sizeof...(cats)] = {named_category<cats>::name()...};

template <typename cat>
struct has_name {
private:
    template <typename t>
    static auto test(int) -> decltype(t::name(), std::true_type());

    template <typename>
    static std::false_type test(...);

public:
    static constexpr bool value = decltype(test<cat>(0))::value;
};

template <typename... cats>
struct all_named;

template <>
struct all_named<> : std::true_type {};

template <typename first, typename... rest>
struct all_named<first, rest...>
    : std::integral_constant<bool, has_name<first>::value && all_named<rest...>::value> {};

template <typename idx_t, typename... cats>
FORCE_INLINE constexpr const char *cat_name_by_index(idx_t idx)
{
    return cat_name_array<cats...>::values[static_cast<size_t>(idx)];
}
} /* namespace internal */

template <bool concurrent, typename... cats>
class NewPerfUsage {
public:
    static_assert(::internal::unique_types<cats...>::value, "Duplicate perf categories in PERFER(...)");
    static_assert(::internal::all_named<cats...>::value, "Perf category tag must provide static constexpr const char* name()");
#if PERF_USAGE_EVENT
    static_assert(!concurrent, "NewPerfUsage does not support concurrency for PERF_USAGE_EVENT");
#endif

    template <typename cat, PerfEventType... pts>
    void start(int64 count = 1)
    {
#if PERF_USAGE_COUNT
        counters[index_of<cat>::value].inc(count);
#endif
#if PERF_USAGE_TIME
        times[index_of<cat>::value].inc(-get_time_ns());
#endif
#if PERF_USAGE_EVENT
        using SupportedSet = typename PerfEventListToSet<typename SupportedPerf<pts...>::type>::type;
        auto *current = active_events[index_of<cat>::value];
        const void *tag = event_type_tag<SupportedSet>();
        if (!current || active_event_tags[index_of<cat>::value] != tag) {
            if (current) {
                delete current;
            }
            active_events[index_of<cat>::value] = new SupportedSet();
            active_event_tags[index_of<cat>::value] = tag;
        }
        active_events[index_of<cat>::value]->start();
#endif
    }
    template <typename cat>
    void stop()
    {
#if PERF_USAGE_TIME
        times[index_of<cat>::value].inc(get_time_ns());
#endif
#if PERF_USAGE_EVENT
        auto *e = active_events[index_of<cat>::value];
        e->stop();
        e->extract_counters(perf_counters);
#endif
    }
    void report(int elevel)
    {
#if PERF_USAGE_COUNT || PERF_USAGE_TIME || PERF_USAGE_EVENT
#if PERF_USAGE_TIME
        char buf[32];
#if PERF_USAGE_COUNT
        char avg_buf[32];
#endif
#endif
#if PERF_USAGE_EVENT
        char perf_buf[128];
#endif
        ann_helper::unroll<total_num>([&](auto i) -> void {
#if PERF_USAGE_COUNT
            int64 v = counters[i].value();
#if PERF_USAGE_TIME
            if (v == 0 && times[i].value() == 0) {
                return;
            }
#else
            if (v == 0) {
                return;
            }
#endif
#endif

#if PERF_USAGE_TIME
            int64 total_ns = times[i].value();
            ann_helper::Timer::ns_to_str(total_ns, buf);
#if PERF_USAGE_COUNT
            int64 avg_ns = (v > 0) ? (total_ns / v) : 0;
            ann_helper::Timer::ns_to_str(avg_ns, avg_buf);
#endif
#endif
#if PERF_USAGE_EVENT
            ::internal::perf_to_str(perf_counters, perf_buf);
#endif

#if PERF_USAGE_COUNT && PERF_USAGE_TIME && PERF_USAGE_EVENT
            ereport(elevel, (errcode(ERRCODE_LOG),
                errmsg("Operation %s: count: %ld, total time: %s, avg time: %s, perf: %s",
                    ::internal::cat_name_by_index<decltype(i), cats...>(i), v, buf, avg_buf, perf_buf)));
#elif PERF_USAGE_COUNT && PERF_USAGE_TIME
            ereport(elevel, (errcode(ERRCODE_LOG),
                errmsg("Operation %s: count: %ld, total time: %s, avg time: %s",
                    ::internal::cat_name_by_index<decltype(i), cats...>(i), v, buf, avg_buf)));
#elif PERF_USAGE_COUNT && PERF_USAGE_EVENT
            ereport(elevel, (errcode(ERRCODE_LOG),
                errmsg("Operation %s: count: %ld, perf: %s",
                    ::internal::cat_name_by_index<decltype(i), cats...>(i), v, perf_buf)));
#elif PERF_USAGE_TIME && PERF_USAGE_EVENT
            ereport(elevel, (errcode(ERRCODE_LOG),
                errmsg("Operation %s: total time: %s, perf: %s",
                    ::internal::cat_name_by_index<decltype(i), cats...>(i), buf, perf_buf)));
#elif PERF_USAGE_COUNT
            ereport(elevel, (errcode(ERRCODE_LOG),
                errmsg("Operation %s: count: %ld", ::internal::cat_name_by_index<decltype(i), cats...>(i), v)));
#elif PERF_USAGE_TIME
            ereport(elevel, (errcode(ERRCODE_LOG),
                errmsg("Operation %s: total time: %s", ::internal::cat_name_by_index<decltype(i), cats...>(i), buf)));
#elif PERF_USAGE_EVENT
            ereport(elevel, (errcode(ERRCODE_LOG),
                errmsg("Operation %s: perf: %s", ::internal::cat_name_by_index<decltype(i), cats...>(i), perf_buf)));
#endif
        });
#else
        (void)elevel;
#endif
    }
    void report_to(NewPerfUsage &perf)
    {
#if PERF_USAGE_COUNT || PERF_USAGE_TIME || PERF_USAGE_EVENT
        ann_helper::unroll<total_num>([&](auto i) {
#if PERF_USAGE_COUNT
            perf.counters[i].inc(counters[i]);
            counters[i].reset();
#endif
#if PERF_USAGE_TIME
            perf.times[i].inc(times[i]);
            times[i].reset();
#endif
        });
#if PERF_USAGE_EVENT
        ann_helper::unroll<PERF_EVENT_TYPE_COUNT>([&](auto i) {
            perf.perf_counters[i] += perf_counters[i];
        });
#endif
#endif
    }
    void perf_destroy()
    {
#if PERF_USAGE_EVENT
        ann_helper::unroll<total_num>([&](auto i) {
            if (active_events[i]) {
                active_events[i]->stop();
                delete active_events[i];
                active_events[i] = NULL;
            }
        });
#endif
    }

private:
    template <typename cat>
    struct index_of {
        static constexpr size_t value = ::internal::index_of<cat, 0, cats...>::value;
    };

    static constexpr size_t total_num = sizeof...(cats);

#if PERF_USAGE_COUNT || PERF_USAGE_TIME
    using counter_type = typename std::conditional<concurrent, ann_helper::ParaCounter, ann_helper::Counter>::type;
#endif
#if PERF_USAGE_COUNT
    counter_type counters[total_num];
#endif
#if PERF_USAGE_TIME
    static int64 get_time_ns() { return std::chrono::high_resolution_clock::now().time_since_epoch().count(); }
    counter_type times[total_num];
#endif
#if PERF_USAGE_EVENT
    template <typename T>
    static const void *event_type_tag()
    {
        static const char tag;
        return &tag;
    }

    ann_helper::perf_event_set_base *active_events[total_num] = {NULL};
    const void *active_event_tags[total_num] = {NULL};
    int64 perf_counters[PERF_EVENT_TYPE_COUNT] = {0};
#endif
};

#define PERF_DECLARE_CAT(r, data, elem) \
    struct BOOST_PP_CAT(tag_, elem) { \
        static constexpr const char *name() { return BOOST_PP_STRINGIZE(elem); } \
    };

#define PERF_EXPAND_CAT(r, scope, elem) scope::BOOST_PP_CAT(tag_, elem)

#define PERF_DECLARE_CATS(scope_name, concurrent, ...) \
    struct scope_name { \
        BOOST_PP_SEQ_FOR_EACH(PERF_DECLARE_CAT, _, BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__)) \
        using type = NewPerfUsage<concurrent, \
            BOOST_PP_SEQ_ENUM( \
                BOOST_PP_SEQ_TRANSFORM(PERF_EXPAND_CAT, scope_name, BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__)) \
            ) \
        >; \
    }

#define PERFER(scope_name) scope_name::type
#define DO_PERF_ON(perf, scope_name, cat, ...) \
    (perf).template start<scope_name::BOOST_PP_CAT(tag_, cat), ##__VA_ARGS__>()
#define DO_PERF_COUNT_ON(perf, scope_name, cat, count, ...) \
    (perf).template start<scope_name::BOOST_PP_CAT(tag_, cat), ##__VA_ARGS__>(count)
#define STOP_PERF_ON(perf, scope_name, cat) \
    (perf).template stop<scope_name::BOOST_PP_CAT(tag_, cat)>()
#define REPORT_PERF_ON(perf, elevel) (perf).report(elevel)
#define REPORT_TO_PERF_ON(perf, other) (perf).report_to(other)
#define PERF_DESTROY_ON(perf) (perf).perf_destroy()

/* keep member-style helpers for existing code */
#define DO_PERF(cat, ...) this->template start<typename PerfCats::BOOST_PP_CAT(tag_, cat), ##__VA_ARGS__>()
#define DO_PERF_COUNT(cat, count, ...) \
    this->template start<typename PerfCats::BOOST_PP_CAT(tag_, cat), ##__VA_ARGS__>(count)
#define STOP_PERF(cat) this->template stop<typename PerfCats::BOOST_PP_CAT(tag_, cat)>()
#define REPORT_PERF(elevel) this->report(elevel)
#define REPORT_TO_PERF(other) this->report_to(other)
#define PERF_DESTROY() this->perf_destroy()

#endif /* PERFUSAGE_H */
