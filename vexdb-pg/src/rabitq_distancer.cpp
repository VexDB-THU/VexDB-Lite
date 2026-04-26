#include "pg_compat.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "floatvector.h"
#include "graph_index/graph_index.h"
#include "graph_index/graph_index_struct.h"
#include "rabitq/rabitq_distancer.h"

namespace rabitq {

namespace {

constexpr uint32 kClusterIdSize = sizeof(uint16);
constexpr uint32 kQueryBits = 4;

size_t GetQuantizerPayloadSize(int dim, int padded_dim)
{
    return static_cast<size_t>(4 * padded_dim / 8) +
           static_cast<size_t>(GRAPH_INDEX_RABITQ_NUM_CLUSTERS) * dim * sizeof(float) +
           static_cast<size_t>(GRAPH_INDEX_RABITQ_NUM_CLUSTERS) * padded_dim * sizeof(float);
}

size_t GetRandomBitsSize(int padded_dim)
{
    return static_cast<size_t>(4 * padded_dim / 8);
}

float L2DistanceSquared(const float *a, const float *b, int dim)
{
    float sum = 0.0f;
    for (int i = 0; i < dim; ++i) {
        const float diff = a[i] - b[i];
        sum += diff * diff;
    }
    return sum;
}

} // namespace

vex::Metric RabitqDistancer::to_core_metric(Metric metric)
{
    switch (metric) {
        case Metric::L2:
            return vex::Metric::L2;
        case Metric::INNER_PRODUCT:
        case Metric::FAST_COSINE:
            return vex::Metric::INNER_PRODUCT;
        default:
            return vex::Metric::L2;
    }
}

void RabitqDistancer::init_code_layout()
{
    cid_size = kClusterIdSize;
    bin_size = static_cast<uint32>(vex::RabitQBinCodeSize(padded_dim));
    ext_size = static_cast<uint32>(vex::RabitQExtCodeSize(padded_dim));
    code_len = static_cast<size_t>(cid_size) + bin_size + ext_size;
}

void RabitqDistancer::destroy()
{
    quantizer.reset();
    estimator.reset();
    rotated_query.clear();
    query_scalar_code.clear();
    query_binary_bits.clear();
    prepared = false;
}

void RabitqDistancer::train_centroids(FloatVectorArray samples, int dim, std::vector<float> &centroids)
{
    centroids.assign(static_cast<size_t>(GRAPH_INDEX_RABITQ_NUM_CLUSTERS) * dim, 0.0f);
    if (samples == nullptr || samples->length <= 0) {
        return;
    }

    const int cluster_count = std::min(samples->length, GRAPH_INDEX_RABITQ_NUM_CLUSTERS);
    for (int i = 0; i < cluster_count; ++i) {
        const float *src = FloatVectorArrayGet(samples, i % samples->length);
        std::memcpy(centroids.data() + static_cast<size_t>(i) * dim, src, static_cast<size_t>(dim) * sizeof(float));
    }

    if (samples->length <= cluster_count) {
        return;
    }

    std::vector<float> sums(static_cast<size_t>(GRAPH_INDEX_RABITQ_NUM_CLUSTERS) * dim, 0.0f);
    std::vector<int> counts(static_cast<size_t>(GRAPH_INDEX_RABITQ_NUM_CLUSTERS), 0);
    std::vector<int> assignment(static_cast<size_t>(samples->length), 0);

    for (int iter = 0; iter < 8; ++iter) {
        std::fill(sums.begin(), sums.end(), 0.0f);
        std::fill(counts.begin(), counts.end(), 0);

        for (int i = 0; i < samples->length; ++i) {
            const float *sample = FloatVectorArrayGet(samples, i);
            int best_cluster = 0;
            float best_dist = std::numeric_limits<float>::max();
            for (int c = 0; c < GRAPH_INDEX_RABITQ_NUM_CLUSTERS; ++c) {
                const float dist = L2DistanceSquared(sample, centroids.data() + static_cast<size_t>(c) * dim, dim);
                if (dist < best_dist) {
                    best_dist = dist;
                    best_cluster = c;
                }
            }
            assignment[static_cast<size_t>(i)] = best_cluster;
            counts[static_cast<size_t>(best_cluster)]++;
            float *sum = sums.data() + static_cast<size_t>(best_cluster) * dim;
            for (int d = 0; d < dim; ++d) {
                sum[d] += sample[d];
            }
        }

        for (int c = 0; c < GRAPH_INDEX_RABITQ_NUM_CLUSTERS; ++c) {
            if (counts[static_cast<size_t>(c)] == 0) {
                continue;
            }
            float *centroid = centroids.data() + static_cast<size_t>(c) * dim;
            const float *sum = sums.data() + static_cast<size_t>(c) * dim;
            const float inv = 1.0f / static_cast<float>(counts[static_cast<size_t>(c)]);
            for (int d = 0; d < dim; ++d) {
                centroid[d] = sum[d] * inv;
            }
        }
    }
}

void RabitqDistancer::train(Relation index, FloatVectorArray samples, int in_dim,
    Metric in_metric, bool need_norm, int parallel_workers, int maintenance_work_mem)
{
    (void)index;
    (void)need_norm;
    (void)parallel_workers;
    (void)maintenance_work_mem;

    dim = in_dim;
    padded_dim = vex::RabitQPaddedDim(dim);
    metric = (in_metric == Metric::FAST_COSINE) ? Metric::INNER_PRODUCT : in_metric;
    init_code_layout();

    quantizer.emplace(dim, padded_dim, to_core_metric(metric));
    quantizer->BuildRandomRotation();

    std::vector<float> centroids;
    train_centroids(samples, dim, centroids);
    std::copy(centroids.begin(), centroids.end(), quantizer->Centroids().begin());

    std::vector<float> rotated(static_cast<size_t>(padded_dim), 0.0f);
    auto &rotated_centroids = quantizer->RotatedCentroids();
    for (int i = 0; i < GRAPH_INDEX_RABITQ_NUM_CLUSTERS; ++i) {
        std::fill(rotated.begin(), rotated.end(), 0.0f);
        quantizer->Rotate(centroids.data() + static_cast<size_t>(i) * dim, rotated.data());
        std::memcpy(rotated_centroids.data() + static_cast<size_t>(i) * padded_dim,
                    rotated.data(),
                    static_cast<size_t>(padded_dim) * sizeof(float));
    }

    quantizer->SetRescalingFactor(vex::RabitQConstScalingFactor(padded_dim, vex::kRabitQExBits));
    estimator.emplace(padded_dim, to_core_metric(metric), quantizer->QueryRescalingFactor());
    estimator->SetQuantizer(&(*quantizer));
    rotated_query.assign(static_cast<size_t>(padded_dim), 0.0f);
    query_scalar_code.assign(static_cast<size_t>(padded_dim), 0);
    query_binary_bits.assign(static_cast<size_t>(padded_dim) * kQueryBits / 64, 0);
    prepared = false;
}

void RabitqDistancer::encode_binary_bits(const float *rotated_vec, const float *rotated_centroid, int padded_dim, char *bin_data)
{
    std::memset(bin_data, 0, static_cast<size_t>(vex::RabitQBinCodeSize(padded_dim)));
    auto *code_bits = reinterpret_cast<uint64 *>(bin_data);
    for (int i = 0; i < padded_dim; ++i) {
        if (rotated_vec[i] >= rotated_centroid[i]) {
            const int word = i / 64;
            const int bit = 63 - (i % 64);
            code_bits[word] |= (static_cast<uint64>(1) << bit);
        }
    }
}

void RabitqDistancer::encode_ext_bits(const float *rotated_vec, const float *rotated_centroid, int padded_dim, char *ext_data)
{
    std::vector<uint16_t> scalar_code;
    float delta = 0.0f;
    float vl = 0.0f;
    quantizer->QuantizeScalar(rotated_vec, rotated_centroid, vex::kRabitQExBits, scalar_code, delta, vl);

    std::memset(ext_data, 0, static_cast<size_t>(vex::RabitQExtCodeSize(padded_dim)));
    for (int i = 0; i < padded_dim; ++i) {
        ext_data[i] = static_cast<char>(scalar_code[static_cast<size_t>(i)] & 0xFF);
    }
}

void RabitqDistancer::compute_code(float *vec, char *code)
{
    std::vector<float> rotated(static_cast<size_t>(padded_dim), 0.0f);
    quantizer->Rotate(vec, rotated.data());
    const uint16 cluster_id = static_cast<uint16>(quantizer->ClosestCluster(vec));
    std::memcpy(code, &cluster_id, sizeof(cluster_id));

    char *bin_data = code + cid_size;
    char *ext_data = bin_data + bin_size;
    const float *rotated_centroid = quantizer->RotatedCentroids().data() + static_cast<size_t>(cluster_id) * padded_dim;
    encode_binary_bits(rotated.data(), rotated_centroid, padded_dim, bin_data);
    encode_ext_bits(rotated.data(), rotated_centroid, padded_dim, ext_data);
}

void RabitqDistancer::read_rabitq_data(Relation index, size_t rabitq_data_size, char *rabitq_data) const
{
    size_t copied = 0;
    BlockNumber block = qtcode_block;
    while (block != InvalidBlockNumber && copied < rabitq_data_size) {
        Buffer buf = ReadBuffer(index, block);
        Page page = BufferGetPage(buf);
        PageHeader phdr = reinterpret_cast<PageHeader>(page);
        const size_t used = phdr->pd_lower - SizeOfPageHeaderData;
        const size_t to_copy = std::min(used, rabitq_data_size - copied);
        if (to_copy > 0) {
            std::memcpy(rabitq_data + copied, reinterpret_cast<char *>(page) + SizeOfPageHeaderData, to_copy);
            copied += to_copy;
        }
        block = GRAPH_INDEX_PAGE_GET_OPAQUE(page)->nextblkno;
        ReleaseBuffer(buf);
    }

    if (copied != rabitq_data_size) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("could not read complete RaBitQ payload"),
             errdetail("expected=%zu actual=%zu", rabitq_data_size, copied)));
    }
}

