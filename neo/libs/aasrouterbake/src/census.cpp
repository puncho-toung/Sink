// S4a-walk: exact native-metric port of consult-9/fable-scratch/levels.cpp.
// All floods are offline. No radius, flood, loop erasure or search in the query.
#include "native_metric.h"
#include "classification.h"
#include <atomic>
using namespace s3c;
constexpr int NB=11;
constexpr U RB[NB+1]={0,128,256,384,512,768,1024,1536,2048,3072,4096,BIG};
constexpr U KS[4]={1,2,4,8};
int Band(U t){int b=0;while(b+1<NB&&t>=RB[b+1])++b;return b;}
struct PathMetric {
 long double length=INFINITY;
 U graph=BIG,first=NONE,last=NONE;
};
struct Attachment {PathMetric p;U waypoint=NONE,native=BIG;};
struct Attachments {std::array<Attachment,8> a;U count=0;};
bool Before(const Attachment&a,const Attachment&b){return std::tie(a.native,a.p.length,a.waypoint)<std::tie(b.native,b.p.length,b.waypoint);}
void Insert(Attachments&v,const Attachment&a){U i=0;while(i<v.count&&!Before(a,v.a[i]))++i;if(i>=8)return;for(U j=std::min(v.count,7u);j>i;--j)v.a[j]=v.a[j-1];v.a[i]=a;v.count=std::min(v.count+1,8u);}
PathMetric Extract(const NativeFlood&d,U v,bool reverse,const NativeGeometry&geo){
 if(d.time[v]==BIG)return {};
 auto p=d.Path(v,reverse);
 return {d.length[v],geo.PathTime(p,false),p.empty()?NONE:p.front(),p.empty()?NONE:p.back()};
}
PathMetric Join(const PathMetric&a,const PathMetric&b,const NativeGeometry&geo){
 if(a.graph==BIG||b.graph==BIG)return {};
 auto m=geo.Concat({a.graph,a.length},a.last,{b.graph,b.length},b.first);
 return {m.l,m.t,a.first!=NONE?a.first:b.first,b.last!=NONE?b.last:a.last};
}
struct Candidate {long double length=INFINITY;U time=BIG,wa=NONE,wg=NONE;};
bool BetterCandidate(const Candidate&a,const Candidate&b){return std::tie(a.time,a.length,a.wa,a.wg)<std::tie(b.time,b.length,b.wa,b.wg);}
struct GoalStats {
 W fail[4][NB]={},tf[4]={},lf[4]={},pairs=0,unreachable=0;
 U farthest[4]={},maxTime=0;
};
W Mix(W x){x+=0x9e3779b97f4a7c15ULL;x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL;x=(x^(x>>27))*0x94d049bb133111ebULL;return x^(x>>31);}

std::vector<U> Cover(const Fixture4&f,const NativeGeometry&geo,const std::vector<U>&active,U cap){
 double begin=omp_get_wtime();U n=active.size();std::vector<std::vector<U>>cov(n);
 std::vector<U>index(f.g.n,NONE);for(U i=0;i<n;++i)index[active[i]]=i;
 #pragma omp parallel
 {
  NativeFlood to(f,geo),from(f,geo);
  #pragma omp for schedule(dynamic,16)
  for(U i=0;i<n;++i){U c=active[i];to.To(c,true,cap);from.From(c,true,cap);cov[i].push_back(i);
   for(U a:to.order)if(a!=c&&to.time[a]<=cap&&from.time[a]<=cap)cov[i].push_back(index[a]);
   std::sort(cov[i].begin(),cov[i].end());
  }
 }
 // Same deterministic lazy greedy as levels.cpp: maximum uncovered gain,
 // then smallest enabled area ID. Stale gains are upper bounds.
 std::vector<uint8_t>covered(n);std::vector<U>gain(n),wp;for(U i=0;i<n;++i)gain[i]=cov[i].size();U left=n;
 while(left){U best=NONE,bg=0;for(U i=0;i<n;++i)if(gain[i]>bg){bg=gain[i];best=i;}Need(best!=NONE,"cover lost an uncovered area");U real=0;for(U a:cov[best])real+=!covered[a];if(real!=gain[best]){gain[best]=real;continue;}
  wp.push_back(active[best]);for(U a:cov[best])if(!covered[a]){covered[a]=1;--left;}gain[best]=0;
 }
 std::sort(wp.begin(),wp.end());
 printf("M COVER cap=%u enabled=%u waypoints=%zu share=%.12f seconds=%.6f\n",cap,n,wp.size(),double(wp.size())/n,omp_get_wtime()-begin);
 return wp;
}

