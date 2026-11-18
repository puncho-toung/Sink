#pragma once
#include <cstdint>
#include <stdexcept>
#include <vector>
namespace sn {
using U=uint32_t; using W=uint64_t;
constexpr U NONE=UINT32_MAX, INF=UINT32_MAX;
struct Work {
 W units=0;
 template<class T> T E(T x){Add(1);return x;}
 bool C(bool x){Add(1);return x;}
 template<class T,class I> void P(const T&,I){Add(1);}
 template<class T,class V> void Copy(T&dst,V x){Add(1);dst=T(x);}
 template<class T> T R(const std::vector<T>&v,U i){Add(3);return v[i];} // E index, P, E value
 template<class T,class V> void Set(std::vector<T>&v,U i,V x){Add(3);v[i]=T(x);} // E index,P,W; caller E(rhs)
 void Add(W n){if(n>UINT64_MAX-units)throw std::runtime_error("work counter width refused");units+=n;}
};
}
