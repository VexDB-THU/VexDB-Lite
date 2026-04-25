/**
 * Copyright ...
 */

#ifndef TEMPFILE_VECTOR_H
#define TEMPFILE_VECTOR_H

#include <vtl/bitlock.hpp>
#include <vtl/internal/container.hpp>
#include <vtl/internal/expr.hpp>

#include "postgres.h"
#include "storage/block.h"
#include "storage/buf/buffile.h"

namespace disk_container {
template <typename T, bool concurrent = false>
class TempFileVector {
public:
    struct iterator {
        size_t idx;
        iterator(size_t i, TempFileVector &v) : idx(i), vec(v) {}
        iterator(const iterator &other) : idx(other.idx), vec(other.vec) {}
        iterator(iterator &&other) : idx(other.idx), vec(other.vec) {}
        T operator*() { return vec.get(idx); }
        bool operator==(const iterator &other) const { return idx == other.idx; }
        bool operator!=(const iterator &other) const { return idx != other.idx; }
        iterator &operator++() { ++idx; return *this; }
        iterator &operator--() { idx = idx == 0 ? vec.size() : idx - 1; return *this; }
    private:
        TempFileVector &vec;
    };
    using const_iterator = iterator;
    constexpr static bool return_reference = false;

    TempFileVector()
    {
        static_assert(!concurrent, "concurrent mode not supported");
        static_assert(std::is_standard_layout<T>::value, "TempFileVector only applies to POD types");
        _file = BufFileCreateTemp(false);
        BufFileTell(_file, &_file_no, &_file_offset);
        pthread_mutex_init(&_mutex, NULL);
    }
    ~TempFileVector() {}
    void swap(TempFileVector &other)
    {
        std::swap(_size, other._size);
        std::swap(_file, other._file);
        std::swap(_file_no, other._file_no);
        std::swap(_file_offset, other._file_offset);
    }

    template <AccessorLockType lock_type = AccessorLockType::NoLockUnsafe> T get(size_t idx)
    {
        T res;
        lock<lock_type>(idx);
        seek(idx);
        read(&res, 1);
        unlock<lock_type>(idx);
        return res;
    }
    template <AccessorLockType lock_type = AccessorLockType::NoLockUnsafe> void get_n(size_t idx, size_t n, T *dest)
    {
        lock<lock_type>(idx);
        seek(idx);
        read(dest, n);
        unlock<lock_type>(idx);
    }
    inline T operator[](size_t idx) { return get(idx); }
    iterator at(size_t idx) { return iterator(std::min(idx, _size), *this); }

    /* currently we don't do anything with locks */

    template <AccessorLockType lock_type = AccessorLockType::NoLockUnsafe> void set(size_t idx, const T &elem)
    {
        lock<lock_type>(idx);
        seek(idx);
        write(&elem, 1);
        unlock<lock_type>(idx);
    }
    template <AccessorLockType lock_type = AccessorLockType::NoLockUnsafe> void set_n(size_t idx, size_t n, const T *elem)
    {
        lock<lock_type>(idx);
        seek(idx);
        write(elem, n);
        unlock<lock_type>(idx);
    }
    template <AccessorLockType lock_type = AccessorLockType::NoLockUnsafe> void set_n(size_t idx, size_t n, const T &elem)
    {
        lock<lock_type>(idx);
        seek(idx);
        for (size_t i = 0; i < n; ++i) {
            write(&elem, 1);
            seek_cur(sizeof(T));
        }
        unlock<lock_type>(idx);
    }

