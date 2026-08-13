#ifndef GRAPH_INDEX_DEPEND_H
#define GRAPH_INDEX_DEPEND_H

#if defined(PG_VEXDB_TARGET_DUCK)
#include "vex_graph_index_depend_duck.hpp"
#elif defined(PG_VEXDB_TARGET_SQLITE)
// SQLite 宿主依赖（vexdb_sqlite/include/store/，由 vexdb_sqlite CMake 提供 include path）
#include "vex_graph_index_depend_sqlite.hpp"
#elif defined(PG_VEXDB_TARGET_PG)
#include "postgres.h"
#include "global_instance.h"
#include "graph_index/graph_index_cluster.h"
#include "graph_index/graph_index_storage.h"
#include "vector_page_storage.h"
#else
#error "One of PG_VEXDB_TARGET_DUCK or PG_VEXDB_TARGET_PG must be defined"
#endif

#endif
