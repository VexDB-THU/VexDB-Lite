/*
 * graph_index_build.cpp - Graph index build implementation
 * Aligned with openGauss: single-thread memory build, on OOM flush + launch
 * parallel workers for disk build.
 */

#include <vtl/holder>
#include <vtl/disk_container/freespace.hpp>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <thread>
#include <vector>
/* PG port.h (via vtl/disk_container) 在前面已 #define snprintf pg_snprintf,
 * boost::source_location::to_string() 用 std::snprintf 会被替换成
 * std::pg_snprintf(不存在,ARM g++-9 严格解析时报错)。
 * 在 boost/lockfree include 之前 #undef 让 boost 内部用真 std::snprintf;
 * pg_compat.h 在 boost 之后重新 include,带回 PG 的 macro。 */
#ifdef snprintf
#undef snprintf
#endif
#include <boost/lockfree/queue.hpp>

#include "pg_compat.h"

extern "C" {
#include "access/parallel.h"
#include "storage/shm_toc.h"
#include "commands/progress.h"
#include "port/atomics.h"
}

#include "access/tableam.h"
#include "graph_index/graph_index.h"
#include "graph_index/graph_index_cluster.h"
#include "graph_index/graph_index_storage.h"
#include "graph_index/graph_index_algorithm.h"
#include "graph_index/graph_index_xlog.h"
#include "ann_utils.h"
#include "module/timer.h"
#include "distance/core/distance_dispatcher.h"
#include "annkmeans.h"
#include "quantizer/annkmeans.h"
#include "floatvector.h"
#include "pq.h"
#include "rabitq/rabitq_distancer.h"
#include "rel_utils.h"
#include "vector_buffer/vector_smgr.h"

using namespace disk_container;
using namespace ann_helper;
using namespace rabitq;

extern int maintenance_work_mem;

/* Slim shared state for parallel disk build — only OIDs + blockno + scan desc.
 * Leader creates the ParallelContext at build_graph start (workers NOT launched).
 * When memory is exhausted, build_callback calls LaunchParallelWorkers and
 * workers join the already-running parallel scan. */
struct GraphIndexShared {
    Oid heaprelid;
    Oid indexrelid;
    BlockNumber metablkno;
    bool isconcurrent;
    pg_atomic_uint32 tuples_done;
    pg_atomic_uint32 worker_reltuples;
    /* ParallelBlockTableScanDescData follows (variable-length, must be last) */
};

#define PARALLEL_KEY_GRAPH_INDEX_SHARED  UINT64CONST(0x76580001)

#define ParallelTableScanFromGraphIndexShared(shared) \
    ((ParallelTableScanDesc)((char *)(shared) + MAXALIGN(sizeof(GraphIndexShared))))

/* Lock-free bounded work queue for parallel memory build.
 * Uses boost::lockfree::queue with pg_yield backoff instead of mutex. */
struct PooledThreadWorkItem {
    ItemPointerData tid;
    char _pad[2]; /* pad to 8 bytes for natural alignment */
    /* vector_size bytes of query data follow, requires 64-byte alignment */
};

struct ThreadWorkQueue {
    static constexpr size_t MAX_QUEUE = 65534;
    using Queue = boost::lockfree::queue<PooledThreadWorkItem*,
                    boost::lockfree::capacity<MAX_QUEUE>>;

    Queue queue;
    bool done{false};
    std::atomic<uint32_t> tuples_done{0};

    void push(PooledThreadWorkItem *item) {
        for (unsigned int k = 0; !queue.push(item); ++k)
            pg_yield(k);
    }

    PooledThreadWorkItem* pop() {
        PooledThreadWorkItem *item = nullptr;
        for (unsigned int k = 0; !queue.pop(item); ++k) {
            if (done) return nullptr;
            pg_yield(k);
        }
        return item;
    }
};

static DistPrecisionType get_data_type(Relation index)
{
    return DistPrecisionType::FLOAT;
}

static uint_fast16_t adjust_m(uint_fast16_t orig_m, IdType id_type)
{
    const size_t base_size = 2 * (id_type == IdType::U32 ? sizeof(uint32) : sizeof(size_t));
    size_t elem_size = base_size * orig_m;
    size_t nelem = disk_container::vtl::VarParam<char>{elem_size}.n_data_per_block();
    for (uint_fast16_t m = orig_m + 1;; ++m) {
        elem_size += base_size;
        if (nelem != disk_container::vtl::VarParam<char>(elem_size).n_data_per_block()) {
            return m - 1;
        }
    }
}

static Metric get_metric_from_index(Relation index)
{
    FmgrInfo *procinfo = index_getprocinfo(index, 1, GRAPH_INDEX_DISTANCE_PROC);
    if (procinfo == NULL) {
        return Metric::L2;
    }
    Metric metric = get_func_metric(procinfo->fn_oid);
    if (metric == Metric::INNER_PRODUCT &&
        graph_index_optional_proc_info(index, GRAPH_INDEX_NORM_PROC) != NULL) {
        return Metric::FAST_COSINE;
    }
    return metric;
}

extern "C" PGDLLEXPORT void graph_index_parallel_build_main(dsm_segment *seg, shm_toc *toc);

class GraphIndexBuild {
    friend void graph_index_parallel_build_main(dsm_segment *seg, shm_toc *toc);

public:
    GraphIndexBuild(Relation index, int nparallel, MemoryContext build_ctx, ForkNumber fork_num)
        : fork_num(fork_num),
          id_type(graph_index_get_id_type(index)),
          qt_type(graph_index_get_quantizer_type(index)),
          compact_mode(graph_index_get_compact_mode(index)),
          precision_type(get_data_type(index)),
          m(adjust_m(graph_index_get_m(index), id_type)),
          ef_construction(graph_index_get_ef_construction(index)),
          parallel_workers(nparallel),
          maintenance_work_mem_kb(maintenance_work_mem),
          collation(index->rd_indcollation[0]),
          build_ctx(build_ctx)
    {
        if (ef_construction < 2 * m) {
            elog(ERROR, "ef_construction must be greater than or equal to 2 * m");
        }

        need_norm = graph_index_optional_proc_info(index, GRAPH_INDEX_NORM_PROC) != NULL;

        int temp_dim = TupleDescAttr(index->rd_att, 0)->atttypmod;
        dimension = temp_dim > 0 ? (uint_fast16_t)temp_dim : 0;

        if (dimension == 0) {
            elog(ERROR, "Could not determine vector dimension from index attribute");
        }

        if (precision_type != DistPrecisionType::CUSTOM) {
            vector_size = dimension * get_dtype_size(precision_type);
        } else if (TupleDescAttr(index->rd_att, 0)->attbyval) {
            vector_size = std::max<int16>(TupleDescAttr(index->rd_att, 0)->attlen, 0);
        } else {
            vector_size = 0;
        }

        metric = get_metric_from_index(index);

        concurrent_quant = true;
        elem_size = vector_size;

        if (parallel_workers > 0) {
            if (index->rd_index->indrelid != InvalidOid) {
                Relation heap = table_open(index->rd_index->indrelid, AccessShareLock);
                if (heap && heap->rd_rel->relpersistence == RELPERSISTENCE_TEMP) {
                    ereport(NOTICE, (errmsg("switch off parallel mode for temp table")));
                    parallel_workers = 0;
                }
                table_close(heap, AccessShareLock);
            }
        }

        init_single_thread_memstore(index);
    }

