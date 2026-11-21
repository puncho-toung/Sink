/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013-2014 Robert Beckebans
Copyright (C) 2016-2017 Dustin Land
Copyright (C) 2022 Stephen Pridham

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#include "precompiled.h"
#pragma hdrstop

#include "RenderCommon.h"
#include "sys/DeviceManager.h"

idVertexCache vertexCache;

idCVar r_showVertexCache( "r_showVertexCache", "0", CVAR_RENDERER | CVAR_BOOL, "Print stats about the vertex cache every frame" );
idCVar r_showVertexCacheTimings( "r_showVertexCacheTimings", "0", CVAR_RENDERER | CVAR_BOOL, "Print stats about the vertex cache every frame" );

//puncho start - static arena (P2b.1.8)
idCVar r_staticArenaSplit( "r_staticArenaSplit", "0", CVAR_RENDERER | CVAR_BOOL, "1: two-ended static sets; a game-driven load at com_levelKeep >= 1 keeps the persistent models' static buffers (latched at each level load)" );
//puncho start - static index (2R R3.3): the caps of the two static sets. A set starts at its old fixed size and grows by copy when a
// level needs more, up to its cap; it never shrinks while the process runs. Not CVAR_ARCHIVE: a cap lowered for a test would stay
// in the config file and end the next large level in the cap's fatal error
idCVar r_staticIndexMB( "r_staticIndexMB", "4095", CVAR_RENDERER | CVAR_INTEGER | CVAR_NOCHEAT, "the most MiB of static index data a level may hold (read at each level load; the buffer grows up to it and never shrinks: a smaller buffer needs a restart)", STATIC_SET_MIN_MB, STATIC_SET_MAX_MB );
idCVar r_staticVertexMB( "r_staticVertexMB", "1024", CVAR_RENDERER | CVAR_INTEGER | CVAR_NOCHEAT, "the most MiB of static vertex data a level may hold (read at each level load; the buffer grows up to it and never shrinks: a smaller buffer needs a restart)", STATIC_SET_MIN_MB, STATIC_SET_MAX_MB );
//puncho end
#if !defined( ID_RETAIL )
idCVar r_staticArenaTestCapMB( "r_staticArenaTestCapMB", "0", CVAR_RENDERER | CVAR_FLOAT, "test only: the set size in MB the static arena's preflight decides against (0: the set's own); allocation keeps the whole set" );
idCVar r_staticTestStale( "r_staticTestStale", "0", CVAR_RENDERER | CVAR_BOOL, "test only: the next valid static handle CacheIsCurrent tests reads as stale once (split 1): it is counted and the caller re-uploads it" );
//puncho start - static index (2R R3.3): test instruments for the growth. They act at every level load while they are set, the main
// menu's own load too: set them in the console once the menu is up
idCVar r_staticArenaTestPadMB( "r_staticArenaTestPadMB", "", CVAR_RENDERER, "test only: \"<index MiB> <vertex MiB>\": empty blocks placed before a level load's uploads (split 0), so the level's own blocks start behind them" );
idCVar r_staticArenaTestFillMB( "r_staticArenaTestFillMB", "", CVAR_RENDERER, "test only: \"<a> <b> [force]\": empty blocks fill the index set down to <a> MiB free after a level load's uploads and to <b> MiB free after its interactions (-1: no fill); force 1 (with <a> 0 to 2) then asks for one block more than is free, which grows the set" );
idCVar r_staticArenaTestFlushKB( "r_staticArenaTestFlushKB", "0", CVAR_RENDERER | CVAR_INTEGER, "test only: above 0, the upload loops execute their command list each time this many KiB of static uploads wait in it, in place of 32 MiB", 0, 1048576 );
static const int STATIC_TEST_BLOCK_BYTES = 2 * 1024 * 1024;
//puncho end
#endif

//puncho start - static index (2R R3.3)
/*
==============
StaticCapCvarMB

A static set's cap as its cvar asks for it now, in MiB; idVertexCache latches it at Init and at each level load.
==============
*/
static int StaticCapCvarMB( cacheType_t type )
{
	return idMath::ClampInt( STATIC_SET_MIN_MB, STATIC_SET_MAX_MB, ( type == CACHE_INDEX ) ? r_staticIndexMB.GetInteger() : r_staticVertexMB.GetInteger() );
}

static const char* StaticSetName( cacheType_t type )
{
	return ( type == CACHE_INDEX ) ? "index" : "vertex";
}

/*
==============
ListStaticGrowth

listStaticSegments' lines at both splits: each set's cap and what its growth did, and the window.
==============
*/
static void ListStaticGrowth()
{
	static const char* windowNames[] = { "closed", "load", "slice" };
	for( int type = CACHE_VERTEX; type <= CACHE_INDEX; type++ )
	{
		const staticArenaStats_t& set = vertexCache.staticSets[ type ];
		idLib::Printf( "%s set: cap %d MiB (cvar %d); most held %lld, late %lld, lateRuntime %lld bytes; this load: growths %d, skipped %d, copied %lld, last transient %lld bytes; largest transient %lld bytes\n",
					   StaticSetName( ( cacheType_t )type ), set.capMB, StaticCapCvarMB( ( cacheType_t )type ), ( long long )set.highLoad, ( long long )set.late, ( long long )set.lateRuntime,
					   set.growths, set.growthSkips, ( long long )set.copied, ( long long )set.lastTransient, ( long long )set.maxTransient );
	}
	idLib::Printf( "static window: %s, load %d\n", windowNames[ vertexCache.staticWindow ], vertexCache.staticEpoch );
}
//puncho end

CONSOLE_COMMAND( listStaticSegments, "lists the static arena's segments: bytes used, high-water, generation, models", NULL )
{
	const staticArenaStats_t& vertex = vertexCache.staticSets[ CACHE_VERTEX ];
	const staticArenaStats_t& index = vertexCache.staticSets[ CACHE_INDEX ];
	idLib::Printf( "static arena: split %d, staticStaleHandles %d, resets %d\n", vertexCache.staticSplit ? 1 : 0,
				   vertexCache.staticStaleHandles.GetValue(), vertexCache.staticResets );
	//puncho start - static index (2R R3.1): bytes through the accessors (the sets are counted in 16-byte units)
	if( !vertexCache.staticSplit )
	{
		idLib::Printf( "one set: vtx %lld/%lld idx %lld/%lld bytes\n", ( long long )vertexCache.StaticUsedBytes( CACHE_VERTEX ), ( long long )vertexCache.StaticSizeBytes( CACHE_VERTEX ),
					   ( long long )vertexCache.StaticUsedBytes( CACHE_INDEX ), ( long long )vertexCache.StaticSizeBytes( CACHE_INDEX ) );
		ListStaticGrowth();	//puncho - static index (2R R3.3)
		return;
	}
	for( int segment = 0; segment < 2; segment++ )
	{
		idLib::Printf( "segment %d: generation %d vtx %lld bytes (high %lld) idx %lld bytes (high %lld) models %d\n", segment,
					   vertexCache.staticGenerations[ segment ], ( long long )vertexCache.StaticSegmentBytes( CACHE_VERTEX, segment ), ( long long )vertex.high[ segment ],
					   ( long long )vertexCache.StaticSegmentBytes( CACHE_INDEX, segment ), ( long long )index.high[ segment ], vertexCache.staticSegmentModels[ segment ] );
	}
	idLib::Printf( "free: vtx %lld/%lld idx %lld/%lld bytes; late: vtx %lld idx %lld bytes\n",
				   ( long long )( vertexCache.StaticSizeBytes( CACHE_VERTEX ) - vertexCache.StaticSegmentBytes( CACHE_VERTEX, 0 ) - vertexCache.StaticSegmentBytes( CACHE_VERTEX, 1 ) ),
				   ( long long )vertexCache.StaticSizeBytes( CACHE_VERTEX ),
				   ( long long )( vertexCache.StaticSizeBytes( CACHE_INDEX ) - vertexCache.StaticSegmentBytes( CACHE_INDEX, 0 ) - vertexCache.StaticSegmentBytes( CACHE_INDEX, 1 ) ),
				   ( long long )vertexCache.StaticSizeBytes( CACHE_INDEX ), ( long long )vertex.late, ( long long )index.late );
	//puncho end
	ListStaticGrowth();	//puncho - static index (2R R3.3)
}
//puncho end

extern DeviceManager* deviceManager;

/*
==============
ClearGeoBufferSet
==============
*/
static void ClearGeoBufferSet( geoBufferSet_t& gbs )
{
	gbs.indexMemUsed.SetValue( 0 );
	gbs.vertexMemUsed.SetValue( 0 );
	gbs.jointMemUsed.SetValue( 0 );
	gbs.allocations = 0;
}

/*
==============
MapGeoBufferSet
==============
*/
static void MapGeoBufferSet( geoBufferSet_t& gbs )
{
	if( gbs.mappedVertexBase == NULL )
	{
		gbs.mappedVertexBase = ( byte* )gbs.vertexBuffer.MapBuffer( BM_WRITE );
	}

	if( gbs.mappedIndexBase == NULL )
	{
		gbs.mappedIndexBase = ( byte* )gbs.indexBuffer.MapBuffer( BM_WRITE );
	}

	if( gbs.mappedJointBase == NULL && gbs.jointBuffer.GetAllocedSize() != 0 )
	{
		gbs.mappedJointBase = ( byte* )gbs.jointBuffer.MapBuffer( BM_WRITE );
	}
}

/*
==============
UnmapGeoBufferSet
==============
*/
static void UnmapGeoBufferSet( geoBufferSet_t& gbs )
{
	if( gbs.mappedVertexBase != NULL )
	{
		gbs.vertexBuffer.UnmapBuffer();
		gbs.mappedVertexBase = NULL;
	}

	if( gbs.mappedIndexBase != NULL )
	{
		gbs.indexBuffer.UnmapBuffer();
		gbs.mappedIndexBase = NULL;
	}

	if( gbs.mappedJointBase != NULL )
	{
		gbs.jointBuffer.UnmapBuffer();
		gbs.mappedJointBase = NULL;
	}
}

