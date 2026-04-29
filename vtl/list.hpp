/**
 * Copyright ...
 */

#ifndef CONTAINER_LIST_H
#define CONTAINER_LIST_H

#include <type_traits>  /* is_trivially_constructible_v */
#include <new>          /* placement new */
#include <utility>      /* forward */
#include <atomic>       /* atomic_flag */
#include <mutex>        /* lock_guard */
#include <vtl/internal/container.hpp>
#include <vtl/optional>
#include <vtl/internal/expr.hpp>
#include "utils/palloc.h"

namespace vtl {
template <typename T, class L, class Cell>
struct ListIterator {
    Cell *cur;
    ListIterator(Cell *cur) : cur(cur) {}
    template <typename... Args>
    ListIterator(L &list, Args &&...args) : cur(list.emplace_back(std::forward<Args>(args)...)) {}
    ListIterator &operator++() { cur = cur->next; return *this; }
    T &operator*() { return cur->data; }
    const T &operator*() const { return cur->data; }
    T *operator->() { return &cur->data; }
    const T *operator->() const { return &cur->data; }
    bool operator==(const ListIterator &other) const { return cur == other.cur; }
    bool operator!=(const ListIterator &other) const { return cur != other.cur; }
};

template <typename T, class Cell>
struct ListConstIterator {
    const Cell *cur;
    /* intensionally implicit */
    ListConstIterator(const Cell *cur) : cur(cur) {}
    ListConstIterator &operator++() { cur = cur->next; return *this; }
    const T &operator*() const { return cur->data; }
    const T *operator->() const { return &cur->data; }
    bool operator==(const ListConstIterator &other) const { return cur == other.cur; }
    bool operator!=(const ListConstIterator &other) const { return cur != other.cur; }
};

template <typename T>
class List : public BaseObject {
public:
    struct ListCell : public BaseObject {
        T data;
        ListCell *next;

        ListCell(const T &data) : data(data), next(NULL) {}
        ListCell(T &&data) : data(std::move(data)), next(NULL) {}
        template <typename... Args>
        ListCell(Args &&...args) : data(std::forward<Args>(args)...), next(NULL) {}
    };

    using iterator = ListIterator<T, List, ListCell>;
    using const_iterator = ListConstIterator<T, ListCell>;

    /* Caller is responsible for managing the memory context, the default is CurrentMemoryContext */
    explicit List(MemoryContext ctx = NULL) : _ctx(ctx)
    {
        if (!_ctx) {
            _ctx = CurrentMemoryContext;
        }
    }
    List(const List &other) : List(other._ctx)
    {
        ListCell *cur = other._head;
        while (cur) {
            push_back(cur->data);
            cur = cur->next;
        }
    }
    List(List &&other) : _ctx(other._ctx), _head(other._head), _tail(other._tail)
    {
        other._ctx = other._head = other._tail = NULL;
    }
    List &operator=(const List &other)
    {
        if (this != &other) {
            destroy();
            _ctx = other._ctx;
            ListCell *cur = other._head;
            while (cur) {
                push_back(cur->data);
                cur = cur->next;
            }
        }
        return *this;
    }
    List &operator=(List &&other)
    {
        if (this != &other) {
            destroy();
            swap(other);
        }
        return *this;
    }
    void swap(List &other)
    {
        std::swap(_head, other._head);
        std::swap(_tail, other._tail);
        std::swap(_ctx, other._ctx);
    }