    template <AccessorLockType lock_type, class F> struct Applier {
        Applier(TempFileVector<T, concurrent> &vector, const F &func) : _vector(vector), _apply_func(func)
        {
            static_assert(IS_INVOCABLE_R(F, bool, T &), "F must be invocable with T & and return bool");
        }
        Applier(TempFileVector<T, concurrent> &vector, F &&func) : _vector(vector), _apply_func(std::move(func))
        {
            static_assert(IS_INVOCABLE_R(F, bool, T &), "F must be invocable with T & and return bool");
        }
        bool operator()(size_t idx)
        {
            T elem;
            _vector.template lock<lock_type>(idx);
            _vector.seek(idx);
            _vector.read(&elem, 1);
            bool res = _apply_func(elem);
            if (res) {
                _vector.seek(idx);
                _vector.write(&elem, 1);
            }
            _vector.template unlock<lock_type>(idx);
            return res;
        }

        friend class TempFileVector<T, concurrent>;
    private:
        TempFileVector<T, concurrent> &_vector;
        F _apply_func;
    };
    template <AccessorLockType lock_type, class F> inline Applier<lock_type, F> apply(const F &func) { return Applier<lock_type, F>(*this, func); }
    template <AccessorLockType lock_type, class F> inline Applier<lock_type, F> apply(F &&func) { return Applier<lock_type, F>(*this, std::move(func)); }
    template <AccessorLockType lock_type, class F> struct Visitor {
        Visitor(TempFileVector<T, concurrent> &vector, const F &func) : _vector(vector), _visit_func(func)
        {
            static_assert(IS_INVOCABLE_R(F, void, const T *, size_t),
                          "F must be invocable with (T *, size_t) and return void");
        }
        Visitor(TempFileVector<T, concurrent> &vector, F &&func) : _vector(vector), _visit_func(std::move(func))
        {
            static_assert(IS_INVOCABLE_R(F, void, const T *, size_t),
                          "F must be invocable with (T *, size_t) and return void");
        }
        void operator()(size_t idx, size_t n)
        {
            T *buf = (T *)palloc0(n * sizeof(T));
            _vector.template lock<lock_type>(idx);
            _vector.seek(idx);
            _vector.read(buf, n);
            _visit_func(buf, n);
            _vector.template unlock<lock_type>(idx);
            pfree(buf);
        }

        friend class TempFileVector<T, concurrent>;
    private:
        TempFileVector<T, concurrent> &_vector;
        F _visit_func;
    };
    template <AccessorLockType lock_type, class F> inline Visitor<lock_type, F> visit(const F &func) { return Visitor<lock_type, F>(*this, func); }
    template <AccessorLockType lock_type, class F> inline Visitor<lock_type, F> visit(F &&func) { return Visitor<lock_type, F>(*this, std::move(func)); }

    void reserve(size_t expect_size)
    {
        pthread_mutex_lock(&_mutex);
        auto on_exit = [this]() { pthread_mutex_unlock(&_mutex); };
        if (_size < expect_size) {
            seek(_size, on_exit);
            constexpr size_t buf_len = 1'000'000;
            constexpr size_t buf_step = buf_len / sizeof(T);
            T *buf = (T *)palloc0(buf_step * sizeof(T));
            for (size_t i = _size; i < expect_size; i += buf_step) {
                size_t write_size = std::min(buf_step, expect_size - i);
                write(buf, write_size, on_exit);
                seek_cur(write_size * sizeof(T), on_exit);
            }
            pfree(buf);
            _size = expect_size;
            _lock.resize(_size);
        }
        pthread_mutex_unlock(&_mutex);
    }
    void resize(size_t expect_size) { shrink(expect_size); extend(expect_size); }
    size_t push_back(const T &elem)
    {
        pthread_mutex_lock(&_mutex);
        auto on_exit = [this]() { pthread_mutex_unlock(&_mutex); };
        seek(_size, on_exit);
        write(&elem, 1, on_exit);
        size_t res = _size;
        ++_size;
        _lock.resize(_size);
        pthread_mutex_unlock(&_mutex);
        return res;
    }
    template <typename ...Args>
    size_t emplace_back(Args &&...args)
    {
        pthread_mutex_lock(&_mutex);
        auto on_exit = [this]() { pthread_mutex_unlock(&_mutex); };
        seek(_size, on_exit);
        T elem(std::forward<Args>(args)...);
        write(&elem, 1, on_exit);
        ann_helper::optional_destroy(elem);
        size_t res = _size;
        ++_size;
        _lock.resize(_size);
        pthread_mutex_unlock(&_mutex);
        return res;
    }

