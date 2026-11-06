#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace vex_duck {
struct ConnectivityRepairStats {
    size_t reachable_before=0, reachable_after=0, repaired_edges=0, fallback_scans=0;
};

// Run after bulk-build workers have joined, before publishing the index. A
// spanning tree protects every previously reachable node while missing base
// layer components are linked. Node degrees and stored vectors do not grow.
template<class Store,class FindNeighbors,class Distance>
ConnectivityRepairStats RepairBaseConnectivity(Store&store,typename Store::T root,
                                               FindNeighbors find_neighbors,Distance distance){
    using T=typename Store::T;
    constexpr T invalid=std::numeric_limits<T>::max();
    ConnectivityRepairStats stats;
    // ReserveCapacity pre-sizes elems for every input row. Deduplication can
    // leave its tail unused; only IDs actually assigned by the joined workers
    // are graph nodes. Connecting reserved slots invents empty components.
    const size_t n=store.next_base_id_.load(std::memory_order_acquire),slots=size_t(store.m)*2;
    if(n>store.elems.size())throw std::logic_error("Graph node count exceeds reserved slots during connectivity check");
    if(!n)return stats;
    if(size_t(root)>=n||!slots)throw std::logic_error("Invalid graph root or degree during connectivity check");
    std::vector<T>parent(n,invalid),queue;
    queue.reserve(n);
    auto visit=[&](T first){
        queue.clear();queue.push_back(first);
        for(size_t at=0;at<queue.size();++at){
            T from=queue[at];auto*neighbors=std::get<0>(store.template get_point_info<true>(from));
            for(size_t j=0;j<slots;++j){
                T to=neighbors[j];
                if(to==invalid)continue;
                if(size_t(to)>=n)throw std::logic_error("Invalid neighbor during graph connectivity check");
                if(parent[to]==invalid){parent[to]=from;queue.push_back(to);}
            }
        }
        return queue.size();
    };
    parent[root]=root;
    stats.reachable_before=stats.reachable_after=visit(root);
    auto victim_slot=[&](T from){
        auto*neighbors=std::get<0>(store.template get_point_info<true>(from));
        int chosen=-1;float worst=-std::numeric_limits<float>::infinity();
        for(size_t j=0;j<slots;++j){
            T to=neighbors[j];
            if(to==invalid)return int(j);
            if(size_t(to)>=n)throw std::logic_error("Invalid neighbor during graph connectivity repair");
            if(to!=root&&parent[to]==from)continue; // never remove a spanning-tree edge
            float score=distance(from,to);
            if(chosen<0||score>worst){chosen=int(j);worst=score;}
        }
        return chosen;
    };
    for(size_t i=0;i<n;++i){
        if(parent[i]!=invalid)continue;
        T target=T(i),source=invalid;int slot=-1;
        for(T candidate:find_neighbors(target)){
            if(size_t(candidate)>=n||parent[candidate]==invalid)continue;
            int available=victim_slot(candidate);
            if(available>=0){source=candidate;slot=available;break;}
        }
        if(source==invalid){
            ++stats.fallback_scans;float best=std::numeric_limits<float>::infinity();
            for(size_t j=0;j<n;++j){
                if(parent[j]==invalid)continue;
                int available=victim_slot(T(j));if(available<0)continue;
                float score=distance(target,T(j));
                if(source==invalid||score<best){source=T(j);slot=available;best=score;}
            }
        }
        if(source==invalid)throw std::logic_error("Cannot connect a graph component without removing its spanning tree");
        store.template set_neighbor<true>(source,static_cast<int16_t>(slot),target,target);
        parent[target]=source;
        stats.reachable_after+=visit(target);
        ++stats.repaired_edges;
    }
    return stats;
}
}
