#include "vex/vex_node_store_memory.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace vex {
namespace {

constexpr uint32_t kSnapshotMagic = 0x56584E53; // "VXNS"
constexpr uint32_t kSnapshotVersion = 1;

#pragma pack(push, 1)
struct SnapshotHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t dim;
    int32_t m;
    uint32_t metadata_size;
    uint64_t node_count;
    uint64_t total_slots;
};
#pragma pack(pop)

static_assert(std::is_trivially_copyable<SnapshotHeader>::value, "SnapshotHeader must be trivially copyable");

class Writer {
public:
    explicit Writer(std::vector<uint8_t> &buf) : buf_(buf) {}

    template <typename T>
    void PutPod(const T &value) {
        const auto *ptr = reinterpret_cast<const uint8_t *>(&value);
        buf_.insert(buf_.end(), ptr, ptr + sizeof(T));
    }

    void PutBytes(const void *data, size_t len) {
        const auto *ptr = reinterpret_cast<const uint8_t *>(data);
        buf_.insert(buf_.end(), ptr, ptr + len);
    }

private:
    std::vector<uint8_t> &buf_;
};

class Reader {
public:
    Reader(const uint8_t *data, size_t size) : data_(data), size_(size), off_(0) {}

    template <typename T>
    bool GetPod(T &out) {
        if (!CanRead(sizeof(T))) {
            return false;
        }
        std::memcpy(&out, data_ + off_, sizeof(T));
        off_ += sizeof(T);
        return true;
    }

    bool GetBytes(void *out, size_t len) {
        if (!CanRead(len)) {
            return false;
        }
        std::memcpy(out, data_ + off_, len);
        off_ += len;
        return true;
    }

private:
    bool CanRead(size_t len) const {
        return off_ <= size_ && len <= (size_ - off_);
    }

private:
    const uint8_t *data_;
    size_t size_;
    size_t off_;
};

} // namespace

MemoryNodeStore::MemoryNodeStore(uint32_t dim, int m, uint32_t metadata_size)
    : dim_(dim), m_(m), metadata_size_(metadata_size) {
    if (dim_ == 0 || m_ <= 0) {
        throw std::invalid_argument("MemoryNodeStore requires dim > 0 and m > 0");
    }
}

bool MemoryNodeStore::IsNodeValid(node_id_t node_id) const {
    return node_id < headers_.size();
}

MemoryNodeStore::MemoryNodeHandle::MemoryNodeHandle(MemoryNodeStore *store, node_id_t node_id)
    : store_(store), node_id_(node_id) {
}

const NodeHeader *MemoryNodeStore::MemoryNodeHandle::Header() const {
    return &store_->headers_[node_id_];
}

const float *MemoryNodeStore::MemoryNodeHandle::Vector() const {
    return store_->vectors_.data() + static_cast<size_t>(node_id_) * store_->dim_;
}

const node_id_t *MemoryNodeStore::MemoryNodeHandle::Level0Neighbors() const {
    const size_t l0_size = static_cast<size_t>(store_->m_) * 2;
    return store_->neighbors_l0_.data() + static_cast<size_t>(node_id_) * l0_size;
}

uint16_t MemoryNodeStore::MemoryNodeHandle::Level0Count() const {
    return Header()->level0_count;
}

const node_id_t *MemoryNodeStore::MemoryNodeHandle::UpperNeighbors(int level_idx) const {
    if (level_idx < 0 || level_idx >= HNSW_MAX_UPPER_LEVELS) {
        return nullptr;
    }
    const auto &blk = store_->upper_neighbors_[node_id_];
    return blk.neighbors.data() + static_cast<size_t>(level_idx) * static_cast<size_t>(store_->m_);
}

uint16_t MemoryNodeStore::MemoryNodeHandle::UpperCount(int level_idx) const {
    if (level_idx < 0 || level_idx >= HNSW_MAX_UPPER_LEVELS) {
        return 0;
    }
    return store_->upper_neighbors_[node_id_].counts[static_cast<size_t>(level_idx)];
}

