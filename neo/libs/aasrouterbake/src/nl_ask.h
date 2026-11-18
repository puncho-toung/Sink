#pragma once
// exp-active2, part W3 on version 4 of the packed file (director, 2026-11-14): the game-side ask.
// The same ask as ../nl_ask.h, statement for statement; what changed is how a record is read: the list records,
// the hop records and the table slots are bit fields (nl_pack.h), found by a bit position instead of a pointer.
// It reads only the packed bytes and the map's reaches (target area, time, Origin).
// Every statement group is charged to a Meter; the charges are listed in COUNTING.md. A read of a bit field is
// charged by the bytes it lies in, where it lies (Bits below); fields that sit side by side and are needed
// together are taken with one read.
// Since 2026-11-15 a bit field is fetched with one 8-byte load (RdBits8 in nl_pack.h) in place of a byte loop.
// The value, the charge and the bytes the field lies in are the same; only the fetch is faster. The load can take
// up to 7 bytes after a section's end, so the constructor refuses a file in memory that lacks its spare bytes.
// Byte fields (Rd) are read as before: Rd is also used on buffers that have no spare bytes (a saved walker).
// Since 2026-11-17 the table probe of Ask takes its two remainders by multiplication (ModBy below) in place of the
// machine's division. The slot, the charge and the answer are the same; the load still proves every table entry
// with the division (EntryRaw). The constant of each column is worked out once at load (colMagic).
// Doors: loop doors stay as baked (closed). Forced doors may close: the bake makes every route pass only the
// doors its pair needs, so an ask is to be made only for a pair that has a route under the current doors (the
// door labels answer that first); `closed` marks the closed areas and is read for start patches only.
// Write takes only a route that Ask has just returned: it does not check the route again.
#include "nl_pack.h"
#if defined(_MSC_VER)
#include <intrin.h>                                    // __umulh
#endif
namespace nl {

struct Meter { W steps = 0; void Tick(W k) { steps += k; } };
inline W Fld(U bytes) { return 2 * W(bytes) + 3; }     // reading a field of that many bytes and keeping it
constexpr W NOPOS = ~W(0);                             // "no such record"
// An exact a % d for 32-bit a and d >= 1 by two multiplications (Lemire, Kaser, Kurz: "Faster remainder by direct
// computation", 2019): M = ModMagic(d) is the ceiling of 2^64 / d (it wraps to 0 for d = 1, and ModBy then gives 0).
#if defined(_MSC_VER)
inline W MulHigh(W a, W b) { return __umulh(a, b); }                                    // the high 64 bits of the 128-bit product
#else
inline W MulHigh(W a, W b) { return W((static_cast<unsigned __int128>(a) * b) >> 64); }
#endif
inline W ModMagic(U d) { return ~W(0) / d + 1; }
inline U ModBy(U a, W M, U d) { return U(MulHigh(M * a, d)); }

struct View { U cap = 0, K = 0, range = 0, m = 0;
              U wb = 0, tb = 0, oob = 0, iob = 0, farb = 0, hb = 0, hopb = 0, db = 0, lb = 0;      // field widths in bits (nl_pack.h)
              U outBits = 0, inBits = 0, hopBits = 0, slotBits = 0;                              // record widths in bits
              U pad = 0, ooNone = 0, ioNone = 0, farNone = 0;                                    // the all-ones values
              U hopCount = 0, entries = 0, longCount = 0, pathBytes = 0;                         // entries: table slots
              const uint8_t *wpArea = nullptr, *out = nullptr, *in = nullptr, *hopIndex = nullptr, *hops = nullptr, *colIndex = nullptr, *table = nullptr, *lng = nullptr, *paths = nullptr; };

#ifndef NL_PREFIX_CAP
#define NL_PREFIX_CAP 48                            // the most patched reaches a route may begin with (Route::prefix). 48 by the user's
#endif                                               // ruling of 2026-11-16 (24 before). The stitched maps need 23, the outdoor walk maps up
                                                     // to 29, the outdoor fly maps 36 to 38. A file whose bake needed more is refused at load.
struct Route {                                          // the answer to one ask
  bool found = false; W time = 0, graph = 0; U first = NONE;   // time = origin[first reach] + sum of reach times + 1
  U prefixCount = 0; U prefix[NL_PREFIX_CAP];                      // the patched reaches: start patch, then continuation patches
  U baseArea = NONE, level = NONE, x = NONE, y = NONE;  // then the base route from baseArea; NONE when the prefix ends on the goal
};

struct Runtime {
  const Fixture4& f; const std::vector<U>& active; const std::vector<U>& index; const std::vector<U>& origin;
  U n = 0, NL = 0, maxPrefix = 0, maxBack = 0, startCount = 0, contCount = 0; View lv[4];
  const uint8_t *startIndex = nullptr, *start = nullptr, *contIndex = nullptr, *cont = nullptr;
  const uint8_t* closed = nullptr;       // per area (graph number): 1 = closed now; null = none closed
  std::vector<W> colMagic[4];            // per level and target waypoint: ModMagic of its column's slot count (0: an empty column). Made at load.

