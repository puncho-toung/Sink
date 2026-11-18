#ifndef __ACOUSTIC_PHONON_H__
#define __ACOUSTIC_PHONON_H__

//puncho - SB0: the engine's only way to reach Steam Audio. phonon.dll is loaded by path while a tool
// command runs (dmap's bake, bakeAcoustics, phononInfo) and unloaded at its end; the executable links
// nothing of Steam Audio, so play never touches it and the game runs with no phonon.dll on disk.
// With IPLAPI empty outside the library's own build, a direct ipl* call fails at link: every call
// goes through this table.

// the one switch for this header and acoustic_phonon.cpp: the engine with the option on. rbdmap
// (DMAP) compiles these files too but links its own small system layer with no Sys_DLL_*, and never
// bakes, so there the loader is always the "built without" form.
#if defined( USE_STEAMAUDIO_BAKE ) && !defined( DMAP )
	#define PHONON_LOADER	1
#else
	#define PHONON_LOADER	0
#endif

#if PHONON_LOADER
	#include <phonon.h>
#endif

// the loader refuses a phonon.dll whose iplLocalPatchLevel() is missing or not exactly this:
// bit 0 = patch 0001, bit 1 = patch 0002. A patch change raises SND_BAKER_REVISION_B with it.
#define PHONON_PATCH_LEVEL	0x3

// filled by PhononApi_Load, zeroed by PhononApi_Unload and by every failed load.
// The member list is WP-SB0 R-SB0-15's, in its order; a new need is a change to that list.
struct phononApi_t
{
#if PHONON_LOADER
	decltype( &iplContextCreate )						ContextCreate;
	decltype( &iplContextRelease )						ContextRelease;
	decltype( &iplSceneCreate )							SceneCreate;
	decltype( &iplSceneRelease )						SceneRelease;
	decltype( &iplSceneCommit )							SceneCommit;
	decltype( &iplSceneSaveOBJ )						SceneSaveOBJ;
	decltype( &iplStaticMeshCreate )					StaticMeshCreate;
	decltype( &iplStaticMeshRelease )					StaticMeshRelease;
	decltype( &iplStaticMeshAdd )						StaticMeshAdd;
	decltype( &iplProbeBatchCreate )					ProbeBatchCreate;
	decltype( &iplProbeBatchRelease )					ProbeBatchRelease;
	decltype( &iplProbeBatchAddProbe )					ProbeBatchAddProbe;
	decltype( &iplProbeBatchCommit )					ProbeBatchCommit;
	decltype( &iplProbeBatchGetEnergyField )			ProbeBatchGetEnergyField;
	decltype( &iplEnergyFieldCreate )					EnergyFieldCreate;
	decltype( &iplEnergyFieldRelease )					EnergyFieldRelease;
	decltype( &iplEnergyFieldGetData )					EnergyFieldGetData;
	decltype( &iplEnergyFieldGetNumBins )				EnergyFieldGetNumBins;
	decltype( &iplEnergyFieldGetBand )					EnergyFieldGetBand;
	decltype( &iplReflectionsBakerBake )				ReflectionsBakerBake;
	decltype( &iplReflectionsBakerCancelBake )			ReflectionsBakerCancelBake;
	decltype( &iplReflectionsBakerGetLastProbeStats )	ReflectionsBakerGetLastProbeStats;
	decltype( &iplLocalPatchLevel )						LocalPatchLevel;
#endif
	intptr_t											handle;		// Sys_DLL_Load's module, 0 when not loaded
};

// main thread only. Load: <folder of Sys_EXEPath()>\phonon.dll, every member, the patch level.
// On any failure the module is unloaded, the table zeroed, error filled, and false returned.
// Never call Load on a loaded table: it zeroes the table first, so the live handle would be dropped and
// the module stay loaded for the life of the process. One load and one unload per command.
// Built without the loader (PHONON_LOADER 0), Load always fails with "built without the acoustic bake".
bool	PhononApi_Load( phononApi_t& api, idStr& error );
// call only after every Steam Audio object made through the table is released
void	PhononApi_Unload( phononApi_t& api );

// the phononInfo command (CMD_FL_TOOL)
void	PhononInfo_f( const idCmdArgs& args );

#endif /* !__ACOUSTIC_PHONON_H__ */