void RabitqDistancer::load_rabitq(Relation index, void *metapage)
{
    auto *metap = static_cast<GraphIndexMetaPage>(metapage);
    dim = metap->dimension;
    padded_dim = vex::RabitQPaddedDim(dim);
    metric = (metap->metric == Metric::FAST_COSINE) ? Metric::INNER_PRODUCT : metap->metric;
    qtcode_block = metap->qtcode_block;
    init_code_layout();

    const size_t random_bits_size = GetRandomBitsSize(padded_dim);
    const size_t centroid_size = static_cast<size_t>(GRAPH_INDEX_RABITQ_NUM_CLUSTERS) * dim;
    const size_t rotated_centroid_size = static_cast<size_t>(GRAPH_INDEX_RABITQ_NUM_CLUSTERS) * padded_dim;
    const size_t payload_size = GetQuantizerPayloadSize(dim, padded_dim);

    std::vector<char> payload(payload_size, 0);
    read_rabitq_data(index, payload_size, payload.data());

    const char *ptr = payload.data();
    std::vector<uint8_t> random_bits(random_bits_size, 0);
    std::memcpy(random_bits.data(), ptr, random_bits_size);
    ptr += random_bits_size;

    std::vector<float> centroids(centroid_size, 0.0f);
    std::memcpy(centroids.data(), ptr, centroid_size * sizeof(float));
    ptr += centroid_size * sizeof(float);

    std::vector<float> rotated_centroids(rotated_centroid_size, 0.0f);
    std::memcpy(rotated_centroids.data(), ptr, rotated_centroid_size * sizeof(float));

    quantizer.emplace(dim, padded_dim, to_core_metric(metric));
    quantizer->Load(random_bits, centroids, rotated_centroids);
    quantizer->SetRescalingFactor(metap->quantizer_metainfo.get_rabitq_meta().query_rescaling_factor);

    estimator.emplace(padded_dim, to_core_metric(metric),
                      metap->quantizer_metainfo.get_rabitq_meta().query_rescaling_factor);
    estimator->SetQuantizer(&(*quantizer));
    rotated_query.assign(static_cast<size_t>(padded_dim), 0.0f);
    query_scalar_code.assign(static_cast<size_t>(padded_dim), 0);
    query_binary_bits.assign(static_cast<size_t>(padded_dim) * kQueryBits / 64, 0);
    prepared = false;
}

