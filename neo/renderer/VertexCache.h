/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2016-2017 Dustin Land
Copyright (C) 2022 Stephen Pridham
Copyright (C) 2024 Robert Beckebans

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
#ifndef __VERTEXCACHE_H__
#define __VERTEXCACHE_H__

#if 1

	// RB: quadruppled static memory limits for custom content

	const int VERTCACHE_INDEX_MEMORY_PER_FRAME = 31 * 1024 * 1024;
	const int VERTCACHE_VERTEX_MEMORY_PER_FRAME = 31 * 1024 * 1024;
	//puncho start - ai-harness: 1280 KB (was 256 KB). The joint buffer is a structured SRV bound at an offset, so
	// no 64 KB constant-buffer limit applies; the only cost is NUM_FRAME_DATA x this much host-visible memory.
	// A skinned monster with its head uploads about 3.1 KB per frame (256 B alignment): 256 KB overflowed at
	// about 85 in view with "Out of joint buffer cache" (a map-dropping idLib::Error), this holds about 400.
	const int VERTCACHE_JOINT_MEMORY_PER_FRAME = 1280 * 1024;
	//puncho end

	// there are a lot more static indexes than vertexes, because interactions are just new
	// index lists that reference existing vertexes
	//puncho - vertex cache (P2 review): only the index set had a measured need (178 MB on the 64K fixture); vertices
	// used 13 MB there, so their set stays at the 124 MB this fork had
	const int STATIC_INDEX_MEMORY = 256 * 1024 * 1024;
	const int STATIC_VERTEX_MEMORY = 4 * 31 * 1024 * 1024;	// make sure it fits in VERTCACHE_OFFSET_MASK!

	// vertCacheHandle_t packs size, offset, and frame number into 64 bits
	typedef uint64 vertCacheHandle_t;
	const int VERTCACHE_STATIC = 1;						// in the static set, not the per-frame set

	//puncho start - vertex cache (P2 review): the frame field keeps its 13 bits, because persistent handles exist
	// (jointsInvertedBuffer, the cached dynamic surfaces) and a stale one passes CacheIsCurrent at every wrap. The
	// 256 MB index set needs 28 offset bits, so the size field gives up one: 4 MB per block, which ActuallyAlloc
	// enforces. With 16-bit indexes no block nears it: 65536 verts are 2 MB, the GUI's 80000 verts 2.5 MB
	const int VERTCACHE_SIZE_SHIFT = 1;
	const int VERTCACHE_SIZE_MASK = 0x3fffff;			// 22 bits = 4 megs

	const int VERTCACHE_OFFSET_SHIFT = 23;
	const int VERTCACHE_OFFSET_MASK = 0xfffffff;		// 28 bits = 256 megs

	const int VERTCACHE_FRAME_SHIFT = 51;
	const int VERTCACHE_FRAME_MASK = 0x1fff;			// 13 bits = 8191 frames to wrap around

	compile_time_assert( STATIC_INDEX_MEMORY <= VERTCACHE_OFFSET_MASK + 1 && STATIC_VERTEX_MEMORY <= VERTCACHE_OFFSET_MASK + 1 );
	//puncho end


#else

	// RB: original values which are still good for low spec hardware and performance

	const int VERTCACHE_INDEX_MEMORY_PER_FRAME = 31 * 1024 * 1024;
	const int VERTCACHE_VERTEX_MEMORY_PER_FRAME = 31 * 1024 * 1024;
	const int VERTCACHE_JOINT_MEMORY_PER_FRAME = 256 * 1024;

	// there are a lot more static indexes than vertexes, because interactions are just new
	// index lists that reference existing vertexes
	const int STATIC_INDEX_MEMORY = 31 * 1024 * 1024;
	const int STATIC_VERTEX_MEMORY = 31 * 1024 * 1024;	// make sure it fits in VERTCACHE_OFFSET_MASK!

	// vertCacheHandle_t packs size, offset, and frame number into 64 bits
	typedef uint64 vertCacheHandle_t;
	const int VERTCACHE_STATIC = 1;					// in the static set, not the per-frame set
	const int VERTCACHE_SIZE_SHIFT = 1;
	const int VERTCACHE_SIZE_MASK = 0x7fffff;		// 8 megs
	const int VERTCACHE_OFFSET_SHIFT = 24;
	const int VERTCACHE_OFFSET_MASK = 0x1ffffff;	// 32 megs
	const int VERTCACHE_FRAME_SHIFT = 49;
	const int VERTCACHE_FRAME_MASK = 0x7fff;		// 15 bits = 32k frames to wrap around, python hex( ( 1 << 15 ) - 1 )

