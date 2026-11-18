#pragma once
// R2 router, engine port: the GAME SIDE of the door layer (exp-active2/native-lists/pack4/outdoor/doors/lastleg/
// dc_layer.h without its bake side: LegPatch, LayerContent and WriteLayer are left out; every other line is
// verbatim). The head below is that file's.
//
// exp-active2, outdoor maps, LAST-LEG PATCHES (director, 2026-11-17; README.md here). A copy of ../many/dc_layer.h
// (the door layer with pockets). Everything is as there except what this first paragraph names; where the text
// below says "here" for SPEC.md, COUNTING-DOORS.md or dm_pockets.h, those files are in ../many/.
//
// LAST-LEG PATCHES. A door route is exact up to its last door area q. From there it took q's exit reach and the
// lists' own route to the goal g, and the bake step refused a map on which that last leg was above its line
// (4 leg <= 5 From + slack; slack = 684 - 4 x the largest Origin difference at a door area). Now the bake step
// gives such a pair (q, g) a PATCH: the first reaches of the least-time way from q to g that From's time is the
// time of, as many as are needed for "those reaches, then the lists' own route from where they end" to be on or
// under the line. At worst the patch is the whole way, which is exact and so under the line. The ask and the
// walker take the patch in place of the exit reach whenever the pair has one. Nothing else changes, and the lists
// file stays byte for byte.
//   - WHY THE RULE HOLDS: as before (../DESIGN.md section 6). A door route differs from the exact door time only
//     in its last leg, and the bake step proves the line for EVERY pair of door area and goal with the leg the ask
//     takes, patched or not. No pair is left above the line, so no map is refused for it.
//   - A patch is a least-time way in the lists' map (every wide door shut), so it passes no wide door, and it
//     passes only the narrow necks its two ends need (ll_gate.h).
//   - STILL REFUSED: a door area whose reaches' Origin values differ by more than 171 (4 x 171 = 684: no slack
//     would be left, and a route that STARTS ON such a door area could break the rule with an exact last leg).
// THE FILE (version 3) is version 2 with three more sections:
//   door.legIndex  per door area + 1: the 4-byte number of the first patch record of that door area
//   door.legRec    per patch 18 bytes, by door area, then by goal ascending: the goal (4, its enabled index), where
//                  its reaches start in door.legPath (4), how many they are (2; at least 2), their time (4), and
//                  the area they end on (4, enabled index; the goal itself when the patch is the whole way)
//   door.legPath   the reaches, one byte each: the position of the reach among the reaches of the area it leaves
//                  (the first one leaves the door area)
// The load checks every patch: its door area reaches its goal; each reach stays in the lists' map and in the
// goal's region and arrives in From's own time (so the patch is a least-time way from the door area); it does not
// go on after the goal; its time and its end are as recorded. That it leads TOWARD the goal is the bake's doing,
// as the least times are: a forged patch that is consistent in itself could cost route quality, not safety.
//
// ---- what follows is the head of ../many/dc_layer.h ----
// exp-active2, outdoor maps, MANY WIDE DOORS, stage 1 "pockets" (SPEC.md here; 2026-11-16). A copy of ../dc_layer.h
// (the door layer and the game-side ask that crosses wide doors), changed so that the doors of small closed
// places cost nothing outside their place. Read ../dc_layer.h's header first: the lists, regions, To, From, the
// table, the ask and what the load checks give are as there. What is new:
//
// POCKETS. A pocket is a region S whose door areas (its MOUTHS) are all beside exactly S and one other region B
// (its OUTSIDE) and are never a shortcut for a route between two outside areas; the bake step decides it
// (dm_pockets.h has the exact check and why Claim 1 follows). Every other door area is a THROUGH door.
// Claim 1: a pair with neither end in S or on a mouth of S has a fastest route that stands on no mouth of S,
// under any door state. So:
//   - the table under the current doors (cT) holds THROUGH doors only; Rebuild, Insert and Hop are the built ones
//     over that smaller set (the "outer world");
//   - each pocket has a small table of its own (pT) over its mouths: least times over its open mouths, one step
//     being the baked time from mouth to mouth (through S, by a direct reach, or through door-free ground
//     outside). A mouth that opens or shuts rebuilds that one table and nothing else;
//   - an ask looks only at the through doors beside its ends' regions and at the mouths of the pockets its ends
//     are in. A pocket that holds neither end plays no part and costs no step.
// THE DOOR ROUTE has up to three SEGMENTS, each a least way inside one table, joined by one baked table step:
//     [mouths of the source's pocket]  ->  [through doors]  ->  [mouths of the goal's pocket]
// A segment is named by its first and last door area. The answer carries them (DoorRoute: toDoor, segEnd, then
// n1First/n1Last and n2First/n2Last for the segments that follow), the walker follows them (dc_walk.h), and both
// work out the next door area inside a segment with the same rule on the same table (SegHop). The route's legs are
// exact as built; its last leg is the exit reach and the lists' own route (the one inexact leg).
// WHY THE TIME IS EXACT. By Claim 1 some fastest route stands only on through doors and on mouths of the two
// pockets. Cut it at its door areas: its first piece is a To time at the source, its last a From time at the goal,
// and each piece between is a baked table step. The ask takes the least over exactly such chains:
//   - a goal in a pocket: once a route stands on a mouth of the goal's pocket it can stay on that pocket's mouths
//     and S (a later way out and in again is a visit that loses nothing when cut; a way from mouth to mouth through
//     the outside is in pT anyway). So the goal's side is: per mouth the least time on to the goal over pT
//     (GoalSide::mr), and per through door beside B one table step onto a mouth, then mr (GoalSide::q, fq, qin);
//   - a source in a pocket: per mouth j of its pocket the least time to stand on it (To at the source, then pT),
//     and from j: straight to the goal (From), one table step to a mouth of the goal's pocket beside the same
//     outside, or one table step to a through door beside the outside and on through the outer world;
//   - anything else is the built ask over through doors.
// A map with no pocket is answered exactly as ../dc_layer.h answers it (dc_bake's DM_NO_POCKETS=1 makes such a
// layer of any map; the ask check's digests are compared with ../).
//
// THE CALLER'S RULE FOR DOOR CHANGES. All door changes of one tic go into ONE call of Set. A through door that
// shuts rebuilds the whole outer table (about one million steps with the 40 through-door areas the bake allows);
// two such calls in one tic would pass the tic's cap of 2,097,152 by themselves. Set(opening, shutting) takes
// door areas that open and door areas that shut in ONE call and works the outer table out once. The router here
// refuses to make more than one counted Set call in a tic.
// A TIC THAT CHANGES A THROUGH DOOR CARRIES FEWER ASKS. The user's ruling of 2026-11-16, after the router on towns
// fly had tics over the cap in which 16 or more through-door areas opened at once (the whole outer table,
// 1,057,423 steps) beside 51 asks out of buildings (1,337,079 steps at most): "Game spreads the asks". Those four
// words are all the user said. THE DIRECTOR'S READING of them, as he widened it on 2026-11-17: a tic in which ANY
// through door (a door of the map, whatever its areas) changes, opening or shutting, one door or many, answers at
// most 25 asks; the other asks of that tic wait for the next tic, which has no door change. Pocket doors do not
// count. His first reading was "more than ONE through door". It left one tic over the cap on the map built at
// both limits (40 through-door areas and a pocket of 8 mouths; towns-crow2-fly): one through door shutting, the
// whole outer table, 806,136 steps, with 51 walkers asking again, 2,172,553 steps in all. Nothing in this file
// enforces the reading (the layer does not know a tic); the router here runs its tics that way (Harness::Ruled).
// IF THE GAME MUST PUT PART OF A CHANGE OFF to a later tic (to stay inside the cap), IT PUTS OFF THE OPENING, NEVER
// THE SHUTTING: a route through a door that is shut in the game is illegal; a door that the layer learns to be
// open a tic late only costs a detour for that tic.
// A POCKET HAS AT MOST MOUTH_LIMIT MOUTHS (8). A pocket's own table is rebuilt whole when one of its mouths
// changes, at a cost cubic in its mouths, and an ask with an end in a pocket grows with its mouths, with their
// square, and with its mouths times the through doors beside its outside. The bake step and the load refuse a
// pocket with more. 8 is the largest of 8, 12 and 16 at which, beside 40 through-door areas, a door of the pocket
// changing and 51 asks into, out of and inside the pocket each stay at most 90 % of the tic's cap (dc_bake.cpp
// has the figures; the 90 % is the director's margin, not a pass line).
// WHAT THE LOAD DOES NOT PROVE AGAIN. The load refuses a file that is not the baked bytes (its hash), anything an
// ask or a walk could trip over, and a file with more through-door areas than THROUGH_LIMIT. It does NOT work out
// again that a pocket's mouths have no reach to a foreign door area and are no shortcut (rules 2 and 4 of
// dm_pockets.h): those are the bake's doing, as the least times are. A forged file that is consistent in itself
// and carries its own hash is not caught, as with the lists; it could cost route quality, not safety.
//
// THE FILE (version 2). As ../dc_layer.h, and:
//   door.meta        4-byte fields: version (2), enabled areas, areas of the map, door areas D, regions R, the three
//                    widths, the slack, the Origin difference, then doors G, pockets P, through-door areas T. Then
//                    the three 8-byte fields as before (lists' bytes, lists' hash, this file's hash).
//   door.area        per door area 10 bytes: its area (4), time bits (1), position bits (1), its DOOR (2: the group
//                    that opens and shuts together, numbered as the map's census lists them), its POCKET (2; 65535:
//                    a through door).
//   door.pocket      per pocket 8 bytes: its region S (4), its outside B (4)
//   door.mouthIndex  per pocket + 1: 4-byte offset into door.mouths
//   door.mouths      per pocket its mouths, ascending (2 bytes each)
//   door.thruIndex   per region + 1: 4-byte offset into door.thru
//   door.thru        per region the THROUGH doors beside it, ascending (2 bytes each)
//   door.regionPocket per region 2 bytes: the pocket it is the S of (65535: none)
// door.beside (every door area beside a region) is still written and checked; the game side reads it at load only.
// Every statement group of the game side is charged; COUNTING-DOORS.md here lists the charges.
#include "nl_walk.h"
namespace nl {

constexpr W DNOBLOCK = (W(1) << 48) - 1; constexpr W NOTIME = ~W(0); constexpr U THROUGH_LIMIT = 40;      // the bake step refuses a map with more through-door areas (dc_bake.cpp says why 40)
#ifndef DC_MOUTH_LIMIT
#define DC_MOUTH_LIMIT 8                  // a MEASURING build may set another number (-DDC_MOUTH_LIMIT=64: the programs *_m64.exe, for the cost runs the limit was taken from)
#endif
constexpr U MOUTH_LIMIT = DC_MOUTH_LIMIT;   // the bake step and the load refuse a pocket with more mouths (dc_bake.cpp says why 8)
inline W LayerHash(const Blob& b) { W h = 1469598103934665603ull; for (const auto& s : b.sec) { if (s.first == "door.meta") { for (size_t i = 0; i + 8 < s.second.size(); ++i) { h ^= s.second[i]; h *= 1099511628211ull; } continue; }      /* of the header everything but the hash itself */
    for (char c : s.first) { h ^= uint8_t(c); h *= 1099511628211ull; } for (uint8_t c : s.second) { h ^= c; h *= 1099511628211ull; } } return h; }
// A number that no door state of any Doors object has had before: one at load, a new one at every change of a
// door. (A copy of a Doors keeps its stamp for as long as its doors stay the same.)
inline W NextDoorStamp() { static std::atomic<W> last{0}; return ++last; }
inline W Fnv(const Blob& b) { W h = 1469598103934665603ull; for (const auto& s : b.sec) { for (char c : s.first) { h ^= uint8_t(c); h *= 1099511628211ull; } for (uint8_t c : s.second) { h ^= c; h *= 1099511628211ull; } } return h; }

// ---- the game side
// One goal under one door state: worked out when first needed. A caller may keep it and hand it to the next ask;
// it is used again only for the same goal under the same door state (Doors::stamp, which no other door state of
// any layer shares), so answers never depend on it. Every array has one slot per door area of the layer.
struct GoalSide {
  U goal = NONE; W stamp = 0; bool listed = false; U count = 0, least = BIG, leastC = NONE;      // leastC: the place in q of the least fq (the first among equals)
  W kTurn = 0; long long K = 0; U kInf = BIG;   // Doors::RestBound's two numbers for this goal side (kTurn == turn: worked out)
  std::vector<U> q, fq, qin;            // the open THROUGH doors (as through numbers) the outer world is left by toward the goal, each with the least time on to the goal; for a goal in a pocket: the mouth (its number in the pocket) stepped onto from it
  std::vector<U> rest, restBy; std::vector<W> restOf; W turn = 1;   // per through number, once worked out for this goal (restOf[t] == turn): the least time on to the goal through the outer world, and the place in q that way leaves it by
  U gk = NONE, leastM = BIG;            // the goal's pocket (NONE: none), and the least of mr
  std::vector<U> xq, mr, mx;            // per mouth of the goal's pocket: From's time at the goal (0 on a goal that is this mouth; BIG: none); the least time on to the goal over the pocket's own table; the mouth that way leaves by
  std::vector<U> sp, st; std::vector<W> sb;   // scratch of one ask: the open through doors the source reaches, each with To's time at the source and its block
  std::vector<W> lv; std::vector<U> li, lc;   // scratch of one ask from a pocket: per mouth j of the source's pocket the least time to stand on it (Origin and first reach included), the mouth walked to first, the first reach's slot
  std::vector<U> xp; std::vector<W> xlb;      // scratch of one ask from a pocket: the through doors beside its outside that can still win, each with RestBound's bound
  explicit GoalSide(U D = 0) : q(D), fq(D), qin(D), rest(D), restBy(D), restOf(D, 0), xq(D), mr(D), mx(D), sp(D), st(D), sb(D), lv(D), li(D), lc(D), xp(D), xlb(D) {}
};
struct DoorRoute {                      // the answer to one ask
  bool found = false, door = false;     // door: the route stands on at least one door area
  W time = 0; U first = NONE;           // as Route: origin[first reach] + sum of reach times + 1
  Route r;                              // door false: the lists' route. door true: the last leg, the lists' route from tailArea to the goal (r.found false: none)
  U toDoor = NONE, exitDoor = NONE;     // door true: the door area the route walks to first (or starts on); the one it leaves the layer by (a door goal: the goal)
  U segEnd = NONE, n1First = NONE, n1Last = NONE, n2First = NONE, n2Last = NONE;   // door true: the last door area of the segment toDoor is in; the first and last door area of the next segment and of the one after (NONE: no such segment)
  U tailArea = NONE;                    // door true: the area after the exit reach (enabled index); NONE when the goal is a door area
  U segT = 0, n1T = 0, n2T = 0;         // door true: the TABLE TIME of each of its segments under the doors of the ask (from toDoor to segEnd, n1First to n1Last, n2First to n2Last; 0: no such segment). The walker carries them from here: DBegin copies them
  U legRec = NONE;                      // door true: the last-leg patch the route takes after its last door area (its record number; NONE: the exit reach, as before). Then tailArea is the area the patch ends on
  U how = 0;                            // not charged, for the logs: 1 lists' route, no door route exists; 2 lists' route by the bound; 3 lists' route by the exact time; 4 door route
  W bound = 0;                          // not charged, for the checks (how 2): the bound the lists' route was answered by, as a route time; no door route may be faster than it
  W exact = 0;                          // not charged, for the checks (how 3 and 4): the time of the fastest door route as the ask worked it out, as a route time (Origin and the 1 included)
};

struct Doors {
  const Runtime& rt; const Fixture4& f; U n = 0, N = 0, D = 0, R = 0, rb = 0, gb = 0, pb = 0, legSlack = 0, originSpread = 0, G = 0, P = 0, T = 0, maxMouths = 0; W fileBytes = 0;
  const uint8_t *regionOf = nullptr, *block = nullptr, *thruIndex = nullptr, *thru = nullptr, *regionPocket = nullptr, *fields = nullptr, *table = nullptr;
  const uint8_t *legIndex = nullptr, *legRec = nullptr, *legPath = nullptr; U L = 0; W legPathBytes = 0;      // the last-leg patches: per door area its first record; the records; their reaches; how many records
  std::vector<U> da, tb, xb, dix;       // per door area: its area and its field widths; per area of the map: its door area or NONE
  std::vector<U> grp, pk, tix, mix;     // per door area: its door (group); its pocket (NONE: a through door); its through number (NONE: a mouth); its number among its pocket's mouths
  std::vector<U> thr;                   // per through number: the door area
  std::vector<U> pS, pB, pm0, pmN, pOff, mth;   // per pocket: its region, its outside, where its mouths start in mth and how many, where its table starts in pT; mth: the pockets' mouths (door areas), pocket by pocket
  std::vector<uint8_t> open;            // THE DOOR STATE: per door area, 1 = open. Every door is shut after the load.
  std::vector<U> cT;                    // THE TABLE UNDER THE CURRENT DOORS, through doors only (T x T by through number): least time over open through doors (BIG: none)
  std::vector<U> pT;                    // per pocket ITS OWN TABLE (m x m by mouth number): least time over its open mouths (BIG: none)
  std::vector<U> wOut;                  // per pocket and through number (P x T), FIXED AT LOAD from the baked table: the least step from ANY mouth of the pocket to that through door (BIG: none). A lower bound for the ask; no door changes it
  std::vector<U> tin, tout, ptouch; std::vector<uint8_t> pmark, smark; U nOpen = 0, nOpenT = 0;   // scratch of Insert and Set; the number of open door areas, and of open through doors
  W stamp = 0;                          // names the door state: NextDoorStamp() at load and at every change of a door

