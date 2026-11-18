#pragma once
// exp-active2, outdoor maps, LAST-LEG PATCHES (director, 2026-11-17). A copy of ../necks/dn_gate.h with ONE line
// changed: it includes dc_walk.h of this folder, so the gate stands in front of the door layer with pockets and
// last-leg patches (../many/ and here) in place of the first door layer (../). Nothing in the gate needs to know
// which layer is behind it: a pocket's tables and a last-leg patch are least-time ways in a map whose necks are
// open, and a least-time way never passes a neck its two ends do not need.
//
// ---- what follows is ../necks/dn_gate.h ----
// exp-active2, outdoor maps (director, 2026-11-16): NARROW NECKS together with WIDE DOORS.
// The user: "Make a narrow neck+wide fixture and test it". It adds to ../dc_layer.h and ../dc_walk.h, which it does
// not change. Include it after dn_native.h.
//
// WHY SO LITTLE IS NEEDED. A narrow neck is a door area whose every connection is a bridge of the map WITH EVERY
// DOOR OPEN (the bridge rule is applied with every door open, wide doors too). So the necks cut the whole map,
// wide doors included, into PIECES, and pieces and necks form a forest. Then:
//   1. Any route between two areas passes every neck on the forest path between them, and a fastest route passes
//      no other neck: a route that leaves that path through a neck must come back through the same neck, so it
//      stands on that area twice and is not fastest.
//   2. So a route exists under the current doors exactly when every neck on the forest path between its ends is
//      open (THE GATE below) and a route exists with every neck open (the door layer and the lists say that).
//   3. And when the gate is open, the fastest time is the fastest time with EVERY neck open, because the necks off
//      the path do not matter to a fastest route. The answer of Doors::Ask, which never looks at a neck, is
//      therefore right as it is: its lists' routes pass only the necks their pair needs (proven at the lists'
//      bake), its legs on To are least ways and its table legs too (a least way never passes a bridge it does
//      not need), and the necks its route needs are on the forest path between the ends, which is open.
// So: the gate first; then the unchanged ask. The lists' start-patch ladder still wants the closed necks
// (Runtime::closed), as on the stitched maps.
//
// WALKERS. When a neck changes, a walker may keep its route while the gate is open between the area it stands on
// and its goal: what is left of its route is a route between those two and passes only necks on that path.
// If the gate is shut it must ask again (the answer is "no route"). Wide doors: as ../dc_walk.h says.
//
// The gate is built at load from the map and the door kinds (as the door labels of the stitched router were);
// that is not charged. Its questions and its upkeep are charged with the unit prices of ../../../COUNTING.md.
#include "dc_walk.h"
namespace nl {

struct NeckGate {
  std::vector<U> node;                 // per area of the map: its piece, or pieces + its neck number; NONE: no area
  std::vector<U> neckOf, neckArea;     // per area: its neck number or NONE; per neck: its area
  std::vector<std::vector<U>> adj;     // the forest: per node the nodes it touches (a neck touches pieces and, where two necks meet, necks)
  std::vector<uint8_t> shut;           // per neck: 1 = closed now
  std::vector<U> comp, queue;          // per node: its part of the forest under the current necks; NONE: a closed neck
  U pieces = 0, necks = 0;
  // `open` is the map with EVERY door enabled, wide doors and necks; `kinds` its bridge-rule classification.
  NeckGate(const Fixture4& open, const s3c::DoorClassification& kinds) : node(open.g.n, NONE), neckOf(open.g.n, NONE) {
    const U N = open.g.n; const auto und = s3c::Undirected(open); std::vector<uint8_t> live(N); std::vector<W> sizes;
    for (U a = 0; a < N; ++a) { Need(!open.g.mutableArea[a] || open.g.enabled[a], "the gate is built on the map with every door open"); live[a] = open.g.enabled[a] && !kinds.forced[a]; }
    s3c::Components(und, live, node, sizes); pieces = U(sizes.size());
    for (U a = 0; a < N; ++a) { if (!live[a]) node[a] = NONE; if (open.g.enabled[a] && kinds.forced[a]) { neckOf[a] = necks; node[a] = pieces + necks; neckArea.push_back(a); ++necks; } }
    adj.resize(pieces + necks); shut.assign(necks, 0);
    size_t edges = 0;
    for (U k = 0; k < necks; ++k) for (U u : und[neckArea[k]]) { Need(node[u] != NONE, "a neck beside no area"); auto& mine = adj[pieces + k];
      if (std::find(mine.begin(), mine.end(), node[u]) == mine.end()) { mine.push_back(node[u]); adj[node[u]].push_back(pieces + k); ++edges; } }
    for (U k = 0; k < necks; ++k) Need(adj[pieces + k].size() >= 2, "a neck that joins nothing");
    Need(edges + CountTrees() == pieces + necks, "pieces and necks do not form a forest"); Meter z; Update(z);      // a forest has as many edges as nodes less trees
  }
  U CountTrees() { U trees = 0; std::vector<U> c(pieces + necks, NONE); for (U s = 0; s < pieces + necks; ++s) { if (c[s] != NONE) continue; queue.assign(1, s); c[s] = trees; for (size_t i = 0; i < queue.size(); ++i) for (U v : adj[queue[i]]) if (c[v] == NONE) { c[v] = trees; queue.push_back(v); } ++trees; } return trees; }
  // The parts of the forest under the current necks. Worked out whole: it depends on the neck state alone.
  void Update(Meter& m) {
    const U nodes = pieces + necks; m.Tick(4); comp.assign(nodes, NONE); U parts = 0;
    for (U s = 0; s < nodes; ++s) { m.Tick(3 + 2 + 3 + 3); if (comp[s] != NONE || (s >= pieces && shut[s - pieces])) continue; m.Tick(2 * 2); queue.assign(1, s); comp[s] = parts;
      for (size_t i = 0; i < queue.size(); ++i) { m.Tick(3 + 2); for (U v : adj[queue[i]]) { m.Tick(3 + 2 + 3 + 3); if (comp[v] != NONE || (v >= pieces && shut[v - pieces])) continue; m.Tick(2 * 2); comp[v] = parts; queue.push_back(v); } }
      m.Tick(2); ++parts; }
    m.Tick(3); }
  // Is every neck on the forest path between the two areas open (and are both areas of the map now)?
  bool Open(U aArea, U bArea, Meter& m) const {
    m.Tick(2 * (4 + 3) + 2 * (2 + 3) + 3); const U x = node[aArea], y = node[bArea]; if (x == NONE || y == NONE) return false;
    return comp[x] != NONE && comp[x] == comp[y]; }
};

// The lists, the door layer and the necks as one: what a router asks and tells about doors.
struct NeckDoors {
  Runtime& rt; Doors& doors; NeckGate gate; std::vector<uint8_t> closedArea; std::vector<U> wide;
  NeckDoors(Runtime& r, Doors& d, const Fixture4& open, const s3c::DoorClassification& kinds) : rt(r), doors(d), gate(open, kinds), closedArea(open.g.n, 0) {
    rt.closed = closedArea.data();                                               // the lists skip a start patch that enters a closed neck
    for (U k = 0; k < gate.necks; ++k) Need(rt.index[gate.neckArea[k]] != NONE && doors.dix[gate.neckArea[k]] == NONE, "a neck must be an area of the lists' map and no door area of the layer"); }
  // A door changes: wide door areas go to the layer, necks to the gate. Any other area is refused.
  void Set(const std::vector<U>& areas, bool nowOpen, Meter& m) {
    wide.clear(); bool neckChanged = false;
    for (U a : areas) { m.Tick(3 + 2 + 4 + 3 + 4 + 3); Need(a < doors.N && (doors.dix[a] != NONE || gate.neckOf[a] != NONE), "a door change for an area that is no door");
      if (doors.dix[a] != NONE) { m.Tick(2); wide.push_back(a); } else { m.Tick(3 + 3 * 2); const U k = gate.neckOf[a]; neckChanged = neckChanged || gate.shut[k] != uint8_t(!nowOpen); gate.shut[k] = !nowOpen; closedArea[a] = !nowOpen; } }
    m.Tick(3 + 3); if (!wide.empty()) doors.Set(wide, nowOpen, m); if (neckChanged) gate.Update(m); }
  DoorRoute Ask(U sArea, U gArea, GoalSide& gs, Meter& m) const {
    m.Tick(3 + 3); Need(sArea < doors.N && gArea < doors.N, "ask outside the map"); if (!gate.Open(sArea, gArea, m)) return DoorRoute();
    return doors.Ask(sArea, gArea, gs, m); }
  // After a neck changed: may a walker that stands on `atArea` keep its route to `goalArea`?
  bool MayKeep(U atArea, U goalArea, Meter& m) const { return gate.Open(atArea, goalArea, m); }
  W StateBytes() const { return doors.StateBytes() + gate.shut.size() + 4 * W(gate.comp.size()) + closedArea.size(); }
  W GateBytes() const { W b = 4 * W(gate.node.size()) + 4 * W(gate.neckOf.size()) + 4 * W(gate.neckArea.size()); for (const auto& v : gate.adj) b += 4 * v.size(); return b; }
};

} // namespace nl