  // ---- raw record access (no charge here; callers charge). A record is named by the bit position where it starts.
  W OutPos(const View& v, U area, U i) const { return (W(area) * v.K + i) * v.outBits; }
  W InPos(const View& v, U area, U i) const { return (W(area) * v.K + i) * v.inBits; }
  W SlotPos(const View& v, W slot) const { return slot * v.slotBits; }
  W HopPos(const View& v, W hop) const { return hop * v.hopBits; }
  U OutW(const View& v, W p) const { return U(RdBits8(v.out, p, v.wb)); }
  U OutT(const View& v, W p) const { return U(RdBits8(v.out, p + v.wb, v.tb)); }
  U OutOrd(const View& v, W p) const { return U(RdBits8(v.out, p + v.wb + v.tb, v.oob)); }
  U InW(const View& v, W p) const { return U(RdBits8(v.in, p, v.wb)); }
  U InT(const View& v, W p) const { return U(RdBits8(v.in, p + v.wb, v.tb)); }
  U InOrd(const View& v, W p) const { return U(RdBits8(v.in, p + v.wb + v.tb, v.iob)); }
  U InFar(const View& v, W p) const { return U(RdBits8(v.in, p + v.wb + v.tb + v.iob, v.farb)); }
  U SlotX(const View& v, W p) const { return U(RdBits8(v.table, p, v.wb)); }
  U SlotH(const View& v, W p) const { return U(RdBits8(v.table, p + v.wb, v.hb)); }
  U SlotHop(const View& v, W p) const { return U(RdBits8(v.table, p + v.wb + v.hb, v.hopb)); }
  U SlotD(const View& v, W p) const { return U(RdBits8(v.table, p + v.wb + v.hb + v.hopb, v.db)); }
  U HopZ(const View& v, W p) const { return U(RdBits8(v.hops, p, v.wb)); }
  U HopKind(const View& v, W p) const { return U(RdBits8(v.hops, p + v.wb, 2)); }
  U HopLong(const View& v, W p) const { return U(RdBits8(v.hops, p + v.wb + 2, v.lb)); }
  U HopFirst(const View& v, U x) const { return U(Rd(v.hopIndex + 4 * size_t(x), 4)); }          // the first hop of waypoint x
  U WpArea(const View& v, U w) const { return U(Rd(v.wpArea + 3 * size_t(w), 3)); }
  U OutReach(U area, U ord) const { return f.g.out[active[area]] + ord; }
  U InReach(U area, U ord) const { return f.g.rev[f.g.in[active[area]] + ord]; }
  W FindOutRaw(const View& v, U area, U x) const { for (U i = 0; i < v.K; ++i) { const W p = OutPos(v, area, i); if (OutW(v, p) == x) return p; } return NOPOS; }
  W FindInRaw(const View& v, U area, U y) const { for (U i = 0; i < v.K; ++i) { const W p = InPos(v, area, i); if (InW(v, p) == y) return p; } return NOPOS; }
  W EntryRaw(const View& v, U x, U y) const { const U c0 = U(Rd(v.colIndex + 4 * size_t(y), 4)), slots = U(Rd(v.colIndex + 4 * size_t(y) + 4, 4)) - c0; if (!slots) return NOPOS;
    const U dd = SlotD(v, SlotPos(v, W(c0) + Bucket(x, slots))); const W p = SlotPos(v, W(c0) + Slot(x, dd, slots)); return SlotX(v, p) == x ? p : NOPOS; }