  // ---- raw access (no charge here; callers charge)
  U Ew(U p) const { return 2 * tb[p] + rb + xb[p]; }                         // the bits of one entry in door area p's fields
  U TNone(U p) const { return (U(1) << tb[p]) - 1; }
  W BlockRaw(U p, U r) const { const W v = Rd(block + 6 * (size_t(p) * R + r), 6); return v == DNOBLOCK ? NOPOS : v; }
  void RegionRaw(U i, U& r, U& pl) const { const W x = RdBits8(regionOf, W(i) * (gb + pb), gb + pb); r = U(x & ((W(1) << gb) - 1)); pl = U(x >> gb); }
  U ToT(U p, W blk, U pl) const { return U(RdBits8(fields, blk + W(pl) * Ew(p), tb[p])); }
  U ToR(U p, W blk, U pl) const { return U(RdBits8(fields, blk + W(pl) * Ew(p) + tb[p], rb)); }
  U FrT(U p, W blk, U pl) const { return U(RdBits8(fields, blk + W(pl) * Ew(p) + tb[p] + rb, tb[p])); }
  U FrX(U p, W blk, U pl) const { return U(RdBits8(fields, blk + W(pl) * Ew(p) + 2 * tb[p] + rb, xb[p])); }
  U M0T(U p, U q) const { const U t = U(Rd(table + 5 * (size_t(p) * D + q), 4)); return t == 0xFFFFFFFFu ? BIG : t; }
  U M0R(U p, U q) const { return table[5 * (size_t(p) * D + q) + 4]; }
  U Deg(U area) const { return f.g.out[area + 1] - f.g.out[area]; }
  U Spot(U area) const { return rt.index[area] != NONE ? rt.index[area] : dix[area] != NONE ? n + dix[area] : NONE; }   // an area as a walker holds it: enabled index, or n + door area
  U AreaOf(U spot) const { return spot < n ? rt.active[spot] : da[spot - n]; }
  W StateBytes() const { return open.size() + 4 * W(cT.size()) + 4 * W(pT.size()) + 4 * W(tin.size()) + 4 * W(tout.size()) + 4 * W(ptouch.size()) + pmark.size() + smark.size() + 4 * W(wOut.size()); }
  U RegionPocketRaw(U r) const { const U k = U(Rd(regionPocket + 2 * size_t(r), 2)); return k == 65535 ? NONE : k; }
  // the last-leg patches (record number r; no charge here)
  U LegLo(U p) const { return U(Rd(legIndex + 4 * size_t(p), 4)); }                         // the first record of door area p; LegLo(p + 1): one past its last
  U LegGoal(U r) const { return U(Rd(legRec + 18 * size_t(r), 4)); }
  U LegOff(U r) const { return U(Rd(legRec + 18 * size_t(r) + 4, 4)); }
  U LegCount(U r) const { return U(Rd(legRec + 18 * size_t(r) + 8, 2)); }
  U LegTime(U r) const { return U(Rd(legRec + 18 * size_t(r) + 10, 4)); }
  U LegEnd(U r) const { return U(Rd(legRec + 18 * size_t(r) + 14, 4)); }
  U LegFindRaw(U p, U gi) const { U lo = LegLo(p), hi = LegLo(p + 1); const U end = hi; while (lo < hi) { const U mid = (lo + hi) / 2; if (LegGoal(mid) < gi) lo = mid + 1; else hi = mid; } return lo != end && LegGoal(lo) == gi ? lo : NONE; }   // the patch of (door area p, goal gi), or NONE
  U LegDoorRaw(U r) const { U lo = 0, hi = D; while (hi - lo > 1) { const U mid = (lo + hi) / 2; if (LegLo(mid) <= r) lo = mid; else hi = mid; } return lo; }   // the door area whose record r is (r < L; the last door area whose first record is not after r)
  // The least time from door area p to door area q INSIDE ONE SEGMENT: both through doors (the outer table), or
  // both mouths of one pocket (that pocket's table). BIG: no way, or the two are not of one table.
  U SegTimeRaw(U p, U q) const { if (pk[p] != pk[q]) return BIG; if (pk[p] == NONE) return cT[size_t(tix[p]) * T + tix[q]]; return pT[pOff[pk[p]] + size_t(mix[p]) * pmN[pk[p]] + mix[q]]; }
  // Not charged, for the checks and the walker's load: is the rest of a door route (a segment and those that follow) still there under the current doors
  bool PlanHoldsRaw(U door, U segEnd, U n1First, U n1Last, U n2First, U n2Last) const {
    if (door >= D || segEnd >= D || !open[door] || SegTimeRaw(door, segEnd) == BIG) return false;
    if (n1First == NONE) return n1Last == NONE && n2First == NONE && n2Last == NONE;
    if (n1First >= D || n1Last >= D || M0T(segEnd, n1First) == BIG || !open[n1First] || SegTimeRaw(n1First, n1Last) == BIG) return false;
    if (n2First == NONE) return n2Last == NONE;
    return n2First < D && n2Last < D && M0T(n1Last, n2First) != BIG && open[n2First] && SegTimeRaw(n2First, n2Last) != BIG; }

