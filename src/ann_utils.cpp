#include "ann_utils.h"
#include "floatvector.h"
#include "halfvec.h"
#include "pg_compat.h"

extern "C" {
#include "catalog/namespace.h"
}

Oid get_floatvector_oid(void)
{
    static Oid cached_oid = InvalidOid;
    if (cached_oid == InvalidOid) {
        cached_oid = TypenameGetTypid("floatvector");
    }
    return cached_oid;
}

Oid get_halfvector_oid(void)
{
    static Oid cached_oid = InvalidOid;
    if (cached_oid == InvalidOid) {
        cached_oid = TypenameGetTypid("halfvector");
    }
    return cached_oid;
}

Oid get_int8vector_oid(void)
{
    static Oid cached_oid = InvalidOid;
    if (cached_oid == InvalidOid) {
        cached_oid = TypenameGetTypid("int8vector");
    }
    return cached_oid;
}

size_t get_relstats_reltuples(Relation rel)
{
    size_t reltuples_stats = 0;
    if (rel == NULL) {
        return reltuples_stats;
    }

    Oid relid = RelationGetRelid(rel);
    HeapTuple tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
    if (HeapTupleIsValid(tuple)) {
        Form_pg_class pg_class_form = (Form_pg_class)GETSTRUCT(tuple);
        reltuples_stats = (size_t)pg_class_form->reltuples;
        ReleaseSysCache(tuple);
    }

    return reltuples_stats;
}

void populate_index_partition_name(Relation index, char *indexName, char *partIndexName)
{
    sprintf(indexName, "%s", RelationGetRelationName(index));
    partIndexName[0] = '\0';
}

Buffer AnnNewBuffer(Relation index, ForkNumber forkNum)
{
    Buffer buf = ReadBufferExtended(index, forkNum, P_NEW, RBM_NORMAL, NULL);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    return buf;
}

void AnnCommitBuffer(Buffer buf)
{
    MarkBufferDirty(buf);
    UnlockReleaseBuffer(buf);
}

void check_ann_attributes(Relation index)
{
    Assert(RelationGetDescr(index)->natts >= 1);
    Oid floatvector_oid = get_floatvector_oid();
    Oid halfvector_oid = get_halfvector_oid();
    if (TupleDescAttr(RelationGetDescr(index), 0)->atttypid != floatvector_oid &&
        TupleDescAttr(RelationGetDescr(index), 0)->atttypid != halfvector_oid) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("The first attribute of index must be floatvector or halfvector.")));
    }
    for (int i = 1; i < RelationGetDescr(index)->natts; ++i) {
        if (TupleDescAttr(RelationGetDescr(index), i)->atttypid == floatvector_oid ||
            TupleDescAttr(RelationGetDescr(index), i)->atttypid == halfvector_oid) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("Index can only store one vector attribute.")));
        }
    }
}

Buffer AnnLoadBuffer(Relation index, BlockNumber blkNo)
{
    Buffer buf = ReadBuffer(index, blkNo);
    LockBuffer(buf, BUFFER_LOCK_SHARE);
    return buf;
}

Buffer AnnLoadBufferExtended(Relation index, ForkNumber forkNum, BlockNumber blkNo)
{
    Buffer buf = ReadBufferExtended(index, forkNum, blkNo, RBM_NORMAL, NULL);
    LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
    return buf;
}

bool isHybridIndex(Relation index) { return RelationGetDescr(index)->natts > 1; }

bool AnnNormValue(ann_helper::distance_func procinfo, Datum *value, FloatVector *result)
{
    FloatVector *v = DatumGetFloatVector(*value);
    double norm = procinfo(v->x, v->x, v->dim);
    if (norm <= 0) {
        return false;
    }

    if (result == NULL) {
        result = InitFloatVector(v->dim);
    }

    for (int i = 0; i < v->dim; ++i) {
        result->x[i] = v->x[i] / norm;
    }

    if ((Pointer)v != DatumGetPointer(*value)) {
        pfree(v);
    }

    *value = PointerGetDatum(result);
    return true;
}

char *DatumGetVector(Datum value, DistPrecisionType type, Pointer *vec_out)
{
    char *vector = NULL;
    if (type == DistPrecisionType::FLOAT) {
        FloatVector *tempvec = DatumGetFloatVector(value);
        *vec_out = (Pointer)tempvec;
        vector = (char *)tempvec->x;
    } else if (type == DistPrecisionType::HALF){
        HalfVector *tempvec = DatumGetHalfVector(value);
        *vec_out = (Pointer)tempvec;
        vector = (char *)tempvec->x;
    } else {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("Int8Vector not yet supported")));
    }
    return vector;
}