  // ---- load: take the sections and refuse anything a walk could trip over
  Runtime(const Blob& b, const Fixture4& fx, const std::vector<U>& ac, const std::vector<U>& ix, const std::vector<U>& org) : f(fx), active(ac), index(ix), origin(org) {
    Need(b.HasSpare(), "a packed file in memory without the spare bytes after its sections (Blob::Load makes them)");
    const auto& meta = b.Get("meta"); Need(meta.size() >= 20 && Rd(meta.data(), 4) == 4, "packed version"); n = U(Rd(meta.data() + 4, 4)); NL = U(Rd(meta.data() + 8, 4)); maxPrefix = U(Rd(meta.data() + 12, 4)); maxBack = U(Rd(meta.data() + 16, 4));
    Need(n == active.size() && NL >= 1 && NL <= 4 && meta.size() == 20 + 52 * size_t(NL) && maxPrefix <= NL_PREFIX_CAP, "packed header does not fit this map");
    for (const auto& e : f.g.edges) Need(e.w >= 1, "a reach of time 0: the records' times could not tell an area from its waypoint");
    auto deg = [&](U area) { return f.g.out[active[area] + 1] - f.g.out[active[area]]; }; auto degIn = [&](U area) { return f.g.in[active[area] + 1] - f.g.in[active[area]]; };
    for (U lev = 0; lev < NL; ++lev) { View& v = lv[lev]; const uint8_t* q = meta.data() + 20 + 52 * size_t(lev);
      v.cap = U(Rd(q, 4)); v.K = U(Rd(q + 4, 4)); v.range = U(Rd(q + 8, 4)); v.m = U(Rd(q + 12, 4));
      v.wb = U(Rd(q + 16, 4)); v.tb = U(Rd(q + 20, 4)); v.oob = U(Rd(q + 24, 4)); v.iob = U(Rd(q + 28, 4)); v.farb = U(Rd(q + 32, 4)); v.hb = U(Rd(q + 36, 4)); v.hopb = U(Rd(q + 40, 4)); v.db = U(Rd(q + 44, 4)); v.lb = U(Rd(q + 48, 4));
      Need(v.K >= 1 && v.K <= 8 && v.wb >= 1 && v.wb <= 24 && v.tb >= 1 && v.tb <= 24 && v.oob >= 1 && v.oob <= 8 && v.iob >= 1 && v.iob <= 8 && v.farb >= 1 && v.farb <= 8
           && v.hb >= 1 && v.hb <= 24 && v.hopb >= 1 && v.hopb <= 8 && v.db >= 1 && v.db <= 8 && v.lb <= 24, "packed level header");
      v.outBits = v.wb + v.tb + v.oob; v.inBits = v.wb + v.tb + v.iob + v.farb; v.hopBits = v.wb + 2 + v.lb; v.slotBits = v.wb + v.hb + v.hopb + v.db;
      v.pad = (U(1) << v.wb) - 1; v.ooNone = (U(1) << v.oob) - 1; v.ioNone = (U(1) << v.iob) - 1; v.farNone = (U(1) << v.farb) - 1; Need(v.m <= v.pad, "waypoint count against its field");
      const auto &wa = b.Get(Sec(lev, "wpArea")), &o = b.Get(Sec(lev, "out")), &in = b.Get(Sec(lev, "in")), &hi = b.Get(Sec(lev, "hopIndex")), &hp = b.Get(Sec(lev, "hops")), &ci = b.Get(Sec(lev, "colIndex")), &tb = b.Get(Sec(lev, "table")), &lg = b.Get(Sec(lev, "long")), &ph = b.Get(Sec(lev, "paths"));
      v.lng = lg.data(); v.paths = ph.data(); v.longCount = U(lg.size() / 9); v.pathBytes = U(ph.size()); Need(lg.size() % 9 == 0 && (v.lb == 0) == (v.longCount == 0) && (v.lb == 0 || v.longCount < (U(1) << v.lb)), "long hop section size");
      Need(wa.size() == 3 * size_t(v.m) && hi.size() == 4 * (size_t(v.m) + 1) && ci.size() == 4 * (size_t(v.m) + 1), "packed section sizes");
      v.wpArea = wa.data(); v.out = o.data(); v.in = in.data(); v.hopIndex = hi.data(); v.hops = hp.data(); v.colIndex = ci.data(); v.table = tb.data();
      v.hopCount = U(Rd(v.hopIndex + 4 * size_t(v.m), 4)); v.entries = U(Rd(v.colIndex + 4 * size_t(v.m), 4));     // the indexes end on the counts; the sections must hold exactly that many records
      Need(o.size() == BitBytes(W(n) * v.K, v.outBits) && in.size() == BitBytes(W(n) * v.K, v.inBits) && hp.size() == BitBytes(v.hopCount, v.hopBits) && tb.size() == BitBytes(v.entries, v.slotBits), "packed section sizes");
      for (U w = 0; w < v.m; ++w) Need(WpArea(v, w) < n && (w == 0 || WpArea(v, w - 1) < WpArea(v, w)), "waypoint areas not ascending");
      // Out and In records: the reach belongs to the area, and the next (or previous) area keeps the waypoint with the remaining time
      for (U a = 0; a < n; ++a) { bool padOut = false, padIn = false;
        for (U i = 0; i < v.K; ++i) { const W p = OutPos(v, a, i); const U w = OutW(v, p), t = OutT(v, p), ord = OutOrd(v, p);
          if (w == v.pad) { padOut = true; continue; } Need(!padOut && w < v.m, "Out record waypoint"); for (U k = 0; k < i; ++k) Need(OutW(v, OutPos(v, a, k)) != w, "Out list names a waypoint twice");
          if (ord == v.ooNone) { Need(t == 0 && WpArea(v, w) == a, "Out self record"); continue; }
          Need(ord < deg(a), "Out reach position"); const auto& e = f.g.edges[OutReach(a, ord)]; const U to = index[e.to]; Need(to != NONE && t >= e.w, "Out reach target");
          if (to == WpArea(v, w)) Need(t == e.w, "Out record time at the waypoint"); else { const W r = FindOutRaw(v, to, w); Need(r != NOPOS && OutT(v, r) == t - e.w, "Out record does not continue"); } }
        for (U i = 0; i < v.K; ++i) { const W p = InPos(v, a, i); const U w = InW(v, p), t = InT(v, p), ord = InOrd(v, p), far = InFar(v, p);
          if (w == v.pad) { padIn = true; continue; } Need(!padIn && w < v.m, "In record waypoint"); for (U k = 0; k < i; ++k) Need(InW(v, InPos(v, a, k)) != w, "In list names a waypoint twice");
          if (ord == v.ioNone) { Need(t == 0 && far == v.farNone && WpArea(v, w) == a, "In self record"); continue; }
          Need(ord < degIn(a), "In reach position"); const U en = InReach(a, ord); const auto& e = f.g.edges[en]; const U from = index[e.from]; Need(from != NONE && t >= e.w && far != v.farNone && far < deg(WpArea(v, w)), "In reach source");
          if (from == WpArea(v, w)) Need(t == e.w && OutReach(from, far) == en, "In record at the waypoint"); else { const W r = FindInRaw(v, from, w); Need(r != NOPOS && InT(v, r) == t - e.w && InFar(v, r) == far, "In record does not continue"); } } }
      // hops and table entries
      Need(Rd(v.hopIndex, 4) == 0 && Rd(v.colIndex, 4) == 0, "packed index ends");
      for (U x = 0; x < v.m; ++x) { const U h0 = U(Rd(v.hopIndex + 4 * size_t(x), 4)), h1 = U(Rd(v.hopIndex + 4 * size_t(x) + 4, 4)); Need(h0 <= h1 && h1 <= v.hopCount && h1 - h0 < 255, "hop index");
        for (U h = h0; h < h1; ++h) { U z, kind, w; const U li = HopRaw(v, x, h - h0, z, kind, w); Need(w != BIG, "hop without its record"); if (kind != 2) continue;
          const uint8_t* lp = v.lng + 9 * size_t(li); const U off = U(Rd(lp, 4)), len = U(Rd(lp + 4, 2)); Need(len >= 1 && W(off) + len <= v.pathBytes, "long hop path range"); U at = WpArea(v, x); W sum = 0;   // the stored path leads from x to z in the stored time
          for (U i = 0; i < len; ++i) { const U ord = v.paths[off + i]; Need(ord < deg(at), "long hop reach position"); const auto& e = f.g.edges[OutReach(at, ord)]; Need(index[e.to] != NONE, "long hop reach target"); sum += e.w; at = index[e.to]; }
          Need(at == WpArea(v, z) && sum == w, "long hop path does not reach its waypoint in its time"); } }
      colMagic[lev].assign(v.m, 0); for (U y = 0; y < v.m; ++y) { const U c0 = U(Rd(v.colIndex + 4 * size_t(y), 4)), c1 = U(Rd(v.colIndex + 4 * size_t(y) + 4, 4)); Need(c0 <= c1 && c1 <= v.entries, "table index"); if (c1 > c0) colMagic[lev][y] = ModMagic(c1 - c0);
        for (U c = c0; c < c1; ++c) { const W p = SlotPos(v, c); const U x = SlotX(v, p), h = SlotH(v, p), hop = SlotHop(v, p);
          if (x == v.pad) { Need(h == 0 && hop == 0, "empty table slot"); continue; }
          Need(x < v.m && x != y && h < v.range && EntryRaw(v, x, y) == p, "table entry is not where its source is looked for");
          Need(hop < U(Rd(v.hopIndex + 4 * size_t(x) + 4, 4)) - U(Rd(v.hopIndex + 4 * size_t(x), 4)), "table entry hop number"); U z, kind, w; HopRaw(v, x, hop, z, kind, w);
          if (z == y) Need(h == w, "table entry time at the target"); else { const W r = EntryRaw(v, z, y); Need(r != NOPOS && SlotH(v, r) + w == h, "table entry does not continue"); } } }
    }
    const auto &si = b.Get("startIndex"), &sb = b.Get("start"), &ci = b.Get("contIndex"), &cb = b.Get("cont");
    Need(si.size() == 4 * (size_t(n) + 1) && ci.size() == si.size() && sb.size() % 4 == 0 && cb.size() % 4 == 0, "patch section sizes");
    startIndex = si.data(); start = sb.data(); contIndex = ci.data(); cont = cb.data(); startCount = U(sb.size() / 4); contCount = U(cb.size() / 4);
    for (int which = 0; which < 2; ++which) { const uint8_t* ix = which ? contIndex : startIndex; const uint8_t* body = which ? cont : start; const U count = which ? contCount : startCount;
      Need(Rd(ix, 4) == 0 && Rd(ix + 4 * size_t(n), 4) == count, "patch index ends");
      for (U g = 0; g < n; ++g) { const U p0 = U(Rd(ix + 4 * size_t(g), 4)), p1 = U(Rd(ix + 4 * size_t(g) + 4, 4)); Need(p0 <= p1 && p1 <= count, "patch index");
        for (U p = p0; p < p1; ++p) { const U area = U(Rd(body + 4 * size_t(p), 3)), ord = body[4 * size_t(p) + 3]; Need(area < n && area != g && (p == p0 || U(Rd(body + 4 * size_t(p) - 4, 3)) < area || (!which && U(Rd(body + 4 * size_t(p) - 4, 3)) == area)) && ord < deg(area) && index[f.g.edges[OutReach(area, ord)].to] != NONE, "patch record"); } } }
  }
  // hop number `hop` of waypoint x: its target, kind, and time (read from the Out or In record that carries it; BIG if absent)
  // Returns the stored path's number for kind 2, else NONE.
  U HopRaw(const View& v, U x, U hop, U& z, U& kind, U& w) const { const W p = HopPos(v, W(HopFirst(v, x)) + hop); z = HopZ(v, p); kind = HopKind(v, p); w = BIG;
    Need(z < v.m && z != x && kind <= 2, "hop record");
    if (kind == 2) { Need(v.longCount != 0, "long hop on a level without any"); const U li = HopLong(v, p); Need(li < v.longCount, "long hop number"); w = U(Rd(v.lng + 9 * size_t(li) + 6, 3)); return li; }
    const W r = kind == 0 ? FindOutRaw(v, WpArea(v, x), z) : FindInRaw(v, WpArea(v, z), x); if (r != NOPOS) w = kind == 0 ? OutT(v, r) : InT(v, r); return NONE; }

