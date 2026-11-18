aasrouterbake: the bake program of the AAS router's lists
==========================================================

What it is
  The game's baked router ( cvar aas_router, neo/d3xp/ai/AAS_router.cpp, neo/libs/aasrouter/ ) reads a lists
  file beside each AAS file: "<aas file>.walk.nlw4". This program makes that file from a router map
  ( "<aas file>.walk.rtg" ), which the map compiler writes. After an AAS compile the map compiler starts
  this program by itself ( neo/tools/compilers/aas/AASBuild.cpp, BakeRouterLists; cvar aas_routerBake )
  and then removes the router map and the certificate again: the lists are the only file a compile leaves.
  The command "rbdmap -nogui routerMap <map>" writes the router maps of a map's AAS files and keeps them.
  The map compiler looks for this program in the base path under the name aasrouterbake ( aasrouterbake.exe
  on Windows ); the build copies the program there ( CMakeLists.txt in this folder ).

  By hand:  set RT_MAP=<file.rtg> and OD_DOORS=bridge in the environment, then
            aasrouterbake 2 "128:4:512,256:8:3072,1024:8:0:4096" FB 1 0 <output file>

The sources
  src/ holds generated copies of the standalone programs the router was developed and proven with. They are
  written for a single translation unit: aasrouterbake.cpp includes them, and nothing in src/ is compiled by
  itself ( nl_bake.cpp and census.cpp are included like headers ). SOURCES.txt lists where each file comes
  from, with a hash of the source. The copies differ from their sources only in include lines ( the sources
  lie in several folders ) and in two lines of fixtures4.h ( an absolute path made relative; that code is
  not reached by this program ).

  DO NOT EDIT OR FORMAT THE FILES IN src/. A change is made in the standalone sources, proven there, and
  copied here again by the script that made these copies.

  nl_ask.h and nl_walk.h are also in neo/libs/aasrouter/, where the game uses them. Both sets come from the
  same sources; the copies here include the bake's own chain of headers.

Building
  One source file, C++17, OpenMP. With GCC the program builds as it is. With Visual Studio it needs
  /openmp:llvm ( the sources use OpenMP 3.1 ) and the header compat/bits/stdc++.h, which stands in for the
  header of that name that GCC has. A Visual Studio build needs libomp140.x86_64.dll at run time; it comes
  with Visual Studio.

Proof of a build
  The lists a build writes for a router map must be the same bytes as the lists of the build before it.
  Checked for the first Visual Studio build against the MinGW build on two maps ( 651 and 9,770 areas ).
