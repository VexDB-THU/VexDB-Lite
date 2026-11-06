/*
 * guc_config.h - GUC parameter definitions for vexdb_lite
 */

#ifndef PG_VEXDB_GUC_CONFIG_H
#define PG_VEXDB_GUC_CONFIG_H

#include "postgres.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize GUC parameters - call from _PG_init */
extern void vexdb_lite_init_guc(void);

/* Accessor functions */
extern int vexdb_lite_get_ef_search(void);
extern bool vexdb_lite_get_signature_rescue(void);
extern bool vexdb_lite_get_enable_vec_buffer_manager(void);
extern int vexdb_lite_get_vector_buffers(void);
extern int vexdb_lite_get_vector_buffer_workers(void);

#ifdef __cplusplus
}
#endif

#endif /* PG_VEXDB_GUC_CONFIG_H */