void RabitqDistancer::prepare(Relation index, void *metap)
{
    load_rabitq(index, metap);
}

void RabitqDistancer::process(const char *query)
{
    if (!quantizer.has_value() || !estimator.has_value()) {
        ereport(ERROR, (errmsg("RaBitQ distancer is not initialized")));
    }

    estimator->PrepareQuery(reinterpret_cast<const float *>(query), static_cast<uint32_t>(dim));
    rotated_query = estimator->RotatedQuery();
    prepared = true;
}

void RabitqDistancer::flush(Relation index, BlockNumber in_qtcode_block, bool enabling)
{
    qtcode_block = in_qtcode_block;
    if (!quantizer.has_value()) {
        ereport(ERROR, (errmsg("RaBitQ quantizer is not trained")));
    }

    const size_t random_bits_size = quantizer->Rotator().RandomBitsSize();
    const size_t centroid_count = static_cast<size_t>(GRAPH_INDEX_RABITQ_NUM_CLUSTERS) * dim;
    const size_t rotated_centroid_count = static_cast<size_t>(GRAPH_INDEX_RABITQ_NUM_CLUSTERS) * padded_dim;
    const size_t payload_size = GetQuantizerPayloadSize(dim, padded_dim);

    std::vector<char> payload(payload_size, 0);
    char *ptr = payload.data();
    std::memcpy(ptr, quantizer->Rotator().RandomBits().data(), random_bits_size);
    ptr += random_bits_size;
    std::memcpy(ptr, quantizer->Centroids().data(), centroid_count * sizeof(float));
    ptr += centroid_count * sizeof(float);
    std::memcpy(ptr, quantizer->RotatedCentroids().data(), rotated_centroid_count * sizeof(float));

    graph_index_store_qt_centroids(index, qtcode_block,
                                   reinterpret_cast<const float *>(payload.data()),
                                   payload_size,
                                   enabling);
}

