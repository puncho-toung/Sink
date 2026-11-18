#pragma once
// exp-active2 (director, 2026-11-15): the unchanged version 4 programs of the folder above on the SYNTHETIC
// OUTDOOR walk maps of exp-overlay4 (fixtures4.h, MakeSynthetic4): 65,536 x 65,536 units, 512 x 512 cells.
// Each od_*.cpp includes this file and then the unchanged program. Two names are redirected, nothing else:
//
// 1. `Native(side)` becomes the outdoor map:  SIDE 2 = open terrain,  4 = maze,  8 = mixed
//    (the programs accept only 2, 4 and 8 as SIDE).
//
// 2. `ClassifyDoors` becomes OutdoorClassify. The classifier of the stitched map calls a door FORCED only when
//    every connection of it is a bridge. A district entrance here has two neighbours on one side, so that rule
//    calls it a loop door and the bake would shut it for good, cutting the district off (the first sampled bakes
//    of 2026-11-15 did exactly that: entrances-shut-sample-*.txt). Here a door is FORCED when closing it alone, with
//    every other door open, cuts areas off from each other (a cut vertex of the map); every other door is a LOOP
//    door (a shortcut). The pieces between forced doors are taken with the loop doors shut, as the bake has them.
//    The door tree (nl_doors.h) still refuses at the bake if the forced doors and the pieces do not form a
//    forest, or if a least-time record path or a patch turns back at a door.
//
// Doors on these maps under that rule: the district entrances are forced doors (they open and close); the four
// columns of the open map, the ring shortcut and the long-wall shortcut are loop doors. Every program shuts the
// loop doors, as the bake does: what is tested here is the map with its shortcuts shut. od_loop_open.cpp
// measures what an open shortcut does to the answers.
#include "nl_walk.h"
#include "nl_doors.h"
#include "reachability.h"
inline overlay_four::Fixture4 Outdoor(uint32_t side) {
  if (side != 2 && side != 4 && side != 8) throw std::runtime_error("outdoor SIDE: 2 = open terrain, 4 = maze, 8 = mixed");
  return overlay_four::MakeSynthetic4(side == 2 ? "open" : side == 4 ? "maze" : "mixed", false);
}
// OD_DOORS=bridge in the environment keeps the stitched map's rule. On these maps it makes EVERY door a loop door,
// so every door is shut for good: the map as a set of door-free regions. (The cut rule turned out not to be enough
// for maze and mixed; README.md has what happened.)
inline s3c::DoorClassification OutdoorClassify(const overlay_four::Fixture4& f) {      // call it with every door open, as the programs do
  auto c = s3c::ClassifyDoors(f); const char* rule = getenv("OD_DOORS"); if (rule && !strcmp(rule, "bridge")) return c;
  const auto adj = s3c::Undirected(f); const uint32_t n = f.g.n;
  std::vector<uint32_t> seen(n, 0), queue; uint32_t stamp = 0;
  for (uint32_t v = 0; v < n; ++v) { if (!f.g.mutableArea[v] || !f.g.enabled[v] || c.forced[v] || adj[v].empty()) continue;
    ++stamp; seen[v] = stamp; queue.assign(1, adj[v][0]); seen[adj[v][0]] = stamp;          // what one neighbour reaches without the door
    for (size_t i = 0; i < queue.size(); ++i) for (uint32_t w : adj[queue[i]]) if (seen[w] != stamp) { seen[w] = stamp; queue.push_back(w); }
    bool cut = false; for (uint32_t w : adj[v]) cut = cut || seen[w] != stamp;
    if (cut) { c.forced[v] = 1; c.loop[v] = 0; ++c.forcedCount; --c.loopCount; } }
  std::vector<uint8_t> live(n);
  for (uint32_t v = 0; v < n; ++v) live[v] = f.g.enabled[v] && !c.loop[v];
  s3c::Components(adj, live, c.bakePiece, c.bakeSizes);
  for (uint32_t v = 0; v < n; ++v) live[v] = f.g.enabled[v] && !c.loop[v] && !c.forced[v];   // pieces with the loop doors shut
  s3c::Components(adj, live, c.treePiece, c.treeSizes);
  return c;
}
// OD_WAYPOINTS=off-doors in the environment: a test variant of the cover. The cover (census.cpp, greedy) is free
// to make a forced door a waypoint, and on the maze it does: a top-level waypoint sits on the ring district's
// entrance, and every hop chain that turns there passes a door its pair does not need (info-maze.txt). With the
// variant such a waypoint is moved to the door's first neighbour that is not a door and not yet a waypoint (or
// dropped when there is none). Called by the bake after the loop doors are shut, so a mutable area that is
// still enabled is a forced door.
inline std::vector<uint32_t> OutdoorCover(const overlay_four::Fixture4& f, const s3c::NativeGeometry& geo, const std::vector<uint32_t>& active, uint32_t cap) {
  auto wp = Cover(f, geo, active, cap); const char* rule = getenv("OD_WAYPOINTS"); if (!rule || strcmp(rule, "off-doors")) return wp;
  std::set<uint32_t> have(wp.begin(), wp.end()); uint32_t moved = 0, dropped = 0;
  for (uint32_t a : wp) { if (!f.g.mutableArea[a] || !f.g.enabled[a]) continue; have.erase(a); uint32_t to = UINT32_MAX;
    for (uint32_t e = f.g.out[a]; e < f.g.out[a + 1] && to == UINT32_MAX; ++e) { const uint32_t v = f.g.edges[e].to; if (f.g.enabled[v] && !f.g.mutableArea[v] && !have.count(v)) to = v; }
    if (to != UINT32_MAX) { have.insert(to); ++moved; } else ++dropped; }
  printf("M COVER_OFF_DOORS cap=%u waypointsMovedOffAForcedDoor=%u dropped=%u waypoints=%zu\n", cap, moved, dropped, have.size());
  return std::vector<uint32_t>(have.begin(), have.end());
}
#define Cover OutdoorCover
#define Native(side) Outdoor(side)
#define ClassifyDoors OutdoorClassify