  // ---- load: take the sections and refuse anything an ask or a walk could trip over
  Doors(const Blob& b, const Blob& lists, const Runtime& r) : rt(r), f(r.f) {
    Need(b.HasSpare(), "a door layer in memory without the spare bytes after its sections (Blob::Load makes them)");
    const auto& meta = b.Get("door.meta"); Need(meta.size() == 76 && Rd(meta.data(), 4) == 3, "door layer version");
    n = U(Rd(meta.data() + 4, 4)); N = U(Rd(meta.data() + 8, 4)); D = U(Rd(meta.data() + 12, 4)); R = U(Rd(meta.data() + 16, 4)); rb = U(Rd(meta.data() + 20, 4)); gb = U(Rd(meta.data() + 24, 4)); pb = U(Rd(meta.data() + 28, 4));
    legSlack = U(Rd(meta.data() + 32, 4)); originSpread = U(Rd(meta.data() + 36, 4)); G = U(Rd(meta.data() + 40, 4)); P = U(Rd(meta.data() + 44, 4)); T = U(Rd(meta.data() + 48, 4)); fileBytes = b.Bytes();
    Need(n == rt.n && N == f.g.n && D >= 1 && D < 65535 && R >= 1 && R <= n && R < 65535 && rb >= 1 && rb <= 8 && gb >= 1 && gb <= 24 && pb >= 1 && pb <= 30 && R - 1 < (W(1) << gb) && legSlack <= 684 && 4 * W(originSpread) + legSlack == 684 && G >= 1 && G <= D && P < 65535 && T <= D, "door layer header does not fit this map");
    Need(Rd(meta.data() + 52, 8) == lists.Bytes() && Rd(meta.data() + 60, 8) == Fnv(lists), "the door layer was baked for another lists file");
    Need(Rd(meta.data() + 68, 8) == LayerHash(b), "the door layer's bytes are not the ones that were baked");
    const auto &ar = b.Get("door.area"), &ro = b.Get("door.regionOf"), &bl = b.Get("door.block"), &bi = b.Get("door.besideIndex"), &bs = b.Get("door.beside"), &fl = b.Get("door.fields"), &tbl = b.Get("door.table");
    const auto &pks = b.Get("door.pocket"), &mi = b.Get("door.mouthIndex"), &ms = b.Get("door.mouths"), &ti = b.Get("door.thruIndex"), &ts = b.Get("door.thru"), &rp = b.Get("door.regionPocket");
    Need(ar.size() == 10 * size_t(D) && ro.size() == BitBytes(n, gb + pb) && bl.size() == 6 * size_t(D) * R && bi.size() == 4 * (size_t(R) + 1) && tbl.size() == 5 * size_t(D) * D, "door layer section sizes");
    Need(pks.size() == 8 * size_t(P) && mi.size() == 4 * (size_t(P) + 1) && ti.size() == 4 * (size_t(R) + 1) && rp.size() == 2 * size_t(R), "door layer section sizes: pockets");
    regionOf = ro.data(); block = bl.data(); thruIndex = ti.data(); thru = ts.data(); regionPocket = rp.data(); fields = fl.data(); table = tbl.data(); const uint8_t* besideIndex = bi.data(); const uint8_t* beside = bs.data();
    da.resize(D); tb.resize(D); xb.resize(D); dix.assign(N, NONE); grp.resize(D); pk.resize(D); tix.assign(D, NONE); mix.assign(D, NONE); U mouths = 0;
    for (U p = 0; p < D; ++p) { da[p] = U(Rd(ar.data() + 10 * size_t(p), 4)); tb[p] = ar[10 * size_t(p) + 4]; xb[p] = ar[10 * size_t(p) + 5]; grp[p] = U(Rd(ar.data() + 10 * size_t(p) + 6, 2)); const U k = U(Rd(ar.data() + 10 * size_t(p) + 8, 2)); pk[p] = k == 65535 ? NONE : k;
      Need(da[p] < N && (p == 0 || da[p - 1] < da[p]) && f.g.mutableArea[da[p]] && rt.index[da[p]] == NONE && tb[p] >= 1 && tb[p] <= 24 && xb[p] >= 1 && xb[p] <= 8 && Deg(da[p]) <= 255 && grp[p] < G && (pk[p] == NONE || pk[p] < P), "door area record"); dix[da[p]] = p;
      if (pk[p] == NONE) { tix[p] = U(thr.size()); thr.push_back(p); } else ++mouths; }
    Need(thr.size() == T, "the through doors are not as many as the header says");
    Need(T <= THROUGH_LIMIT, "the layer has more through-door areas than the outer table may hold inside a tic (THROUGH_LIMIT)");
    for (U a = 0; a < N; ++a) Need(!f.g.mutableArea[a] || rt.index[a] != NONE || dix[a] != NONE, "the map has a wide door that the layer does not have");
    // regions and places: a region's areas in ascending order; a reach between two of the lists' areas stays in one region
    std::vector<U> size(R, 0); std::vector<std::vector<U>> areaAt(R);
    for (U i = 0; i < n; ++i) { U rg, pl; RegionRaw(i, rg, pl); Need(rg < R && pl == size[rg], "an area's region or place"); ++size[rg]; areaAt[rg].push_back(i); }
    for (U rg = 0; rg < R; ++rg) Need(size[rg] >= 1, "an empty region");
    for (const auto& e : f.g.edges) { const U i = rt.index[e.from], j = rt.index[e.to]; if (i == NONE || j == NONE) continue; U r1, p1, r2, p2; RegionRaw(i, r1, p1); RegionRaw(j, r2, p2); Need(r1 == r2, "a reach between two regions that passes no door"); }
    // blocks, and the door areas beside each region; the through doors beside each region
    Need(Rd(bi.data(), 4) == 0 && bs.size() == 2 * size_t(Rd(bi.data() + 4 * size_t(R), 4)), "beside index ends"); Need(Rd(ti.data(), 4) == 0 && ts.size() == 2 * size_t(Rd(ti.data() + 4 * size_t(R), 4)), "through index ends");
    std::vector<std::pair<W, W>> spans; const W fieldBits = 8 * W(fl.size());
    for (U rg = 0; rg < R; ++rg) { const U b0 = U(Rd(besideIndex + 4 * size_t(rg), 4)), b1 = U(Rd(besideIndex + 4 * size_t(rg) + 4, 4)); Need(b0 <= b1 && 2 * size_t(b1) <= bs.size(), "beside index"); U k = b0;
      const U t0 = U(Rd(thruIndex + 4 * size_t(rg), 4)), t1 = U(Rd(thruIndex + 4 * size_t(rg) + 4, 4)); Need(t0 <= t1 && 2 * size_t(t1) <= ts.size(), "through index"); U kt = t0;
      for (U p = 0; p < D; ++p) { const W blk = BlockRaw(p, rg); const bool listed = k < b1 && U(Rd(beside + 2 * size_t(k), 2)) == p; Need(listed == (blk != NOPOS), "a region's door areas and its blocks disagree");
        if (!listed) continue; ++k; const W bits = W(size[rg]) * Ew(p); Need(blk <= fieldBits && bits <= fieldBits - blk, "a block outside the fields"); spans.push_back({blk, blk + bits});
        if (pk[p] == NONE) { Need(kt < t1 && U(Rd(thru + 2 * size_t(kt), 2)) == p, "a region's through doors are not listed as they are"); ++kt; } }
      Need(k == b1 && kt == t1, "beside list"); }
    std::sort(spans.begin(), spans.end()); for (size_t i = 1; i < spans.size(); ++i) Need(spans[i - 1].second <= spans[i].first, "blocks overlap");
    // pockets: S and B, the mouths; a mouth is beside exactly its pocket's S and B, and every door area beside S is a mouth of it
    pS.resize(P); pB.resize(P); pm0.resize(P); pmN.resize(P); pOff.resize(P); Need(Rd(mi.data(), 4) == 0 && ms.size() == 2 * size_t(Rd(mi.data() + 4 * size_t(P), 4)) && ms.size() == 2 * size_t(mouths), "mouth index ends"); mth.resize(mouths); size_t tableSlots = 0;
    for (U k = 0; k < P; ++k) { pS[k] = U(Rd(pks.data() + 8 * size_t(k), 4)); pB[k] = U(Rd(pks.data() + 8 * size_t(k) + 4, 4)); const U m0 = U(Rd(mi.data() + 4 * size_t(k), 4)), m1 = U(Rd(mi.data() + 4 * size_t(k) + 4, 4));
      Need(pS[k] < R && pB[k] < R && pS[k] != pB[k] && m0 < m1 && m1 <= mouths && RegionPocketRaw(pS[k]) == k, "pocket record"); pm0[k] = m0; pmN[k] = m1 - m0; pOff[k] = U(tableSlots); tableSlots += size_t(pmN[k]) * pmN[k]; maxMouths = std::max(maxMouths, pmN[k]);
      for (U j = m0; j < m1; ++j) { const U p = U(Rd(ms.data() + 2 * size_t(j), 2)); Need(p < D && pk[p] == k && (j == m0 || mth[j - 1] < p), "a pocket's mouths"); mth[j] = p; mix[p] = j - m0;
        for (U rg = 0; rg < R; ++rg) Need((BlockRaw(p, rg) != NOPOS) == (rg == pS[k] || rg == pB[k]), "a mouth that is not beside exactly its pocket and the outside"); } }
    for (U rg = 0; rg < R; ++rg) { const U k = RegionPocketRaw(rg); Need(k == NONE || (k < P && pS[k] == rg), "a region's pocket"); if (k == NONE) continue; for (U p = 0; p < D; ++p) Need(BlockRaw(p, rg) == NOPOS || pk[p] == k, "a door area beside a pocket that is not its mouth"); }
    Need(maxMouths <= MOUTH_LIMIT, "the layer has a pocket with more mouths than one pocket may have (MOUTH_LIMIT)");
    // the fields: a To entry continues to its door area with the times adding up; an exit reach leaves the door area into the entry's region
    for (U p = 0; p < D; ++p) for (U rg = 0; rg < R; ++rg) { const W blk = BlockRaw(p, rg); if (blk == NOPOS) continue; const U none = TNone(p);
      for (U pl = 0; pl < size[rg]; ++pl) { const U i = areaAt[rg][pl], a = rt.active[i]; const U t = ToT(p, blk, pl), ft = FrT(p, blk, pl);
        if (t != none) { const U ord = ToR(p, blk, pl); Need(ord < Deg(a), "To reach position"); const auto& e = f.g.edges[f.g.out[a] + ord];
          if (e.to == da[p]) Need(t == e.w, "To time at the door area"); else { const U j = rt.index[e.to]; Need(j != NONE, "To reach leaves the lists' map"); U r2, p2; RegionRaw(j, r2, p2); Need(ToT(p, blk, p2) != none && W(ToT(p, blk, p2)) + e.w == t, "To entry does not continue"); } }
        if (ft != none) { const U x = FrX(p, blk, pl); Need(x < Deg(da[p]), "exit reach position"); const auto& e = f.g.edges[f.g.out[da[p]] + x]; const U j = rt.index[e.to]; Need(j != NONE && ft >= e.w, "exit reach leaves the lists' map");
          U r2, p2; RegionRaw(j, r2, p2); Need(r2 == rg && FrT(p, blk, p2) == e.w && (j != i || ft == e.w), "exit reach does not lead toward the area"); } } }
    // the table: a way from door area to door area starts with a reach of the first and continues on To of the second
    for (U p = 0; p < D; ++p) for (U q = 0; q < D; ++q) { const U t = M0T(p, q), ord = M0R(p, q); if (p == q || t == BIG) { Need(t == BIG && ord == 255, "empty table entry"); continue; }
      Need(ord < Deg(da[p]), "table reach position"); const auto& e = f.g.edges[f.g.out[da[p]] + ord]; if (e.to == da[q]) { Need(t == e.w, "table time of a direct reach"); continue; }
      const U j = rt.index[e.to]; Need(j != NONE, "table reach leaves the lists' map"); U r2, p2; RegionRaw(j, r2, p2); const W blk = BlockRaw(q, r2); Need(blk != NOPOS && ToT(q, blk, p2) != TNone(q) && W(ToT(q, blk, p2)) + e.w == t, "table entry does not continue"); }
    // the last-leg patches (the head of this file says what is checked and what is the bake's doing)
    { const auto &li = b.Get("door.legIndex"), &lr = b.Get("door.legRec"), &lp = b.Get("door.legPath");
      Need(li.size() == 4 * (size_t(D) + 1) && lr.size() % 18 == 0 && lr.size() / 18 < NONE && Rd(li.data(), 4) == 0 && Rd(li.data() + 4 * size_t(D), 4) == lr.size() / 18, "last-leg patch sections");
      legIndex = li.data(); legRec = lr.data(); legPath = lp.data(); L = U(lr.size() / 18); legPathBytes = lp.size(); W pathAt = 0;
      for (U p = 0; p < D; ++p) { const U r0 = LegLo(p), r1 = LegLo(p + 1); Need(r0 <= r1 && r1 <= L, "last-leg patch index");
        for (U r = r0; r < r1; ++r) { const U g = LegGoal(r), off = LegOff(r), cnt = LegCount(r); Need(g < n && (r == r0 || LegGoal(r - 1) < g) && cnt >= 2 && off == pathAt && W(off) + cnt <= legPathBytes, "last-leg patch record"); pathAt += cnt;
          U rg, pl; RegionRaw(g, rg, pl); const W blk = BlockRaw(p, rg); Need(blk != NOPOS && FrT(p, blk, pl) != TNone(p), "a last-leg patch for a goal that its door area does not reach"); U at = NONE; W sum = 0;
          for (U i = 0; i < cnt; ++i) { const U from = i == 0 ? da[p] : rt.active[at], ord = legPath[off + i]; Need(ord < Deg(from) && (i == 0 || at != g), "last-leg patch: a reach position, or a reach after the goal");
            const auto& e = f.g.edges[f.g.out[from] + ord]; const U j = rt.index[e.to]; Need(j != NONE, "last-leg patch: a reach that leaves the lists' map"); sum += e.w; at = j;
            U r2, p2; RegionRaw(j, r2, p2); Need(r2 == rg && FrT(p, blk, p2) == sum, "last-leg patch: not a least-time way from its door area"); }
          Need(at == LegEnd(r) && sum == LegTime(r) && (at == g || sum < FrT(p, blk, pl)), "last-leg patch: its end or its time"); } }
      Need(pathAt == legPathBytes, "last-leg patch reaches that belong to no patch"); }
    wOut.assign(size_t(P) * T, BIG); for (U k = 0; k < P; ++k) for (U j = pm0[k]; j < pm0[k] + pmN[k]; ++j) for (U t = 0; t < T; ++t) wOut[size_t(k) * T + t] = std::min(wOut[size_t(k) * T + t], M0T(mth[j], thr[t]));
    open.assign(D, 0); cT.assign(size_t(T) * T, BIG); pT.assign(tableSlots, BIG); tin.assign(T, BIG); tout.assign(T, BIG); ptouch.assign(P, 0); pmark.assign(P, 0); smark.assign(D, 0); nOpen = nOpenT = 0; stamp = NextDoorStamp();
  }

