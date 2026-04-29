/**
 * Copyright ...
 */

#ifndef QUANTIZER_H
#define QUANTIZER_H

#include "pg_compat.h"
#include <vtl/hashtable>
#include "timering.h"

enum class QuantizerType : uint8 {
	NONE = 0,
	PQ,
	RABITQ
	/* others... */
};
inline const char *quantizer_name(QuantizerType qt)
{
    if (qt == QuantizerType::PQ) {
        return "PQ";
    }
    if (qt == QuantizerType::RABITQ) {
        return "RaBitQ";
    }
    return "Plain Vector";
}

extern void validate_quantizer(const char *value);
extern QuantizerType extract_qt(const char *value);

/* === Used by GRAPH_INDEX and HNSW === */
struct PQMetaInfo {
    bool graph_pq; /* false if data is not enough to calculate pq */
    uint16 m;
    uint16 k;
    void init(uint32 dim);
    uint32 code_size() const { return m * (k <= 256 ? 1 : 2); }
    uint16 nbits() const { Assert(k > 0); Assert(31 - __builtin_clz(k) == 8); return 31 - __builtin_clz(k); }
};

struct RaBitQMeta {
    bool enabled; /* RaBitQ is enabled for current index */
    bool keep_vecs; /* unused now */
    int quant_size; /* cid_size + bin_size + ext_size */
    double query_rescaling_factor; /* pre-computed factor for query only */
    void init(uint32 dim);
};

/* quantizer meta info, write to disk */
struct QuantizerMetaInfo {
    union {
        PQMetaInfo pq_metainfo;
        RaBitQMeta rbq_meta;
    } metainfo;
    QuantizerType quantizer_type;
    uint32 num_new_data;
    uint8 centroids_version;
    uint8 code_version;

    void init(QuantizerType qt_type, uint32 dimension);
    QuantizerType get_type() const {
        int retry = 0;
        while (centroids_version != code_version) {
            ereport(WARNING,
                (errmsg("Index version does not match, it may be updating. "
                        "Wait for five seconds, up to a maximum of twenty seconds.")));
            ++retry;
            pg_usleep(5000000);
            if (retry == 4) {
                ereport(ERROR, (errmsg("index may be broken, need check and rebuild")));
            } 
        }
        if (quantizer_type == QuantizerType::PQ) {
            return get_pq_metainfo().graph_pq ? QuantizerType::PQ : QuantizerType::NONE;
        } else if (quantizer_type == QuantizerType::RABITQ) {
            return get_rabitq_meta().enabled ? QuantizerType::RABITQ : QuantizerType::NONE;
        } else {
            return QuantizerType::NONE;
        }
    }
    QuantizerType get_setting_type() const { return quantizer_type; }
    bool has_quant() const { return get_type() != QuantizerType::NONE; }
    void set_type(QuantizerType qt_type) { quantizer_type = qt_type; }
    const PQMetaInfo &get_pq_metainfo() const { return metainfo.pq_metainfo; }
    PQMetaInfo &get_pq_metainfo() { return metainfo.pq_metainfo; }
    const RaBitQMeta &get_rabitq_meta() const { return metainfo.rbq_meta; }
    RaBitQMeta &get_rabitq_meta() { return metainfo.rbq_meta; }
    void set_enable()
    {
        if (quantizer_type == QuantizerType::PQ) {
            metainfo.pq_metainfo.graph_pq = true;
        } else if (quantizer_type == QuantizerType::RABITQ) {
            metainfo.rbq_meta.enabled = true;
        }
    }
};

/* === Quantizer Updating === */

#define OUTPUT_QTUPDATE_LOG true
#if OUTPUT_QTUPDATE_LOG
#define QT_UPDATE_LOG(fmt, ...) ereport(LOG, (errcode(ERRCODE_LOG), errmsg(fmt, ##__VA_ARGS__)))
#else
#define QT_UPDATE_LOG(fmt, ...)
#endif /* OUTPUT_TASKPOOL_LOG */

struct QtUpdateMgr {
    UnorderedSet<Oid> qt_updating_set;
    slock_t qt_updating_setlock;
    UnorderedMap<Oid, TimeRing *> qt_timering_map;
    slock_t qt_timering_maplock;

    bool insert_updating(Oid oid);
    void erase_updating(Oid oid);
    bool contain_updating(Oid oid);
    TimeRing *insert_timgring(Oid oid);
    TimeRing *find_timering(Oid oid);
    void erase_timering(Oid oid);
};

/* quantizer update */
extern void add_quantizer_update_task(Relation index, void *params);
extern void qt_update_init(void *annvec_cxt);
Datum index_qtupdate(PG_FUNCTION_ARGS);

#endif /* QUANTIZER_H */