    size_t push_back_n(const T &elem, size_t n)
    {
        pthread_mutex_lock(&_mutex);
        auto on_exit = [this]() { pthread_mutex_unlock(&_mutex); };
        seek(_size, on_exit);
        for (size_t i = 0; i < n; ++i) {
            write(&elem, 1, on_exit);
            seek_cur(sizeof(T), on_exit);
        }
        size_t res = _size;
        _size += n;
        _lock.resize(_size);
        pthread_mutex_unlock(&_mutex);
        return res;
    }
    size_t push_back_n(const T *elem, size_t n)
    {
        pthread_mutex_lock(&_mutex);
        auto on_exit = [this]() { pthread_mutex_unlock(&_mutex); };
        seek(_size, on_exit);
        write(elem, n, on_exit);
        size_t res = _size;
        _size += n;
        _lock.resize(_size);
        pthread_mutex_unlock(&_mutex);
        return res;
    }

    iterator begin() { return iterator(0, *this); }
    iterator end() { return iterator(_size, *this); }
    const_iterator cbegin() { return iterator(0, *this); }
    const_iterator cend() { return iterator(_size, *this); }

    void extend(size_t size) { reserve(size); }
    void shrink(size_t size) { _size = std::min(size, _size); }
    inline size_t size() { return _size; }
    inline bool empty() { return _size == 0; }
    inline void prefetch(size_t start, size_t len) { /* no-op */ }
    inline void destroy()
    {
        pthread_mutex_destroy(&_mutex);
        ann_helper::optional_destroy(_lock);
        BufFileClose(_file);
    }
    void clear() { _size = 0; }
private:
    size_t _size{0};
    BufFile *_file;
    int _file_no;
    off_t _file_offset;
    pthread_mutex_t _mutex;
    NO_UNIQUE_ADDRESS std::conditional_t<concurrent, BitLock, DummyLocker> _lock{};

    template <class F>
    void seek(size_t idx, const F &on_error)
    {
        if (BufFileSeek(_file, _file_no, idx * sizeof(T), SEEK_SET) != 0) {
            on_error();
            ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("Invalid access idx: %lu", _size)));
        }
    }
    template <class F>
    void seek_cur(size_t len, const F &on_error)
    {
        if (BufFileSeek(_file, _file_no, len, SEEK_CUR) != 0) {
            on_error();
            ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("Seek failed")));
        }
    }
    template <class F>
    void read(T *dest, size_t n, const F &on_error)
    {
        if (BufFileRead(_file, dest, n * sizeof(T)) != n * sizeof(T)) {
            on_error();
            ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("Read failed")));
        }
    }
    template <class F>
    void write(const T *src, size_t n, const F &on_error)
    {
        if (BufFileWrite(_file, src, n * sizeof(T)) != n * sizeof(T)) {
            on_error();
            ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("Write failed")));
        }
    }
    static void dummy_error_handler() {}
    void seek(size_t idx) { seek(idx, dummy_error_handler); }
    void seek_cur(size_t len) { seek_cur(len, dummy_error_handler); }
    void read(T *dest, size_t n) { read(dest, n, dummy_error_handler); }
    void write(const T *src, size_t n) { write(src, n, dummy_error_handler); }

    template <AccessorLockType lock_type> void lock(size_t idx)
    {
        if (lock_type == AccessorLockType::ReadLock || lock_type == AccessorLockType::WriteLock) {
            _lock.lock(idx);
        }
    }
    template <AccessorLockType lock_type> void unlock(size_t idx)
    {
        if (lock_type == AccessorLockType::ReadLock || lock_type == AccessorLockType::WriteLock) {
            _lock.unlock(idx);
        }
    }
};

template <typename T> using ThreadUnsafeTempFileVector = TempFileVector<T, false>;
template <typename T> using ThreadSafeTempFileVector = TempFileVector<T, true>; /* not implemented */
} /* namespace disk_container */

#endif /* TEMPFILE_VECTOR_H */
