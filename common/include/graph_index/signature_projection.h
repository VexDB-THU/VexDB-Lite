#ifndef VEXDB_SIGNATURE_PROJECTION_H
#define VEXDB_SIGNATURE_PROJECTION_H
// Structured sign codes. All buffers belong to the caller.
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <algorithm>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
namespace signature_rescue {
constexpr unsigned rounds = 3;
inline uint64_t Next(uint64_t &state) {
    uint64_t z=(state+=0x9e3779b97f4a7c15ULL);
    z=(z^(z>>30))*0xbf58476d1ce4e5b9ULL;
    z=(z^(z>>27))*0x94d049bb133111ebULL;
    return z^(z>>31);
}
inline size_t Padded(size_t dimension,size_t max_bits) {
    size_t n=1;while(n<std::max(dimension,max_bits))n*=2;return n;
}
inline void Initialize(uint64_t seed,size_t padded,int8_t *signs,uint32_t *order) {
    for(size_t i=0;i<rounds*padded;++i)signs[i]=(Next(seed)&1)?1:-1;
    for(size_t i=0;i<padded;++i)order[i]=uint32_t(i);
    for(size_t i=padded;i>1;--i)std::swap(order[i-1],order[Next(seed)%i]);
}
inline bool Encode(const float *input,const float *center,size_t dim,size_t padded,
                   size_t bits,const int8_t *signs,const uint32_t *order,
                   float *scratch,uint64_t *out,float &norm) {
    if(!bits||bits%64||bits>padded)return false;
    double sum=0;
    for(size_t i=0;i<dim;++i){scratch[i]=input[i]-center[i];sum+=double(scratch[i])*scratch[i];}
    norm=float(std::sqrt(sum));if(!std::isfinite(norm))return false;
    std::fill(scratch+dim,scratch+padded,0.f);
    const float scale=1.f/std::sqrt(float(padded));
    for(unsigned r=0;r<rounds;++r) {
        for(size_t i=0;i<padded;++i)scratch[i]*=signs[r*padded+i];
        for(size_t stride=1;stride<padded;stride*=2)
            for(size_t i=0;i<padded;i+=stride*2)
                for(size_t j=0;j<stride;++j){float x=scratch[i+j],y=scratch[i+j+stride];scratch[i+j]=x+y;scratch[i+j+stride]=x-y;}
        for(size_t i=0;i<padded;++i)scratch[i]*=scale;
    }
    std::fill(out,out+bits/64,uint64_t(0));
    for(size_t bit=0;bit<bits;++bit)if(scratch[order[bit]]>0)out[bit/64]|=uint64_t(1)<<(bit%64);
    return true;
}
inline unsigned Hamming(const uint64_t *left,const uint64_t *right,size_t words) {
    unsigned distance=0;size_t i=0;
#if defined(__aarch64__)
    for(;i+2<=words;i+=2) {
        auto x=veorq_u64(vld1q_u64(left+i),vld1q_u64(right+i));
        distance+=vaddvq_u8(vcntq_u8(vreinterpretq_u8_u64(x)));
    }
#endif
    for(;i<words;++i)distance+=unsigned(__builtin_popcountll(left[i]^right[i]));return distance;
}
inline void Cosines(size_t bits,float *lut) {
    for(size_t i=0;i<=bits;++i)lut[i]=float(std::cos(3.14159265358979323846*double(i)/bits));
}
inline float Estimate(float query_norm,float norm,unsigned hamming,const float *lut) {
    return query_norm*query_norm+norm*norm-2.f*query_norm*norm*lut[hamming];
}
// Packed records put the code immediately after a float norm. Use memcpy so
// neither a 4-byte offset nor a host BLOB requires uint64 alignment.
inline unsigned HammingBytes(const uint64_t *left,const void *right,size_t words) {
    const auto *bytes=static_cast<const unsigned char*>(right);
    unsigned distance=0;size_t i=0;
#if defined(__aarch64__)
    for(;i+2<=words;i+=2) {
        uint64x2_t r;std::memcpy(&r,bytes+i*8,16);
        auto x=veorq_u64(vld1q_u64(left+i),r);
        distance+=vaddvq_u8(vcntq_u8(vreinterpretq_u8_u64(x)));
    }
#endif
    for(;i<words;++i){uint64_t r;std::memcpy(&r,bytes+i*8,8);distance+=unsigned(__builtin_popcountll(left[i]^r));}
    return distance;
}
}
#endif