    BlockNumber build_index(Relation heap, Relation index, IndexInfo *index_info)
    {
        if (compact_mode && heap != NULL &&
            heap->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("memory_mode='compact' is not supported for unlogged tables"),
                 errdetail("The unlogged init fork cannot preserve a trained quantizer after a crash.")));
        }

        create_metapage(index);
        if (qt_type != QuantizerType::NONE) {
            if (init_quantizer(heap, index)) {
                elem_size = visit([](auto &q) { return q.code_size(); },
                    quantizer.value());
                /*
                 * A compact index must not build its graph from approximate
                 * codes.  Keep the trained quantizer disabled while the graph
                 * is built from raw vectors, then encode the vector fork in
                 * place and publish the quantizer only after compaction.
                 */
                defer_compact_encoding = compact_mode;
                if (!defer_compact_encoding) {
                    enable_quantizer();
                }
            } else {
                if (compact_mode && heap != NULL) {
                    ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("memory_mode='compact' requires an active %s quantizer",
                             quantizer_name(qt_type))));
                }
                qt_type = QuantizerType::NONE;
            }
        }
        build_graph(heap, index, index_info);
        if (defer_compact_encoding) {
            compact_raw_vectors(index);
            enable_quantizer();
        }
        if (RelationNeedsWAL(index) || fork_num == INIT_FORKNUM) {
            log_index(index);
        }
        return metablkno;
    }

    void create_metapage(Relation index)
    {
        metabuf = ReadBufferExtended(index, fork_num, P_NEW, RBM_NORMAL, NULL);
        LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
        metablkno = BufferGetBlockNumber(metabuf);
        Assert(metablkno == GRAPH_INDEX_METAPAGE_BLKNO);
        BlockNumber ps_blkno = PlainStore::get_plain_store(index, false, fork_num);
        Assert(ps_blkno == GRAPH_INDEX_PS_BLKNO);

        Page metapage = BufferGetPage(metabuf);
        metablkno = BufferGetBlockNumber(metabuf);
        graph_index_init_page(metabuf, metapage);
        metap = GRAPH_INDEX_PAGE_GET_META(metapage);

        metap->magic_number = GRAPH_INDEX_MAGIC_NUMBER;
        metap->version = GRAPH_INDEX_VERSION;
        metap->dimension = dimension;
        metap->m = m;
        metap->ef_construction = ef_construction;
        metap->metric = metric;
        metap->precision_type = precision_type;

        metap->cluster_block = InvalidBlockNumber;

        if (id_type == IdType::U32) {
            metap->base_block = DiskVector<GraphIndexDiskBasePoint<uint32>>::get_disk_vector(index, false, fork_num);
            metap->upper_block = DiskVector<GraphIndexDiskUpperPoint<uint32>>::get_disk_vector(index, false, fork_num);
            metap->free_id_list_block = FreeSpace<uint32>::get_freespace_meta(index, false, fork_num);
            metap->free_upper_list_block = FreeSpace<uint32>::get_freespace_meta(index, false, fork_num);
            metap->async_id_list_block = FreeSpace<uint32>::get_freespace_meta(index, false, fork_num);
        } else {
            metap->base_block = DiskVector<GraphIndexDiskBasePoint<size_t>>::get_disk_vector(index, false, fork_num);
            metap->upper_block = DiskVector<GraphIndexDiskUpperPoint<size_t>>::get_disk_vector(index, false, fork_num);
            metap->free_id_list_block = FreeSpace<size_t>::get_freespace_meta(index, false, fork_num);
            metap->free_upper_list_block = FreeSpace<size_t>::get_freespace_meta(index, false, fork_num);
            metap->async_id_list_block = FreeSpace<size_t>::get_freespace_meta(index, false, fork_num);
        }

        metap->elems_block = VarDiskVector<GraphIndexPoint>::get_disk_vector(index, false, fork_num);

        if (qt_type == QuantizerType::NONE) {
            metap->qtcode_block = InvalidBlockNumber;
        } else {
            Buffer qt_buf = ReadBufferExtended(index, fork_num, P_NEW, RBM_NORMAL, NULL);
            qtcode_block = metap->qtcode_block = BufferGetBlockNumber(qt_buf);
            ReleaseBuffer(qt_buf);
        }

        metap->quantizer_metainfo.init(qt_type, dimension,
                                       (uint32)graph_index_get_pq_m(index));

        metap->entry_level = -1;
        metap->entrypoint_id = INVALID_VECTOR_ID;
        metap->entry_cur_layer_idx = INVALID_VECTOR_ID;
        metap->num_vectors = 0;

        ((PageHeader)metapage)->pd_lower = ((char *)metap + sizeof(GraphIndexMetaPageData)) - (char *)metapage;
        MarkBufferDirty(metabuf);
        LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
    }

    void log_index(Relation index)
    {
        GraphIndexXlog xlog;
        xlog.init(index, metabuf, BufferGetPage(metabuf));
        xlog.log_build_index(fork_num);
    }

    void destroy() {
        if (quantizer.has_value()) {
            visit([](auto &q) { q.destroy(); }, quantizer.value());
            quantizer.destroy();
        }
        if (BufferIsValid(metabuf)) {
            ReleaseBuffer(metabuf);
        }
    }

    double get_reltuples() const { return reltuples; }

