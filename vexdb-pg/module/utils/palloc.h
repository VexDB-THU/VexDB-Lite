/**
 * Copyright ...
 * Base object class for memory management
 */

#ifndef PALLOC_H
#define PALLOC_H

#include "c.h"

class BaseObject {
public:
    virtual ~BaseObject() {}
    void* operator new(size_t size, MemoryContextData* pmc, const char* file, int line);
    void* operator new(size_t size, void *res);
    void* operator new[](size_t size, MemoryContextData* pmc, const char* file, int line);
    void operator delete(void* p);
    void operator delete[](void* p);
};

#endif /* PALLOC_H */