// exp-active2, part W2 (director, 2026-11-13): bake and prove the native lists that are followed, not stored.
//
//   nl_bake SIDE [LEVELS] [HOPS] [GOALSTRIDE] [STRETCH] [PACKFILE]      (run from the claudespace root)
//     SIDE        2, 4 or 8: the stitched native walk map SIDE x SIDE
//     LEVELS      cap:K:range[:hopRadius],...  default 128:4:512,256:8:3072,1024:8:0   (range 0 = any;
//                 hopRadius > 0 = long hops with stored paths to every waypoint within that time)
//     HOPS        F = hop from waypoint x only to waypoints in Out(x); FB = also to z that keeps x in In(z)
//     GOALSTRIDE  1 = every goal (the proof). k > 1 = every k-th goal: a SAMPLE, printed as one
//     STRETCH     1 = also compare hop-chain times with exact times between waypoints (one flood per waypoint)
//     PACKFILE    write the packed lists there (nl_pack.h) and PACKFILE.certificate.txt; needs GOALSTRIDE 1
//
// A route from s to g, as the game would produce it:
//   1. if (s, g) has a START patch: its reach (the first reach of the exact Origin-aware fastest route);
//   2. while the area reached has a CONTINUATION patch for g: its reach (the next reach of the plain fastest path);
//   3. then the BASE route of the area reached: over the levels, x in Out(area), y in In(g), table entry (x, y):
//      least  t(area, x) + h(x, y) + t(y, g), ties by level, x, y. Walk to x, hop to y, walk to g. No more choices.
// Route time = origin[first reach] + sum of reach times + 1.  Rule: 4 T <= 5 T_fast + 684, time only.
// Forced doors (nl_doors.h): a base route that passes a door its pair does not need is patched like a slow one,
// so every baked route passes only needed doors and stays legal whichever other doors close. A start patch whose
// reach steps into such a door is usable only while that door is open; the pair then carries further start
// patches, in ascending exact time, down to one that passes only needed doors (or to the unpatched route).
// The closure is the one of ../walk-closure/closure_time.cpp; what is new is where the times come from.
#include "nl_pack.h"
#include "nl_doors.h"
using namespace nl;
namespace {
struct Row { std::vector<U> t, y, first, tree, touched; std::vector<size_t> pos; };
struct Base { U t = BIG, level = NONE, wa = NONE, wg = NONE, first = NONE; size_t pos = SIZE_MAX; U tree = 0; };
struct Totals {
  W goals = 0, pairs = 0, reachable = 0, unreachable = 0, baseMissing = 0, cont = 0, start = 0, violate = 0, digest = 0;
  std::vector<Patch> startP, contP;
  W walks = 0, walkReaches = 0, baseByLevel[4] = {}; U maxChain = 0, maxContPerGoal = 0, maxStartPerGoal = 0, maxWalkReaches = 0, maxBuf = 0; W startBand[6] = {};
  double sumRatio = 0; W contBand[7] = {}, doorPatches = 0, doorStepStarts = 0, ladderPairs = 0;
};
inline bool Fails(W time, W exact) { return 4 * time > 5 * exact + 684; }
std::vector<std::array<U, 4>> ParseLevels(const std::string& s) {
  std::vector<std::array<U, 4>> out; std::stringstream ss(s); std::string item;
  while (std::getline(ss, item, ',')) { std::array<U, 4> v{}; Need(sscanf(item.c_str(), "%u:%u:%u:%u", &v[0], &v[1], &v[2], &v[3]) >= 3 && v[0] && v[1] >= 1 && v[1] <= 8, "LEVELS must be cap:K:range[:hopRadius],..."); if (!v[2]) v[2] = BIG; out.push_back(v); }
  Need(!out.empty() && out.size() <= 4, "one to four levels"); return out;
}
}
int main(int argc, char** argv) { try {
  setvbuf(stdout, nullptr, _IONBF, 0);
  const U side = argc > 1 ? U(std::stoul(argv[1])) : 2; Need(side == 2 || side == 4 || side == 8, "SIDE must be 2, 4 or 8");
  const std::string levelSpec = argc > 2 ? argv[2] : "128:4:512,256:8:3072,1024:8:0", hopSpec = argc > 3 ? argv[3] : "FB";
  const U goalStride = argc > 4 ? U(std::stoul(argv[4])) : 1; const bool stretch = argc > 5 && std::string(argv[5]) == "1";
  const std::string packPath = argc > 6 ? argv[6] : ""; Need(packPath.empty() || goalStride == 1, "PACKFILE needs every goal");
  Need(hopSpec == "F" || hopSpec == "FB", "HOPS must be F or FB"); Need(goalStride >= 1, "GOALSTRIDE");
  const auto spec = ParseLevels(levelSpec); const U NL = U(spec.size());
  const double t0 = omp_get_wtime();
  auto f = Native(side); for (U v = 0; v < f.g.n; ++v) if (f.g.mutableArea[v]) f.g.enabled[v] = 1;
  auto doors = ClassifyDoors(f); U loopDoors = 0, forcedDoors = 0;
  for (U v = 0; v < f.g.n; ++v) { if (doors.loop[v]) { f.g.enabled[v] = 0; ++loopDoors; } forcedDoors += doors.forced[v]; }
  NativeGeometry geo(f); std::vector<U> active, index(f.g.n, NONE);
  for (U a = 0; a < f.g.n; ++a) if (f.g.enabled[a]) { index[a] = U(active.size()); active.push_back(a); }
  const U n = U(active.size()); U maxOut = 0, maxIn = 0;
  for (U a : active) { maxOut = std::max(maxOut, f.g.out[a + 1] - f.g.out[a]); maxIn = std::max(maxIn, f.g.in[a + 1] - f.g.in[a]); }
  printf("A W2_RUN fixture=%s side=%u areas=%u enabled=%u reaches=%zu maxReachTime=%u maxOrigin=%u maxOutReaches=%u maxInReaches=%u forcedDoorsOpen=%u loopDoorsClosed=%u threads=%d levels=%s hops=%s goalStride=%u quality=time-only(4T<=5F+684)\n",
         f.name.c_str(), side, f.g.n, n, f.g.edges.size(), geo.maxWeight, geo.maxOrigin, maxOut, maxIn, forcedDoors, loopDoors, omp_get_max_threads(), levelSpec.c_str(), hopSpec.c_str(), goalStride);

  // ---- levels
  std::vector<Level> levels(NL);
  for (U lev = 0; lev < NL; ++lev) { Level& l = levels[lev]; const double b = omp_get_wtime(); l.cap = spec[lev][0]; l.K = spec[lev][1]; l.range = spec[lev][2]; l.hopRadius = spec[lev][3];
    const auto wpAreas = Cover(f, geo, active, l.cap); l.m = U(wpAreas.size()); l.wpOf.assign(n, NONE);
    for (U w = 0; w < l.m; ++w) { l.wp.push_back(index[wpAreas[w]]); l.wpOf[index[wpAreas[w]]] = w; }
    BuildLists(f, active, index, l); CloseLists(f, index, l); BuildHops(f, active, index, l, hopSpec == "FB"); BuildTable(l);
    W outRec = 0, inRec = 0, hopCount = 0, fwd = 0, lng = 0; U maxT = 0, maxH = 0; W shortOut = 0, shortIn = 0;
    for (U a = 0; a < n; ++a) { const U ko = std::min(l.K, l.out[a].count), ki = std::min(l.K, l.in[a].count); outRec += ko; inRec += ki; shortOut += ko < l.K; shortIn += ki < l.K;
      for (U i = 0; i < ko; ++i) maxT = std::max(maxT, l.out[a].a[i].t); for (U i = 0; i < ki; ++i) maxT = std::max(maxT, l.in[a].a[i].t); }
    for (U x = 0; x < l.m; ++x) { hopCount += l.hops[x].size(); for (const Hop& h : l.hops[x]) { fwd += h.kind == 0; lng += h.kind == 2; } }
    for (U h : l.ch) maxH = std::max(maxH, h);
    printf("M W2_LEVEL level=%u cap=%u K=%u range=%u waypoints=%u share=%.6f outRecords=%llu inRecords=%llu areasWithFewerOut=%llu areasWithFewerIn=%llu maxRecordTime=%u outClosureChecked=%llu inClosureChecked=%llu maxOutPathReaches=%u maxInPathReaches=%u hops=%llu forwardHops=%llu longHops=%llu longHopReaches=%zu maxHopsPerWaypoint=%u tableEntries=%zu perWaypoint=%.3f maxChainTime=%u seconds=%.3f\n",
           lev, l.cap, l.K, l.range == BIG ? 0 : l.range, l.m, double(l.m) / n, (unsigned long long)outRec, (unsigned long long)inRec, (unsigned long long)shortOut, (unsigned long long)shortIn, maxT,
           (unsigned long long)l.outClosureChecked, (unsigned long long)l.inClosureChecked, l.maxOutReaches, l.maxInReaches, (unsigned long long)hopCount, (unsigned long long)fwd, (unsigned long long)lng, l.paths.size(), l.maxHops, l.cx.size(), double(l.cx.size()) / l.m, maxH, omp_get_wtime() - b);
  }

  // ---- the forced doors as a tree; the tree length of every table entry's chain
  const DoorTree dt(f, doors); std::vector<std::vector<U>> chainTree(NL); auto nodeOf = [&](U area) { return dt.node[active[area]]; };
  for (U lev = 0; lev < NL; ++lev) { W checked = 0, turning = 0; chainTree[lev] = ChainTree(f, active, index, dt, levels[lev], checked, turning);
    printf("M W2_DOOR_TREE level=%u pieces=%u forcedDoors=%u trees=%u depth=%u recordAndLongHopPathsChecked=%llu noneTurnsBack=1 tableEntriesWhoseChainTurnsBack=%llu\n", lev, dt.pieces, dt.doors, dt.trees, dt.depth, (unsigned long long)checked, (unsigned long long)turning); }

  // ---- optional: how far the hop chains are from exact, waypoint to waypoint
  if (stretch) for (U lev = 0; lev < NL; ++lev) { const Level& l = levels[lev]; W exactPairs = 0, inRange = 0, noChain = 0, chainFails = 0; double sum = 0; double worst = 1; W over[4] = {};
    #pragma omp parallel
    { FullFlood ref(f, geo.maxWeight); W ep = 0, ir = 0, nc = 0, cf = 0, ov[4] = {}; double sm = 0, wr = 1;
      #pragma omp for schedule(dynamic, 8)
      for (U y = 0; y < l.m; ++y) { try { ref.To(active[l.wp[y]]);
        for (U x = 0; x < l.m; ++x) { if (x == y) continue; const U d = ref.d[active[l.wp[x]]]; if (d == BIG) continue; ++ep; if (d >= l.range) continue; ++ir;
          const size_t p = l.FindEntry(x, y); if (p == SIZE_MAX) { ++nc; continue; } const U h = l.ch[p]; Need(h >= d, "hop chain faster than exact");
          const double r = double(h) / d; sm += r; wr = std::max(wr, r); cf += Fails(h, d); ov[0] += h > d; ov[1] += 100ULL * h > 105ULL * d; ov[2] += 100ULL * h > 110ULL * d; ov[3] += 100ULL * h > 125ULL * d; } } catch (const std::exception& e) { Die("stretch", e); } }
      #pragma omp critical
      { exactPairs += ep; inRange += ir; noChain += nc; chainFails += cf; sum += sm; worst = std::max(worst, wr); for (int i = 0; i < 4; ++i) over[i] += ov[i]; }
    }
    printf("M W2_STRETCH level=%u reachableWaypointPairs=%llu exactTimeBelowRange=%llu ofThoseWithoutChainBelowRange=%llu meanChainOverExact=%.6f worst=%.4f chainSlower=%llu over5pct=%llu over10pct=%llu over25pct=%llu chainAloneBreaksRule=%llu\n",
           lev, (unsigned long long)exactPairs, (unsigned long long)inRange, (unsigned long long)noChain, inRange > noChain ? sum / double(inRange - noChain) : 0.0, worst, (unsigned long long)over[0], (unsigned long long)over[1], (unsigned long long)over[2], (unsigned long long)over[3], (unsigned long long)chainFails);
  }

  // ---- the proof: every goal, every source
  const double begin = omp_get_wtime(); std::vector<Totals> per(omp_get_max_threads());
  #pragma omp parallel
  {
    Totals& T = per[omp_get_thread_num()]; FullFlood ref(f, geo.maxWeight); Walker wk(f, levels, active, index);
    std::vector<Row> rows(NL); for (U lev = 0; lev < NL; ++lev) { const U m = levels[lev].m; rows[lev].t.assign(m, BIG); rows[lev].y.assign(m, NONE); rows[lev].first.assign(m, NONE); rows[lev].tree.assign(m, 0); rows[lev].pos.assign(m, SIZE_MAX); }
    std::vector<Base> base(n); std::vector<U> fin(n), firstC(n), chain(n), next(n), exactReach(n), startReach(n), order, distG, queue; std::vector<std::pair<W, U>> rungs; std::vector<W> exactTime(n); std::vector<uint8_t> contPatch(n), startPatch(n);
    #pragma omp for schedule(dynamic, 4)
    for (U gi = 0; gi < n; gi += goalStride) { try {
      const U goal = active[gi]; ref.To(goal); ++T.goals; dt.From(nodeOf(gi), distG, queue);
      // the goal's rows: for every waypoint x of a level, the best  h(x, y) + t(y, goal)  over the goal's In records
      for (U lev = 0; lev < NL; ++lev) { const Level& l = levels[lev]; Row& r = rows[lev]; for (U x : r.touched) r.t[x] = BIG;
        r.touched.clear(); const Lists& tail = l.in[gi];
        for (U j = 0; j < std::min(l.K, tail.count); ++j) { const Att& z = tail.a[j]; const U tailTree = distG[nodeOf(l.wp[z.wp])];
          for (size_t p = l.col[z.wp]; p < l.col[z.wp + 1]; ++p) { const U x = l.cx[p], t = l.ch[p] + z.t;
            if (t < r.t[x] || (t == r.t[x] && z.wp < r.y[x])) { if (r.t[x] == BIG) r.touched.push_back(x); r.t[x] = t; r.y[x] = z.wp; r.pos[x] = p; r.first[x] = x != z.wp ? l.hops[x][l.cn[p]].first : z.far; r.tree[x] = chainTree[lev][p] + tailTree; } } } }
      // every source: its base route, the exact Origin-aware time and first reach
      order.clear(); U contThis = 0, startThis = 0;
      for (U si = 0; si < n; ++si) { const U s = active[si]; Base a;
        if (si == gi) a.t = 0;
        else for (U lev = 0; lev < NL; ++lev) { const Level& l = levels[lev]; const Row& r = rows[lev]; const Lists& head = l.out[si];
          for (U i = 0; i < std::min(l.K, head.count); ++i) { const Att& h = head.a[i]; if (r.t[h.wp] == BIG) continue; const U t = h.t + r.t[h.wp];
            if (std::tie(t, lev, h.wp, r.y[h.wp]) < std::tie(a.t, a.level, a.wa, a.wg)) { a = {t, lev, h.wp, r.y[h.wp], h.reach != NONE ? h.reach : r.first[h.wp], r.pos[h.wp], r.tree[h.wp]}; Need(a.first != NONE, "base route without a first reach"); } } }
        if (a.t != BIG && si != gi) a.tree += dt.Dist(nodeOf(si), nodeOf(levels[a.level].wp[a.wa]));     // tree length of the whole base route
        base[si] = a; contPatch[si] = 0; startPatch[si] = 0; chain[si] = 0; next[si] = NONE; ++T.pairs;
        if (ref.d[s] == BIG) { ++T.unreachable; Need(a.t == BIG, "base route to an unreachable goal"); fin[si] = BIG; firstC[si] = NONE; continue; }
        ++T.reachable; order.push_back(si); T.baseMissing += a.t == BIG;
        W bestKey = UINT64_MAX; U bestReach = NONE;
        for (U e = f.g.out[s]; e < f.g.out[s + 1]; ++e) { const U to = f.g.edges[e].to; if (!f.g.enabled[to] || ref.d[to] == BIG) continue; const W key = W(geo.origin[e]) + f.g.edges[e].w + ref.d[to]; if (key < bestKey) { bestKey = key; bestReach = e; } }
        exactTime[si] = si == gi ? 0 : bestKey + 1; exactReach[si] = bestReach; }
      // continuation closure, nearest first
      std::sort(order.begin(), order.end(), [&](U x, U y) { return std::tie(ref.d[active[x]], x) < std::tie(ref.d[active[y]], y); });
      for (U si : order) { const U s = active[si], d = ref.d[s]; const U t = base[si].t; const bool unneededDoor = t != BIG && !Fails(t, d) && base[si].tree != distG[dt.node[s]];
        if (t == BIG || Fails(t, d) || unneededDoor) { ++contThis; contPatch[si] = 1; U best = NONE, bestKey = BIG; T.doorPatches += unneededDoor; ++T.contBand[d <= 128 ? 0 : d <= 256 ? 1 : d <= 512 ? 2 : d <= 1365 ? 3 : d <= 2730 ? 4 : d <= 5460 ? 5 : 6];
          for (U e = f.g.out[s]; e < f.g.out[s + 1]; ++e) { const U to = f.g.edges[e].to; if (!f.g.enabled[to] || ref.d[to] == BIG) continue; const U key = f.g.edges[e].w + ref.d[to]; if (key < bestKey) { bestKey = key; best = e; } }
          Need(best != NONE && bestKey == d, "plain next reach missing"); const U ti = index[f.g.edges[best].to]; Need(ref.d[f.g.edges[best].to] < d, "plain next reach does not descend");
          Need(fin[ti] != BIG, "continuation suffix missing"); fin[si] = f.g.edges[best].w + fin[ti]; firstC[si] = best; next[si] = best; chain[si] = chain[ti] + 1; T.maxChain = std::max(T.maxChain, chain[si]);
          Need(!Fails(fin[si], d), "continuation patch did not repair"); T.contP.push_back({gi, si, best});
          Need(dt.Dist(dt.node[s], dt.node[f.g.edges[best].to]) + distG[dt.node[f.g.edges[best].to]] == distG[dt.node[s]], "the plain next reach turns back in the door tree"); }
        else { fin[si] = t; firstC[si] = base[si].first;
          if (si != gi) { ++T.baseByLevel[base[si].level]; uint8_t* mark = &levels[base[si].level].used[base[si].pos];
            #pragma omp atomic write
            *mark = 1;
          } } }
      // start checks against the exact Origin-aware time
      for (U si : order) { if (si == gi) continue; const U s = active[si]; const W exact = exactTime[si]; Need(exactReach[si] != NONE, "reference start missing");
        const W a = W(geo.origin[firstC[si]]) + fin[si] + 1; Need(a >= exact, "route faster than the exact time");
        if (!Fails(a, exact)) continue;      // passes with every door open, so under any doors (closing a door never lowers the exact time)
        // the ladder: the first reaches the standing route fails against, in ascending exact time; it ends at the first one that passes only needed doors
        rungs.clear(); const U needS = distG[dt.node[s]]; bool top = true, sawStep = false;
        for (U e = f.g.out[s]; e < f.g.out[s + 1]; ++e) { const U to = f.g.edges[e].to; if (!f.g.enabled[to] || ref.d[to] == BIG) continue; const W key = W(geo.origin[e]) + f.g.edges[e].w + ref.d[to] + 1; if (Fails(a, key)) rungs.push_back({key, e}); }
        std::sort(rungs.begin(), rungs.end()); Need(!rungs.empty() && rungs[0].first == exact && rungs[0].second == exactReach[si], "start ladder does not begin at the exact first reach");
        for (const auto& rung : rungs) { const U e = rung.second, to = f.g.edges[e].to; const bool needed = dt.Dist(dt.node[s], dt.node[to]) + distG[dt.node[to]] == needS;
          const W patched = W(geo.origin[e]) + f.g.edges[e].w + fin[index[to]] + 1; Need(!Fails(patched, rung.first) && patched >= rung.first, "start patch did not repair");
          T.startP.push_back({gi, si, e}); ++startThis; T.doorStepStarts += !needed; sawStep = sawStep || !needed;
          if (top) { top = false; startPatch[si] = 1; startReach[si] = e; const U band = exact <= 128 ? 0 : exact <= 256 ? 1 : exact <= 512 ? 2 : exact <= 1365 ? 3 : exact <= 2730 ? 4 : 5; ++T.startBand[band]; }
          if (needed) break; }
        T.ladderPairs += sawStep; }
      // what the game would do, for every source: from the tables; and reach by reach for a sample and every patched source
      for (U si : order) { if (si == gi) continue; const W exact = exactTime[si]; U at = si, first = NONE; W graph = 0;
        if (startPatch[si]) { const U e = startReach[si]; first = e; graph = f.g.edges[e].w; at = index[f.g.edges[e].to]; }
        U guard = 0; while (contPatch[at]) { const U e = next[at]; if (first == NONE) first = e; graph += f.g.edges[e].w; at = index[f.g.edges[e].to]; Need(++guard <= n, "continuation chain does not end"); }
        if (at != gi) { Need(base[at].t != BIG, "replay base missing"); graph += base[at].t; if (first == NONE) first = base[at].first; }
        Need(first != NONE, "replay without a first reach"); const W time = W(geo.origin[first]) + graph + 1; if (Fails(time, exact) || time < exact) ++T.violate;
        T.sumRatio += double(time) / double(exact);
        // what an ask returns: time, first reach, and the route's shape (patched reaches; level and the two waypoints of the base route)
        const W shape = (W(guard + startPatch[si]) << 59) | (at != gi ? (W(base[at].level + 1) << 56) | (W(base[at].wa) << 28) | W(base[at].wg) : 0);
        T.digest ^= Mix(Mix((W(goal) << 32) | active[si]) ^ Mix(time) ^ Mix(W(first) + 0x51ED270B) ^ Mix(shape + 0x7F4A7C15));
        const bool sampled = (gi / goalStride) % 16 == 0 && (startPatch[si] || contPatch[si] || (si + gi) % 61 == 0);
        if (sampled) { wk.Start(si); if (startPatch[si]) wk.Step(startReach[si]);
          while (contPatch[wk.at]) wk.Step(next[wk.at]);
          if (wk.at != gi) { const Base& b = base[wk.at]; wk.Base(b.level, b.wa, b.wg, gi); }
          Need(wk.at == gi && wk.first == first && wk.time == graph, "the walk reach by reach differs from the tables");
          ++T.walks; T.walkReaches += wk.reaches; T.maxWalkReaches = std::max(T.maxWalkReaches, wk.reaches); } }
      T.cont += contThis; T.start += startThis; T.maxContPerGoal = std::max(T.maxContPerGoal, contThis); T.maxStartPerGoal = std::max(T.maxStartPerGoal, startThis); T.maxBuf = std::max(T.maxBuf, wk.maxBuf);
    } catch (const std::exception& e) { Die("proof", e); } }
  }
  Totals t; for (const auto& p : per) { t.startP.insert(t.startP.end(), p.startP.begin(), p.startP.end()); t.contP.insert(t.contP.end(), p.contP.begin(), p.contP.end()); t.goals += p.goals; t.pairs += p.pairs; t.reachable += p.reachable; t.unreachable += p.unreachable; t.baseMissing += p.baseMissing; t.cont += p.cont; t.start += p.start; t.violate += p.violate; t.digest ^= p.digest;
    t.walks += p.walks; t.walkReaches += p.walkReaches; t.sumRatio += p.sumRatio; for (int i = 0; i < 4; ++i) t.baseByLevel[i] += p.baseByLevel[i]; for (int i = 0; i < 6; ++i) t.startBand[i] += p.startBand[i]; for (int i = 0; i < 7; ++i) t.contBand[i] += p.contBand[i]; t.doorPatches += p.doorPatches; t.doorStepStarts += p.doorStepStarts; t.ladderPairs += p.ladderPairs;
    t.maxChain = std::max(t.maxChain, p.maxChain); t.maxContPerGoal = std::max(t.maxContPerGoal, p.maxContPerGoal); t.maxStartPerGoal = std::max(t.maxStartPerGoal, p.maxStartPerGoal); t.maxWalkReaches = std::max(t.maxWalkReaches, p.maxWalkReaches); t.maxBuf = std::max(t.maxBuf, p.maxBuf); }
  const char* scope = goalStride == 1 ? "EVERY-GOAL" : "SAMPLE-OF-GOALS";
  printf("M W2_CLOSURE scope=%s fixture=%s goals=%llu ofGoals=%u orderedPairs=%llu reachable=%llu unreachable=%llu baseRouteMissing=%llu continuationPatches=%llu perGoal=%.6f maxPerGoal=%u maxChain=%u startPatches=%llu perGoal=%.6f maxPerGoal=%u replayViolations=%llu meanRouteTimeOverExact=%.6f digest=%016llx seconds=%.3f\n",
         scope, f.name.c_str(), (unsigned long long)t.goals, n, (unsigned long long)t.pairs, (unsigned long long)t.reachable, (unsigned long long)t.unreachable, (unsigned long long)t.baseMissing, (unsigned long long)t.cont, double(t.cont) / t.goals, t.maxContPerGoal, t.maxChain,
         (unsigned long long)t.start, double(t.start) / t.goals, t.maxStartPerGoal, (unsigned long long)t.violate, t.sumRatio / double(t.reachable - t.goals), (unsigned long long)t.digest, omp_get_wtime() - begin);
  printf("M W2_START_BANDS exactTimeUpTo128=%llu upTo256=%llu upTo512=%llu upTo1365=%llu upTo2730=%llu above=%llu\n", (unsigned long long)t.startBand[0], (unsigned long long)t.startBand[1], (unsigned long long)t.startBand[2], (unsigned long long)t.startBand[3], (unsigned long long)t.startBand[4], (unsigned long long)t.startBand[5]);
  printf("M W2_CONT_BANDS plainTimeUpTo128=%llu upTo256=%llu upTo512=%llu upTo1365=%llu upTo2730=%llu upTo5460=%llu above=%llu\n", (unsigned long long)t.contBand[0], (unsigned long long)t.contBand[1], (unsigned long long)t.contBand[2], (unsigned long long)t.contBand[3], (unsigned long long)t.contBand[4], (unsigned long long)t.contBand[5], (unsigned long long)t.contBand[6]);
  printf("M W2_DOORS continuationPatchesOnlyBecauseTheBasePassesAnUnneededDoor=%llu startPatchesThatStepIntoAnUnneededDoor=%llu pairsWithSuchALadder=%llu (startPatches counts every rung)\n", (unsigned long long)t.doorPatches, (unsigned long long)t.doorStepStarts, (unsigned long long)t.ladderPairs);
  printf("M W2_BASE_BY_LEVEL level0=%llu level1=%llu level2=%llu level3=%llu\n", (unsigned long long)t.baseByLevel[0], (unsigned long long)t.baseByLevel[1], (unsigned long long)t.baseByLevel[2], (unsigned long long)t.baseByLevel[3]);
  printf("M W2_WALKS reachByReach=%llu (every 16th proved goal: every patched source and one source in 61) allEqualTheTables=1 meanReaches=%.3f maxReaches=%u longestBackwardPiece=%u\n", (unsigned long long)t.walks, t.walks ? double(t.walkReaches) / t.walks : 0.0, t.maxWalkReaches, t.maxBuf);

  // ---- what must be kept: entries some base route chose, and the entries their hop chains pass
  W bytesLists = 0, bytesHops = 0, bytesTable = 0, keptAll = 0; std::vector<std::vector<uint8_t>> keeps(NL);
  for (U lev = 0; lev < NL; ++lev) { Level& l = levels[lev]; std::vector<uint8_t>& keep = keeps[lev]; keep = l.used; W chosen = 0, kept = 0;
    for (U y = 0; y < l.m; ++y) for (size_t p = l.col[y]; p < l.col[y + 1]; ++p) { if (!l.used[p]) continue; ++chosen; U x = l.cx[p]; size_t q = p;
      while (x != y) { x = l.hops[x][l.cn[q]].to; q = l.FindEntry(x, y); Need(q != SIZE_MAX, "kept chain broken"); if (keep[q]) break; keep[q] = 1; } }
    for (uint8_t k : keep) kept += k;
    U maxT = 0, maxH = 0; for (U a = 0; a < n; ++a) for (U i = 0; i < 8; ++i) { if (i < std::min(l.K, l.out[a].count)) maxT = std::max(maxT, l.out[a].a[i].t); if (i < std::min(l.K, l.in[a].count)) maxT = std::max(maxT, l.in[a].a[i].t); }
    for (size_t p = 0; p < l.cx.size(); ++p) if (keep[p]) maxH = std::max(maxH, l.ch[p]);
    const U wpB = l.m <= 65535 ? 2 : 3, tB = maxT <= 65535 ? 2 : 3, hB = maxH <= 65535 ? 2 : 3; W hopCount = 0, longCount = 0; for (U x = 0; x < l.m; ++x) { hopCount += l.hops[x].size(); for (const Hop& h : l.hops[x]) longCount += h.kind == 2; }
    const W lists = W(n) * l.K * ((wpB + tB + 1) + (wpB + tB + 2)), hopB = hopCount * (wpB + 4) + 4ULL * l.m + 9 * longCount + l.paths.size(), table = kept * (wpB + hB + 1) + 4ULL * l.m;
    bytesLists += lists; bytesHops += hopB; bytesTable += table; keptAll += kept;
    printf("D W2_KEEP level=%u tableEntries=%zu chosenByABaseRoute=%llu keptWithTheirChains=%llu perEnabledArea=%.4f waypointBytes=%u timeBytes=%u chainTimeBytes=%u listBytes=%llu hopListBytes=%llu tableBytes=%llu\n",
           lev, l.cx.size(), (unsigned long long)chosen, (unsigned long long)kept, double(kept) / n, wpB, tB, hB, (unsigned long long)lists, (unsigned long long)hopB, (unsigned long long)table);
  }
  const W patchBytes = 4 * (t.cont + t.start) + 8ULL * n, total = bytesLists + bytesHops + bytesTable + patchBytes;
  printf("D W2_BYTES scope=%s lists=%llu hopLists=%llu tables=%llu patchesAt4BytesPlusTwoGoalIndexes=%llu total=%llu perEnabledArea=%.3f (lists %.3f, hop lists %.3f, tables %.3f, patches %.3f)%s\n", scope,
         (unsigned long long)bytesLists, (unsigned long long)bytesHops, (unsigned long long)bytesTable, (unsigned long long)patchBytes, (unsigned long long)total, double(total) / n, double(bytesLists) / n, double(bytesHops) / n, double(bytesTable) / n, double(patchBytes) / n,
         goalStride == 1 ? "" : " tables and patches UNDERCOUNTED: only the sampled goals chose entries");
  if (t.violate) { puts("M W2_DEAD replay violations"); return 3; }
  if (!packPath.empty()) {
    auto byGoal = [](const Patch& a, const Patch& b) { return std::tie(a.goal, a.area) < std::tie(b.goal, b.area); };
    std::stable_sort(t.startP.begin(), t.startP.end(), byGoal); std::sort(t.contP.begin(), t.contP.end(), byGoal);     // stable: a pair's ladder keeps its order
    Need(t.startP.size() == t.start && t.contP.size() == t.cont, "patch lists against their counts");
    const Blob blob = Pack(f, active, levels, keeps, t.startP, t.contP, t.maxChain + 1); blob.Save(packPath);
    W lists = 0, hopsB = 0, tables = 0, patches = 0, other = 0;
    for (const auto& s : blob.sec) { const std::string& nm = s.first; const W z = s.second.size();
      if (nm.find(".out") != std::string::npos || nm.find(".in") != std::string::npos) lists += z; else if (nm.find(".hop") != std::string::npos) hopsB += z; else if (nm.find(".table") != std::string::npos || nm.find(".colIndex") != std::string::npos) tables += z;
      else if (nm == "start" || nm == "cont" || nm == "startIndex" || nm == "contIndex") patches += z; else other += z;
      printf("M W2_SECTION name=%s bytes=%llu\n", nm.c_str(), (unsigned long long)z); }
    printf("M W2_PACK file=%s bytes=%llu perEnabledArea=%.3f (lists %.3f, hop lists %.3f, tables %.3f, patches %.3f, waypoint areas and header %.3f) longestPatchedPrefix=%u\n", packPath.c_str(), (unsigned long long)blob.Bytes(), double(blob.Bytes()) / n,
           double(lists) / n, double(hopsB) / n, double(tables) / n, double(patches) / n, double(other) / n, t.maxChain + 1);
    FILE* cert = fopen((packPath + ".certificate.txt").c_str(), "wb"); Need(cert != nullptr, "cannot write the certificate");
    fprintf(cert, "fixture=%s\nlevels=%s\nhops=%s\nenabled=%u\norderedPairs=%llu\nreachable=%llu\nunreachable=%llu\ncontinuationPatches=%llu\nstartPatches=%llu\nreplayViolations=%llu\ndigest=%016llx\npackedBytes=%llu\n",
            f.name.c_str(), levelSpec.c_str(), hopSpec.c_str(), n, (unsigned long long)t.pairs, (unsigned long long)t.reachable, (unsigned long long)t.unreachable, (unsigned long long)t.cont, (unsigned long long)t.start, (unsigned long long)t.violate, (unsigned long long)t.digest, (unsigned long long)blob.Bytes());
    fclose(cert);
  }
  printf("M W2_COMPLETE status=%s seconds=%.3f\n", goalStride == 1 ? "EVERY-ORDERED-PAIR-CLOSED" : "SAMPLED-GOALS-CLOSED", omp_get_wtime() - t0); return 0;
} catch (const std::exception& e) { fprintf(stderr, "M W2_REFUSAL %s\n", e.what()); printf("M W2_REFUSAL %s\n", e.what()); return 2; } }
