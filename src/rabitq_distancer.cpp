#include "pg_compat.h"
#include "floatvector.h"
#include "rabitq/rabitq_distancer.h"

namespace rabitq {

void RabitqDistancer::destroy()
{
    prepared = false;
}

void RabitqDistancer::load_rabitq(Relation index, void *metapage)
{
    (void)index;
    (void)metapage;
}

void RabitqDistancer::train(Relation index, FloatVectorArray samples, int dim,
    Metric metric, bool need_norm, int parallel_workers, int maintenance_work_mem)
{
    (void)index;
    (void)samples;
    (void)dim;
    (void)metric;
    (void)need_norm;
    (void)parallel_workers;
    (void)maintenance_work_mem;
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
        errmsg("RaBitQ quantizer not implemented")));
}

void RabitqDistancer::prepare(Relation index, void *metap)
{
    (void)index;
    (void)metap;
}

void RabitqDistancer::process(const char *query)
{
    (void)query;
}

void RabitqDistancer::flush(Relation index, BlockNumber qtcode_block, bool enabling)
{
    (void)index;
    (void)qtcode_block;
    (void)enabling;
}

} /* namespace rabitq */
