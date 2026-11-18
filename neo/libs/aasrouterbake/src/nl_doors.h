#pragma once
// exp-active2, part W4 (director, 2026-11-13): the forced doors as a tree.
//
// A forced door is an area whose every connection is a bridge: with it closed, the areas on its two sides have no
// route to each other. Take the forced doors out and the map falls into PIECES. Pieces and forced doors form a
// forest: a node per piece, a node per forced door, an edge where a reach joins them.
//
// What it is for. A route must never pass a closed door. A pair (s, g) with a route NEEDS exactly the doors on
// the tree path from s's node to g's node, and those are open whenever the pair has a route. Any other door the
// route passes could be closed. A route's "tree length" is the number of tree edges its areas walk along; it
// passes only needed doors exactly when its tree length equals the tree distance of its two ends (a walk in a
// tree is as short as the distance only if it never turns back).
// A least-time path never turns back in the tree (it would enter a door and leave it the way it came, a pure
// detour), so its tree length is the tree distance of its ends. A route made of several least-time pieces can
// turn back where the pieces join.
#include "nl_core.h"
namespace nl {

struct DoorTree {
  std::vector<U> node;                 // per area (graph number): its node, or NONE when the area is disabled
  std::vector<U> parent, level, root;  // per node
  std::vector<std::vector<U>> adj;     // per node, ascending
  U pieces = 0, doors = 0, edges = 0, trees = 0, depth = 0;
  DoorTree(const Fixture4& f, const s3c::DoorClassification& c) {
    pieces = U(c.treeSizes.size()); node.assign(f.g.n, NONE); U next = pieces;
    for (U v = 0; v < f.g.n; ++v) if (f.g.enabled[v]) { node[v] = c.forced[v] ? next++ : c.treePiece[v]; Need(node[v] != s3c::NO, "enabled area without a piece"); }
    const U count = next; doors = count - pieces; adj.assign(count, {});
    for (const auto& e : f.g.edges) { if (!f.g.enabled[e.from] || !f.g.enabled[e.to]) continue; const U a = node[e.from], b = node[e.to]; if (a == b) continue; Need(a >= pieces || b >= pieces, "two pieces joined without a door"); adj[a].push_back(b); adj[b].push_back(a); }
    for (auto& a : adj) { std::sort(a.begin(), a.end()); a.erase(std::unique(a.begin(), a.end()), a.end()); edges += U(a.size()); }
    edges /= 2; parent.assign(count, NONE); level.assign(count, NONE); root.assign(count, NONE); std::vector<U> queue;
    for (U r = 0; r < count; ++r) { if (level[r] != NONE) continue; ++trees; level[r] = 0; root[r] = r; queue.assign(1, r);
      for (size_t i = 0; i < queue.size(); ++i) { const U v = queue[i]; for (U w : adj[v]) if (level[w] == NONE) { level[w] = level[v] + 1; parent[w] = v; root[w] = r; depth = std::max(depth, level[w]); queue.push_back(w); } } }
    Need(edges + trees == count, "the pieces and forced doors do not form a forest");
  }
  U Dist(U a, U b) const {             // tree edges between two nodes; BIG when they are in different trees
    if (root[a] != root[b]) return BIG; U d = 0;
    while (level[a] > level[b]) { a = parent[a]; ++d; }
    while (level[b] > level[a]) { b = parent[b]; ++d; }
    while (a != b) { a = parent[a]; b = parent[b]; d += 2; }
    return d; }
  void From(U start, std::vector<U>& dist, std::vector<U>& queue) const {   // tree distance from one node to every node (BIG elsewhere)
    dist.assign(adj.size(), BIG); dist[start] = 0; queue.assign(1, start);
    for (size_t i = 0; i < queue.size(); ++i) { const U v = queue[i]; for (U w : adj[v]) if (dist[w] == BIG) { dist[w] = dist[v] + 1; queue.push_back(w); } } }
};

// Checks on this map that no record path and no long hop turns back in the door tree (the lemma above), and
// returns the tree length of every table entry's hop chain (a chain can turn back where two hops join).
inline std::vector<U> ChainTree(const Fixture4& f, const std::vector<U>& active, const std::vector<U>& index, const DoorTree& dt, const Level& l, W& pathsChecked, W& turning) {
  auto nodeOf = [&](U area) { return dt.node[active[area]]; }; const U n = U(l.out.size());
  for (int pass = 0; pass < 2; ++pass) for (U a = 0; a < n; ++a) { const Lists& L = pass ? l.in[a] : l.out[a];
    for (U i = 0; i < std::min(l.K, L.count); ++i) { const Att& r = L.a[i]; if (r.t == 0) continue; const auto& e = f.g.edges[r.reach]; const U other = index[pass ? e.from : e.to];
      const U whole = dt.Dist(nodeOf(a), nodeOf(l.wp[r.wp])), step = dt.Dist(nodeOf(a), nodeOf(other)), rest = dt.Dist(nodeOf(other), nodeOf(l.wp[r.wp]));
      Need(whole != BIG && step <= 1 && step + rest == whole, "a least-time record path turns back in the door tree"); ++pathsChecked; } }
  for (U x = 0; x < l.m; ++x) for (const Hop& h : l.hops[x]) { if (h.kind != 2) continue; U at = l.wp[x], len = 0;
    for (U i = 0; i < h.pathLen; ++i) { const U to = index[f.g.edges[l.paths[h.pathStart + i]].to]; len += dt.Dist(nodeOf(at), nodeOf(to)); at = to; }
    Need(len == dt.Dist(nodeOf(l.wp[x]), nodeOf(l.wp[h.to])), "a long hop turns back in the door tree"); ++pathsChecked; }
  std::vector<U> ct(l.cx.size(), NONE); std::vector<size_t> stack;
  for (U y = 0; y < l.m; ++y) for (size_t p0 = l.col[y]; p0 < l.col[y + 1]; ++p0) { size_t p = p0; stack.clear();
    while (ct[p] == NONE) { const U x = l.cx[p]; if (x == y) { ct[p] = 0; break; } stack.push_back(p); p = l.FindEntry(l.hops[x][l.cn[p]].to, y); Need(p != SIZE_MAX, "chain entry missing"); Need(stack.size() <= l.cx.size(), "hop chain does not end"); }
    while (!stack.empty()) { const size_t q = stack.back(); stack.pop_back(); const U x = l.cx[q]; const Hop& h = l.hops[x][l.cn[q]]; ct[q] = dt.Dist(nodeOf(l.wp[x]), nodeOf(l.wp[h.to])) + ct[l.FindEntry(h.to, y)]; } }
  for (U y = 0; y < l.m; ++y) for (size_t p = l.col[y]; p < l.col[y + 1]; ++p) turning += ct[p] != dt.Dist(nodeOf(l.wp[l.cx[p]]), nodeOf(l.wp[y]));
  return ct;
}

} // namespace nl