  // ---- charged helpers
  void RegionOf(U i, U& r, U& pl, Meter& m) const { m.Tick(2); const W x = Runtime::Bits(regionOf, W(i) * (gb + pb), gb + pb, m); r = U(x & ((W(1) << gb) - 1)); pl = U(x >> gb); }
  W Block(U p, U r, Meter& m) const { m.Tick(2 + Fld(6) + 3); return BlockRaw(p, r); }
  void Thru(U r, U& b0, U& b1, Meter& m) const { m.Tick(2 + 2 * Fld(4)); b0 = U(Rd(thruIndex + 4 * size_t(r), 4)); b1 = U(Rd(thruIndex + 4 * size_t(r) + 4, 4)); }      // the through doors beside a region
  U RegionPocket(U r, Meter& m) const { m.Tick(2 + Fld(2) + 3); return RegionPocketRaw(r); }
  U SegTime(U p, U q, Meter& m) const { m.Tick(2 * 4 + 3 + 3 + 2 * 4 + 2 + 3); return SegTimeRaw(p, q); }      // the two pockets, the two tests, the two numbers, the address, the time
  // The last-leg patch of (door area p, goal gi: an enabled index), or NONE: a binary search among p's records,
  // which are in the order of their goals. Charged as the lists' PatchLower and PatchFind are, with a 4-byte key.
  U LegFind(U p, U gi, Meter& m) const {
    m.Tick(2 + 2 * Fld(4)); U lo = LegLo(p), hi = LegLo(p + 1); const U end = hi;
    while (lo < hi) { m.Tick(3 + 2 + 2 + Fld(4) + 3 + 2); const U mid = (lo + hi) / 2; if (LegGoal(mid) < gi) lo = mid + 1; else hi = mid; }
    m.Tick(3 + 3); if (lo == end) return NONE; m.Tick(2 + Fld(4) + 3); return LegGoal(lo) == gi ? lo : NONE; }

