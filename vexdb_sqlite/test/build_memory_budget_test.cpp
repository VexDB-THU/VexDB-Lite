#include "store/build_memory_budget.h"
#include <iostream>
#include <stdexcept>

#define CHECK(c) do { if (!(c)) throw std::runtime_error(#c); } while (0)

int main() {
    using vexdb_sqlite::BuildMemoryBudget;
    using vexdb_sqlite::ParseBuildMemoryLimit;
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    std::int64_t value = 17;
    for (const auto text : {"", "-1", "+1", "1k", " 1", "1 ",
                            "9223372036854775808", "18446744073709551615"}) {
        CHECK(!ParseBuildMemoryLimit(text, value));
        CHECK(value == 17);
    }
    CHECK(ParseBuildMemoryLimit("0", value) && value == 0);
    CHECK(ParseBuildMemoryLimit("9223372036854775807", value) && value == maximum);
    CHECK(BuildMemoryBudget(40 * 1024 * 1024, 0) == 160 * 1024 * 1024);
    CHECK(BuildMemoryBudget(1, 2 * 1024 * 1024) == 2 * 1024 * 1024);
    CHECK(BuildMemoryBudget(maximum, 2 * 1024 * 1024) == 2 * 1024 * 1024);
    CHECK(BuildMemoryBudget(0, 0) == 0);
    CHECK(BuildMemoryBudget(maximum / 4, 0) == maximum / 4 * 4);
    CHECK(BuildMemoryBudget(maximum / 4 + 1, 0) == maximum);
    CHECK(BuildMemoryBudget(maximum, 0) == maximum);
    std::cout << "Build memory budget: parsing, overflow, old default and independent budget passed\n";
}
