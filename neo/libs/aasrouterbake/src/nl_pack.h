#pragma once
// exp-active2, version 4 of the packed native lists (director, 2026-11-14): the same content as version 3
// (../nl_pack.h), with the list records, the hop records and the table slots packed bit by bit at the widths
// their values need, and the table columns laid out with a twentieth of slack instead of a quarter.
// It follows the eighth Opus review (../../review-w2/OPUS-8.md, P1).
//
// Sections ("Ln." = level n). Byte fields are little-endian. A bit-packed section is one stream of bits, the
// lowest bit of each byte first; record i starts at bit i x (record bits); a field's lowest bit comes first.
//   meta        4-byte fields: version (4), enabled areas, levels, longest patched prefix, longest backward piece;
//               then per level 13 fields: cap, K, range, waypoints, and the widths in BITS of: a waypoint, a record
//               time, an outgoing reach position, an incoming reach position, a far reach position, a chain time,
//               a hop number, a displacement, a stored-path number (0 on a level without stored paths)
//   Ln.wpArea   per waypoint: its area (3 bytes, enabled index)                                   [as version 3]
//   Ln.out      BITS. Per area K records: waypoint, time, reach (position among the area's reaches; all ones =
//               none). An unused record has a waypoint of all ones, time 0 and reach all ones.
//   Ln.in       BITS. Per area K records: waypoint, time, last reach (position among the area's INCOMING reaches;
//               all ones = none), far reach (position among the WAYPOINT's reaches; all ones = none).
//   Ln.hopIndex per waypoint + 1: 4-byte offset into Ln.hops, counted in hops                    [as version 3]
//   Ln.hops     BITS. Per hop: target waypoint, kind (2 bits: 0 forward, 1 backward, 2 stored path); on a level
//               with stored paths also the path's number in Ln.long (all ones for the other kinds).
//   Ln.long     per stored path: offset into Ln.paths (4 bytes), reaches (2 bytes), time (3 bytes) [as version 3]
//   Ln.paths    per reach of a stored path: its position among the reaches of the area it leaves (1 byte)
//   Ln.colIndex per target waypoint + 1: 4-byte offset into Ln.table, counted in slots           [as version 3]
//   Ln.table    BITS. Per target a column of slots, about a twentieth more slots than kept entries. A slot: source
//               waypoint, chain time, hop number, displacement. An empty slot has a source of all ones and time
//               and hop number 0. A source is found with one probe (LayoutColumn below).
//               An entry for source == target is never stored: its time is 0.
//   startIndex, contIndex, start, cont                                                           [as version 3]
#include "nl_core.h"
namespace nl {

// Spare bytes (2026-11-15). The game side reads a bit field with one 8-byte load (RdBits8 below), which takes up to
// 7 bytes after the field's last byte. A section in memory therefore keeps SPARE zero bytes after its end: inside
// the vector's capacity and outside its size, so no size, no size check and no byte of a file changes. Blob::Load
// makes them and a copy of a Blob keeps them; Runtime refuses a Blob without them (HasSpare).
constexpr size_t SPARE = 8;

struct Blob {
  std::vector<std::pair<std::string, std::vector<uint8_t>>> sec;
  Blob() { sec.reserve(64); }   // never reallocated, so a reference to one section stays valid while others are added
  Blob(const Blob& o) { sec.reserve(64); for (const auto& s : o.sec) { auto& v = Add(s.first); v.reserve(s.second.size() + SPARE); v.assign(s.second.begin(), s.second.end()); Spare(v); } }   // a copy has the spare bytes
  Blob& operator=(const Blob& o) { if (this != &o) { Blob t(o); sec.swap(t.sec); } return *this; }
  Blob(Blob&&) = default; Blob& operator=(Blob&&) = default;                                // a move keeps each section's memory, and with it the spare bytes
  static void Spare(std::vector<uint8_t>& v) { const size_t n = v.size(); v.reserve(n + SPARE); v.resize(n + SPARE); v.resize(n); }   // SPARE zero bytes after the end; the size stays n
  bool HasSpare() const { for (const auto& s : sec) if (s.second.capacity() < s.second.size() + SPARE) return false; return true; }
  std::vector<uint8_t>& Add(const std::string& name) { Need(sec.size() < 64, "too many packed sections"); sec.emplace_back(name, std::vector<uint8_t>()); return sec.back().second; }
  const std::vector<uint8_t>& Get(const std::string& name) const { for (const auto& s : sec) if (s.first == name) return s.second; throw std::runtime_error("packed section missing: " + name); }
  W Bytes() const { W b = 0; for (const auto& s : sec) b += s.second.size(); return b; }
  struct File { FILE* fp; ~File() { if (fp) fclose(fp); } };            // closed also when a refusal leaves the function
  void Save(const std::string& path) const {
    File f{fopen(path.c_str(), "wb")}; Need(f.fp != nullptr, "cannot write the packed file"); const U count = U(sec.size()); Need(fwrite("NLW2PACK", 1, 8, f.fp) == 8 && fwrite(&count, 4, 1, f.fp) == 1, "packed file write");
    for (const auto& s : sec) { char name[24] = {}; strncpy(name, s.first.c_str(), 23); const W size = s.second.size(); Need(size < (W(1) << 31), "packed section too large"); Need(fwrite(name, 1, 24, f.fp) == 24 && fwrite(&size, 8, 1, f.fp) == 1, "packed file write"); if (size) Need(fwrite(s.second.data(), 1, size, f.fp) == size, "packed file write"); }
    FILE* fp = f.fp; f.fp = nullptr; Need(fclose(fp) == 0, "packed file close"); }
  static Blob Load(const std::string& path) {
    File f{fopen(path.c_str(), "rb")}; Need(f.fp != nullptr, "cannot read the packed file"); char magic[8]; U count = 0; Blob b;
    Need(fread(magic, 1, 8, f.fp) == 8 && !memcmp(magic, "NLW2PACK", 8) && fread(&count, 4, 1, f.fp) == 1 && count < 64, "packed file header");
    for (U i = 0; i < count; ++i) { char name[24]; W size = 0; Need(fread(name, 1, 24, f.fp) == 24 && fread(&size, 8, 1, f.fp) == 1 && size < (W(1) << 31), "packed section header"); name[23] = 0;
      auto& v = b.Add(name); v.reserve(size + SPARE); v.resize(size); if (size) Need(fread(v.data(), 1, size, f.fp) == size, "packed section body"); Spare(v); }
    return b; }
};
inline void Put(std::vector<uint8_t>& v, W x, U bytes) { Need(x < (W(1) << (8 * bytes)), "value too wide for its field"); for (U i = 0; i < bytes; ++i) v.push_back(uint8_t(x >> (8 * i))); }
inline W Rd(const uint8_t* p, U bytes) { W x = 0; for (U i = 0; i < bytes; ++i) x |= W(p[i]) << (8 * i); return x; }
struct Patch { U goal = 0, area = 0, reach = 0; };   // enabled indexes; reach = graph reach number
inline std::string Sec(U lev, const char* name) { return "L" + std::to_string(lev) + "." + name; }

// ---- bit fields
inline U BitsFor(W v) { U b = 1; while (b < 32 && (v >> b)) ++b; return b; }           // the bits that hold the value v (at least 1)
inline U BytesAt(W pos, U width) { return U(((pos & 7) + width + 7) >> 3); }           // the bytes a field of `width` bits at bit `pos` lies in
// `width` bits at bit `pos` of a bit-packed section (width at most 56). It reads the bytes the field lies in, no more.
inline W RdBits(const uint8_t* base, W pos, U width) { const uint8_t* p = base + (pos >> 3); const U sh = U(pos & 7), nb = BytesAt(pos, width); W x = 0; for (U i = 0; i < nb; ++i) x |= W(p[i]) << (8 * i); return (x >> sh) & ((W(1) << width) - 1); }
// The same value with one 8-byte load in place of the byte loop (2026-11-15; a little-endian machine). It takes 8
// bytes from the field's first byte, so up to 7 bytes after the field's last: only for a section that has its spare
// bytes (SPARE above). What those bytes hold never reaches the result: the shift and the mask cut it off.
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "RdBits8 loads a little-endian word"
#endif
inline W RdBits8(const uint8_t* base, W pos, U width) { W x; memcpy(&x, base + (pos >> 3), 8); return (x >> (pos & 7)) & ((W(1) << width) - 1); }
inline W BitBytes(W count, U bits) { return (count * bits + 7) / 8; }                  // the bytes of `count` records of `bits` bits
struct BitPut {                                                                        // appends fields to a section that holds nothing else
  std::vector<uint8_t>& v; W n = 0;
  void Put(W x, U width) { Need(width >= 1 && width <= 32 && x < (W(1) << width), "value too wide for its field");
    for (U i = 0; i < width;) { const U off = U(n & 7), take = std::min<U>(8 - off, width - i); if (off == 0) v.push_back(0); v.back() |= uint8_t(((x >> i) & ((W(1) << take) - 1)) << off); i += take; n += take; } }
};

// The table's columns are laid out for one probe (hash and displace). Arithmetic only, then two reads: slot
// Bucket(x, m) of a column of m slots carries a displacement d; source x, if kept, sits in slot Slot(x, d, m).
inline U Mix32(U v) { v ^= v >> 16; v *= 0x85EBCA6Bu; v ^= v >> 13; v *= 0xC2B2AE35u; v ^= v >> 16; return v; }
inline U Bucket(U x, U m) { return Mix32(x) % m; }
inline U Slot(U x, U d, U m) { return Mix32(x + 0x9E3779B9u * (d + 1)) % m; }
struct Ent { U x = 0, h = 0, hop = 0; };                // one kept entry of a column: source waypoint, chain time, hop number
constexpr U TABLE_SLACK = 20;                           // a column of n entries starts with n + n / TABLE_SLACK + 1 slots (version 3: n / 4)
constexpr U DISP_BITS = 8;                              // displacements 0..255, as in version 3
// One target's kept entries (distinct sources) laid out as a column. Returns the slot count; at[s] is the entry in
// slot s or NONE, disp[s] the displacement of bucket s. Buckets are placed fullest first (ties by bucket number),
// each with the lowest displacement that puts all its sources on free slots; if some bucket has none among 256,
// the column gets one more slot and is laid out again. The result depends on the SET of sources only.
inline U LayoutColumn(const Ent* ents, U n, std::vector<U>& at, std::vector<U>& disp) {
  at.clear(); disp.clear(); if (!n) return 0;
  for (U m = n + n / TABLE_SLACK + 1;; ++m) { Need(m <= 4 * n + 64, "no one-probe layout for a table column");
    std::vector<std::vector<U>> bucket(m); for (U i = 0; i < n; ++i) bucket[Bucket(ents[i].x, m)].push_back(i);
    std::vector<U> order(m); for (U b = 0; b < m; ++b) order[b] = b; std::stable_sort(order.begin(), order.end(), [&](U a, U b) { return bucket[a].size() > bucket[b].size(); });
    at.assign(m, NONE); disp.assign(m, 0); bool ok = true;
    for (U b : order) { const auto& keys = bucket[b]; if (keys.empty()) break; U dd = 0;
      for (; dd < (U(1) << DISP_BITS); ++dd) { bool fits = true;
        for (size_t i = 0; i < keys.size() && fits; ++i) { const U s = Slot(ents[keys[i]].x, dd, m); fits = at[s] == NONE; for (size_t j = 0; j < i && fits; ++j) fits = Slot(ents[keys[j]].x, dd, m) != s; }
        if (fits) break; }
      if (dd == (U(1) << DISP_BITS)) { ok = false; break; }
      disp[b] = dd; for (U i : keys) at[Slot(ents[i].x, dd, m)] = i; }
    if (ok) return m; }
}

// One level's content as plain numbers. The bake fills it from its tables (Pack below); the converter fills it
// from a version 3 file (nl_repack4.cpp). Both hand it to Write4, so both write the same bytes.
struct LevelContent {
  U cap = 0, K = 0, range = 0, m = 0;
  std::vector<U> wpArea;                        // per waypoint: its area (enabled index)
  std::vector<U> outW, outT, outOrd;            // per area K records. Unused record: outW NONE (outT 0, outOrd NONE). outOrd NONE: no reach (the area is the waypoint's)
  std::vector<U> inW, inT, inOrd, inFar;        // likewise; inFar NONE: none
  std::vector<U> hopIndex;                      // per waypoint + 1, counted in hops
  std::vector<U> hopZ, hopKind, hopLong;        // per hop; hopLong NONE unless kind 2
  std::vector<U> longOff, longLen, longTime;    // per stored path
  std::vector<uint8_t> paths;                   // the stored paths' reach positions
  std::vector<size_t> colIndex;                 // per target + 1, counted in ENTRIES of `ents`
  std::vector<Ent> ents;                        // the kept entries, by target
};
struct Widths { U w = 0, t = 0, oo = 0, io = 0, far = 0, h = 0, hop = 0, d = 0, l = 0; };
inline Widths WidthsOf(const LevelContent& c) {
  W maxT = 0, maxOO = 0, maxIO = 0, maxFar = 0, maxH = 0, maxHop = 0;
  for (size_t r = 0; r < c.outW.size(); ++r) { if (c.outW[r] != NONE) maxT = std::max<W>(maxT, c.outT[r]); if (c.outOrd[r] != NONE) maxOO = std::max<W>(maxOO, c.outOrd[r]); }
  for (size_t r = 0; r < c.inW.size(); ++r) { if (c.inW[r] != NONE) maxT = std::max<W>(maxT, c.inT[r]); if (c.inOrd[r] != NONE) maxIO = std::max<W>(maxIO, c.inOrd[r]); if (c.inFar[r] != NONE) maxFar = std::max<W>(maxFar, c.inFar[r]); }
  for (const Ent& e : c.ents) { maxH = std::max<W>(maxH, e.h); maxHop = std::max<W>(maxHop, e.hop); }
  Widths wd; wd.w = BitsFor(c.m); wd.t = BitsFor(maxT); wd.oo = BitsFor(maxOO + 1); wd.io = BitsFor(maxIO + 1); wd.far = BitsFor(maxFar + 1);      // "+ 1": all ones stays free for "none"
  wd.h = BitsFor(maxH); wd.hop = BitsFor(maxHop); wd.d = DISP_BITS; wd.l = c.longOff.empty() ? 0 : BitsFor(c.longOff.size()); return wd;
}
inline Blob Write4(U n, U maxPrefix, U maxBack, const std::vector<LevelContent>& levels, const std::vector<uint8_t>& startIndex, const std::vector<uint8_t>& start,
                   const std::vector<uint8_t>& contIndex, const std::vector<uint8_t>& cont) {
  Blob b; const U NL = U(levels.size()); Need(NL >= 1 && NL <= 4, "levels of a packed file");
  auto& meta = b.Add("meta"); Put(meta, 4, 4); Put(meta, n, 4); Put(meta, NL, 4); Put(meta, maxPrefix, 4); Put(meta, maxBack, 4);
  for (U lev = 0; lev < NL; ++lev) { const LevelContent& c = levels[lev]; const size_t recs = size_t(n) * c.K, hops = c.hopZ.size(), longs = c.longOff.size();
    Need(c.wpArea.size() == c.m && c.outW.size() == recs && c.outT.size() == recs && c.outOrd.size() == recs && c.inW.size() == recs && c.inT.size() == recs && c.inOrd.size() == recs && c.inFar.size() == recs, "level content: lists");
    Need(c.hopIndex.size() == size_t(c.m) + 1 && c.hopIndex[0] == 0 && c.hopIndex[c.m] == hops && c.hopKind.size() == hops && c.hopLong.size() == hops && c.longLen.size() == longs && c.longTime.size() == longs, "level content: hops");
    Need(c.colIndex.size() == size_t(c.m) + 1 && c.colIndex[0] == 0 && c.colIndex[c.m] == c.ents.size(), "level content: table");
    const Widths wd = WidthsOf(c); const W pad = (W(1) << wd.w) - 1, ooNone = (W(1) << wd.oo) - 1, ioNone = (W(1) << wd.io) - 1, farNone = (W(1) << wd.far) - 1, lNone = wd.l ? (W(1) << wd.l) - 1 : 0;
    Need(c.m <= pad && wd.w <= 24 && wd.t <= 24 && wd.oo <= 8 && wd.io <= 8 && wd.far <= 8 && wd.h <= 24 && wd.hop <= 8 && wd.d <= 8 && wd.l <= 24, "a field wider than the game side reads (the limits of nl_ask.h)");
    for (U x = 0; x < c.m; ++x) Need(c.hopIndex[x] <= c.hopIndex[x + 1] && c.hopIndex[x + 1] - c.hopIndex[x] < 255, "level content: hops of one waypoint");
    for (U v : {c.cap, c.K, c.range, c.m, wd.w, wd.t, wd.oo, wd.io, wd.far, wd.h, wd.hop, wd.d, wd.l}) Put(meta, v, 4);
    auto& wa = b.Add(Sec(lev, "wpArea")); for (U w = 0; w < c.m; ++w) Put(wa, c.wpArea[w], 3);
    { BitPut o{b.Add(Sec(lev, "out"))}; for (size_t r = 0; r < recs; ++r) {
        if (c.outW[r] == NONE) { o.Put(pad, wd.w); o.Put(0, wd.t); o.Put(ooNone, wd.oo); continue; }
        Need(c.outW[r] < c.m, "level content: Out waypoint"); o.Put(c.outW[r], wd.w); o.Put(c.outT[r], wd.t); o.Put(c.outOrd[r] == NONE ? ooNone : W(c.outOrd[r]), wd.oo); } }
    { BitPut i{b.Add(Sec(lev, "in"))}; for (size_t r = 0; r < recs; ++r) {
        if (c.inW[r] == NONE) { i.Put(pad, wd.w); i.Put(0, wd.t); i.Put(ioNone, wd.io); i.Put(farNone, wd.far); continue; }
        Need(c.inW[r] < c.m, "level content: In waypoint"); i.Put(c.inW[r], wd.w); i.Put(c.inT[r], wd.t); i.Put(c.inOrd[r] == NONE ? ioNone : W(c.inOrd[r]), wd.io); i.Put(c.inFar[r] == NONE ? farNone : W(c.inFar[r]), wd.far); } }
    auto& hi = b.Add(Sec(lev, "hopIndex")); for (U x = 0; x <= c.m; ++x) Put(hi, c.hopIndex[x], 4);
    { BitPut h{b.Add(Sec(lev, "hops"))}; for (size_t k = 0; k < hops; ++k) { Need(c.hopZ[k] < c.m && c.hopKind[k] <= 2 && (c.hopKind[k] == 2) == (c.hopLong[k] != NONE) && (c.hopLong[k] == NONE || c.hopLong[k] < longs), "level content: hop");
        h.Put(c.hopZ[k], wd.w); h.Put(c.hopKind[k], 2); if (wd.l) h.Put(c.hopLong[k] == NONE ? lNone : W(c.hopLong[k]), wd.l); } }
    auto& lg = b.Add(Sec(lev, "long")); for (size_t k = 0; k < longs; ++k) { Put(lg, c.longOff[k], 4); Put(lg, c.longLen[k], 2); Put(lg, c.longTime[k], 3); }
    b.Add(Sec(lev, "paths")) = c.paths;
    auto& ci = b.Add(Sec(lev, "colIndex")); BitPut t{b.Add(Sec(lev, "table"))}; W slots = 0; std::vector<U> at, disp, xs;
    for (U y = 0; y < c.m; ++y) { Put(ci, slots, 4); Need(c.colIndex[y] <= c.colIndex[y + 1], "level content: table index"); const Ent* col = c.ents.data() + c.colIndex[y]; const U count = U(c.colIndex[y + 1] - c.colIndex[y]);
      xs.clear(); for (U i = 0; i < count; ++i) xs.push_back(col[i].x); std::sort(xs.begin(), xs.end()); Need(std::adjacent_find(xs.begin(), xs.end()) == xs.end(), "level content: a source twice in one table column");      // a repeated source could never be laid out
      const U m = LayoutColumn(col, count, at, disp);
      for (U s = 0; s < m; ++s) { if (at[s] == NONE) { t.Put(pad, wd.w); t.Put(0, wd.h); t.Put(0, wd.hop); } else { const Ent& e = col[at[s]]; Need(e.x < c.m && e.x != y, "level content: table source"); t.Put(e.x, wd.w); t.Put(e.h, wd.h); t.Put(e.hop, wd.hop); } t.Put(disp[s], wd.d); }
      slots += m; }
    Put(ci, slots, 4);
  }
  b.Add("startIndex") = startIndex; b.Add("start") = start; b.Add("contIndex") = contIndex; b.Add("cont") = cont;
  return b;
}

// keep[lev][entry] marks the table entries to store. start and cont must be sorted by (goal, area).
inline Blob Pack(const Fixture4& f, const std::vector<U>& active, const std::vector<Level>& levels, const std::vector<std::vector<uint8_t>>& keep,
                 const std::vector<Patch>& start, const std::vector<Patch>& cont, U maxPrefix) {
  const U n = U(active.size()), NL = U(levels.size());
  auto outOrd = [&](U area, U e) -> U { if (e == NONE) return NONE; Need(f.g.edges[e].from == active[area], "packed reach does not leave its area"); const U o = e - f.g.out[active[area]]; Need(o < 255, "reach position too large"); return o; };
  auto inOrd = [&](U area, U e) -> U { if (e == NONE) return NONE; const U a = active[area]; for (U j = f.g.in[a]; j < f.g.in[a + 1]; ++j) if (f.g.rev[j] == e) { Need(j - f.g.in[a] < 255, "incoming reach position too large"); return j - f.g.in[a]; } throw std::runtime_error("packed reach does not enter its area"); };
  U maxBack = 0; for (const Level& l : levels) maxBack = std::max(maxBack, l.maxInReaches);
  std::vector<LevelContent> lc(NL);
  for (U lev = 0; lev < NL; ++lev) { const Level& l = levels[lev]; LevelContent& c = lc[lev]; c.cap = l.cap; c.K = l.K; c.range = l.range; c.m = l.m;
    for (U w = 0; w < l.m; ++w) c.wpArea.push_back(l.wp[w]);
    for (U a = 0; a < n; ++a) for (U i = 0; i < l.K; ++i) {
      if (i < l.out[a].count) { const Att& r = l.out[a].a[i]; c.outW.push_back(r.wp); c.outT.push_back(r.t); c.outOrd.push_back(outOrd(a, r.reach)); } else { c.outW.push_back(NONE); c.outT.push_back(0); c.outOrd.push_back(NONE); }
      if (i < l.in[a].count) { const Att& r = l.in[a].a[i]; c.inW.push_back(r.wp); c.inT.push_back(r.t); c.inOrd.push_back(inOrd(a, r.reach)); c.inFar.push_back(r.far == NONE ? NONE : outOrd(l.wp[r.wp], r.far)); } else { c.inW.push_back(NONE); c.inT.push_back(0); c.inOrd.push_back(NONE); c.inFar.push_back(NONE); } }
    for (U x = 0; x < l.m; ++x) { c.hopIndex.push_back(U(c.hopZ.size())); for (const Hop& h : l.hops[x]) { c.hopZ.push_back(h.to); c.hopKind.push_back(h.kind);
        if (h.kind != 2) { c.hopLong.push_back(NONE); continue; }
        c.hopLong.push_back(U(c.longOff.size())); c.longOff.push_back(U(c.paths.size())); c.longLen.push_back(h.pathLen); c.longTime.push_back(h.w);
        for (U i = 0; i < h.pathLen; ++i) { const U e = l.paths[h.pathStart + i]; const U o = e - f.g.out[f.g.edges[e].from]; Need(o < 255, "reach position too large"); c.paths.push_back(uint8_t(o)); } } }
    c.hopIndex.push_back(U(c.hopZ.size()));
    for (U y = 0; y < l.m; ++y) { c.colIndex.push_back(c.ents.size()); for (size_t p = l.col[y]; p < l.col[y + 1]; ++p) if (keep[lev][p] && l.cx[p] != y) c.ents.push_back({l.cx[p], l.ch[p], l.cn[p]}); }
    c.colIndex.push_back(c.ents.size());
  }
  std::vector<uint8_t> ix[2], body[2];
  for (int which = 0; which < 2; ++which) { const auto& list = which ? cont : start; size_t p = 0;
    for (U g = 0; g < n; ++g) { Put(ix[which], p, 4); while (p < list.size() && list[p].goal == g) { Need(p == 0 || list[p - 1].goal != g || list[p - 1].area < list[p].area || (!which && list[p - 1].area == list[p].area), "patches not sorted"); Put(body[which], list[p].area, 3); const U o = outOrd(list[p].area, list[p].reach); Need(o != NONE, "patch without a reach"); Put(body[which], o, 1); ++p; } }
    Need(p == list.size(), "patch for a goal out of range"); Put(ix[which], p, 4); }
  return Write4(n, maxPrefix, maxBack, lc, ix[0], body[0], ix[1], body[1]);
}

} // namespace nl