  // ---- charged helpers
  // A bit field read and charged by the bytes it lies in.
  static W Bits(const uint8_t* base, W pos, U width, Meter& m) { m.Tick(Fld(BytesAt(pos, width))); return RdBits8(base, pos, width); }
  U PatchLower(const uint8_t* ix, const uint8_t* body, U g, U area, U& end, Meter& m) const {   // first patch of the goal whose area is not below `area`
    m.Tick(2 + 2 * Fld(4)); U lo = U(Rd(ix + 4 * size_t(g), 4)); end = U(Rd(ix + 4 * size_t(g) + 4, 4)); U hi = end;
    while (lo < hi) { m.Tick(3 + 2 + 2 + Fld(3) + 3 + 2); const U mid = (lo + hi) / 2; if (U(Rd(body + 4 * size_t(mid), 3)) < area) lo = mid + 1; else hi = mid; }
    m.Tick(3); return lo; }
  U PatchFind(const uint8_t* ix, const uint8_t* body, U g, U area, Meter& m) const {     // the patched reach of (area, goal), or NONE
    U end; const U lo = PatchLower(ix, body, g, area, end, m); m.Tick(3); if (lo == end) return NONE;
    m.Tick(2 + Fld(3) + 3); const uint8_t* p = body + 4 * size_t(lo); if (U(Rd(p, 3)) != area) return NONE;
    m.Tick(Fld(1) + 4); return OutReach(area, p[3]); }
  void Column(const View& v, U y, U& c0, U& slots, Meter& m) const {                     // the column of target y: its first slot and its slot count
    m.Tick(2 + 2 * Fld(4)); c0 = U(Rd(v.colIndex + 4 * size_t(y), 4)); slots = U(Rd(v.colIndex + 4 * size_t(y) + 4, 4)) - c0; }
  // The table entry of source x in that column: its slot, or NOPOS. One probe: the bucket's displacement, then the
  // slot's source and chain time together. `h` is the chain time when the entry is found.
  W Entry(const View& v, U c0, U slots, U x, U& h, Meter& m) const {
    m.Tick(3); if (!slots) return NOPOS;
    m.Tick(4); const U dd = U(Bits(v.table, SlotPos(v, W(c0) + Bucket(x, slots)) + v.wb + v.hb + v.hopb, v.db, m));
    m.Tick(4 + 3); const W p = SlotPos(v, W(c0) + Slot(x, dd, slots)); const W xh = Bits(v.table, p, v.wb + v.hb, m); h = U(xh >> v.wb); return U(xh & v.pad) == x ? p : NOPOS; }
  // The same entry for Ask, with both remainders taken by multiplication: M is ModMagic(slots), hx is Mix32(x).
  // The same slot and the same charges as Entry.
  W EntryFast(const View& v, U c0, U slots, W M, U x, U hx, U& h, Meter& m) const {
    m.Tick(3); if (!slots) return NOPOS;
    m.Tick(4); const U dd = U(Bits(v.table, SlotPos(v, W(c0) + ModBy(hx, M, slots)) + v.wb + v.hb + v.hopb, v.db, m));
    m.Tick(4 + 3); const W p = SlotPos(v, W(c0) + ModBy(Mix32(x + 0x9E3779B9u * (dd + 1)), M, slots)); const W xh = Bits(v.table, p, v.wb + v.hb, m); h = U(xh >> v.wb); return U(xh & v.pad) == x ? p : NOPOS; }
  // The hop that a table entry (slot position e, source x) names: its target waypoint and kind; `hp` is the hop record.
  void EntryHop(const View& v, W e, U x, W& hp, U& z, U& kind, Meter& m) const {
    m.Tick(2 + Fld(4) + 3); const U hop = U(Bits(v.table, e + v.wb + v.hb, v.hopb, m)); hp = HopPos(v, W(HopFirst(v, x)) + hop);
    const W zk = Bits(v.hops, hp, v.wb + 2, m); z = U(zk & v.pad); kind = U(zk >> v.wb); }
  W FindOut(const View& v, U area, U x, Meter& m) const { m.Tick(2); for (U i = 0; i < v.K; ++i) { m.Tick(3 + 2 + 3); const W p = OutPos(v, area, i); if (U(Bits(v.out, p, v.wb, m)) == x) return p; } m.Tick(3); return NOPOS; }
  W FindIn(const View& v, U area, U y, Meter& m) const { m.Tick(2); for (U i = 0; i < v.K; ++i) { m.Tick(3 + 2 + 3); const W p = InPos(v, area, i); if (U(Bits(v.in, p, v.wb, m)) == y) return p; } m.Tick(3); return NOPOS; }
  void Take(Route& r, U e, U& at, Meter& m) const { m.Tick(3 + 4 + 4 + 4); Need(r.prefixCount < maxPrefix && r.prefixCount < NL_PREFIX_CAP, "patched prefix longer than the bake's bound"); r.prefix[r.prefixCount++] = e; r.graph += f.g.edges[e].w; at = index[f.g.edges[e].to]; }

