#ifndef ANNKMEANS_H
#define ANNKMEANS_H

#include "floatvector.h"
#include "access/reloptions.h"
#include "distance/core/distance.h"

struct AnnKmeansState {
    bool skipCheckDuplicate;
    Metric metric;
    ann_helper::distance_func kmeansprocinfo;
    ann_helper::distance_func kmeansnormprocinfo;
    char indexName[NAMEDATALEN + 1];
};

struct SampleState {
    DistPrecisionType precision_type;
    bool need_norm;
    uint32 dim;
    bool *fillflags;
    FloatVectorArray samples;
};

#define FREE_ANNKEMANSTATE(astate) pfree(astate)
#define MAX_SAMPLE_VECTOR_NUM 50000

extern void setupKmeansState(Metric metric, Relation index, AnnKmeansState *kmeanstate, int dimension, bool ispq, bool pqtrain);
extern void AnnKmeans(AnnKmeansState *kmeanstate, FloatVectorArray samples, FloatVectorArray centers, int avgMaintenanceWorkMem, int parallelWorkers);
extern int GetSampleNumbers(Relation heap, Relation index, int listNum);
extern void ann_sample_rows(FloatVectorArray samples, Relation heap, Relation index, int dimensions,
    int parallelWorkers, bool need_norm, DistPrecisionType precision_type = DistPrecisionType::FLOAT);

#endif