const uint8_t *MemoryNodeStore::MemoryNodeHandle::Metadata() const {
    if (store_->metadata_size_ == 0) {
        return nullptr;
    }
    return store_->metadata_.data() + static_cast<size_t>(node_id_) * store_->metadata_size_;
}

NodeHeader *MemoryNodeStore::MemoryNodeHandle::MutableHeader() {
    return const_cast<NodeHeader *>(Header());
}

node_id_t *MemoryNodeStore::MemoryNodeHandle::MutableLevel0Neighbors() {
    return const_cast<node_id_t *>(Level0Neighbors());
}

void MemoryNodeStore::MemoryNodeHandle::SetLevel0Count(uint16_t count) {
    MutableHeader()->level0_count = count;
}

node_id_t *MemoryNodeStore::MemoryNodeHandle::MutableUpperNeighbors(int level_idx) {
    return const_cast<node_id_t *>(UpperNeighbors(level_idx));
}

void MemoryNodeStore::MemoryNodeHandle::SetUpperCount(int level_idx, uint16_t count) {
    if (level_idx < 0 || level_idx >= HNSW_MAX_UPPER_LEVELS) {
        return;
    }
    store_->upper_neighbors_[node_id_].counts[static_cast<size_t>(level_idx)] = count;
}

uint16_t *MemoryNodeStore::MemoryNodeHandle::MutableUpperCounts() {
    return store_->upper_neighbors_[node_id_].counts.data();
}

const uint16_t *MemoryNodeStore::MemoryNodeHandle::UpperCounts() const {
    return store_->upper_neighbors_[node_id_].counts.data();
}

node_id_t MemoryNodeStore::AllocateNode(row_id_t row_id, const float *vec, uint32_t dim, uint8_t level) {
    if (vec == nullptr) {
        throw std::invalid_argument("AllocateNode: vec is null");
    }
    if (dim != dim_) {
        throw std::invalid_argument("AllocateNode: dimension mismatch");
    }

    const node_id_t node_id = static_cast<node_id_t>(headers_.size());

    NodeHeader header{};
    header.row_id = row_id;
    header.level = level;
    headers_.push_back(header);

    vectors_.insert(vectors_.end(), vec, vec + dim_);

    const size_t l0_size = static_cast<size_t>(m_) * 2;
    neighbors_l0_.insert(neighbors_l0_.end(), l0_size, INVALID_NODE_ID);

    UpperBlock upper;
    upper.counts.assign(HNSW_MAX_UPPER_LEVELS, 0);
    upper.neighbors.assign(static_cast<size_t>(HNSW_MAX_UPPER_LEVELS) * static_cast<size_t>(m_), INVALID_NODE_ID);
    upper_neighbors_.push_back(std::move(upper));

    if (metadata_size_ > 0) {
        metadata_.insert(metadata_.end(), metadata_size_, static_cast<uint8_t>(0));
    }

    alive_.push_back(1);
    node_count_++;
    return node_id;
}

void MemoryNodeStore::FreeNode(node_id_t node_id) {
    if (node_id >= alive_.size()) {
        return;
    }
    if (!alive_[node_id]) {
        return;
    }
    alive_[node_id] = 0;
    headers_[node_id].deleted = 1;
    if (node_count_ > 0) {
        node_count_--;
    }
}

std::unique_ptr<NodeHandle> MemoryNodeStore::PinNode(node_id_t node_id) const {
    if (!IsNodeValid(node_id)) {
        return nullptr;
    }
    return std::unique_ptr<NodeHandle>(new MemoryNodeHandle(const_cast<MemoryNodeStore *>(this), node_id));
}

std::unique_ptr<MutableNodeHandle> MemoryNodeStore::PinNodeForUpdate(node_id_t node_id) {
    if (!IsNodeValid(node_id)) {
        return nullptr;
    }
    return std::unique_ptr<MutableNodeHandle>(new MemoryNodeHandle(this, node_id));
}

