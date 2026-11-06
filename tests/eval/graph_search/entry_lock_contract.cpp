#if defined(PG_VEXDB_TARGET_DUCK)
#include "vex_graph_index_depend_duck.hpp"
#else
#include "store/vex_graph_index_depend_sqlite.hpp"
#endif
#include <atomic>
#include <iostream>
#include <thread>
#include <vector>

int main(){
    MemStore<> store(4,8,16);
    store.entry_info.set(0,0,0);
    auto snapshot=store.template get_entry<false>();
    if(snapshot.first.id!=0)return 1;
#if defined(PG_VEXDB_TARGET_DUCK)
    bool released=LWLockConditionalAcquire(&store.entry_lock,LW_EXCLUSIVE);
    if(released)LWLockRelease(&store.entry_lock);
#else
    bool released=store.entry_mutex_.try_lock();
    if(released)store.entry_mutex_.unlock();
#endif
    if(!released){store.release_entry_lock(true);std::cerr<<"query snapshot retained its entry lock\n";return 1;}
    auto promoted=store.template get_entry<true>(GRAPH_INDEX_MAX_LEVEL+1);
    store.release_entry_lock(promoted.second);
    if(promoted.second){std::cerr<<"higher-level insertion did not acquire exclusive entry lock\n";return 1;}
    auto forced_shared=store.template get_entry<true,true>(GRAPH_INDEX_MAX_LEVEL+1);
    store.release_entry_lock(forced_shared.second);
    if(!forced_shared.second){std::cerr<<"force-share flag did not retain shared mode\n";return 1;}
    std::atomic<int>inside{0},bad{0};std::vector<std::thread>workers;
    for(int t=0;t<4;++t)workers.emplace_back([&]{for(int i=0;i<100;++i){
        auto held=store.template get_entry<true>(GRAPH_INDEX_MAX_LEVEL+1);
        const auto previous=inside.fetch_add(1);
        if(held.second||previous!=0)++bad;
        std::this_thread::yield();--inside;store.release_entry_lock(held.second);
        auto view=store.template get_entry<false>();if(view.first.id!=0)++bad;
    }});
    for(auto&w:workers)w.join();
    if(bad){std::cerr<<"entry lock mutual exclusion failed\n";return 1;}
    std::cout<<"PASS entry snapshots release locks, higher levels upgrade,400 concurrent writer/reader rounds\n";
}
