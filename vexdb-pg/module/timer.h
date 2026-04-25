/**
 * Copyright ...
 * Time utilities.
 */

#ifndef ANN_TIMER_H
#define ANN_TIMER_H

#include <chrono>
#include <cstdio>
#include <utility>

#include "c.h"
#include "utils/elog.h"
#include "postmaster/bgworker.h"
#include "leak_checker.h"

/* PostgreSQL compatibility: IsBgWorkerProcess */
#ifndef IsBgWorkerProcess
#define IsBgWorkerProcess() (MyBgworkerEntry != NULL)
#endif

namespace ann_helper {
class Timer : public LeakChecker {
public:
    constexpr static size_t time_buf_len = 64ul;
    char buf[time_buf_len];
    std::chrono::time_point<std::chrono::high_resolution_clock> _start;
    size_t _nloop{0};
    size_t _step_size{0};
    size_t _nloop_count{0};
    bool   _need_report{false};
    bool   _nloop_count_unknown{false};
    int    _index_progress_slot{-1};
    char*  _index_name{NULL};
    char*  _part_index_name{NULL};
    char*  _stage{NULL};
    

    Timer() : _start(std::chrono::high_resolution_clock::now()) {}
    explicit Timer(size_t nloop)
        : _start(std::chrono::high_resolution_clock::now()), _nloop(nloop) {}
    Timer(size_t nloop, size_t step_size, char* index_name = NULL, char* part_index_name = NULL);
    void reset() { _start = std::chrono::high_resolution_clock::now(); }
    void set_stage(char* stage);
    void reset_step(size_t new_step_size)
    {
        _step_size = new_step_size;
        _nloop_count = 0;
    }
    void set_nloop(size_t v) { _nloop = v; }
    void set_nloop_count_unknown(bool flag) {
        _nloop_count_unknown = flag;
    }
    double elapsed() const
    {
        return std::chrono::duration_cast<std::chrono::duration<double>>(
            std::chrono::high_resolution_clock::now() - _start).count();
    }
    double elapsed_millis() const
    {
        return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
            std::chrono::high_resolution_clock::now() - _start).count();
    }
    double elapsed_micros() const
    {
        return std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(
            std::chrono::high_resolution_clock::now() - _start).count();
    }
    double elapsed_nanos() const
    {
        return std::chrono::duration_cast<std::chrono::duration<double, std::nano>>(
            std::chrono::high_resolution_clock::now() - _start).count();
    }

    static void ns_to_str(double ns, char *buf)
    {
        constexpr size_t us = 1000lu;
        constexpr size_t ms = us * 1000lu;
        constexpr size_t s = ms * 1000lu;
        constexpr size_t min = s * 60lu;
        constexpr size_t hour = min * 60lu;
        if (ns < us) {
            if (ns == 0) {
                sprintf(buf, "0");
            } else {
                sprintf(buf, "%lu ns", size_t(ns));
            }
        } else if (ns < ms) {
            sprintf(buf, "%.2f μs", ns / us);
        } else if (ns < s) {
            sprintf(buf, "%.2f ms", ns / ms);
        } else if (ns < min) {
            sprintf(buf, "%.3f s", ns / s);
        } else if (ns < hour) {
            size_t nmin = static_cast<size_t>(ns / min);
            ns -= nmin * min;
            if (ns >= s) {
                buf += sprintf(buf, "%lu min ", nmin);
                ns_to_str(ns, buf);
            } else {
                sprintf(buf, "%lu min", nmin);
            }
        } else {
            size_t nhour = static_cast<size_t>(ns / hour);
            ns -= nhour * hour;
            if (ns >= min) {
                buf += sprintf(buf, "%lu hour ", nhour);
                ns_to_str(ns, buf);
            } else {
                sprintf(buf, "%lu hour", nhour);
            }
        }
    }
    void report_elapsed() { ns_to_str(elapsed_nanos(), buf); }

    template <typename... Args>
    void report(const char *msg, Args &&...args) 
    {
        constexpr int init_size = 128;
        int size = init_size;
        char *msg_buf = (char *)palloc(size);
        int n;
        do {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-security"
            n = snprintf(msg_buf, size, msg, std::forward<Args>(args)...);
#pragma GCC diagnostic pop
            if (n < 0) {
                pfree(msg_buf);
                return;
            }
            if (n >= size) {
                size *= 2;
                msg_buf = (char *)repalloc(msg_buf, size);
            }
        } while (n >= size);
        ns_to_str(elapsed_nanos(), buf);
        ereport(NOTICE, (errmsg("%s: %s", msg_buf, buf)));
        pfree(msg_buf);
    }

    static char* append_msg(const char* msg, const char *new_flag) {
        char *new_msg = (char *)palloc(strlen(msg) + strlen(new_flag) + 1);
        sprintf(new_msg, "%s%s", msg, new_flag);
        return new_msg;
    }

    template <typename... Args>
    void report_loop(const char *msg, Args &&...args)
    {
        ++_nloop_count;
        char *new_msg = NULL;
        if (_step_size > 0) {
            if (_nloop_count % _step_size == 0) {
                if (_nloop > 0) {
                    new_msg = append_msg(msg, " (%lu/%lu)");
                    report(new_msg, _nloop_count, _nloop, std::forward<Args>(args)...);
                } else {
                    new_msg = append_msg(msg, " (%lu)");
                    report(new_msg, _nloop_count, std::forward<Args>(args)...);
                }
            }
        } else if (_nloop > 0) {
            new_msg = append_msg(msg, " (%lu/%lu)");
            report(new_msg, _nloop_count, _nloop, std::forward<Args>(args)...);
        } else {
            new_msg = append_msg(msg, " (%lu)");
            report(new_msg, _nloop_count, std::forward<Args>(args)...);
        }
        if (new_msg) {
            pfree(new_msg);
        }
    }

    template <typename... Args>
    void inc_loop_count_forground_report(const char *msg, Args &&...args)
    {
        size_t cur_nloop_count = __atomic_add_fetch(&_nloop_count, 1ul, __ATOMIC_RELAXED);
        if (cur_nloop_count % _step_size == 0) {
            _need_report = true;
        }

        forground_report(msg, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void forground_report(const char *msg, Args &&...args)
    {
        if(_need_report && !IsBgWorkerProcess()) {
            if (_step_size > 0) {
                char *new_msg = NULL;
                if (_nloop > 0) {
                    new_msg = append_msg(msg, " (%lu/%lu)");
                    report(new_msg, _nloop_count, _nloop, std::forward<Args>(args)...);
                } else {
                    new_msg = append_msg(msg, " (%lu)");
                    report(new_msg, _nloop_count, std::forward<Args>(args)...);
                }
                pfree(new_msg);
            }
            _need_report = false;
        }
    }
    
    void destroy();

};
} /* namespace ann_helper */

#endif /* ANN_TIMER_H */
