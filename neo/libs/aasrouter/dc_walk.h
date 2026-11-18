#pragma once
// exp-active2, outdoor maps, LAST-LEG PATCHES (director, 2026-11-17; README.md here). A copy of ../many/dc_walk.h;
// everything is as there except what this first paragraph names.
// A route whose last leg is PATCHED (dc_layer.h's head) leaves its last door area by the patch's first reach, not
// by From's exit reach, and then takes the patch's other reaches one by one before the lists' route begins:
//   7  on a last-leg patch, past the door area: the next reach is read from the patch's record. No door is ahead
//      any more (a patch lies in the lists' map), so nothing is held against the door tables in this phase, and a
//      wide door that shuts obliges this walker to nothing, exactly as on a lists' route.
// The walker carries two more numbers: the patch's record (legRec; NONE: the last leg is not patched) and the
// reaches of it already taken (legDone). The answer names the record (DoorRoute::legRec) and DBegin copies it.
// A saved walker is DWALK_BYTES = 96 bytes (88 before). DLoad refuses a walker whose last leg is not the one the
// ask gives for its last door area and goal (a patch where the pair has one, none where it has none), and a
// walker in phase 7 that does not stand where its patch has got to.
//
// ---- what follows is the head of ../many/dc_walk.h ----
// exp-active2, outdoor maps, MANY WIDE DOORS, stage 1 (SPEC.md here; 2026-11-16). A copy of ../dc_walk.h: a route
// that crosses wide doors, taken a few reaches at a time, with a state that can be saved. It adds to nl_walk.h,
// which it does not change; a lists' route is handed to the lists' walker as it is.
//
//   DBegin(doors, answer, s, g)        the walker's state from the answer of Doors::Ask (areas as the map's numbers)
//   DNext(doors, walk, buf, want, out) the next `want` reaches (fewer only when the walker arrives)
//   DSave / DLoad                      the state as DWALK_BYTES bytes
//   DHeld(doors, reaches, count)       of the reaches a monster still holds, how many it may walk (see THE GAME'S RULES)
//
// THE GAME'S RULES (each was shown by a run of the Fable reviewer; the walker check and the router hold them):
//   1. REACHES ALREADY HANDED OUT. DNext checks the doors once per call. Reaches it handed out and the monster has
//      not walked yet are NOT protected against a door that shuts afterwards, and the walker's next call need not
//      refuse (its state may be past that door). So: WHEN A DOOR SHUTS, the game calls DHeld for the reaches each
//      monster holds; a monster keeps only the reaches before the first one that enters a door area that is shut
//      now, drops the rest, and ASKS AGAIN from where the kept ones end (its walker is thrown away). A monster
//      whose held reaches all pass goes on with its walker; its next DNext call checks the rest of the route.
//   2. A LOAD REFUSAL MEANS "ASK AGAIN". After a door of its route has shut, a saved walker on a door leg can be
//      refused by DLoad although it was saved truly: the rest of its route is not there any more. The game takes
//      a DLoad refusal of a walker on a door leg as "ask again", exactly as a DNext refusal.
//   3. A DOOR CHANGE BETWEEN Ask AND DBegin is allowed: the answer carries the table times of its segments
//      (DoorRoute::segT, n1T, n2T) and DBegin copies them, so the walker's first call holds the route against
//      the doors as they are then.
//
// What changed against ../dc_walk.h: a door route is a chain of up to three SEGMENTS (dc_layer.h: the mouths of
// the source's pocket, the through doors, the mouths of the goal's pocket). The walker carries the door area it
// walks to (`door`), the last door area of that door's segment (`segEnd`), and the first and last door area of the
// one or two segments that follow (n1First, n1Last, n2First, n2Last; NONE: no such segment). The two door phases:
//   5  toward door area `door` through door-free ground: the reach still held in `first` (the route's first reach,
//      or the table's reach out of the door area before), then at each area To(door)'s reach. One record a reach.
//   6  on door area `door`.
//        - It is not its segment's last: the next door area of the segment by Doors::SegHop (the rule the ask
//          used, on the same table), and the baked table's reach toward it.
//        - It is its segment's last and a segment follows: the baked table's reach toward that segment's first
//          door area; the walker's numbers move up one segment.
//        - It is the route's last door area: the exit reach of From for the goal, and from the area after it the
//          lists' route, whose start the state has carried since the ask. A goal that is a door area ends the walk.
// The walker never asks. What is saved: the lists' twelve numbers and ten more. In a door phase the lists'
// numbers hold: goal and area as SPOTS (enabled index, or n + door area), `first`, and the last leg's start
// (prefixLeft, level, y, wp).
// When a wide door SHUTS, a walker in a door phase must ask again (ruled 2026-11-16; a door that opens obliges no
// walker). The walker carries, for each of its segments, the TABLE TIME between the segment's two ends as the ask
// timed it (segT, n1T, n2T; 0: no such segment). At the first door-phase reach of every call DNext holds each
// against the table as it is now:
//   - LARGER, or no way, or the door area it walks to is shut: a door on the rest of its route has shut. It
//     refuses ("ask again"). So it never steps into a shut door area, and it never walks a route slower than timed.
//   - SMALLER: a door has opened. That obliges no walker: it takes the smaller time and walks on by the tables as
//     they are now.
//   - EQUAL: it walks on (it may be another way of the same time).
// Why that is all: the first leg, the last leg and the baked steps between segments pass no door. With only
// shuts since the ask, a walker that is allowed on has a route no slower than timed while the fastest route has
// got no faster, so the rule holds; after an opening the ruling says it is judged at its next ask.
// The door state changes only through Doors::Set.
// An arrived walker is no longer on a door leg: its door numbers are cleared, on a door-area goal too.
// A saved game is not trusted: DLoad refuses a state that is out of range or does not stand on its own leg, and
// DNext stops with a refusal, never with a read outside an area's reaches, on anything else.
#include "rt_doors.h"
namespace nl {

struct DWalk { Walk w; U door = NONE, segEnd = NONE, n1First = NONE, n1Last = NONE, n2First = NONE, n2Last = NONE, tailFirst = NONE;
               U segT = 0, n1T = 0, n2T = 0;         // the table time from door to segEnd, from n1First to n1Last, from n2First to n2Last, as the ask timed them (0: no such segment)
               U legRec = NONE, legDone = 0; };      // the last-leg patch of the route (its record; NONE: none) and the reaches of it already taken (phase 7: at least 1)
constexpr U DWALK_NUMBERS = 24, DWALK_BYTES = WALK_BYTES + 48;
inline U ExitDoor(const DWalk& x) { return x.n2Last != NONE ? x.n2Last : x.n1Last != NONE ? x.n1Last : x.segEnd; }      // the door area the route leaves the layer by
inline bool OnDoorLeg(const DWalk& x) { return x.w.phase == 5 || x.w.phase == 6; }      // a door area is still ahead of the walker (phase 7, on a last-leg patch, is past its doors)

inline void DSave(const DWalk& x, uint8_t* out) { Save(x.w, out); const U more[12] = {x.door, x.segEnd, x.n1First, x.n1Last, x.n2First, x.n2Last, x.tailFirst, x.segT, x.n1T, x.n2T, x.legRec, x.legDone}; for (U i = 0; i < 12; ++i) for (U k = 0; k < 4; ++k) out[WALK_BYTES + 4 * i + k] = uint8_t(more[i] >> (8 * k)); }

inline DWalk DLoad(const Doors& d, const uint8_t* in, WalkBuf& b) {
  const Runtime& rt = d.rt; DWalk x; U more[12]; for (U i = 0; i < 12; ++i) more[i] = U(Rd(in + WALK_BYTES + 4 * i, 4));
  x.door = more[0]; x.segEnd = more[1]; x.n1First = more[2]; x.n1Last = more[3]; x.n2First = more[4]; x.n2Last = more[5]; x.tailFirst = more[6]; x.segT = more[7]; x.n1T = more[8]; x.n2T = more[9]; x.legRec = more[10]; x.legDone = more[11];
  bool noDoors = true; for (U i = 0; i < 12; ++i) noDoors = noDoors && more[i] == (i < 7 || i == 10 ? NONE : 0);
  U v[12]; for (U i = 0; i < 12; ++i) v[i] = U(Rd(in + 4 * i, 4)); const U goal = v[0], at = v[1], phase = v[6];
  if (phase == 4 && goal >= d.n) {          // arrived on a goal that is a door area
    b.valid = false; b.count = 0; Need(goal < d.n + d.D && at == goal && noDoors && v[2] == NONE && v[3] == 0 && v[4] == NONE, "saved walker: arrived on a door area");
    x.w.goal = goal; x.w.at = at; x.w.phase = 4; return x; }
  if (phase <= 4) { Need(noDoors, "saved walker: door numbers on a lists' route"); x.w = Load(rt, in, b); return x; }
  b.valid = false; b.count = 0; Walk& w = x.w; w.goal = v[0]; w.at = v[1]; w.first = v[2]; w.prefixLeft = v[3]; w.level = v[4]; w.y = v[5]; w.phase = v[6]; w.wp = v[7]; w.target = v[8]; w.after = v[9]; w.longIndex = v[10]; w.pathDone = v[11];
  // the lists' part of a last leg, which begins on area `after`: it must be a state the lists' walker accepts there
  auto tail = [&](U after) { if (after == w.goal) { Need(w.prefixLeft == 0 && w.level == NONE && w.y == NONE && w.wp == NONE && x.tailFirst == NONE, "saved walker: a last leg after the goal"); return; }
    Need(w.prefixLeft > 0 || (w.level != NONE && x.tailFirst == NONE), "saved walker: no last leg");
    Walk t; t.goal = w.goal; t.at = after; t.first = x.tailFirst; t.prefixLeft = w.prefixLeft; t.level = w.level; t.y = w.y; t.wp = w.wp; t.phase = w.prefixLeft ? 0 : 1;
    uint8_t bytes[WALK_BYTES]; Save(t, bytes); WalkBuf tb; Load(rt, bytes, tb); };
  if (phase == 7) {                         // on a last-leg patch, past the door area
    Need(x.door == NONE && x.segEnd == NONE && x.n1First == NONE && x.n1Last == NONE && x.n2First == NONE && x.n2Last == NONE && x.segT == 0 && x.n1T == 0 && x.n2T == 0, "saved walker: door numbers on a last-leg patch");
    Need(w.goal < d.n && w.at < d.n && w.first == NONE && w.target == NONE && w.after == NONE && w.longIndex == 0 && w.pathDone == 0 && x.legRec < d.L, "saved walker: on a last-leg patch");
    const U off = d.LegOff(x.legRec), cnt = d.LegCount(x.legRec), p = d.LegDoorRaw(x.legRec); Need(d.LegGoal(x.legRec) == w.goal && x.legDone >= 1 && x.legDone < cnt, "saved walker: its last-leg patch is of another goal, or its place on it");
    U at = NONE; for (U i = 0; i < x.legDone; ++i) { const U from = i == 0 ? d.da[p] : rt.active[at]; at = rt.index[d.f.g.edges[d.f.g.out[from] + d.legPath[off + i]].to]; }      /* the layer's load walked every patch already */
    Need(at == w.at, "saved walker: not where its last-leg patch has got to"); tail(d.LegEnd(x.legRec)); return x; }
  Need(phase <= 6 && d.PlanHoldsRaw(x.door, x.segEnd, x.n1First, x.n1Last, x.n2First, x.n2Last), "saved walker: its door areas, or the rest of its route is not there under these doors");
  Need(w.target == NONE && w.after == NONE && w.longIndex == 0 && w.pathDone == 0 && x.legDone == 0, "saved walker: a piece of the lists on a door leg");
  // the carried segment times: in range, and none without its segment. (One below the table's time now makes DNext refuse at its next call; one above is taken down there.)
  Need(x.segT < BIG && x.n1T < BIG && x.n2T < BIG && (x.n1First != NONE || x.n1T == 0) && (x.n2First != NONE || x.n2T == 0), "saved walker: its segments' times");
  // the last leg
  const U ex = ExitDoor(x);
  if (w.goal >= d.n) Need(w.goal == d.n + ex && w.prefixLeft == 0 && w.level == NONE && w.y == NONE && w.wp == NONE && x.tailFirst == NONE && x.legRec == NONE, "saved walker: a door goal with a last leg, or not its last door area");
  else { U rg, pl; d.RegionRaw(w.goal, rg, pl); const W blk = d.BlockRaw(ex, rg); Need(blk != NOPOS && d.FrT(ex, blk, pl) != d.TNone(ex), "saved walker: its last door area does not reach the goal");
    Need(x.legRec == d.LegFindRaw(ex, w.goal), "saved walker: its last leg is not the one the ask gives for its last door area and goal");      // a patch where the pair has one, none where it has none
    if (x.legRec != NONE) tail(d.LegEnd(x.legRec));
    else { const U xo = d.FrX(ex, blk, pl); Need(xo < d.Deg(d.da[ex]), "saved walker: exit reach"); const U after = rt.index[d.f.g.edges[d.f.g.out[d.da[ex]] + xo].to]; Need(after != NONE, "saved walker: exit reach"); tail(after); } }
  // where it stands
  if (phase == 5) { Need(w.at < d.n, "saved walker: toward a door area from no area of a region"); U at2 = w.at;
    if (w.first != NONE) { Need(w.first < d.f.g.edges.size() && d.f.g.edges[w.first].from == rt.active[w.at], "saved walker: first reach"); const U to = d.f.g.edges[w.first].to; if (to == d.da[x.door]) return x; at2 = rt.index[to]; Need(at2 != NONE, "saved walker: first reach"); }
    U rg, pl; d.RegionRaw(at2, rg, pl); const W blk = d.BlockRaw(x.door, rg); Need(blk != NOPOS && d.ToT(x.door, blk, pl) != d.TNone(x.door), "saved walker: no way to its door area from where it stands"); }
  else Need(w.at == d.n + x.door && w.first == NONE, "saved walker: not on its door area");
  return x; }

// The walker's state right after Ask(sArea, gArea) returned `a` (a.found must be true).
inline DWalk DBegin(const Doors& d, const DoorRoute& a, U sArea, U gArea, Meter& m) {
  const Runtime& rt = d.rt; DWalk x; m.Tick(3 * 3); Need(a.found && sArea < d.N && gArea < d.N, "walker: no route to begin");
  m.Tick(3); if (sArea == gArea) { m.Tick(14 + 3 * 2); x.w.goal = x.w.at = d.Spot(gArea); x.w.phase = 4; return x; }      // 14: an area's spot (its enabled index and its door number, each read and tested)
  m.Tick(3); if (!a.door) { m.Tick(2 * 4); x.w = Begin(rt, a.r, rt.index[sArea], rt.index[gArea], m); return x; }
  m.Tick(2 * 14 + 8 * 2 + 3); x.w.goal = d.Spot(gArea); x.w.at = d.Spot(sArea); x.door = a.toDoor; x.segEnd = a.segEnd; x.n1First = a.n1First; x.n1Last = a.n1Last; x.n2First = a.n2First; x.n2Last = a.n2Last;
  m.Tick(3 * 2); x.segT = a.segT; x.n1T = a.n1T; x.n2T = a.n2T;                             // each segment's table time under the doors of the ASK, as the answer carries them (not read from the tables here: the doors may have changed since)
  m.Tick(2); x.legRec = a.legRec;                                                           // the last-leg patch, if the route has one (then a.tailArea is where the patch ends)
  if (a.r.found) { m.Tick(4); const Walk t = Begin(rt, a.r, a.tailArea, rt.index[gArea], m); m.Tick(5 * 2); x.tailFirst = t.first; x.w.prefixLeft = t.prefixLeft; x.w.level = t.level; x.w.y = t.y; x.w.wp = t.wp; }
  m.Tick(3); if (x.w.at < d.n) { m.Tick(2 * 2); x.w.phase = 5; x.w.first = a.first; } else { m.Tick(2); x.w.phase = 6; }
  return x; }

// THE GAME'S RULE 1 (the head): of `count` reaches a monster holds and has not walked, how many it may still walk:
// all before the first one that enters a door area that is shut under the current doors. (`count`: all pass.)
inline U DHeld(const Doors& d, const U* reaches, U count, Meter& m) {
  const auto& g = d.f.g; m.Tick(2);
  for (U i = 0; i < count; ++i) { m.Tick(3 + 2 + 2 + 3 + 4 + 4 + 3 + 3); Need(reaches[i] < g.edges.size(), "held reaches: not a reach of the map"); const U p = d.dix[g.edges[reaches[i]].to]; if (p != NONE && !d.open[p]) return i; }
  m.Tick(3); return count; }

// The next `want` reaches, written to `out`; returns how many (fewer than `want` only when the walker arrives).
inline U DNext(const Doors& d, DWalk& x, WalkBuf& b, U want, U* out, Meter& m) {
  const Runtime& rt = d.rt; const auto& g = d.f.g; Walk& w = x.w; U count = 0; m.Tick(6);
  auto emit = [&](U e) -> U { m.Tick(3 + 4 + 4 + 10); Need(e < g.edges.size() && w.at < d.n + d.D && g.edges[e].from == d.AreaOf(w.at), "walker: a reach that does not leave its area"); out[count++] = e; return g.edges[e].to; };
  auto fromDoor = [&](U ord) -> U { m.Tick(3 + 4); Need(ord < d.Deg(d.da[x.door]), "walker: reach position outside its door area"); return g.out[d.da[x.door]] + ord; };
  // a segment's table time under the current doors against the time carried since the ask: false when it has
  // risen or there is no way (a door on the rest of the route has shut); a smaller time is taken (a door has opened)
  auto still = [&](U p, U q, U& carried) -> bool { m.Tick(2 * 3); if (p >= d.D || q >= d.D) return false; const U now = d.SegTime(p, q, m); m.Tick(3 + 3 + 3); if (now == BIG || now > carried) return false; if (now < carried) { m.Tick(2); carried = now; } return true; };
  // from door area `from` one table step to door area `to`: the reach, and where the walker then is
  auto step = [&](U to2) { m.Tick(2 + Fld(1)); const U to = emit(fromDoor(d.M0R(x.door, to2))); x.door = to2; m.Tick(2 + 3);
    if (to == d.da[to2]) { m.Tick(2 * 2); w.at = d.n + to2; w.phase = 6; } else { m.Tick(4 + 3 + 2 * 2); Need(rt.index[to] != NONE, "walker: a door leg leaves the lists' map"); w.at = rt.index[to]; w.phase = 5; } };
  // the walker has taken the last reach of its last leg's door part (the exit reach, or a patch's last reach):
  // from here on the lists' route, as the ask got it
  auto endLeg = [&]() { m.Tick(4 * 2 + 3); w.first = x.tailFirst; x.tailFirst = NONE; x.legRec = NONE; x.legDone = 0;
    if (w.at == w.goal) { m.Tick(3 * 3 + 2); Need(w.prefixLeft == 0 && w.level == NONE && w.first == NONE, "walker: a last leg after the goal"); w.phase = 4; }
    else { m.Tick(3 * 3 + 3 + 2); Need(w.prefixLeft > 0 || (w.level != NONE && w.first == NONE), "walker: no last leg"); w.phase = w.prefixLeft ? 0 : 1; } };      /* the first Tick: four copies and the goal test */
  bool held = false;                    // the rest of the route has been checked against the current doors in this call
  for (;;) { m.Tick(3 + 3); if (count == want || w.phase == 4) break;
    m.Tick(3); if (w.phase < 4) { m.Tick(13 * 3); Need(x.door == NONE && x.segEnd == NONE && x.n1First == NONE && x.n1Last == NONE && x.n2First == NONE && x.n2Last == NONE && x.segT == 0 && x.n1T == 0 && x.n2T == 0 && x.legRec == NONE && x.legDone == 0 && w.at < d.n && w.goal < d.n, "walker: a lists' phase on a door leg"); count += Next(rt, w, b, want - count, out + count, m); break; }
    // phase 7: on a last-leg patch, past its door area. The next reach comes from the patch's record; after its last
    // reach the lists' route begins. No door is ahead, so nothing is held against the door tables here.
    m.Tick(3); if (w.phase == 7) { m.Tick(6 * 3); Need(x.door == NONE && x.segEnd == NONE && x.legRec < d.L && w.at < d.n && w.goal < d.n && w.first == NONE, "walker: on a last-leg patch without its record");
      m.Tick(2 + Fld(4) + Fld(2) + 2 * 3); const U off = d.LegOff(x.legRec), cnt = d.LegCount(x.legRec); Need(x.legDone >= 1 && x.legDone < cnt, "walker: its place on its last-leg patch");
      m.Tick(Fld(1) + 4); const U to = emit(OutAt(rt, w.at, d.legPath[off + x.legDone])); m.Tick(4 + 3 + 2 + 2 + 3); Need(rt.index[to] != NONE, "walker: a last-leg patch leaves the lists' map"); w.at = rt.index[to]; ++x.legDone;
      if (x.legDone == cnt) endLeg(); continue; }
    // a door phase: no segment of the rest of its route may have got slower since the ask (the head says why); if one
    // has, a door has shut and the monster must ask again. Checked at the first door-phase reach of every call: the
    // door state cannot change inside a call, and what the walker's numbers become later in the call are the numbers
    // checked here.
    m.Tick(3); if (!held) { held = true;
    m.Tick(3 + 3); Need(w.phase <= 6 && x.door < d.D && d.open[x.door], "walker: the door area it walks to is shut, or its numbers are not a route (ask again)");
    Need(still(x.door, x.segEnd, x.segT), "walker: a door on the rest of its segment has shut (ask again)");
    m.Tick(3); if (x.n1First != NONE) { m.Tick(2 * 3 + 2 + Fld(4) + 3 + 3); Need(x.n1First < d.D && x.n1Last < d.D && d.M0T(x.segEnd, x.n1First) != BIG && d.open[x.n1First] && still(x.n1First, x.n1Last, x.n1T), "walker: a door of its next segment has shut (ask again)");
      m.Tick(3); if (x.n2First != NONE) { m.Tick(2 * 3 + 2 + Fld(4) + 3 + 3); Need(x.n2First < d.D && x.n2Last < d.D && d.M0T(x.n1Last, x.n2First) != BIG && d.open[x.n2First] && still(x.n2First, x.n2Last, x.n2T), "walker: a door of its last segment has shut (ask again)"); }
      else { m.Tick(2 * 3); Need(x.n2Last == NONE && x.n2T == 0, "walker: half a segment"); } }
    else { m.Tick(5 * 3); Need(x.n1Last == NONE && x.n2First == NONE && x.n2Last == NONE && x.n1T == 0 && x.n2T == 0, "walker: half a segment"); } }
    m.Tick(3); if (w.phase == 5) { m.Tick(3 + 3); Need(w.at < d.n, "walker: toward a door area from no area of a region"); U e;
      if (w.first != NONE) { m.Tick(2 + 2); e = w.first; w.first = NONE; }
      else { U rg, pl; d.RegionOf(w.at, rg, pl, m); const W blk = d.Block(x.door, rg, m); Need(blk != NOPOS, "walker: not beside its door area");
        m.Tick(3 + 2 + 3 + 7); const W tr = Runtime::Bits(d.fields, blk + W(pl) * d.Ew(x.door), d.tb[x.door] + d.rb, m); Need(U(tr & d.TNone(x.door)) != d.TNone(x.door), "walker: no way to its door area from here"); e = OutAt(rt, w.at, U(tr >> d.tb[x.door])); }
      const U to = emit(e); m.Tick(3);
      if (to == d.da[x.door]) { m.Tick(2 * 2); w.at = d.n + x.door; w.phase = 6; } else { m.Tick(4 + 3 + 2); Need(rt.index[to] != NONE, "walker: a door leg leaves the lists' map"); w.at = rt.index[to]; }
      continue; }
    m.Tick(3 + 3); Need(w.at == d.n + x.door && w.first == NONE, "walker: not on its door area");
    m.Tick(3); if (x.door != x.segEnd) {              // the next door area of this segment, by the rule the ask used, and the table's reach toward it
      const U h = d.SegHop(x.door, x.segEnd, m); m.Tick(3); Need(h != NONE, "walker: no way on to the end of its segment (ask again)");      /* SegHop gives an open door area of the same table that is nearer to the segment's end */
      step(h); m.Tick(2); x.segT = d.SegTime(x.door, x.segEnd, m); continue; }      /* the rest of this segment from h: by the hop rule the old time less the step */
    m.Tick(3); if (x.n1First != NONE) {               // this segment is done: one table step to the next segment's first door area; the numbers move up
      const U nx = x.n1First; m.Tick(9 * 2); x.segEnd = x.n1Last; x.n1First = x.n2First; x.n1Last = x.n2Last; x.n2First = x.n2Last = NONE; x.segT = x.n1T; x.n1T = x.n2T; x.n2T = 0; step(nx); continue; }
    m.Tick(3); if (w.goal == d.n + x.door) { m.Tick(4 * 2); x.door = x.segEnd = NONE; x.segT = 0; w.phase = 4; continue; }      // the goal is this door area: arrived, and no longer on a door leg
    m.Tick(3); Need(w.goal < d.n, "walker: goal");
    m.Tick(3); if (x.legRec != NONE) {                // the last leg is patched: the patch's first reach leaves this door area
      m.Tick(3 * 3 + 2 * Fld(4) + 2 + Fld(4) + 3 + Fld(4) + Fld(2) + 3); Need(x.legRec < d.L && x.legDone == 0 && d.LegLo(x.door) <= x.legRec && x.legRec < d.LegLo(x.door + 1) && d.LegGoal(x.legRec) == w.goal, "walker: its last-leg patch is not one of its last door area and goal");
      const U off = d.LegOff(x.legRec), cnt = d.LegCount(x.legRec); Need(cnt >= 1, "walker: an empty last-leg patch");
      m.Tick(Fld(1)); const U to = emit(fromDoor(d.legPath[off])); m.Tick(4 + 3 + 5 * 2 + 3 + 2); Need(rt.index[to] != NONE, "walker: a last-leg patch leaves the lists' map");
      w.at = rt.index[to]; x.door = x.segEnd = NONE; x.segT = 0; x.legDone = 1; if (x.legDone == cnt) endLeg(); else w.phase = 7; continue; }
    U rg, pl; d.RegionOf(w.goal, rg, pl, m); const W blk = d.Block(x.door, rg, m); Need(blk != NOPOS, "walker: its last door area is not beside the goal's region");
    m.Tick(3 + 2 + 3); const W tx = Runtime::Bits(d.fields, blk + W(pl) * d.Ew(x.door) + d.tb[x.door] + d.rb, d.tb[x.door] + d.xb[x.door], m); Need(U(tx & d.TNone(x.door)) != d.TNone(x.door), "walker: its last door area does not reach the goal");
    const U to = emit(fromDoor(U(tx >> d.tb[x.door]))); m.Tick(4 + 3 + 4 * 2); Need(rt.index[to] != NONE, "walker: the exit reach leaves the lists' map");
    w.at = rt.index[to]; x.door = x.segEnd = NONE; x.segT = 0; endLeg(); }
  m.Tick(3 + 3); if (w.phase == 4) Need(w.at == w.goal, "walker: arrived somewhere else than the goal");
  return count; }

} // namespace nl
