// aasrouterbake: bakes the walk lists of the AAS router ( "<aas file>.walk.nlw4" ) from a router map
// ( "<aas file>.walk.rtg", written by the map compiler ). The map compiler starts this program after an AAS
// compile ( neo/tools/compilers/aas/AASBuild.cpp, BakeRouterLists ) and looks for it in the base path.
//
//   the router map's path in the environment variable RT_MAP, OD_DOORS=bridge, then
//   aasrouterbake 2 "128:4:512,256:8:3072,1024:8:0:4096" FB 1 0 <output file>
//
// It proves every ordered pair of areas before it writes, and writes "<output file>.certificate.txt" beside
// the lists. Exit code 0: the lists are written.
//
// The sources in src/ are generated copies of the standalone programs the router was developed and proven
// with. Do not edit or format them here: see README.txt and SOURCES.txt.
#include "src/rd_native.h"
#include "src/nl_bake.cpp"
