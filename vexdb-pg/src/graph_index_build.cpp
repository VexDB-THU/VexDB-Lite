/*
 * graph_index_build.cpp - Graph index build implementation
 * Adapted from openGauss for PostgreSQL
 */

#include <vtl/holder>
#include <vtl/disk_container/freespace.hpp>

#include "pg_compat.h"
#include "access/tableam.h"
#include "graph_index/graph_index.h"
#include "graph_index/graph_index_cluster.h"
#include "graph_index/graph_index_storage.h"
#include "graph_index/graph_index_algorithm.h"
#include "graph_index/graph_index_xlog.h"
#include "ann_utils.h"
#include "module/timer.h"
#include "distance/distance_dispatcher.h"
#include "floatvector.h"

#ifdef PG_VEXDB_ENABLE_LIBVEX_CORE_BRIDGE
#include "graph_index/core_node_store_bridge_runtime.hpp"
#endif

#include <chrono>
#include <cstring>
#include <memory>
#include <type_traits>

using namespace disk_container;
using namespace ann_helper;
using namespace rabitq;

extern int maintenance_work_mem;

static DistPrecisionType get_data_type(Relation index)
{
    Oid floatvector_oid = get_floatvector_oid();
    Oid halfvector_oid = get_halfvector_oid();
    Oid type_oid = TupleDescAttr(index->rd_att, 0)->atttypid;
    if (type_oid == floatvector_oid || type_oid == FLOAT4ARRAYOID) {
        return DistPrecisionType::FLOAT;
    } else if (type_oid == halfvector_oid) {
        return DistPrecisionType::HALF;
    } else {
        return DistPrecisionType::FLOAT;
    }
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
    return get_func_metric(procinfo->fn_oid);
}

class GraphIndexBuild {
public:
    GraphIndexBuild(Relation index, int nparallel, MemoryContext build_ctx, ForkNumber fork_num)
        : fork_num(fork_num),
          id_type(graph_index_get_id_type(index)),
          qt_type(graph_index_get_quantizer_type(index)),
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
        
        if (metric != Metric::CUSTOM) {
            if (need_norm) {
                norm_func_ptr = get_vector_preprocess_func(Metric::FAST_COSINE, precision_type, dimension);
            }
        } else {
            if (need_norm) {
                norminfo = graph_index_optional_proc_info(index, GRAPH_INDEX_NORM_PROC);
            }
        }

        concurrent_quant = true;
        elem_size = vector_size;

        /* Set memory - determine build state */
        constexpr int min_memory_required_kb = 1024 * 1024; /* 1GB */
        if (maintenance_work_mem_kb < min_memory_required_kb) {
            build_state = BuildState::DISK;
            create_vec_data(index, true);
            ereport(WARNING, (errmsg("maintenance_work_mem <= 1GB, will turn into disk build stage "
                                     "and take significantly more time.")));
            flush_warned = true;
        } else {
            build_state = BuildState::MEMORY;
            size_t neighbors_size = id_type == IdType::U32 ?
                m * 2 * (sizeof(uint32) + sizeof(float)) :
                m * 2 * (sizeof(size_t) + sizeof(float));
            size_t mempool_initsize_mb = maintenance_work_mem_kb / 1024 - 200;
            /* Avoid integer division issues */
            double ratio = (double)vector_size / (double)(vector_size + neighbors_size * 1.1);
            size_t vectorpool_initsize = (size_t)(mempool_initsize_mb * ratio);
            size_t pointpool_initsize = mempool_initsize_mb - vectorpool_initsize;
            mem_store.emplace(dimension, m, vector_size, vectorpool_initsize, pointpool_initsize, build_ctx);
            flush_warned = false;
        }

