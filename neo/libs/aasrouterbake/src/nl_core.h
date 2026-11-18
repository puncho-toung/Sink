#pragma once
// exp-active2, part W2 (director, 2026-11-13): native lists whose routes are FOLLOWED from records, not stored.
//
// A level is a set of waypoints. Every area keeps, per level:
//   Out: its K nearest waypoints by plain graph time (ties: lower waypoint number), each with the time and the
//        FIRST reach of a least-time path to it;
//   In:  its K nearest waypoints by plain graph time INTO the area, each with the time, the LAST reach of a
//        least-time path from it, and the first reach of that same path ("far").
// Closure (proved in ../consult-w2/SYNTHESIS.md, checked here for every record): the next area on an Out path
// keeps the same waypoint in its Out list; the area before on an In path keeps the same waypoint in its In list.
// So an Out path can be walked forward from the records, and an In path can be read backward from its end.
//
// Waypoint to waypoint, the walker HOPS: from waypoint x to a waypoint z in Out(x) (walked forward), or to a
// waypoint z that keeps x in In(z) (read backward from z). A table holds, for (x, y), the least total time of a
// chain of hops from x to y and the first hop of one such chain. A level may also have LONG hops: from x to every
// waypoint within the level's hop radius, with the reaches of a least-time path stored (one number per reach).
// Only long hops store a path; they are affordable where waypoints are few (the coarsest level).
//
// Plain graph time everywhere; Origin is paid once, by the first reach of a whole route (nl_bake.cpp).
#include "level_data.h"   // fixture, geometry (origin), Cover(), Mix(), Need()
namespace nl {

inline void Die(const char* where, const std::exception& e) {   // a refusal inside a parallel loop: say so, then stop
  #pragma omp critical(nl_die)
  { printf("M W2_REFUSAL %s: %s\n", where, e.what()); fflush(stdout); std::_Exit(2); }
}
struct Att { U wp = NONE, t = BIG, reach = NONE, far = NONE; };   // Out: reach = first reach. In: reach = last, far = first
struct Lists { std::array<Att, 8> a; U count = 0; };
struct Hop { U to = NONE, w = BIG, first = NONE; uint8_t kind = 0; U pathStart = 0, pathLen = 0; };   // kind 0: forward (to is in Out(x)); 1: backward (x is in In(to)); 2: long (stored path)

struct Level {
  U cap = 0, K = 8, range = BIG, m = 0, hopRadius = 0;
  std::vector<U> wp;                     // waypoint number -> area (enabled index), ascending
  std::vector<U> wpOf;                   // area (enabled index) -> waypoint number, or NONE
  std::vector<Lists> out, in;            // per area (enabled index); only the first K records are used
  std::vector<std::vector<Hop>> hops;    // per waypoint, in a fixed order: forward, backward, long
  std::vector<U> paths;                  // the reaches of the long hops
  std::vector<size_t> col;               // table by target y: entries [col[y], col[y+1]) sorted by source x
  std::vector<U> cx, ch;                 // source waypoint, time of the hop chain
  std::vector<uint8_t> cn;               // index of the first hop in hops[x] (255 for x == y)
  std::vector<uint8_t> used;             // entry chosen by some pair's base route
  U maxInReaches = 0, maxOutReaches = 0, maxHops = 0; W inClosureChecked = 0, outClosureChecked = 0;
  const Att* FindOut(U area, U x) const { const Lists& L = out[area]; for (U i = 0, k = std::min(K, L.count); i < k; ++i) if (L.a[i].wp == x) return &L.a[i]; return nullptr; }
  const Att* FindIn(U area, U y) const { const Lists& L = in[area]; for (U i = 0, k = std::min(K, L.count); i < k; ++i) if (L.a[i].wp == y) return &L.a[i]; return nullptr; }
  size_t FindEntry(U x, U y) const { auto b = cx.begin() + col[y], e = cx.begin() + col[y + 1]; auto it = std::lower_bound(b, e, x); return it != e && *it == x ? size_t(it - cx.begin()) : SIZE_MAX; }
};

// Least plain time outward (forward) or inward (reverse) from one area, areas settled in (time, area) order.
// via = the reach at the root's end of the path, par = the reach at the settled area's end.
struct NearFlood {
  const Fixture4& f; std::vector<U> d, via, par, touched; std::vector<std::pair<U, U>> heap;
  explicit NearFlood(const Fixture4& fx) : f(fx), d(fx.g.n, BIG), via(fx.g.n, NONE), par(fx.g.n, NONE) {}
  template <class Visit> void Run(U root, bool reverse, Visit&& visit) {
    for (U v : touched) { d[v] = BIG; via[v] = par[v] = NONE; }
    touched.clear(); heap.clear(); d[root] = 0; touched.push_back(root); heap.push_back({0, root});
    auto relax = [&](U v, U to, U e, U c) {
      if (c >= d[to]) return;
      if (d[to] == BIG) touched.push_back(to);
      d[to] = c; via[to] = v == root ? e : via[v]; par[to] = e;
      heap.push_back({c, to}); std::push_heap(heap.begin(), heap.end(), std::greater<>()); };
    while (!heap.empty()) {
      std::pop_heap(heap.begin(), heap.end(), std::greater<>()); const U dv = heap.back().first, v = heap.back().second; heap.pop_back();
      if (d[v] != dv) continue;
      if (!visit(v, dv)) break;
      if (!reverse) for (U e = f.g.out[v]; e < f.g.out[v + 1]; ++e) { const auto& ed = f.g.edges[e]; if (f.g.enabled[ed.to]) relax(v, ed.to, e, dv + ed.w); }
      else for (U j = f.g.in[v]; j < f.g.in[v + 1]; ++j) { const U e = f.g.rev[j]; const auto& ed = f.g.edges[e]; if (f.g.enabled[ed.from]) relax(v, ed.from, e, dv + ed.w); }
    }
  }
};

// Least plain time from every area to one goal (the exact reference of the proof). Dial buckets.
struct FullFlood {
  const Fixture4& f; U mask = 0; std::vector<std::vector<U>> bucket; std::vector<U> d, order;
  FullFlood(const Fixture4& fx, U maxWeight) : f(fx), d(fx.g.n, BIG) { U c = 1; while (c <= maxWeight) c *= 2; bucket.resize(c); mask = c - 1; order.reserve(fx.g.n); }
  void To(U goal) {
    std::fill(d.begin(), d.end(), BIG); order.clear(); d[goal] = 0; bucket[0].push_back(goal); W queued = 1; U cur = 0;
    while (queued) {
      auto& b = bucket[cur & mask]; if (b.empty()) { ++cur; continue; }
      const U v = b.back(); b.pop_back(); --queued; if (d[v] != cur) continue;
      order.push_back(v);
      for (U j = f.g.in[v]; j < f.g.in[v + 1]; ++j) { const auto& e = f.g.edges[f.g.rev[j]]; if (!f.g.enabled[e.from]) continue; const U c = cur + e.w; if (c < d[e.from]) { d[e.from] = c; bucket[c & mask].push_back(e.from); ++queued; } }
    }
  }
};

// Out and In lists of one level: two early-stopped floods per area. The first eight waypoints settled are the
// eight nearest by (time, waypoint number), because waypoint numbers ascend with area numbers.
inline void BuildLists(const Fixture4& f, const std::vector<U>& active, const std::vector<U>& index, Level& l) {
  const U n = U(active.size()); l.out.assign(n, {}); l.in.assign(n, {});
  #pragma omp parallel
  {
    NearFlood fl(f);
    #pragma omp for schedule(dynamic, 64)
    for (U i = 0; i < n; ++i) for (int rev = 0; rev < 2; ++rev) {
      Lists& L = rev ? l.in[i] : l.out[i];
      fl.Run(active[i], rev != 0, [&](U v, U dv) { const U w = l.wpOf[index[v]]; if (w != NONE) L.a[L.count++] = {w, dv, fl.via[v], NONE}; return L.count < 8; });
    }
  }
}

// Checks the closure for every used record, and fills In.far, the first reach of the path that reading backward
// from the record actually gives. Also the longest Out and In paths in reaches.
inline void CloseLists(const Fixture4& f, const std::vector<U>& index, Level& l) {
  const U n = U(l.out.size()); std::vector<uint16_t> lenOut(size_t(n) * 8, 0), lenIn(size_t(n) * 8, 0); std::vector<uint8_t> known(size_t(n) * 8, 0);
  std::vector<std::pair<U, U>> chain;
  for (int pass = 0; pass < 2; ++pass) {           // pass 0: Out, pass 1: In
    std::fill(known.begin(), known.end(), 0); auto& lists = pass ? l.in : l.out; auto& len = pass ? lenIn : lenOut;
    for (U a0 = 0; a0 < n; ++a0) for (U s0 = 0; s0 < std::min(l.K, lists[a0].count); ++s0) {
      chain.clear(); U a = a0, s = s0; const U x = lists[a0].a[s0].wp; U baseLen = 0, baseFar = NONE;
      for (;;) {
        Att& r = lists[a].a[s];
        if (known[size_t(a) * 8 + s]) { baseLen = len[size_t(a) * 8 + s]; baseFar = r.far; break; }
        if (r.t == 0) { Need(l.wpOf[a] == x && r.reach == NONE, "self record is not the waypoint"); baseLen = 0; baseFar = NONE; known[size_t(a) * 8 + s] = 1; r.far = NONE; break; }
        chain.push_back({a, s}); Need(chain.size() <= n, "record chain does not end"); Need(r.reach != NONE && r.reach < f.g.edges.size(), "record with a time and no reach");
        const auto& e = f.g.edges[r.reach]; const U here = index[pass ? e.to : e.from], next = index[pass ? e.from : e.to];
        Need(here == a && next != NONE && r.t >= e.w, "record reach does not belong to its area");
        const Att* q = pass ? l.FindIn(next, x) : l.FindOut(next, x);
        Need(q != nullptr, pass ? "In closure broken: the area before does not keep the waypoint" : "Out closure broken: the next area does not keep the waypoint");
        Need(q->t == r.t - e.w, "record times do not add up along the path");
        ++(pass ? l.inClosureChecked : l.outClosureChecked);
        a = next; s = U(q - lists[next].a.data());
      }
      for (size_t i = chain.size(); i-- > 0;) {    // nearest the waypoint first
        Att& r = lists[chain[i].first].a[chain[i].second]; ++baseLen; Need(baseLen < 65535, "path too long for the length field");
        if (pass) { if (baseFar == NONE) baseFar = r.reach; r.far = baseFar; }
        len[size_t(chain[i].first) * 8 + chain[i].second] = uint16_t(baseLen); known[size_t(chain[i].first) * 8 + chain[i].second] = 1;
        (pass ? l.maxInReaches : l.maxOutReaches) = std::max<U>(pass ? l.maxInReaches : l.maxOutReaches, baseLen);
      }
    }
  }
}

inline void BuildHops(const Fixture4& f, const std::vector<U>& active, const std::vector<U>& index, Level& l, bool backward) {
  l.hops.assign(l.m, {});
  for (U x = 0; x < l.m; ++x) { const Lists& L = l.out[l.wp[x]]; for (U i = 0; i < std::min(l.K, L.count); ++i) if (L.a[i].wp != x) l.hops[x].push_back({L.a[i].wp, L.a[i].t, L.a[i].reach, 0}); }
  if (backward) for (U z = 0; z < l.m; ++z) { const Lists& L = l.in[l.wp[z]];
    for (U i = 0; i < std::min(l.K, L.count); ++i) { const U x = L.a[i].wp; if (x == z) continue;
      bool forward = false; for (const Hop& h : l.hops[x]) forward = forward || (h.to == z && h.kind == 0);
      if (!forward) l.hops[x].push_back({z, L.a[i].t, L.a[i].far, 1}); } }
  if (l.hopRadius) {       // long hops: every other waypoint within the radius that is not already a forward hop
    std::vector<std::vector<Hop>> found(l.m); std::vector<std::vector<U>> pathOf(l.m);
    #pragma omp parallel
    {
      NearFlood fl(f); std::vector<U> rev;
      #pragma omp for schedule(dynamic, 16)
      for (U x = 0; x < l.m; ++x) { const U root = active[l.wp[x]];
        fl.Run(root, false, [&](U v, U dv) { if (dv > l.hopRadius) return false; const U z = l.wpOf[index[v]]; if (z == NONE || z == x) return true;
          for (const Hop& h : l.hops[x]) if (h.to == z) return true;   // already a forward or backward hop, at the same time
          rev.clear(); for (U at = v; at != root; at = f.g.edges[fl.par[at]].from) rev.push_back(fl.par[at]);
          Hop h; h.to = z; h.w = dv; h.kind = 2; h.first = rev.back(); h.pathStart = U(pathOf[x].size()); h.pathLen = U(rev.size()); found[x].push_back(h);
          pathOf[x].insert(pathOf[x].end(), rev.rbegin(), rev.rend()); return true; });
      }
    }
    for (U x = 0; x < l.m; ++x) { for (Hop h : found[x]) { h.pathStart += U(l.paths.size()); l.hops[x].push_back(h); } l.paths.insert(l.paths.end(), pathOf[x].begin(), pathOf[x].end()); }
  }
  for (U x = 0; x < l.m; ++x) { Need(l.hops[x].size() < 255, "too many hops for one byte"); l.maxHops = std::max<U>(l.maxHops, U(l.hops[x].size())); }
}

// The table: for every target y, every waypoint x whose least hop chain to y takes less than the level's range.
inline void BuildTable(Level& l) {
  const U m = l.m; std::vector<std::vector<std::array<U, 3>>> radj(m), cols(m);
  for (U x = 0; x < m; ++x) for (U i = 0; i < l.hops[x].size(); ++i) radj[l.hops[x][i].to].push_back({x, l.hops[x][i].w, i});
  #pragma omp parallel
  {
    std::vector<U> h(m, BIG), touched; std::vector<std::pair<U, U>> heap;
    #pragma omp for schedule(dynamic, 16)
    for (U y = 0; y < m; ++y) { try {
      for (U v : touched) h[v] = BIG;
      touched.clear(); heap.clear(); h[y] = 0; touched.push_back(y); heap.push_back({0, y});
      while (!heap.empty()) {
        std::pop_heap(heap.begin(), heap.end(), std::greater<>()); const U hz = heap.back().first, z = heap.back().second; heap.pop_back();
        if (h[z] != hz) continue;
        for (const auto& r : radj[z]) { const U c = hz + r[1]; if (c >= l.range || c >= h[r[0]]) continue; if (h[r[0]] == BIG) touched.push_back(r[0]); h[r[0]] = c; heap.push_back({c, r[0]}); std::push_heap(heap.begin(), heap.end(), std::greater<>()); }
      }
      auto& colY = cols[y]; colY.reserve(touched.size());
      for (U x : touched) { U hop = 255;
        if (x != y) { for (U i = 0; i < l.hops[x].size(); ++i) { const Hop& q = l.hops[x][i]; if (h[q.to] != BIG && q.w + h[q.to] == h[x]) { hop = i; break; } } Need(hop != 255, "hop chain has no first hop"); }
        colY.push_back({x, h[x], hop}); }
      std::sort(colY.begin(), colY.end());
    } catch (const std::exception& e) { Die("table", e); } }
  }
  l.col.assign(m + 1, 0); for (U y = 0; y < m; ++y) l.col[y + 1] = l.col[y] + cols[y].size();
  l.cx.resize(l.col[m]); l.ch.resize(l.col[m]); l.cn.resize(l.col[m]); l.used.assign(l.col[m], 0);
  for (U y = 0; y < m; ++y) for (size_t i = 0; i < cols[y].size(); ++i) { const size_t p = l.col[y] + i; l.cx[p] = cols[y][i][0]; l.ch[p] = cols[y][i][1]; l.cn[p] = uint8_t(cols[y][i][2]); }
  // every entry's first hop leads to an entry for the same target, with the times adding up
  for (U y = 0; y < m; ++y) for (size_t p = l.col[y]; p < l.col[y + 1]; ++p) { const U x = l.cx[p]; if (x == y) { Need(l.ch[p] == 0, "self entry time"); continue; }
    const Hop& q = l.hops[x][l.cn[p]]; const size_t r = l.FindEntry(q.to, y); Need(r != SIZE_MAX && l.ch[r] + q.w == l.ch[p], "table entry does not continue"); }
}

// What the game would do with one base route: follows records only, one reach at a time.
struct Walker {
  const Fixture4& f; const std::vector<Level>& levels; const std::vector<U>& active; const std::vector<U>& index;
  std::vector<U> buf; W time = 0; U reaches = 0, first = NONE, at = NONE, maxBuf = 0;
  Walker(const Fixture4& fx, const std::vector<Level>& lv, const std::vector<U>& ac, const std::vector<U>& ix) : f(fx), levels(lv), active(ac), index(ix) {}
  void Start(U area) { time = 0; reaches = 0; first = NONE; at = area; }
  void Step(U e) {
    Need(e < f.g.edges.size() && f.g.edges[e].from == active[at] && f.g.enabled[f.g.edges[e].to], "walk: the reach does not leave the current area");
    time += f.g.edges[e].w; ++reaches; if (first == NONE) first = e; at = index[f.g.edges[e].to]; Need(reaches <= 4 * f.g.n, "walk does not end"); }
  void Forward(const Level& l, U x) { while (l.wpOf[at] != x) { const Att* r = l.FindOut(at, x); Need(r != nullptr, "walk: the waypoint left the Out list"); Step(r->reach); } }
  void Backward(const Level& l, U y, U target) {   // from the current area, which is waypoint y, to the target area
    Need(l.wpOf[at] == y, "walk: backward piece does not start on its waypoint"); buf.clear(); U p = target;
    while (l.wpOf[p] != y) { const Att* r = l.FindIn(p, y); Need(r != nullptr, "walk: the waypoint left the In list"); buf.push_back(r->reach); p = index[f.g.edges[r->reach].from]; Need(buf.size() <= f.g.n, "walk: backward piece does not end"); }
    maxBuf = std::max<U>(maxBuf, U(buf.size())); for (size_t i = buf.size(); i-- > 0;) Step(buf[i]);
    Need(at == target, "walk: backward piece missed its target"); }
  void Base(U lev, U x, U y, U goalArea) {
    const Level& l = levels[lev]; Forward(l, x);
    while (x != y) { const size_t p = l.FindEntry(x, y); Need(p != SIZE_MAX, "walk: table entry missing"); const Hop& q = l.hops[x][l.cn[p]];
      if (q.kind == 0) Forward(l, q.to); else if (q.kind == 1) Backward(l, x, l.wp[q.to]); else { for (U i = 0; i < q.pathLen; ++i) Step(l.paths[q.pathStart + i]); Need(at == l.wp[q.to], "walk: long hop missed its waypoint"); }
      x = q.to; }
    Backward(l, y, goalArea); }
};

} // namespace nl
