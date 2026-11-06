#include "vex_graph_connectivity.hpp"
#include <cassert>
#include <iostream>
struct Store {
    using T=uint32_t;
    int m=1;
    std::vector<int> elems;
    std::atomic<T> next_base_id_;
    std::vector<std::vector<T>> edges;
    explicit Store(size_t n):elems(n),next_base_id_(T(n)),edges(n,std::vector<T>(2,UINT32_MAX)){}
    template<bool>auto get_point_info(T id){return std::make_tuple(edges[id].data(),id,id);}
    template<bool>void set_neighbor(T id,int16_t slot,T to,T){edges[id][slot]=to;}
};
size_t Reachable(Store&s){
    if(s.elems.empty())return 0;std::vector<bool>seen(s.elems.size());std::vector<uint32_t>q{0};seen[0]=true;
    for(size_t i=0;i<q.size();++i)for(auto to:s.edges[q[i]])if(to<s.elems.size()&&!seen[to]){seen[to]=true;q.push_back(to);}
    return q.size();
}
int main(){
    auto distance=[](uint32_t a,uint32_t b){return float(a>b?a-b:b-a);};
    auto none=[](uint32_t){return std::vector<uint32_t>{};};
    {Store s(0);auto st=vex_duck::RepairBaseConnectivity(s,0,none,distance);assert(st.reachable_after==0);}
    {Store s(1);auto old=s.edges;auto st=vex_duck::RepairBaseConnectivity(s,0,none,distance);assert(st.reachable_after==1&&st.repaired_edges==0&&s.edges==old);}
    {Store s(20000);s.next_base_id_.store(3);s.edges[0][0]=1;s.edges[1][0]=2;auto old=s.edges;size_t calls=0;
     auto checked_distance=[&](uint32_t a,uint32_t b){++calls;assert(a<3&&b<3);return distance(a,b);};
     auto st=vex_duck::RepairBaseConnectivity(s,0,none,checked_distance);assert(st.reachable_after==3&&st.repaired_edges==0&&calls==0&&s.edges==old);}
    {Store s(20000);s.next_base_id_.store(0);auto st=vex_duck::RepairBaseConnectivity(s,0,none,distance);assert(st.reachable_after==0&&st.repaired_edges==0);}
    {Store s(20000);s.next_base_id_.store(3);s.edges[1][0]=2;s.edges[2][0]=1;
     auto checked_distance=[&](uint32_t a,uint32_t b){assert(a<3&&b<3);return distance(a,b);};
     auto st=vex_duck::RepairBaseConnectivity(s,0,none,checked_distance);assert(st.reachable_before==1&&st.reachable_after==3&&st.repaired_edges==1);for(size_t i=3;i<s.edges.size();++i)assert(s.edges[i][0]==UINT32_MAX&&s.edges[i][1]==UINT32_MAX);}
    {Store s(2);s.next_base_id_.store(3);bool rejected=false;try{vex_duck::RepairBaseConnectivity(s,0,none,distance);}catch(const std::logic_error&){rejected=true;}assert(rejected);}
    {Store s(2);s.edges={{0,0},{1,1}};auto st=vex_duck::RepairBaseConnectivity(s,0,none,distance);assert(st.reachable_after==2&&st.repaired_edges==1&&Reachable(s)==2);}
    {Store s(3);s.edges[0][0]=1;s.edges[1][0]=2;auto old=s.edges;auto st=vex_duck::RepairBaseConnectivity(s,0,none,distance);assert(st.reachable_after==3&&st.repaired_edges==0&&s.edges==old);}
    {Store s(5);s.edges={{1,2},{0,2},{0,1},{4,UINT32_MAX},{3,UINT32_MAX}};auto st=vex_duck::RepairBaseConnectivity(s,0,none,distance);assert(st.reachable_before==3&&st.reachable_after==5&&st.repaired_edges==1&&st.fallback_scans==1);assert(s.edges[0][0]==1&&s.edges[0][1]==2&&Reachable(s)==5);}
    {Store s(8);s.edges={{1,2},{0,2},{0,1},{4,5},{3,5},{3,4},{7,UINT32_MAX},{6,UINT32_MAX}};auto neighbors=[](uint32_t){return std::vector<uint32_t>{0,1,2};};auto st=vex_duck::RepairBaseConnectivity(s,0,neighbors,distance);assert(st.reachable_before==3&&st.reachable_after==8&&st.repaired_edges==2&&Reachable(s)==8);assert(s.edges[0][0]==1&&s.edges[0][1]==2);for(auto&e:s.edges)assert(e.size()==2);}
    {Store s(2);s.edges[0][0]=99;bool rejected=false;try{vex_duck::RepairBaseConnectivity(s,0,none,distance);}catch(const std::logic_error&){rejected=true;}assert(rejected);}
    std::cout<<"PASS connectivity repair: empty/single/connected graphs, reserved unused slots, disconnected components, protected tree edges, degree bound and invalid references\n";
}
