//puncho - TC0: appended compiler phase slots; legacy slots 0..16 never move.
#ifndef __COMPILER_PHASE_H__
#define __COMPILER_PHASE_H__

extern idCVar dmap_phaseTimes;

enum compilerPhase_t
{
	DP_WRITE_SURFACES = 17,
	DP_WRITE_FORMAT,
	DP_WRITE_FILE,
	DP_CM_BUILD,
	DP_CM_WRITE,
	DP_AAS_LOAD,
	DP_AAS_BRUSHES,
	DP_AAS_EXPAND,
	DP_AAS_BSP,
	DP_AAS_SUBDIVIDE,
	DP_AAS_STORE,
	DP_AAS_REACH,
	DP_AAS_CLUSTER,
	DP_AAS_OPTIMIZE,
	DP_AAS_WRITE,
	DP_COUNT
};

// Call only under the caller's captured dmap_phaseTimes flag.
void DmapPhaseAdd( compilerPhase_t phase, int milliseconds );
int DmapPhaseMilliseconds( compilerPhase_t phase );

#endif
