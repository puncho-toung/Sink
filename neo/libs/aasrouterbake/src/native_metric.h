#pragma once
#include "native_common.h"
// Native metric follows the two-pass reach-state flood in S3a/lex_oracle.h.
// Integer time settles first; exact binary80 reach joins decide equal-time paths.
namespace s3c {
inline void MetricRequire(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
inline long double NativeDistance(const overlay_four::Point4&a,const overlay_four::Point4&b) {
 const long double x=(long double)a.x-b.x,y=(long double)a.y-b.y,z=(long double)a.z-b.z;
 return std::sqrt(x*x+y*y+z*z);
}
inline U NativeOrigin(const overlay_four::Fixture4&f,U s,const overlay_four::Point4&target) {
 float x=float(f.centres[s].x)-float(target.x),y=float(f.centres[s].y)-float(target.y),z=float(f.centres[s].z)-float(target.z);
 float q=x*x+y*y+z*z;float d=q==0?0:q*std::sqrt(1.0f/q);
 d*=s<f.water.size()&&f.water[s]?100.0f/150.0f:100.0f/300.0f;
 MetricRequire(std::isfinite(d)&&d<float(INT_MAX),"native Origin overflow");return d<1?1:U(d);
}
struct NativeGeometry {
 const overlay_four::Fixture4&f;
 std::vector<long double> reachLength,startCentre,endCentre,joins;
 std::vector<U> origin;std::vector<size_t> joinOffset;
 U maxWeight=0,maxOrigin=0;long double maxCentreOffset=0;
 explicit NativeGeometry(const overlay_four::Fixture4&fixture):f(fixture) {
  const size_t m=f.g.edges.size();reachLength.resize(m);startCentre.resize(m);endCentre.resize(m);origin.resize(m);joinOffset.resize(m+1);
  size_t total=0;
  for(U e=0;e<m;++e){const auto&edge=f.g.edges[e];MetricRequire(edge.w>0&&edge.w<BIG,"native weight range");maxWeight=std::max(maxWeight,edge.w);
   reachLength[e]=NativeDistance(f.geo[e].start,f.geo[e].end);startCentre[e]=NativeDistance(f.centres[edge.from],f.geo[e].start);endCentre[e]=NativeDistance(f.geo[e].end,f.centres[edge.to]);origin[e]=NativeOrigin(f,edge.from,f.geo[e].start);maxOrigin=std::max(maxOrigin,origin[e]);maxCentreOffset=std::max({maxCentreOffset,startCentre[e],endCentre[e]});
   joinOffset[e]=total;total+=f.g.out[edge.to+1]-f.g.out[edge.to];
  }joinOffset[m]=total;joins.resize(total);
  for(U e=0;e<m;++e){const U v=f.g.edges[e].to;for(U q=f.g.out[v];q<f.g.out[v+1];++q)joins[joinOffset[e]+q-f.g.out[v]]=NativeDistance(f.geo[e].end,f.geo[q].start);}
 }
 long double Join(U e,U q) const {return joins[joinOffset[e]+q-f.g.out[f.g.edges[e].to]];}
 // Compose the fixed paths, replacing their through-centre boundary detour by
 // the exact reach-end/start join. Callers retain first/last reaches themselves.
 // Binary80 association is deliberate; proof guards must include cancelled
 // centre legs (bounded by 2*hopCount*maxCentreOffset), not only final length.
 Metric Concat(Metric a,U lastA,Metric b,U firstB) const {
  if(a.t==BIG||b.t==BIG)return {};
  MetricRequire(a.t<BIG-b.t,"native concatenated time overflow");
  Metric out{a.t+b.t,a.l+b.l};
  if(lastA!=NONE&&firstB!=NONE){
   MetricRequire(lastA<f.g.edges.size()&&firstB<f.g.edges.size(),"native concatenated reach range");
   MetricRequire(f.g.edges[lastA].to==f.g.edges[firstB].from,"native concatenated endpoint mismatch");
   out.l=out.l-endCentre[lastA]-startCentre[firstB]+Join(lastA,firstB);
  }
  MetricRequire(std::isfinite(out.l),"native concatenated length nonfinite");
  return out;
 }
 long double PathLength(U source,U goal,const std::vector<U>&p)const {
  if(p.empty())return NativeDistance(f.centres[source],f.centres[goal]);
  long double result=startCentre[p.front()]+reachLength[p.front()];
  for(size_t i=1;i<p.size();++i)result+=Join(p[i-1],p[i])+reachLength[p[i]];
  return result+endCentre[p.back()];
 }
 U PathTime(const std::vector<U>&p,bool payOrigin=true)const {
  if(p.empty())return 0;
  uint64_t result=payOrigin?origin[p.front()]:0;
  for(U e:p)result+=f.g.edges[e].w;
  MetricRequire(result<BIG,"native path time range");return U(result);
 }
};
struct NativeFlood {
 const overlay_four::Fixture4&f;const NativeGeometry&g;
 std::vector<U> graphTime,time,endpoint,policy,order;
 std::vector<long double> length;
private:
 std::vector<long double> edgeMetric;
 std::vector<U> touched,edgeTouched,tight;
 std::vector<std::vector<U>> buckets;
 using Item=std::pair<U,U>;std::priority_queue<Item,std::vector<Item>,std::greater<Item>> heap;
 U root=NONE,current=0;uint64_t queued=0;bool isReverse=true,originPaid=true,useDial=true;
 bool Before(U a,U b) const {return b==NONE||std::tie(f.g.edges[a].ordinal,a)<std::tie(f.g.edges[b].ordinal,b);}
 void Reset(U at,bool reverse,bool payOrigin) {
  // Full floods touch most of these arrays in distance order. A contiguous
  // reset avoids scattering writes; local attachment floods keep sparse resets.
  if(touched.size()>f.g.n/4){std::fill(graphTime.begin(),graphTime.end(),BIG);std::fill(time.begin(),time.end(),BIG);std::fill(length.begin(),length.end(),INFINITY);std::fill(endpoint.begin(),endpoint.end(),NONE);}
  else for(U v:touched){graphTime[v]=time[v]=BIG;length[v]=INFINITY;endpoint[v]=NONE;}
  touched.clear();order.clear();
  if(edgeTouched.size()>f.g.edges.size()/4){std::fill(edgeMetric.begin(),edgeMetric.end(),INFINITY);std::fill(policy.begin(),policy.end(),NONE);}
  else for(U e:edgeTouched){edgeMetric[e]=INFINITY;policy[e]=NONE;}
  edgeTouched.clear();
  MetricRequire(queued==0&&heap.empty(),"native previous flood queue nonempty");root=at;isReverse=reverse;originPaid=payOrigin;current=0;
  MetricRequire(at<f.g.n&&f.g.enabled[at],"native flood disabled root");
 }
 void Push(U v,U d) {if(graphTime[v]==BIG)touched.push_back(v);graphTime[v]=d;if(useDial)buckets[d&(buckets.size()-1)].push_back(v);else heap.push({d,v});++queued;}
 bool Pop(U&v) {
  while(queued){if(useDial){auto&b=buckets[current&(buckets.size()-1)];if(b.empty()){++current;MetricRequire(current<BIG,"native Dial time overflow");continue;}v=b.back();b.pop_back();--queued;if(graphTime[v]!=current)continue;return true;}
   auto q=heap.top();heap.pop();--queued;v=q.second;current=q.first;if(graphTime[v]!=current)continue;return true;
  }return false;
 }
 void SetEdge(U e,long double value,U p) {edgeTouched.push_back(e);edgeMetric[e]=value;policy[e]=p;}
public:
 NativeFlood(const overlay_four::Fixture4&fixture,const NativeGeometry&geometry):f(fixture),g(geometry),graphTime(f.g.n,BIG),time(f.g.n,BIG),endpoint(f.g.n,NONE),policy(f.g.edges.size(),NONE),length(f.g.n,INFINITY),edgeMetric(f.g.edges.size(),INFINITY) {
  const uint64_t largestStep=uint64_t(g.maxWeight)+g.maxOrigin;useDial=largestStep<=65535;size_t count=1;while(count<=largestStep)count*=2;if(useDial)buckets.resize(count);touched.reserve(f.g.n);order.reserve(f.g.n);edgeTouched.reserve(f.g.edges.size());
 }
 void To(U goal,bool payOrigin=true,U radius=BIG) {
  Reset(goal,true,payOrigin);Push(goal,0);U v;
  while(Pop(v)){
   order.push_back(v);
   for(U j=f.g.in[v];j<f.g.in[v+1];++j){U ei=f.g.rev[j];const auto&e=f.g.edges[ei];if(!f.g.enabled[e.from])continue;
    MetricRequire(current<BIG-e.w,"native reverse time overflow");U candidate=current+e.w;if(candidate>radius)continue;if(candidate<graphTime[e.from])Push(e.from,candidate);
   }
  }
  for(U a:order){
   // The time-tight successor set depends only on the area, not on which
   // incoming reach supplied arrival geometry. Preserve original CSR order.
   tight.clear();
   if(a!=goal)for(U fi=f.g.out[a];fi<f.g.out[a+1];++fi){const auto&next=f.g.edges[fi];
    if(f.g.enabled[next.to]&&graphTime[next.to]!=BIG&&next.w+graphTime[next.to]==graphTime[a])tight.push_back(fi);
   }
   for(U j=f.g.in[a];j<f.g.in[a+1];++j){U ei=f.g.rev[j];const auto&e=f.g.edges[ei];if(!f.g.enabled[e.from])continue;
    if(a==goal){SetEdge(ei,g.endCentre[ei],NONE);continue;}
    U chosen=NONE;long double best=INFINITY;
    for(U fi:tight){
     long double candidate=g.Join(ei,fi)+g.reachLength[fi]+edgeMetric[fi];
     if(candidate<best||(candidate==best&&Before(fi,chosen))){best=candidate;chosen=fi;}
    }
    MetricRequire(chosen!=NONE&&std::isfinite(best),"native reverse tight continuation missing");SetEdge(ei,best,chosen);
   }
  }
  for(U s:order){if(s==goal)continue;
   for(U ei=f.g.out[s];ei<f.g.out[s+1];++ei){const auto&e=f.g.edges[ei];if(!f.g.enabled[e.to]||graphTime[e.to]==BIG)continue;
    uint64_t value=uint64_t(payOrigin?g.origin[ei]:0)+e.w+graphTime[e.to];MetricRequire(value<BIG,"native query time overflow");U candidate=U(value);
    if(candidate>time[s])continue;
    long double len=g.startCentre[ei]+g.reachLength[ei]+edgeMetric[ei];
    if(candidate<time[s]||(candidate==time[s]&&(len<length[s]||(len==length[s]&&Before(ei,endpoint[s]))))){time[s]=candidate;length[s]=len;endpoint[s]=ei;}
   }
  }time[goal]=0;length[goal]=0;
 }
 void From(U source,bool payOrigin=true,U radius=BIG) {
  Reset(source,false,payOrigin);
  // A virtual source pays Origin once. The real source remains available as an
  // intermediate vertex, so a beneficial first-Origin excursion is representable.
  for(U ei=f.g.out[source];ei<f.g.out[source+1];++ei){const auto&e=f.g.edges[ei];if(!f.g.enabled[e.to])continue;
   uint64_t value=uint64_t(payOrigin?g.origin[ei]:0)+e.w;MetricRequire(value<BIG,"native initial time overflow");U d=U(value);if(d<=radius&&d<graphTime[e.to])Push(e.to,d);
  }
  U v;
  while(Pop(v)){
   order.push_back(v);
   for(U ei=f.g.out[v];ei<f.g.out[v+1];++ei){const auto&e=f.g.edges[ei];if(!f.g.enabled[e.to])continue;
    MetricRequire(current<BIG-e.w,"native forward time overflow");U candidate=current+e.w;if(candidate>radius)continue;if(candidate<graphTime[e.to])Push(e.to,candidate);
   }
  }
  for(U a:order){
   for(U j=f.g.in[a];j<f.g.in[a+1];++j){U ei=f.g.rev[j];const auto&e=f.g.edges[ei];if(!f.g.enabled[e.from])continue;
    U chosen=NONE;long double best=INFINITY;
    if(e.from==source&&uint64_t(payOrigin?g.origin[ei]:0)+e.w==graphTime[a])best=g.startCentre[ei]+g.reachLength[ei];
    if(graphTime[e.from]!=BIG&&graphTime[e.from]+e.w==graphTime[a]){
     for(U k=f.g.in[e.from];k<f.g.in[e.from+1];++k){U pi=f.g.rev[k];if(!std::isfinite(edgeMetric[pi]))continue;
      long double candidate=edgeMetric[pi]+g.Join(pi,ei)+g.reachLength[ei];
      if(candidate<best||(candidate==best&&chosen!=NONE&&Before(pi,chosen))){best=candidate;chosen=pi;}
     }
    }
    if(!std::isfinite(best))continue;
    SetEdge(ei,best,chosen);long double len=best+g.endCentre[ei];
    if(len<length[a]||(len==length[a]&&Before(ei,endpoint[a]))){time[a]=graphTime[a];length[a]=len;endpoint[a]=ei;}
   }
   MetricRequire(endpoint[a]!=NONE,"native forward tight endpoint missing");
  }
  // Ensure self output is reset on the next call even in a directed acyclic graph.
  if(graphTime[source]==BIG)touched.push_back(source);
  time[source]=0;length[source]=0;endpoint[source]=NONE;
 }
 std::vector<U> Path(U area,bool reverse)const {
  MetricRequire(reverse==isReverse,"native Path direction differs from last flood");MetricRequire(area<f.g.n&&time[area]!=BIG,"native Path unreachable endpoint");
  std::vector<U> p;if(area==root)return p;U e=endpoint[area];
  while(e!=NONE){MetricRequire(e<f.g.edges.size(),"native Path reach range");p.push_back(e);MetricRequire(p.size()<=f.g.n+1,"native Path cycle");e=policy[e];}
  if(!reverse)std::reverse(p.begin(),p.end());
  U at=reverse?area:root;
  for(U ei:p){MetricRequire(f.g.edges[ei].from==at&&f.g.enabled[f.g.edges[ei].to],"native Path disconnected");at=f.g.edges[ei].to;}
  MetricRequire(at==(reverse?root:area),"native Path wrong endpoint");return p;
 }
};
} // namespace s3c
