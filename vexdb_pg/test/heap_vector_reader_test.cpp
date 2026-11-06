// Test-only PG module: independently check expression values against a slice
// of the stored floatvector. Run only in the disposable regression database.
#include "pg_compat.h"
#include "floatvector.h"
#include "heap_vector_reader.h"
#include "vector_buffer/vector_buffer_manager.h"
#include "vector_buffer/vector_smgr.h"

extern "C" {
PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(test_heap_index_vector_reader);
Datum test_heap_index_vector_reader(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(test_vector_cache_pending_invalidation);
Datum test_vector_cache_pending_invalidation(PG_FUNCTION_ARGS);
}

Datum test_heap_index_vector_reader(PG_FUNCTION_ARGS)
{
    VecBufferLoc missing;
    if (!missing.empty() || missing.valid())
        elog(ERROR, "default cache location must be absent");
    missing.set_empty();
    missing.set_invalid();
    if (!missing.empty() || missing.valid())
        elog(ERROR, "invalidating an absent cache slot lost its empty state");
    VecBufferLoc first_slot(0, 0);
    if (first_slot.empty() || !first_slot.valid())
        elog(ERROR, "physical cache slot 0:0 must be valid");
    first_slot.set_invalid();
    if (first_slot.empty() || first_slot.valid())
        elog(ERROR, "an allocated invalid slot must retain its allocation");
    first_slot.set_valid();
    if (first_slot.empty() || !first_slot.valid())
        elog(ERROR, "allocated cache slot could not be restored");
    const Oid oid = PG_GETARG_OID(0);
    const int attribute = PG_GETARG_INT32(1);
    const int first = PG_GETARG_INT32(2);
    const int count = PG_GETARG_INT32(3);
    if (first < 0 || count < 1 || count > FLOATVECTOR_MAX_DIM)
        elog(ERROR, "invalid vector reader test range");
    Relation index = index_open(oid, AccessShareLock);
    Relation heap = table_open(index->rd_index->indrelid, AccessShareLock);
    if (attribute < 1 || attribute > RelationGetDescr(heap)->natts)
        elog(ERROR, "invalid vector reader test attribute");
#if PG_VERSION_NUM >= 190000
    TableScanDesc scan = table_beginscan(heap, GetActiveSnapshot(), 0, nullptr, 0);
#else
    TableScanDesc scan = table_beginscan(heap, GetActiveSnapshot(), 0, nullptr);
#endif
    TupleTableSlot *slot = table_slot_create(heap, nullptr);
    HeapIndexVectorReader reader;
    char *buffer = (char *)palloc(size_t(count) * sizeof(float));
    int64 checked = 0;
    while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
        CHECK_FOR_INTERRUPTS();
        bool free_tuple = false;
        HeapTuple tuple = ExecFetchSlotHeapTuple(slot, false, &free_tuple);
        bool is_null = false;
        Datum value = heap_getattr(tuple, attribute, RelationGetDescr(heap), &is_null);
        const bool copied = reader.Copy(index, heap, tuple, uint16(count),
                                         size_t(count) * sizeof(float), buffer);
        if (copied == is_null) elog(ERROR, "vector reader NULL result mismatch");
        if (copied) {
            FloatVector *original = DatumGetFloatVector(value);
            if (first > original->dim - count ||
                memcmp(buffer, original->x + first, size_t(count) * sizeof(float)) != 0)
                elog(ERROR, "vector reader differs from independently selected coordinates");
            if ((Pointer)original != DatumGetPointer(value)) pfree(original);
            ++checked;
        }
        if (free_tuple) heap_freetuple(tuple);
    }
    reader.Destroy();
    pfree(buffer);
    ExecDropSingleTupleTableSlot(slot);
    table_endscan(scan);
    table_close(heap, AccessShareLock);
    index_close(index, AccessShareLock);
    PG_RETURN_INT64(checked);
}

// Hold a real cache pin while invalidating its key. A second read must decline
// the optional cache instead of allocating blocks for an unusable map entry.
Datum test_vector_cache_pending_invalidation(PG_FUNCTION_ARGS)
{
    Relation index = index_open(PG_GETARG_OID(0), AccessShareLock);
    const int dimensions = PG_GETARG_INT32(1);
    if (dimensions < 8 || dimensions > FLOATVECTOR_MAX_DIM)
        elog(ERROR, "invalid cache regression dimensions");
    RelationGetSmgr(index);
    const size_t bytes = size_t(dimensions) * sizeof(float);
    bool success = false;
    VecBuffer held = VecBufMgr->get_buffer(index, 0, bytes, VecStorageType::PureVec, success);
    if (!success) elog(ERROR, "could not create initial test cache pin");
    char *expected = (char *)palloc(bytes);
    memcpy(expected, held.get_vecbuf(), bytes);
    const uint32 blocks = VecBufMgr->nalloced;
    vec_invalidate_buffer_cache(index->rd_smgr->smgr_rlocator.locator.relNumber, 0, bytes);
    VecBuffer declined = VecBufMgr->get_buffer(index, 0, bytes, VecStorageType::PureVec, success);
    if (success || declined.get_vecbuf() || VecBufMgr->nalloced != blocks)
        elog(ERROR, "pending cache invalidation did not use bounded fallback");
    held.release();
    vec_invalidate_buffer_cache(index->rd_smgr->smgr_rlocator.locator.relNumber, 0, bytes);
    VecBuffer restored = VecBufMgr->get_buffer(index, 0, bytes, VecStorageType::PureVec, success);
    if (!success || memcmp(expected, restored.get_vecbuf(), bytes) != 0)
        elog(ERROR, "cache vector was not restored after releasing the pin");
    restored.release();
    pfree(expected);
    index_close(index, AccessShareLock);
    PG_RETURN_BOOL(true);
}
