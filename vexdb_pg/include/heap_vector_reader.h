#ifndef VEXDB_HEAP_VECTOR_READER_H
#define VEXDB_HEAP_VECTOR_READER_H

#include "pg_compat.h"

// The PG executor evaluates arbitrary index expressions, including casts and
// nested functions. Plain-column refinement keeps its direct attribute path.
struct HeapIndexVectorReader {
    bool Copy(Relation index, Relation heap, HeapTuple tuple,
              uint16 dimensions, size_t bytes, char *dest);
    void Destroy();
private:
    EState *estate_ = nullptr;
    IndexInfo *index_info_ = nullptr;
    TupleTableSlot *slot_ = nullptr;
};

#endif