  // ---- a door changes. A table under the current doors holds least TIMES only, so it is a function of the door
  // state alone, however it was worked out. The outer table (through doors): whole (Rebuild), or by putting a newly
  // opened door area into it (Insert), as built. A pocket's own table: whole (PocketRebuild), whenever one of its
  // mouths changes; no outer table work.
  void Set(const std::vector<U>& areas, bool nowOpen, Meter& m) {
    U changingT = 0, changingM = 0;
    for (U a : areas) { m.Tick(3 + 2 + 4 + 2 * 3 + 3 + 4 + 3 + 2); Need(a < N && dix[a] != NONE, "a door change for an area that is not a wide door"); const U p = dix[a]; if (open[p] != uint8_t(nowOpen)) ++(pk[p] == NONE ? changingT : changingM); }
    m.Tick(3 + 3 + 3); if (!changingT && !changingM) return;                 // every one of them is as asked already: the door state and its stamp stay
    m.Tick(3); if (changingM) { U touched = 0; m.Tick(2);
      for (U a : areas) { m.Tick(3 + 2 + 4 + 4 + 3 + 3); const U p = dix[a], k = pk[p]; if (k == NONE || open[p] == uint8_t(nowOpen)) continue;
        m.Tick(2 * 2 + 3 + 3); open[p] = nowOpen; if (nowOpen) ++nOpen; else --nOpen; if (!pmark[k]) { m.Tick(3 * 2); pmark[k] = 1; ptouch[touched++] = k; } }
      m.Tick(3); for (U i = 0; i < touched; ++i) { m.Tick(3 + 2 + 2 * 2); pmark[ptouch[i]] = 0; PocketRebuild(ptouch[i], m); } m.Tick(3); }
    m.Tick(3); if (changingT) { m.Tick(3 + 3 + 4);
      bool whole = !nowOpen;                                                   // a through door shuts: the whole table
      if (nowOpen) { m.Tick(40);                                               // several open at once: the whole table, or one Insert per area, whichever is charged less (the two sums of charges, for choosing)
        const W n0 = nOpenT, c = changingT, n1 = n0 + c, t = T;
        const W rebuild = 17 * t * t + 15 * t + 19 * n1 * n1 + 13 * n1 * t + n1 * (n1 - 1) * (10 * t + 5 * n1 + 3);
        auto squares = [](W x) { return x * (x + 1) * (2 * x + 1) / 6; };     // 1 + 4 + ... + x^2
        const W sumN = c * (2 * n0 + c - 1) / 2, sumN2 = squares(n1 - 1) - (n0 ? squares(n0 - 1) : 0);
        const W inserts = c * (43 * t + 15) + (25 * t + 36) * sumN + 19 * (sumN2 + sumN);      // sumN: the open ones before each Insert, added up; sumN2: their squares
        whole = rebuild <= inserts; }
      if (whole) {
        for (U a : areas) { m.Tick(3 + 2 + 4 + 4 + 3 + 3 + 3 + 3 * 2); const U p = dix[a]; if (pk[p] == NONE && open[p] != uint8_t(nowOpen)) { open[p] = nowOpen; if (nowOpen) { ++nOpen; ++nOpenT; } else { --nOpen; --nOpenT; } } }
        m.Tick(3); Rebuild(m); }
      else { for (U a : areas) { m.Tick(3 + 2 + 4 + 4 + 3 + 3); const U p = dix[a]; if (pk[p] == NONE && !open[p]) { m.Tick(3 * 2 + 4); open[p] = 1; ++nOpen; ++nOpenT; Insert(tix[p], m); } } m.Tick(3); } }
    m.Tick(2); stamp = NextDoorStamp(); }
  // ONE door change that OPENS some door areas and SHUTS others (the caller's rule: every door change of a tic in
  // one call). No area may be in both lists. When a through door shuts, every change is written into the door state
  // first, then each touched pocket's table and the whole outer table are worked out ONCE. When none does (only
  // mouths shut), the shutting and the opening are done as two changes of one kind, the shutting first.
  void Set(const std::vector<U>& opening, const std::vector<U>& shutting, Meter& m) {
    m.Tick(3 + 3); if (shutting.empty()) { Set(opening, true, m); return; } if (opening.empty()) { Set(shutting, false, m); return; }
    U shutT = 0;
    for (U a : shutting) { m.Tick(3 + 2 + 4 + 2 * 3 + 3 + 4 + 3 + 2 + 2); Need(a < N && dix[a] != NONE, "a door change for an area that is not a wide door"); const U p = dix[a]; smark[p] = 1; if (open[p] && pk[p] == NONE) ++shutT; }
    bool both = false; for (U a : opening) { m.Tick(3 + 2 + 4 + 2 * 3 + 3 + 3); Need(a < N && dix[a] != NONE, "a door change for an area that is not a wide door"); both = both || smark[dix[a]]; }
    for (U a : shutting) { m.Tick(3 + 2 + 4 + 2); smark[dix[a]] = 0; } m.Tick(3 * 3 + 3);
    Need(!both, "a door area to open and to shut in one change");
    if (!shutT) { Set(shutting, false, m); Set(opening, true, m); return; }
    U touched = 0;
    auto flip = [&](U a, bool to) { m.Tick(3 + 2 + 4 + 4 + 3 + 3); const U p = dix[a], k = pk[p]; if (open[p] == uint8_t(to)) return; m.Tick(2 * 2 + 3 + 3); open[p] = to; if (to) ++nOpen; else --nOpen;
      if (k == NONE) { m.Tick(2); if (to) ++nOpenT; else --nOpenT; } else if (!pmark[k]) { m.Tick(3 * 2); pmark[k] = 1; ptouch[touched++] = k; } };
    for (U a : shutting) flip(a, false); for (U a : opening) flip(a, true); m.Tick(2 * 3);
    for (U i = 0; i < touched; ++i) { m.Tick(3 + 2 + 2 * 2); pmark[ptouch[i]] = 0; PocketRebuild(ptouch[i], m); } m.Tick(3);
    Rebuild(m); m.Tick(2); stamp = NextDoorStamp(); }
  void Rebuild(Meter& m) {                                                    // the whole outer table from the door state (p, q, k, i, j: through numbers)
    m.Tick(4);
    for (U p = 0; p < T; ++p) { m.Tick(3 + 2);
      for (U q = 0; q < T; ++q) { m.Tick(3 + 2 + 2 * 3 + 2 + 2 * 2); U t = BIG;
        if (open[thr[p]] && open[thr[q]]) { m.Tick(3); if (p == q) t = 0; else { m.Tick(2 + Fld(4) + 3); t = M0T(thr[p], thr[q]); } }
        cT[size_t(p) * T + q] = t; }
      m.Tick(3); }
    m.Tick(3);
    for (U k = 0; k < T; ++k) { m.Tick(3 + 2 + 2 + 3); if (!open[thr[k]]) continue;
      for (U i = 0; i < T; ++i) { m.Tick(3 + 2 + 2 + 3 + 3); const U ik = cT[size_t(i) * T + k]; if (i == k || ik == BIG) continue;
        for (U j = 0; j < T; ++j) { m.Tick(3 + 2 + 2 + 3); const U kj = cT[size_t(k) * T + j]; if (kj == BIG) continue;
          m.Tick(2 + 3); if (W(ik) + kj < cT[size_t(i) * T + j]) { m.Tick(2); cT[size_t(i) * T + j] = ik + kj; } }
        m.Tick(3); }
      m.Tick(3); }
    m.Tick(3); }
  // Through door v (a through number) has just opened (open is set); its row and column of the outer table are
  // still empty, and the table is right for every other open through door. Least times to v and from v end or
  // begin with one table step of v, after or before a way among the others; then every pair may go through v once.
  void Insert(U v, Meter& m) {
    m.Tick(4);
    for (U j = 0; j < T; ++j) { m.Tick(3 + 2 + 3 + 2 + 3 + 2 * 2); U a = BIG, b = BIG; if (j != v && open[thr[j]]) { m.Tick(2 * (2 + Fld(4) + 3)); a = M0T(thr[j], thr[v]); b = M0T(thr[v], thr[j]); } tin[j] = a; tout[j] = b; }
    m.Tick(3);
    for (U i = 0; i < T; ++i) { m.Tick(3 + 2 + 3 + 2 + 3); if (i == v || !open[thr[i]]) continue; m.Tick(2 * 2); U to = tin[i], from = tout[i];
      for (U j = 0; j < T; ++j) { m.Tick(3 + 2 + 2 + 3); const U ij = cT[size_t(i) * T + j];
        if (ij != BIG) { m.Tick(3 + 3); if (tin[j] != BIG && W(ij) + tin[j] < to) { m.Tick(2); to = ij + tin[j]; } }
        m.Tick(2 + 3); const U ji = cT[size_t(j) * T + i];
        if (ji != BIG) { m.Tick(3 + 3); if (tout[j] != BIG && W(tout[j]) + ji < from) { m.Tick(2); from = tout[j] + ji; } } }
      m.Tick(3 + 2 * 2); cT[size_t(i) * T + v] = to; cT[size_t(v) * T + i] = from; }
    m.Tick(3 + 2); cT[size_t(v) * T + v] = 0;
    for (U i = 0; i < T; ++i) { m.Tick(3 + 2 + 2 + 3 + 3); const U iv = cT[size_t(i) * T + v]; if (i == v || iv == BIG) continue;
      for (U j = 0; j < T; ++j) { m.Tick(3 + 2 + 2 + 3); const U vj = cT[size_t(v) * T + j]; if (vj == BIG) continue;
        m.Tick(2 + 3); if (W(iv) + vj < cT[size_t(i) * T + j]) { m.Tick(2); cT[size_t(i) * T + j] = iv + vj; } }
      m.Tick(3); }
    m.Tick(3); }
  // Pocket k's own table from the door state: least times over its open mouths (i, j, h: mouth numbers of k).
  void PocketRebuild(U k, Meter& m) {
    m.Tick(4 + 3 * 2); const U mk = pmN[k]; const U* mo = mth.data() + pm0[k]; U* t = pT.data() + pOff[k];
    for (U i = 0; i < mk; ++i) { m.Tick(3 + 2);
      for (U j = 0; j < mk; ++j) { m.Tick(3 + 2 + 2 * 3 + 2 + 2 * 2); U v = BIG;
        if (open[mo[i]] && open[mo[j]]) { m.Tick(3); if (i == j) v = 0; else { m.Tick(2 + Fld(4) + 3); v = M0T(mo[i], mo[j]); } }
        t[size_t(i) * mk + j] = v; }
      m.Tick(3); }
    m.Tick(3);
    for (U h = 0; h < mk; ++h) { m.Tick(3 + 2 + 2 + 3); if (!open[mo[h]]) continue;
      for (U i = 0; i < mk; ++i) { m.Tick(3 + 2 + 2 + 3 + 3); const U ih = t[size_t(i) * mk + h]; if (i == h || ih == BIG) continue;
        for (U j = 0; j < mk; ++j) { m.Tick(3 + 2 + 2 + 3); const U hj = t[size_t(h) * mk + j]; if (hj == BIG) continue;
          m.Tick(2 + 3); if (W(ih) + hj < t[size_t(i) * mk + j]) { m.Tick(2); t[size_t(i) * mk + j] = ih + hj; } }
        m.Tick(3); }
      m.Tick(3); }
    m.Tick(3); }
  // Not charged, for the checks: every table is exactly the least times over its open door areas, worked out afresh.
  bool TableHolds() const { U count = 0, countT = 0; for (U p = 0; p < D; ++p) { count += open[p] != 0; countT += open[p] != 0 && pk[p] == NONE; } if (count != nOpen || countT != nOpenT) return false;
    auto fresh = [&](const U* ids, U cnt, const U* have) { std::vector<U> t(size_t(cnt) * cnt, BIG);
      for (U p = 0; p < cnt; ++p) for (U q = 0; q < cnt; ++q) if (open[ids[p]] && open[ids[q]]) t[size_t(p) * cnt + q] = p == q ? 0 : M0T(ids[p], ids[q]);
      for (U k = 0; k < cnt; ++k) for (U i = 0; i < cnt; ++i) for (U j = 0; j < cnt; ++j) { const U ik = t[size_t(i) * cnt + k], kj = t[size_t(k) * cnt + j]; if (ik != BIG && kj != BIG && W(ik) + kj < t[size_t(i) * cnt + j]) t[size_t(i) * cnt + j] = ik + kj; }
      return std::equal(t.begin(), t.end(), have); };
    if (!fresh(thr.data(), T, cT.data())) return false; for (U k = 0; k < P; ++k) if (!fresh(mth.data() + pm0[k], pmN[k], pT.data() + pOff[k])) return false; return true; }
  // The next through door on a least way from through door p to through door q over open through doors (all three
  // as through numbers): the lowest-numbered one that the table's times fit. NONE: no way, or p is q.
  U Hop(U p, U q, Meter& m) const {
    m.Tick(2 + 3 + 3); const U whole = cT[size_t(p) * T + q]; if (whole == BIG || p == q) return NONE;
    for (U h = 0; h < T; ++h) { m.Tick(3 + 2 + 3 + 2 + 3); if (h == p || !open[thr[h]]) continue; m.Tick(2 + 3 + 3); const U rest = cT[size_t(h) * T + q]; if (rest == BIG || rest >= whole) continue;
      m.Tick(2 * 2 + 2 + Fld(4) + 3 + 3); const U t = M0T(thr[p], thr[h]); if (t != BIG && W(t) + rest == whole) return h; }
    m.Tick(3); return NONE; }
  // The same inside pocket k, over its own table (i, j and the result: mouth numbers of k).
  U PocketHop(U k, U i, U j, Meter& m) const {
    m.Tick(3 * 2 + 2 + 3 + 3); const U mk = pmN[k]; const U* mo = mth.data() + pm0[k]; const U* t = pT.data() + pOff[k]; const U whole = t[size_t(i) * mk + j]; if (whole == BIG || i == j) return NONE;
    for (U h = 0; h < mk; ++h) { m.Tick(3 + 2 + 3 + 2 + 3); if (h == i || !open[mo[h]]) continue; m.Tick(2 + 3 + 3); const U rest = t[size_t(h) * mk + j]; if (rest == BIG || rest >= whole) continue;
      m.Tick(2 * 2 + 2 + Fld(4) + 3 + 3); const U v = M0T(mo[i], mo[h]); if (v != BIG && W(v) + rest == whole) return h; }
    m.Tick(3); return NONE; }
  // The next DOOR AREA on the least way from door area p to door area q of the same segment (p is not q): the one
  // rule the ask and the walker share. NONE: no way under the current doors.
  U SegHop(U p, U q, Meter& m) const {
    m.Tick(2 * 4 + 3 + 3); Need(p < D && q < D && pk[p] == pk[q], "a segment whose two ends are not of one table");
    if (pk[p] == NONE) { m.Tick(2 * 4 + 3 + 2); const U h = Hop(tix[p], tix[q], m); return h == NONE ? NONE : thr[h]; }
    m.Tick(2 * 4 + 3 + 3 * 2); const U h = PocketHop(pk[p], mix[p], mix[q], m); return h == NONE ? NONE : mth[pm0[pk[p]] + h]; }

