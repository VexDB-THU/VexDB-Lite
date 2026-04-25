#ifndef VEX_CORE_H
#define VEX_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vex_index vex_index;

typedef enum vex_metric_t {
    VEX_METRIC_L2 = 0,
    VEX_METRIC_COSINE = 1,
    VEX_METRIC_INNER_PRODUCT = 2
} vex_metric_t;

typedef struct vex_index_config_t {
    uint32_t dimension;
    uint32_t m;
    uint32_t ef_construction;
    vex_metric_t metric;
} vex_index_config_t;

typedef struct vex_result_t {
    int64_t row_id;
    float distance;
} vex_result_t;

typedef enum vex_error_t {
    VEX_OK = 0,
    VEX_ERROR_INVALID_ARGUMENT = 1,
    VEX_ERROR_DIMENSION_MISMATCH = 2,
    VEX_ERROR_INTERNAL = 3
} vex_error_t;

vex_index *vex_index_create(const vex_index_config_t *config);
void vex_index_destroy(vex_index *idx);

int vex_index_add(vex_index *idx, int64_t row_id, const float *vec, uint32_t dim);
int vex_index_add_batch(vex_index *idx, const int64_t *row_ids, const float *vecs,
                        uint32_t dim, uint32_t count, int threads);

int vex_index_search(vex_index *idx, const float *query, uint32_t dim, uint32_t k,
                     int ef, vex_result_t *results, uint32_t *count);

int vex_index_serialize(vex_index *idx, void **data, size_t *size);
vex_index *vex_index_deserialize(const void *data, size_t size,
                                 const vex_index_config_t *cfg);

void vex_free_buffer(void *ptr);

#ifdef __cplusplus
}
#endif

#endif // VEX_CORE_H
