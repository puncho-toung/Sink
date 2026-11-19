//puncho - SB0: the phonon.dll loader and the phononInfo command (WP-SB0 R-SB0-14 to R-SB0-18).
#include "precompiled.h"
#pragma hdrstop

#include "acoustic_phonon.h"

#if PHONON_LOADER

/*
===============
PhononApi_Path

the folder of the executable, never the working directory: the debugger's is the repo root
===============
*/
static void PhononApi_Path( idStr& path )
{
	path = Sys_EXEPath();
	path.StripFilename();
	// LoadLibrary wants backslashes in a path
	path += "\\phonon.dll";
}

/*
===============
PhononApi_Fail
===============
*/
static bool PhononApi_Fail( phononApi_t& api, idStr& error, const char* text )
{
	error = text;
	PhononApi_Unload( api );
	return false;
}

#endif

/*
===============
PhononApi_Load
===============
*/
bool PhononApi_Load( phononApi_t& api, idStr& error )
{
	memset( &api, 0, sizeof( api ) );
#if PHONON_LOADER
	idStr path;
	PhononApi_Path( path );

	api.handle = Sys_DLL_Load( path.c_str() );
	if( api.handle == 0 )
	{
		// only on the failure path: tell a missing file from one Windows would not load
		if( Sys_IsFolder( path.c_str() ) == FOLDER_ERROR )
		{
			return PhononApi_Fail( api, error, va( "%s not found", path.c_str() ) );
		}
		return PhononApi_Fail( api, error, va( "%s could not be loaded", path.c_str() ) );
	}

	// the level first: an unpatched or 0001-only build also lacks iplReflectionsBakerGetLastProbeStats,
	// and must read as a wrong level, not as a missing export. Plain equality also refuses
	// phonon_measure.dll, whose level carries bit 31.
	api.LocalPatchLevel = ( decltype( api.LocalPatchLevel ) )Sys_DLL_GetProcAddress( api.handle, "iplLocalPatchLevel" );
	if( api.LocalPatchLevel == NULL )
	{
		return PhononApi_Fail( api, error, va( "patch level missing or not 0x%x: %s has no iplLocalPatchLevel", PHONON_PATCH_LEVEL, path.c_str() ) );
	}
	const IPLuint32 level = api.LocalPatchLevel();
	if( level != PHONON_PATCH_LEVEL )
	{
		return PhononApi_Fail( api, error, va( "patch level missing or not 0x%x: %s reports 0x%x", PHONON_PATCH_LEVEL, path.c_str(), level ) );
	}

	const char* missing = NULL;
#define PHONON_GET( name ) \
	if( missing == NULL ) \
	{ \
		api.name = ( decltype( api.name ) )Sys_DLL_GetProcAddress( api.handle, "ipl" #name ); \
		if( api.name == NULL ) \
		{ \
			missing = "ipl" #name; \
		} \
	}
	PHONON_GET( ContextCreate )
	PHONON_GET( ContextRelease )
	PHONON_GET( SceneCreate )
	PHONON_GET( SceneRelease )
	PHONON_GET( SceneCommit )
	PHONON_GET( SceneSaveOBJ )
	PHONON_GET( StaticMeshCreate )
	PHONON_GET( StaticMeshRelease )
	PHONON_GET( StaticMeshAdd )
	PHONON_GET( ProbeBatchCreate )
	PHONON_GET( ProbeBatchRelease )
	PHONON_GET( ProbeBatchAddProbe )
	PHONON_GET( ProbeBatchCommit )
	PHONON_GET( ProbeBatchGetEnergyField )
	PHONON_GET( EnergyFieldCreate )
	PHONON_GET( EnergyFieldRelease )
	PHONON_GET( EnergyFieldGetData )
	PHONON_GET( EnergyFieldGetNumBins )
	PHONON_GET( EnergyFieldGetBand )
	PHONON_GET( ReflectionsBakerBake )
	PHONON_GET( ReflectionsBakerCancelBake )
	PHONON_GET( ReflectionsBakerGetLastProbeStats )
#undef PHONON_GET
	if( missing != NULL )
	{
		return PhononApi_Fail( api, error, va( "%s has no export %s", path.c_str(), missing ) );
	}
	return true;
#else
	error = "built without the acoustic bake";
	return false;
#endif
}

/*
===============
PhononApi_Unload
===============
*/
void PhononApi_Unload( phononApi_t& api )
{
#if PHONON_LOADER
	if( api.handle != 0 )
	{
		Sys_DLL_Unload( api.handle );
	}
#endif
	memset( &api, 0, sizeof( api ) );
}

/*
===============
PhononInfo_f

load, create and release one context, unload; prints the three times in milliseconds (M-SB0-15).
Writes no file.
===============
*/
void PhononInfo_f( const idCmdArgs& args )
{
	phononApi_t	api;
	idStr		error;
#if PHONON_LOADER
	const uint64 loadStart = Sys_Microseconds();
	if( !PhononApi_Load( api, error ) )
	{
		common->Printf( "phonon: %s\n", error.c_str() );
		return;
	}
	const uint64 contextStart = Sys_Microseconds();

	IPLContextSettings settings;
	memset( &settings, 0, sizeof( settings ) );
	settings.version = STEAMAUDIO_VERSION;
	IPLContext context = NULL;
	const IPLerror status = api.ContextCreate( &settings, &context );
	if( status == IPL_STATUS_SUCCESS )
	{
		api.ContextRelease( &context );
	}
	const uint64 unloadStart = Sys_Microseconds();

	// read before the unload zeroes the table
	const IPLuint32 level = api.LocalPatchLevel();
	idStr path;
	PhononApi_Path( path );

	PhononApi_Unload( api );
	const uint64 unloadEnd = Sys_Microseconds();

	if( status != IPL_STATUS_SUCCESS )
	{
		common->Printf( "phonon: %s: iplContextCreate failed (IPLerror %d)\n", path.c_str(), ( int )status );
		return;
	}
	common->Printf( "phonon: %s %d.%d.%d patch 0x%x load %.3f context %.3f unload %.3f\n", path.c_str(),
					STEAMAUDIO_VERSION_MAJOR, STEAMAUDIO_VERSION_MINOR, STEAMAUDIO_VERSION_PATCH, level,
					( contextStart - loadStart ) * 0.001, ( unloadStart - contextStart ) * 0.001, ( unloadEnd - unloadStart ) * 0.001 );
#else
	PhononApi_Load( api, error );
	common->Printf( "phonon: %s\n", error.c_str() );
#endif
}