/*
==============
AllocGeoBufferSet
==============
*/
//puncho start - static index (2R R3.2): int64 sizes, and a refused buffer is reported. False when the device refuses one of
// the set's buffers: the ones made before it are freed again, and failedKind and failedBytes name the refused one
static bool AllocGeoBufferSet( geoBufferSet_t& gbs, const int64 vertexBytes, const int64 indexBytes, const int64 jointBytes, bufferUsageType_t usage, nvrhi::ICommandList* commandList, const char*& failedKind, int64& failedBytes )
{
	if( !gbs.vertexBuffer.AllocBufferObject( NULL, vertexBytes, usage, commandList ) )
	{
		failedKind = "vertex";
		failedBytes = vertexBytes;
		return false;
	}
	if( !gbs.indexBuffer.AllocBufferObject( NULL, indexBytes, usage, commandList ) )
	{
		gbs.vertexBuffer.FreeBufferObject();
		failedKind = "index";
		failedBytes = indexBytes;
		return false;
	}
	if( jointBytes > 0 && !gbs.jointBuffer.AllocBufferObject( NULL, jointBytes, usage, commandList ) )
	{
		gbs.indexBuffer.FreeBufferObject();
		gbs.vertexBuffer.FreeBufferObject();
		failedKind = "joint";
		failedBytes = jointBytes;
		return false;
	}

	ClearGeoBufferSet( gbs );
	return true;
}
//puncho end

/*
==============
idVertexCache::Init
==============
*/
void idVertexCache::Init( int _uniformBufferOffsetAlignment, nvrhi::ICommandList* commandList )
{
	currentFrame = 0;
	listNum = 0;

	uniformBufferOffsetAlignment = _uniformBufferOffsetAlignment;

	mostUsedVertex = 0;
	mostUsedIndex = 0;
	mostUsedJoint = 0;

	nvrhi::CommandListParameters parms;
	parms.setQueueType( nvrhi::CommandQueue::Copy );

	//puncho start - static index (2R R3.2): a refused buffer ends the start with a message that names it. Before, the
	// result was ignored and the first use of the missing buffer failed
	const char* failedKind = "";
	int64 failedBytes = 0;
	for( int i = 0; i < NUM_FRAME_DATA; i++ )
	{
		if( !AllocGeoBufferSet( frameData[i], VERTCACHE_VERTEX_MEMORY_PER_FRAME, VERTCACHE_INDEX_MEMORY_PER_FRAME, VERTCACHE_JOINT_MEMORY_PER_FRAME, BU_DYNAMIC, commandList, failedKind, failedBytes ) )
		{
			Shutdown();
			// the text stays under 80 characters with any size: the Windows error box shows no more (seen in the game run)
			idLib::FatalError( "idVertexCache::Init: frame set %d: %s buffer of %lld bytes refused", i, failedKind, ( long long )failedBytes );
		}
	}
	//puncho - static index (2R R3.3): a static set starts at its old fixed size, or at its cap when that is smaller, and grows from
	// there (GrowStaticSet). The sets are BU_STATIC only: a mapped set could not grow, its mapped base pointers would have
	// to move with the buffer (the BU_DYNAMIC variant that stood here under #else was dead code)
	const int64 staticVertexBytes = Min( ( int64 )StaticCapCvarMB( CACHE_VERTEX ) << 20, ( int64 )STATIC_VERTEX_MEMORY );
	const int64 staticIndexBytes = Min( ( int64 )StaticCapCvarMB( CACHE_INDEX ) << 20, ( int64 )STATIC_INDEX_MEMORY );
	if( !AllocGeoBufferSet( staticData, staticVertexBytes, staticIndexBytes, 0, BU_STATIC, commandList, failedKind, failedBytes ) )
	{
		Shutdown();
		idLib::FatalError( "idVertexCache::Init: static set: %s buffer of %lld bytes refused", failedKind, ( long long )failedBytes );
	}
	//puncho end

	//puncho start - static arena (P2b.1.8): split 0 until the first level load latches r_staticArenaSplit; generation 0
	// everywhere, so no handle is valid before its segment's first free
	memset( staticSets, 0, sizeof( staticSets ) );
	//puncho - static index (2R R3.1): the arena counts 16-byte units
	staticSets[ CACHE_VERTEX ].arena.size = ( int )( staticVertexBytes >> STATIC_OFFSET_UNIT_BITS );
	staticSets[ CACHE_INDEX ].arena.size = ( int )( staticIndexBytes >> STATIC_OFFSET_UNIT_BITS );
	staticSets[ CACHE_VERTEX ].mark = -1;
	staticSets[ CACHE_INDEX ].mark = -1;
	//puncho start - static index (2R R3.3): the caps as the cvars ask now; no window is open and no load has run
	staticSets[ CACHE_VERTEX ].markLoad = -1;
	staticSets[ CACHE_INDEX ].markLoad = -1;
	staticSets[ CACHE_VERTEX ].capMB = StaticCapCvarMB( CACHE_VERTEX );
	staticSets[ CACHE_INDEX ].capMB = StaticCapCvarMB( CACHE_INDEX );
	staticWindow = STATIC_WINDOW_CLOSED;
	staticEpoch = 0;
	staticDiscard = false;
	staticPhase = "start";
	staticUploadList = NULL;
	staticUploadBytes = 0;
	staticFlushes = 0;
	//puncho end
	memset( staticGenerations, 0, sizeof( staticGenerations ) );
	staticSegment = 1;
	staticSplit = false;
	staticSplitChanged = false;
	staticKeepPersistent = false;
	staticMeasure = false;
	staticReports = 0;
	staticSegmentModels[0] = 0;
	staticSegmentModels[1] = 0;
	staticResets = 0;
	staticUploadsRetained = 0;
	staticUploadsNew = 0;
	staticRetainedBytes = 0;
	staticNewBytes = 0;
	staticStaleHandles.SetValue( 0 );
	staticTestStale.SetValue( 0 );
	//puncho end

	MapGeoBufferSet( frameData[ listNum ] );
}

/*
==============
idVertexCache::Shutdown
==============
*/
void idVertexCache::Shutdown()
{
	for( int i = 0; i < NUM_FRAME_DATA; i++ )
	{
		frameData[i].vertexBuffer.FreeBufferObject();
		frameData[i].indexBuffer.FreeBufferObject();
		frameData[i].jointBuffer.FreeBufferObject();
	}

	// SRS - free static buffers to avoid Vulkan validation layer errors on shutdown
	staticData.vertexBuffer.FreeBufferObject();
	staticData.indexBuffer.FreeBufferObject();
	staticData.jointBuffer.FreeBufferObject();
}

/*
==============
idVertexCache::PurgeAll
==============
*/
void idVertexCache::PurgeAll( nvrhi::ICommandList* commandList )
{
	Shutdown();
	Init( uniformBufferOffsetAlignment, commandList );
}

/*
==============
idVertexCache::FreeStaticData

call on loading a new map
==============
*/
void idVertexCache::FreeStaticData()
{
	ClearGeoBufferSet( staticData );
	//puncho - static arena (P2b.1.8): both segments, each with a new generation
	FreeStaticSegment( 0 );
	FreeStaticSegment( 1 );
	mostUsedVertex = 0;
	mostUsedIndex = 0;
	mostUsedJoint = 0;
}

//puncho start - static arena (P2b.1.8)
/*
==============
idVertexCache::BeginStaticLoad

Called by BeginLevelLoad. r_staticArenaSplit is latched here, so a change takes effect at this
load, which frees both segments; and since a split 0 handle's frame field can read as any segment
and generation, no handle is valid until this load's upload loops end (EndStaticUploads).
==============
*/
void idVertexCache::BeginStaticLoad( bool keepPersistent )
{
	//puncho start - static index (2R R3.3): a load or a slice that did not end (an error that did not pass idCommonLocal::Stop) is
	// cancelled here: its figures are dropped, and this load keeps no segment
	if( staticWindow != STATIC_WINDOW_CLOSED )
	{
		idLib::Warning( "idVertexCache::BeginStaticLoad: the static window of load %d was still open: cancelled", staticEpoch );
		CancelStaticLoad();
	}
	//puncho end
	// what the previous load allocated after its upload loops (its interactions' light tris) is the late use
	for( int type = CACHE_VERTEX; type <= CACHE_INDEX; type++ )
	{
		staticArenaStats_t& set = staticSets[ type ];
		const int64 used = StaticUsedBytes( ( cacheType_t )type ) - set.tail0;	//puncho - static index (2R R3.1): bytes as the byte counter held them (the counters hold units; without segment 0's tail)
		//puncho start - static index (2R R3.3): test blocks are no use. What came after the load had ended (the interaction slices)
		// is the late runtime use, which the next load's top-up makes room for. The caps are latched here
		const int64 synth = set.synth[0] + set.synth[1];
		// the load that ends here: its own two figures (-1: no sample), printed beside the most of each since the start
		const int64 lateNow = ( set.mark >= 0 ) ? used - set.mark - ( synth - set.synthMark ) : -1;
		const int64 runtimeNow = ( set.markLoad >= 0 ) ? StaticUsedBytes( ( cacheType_t )type ) - set.markLoad - ( synth - set.synthMarkLoad ) : -1;
		if( set.mark >= 0 )
		{
			set.late = Max( set.late, lateNow );
		}
		if( set.markLoad >= 0 )
		{
			set.lateRuntime = Max( set.lateRuntime, runtimeNow );
		}
		if( set.mark >= 0 || set.markLoad >= 0 )
		{
			idLib::Printf( "static %s after load %d: late %lld, runtime %lld bytes; most so far: late %lld, runtime %lld\n", StaticSetName( ( cacheType_t )type ), staticEpoch,
						   ( long long )lateNow, ( long long )runtimeNow, ( long long )set.late, ( long long )set.lateRuntime );
		}
		set.mark = -1;
		set.markLoad = -1;
		set.previousHigh = set.highLoad;
		set.capMB = StaticCapCvarMB( ( cacheType_t )type );
		//puncho end
	}
	staticUploadsRetained = 0;
	staticUploadsNew = 0;
	staticRetainedBytes = 0;
	staticNewBytes = 0;
	staticSegmentModels[0] = 0;
	staticSegmentModels[1] = 0;

	const bool split = r_staticArenaSplit.GetBool();
	staticSplitChanged = ( split != staticSplit );
	if( split && !staticSplitChanged && keepPersistent && !staticDiscard )	//puncho - static index (2R R3.3): nothing is kept after a cancelled load
	{
		FreeStaticSegment( 1 );
		mostUsedVertex = 0;
		mostUsedIndex = 0;
		mostUsedJoint = 0;
	}
	else
	{
		staticSplit = split;
		FreeStaticData();
	}
	// read per model by StaticSegmentForModel: at split 0 it needs no model call
	staticKeepPersistent = staticSplit && keepPersistent;
	staticSegment = 1;
	staticMeasure = false;
	//puncho start - static index (2R R3.3): the load's window opens and its figures start. At split 1 a set is sized here, once,
	// from the load before: what that load held at most plus the late runtime use, at least the start size and at most the
	// cap (during the load a set cannot grow at split 1: segment 1's blocks lie at the set's end). The resize is optional
	staticDiscard = false;
	staticEpoch++;
	staticWindow = STATIC_WINDOW_LOAD;
	staticPhase = "load";
	staticUploadList = NULL;
	staticFlushes = 0;
	for( int type = CACHE_VERTEX; type <= CACHE_INDEX; type++ )
	{
		staticArenaStats_t& set = staticSets[ type ];
		set.growths = 0;
		set.growthSkips = 0;
		set.copied = 0;
		set.lastTransient = 0;
		set.resizeSkipped = 0;
		set.highLoad = StaticUsedBytes( ( cacheType_t )type ) - set.synth[0] - set.synth[1];
	}
	idLib::Printf( "static sets: load %d, split %d, caps vtx %d idx %d MiB, allocated vtx %lld idx %lld bytes\n", staticEpoch, staticSplit ? 1 : 0, staticSets[ CACHE_VERTEX ].capMB,
				   staticSets[ CACHE_INDEX ].capMB, ( long long )StaticSizeBytes( CACHE_VERTEX ), ( long long )StaticSizeBytes( CACHE_INDEX ) );
	if( staticSplit )
	{
		staticPhase = "split-resize";
		for( int type = CACHE_VERTEX; type <= CACHE_INDEX; type++ )
		{
			staticArenaStats_t& set = staticSets[ type ];
			const int64 align = ( type == CACHE_INDEX ) ? INDEX_CACHE_ALIGN : VERTEX_CACHE_ALIGN;
			const int64 size = StaticSizeBytes( ( cacheType_t )type );
			const int64 cap = StaticCapBytes( ( cacheType_t )type );
			const int64 start = Min( cap, ( int64 )( ( type == CACHE_INDEX ) ? STATIC_INDEX_MEMORY : STATIC_VERTEX_MEMORY ) );
			const int64 target = Max( size, Min( cap, Max( start, ALIGN( set.previousHigh + set.lateRuntime, align ) ) ) );
			if( target > size && !GrowStaticSet( ( cacheType_t )type, target, NULL, false ) )
			{
				set.resizeSkipped = target;
			}
		}
		staticPhase = "load";
	}
	//puncho end
}

