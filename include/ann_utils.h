#ifndef ANN_UTILS_H
#define ANN_UTILS_H

#include <cstddef>
#include "pg_compat.h"
#include "floatvector.h"
#include "distance/core/distance.h"

#define VEC_ELEM_SIZE(precision_type) \
    (precision_type == DistPrecisionType::FLOAT ? sizeof(float) : precision_type == DistPrecisionType::HALF ? sizeof(half) : sizeof(int8_t))

/* Type OID helpers - runtime lookup */
extern Oid get_floatvector_oid(void);
extern Oid get_halfvector_oid(void);
extern Oid get_int8vector_oid(void);

extern size_t get_relstats_reltuples(Relation rel);
extern void populate_index_partition_name(Relation index, char *indexName, char *partIndexName);
extern Buffer AnnNewBuffer(Relation index, ForkNumber forkNum);
extern void AnnCommitBuffer(Buffer buf);
extern void check_ann_attributes(Relation index);
extern Buffer AnnLoadBuffer(Relation index, BlockNumber blkNo);
extern Buffer AnnLoadBufferExtended(Relation index, ForkNumber forkNum, BlockNumber blkNo);
extern bool isHybridIndex(Relation index);
extern bool AnnNormValue(ann_helper::distance_func procinfo, Datum *value, FloatVector *result);
extern char *DatumGetVector(Datum value, DistPrecisionType type, Pointer *vec_out);
#endif /* ANN_UTILS_H */
