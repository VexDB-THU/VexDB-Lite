#include "vex_functions.hpp"
#include "vex_graph_index.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/index_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/enums/database_modification_type.hpp"
#include "duckdb/execution/index/bound_index.hpp"
#include "duckdb/function/pragma_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

#include <cerrno>
#include <cmath>
#include <limits>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace duckdb {

namespace {

// Rebuild sources can be much larger than the graph metadata. Keep the raw
// vectors in a temporary file and map them read-only during BuildBulk instead
// of growing a process-heap vector while the old index is still resident.
class VectorSpool {
public:
    VectorSpool(ClientContext &context, AttachedDatabase &db) {
#if !defined(_WIN32)
        auto &buffer_manager = BufferManager::GetBufferManager(db);
        const auto &temporary_directory = buffer_manager.GetTemporaryDirectory();
        if (temporary_directory.empty()) {
            throw IOException(
                "vexdb_rebuild_index needs DuckDB temporary_directory to store rebuild vectors");
        }
        auto &fs = FileSystem::GetFileSystem(context);
        if (!fs.DirectoryExists(temporary_directory)) {
            fs.CreateDirectory(temporary_directory);
        }
        const auto configured_limit =
            DBConfig::GetConfig(db.GetDatabase()).options.maximum_swap_space;
        if (configured_limit != DConstants::INVALID_INDEX) {
            idx_t existing_temporary_bytes = 0;
            for (const auto &file : buffer_manager.GetTemporaryFiles()) {
                if (file.size > configured_limit -
                                    std::min(configured_limit, existing_temporary_bytes)) {
                    existing_temporary_bytes = configured_limit;
                    break;
                }
                existing_temporary_bytes += file.size;
            }
            max_spool_bytes_ = configured_limit -
                               std::min(configured_limit, existing_temporary_bytes);
        }
        auto path = fs.JoinPath(temporary_directory, "vexdb-rebuild-XXXXXX");
        std::vector<char> mutable_path(path.begin(), path.end());
        mutable_path.push_back('\0');
        fd_ = mkstemp(mutable_path.data());
        if (fd_ < 0) {
            throw IOException("vexdb_rebuild_index cannot create temporary vector storage");
        }
        if (fcntl(fd_, F_SETFD, FD_CLOEXEC) != 0) {
            auto saved_errno = errno;
            close(fd_);
            unlink(mutable_path.data());
            fd_ = -1;
            throw IOException("vexdb_rebuild_index cannot secure temporary vector storage: errno %d", saved_errno);
        }
        if (unlink(mutable_path.data()) != 0) {
            auto saved_errno = errno;
            close(fd_);
            fd_ = -1;
            throw IOException("vexdb_rebuild_index cannot unlink temporary vector storage: errno %d", saved_errno);
        }
        write_buffer_.reserve(WRITE_BUFFER_FLOATS);
#endif
    }

    VectorSpool(const VectorSpool &) = delete;
    VectorSpool &operator=(const VectorSpool &) = delete;
    VectorSpool(VectorSpool &&) = delete;
    VectorSpool &operator=(VectorSpool &&) = delete;

    ~VectorSpool() {
#if !defined(_WIN32)
        Unmap();
        if (fd_ >= 0) {
            close(fd_);
        }
#endif
    }

    void Unmap() {
#if !defined(_WIN32)
        if (mapped_ && mapped_bytes_ > 0) {
            munmap(mapped_, mapped_bytes_);
            mapped_ = nullptr;
            mapped_bytes_ = 0;
        }
#endif
    }

    void Append(const float *data, size_t count) {
        if (count == 0) {
            return;
        }
        if (!data || count > std::numeric_limits<size_t>::max() / sizeof(float)) {
            throw InvalidInputException("vexdb_rebuild_index vector chunk is too large");
        }
        if (count_ > std::numeric_limits<size_t>::max() - count) {
            throw InvalidInputException("vexdb_rebuild_index vector data is too large");
        }
#if !defined(_WIN32)
        const auto total_count = count_ + count;
        if (max_spool_bytes_ != DConstants::INVALID_INDEX &&
            total_count > max_spool_bytes_ / sizeof(float)) {
            throw OutOfMemoryException(
                "vexdb_rebuild_index temporary vector data exceeds max_temp_directory_size");
        }
#endif
#if !defined(_WIN32)
        size_t offset = 0;
        while (offset < count) {
            auto available = WRITE_BUFFER_FLOATS - write_buffer_.size();
            auto append_count = std::min(available, count - offset);
            write_buffer_.insert(write_buffer_.end(), data + offset, data + offset + append_count);
            offset += append_count;
            if (write_buffer_.size() == WRITE_BUFFER_FLOATS) {
                FlushWriteBuffer();
            }
        }
#else
        if (count > memory_.max_size() - memory_.size()) {
            throw InvalidInputException("vexdb_rebuild_index vector data is too large");
        }
        memory_.insert(memory_.end(), data, data + count);
#endif
        count_ += count;
    }