/*
==============
idVertexCache::CancelStaticLoad

idCommonLocal::Stop calls it: an error, a timeout, leaving the game (static index, 2R R3.3). With no window open it does
nothing, so a normal unload keeps the finished load's figures. A load or a slice that was still open is given up: its
figures are not harvested, and since segment 0 may hold half a load, the next load keeps no segment.
==============
*/
void idVertexCache::CancelStaticLoad()
{
	if( staticWindow == STATIC_WINDOW_CLOSED )
	{
		return;
	}
	idLib::Printf( "static sets: load %d cancelled (%s)\n", staticEpoch, staticPhase );
	staticWindow = STATIC_WINDOW_CLOSED;
	staticPhase = "cancelled";
	staticUploadList = NULL;
	staticDiscard = true;
	for( int type = CACHE_VERTEX; type <= CACHE_INDEX; type++ )
	{
		staticSets[ type ].mark = -1;
		staticSets[ type ].markLoad = -1;
		staticSets[ type ].highLoad = 0;
	}
}

/*
==============
idVertexCache::BeginStaticSlice, EndStaticSlice

Around FinishInteractionSlice's command list (static index, 2R R3.3): light tris made after a load may grow a set too.
EndStaticSlice comes after the list was executed. A level load's own window is left as it is.
==============
*/
void idVertexCache::BeginStaticSlice()
{
	if( staticWindow == STATIC_WINDOW_CLOSED )
	{
		staticWindow = STATIC_WINDOW_SLICE;
		staticPhase = "slice";
	}
}

void idVertexCache::EndStaticSlice()
{
	if( staticWindow == STATIC_WINDOW_SLICE )
	{
		staticWindow = STATIC_WINDOW_CLOSED;
		staticPhase = "closed";
	}
	staticUploadList = NULL;
}

/*
==============
idVertexCache::SetStaticSegment
==============
*/
void idVertexCache::SetStaticSegment( int segment )
{
	assert( segment == 0 || segment == 1 );
	staticSegment = segment;
}

/*
==============
idVertexCache::FreeStaticSegment

Resets one segment in both sets; its new generation makes every handle into it stale.
==============
*/
void idVertexCache::FreeStaticSegment( int segment )
{
	assert( segment == 0 || segment == 1 );
	assert( staticGenerations[ segment ] >= 0 && staticGenerations[ segment ] <= STATIC_ARENA_MAX_GENERATION );
	StaticArenaFree( staticSets[ CACHE_VERTEX ].arena, segment );
	StaticArenaFree( staticSets[ CACHE_INDEX ].arena, segment );
	//puncho start - static index (2R R3.1): segment 0's padding figure goes with it, and the counters mirror the arena
	// at both splits now (in units)
	if( segment == 0 )
	{
		staticSets[ CACHE_VERTEX ].tail0 = 0;
		staticSets[ CACHE_INDEX ].tail0 = 0;
	}
	//puncho - static index (2R R3.3): the segment's test blocks go with it
	staticSets[ CACHE_VERTEX ].synth[ segment ] = 0;
	staticSets[ CACHE_INDEX ].synth[ segment ] = 0;
	staticGenerations[ segment ] = StaticArenaNextGeneration( staticGenerations[ segment ] );
	staticData.vertexMemUsed.SetValue( staticSets[ CACHE_VERTEX ].arena.used0 + staticSets[ CACHE_VERTEX ].arena.used1 );
	staticData.indexMemUsed.SetValue( staticSets[ CACHE_INDEX ].arena.used0 + staticSets[ CACHE_INDEX ].arena.used1 );
	//puncho end
}

/*
==============
idVertexCache::StaticUploadNeeded

The two creation functions' first test. A block of no bytes allocates nothing (ActuallyAlloc
returns handle 0), so it does not force the other block's upload while its handle is 0. In the
preflight it reports instead: kept handles' bytes into live0 (only segment 0 can hold one,
BeginStaticLoad freed segment 1), the rest into incoming, both blocks ALIGN'd as the arena
places them.
==============
*/
bool idVertexCache::StaticUploadNeeded( vertCacheHandle_t indexHandle, int indexBytes, vertCacheHandle_t vertexHandle, int vertexBytes )
{
	// a block of no bytes is kept as an upload would leave it, at handle 0 (at split 0 only such calls are skipped)
	const bool indexKept = ( indexBytes > 0 ) ? StaticHandleValid( indexHandle ) : ( indexHandle == 0 );
	const bool vertexKept = ( vertexBytes > 0 ) ? StaticHandleValid( vertexHandle ) : ( vertexHandle == 0 );
	const bool kept = indexKept && vertexKept;
	const int indexFootprint = indexBytes > 0 ? ALIGN( indexBytes, INDEX_CACHE_ALIGN ) : 0;
	const int vertexFootprint = vertexBytes > 0 ? ALIGN( vertexBytes, VERTEX_CACHE_ALIGN ) : 0;
	// the upload loops count a model in its segment when one of its calls holds bytes (staticSegmentModels)
	staticReports += ( indexFootprint + vertexFootprint > 0 ) ? 1 : 0;
	if( staticMeasure )
	{
		if( kept )
		{
			staticSets[ CACHE_INDEX ].live0 += ( indexFootprint > 0 && StaticArenaSegment( indexHandle ) == 0 ) ? indexFootprint : 0;
			staticSets[ CACHE_VERTEX ].live0 += ( vertexFootprint > 0 && StaticArenaSegment( vertexHandle ) == 0 ) ? vertexFootprint : 0;
		}
		else
		{
			staticSets[ CACHE_INDEX ].incoming += indexFootprint;
			staticSets[ CACHE_VERTEX ].incoming += vertexFootprint;
		}
		return false;
	}
	if( kept )
	{
		return false;
	}
	// geometry holding a static handle was uploaded by an earlier load and is resident (at split 0 every such model
	// uploads again); without one (none, or the frontend's per-frame copy) it is new to the static sets
	if( CacheIsStatic( indexHandle ) || CacheIsStatic( vertexHandle ) )
	{
		staticUploadsRetained++;
		staticRetainedBytes += Max( indexBytes, 0 ) + Max( vertexBytes, 0 );
	}
	else
	{
		staticUploadsNew++;
		staticNewBytes += Max( indexBytes, 0 ) + Max( vertexBytes, 0 );
	}
	return true;
}

//puncho start - static index (2R R3.3): the test cvars' numbers
#if !defined( ID_RETAIL )
/*
==============
StaticTestNumbers

The whole numbers of a test cvar's text, separated by spaces (spaces before the first and behind the last are skipped):
each a decimal number from minimum to INT_MAX with nothing but a space or the text's end behind it. Returns how many
there are (0 for an empty text), or -1 when a part is not such a number or there are more than maxValues. The caller
uses values only when the count is the one it wants.
==============
*/
static int StaticTestNumbers( const char* text, int minimum, int* values, int maxValues )
{
	int count = 0;
	for( const char* p = text; ; )
	{
		while( *p == ' ' )
		{
			p++;
		}
		if( *p == '\0' )
		{
			return count;
		}
		char* end = NULL;
		const long long n = strtoll( p, &end, 10 );
		if( count == maxValues || end == p || ( *end != ' ' && *end != '\0' ) || n < minimum || n > INT_MAX )
		{
			return -1;
		}
		values[ count++ ] = ( int )n;
		p = end;
	}
}

