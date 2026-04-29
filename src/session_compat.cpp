#include "pg_compat.h"

#ifdef PG_EXTENSION

PgVexdbSessionAttrs pg_vexdb_session = {
    .attr_storage = {
        .ef_search = 64,
        .float_l2_arch = 0,
        .float_ip_arch = 0,
        .float_cos_arch = 0,
        .half_l2_arch = 0,
        .half_ip_arch = 0,
        .half_cos_arch = 0,
        .int8_l2_arch = 0,
        .int8_ip_arch = 0,
        .int8_cos_arch = 0
    }
};

#endif