    void AppendVector(const float *data, idx_t dimension, bool normalize) {
        if (dimension > 0 && !data) {
            throw InvalidInputException("vexdb_rebuild_index vector data is null");
        }
        for (idx_t i = 0; i < dimension; i++) {
            if (!std::isfinite(data[i])) {
                throw InvalidInputException("vexdb_rebuild_index vector contains NaN or infinity");
            }
        }
        if (!normalize) {
            Append(data, dimension);
            return;
        }
        normalized_.assign(data, data + dimension);
        NormalizeDuckVectorInPlace(normalized_.data(), dimension);
        Append(normalized_.data(), normalized_.size());
    }

    const float *Map() {
#if !defined(_WIN32)
        FlushWriteBuffer();
        if (count_ == 0) {
            return nullptr;
        }
        if (count_ > std::numeric_limits<size_t>::max() / sizeof(float)) {
            throw InvalidInputException("vexdb_rebuild_index vector data is too large");
        }
        mapped_bytes_ = count_ * sizeof(float);
        mapped_ = mmap(nullptr, mapped_bytes_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (mapped_ == MAP_FAILED) {
            mapped_ = nullptr;
            throw IOException("vexdb_rebuild_index cannot map temporary vector storage");
        }
        return reinterpret_cast<const float *>(mapped_);
#else
        return memory_.empty() ? nullptr : memory_.data();
#endif
    }

private:
#if !defined(_WIN32)
    static constexpr size_t WRITE_BUFFER_FLOATS = (1U << 20U) / sizeof(float);

    void FlushWriteBuffer() {
        const char *ptr = reinterpret_cast<const char *>(write_buffer_.data());
        size_t bytes = write_buffer_.size() * sizeof(float);
        while (bytes > 0) {
            auto written = write(fd_, ptr, bytes);
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                throw IOException("vexdb_rebuild_index failed writing temporary vector storage");
            }
            ptr += written;
            bytes -= static_cast<size_t>(written);
        }
        write_buffer_.clear();
    }