/*
==============
StaticTestFillNumbers

r_staticArenaTestFillMB as three numbers: the MiB left free after the uploads and after the interactions (-1: no fill)
and force (0 or 1; 1 needs a first number from 0 to 2). False when the cvar is empty or not of this form.
==============
*/
static bool StaticTestFillNumbers( int fill[3] )
{
	fill[0] = -1;
	fill[1] = -1;
	fill[2] = 0;
	const int count = StaticTestNumbers( r_staticArenaTestFillMB.GetString(), -1, fill, 3 );
	if( count == 0 )
	{
		return false;
	}
	if( count < 2 || fill[2] < 0 || fill[2] > 1 || ( fill[2] == 1 && ( fill[0] < 0 || fill[0] > 2 ) ) )
	{
		idLib::Warning( "r_staticArenaTestFillMB: \"<a> <b> [force]\", MiB from -1, force 0 or 1 (with <a> 0 to 2); not used" );
		return false;
	}
	return true;
}
#endif
//puncho end

/*
==============
idVertexCache::BeginStaticPreflight
==============
*/
bool idVertexCache::BeginStaticPreflight()
{
	if( !staticSplit )
	{
		return false;
	}
	staticSets[ CACHE_VERTEX ].dead0 = 0;
	staticSets[ CACHE_INDEX ].dead0 = 0;
	// nothing to keep (every grace 0 load, the first after a split change): the walk cannot change the decision
	if( staticSets[ CACHE_VERTEX ].arena.used0 == 0 && staticSets[ CACHE_INDEX ].arena.used0 == 0 )
	{
		idLib::Printf( "static arena preflight: used0 0 in both sets, no walk; segment 0 empty (generation %d, 0 us)\n", staticGenerations[0] );
		return false;
	}
	staticSets[ CACHE_VERTEX ].live0 = 0;
	staticSets[ CACHE_VERTEX ].incoming = 0;
	staticSets[ CACHE_INDEX ].live0 = 0;
	staticSets[ CACHE_INDEX ].incoming = 0;
	staticReports = 0;
	staticMeasure = true;
	return true;
}

/*
==============
idVertexCache::BeginStaticUploads

Called by EndLevelLoad with its open command list, after the preflight and before the upload loops (static index, 2R
R3.3). Test only: the pads of r_staticArenaTestPadMB go in first, so that the level's own blocks start behind them
(split 0; a pad is a test block and no model use).
==============
*/
void idVertexCache::BeginStaticUploads( nvrhi::ICommandList* commandList )
{
	staticPhase = "uploads";
	staticUploadList = NULL;
#if !defined( ID_RETAIL )
	int padMB[2] = { 0, 0 };	// index, vertex
	const int count = StaticTestNumbers( r_staticArenaTestPadMB.GetString(), 0, padMB, 2 );
	if( count != 0 && count != 2 )
	{
		idLib::Warning( "r_staticArenaTestPadMB: two whole numbers from 0, \"<index MiB> <vertex MiB>\"; not used" );
	}
	else if( count == 2 && staticSplit && ( padMB[0] > 0 || padMB[1] > 0 ) )
	{
		idLib::Printf( "static test pad: not placed at r_staticArenaSplit 1\n" );
	}
	else if( count == 2 )
	{
		for( int i = 0; i < 2; i++ )
		{
			const cacheType_t type = ( i == 0 ) ? CACHE_INDEX : CACHE_VERTEX;
			for( int64 left = ( int64 )padMB[i] << 20; left > 0; left -= STATIC_TEST_BLOCK_BYTES )
			{
				AllocStaticArena( NULL, ( int )Min( left, ( int64 )STATIC_TEST_BLOCK_BYTES ), type, commandList, true );
			}
			if( padMB[i] > 0 )
			{
				idLib::Printf( "static test pad: %s set padded by %d MiB: %lld of %lld bytes used\n", StaticSetName( type ), padMB[i], ( long long )StaticUsedBytes( type ), ( long long )StaticSizeBytes( type ) );
			}
		}
	}
#endif
}

/*
==============
idVertexCache::EndStaticPreflight

Per set, with footprints ALIGN'd: dead0 = used0 - live0; segment 0 is reset when it holds bytes
and either more than 25 % of them are dead or ALIGN( used0 ) + used1 + incoming + late exceeds the
set's size. Kept, segment 0's frontier stays where it is, so the bytes this load needs are used0,
not live0: a reset is taken whenever keeping would run out at the forecast late (the largest seen;
a heavier map can still run out by up to dead0), and after one the load needs what the single
arena needed (live0 + incoming + late). r_staticArenaTestCapMB moves the decision only.
==============
*/
void idVertexCache::EndStaticPreflight( uint64 walkUs )
{
	staticMeasure = false;

	bool reset = false;
	bool holds = false;
	idStr line;
	for( int type = CACHE_VERTEX; type <= CACHE_INDEX; type++ )
	{
		staticArenaStats_t& set = staticSets[ type ];
		const int64 align = ( type == CACHE_INDEX ) ? INDEX_CACHE_ALIGN : VERTEX_CACHE_ALIGN;
		//puncho start - static index (2R R3.3): a load may use the set's size or its cap, whichever is less (after the cap was lowered
		// the set keeps its size). The test cap can only tighten that, and is compared before it is converted: a float as large
		// as the cvar system lets through does not fit an int64 (NaN and infinity fail the two comparisons)
		int64 size = Min( StaticSizeBytes( ( cacheType_t )type ), StaticCapBytes( ( cacheType_t )type ) );
#if !defined( ID_RETAIL )
		const double testCapMB = r_staticArenaTestCapMB.GetFloat();
		if( testCapMB > 0.0 && testCapMB < ( double )size / ( 1024.0 * 1024.0 ) )
		{
			size = ( int64 )( testCapMB * 1024.0 * 1024.0 );
		}
#endif
		//puncho end
		// BeginStaticLoad freed segment 1 and nothing allocates static data between BeginLevelLoad and the upload loops;
		// used1 is counted all the same, so a static allocation there (stage 4's cells) cannot pass unbudgeted
		assert( set.arena.used1 == 0 );
		//puncho start - static index (2R R3.1): the figures in bytes as they were before the units. Segment 0's is its
		// frontier without the padding behind its last block, so the 25 % test and the forecast decide as they did
		const int64 used0 = StaticSegmentBytes( ( cacheType_t )type, 0 ) - set.tail0;
		const int64 dead0 = Max( ( int64 )0, used0 - set.live0 );
		const int64 needed = ALIGN( used0, align ) + StaticSegmentBytes( ( cacheType_t )type, 1 ) + set.incoming + set.late;
		//puncho end
		set.dead0 = dead0;
		holds = holds || used0 > 0;
		reset = reset || ( used0 > 0 && ( 4 * dead0 > used0 || needed > size ) );
		line += va( " %s used0 %lld live0 %lld dead0 %lld incoming %lld late %lld size %lld;", ( type == CACHE_INDEX ) ? "idx" : "vtx",
					( long long )used0, ( long long )set.live0, ( long long )dead0, ( long long )set.incoming, ( long long )set.late, ( long long )size );
	}
	if( reset )
	{
		FreeStaticSegment( 0 );
		staticResets++;
	}
	idLib::Printf( "static arena preflight:%s segment 0 %s (generation %d, %llu us)\n", line.c_str(),
				   reset ? "reset" : ( holds ? "kept" : "empty" ), staticGenerations[0], ( unsigned long long )walkUs );
}

/*
==============
idVertexCache::StaticTestFill

Test only (static index, 2R R3.3): empty blocks fill the index set until marginBytes are free of what the set may use (its
size or its cap, whichever is less). No block is larger than the room over the margin, so a fill never grows the set.
==============
*/
void idVertexCache::StaticTestFill( int64 marginBytes, nvrhi::ICommandList* commandList )
{
#if !defined( ID_RETAIL )
	const int64 before = StaticUsedBytes( CACHE_INDEX );
	for( ;; )
	{
		const int64 freeBytes = Min( StaticSizeBytes( CACHE_INDEX ), StaticCapBytes( CACHE_INDEX ) ) - StaticUsedBytes( CACHE_INDEX );
		if( freeBytes <= marginBytes )
		{
			break;
		}
		AllocStaticArena( NULL, ( int )Min( freeBytes - marginBytes, ( int64 )STATIC_TEST_BLOCK_BYTES ), CACHE_INDEX, commandList, true );
	}
	idLib::Printf( "static test fill (%s): index set filled by %lld bytes, %lld free\n", staticPhase, ( long long )( StaticUsedBytes( CACHE_INDEX ) - before ),
				   ( long long )( Min( StaticSizeBytes( CACHE_INDEX ), StaticCapBytes( CACHE_INDEX ) ) - StaticUsedBytes( CACHE_INDEX ) ) );
#endif
}