    ListCell *push_back(const T &data)
    {
        ListCell *cell = alloc_new_cell(data);
        if (!_head) {
            _head = _tail = cell;
        } else {
            _tail->next = cell;
            _tail = cell;
        }
        return cell;
    }
    ListCell *push_front(const T &data)
    {
        ListCell *cell = alloc_new_cell(data);
        if (!_head) {
            _head = _tail = cell;
        } else {
            cell->next = _head;
            _head = cell;
        }
        return cell;
    }
    template <typename... Args>
    ListCell *emplace_back(Args &&...args)
    {
        ListCell *cell = alloc_new_cell(std::forward<Args>(args)...);
        if (!_head) {
            _head = _tail = cell;
        } else {
            _tail->next = cell;
            _tail = cell;
        }
        return cell;
    }
    template <typename... Args>
    ListCell *emplace_front(Args &&...args)
    {
        ListCell *cell = alloc_new_cell(std::forward<Args>(args)...);
        if (!_head) {
            _head = _tail = cell;
        } else {
            cell->next = _head;
            _head = cell;
        }
        return cell;
    }
    void pop_back()
    {
        if (!_head) {
            return;
        }
        if (_head == _tail) {
            free_cell(_head);
            _head = _tail = NULL;
            return;
        }
        ListCell *cur = _head;
        while (cur->next != _tail) {
            cur = cur->next;
        }
        free_cell(_tail);
        _tail = cur;
        _tail->next = NULL;
    }
    void pop_front()
    {
        if (!_head) {
            return;
        }
        if (_head == _tail) {
            free_cell(_head);
            _head = _tail = NULL;
            return;
        }
        ListCell *tmp = _head;
        _head = _head->next;
        free_cell(tmp);
    }

    size_t size() const
    {
        size_t cnt = 0;
        ListCell *cur = _head;
        while (cur) {
            ++cnt;
            cur = cur->next;
        }
        return cnt;
    }
    bool empty() const { return _head == NULL; }

    iterator begin() { return _head; }
    iterator end() { return NULL; }
    const_iterator cbegin() const { return _head; }
    const_iterator cend() const { return NULL; }
    T &front() { Assert(_head); return _head->data; }
    const T &front() const { Assert(_head); return _head->data; }
    T &back() { Assert(_tail); return _tail->data; }
    const T &back() const { Assert(_tail); return _tail->data; }

    void erase(iterator it) { erase(it.cur); }
    void destroy()
    {
        ListCell *cur = _head;
        while (cur) {
            ListCell *tmp = cur;
            cur = cur->next;
            free_cell(tmp);
        }
        _head = _tail = NULL;
        _ctx = NULL;
    }
    void clear() { destroy(); }
private:
    MemoryContext _ctx{NULL};
    ListCell *_head{NULL};
    ListCell *_tail{NULL};

    ListCell *alloc_new_cell(const T &data)
    {
        ListCell *cell;
        if (std::is_standard_layout<T>::value) {
            cell = New(_ctx) ListCell(data);
        } else {
            auto ctx = MemoryContextSwitchTo(_ctx);            
            cell = New(_ctx) ListCell(data);
            MemoryContextSwitchTo(ctx);
        }
        return cell;
    }
    ListCell *alloc_new_cell(T &&data)
    {
        /* no mem alloc should happen during move */
        return New(_ctx) ListCell(data);
    }
    template <typename... Args>
    ListCell *alloc_new_cell(Args &&...args)
    {
        ListCell *cell;
        if (std::is_standard_layout<T>::value) {
            cell = New(_ctx) ListCell(std::forward<Args>(args)...);
        } else {
            auto ctx = MemoryContextSwitchTo(_ctx);
            cell = New(_ctx) ListCell(std::forward<Args>(args)...);
            MemoryContextSwitchTo(ctx);
        }
        return cell;
    }
    void free_cell(ListCell *cell)
    {
        ann_helper::optional_destroy(cell->data);
        ann_helper::optional_destroy(cell);
        delete cell;
    }

    void erase(ListCell *cell)
    {
        if (cell == _head) {
            _head = _head->next;
            free_cell(cell);
            return;
        }
        ListCell *cur = _head;
        while (cur->next != cell) {
            cur = cur->next;
        }
        cur->next = cell->next;
        free_cell(cell);
    }
};

template <typename T>
class DLList : public BaseObject {
public:
    struct DLListCell : public BaseObject {
        T data;
        DLListCell *prev;
        DLListCell *next;

        DLListCell(const T &data) : data(data), prev(NULL), next(NULL) {}
        DLListCell(T &&data) : data(std::move(data)), prev(NULL), next(NULL) {}
        template <typename... Args>
        DLListCell(Args &&...args) : data(std::forward<Args>(args)...), prev(NULL), next(NULL) {}
    };

    using iterator = ListIterator<T, DLList, DLListCell>;
    using const_iterator = ListConstIterator<T, DLListCell>;

