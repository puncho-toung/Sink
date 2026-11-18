The baked router's runtime (R2 router).

These headers are GENERATED: they are the route planner as it was developed and tested as standalone programs,
copied in by a script, unchanged but for include lines. Do not edit them here and do not run the code formatter
over them (the formatter leaves neo/libs alone): a parity test proves that this copy answers exactly as the
tested programs do, and it stops holding the moment the text changes. A change is made in the standalone source,
tested there, and copied in again. SOURCES.txt names the source of every header with its hash.

They are included by d3xp/ai/AAS_router.cpp and by nothing else (d3xp/ai/AAS_router.h says why).

Where the things their comments name live (outside this repository, in the project's working folder
claudespace/plans/single-map/math/R2/atlas/):
  engine-port/make_core.py, engine-port/core_test.cpp     the script that makes these headers; the parity test
  exp-active2/native-lists/pack4/                          nl_pack.h, nl_ask.h, nl_walk.h and COUNTING.md (the counted steps)
  exp-active2/native-lists/pack4/outdoor/doors/           DESIGN.md, BUILT.md; many/ (SPEC.md, COUNTING-DOORS.md, dm_pockets.h,
                                                           dc_bake.cpp), necks/ (the gate), lastleg/ (the door layer as copied here)
  ATLAS.md                                                 the cost of a reach (section 3.1)
