/**
 * Copyright (c) 2026 VexDB-THU
 */

#ifndef QUANTIZER_H
#define QUANTIZER_H

#include "postgres.h"
#include "quantizer_type.h"
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
    uint8 storage_version; /* code/fixed-data layout; unsupported versions require REINDEX */
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

    void init(QuantizerType qt_type, uint32 dimension, uint32 requested_pq_m = 0);
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
            if (get_rabitq_meta().enabled && get_rabitq_meta().storage_version != 2) {
                ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("unsupported RaBitQ index format version %u; REINDEX required",
                        (unsigned)get_rabitq_meta().storage_version)));
            }
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

#endif /* QUANTIZER_H */