    explicit DLList(MemoryContext ctx) : _ctx(ctx)
    {
        if (!_ctx) {
            _ctx = CurrentMemoryContext;
        }
    }
    DLList(const DLList &other) : DLList(other._ctx)
    {
        DLListCell *cur = other._head;
        while (cur) {
            push_back(cur->data);
            cur = cur->next;
        }
    }
    DLList(DLList &&other) : _ctx(other._ctx), _head(other._head), _tail(other._tail)
    {
        other._ctx = other._head = other._tail = NULL;
    }
    DLList &operator=(const DLList &other)
    {
        if (this != &other) {
            destroy();
            _ctx = other._ctx;
            DLListCell *cur = other._head;
            while (cur) {
                push_back(cur->data);
                cur = cur->next;
            }
        }
        return *this;
    }
    DLList &operator=(DLList &&other)
    {
        if (this != &other) {
            destroy();
            swap(other);
        }
        return *this;
    }
    void swap(DLList &other)
    {
        std::swap(_head, other._head);
        std::swap(_tail, other._tail);
        std::swap(_ctx, other._ctx);
    }

    DLListCell *push_back(const T &data)
    {
        DLListCell *cell = alloc_new_cell(data);
        if (!_head) {
            _head = _tail = cell;
        } else {
            _tail->next = cell;
            cell->prev = _tail;
            _tail = cell;
        }
        return cell;
    }
    DLListCell *push_front(const T &data)
    {
        DLListCell *cell = alloc_new_cell(data);
        if (!_head) {
            _head = _tail = cell;
        } else {
            cell->next = _head;
            _head->prev = cell;
            _head = cell;
        }
        return cell;
    }
    template <typename... Args>
    DLListCell *emplace_back(Args &&...args)
    {
        DLListCell *cell = alloc_new_cell(std::forward<Args>(args)...);
        if (!_head) {
            _head = _tail = cell;
        } else {
            _tail->next = cell;
            cell->prev = _tail;
            _tail = cell;
        }
        return cell;
    }
    template <typename... Args>
    DLListCell *emplace_front(Args &&...args)
    {
        DLListCell *cell = alloc_new_cell(std::forward<Args>(args)...);
        if (!_head) {
            _head = _tail = cell;
        } else {
            cell->next = _head;
            _head->prev = cell;
            _head = cell;
        }
        return cell;
    }
    void pop_back()
    {
        if (!_head) {
            return;
        }
        if (_head == _tail) {
            free_cell(_head);
            _head = _tail = NULL;
            return;
        }
        DLListCell *tmp = _tail;
        _tail = _tail->prev;
        free_cell(tmp);
        _tail->next = NULL;
    }
    void pop_front()
    {
        if (!_head) {
            return;
        }
        if (_head == _tail) {
            free_cell(_head);
            _head = _tail = NULL;
            return;
        }
        DLListCell *tmp = _head;
        _head = _head->next;
        free_cell(tmp);
        _head->prev = NULL;
    }

    size_t size() const
    {
        size_t cnt = 0;
        DLListCell *cur = _head;
        while (cur) {
            ++cnt;
            cur = cur->next;
        }
        return cnt;
    }
    bool empty() const { return _head == NULL; }

    iterator begin() { return _head; }
    iterator last() { return _tail; }
    iterator end() { return NULL; }
    const_iterator cbegin() const { return _head; }
    const_iterator clast() const { return _tail; }
    const_iterator cend() const { return NULL; }
    T &front() { Assert(_head); return _head->data; }
    const T &front() const { Assert(_head); return _head->data; }
    T &back() { Assert(_tail); return _tail->data; }
    const T &back() const { Assert(_tail); return _tail->data; }

    void move_back(iterator it) { move_back(it.cur); }
    void move_back(const_iterator it) { move_back(iterator(it.cur)); }
    void erase(iterator it) { erase(it.cur); }
    void destroy()
    {
        DLListCell *cur = _head;
        while (cur) {
            DLListCell *tmp = cur;
            cur = cur->next;
            free_cell(tmp);
            delete tmp;
        }
        _head = _tail = NULL;
        _ctx = NULL;
    }
    void clear() { destroy(); }
private:
    MemoryContext _ctx{NULL};
    DLListCell *_head{NULL};
    DLListCell *_tail{NULL};