private:
    struct BuildCallbackDataBase {
        GraphIndexBuild *build;
        Relation index;
        Relation heap;
        Buffer own_metabuf;
        DiskStoreVariant disk_store;
        PointExtensionContext ctx;
        GraphIndexShared *shared;
        DistPrecisionType precision_type;
        uint_fast16_t dimension;
        size_t vector_size;
        bool need_norm;
        vector_preprocess_func norm_func_ptr;

        BuildCallbackDataBase(GraphIndexBuild *reference, Relation index, Relation heap,
            BlockNumber metablkno, GraphIndexShared *shared_arg = nullptr)
            : build(reference),
              index(index),
              heap(heap),
              own_metabuf(ReadBuffer(index, metablkno)),
              ctx(index, GRAPH_INDEX_PS_BLKNO, false),
              shared(shared_arg),
              precision_type(GRAPH_INDEX_PAGE_GET_META(
                  BufferGetPage(own_metabuf))->precision_type),
              dimension(GRAPH_INDEX_PAGE_GET_META(
                  BufferGetPage(own_metabuf))->dimension),
              vector_size((size_t)dimension * get_dtype_size(precision_type)),
              need_norm(graph_index_optional_proc_info(index, GRAPH_INDEX_NORM_PROC) != NULL),
              norm_func_ptr(need_norm
                  ? get_vector_preprocess_func(Metric::FAST_COSINE, precision_type, dimension)
                  : nullptr)
        {
            RelationGetSmgr(index);
            create_disk_store(disk_store, index, heap, own_metabuf, false, true);
        }

        Pair<char *, bool> read_vec(Pointer &vec_p, Datum *values)
        {
            char *v = DatumGetVector(values[0], precision_type, &vec_p);
            char *query = v;
            bool is_alloc = false;
            if (!is_aligned(v) || need_norm) {
                query = alloc_vector(vector_size);
                memcpy(query, v, vector_size);
                is_alloc = true;
            }
            if (need_norm) {
                norm_func_ptr(query, dimension, query);
            }
            return {query, is_alloc};
        }

        void free_vec(Pointer &vec_p, Datum *values, char *query, bool is_alloc)
        {
            if (vec_p != DatumGetPointer(values[0])) {
                pfree(vec_p);
            }
            if (is_alloc) {
                free_vector(query);
            }
        }

        void destroy()
        {
            disk_store.destroy();
            ctx.destroy();
            ReleaseBuffer(own_metabuf);
        }
    };

    template <typename D1, typename D2>
    struct BuildCallbackData : public BuildCallbackDataBase {
        D1 &mem_distancer;
        D2 &disk_distancer;

        BuildCallbackData(GraphIndexBuild *reference, Relation index, Relation heap,
                          D1 &d1, D2 &d2, BlockNumber metablkno,
                          GraphIndexShared *shared_arg = nullptr)
            : BuildCallbackDataBase(reference, index, heap, metablkno, shared_arg),
              mem_distancer(d1),
              disk_distancer(d2) {}
    };

    /* Info */
    ForkNumber fork_num;
    BlockNumber metablkno;
    Buffer metabuf;

    /* Settings */
    GraphIndexMetaPage metap;
    bool need_norm;
    IdType id_type;
    QuantizerType qt_type;
    bool compact_mode;
    DistPrecisionType precision_type;
    static constexpr VecStorageType storage_type = VecStorageType::PureVec;
    uint_fast16_t dimension;
    uint_fast16_t m;
    uint_fast16_t ef_construction;
    BlockNumber qtcode_block;
    int parallel_workers;
    int maintenance_work_mem_kb;
    size_t vector_size;

    /* Quantizer */
    bool concurrent_quant;
    Optional<Variant<PQDistancer, RabitqDistancer>> quantizer;
    size_t elem_size;
    bool defer_compact_encoding{false};

    /* Statistics */
    double reltuples{0};
    uint64 progress_tuples_done{0};
    static constexpr uint64 progress_update_step = 4096;

    /* Support functions */
    Metric metric;
    Oid collation;

    /* Memory */
    MemoryContext build_ctx;
    /* MemStore 专用 ctx, 独立于 build_ctx: 并行 memory build 时 leader 的 build_callback 在
     * build_ctx 上 detoast palloc/pfree(read_vec/free_vec), worker 若也在 build_ctx 上分配
     * MemStore 内存(elems realloc 等)会与 leader 并发 race build_ctx 的 AllocSet blocks 链表
     * → wild pointer / double-free。 */
    MemoryContext mem_store_ctx = nullptr;

    /* Build state */
    enum class BuildState {
        MEMORY,
        DISK
    };
    BuildState build_state{BuildState::MEMORY};

    Holder<MemStore<>> mem_store;

    /* Flush */
    std::shared_mutex flush_lock;
    bool flush_warned;

    /* Parallel disk build (workers launched on OOM) */
    ParallelContext *parallel_pcxt = nullptr;
    bool parallel_workers_launched = false;
    GraphIndexShared *cached_shared = nullptr;

    /* Thread pool for parallel memory build (thread-based, not PG workers) */
    std::vector<std::thread> thread_pool;
    ThreadWorkQueue *thread_queue = nullptr;
    PointExtensionContext *thread_ctx = nullptr;
    size_t estimated_limit{0};

    void initialize_memstore()
    {
        build_state = BuildState::MEMORY;
        /* MemStore 用独立 ctx(build_ctx 子), 让 worker 的 MemStore 分配完全不碰 build_ctx,
         * leader 的 build_callback detoast 独占 build_ctx, 消除跨线程 AllocSet race。 */
        mem_store_ctx = AllocSetContextCreate(build_ctx, "mem_store context", ALLOCSET_DEFAULT_SIZES);
        MemoryContext old_msc = MemoryContextSwitchTo(mem_store_ctx);
        mem_store.emplace(dimension, m, vector_size, mem_store_ctx, false);
        MemoryContextSwitchTo(old_msc);
    }

    /* Aligned with openGauss: memory build when at least 1GB is available,
     * otherwise start directly on disk.  DiskStore and MemStore must produce
     * the same graph quality; hiding a DiskStore correctness bug behind a large
     * serial memory seed both wastes build time and defeats worker scaling. */
    void init_single_thread_memstore(Relation index)
    {
        constexpr int min_memory_required_kb = 1024 * 1024; /* 1GB */
        if (maintenance_work_mem_kb < min_memory_required_kb) {
            build_state = BuildState::DISK;
            create_vec_data(index, true);
            ereport(WARNING,
                (errmsg("maintenance_work_mem below 1GB, will use direct disk build "
                        "and take significantly more time.")));
            flush_warned = true;
            return;
        }
        flush_warned = false;
        initialize_memstore();
    }

    /* Pre-create ParallelContext (WITHOUT LaunchParallelWorkers) so the parallel scan
     * descriptor is ready from the beginning. Workers are launched later in
     * build_callback when OOM triggers flush. */
    void prepare_parallel_context(Relation heap, Relation index, IndexInfo *index_info)
    {
        Snapshot snapshot;
        if (index_info->ii_Concurrent) {
            snapshot = RegisterSnapshot(GetTransactionSnapshot());
        } else {
            snapshot = SnapshotAny;
        }

        EnterParallelMode();

        parallel_pcxt = CreateParallelContext("vexdb_lite",
            "graph_index_parallel_build_main", parallel_workers);

        Size dsm_size = MAXALIGN(sizeof(GraphIndexShared))
                      + table_parallelscan_estimate(heap, snapshot);
        shm_toc_estimate_chunk(&parallel_pcxt->estimator, dsm_size);
        shm_toc_estimate_keys(&parallel_pcxt->estimator, 1);
        InitializeParallelDSM(parallel_pcxt);

        if (parallel_pcxt->seg == NULL) {
            DestroyParallelContext(parallel_pcxt);
            ExitParallelMode();
            parallel_pcxt = nullptr;
            return;
        }

        GraphIndexShared *shared = (GraphIndexShared *)
            shm_toc_allocate(parallel_pcxt->toc, dsm_size);
        shared->heaprelid = RelationGetRelid(heap);
        shared->indexrelid = RelationGetRelid(index);
        shared->metablkno = metablkno;
        shared->isconcurrent = index_info->ii_Concurrent;
        pg_atomic_init_u32(&shared->tuples_done, 0);
        pg_atomic_init_u32(&shared->worker_reltuples, 0);
        table_parallelscan_initialize(heap, ParallelTableScanFromGraphIndexShared(shared), snapshot);
        shm_toc_insert(parallel_pcxt->toc, PARALLEL_KEY_GRAPH_INDEX_SHARED, shared);
    }

    /* Called inside build_callback's EXCLUSIVE flush_lock section after flush. */
    void launch_parallel_workers()
    {
        if (parallel_pcxt == nullptr || parallel_workers_launched) return;
        LaunchParallelWorkers(parallel_pcxt);
        parallel_workers_launched = true;
        cached_shared = (GraphIndexShared *)
            shm_toc_lookup(parallel_pcxt->toc, PARALLEL_KEY_GRAPH_INDEX_SHARED, false);
    }

    bool init_quantizer(Relation heap, Relation index)
    {
        if (heap == NULL || qt_type == QuantizerType::NONE) return false;

        const int minimum_samples = qt_type == QuantizerType::PQ
            ? 256
            : GRAPH_INDEX_MIN_QT_SAMPLES_SIZE;
        size_t estimated_samples = get_relstats_reltuples(heap);
        size_t target = std::max<size_t>(minimum_samples, estimated_samples);
        // PQ has 256 centroids per subquantizer. Beyond 64 training points per
        // centroid, serial PG-process K-means adds substantial build time for
        // little codebook gain. Keep RaBitQ's existing 50k cap because it
        // trains one shared 16-cluster model instead of M independent models.
        constexpr size_t pq_training_points_per_centroid = 64;
        constexpr size_t pq_centroids = 256;
        const size_t quantizer_sample_cap = qt_type == QuantizerType::PQ
            ? pq_training_points_per_centroid * pq_centroids
            : static_cast<size_t>(MAX_SAMPLE_VECTOR_NUM);
        target = std::min(target, quantizer_sample_cap);

        size_t bytes_per_sample = (size_t)dimension * sizeof(float);
        const size_t work_mem_bytes = (size_t)maintenance_work_mem_kb * 1024;
        // Leave 20% for executor state, detoasted tuples, the distancer and PG
        // memory-context overhead. The remaining budget covers the complete
        // overlapping training peak, not just the raw sample matrix.
        const size_t training_budget = work_mem_bytes - work_mem_bytes / 5;
        auto estimate_training_peak = [&](size_t sample_count) -> size_t {
            if (sample_count > std::numeric_limits<size_t>::max() / bytes_per_sample) {
                return std::numeric_limits<size_t>::max();
            }
            size_t total = sample_count * bytes_per_sample;
            auto add = [&](size_t bytes) {
                if (total > std::numeric_limits<size_t>::max() - bytes) {
                    total = std::numeric_limits<size_t>::max();
                } else {
                    total += bytes;
                }
            };

            if (qt_type == QuantizerType::PQ) {
                uint16 pq_m = 0;
                uint16 pq_k = 0;
                pq_set_param((uint32)dimension, pq_m, pq_k,
                             (uint32)graph_index_get_pq_m(index));
                const size_t dsub = (size_t)dimension / pq_m;
                add((size_t)dimension * pq_k * sizeof(float)); // global codebook
                add(sample_count * dsub * sizeof(float));      // packed subvectors
                add((size_t)pq_k * dsub * sizeof(float));      // sub-centers
                add(vex::quantizer::EstimateKMeansScratchBytes(
                    sample_count, pq_k, dsub));
            } else {
                const size_t padded_dim = RABITQ_PADDED_DIM((size_t)dimension);
                add((size_t)GRAPH_INDEX_RABITQ_NUM_CLUSTERS * dimension * sizeof(float));
                add((size_t)GRAPH_INDEX_RABITQ_NUM_CLUSTERS * padded_dim * sizeof(float));
                add(4 * padded_dim / 8); // FhtKacRotator sign-flip matrix
                add(vex::quantizer::EstimateKMeansScratchBytes(
                    sample_count, GRAPH_INDEX_RABITQ_NUM_CLUSTERS, dimension));
            }
            return total;
        };

        size_t low = 0;
        size_t high = std::min(quantizer_sample_cap,
                               (size_t)MaxAllocSize / bytes_per_sample);
        while (low < high) {
            const size_t mid = low + (high - low + 1) / 2;
            if (estimate_training_peak(mid) <= training_budget) {
                low = mid;
            } else {
                high = mid - 1;
            }
        }
        const size_t max_samples_by_memory = low;
        if (max_samples_by_memory < (size_t)minimum_samples) {
            const size_t minimum_bytes = estimate_training_peak(minimum_samples);
            if (compact_mode) {
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("memory_mode='compact' requires an active %s quantizer",
                         quantizer_name(qt_type)),
                     errdetail("Training requires at least %zu MB including samples and working memory.",
                         (minimum_bytes + 1024 * 1024 - 1) / (1024 * 1024)),
                     errhint("Raise maintenance_work_mem.")));
            }
            ereport(NOTICE,
                (errmsg("vex %s: training needs at least %zu MB including working memory, "
                        "skipping quantizer training",
                        quantizer_name(qt_type),
                        (minimum_bytes + 1024 * 1024 - 1) / (1024 * 1024)),
                 errhint("Raise maintenance_work_mem to enable this quantizer.")));
            return false;
        }
        target = std::min(target, max_samples_by_memory);

        FloatVectorArray samples = FloatVectorArrayInit((int)target, dimension);
        ann_sample_rows(samples, heap, index, dimension, (int)target,
                        need_norm, DistPrecisionType::FLOAT);
        if (samples->length < minimum_samples) {
            int sample_len = samples->length;
            FloatVectorArrayFree(samples);
            if (compact_mode) {
                ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("memory_mode='compact' requires an active %s quantizer",
                         quantizer_name(qt_type)),
                     errdetail("Only %d sample rows are available; at least %d are required.",
                         sample_len, minimum_samples)));
            }
            ereport(NOTICE,
                (errmsg("vex %s: only %d sample rows < required=%d, "
                        "skipping quantizer training",
                        quantizer_name(qt_type), sample_len, minimum_samples)));
            return false;
        }

        if (qt_type == QuantizerType::PQ) {
            quantizer.emplace();
            quantizer->template emplace<PQDistancer>();
            PQDistancer &tmp = quantizer->template get<PQDistancer>();
            tmp.train(index, samples, dimension, metric, false,
                      parallel_workers, maintenance_work_mem_kb,
                      (uint32)graph_index_get_pq_m(index));
            tmp.flush(index, qtcode_block, false);
        } else {
            quantizer.emplace();
            quantizer->template emplace<RabitqDistancer>();
            RabitqDistancer &tmp = quantizer->template get<RabitqDistancer>();
            tmp.train(index, samples, dimension, metric, need_norm,
                      parallel_workers, maintenance_work_mem_kb);
            tmp.flush(index, qtcode_block, false);
        }
        FloatVectorArrayFree(samples);
        return true;
    }

    template <typename D>
    void insert_in_memory(BuildCallbackDataBase &data, D &d, const char *query, ItemPointer tid)
    {
        GraphIndexAlgorithm algo{ef_construction, m, *mem_store, d};
        typename decltype(algo)::InsertContext ctx{data.ctx, query, tid};
        algo.insert(ctx);
        ctx.destroy();
    }

    void enable_quantizer()
    {
        LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
        metap->quantizer_metainfo.set_enable();
        MarkBufferDirty(metabuf);
        LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
    }

    void compact_raw_vectors(Relation index)
    {
        if (!quantizer.has_value()) {
            ereport(ERROR,
                (errmsg("%s codebook unavailable during compact vector encoding",
                    quantizer_name(qt_type))));
        }

        RelationGetSmgr(index);
        const size_t vector_count = metap->num_vectors;
        constexpr size_t batch_rows = 4096;
        char *raw = (char *)palloc(batch_rows * vector_size);
        char *codes = (char *)palloc(batch_rows * elem_size);

        visit([&](auto &encoder) {
            auto encode_batch = [&](size_t first, size_t count) {
                const size_t raw_bytes = count * vector_size;
                SMGR_READ_STATUS status = vec_read(
                    index->rd_smgr, (off_t)(first * vector_size), raw_bytes,
                    raw, VecStorageType::PureVec);
                if (status != SMGR_RD_OK) {
                    ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("could not read raw vectors while compacting graph index"),
                         errdetail("first vector %zu, count %zu, read status %d",
                             first, count, (int)status)));
                }
                for (size_t i = 0; i < count; ++i) {
                    encoder.compute_code(
                        (float *)(raw + i * vector_size),
                        codes + i * elem_size);
                }
                vec_write(index->rd_smgr, (off_t)(first * elem_size),
                          count * elem_size, codes, false,
                          VecStorageType::PureCode);
            };

            if (elem_size <= vector_size) {
                /* Shrinking in place: low destinations never overwrite raw
                 * vectors needed by a later forward batch. */
                for (size_t first = 0; first < vector_count; first += batch_rows) {
                    encode_batch(first, Min(batch_rows, vector_count - first));
                }
            } else {
                /* Very small dimensions can have a RaBitQ code larger than
                 * the raw vector because of fixed estimator metadata.  Grow
                 * in place from the end so code writes cannot overwrite raw
                 * vectors that have not been encoded yet. */
                size_t end = vector_count;
                while (end > 0) {
                    const size_t first = end > batch_rows ? end - batch_rows : 0;
                    encode_batch(first, end - first);
                    end = first;
                }
            }
        }, quantizer.value());

        pfree(codes);
        pfree(raw);

        if (enable_vec_buffer_manager()) {
            Oid relnode = index->rd_smgr->smgr_rlocator.locator.relNumber;
            vec_invalidate_buffer_cache(relnode, vector_size);
        }
        truncate_vector_file_to(index, vector_count * elem_size);
    }

    template <typename D>
    void insert_on_disk(BuildCallbackDataBase &data, D &d, const char *query, ItemPointer tid)
    {
        d.process(query);
        if (id_type == IdType::U32) {
            auto &ds = data.disk_store.template get<DiskStore<uint32>>();
            GraphIndexAlgorithm algo{ef_construction, m, ds, d};
            typename decltype(algo)::InsertContext ctx{data.ctx, query, tid};
            algo.insert(ctx);
            ctx.destroy();
        } else {
            auto &ds = data.disk_store.template get<DiskStore<size_t>>();
            GraphIndexAlgorithm algo{ef_construction, m, ds, d};
            typename decltype(algo)::InsertContext ctx{data.ctx, query, tid};
            algo.insert(ctx);
            ctx.destroy();
        }
    }

    /* openGauss-style: measure palloc usage of build_ctx, compare against
     * maintenance_work_mem minus 100MB reserve. */
    /* openGauss double-check pattern: SHARED lock for memory inserts,
     * upgrade to EXCLUSIVE for flush + launch parallel workers. */
    template <typename D1, typename D2>
    static void build_callback(Relation index, ItemPointer tid, Datum *values, bool *isnull,
                               bool tupleIsAlive, void *state)
    {
        if (isnull[0] || !tupleIsAlive) {
            return;
        }

        auto &data = *(BuildCallbackData<D1, D2> *)state;
        GraphIndexBuild &build = *(GraphIndexBuild *)data.build;

        Pointer vec_p;
        auto [query, is_alloc] = data.read_vec(vec_p, values);

        /* Thread pool path: copy query → push to workers, main thread returns.
         * Progress counting is deferred to workers after actual insert. */
        if (!build.thread_pool.empty() && build.build_state == BuildState::MEMORY) {
            /* Threshold check: memory exhausted → drain pool → flush → switch DISK */
            if (build.thread_queue->tuples_done.load() >= build.estimated_limit) {
                build.thread_queue->done = true;
                for (auto &t : build.thread_pool) t.join();
                build.thread_pool.clear();
                ereport(WARNING,
                    (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                     errmsg("graph_index graph no longer fits into maintenance_work_mem after "
                            "%u tuples", build.thread_queue->tuples_done.load()),
                     errdetail("Building will take significantly more time."),
                     errhint("Increase maintenance_work_mem to speed up builds.")));
                build.flush(index);
                build.mem_store->destroy();
                build.build_state = BuildState::DISK;
                build.launch_parallel_workers();
                /* Carry over progress counter to DISK phase */
                pg_atomic_write_u32(&build.cached_shared->tuples_done, build.thread_queue->tuples_done.load());
            } else {
                static constexpr size_t align = 64;
                size_t item_size = (sizeof(PooledThreadWorkItem) + align - 1) & ~(align - 1);
                auto *item = (PooledThreadWorkItem*)aligned_alloc(align, item_size + build.vector_size);
                item->tid = *tid;
                memcpy((char *)item + item_size, query, build.vector_size);
                build.thread_queue->push(item);
                uint64 n = build.thread_queue->tuples_done.fetch_add(1) + 1;
                if (n % progress_update_step == 0) {
                    pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE, (int64)n);
                }
                data.free_vec(vec_p, values, query, is_alloc);
                return;
            }
        }

        /* Non-pool path: count tuples as they are actually inserted */
        uint64 tuples_done;
        if (build.cached_shared) {
            tuples_done = pg_atomic_fetch_add_u32(&build.cached_shared->tuples_done, 1) + 1;
        } else {
            tuples_done = ++build.progress_tuples_done;
        }
        if (tuples_done % progress_update_step == 0) {
            pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE, (int64)tuples_done);
        }

        auto prepare_quantizer_disk_build = [&]() {
            GraphIndexMetaPage metap = GRAPH_INDEX_PAGE_GET_META(BufferGetPage(data.own_metabuf));
            if (metap->quantizer_metainfo.get_type() != QuantizerType::NONE) {
                data.disk_distancer.prepare(index, metap);
            }
        };

        if (build.build_state == BuildState::DISK) {
            prepare_quantizer_disk_build();
            build.insert_on_disk(data, data.disk_distancer, query, tid);
            data.free_vec(vec_p, values, query, is_alloc);
            return;
        }

        if (build.parallel_workers > 0 && build.estimated_limit > 0 &&
            tuples_done >= build.estimated_limit) {
            ereport(WARNING,
                (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                 errmsg("graph_index graph no longer fits into maintenance_work_mem after "
                        "%lu tuples", (unsigned long)tuples_done),
                 errdetail("Building will take significantly more time."),
                 errhint("Increase maintenance_work_mem to speed up builds.")));
            build.flush(index);
            build.mem_store->destroy();
            build.build_state = BuildState::DISK;
            build.launch_parallel_workers();
            if (build.cached_shared) {
                pg_atomic_write_u32(&build.cached_shared->tuples_done, (uint32)tuples_done);
            }
            prepare_quantizer_disk_build();
            build.insert_on_disk(data, data.disk_distancer, query, tid);
            data.free_vec(vec_p, values, query, is_alloc);
            return;
        }

        build.insert_in_memory(data, data.mem_distancer, query, tid);
        data.free_vec(vec_p, values, query, is_alloc);
    }

    void build_single_thread(Relation heap, Relation index, IndexInfo *index_info)
    {
        TableScanDesc scan = NULL;
        if (parallel_pcxt && parallel_pcxt->seg) {
            GraphIndexShared *shared = (GraphIndexShared *)
                shm_toc_lookup(parallel_pcxt->toc, PARALLEL_KEY_GRAPH_INDEX_SHARED, false);
            scan = table_beginscan_parallel(heap, ParallelTableScanFromGraphIndexShared(shared)
#if PG_VERSION_NUM >= 190000
                                            , SO_NONE
#endif
                                            );
        }

        auto run_build_index = [&](auto &d1, auto &d2) {
            using D1 = std::decay_t<decltype(d1)>;
            using D2 = std::decay_t<decltype(d2)>;
            BuildCallbackData<D1, D2> data{this, index, heap, d1, d2, metablkno};
            reltuples = table_index_build_scan(heap, index, index_info, true, false,
                                               build_callback<D1, D2>, (void *)&data, scan);
            data.destroy();
        };

        DispatchRunner<true,
            MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::FAST_COSINE>,
            DistPrecisionTypeList<
                DistPrecisionType::FLOAT
            >, DispatcherMode::BUILD_PAIR>::call(
            metric, precision_type, dimension,
            defer_compact_encoding ? QuantizerType::NONE : qt_type,
            run_build_index);
    }

    void build_graph(Relation heap, Relation index, IndexInfo *index_info)
    {
        if (heap == NULL) {
            return;
        }

        progress_tuples_done = 0;
        pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE, 0);
        size_t reltuples_est = get_relstats_reltuples(heap);
        if (reltuples_est > 0) {
            pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_TOTAL, (int64)reltuples_est);
        }

        /* Pre-create ParallelContext without launching workers.  Direct-disk
         * builds launch immediately; memory builds launch after their graph is
         * flushed because it no longer fits the configured budget. */
        if (parallel_workers > 0) {
            prepare_parallel_context(heap, index, index_info);
            if (build_state == BuildState::DISK) {
                launch_parallel_workers();
            }
        }

        /* Do not run std::thread workers inside a PostgreSQL backend. PG keeps
         * process-global state such as CurrentMemoryContext and CritSectionCount;
         * using PG allocators from C++ threads can trip casserts or corrupt memory.
         * Keep the memory phase single-threaded and use PG parallel workers after
         * the memory budget is flushed to disk. */
        if (parallel_workers > 0 && build_state == BuildState::MEMORY) {
            size_t per_tuple = vector_size
                             + MemStore<>::get_base_point_size(m)
                             + MemStore<>::get_upper_point_size(m) / (m - 1);
            size_t budget = (size_t)maintenance_work_mem_kb * 1024 - 200LL * 1024 * 1024;
            estimated_limit = budget / per_tuple;
        }

        build_single_thread(heap, index, index_info);

        /* Signal workers done and join (threshold may not have been reached) */
        if (!thread_pool.empty()) {
            thread_queue->done = true;
            for (auto &t : thread_pool) t.join();
        }

        /* Flush if still in MEMORY mode (threshold never reached) */
        if (build_state == BuildState::MEMORY) {
            flush(index);
            mem_store->destroy();
        }

        /* Read final progress */
        uint64 final_done;
        if (thread_queue) {
            final_done = thread_queue->tuples_done.load();
        } else if (cached_shared) {
            final_done = pg_atomic_read_u32(&cached_shared->tuples_done);
            reltuples += pg_atomic_read_u32(&cached_shared->worker_reltuples);
        } else {
            final_done = progress_tuples_done;
        }

        /* Clean up thread pool resources */
        if (thread_queue) {
            delete thread_queue;
            thread_queue = nullptr;
        }
        if (thread_ctx) {
            thread_ctx->destroy();
            delete thread_ctx;
            thread_ctx = nullptr;
        }

        /* Wait for any PG parallel workers launched during the scan */
        if (parallel_workers_launched) {
            WaitForParallelWorkersToFinish(parallel_pcxt);
            DestroyParallelContext(parallel_pcxt);
            ExitParallelMode();
        } else if (parallel_pcxt) {
            DestroyParallelContext(parallel_pcxt);
            ExitParallelMode();
        }

        pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE, (int64)final_done);
        pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_TOTAL, (int64)reltuples);
    }

    template <typename T>
    void flush_graph(Relation index)
    {
        GraphIndexMetaPage metap = GRAPH_INDEX_PAGE_GET_META(BufferGetPage(metabuf));
        Timer flush_timer{0, 500'000, "", ""};
        flush_timer.set_stage("Flush Graph Index");

        RelationGetSmgr(index);

        build_state = BuildState::DISK;
        MemStore<> &store = *mem_store;
        GraphIndexEntryInfo entry_info = store.entry_info;

        metap->entry_level = entry_info.level;
        metap->entrypoint_id = entry_info.id;
        metap->entry_cur_layer_idx = entry_info.cur_layer_idx;
        metap->num_vectors = (size_t)store.get_vector_num();

        if (metap->num_vectors > 0) {
            flush_timer.report("Flushing Elems");
            store.flush_points(index, metap->elems_block);

            flush_timer.report("Flushing Basepoint");
            constexpr size_t copybuf_size = 10 * 1024 * 1024;
            T *copybuf = (T *)palloc(copybuf_size);
            size_t basepoint_size = sizeof(T) * m * 2;
            VarDiskVector<GraphIndexDiskBasePoint<T>> base_layer{index, metap->base_block, false, basepoint_size};
            auto &basepoint_pool = *store.basepoint_pool;
            size_t copy_num = copybuf_size / basepoint_size;
            uint32 num_vectors = store.get_vector_num();

            for (uint32 i = 0; i <= num_vectors / copy_num; ++i) {
                size_t batch_offset = i * copy_num;
                size_t actual_copy_num = Min(copy_num, num_vectors - batch_offset);
                if (actual_copy_num == 0) break;
                for (size_t j = 0; j < actual_copy_num; ++j) {
                    size_t vec_idx = batch_offset + j;
                    uint32 *src = (uint32 *)basepoint_pool.get(vec_idx);
                    for (size_t k = 0; k < m * 2; ++k) {
                        copybuf[j * (m * 2) + k] = (T)src[k];
                    }
                }
                base_layer.push_back_n((const GraphIndexDiskBasePoint<T> *)copybuf, actual_copy_num);
            }
            base_layer.destroy();

            flush_timer.report("Flushing Upperpoint");
            size_t upperpoint_size = (m + 1) * 2 * sizeof(T);
            copy_num = copybuf_size / upperpoint_size;
            auto &upperpoint_pool = *store.upperpoint_pool;
            VarDiskVector<GraphIndexDiskUpperPoint<T>> upper_layer{index, metap->upper_block, false, upperpoint_size};
            num_vectors = store.get_upper_num();

            for (size_t i = 0; i <= num_vectors / copy_num; ++i) {
                size_t batch_offset = i * copy_num;
                size_t actual_copy_num = Min(copy_num, num_vectors - batch_offset);
                if (actual_copy_num == 0) break;
                for (size_t j = 0; j < actual_copy_num; ++j) {
                    size_t vec_idx = batch_offset + j;
                    uint32 *neighbors_info = (uint32 *)upperpoint_pool.get(vec_idx);
                    size_t offset = j * (2 + m * 2);
                    copybuf[offset] = neighbors_info[m * 2];
                    copybuf[offset + 1] = neighbors_info[m * 2 + 1];
                    for (size_t k = 0; k < m * 2; ++k) {
                        copybuf[offset + 2 + k] = neighbors_info[k];
                    }
                }
                upper_layer.push_back_n((const GraphIndexDiskUpperPoint<T> *)copybuf, actual_copy_num);
            }
            upper_layer.destroy();
            pfree(copybuf);
        }

        flush_timer.report("Flushing Vector");
        create_vec_data(index, true);
        auto &vector_pool = (*store.vector_pool);
        auto &vec = *vector_pool.get_vec();
        uint32 num_vectors = store.get_vector_num();
        uint32 one_chunk_elem_nums = vector_pool.get_one_chunk_elem_nums();

        bool quantizer_on = (qt_type != QuantizerType::NONE) && !defer_compact_encoding;
        if (compact_mode && !quantizer_on && !defer_compact_encoding) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("memory_mode='compact' cannot flush raw vectors")));
        }
        if (quantizer_on) {
            if (!quantizer.has_value()) {
                ereport(ERROR,
                    (errmsg("%s codebook unavailable during flush_graph",
                        quantizer_name(qt_type))));
            }
            auto write_quantizer_codes = [&](auto &encoder) {
                const size_t code_size = encoder.code_size();
                char *code_chunk = (char *)palloc(one_chunk_elem_nums * code_size);
                for (size_t i = 0; i < vec.size(); ++i) {
                    size_t batch_offset = i * one_chunk_elem_nums;
                    size_t actual_copy_num = Min(one_chunk_elem_nums, num_vectors - batch_offset);
                    if (actual_copy_num == 0) break;
                    for (size_t j = 0; j < actual_copy_num; ++j) {
                        float *raw_vec = (float *)(vec[i].buf + j * vector_size);
                        char *code_dest = code_chunk + j * code_size;
                        encoder.compute_code(raw_vec, code_dest);
                    }
                    off_t offset = batch_offset * code_size;
                    size_t nbytes = actual_copy_num * code_size;
                    vec_write(index->rd_smgr, offset, nbytes, code_chunk, false,
                              VecStorageType::PureCode);
                }
                pfree(code_chunk);
            };
            visit(write_quantizer_codes, quantizer.value());
        } else {
            for (size_t i = 0; i < vec.size(); ++i) {
                size_t batch_offset = i * one_chunk_elem_nums;
                size_t actual_copy_num = Min(one_chunk_elem_nums, num_vectors - batch_offset);
                if (actual_copy_num == 0) break;
                off_t offset = batch_offset * vector_size;
                size_t nbytes = actual_copy_num * vector_size;
                vec_write(index->rd_smgr, offset, nbytes, vec[i].buf, false, storage_type);
            }
        }

        flush_timer.report("Flush Finished");
        LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
        if (quantizer_on) {
            metap->quantizer_metainfo.set_enable();
        }
        MarkBufferDirty(metabuf);
        LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
        flush_timer.destroy();
    }

    void flush(Relation index)
    {
        if (id_type == IdType::U32) {
            flush_graph<uint32>(index);
        } else {
            flush_graph<size_t>(index);
        }
    }

    /* Thread worker for parallel memory build — each thread gets its own
     * MemoryContext so palloc in algo containers is thread-isolated. */
    static void memory_build_worker(GraphIndexBuild *build, int worker_id)
    {
        ThreadWorkQueue &queue = *build->thread_queue;
        PointExtensionContext &ctx = *build->thread_ctx;

        DispatchRunner<true,
            MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::FAST_COSINE>,
            DistPrecisionTypeList<DistPrecisionType::FLOAT>,
            DispatcherMode::DEFAULT>::call(
            build->metric, DistPrecisionType::FLOAT, build->dimension,
            QuantizerType::NONE, [&](auto &distancer) {
            (void)worker_id;

            while (true) {
                PooledThreadWorkItem *item = queue.pop();
                if (!item) break;  // queue.done was set
                static constexpr size_t align = 64;
                static constexpr size_t item_size = (sizeof(PooledThreadWorkItem) + align - 1) & ~(align - 1);
                char *query = (char *)item + item_size;
                auto &store = *build->mem_store;
                GraphIndexAlgorithm<
                    std::remove_reference_t<decltype(store)>,
                    std::remove_reference_t<decltype(distancer)>,
                    MallocAlloc> algo{build->ef_construction, build->m, store, distancer};
                typename decltype(algo)::InsertContext ictx{ctx, query, &item->tid};
                algo.insert(ictx);
                ictx.destroy();
                free(item);
            }
        });
    }
};

