#ifndef TIMERING_H
#define TIMERING_H

#include "utils/timestamp.h"

class TimeRing {
    size_t slot_num;
    TimestampTz unit;
    size_t cur_idx;
    TimestampTz base_timestamp;
    size_t *record;
    slock_t lock;
public:    
    TimeRing(size_t slots = 10, TimestampTz unit = 60 * USECS_PER_SEC) :
        slot_num(slots), unit(unit), cur_idx(0), base_timestamp(GetCurrentTimestamp()) {
        record = (size_t *)palloc0(sizeof(size_t) * slot_num);
        SpinLockInit(&lock);
    }
    
    void destroy() {
        if (record) pfree(record);
        record = nullptr;
    }
    
    void visit() {
        SpinLockAcquire(&lock);
        TimestampTz now = GetCurrentTimestamp();
        if (now < base_timestamp) {
            reset();
            SpinLockRelease(&lock);
            return;
        }
        size_t now_idx = (now - base_timestamp) / unit;
        if (now_idx > cur_idx) {
            size_t clear_slots = now_idx - cur_idx;
            if (clear_slots >= slot_num) {
                clear();
            } else {
                for (size_t i = 1; i <= clear_slots; ++i) {
                    size_t idx = (cur_idx + i) % slot_num;
                    record[idx] = 0;
                }
            }
            cur_idx = now_idx;
        }
        size_t current_slot = cur_idx % slot_num;
        ++record[current_slot];
        SpinLockRelease(&lock);
    }
    
    size_t get_lastn(size_t n) {
        SpinLockAcquire(&lock);
        TimestampTz now = GetCurrentTimestamp();
        if (now < base_timestamp) {
            reset();
            SpinLockRelease(&lock);
            return 0;
        }
        size_t count = 0;
        size_t now_idx = (now - base_timestamp) / unit;
        if (now_idx > cur_idx) {
            size_t skip_slots = now_idx - cur_idx;
            if (skip_slots > slot_num) {
                clear();
            } else {
                for (size_t i = 1; i <= skip_slots; ++i) {
                    size_t idx = (cur_idx + i) % slot_num;
                    record[idx] = 0;
                }
            }
            cur_idx = now_idx;
        }
        size_t i = n > cur_idx ? 0 : cur_idx - n + 1;
        for (; i <= cur_idx; ++i) {
            count += record[i % slot_num];
        }
        
        SpinLockRelease(&lock);
        return count;
    }

    size_t get_all() { return get_lastn(slot_num); }
    
private:
    void reset() {
        for (size_t i = 0; i < slot_num; ++i) {
            record[i] = 0;
        }
        cur_idx = 0;
        base_timestamp = GetCurrentTimestamp();
    }

    void clear() {
        for (size_t i = 0; i < slot_num; ++i) {
            record[i] = 0;
        }
    }
};

#endif /* TIMERING_H */
