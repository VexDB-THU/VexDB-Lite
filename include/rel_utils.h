#ifndef REL_UTILS_H
#define REL_UTILS_H

#include "postgres.h"
#include "utils/rel.h"
#include "access/heapam.h"
#include "access/genam.h"

struct RelMgr {
    Oid heap_id;
    Oid index_id;

    RelMgr(Oid heap_oid, Oid index_oid)
        : heap_id(heap_oid), index_id(index_oid) {}
    
    RelMgr(Relation heap, Relation index)
        : heap_id(RelationGetRelid(heap)),
          index_id(RelationGetRelid(index)) {}
    
    RelMgr(Relation index)
        : heap_id(index->rd_index->indrelid),
          index_id(RelationGetRelid(index)) {}

    template <typename Func>
    void execute(Func &&func, LOCKMODE heap_lockmode, LOCKMODE index_lockmode)
    {
        Assert(OidIsValid(heap_id));
        Assert(OidIsValid(index_id));
        
        Relation heap = table_open(heap_id, heap_lockmode);
        Relation index = index_open(index_id, index_lockmode);
        
        func(heap, index);
        
        index_close(index, index_lockmode);
        table_close(heap, heap_lockmode);
    }

    template <typename Func>
    void execute(Func &&func, LOCKMODE index_lockmode)
    {
        Assert(OidIsValid(index_id));
        
        Relation index = index_open(index_id, index_lockmode);
        
        func(index);
        
        index_close(index, index_lockmode);
    }
};

#endif /* REL_UTILS_H */
