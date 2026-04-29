#ifndef KNL_VARIABLE_H
#define KNL_VARIABLE_H

#include "distance/core/distance_func.h"
#include "utils/memutils.h"

typedef struct knl_g_annvec_context {
    ann_helper::distance_func l2_squared_distance;
    ann_helper::distance_func negative_inner_product;
    ann_helper::distance_func cosine_distance;
    ann_helper::distance_func half_l2_squared_distance;
    ann_helper::distance_func half_negative_inner_product;
    ann_helper::distance_func half_cosine_distance;
    ann_helper::distance_func int8_l2_squared_distance;
    ann_helper::distance_func int8_negative_inner_product;
    ann_helper::distance_func int8_cosine_distance;
    ann_helper::flip_sign_func f_flip_sign;
    ann_helper::kacs_walk_func f_kacs_walk;
    ann_helper::warmup_ip_x0_q_func f_warmup_ip_x0_q;
    ann_helper::ip_fxi_func f_ip_fxi;
    ann_helper::mask_ip_x0_q_func f_mask_ip_x0_q;
    ann_helper::float_to_half_func float_to_half;
    ann_helper::half_to_float_func half_to_float;
    void *ann_cxt;
    void *redistrib_elem_tracker;
    void *qt_update_cxt;
    void *qt_update_mgr;
} knl_g_annvec_context;

typedef struct knl_g_rabitq_context {
    void *cache_ctx;
    int cache_size;
    void *lru_cache;
    char caches[1024];
} knl_g_rabitq_context;

/* Vector buffer manager context */
typedef struct knl_g_diskann_context {
    MemoryContext vec_buf_ctx;
    void *vec_buffer_mgr;
    size_t vector_buffers;
    bool enable_buffer_manager;
    int vector_buffer_workers;
    int vec_writer_nproc;
    int16 pool_offset_to_write;
    void **vec_writer_latch;
} knl_g_diskann_context;

struct GlobalInstance {
    knl_g_annvec_context annvec_cxt;
    knl_g_rabitq_context rabitq_ctx;
    knl_g_diskann_context diskann_cxt;
};

extern GlobalInstance g_instance;

void* mem_align_alloc(size_t alignment, size_t size);
void mem_align_free(void* ptr);

#endif
