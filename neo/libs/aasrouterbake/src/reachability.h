#pragma once
#include "work.h"
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace sn {
// Exact dynamic directed reachability. Immutable SCCs are baked with every
// mutable area removed. Every mutable area is its own vertex. Only vertices
// incident to an inter-SCC arc enter the dynamic core; isolated SCCs cannot
// reach a different component and need no dynamic state. Current SCCs and the
// transitive closure of their directed condensation are rebuilt on a toggle.
// All runtime scalar work is wrapped; bake/allocation/diagnostics are excluded.
class Reachability {
 const overlay_four::Fixture4& f;
 U n=0, staticCount=0, componentCount=0, coreCount=0, stride=0, sccCount=0;
 std::vector<U> component,coreOf,doorOf,out,to,in,from;
 std::vector<uint8_t> live,seen;
 std::vector<U> cursor,stack,order,scc,head,next;
 std::vector<W> closure;
 static void Need(bool b,const char* m){if(!b)throw std::runtime_error(m);}
 void Rebuild(Work& z) {
  U a;z.Copy(a,0);
  for(;z.C(z.E(a)<z.E(coreCount));z.Copy(a,z.E(a)+1)) {
   U door;z.Copy(door,z.R(doorOf,a));
   if(z.C(z.E(door)==NONE))z.Set(live,a,1);
   else z.Set(live,a,z.R(f.g.enabled,door));
  }
  // Bulk scalar clears: exactly one W per scalar, as COUNTING.md specifies.
  std::fill(seen.begin(),seen.end(),0);z.Add(coreCount);
  U count;z.Copy(count,0);U sp;z.Copy(sp,0);U root;z.Copy(root,0);
  for(;z.C(z.E(root)<z.E(coreCount));z.Copy(root,z.E(root)+1)) {
   if(z.C(!z.R(live,root)))continue;
   if(z.C(z.R(seen,root)))continue;
   z.Set(seen,root,1);z.Set(cursor,root,z.R(out,root));
   z.Set(stack,sp,z.E(root));z.Copy(sp,z.E(sp)+1);
   while(z.C(z.E(sp)!=0)) {
    U v;z.Copy(v,z.R(stack,z.E(sp)-1));
    U i;z.Copy(i,z.R(cursor,v));U end;z.Copy(end,z.R(out,z.E(v)+1));
    if(z.C(z.E(i)<z.E(end))) {
     z.Set(cursor,v,z.E(i)+1);U t;z.Copy(t,z.R(to,i));
     if(z.C(!z.R(live,t)))continue;
     if(z.C(z.R(seen,t)))continue;
     z.Set(seen,t,1);z.Set(cursor,t,z.R(out,t));
     z.Set(stack,sp,z.E(t));z.Copy(sp,z.E(sp)+1);
    }else{
     z.Set(order,count,z.E(v));z.Copy(count,z.E(count)+1);z.Copy(sp,z.E(sp)-1);
    }
   }
  }
  std::fill(scc.begin(),scc.end(),NONE);z.Add(coreCount);z.Copy(sccCount,0);
  while(z.C(z.E(count)!=0)) {
   z.Copy(count,z.E(count)-1);U v;z.Copy(v,z.R(order,count));
   if(z.C(z.R(scc,v)!=NONE))continue;
   z.Set(scc,v,z.E(sccCount));z.Set(stack,sp,z.E(v));z.Copy(sp,z.E(sp)+1);
   while(z.C(z.E(sp)!=0)) {
    z.Copy(sp,z.E(sp)-1);U u;z.Copy(u,z.R(stack,sp));
    U j;z.Copy(j,z.R(in,u));U end;z.Copy(end,z.R(in,z.E(u)+1));
    for(;z.C(z.E(j)<z.E(end));z.Copy(j,z.E(j)+1)) {
     U t;z.Copy(t,z.R(from,j));
     if(z.C(!z.R(live,t)))continue;
     if(z.C(z.R(scc,t)!=NONE))continue;
     z.Set(scc,t,z.E(sccCount));z.Set(stack,sp,z.E(t));z.Copy(sp,z.E(sp)+1);
    }
   }
   z.Copy(sccCount,z.E(sccCount)+1);
  }
  // Kosaraju numbers SCCs in source-to-sink topological order. Build their
  // member lists, then union successor closure rows in reverse order. Closed
  // core vertices have no SCC and never enter these lists.
  std::fill(head.begin(),head.begin()+sccCount,NONE);z.Add(sccCount);
  z.Copy(a,0);
  for(;z.C(z.E(a)<z.E(coreCount));z.Copy(a,z.E(a)+1)) {
   U c;z.Copy(c,z.R(scc,a));if(z.C(z.E(c)==NONE))continue;
   z.Set(next,a,z.R(head,c));z.Set(head,c,z.E(a));
  }
  // Fixed stride permits reuse of preallocated workspace; only live SCC rows
  // are cleared and queried. Bitset operation counts each scalar word.
  U clearWords;z.Copy(clearWords,z.E(sccCount)*z.E(stride));
  std::fill(closure.begin(),closure.begin()+clearWords,W(0));z.Add(clearWords);
  U c;z.Copy(c,z.E(sccCount));
  while(z.C(z.E(c)!=0)) {
   z.Copy(c,z.E(c)-1);U row;z.Copy(row,z.E(c)*z.E(stride));
   U diagonal;z.Copy(diagonal,z.E(row)+(z.E(c)>>6));
   z.Set(closure,diagonal,W(1)<<(z.E(c)&63));
   U v;z.Copy(v,z.R(head,c));
   while(z.C(z.E(v)!=NONE)) {
    U j;z.Copy(j,z.R(out,v));U end;z.Copy(end,z.R(out,z.E(v)+1));
    for(;z.C(z.E(j)<z.E(end));z.Copy(j,z.E(j)+1)) {
     U t;z.Copy(t,z.R(to,j));U d;z.Copy(d,z.R(scc,t));
     if(z.C(z.E(d)==NONE))continue;
     if(z.C(z.E(c)==z.E(d)))continue;
     // This is a structural assertion, not work hidden from the algorithm:
     // without it a wrong topological ordering would silently corrupt labels.
     if(z.C(z.E(d)<z.E(c)))throw std::runtime_error("reachability SCC order");
     U other;z.Copy(other,z.E(d)*z.E(stride));U word;z.Copy(word,0);
     U words;z.Copy(words,(z.E(sccCount)+63)/64);
     for(;z.C(z.E(word)<z.E(words));z.Copy(word,z.E(word)+1)) {
      U at;z.Copy(at,z.E(row)+z.E(word));U there;z.Copy(there,z.E(other)+z.E(word));
      z.Set(closure,at,z.R(closure,at)|z.R(closure,there));
     }
    }
    z.Copy(v,z.R(next,v));
   }
  }
 }
public:
 explicit Reachability(const overlay_four::Fixture4& fixture,Work* initial=nullptr):f(fixture),n(f.g.n) {
  // Iterative Kosaraju on the permanently open, nonmutable subgraph.
  component.assign(n,NONE);std::vector<uint8_t> visited(n,0);
  std::vector<U> finish,st,edge(n);finish.reserve(n);st.reserve(n);
  auto fixed=[&](U v){return f.g.enabled[v]&&!f.g.mutableArea[v];};
  for(U r=0;r<n;++r)if(fixed(r)&&!visited[r]) {
   visited[r]=1;edge[r]=f.g.out[r];st.push_back(r);
   while(!st.empty()) {
    U v=st.back();if(edge[v]<f.g.out[v+1]) {
     U t=f.g.edges[edge[v]++].to;if(fixed(t)&&!visited[t]){visited[t]=1;edge[t]=f.g.out[t];st.push_back(t);}
    }else{finish.push_back(v);st.pop_back();}
   }
  }
  for(auto it=finish.rbegin();it!=finish.rend();++it)if(component[*it]==NONE) {
   component[*it]=staticCount;st.push_back(*it);
   while(!st.empty()) {U v=st.back();st.pop_back();for(U j=f.g.in[v];j<f.g.in[v+1];++j){U t=f.g.edges[f.g.rev[j]].from;if(fixed(t)&&component[t]==NONE){component[t]=staticCount;st.push_back(t);}}}
   ++staticCount;
  }
  componentCount=staticCount;
  std::vector<U> doorComponent;
  for(U v=0;v<n;++v)if(f.g.mutableArea[v]){component[v]=componentCount++;doorComponent.push_back(v);}
  std::vector<std::pair<U,U>> arcs;
  for(const auto&e:f.g.edges)if(component[e.from]!=NONE&&component[e.to]!=NONE&&component[e.from]!=component[e.to])arcs.emplace_back(component[e.from],component[e.to]);
  std::sort(arcs.begin(),arcs.end());arcs.erase(std::unique(arcs.begin(),arcs.end()),arcs.end());
  coreOf.assign(componentCount,NONE);
  for(auto p:arcs){coreOf[p.first]=0;coreOf[p.second]=0;}
  for(U c=0;c<componentCount;++c)if(coreOf[c]!=NONE)coreOf[c]=coreCount++;
  doorOf.assign(coreCount,NONE);
  for(U c=staticCount;c<componentCount;++c)if(coreOf[c]!=NONE)doorOf[coreOf[c]]=doorComponent[c-staticCount];
  out.assign(coreCount+1,0);in.assign(coreCount+1,0);
  for(auto p:arcs){++out[coreOf[p.first]+1];++in[coreOf[p.second]+1];}
  for(U i=0;i<coreCount;++i){out[i+1]+=out[i];in[i+1]+=in[i];}
  to.resize(arcs.size());from.resize(arcs.size());auto at=out,back=in;
  for(auto p:arcs){U a=coreOf[p.first],b=coreOf[p.second];to[at[a]++]=b;from[back[b]++]=a;}
  stride=(coreCount+63)/64;
  Need(W(coreCount)*stride<UINT32_MAX,"reachability closure index width refused");
  Need(W(coreCount)*stride*sizeof(W)<=W(1)<<30,"reachability closure byte cap refused");
  live.resize(coreCount);seen.resize(coreCount);cursor.resize(coreCount);stack.resize(coreCount);order.resize(coreCount);scc.resize(coreCount);head.resize(coreCount);next.resize(coreCount);closure.resize(W(coreCount)*stride);
  Work scratch;Rebuild(initial?*initial:scratch);
 }
 // The caller mutates f.g.enabled first. Immutable enabled flags must never
 // change; that would invalidate the baked SCCs and is refused here.
 void Update(const std::vector<U>& areas,bool open,Work& z) {
  (void)open;U i;z.Copy(i,0);U size;z.Copy(size,z.E(areas.size()));
  for(;z.C(z.E(i)<z.E(size));z.Copy(i,z.E(i)+1)) {
   U a;z.Copy(a,z.R(areas,i));
   if(z.C(z.E(a)>=z.E(n)))throw std::runtime_error("reachability toggle area width");
   if(z.C(!z.R(f.g.mutableArea,a)))throw std::runtime_error("reachability nonmutable toggle refused");
  }
  if(z.C(z.E(size)!=0))Rebuild(z);
 }
 bool Can(U source,U goal,Work& z) const {
  if(z.C(z.E(source)>=z.E(n)))return false;
  if(z.C(z.E(goal)>=z.E(n)))return false;
  if(z.C(!z.R(f.g.enabled,source)))return false;
  if(z.C(!z.R(f.g.enabled,goal)))return false;
  U a,b;z.Copy(a,z.R(component,source));z.Copy(b,z.R(component,goal));
  if(z.C(z.E(a)==NONE))return false;
  if(z.C(z.E(b)==NONE))return false;
  if(z.C(z.E(a)==z.E(b)))return true;
  U ca,cb;z.Copy(ca,z.R(coreOf,a));z.Copy(cb,z.R(coreOf,b));
  if(z.C(z.E(ca)==NONE))return false;
  if(z.C(z.E(cb)==NONE))return false;
  U x,y;z.Copy(x,z.R(scc,ca));z.Copy(y,z.R(scc,cb));
  if(z.C(z.E(x)==NONE))return false;
  if(z.C(z.E(y)==NONE))return false;
  U index;z.Copy(index,z.E(x)*z.E(stride)+(z.E(y)>>6));
  return z.C((z.R(closure,index)&(W(1)<<(z.E(y)&63)))!=0);
 }
 W Bytes() const {
  W b=sizeof(*this);
  for(const auto*p:{&component,&coreOf,&doorOf,&out,&to,&in,&from,&cursor,&stack,&order,&scc,&head,&next})b+=W(p->capacity())*sizeof(U);
  return b+live.capacity()+seen.capacity()+W(closure.capacity())*sizeof(W);
 }
 U StaticComponents()const{return staticCount;}
 U Components()const{return componentCount;}
 U CoreVertices()const{return coreCount;}
 U CoreArcs()const{return U(to.size());}
 U CurrentComponents()const{return sccCount;}
};
}