#endif

//puncho start - static arena (P2b.1.8): the frozen math core (plans/map-graph/gates-2b/s1/check/static_arena_core.h,
// FROZEN-arena.txt: moved verbatim but for static_assert -> compile_time_assert (messages dropped); its check runs
// against this copy). At r_staticArenaSplit 1 a static handle's frame field holds segment and generation
#ifndef STATIC_ARENA_CORE_H
#define STATIC_ARENA_CORE_H

// The including scope supplies uint64 as an unsigned 64-bit integer (the engine's
// existing typedef) and the VERTCACHE_ handle layout constants (VertexCache.h).
// At split 0 the caller retains the frame field and validity always fails. At
// split 1 the caller replaces that field; low bits, including the static flag,
// remain untouched. The caller owns load latching and generation invalidation.
const int STATIC_ARENA_SEGMENT_BITS = 6;
const int STATIC_ARENA_GENERATION_BITS = 7;
const int STATIC_ARENA_SEGMENT_SHIFT = 51;
const int STATIC_ARENA_GENERATION_SHIFT = 57;
const int STATIC_ARENA_MAX_SEGMENTS = 63;		// IDs 0..62; 63 means none
const int STATIC_ARENA_MAX_GENERATION = 127;	// 0 means never valid

// The MAX_ values double as the field masks, and the two fields must exactly replace
// the frame field: if VertexCache.h's layout changes, these fail before a handle does
compile_time_assert( STATIC_ARENA_MAX_SEGMENTS == ( 1 << STATIC_ARENA_SEGMENT_BITS ) - 1 );
compile_time_assert( STATIC_ARENA_MAX_GENERATION == ( 1 << STATIC_ARENA_GENERATION_BITS ) - 1 );
compile_time_assert( STATIC_ARENA_SEGMENT_SHIFT == VERTCACHE_FRAME_SHIFT );
compile_time_assert( STATIC_ARENA_GENERATION_SHIFT == STATIC_ARENA_SEGMENT_SHIFT + STATIC_ARENA_SEGMENT_BITS );
compile_time_assert( STATIC_ARENA_GENERATION_SHIFT + STATIC_ARENA_GENERATION_BITS == 64 );
compile_time_assert( ( 1 << ( STATIC_ARENA_SEGMENT_BITS + STATIC_ARENA_GENERATION_BITS ) ) - 1 == VERTCACHE_FRAME_MASK );
compile_time_assert( ( ( uint64 )VERTCACHE_OFFSET_MASK << VERTCACHE_OFFSET_SHIFT ) < ( ( uint64 )1 << STATIC_ARENA_SEGMENT_SHIFT ) );

/*
====================
StaticArenaPackTop
====================
*/
inline uint64 StaticArenaPackTop( int segment, int generation )
{
	return ( ( uint64 )( segment & STATIC_ARENA_MAX_SEGMENTS ) << STATIC_ARENA_SEGMENT_SHIFT ) |
		   ( ( uint64 )( generation & STATIC_ARENA_MAX_GENERATION ) << STATIC_ARENA_GENERATION_SHIFT );
}

/*
====================
StaticArenaSegment
====================
*/
inline int StaticArenaSegment( uint64 handle )
{
	return ( int )( ( handle >> STATIC_ARENA_SEGMENT_SHIFT ) & STATIC_ARENA_MAX_SEGMENTS );
}

/*
====================
StaticArenaGeneration
====================
*/
inline int StaticArenaGeneration( uint64 handle )
{
	return ( int )( ( handle >> STATIC_ARENA_GENERATION_SHIFT ) & STATIC_ARENA_MAX_GENERATION );
}

