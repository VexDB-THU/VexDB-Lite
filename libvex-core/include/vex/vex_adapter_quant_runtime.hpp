#ifndef VEX_ADAPTER_QUANT_RUNTIME_HPP
#define VEX_ADAPTER_QUANT_RUNTIME_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace vex {

template <typename QuantizerT>
inline uint32_t ResolveQuantizerSubspaces(uint32_t dimension, uint32_t requested_subspaces) {
    if (requested_subspaces != 0) {
        return requested_subspaces;
    }
    return QuantizerT::AutoSelectM(dimension);
}

template <typename RowMapT, typename HeaderLookup, typename VectorLookup>
inline std::vector<float> CollectTrainingVectors(const RowMapT &row_id_map, uint32_t dimension,
                                                 HeaderLookup &&header_lookup, VectorLookup &&vector_lookup) {
    std::vector<float> training_data;
    if (dimension == 0) {
        return training_data;
    }

    training_data.reserve(static_cast<size_t>(row_id_map.size()) * dimension);
    for (const auto &entry : row_id_map) {
        auto *header = header_lookup(entry.second);
        if (header == nullptr || header->deleted) {
            continue;
        }
        const float *vec = vector_lookup(*header);
        if (vec == nullptr) {
            continue;
        }
        training_data.insert(training_data.end(), vec, vec + dimension);
    }
    return training_data;
}

template <typename RowMapT, typename QuantizerT, typename HeaderLookup, typename VectorLookup>
inline void EncodeAllQuantizedCodes(const RowMapT &row_id_map, const QuantizerT &quantizer,
                                    HeaderLookup &&header_lookup, VectorLookup &&vector_lookup,
                                    std::vector<uint8_t> &out_codes) {
    const uint32_t code_size = quantizer.CodeSize();
    out_codes.assign(static_cast<size_t>(row_id_map.size()) * code_size, 0);

    size_t code_index = 0;
    for (const auto &entry : row_id_map) {
        auto *header = header_lookup(entry.second);
        if (header == nullptr || header->deleted) {
            ++code_index;
            continue;
        }
        const float *vec = vector_lookup(*header);
        if (vec != nullptr) {
            quantizer.Encode(vec, out_codes.data() + code_index * code_size);
        }
        ++code_index;
    }
}

template <typename RowMapT, typename CodeVisitor>
inline bool VisitFlatCodesByRowId(const RowMapT &row_id_map, const std::vector<uint8_t> &flat_codes,
                                  uint32_t code_size, CodeVisitor &&visitor) {
    size_t code_index = 0;
    for (const auto &entry : row_id_map) {
        const size_t code_offset = code_index * code_size;
        if (code_offset + code_size > flat_codes.size()) {
            return false;
        }
        visitor(entry.first, flat_codes.data() + code_offset, code_index);
        ++code_index;
    }
    return true;
}

template <typename RowMapT, typename OutputMapT>
inline void BuildRowCodeIndex(const RowMapT &row_id_map, OutputMapT &out_row_to_code_index) {
    out_row_to_code_index.clear();
    out_row_to_code_index.reserve(row_id_map.size());

    size_t code_index = 0;
    for (const auto &entry : row_id_map) {
        out_row_to_code_index[entry.first] = code_index;
        ++code_index;
    }
}

template <typename NodeStoreT, typename RowMapT>
inline bool BuildNodeFlatCodePointerIndex(NodeStoreT &store, const RowMapT &row_id_map,
                                          const std::vector<uint8_t> &flat_codes, uint32_t code_size,
                                          std::vector<const uint8_t *> &out_code_by_node_id) {
    std::unordered_map<row_id_t, const uint8_t *> code_by_row_id;
    code_by_row_id.reserve(row_id_map.size());

    if (!VisitFlatCodesByRowId(row_id_map, flat_codes, code_size,
                               [&](row_id_t row_id, const uint8_t *code, size_t) {
                                   code_by_row_id[row_id] = code;
                               })) {
        return false;
    }

    out_code_by_node_id.assign(static_cast<size_t>(store.GetNodeCount()), nullptr);
    store.ForEachNode([&](node_id_t node_id) {
        auto handle = store.PinNode(node_id);
        if (!handle || !handle->Header()) {
            return;
        }
        auto it = code_by_row_id.find(handle->Header()->row_id);
        if (it != code_by_row_id.end() && static_cast<size_t>(node_id) < out_code_by_node_id.size()) {
            out_code_by_node_id[node_id] = it->second;
        }
    });
    return true;
}

template <typename DistancerT, typename QuantizerT>
inline void LoadQuantizerIntoDistancer(DistancerT *distancer, const QuantizerT &quantizer) {
    if (distancer == nullptr) {
        return;
    }
    distancer->LoadQuantizer(quantizer);
}

template <typename QuantizerT>
inline bool PrepareQuantizedQuery(const QuantizerT &quantizer, const float *query, std::vector<float> &dist_table) {
    dist_table.assign(static_cast<size_t>(quantizer.m) * QuantizerT::KSUB, 0.0f);
    quantizer.ComputeDistanceTable(query, dist_table.data());
    return false;
}

template <typename QuantizerT, typename DistancerT>
inline bool PrepareQuantizedQuery(const QuantizerT &quantizer, DistancerT *distancer, const float *query,
                                  uint32_t dimension, std::vector<float> &dist_table) {
    if (distancer != nullptr) {
        distancer->PrepareQuery(query, dimension);
        return true;
    }

    dist_table.assign(static_cast<size_t>(quantizer.m) * QuantizerT::KSUB, 0.0f);
    quantizer.ComputeDistanceTable(query, dist_table.data());
    return false;
}

template <typename QuantizerT>
inline float DistanceForQuantizedCode(const QuantizerT &quantizer, const std::vector<uint8_t> &flat_codes,
                                      size_t code_index, const std::vector<float> &dist_table) {
    const size_t code_offset = code_index * quantizer.CodeSize();
    const uint8_t *code = flat_codes.data() + code_offset;
    return QuantizerT::DistanceFromTable(code, dist_table.data(), quantizer.m);
}

template <typename QuantizerT, typename DistancerT>
inline float DistanceForQuantizedCode(const QuantizerT &quantizer, const std::vector<uint8_t> &flat_codes,
                                      size_t code_index, const std::vector<float> &dist_table,
                                      const DistancerT *distancer) {
    const size_t code_offset = code_index * quantizer.CodeSize();
    const uint8_t *code = flat_codes.data() + code_offset;
    if (distancer != nullptr) {
        return distancer->DistanceSingle(code);
    }
    return QuantizerT::DistanceFromTable(code, dist_table.data(), quantizer.m);
}

template <typename CandidateT, typename DistanceFn, typename RowIdFn>
inline void RefineAndSortCandidates(std::vector<CandidateT> &candidates, DistanceFn &&exact_distance,
                                    RowIdFn &&row_id_of) {
    for (auto &candidate : candidates) {
        candidate.distance = exact_distance(candidate);
    }

    std::sort(candidates.begin(), candidates.end(), [&](const CandidateT &lhs, const CandidateT &rhs) {
        if (lhs.distance != rhs.distance) {
            return lhs.distance < rhs.distance;
        }
        return row_id_of(lhs) < row_id_of(rhs);
    });
}

} // namespace vex

#endif // VEX_ADAPTER_QUANT_RUNTIME_HPP