float RabitqDistancer::decode_binary_distance(const uint64 *query_bits, const uint64 *code_bits, int padded_dim)
{
    const int words_per_plane = padded_dim / 64;
    float score = 0.0f;
    for (uint32 bit_idx = 0; bit_idx < kQueryBits; ++bit_idx) {
        for (int word = 0; word < words_per_plane; ++word) {
            const uint64 lhs = query_bits[static_cast<size_t>(bit_idx) * words_per_plane + word];
            const uint64 rhs = code_bits[word];
            score += static_cast<float>(__builtin_popcountll(lhs ^ rhs));
        }
    }
    return score;
}

float RabitqDistancer::decode_ext_distance(const char *query, const char *ext_data, int dim)
{
    float score = 0.0f;
    for (int i = 0; i < dim; ++i) {
        score += std::abs(query[i] - static_cast<unsigned char>(ext_data[i]));
    }
    return score;
}

namespace {

float ComputeQueryBinaryDistance(const std::vector<float> &rotated_query,
                                 const float *rotated_centroid,
                                 const uint64 *code_bits,
                                 int padded_dim)
{
    float score = 0.0f;
    for (int i = 0; i < padded_dim; ++i) {
        const bool query_bit = rotated_query[static_cast<size_t>(i)] >= rotated_centroid[i];
        const uint64 mask = static_cast<uint64>(1) << (63 - (i % 64));
        const bool code_bit = (code_bits[i / 64] & mask) != 0;
        if (query_bit != code_bit) {
            score += 1.0f;
        }
    }
    return score;
}

float ComputeQueryExtDistance(const vex::RabitQQuantizer &quantizer,
                              const std::vector<float> &rotated_query,
                              const float *rotated_centroid,
                              const char *ext_data,
                              int padded_dim)
{
    std::vector<uint16_t> query_codes;
    float delta = 0.0f;
    float vl = 0.0f;
    quantizer.QuantizeScalar(rotated_query.data(), rotated_centroid, vex::kRabitQExBits, query_codes, delta, vl);

    float score = 0.0f;
    for (int i = 0; i < padded_dim; ++i) {
        score += std::abs(static_cast<int>(query_codes[static_cast<size_t>(i)]) -
                          static_cast<int>(static_cast<unsigned char>(ext_data[i])));
    }
    return score;
}

} // namespace

float RabitqDistancer::get_distance_est_single(const void *x, const void *y, uint16 in_dim) const
{
    (void)x;
    (void)in_dim;
    const char *quant_data = static_cast<const char *>(y);
    const uint16 cluster_id = *reinterpret_cast<const uint16 *>(quant_data);
    const char *bin_data = quant_data + cid_size;
    const float *rotated_centroid =
        quantizer->RotatedCentroids().data() + static_cast<size_t>(cluster_id) * padded_dim;
    rec.low_dist = ComputeQueryBinaryDistance(rotated_query,
                                              rotated_centroid,
                                              reinterpret_cast<const uint64 *>(bin_data),
                                              padded_dim);
    rec.est_dist = rec.low_dist;
    return rec.low_dist;
}

float RabitqDistancer::get_distance_single(const void *x, const void *y, uint16 in_dim) const
{
    (void)x;
    (void)in_dim;
    const char *quant_data = static_cast<const char *>(y);
    const uint16 cluster_id = *reinterpret_cast<const uint16 *>(quant_data);
    const char *bin_data = quant_data + cid_size;
    const char *ext_data = bin_data + bin_size;
    const float *rotated_centroid =
        quantizer->RotatedCentroids().data() + static_cast<size_t>(cluster_id) * padded_dim;
    rec.low_dist = ComputeQueryBinaryDistance(rotated_query,
                                              rotated_centroid,
                                              reinterpret_cast<const uint64 *>(bin_data),
                                              padded_dim);
    rec.est_dist = rec.low_dist +
                   ComputeQueryExtDistance(*quantizer,
                                           rotated_query,
                                           rotated_centroid,
                                           ext_data,
                                           padded_dim);
    return rec.est_dist;
}

double RabitqDistancer::get_query_rescaling_factor() const
{
    if (!quantizer.has_value()) {
        return 0.0;
    }
    return quantizer->QueryRescalingFactor();
}

float RabitqDistancer::get_distance_precise(const void *x, const void *y, uint16 in_dim) const
{
    Metric exact_metric = metric == Metric::FAST_COSINE ? Metric::INNER_PRODUCT : metric;
    ann_helper::distance_func func = ann_helper::get_general_distance_func(exact_metric, in_dim);
    return func(reinterpret_cast<const float *>(x), reinterpret_cast<const float *>(y), in_dim);
}

} /* namespace rabitq */