  // ---- the patched prefix of an ask. Returns true when the answer is complete (the prefix ended on the goal).
  bool Prefix(Route& r, U s, U g, U& at, Meter& m) const {
    { U end; U p = PatchLower(startIndex, start, g, s, end, m);       // the pair's start patches in order: the first whose reach does not enter a closed area
      for (;; ++p) { m.Tick(3 + 2); if (p == end) break; m.Tick(2 + Fld(3) + 3); const uint8_t* q = start + 4 * size_t(p); if (U(Rd(q, 3)) != s) break;
        m.Tick(Fld(1) + 4 + 4 + 3); const U e = OutReach(s, q[3]); if (closed && closed[f.g.edges[e].to]) { m.Tick(2); continue; }
        Take(r, e, at, m); break; } }
    for (;;) { m.Tick(3); if (at == g) break; const U e = PatchFind(contIndex, cont, g, at, m); m.Tick(3); if (e == NONE) break; Take(r, e, at, m); }
    m.Tick(3); if (at == g) { m.Tick(8); r.found = true; r.first = r.prefix[0]; r.time = W(origin[r.first]) + r.graph + 1; return true; }
    return false;
  }

  // ---- the ask: one source, one goal (enabled indexes). No shared state.
  Route Ask(U s, U g, Meter& m) const {
    Route r; m.Tick(12); Need(s < n && g < n, "ask outside the map"); U at = s;
    m.Tick(3); if (s == g) { r.found = true; return r; }
    if (Prefix(r, s, g, at, m)) return r;
    U bestT = BIG, bestLev = NONE, bestX = NONE, bestY = NONE, bestI = 0, bestJ = 0, bestSt = 0; W bestEntry = NOPOS; m.Tick(8);
    for (U lev = 0; lev < NL; ++lev) { const View& v = lv[lev]; m.Tick(3 + 2 + 6 + 4);
      U gy[8], gt[8], sx[8], st[8], gk = 0, sk = 0;                 // a record's waypoint and time are side by side: one read
      for (U j = 0; j < v.K; ++j) { m.Tick(3 + 2 + 2 + 3); const W wt = Bits(v.in, InPos(v, g, j), v.wb + v.tb, m); const U y = U(wt & v.pad); if (y == v.pad) break; m.Tick(4); gy[gk] = y; gt[gk] = U(wt >> v.wb); ++gk; }
      for (U i = 0; i < v.K; ++i) { m.Tick(3 + 2 + 2 + 3); const W wt = Bits(v.out, OutPos(v, at, i), v.wb + v.tb, m); const U x = U(wt & v.pad); if (x == v.pad) break; m.Tick(4); sx[sk] = x; st[sk] = U(wt >> v.wb); ++sk; }
      m.Tick(3 + 3); const W* magic = colMagic[lev].data(); U hx[8]; for (U i = 0; i < sk; ++i) hx[i] = Mix32(sx[i]);   // each source hashed once per level (no charge: Entry's charge covers the hash)
      for (U j = 0; j < gk; ++j) { m.Tick(3 + 2); U c0, slots; Column(v, gy[j], c0, slots, m); const W M = magic[gy[j]];   // the column's constant, read once for every source
        for (U i = 0; i < sk; ++i) { m.Tick(3 + 2 + 3); U h = 0; W e = NOPOS;
          if (sx[i] != gy[j]) { e = EntryFast(v, c0, slots, M, sx[i], hx[i], h, m); m.Tick(3); if (e == NOPOS) continue; }
          const U t = st[i] + h + gt[j]; m.Tick(4 + 3); if (t > bestT) continue;
          m.Tick(3 + 3 + 3 + 3 + 3);     // time equal, same level, source less, source equal, target less
          if (t == bestT && (lev != bestLev || !(std::tie(sx[i], gy[j]) < std::tie(bestX, bestY)))) continue;
          m.Tick(16); bestT = t; bestLev = lev; bestX = sx[i]; bestY = gy[j]; bestI = i; bestJ = j; bestSt = st[i]; bestEntry = e; }
        m.Tick(3); }
      m.Tick(3); }
    Finish(r, at, g, bestT, bestLev, bestX, bestY, bestI, bestJ, bestSt, bestEntry, m); return r;
  }

