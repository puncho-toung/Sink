#pragma once
// R2 router, engine port (director, 2026-11-17): the base definitions of the router's runtime.
// The standalone experiments took these from a chain of earlier experiment headers; here they stand alone, so that
// the runtime compiles with nothing but the C++ standard library. The names and the values are the experiments'.
//   U, W, NONE, BIG, Need     exp-signpost/S3c/native_common.h
//   Edge, FlatGraph           exp-overlay6/6E/scratch/input5.h (only the members the runtime reads)
//   Undirected, Components    exp-signpost/S3c/classification.h (for the neck gate)
// A refusal is a C++ exception (std::runtime_error) with the refusal's text; the engine side catches it at its
// own boundary.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
namespace nl {
using U = uint32_t; using W = uint64_t;
constexpr U NONE = UINT32_MAX, BIG = UINT32_MAX / 4;
// A refusal. The throw is kept out of line (review OPUS-2, 9): Need is used at hundreds of places, and each
// would otherwise carry the making of the exception inline.
#if defined(_MSC_VER)
[[noreturn]] __declspec(noinline) inline void Refuse(const char* message) { throw std::runtime_error(message); }
#else
[[noreturn]] __attribute__((noinline)) inline void Refuse(const char* message) { throw std::runtime_error(message); }
#endif
inline void Need(bool yes, const char* message) { if (!yes) Refuse(message); }
// The map as the router sees it. Areas are numbered as in the .aas file. The reaches (edges) of one profile are
// stored area by area in the order the engine holds them: out[a] .. out[a + 1] are the reaches leaving area a,
// in[a] .. in[a + 1] index rev[], which names the reaches entering a. w is a reach's time (ATLAS.md section 3.1).
struct Edge { U from, to, w, ordinal; };
struct FlatGraph { U n = 0; std::vector<Edge> edges; std::vector<U> out, in, rev; std::vector<uint8_t> enabled, mutableArea; };
struct Fixture4 { FlatGraph g; };
} // namespace nl
namespace s3c {
using nl::U; using nl::W;
struct DoorClassification { std::vector<uint8_t> forced, loop; U forcedCount = 0, loopCount = 0; };   // per area: a narrow neck (every connection a bridge); a wide door
inline std::vector<std::vector<U>> Undirected(const nl::Fixture4& f) {
  std::vector<std::vector<U>> adj(f.g.n);
  for (const auto& e : f.g.edges) if (f.g.enabled[e.from] && f.g.enabled[e.to] && e.from != e.to) { adj[e.from].push_back(e.to); adj[e.to].push_back(e.from); }
  for (auto& a : adj) { std::sort(a.begin(), a.end()); a.erase(std::unique(a.begin(), a.end()), a.end()); } return adj;
}
inline void Components(const std::vector<std::vector<U>>& adj, const std::vector<uint8_t>& live, std::vector<U>& comp, std::vector<W>& sizes) {
  comp.assign(adj.size(), nl::NONE); sizes.clear(); std::vector<U> q;
  for (U s = 0; s < adj.size(); s++) if (live[s] && comp[s] == nl::NONE) { const U c = U(sizes.size()); q = {s}; comp[s] = c; for (size_t i = 0; i < q.size(); i++) for (U v : adj[q[i]]) if (live[v] && comp[v] == nl::NONE) { comp[v] = c; q.push_back(v); } sizes.push_back(q.size()); }
}
} // namespace s3c
