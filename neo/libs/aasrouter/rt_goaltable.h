#pragma once
// R2 router, engine port (director, 2026-11-17): A TABLE OF NEXT REACHES PER GOAL, in front of the lists' ask.
//
// Why. The lists' proof covers the route that ONE ask fixes. The engine asks again at every area and every tic
// (idAASLocal::WalkPathToGoal, idAI::GetMovePos). Asked that way the lists loop: on the benchmark map 54.5 % of the
// pairs with a route never arrive (../core_reask_test.cpp). And every ask costs the whole ask again.
//
// What. One table per goal area; a fixed number of goals is kept, the least recently asked one goes first. An ask
// for (area, goal) looks the area up in the goal's table.
//   Hit:  the entry's reach and time. A few steps.
//   Miss: ONE ask of the lists; the route it fixes is written out whole; walking it backwards from the goal, every
//         area on it that has no entry yet gets its reach of that route and its time to the goal. Where a route
//         passes an area twice, the later pass counts. An entry that exists is never changed.
// So from any area with an entry, the entries lead along written routes to the goal: a walk that asks again at
// every area arrives and cannot cycle. "No route" is kept as an entry too.
// What it gives up: where a new route joins an older one, the walk is the new route's head and the older route's
// tail; that walk is not the one route the bake proved for its source. Measured on two maps, about one pair in
// ten thousand breaks the quality rule 4 T <= 5 exact + 684 (../README.md section 9).
//
// Doors. Any door change may change any answer: Clear() voids every entry. The caller calls it on every change.
// This first form serves a set WITHOUT a door layer (the gate with its necks in front of the lists).
//
// Memory: goals kept x areas of the map x 12 bytes, allocated once. One thread, like the rest of the runtime.
// A refusal (Need) is thrown as everywhere in the runtime; the engine's caller switches the set off.
#include "rt_gate.h"
namespace nl {
struct GoalTable {
  struct Cell { U edge = NONE; U time = 0; U stamp = 0; };   // edge: the reach to take, NONE = no route; time: reach times from here to the goal; valid while stamp is its slot's
  struct Slot { U goal = NONE; U stamp = 1; W used = 0; std::vector<Cell> cells; };
  const Runtime& rt; const NeckGate& gate;
  std::vector<Slot> slots; std::vector<U> slotOf;            // slotOf: per goal area its slot, or NONE
  std::vector<U> reaches, buf, mark; U markStamp = 0; W clock = 0;
  W asks = 0, misses = 0, evictions = 0, clears = 0;         // counted for the caller's statistics
  GoalTable(const Runtime& rt_, const NeckGate& gate_, U goalsKept) : rt(rt_), gate(gate_) {
    const U N = rt.f.g.n; Need(goalsKept >= 1 && goalsKept <= 65536, "goal table: the number of goals kept");
    slots.resize(goalsKept); for (auto& s : slots) s.cells.resize(N); slotOf.assign(N, NONE); mark.assign(N, 0);
    reaches.resize(4 * size_t(rt.n) + 16); buf.resize(std::max<U>(rt.maxBack, 1)); }
  size_t Bytes() const { return slots.size() * (size_t(rt.f.g.n) * sizeof(Cell)) + slotOf.size() * sizeof(U) + mark.size() * sizeof(U) + reaches.size() * sizeof(U) + buf.size() * sizeof(U); }
  // A door changed (or anything else that may change an answer): every entry is void.
  void Clear() { ++clears; for (auto& s : slots) Void(s); }
  // One ask. False: no route. True: `first` is the reach to take from `area`, `time` the time of the walk the table
  // fixes from the area's centre (Origin of that reach + the reach times to the goal + 1, as the lists give it).
  bool Ask(U area, U goal, U& first, W& time, Meter& m) {
    ++asks; m.Tick(3 + 3); U si = slotOf[goal];
    if (si == NONE) { si = 0; for (U i = 1; i < slots.size(); ++i) { m.Tick(2); if (slots[i].used < slots[si].used) si = i; }   // the least recently asked goal goes
      Slot& t = slots[si]; if (t.goal != NONE) { slotOf[t.goal] = NONE; ++evictions; } Void(t); t.goal = goal; slotOf[goal] = si; m.Tick(6); }
    Slot& s = slots[si]; s.used = ++clock; Cell& c = s.cells[area];
    if (c.stamp != s.stamp) { ++misses; Fill(s, area, goal, m); }
    if (c.edge == NONE) return false;
    m.Tick(3); first = c.edge; time = W(rt.origin[c.edge]) + c.time + 1; return true; }
private:
  void Void(Slot& s) { if (++s.stamp == 0) { for (auto& c : s.cells) c.stamp = 0; s.stamp = 1; } }
  void Fill(Slot& s, U area, U goal, Meter& m) {
    Cell& c = s.cells[area]; const U sIx = rt.index[area], gIx = rt.index[goal]; Route r; bool found = false;
    if (sIx != NONE && gIx != NONE && gate.Open(area, goal, m)) { r = rt.Ask(sIx, gIx, m); found = r.found; }
    if (!found) { c.edge = NONE; c.time = 0; c.stamp = s.stamp; m.Tick(3); return; }
    U at = sIx; const U count = rt.Write(r, gIx, U(reaches.size()), reaches.data(), buf, at, m);
    Need(count > 0 && at == gIx, "goal table: a route does not write out whole");
    if (++markStamp == 0) { std::fill(mark.begin(), mark.end(), 0); markStamp = 1; }
    const auto& edges = rt.f.g.edges;
    for (U i = count; i-- > 0;) { const U e = reaches[i]; const auto& edge = edges[e]; m.Tick(4);
      if (mark[edge.from] == markStamp) continue;                // the route passes this area again later: that pass counted
      mark[edge.from] = markStamp; Cell& x = s.cells[edge.from];
      if (x.stamp == s.stamp) continue;                          // an entry that exists is never changed
      U after = 0; if (edge.to != goal) { const Cell& n = s.cells[edge.to]; Need(n.stamp == s.stamp && n.edge != NONE, "goal table: the next area of a route has no entry"); after = n.time; }
      x.edge = e; x.time = edge.w + after; x.stamp = s.stamp; }
    Need(c.stamp == s.stamp && c.edge != NONE, "goal table: the asked area got no entry"); }
};
} // namespace nl
