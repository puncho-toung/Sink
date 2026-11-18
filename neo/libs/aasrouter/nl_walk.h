#pragma once
// exp-active2 (director, 2026-11-13): a route taken a few reaches at a time, with a state that can be saved.
//
// Runtime::Write (nl_ask.h) writes a route from its start. A game needs the NEXT reaches from where a walker
// stands, tic after tic, and must be able to save the walker and load it again. This file adds that on top of
// nl_ask.h, which it does not change:
//   Begin(rt, route, s, g)          the walker's state right after an ask
//   Next(rt, walk, buf, want, out)  the next `want` reaches (fewer only when the walker arrives)
//   Save / Load                     the state as WALK_BYTES bytes; Load refuses values that are out of range and
//                                   marks the buffer as not filled
// The reaches it hands out are exactly the route's reaches as Write gives them (nl_walk_driver.cpp checks that).
//
// What is saved is the state alone. A backward piece (read from its end toward the walker, then walked) needs a
// buffer; the buffer is not saved: after a load it is read again from the piece's end back to where the walker
// stands. The buffer remembers which piece it holds and where it has got to, so several walkers may share one
// (it is then read again whenever another walker used it in between).
// A saved game is not trusted: Load refuses a state that is out of range or that does not stand on its own piece,
// and Next stops with a refusal, never with a read outside an area's reaches, on anything else.
// Every statement group is charged to a Meter with the unit prices of COUNTING.md.
// Version 4 of the packed file (2026-11-14): the same walker as ../nl_walk.h; records are bit fields found by a
// bit position (nl_ask.h), and "no reach" is the all-ones value of its field, tested by name before it is used.
#include "nl_ask.h"
namespace nl {

struct Walk {                           // a walker's whole state
  U goal = NONE, at = NONE;             // enabled indexes: where it is going, where it stands
  U first = NONE;                       // the first patched reach while it is still to be taken, else NONE
  U prefixLeft = 0;                     // patched reaches still to take
  U level = NONE, y = NONE;             // the base route's level and target waypoint; level NONE: the route has no base part
  U phase = 4;                          // 0 patched prefix; 1 forward to waypoint wp; 2 backward piece (the In lists of waypoint wp,
                                        // toward area target); 3 a stored path that left waypoint wp; 4 arrived
  U wp = NONE, target = NONE;           // see phase
  U after = NONE;                       // phases 2 and 3: the waypoint the walker stands on when the piece ends; NONE: the goal
  U longIndex = 0, pathDone = 0;        // phase 3: the stored path and the reaches already taken on it
};
struct WalkBuf { std::vector<U> reach; U count = 0; bool valid = false; U level = NONE, wp = NONE, target = NONE; };     // the rest of ONE backward piece (level, waypoint, end area), last reach first; never saved
constexpr U WALK_BYTES = 48;

// A reach from its position among an area's reaches, refused when the position is outside them.
inline U OutAt(const Runtime& rt, U area, U ord) { Need(ord < rt.f.g.out[rt.active[area] + 1] - rt.f.g.out[rt.active[area]], "walker: reach position outside its area"); return rt.OutReach(area, ord); }
inline U InAt(const Runtime& rt, U area, U ord) { Need(ord < rt.f.g.in[rt.active[area] + 1] - rt.f.g.in[rt.active[area]], "walker: reach position outside its area"); return rt.InReach(area, ord); }

inline void Save(const Walk& w, uint8_t* out) { const U fields[12] = {w.goal, w.at, w.first, w.prefixLeft, w.level, w.y, w.phase, w.wp, w.target, w.after, w.longIndex, w.pathDone};
  for (U i = 0; i < 12; ++i) for (U k = 0; k < 4; ++k) out[4 * i + k] = uint8_t(fields[i] >> (8 * k)); }
inline Walk Load(const Runtime& rt, const uint8_t* in, WalkBuf& b) { b.valid = false; b.count = 0; U x[12]; for (U i = 0; i < 12; ++i) x[i] = U(Rd(in + 4 * i, 4));
  Walk w; w.goal = x[0]; w.at = x[1]; w.first = x[2]; w.prefixLeft = x[3]; w.level = x[4]; w.y = x[5]; w.phase = x[6]; w.wp = x[7]; w.target = x[8]; w.after = x[9]; w.longIndex = x[10]; w.pathDone = x[11];
  Need(w.goal < rt.n && w.at < rt.n && w.phase <= 4 && w.prefixLeft <= rt.maxPrefix && (w.first == NONE || w.first < rt.f.g.edges.size()), "saved walker: goal, area, phase or prefix");
  if (w.first != NONE) { const auto& e = rt.f.g.edges[w.first]; Need(w.phase == 0 && w.prefixLeft > 0 && e.from == rt.active[w.at] && rt.index[e.to] != NONE, "saved walker: first reach"); }
  if (w.level == NONE) { Need(w.phase == 0 || w.phase == 4, "saved walker: a base phase without a base route"); return w; }
  Need(w.level < rt.NL, "saved walker: level"); const View& v = rt.lv[w.level]; Need(w.y < v.m, "saved walker: target waypoint");
  if (w.phase == 0 || w.phase == 1) Need(w.wp < v.m, "saved walker: waypoint");
  if (w.phase == 2) { Need(w.wp < v.m && w.target < rt.n && (w.after == NONE || w.after < v.m), "saved walker: backward piece");
    Need(w.after == NONE ? (w.target == w.goal && w.wp == w.y) : w.target == rt.WpArea(v, w.after), "saved walker: its backward piece ends somewhere else");
    const U ya = rt.WpArea(v, w.wp); U p = w.target, steps = 0;                         // the walker must stand ON the piece: read it back from its end
    while (p != w.at) { Need(p != ya && steps++ <= rt.maxBack, "saved walker: not on its backward piece"); const W q = rt.FindInRaw(v, p, w.wp);
      Need(q != NOPOS && rt.InOrd(v, q) != v.ioNone, "saved walker: not on its backward piece"); p = rt.index[rt.f.g.edges[InAt(rt, p, rt.InOrd(v, q))].from]; } }
  if (w.phase == 3) { Need(w.longIndex < v.longCount && w.after < v.m && w.wp < v.m, "saved walker: stored path");
    bool hop = false; const U h0 = U(Rd(v.hopIndex + 4 * size_t(w.wp), 4)), h1 = U(Rd(v.hopIndex + 4 * size_t(w.wp) + 4, 4));          // the path must be the one of a hop of wp to `after`
    for (U h = h0; h < h1; ++h) { const W hp = rt.HopPos(v, h); hop = hop || (rt.HopKind(v, hp) == 2 && rt.HopZ(v, hp) == w.after && rt.HopLong(v, hp) == w.longIndex); }
    Need(hop, "saved walker: its stored path is not a hop of its waypoint");
    const uint8_t* lp = v.lng + 9 * size_t(w.longIndex); const uint8_t* path = v.paths + Rd(lp, 4); Need(w.pathDone <= U(Rd(lp + 4, 2)), "saved walker: stored path position");
    U at = rt.WpArea(v, w.wp); for (U i = 0; i < w.pathDone; ++i) at = rt.index[rt.f.g.edges[OutAt(rt, at, path[i])].to];      // the pack's load checks walked this path already
    Need(at == w.at, "saved walker: not where its stored path has reached"); }
  return w; }

// The walker's state right after Ask(s, g) returned `r` (r.found must be true).
inline Walk Begin(const Runtime& rt, const Route& r, U s, U g, Meter& m) {
  Walk w; m.Tick(3 + 2 * 8); Need(r.found && s < rt.n && g < rt.n, "walker: no route to begin");
  w.goal = g; w.at = s; w.prefixLeft = r.prefixCount; w.first = r.prefixCount ? r.prefix[0] : NONE;
  if (r.baseArea != NONE) { w.level = r.level; w.y = r.y; w.wp = r.x; }
  w.phase = r.prefixCount ? 0 : (r.baseArea != NONE ? 1 : 4); return w; }

// The next `want` reaches, written to `out`; returns how many (fewer than `want` only when the walker arrives).
// `b` is scratch for one backward piece (b.reach sized to the pack's longest backward piece).
inline U Next(const Runtime& rt, Walk& w, WalkBuf& b, U want, U* out, Meter& m) {
  U count = 0; m.Tick(6);
  auto emit = [&](U e) { m.Tick(3 + 4 + 4 + 10); Need(e < rt.f.g.edges.size() && rt.f.g.edges[e].from == rt.active[w.at], "walker: a reach that does not leave its area"); out[count++] = e; w.at = rt.index[rt.f.g.edges[e].to]; };
  auto arrive = [&](U cur) {             // the walker stands on waypoint `cur` of the base route's level: choose the next piece
    const View& v = rt.lv[w.level]; m.Tick(3 + Fld(3) + 3); Need(w.at == rt.WpArea(v, cur), "walker: not on the waypoint it arrives at");
    if (cur == w.y) { m.Tick(2 * 4); w.phase = 2; w.wp = cur; w.target = w.goal; w.after = NONE; return; }      /* the last piece: from waypoint y to the goal */
    U c0, slots; rt.Column(v, w.y, c0, slots, m); U h; const W e = rt.Entry(v, c0, slots, cur, h, m); Need(e != NOPOS, "walker: a hop chain without its table entry");
    W hp; U z, kind; rt.EntryHop(v, e, cur, hp, z, kind, m);
    if (kind == 0) { m.Tick(2 * 2); w.phase = 1; w.wp = z; }
    else if (kind == 1) { m.Tick(3 + Fld(3) + 2 * 4); w.phase = 2; w.wp = cur; w.target = rt.WpArea(v, z); w.after = z; }
    else { m.Tick(3 + 2 * 5); w.phase = 3; w.wp = cur; w.longIndex = U(Runtime::Bits(v.hops, hp + v.wb + 2, v.lb, m)); w.pathDone = 0; w.after = z; } };
  for (;;) { m.Tick(3 + 3); if (count == want || w.phase == 4) break;
    m.Tick(3); if (w.phase == 0) { m.Tick(3);
      if (w.prefixLeft == 0) { m.Tick(3 + 2); w.phase = w.level == NONE ? 4 : 1; continue; }
      U e; m.Tick(3); if (w.first != NONE) { m.Tick(2 + 2); e = w.first; w.first = NONE; } else { e = rt.PatchFind(rt.contIndex, rt.cont, w.goal, w.at, m); m.Tick(3); Need(e != NONE, "walker: the patched prefix ends early"); }
      m.Tick(2); --w.prefixLeft; emit(e); continue; }
    Need(w.level < rt.NL, "walker: a base phase without a base route"); const View& v = rt.lv[w.level]; m.Tick(3 + 3);
    m.Tick(3); if (w.phase == 1) { m.Tick(Fld(3) + 2 + 3); if (w.at == rt.WpArea(v, w.wp)) { arrive(w.wp); continue; }
      const W q = rt.FindOut(v, w.at, w.wp, m); Need(q != NOPOS, "walker: a forward piece loses its waypoint"); m.Tick(3 + 3 + 4 + 7);
      const U ord = U(Runtime::Bits(v.out, q + v.wb + v.tb, v.oob, m)); Need(ord != v.ooNone, "walker: a forward piece loses its waypoint"); emit(OutAt(rt, w.at, ord)); continue; }
    m.Tick(3); if (w.phase == 2) { m.Tick(3); if (w.at == w.target) { m.Tick(3 + 2); if (w.after == NONE) w.phase = 4; else arrive(w.after); continue; }
      m.Tick(3 + 3 * 3 + 3 + 4 + 3);              /* is the buffer filled, for this piece, with a next reach that leaves the walker's area */
      if (!(b.valid && b.level == w.level && b.wp == w.wp && b.target == w.target && b.count > 0 && rt.index[rt.f.g.edges[b.reach[b.count - 1]].from] == w.at)) {
        U p = w.target; b.count = 0; b.level = w.level; b.wp = w.wp; b.target = w.target; m.Tick(4 + 6 + Fld(3) + 2); const U ya = rt.WpArea(v, w.wp);      /* read the rest of the piece backward, from its end to where the walker stands */
        for (;;) { m.Tick(3); if (p == w.at) break; m.Tick(3); Need(p != ya, "walker: not on its backward piece"); const W q = rt.FindIn(v, p, w.wp, m); Need(q != NOPOS, "walker: a backward piece loses its waypoint"); m.Tick(4 + 3 + 4 + 4 + 3 + 7);
          const U ord = U(Runtime::Bits(v.in, q + v.wb + v.tb, v.iob, m)); Need(ord != v.ioNone, "walker: a backward piece loses its waypoint");
          Need(b.count < b.reach.size(), "walker: backward piece longer than the bake's bound"); const U e = InAt(rt, p, ord); b.reach[b.count++] = e; p = rt.index[rt.f.g.edges[e].from]; }
        m.Tick(2); b.valid = true; }
      m.Tick(3 + 2); Need(b.count > 0, "walker: a backward piece without a next reach"); emit(b.reach[--b.count]); continue; }
    { m.Tick(2 + Fld(4) + Fld(2) + 3); const uint8_t* lp = v.lng + 9 * size_t(w.longIndex); const uint8_t* path = v.paths + Rd(lp, 4); const U len = U(Rd(lp + 4, 2));      /* phase 3: a stored path */
      if (w.pathDone == len) { arrive(w.after); continue; }
      m.Tick(Fld(1) + 4 + 2 + 7); emit(OutAt(rt, w.at, path[w.pathDone++])); } }
  m.Tick(3 + 3); if (w.phase == 4) Need(w.at == w.goal, "walker: arrived somewhere else than the goal");
  return count; }

} // namespace nl
