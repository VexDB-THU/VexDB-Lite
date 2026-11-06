// Independent scalar answers for direct, function-pointer and batched native
// distance calls. Exercise both the selected ISA and the portable fallback.
#include "duck_pg_shim.hpp"
#include "distance/core/distance_dispatcher.h"
#include "distance/core/distance_utils_core.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <vector>

template<class T>
static long double Reference(Metric metric, const T *a, const T *b, size_t dim) {
    long double squared=0,dot=0,an=0,bn=0;
    for(size_t i=0;i<dim;++i){const long double x=a[i],y=b[i];squared+=(x-y)*(x-y);dot+=x*y;an+=x*x;bn+=y*y;}
    if(metric==Metric::L2)return squared;
    if(metric==Metric::INNER_PRODUCT)return -dot;
    return an&&bn ? -dot/std::sqrt(an*bn) : 0;
}
static void Check(float actual,long double expected,const char *path,int dim,Metric metric,int bytes){
    const auto tolerance=4e-5L*std::max(1.0L,std::abs(expected));
    if(!std::isfinite(actual)||std::abs(actual-expected)>tolerance){
        std::cerr<<path<<" dim="<<dim<<" metric="<<int(metric)<<" element_bytes="<<bytes
                 <<" actual="<<actual<<" expected="<<expected<<'\n';
        throw std::runtime_error("native distance contract mismatch");
    }
}
template<class T,class D>
static size_t Exercise(D &d,Metric metric,const std::vector<T>&q,const std::vector<T>&v,int dim){
    std::vector<void*>p;for(int i=0;i<11;++i)p.push_back(const_cast<T*>(v.data()+i*dim));
    float batch[11];d.get_distance_batch2(q.data(),p.data(),dim,11,batch);auto fn=&D::get_distance_single;
    for(int i=0;i<11;++i){auto expected=Reference(metric,q.data(),v.data()+i*dim,dim);
        Check(d.get_distance_single(q.data(),p[i],dim),expected,"direct",dim,metric,sizeof(T));
        Check(fn(q.data(),p[i],dim),expected,"pointer",dim,metric,sizeof(T));
        Check(batch[i],expected,"batch",dim,metric,sizeof(T));}
    return 33;
}
template<class T,DistPrecisionType dtype>
static size_t ExtraISAs(Metric metric,const std::vector<T>&q,const std::vector<T>&v,int dim){
    size_t checks=0;
    auto exercise=[&](auto arch_tag){
        constexpr Arch arch=decltype(arch_tag)::value;
        if(!ann_helper::is_arch_available(arch,metric,dtype))return;
        Distancer<arch,Metric::L2,dtype,RemainderSituation::Unknown,false>l2;
        Distancer<arch,Metric::INNER_PRODUCT,dtype,RemainderSituation::Unknown,false>ip;
        Distancer<arch,Metric::COSINE,dtype,RemainderSituation::Unknown,false>cos;
        if(metric==Metric::L2)checks+=Exercise(l2,metric,q,v,dim);
        else if(metric==Metric::INNER_PRODUCT)checks+=Exercise(ip,metric,q,v,dim);
        else checks+=Exercise(cos,metric,q,v,dim);
    };
#if COMPILER_TARGET_X86_64
    exercise(std::integral_constant<Arch,Arch::SSE>{});
    exercise(std::integral_constant<Arch,Arch::AVX>{});
    exercise(std::integral_constant<Arch,Arch::AVX512>{});
#elif COMPILER_TARGET_ARM
    exercise(std::integral_constant<Arch,Arch::NEONV8>{});
#endif
    return checks;
}
template<class T,DistPrecisionType dtype>
static size_t Run(bool extreme=false){
    size_t checks=0;
    const std::vector<int>dims=extreme ? std::vector<int>{65,129,1536,32769,65535} :
        std::vector<int>{1,3,4,5,7,8,9,15,16,17,31,32,33,63,64,65,127,128,129,1536};
    for(int dim:dims){
        std::vector<T>q(dim),v(dim*11);
        for(int d=0;d<dim;++d){
            if constexpr(std::is_same_v<T,float>)q[d]=std::sin(float(d+1)*.117f);
            else q[d]=T((d*71+17)%256-128);
        }
        for(int i=0;i<11;++i)for(int d=0;d<dim;++d){
            if constexpr(std::is_same_v<T,float>)v[i*dim+d]=std::cos(float((i+1)*(d+1))*.073f);
            else v[i*dim+d]=T((i*79+d*37+31)%256-128);
        }
        if(extreme){
            std::fill(q.begin(),q.end(),T(-128));
            const int values[]={127,-128,0,-1,1,64,-64,126,-127,63,-63};
            for(int i=0;i<11;++i)std::fill(v.begin()+i*dim,v.begin()+(i+1)*dim,T(values[i]));
        }
        for(auto metric:{Metric::L2,Metric::INNER_PRODUCT,Metric::COSINE}){
            DispatchRunner<false,MetricList<Metric::L2,Metric::INNER_PRODUCT,Metric::COSINE>,
                DistPrecisionTypeList<dtype>,DispatcherMode::NO_QUANT>::call(
                metric,dtype,dim,QuantizerType::NONE,[&](auto&d){checks+=Exercise(d,metric,q,v,dim);});
            checks+=ExtraISAs<T,dtype>(metric,q,v,dim);
        }
        Distancer<Arch::GENERAL,Metric::L2,dtype,RemainderSituation::Unknown,false>l2;
        Distancer<Arch::GENERAL,Metric::INNER_PRODUCT,dtype,RemainderSituation::Unknown,false>ip;
        Distancer<Arch::GENERAL,Metric::COSINE,dtype,RemainderSituation::Unknown,false>cos;
        checks+=Exercise(l2,Metric::L2,q,v,dim)+Exercise(ip,Metric::INNER_PRODUCT,q,v,dim)+Exercise(cos,Metric::COSINE,q,v,dim);
    }
    return checks;
}
int main(){const auto checks=Run<float,DistPrecisionType::FLOAT>()+Run<int8_t,DistPrecisionType::INT8>()+
    Run<int8_t,DistPrecisionType::INT8>(true);
    std::cout<<"PASS "<<checks<<" distance checks across3 metrics,2 precisions,all available ISAs and byte extremes through65535 dimensions\n";}
