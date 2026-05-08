#include "vex_graph_index.hpp"

#include "vex/vex_disk_block_store.hpp"
#include "vex/vex_disk_layout.hpp"

#include "duckdb/common/types/value.hpp"
#include "duckdb/storage/table_io_manager.hpp"
#include "duckdb/storage/partial_block_manager.hpp"

#include <cstring>
#include <stdexcept>

namespace duckdb {

namespace {

template <class T>
static void AppendBytes(std::string &out, const T &value) {
    auto old_size = out.size();
    out.resize(old_size + sizeof(T));
    std::memcpy(out.data() + old_size, &value, sizeof(T));
}

static void AppendRaw(std::string &out, const void *ptr, size_t len) {
    auto old_size = out.size();
    out.resize(old_size + len);
    std::memcpy(out.data() + old_size, ptr, len);
}

template <class T>
static T ReadBytes(const std::string &blob, size_t &offset) {
    if (offset + sizeof(T) > blob.size()) {
        throw InvalidInputException("GRAPH_INDEX disk image truncated");
    }
    T value;
    std::memcpy(&value, blob.data() + offset, sizeof(T));
    offset += sizeof(T);
    return value;
}

static void ReadRaw(const std::string &blob, size_t &offset, void *ptr, size_t len) {
    if (offset + len > blob.size()) {
        throw InvalidInputException("GRAPH_INDEX disk image truncated");
    }
    std::memcpy(ptr, blob.data() + offset, len);
    offset += len;
}

} // namespace

std::string GraphIndex::BuildDiskImage() const {
    using namespace vex_disk;

    if (!runtime_) {
        return {};
    }

    std::string meta_blob;
    std::string base_blob;
    std::string upper_blob;
    std::string vec_blob;
    std::string rowid_blob;

    const auto &store = runtime_->store;

    DiskMeta meta;
    meta.dimension = uint32_t(dimension_);
    meta.m = uint32_t(m_);
    meta.ef_construction = uint32_t(ef_construction_);
    meta.metric = uint32_t(metric_);
    meta.vector_count = store.elems.size();
    meta.upper_count = store.upper_points.size();
    meta.entry_id = store.entry_info.id;
    meta.entry_cur_layer_idx = store.entry_info.cur_layer_idx;
    meta.entry_level = store.entry_info.level;
    AppendBytes(meta_blob, meta);

    for (idx_t i = 0; i < store.base_points.size(); i++) {
        auto &bp = store.base_points[i];
        vex_disk::DiskBaseRecordHeader hdr;
        hdr.neighbor_count = uint32_t(bp.neighbors.size());
        AppendBytes(base_blob, hdr);
        if (!bp.neighbors.empty()) {
            AppendRaw(base_blob, bp.neighbors.data(), bp.neighbors.size() * sizeof(bp.neighbors[0]));
        }
        if (!bp.dists.empty()) {
            AppendRaw(base_blob, bp.dists.data(), bp.dists.size() * sizeof(bp.dists[0]));
        }
    }

    for (idx_t i = 0; i < store.upper_points.size(); i++) {
        auto &up = store.upper_points[i];
        vex_disk::DiskUpperRecordHeader hdr;
        hdr.neighbor_count = uint32_t(m_);
        hdr.lower_layer_idx = uint32_t(up.lower_layer_idx);
        hdr.id = uint32_t(up.id);
        AppendBytes(upper_blob, hdr);
        if (!up.neighbors_info.empty()) {
            AppendRaw(upper_blob, up.neighbors_info.data(), up.neighbors_info.size() * sizeof(up.neighbors_info[0]));
        }
        if (!up.dists.empty()) {
            AppendRaw(upper_blob, up.dists.data(), up.dists.size() * sizeof(up.dists[0]));
        }
    }

    for (idx_t i = 0; i < store.vectors.size(); i++) {
        auto &vec = store.vectors[i];
        if (!vec.empty()) {
            AppendRaw(vec_blob, vec.data(), vec.size());
        }
    }

    for (idx_t i = 0; i < store.elems.size(); i++) {
        auto &elem = store.elems[i];
        vex_disk::DiskRowIdRecordHeader hdr;
        hdr.row_count = uint32_t(elem.tids.size());
        AppendBytes(rowid_blob, hdr);
        if (!elem.tids.empty()) {
            AppendRaw(rowid_blob, elem.tids.data(), elem.tids.size() * sizeof(elem.tids[0]));
        }
    }

    DiskImageHeader header;
    size_t offset = sizeof(DiskImageHeader);
    header.meta_offset = offset;
    header.meta_size = meta_blob.size();
    offset += meta_blob.size();
    header.base_offset = offset;
    header.base_size = base_blob.size();
    offset += base_blob.size();
    header.upper_offset = offset;
    header.upper_size = upper_blob.size();
    offset += upper_blob.size();
    header.vec_offset = offset;
    header.vec_size = vec_blob.size();
    offset += vec_blob.size();
    header.rowid_offset = offset;
    header.rowid_size = rowid_blob.size();
    offset += rowid_blob.size();
    header.total_size = offset;

    std::string out;
    out.reserve(header.total_size);
    AppendBytes(out, header);
    AppendRaw(out, meta_blob.data(), meta_blob.size());
    AppendRaw(out, base_blob.data(), base_blob.size());
    AppendRaw(out, upper_blob.data(), upper_blob.size());
    AppendRaw(out, vec_blob.data(), vec_blob.size());
    AppendRaw(out, rowid_blob.data(), rowid_blob.size());
    return out;
}

void GraphIndex::LoadFromDiskImage(const std::string &blob) {
    using namespace vex_disk;

    if (blob.empty()) {
        return;
    }

    size_t offset = 0;
    auto header = ReadBytes<DiskImageHeader>(blob, offset);
    if (header.magic != GRAPH_INDEX_DISK_MAGIC) {
        throw InvalidInputException("GRAPH_INDEX disk image magic mismatch");
    }
    if (header.version != GRAPH_INDEX_DISK_VERSION) {
        throw InvalidInputException("GRAPH_INDEX disk image version mismatch");
    }

    auto meta = ReadBytes<DiskMeta>(blob, offset);

    dimension_ = meta.dimension;
    m_ = int(meta.m);
    ef_construction_ = int(meta.ef_construction);
    metric_ = VexMetric(meta.metric);
    runtime_ = make_uniq<GraphIndexRuntimeState>(dimension_, m_);
    auto &store = runtime_->store;
    store.entry_info.set(meta.entry_id, meta.entry_cur_layer_idx, meta.entry_level);

    for (idx_t i = 0; i < idx_t(meta.vector_count); i++) {
        auto hdr = ReadBytes<DiskBaseRecordHeader>(blob, offset);
        auto id = store.template assign_vector_id<true>();
        (void)id;
        auto &bp = store.base_points.back();
        bp.neighbors.resize(hdr.neighbor_count);
        bp.dists.resize(hdr.neighbor_count);
        if (hdr.neighbor_count > 0) {
            ReadRaw(blob, offset, bp.neighbors.data(), hdr.neighbor_count * sizeof(bp.neighbors[0]));
            ReadRaw(blob, offset, bp.dists.data(), hdr.neighbor_count * sizeof(bp.dists[0]));
        }
    }

    for (idx_t i = 0; i < idx_t(meta.upper_count); i++) {
        auto hdr = ReadBytes<DiskUpperRecordHeader>(blob, offset);
        auto idx = store.template assign_vector_id<false>();
        (void)idx;
        auto &up = store.upper_points.back();
        up.lower_layer_idx = hdr.lower_layer_idx;
        up.id = hdr.id;
        up.neighbors_info.resize(hdr.neighbor_count * 2);
        up.dists.resize(hdr.neighbor_count);
        if (hdr.neighbor_count > 0) {
            ReadRaw(blob, offset, up.neighbors_info.data(), up.neighbors_info.size() * sizeof(up.neighbors_info[0]));
            ReadRaw(blob, offset, up.dists.data(), up.dists.size() * sizeof(up.dists[0]));
        }
    }

    for (idx_t i = 0; i < idx_t(meta.vector_count); i++) {
        store.vectors[i].resize(dimension_ * sizeof(float));
        ReadRaw(blob, offset, store.vectors[i].data(), store.vectors[i].size());
    }

    for (idx_t i = 0; i < idx_t(meta.vector_count); i++) {
        auto hdr = ReadBytes<DiskRowIdRecordHeader>(blob, offset);
        auto &elem = store.elems[i];
        elem.tids.resize(hdr.row_count);
        if (hdr.row_count > 0) {
            ReadRaw(blob, offset, elem.tids.data(), hdr.row_count * sizeof(elem.tids[0]));
        }
    }
}

IndexStorageInfo GraphIndex::ExportStorageInfo() const {
    IndexStorageInfo info(name);

    if (!runtime_) {
        return info;
    }

    auto &store = runtime_->store;

    info.options["dimension"] = Value::UBIGINT(uint64_t(dimension_));
    info.options["m"] = Value::INTEGER(m_);
    info.options["ef_construction"] = Value::INTEGER(ef_construction_);
    info.options["metric"] = Value("l2");
    info.options["node_count"] = Value::UBIGINT(uint64_t(store.elems.size()));
    info.options["upper_count"] = Value::UBIGINT(uint64_t(store.upper_points.size()));
    info.options["entry_id"] = Value::UBIGINT(uint64_t(store.entry_info.id));
    info.options["entry_cur_layer_idx"] = Value::UBIGINT(uint64_t(store.entry_info.cur_layer_idx));
    info.options["entry_level"] = Value::INTEGER(int(store.entry_info.level));

    if (store.node_alloc_) {
        info.allocator_infos.push_back(store.node_alloc_->GetInfo());
    }
    if (store.vector_alloc_) {
        info.allocator_infos.push_back(store.vector_alloc_->GetInfo());
    }
    if (store.upper_alloc_) {
        info.allocator_infos.push_back(store.upper_alloc_->GetInfo());
    }

    if (!store.id_to_node_ptr_.empty()) {
        string id_ptr_blob;
        uint64_t num_entries = store.id_to_node_ptr_.size();
        id_ptr_blob.append(reinterpret_cast<const char *>(&num_entries), sizeof(num_entries));
        for (auto &ptr : store.id_to_node_ptr_) {
            uint64_t ptr_val = ptr.Get();
            id_ptr_blob.append(reinterpret_cast<const char *>(&ptr_val), sizeof(ptr_val));
        }
        info.options["id_ptr_map"] = Value::BLOB(const_data_ptr_cast(id_ptr_blob.data()), id_ptr_blob.size());
    }

    if (!deleted_rids_.empty()) {
        string deleted_blob;
        uint64_t num_deleted = deleted_rids_.size();
        deleted_blob.append(reinterpret_cast<const char *>(&num_deleted), sizeof(num_deleted));
        for (auto &rid : deleted_rids_) {
            int64_t rid_val = static_cast<int64_t>(rid);
            deleted_blob.append(reinterpret_cast<const char *>(&rid_val), sizeof(rid_val));
        }
        info.options["deleted_rids"] = Value::BLOB(const_data_ptr_cast(deleted_blob.data()), deleted_blob.size());
    }

    if (!store.upper_idx_to_ptr_.empty()) {
        string upper_ptr_blob;
        uint64_t num_entries = store.upper_idx_to_ptr_.size();
        upper_ptr_blob.append(reinterpret_cast<const char *>(&num_entries), sizeof(num_entries));
        for (auto &ptr : store.upper_idx_to_ptr_) {
            uint64_t ptr_val = ptr.Get();
            upper_ptr_blob.append(reinterpret_cast<const char *>(&ptr_val), sizeof(ptr_val));
        }
        info.options["upper_ptr_map"] = Value::BLOB(const_data_ptr_cast(upper_ptr_blob.data()), upper_ptr_blob.size());
    }

    // Persist UpperPointRec fields. HNSWUpperLevel's segment only stores
    // neighbor IDs per slot — cur_layer_idxs (the second half of neighbors_info)
    // live exclusively in the in-memory copy and are needed for upper-layer
    // descent, so we write them out alongside lower_layer_idx and id.
    if (!store.upper_points.empty()) {
        string upper_data_blob;
        uint64_t num_entries = store.upper_points.size();
        upper_data_blob.append(reinterpret_cast<const char *>(&num_entries), sizeof(num_entries));
        for (auto &up : store.upper_points) {
            uint32_t id_val = static_cast<uint32_t>(up.id);
            uint32_t lower_val = static_cast<uint32_t>(up.lower_layer_idx);
            upper_data_blob.append(reinterpret_cast<const char *>(&id_val), sizeof(id_val));
            upper_data_blob.append(reinterpret_cast<const char *>(&lower_val), sizeof(lower_val));
            uint64_t nbr_size = up.neighbors_info.size();
            upper_data_blob.append(reinterpret_cast<const char *>(&nbr_size), sizeof(nbr_size));
            if (nbr_size) {
                upper_data_blob.append(reinterpret_cast<const char *>(up.neighbors_info.data()),
                                       nbr_size * sizeof(uint32_t));
            }
        }
        info.options["upper_points_data"] = Value::BLOB(const_data_ptr_cast(upper_data_blob.data()),
                                                        upper_data_blob.size());
    }

    if (pq_use_) {
        info.options["pq_m"] = Value::UINTEGER(pq_quantizer_.m);
        info.options["pq_dim"] = Value::UINTEGER(pq_quantizer_.d);

        // Codebook: m * KSUB * dsub floats. Pin layout in case future versions
        // change the in-memory shape — write dimensions inline so reload doesn't
        // have to reconstruct from pq_m alone.
        string codebook_blob;
        uint64_t centroids_n = pq_quantizer_.centroids.size();
        codebook_blob.append(reinterpret_cast<const char *>(&centroids_n), sizeof(centroids_n));
        codebook_blob.append(reinterpret_cast<const char *>(pq_quantizer_.centroids.data()),
                             centroids_n * sizeof(float));
        info.options["pq_codebook"] = Value::BLOB(const_data_ptr_cast(codebook_blob.data()),
                                                  codebook_blob.size());

        // Codes are indexed by row_id sort order; persist the order alongside the
        // bytes so reload can reconstruct the row → code-slot mapping.
        string codes_blob;
        uint64_t codes_n = pq_codes_.size();
        codes_blob.append(reinterpret_cast<const char *>(&codes_n), sizeof(codes_n));
        codes_blob.append(reinterpret_cast<const char *>(pq_codes_.data()), codes_n);
        info.options["pq_codes"] = Value::BLOB(const_data_ptr_cast(codes_blob.data()), codes_blob.size());

        string order_blob;
        uint64_t order_n = pq_row_id_order_.size();
        order_blob.append(reinterpret_cast<const char *>(&order_n), sizeof(order_n));
        for (auto &rid : pq_row_id_order_) {
            int64_t v = static_cast<int64_t>(rid);
            order_blob.append(reinterpret_cast<const char *>(&v), sizeof(v));
        }
        info.options["pq_row_order"] = Value::BLOB(const_data_ptr_cast(order_blob.data()), order_blob.size());
    }

    return info;
}

IndexStorageInfo GraphIndex::SerializeToDisk(QueryContext context, const case_insensitive_map_t<Value> &options) {
    (void)options;

    if (!runtime_ || !runtime_->store.node_alloc_) {
        return ExportStorageInfo();
    }

    if (!context.Valid()) {
        return ExportStorageInfo();
    }

    auto &block_manager = table_io_manager.GetIndexBlockManager();
    PartialBlockManager partial_block_manager(context, block_manager, PartialBlockType::FULL_CHECKPOINT);

    runtime_->store.node_alloc_->SerializeBuffers(partial_block_manager);
    runtime_->store.vector_alloc_->SerializeBuffers(partial_block_manager);
    runtime_->store.upper_alloc_->SerializeBuffers(partial_block_manager);
    partial_block_manager.FlushPartialBlocks();

    return ExportStorageInfo();
}

IndexStorageInfo GraphIndex::SerializeToWAL(const case_insensitive_map_t<Value> &options) {
    (void)options;

    if (!runtime_ || !runtime_->store.node_alloc_) {
        return ExportStorageInfo();
    }

    auto info = ExportStorageInfo();

    auto node_buffers = runtime_->store.node_alloc_->InitSerializationToWAL();
    auto vector_buffers = runtime_->store.vector_alloc_->InitSerializationToWAL();
    auto upper_buffers = runtime_->store.upper_alloc_->InitSerializationToWAL();

    (void)node_buffers;
    (void)vector_buffers;
    (void)upper_buffers;

    return info;
}

} // namespace duckdb