/*
====================
StaticArenaSetTop
====================
*/
inline uint64 StaticArenaSetTop( uint64 handle, int segment, int generation )
{
	const uint64 lowMask = ( ( uint64 )1 << STATIC_ARENA_SEGMENT_SHIFT ) - 1;
	return ( handle & lowMask ) | StaticArenaPackTop( segment, generation );
}

/*
====================
StaticArenaHandleValid

The caller selects static handles; this predicate does not inspect the static
flag. A nonreserved segment/generation at split 1 requires a 63-entry table.
====================
*/
inline bool StaticArenaHandleValid( bool split, uint64 handle, const int generations[STATIC_ARENA_MAX_SEGMENTS] )
{
	if( !split )
	{
		return false;
	}

	const int segment = StaticArenaSegment( handle );
	const int generation = StaticArenaGeneration( handle );
	return segment != STATIC_ARENA_MAX_SEGMENTS && generation != 0 && generation == generations[segment];
}

// Used bytes include alignment padding. The live regions are [0, used0) and
// [size - used1, size); equal frontiers are allowed. Only segments 0 and 1 bump.
struct staticArenaSet_t
{
	int		size;
	int		used0;
	int		used1;
};

/*
====================
StaticArenaFits

Reject invalid sets, nonpositive byte counts, non-power-of-two alignment, and
unsupported segments. Subtract within bounds before computing either start,
so even a set of INT_MAX bytes cannot overflow signed arithmetic.
====================
*/
inline bool StaticArenaFits( const staticArenaSet_t& set, int segment, int bytes, int align )
{
	if( ( segment != 0 && segment != 1 ) || bytes <= 0 || align <= 0 || ( align & ( align - 1 ) ) != 0 )
	{
		return false;
	}
	if( set.size < 0 || set.used0 < 0 || set.used1 < 0 || set.used0 > set.size || set.used1 > set.size - set.used0 )
	{
		return false;
	}

	const int freeBytes = set.size - set.used0 - set.used1;
	if( bytes > freeBytes )
	{
		return false;
	}

	const int alignMask = align - 1;
	if( segment == 0 )
	{
		const int padding = ( align - ( set.used0 & alignMask ) ) & alignMask;
		return padding <= freeBytes - bytes;
	}
	const int padding = ( set.size - set.used1 - bytes ) & alignMask;
	return padding <= freeBytes - bytes;
}

/*
====================
StaticArenaAlloc

Failure leaves the set unchanged. Starts, rather than byte counts, are aligned;
segment 0 charges leading padding and segment 1 charges trailing padding.
====================
*/
inline int StaticArenaAlloc( staticArenaSet_t& set, int segment, int bytes, int align )
{
	if( !StaticArenaFits( set, segment, bytes, align ) )
	{
		return -1;
	}

	const int alignMask = align - 1;
	if( segment == 0 )
	{
		const int offset = set.used0 + ( ( align - ( set.used0 & alignMask ) ) & alignMask );
		set.used0 = offset + bytes;
		return offset;
	}
	const int offset = ( set.size - set.used1 - bytes ) & ~alignMask;
	set.used1 = set.size - offset;
	return offset;
}

/*
====================
StaticArenaFree

Unsupported segments leave the set unchanged; generation changes are separate.
====================
*/
inline void StaticArenaFree( staticArenaSet_t& set, int segment )
{
	if( segment == 0 )
	{
		set.used0 = 0;
	}
	else if( segment == 1 )
	{
		set.used1 = 0;
	}
}

/*
====================
StaticArenaNextGeneration

Input is a decoded generation in [0, 127]. A fresh arena advances 0 to 1.
====================
*/
inline int StaticArenaNextGeneration( int generation )
{
	return ( generation == 0 || generation == STATIC_ARENA_MAX_GENERATION ) ? 1 : generation + 1;
}

#endif
//puncho end

const int VERTEX_CACHE_ALIGN		= 32;
const int INDEX_CACHE_ALIGN			= 16;
const int JOINT_CACHE_ALIGN			= 16;

enum cacheType_t
{
	CACHE_VERTEX,
	CACHE_INDEX,
	CACHE_JOINT
};