  // ---- the chosen base route joins the answer: its first reach (when the prefix is empty) and the route time
  void Finish(Route& r, U at, U g, U bestT, U bestLev, U bestX, U bestY, U bestI, U bestJ, U bestSt, W bestEntry, Meter& m) const {
    m.Tick(3 + 3); if (bestLev == NONE) return;
    m.Tick(10); r.baseArea = at; r.level = bestLev; r.x = bestX; r.y = bestY; r.graph += bestT;
    m.Tick(3); if (r.prefixCount) { m.Tick(4); r.first = r.prefix[0]; }
    else { const View& v = lv[bestLev]; U ord = 0; m.Tick(3 + 3);
      if (bestSt > 0) { m.Tick(2); ord = U(Bits(v.out, OutPos(v, at, bestI) + v.wb + v.tb, v.oob, m)); }
      else { m.Tick(3);
        if (bestX != bestY) { W hp; U z, kind; EntryHop(v, bestEntry, bestX, hp, z, kind, m);
          if (kind == 0) { const W q = FindOut(v, at, z, m); ord = U(Bits(v.out, q + v.wb + v.tb, v.oob, m)); }
          else if (kind == 1) { m.Tick(3 + Fld(3)); const W q = FindIn(v, WpArea(v, z), bestX, m); ord = U(Bits(v.in, q + v.wb + v.tb + v.iob, v.farb, m)); }
          else { m.Tick(3 + 2 + Fld(4) + Fld(1)); ord = v.paths[Rd(v.lng + 9 * size_t(Bits(v.hops, hp + v.wb + 2, v.lb, m)), 4)]; } }
        else { m.Tick(2); ord = U(Bits(v.in, InPos(v, g, bestJ) + v.wb + v.tb + v.iob, v.farb, m)); } }
      m.Tick(4); r.first = OutReach(at, ord); }
    m.Tick(8); r.time = W(origin[r.first]) + r.graph + 1; r.found = true;
  }

