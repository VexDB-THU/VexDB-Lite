/**
 * Copyright ...
 * Utilities.
 */

#ifndef ANN_HELPER_EXPR_H
#define ANN_HELPER_EXPR_H

#include <cstdio>
#include <type_traits>
#include <utility>
#if __cplusplus < 201703L
#include <functional>
#endif /* c++14 or less */

#include <vtl/internal/container.hpp>

#if defined(__cplusplus) && __cplusplus >= 202302L
#define Assume(expr) [[assume(expr)]]
#elif defined(__clang_major__)
#define Assume(expr) __builtin_assume(expr)
#else
/* intentionally make it open (without do while) to hint compilers */
#define Assume(expr) Assert(expr); (expr) ? static_cast<void>(0) : __builtin_unreachable()
#endif

#if defined(__cplusplus) && __cplusplus >= 201703L
#define CONSTEXPR_IF if constexpr
#else
#define CONSTEXPR_IF if
#endif

#ifndef _GLIBCXX17_CONSTEXPR
#if __cplusplus >= 201703L
#define _GLIBCXX17_CONSTEXPR constexpr
#else
#define _GLIBCXX17_CONSTEXPR
#endif
#endif

#ifndef _GLIBCXX17_INLINE
#if __cplusplus >= 201703L
#define _GLIBCXX17_INLINE inline
#else
#define _GLIBCXX17_INLINE
#endif
#endif

#ifndef _GLIBCXX17_NO_STATIC
#if __cplusplus >= 201703L
#define _GLIBCXX17_NO_STATIC
#else
#define _GLIBCXX17_NO_STATIC static
#endif
#endif

#ifdef _MSC_VER 
#define NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#elif defined(__cplusplus) && __cplusplus >= 202002L
#define NO_UNIQUE_ADDRESS [[no_unique_address]]
#else
#define NO_UNIQUE_ADDRESS
#endif

#if __cplusplus >= 201703L  /* c++17 or greater */
#define IS_INVOCABLE_R(call_type, ret_type, ...) std::is_invocable_r_v<ret_type, call_type, __VA_ARGS__>
#define IS_INVOCABLE(call_type, ...) std::is_invocable_v<call_type, __VA_ARGS__>
#define RESULT_OF(call_type, ...) std::invoke_result_t<call_type, __VA_ARGS__>
#else
#define IS_INVOCABLE_R(call_type, ret_type, ...) std::is_constructible< \
    std::function<ret_type(__VA_ARGS__)>,                               \
    std::reference_wrapper<typename std::remove_reference<call_type>::type>>::value
#define IS_INVOCABLE(call_type, ...) std::is_constructible< \
    std::function<void(__VA_ARGS__)>,                       \
    std::reference_wrapper<typename std::remove_reference<call_type>::type>>::value
#if __cplusplus >= 201402L    /* c++14 */
#define RESULT_OF(call_type, ...) std::result_of_t<call_type(__VA_ARGS__)>
#else /* c++11 or less */
#define RESULT_OF(call_type, ...) typename std::result_of<call_type(__VA_ARGS__)>::type
#endif
#endif
#define PureType(in_type) typename std::remove_cv<typename std::remove_reference<in_type>::type>::type

