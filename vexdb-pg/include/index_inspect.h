/**
 * Copyright (c) 2021-2024 Huawei Technologies Co., Ltd.
 * Index inspection result structure.
 * Adapted for PostgreSQL from openGauss src/include/access/bm25/index_inspect.h
 */

#ifndef PG_VEXDB_INDEX_INSPECT_H
#define PG_VEXDB_INDEX_INSPECT_H

#include <cstdarg>
#include <cmath>
#include <utility>

#include "pg_compat.h"
#include "module/size_format.h"

struct IndexInspectResult {
    size_t nattr{0};
    size_t capacity{0};
    Datum *attributes{nullptr};
    Datum *contents{nullptr};
    
    ~IndexInspectResult() { destroy(); }
    
    void destroy()
    {
        for (size_t i = 0; i < nattr; ++i) {
            void *t = DatumGetPointer(attributes[i]);
            if (t) pfree(t);
            t = DatumGetPointer(contents[i]);
            if (t) pfree(t);
        }
        if (attributes) pfree(attributes);
        if (contents) pfree(contents);
        attributes = nullptr;
        contents = nullptr;
        nattr = 0;
        capacity = 0;
    }
    
    void append(IndexInspectResult &&other)
    {
        size_t new_capacity = capacity;
        while (new_capacity < nattr + other.nattr) {
            new_capacity = new_capacity == 0 ? 8 : new_capacity * 2;
        }
        if (new_capacity > capacity) {
            capacity = new_capacity;
            attributes = (Datum *)repalloc(attributes, sizeof(Datum) * capacity);
            contents = (Datum *)repalloc(contents, sizeof(Datum) * capacity);
        }
        for (size_t i = 0; i < other.nattr; ++i) {
            attributes[nattr + i] = other.attributes[i];
            contents[nattr + i] = other.contents[i];
        }
        nattr += other.nattr;
        other.nattr = 0;
        other.destroy();
    }

    void append_attr(const char *msg)
    {
        expand();
        attributes[nattr] = PointerGetDatum(cstring_to_text(msg));
    }
    
    template <typename... Args>
    void append_attr(const char *msg, Args &&...args)
    {
        expand();
        attributes[nattr] = get_data(msg, std::forward<Args>(args)...);
    }

    void fill_content(const char *msg)
    {
        contents[nattr] = PointerGetDatum(cstring_to_text(msg));
        ++nattr;
    }
    
    template <typename... Args>
    void fill_content(const char *msg, Args &&...args)
    {
        contents[nattr] = get_data(msg, std::forward<Args>(args)...);
        ++nattr;
    }
    
    void fill_content(size_t bytes)
    {
        auto sf = ann_helper::format_size(bytes);
        if (std::trunc(sf.n) == sf.n) {
            contents[nattr] = get_data("%lu %s", (size_t)sf.n, sf.unit_str());
        } else {
            contents[nattr] = get_data("%.3f %s", sf.n, sf.unit_str());
        }
        ++nattr;
    }
    
private:
    void expand()
    {
        if (capacity <= nattr) {
            const size_t new_capacity = capacity == 0 ? 8 : nattr * 2;
            if (capacity == 0) {
                attributes = (Datum *)palloc(sizeof(Datum) * new_capacity);
                contents = (Datum *)palloc(sizeof(Datum) * new_capacity);
            } else {
                attributes = (Datum *)repalloc(attributes, sizeof(Datum) * new_capacity);
                contents = (Datum *)repalloc(contents, sizeof(Datum) * new_capacity);
            }
            capacity = new_capacity;
        }
    }

    template <typename... Args>
    Datum get_data(const char *msg, Args &&...args)
    {
        constexpr int init_size = 128;
        int size = init_size;
        char *msg_buf = (char *)palloc(size);
        int n;
        do {
            n = snprintf(msg_buf, size, msg, std::forward<Args>(args)...);
            if (n < 0) {
                pfree(msg_buf);
                ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                                errmsg("Failed to parse format text")));
            }
            if (n >= size) {
                size *= 2;
                msg_buf = (char *)repalloc(msg_buf, size);
            }
        } while (n >= size);
        text *t = cstring_to_text_with_len(msg_buf, n);
        pfree(msg_buf);
        return PointerGetDatum(t);
    }
};

extern "C" {
Datum index_inspect(PG_FUNCTION_ARGS);
}

#endif /* PG_VEXDB_INDEX_INSPECT_H */