    int fd_ = -1;
    void *mapped_ = nullptr;
    size_t mapped_bytes_ = 0;
    std::vector<float> write_buffer_;
    idx_t max_spool_bytes_ = DConstants::INVALID_INDEX;
#else
    std::vector<float> memory_;
#endif
    size_t count_ = 0;
    // Reused for one vector at a time. Cosine rebuilds therefore do not need
    // a second table-sized normalized buffer in the process heap.
    std::vector<float> normalized_;
};

struct RebuildTarget {
    string catalog_name;
    string schema_name;
    string table_name;
    string index_name;
};

static vector<RebuildTarget> FindGraphIndexTargets(ClientContext &context, const string &index_name) {
    vector<RebuildTarget> targets;
    auto schemas = Catalog::GetAllSchemas(context);
    for (auto &schema_ref : schemas) {
        auto &schema = schema_ref.get();
        schema.Scan(context, CatalogType::INDEX_ENTRY, [&](CatalogEntry &entry) {
            auto &index_entry = entry.Cast<IndexCatalogEntry>();
            if (index_entry.index_type != GraphIndex::TYPE_NAME || index_entry.name != index_name) {
                return;
            }
            RebuildTarget target;
            target.catalog_name = index_entry.ParentCatalog().GetName();
            target.schema_name = index_entry.GetSchemaName();
            target.table_name = index_entry.GetTableName();
            target.index_name = index_entry.name;
            targets.push_back(std::move(target));
        });
    }
    return targets;
}

static void CollectTableVectors(ClientContext &context, DataTable &storage, DuckTransaction &transaction,
                                column_t vector_column_id, idx_t dimension, VectorSpool &vectors,
                                std::vector<row_t> &row_ids, bool normalize_vectors) {
    auto table_types = storage.GetTypes();
    if (vector_column_id >= table_types.size()) {
        throw InvalidInputException("GRAPH_INDEX vector column id is out of range");
    }
    auto &vec_type = table_types[vector_column_id];
    if (vec_type.id() != LogicalTypeId::ARRAY || ArrayType::GetChildType(vec_type).id() != LogicalTypeId::FLOAT) {
        throw InvalidInputException("GRAPH_INDEX vector column must be FLOAT[N], got %s", vec_type.ToString());
    }
    auto actual_dim = ArrayType::GetSize(vec_type);
    if (actual_dim != dimension) {
        throw InvalidInputException("GRAPH_INDEX dimension mismatch: expected %llu, got %llu",
                                    static_cast<unsigned long long>(dimension),
                                    static_cast<unsigned long long>(actual_dim));
    }

    vector<StorageIndex> scan_column_ids;
    scan_column_ids.emplace_back(vector_column_id);
    scan_column_ids.emplace_back(COLUMN_IDENTIFIER_ROW_ID);

    TableScanState scan_state;
    storage.InitializeScan(context, transaction, scan_state, scan_column_ids);

    DataChunk chunk;
    chunk.Initialize(Allocator::Get(context), {vec_type, LogicalType::ROW_TYPE});
    while (true) {
        chunk.Reset();
        storage.Scan(transaction, chunk, scan_state);
        if (chunk.size() == 0) {
            break;
        }

        auto &vec_vector = chunk.data[0];
        auto &rowid_vec = chunk.data[1];
        vec_vector.Flatten(chunk.size());
        rowid_vec.Flatten(chunk.size());

        auto &vec_validity = FlatVector::Validity(vec_vector);
        auto &rowid_validity = FlatVector::Validity(rowid_vec);
        auto &child_vec = ArrayVector::GetEntry(vec_vector);
        child_vec.Flatten(chunk.size() * dimension);
        auto vec_data = FlatVector::GetData<float>(child_vec);
        auto row_id_data = FlatVector::GetData<row_t>(rowid_vec);

        for (idx_t i = 0; i < chunk.size(); i++) {
            if (!vec_validity.RowIsValid(i) || !rowid_validity.RowIsValid(i)) {
                continue;
            }
            auto row_id = row_id_data[i];
            if (row_id < 0 || row_id >= MAX_ROW_ID) {
                continue;
            }
            auto *src = vec_data + i * dimension;
            vectors.AppendVector(src, dimension, normalize_vectors);
            row_ids.push_back(row_id);
        }
    }
}

static GraphIndex &FindBoundGraphIndex(ClientContext &context, DataTable &storage, const string &index_name) {
    auto &table_info = *storage.GetDataTableInfo();
    if (table_info.GetIndexes().HasUnbound()) {
        table_info.BindIndexes(context, GraphIndex::TYPE_NAME);
    }
    auto &index_list = table_info.GetIndexes();
    for (auto &index : index_list.Indexes()) {
        if (!index.IsBound() || index.GetIndexName() != index_name) {
            continue;
        }
        auto &bound_index = index.Cast<BoundIndex>();
        if (bound_index.GetIndexType() != GraphIndex::TYPE_NAME) {
            continue;
        }
        return bound_index.Cast<GraphIndex>();
    }
    throw InvalidInputException("GRAPH_INDEX '%s' is not bound to its table", index_name);
}

static void VexRebuildIndexPragma(ClientContext &context, const FunctionParameters &parameters) {
    if (parameters.values.size() != 1) {
        throw InvalidInputException("vexdb_rebuild_index expects exactly one index name");
    }
    auto index_name = parameters.values[0].GetValue<string>();
    if (index_name.empty()) {
        throw InvalidInputException("vexdb_rebuild_index index name cannot be empty");
    }
    if (!context.transaction.IsAutoCommit()) {
        throw InvalidInputException("vexdb_rebuild_index must run in auto-commit mode");
    }

    auto targets = FindGraphIndexTargets(context, index_name);
    if (targets.empty()) {
        throw InvalidInputException("GRAPH_INDEX '%s' does not exist", index_name);
    }
    if (targets.size() > 1) {
        throw InvalidInputException("GRAPH_INDEX name '%s' is ambiguous; found %llu indexes with that name",
                                    index_name, static_cast<unsigned long long>(targets.size()));
    }

    auto &target = targets[0];
    auto &table_entry = Catalog::GetEntry(context, CatalogType::TABLE_ENTRY, target.catalog_name,
                                          target.schema_name, target.table_name).Cast<TableCatalogEntry>();
    auto &duck_table = table_entry.Cast<DuckTableEntry>();
    auto &storage = duck_table.GetStorage();
    auto &db = duck_table.ParentCatalog().GetAttached();
    auto &transaction = DuckTransaction::Get(context, db);
    if (transaction.ChangesMade()) {
        throw InvalidInputException("vexdb_rebuild_index cannot run after changes in the current transaction");
    }

    // Mark this as a database write so read-only attachments are rejected and
    // checkpoints cannot overlap the in-memory index replacement. The table's
    // checkpoint lock is the narrower DML barrier: active writes to this table
    // finish before the scan, while unrelated tables remain writable. A write
    // transaction whose statement already finished can safely commit later;
    // DuckDB applies its index delta to the newly published GraphIndex.
    MetaTransaction::Get(context).ModifyDatabase(db, DatabaseModificationType::UPDATE_DATA);
    auto table_guard = storage.GetCheckpointLock();

    auto &graph_idx = FindBoundGraphIndex(context, storage, target.index_name);
    auto column_ids = graph_idx.GetColumnIds();
    if (column_ids.empty()) {
        throw InvalidInputException("GRAPH_INDEX '%s' has no indexed columns", index_name);
    }

    VectorSpool vectors(context, db);
    std::vector<row_t> row_ids;
    const bool vectors_are_normalized = graph_idx.GetMetric() == VexMetric::COSINE;
    CollectTableVectors(context, storage, transaction, column_ids[0], graph_idx.GetDimension(), vectors, row_ids,
                        vectors_are_normalized);
    const float *vector_data = vectors.Map();
    graph_idx.RebuildBulk(vector_data, row_ids.size(), row_ids, vectors_are_normalized,
                          [&vectors]() { vectors.Unmap(); });
}

} // namespace

void VexFunctions::RegisterRebuildIndexFunction(ExtensionLoader &loader) {
    auto func = PragmaFunction::PragmaCall("vexdb_rebuild_index", VexRebuildIndexPragma,
                                           {LogicalType::VARCHAR});
    loader.RegisterFunction(func);
}

} // namespace duckdb