/*
==============
idVertexCache::EndStaticUploads

After the upload loops: what is allocated from here to the next load (light tris) is late use, in
segment 1, and the handles this load uploaded are valid from now on.
==============
*/
void idVertexCache::EndStaticUploads( nvrhi::ICommandList* commandList )
{
	//puncho - static index (2R R3.1): bytes as the byte counters held them (the counters hold units; without segment 0's
	// tail, as BeginStaticLoad reads the use it compares with this mark)
	staticSets[ CACHE_VERTEX ].mark = StaticUsedBytes( CACHE_VERTEX ) - staticSets[ CACHE_VERTEX ].tail0;
	staticSets[ CACHE_INDEX ].mark = StaticUsedBytes( CACHE_INDEX ) - staticSets[ CACHE_INDEX ].tail0;
	//puncho - static index (2R R3.3): the test blocks placed so far are not model use
	staticSets[ CACHE_VERTEX ].synthMark = staticSets[ CACHE_VERTEX ].synth[0] + staticSets[ CACHE_VERTEX ].synth[1];
	staticSets[ CACHE_INDEX ].synthMark = staticSets[ CACHE_INDEX ].synth[0] + staticSets[ CACHE_INDEX ].synth[1];
	staticSplitChanged = false;
	staticSegment = 1;
	//puncho start - static index (2R R3.3). Test only: r_staticArenaTestFillMB's first fill; with force 1, one block more than is
	// free then: the index set grows over the models just uploaded, with the caller's list open as in a real overflow
#if !defined( ID_RETAIL )
	int fill[3];
	if( StaticTestFillNumbers( fill ) )
	{
		staticPhase = "uploads-fill";
		if( fill[0] >= 0 )
		{
			StaticTestFill( ( int64 )fill[0] << 20, commandList );
		}
		if( fill[2] == 1 )
		{
			const int64 used = StaticUsedBytes( CACHE_INDEX );
			const int64 size = StaticSizeBytes( CACHE_INDEX );
			const int64 cap = StaticCapBytes( CACHE_INDEX );
			const int64 real = used - staticSets[ CACHE_INDEX ].synth[0] - staticSets[ CACHE_INDEX ].synth[1];
			const int64 block = size - used + INDEX_CACHE_ALIGN;
			if( real > 0 && size < cap && block > 0 && block <= VERTCACHE_SIZE_MASK && used + block <= cap )
			{
				AllocStaticArena( NULL, ( int )block, CACHE_INDEX, commandList, true );
			}
			else
			{
				idLib::Printf( "static test fill: no forced growth: %lld model bytes, %lld of %lld bytes used, cap %lld\n", ( long long )real, ( long long )used, ( long long )size, ( long long )cap );
			}
		}
	}
#endif
	staticPhase = "interactions";
	// the model manager closes and executes its list right after this call, and nothing allocates static data in between
	staticUploadList = NULL;
	//puncho end
}

//puncho start - static index (2R R3.3)
/*
==============
idVertexCache::FlushStaticUploads

The loops that upload many static blocks call this between two of them: the model manager's two upload loops (behind
each model's deform caches, behind each surface) and idInteraction::CreateStaticInteraction (behind each lit
surface's light tris), which serves every caller of the interaction generation. One model, one light or one
interaction can make far more than the threshold; one surface cannot (a block is at most 4 MiB).
Once STATIC_UPLOAD_FLUSH_MB of static data wait in commandList (counted as the sets charge them), the list
is closed, executed and opened again, and the device collects what the GPU has finished (the command buffer of an
earlier execution that is done goes back to the pool). Why: NVRHI keeps a list's upload chunks for as long as the list
lives, and reuses a chunk only after the list was executed and the GPU is done with it. A load that grows no set would
record all of its static data in one recording, and its list would keep that much upload memory until the process
ends; on D3D12 the interaction lists make it in 64 KiB chunks, which costs seconds for each GiB. Nothing waits here:
the GPU works on while the next blocks are recorded. The bytes, the offsets and every handle are the same as without
the call; only the upload reaches the buffer earlier.
==============
*/
void idVertexCache::FlushStaticUploads( nvrhi::ICommandList* commandList )
{
	int64 threshold = ( int64 )STATIC_UPLOAD_FLUSH_MB << 20;
#if !defined( ID_RETAIL )
	// test only: with a small threshold a small level executes its lists many times
	if( r_staticArenaTestFlushKB.GetInteger() > 0 )
	{
		threshold = ( int64 )r_staticArenaTestFlushKB.GetInteger() << 10;
	}
#endif
	if( commandList == NULL || staticUploadList != commandList || staticUploadBytes < threshold )
	{
		return;
	}
	nvrhi::IDevice* device = deviceManager->GetDevice();
	commandList->close();
	device->executeCommandList( commandList );
	device->runGarbageCollection();
	commandList->open();
	// everything recorded into the list has been executed: no static upload waits, and the count starts again
	staticUploadList = NULL;
	staticUploadBytes = 0;
	staticFlushes++;
}
//puncho end

/*
==============
idVertexCache::EndStaticLoad

The end of a level load's window (static index, 2R R3.3): after the load's interactions were generated (a map) or after
its EndLevelLoad (the main menu). What is allocated from here to the next load is late runtime use (the interaction
slices). The top-up makes room for the most such use seen so far while the load still stands, so that a slice need not
grow the set in a running game. It is optional: where the cap, split 1 or the device refuses, the load goes on with
the set as it is. Without an open load window this does nothing.
==============
*/
void idVertexCache::EndStaticLoad()
{
	if( staticWindow != STATIC_WINDOW_LOAD )
	{
		return;
	}
	for( int type = CACHE_VERTEX; type <= CACHE_INDEX; type++ )
	{
		staticArenaStats_t& set = staticSets[ type ];
		set.markLoad = StaticUsedBytes( ( cacheType_t )type );
		set.synthMarkLoad = set.synth[0] + set.synth[1];
	}
#if !defined( ID_RETAIL )
	// test only: r_staticArenaTestFillMB's second fill, after the mark and before the top-up
	int fill[3];
	if( StaticTestFillNumbers( fill ) && fill[1] >= 0 )
	{
		staticPhase = "load-fill";
		StaticTestFill( ( int64 )fill[1] << 20, NULL );
	}
#endif
	staticPhase = "top-up";
	const int64 quantum = ( int64 )STATIC_GROW_QUANTUM_MB << 20;
	for( int type = CACHE_VERTEX; type <= CACHE_INDEX; type++ )
	{
		staticArenaStats_t& set = staticSets[ type ];
		const int64 size = StaticSizeBytes( ( cacheType_t )type );
		const int64 cap = StaticCapBytes( ( cacheType_t )type );
		const int64 need = StaticUsedBytes( ( cacheType_t )type ) + set.lateRuntime;
		if( need <= Min( size, cap ) )
		{
			continue;
		}
		if( need > cap || staticSplit )
		{
			set.growthSkips++;
			idLib::Printf( "static %s top-up: %lld bytes wanted, skipped: %s\n", StaticSetName( ( cacheType_t )type ), ( long long )need, ( need > cap ) ? "over the cap" : "r_staticArenaSplit 1" );
			continue;
		}
		GrowStaticSet( ( cacheType_t )type, Min( cap, ( need + quantum - 1 ) / quantum * quantum ), NULL, false );
	}
	staticWindow = STATIC_WINDOW_CLOSED;
	staticPhase = "closed";
	staticUploadList = NULL;
	for( int type = CACHE_VERTEX; type <= CACHE_INDEX; type++ )
	{
		const staticArenaStats_t& set = staticSets[ type ];
		// mark is the old byte figure: at split 1 the models' bytes lack segment 0's tail (under 32 bytes), which the interactions' then hold
		const int64 models = ( set.mark >= 0 ) ? set.mark - set.synthMark : 0;
		const int64 interactions = ( set.mark >= 0 ) ? ( set.markLoad - set.mark ) - ( set.synthMarkLoad - set.synthMark ) : 0;
		idLib::Printf( "static %s load %d: %lld of %lld bytes used, cap %d MiB; models %lld, interactions %lld, test blocks %lld; growths %d, skipped %d, copied %lld, last transient %lld; late %lld, lateRuntime %lld\n",
					   StaticSetName( ( cacheType_t )type ), staticEpoch, ( long long )StaticUsedBytes( ( cacheType_t )type ), ( long long )StaticSizeBytes( ( cacheType_t )type ), set.capMB,
					   ( long long )models, ( long long )interactions, ( long long )( set.synth[0] + set.synth[1] ), set.growths, set.growthSkips, ( long long )set.copied,
					   ( long long )set.lastTransient, ( long long )set.late, ( long long )set.lateRuntime );
	}
#if !defined( ID_RETAIL )
	if( r_staticArenaTestFlushKB.GetInteger() > 0 )
	{
		idLib::Printf( "static uploads, load %d: %d early executions of an upload list (test: one every %d KiB)\n", staticEpoch, staticFlushes, r_staticArenaTestFlushKB.GetInteger() );
	}
	else
#endif
	{
		idLib::Printf( "static uploads, load %d: %d early executions of an upload list (one every %d MiB)\n", staticEpoch, staticFlushes, STATIC_UPLOAD_FLUSH_MB );
	}
}

