#pragma once
// Shared S4a level builder. Metrics and cover are inherited from the census.
#define main s4walk_single_level_main
#include "census.cpp"
#undef main
namespace s4walk {
struct Level {
 U cap=0;
 std::vector<U> wp;
 std::vector<Attachments> out,in;
 std::vector<PathMetric> middle;
};
inline Level BuildLevel(const Fixture4&f,const NativeGeometry&geo,const std::vector<U>&active,U cap){
 const double begin=omp_get_wtime();Level l;l.cap=cap;l.wp=Cover(f,geo,active,cap);const U n=active.size(),m=l.wp.size();
 std::vector<U>index(f.g.n,NONE);for(U i=0;i<n;++i)index[active[i]]=i;
 l.out.resize(n);l.in.resize(n);
 const U batch=std::min(8,omp_get_max_threads());
 struct Records {std::vector<std::pair<U,Attachment>>out,in;};std::vector<Records>records(batch);
 std::vector<std::unique_ptr<NativeFlood>>floods;for(U b=0;b<batch;++b)floods.emplace_back(new NativeFlood(f,geo));
 for(U base=0;base<m;base+=batch){U end=std::min(m,base+batch);
  #pragma omp parallel for schedule(static)
  for(U w=base;w<end;++w){auto&d=*floods[w-base];auto&r=records[w-base];r.out.clear();r.in.clear();
   d.To(l.wp[w],true,2*cap);for(U a:d.order)if(d.time[a]<=2*cap)r.out.push_back({index[a],{Extract(d,a,true,geo),w,d.time[a]}});
   d.From(l.wp[w],true,2*cap);r.in.push_back({index[l.wp[w]],{{0,0,NONE,NONE},w,0}});for(U a:d.order)if(a!=l.wp[w]&&d.time[a]<=2*cap)r.in.push_back({index[a],{Extract(d,a,false,geo),w,d.time[a]}});
  }
  for(U w=base;w<end;++w){for(auto&r:records[w-base].out)Insert(l.out[r.first],r.second);for(auto&r:records[w-base].in)Insert(l.in[r.first],r.second);}
 }
 W att[4]={};for(U a=0;a<n;++a){Need(l.out[a].count&&l.in[a].count,"level attachment coverage absent");for(U k=0;k<4;++k)att[k]+=std::min(KS[k],l.out[a].count)+std::min(KS[k],l.in[a].count);}
 for(U k=0;k<4;++k)printf("M HIER_ATTACH cap=%u K=%u records=%llu perEnabled=%.12f\n",cap,KS[k],(unsigned long long)att[k],double(att[k])/n);
 l.middle.resize(size_t(m)*m);std::atomic<U>done{0};
 #pragma omp parallel
 {
  NativeFlood d(f,geo);std::vector<U>last(f.g.edges.size(),NONE);
  #pragma omp for schedule(dynamic,1)
  for(U goal=0;goal<m;++goal){d.To(l.wp[goal],false);
   for(U a:d.order)for(U j=f.g.in[a];j<f.g.in[a+1];++j){U e=f.g.rev[j];if(!f.g.enabled[f.g.edges[e].from])continue;last[e]=d.policy[e]==NONE?e:last[d.policy[e]];}
   for(U s=0;s<m;++s){U a=l.wp[s];if(d.time[a]==BIG)continue;U first=d.endpoint[a];l.middle[size_t(s)*m+goal]={d.length[a],d.time[a],first,first==NONE?NONE:last[first]};}
   U completed=++done;if(completed%2048==0||completed==m){
    #pragma omp critical(s4walk_level_output)
    printf("M HIER_MIDDLE cap=%u completed=%u total=%u seconds=%.6f\n",cap,completed,m,omp_get_wtime()-begin);
   }
  }
 }
 printf("M HIER_LEVEL_READY cap=%u waypoints=%u enabled=%u seconds=%.6f\n",cap,m,n,omp_get_wtime()-begin);return l;
}
} // namespace s4walk