    DLListCell *alloc_new_cell(const T &data)
    {
        DLListCell *cell;
        if (std::is_standard_layout<T>::value) {
            cell = New(_ctx) DLListCell(data);
        } else {
            auto ctx = MemoryContextSwitchTo(_ctx);
            cell = New(_ctx) DLListCell(data);
            MemoryContextSwitchTo(ctx);
        }
        return cell;
    }
    DLListCell *alloc_new_cell(T &&data)
    {
        /* no mem alloc should happen during move */
        return New(_ctx) DLListCell(data);
    }
    template <typename... Args>
    DLListCell *alloc_new_cell(Args &&...args)
    {
        DLListCell *cell;
        if (std::is_standard_layout<T>::value) {
            cell = New(_ctx) DLListCell(std::forward<Args>(args)...);
        } else {
            auto ctx = MemoryContextSwitchTo(_ctx);
            cell = New(_ctx) DLListCell(std::forward<Args>(args)...);
            MemoryContextSwitchTo(ctx);
        }
        return cell;
    }
    void free_cell(DLListCell *cell)
    {
        ann_helper::optional_destroy(cell->data);
        ann_helper::optional_destroy(cell);
        delete cell;
    }

    void erase(DLListCell *cell)
    {
        if (cell == _head) {
            _head = _head->next;
            free_cell(cell);
            return;
        }
        if (cell == _tail) {
            _tail = _tail->prev;
            free_cell(cell);
            return;
        }
        cell->prev->next = cell->next;
        cell->next->prev = cell->prev;
        free_cell(cell);
    }
    void move_back(DLListCell *cell)
    {
        if (cell == _tail) {
            return;
        }
        if (cell == _head) {
            _head = _head->next;
            _head->prev = NULL;
            _tail->next = cell;
            cell->prev = _tail;
            cell->next = NULL;
            _tail = cell;
            return;
        }
        cell->prev->next = cell->next;
        cell->next->prev = cell->prev;
        _tail->next = cell;
        cell->prev = _tail;
        cell->next = NULL;
        _tail = cell;
    }
};

template <typename T, bool use_spin_lock = false>
class ConcurrentList : public BaseObject {
public:
    class SpinLock {
    public:
        void lock() { while (locked.test_and_set(std::memory_order_acquire)) {} }
        bool try_lock() { return !locked.test_and_set(std::memory_order_acquire); }
        void unlock() { locked.clear(std::memory_order_release); }
    private:
        std::atomic_flag locked{ATOMIC_FLAG_INIT};
    };
    using lock_type = SpinLock;
    struct ConcurrentListCell : public BaseObject {
        lock_type slock{};
        T data;
        ConcurrentListCell *prev{NULL};
        ConcurrentListCell *next{NULL};

        ConcurrentListCell(const T &data) : data(data) {}
        ConcurrentListCell(T &&data) : data(std::move(data)) {}
        template <typename... Args>
        ConcurrentListCell(Args &&...args) : data(std::forward<Args>(args)...) {}

        void lock() { slock.lock(); }
        bool try_lock() { return slock.try_lock(); }
        void unlock() { slock.unlock(); }
    };

    using cell_type = ConcurrentListCell;
    using iterator = ListIterator<T, ConcurrentList, cell_type>;
    using const_iterator = ListConstIterator<T, cell_type>;

    explicit ConcurrentList(MemoryContext ctx = NULL) : _ctx(ctx)
    {
        if (!_ctx) {
            _ctx = CurrentMemoryContext;
        }
    }
    ConcurrentList(const ConcurrentList &other) = delete;
    ConcurrentList operator=(const ConcurrentList &other) = delete;
    ConcurrentList(ConcurrentList &&other) : _ctx(other._ctx), _head(other._head), _tail(other._tail)
    {
        other._ctx = other._head = other._tail = NULL;
    }
    ConcurrentList operator=(ConcurrentList &&other)
    {
        if (this != &other) {
            clear();
            swap(other);
        }
        return *this;
    }
    void swap(ConcurrentList &other)
    {
        std::swap(_head, other._head);
        std::swap(_tail, other._tail);
        std::swap(_ctx, other._ctx);
    }
    ~ConcurrentList() {}