PGDLLEXPORT void
graph_index_parallel_build_main(dsm_segment *seg, shm_toc *toc)
{
    GraphIndexShared *shared = (GraphIndexShared *)
        shm_toc_lookup(toc, PARALLEL_KEY_GRAPH_INDEX_SHARED, false);

    RelMgr relmgr(shared->heaprelid, shared->indexrelid);
    LOCKMODE heapLockmode = shared->isconcurrent ? ShareUpdateExclusiveLock : ShareLock;
    LOCKMODE indexLockmode = shared->isconcurrent ? RowExclusiveLock : AccessExclusiveLock;

    relmgr.execute([&](Relation heapRel, Relation indexRel) {
        IndexInfo *indexInfo = BuildIndexInfo(indexRel);
        indexInfo->ii_Concurrent = shared->isconcurrent;

        MemoryContext build_ctx = AllocSetContextCreate(CurrentMemoryContext,
            "GRAPH_INDEX parallel build context", ALLOCSET_DEFAULT_SIZES);
        MemoryContext old_ctx = MemoryContextSwitchTo(build_ctx);

        /* Each worker has its own DiskStore over the same disk structures. The
         * parallel-build lock protocol coordinates entry snapshots, short
         * DiskVector metadata changes and compound neighbor updates. */
        Buffer metabuf = ReadBuffer(indexRel, shared->metablkno);
        BlockNumber metablkno = BufferGetBlockNumber(metabuf);
        DiskStoreVariant disk_store;
        create_disk_store(disk_store, indexRel, heapRel, metabuf, false, true);
        PointExtensionContext ctx(indexRel, GRAPH_INDEX_PS_BLKNO, false);

        auto run_build_index = [&](auto &d1, auto &d2) {
            using D1 = std::decay_t<decltype(d1)>;
            using D2 = std::decay_t<decltype(d2)>;
            GraphIndexBuild::BuildCallbackData<D1, D2> data{
                /* build = */ nullptr,
                indexRel, heapRel, d1, d2, metablkno, shared};

            TableScanDesc scan = table_beginscan_parallel(heapRel,
                ParallelTableScanFromGraphIndexShared(shared)
#if PG_VERSION_NUM >= 190000
                , SO_NONE
#endif
            );

            /* Worker always inserts directly to disk — the leader already flushed
             * before launching workers. Use a minimal callback that skips OOM checks. */
            auto worker_callback = [](Relation index, ItemPointer tid, Datum *values,
                                      bool *isnull, bool alive, void *state) {
                if (isnull[0] || !alive) return;
                auto &cbdata = *(GraphIndexBuild::BuildCallbackData<D1, D2> *)state;

                if (cbdata.shared) {
                    uint32 n = pg_atomic_fetch_add_u32(&cbdata.shared->tuples_done, 1) + 1;
                    if (n % 4096 == 0) {
                        pgstat_progress_update_param(PROGRESS_CREATEIDX_TUPLES_DONE, (int64)n);
                    }
                }

                GraphIndexMetaPage metap = GRAPH_INDEX_PAGE_GET_META(BufferGetPage(cbdata.own_metabuf));
                if (metap->quantizer_metainfo.get_type() != QuantizerType::NONE) {
                    cbdata.disk_distancer.prepare(index, metap);
                }

                /* Use the exact same detoast/alignment/cosine-normalization
                 * path as the leader.  Feeding raw cosine vectors from only
                 * the parallel workers builds a graph with mixed vector
                 * representations and destroys recall at scale. */
                Pointer vec_p;
                auto [query, is_alloc] = cbdata.read_vec(vec_p, values);
                cbdata.disk_distancer.process(query);
                PG_TRY();
                {
                    if (GRAPH_INDEX_PAGE_GET_META(BufferGetPage(cbdata.own_metabuf))->id_type == IdType::U32) {
                        auto &ds = cbdata.disk_store.template get<DiskStore<uint32>>();
                        GraphIndexAlgorithm algo{GRAPH_INDEX_PAGE_GET_META(BufferGetPage(cbdata.own_metabuf))->ef_construction,
                                                 GRAPH_INDEX_PAGE_GET_META(BufferGetPage(cbdata.own_metabuf))->m, ds,
                                                 cbdata.disk_distancer};
                        typename decltype(algo)::InsertContext ctx{cbdata.ctx, query, tid};
                        algo.insert(ctx);
                        ctx.destroy();
                    } else {
                        auto &ds = cbdata.disk_store.template get<DiskStore<size_t>>();
                        GraphIndexAlgorithm algo{GRAPH_INDEX_PAGE_GET_META(BufferGetPage(cbdata.own_metabuf))->ef_construction,
                                                 GRAPH_INDEX_PAGE_GET_META(BufferGetPage(cbdata.own_metabuf))->m, ds,
                                                 cbdata.disk_distancer};
                        typename decltype(algo)::InsertContext ctx{cbdata.ctx, query, tid};
                        algo.insert(ctx);
                        ctx.destroy();
                    }
                }
                PG_CATCH();
                {
                    cbdata.free_vec(vec_p, values, query, is_alloc);
                    PG_RE_THROW();
                }
                PG_END_TRY();

                cbdata.free_vec(vec_p, values, query, is_alloc);
            };

            double local_reltuples = table_index_build_scan(heapRel, indexRel, indexInfo, true, false,
                                    worker_callback, (void *)&data, scan);
            if (shared) {
                pg_atomic_fetch_add_u32(&shared->worker_reltuples, (uint32)local_reltuples);
            }
            data.destroy();
        };

        GraphIndexMetaPage metap = GRAPH_INDEX_PAGE_GET_META(BufferGetPage(metabuf));
        QuantizerType worker_qt_type = metap->quantizer_metainfo.get_type();

        DispatchRunner<true,
            MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::FAST_COSINE>,
            DistPrecisionTypeList<DistPrecisionType::FLOAT>,
            DispatcherMode::BUILD_PAIR>::call(
            get_metric_from_index(indexRel), DistPrecisionType::FLOAT,
            TupleDescAttr(indexRel->rd_att, 0)->atttypmod > 0 ? (uint_fast16_t)TupleDescAttr(indexRel->rd_att, 0)->atttypmod : 0,
            worker_qt_type, run_build_index);

        disk_store.destroy();
        ctx.destroy();
        ReleaseBuffer(metabuf);
        MemoryContextSwitchTo(old_ctx);
        MemoryContextDelete(build_ctx);
    }, heapLockmode, indexLockmode);
}

