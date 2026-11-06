#pragma once

#include <algorithm>
#include <cstddef>
#include <exception>
#include <functional>
#include <thread>
#include <vector>

namespace vex_duck {

// Bound the number of live K-means scratch arrays, while preserving each
// subquantizer's fresh-thread RNG from the existing training path.
class PQTrainingExecutor {
public:
    using Task = std::function<void(size_t)>;

    explicit PQTrainingExecutor(size_t workers) : workers_(std::max<size_t>(1, workers)) {}

    static void Run(size_t n, const Task &body, void *user) {
        auto &executor = *static_cast<PQTrainingExecutor *>(user);
        // Preserve the existing large-M serial path. Nested K-means batches
        // contain >=256 centers/samples and must not spawn nested workers.
        if (n <= 1 || n > 64) {
            for (size_t i = 0; i < n; ++i) body(i);
            return;
        }
        const size_t width = std::min(n, executor.workers_);
        for (size_t first = 0; first < n; first += width) {
            const size_t count = std::min(width, n - first);
            std::vector<std::thread> workers;
            std::vector<std::exception_ptr> errors(count);
            workers.reserve(count);
            try {
                for (size_t offset = 0; offset < count; ++offset) {
                    workers.emplace_back([&, offset] {
                        try { body(first + offset); }
                        catch (...) { errors[offset] = std::current_exception(); }
                    });
                }
            } catch (...) {
                for (auto &worker : workers) if (worker.joinable()) worker.join();
                throw;
            }
            for (auto &worker : workers) worker.join();
            for (const auto &error : errors) if (error) std::rethrow_exception(error);
        }
    }

private:
    size_t workers_;
};

} // namespace vex_duck
