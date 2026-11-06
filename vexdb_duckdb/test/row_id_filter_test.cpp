#include "vex_row_id_filter.hpp"
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>
#include <limits>
int main() {
    using Row=int64_t;
    std::mt19937_64 rng(3921029);
    size_t checks=0;
    for (int round=0; round<100; ++round) {
        std::vector<Row> rows;
        for (int i=0;i<1000;++i) rows.push_back(round%2? Row(rng()%1000000000):Row(rng()%3000));
        if(round%3==0)rows.push_back(std::numeric_limits<Row>::max());
        if(round%5==0)rows.push_back(std::numeric_limits<Row>::min());
        if(round%7==0)rows.clear();
        const std::unordered_set<Row> truth(rows.begin(),rows.end());
        const vex_duck::RowIdFilter<Row> filter(rows);
        for(auto r:rows)if(!filter.Contains(r))throw std::runtime_error("missing member");
        for(int i=0;i<10000;++i) {
            Row r=i<4000?i:Row(rng());
            if(filter.Contains(r)!=(truth.count(r)!=0))throw std::runtime_error("membership mismatch");
            ++checks;
        }
        if(filter.DenseBytes()>rows.size()*sizeof(Row))throw std::runtime_error("dense byte budget");
    }
    std::cout << "PASS " << checks << " membership probes, dense/sparse/empty/duplicates/signed limits\n";
}
