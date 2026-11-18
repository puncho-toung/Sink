#pragma once
// R2 router, engine port: a packed file from memory. Blob::Load (rt_pack.h) reads a file with fopen; the engine
// reads files through its file system and hands the bytes over. The format is Blob::Save's: 8 bytes "NLW2PACK",
// a 4-byte section count (below 64), then per section a 24-byte name (zero-terminated inside the 24 bytes), an
// 8-byte size (below 2^31) and the bytes. Every byte of the memory must be accounted for. Each section gets its
// spare bytes, as Blob::Load gives them. Returns false on anything else; b is then not to be used.
// The parity test (../core_test.cpp) reads its files through this function, so it is covered by that test.
#include "rt_pack.h"
namespace nl {
inline bool BlobFromMemory(const uint8_t* data, size_t size, Blob& b) {
  if (size < 12 || memcmp(data, "NLW2PACK", 8) != 0) return false;
  size_t pos = 8; const U count = U(Rd(data + pos, 4)); pos += 4; if (count >= 64) return false;
  for (U s = 0; s < count; ++s) {
    if (size - pos < 32) return false;
    char name[25]; memcpy(name, data + pos, 24); name[24] = 0; if (strlen(name) >= 24) return false;
    const W bytes = Rd(data + pos + 24, 8); pos += 32;
    if (bytes >= (W(1) << 31) || bytes > W(size - pos)) return false;       // the second test in 64 bits: no sum that could wrap (reviews OPUS-1, 1 and OPUS-2, 2)
    std::vector<uint8_t>& v = b.Add(name); v.reserve(size_t(bytes) + SPARE); v.assign(data + pos, data + pos + size_t(bytes)); Blob::Spare(v);
    pos += size_t(bytes);
  }
  return pos == size;
}
} // namespace nl
