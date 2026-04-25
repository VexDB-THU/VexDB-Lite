/**
 * Copyright ...
 * Free space management for offsets.
 */

#ifndef DISKANN_FREESPACE_H
#define DISKANN_FREESPACE_H

#include <vtl/disk_container/diskvector.hpp>

namespace disk_container {
template <typename T>
class FreeSpace {
public:
    FreeSpace(Relation rel, BlockNumber fs_meta_blkno, bool is_wal = true)
        : _freelist(rel, fs_meta_blkno, is_wal) {}
    void destroy() { _freelist.destroy(); }
    static BlockNumber get_freespace_meta(Relation rel, bool is_wal, ForkNumber fork_num = MAIN_FORKNUM)
        { return DiskVector<T>::get_disk_vector(rel, is_wal, fork_num); }
    void insert(T *data, size_t n)
    {
        if (n == 0) {
            return;
        }
        _freelist.wlock();
        _freelist.push_back_n(data, n);
        _freelist.wunlock();
    }
    void insert(const T &data)
    {
        _freelist.wlock();
        _freelist.push_back(data);
        _freelist.wunlock();
    }
    bool pop(T &i)
    {
        _freelist.rlock();
        bool res = _freelist.template pop_back<AccessorLockType::NoLockRead>(i);
        _freelist.runlock();
        return res;
    }

    size_t size() const { return _freelist.size(); }
    size_t capacity() const { return _freelist.capacity(); }
    size_t get_nblocks() const { return _freelist.get_nblocks(); }
private:
    DiskVector<T> _freelist;
};
} /* namespace disk_container */
#endif /* DISKANN_FREESPACE_H */