struct geoBufferSet_t
{
	idIndexBuffer			indexBuffer;
	idVertexBuffer			vertexBuffer;
	idUniformBuffer			jointBuffer;
	byte* 					mappedVertexBase;
	byte* 					mappedIndexBase;
	byte* 					mappedJointBase;
	idSysInterlockedInteger	indexMemUsed;
	idSysInterlockedInteger	vertexMemUsed;
	idSysInterlockedInteger	jointMemUsed;
	int						allocations;	// number of index and vertex allocations combined
};

//puncho start - static arena (P2b.1.8): one static set's bookkeeping, staticSets[ CACHE_VERTEX ], [ CACHE_INDEX ]; arena
// and high at split 1 only, late and mark at both; at split 1 geoBufferSet_t's vertexMemUsed and indexMemUsed mirror
// used0 + used1
struct staticArenaStats_t
{
	staticArenaSet_t		arena;			// segment 0 from offset 0 up, segment 1 from the end down
	int						high[2];		// per segment: the most bytes it held
	int						late;			// the most bytes allocated after a load's upload loops, before the next load
	int						mark;			// the set's bytes when the last upload loops ended, -1 before any
	int64					live0;			// preflight: the bytes segment 0's kept handles hold (ALIGN'd)
	int64					incoming;		// preflight: the bytes the upload loops will ask for (ALIGN'd)
	int64					dead0;			// the last preflight's dead bytes in segment 0 (0 when it did not walk)
};
compile_time_assert( sizeof( staticArenaStats_t ) * 2 >= 64 );	// keeps idVertexCache's two counters on separate cache lines
//puncho end

class idVertexCache
{
public:
	void			Init( int uniformBufferOffsetAlignment, nvrhi::ICommandList* commandList );
	void			Shutdown();
	void			PurgeAll( nvrhi::ICommandList* commandList );

	// call on loading a new map
	void			FreeStaticData();

	//puncho start - static arena (P2b.1.8): at r_staticArenaSplit 1 each static set is two-ended: segment 0 (persistent
	// models, reloadable or default, and their deform caches at LevelKeepGrace() >= 1) grows up from 0, segment 1 (the
	// map's own models, light tris, everything at grace 0) down from the end, so a game-driven load keeps segment 0's
	// handles and frees segment 1 only. Main thread only, as every static allocation is (EndLevelLoad's upload loops,
	// GenerateInteractionsForLight)
	void			BeginStaticLoad( bool keepPersistent );		// BeginLevelLoad: latch the split, free segment 1 or both
	void			SetStaticSegment( int segment );			// the segment the next static allocations go to
	void			FreeStaticSegment( int segment );			// reset: both sets' frontier, generation + 1
	// false at split 0; the static flag selects the handles the core may decode (a per-frame handle's frame field is not
	// a segment and generation)
	bool			StaticHandleValid( const vertCacheHandle_t handle ) const
	{
		return CacheIsStatic( handle ) && StaticArenaHandleValid( staticSplit && !staticSplitChanged, handle, staticGenerations );
	}
	// the creation functions' first test: false when both handles are kept (or have no bytes), and always in the preflight,
	// which only measures; true when the caller must zero both handles and upload both blocks
	bool			StaticUploadNeeded( vertCacheHandle_t indexHandle, int indexBytes, vertCacheHandle_t vertexHandle, int vertexBytes );
	bool			BeginStaticPreflight();						// false at split 0 or with segment 0 empty; else measure mode
	void			EndStaticPreflight( uint64 walkUs );		// the reset decision and its line
	void			EndStaticUploads();							// after the upload loops: late use starts, segment 1
	//puncho end

	// this data is only valid for one frame of rendering
	vertCacheHandle_t	AllocVertex( const void* data, int num, size_t size = sizeof( idDrawVert ), nvrhi::ICommandList* commandList = nullptr );
	vertCacheHandle_t	AllocIndex( const void* data, int num, size_t size = sizeof( triIndex_t ), nvrhi::ICommandList* commandList = nullptr );
	vertCacheHandle_t	AllocJoint( const void* data, int num, size_t size = sizeof( idJointMat ), nvrhi::ICommandList* commandList = nullptr );