  // ---- write-out: the first `want` reaches of the route (all of them when want is large). buf is scratch of maxBack reaches.
  // Returns the number written; `at` ends on the area reached.
  U Write(const Route& r, U g, U want, U* outReaches, std::vector<U>& buf, U& at, Meter& m) const {
    U count = 0; m.Tick(6);
    auto emit = [&](U e) { m.Tick(3 + 4 + 4); outReaches[count++] = e; at = index[f.g.edges[e].to]; };
    for (U i = 0; i < r.prefixCount && count < want; ++i) { m.Tick(6 + 2); emit(r.prefix[i]); }
    m.Tick(6 + 6); if (r.baseArea == NONE || count == want) return count;
    const View& v = lv[r.level]; at = r.baseArea; m.Tick(6);
    auto forward = [&](U x) -> bool { m.Tick(Fld(3) + 2); const U xa = WpArea(v, x);
      for (;;) { m.Tick(3); if (at == xa) return true; m.Tick(3); if (count == want) return false; const W q = FindOut(v, at, x, m); m.Tick(4); emit(OutReach(at, U(Bits(v.out, q + v.wb + v.tb, v.oob, m)))); } };
    auto backward = [&](U y, U target) -> bool { m.Tick(Fld(3) + 4); const U ya = WpArea(v, y); U p = target, b = 0;
      for (;;) { m.Tick(3); if (p == ya) break; const W q = FindIn(v, p, y, m); m.Tick(4 + 3 + 4 + 4); Need(b < buf.size(), "backward piece longer than the bake's bound"); const U e = InReach(p, U(Bits(v.in, q + v.wb + v.tb, v.iob, m))); buf[b++] = e; p = index[f.g.edges[e].from]; }
      while (b) { m.Tick(3 + 3); if (count == want) return false; emit(buf[--b]); }
      m.Tick(3); return true; };
    m.Tick(3 + 2); if (!forward(r.x)) return count;
    U c0, slots; Column(v, r.y, c0, slots, m);                    /* the target waypoint's column, read once for every hop */
    U x = r.x; for (;;) { m.Tick(3 + 3 + 2); /* arrival test; the hop's result test and the waypoint advanced */ if (x == r.y) break; U h; const W e = Entry(v, c0, slots, x, h, m);
      W hp; U z, kind; EntryHop(v, e, x, hp, z, kind, m);
      if (kind == 0) { if (!forward(z)) return count; } else if (kind == 1) { m.Tick(3 + Fld(3)); if (!backward(x, WpArea(v, z))) return count; }
      else { m.Tick(3 + 2 + Fld(4) + Fld(2)); const uint8_t* lp = v.lng + 9 * size_t(Bits(v.hops, hp + v.wb + 2, v.lb, m)); const uint8_t* path = v.paths + Rd(lp, 4); const U len = U(Rd(lp + 4, 2));
        for (U i = 0; i < len; ++i) { m.Tick(3 + 3); if (count == want) return count; m.Tick(Fld(1) + 4); emit(OutReach(at, path[i])); }
        m.Tick(3); }
      x = z; }
    m.Tick(3); backward(r.y, g); return count;
  }
};

} // namespace nl
