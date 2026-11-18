#pragma once
// R2 router, engine port: the READING half of the packed lists file (exp-active2/native-lists/pack4/nl_pack.h, its
// lines from "namespace nl {" to the table's hash, verbatim; that file's head describes every section). The
// packer and the column layout, which only the bake uses, are left out.
#include "rt_base.h"
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

} // namespace nl