        /* Initialize flush lock */
        LWLockInitialize(&flush_lock, LWTRANCHE_EXTEND);
    }

    BlockNumber build_index(Relation heap, Relation index, IndexInfo *index_info)
    {
        ereport(NOTICE,
                (errmsg("PG graph build config"),
                 errdetail("dim=%u m=%u ef_construction=%u id_type=%d precision_type=%d quantizer_type=%d build_state=%d",
                           static_cast<unsigned>(dimension),
                           static_cast<unsigned>(m),
                           static_cast<unsigned>(ef_construction),
                           static_cast<int>(id_type),
                           static_cast<int>(precision_type),
                           static_cast<int>(qt_type),
                           static_cast<int>(build_state))));
        create_metapage(index);
        bool quant_trained = init_quantizer(heap, index);
        if (!quant_trained) {
            qt_type = QuantizerType::NONE;
        }
        build_graph(heap, index, index_info);
        log_index(index);
        return metablkno;
    }

    void create_metapage(Relation index)
    {
        metabuf = ReadBufferExtended(index, fork_num, P_NEW, RBM_NORMAL, NULL);
        LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
        metablkno = BufferGetBlockNumber(metabuf);
        BlockNumber ps_blkno = PlainStore::get_plain_store(index, false, fork_num);
        Assert(ps_blkno == GRAPH_INDEX_PS_BLKNO);
        
        Page metapage = BufferGetPage(metabuf);
        metablkno = BufferGetBlockNumber(metabuf);
        graph_index_init_page(metabuf, metapage);
        metap = GRAPH_INDEX_PAGE_GET_META(metapage);

        /* Constant settings */
        metap->magic_number = GRAPH_INDEX_MAGIC_NUMBER;
        metap->version = GRAPH_INDEX_VERSION;
        metap->dimension = dimension;
        metap->m = m;
        metap->ef_construction = ef_construction;
        metap->metric = metric;
        metap->precision_type = precision_type;
        
        /* Cluster */
        metap->num_cluster = 0;
        metap->cluster_block = InvalidBlockNumber;

        /* Allocate disk structures */
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

        /* Init quantizer */
        metap->quantizer_metainfo.init(qt_type, dimension);

        /* Mutable settings */
        metap->entry_level = -1;
        metap->entrypoint_id = INVALID_VECTOR_ID;
        metap->entry_cur_layer_idx = INVALID_VECTOR_ID;
        metap->num_vectors = 0;
        metap->vacuum_flag = false;

        ((PageHeader)metapage)->pd_lower = ((char *)metap + sizeof(GraphIndexMetaPageData)) - (char *)metapage;
        MarkBufferDirty(metabuf);
        LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
    }

    void log_index(Relation index)
    {
        LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
        GraphIndexXlog xlog;
        xlog.init(index, metabuf, BufferGetPage(metabuf));
        xlog.log_build_index(fork_num);
        LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
        /* WAL logging for vectors deferred */
    }

    void destroy() { ReleaseBuffer(metabuf); }

    double get_reltuples() const { return reltuples; }