bool Census(const Fixture4&f,const NativeGeometry&geo,const std::vector<U>&active,U cap,U goalLimit){
 const double begin=omp_get_wtime();const U n=active.size();auto wp=Cover(f,geo,active,cap);const U m=wp.size();
 std::vector<U>index(f.g.n,NONE);for(U i=0;i<n;++i)index[active[i]]=i;
 std::vector<Attachments>out(n),in(n);
 // Each waypoint writes its own temporary records; merge in ascending waypoint
 // order, keeping the sorted nearest eight. Work and answer do not depend on jobs.
 const U batch=std::min(8,omp_get_max_threads());
 struct Records {std::vector<std::pair<U,Attachment>>out,in;};std::vector<Records>records(batch);
 std::vector<std::unique_ptr<NativeFlood>>floods;for(U b=0;b<batch;++b)floods.emplace_back(new NativeFlood(f,geo));
 for(U base=0;base<m;base+=batch){U end=std::min(m,base+batch);
  #pragma omp parallel for schedule(static)
  for(U w=base;w<end;++w){auto&d=*floods[w-base];auto&r=records[w-base];r.out.clear();r.in.clear();
   d.To(wp[w],true,2*cap);for(U a:d.order)if(d.time[a]<=2*cap)r.out.push_back({index[a],{Extract(d,a,true,geo),w,d.time[a]}});
   d.From(wp[w],true,2*cap);r.in.push_back({index[wp[w]],{{0,0,NONE,NONE},w,0}});for(U a:d.order)if(a!=wp[w]&&d.time[a]<=2*cap)r.in.push_back({index[a],{Extract(d,a,false,geo),w,d.time[a]}});
  }
  for(U w=base;w<end;++w){for(auto&r:records[w-base].out)Insert(out[r.first],r.second);for(auto&r:records[w-base].in)Insert(in[r.first],r.second);}
 }
 W att[4]={};U maxTrip[4]={};for(U a=0;a<n;++a){Need(out[a].count&&in[a].count,"attachment coverage absent");for(U k=0;k<4;++k){att[k]+=std::min(KS[k],out[a].count)+std::min(KS[k],in[a].count);for(U j=0;j<std::min(KS[k],out[a].count);++j)maxTrip[k]=std::max(maxTrip[k],out[a].a[j].native);for(U j=0;j<std::min(KS[k],in[a].count);++j)maxTrip[k]=std::max(maxTrip[k],in[a].a[j].native);}}
 for(U k=0;k<4;++k)printf("M ATTACH cap=%u K=%u records=%llu perEnabled=%.12f maxTrip=%u\n",cap,KS[k],(unsigned long long)att[k],double(att[k])/n,maxTrip[k]);
 // Full exact graph-time middle paths, the same middle metric as S3c. The
 // canonical path includes length and stable reach tie-breaking from NativeFlood.
 std::vector<PathMetric>middle(size_t(m)*m);std::atomic<U>done{0};
 #pragma omp parallel
 {
  NativeFlood d(f,geo);std::vector<U>last(f.g.edges.size(),NONE);
  #pragma omp for schedule(dynamic,1)
  for(U goal=0;goal<m;++goal){d.To(wp[goal],false);
   // Reach-state successor chains strictly reduce graph distance. Memoize the
   // last reach in that chain, preserving initial-Origin excursions elsewhere.
   for(U a:d.order)for(U j=f.g.in[a];j<f.g.in[a+1];++j){U e=f.g.rev[j];if(!f.g.enabled[f.g.edges[e].from])continue;last[e]=d.policy[e]==NONE?e:last[d.policy[e]];}
   for(U s=0;s<m;++s){U a=wp[s];if(d.time[a]==BIG)continue;U first=d.endpoint[a];middle[size_t(s)*m+goal]={d.length[a],d.time[a],first,first==NONE?NONE:last[first]};}
   U completed=++done;if(completed%1024==0||completed==m){
    #pragma omp critical(output)
    printf("M MIDDLE_PROGRESS cap=%u completed=%u total=%u seconds=%.6f\n",cap,completed,m,omp_get_wtime()-begin);
   }
  }
 }
 W byRange[NB]={};for(const auto&p:middle)if(p.graph!=BIG)++byRange[Band(p.graph)];W cumul=0;for(U b=0;b<NB;++b){cumul+=byRange[b];printf("M ALL_MIDDLE_RANGE cap=%u below=%u entries=%llu perEnabled=%.12f\n",cap,RB[b+1],(unsigned long long)cumul,double(cumul)/n);}
 std::vector<W>mask(size_t(m)*m);std::vector<GoalStats>stats(n);std::array<W,NB>bandPairs{};U limit=goalLimit?std::min(goalLimit,n):n;done=0;
 printf("A CENSUS_METHOD cap=%u K=1,2,4,8 attachmentAdmission=2cap attachmentSort=native-time,length,waypoint-ID middle=graph-time,length,reach-ID join=exact firstOrigin=once terminal=plus1 loops=left-in self=empty-route reference=NativeFlood.To.native-lex noRadius=1 noRuntimeFlood=1\n",cap);
 #pragma omp parallel
 {
  NativeFlood reference(f,geo);std::vector<std::array<PathMetric,8>>suffix(m);std::array<W,NB>localBands{};
  #pragma omp for schedule(dynamic,1)
  for(U gi=0;gi<limit;++gi){U goal=active[gi];reference.To(goal,true);auto&st=stats[gi];const auto&tail=in[gi];
   for(U w=0;w<m;++w)for(U j=0;j<tail.count;++j)suffix[w][j]=Join(middle[size_t(w)*m+tail.a[j].waypoint],tail.a[j].p,geo);
   for(U si=0;si<n;++si){U s=active[si];if(reference.time[s]==BIG){++st.unreachable;continue;}++st.pairs;if(s==goal)continue;U ref=reference.time[s]+1;int band=Band(ref);++localBands[band];st.maxTime=std::max(st.maxTime,ref);
    Candidate best[4];const auto&head=out[si];
    for(U i=0;i<head.count;++i){const auto&a=head.a[i];for(U j=0;j<tail.count;++j){const auto&b=suffix[a.waypoint][j];if(b.graph==BIG)continue;U rank=std::max(i,j),k=rank<1?0:rank<2?1:rank<4?2:3;U first=a.p.first!=NONE?a.p.first:b.first;U time=a.p.graph+b.graph+(first==NONE?0:geo.origin[first])+1;if(time>best[k].time)continue;
      auto p=Join(a.p,b,geo);Candidate c{p.length,time,a.waypoint,tail.a[j].waypoint};if(BetterCandidate(c,best[k]))best[k]=c;
    }}
    for(U k=0;k<4;++k){if(k&&BetterCandidate(best[k-1],best[k]))best[k]=best[k-1];const auto&q=best[k];bool tf=q.time==BIG||W(4)*q.time>W(5)*ref+684;
     long double guard=64*std::numeric_limits<long double>::epsilon()*(static_cast<long double>(q.time)+f.g.n+32)*(4*q.length+5*reference.length[s]+2048+8*static_cast<long double>(q.time)*geo.maxCentreOffset);
     bool lf=q.time==BIG||4*q.length+guard>5*reference.length[s]+2048;
     if(tf||lf){++st.fail[k][band];st.tf[k]+=tf;st.lf[k]+=lf;st.farthest[k]=std::max(st.farthest[k],ref);}
     else {W bit=W(1)<<(k*NB+band);size_t at=size_t(q.wa)*m+q.wg;
      #pragma omp atomic update
      mask[at]|=bit;
     }
    }
   }
   U completed=++done;if(completed%1024==0||completed==limit){
    #pragma omp critical(output)
    printf("M CHECK_PROGRESS cap=%u completedGoals=%u enabledGoals=%u fraction=%.12f seconds=%.6f\n",cap,completed,n,double(completed)/n,omp_get_wtime()-begin);
   }
  }
  #pragma omp critical(bands)
  for(U b=0;b<NB;++b)bandPairs[b]+=localBands[b];
 }
 W totalFail[4]={},timeFail[4]={},lengthFail[4]={},bands[4][NB]={},pairs=0,unreachable=0,digest=0;U farthest[4]={},maxPerGoal[4]={},maxReference=0;
 for(U gi=0;gi<limit;++gi){const auto&s=stats[gi];pairs+=s.pairs;unreachable+=s.unreachable;maxReference=std::max(maxReference,s.maxTime);for(U k=0;k<4;++k){W count=0;for(U b=0;b<NB;++b){count+=s.fail[k][b];bands[k][b]+=s.fail[k][b];}totalFail[k]+=count;timeFail[k]+=s.tf[k];lengthFail[k]+=s.lf[k];farthest[k]=std::max(farthest[k],s.farthest[k]);maxPerGoal[k]=std::max(maxPerGoal[k],U(count));digest^=Mix((W(active[gi])<<32)|(W(k)<<28)|count);}}
 Need(pairs+unreachable==W(limit)*n,"exhaustive pair accounting");
 printf("M EXHAUSTIVE cap=%u completedGoals=%u enabledGoals=%u fraction=%.12f orderedPairs=%llu reachableIncludingSelf=%llu unreachable=%llu digest=%016llx seconds=%.6f\n",cap,limit,n,double(limit)/n,(unsigned long long)W(limit)*n,(unsigned long long)pairs,(unsigned long long)unreachable,(unsigned long long)digest,omp_get_wtime()-begin);
 printf("M BAND_PAIRS cap=%u",cap);for(U b=0;b<NB;++b)printf(" [%u,%u)=%llu",RB[b],RB[b+1],(unsigned long long)bandPairs[b]);puts("");
 printf("M OBSERVED_TRIP_DOMAIN cap=%u maxReferenceTrip=%u\n",cap,maxReference);
 W histogram[4][1<<NB]={};for(W x:mask)for(U k=0;k<4;++k)++histogram[k][(x>>(k*NB))&((1u<<NB)-1)];
 for(U k=0;k<4;++k){printf("M LEVEL cap=%u K=%u waypoints=%u share=%.12f attachmentRecords=%llu attachPerEnabled=%.12f failingPairs=%llu failuresPerGoal=%.12f maxFailuresPerGoal=%u timeFailures=%llu lengthFailures=%llu farthestFailingTrip=%u noFailureAtOrAbove=%u\n",cap,KS[k],m,double(m)/n,(unsigned long long)att[k],double(att[k])/n,(unsigned long long)totalFail[k],double(totalFail[k])/limit,maxPerGoal[k],(unsigned long long)timeFail[k],(unsigned long long)lengthFail[k],farthest[k],farthest[k]?farthest[k]+1:0);
  printf("M FAIL_BANDS cap=%u K=%u",cap,KS[k]);for(U b=0;b<NB;++b)printf(" [%u,%u)=%llu",RB[b],RB[b+1],(unsigned long long)bands[k][b]);puts("");
  for(U lo=0;lo<NB;++lo){U bits=0;for(U hi=lo;hi<NB;++hi){bits|=1u<<hi;W count=0;for(U x=1;x<(1u<<NB);++x)if(x&bits)count+=histogram[k][x];printf("M USED_LIST_RANGE cap=%u K=%u refFrom=%u refBelow=%u entries=%llu perEnabled=%.12f\n",cap,KS[k],RB[lo],RB[hi+1],(unsigned long long)count,double(count)/n);}}
  W selected=0;for(U x=1;x<(1u<<NB);++x)selected+=histogram[k][x];W assumed=6*selected+4*att[k]+7*totalFail[k];
  printf("D SINGLE_LEVEL_ESTIMATE cap=%u K=%u selectedLists=%llu attachments=%llu patches=%llu bytes=%llu perEnabled=%.12f excludes=followability-closure-indices-waypoints-door-data-runtime\n",cap,KS[k],(unsigned long long)(6*selected),(unsigned long long)(4*att[k]),(unsigned long long)(7*totalFail[k]),(unsigned long long)assumed,double(assumed)/n);
 }
 // Canonical per-goal raw rows are emitted after the parallel section.
 for(U gi=0;gi<limit;++gi)for(U k=0;k<4;++k){printf("M GOAL cap=%u K=%u goal=%u reachable=%llu unreachable=%llu failBands=",cap,KS[k],active[gi],(unsigned long long)stats[gi].pairs,(unsigned long long)stats[gi].unreachable);for(U b=0;b<NB;++b)printf("%s%llu",b?",":"",(unsigned long long)stats[gi].fail[k][b]);printf(" farthest=%u\n",stats[gi].farthest[k]);}
 bool dead=cap==128&&limit==n;for(U k=0;k<4;++k)dead=dead&&totalFail[k]>W(8)*n;
 printf("D CAP128_DEAD_TEST cap=%u full=%u thresholdFailuresPerGoal=8 result=%s\n",cap,limit==n,dead?"DEAD-STOP":cap==128&&limit==n?"SURVIVES":"NOT-APPLICABLE");
 return dead;
}
int main(int argc,char**argv){try{
 setvbuf(stdout,nullptr,_IOLBF,0);Need(omp_get_max_threads()<=8,"OMP threads exceed eight");U side=argc>1?std::stoul(argv[1]):2,onlyCap=argc>2?std::stoul(argv[2]):0,goalLimit=argc>3?std::stoul(argv[3]):0;
 Need(side==2||side==4,"S4a walk side must be two or four");auto f=Native(side);for(U v=0;v<f.g.n;++v)if(f.g.mutableArea[v])f.g.enabled[v]=1;auto doors=ClassifyDoors(f);for(U v=0;v<f.g.n;++v)if(doors.loop[v])f.g.enabled[v]=0;
 NativeGeometry geo(f);std::vector<U>active;for(U v=0;v<f.g.n;++v)if(f.g.enabled[v])active.push_back(v);
 printf("M FIXTURE side=%u enabled=%zu allocated=%u edges=%zu forced=%u loop=%u threads=%d\n",side,active.size(),f.g.n,f.g.edges.size(),doors.forcedCount,doors.loopCount,omp_get_max_threads());
 puts("A CENSUS_WIDTHS listBytes=6 attachmentBytes=4 patchBytes=7 these-are-requested-estimates-not-a-packed-ledger");
 for(U cap:{64u,128u,256u,1024u})if(!onlyCap||onlyCap==cap)if(Census(f,geo,active,cap,goalLimit))return 4;
 return 0;
}catch(const std::exception&e){fprintf(stderr,"M REFUSAL %s\n",e.what());return 1;}}