void MemoryNodeStore::ForEachNode(std::function<void(node_id_t)> cb) const {
    for (node_id_t i = 0; i < alive_.size(); i++) {
        if (alive_[i]) {
            cb(i);
        }
    }
}

bool MemoryNodeStore::Serialize(std::vector<uint8_t> &out) const {
    out.clear();

    SnapshotHeader hdr{};
    hdr.magic = kSnapshotMagic;
    hdr.version = kSnapshotVersion;
    hdr.dim = dim_;
    hdr.m = m_;
    hdr.metadata_size = metadata_size_;
    hdr.node_count = node_count_;
    hdr.total_slots = headers_.size();

    Writer w(out);
    w.PutPod(hdr);

    for (size_t i = 0; i < headers_.size(); i++) {
        w.PutPod(headers_[i]);

        const float *vec = vectors_.data() + i * dim_;
        w.PutBytes(vec, sizeof(float) * dim_);

        const size_t l0_size = static_cast<size_t>(m_) * 2;
        const node_id_t *l0 = neighbors_l0_.data() + i * l0_size;
        w.PutBytes(l0, sizeof(node_id_t) * l0_size);

        const auto &upper = upper_neighbors_[i];
        w.PutBytes(upper.counts.data(), sizeof(uint16_t) * upper.counts.size());
        w.PutBytes(upper.neighbors.data(), sizeof(node_id_t) * upper.neighbors.size());

        if (metadata_size_ > 0) {
            const uint8_t *meta = metadata_.data() + i * metadata_size_;
            w.PutBytes(meta, metadata_size_);
        }

        w.PutPod(alive_[i]);
    }

    return true;
}

