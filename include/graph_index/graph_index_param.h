/**
 * Copyright (c) 2024, openGauss Contributors
 * Copyright (c) 2024, pg_vexdb Contributors
 * 
 * Graph index parameters - merged from include/graph_index_param.h
 * Values from openGauss src/include/access/graph_index/graph_index_param.h
 */

#ifndef GRAPH_INDEX_PARAM_H
#define GRAPH_INDEX_PARAM_H

#include "pg_compat.h"
#include <cfloat>

/* Graph index version and magic */
#define GRAPH_INDEX_VERSION 2
#define GRAPH_INDEX_MAGIC_NUMBER 0xA953A953

/* Graph index block numbers */
#define GRAPH_INDEX_METAPAGE_BLKNO 0
#define GRAPH_INDEX_PS_BLKNO 1

/* Graph index limits */
#define GRAPH_INDEX_DEFAULT_M 16
#define GRAPH_INDEX_MIN_M 2
#define GRAPH_INDEX_MAX_M 100
#define GRAPH_INDEX_DEFAULT_EF_CONSTRUCTION 64
#define GRAPH_INDEX_MIN_EF_CONSTRUCTION 4
#define GRAPH_INDEX_MAX_EF_CONSTRUCTION 1000
#define GRAPH_INDEX_DEFAULT_EF_SEARCH 40
#define GRAPH_INDEX_MIN_EF_SEARCH 1
#define GRAPH_INDEX_MAX_EF_SEARCH 1000
#define GRAPH_INDEX_DEFAULT_QUANTIZER_TYPE 0
#define GRAPH_INDEX_MAX_LEVEL 16
#define GRAPH_INDEX_MAX_HEAPTIDS 10
static_assert(GRAPH_INDEX_MAX_HEAPTIDS <= 15 && GRAPH_INDEX_MAX_HEAPTIDS > 0, "wrong GRAPH_INDEX_MAX_HEAPTIDS");

/* Quantizer parameters */
#define GRAPH_INDEX_MIN_QT_SAMPLES_SIZE 10000
#define GRAPH_INDEX_RABITQ_NUM_CLUSTERS 16

/* Page IDs */
#define GRAPH_INDEX_PAGE_ID 0xFF90

/* Page macros */
#define GRAPH_INDEX_PAGE_GET_OPAQUE(page) ((GraphIndexPageOpaque)PageGetSpecialPointer(page))
#define GRAPH_INDEX_PAGE_GET_META(page) ((GraphIndexMetaPage)PageGetContents(page))

/* Special values */
#define INVALID_VECTOR_ID SIZE_MAX
#define INVALID_DIST FLT_MAX

/* Procs */
#define GRAPH_INDEX_DISTANCE_PROC 1
#define GRAPH_INDEX_NORM_PROC 2

#endif /* GRAPH_INDEX_PARAM_H */
