#pragma once
// Cover's choice of the next waypoint (census.cpp): the first area that holds the largest gain.
// census.cpp includes this file after its `using namespace s3c;`, which gives U (uint32_t) and NONE (UINT32_MAX);
// gain_test.cpp defines the two itself and includes the file alone.
#if defined(__SSE2__)||defined(_M_X64)
#include <emmintrin.h>
#endif
#include <queue>
#include <utility>
#include <vector>

// What the scan `U best=NONE,bg=0;for(U i=0;i<n;++i)if(gain[i]>bg){bg=gain[i];best=i;}` leaves in best: the first area
// with the largest gain, NONE when every gain is 0. Here four gains at a time are compared with the largest gain so
// far (bg, held in all four lanes of m); only when one of the four is above it does the scan's own update run for
// those four, in order, and m is filled again. In a block where no gain is above bg the scan changes neither bg nor
// best, so passing over it changes nothing; within a block the update is the scan's, so ties keep the first area.
// Gains must be below 2^31 (Cover refuses more areas than that): SSE2 compares them as signed numbers.
inline U LargestGain(const std::vector<U>&gain){
 const U n=U(gain.size());const U*g=gain.data();U bg=0,best=NONE,i=0;
#if defined(__SSE2__)||defined(_M_X64)
 __m128i m=_mm_setzero_si128();
 for(;i+4<=n;i+=4)if(_mm_movemask_epi8(_mm_cmpgt_epi32(_mm_loadu_si128((const __m128i*)(g+i)),m))){
  for(U k=i;k<i+4;++k)if(g[k]>bg){bg=g[k];best=k;}
  m=_mm_set1_epi32(int(bg));}
#endif
 for(;i<n;++i)if(g[i]>bg){bg=g[i];best=i;}
 return best;
}

// The same choice from a heap: the largest gain, then the smallest area. An entry is alive while it holds its area's
// present gain. A gain only ever falls, so a dead entry never comes alive again, and no area has two alive entries.
struct GainHeap{
 std::priority_queue<std::pair<U,U>>q;   // (gain, NONE - area): the largest pair is the largest gain, then the smallest area
 void Fill(const std::vector<U>&gain){for(U i=0;i<gain.size();++i)if(gain[i])q.push({gain[i],NONE-i});}
 U Top(const std::vector<U>&gain){while(!q.empty()){const U g=q.top().first,i=NONE-q.top().second;if(g==gain[i])return i;q.pop();}return NONE;}
 void Lower(U i,U g){if(g)q.push({g,NONE-i});}
};