/*
==============
idVertexCache::AllocStaticArena

AllocStaticVertex/Index at both splits (2R R3.1; split 1 only before). The set is counted in 16-byte
units: a block is charged its ALIGN'd length and starts on the set's alignment, which gives the
starts and the first refusal the byte arithmetic gave. At split 0 there is one frontier (segment 0)
and the handle's top field keeps the frame number; at split 1 the frozen core places the block at
the current segment's frontier (-1 when the two frontiers would cross) and the top field carries
segment and generation. The handle's offset field holds units (CacheOffsetBytes turns them into
bytes). The 4 MiB block guard comes first.
==============
*/
vertCacheHandle_t idVertexCache::AllocStaticArena( const void* data, int bytes, cacheType_t type, nvrhi::ICommandList* commandList, bool synthetic )
{
	//puncho - static index (2R R3.3): the block guard comes first and takes a negative length too (the handle keeps 22 bits of
	// size); a block of no bytes changes nothing
	if( bytes < 0 || bytes > VERTCACHE_SIZE_MASK )
	{
		idLib::Error( "idVertexCache::AllocStaticArena: %i bytes exceeds the %i byte block limit", bytes, VERTCACHE_SIZE_MASK );
	}
	if( bytes == 0 )
	{
		return ( vertCacheHandle_t )0;
	}
	assert( ( ( ( uintptr_t )( data ) ) & 15 ) == 0 );

	assert( type == CACHE_VERTEX || type == CACHE_INDEX );
	staticArenaStats_t& set = staticSets[ type ];
	const int segment = staticSplit ? staticSegment : 0;	//puncho - static index (2R R3.1): split 0 has one frontier
	assert( segment == 0 || segment == 1 );
	//puncho start - static index (2R R3.1)
	const int align = ( type == CACHE_INDEX ) ? INDEX_CACHE_ALIGN : VERTEX_CACHE_ALIGN;
	const int alignedBytes = ALIGN( bytes, align );
	//puncho start - static index (2R R3.3): a block that does not fit the set, or would pass the set's cap, goes to EnsureStaticRoom,
	// which grows the set or ends in a fatal error that names the reason; it returns only when the block fits
	const int64 need = StaticUsedBytes( type ) + alignedBytes;
	if( need > StaticSizeBytes( type ) || need > StaticCapBytes( type ) )
	{
		EnsureStaticRoom( type, need, bytes, alignedBytes, commandList );
	}
	//puncho end
	const int unitOffset = StaticArenaAlloc( set.arena, segment, alignedBytes >> STATIC_OFFSET_UNIT_BITS, align >> STATIC_OFFSET_UNIT_BITS );
	if( unitOffset < 0 )
	{
		// not reached: the two tests above admit what the arena admits (every frontier and the set's size lie on its alignment)
		idLib::Printf( "%s: segment %i: %lld + %lld used + %i requested, %lld bytes allocated\n", ( type == CACHE_INDEX ) ? "AllocStaticIndex" : "AllocStaticVertex", segment,
					   ( long long )StaticSegmentBytes( type, 0 ), ( long long )StaticSegmentBytes( type, 1 ), bytes, ( long long )StaticSizeBytes( type ) );
		idLib::FatalError( "%s failed: the arena refused %i bytes in segment %i", ( type == CACHE_INDEX ) ? "AllocStaticIndex" : "AllocStaticVertex", bytes, segment );
	}
	if( segment == 0 )
	{
		// the preflight's byte figure for segment 0 ends at its last block's own end: at split 1 keep the padding behind it
		set.tail0 = staticSplit ? alignedBytes - bytes : 0;
	}
	if( staticSplit )
	{
		// kept and printed at split 1 only, as before: split 0's use is not in it
		set.high[ segment ] = Max( set.high[ segment ], StaticSegmentBytes( type, segment ) );
	}
	const int64 offset = ( int64 )unitOffset << STATIC_OFFSET_UNIT_BITS;	// bytes; Update takes an int64 offset since R3.2
	//puncho end
	//puncho start - static index (2R R3.3): a test block is counted apart, and the load's high mark counts the level's own bytes
	if( synthetic )
	{
		set.synth[ segment ] += alignedBytes;
	}
	set.highLoad = Max( set.highLoad, need - set.synth[0] - set.synth[1] );
	//puncho end
	if( type == CACHE_INDEX )
	{
		staticData.indexMemUsed.SetValue( set.arena.used0 + set.arena.used1 );
		if( data != NULL )
		{
			if( staticData.indexBuffer.GetUsage() == BU_DYNAMIC )
			{
				MapGeoBufferSet( staticData );
			}
			staticData.indexBuffer.Update( data, bytes, offset, false, commandList );
		}
	}
	else
	{
		staticData.vertexMemUsed.SetValue( set.arena.used0 + set.arena.used1 );
		if( data != NULL )
		{
			if( staticData.vertexBuffer.GetUsage() == BU_DYNAMIC )
			{
				MapGeoBufferSet( staticData );
			}
			staticData.vertexBuffer.Update( data, bytes, offset, false, commandList );
		}
	}
	staticData.allocations++;
	//puncho - static index (2R R3.3): the list this upload was recorded into. A growth flushes its caller's list only, so it must
	// be asked for through this list as long as the upload waits in it (EnsureStaticRoom)
	if( data != NULL )
	{
		// and how much waits in that list (FlushStaticUploads): the count starts again with another list, or after the list was executed
		if( staticUploadList != commandList )
		{
			staticUploadList = commandList;
			staticUploadBytes = 0;
		}
		staticUploadBytes += alignedBytes;
	}

	//puncho start - static index (2R R3.1): the offset field holds units. The top field: at split 0 the frame number, as
	// ActuallyAlloc wrote it into a static handle; at split 1 segment and generation
	assert( !staticSplit || ( staticGenerations[ segment ] >= 1 && staticGenerations[ segment ] <= STATIC_ARENA_MAX_GENERATION ) );
	const uint64 top = staticSplit ? StaticArenaPackTop( segment, staticGenerations[ segment ] ) : ( ( uint64 )( currentFrame & VERTCACHE_FRAME_MASK ) << VERTCACHE_FRAME_SHIFT );
	return	top |
			( ( uint64 )( unitOffset & VERTCACHE_OFFSET_MASK ) << VERTCACHE_OFFSET_SHIFT ) |
			( ( uint64 )( bytes & VERTCACHE_SIZE_MASK ) << VERTCACHE_SIZE_SHIFT ) | VERTCACHE_STATIC;
	//puncho end
}

/*
==============
idVertexCache::EnsureStaticRoom

AllocStaticArena's cold path (static index, 2R R3.3): the block does not fit the set as it stands, or would pass the
set's cap. The refusals, in this order: cap (the level needs more than r_staticIndexMB or r_staticVertexMB allows),
window (no level load and no interaction slice is running, or this is not the main thread: the set's buffer must not be
replaced now), split (at r_staticArenaSplit 1 a set is sized once, when the load begins), uploads waiting in another
command list than the caller's (the growth flushes the caller's list only; no caller in the engine does this), device
(the larger buffer was refused). Each is a fatal error. Its text stays under 80 characters, all the Windows error box
shows; the line printed before it has every figure. Otherwise the set grows to the next multiple of 256 MiB that holds
the block, at most to its cap, and the caller goes on.
==============
*/
void idVertexCache::EnsureStaticRoom( cacheType_t type, int64 need, int bytes, int alignedBytes, nvrhi::ICommandList* commandList )
{
	staticArenaStats_t& set = staticSets[ type ];
	const char* name = StaticSetName( type );
	const char* cvarName = ( type == CACHE_INDEX ) ? "r_staticIndexMB" : "r_staticVertexMB";
	const int64 used = need - alignedBytes;
	const int64 size = StaticSizeBytes( type );
	const int64 cap = StaticCapBytes( type );
	idLib::Printf( "static %s set: %lld used + %d requested (%d charged) bytes; %lld allocated; cap %lld bytes (%s %d, latched %d); split %d, %s\n", name, ( long long )used, bytes,
				   alignedBytes, ( long long )size, ( long long )cap, cvarName, StaticCapCvarMB( type ), set.capMB, staticSplit ? 1 : 0, staticPhase );
	if( need > cap )
	{
		idLib::FatalError( "static %s cap: %lld + %d KiB > %s %d (latched %d)", name, ( long long )( used >> 10 ), ( alignedBytes + 1023 ) >> 10, cvarName, StaticCapCvarMB( type ), set.capMB );
	}
	if( staticWindow == STATIC_WINDOW_CLOSED || !idLib::IsMainThread() )
	{
		idLib::FatalError( "static %s window: full outside a load or slice, %lld of %lld MiB", name, ( long long )( used >> 20 ), ( long long )( size >> 20 ) );
	}
	if( staticSplit )
	{
		if( set.resizeSkipped > 0 )
		{
			idLib::Printf( "static %s set: resize skipped: device refused %lld bytes\n", name, ( long long )set.resizeSkipped );
		}
		idLib::FatalError( "static %s split: r_staticArenaSplit 1 sizes the set at load, %lld MiB", name, ( long long )( size >> 20 ) );
	}
	if( staticUploadList != NULL && staticUploadList != commandList )
	{
		// the growth flushes the caller's list only: a write that waits in another list would reach the old buffer after the copy
		idLib::FatalError( "static %s set: uploads wait in another command list", name );
	}
	const int64 quantum = ( int64 )STATIC_GROW_QUANTUM_MB << 20;
	const int64 newBytes = Min( cap, ( need + quantum - 1 ) / quantum * quantum );
	if( !GrowStaticSet( type, newBytes, commandList, true ) )
	{
		idLib::FatalError( "static %s device: growing %lld to %lld MiB was refused", name, ( long long )( size >> 20 ), ( long long )( newBytes >> 20 ) );
	}
}

/*
==============
idVertexCache::CountStaleHandle

CacheIsCurrent's cold path, out of its body: count the stale static handle the caller is about to
re-upload.
==============
*/
bool idVertexCache::CountStaleHandle()
{
	staticStaleHandles.Increment();
	return false;
}
//puncho end

//puncho start - static index (2R R3.3)
/*
==============
staticSoftErrors_t

While an object of this type lives, an NVRHI error raised on the main thread is a warning and not fatal
(DefaultMessageCallback, sys/DeviceManager.h). An exception that passes the object ends that too.
==============
*/
struct staticSoftErrors_t
{
	staticSoftErrors_t()
	{
		DefaultMessageCallback::GetInstance().BeginSoftErrors();
	}
	~staticSoftErrors_t()
	{
		DefaultMessageCallback::GetInstance().EndSoftErrors();
	}
	// one object, one scope: a copy would end the scope twice
	staticSoftErrors_t( const staticSoftErrors_t& ) = delete;
	staticSoftErrors_t& operator=( const staticSoftErrors_t& ) = delete;
};

/*
==============
StaticGrowBuffer

GrowStaticSet's middle, for either kind of buffer: live, a static set's buffer, is replaced by one of newBytes that holds
live's first copyBytes bytes. The caller has executed every list that wrote into live and has emptied the GPU's queue.
The larger buffer is made in an object of its own, so a refusal leaves live untouched; while it is made an NVRHI error is
not fatal (on D3D12 a refused buffer is an NVRHI error). copyList is closed and is opened here for the one copy; after
it the queue is emptied again, so nothing refers to the old buffer when it is freed (the Vulkan allocator destroys a
buffer at once). Two takes: nothing is destroyed between them, and the old buffer goes only once the new one is in
place. False when the device refuses the larger buffer.
==============
*/
template< class _bufferType_ >
static bool StaticGrowBuffer( _bufferType_& live, const int64 newBytes, const int64 copyBytes, nvrhi::ICommandList* copyList, uint64& allocUs, uint64& copyUs )
{
	nvrhi::IDevice* device = deviceManager->GetDevice();

	const uint64 allocStart = Sys_Microseconds();
	_bufferType_ larger;
	bool made;
	{
		const staticSoftErrors_t softErrors;
		made = larger.AllocBufferObject( NULL, newBytes, BU_STATIC, NULL );
	}
	allocUs = Sys_Microseconds() - allocStart;
	if( !made )
	{
		return false;
	}

	const uint64 copyStart = Sys_Microseconds();
	if( copyBytes > 0 )
	{
		copyList->open();
		copyList->copyBuffer( larger.GetAPIObject(), 0, live.GetAPIObject(), 0, ( uint64 )copyBytes );
		copyList->close();
		device->executeCommandList( copyList );
		if( !device->waitForIdle() )
		{
			idLib::FatalError( "idVertexCache::GrowStaticSet: the device was lost" );
		}
		device->runGarbageCollection();
	}
	copyUs = Sys_Microseconds() - copyStart;

	_bufferType_ old;
	if( !old.TakeFrom( live ) || !live.TakeFrom( larger ) )
	{
		idLib::FatalError( "idVertexCache::GrowStaticSet: the buffers could not change places" );
	}
	old.FreeBufferObject();
	return true;
}
//puncho end

