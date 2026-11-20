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
#if !defined( ID_RETAIL )
idCVar r_staticArenaTestCapMB( "r_staticArenaTestCapMB", "0", CVAR_RENDERER | CVAR_FLOAT, "test only: the set size in MB the static arena's preflight decides against (0: the set's own); allocation keeps the whole set" );
idCVar r_staticTestStale( "r_staticTestStale", "0", CVAR_RENDERER | CVAR_BOOL, "test only: the next valid static handle CacheIsCurrent tests reads as stale once (split 1): it is counted and the caller re-uploads it" );
#endif

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
#if 1
	if( !AllocGeoBufferSet( staticData, STATIC_VERTEX_MEMORY, STATIC_INDEX_MEMORY, 0, BU_STATIC, commandList, failedKind, failedBytes ) )
#else
	if( !AllocGeoBufferSet( staticData, STATIC_VERTEX_MEMORY, STATIC_INDEX_MEMORY, 0, BU_DYNAMIC, commandList, failedKind, failedBytes ) )
#endif
	{
		Shutdown();
		idLib::FatalError( "idVertexCache::Init: static set: %s buffer of %lld bytes refused", failedKind, ( long long )failedBytes );
	}
	//puncho end

	//puncho start - static arena (P2b.1.8): split 0 until the first level load latches r_staticArenaSplit; generation 0
	// everywhere, so no handle is valid before its segment's first free
	memset( staticSets, 0, sizeof( staticSets ) );
	//puncho - static index (2R R3.1): the arena counts 16-byte units
	staticSets[ CACHE_VERTEX ].arena.size = STATIC_VERTEX_MEMORY >> STATIC_OFFSET_UNIT_BITS;
	staticSets[ CACHE_INDEX ].arena.size = STATIC_INDEX_MEMORY >> STATIC_OFFSET_UNIT_BITS;
	staticSets[ CACHE_VERTEX ].mark = -1;
	staticSets[ CACHE_INDEX ].mark = -1;
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
	// what the previous load allocated after its upload loops (its interactions' light tris) is the late use
	for( int type = CACHE_VERTEX; type <= CACHE_INDEX; type++ )
	{
		staticArenaStats_t& set = staticSets[ type ];
		const int64 used = StaticUsedBytes( ( cacheType_t )type ) - set.tail0;	//puncho - static index (2R R3.1): bytes as the byte counter held them (the counters hold units; without segment 0's tail)
		if( set.mark >= 0 )
		{
			set.late = Max( set.late, used - set.mark );
		}
		set.mark = -1;
	}
	staticUploadsRetained = 0;
	staticUploadsNew = 0;
	staticRetainedBytes = 0;
	staticNewBytes = 0;
	staticSegmentModels[0] = 0;
	staticSegmentModels[1] = 0;

	const bool split = r_staticArenaSplit.GetBool();
	staticSplitChanged = ( split != staticSplit );
	if( split && !staticSplitChanged && keepPersistent )
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
		int64 size = StaticSizeBytes( ( cacheType_t )type );	//puncho - static index (2R R3.1): bytes; the arena holds units
#if !defined( ID_RETAIL )
		if( r_staticArenaTestCapMB.GetFloat() > 0.0f )
		{
			size = Min( size, ( int64 )( r_staticArenaTestCapMB.GetFloat() * 1024.0f * 1024.0f ) );
		}
#endif
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
idVertexCache::EndStaticUploads

After the upload loops: what is allocated from here to the next load (light tris) is late use, in
segment 1, and the handles this load uploaded are valid from now on.
==============
*/
void idVertexCache::EndStaticUploads()
{
	//puncho - static index (2R R3.1): bytes as the byte counters held them (the counters hold units; without segment 0's
	// tail, as BeginStaticLoad reads the use it compares with this mark)
	staticSets[ CACHE_VERTEX ].mark = StaticUsedBytes( CACHE_VERTEX ) - staticSets[ CACHE_VERTEX ].tail0;
	staticSets[ CACHE_INDEX ].mark = StaticUsedBytes( CACHE_INDEX ) - staticSets[ CACHE_INDEX ].tail0;
	staticSplitChanged = false;
	staticSegment = 1;
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
vertCacheHandle_t idVertexCache::AllocStaticArena( const void* data, int bytes, cacheType_t type, nvrhi::ICommandList* commandList )
{
	if( bytes == 0 )
	{
		return ( vertCacheHandle_t )0;
	}
	assert( ( ( ( uintptr_t )( data ) ) & 15 ) == 0 );
	if( bytes > VERTCACHE_SIZE_MASK )
	{
		idLib::Error( "idVertexCache::AllocStaticArena: %i bytes exceeds the %i byte block limit", bytes, VERTCACHE_SIZE_MASK );
	}

	assert( type == CACHE_VERTEX || type == CACHE_INDEX );
	staticArenaStats_t& set = staticSets[ type ];
	const int segment = staticSplit ? staticSegment : 0;	//puncho - static index (2R R3.1): split 0 has one frontier
	assert( segment == 0 || segment == 1 );
	//puncho start - static index (2R R3.1)
	const int align = ( type == CACHE_INDEX ) ? INDEX_CACHE_ALIGN : VERTEX_CACHE_ALIGN;
	const int alignedBytes = ALIGN( bytes, align );
	const int unitOffset = StaticArenaAlloc( set.arena, segment, alignedBytes >> STATIC_OFFSET_UNIT_BITS, align >> STATIC_OFFSET_UNIT_BITS );
	if( unitOffset < 0 )
	{
		if( !staticSplit )
		{
			idLib::FatalError( "%s failed: %lld used + %i requested > %lld bytes", ( type == CACHE_INDEX ) ? "AllocStaticIndex" : "AllocStaticVertex",
							   ( long long )StaticUsedBytes( type ), bytes, ( long long )StaticSizeBytes( type ) );
		}
		// the forecast's residual (EndStaticPreflight): a map whose late use exceeds every earlier one by up to dead0
		idLib::FatalError( "%s failed: segment %i: %lld + %lld used + %i requested > %lld bytes (segment 0 dead %lld at the preflight, late %lld)",
						   ( type == CACHE_INDEX ) ? "AllocStaticIndex" : "AllocStaticVertex", segment, ( long long )StaticSegmentBytes( type, 0 ),
						   ( long long )StaticSegmentBytes( type, 1 ), bytes, ( long long )StaticSizeBytes( type ), ( long long )set.dead0, ( long long )set.late );
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

