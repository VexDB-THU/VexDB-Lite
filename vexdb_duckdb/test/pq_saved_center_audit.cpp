// Validate actual persisted codebooks against their full original input.
// This does not retrain the model or measure search performance.
#include "quantizer/annkmeans.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

using namespace vex::quantizer;
static std::string Quote(const std::string &s) {
    std::string out = "\"";
    for (auto c : s) { if (c == '"' || c == '\\') out += '\\'; out += c; }
    return out + "\"";
}
int main(int argc, char **argv) {
    if (argc != 7 || std::filesystem::exists(argv[6])) return 2;
    try {
        const size_t n = std::stoull(argv[3]), dim = std::stoull(argv[4]), m = std::stoull(argv[5]);
        if (!n || !dim || !m || dim % m) throw std::runtime_error("invalid layout");
        const size_t dsub = dim / m, k = 256;
        if (std::filesystem::file_size(argv[1]) != n * dim * sizeof(float))
            throw std::runtime_error("base size mismatch");
        std::ifstream input(argv[1], std::ios::binary), book(argv[2], std::ios::binary);
        uint64_t count = 0;
        book.read(reinterpret_cast<char *>(&count), sizeof(count));
        if (count != k * dim || std::filesystem::file_size(argv[2]) != sizeof(count) + count * sizeof(float))
            throw std::runtime_error("codebook size mismatch");
        std::vector<float> base(n * dim), centers(count), packed(n * dsub);
        if (!input.read(reinterpret_cast<char *>(base.data()), base.size() * sizeof(float)) ||
            !book.read(reinterpret_cast<char *>(centers.data()), centers.size() * sizeof(float)))
            throw std::runtime_error("truncated input");
        KMeansState policy;
        policy.allow_low_diversity_centers = true;
        size_t errors = 0, allowed = 0;
        std::ofstream out(argv[6]);
        out << "{\"rows\":" << n << ",\"dim\":" << dim << ",\"m\":" << m << ",\"subspaces\":[";
        for (size_t sub = 0; sub < m; ++sub) {
            for (size_t row = 0; row < n; ++row)
                std::copy_n(base.data() + row * dim + sub * dsub, dsub, packed.data() + row * dsub);
            PQFloatArray samples {packed.data(), n, n, dsub};
            PQFloatArray trained {centers.data() + sub * k * dsub, k, k, dsub};
            std::string error;
            try { ValidateKMeansCenters(policy, samples, trained); }
            catch (const VexQuantizerError &e) { error = e.what(); ++errors; }
            std::set<std::vector<float>> distinct;
            for (size_t c = 0; c < k; ++c) distinct.emplace(trained.Get(c), trained.Get(c) + dsub);
            const bool low_diversity = distinct.size() < k && error.empty();
            allowed += low_diversity;
            if (sub) out << ',';
            out << "{\"subspace\":" << sub << ",\"unique_centers\":" << distinct.size()
                << ",\"allowed_low_diversity\":" << (low_diversity ? "true" : "false")
                << ",\"error\":" << Quote(error) << '}';
        }
        out << "],\"errors\":" << errors << ",\"allowed_low_diversity_subspaces\":" << allowed
            << ",\"passed\":" << (errors ? "false" : "true") << "}\n";
        std::cout << (errors ? "FAIL " : "PASS ") << m << " saved subspaces; allowed low diversity=" << allowed
                  << " unexpected errors=" << errors << '\n';
        return errors ? 1 : 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
