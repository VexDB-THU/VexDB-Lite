#ifndef VEXDB_SIGNATURE_SEARCH_H
#define VEXDB_SIGNATURE_SEARCH_H

#include "signature_projection.h"
#include <limits>

namespace signature_rescue {
// Optional auxiliary format, separate from both graph and PQ code formats.
struct Header {
    uint32_t magic = 0x47535841, version = 1, dim = 0, bits = 256;
    uint32_t padded = 0, valid = 0;
    uint64_t seed = 0, count = 0;
};
inline bool ValidHeader(const Header &h, size_t dim, size_t n) {
    return h.magic == 0x47535841 && h.version == 1 && h.valid == 1 &&
           h.dim == dim && h.bits == 256 && h.padded == Padded(dim,512) && h.count == n;
}
constexpr size_t record_bytes = 36; // float norm + 256 sign bits
// Query-local observer, copied by value; no mutable state in shared graph stores.
struct Observer {
    unsigned char *seen = nullptr;
    size_t capacity = 0;
    size_t *calls = nullptr;
    template<typename ID> void record(ID id) const {
        ++*calls;
        if (uint64_t(id) < capacity) seen[size_t(id)/8] |= 1u << (size_t(id)%8);
    }
    bool contains(size_t id) const { return seen[id/8] & (1u << (id%8)); }
};
inline bool WorthScanning(size_t n, size_t dim, size_t padded, size_t calls) {
    return double(n)*record_bytes + double(padded)*4 <= .10*double(calls)*dim*4;
}
struct Nominee {
    float distance;
    uint32_t id;
    bool operator<(const Nominee &b) const {
        return distance < b.distance || (distance == b.distance && id < b.id);
    }
};
// Caller supplies a bounded heap; the stream may release page pins before exact
// scoring starts. Every old graph candidate stays in the final candidate pool.
struct Selector {
    Nominee *heap;
    size_t count = 0, limit;
    Observer observer;
    const uint64_t *query;
    float query_norm;
    const float *cosines;
    void offer(uint32_t id, const void *record) {
        if (!limit || observer.contains(id)) return;
        float norm; std::memcpy(&norm,record,4);
        if (!std::isfinite(norm)) return;
        auto h = HammingBytes(query,static_cast<const char*>(record)+4,4);
        Nominee next{Estimate(query_norm,norm,h,cosines),id};
        if (!std::isfinite(next.distance)) return;
        if (count < limit) { heap[count++] = next; std::push_heap(heap,heap+count); }
        else if (next < heap[0]) {
            std::pop_heap(heap,heap+count); heap[count-1] = next;
            std::push_heap(heap,heap+count);
        }
    }
};
}
#endif