private:
    /* Build callback data base */
    struct BuildCallbackDataBase {
        GraphIndexBuild *build;
        Relation heap;
        Timer *timer;
        Buffer own_metabuf;
        DiskStoreVariant disk_store;
        PointExtensionContext ctx;

        BuildCallbackDataBase(GraphIndexBuild *reference, Relation index, Relation heap,
            Timer *timer, BlockNumber metablkno)
            : build(reference),
              heap(heap),
              timer(timer),
              own_metabuf(ReadBuffer(index, metablkno)),
              ctx(index, GRAPH_INDEX_PS_BLKNO, false)
        { 
            create_disk_store(disk_store, index, heap, own_metabuf, NULL, false); 
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

        BuildCallbackData(GraphIndexBuild *reference, Relation index, Relation heap, Timer *timer,
                          D1 &d1, D2 &d2, BlockNumber metablkno)
            : BuildCallbackDataBase(reference, index, heap, timer, metablkno),
              mem_distancer(d1),
              disk_distancer(d2) {}
    };

#ifdef PG_VEXDB_ENABLE_LIBVEX_CORE_BRIDGE
    template <typename Store>
    struct CoreBuildCallbackData {
        GraphIndexBuild *build;
        Relation heap;
        Relation index;
        Timer *timer;
        Buffer own_metabuf;
        DiskStoreVariant disk_store;
        PointExtensionContext ctx;
        Store *store = nullptr;
        pgvexdb::CoreBridgeBuildRuntime<Store> runtime;

        CoreBuildCallbackData(GraphIndexBuild *reference, Relation index_rel, Relation heap_rel,
                              Timer *build_timer, BlockNumber metablkno)
            : build(reference),
              heap(heap_rel),
              index(index_rel),
              timer(build_timer),
              own_metabuf(ReadBuffer(index_rel, metablkno)),
              ctx(index_rel, GRAPH_INDEX_PS_BLKNO, false)
        {
            create_disk_store(disk_store, index_rel, heap_rel, own_metabuf, NULL, false);
            store = &disk_store.template get<Store>();
        }

        void init_core()
        {
            if (!runtime.Init(*store, ctx, index, build->fork_num, build->metablkno,
                              build->dimension, build->m, build->ef_construction, build->metric)) {
                ereport(ERROR, (errmsg("PG core bridge build: failed to initialize runtime")));
            }
        }

        void destroy()
        {
            runtime.Reset();
            disk_store.destroy();
            ctx.destroy();
            ReleaseBuffer(own_metabuf);
        }
    };
#endif

    /* Info */
    ForkNumber fork_num;
    BlockNumber metablkno;
    Buffer metabuf;

    /* Settings */
    GraphIndexMetaPage metap;
    bool need_norm;
    IdType id_type;
    QuantizerType qt_type;
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

    /* Statistics */
    double reltuples{0};

    /* Support functions */
    Metric metric;
    Oid collation;
    union {
        vector_preprocess_func norm_func_ptr;
        FmgrInfo *norminfo;
    };

    /* Memory */
    MemoryContext build_ctx;

    /* Build state manager */
    enum class BuildState {
        MEMORY,
        DISK
    };
    BuildState build_state{BuildState::MEMORY};

    /* Default U32, impossible bigger than U32 in memory build */
    Holder<MemStore<>> mem_store;

    /* Flush manager */
    LWLock flush_lock;
    bool flush_warned;

    bool init_quantizer(Relation heap, Relation index)
    {
        if (qt_type == QuantizerType::NONE) {
            elem_size = vector_size;
            return false;
        }

        if (qt_type != QuantizerType::RABITQ) {
            ereport(WARNING,
                (errmsg("requested quantizer is not available in current PG build"),
                 errdetail("quantizer_type=%d", static_cast<int>(qt_type))));
            elem_size = vector_size;
            return false;
        }

        const size_t sample_target = std::max<size_t>(GRAPH_INDEX_MIN_QT_SAMPLES_SIZE,
                                                      GRAPH_INDEX_RABITQ_NUM_CLUSTERS * 32);
        FloatVectorArray samples = graph_index_quantizer_sample_data(heap, index, dimension, need_norm,
                                                                     precision_type, parallel_workers,
                                                                     sample_target);
        const int sample_count = samples ? samples->length : 0;
        if (samples == nullptr || sample_count < GRAPH_INDEX_RABITQ_NUM_CLUSTERS) {
            if (samples != nullptr) {
                FloatVectorArrayFree(samples);
            }
            ereport(WARNING,
                (errmsg("insufficient samples for RaBitQ training"),
                 errdetail("sample_count=%d required=%d",
                           sample_count,
                           GRAPH_INDEX_RABITQ_NUM_CLUSTERS)));
            elem_size = vector_size;
            return false;
        }

        bool trained = DispatchRunner<true,
            MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::FAST_COSINE>,
            DistPrecisionTypeList<DistPrecisionType::FLOAT, DistPrecisionType::HALF>,
            DispatcherMode::DEFAULT>::call(
            metric, precision_type, dimension, qt_type,
            [&](auto &distancer) {
                using Distancer = std::decay_t<decltype(distancer)>;
                if constexpr (std::is_same_v<Distancer, RabitqDistancer>) {
                    distancer.train(index, samples, dimension, metric, need_norm,
                                    parallel_workers, maintenance_work_mem_kb);
                    elem_size = distancer.code_size();
                    distancer.flush(index, qtcode_block, false);
                    Variant<PQDistancer, RabitqDistancer> trained_quantizer;
                    trained_quantizer.emplace<RabitqDistancer>(std::move(distancer));
                    quantizer.emplace(std::move(trained_quantizer));
                    metap->quantizer_metainfo.get_rabitq_meta().quant_size = static_cast<int>(elem_size);
                    metap->quantizer_metainfo.get_rabitq_meta().query_rescaling_factor =
                        quantizer->template get<RabitqDistancer>().get_query_rescaling_factor();
                    metap->quantizer_metainfo.centroids_version = 1;
                    metap->quantizer_metainfo.code_version = 1;
                    metap->quantizer_metainfo.set_enable();
                    return true;
                } else {
                    return false;
                }
            });
        FloatVectorArrayFree(samples);
        if (!trained) {
            elem_size = vector_size;
        }
        return trained;
    }

    template <typename D>
    void insert_in_memory(BuildCallbackDataBase &data, D &d, const char *query, ItemPointer tid)
    {
        GraphIndexAlgorithm algo{ef_construction, m, *mem_store, d};
        typename decltype(algo)::InsertContext ctx{data.ctx, query, tid};
        algo.insert(ctx);
        ctx.destroy();
    }

    template <typename D>
    void insert_on_disk(BuildCallbackDataBase &data, D &d, const char *query, ItemPointer tid)
    {
        d.process(query);
        if (id_type == IdType::U32) {
            auto &ds = data.disk_store.template get<DiskStore<uint32, false>>();
            GraphIndexAlgorithm algo{ef_construction, m, ds, d};
            typename decltype(algo)::InsertContext ctx{data.ctx, query, tid};
            algo.insert(ctx);
            ctx.destroy();
        } else {
            auto &ds = data.disk_store.template get<DiskStore<size_t, false>>();
            GraphIndexAlgorithm algo{ef_construction, m, ds, d};
            typename decltype(algo)::InsertContext ctx{data.ctx, query, tid};
            algo.insert(ctx);
            ctx.destroy();
        }
    }

    bool out_of_memory()
    {
        /* Simple memory check using memory context totals */
        Size total_space = MemoryContextMemAllocated(build_ctx, true);
        int64 avail_size = (int64)maintenance_work_mem_kb * 1024;
        return total_space >= (Size)avail_size;
    }

    void warning_oom()
    {
        flush_warned = true;
        ereport(WARNING,
            (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
             errmsg("graph_index graph no longer fits into maintenance_work_mem after "
                    "%u tuples", mem_store->get_vector_num()),
             errdetail("Building will take significantly more time."),
             errhint("Increase maintenance_work_mem to speed up builds.")));
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

    template <typename D1, typename D2>
    static void build_callback(Relation index, ItemPointer tid, Datum *values, bool *isnull,
                               bool tupleIsAlive, void *state)
    {
        if (isnull[0] || !tupleIsAlive) {
            return;
        }

        auto &data = *(BuildCallbackData<D1, D2> *)state;
        GraphIndexBuild &build = *(GraphIndexBuild *)data.build;
        
        data.timer->inc_loop_count_forground_report("Graph Build");

        Pointer vec_p;
        auto [query, is_alloc] = build.read_vec(vec_p, values);

        auto prepare_quantizer_disk_build = [&]() {
            GraphIndexMetaPage metap = GRAPH_INDEX_PAGE_GET_META(BufferGetPage(data.own_metabuf));
            if (metap->quantizer_metainfo.get_type() != QuantizerType::NONE) {
                data.disk_distancer.prepare(index, metap);
            }
        };

        /* Ensure graph not flushed when inserting */
        if (build.build_state == BuildState::DISK) {
            prepare_quantizer_disk_build();
            build.insert_on_disk(data, data.disk_distancer, query, tid);
            build.free_vec(vec_p, values, query, is_alloc);
            return;
        }
        
        LWLockAcquire(&build.flush_lock, LW_SHARED);
        if (build.build_state == BuildState::DISK) {
            LWLockRelease(&build.flush_lock);
            prepare_quantizer_disk_build();
            build.insert_on_disk(data, data.disk_distancer, query, tid);
            build.free_vec(vec_p, values, query, is_alloc);
            return;
        }

        if (build.out_of_memory()) {
            LWLockRelease(&build.flush_lock);
            LWLockAcquire(&build.flush_lock, LW_EXCLUSIVE);
            if (build.build_state == BuildState::MEMORY) {
                build.warning_oom();
                build.concurrent_quant = false;
                build.flush(index);
                build.mem_store->destroy();
            }
            LWLockRelease(&build.flush_lock);
            prepare_quantizer_disk_build();
            build.insert_on_disk(data, data.disk_distancer, query, tid);
            build.free_vec(vec_p, values, query, is_alloc);
            return;
        }

        build.insert_in_memory(data, data.mem_distancer, query, tid);
        build.free_vec(vec_p, values, query, is_alloc);
        LWLockRelease(&build.flush_lock);
    }

#ifdef PG_VEXDB_ENABLE_LIBVEX_CORE_BRIDGE
    template <typename Store>
    static void core_build_callback(Relation index, ItemPointer tid, Datum *values, bool *isnull,
                                    bool tupleIsAlive, void *state)
    {
        if (isnull[0] || !tupleIsAlive) {
            return;
        }

        auto &data = *(CoreBuildCallbackData<Store> *)state;
        GraphIndexBuild &build = *data.build;

        data.timer->inc_loop_count_forground_report("Graph Build");

        Pointer vec_p;
        auto [query, is_alloc] = build.read_vec(vec_p, values);
        data.runtime.AddPoint(query, *tid, build.dimension);

        build.free_vec(vec_p, values, query, is_alloc);
    }

    template <typename Store>
    bool build_single_thread_core_with_store(Relation heap, Relation index, IndexInfo *index_info)
    {
        if constexpr (std::decay_t<Store>::clustered) {
            return false;
        } else {
            CoreBuildCallbackData<Store> data{this, index, heap, timer, metablkno};
            auto init_start = std::chrono::high_resolution_clock::now();
            data.init_core();
            double init_ms = std::chrono::duration<double, std::milli>(
                std::chrono::high_resolution_clock::now() - init_start).count();
            ereport(NOTICE,
                    (errmsg("PG core bridge build init finished"),
                     errdetail("init_ms=%.3f", init_ms)));

            auto scan_start = std::chrono::high_resolution_clock::now();
            reltuples = table_index_build_scan(heap, index, index_info, true, false,
                                               core_build_callback<Store>, (void *)&data, NULL);
            double scan_ms = std::chrono::duration<double, std::milli>(
                std::chrono::high_resolution_clock::now() - scan_start).count();
            ereport(NOTICE,
                    (errmsg("PG core bridge table_index_build_scan finished"),
                     errdetail("scan_ms=%.3f reltuples=%.0f", scan_ms, reltuples)));

            auto store_start = std::chrono::high_resolution_clock::now();
            bool stored = data.runtime.StoreGraphState();
            double store_ms = std::chrono::duration<double, std::milli>(
                std::chrono::high_resolution_clock::now() - store_start).count();
            ereport(NOTICE,
                    (errmsg("PG core bridge StoreGraphState finished"),
                     errdetail("outer_store_ms=%.3f inner_store_ms=%.3f",
                               store_ms,
                               data.runtime.store_graph_state_ms)));
            data.runtime.ReportBuildStats();
            data.destroy();
            if (!stored) {
                ereport(ERROR, (errmsg("PG core bridge build: failed to persist graph state")));
            }
            return true;
        }
    }

    bool try_build_single_thread_core(Relation heap, Relation index, IndexInfo *index_info)
    {
        if (!pgvexdb::CanUseCoreBridgeBuild(id_type, precision_type, qt_type, metric)) {
            ereport(NOTICE,
                    (errmsg("PG core bridge build disabled: unsupported build config"),
                     errdetail("id_type=%d precision_type=%d quantizer_type=%d metric=%d",
                               static_cast<int>(id_type),
                               static_cast<int>(precision_type),
                               static_cast<int>(qt_type),
                               static_cast<int>(metric))));
            return false;
        }

        if (build_state == BuildState::MEMORY) {
            create_vec_data(index, true);
        }
        build_state = BuildState::DISK;

        ereport(NOTICE,
                (errmsg("PG core bridge build start"),
                 errdetail("dim=%u m=%u ef_construction=%u metric=%d",
                           static_cast<unsigned>(dimension),
                           static_cast<unsigned>(m),
                           static_cast<unsigned>(ef_construction),
                           static_cast<int>(metric))));
        return build_single_thread_core_with_store<DiskStore<uint32, false>>(heap, index, index_info);
    }
#endif

    void build_single_thread(Relation heap, Relation index, IndexInfo *index_info)
    {
        auto run_build_index = [&](auto &d1, auto &d2) {
            using D1 = std::decay_t<decltype(d1)>;
            using D2 = std::decay_t<decltype(d2)>;
            BuildCallbackData<D1, D2> data{this, index, heap, timer, d1, d2, metablkno};
            
            /* Use proper PostgreSQL index build scan API */
            reltuples = table_index_build_scan(heap, index, index_info, true, false,
                                               build_callback<D1, D2>, (void *)&data, NULL);
            
            data.destroy();
        };
        
        bool use_cluster = false;
        DispatchRunner<true,
            MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::FAST_COSINE>,
            DistPrecisionTypeList<
                DistPrecisionType::FLOAT,
                DistPrecisionType::HALF
            >, DispatcherMode::BUILD_PAIR>::call(
            metric, precision_type, dimension, qt_type, run_build_index);
    }

    void build_graph(Relation heap, Relation index, IndexInfo *index_info)
    {
        if (heap == NULL) {
            return;
        }
        
        Timer local_timer{0, 500'000, "", ""};
        local_timer.set_stage("Graph Build");
        local_timer.report("Start Graph Build");
        timer = &local_timer;

        /* For now, use single-threaded build */
        /* Parallel build can be added later using CreateParallelContext() */
        if (parallel_workers > 0) {
            ereport(NOTICE, (errmsg("parallel build not yet implemented, using single-threaded")));
        }
        
#ifdef PG_VEXDB_ENABLE_LIBVEX_CORE_BRIDGE
        const bool had_memory_store = build_state == BuildState::MEMORY;
        if (try_build_single_thread_core(heap, index, index_info)) {
            local_timer.report("PG core bridge build finished");
            local_timer.report("Graph Build Finished");
            local_timer.destroy();
            timer = NULL;
            if (had_memory_store) {
                mem_store->destroy();
            }
            return;
        }
        ereport(NOTICE, (errmsg("PG build fallback to legacy graph build path")));
#endif

        build_single_thread(heap, index, index_info);

        local_timer.report("Graph Build Finished");
        local_timer.destroy();
        timer = NULL;

        if (build_state == BuildState::MEMORY) {
            flush(index);
            mem_store->destroy();
        } else if (!flush_warned) {
            mem_store->destroy();
        }
    }

    template <typename T>
    void flush_graph(Relation index)
    {
        GraphIndexMetaPage metap = GRAPH_INDEX_PAGE_GET_META(BufferGetPage(metabuf));
        Timer flush_timer{0, 500'000, "", ""};
        flush_timer.set_stage("Flush Graph Index");

        build_state = BuildState::DISK;
        MemStore<> &store = *mem_store;
        GraphIndexEntryInfo entry_info = store.entry_info;

        metap->entry_level = entry_info.level;
        metap->entrypoint_id = entry_info.id;
        metap->entry_cur_layer_idx = entry_info.cur_layer_idx;
        metap->num_vectors = (size_t)store.get_vector_num();

        if (metap->num_vectors > 0) {
            /* Flush elems */
            flush_timer.report("Flushing Elems");
            store.flush_points(index, metap->elems_block);

            /* Flush basepoint */
            flush_timer.report("Flushing Basepoint");
            constexpr size_t copybuf_size = 10 * 1024 * 1024; /* 10MB */
            T *copybuf = (T *)palloc(copybuf_size);
            size_t basepoint_size = sizeof(T) * m * 2;
            VarDiskVector<GraphIndexDiskBasePoint<T>> base_layer{index, metap->base_block, false, basepoint_size};
            auto &basepoint_pool = store.basepoint_pool;
            size_t copy_num = copybuf_size / basepoint_size;
            uint32 num_vectors = store.get_vector_num();
            
            for (uint32 i = 0; i <= num_vectors / copy_num; ++i) {
                size_t batch_offset = i * copy_num;
                size_t actual_copy_num = Min(copy_num, num_vectors - batch_offset);
                if (actual_copy_num == 0) {
                    break;
                }
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

            /* Flush upperpoint */
            flush_timer.report("Flushing Upperpoint");
            size_t upperpoint_size = (m + 1) * 2 * sizeof(T);
            copy_num = copybuf_size / upperpoint_size;
            auto &upperpoint_pool = store.upperpoint_pool;
            VarDiskVector<GraphIndexDiskUpperPoint<T>> upper_layer{index, metap->upper_block, false, upperpoint_size};
            num_vectors = store.get_upper_num();
            
            for (size_t i = 0; i <= num_vectors / copy_num; ++i) {
                size_t batch_offset = i * copy_num;
                size_t actual_copy_num = Min(copy_num, num_vectors - batch_offset);
                if (actual_copy_num == 0) {
                    break;
                }
                for (size_t j = 0; j < actual_copy_num; ++j) {
                    size_t vec_idx = batch_offset + j;
                    uint32 *neighbors_info = (uint32 *)upperpoint_pool.get(vec_idx);
                    size_t offset = j * (2 + m * 2);
                    copybuf[offset] = neighbors_info[m * 2]; /* lower_layer_idx */
                    copybuf[offset + 1] = neighbors_info[m * 2 + 1]; /* id */
                    for (size_t k = 0; k < m * 2; ++k) {
                        copybuf[offset + 2 + k] = neighbors_info[k];
                    }
                }
                upper_layer.push_back_n((const GraphIndexDiskUpperPoint<T> *)copybuf, actual_copy_num);
            }
            upper_layer.destroy();
            pfree(copybuf);
        }

        /* Flush vector data */
        flush_timer.report("Flushing Vector");
        create_vec_data(index, true);
        auto &vector_pool = store.vector_pool;
        auto &vec = vector_pool.vec;
        uint32 num_vectors = store.get_vector_num();

        const bool use_rabitq_codes =
            qt_type == QuantizerType::RABITQ && quantizer.has_value() &&
            metap->quantizer_metainfo.get_type() == QuantizerType::RABITQ;
        if (!use_rabitq_codes) {
            for (size_t i = 0; i < vec.size(); ++i) {
                size_t batch_offset = i * vector_pool.one_chunk_elem_nums;
                size_t actual_copy_num = Min(vector_pool.one_chunk_elem_nums, num_vectors - batch_offset);
                if (actual_copy_num == 0) {
                    break;
                }
                off_t offset = batch_offset * vector_size;
                int nbytes = actual_copy_num * vector_size;
                vec_write(index->rd_smgr, offset, nbytes, vec[i].buf, false, storage_type);
            }
        } else {
            auto &distancer = quantizer->template get<RabitqDistancer>();
            std::unique_ptr<char[]> code_buf(new char[static_cast<size_t>(vector_pool.one_chunk_elem_nums) * elem_size]);
            for (size_t i = 0; i < vec.size(); ++i) {
                size_t batch_offset = i * vector_pool.one_chunk_elem_nums;
                size_t actual_copy_num = Min(vector_pool.one_chunk_elem_nums, num_vectors - batch_offset);
                if (actual_copy_num == 0) {
                    break;
                }
                char *src = vec[i].buf;
                for (size_t j = 0; j < actual_copy_num; ++j) {
                    distancer.compute_code(reinterpret_cast<float *>(src + j * vector_size),
                                           code_buf.get() + j * elem_size);
                }
                off_t offset = static_cast<off_t>(batch_offset) * elem_size;
                int nbytes = static_cast<int>(actual_copy_num * elem_size);
                vec_write(index->rd_smgr, offset, nbytes, code_buf.get(), false, VecStorageType::PureCode);
            }
        }

        flush_timer.report("Flush Finished");
        LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
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

    Timer *timer;
};

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