    Optional<T> pop_back()
    {
        std::lock_guard<lock_type> guard(_list_tail_lock);
        if (!_tail) {
            return {};
        }
        auto tmp = _tail;
        tmp.lock();
        if (!tmp->prev) {
            std::lock_guard<lock_type> guard3(_list_head_lock);
            auto res = make_optional(std::move(tmp->data));
            _head = _tail = NULL;
            tmp.unlock();
            free_cell(tmp);
            return res;
        }
        std::lock_guard<lock_type> guard3(tmp->prev->slock);
        tmp->prev->next = NULL;
        auto res = make_optional(std::move(tmp->data));
        _tail = tmp->prev;
        tmp.unlock();
        free_cell(tmp);
        return res;
    }
    Optional<T> pop_front()
    {
        for (;;) {
            std::lock_guard<lock_type> guard(_list_head_lock);
            if (!_head) {
                return {};
            }
            auto tmp = _head;
            tmp->lock();
            if (tmp->next) {
                if (!tmp->next->try_lock()) {
                    tmp->unlock();
                    continue;
                }
            } else if (!_list_tail_lock.try_lock()) {
                tmp->unlock();
                continue;
            }

            if (tmp->next) {
                tmp->next->prev = NULL;
                auto res = make_optional(std::move(tmp->data));
                _head = tmp->next;
                tmp->unlock();
                delete tmp;
                _head->unlock();
                return res;
            }
            auto res = make_optional(std::move(_head->data));
            _head = _tail = NULL;
            _list_tail_lock.unlock();
            tmp->unlock();
            delete tmp;
            return res;
        }
    }
    cell_type *push_back(const T &data)
    {
        cell_type *cell = alloc_new_cell(data);
        push_back(cell);
        return cell;
    }
    cell_type *push_back(T &&data)
    {
        cell_type *cell = alloc_new_cell(std::move(data));
        push_back(cell);
        return cell;
    }
    template <typename... Args>
    cell_type *emplace_back(Args &&... args)
    {
        cell_type *cell = alloc_new_cell(std::forward<Args>(args)...);
        push_back(cell);
        return cell;
    }
    cell_type *push_front(const T &data)
    {
        cell_type *cell = alloc_new_cell(data);
        push_front(cell);
        return cell;
    }
    cell_type *push_front(T &&data)
    {
        cell_type *cell = alloc_new_cell(std::move(data));
        push_front(cell);
        return cell;
    }
    template <typename... Args>
    cell_type *emplace_front(Args &&... args)
    {
        cell_type *cell = alloc_new_cell(std::forward<Args>(args)...);
        push_front(cell);
        return cell;
    }
    void move_back(iterator iter) { move_back(iter.cur); }

    void erase(iterator &iter) { erase_delete(iter.cur); }
    /* thread-unsafe, we don't expect read/write to occur under destroy() */
    void destroy()
    {
        cell_type *cur = _head;
        while (cur) {
            cell_type *tmp = cur;
            cur = cur->next;
            free_cell(tmp);
        }
        _head = _tail = NULL;
        _ctx = NULL;
    }
    void clear() { destroy(); }

    iterator begin() { return iterator(_head); }
    iterator end() { return iterator(nullptr); }
    const_iterator cbegin() const { return const_iterator(_head); }
    const_iterator cend() const { return const_iterator(nullptr); }
    Optional<T> front()
    {
        std::lock_guard<lock_type> guard(_list_head_lock);
        if (!_head) {
            return {};
        }
        return make_optional(_head->data);
    }
    Optional<T> back()
    {
        std::lock_guard<lock_type> guard(_list_tail_lock);
        if (!_tail) {
            return {};
        }
        return make_optional(_tail->data);
    }