/*
==============
idVertexCache::ActuallyAlloc
==============
*/
vertCacheHandle_t idVertexCache::ActuallyAlloc( geoBufferSet_t& vcs, const void* data, int bytes, cacheType_t type, nvrhi::ICommandList* commandList )
{
	if( bytes == 0 )
	{
		return ( vertCacheHandle_t )0;
	}

	// RB: changed UINT_PTR to uintptr_t
	assert( ( ( ( uintptr_t )( data ) ) & 15 ) == 0 );
	// RB end

	// SRS - enforce cache alignment without read beyond boundary for each cache type below
	//assert( ( bytes & 15 ) == 0 );

	//puncho - vertex cache (P2 review): the handle keeps 22 bits of size; a larger block would be referenced short
	if( bytes > VERTCACHE_SIZE_MASK )
	{
		idLib::Error( "idVertexCache::ActuallyAlloc: %i bytes exceeds the %i byte block limit", bytes, VERTCACHE_SIZE_MASK );
	}

	int	endPos = 0;
	int offset = 0;

	switch( type )
	{
		case CACHE_INDEX:
		{
			// SRS - calculate alignedBytes retaining original to prevent read beyond data boundary during update
			int alignedBytes = ALIGN( bytes, INDEX_CACHE_ALIGN );
			endPos = vcs.indexMemUsed.Add( alignedBytes );
			if( endPos > vcs.indexBuffer.GetAllocedSize() )
			{
				idLib::Error( "Out of index cache" );
			}

			offset = endPos - alignedBytes;

			if( data != NULL )
			{
				if( vcs.indexBuffer.GetUsage() == BU_DYNAMIC )
				{
					MapGeoBufferSet( vcs );
				}
				vcs.indexBuffer.Update( data, bytes, offset, false, commandList );
			}

			break;
		}
		case CACHE_VERTEX:
		{
			// SRS - calculate alignedBytes retaining original to prevent read beyond data boundary during update
			int alignedBytes = ALIGN( bytes, VERTEX_CACHE_ALIGN );
			endPos = vcs.vertexMemUsed.Add( alignedBytes );
			if( endPos > vcs.vertexBuffer.GetAllocedSize() )
			{
				idLib::Error( "Out of vertex cache" );
			}

			offset = endPos - alignedBytes;

			if( data != NULL )
			{
				if( vcs.vertexBuffer.GetUsage() == BU_DYNAMIC )
				{
					MapGeoBufferSet( vcs );
				}
				vcs.vertexBuffer.Update( data, bytes, offset, false, commandList );
			}

			break;
		}
		case CACHE_JOINT:
		{
			// SRS - calculate alignedBytes retaining original to prevent read beyond data boundary during update
			int alignedBytes = ALIGN( bytes, uniformBufferOffsetAlignment );
			endPos = vcs.jointMemUsed.Add( alignedBytes );
			if( endPos > vcs.jointBuffer.GetAllocedSize() )
			{
				idLib::Error( "Out of joint buffer cache" );
			}

			offset = endPos - alignedBytes;

			if( data != NULL )
			{
				if( vcs.jointBuffer.GetUsage() == BU_DYNAMIC )
				{
					MapGeoBufferSet( vcs );
				}
				vcs.jointBuffer.Update( data, bytes, offset, false, commandList );
			}

			break;
		}
		default:
			assert( false );
	}

	vcs.allocations++;

	//puncho - static index (2R R3.1): the per-frame sets only, whose offset field counts bytes. A static block comes from
	// AllocStaticArena, whose offset field counts 16-byte units
	assert( &vcs != &staticData );
	vertCacheHandle_t handle =	( ( uint64 )( currentFrame & VERTCACHE_FRAME_MASK ) << VERTCACHE_FRAME_SHIFT ) |
								( ( uint64 )( offset & VERTCACHE_OFFSET_MASK ) << VERTCACHE_OFFSET_SHIFT ) |
								( ( uint64 )( bytes & VERTCACHE_SIZE_MASK ) << VERTCACHE_SIZE_SHIFT );
	return handle;
}

//puncho start - static index (2R R3.3)
/*
==============
idVertexCache::GrowStaticSet

One static set's buffer is replaced by one of newBytes that holds the same bytes at the same offsets, so no handle
changes. Main thread, inside a window, with the back end idle and segment 1 empty (a block of segment 1 lies at the
set's end and would have to move). callerList is the command list the caller is recording into, or NULL when it has
none open. The steps: the caller's list is closed and executed, so what it recorded reaches the old buffer; the GPU
finishes and the finished lists let go of the buffers they used; the back end's cached buffer handles are cleared; the
larger buffer is made and the used bytes are copied on the GPU (StaticGrowBuffer); the caller's list is opened again.
False when the device refuses the larger buffer: the old one, the frontiers and every handle are as before, and the
caller's list is open again. demanded: the caller cannot go on without the room (it then ends in a fatal error);
otherwise the growth is optional and counted as skipped. BeginStaticLoad and EndStaticLoad call this with no list and
past EnsureStaticRoom's test of staticUploadList: at both, every list that took static uploads was closed and executed
by its owner before, and the wait here covers what the GPU has not finished.
==============
*/
bool idVertexCache::GrowStaticSet( cacheType_t type, int64 newBytes, nvrhi::ICommandList* callerList, bool demanded )
{
	staticArenaStats_t& set = staticSets[ type ];
	const int64 oldBytes = StaticSizeBytes( type );
	const int64 usedBytes = StaticUsedBytes( type );
	assert( idLib::IsMainThread() && staticWindow != STATIC_WINDOW_CLOSED );
	assert( newBytes > oldBytes && newBytes <= StaticCapBytes( type ) );
	assert( ( newBytes & ( ( ( type == CACHE_INDEX ) ? INDEX_CACHE_ALIGN : VERTEX_CACHE_ALIGN ) - 1 ) ) == 0 );
	if( set.arena.used1 != 0 )
	{
		idLib::FatalError( "idVertexCache::GrowStaticSet: segment 1 of the %s set holds blocks", StaticSetName( type ) );
	}
	nvrhi::IDevice* device = deviceManager->GetDevice();

	const uint64 waitStart = Sys_Microseconds();
	if( callerList != NULL )
	{
		callerList->close();
		device->executeCommandList( callerList );
	}
	if( !device->waitForIdle() )
	{
		idLib::FatalError( "idVertexCache::GrowStaticSet: the device was lost" );
	}
	device->runGarbageCollection();
	backEnd.ClearGeometryBufferCache();
	const uint64 waitUs = Sys_Microseconds() - waitStart;

	// the copy is recorded into the caller's list, which is closed now; without one, into a list of its own
	nvrhi::CommandListHandle ownList;
	nvrhi::ICommandList* copyList = callerList;
	if( copyList == NULL && usedBytes > 0 )
	{
		nvrhi::CommandListParameters params;
		params.enableImmediateExecution = false;
		ownList = device->createCommandList( params );
		copyList = ownList;
	}
	uint64 allocUs = 0;
	uint64 copyUs = 0;
	const bool grown = ( type == CACHE_INDEX ) ? StaticGrowBuffer( staticData.indexBuffer, newBytes, usedBytes, copyList, allocUs, copyUs )
					   : StaticGrowBuffer( staticData.vertexBuffer, newBytes, usedBytes, copyList, allocUs, copyUs );
	if( grown )
	{
		set.arena.size = ( int )( newBytes >> STATIC_OFFSET_UNIT_BITS );
		set.growths++;
		set.copied += usedBytes;
		set.lastTransient = oldBytes + newBytes;
		set.maxTransient = Max( set.maxTransient, set.lastTransient );
	}
	else if( !demanded )
	{
		set.growthSkips++;
	}
	if( callerList != NULL )
	{
		callerList->open();
	}
	// everything recorded before this growth has been executed
	staticUploadList = NULL;
	// a refused growth copied nothing and held no second buffer: its line says 0 for both
	idLib::Printf( "static %s grow (%s, load %d, %s): %lld -> %lld bytes, %lld copied, transient %lld bytes; wait %llu, alloc %llu, copy %llu us: %s\n",
				   StaticSetName( type ), staticPhase, staticEpoch, demanded ? "demanded" : "optional", ( long long )oldBytes, ( long long )newBytes, ( long long )( grown ? usedBytes : 0 ),
				   ( long long )( grown ? oldBytes + newBytes : 0 ), ( unsigned long long )waitUs, ( unsigned long long )allocUs, ( unsigned long long )copyUs, grown ? "grown" : "refused by the device" );
	return grown;
}

#if !defined( ID_RETAIL )
CONSOLE_COMMAND( testStaticWindow, "test only: asks the static index set for 16 bytes now, outside a level load (a full set answers with the window refusal, a fatal error)", NULL )
{
	const vertCacheHandle_t handle = vertexCache.AllocStaticArena( NULL, INDEX_CACHE_ALIGN, CACHE_INDEX, NULL, true );
	idLib::Printf( "testStaticWindow: 16 bytes placed at %lld; the index set holds %lld of %lld bytes\n", ( long long )idVertexCache::CacheOffsetBytes( handle ),
				   ( long long )vertexCache.StaticUsedBytes( CACHE_INDEX ), ( long long )vertexCache.StaticSizeBytes( CACHE_INDEX ) );
}

CONSOLE_COMMAND( testStaticBlock, "test only: asks the static index set for one block of 4 MiB, one byte over the block limit (an error that ends the running map)", NULL )
{
	vertexCache.AllocStaticArena( NULL, VERTCACHE_SIZE_MASK + 1, CACHE_INDEX, NULL, true );
}
#endif
//puncho end

