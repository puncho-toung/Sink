#pragma once
// R2 router, engine port (director, 2026-11-17): A MAP WRITTEN BY THE ENGINE as a fixture of the standalone
// programs, so that the unchanged bake and check programs (exp-active2/native-lists/pack4 and its
// outdoor/doors/lastleg) run on a real map.
//
// The map compiler writes the file ("rbdmap routerMap <map>", aas/AASFile_router.cpp in the engine tree; the same
// "RTG1" format as ../ll_dump.cpp writes for the experiments' own fixtures). It holds the map of ONE travel-flag
// mask as integers: the reaches with their times, the lists out / in / rev, which areas are of the mask, which
// can be shut, and the Origin of every reach. No position is in it. The engine works out every time and every
// Origin with its own arithmetic; this side takes them as they are.
//
// In the environment:
//   RT_MAP=<file.rtg>    the map (needed)
//   RT_DOORS=<file>      a text file of area numbers: the areas that DOORS shut and open (each must be an area the
//                        map marks as one that can be shut). RT_DOORS=all: every such area. Not set: the map has
//                        no door at all, and every area that could be shut is an ordinary area.
//   RT_FLY=1             the map is of the fly mask (the programs print it; nothing else depends on it)
// Which door areas are narrow necks and which are wide doors is worked out here by the bridge rule, as for every
// other fixture; the wide doors become the map's door census, one door per area.
//
// How the programs get the Origin without positions: they work it out from an area's centre and a reach's start
// point (s3c::NativeOrigin). Here every centre is the zero point and a reach's start is put at x = 3 x Origin + 1.5,
// which that function turns into exactly Origin (checked for every reach below). Lengths and positions mean
// nothing on such a map; nothing in the time-only programs reads them for more than a tie in the waypoint cover.
//
// Each program includes this file through tw_native.h (-DLL_RAW); `Native` and `ClassifyDoors` are redirected as
// in ../../exp-active2/native-lists/pack4/outdoor/doors/necks/ds_native.h (DN_HIDE_NECKS as there). SIDE means
// nothing here; run the programs with SIDE 2.
#include "od_native.h"
#include <set>
#undef Native
#undef ClassifyDoors
inline overlay_four::Fixture4 RawMap(uint32_t side) {
  using overlay_four::U;
  (void)side;      // every SIDE names the map of RT_MAP; the programs are run with SIDE 2 (the lists' bake accepts 2, 4 and 8 only)
  const char* path = getenv("RT_MAP"); if (!path || !*path) throw std::runtime_error("RT_MAP=<file.rtg> is not set");
  FILE* in = fopen(path, "rb"); if (!in) throw std::runtime_error(std::string("cannot read the map ") + path);
  auto fail = [&](const char* what) { fclose(in); throw std::runtime_error(std::string("map file: ") + what); };
  auto u32 = [&]() { uint32_t x = 0; if (fread(&x, 4, 1, in) != 1) fail("it ends early"); return x; };
  auto u8 = [&]() { uint8_t x = 0; if (fread(&x, 1, 1, in) != 1) fail("it ends early"); return x; };
  char magic[4]; if (fread(magic, 1, 4, in) != 4 || memcmp(magic, "RTG1", 4)) fail("not a router map");
  overlay_four::Fixture4 f; const U N = u32(), E = u32(); if (N < 2 || N > (1u << 25) || E > (1u << 30)) fail("its sizes");
  f.g.n = N; f.g.edges.resize(E); f.g.degree.assign(N, 0);
  for (auto& e : f.g.edges) { e.from = u32(); e.to = u32(); e.w = u32(); e.ordinal = u32(); if (e.from >= N || e.to >= N || e.w < 1 || e.ordinal >= 1024) fail("a reach"); f.g.degree[e.from] = std::max(f.g.degree[e.from], e.ordinal + 1); }
  std::vector<U> out(N + 1), inn(N + 1), rev(E); for (U& x : out) x = u32(); for (U& x : inn) x = u32(); for (U& x : rev) x = u32();
  std::vector<uint8_t> canShut(N); f.g.enabled.resize(N); for (U a = 0; a < N; ++a) { f.g.enabled[a] = u8() != 0; canShut[a] = u8() != 0; u8(); }
  std::vector<U> origin(E); for (U& x : origin) x = u32();
  fclose(in);
  // the doors
  f.g.mutableArea.assign(N, 0); const char* doors = getenv("RT_DOORS");
  if (doors && !strcmp(doors, "all")) { for (U a = 0; a < N; ++a) f.g.mutableArea[a] = canShut[a] && f.g.enabled[a]; }
  else if (doors && *doors) { FILE* d = fopen(doors, "rb"); if (!d) throw std::runtime_error(std::string("cannot read the door list ") + doors); unsigned a = 0;
    while (fscanf(d, "%u", &a) == 1) { if (a >= N || !canShut[a] || !f.g.enabled[a]) { fclose(d); throw std::runtime_error("the door list names area " + std::to_string(a) + ", which the map does not mark as one that can be shut"); } f.g.mutableArea[a] = 1; }
    fclose(d); }
  // the lists out, in and rev as the experiments build them must be the file's
  const auto before = f.g.edges; f.g.Index();
  for (U e = 0; e < E; ++e) if (before[e].from != f.g.edges[e].from || before[e].to != f.g.edges[e].to || before[e].ordinal != f.g.edges[e].ordinal || before[e].w != f.g.edges[e].w) throw std::runtime_error("map file: its reaches are not in the order of their areas and positions");
  if (f.g.out != out || f.g.in != inn || f.g.rev != rev) throw std::runtime_error("map file: its lists out, in or rev are not the ones its reaches give");
  // positions that give the file's Origin (the head says how)
  f.centres.assign(N, overlay_four::Point4()); f.geo.resize(E);
  for (U e = 0; e < E; ++e) { f.geo[e].start.x = 3.0 * double(origin[e]) + 1.5; if (origin[e] < 1 || origin[e] > 4000000 || s3c::NativeOrigin(f, f.g.edges[e].from, f.geo[e].start) != origin[e]) throw std::runtime_error("map file: an Origin that cannot be kept"); }
  { std::string name = path; const size_t slash = name.find_last_of("/\\"); if (slash != std::string::npos) name = name.substr(slash + 1); f.name = "raw-" + name; }
  { const char* fly = getenv("RT_FLY"); f.fly = fly && !strcmp(fly, "1"); }
  // the door census: the wide doors by the bridge rule, one door per area (the necks stay areas that can shut)
  const auto kinds = s3c::ClassifyDoors(f);
  for (U a = 0; a < N; ++a) if (kinds.loop[a]) { f.census.emplace_back(); f.census.back().name = "door_" + std::to_string(a); f.census.back().areas.push_back(a); }
  return f;
}
inline s3c::DoorClassification RawClassify(const overlay_four::Fixture4& f) {
  auto c = s3c::ClassifyDoors(f);
#ifdef DN_HIDE_NECKS
  c.forcedCount = 0;
#endif
  return c;
}
#define Native(side) RawMap(side)
#define ClassifyDoors RawClassify