namespace ann_helper {
namespace internal {
template <typename>
constexpr std::false_type has_destroyer_h(long);
template <typename T>
constexpr auto has_destroyer_h(int) -> decltype(std::declval<T>().destroy(), std::true_type{});
template <typename T>
using has_destroyer = decltype(has_destroyer_h<T>(0));
template <typename T>
void optional_destroy(T &obj, std::true_type const &) { obj.destroy(); }
template <typename T>
void optional_destroy(T &obj, std::false_type const &) {}
template <typename>
constexpr std::false_type has_constructor_with_alloc_h(long);
template <typename T>
constexpr auto has_constructor_with_alloc_h(int) ->
    decltype(T::construct_with_alloc, std::true_type{});
template <typename T>
using has_constructor_with_alloc = decltype(has_constructor_with_alloc_h<T>(0));
template <typename T>
constexpr bool constructor_with_alloc(std::false_type const &)
{
    return !std::is_scalar<T>::value &&
        !std::is_trivially_default_constructible<T>::value;
}
template <typename T>
constexpr bool constructor_with_alloc(std::true_type const &)
{
    return !std::is_scalar<T>::value &&
        !std::is_trivially_default_constructible<T>::value &&
        T::construct_with_alloc;
}

template <typename T>
struct constructor_with_alloc_st {
    static constexpr bool value = constructor_with_alloc<T>(has_constructor_with_alloc<T>{});
};
template <typename T1, typename T2>
struct constructor_with_alloc_st<std::pair<T1, T2>> {
    static constexpr bool value =
        constructor_with_alloc<T1>(has_constructor_with_alloc<T1>{}) ||
        constructor_with_alloc<T2>(has_constructor_with_alloc<T2>{});
};
} /* internal */
/* call destroy if the object has a destroy() function */
template <typename T>
inline void optional_destroy(T &obj)
    { internal::optional_destroy(obj, internal::has_destroyer<T>{}); }
template <typename T>
inline void optional_destroy_destruct(T &obj)
{
    internal::optional_destroy(obj, internal::has_destroyer<T>{});
    obj.~T();
}
/* return constructor_with_alloc if a class has one, otherwise true */
template <typename T>
constexpr bool constructor_with_alloc =
    internal::constructor_with_alloc_st<T>::value;
template <typename T>
constexpr bool constructor_need_ctx = constructor_with_alloc<T>;

inline void print_size(size_t nbytes, char *buf)
{
    if (nbytes < 1024) {
        sprintf(buf, "%lu B", nbytes);
    } else if (nbytes < 1024 * 1024) {
        sprintf(buf, "%.2f KB", nbytes / 1024.0);
    } else if (nbytes < 1024 * 1024 * 1024) {
        sprintf(buf, "%.2f MB", nbytes / 1024.0 / 1024.0);
    } else {
        sprintf(buf, "%.2f GB", nbytes / 1024.0 / 1024.0 / 1024.0);
    }
}

#if __cplusplus >= 201703L
template <size_t n, typename F, size_t... i>
constexpr auto unroll(F &&f, std::index_sequence<i...> = {})
{
    if constexpr (sizeof...(i) != n) {
        return unroll<n>(std::forward<F>(f), std::make_index_sequence<n>());
    } else {
        using result_t = decltype(f(std::integral_constant<size_t, 0>()));
        if constexpr (std::is_void_v<result_t>) {
            return (f(std::integral_constant<size_t, i>()), ...);
        } else {
            return (f(std::integral_constant<size_t, i>()) && ...);
        }
    }
}

/**
 * bad implementation with large code size:
template <size_t B, size_t E, typename F, size_t... i>
void unroll_leftovers(size_t base, size_t n, F &&f, std::index_sequence<i...> = {})
{
    if constexpr (E - B <= 1) {
        static_assert(E - B == 1);
        if (n) {
            f(base + B);
        }
    } else {
        constexpr auto M = (E - B) / 2;
        if constexpr (sizeof...(i) != M) {
            (f(base + i + B), ...);
            unroll_leftovers<B + M, E>(base, n - M, std::forward<F>(f));
        } else {
            unroll_leftovers<B, E - M>(base, n, std::forward<F>(f));
        }
    }
}
 */
template <size_t n, typename F, size_t... i>
void unroll_leftovers(size_t base, size_t m, F &&f, std::index_sequence<i...> = {})
{
    if constexpr (n > 0) {
        if constexpr (sizeof...(i) != n / 2) {
            unroll_leftovers<n>(base, m, std::forward<F>(f), std::make_index_sequence<n / 2>());
        } else {
            auto m2 = m >= n / 2 ? m - n / 2 : m;
            unroll_leftovers<n / 2>(base, m2, std::forward<F>(f));
            if (m >= n / 2) {
                (f(base + m2 + i), ...);
            }
        }
    }
}
#elif __cplusplus >= 201402L
template <typename F>
constexpr bool unroll_bool_impl(F &&, std::index_sequence<>)
    { return true; }
template <typename F, size_t i0, size_t... i>
constexpr bool unroll_bool_impl(F &&f, std::index_sequence<i0, i...>)
{
    return f(std::integral_constant<size_t, i0>()) &&
        unroll_bool_impl(std::forward<F>(f), std::index_sequence<i...>{});
}

template <typename F>
constexpr void unroll_nonbool_impl(F &&, std::index_sequence<>) {}
template <typename F, size_t i0>
constexpr auto unroll_nonbool_impl(F &&f, std::index_sequence<i0>)
    -> RESULT_OF(F, std::integral_constant<size_t, i0>)
    { return f(std::integral_constant<size_t, i0>()); }
template <typename F, size_t i0, size_t i1, size_t... i>
constexpr auto unroll_nonbool_impl(F &&f, std::index_sequence<i0, i1, i...>) -> decltype(auto)
{
    f(std::integral_constant<size_t, i0>());
    return unroll_nonbool_impl(std::forward<F>(f), std::index_sequence<i1, i...>{});
}

template <size_t n, typename F, size_t... i, typename std::enable_if<
    !std::is_same<
        RESULT_OF(F, std::integral_constant<size_t, 0>), bool
    >::value, int>::type = 0>
constexpr auto unroll_impl(F &&f, std::index_sequence<i...>) -> decltype(auto)
    { return unroll_nonbool_impl(std::forward<F>(f), std::index_sequence<i...>{}); }
template <size_t n, typename F, size_t... i, typename std::enable_if<
    std::is_same<
        RESULT_OF(F, std::integral_constant<size_t, 0>), bool
    >::value, long>::type = 0>
constexpr auto unroll_impl(F &&f, std::index_sequence<i...>)
    { return unroll_bool_impl(f, std::index_sequence<i...>{}); }
template <size_t n, typename F>
constexpr auto unroll(F &&f)
    { return unroll_impl<n>(std::forward<F>(f), std::make_index_sequence<n>()); }
#endif
} /* namespace ann_helper */

#endif /* ANN_HELPER_EXPR_H */