  // ---- the goal's side
  // A goal outside every pocket: the open through doors beside its region, each with From's time (as built).
  // A goal in pocket k (or on a mouth of k): per mouth of k the least time on to the goal over k's own table
  // (xq, mr, mx), and the open through doors beside k's outside, each with one table step onto a mouth and that
  // mouth's mr (q, fq, qin).
  void List(U gArea, U gp, U gr, U gpl, GoalSide& gs, Meter& m) const {
    m.Tick(3 + 3 + 3); if (gs.goal == gArea && gs.stamp == stamp && gs.listed) return;
    if (gs.q.size() != D) gs = GoalSide(D);                                   // a goal side that was made for another layer, or for none
    m.Tick(9 * 2 + 3); gs.goal = gArea; gs.stamp = stamp; gs.listed = true; gs.count = 0; gs.least = BIG; gs.leastC = NONE; gs.leastM = BIG; ++gs.turn;      // the new turn forgets every remembered time, and RestBound's numbers
    m.Tick(3); if (gp != NONE) { m.Tick(4 + 2); gs.gk = pk[gp]; } else { m.Tick(2); gs.gk = RegionPocket(gr, m); }
    m.Tick(3); const U k = gs.gk;
    if (k == NONE) {
      if (gp != NONE) { m.Tick(4 + 5 * 2); gs.q[0] = tix[gp]; gs.fq[0] = 0; gs.count = 1; gs.least = 0; gs.leastC = 0; return; }       // a goal that is an open through door: the outer world ends on it
      U b0, b1; Thru(gr, b0, b1, m);
      for (U i = b0; i < b1; ++i) { m.Tick(3 + 2 + 2 + Fld(2) + 3); const U p = U(Rd(thru + 2 * size_t(i), 2)); if (!open[p]) continue;
        const W blk = Block(p, gr, m); m.Tick(3 + 2 + 3); const U t = U(Runtime::Bits(fields, blk + W(gpl) * Ew(p) + tb[p] + rb, tb[p], m)); if (t == TNone(p)) continue;
        m.Tick(4 + 3 * 2 + 3 + 2 * 2); gs.q[gs.count] = tix[p]; gs.fq[gs.count] = t; if (t < gs.least) { gs.least = t; gs.leastC = gs.count; } ++gs.count; }
      m.Tick(3); return; }
    // the goal's pocket: each mouth's own way to the goal, then the least over the pocket's table
    m.Tick(3 * 2); const U mk = pmN[k]; const U* mo = mth.data() + pm0[k]; const U* pt = pT.data() + pOff[k];
    for (U j = 0; j < mk; ++j) { m.Tick(3 + 2 + 2 + 3 + 2); const U p = mo[j]; U t = BIG;
      if (open[p]) { m.Tick(3); if (gp != NONE) { m.Tick(3); if (p == gp) t = 0; }
        else { const W blk = Block(p, gr, m); m.Tick(3 + 2 + 3); const U v = U(Runtime::Bits(fields, blk + W(gpl) * Ew(p) + tb[p] + rb, tb[p], m)); if (v != TNone(p)) t = v; } }
      gs.xq[j] = t; }
    m.Tick(3);
    for (U i = 0; i < mk; ++i) { m.Tick(3 + 2 + 2 * 2); U best = BIG, by = NONE;
      for (U j = 0; j < mk; ++j) { m.Tick(3 + 2 + 3 + 2 + 3); if (gs.xq[j] == BIG) continue; const U t = pt[size_t(i) * mk + j]; if (t == BIG) continue; m.Tick(3 + 3); const U v = t + gs.xq[j]; if (v < best) { m.Tick(2 * 2); best = v; by = j; } }
      m.Tick(3 + 2 * 2 + 3); gs.mr[i] = best; gs.mx[i] = by; if (best < gs.leastM) { m.Tick(2); gs.leastM = best; } }
    m.Tick(3 + 3); if (gs.leastM == BIG) return;                               // no open mouth leads to the goal: nothing comes in from the outer world either
    U b0, b1; Thru(pB[k], b0, b1, m);
    for (U i = b0; i < b1; ++i) { m.Tick(3 + 2 + 2 + Fld(2) + 3 + 2 * 2); const U p = U(Rd(thru + 2 * size_t(i), 2)); if (!open[p]) continue; U best = BIG, in = NONE;
      for (U j = 0; j < mk; ++j) { m.Tick(3 + 2 + 3); if (gs.mr[j] == BIG) continue; m.Tick(2 + 2 + Fld(4) + 3); const U t = M0T(p, mo[j]); if (t == BIG) continue; m.Tick(3 + 3); const U v = t + gs.mr[j]; if (v < best) { m.Tick(2 * 2); best = v; in = j; } }
      m.Tick(3 + 3); if (best == BIG) continue;
      m.Tick(4 + 4 * 2 + 3 + 2 * 2); gs.q[gs.count] = tix[p]; gs.fq[gs.count] = best; gs.qin[gs.count] = in; if (best < gs.least) { gs.least = best; gs.leastC = gs.count; } ++gs.count; }
    m.Tick(3); }
  // The least plain time from open through door t (a through number) on to the goal through the outer world (BIG:
  // none), and the place in gs.q of the through door that way leaves the outer world by (the first listed one among
  // equals). Worked out once per goal side and through door.
  U Rest(GoalSide& gs, U t, U& by, Meter& m) const {
    m.Tick(2 + 3); if (gs.restOf[t] == gs.turn) { m.Tick(2 * 2); by = gs.restBy[t]; return gs.rest[t]; }
    U best = BIG; by = NONE; m.Tick(2 * 2);
    for (U c = 0; c < gs.count; ++c) { m.Tick(3 + 2 + 3 + 2 + 3 + 3); const U v0 = cT[size_t(t) * T + gs.q[c]]; if (v0 == BIG) continue; m.Tick(3 + 3); const U v = v0 + gs.fq[c]; if (v < best) { m.Tick(2 * 2); best = v; by = c; } }
    m.Tick(3 + 3 * 2); gs.restOf[t] = gs.turn; gs.rest[t] = best; gs.restBy[t] = by; return best; }

  // A LOWER BOUND of Rest(gs, t) for one read of the outer table (BIG: there is no way on from t at all). q* is the
  // listed through door of least time on to the goal (gs.leastC). The outer table holds least times over the open
  // through doors, so for every listed q with a way to q*:  cT[t][q*] <= cT[t][q] + cT[q][q*],  and so
  //   cT[t][q] + fq[q]  >=  cT[t][q*] - (cT[q][q*] - fq[q])  >=  cT[t][q*] - K,   K = the largest cT[q][q*] - fq[q].
  // A listed q with no way to q* is bounded by its own fq (kInf: the least of those). And no time on to the goal is
  // below gs.least. K and kInf are worked out once per goal side (gs.count must not be 0).
  W RestBound(GoalSide& gs, U t, Meter& m) const {
    m.Tick(3 + 2 + 2); const U qs = gs.q[gs.leastC];
    if (gs.kTurn != gs.turn) { m.Tick(4 * 2); gs.kTurn = gs.turn; long long K = -(1LL << 62); U inf = BIG;
      for (U c = 0; c < gs.count; ++c) { m.Tick(3 + 2 + 2 + 2 + 3 + 3); const U v = cT[size_t(gs.q[c]) * T + qs];
        if (v == BIG) { m.Tick(2 + 3); if (gs.fq[c] < inf) { m.Tick(2); inf = gs.fq[c]; } }
        else { m.Tick(2 + 3 + 3); const long long d = (long long)v - (long long)gs.fq[c]; if (d > K) { m.Tick(2); K = d; } } }
      m.Tick(3 + 2 * 2); gs.K = K; gs.kInf = inf; }
    m.Tick(2 + 3 + 3 + 2); const U x = cT[size_t(t) * T + qs]; W lb = gs.kInf;
    if (x != BIG) { m.Tick(3 + 3 + 3 + 2); const long long via = (long long)x - gs.K; const W fin = via > (long long)gs.least ? W(via) : W(gs.least); if (fin < lb) lb = fin; }
    return lb; }