	// this data is valid until the next map load
	vertCacheHandle_t	AllocStaticVertex( const void* data, int bytes, nvrhi::ICommandList* commandList );
	vertCacheHandle_t	AllocStaticIndex( const void* data, int bytes, nvrhi::ICommandList* commandList );

	byte* 			MappedVertexBuffer( vertCacheHandle_t handle );
	byte* 			MappedIndexBuffer( vertCacheHandle_t handle );

	// Returns false if it's been purged
	// This can only be called by the front end, the back end should only be looking at
	// vertCacheHandle_t that are already validated.
	bool			CacheIsCurrent( const vertCacheHandle_t handle );
	static bool		CacheIsStatic( const vertCacheHandle_t handle )
	{
		return ( handle & VERTCACHE_STATIC ) != 0;
	}

	// vb/ib is a temporary reference -- don't store it
	bool			GetVertexBuffer( vertCacheHandle_t handle, idVertexBuffer* vb );
	bool			GetIndexBuffer( vertCacheHandle_t handle, idIndexBuffer* ib );
	bool			GetJointBuffer( vertCacheHandle_t handle, idUniformBuffer* jb );

	void			BeginBackEnd();

public:
	int				currentFrame;	// for determining the active buffers
	int				listNum;		// currentFrame % NUM_FRAME_DATA
	int				drawListNum;	// (currentFrame-1) % NUM_FRAME_DATA

	geoBufferSet_t	staticData;
	geoBufferSet_t	frameData[ NUM_FRAME_DATA ];

	int				uniformBufferOffsetAlignment;

	// High water marks for the per-frame buffers
	int				mostUsedVertex;
	int				mostUsedIndex;
	int				mostUsedJoint;

	//puncho start - static arena (P2b.1.8). Ordered for CacheIsCurrent: staticTestStale, the two bools and the
	// generations it reads sit together, and staticStaleHandles, which frontend jobs write, comes first with staticSets
	// (over 64 bytes, asserted below the struct) between, so the two counters never share a cache line
	idSysInterlockedInteger	staticStaleHandles;	// since Init: stale static handles CacheIsCurrent refused (frontend jobs; each is then replaced by a per-frame copy)
	staticArenaStats_t	staticSets[2];				// [ CACHE_VERTEX ], [ CACHE_INDEX ]; arena and high at split 1 only, late and mark at both
	idSysInterlockedInteger	staticTestStale;	// dev: 1 while r_staticTestStale's plant is armed
	bool			staticSplit;				// r_staticArenaSplit, latched by BeginStaticLoad
	bool			staticSplitChanged;			// this load changed the split: no handle is valid until its uploads end
	int				staticGenerations[STATIC_ARENA_MAX_SEGMENTS];	// per segment; 0 is never valid
	bool			staticKeepPersistent;		// latched by BeginStaticLoad: split 1 and LevelKeepGrace() >= 1
	bool			staticMeasure;				// inside the preflight
	int				staticSegment;				// 0 or 1
	int				staticReports;				// creation calls that keep or upload bytes (StaticUploadNeeded)
	int				staticSegmentModels[2];		// per segment: the models holding static data after the last load
	int				staticResets;				// preflight resets of segment 0
	int				staticUploadsRetained;		// this load: uploads that replaced a static handle (resident models)
	int				staticUploadsNew;			// this load: uploads of geometry that had no static handle
	int64			staticRetainedBytes;
	int64			staticNewBytes;
	//puncho end

	//puncho start - static arena (P2b.1.8): the split-1 static allocation (AllocStaticVertex/Index) and CacheIsCurrent's
	// cold path
	vertCacheHandle_t	AllocStaticArena( const void* data, int bytes, cacheType_t type, nvrhi::ICommandList* commandList );
	bool			CountStaleHandle();
	//puncho end

	// Try to make room for <bytes> bytes
	vertCacheHandle_t	ActuallyAlloc( geoBufferSet_t& vcs, const void* data, int bytes, cacheType_t type, nvrhi::ICommandList* commandList );
};

// platform specific code to memcpy into vertex buffers efficiently
// 16 byte alignment is guaranteed
void CopyBuffer( byte* dst, const byte* src, int numBytes );

extern	idVertexCache	vertexCache;

#endif // __VERTEXCACHE_H__