BlockNumber build_graph_index(Relation heap, Relation index, IndexInfo *index_info,
    ForkNumber fork_num, double *reltuples, double *indtuples)
{
    int nparallel = graph_index_get_build_parallel(index);
    if (!heap) {
        nparallel = 0;
    }

    if (nparallel > 0) {
        if (heap->rd_rel->relpersistence == RELPERSISTENCE_TEMP) {
            ereport(NOTICE, (errmsg("switch off parallel mode for temp table")));
            nparallel = 0;
        }
    }

    MemoryContext build_ctx = AllocSetContextCreate(CurrentMemoryContext,
        "GRAPH_INDEX build context", ALLOCSET_DEFAULT_SIZES);
    MemoryContext old_ctx = MemoryContextSwitchTo(build_ctx);

    GraphIndexBuild build{index, nparallel, build_ctx, fork_num};
    BlockNumber result_blkno = build.build_index(heap, index, index_info);

    if (reltuples) {
        *reltuples = build.get_reltuples();
    }
    if (indtuples) {
        *indtuples = build.get_reltuples();
    }
    build.destroy();

    MemoryContextSwitchTo(old_ctx);
    MemoryContextDelete(build_ctx);

    return result_blkno;
}

IndexBuildResult *graph_index_build_internal(Relation heap, Relation index, IndexInfo *index_info)
{
    IndexBuildResult *result = (IndexBuildResult *)palloc(sizeof(IndexBuildResult));
    build_graph_index(heap, index, index_info, MAIN_FORKNUM, &result->heap_tuples, &result->index_tuples);
    return result;
}

void graph_index_buildempty_internal(Relation index)
{
    build_graph_index(NULL, index, NULL, INIT_FORKNUM, NULL, NULL);
}

uint16_t graph_index_get_dim(Relation index)
{
    int16 dim = TupleDescAttr(index->rd_att, 0)->atttypmod;
    return dim > 0 ? dim : 128;
}