  // ---- the ask: one source, one goal, as the map's area numbers (a door area has no enabled index)
  DoorRoute Ask(U sArea, U gArea, GoalSide& gs, Meter& m) const {
    DoorRoute a; m.Tick(12 + 2 * (4 + 4 + 3 * 3)); Need(sArea < N && gArea < N, "ask outside the map");
    const U si = rt.index[sArea], gi = rt.index[gArea], sp = dix[sArea], gp = dix[gArea];
    if ((si == NONE && (sp == NONE || !open[sp])) || (gi == NONE && (gp == NONE || !open[gp]))) return a;     // an end that is no area of the map now: a shut door, or no area at all
    m.Tick(3); if (sArea == gArea) { a.found = true; a.how = 1; return a; }
    U sr = NONE, spl = 0, gr = NONE, gpl = 0; m.Tick(3 + 3); if (si != NONE) RegionOf(si, sr, spl, m); if (gi != NONE) RegionOf(gi, gr, gpl, m);
    // 1. the lists' own route: both ends in one region
    bool direct = false; m.Tick(3 + 3 + 3); if (si != NONE && gi != NONE && sr == gr) { a.r = rt.Ask(si, gi, m); direct = a.r.found; }
    auto lists = [&](U how) { m.Tick(8); a.found = true; a.door = false; a.time = a.r.time; a.first = a.r.first; a.how = how; };
    // 2. the goal's side, and the source's pocket
    List(gArea, gp, gr, gpl, gs, m);
    const U gk = gs.gk; U sk; m.Tick(2 + 3); if (sp != NONE) { m.Tick(4 + 2); sk = pk[sp]; } else sk = RegionPocket(sr, m);
    // with no way from any door area on to the goal there is no door route. (A goal outside every pocket can also
    // be reached straight from a mouth of the source's pocket, when the goal is in that pocket's outside.)
    m.Tick(3 + 3 + 3); if (gk != NONE ? gs.leastM == BIG : (gs.count == 0 && !(sk != NONE && gp == NONE && gr == pB[sk]))) { m.Tick(3); if (direct) lists(1); return a; }
    // the best door route so far. v: its exact time (from a region: Origin of the first reach and that reach's time
    // included; from a door area: the plain time). kind 1: it enters the outer world at through door p (from a
    // region) or starts on it; 2: it walks straight to mouth gi2 of the goal's pocket; 3: it starts in the source's
    // pocket and leaves it by mouth sj, then xkind 0: straight to the goal, 1: one step to mouth xa of the goal's
    // pocket, 2: one step to through number xa. c: the place in gs.q where it leaves the outer world.
    W best = NOTIME; U bestE = NONE, kind = 0, bestP = NONE, bestC = NONE, gi2 = NONE, sj = NONE, xkind = 0, xa = NONE; m.Tick(9 * 2);
    // the first reaches of a source in a region, each with its Origin: one into an open door area is held by that
    // door area's number, one into the source's region by its place for To
    U cnt = 0; U fe[256], fpl[256], fdoor[256]; W ff[256];
    auto reaches = [&]() { const U a0 = f.g.out[sArea], a1 = f.g.out[sArea + 1]; m.Tick(4 + 3); Need(a1 - a0 <= 256, "more first reaches than the ask holds");
      for (U e = a0; e < a1; ++e) { m.Tick(3 + 2 + 3 * 4 + 3 + 3); const U to = f.g.edges[e].to, tp = dix[to], ti = rt.index[to]; U place = NONE;      /* three reads of the map: the target, its door number, its enabled index */
        if (tp != NONE) { m.Tick(3); if (!open[tp]) continue; } else { if (ti == NONE) continue; U r2; RegionOf(ti, r2, place, m); }
        m.Tick(4 + 4 + 4 * 2 + 2); fe[cnt] = e; fdoor[cnt] = tp; fpl[cnt] = place; ff[cnt] = W(rt.origin[e]) + f.g.edges[e].w; ++cnt; }
      m.Tick(3); };
    // the least over the held reaches of Origin + reach time + To(p) from where the reach lands (NOTIME: p is out of reach); which reach
    auto lead = [&](U p, W blk, U& which) -> W { W v = NOTIME; m.Tick(3 * 2 + 2); const U none = TNone(p), ew = Ew(p), tbp = tb[p];
      for (U c = 0; c < cnt; ++c) { m.Tick(3 + 2 + 3 + 3); U t = 0;
        if (fdoor[c] != NONE) { if (fdoor[c] != p) continue; }                                                /* a reach straight into this door area */
        else { m.Tick(2 + 3); t = U(Runtime::Bits(fields, blk + W(fpl[c]) * ew, tbp, m)); if (t == none) continue; }
        m.Tick(3 + 3); if (ff[c] + t < v) { m.Tick(2 * 2); v = ff[c] + t; which = c; } }
      m.Tick(3); return v; };
    m.Tick(3);
    if (sk != NONE) {
      // ---- a source in a pocket, or on a mouth of one: L(j) = the least time to stand on mouth j of that pocket
      m.Tick(4 * 2); const U mk = pmN[sk]; const U* mo = mth.data() + pm0[sk]; const U* pt = pT.data() + pOff[sk]; const U out = pB[sk];
      for (U j = 0; j < mk; ++j) { m.Tick(3 + 2 + 2); gs.lv[j] = NOTIME; } m.Tick(3 + 3);
      if (si != NONE) { reaches();
        for (U i = 0; i < mk; ++i) { m.Tick(3 + 2 + 2 + 3); const U p = mo[i]; if (!open[p]) continue; const W blk = Block(p, sr, m); U which = 0; const W ev = lead(p, blk, which); m.Tick(3); if (ev == NOTIME) continue;
          for (U j = 0; j < mk; ++j) { m.Tick(3 + 2 + 2 + 3); const U t = pt[size_t(i) * mk + j]; if (t == BIG) continue; m.Tick(3 + 3); if (ev + t < gs.lv[j]) { m.Tick(3 * 2); gs.lv[j] = ev + t; gs.li[j] = i; gs.lc[j] = which; } }
          m.Tick(3); }
        m.Tick(3); }
      else { m.Tick(4); const U i0 = mix[sp]; for (U j = 0; j < mk; ++j) { m.Tick(3 + 2 + 2 + 3); const U t = pt[size_t(i0) * mk + j]; if (t == BIG) continue; m.Tick(2 * 2); gs.lv[j] = t; gs.li[j] = i0; } m.Tick(3); }
      // from mouth j: straight to the goal (the goal in this pocket or on one of its mouths: xq; the goal in this pocket's outside: From over it)
      const bool same = gk == sk, outside = gk == NONE && gp == NONE && gr == out; m.Tick(2 * 3 + 3 * 3);
      for (U j = 0; j < mk; ++j) { m.Tick(3 + 2 + 3 + 3 + 3); if (gs.lv[j] == NOTIME) continue; U t = BIG;
        if (same) { m.Tick(2); t = gs.xq[j]; } else if (outside) { const U p = mo[j]; const W blk = Block(p, gr, m); m.Tick(2 + 3 + 2 + 3); const U v = U(Runtime::Bits(fields, blk + W(gpl) * Ew(p) + tb[p] + rb, tb[p], m)); if (v != TNone(p)) t = v; }
        m.Tick(3 + 3); if (t != BIG && gs.lv[j] + t < best) { m.Tick(5 * 2); best = gs.lv[j] + t; kind = 3; sj = j; xkind = 0; xa = NONE; } }
      m.Tick(3);
      // from mouth j: one table step to a mouth of the goal's pocket, when that is another pocket beside the same outside
      m.Tick(3 + 3 + 4 + 3); if (gk != NONE && !same && pB[gk] == out) { m.Tick(2 * 2); const U gmk = pmN[gk]; const U* gmo = mth.data() + pm0[gk];
        for (U j = 0; j < mk; ++j) { m.Tick(3 + 2 + 3); if (gs.lv[j] == NOTIME) continue;
          for (U h = 0; h < gmk; ++h) { m.Tick(3 + 2 + 3); if (gs.mr[h] == BIG) continue; m.Tick(2 * 2 + 2 + Fld(4) + 3); const U t = M0T(mo[j], gmo[h]); if (t == BIG) continue; m.Tick(3 + 3); const W v = gs.lv[j] + t + gs.mr[h]; if (v < best) { m.Tick(5 * 2); best = v; kind = 3; sj = j; xkind = 1; xa = h; } }
          m.Tick(3); }
        m.Tick(3); }
      // from mouth j: one table step to a through door beside the outside, and on through the outer world. A through
      // door that cannot give a time below the best so far is left out (its step plus the least time any through
      // door needs on to the goal).
      m.Tick(3); if (gs.count) { U b0, b1; Thru(out, b0, b1, m);
        // First, once for all mouths: which of the through doors can still win. A chain over through door p takes at
        // least (the least time to stand on any mouth) + (the least baked step from any mouth to p, wOut) + (a lower
        // bound of p's time on to the goal). Only those below the best so far are kept, in the list's order.
        W lmin = NOTIME; m.Tick(2); for (U j = 0; j < mk; ++j) { m.Tick(3 + 2 + 3); if (gs.lv[j] < lmin) { m.Tick(2); lmin = gs.lv[j]; } } m.Tick(3);
        U nx = 0; m.Tick(2 + 3);
        if (lmin != NOTIME) { for (U i = b0; i < b1; ++i) { m.Tick(3 + 2 + 2 + Fld(2) + 3); const U p = U(Rd(thru + 2 * size_t(i), 2)); if (!open[p]) continue;
            m.Tick(4 + 2 + 2 + 3); const U tp = tix[p], w = wOut[size_t(sk) * T + tp]; if (w == BIG) continue;
            m.Tick(3 + 3); if (lmin + w + gs.least >= best) continue;
            const W lb = RestBound(gs, tp, m); m.Tick(3 + 3 + 3); if (lb >= BIG || lmin + w + lb >= best) continue;
            m.Tick(3 * 2); gs.xp[nx] = p; gs.xlb[nx] = lb; ++nx; }
          m.Tick(3); }
        // Then mouth by mouth over those that are kept, as before; the leave-out test has the sharper bound.
        m.Tick(3); if (nx) { for (U j = 0; j < mk; ++j) { m.Tick(3 + 2 + 3); if (gs.lv[j] == NOTIME) continue;
            for (U i = 0; i < nx; ++i) { m.Tick(3 + 2 + 2); const U p = gs.xp[i]; m.Tick(2 + 2 + Fld(4) + 3); const U t = M0T(mo[j], p); if (t == BIG) continue;
              m.Tick(3 + 3); if (gs.lv[j] + t + gs.xlb[i] >= best) continue; m.Tick(4); U by; const U rest = Rest(gs, tix[p], by, m); m.Tick(3 + 3 + 3); if (rest == BIG) continue; const W v = gs.lv[j] + t + rest; if (v < best) { m.Tick(6 * 2); best = v; kind = 3; sj = j; xkind = 2; xa = tix[p]; bestC = by; } }
            m.Tick(3); }
          m.Tick(3); } }
      m.Tick(3); if (kind == 3) { m.Tick(3 + 2 * 2 + 4); bestP = mo[gs.li[sj]]; if (si != NONE) bestE = fe[gs.lc[sj]]; }
    } else if (si != NONE) {
      // ---- a source in a region that is no pocket
      reaches();
      // straight to a mouth of the goal's pocket, when the source is in that pocket's outside
      m.Tick(3 + 4 + 3); if (gk != NONE && pB[gk] == sr) { m.Tick(2 * 2); const U gmk = pmN[gk]; const U* gmo = mth.data() + pm0[gk];
        for (U i = 0; i < gmk; ++i) { m.Tick(3 + 2 + 3); if (gs.mr[i] == BIG) continue; m.Tick(2); const U p = gmo[i]; const W blk = Block(p, sr, m); U which = 0; const W ev = lead(p, blk, which); m.Tick(3 + 3 + 3); if (ev == NOTIME) continue;
          if (ev + gs.mr[i] < best) { m.Tick(5 * 2); best = ev + gs.mr[i]; kind = 2; gi2 = i; bestP = p; bestE = fe[which]; } }
        m.Tick(3); }
      // the open through doors beside the source's region that the source reaches, each with To's time from the source's own area
      m.Tick(3); if (gs.count) { U b0, b1; Thru(sr, b0, b1, m); U np = 0, nearest = BIG, first = 0; m.Tick(3 * 2);
        for (U k = b0; k < b1; ++k) { m.Tick(3 + 2 + 2 + Fld(2) + 3); const U p = U(Rd(thru + 2 * size_t(k), 2)); if (!open[p]) continue;
          const W blk = Block(p, sr, m); m.Tick(3 + 2 + 3); const U t = U(Runtime::Bits(fields, blk + W(spl) * Ew(p), tb[p], m)); if (t == TNone(p)) continue;
          m.Tick(3 * 2 + 3 + 2); gs.sp[np] = p; gs.st[np] = t; gs.sb[np] = blk; if (t < nearest) { m.Tick(2 * 2); nearest = t; first = np; } ++np; }
        m.Tick(3 + 3);
        if (np != 0) {
          // the bound: no door route is faster than the nearest of them plus the least time on to the goal. (Both ends
          // in one region that is no pocket: the goal's side lists From's times, as built.)
          m.Tick(3 + 4); if (direct && 4 * a.r.time <= 5 * (W(nearest) + gs.least + 1) + 684) { a.bound = W(nearest) + gs.least + 1; lists(2); return a; }
          // through door by through door, the nearest first. One that cannot give a time below the best so far is left
          // out: a route through it takes at least its To time at the source plus the least time on to the goal, and
          // at least its To time plus its own least time on to the goal. (Origin is not below 0.)
          for (U turn = 0; turn <= np; ++turn) { m.Tick(3 + 2 + 3 + 3); const U i = turn == 0 ? first : turn - 1; if (turn != 0 && i == first) continue;
            m.Tick(3 + 3); if (W(gs.st[i]) + gs.least >= best) continue;
            const U p = gs.sp[i]; m.Tick(4); const U tp = tix[p]; { const W lb = RestBound(gs, tp, m); m.Tick(3 + 3 + 3); if (lb >= BIG || W(gs.st[i]) + lb >= best) continue; }      /* the sharper lower bound of its time on to the goal, before that time is worked out */
            U by; const U rest = Rest(gs, tp, by, m); m.Tick(3 + 3 + 3); if (rest == BIG || W(gs.st[i]) + rest >= best) continue;
            U which = 0; const W ev = lead(p, gs.sb[i], which); m.Tick(3 + 3 + 3); if (ev == NOTIME) continue; if (ev + rest < best) { m.Tick(5 * 2 + 4); best = ev + rest; kind = 1; bestP = p; bestC = by; bestE = fe[which]; } }
          m.Tick(3); } }
    } else { m.Tick(4); U by; const U rest = Rest(gs, tix[sp], by, m); m.Tick(3 + 4 * 2); if (rest != BIG) { best = rest; kind = 1; bestP = sp; bestC = by; } }      // a source that is an open through door starts in the outer world
    m.Tick(3); if (kind == 0) { m.Tick(3); if (direct) lists(1); return a; }
    if (si != NONE) a.exact = best + 1;
    m.Tick(3 + 4); if (direct && 4 * a.r.time <= 5 * (best + 1) + 684) { lists(3); return a; }
    // 3. the door route: its segments, exact up to its last door area, then that area's exit reach and the lists' own route
    m.Tick(3 * 2 + 2 + 3 * 3); a.door = true; a.toDoor = bestP; a.r = Route();
    auto goalSeg = [&](U in, U& sFirst, U& sLast) { m.Tick(3 * 2 + 2 * 2); const U* gmo = mth.data() + pm0[gk]; sFirst = gmo[in]; sLast = gmo[gs.mx[in]]; };      // the segment in the goal's pocket that begins on its mouth `in`
    if (kind == 1) { m.Tick(2 + 2 + 3); a.segEnd = thr[gs.q[bestC]]; if (gk != NONE) goalSeg(gs.qin[bestC], a.n1First, a.n1Last); }
    else if (kind == 2) { U sameFirst; goalSeg(gi2, sameFirst, a.segEnd); }
    else { m.Tick(2 * 2 + 3 + 3); a.segEnd = mth[pm0[sk] + sj];
      if (xkind == 1) goalSeg(xa, a.n1First, a.n1Last);
      else if (xkind == 2) { m.Tick(2 * 2 + 2 + 3); a.n1First = thr[xa]; a.n1Last = thr[gs.q[bestC]]; if (gk != NONE) goalSeg(gs.qin[bestC], a.n2First, a.n2Last); } }
    m.Tick(2 * 3 + 2); const U q = a.n2Last != NONE ? a.n2Last : a.n1Last != NONE ? a.n1Last : a.segEnd; a.exitDoor = q;
    m.Tick(2 + 3); a.segT = SegTime(a.toDoor, a.segEnd, m);                                                    // each segment's table time under these doors, for the walker
    if (a.n1First != NONE) { m.Tick(2 + 3); a.n1T = SegTime(a.n1First, a.n1Last, m); if (a.n2First != NONE) { m.Tick(2); a.n2T = SegTime(a.n2First, a.n2Last, m); } }
    W plain = best; m.Tick(3); if (si != NONE) { m.Tick(4 + 2); plain -= rt.origin[bestE]; }                   // the exact plain time of the whole route
    U exitE = NONE; m.Tick(3); if (gp == NONE) {                                                              // the goal in a region: From's time is replaced by the exit reach and the lists' own route
      const W blk = Block(q, gr, m); Need(blk != NOPOS, "the last door area of a door route is not beside the goal's region"); m.Tick(3 + 2 + 3 + 4 + 4 + 4 + 2 * 2); const W tx = Runtime::Bits(fields, blk + W(gpl) * Ew(q) + tb[q] + rb, tb[q] + xb[q], m); Need(U(tx & TNone(q)) != TNone(q), "the last door area of a door route does not reach the goal");
      U leg = NONE; m.Tick(3); if (L) leg = LegFind(q, gi, m);                 // a map with no patch at all pays this one test
      m.Tick(3); if (leg == NONE) { exitE = f.g.out[da[q]] + U(tx >> tb[q]); plain = plain - U(tx & TNone(q)) + f.g.edges[exitE].w; a.tailArea = rt.index[f.g.edges[exitE].to]; }
      else { m.Tick(2 + 3 * Fld(4) + Fld(1) + 4 + 4 * 2);                       // the patched last leg: the record's address; where its reaches start, their time, their end; the first reach's position and its reach; four copies
        exitE = f.g.out[da[q]] + legPath[LegOff(leg)]; plain = plain - U(tx & TNone(q)) + LegTime(leg); a.tailArea = LegEnd(leg); a.legRec = leg; }
      m.Tick(3); if (a.tailArea != gi) { a.r = rt.Ask(a.tailArea, gi, m); Need(a.r.found, "the lists have no route for a door route's last leg"); m.Tick(4); plain += a.r.graph; } }
    m.Tick(3); if (si == NONE) {                                              // from a door area: the first reach is the one the walker will take there
      m.Tick(3 + 3); if (sp != a.segEnd) { const U h = SegHop(sp, a.segEnd, m); m.Tick(3 + 2 + Fld(1) + 4); Need(h != NONE, "a table has a time and no next door area"); bestE = f.g.out[sArea] + M0R(sp, h); }
      else if (a.n1First != NONE) { m.Tick(2 + Fld(1) + 4); bestE = f.g.out[sArea] + M0R(sp, a.n1First); }
      else { m.Tick(2); Need(exitE != NONE, "a door route from a door area to itself"); bestE = exitE; } }
    if (si == NONE) a.exact = W(rt.origin[bestE]) + best + 1;
    m.Tick(8); a.first = bestE; a.time = W(rt.origin[bestE]) + plain + 1; a.found = true; a.how = 4; return a;
  }
};

} // namespace nl
