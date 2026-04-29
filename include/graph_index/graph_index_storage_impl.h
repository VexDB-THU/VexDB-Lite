/*
 * graph_index_storage_impl.h - Storage implementation header
 */

#ifndef GRAPH_INDEX_STORAGE_IMPL_H
#define GRAPH_INDEX_STORAGE_IMPL_H

#include "pg_compat.h"
#include "graph_index/graph_index_struct.h"
#include "graph_index/graph_index_point.h"

#include <atomic>
#include <functional>
#include <tuple>

using std::tuple;

/* Memory pool for vector storage */
class MemPool {
public:
    struct Chunk {
        char *buf;
        Chunk(char *b = nullptr) : buf(b) {}
    };
    
    uint32 elem_size_;
    uint32 pow_elem_nums_per_chunk_;
    uint32 one_chunk_elem_nums_;
    size_t chunk_size_;
    MemoryContext ctx_;
    Vector<Chunk> chunks_;
    slock_t mutex_;
    
    MemPool(size_t elem_size, size_t target_size_mb, MemoryContext ctx)
        : elem_size_((elem_size + 63) & ~63), /* Align to 64 */
          ctx_(ctx),
          chunks_(1024, ctx)
    {
        pow_elem_nums_per_chunk_ = 20; /* 1M elements per chunk */
        while ((1UL << pow_elem_nums_per_chunk_) * elem_size_ > (size_t)target_size_mb * 1024 * 1024 
               && pow_elem_nums_per_chunk_ > 10) {
            pow_elem_nums_per_chunk_--;
        }
        one_chunk_elem_nums_ = 1 << pow_elem_nums_per_chunk_;
        chunk_size_ = elem_size_ * one_chunk_elem_nums_;
        SpinLockInit(&mutex_);
    }
    
    void destroy()
    {
        for (size_t i = 0; i < chunks_.size(); i++) {
            if (chunks_[i].buf) {
                pfree(chunks_[i].buf);
            }
        }
        chunks_.destroy();
    }
    
    char *get(uint32 idx)
    {
        uint32 chunk_no = idx >> pow_elem_nums_per_chunk_;
        if (chunk_no >= chunks_.size() || !chunks_[chunk_no].buf) {
            return nullptr;
        }
        uint32 offset = (idx & (one_chunk_elem_nums_ - 1)) * elem_size_;
        return chunks_[chunk_no].buf + offset;
    }
    
    void extend(size_t idx)
    {
        uint32 chunk_no = idx >> pow_elem_nums_per_chunk_;
        SpinLockAcquire(&mutex_);
        while (chunk_no >= chunks_.size()) {
            MemoryContext oldctx = MemoryContextSwitchTo(ctx_);
            char *buf = (char *)palloc(chunk_size_);
            MemoryContextSwitchTo(oldctx);
            chunks_.push_back(Chunk(buf));
        }
        SpinLockRelease(&mutex_);
    }
};

/* Main memory storage implementation */
template <typename T, typename point_type>
class GraphIndexMemStoreImpl {
public:
    using elem_type = point_type;
    static constexpr bool clustered = false;
    static constexpr bool has_occlusion_cache = false;
    
    MemPool *base_pool_;
    MemPool *upper_pool_;
    LWLockPadded entry_lock_;
    
    Vector<point_type> elems_;
    std::atomic<T> num_vectors_{0};
    size_t max_size_;
    
    char **vectors_;
    size_t vec_size_ = 512; /* Size of each vector */
    uint16_t dim_ = 128;    /* Dimension */
    
    uint_fast16_t m_;
    int8 entry_level_;
    T entry_id_;
    T entry_cur_layer_idx_;
    
public:
    GraphIndexMemStoreImpl(uint_fast16_t m, uint_fast32_t max_size,
                           size_t base_target_mb, size_t upper_target_mb,
                           MemoryContext ctx);
    ~GraphIndexMemStoreImpl();
    
    void destroy();
    
    T assign_vector_id(bool is_base);
    void add_vector(T id, const char *vec);
    void add_elem(PointExtensionContext &ctx, T id, ItemPointer tid);
    void set_entrypoint(T id, T cur_layer_idx, int8 level);
    GraphIndexEntryInfo get_entrypoint() const;
    void get_itempointer(T id, std::function<void(const point_type *)> callback);
    char *get_vector_data(T id);
    
    template <typename Distancer>
    float get_distance(Distancer &distancer, const char *query, T id);
    
    template <bool is_base>
    tuple<T *, T, bool> get_point_info(T cur_layer_idx);
    
    template <bool exclusive, bool try_lock = false>
    void lock_point(T idx) {}
    
    template <bool exclusive, bool try_lock = false>
    void unlock_point(T idx) {}
    
    void release_entry_lock(bool shared_lock) {}
    
    template <bool exclusive, bool bottom_only = false>
    tuple<GraphIndexEntryInfo, bool> get_entry(int level = 0)
    {
        GraphIndexEntryInfo info;
        info.id = entry_id_;
        info.cur_layer_idx = entry_cur_layer_idx_;
        info.level = entry_level_;
        return {info, false};
    }
    
    Relation get_index() { return nullptr; }
    DistPrecisionType get_precision() { return DistPrecisionType::FLOAT; }
    uint16_t get_dim() { return dim_; }
    size_t get_vecsize() { return vec_size_; }
    size_t get_elemsize() { return vec_size_; }
    
    void apply_elem(T id, std::function<bool(point_type &)> func) {
        if (id < elems_.size()) {
            func(elems_[id]);
        }
    }
};

using GraphIndexMemStore = GraphIndexMemStoreImpl<uint32, GraphIndexPoint>;

extern "C" {
    void *create_mem_store(uint16_t m, uint32_t max_size, MemoryContext ctx);
    void destroy_mem_store(void *store);
}

#endif /* GRAPH_INDEX_STORAGE_IMPL_H */