bool MemoryNodeStore::Deserialize(const uint8_t *data, size_t size, std::string *err) {
    if (data == nullptr || size < sizeof(SnapshotHeader)) {
        if (err) {
            *err = "snapshot too small";
        }
        return false;
    }

    Reader r(data, size);

    SnapshotHeader hdr{};
    if (!r.GetPod(hdr)) {
        if (err) {
            *err = "failed to read snapshot header";
        }
        return false;
    }

    if (hdr.magic != kSnapshotMagic || hdr.version != kSnapshotVersion) {
        if (err) {
            *err = "invalid snapshot magic/version";
        }
        return false;
    }

    if (hdr.dim != dim_ || hdr.m != m_ || hdr.metadata_size != metadata_size_) {
        if (err) {
            *err = "snapshot config mismatch";
        }
        return false;
    }

    headers_.clear();
    vectors_.clear();
    neighbors_l0_.clear();
    upper_neighbors_.clear();
    metadata_.clear();
    alive_.clear();
    node_count_ = 0;

    headers_.reserve(hdr.total_slots);
    vectors_.reserve(static_cast<size_t>(hdr.total_slots) * dim_);
    neighbors_l0_.reserve(static_cast<size_t>(hdr.total_slots) * static_cast<size_t>(m_) * 2);
    upper_neighbors_.reserve(hdr.total_slots);
    alive_.reserve(hdr.total_slots);
    if (metadata_size_ > 0) {
        metadata_.reserve(static_cast<size_t>(hdr.total_slots) * metadata_size_);
    }

    for (uint64_t i = 0; i < hdr.total_slots; i++) {
        NodeHeader header{};
        if (!r.GetPod(header)) {
            if (err) {
                *err = "failed to read node header";
            }
            return false;
        }
        headers_.push_back(header);

        std::vector<float> vec(dim_);
        if (!r.GetBytes(vec.data(), sizeof(float) * dim_)) {
            if (err) {
                *err = "failed to read vector data";
            }
            return false;
        }
        vectors_.insert(vectors_.end(), vec.begin(), vec.end());

        const size_t l0_size = static_cast<size_t>(m_) * 2;
        std::vector<node_id_t> l0(l0_size);
        if (!r.GetBytes(l0.data(), sizeof(node_id_t) * l0_size)) {
            if (err) {
                *err = "failed to read level0 neighbors";
            }
            return false;
        }
        neighbors_l0_.insert(neighbors_l0_.end(), l0.begin(), l0.end());

        UpperBlock upper;
        upper.counts.resize(HNSW_MAX_UPPER_LEVELS);
        if (!r.GetBytes(upper.counts.data(), sizeof(uint16_t) * upper.counts.size())) {
            if (err) {
                *err = "failed to read upper counts";
            }
            return false;
        }

        upper.neighbors.resize(static_cast<size_t>(HNSW_MAX_UPPER_LEVELS) * static_cast<size_t>(m_));
        if (!r.GetBytes(upper.neighbors.data(), sizeof(node_id_t) * upper.neighbors.size())) {
            if (err) {
                *err = "failed to read upper neighbors";
            }
            return false;
        }
        upper_neighbors_.push_back(std::move(upper));

        if (metadata_size_ > 0) {
            std::vector<uint8_t> meta(metadata_size_);
            if (!r.GetBytes(meta.data(), metadata_size_)) {
                if (err) {
                    *err = "failed to read metadata";
                }
                return false;
            }
            metadata_.insert(metadata_.end(), meta.begin(), meta.end());
        }

        uint8_t alive = 0;
        if (!r.GetPod(alive)) {
            if (err) {
                *err = "failed to read alive flag";
            }
            return false;
        }
        alive_.push_back(alive);
        if (alive) {
            node_count_++;
        }
    }

    const uint64_t max_slots = headers_.size();
    const size_t l0_width = static_cast<size_t>(m_) * 2;
    const size_t upper_width = static_cast<size_t>(HNSW_MAX_UPPER_LEVELS) * static_cast<size_t>(m_);
    for (size_t i = 0; i < headers_.size(); i++) {
        if (alive_[i] == 0) {
            continue;
        }
        const auto &h = headers_[i];
        if (h.level > HNSW_MAX_UPPER_LEVELS) {
            if (err) {
                *err = "invalid node level";
            }
            return false;
        }
        if (h.level0_count > l0_width) {
            if (err) {
                *err = "invalid level0 count";
            }
            return false;
        }
        const node_id_t *l0 = neighbors_l0_.data() + i * l0_width;
        for (uint16_t j = 0; j < h.level0_count; j++) {
            node_id_t nid = l0[j];
            if (nid == INVALID_NODE_ID) {
                if (err) {
                    *err = "invalid level0 neighbor id";
                }
                return false;
            }
            if (nid >= max_slots) {
                if (err) {
                    *err = "level0 neighbor id out of range";
                }
                return false;
            }
        }

        const auto &upper = upper_neighbors_[i];
        if (upper.neighbors.size() != upper_width) {
            if (err) {
                *err = "invalid upper neighbor width";
            }
            return false;
        }
        for (int lv = 0; lv < HNSW_MAX_UPPER_LEVELS; lv++) {
            uint16_t cnt = upper.counts[static_cast<size_t>(lv)];
            if (cnt > static_cast<uint16_t>(m_)) {
                if (err) {
                    *err = "invalid upper count";
                }
                return false;
            }
            const node_id_t *arr = upper.neighbors.data() + static_cast<size_t>(lv) * static_cast<size_t>(m_);
            for (uint16_t j = 0; j < cnt; j++) {
                node_id_t nid = arr[j];
                if (nid == INVALID_NODE_ID) {
                    if (err) {
                        *err = "invalid upper neighbor id";
                    }
                    return false;
                }
                if (nid >= max_slots) {
                    if (err) {
                        *err = "upper neighbor id out of range";
                    }
                    return false;
                }
            }
        }
    }

    if (node_count_ != hdr.node_count) {
        if (err) {
            *err = "snapshot node count mismatch";
        }
        return false;
    }

    return true;
}

} // namespace vex
