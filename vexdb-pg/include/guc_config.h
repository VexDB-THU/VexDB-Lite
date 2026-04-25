/*
 * guc_config.h - GUC parameter definitions for pg_vexdb
 */

#ifndef PG_VEXDB_GUC_CONFIG_H
#define PG_VEXDB_GUC_CONFIG_H

#include "postgres.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize GUC parameters - call from _PG_init */
extern void pg_vexdb_init_guc(void);

/* Accessor functions */
extern int pg_vexdb_get_ef_search(void);
extern bool pg_vexdb_get_enable_vec_buffer_manager(void);
extern int pg_vexdb_get_vector_buffers(void);
extern int pg_vexdb_get_vector_buffer_workers(void);

#ifdef __cplusplus
}
#endif

#endif /* PG_VEXDB_GUC_CONFIG_H */