    bool empty() const { return !_head; }
    size_t thread_unsafe_size() const
    {
        size_t res = 0;
        cell_type *cur = _head;
        while (cur) {
            ++res;
            cur = cur->next;
        }
        return res;
    }
    size_t size()
    {
        size_t res = 0;
        std::lock_guard<lock_type> guard(_list_tail_lock);
        std::lock_guard<lock_type> guard2(_list_head_lock);
        cell_type *cur = _tail;
        if (!cur) {
            return 0;
        }
        do {
            std::lock_guard<lock_type> guard3(cur->slock);
            ++res;
            cur = cur->prev;
        } while (cur); /* bug here, don't call it anyway */
        return res;
    }
private:
    MemoryContext _ctx{NULL};
    cell_type *_head{NULL};
    cell_type *_tail{NULL};
    lock_type _list_head_lock{};
    lock_type _list_tail_lock{};

    cell_type *alloc_new_cell(const T &data)
    {
        cell_type *cell;
        if (std::is_standard_layout<T>::value) {
            cell = New(_ctx) cell_type(data);
        } else {
            auto ctx = MemoryContextSwitchTo(_ctx);
            cell = New(_ctx) cell_type(data);
            MemoryContextSwitchTo(ctx);
        }
        return cell;
    }
    cell_type *alloc_new_cell(T &&data)
    {
        /* no mem alloc should happen during move */
        return New(_ctx) cell_type(data);
    }
    template <typename... Args>
    cell_type *alloc_new_cell(Args &&...args)
    {
        cell_type *cell;
        if (std::is_standard_layout<T>::value) {
            cell = New(_ctx) cell_type(std::forward<Args>(args)...);
        } else {
            auto ctx = MemoryContextSwitchTo(_ctx);
            cell = New(_ctx) cell_type(std::forward<Args>(args)...);
            MemoryContextSwitchTo(ctx);
        }
        return cell;
    }
    void free_cell(cell_type *cell)
    {
        ann_helper::optional_destroy(cell->data);
        ann_helper::optional_destroy(cell);
        delete cell;
    }

    void push_back(cell_type *cell)
    {
        std::lock_guard<lock_type> guard(_list_tail_lock);
        if (!_tail) {
            std::lock_guard<lock_type> guard2(_list_head_lock);
            _head = _tail = cell;
            return;
        }
        std::lock_guard<lock_type> guard2(_tail->slock);
        _tail->next = cell;
        cell->prev = _tail;
        _tail = cell;
    }
    void push_front(cell_type *cell)
    {
        for (;;) {
            std::lock_guard<lock_type> guard(_list_head_lock);
            if (!_head) {
                if (!_list_tail_lock.try_lock()) {
                    continue;
                }
                _head = _tail = cell;
                _list_tail_lock.unlock();
                return;
            }
            std::lock_guard<lock_type> guard2(_head->slock);
            cell->next = _head;
            _head->prev = cell;
            _head = cell;
            return;
        }
    }
    void erase(cell_type *cell)
    {
        for (;;) {
            std::lock_guard<lock_type> guard(cell->slock);
            if (!cell->next) {
                if (!_list_tail_lock.try_lock()) {
                    continue;
                }
                if (cell->prev) {
                    std::lock_guard<lock_type> guard3(cell->prev->slock);
                    cell->prev->next = NULL;
                    _tail = cell->prev;
                } else {
                    std::lock_guard<lock_type> guard3(_list_head_lock);
                    _head = _tail = NULL;
                }
                _list_tail_lock.unlock();
                return;
            }
            if (!cell->prev) {
                if (!_list_head_lock.try_lock()) {
                    continue;
                }
                if (!cell->next->try_lock()) {
                    _list_head_lock.unlock();
                    continue;
                }
                cell->next->prev = NULL;
                _head = cell->next;
                cell->next->unlock();
                _list_head_lock.unlock();
                return;
            }
            if (!cell->next->try_lock()) {
                continue;
            }
            std::lock_guard<lock_type> guard2(cell->prev->slock);
            cell->prev->next = cell->next;
            cell->next->prev = cell->prev;
            cell->next->unlock();
            return;
        }
    }
    void erase_delete(cell_type *cell)
    {
        erase(cell);
        free_cell(cell);
    }
    void move_back(cell_type *cell)
    {
        if (cell == _tail) { /* it's likely for new accessed cell to be re-accessed */
            return;
        }
        erase(cell);
        cell->next = NULL;
        push_back(cell);
    }
};
} /* vtl */

#endif /* CONTAINER_LIST_H */
