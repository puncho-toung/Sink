#pragma once
// Stage 1: the definition is equivalent to every incident simple edge being a
// bridge. Duplicate directed reaches do not create an undirected cycle.
#include <bits/stdc++.h>
namespace s3c {
using U=uint32_t; using W=uint64_t;
constexpr U NO=UINT32_MAX;
struct DoorClassification {
 std::vector<uint8_t> forced,loop;
 std::vector<U> bakePiece,treePiece,parentDoor,depth;
 std::vector<W> bakeSizes,treeSizes;
 std::vector<U> loopCounts;
 U forcedCount=0,loopCount=0,treeDepth=0,treeComponents=0,maxLoops=0,capBits=0;
};
inline std::vector<std::vector<U>> Undirected(const overlay_four::Fixture4&f){
 std::vector<std::vector<U>> adj(f.g.n);
 for(const auto&e:f.g.edges)if(f.g.enabled[e.from]&&f.g.enabled[e.to]&&e.from!=e.to){adj[e.from].push_back(e.to);adj[e.to].push_back(e.from);}
 for(auto&a:adj){std::sort(a.begin(),a.end());a.erase(std::unique(a.begin(),a.end()),a.end());} return adj;
}
inline void Components(const std::vector<std::vector<U>>&adj,const std::vector<uint8_t>&live,std::vector<U>&comp,std::vector<W>&sizes){
 comp.assign(adj.size(),NO);sizes.clear();std::vector<U> q;
 for(U s=0;s<adj.size();s++)if(live[s]&&comp[s]==NO){U c=sizes.size();q={s};comp[s]=c;for(size_t i=0;i<q.size();i++)for(U v:adj[q[i]])if(live[v]&&comp[v]==NO){comp[v]=c;q.push_back(v);}sizes.push_back(q.size());}
}
inline DoorClassification ClassifyDoors(const overlay_four::Fixture4&f){
 DoorClassification c;U n=f.g.n;auto adj=Undirected(f);
 std::vector<U> disc(n),low(n),parent(n,NO),next(n),stack;U timer=0;
 for(U root=0;root<n;root++)if(f.g.enabled[root]&&!disc[root]){disc[root]=low[root]=++timer;stack={root};while(!stack.empty()){U v=stack.back();if(next[v]<adj[v].size()){U w=adj[v][next[v]++];if(!disc[w]){parent[w]=v;disc[w]=low[w]=++timer;stack.push_back(w);}else if(w!=parent[v])low[v]=std::min(low[v],disc[w]);}else{stack.pop_back();if(parent[v]!=NO)low[parent[v]]=std::min(low[parent[v]],low[v]);}}}
 c.forced.assign(n,0);c.loop.assign(n,0);
 for(U v=0;v<n;v++)if(f.g.mutableArea[v]){bool forced=true;for(U w:adj[v]){bool bridge=parent[w]==v?low[w]>disc[v]:parent[v]==w&&low[v]>disc[w];forced=forced&&bridge;}c.forced[v]=forced;c.loop[v]=!forced;c.forcedCount+=forced;c.loopCount+=!forced;}
 std::vector<uint8_t>live(n);for(U v=0;v<n;v++)live[v]=f.g.enabled[v]&&!c.loop[v];Components(adj,live,c.bakePiece,c.bakeSizes);
 c.loopCounts.assign(c.bakeSizes.size(),0);
 for(U d=0;d<n;d++)if(c.loop[d]){std::set<U> pieces;for(U v:adj[d])if(c.bakePiece[v]!=NO)pieces.insert(c.bakePiece[v]);for(U p:pieces)c.loopCounts[p]++;}
 for(U x:c.loopCounts)c.maxLoops=std::max(c.maxLoops,x);
 while(c.capBits<32&&(c.capBits==0?0:((W(1)<<c.capBits)-1))<c.maxLoops)++c.capBits;
 for(U v=0;v<n;v++){live[v]=f.g.enabled[v]&&!c.forced[v];}Components(adj,live,c.treePiece,c.treeSizes);
 U pieces=c.treeSizes.size(),nodes=pieces+c.forcedCount;std::vector<U>doorNode(n,NO),nodeDoor(nodes,NO);U dn=pieces;
 for(U v=0;v<n;v++)if(c.forced[v]){doorNode[v]=dn;nodeDoor[dn++]=v;}
 std::vector<std::vector<U>> tree(nodes);
 for(U d=0;d<n;d++)if(c.forced[d])for(U v:adj[d]){U to=c.forced[v]?doorNode[v]:c.treePiece[v];if(to!=NO){tree[doorNode[d]].push_back(to);tree[to].push_back(doorNode[d]);}}
 for(auto&a:tree){std::sort(a.begin(),a.end());a.erase(std::unique(a.begin(),a.end()),a.end());}
 std::vector<U>roots(nodes);std::iota(roots.begin(),roots.end(),0);std::sort(roots.begin(),roots.end(),[&](U a,U b){W x=a<pieces?c.treeSizes[a]:0,y=b<pieces?c.treeSizes[b]:0;return x!=y?x>y:a<b;});
 c.depth.assign(nodes,NO);c.parentDoor.assign(nodes,NO);std::vector<U>q;
 for(U r:roots)if(c.depth[r]==NO){c.treeComponents++;c.depth[r]=0;q={r};for(size_t i=0;i<q.size();i++){U v=q[i];for(U w:tree[v])if(c.depth[w]==NO){c.depth[w]=c.depth[v]+(w>=pieces);c.parentDoor[w]=w>=pieces?nodeDoor[w]:c.parentDoor[v];c.treeDepth=std::max(c.treeDepth,c.depth[w]);q.push_back(w);}}}
 return c;
}
}