/*
==============
idVertexCache::AllocVertex
==============
*/
vertCacheHandle_t idVertexCache::AllocVertex( const void* data, int num, size_t size /*= sizeof( idDrawVert ) */, nvrhi::ICommandList* commandList )
{
	return ActuallyAlloc( frameData[ listNum ], data, num * size, CACHE_VERTEX, commandList );
}

/*
==============
idVertexCache::AllocIndex
==============
*/
vertCacheHandle_t idVertexCache::AllocIndex( const void* data, int num, size_t size /*= sizeof( triIndex_t ) */, nvrhi::ICommandList* commandList )
{
	return ActuallyAlloc( frameData[ listNum ], data, num * size, CACHE_INDEX, commandList );
}

/*
==============
idVertexCache::AllocJoint
==============
*/
vertCacheHandle_t idVertexCache::AllocJoint( const void* data, int num, size_t size /*= sizeof( idJointMat ) */, nvrhi::ICommandList* commandList )
{
	return ActuallyAlloc( frameData[ listNum ], data, num * size, CACHE_JOINT, commandList );
}

/*
==============
idVertexCache::AllocStaticVertex
==============
*/
vertCacheHandle_t idVertexCache::AllocStaticVertex( const void* data, int bytes, nvrhi::ICommandList* commandList )
{
	//puncho - static arena (P2b.1.8), static index (2R R3.1): both splits place the block through the unit arena, which
	// refuses a block that does not fit with the message this function had
	return AllocStaticArena( data, bytes, CACHE_VERTEX, commandList );
}

/*
==============
idVertexCache::AllocStaticIndex
==============
*/
vertCacheHandle_t idVertexCache::AllocStaticIndex( const void* data, int bytes, nvrhi::ICommandList* commandList )
{
	//puncho - static arena (P2b.1.8), static index (2R R3.1): both splits place the block through the unit arena, which
	// refuses a block that does not fit with the message this function had
	return AllocStaticArena( data, bytes, CACHE_INDEX, commandList );
}

/*
==============
idVertexCache::MappedVertexBuffer
==============
*/
byte* idVertexCache::MappedVertexBuffer( vertCacheHandle_t handle )
{
	release_assert( !CacheIsStatic( handle ) );
	const int64 offset = CacheOffsetBytes( handle );	//puncho - static index (2R R3.1): the decoder for either kind of handle
	const uint64 frameNum = ( int )( handle >> VERTCACHE_FRAME_SHIFT ) & VERTCACHE_FRAME_MASK;
	release_assert( frameNum == ( currentFrame & VERTCACHE_FRAME_MASK ) );
	return frameData[ listNum ].mappedVertexBase + offset;
}

/*
==============
idVertexCache::MappedIndexBuffer
==============
*/
byte* idVertexCache::MappedIndexBuffer( vertCacheHandle_t handle )
{
	release_assert( !CacheIsStatic( handle ) );
	const int64 offset = CacheOffsetBytes( handle );	//puncho - static index (2R R3.1): the decoder for either kind of handle
	const uint64 frameNum = ( int )( handle >> VERTCACHE_FRAME_SHIFT ) & VERTCACHE_FRAME_MASK;
	release_assert( frameNum == ( currentFrame & VERTCACHE_FRAME_MASK ) );
	return frameData[ listNum ].mappedIndexBase + offset;
}

/*
==============
idVertexCache::CacheIsCurrent
==============
*/
bool idVertexCache::CacheIsCurrent( const vertCacheHandle_t handle )
{
	const int isStatic = handle & VERTCACHE_STATIC;
	if( isStatic )
	{
		//puncho start - static arena (P2b.1.8): at split 1 a static handle is current while its segment keeps its
		// generation; a stale one is counted and the caller's fallback re-uploads the data into this frame's buffers.
		// Dev: while r_staticTestStale's plant is armed, one valid handle reads as stale (no handle is rewritten)
#if defined( ID_RETAIL )
		if( !staticSplit || StaticHandleValid( handle ) )
#else
		if( !staticSplit || ( StaticHandleValid( handle ) && !( staticTestStale.GetValue() > 0 && staticTestStale.Decrement() == 0 ) ) )
#endif
		{
			return true;
		}
		return CountStaleHandle();
		//puncho end
	}
	const uint64 frameNum = ( int )( handle >> VERTCACHE_FRAME_SHIFT ) & VERTCACHE_FRAME_MASK;
	if( frameNum != ( currentFrame & VERTCACHE_FRAME_MASK ) )
	{
		return false;
	}
	return true;
}

/*
==============
idVertexCache::GetVertexBuffer
==============
*/
bool idVertexCache::GetVertexBuffer( vertCacheHandle_t handle, idVertexBuffer* vb )
{
	const int isStatic = handle & VERTCACHE_STATIC;
	//puncho - static index (2R R3.1): the decoders; a static offset comes back in bytes (Reference takes an int64 since R3.2)
	const int size = CacheSizeBytes( handle );
	const int64 offset = CacheOffsetBytes( handle );
	const uint64 frameNum = ( int )( handle >> VERTCACHE_FRAME_SHIFT ) & VERTCACHE_FRAME_MASK;
	if( isStatic )
	{
		//puncho - static arena (P2b.1.8): stage 4 inherits this (the debug path, RenderDebug_NVRHI.cpp): no generation
		// test here (the frontend's CacheIsCurrent did); P2b.4.11 adds it
		vb->Reference( staticData.vertexBuffer, offset, size );
		return true;
	}
	if( frameNum != ( ( currentFrame - 1 ) & VERTCACHE_FRAME_MASK ) )
	{
		return false;
	}
	vb->Reference( frameData[ drawListNum ].vertexBuffer, offset, size );
	return true;
}

/*
==============
idVertexCache::GetIndexBuffer
==============
*/
bool idVertexCache::GetIndexBuffer( vertCacheHandle_t handle, idIndexBuffer* ib )
{
	const int isStatic = handle & VERTCACHE_STATIC;
	//puncho - static index (2R R3.1): the decoders; a static offset comes back in bytes (Reference takes an int64 since R3.2)
	const int size = CacheSizeBytes( handle );
	const int64 offset = CacheOffsetBytes( handle );
	const uint64 frameNum = ( int )( handle >> VERTCACHE_FRAME_SHIFT ) & VERTCACHE_FRAME_MASK;
	if( isStatic )
	{
		//puncho - static arena (P2b.1.8): stage 4 inherits this debug path unchecked too (P2b.4.11)
		ib->Reference( staticData.indexBuffer, offset, size );
		return true;
	}
	if( frameNum != ( ( currentFrame - 1 ) & VERTCACHE_FRAME_MASK ) )
	{
		return false;
	}
	ib->Reference( frameData[ drawListNum ].indexBuffer, offset, size );
	return true;
}

/*
==============
idVertexCache::GetJointBuffer
==============
*/
bool idVertexCache::GetJointBuffer( vertCacheHandle_t handle, idUniformBuffer* jb )
{
	const int isStatic = handle & VERTCACHE_STATIC;
	//puncho - static index (2R R3.1): the decoders
	const int numBytes = CacheSizeBytes( handle );
	const int64 jointOffset = CacheOffsetBytes( handle );
	const uint64 frameNum = ( int )( handle >> VERTCACHE_FRAME_SHIFT ) & VERTCACHE_FRAME_MASK;
	if( isStatic )
	{
		jb->Reference( staticData.jointBuffer, jointOffset, numBytes );
		return true;
	}
	if( frameNum != ( ( currentFrame - 1 ) & VERTCACHE_FRAME_MASK ) )
	{
		return false;
	}
	jb->Reference( frameData[ drawListNum ].jointBuffer, jointOffset, numBytes );
	return true;
}

/*
==============
idVertexCache::BeginBackEnd
==============
*/
void idVertexCache::BeginBackEnd()
{
	mostUsedVertex = Max( mostUsedVertex, frameData[ listNum ].vertexMemUsed.GetValue() );
	mostUsedIndex = Max( mostUsedIndex, frameData[ listNum ].indexMemUsed.GetValue() );
	mostUsedJoint = Max( mostUsedJoint, frameData[ listNum ].jointMemUsed.GetValue() );

#if !defined( ID_RETAIL )
	//puncho - static arena (P2b.1.8): r_staticTestStale arms one plant for the next frame's frontend (CacheIsCurrent);
	// no frontend job runs here
	if( r_staticTestStale.GetBool() )
	{
		staticTestStale.SetValue( 1 );
		r_staticTestStale.SetBool( false );
	}
#endif

	if( r_showVertexCache.GetBool() )
	{
		idLib::Printf( "%08d: %d allocations, %dkB vertex, %dkB index, %ikB joint : %dkB vertex, %dkB index, %ikB joint\n",
					   currentFrame, frameData[ listNum ].allocations,
					   frameData[ listNum ].vertexMemUsed.GetValue() / 1024,
					   frameData[ listNum ].indexMemUsed.GetValue() / 1024,
					   frameData[ listNum ].jointMemUsed.GetValue() / 1024,
					   mostUsedVertex / 1024,
					   mostUsedIndex / 1024,
					   mostUsedJoint / 1024 );
	}

	// unmap the current frame so the GPU can read it
	const int startUnmap = Sys_Milliseconds();
	UnmapGeoBufferSet( frameData[ listNum ] );
	UnmapGeoBufferSet( staticData );
	const int endUnmap = Sys_Milliseconds();
	if( endUnmap - startUnmap > 1 )
	{
		idLib::PrintfIf( r_showVertexCacheTimings.GetBool(), "idVertexCache::unmap took %i msec\n", endUnmap - startUnmap );
	}
	drawListNum = listNum;

	// prepare the next frame for writing to by the CPU
	currentFrame++;

	listNum = currentFrame % NUM_FRAME_DATA;
	const int startMap = Sys_Milliseconds();
	MapGeoBufferSet( frameData[ listNum ] );
	const int endMap = Sys_Milliseconds();
	if( endMap - startMap > 1 )
	{
		idLib::PrintfIf( r_showVertexCacheTimings.GetBool(), "idVertexCache::map took %i msec\n", endMap - startMap );
	}

	ClearGeoBufferSet( frameData[ listNum ] );
}

