/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2014 Robert Beckebans

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

/*
==========================================================================================

THE LIGHT-LIST CORE

Pure functions over the flat tables of LightLists.h. Nothing from here down to the banner
THE ENGINE SIDE reads a cvar, a global or the clock, or names a type of the renderer: the
same text compiles in an offline test behind a shim of idLib, which checks it against brute
force.

==========================================================================================
*/

/*
==========================================================================================

	Arena

==========================================================================================
*/

/*
====================
R_LightListAlloc

A bump inside the view's one block. Lifetime: the frame ( the block is R_FrameAlloc'd ).
Running out is a sizing bug in R_LightListArenaBytes, never data: fatal.
====================
*/
void* R_LightListAlloc( lightListArena_t& arena, int bytes )
{
	const int at = ( arena.used + 15 ) & ~15;
	if( bytes < 0 || at + bytes > arena.size )
	{
		LL_FATAL( "R_LightListAlloc: arena too small" );
		return NULL;
	}
	arena.used = at + bytes;
	return arena.base + at;
}

/*
====================
R_LightListArenaBytes

Everything R_LightListSetup and R_LightListNumber carve, rounded per allocation. The tile bits and
the pool are separate blocks: they upload as buffers of their own.
====================
*/
int R_LightListArenaBytes( const lightListParms_t& parms, int numLights )
{
	int bytes = 0;
	bytes += 16 + Max( numLights, 1 ) * ( int )sizeof( int );					// recordOfLight
	bytes += 16 + LL_MAX_RECORDS * ( int )sizeof( int );						// lightOfRecord
	bytes += 2 * ( 16 + LL_MAX_RECORDS * ( int )sizeof( uint16 ) );				// features, importanceRank
	bytes += 16 + LL_MAX_RECORDS * ( int )sizeof( lightListRect_t );			// rect
	bytes += 16 + parms.numBins * LL_BIN_STRIDE * ( int )sizeof( uint32 );		// bins
	bytes += 16 + Max( numLights, 1 ) * ( int )sizeof( uint32 );				// R_LightListNumber's importance order
	return bytes;
}

/*
==========================================================================================

	First hook: the grid, the numbering, the bins, the tile rectangles

==========================================================================================
*/

/*
====================
R_LightListSetup
====================
*/
void R_LightListSetup( const lightListParms_t& parms, int numLights, lightListTables_t& t, lightListArena_t& arena )
{
	memset( &t, 0, sizeof( t ) );
	t.parms = parms;
	t.tilesX = ( parms.width + parms.tileSize - 1 ) / parms.tileSize;
	t.tilesY = ( parms.height + parms.tileSize - 1 ) / parms.tileSize;
	if( parms.tileSize < 1 || t.tilesX > LL_MAX_TILES || t.tilesY > LL_MAX_TILES || parms.numBins < 1 || parms.numBins > LL_MAX_BINS
			|| parms.poolCap > LL_MAX_POOL || parms.poolCap < LL_POOL_FIRST )
	{
		LL_FATAL( "R_LightListSetup: parms out of range" );
	}
	t.binScale = ( float )parms.numBins / parms.binFar;
	t.numLights = numLights;

	t.recordOfLight = ( int* )R_LightListAlloc( arena, Max( numLights, 1 ) * ( int )sizeof( int ) );
	t.lightOfRecord = ( int* )R_LightListAlloc( arena, LL_MAX_RECORDS * ( int )sizeof( int ) );
	t.features = ( uint16* )R_LightListAlloc( arena, LL_MAX_RECORDS * ( int )sizeof( uint16 ) );
	t.importanceRank = ( uint16* )R_LightListAlloc( arena, LL_MAX_RECORDS * ( int )sizeof( uint16 ) );
	t.rect = ( lightListRect_t* )R_LightListAlloc( arena, LL_MAX_RECORDS * ( int )sizeof( lightListRect_t ) );
	t.bins = ( uint32* )R_LightListAlloc( arena, parms.numBins * LL_BIN_STRIDE * ( int )sizeof( uint32 ) );
}

/*
====================
R_LightListNumber

Who gets a record, and which index. Importance ( scissor area, larger first; ties by input
order ) decides who survives LL_MAX_RECORDS; the survivors are then ordered by class ( narrow
clustered, wide clustered, fog-only ) and, inside each class, by the integer key
( far bin << 11 | rank ). Record 0 is the primary sun's whether or not one exists. Then the bins
( one range per class ) and every record's tile rectangle, which is the light's own scissor in
tiles: nothing is projected again. Pure: lights in, t out.
====================
*/
class idSort_LightListImportance : public idSort_Quick< uint32, idSort_LightListImportance >
{
public:
	const lightListLight_t* lights;
	int Compare( const uint32& a, const uint32& b ) const
	{
		if( lights[a].importance != lights[b].importance )
		{
			return ( lights[a].importance > lights[b].importance ) ? -1 : 1;
		}
		return ( int )a - ( int )b;			// a total order: idSort_Quick is not stable
	}
};

static void R_LightListRadixSort( uint32* keys, uint32* scratch, int num )	// 26-bit unique keys ( class 2, bin 13, rank 11 ), LSD, three 9-bit digits
{
	uint32* src = keys;
	uint32* dst = scratch;
	for( int shift = 0; shift < 27; shift += 9 )
	{
		int count[513];
		memset( count, 0, sizeof( count ) );
		for( int i = 0; i < num; i++ )
		{
			count[ ( ( src[i] >> shift ) & 511 ) + 1 ]++;
		}
		for( int d = 0; d < 512; d++ )
		{
			count[d + 1] += count[d];
		}
		for( int i = 0; i < num; i++ )
		{
			dst[ count[ ( src[i] >> shift ) & 511 ]++ ] = src[i];
		}
		uint32* swap = src;
		src = dst;
		dst = swap;
	}
	if( src != keys )
	{
		memcpy( keys, src, num * sizeof( uint32 ) );
	}
}

void R_LightListNumber( const lightListLight_t* lights, int numLights, lightListTables_t& t, lightListArena_t& arena )
{
	const int numBins = t.parms.numBins;

	uint32* order = ( uint32* )R_LightListAlloc( arena, Max( numLights, 1 ) * ( int )sizeof( uint32 ) );
	for( int i = 0; i < numLights; i++ )
	{
		order[i] = ( uint32 )i;
	}
	idSort_LightListImportance byImportance;
	byImportance.lights = lights;
	byImportance.Sort( order, ( unsigned int )numLights );

	// 1. the sun takes record 0; the others survive in importance order, LL_MAX_RECORDS - 1 of them
	int sun = -1;
	int survivor[LL_MAX_RECORDS];					// survivor rank -> input light
	uint32 keys[LL_MAX_RECORDS];
	uint32 scratch[LL_MAX_RECORDS];
	int numKept = 0;
	t.numCapped = 0;
	for( int rank = 0; rank < numLights; rank++ )
	{
		const int i = ( int )order[rank];
		const lightListLight_t& L = lights[i];
		if( L.primarySun && sun < 0 )
		{
			sun = i;
			continue;
		}
		if( L.zMax <= 0.0f || L.x0 > L.x1 || L.y0 > L.y1 )
		{
			continue;								// wholly behind the eye, or no pixel: lights nothing on screen
		}
		if( numKept == LL_MAX_RECORDS - 1 )
		{
			t.numCapped++;							// keeps the legacy path; counted, never silent
			continue;
		}
		const float extent = L.zMax - Max( L.zMin, 0.0f );
		const uint32 cls = !L.clustered ? 2u : ( ( extent >= t.parms.wideDepth ) ? 1u : 0u );
		const uint32 farBin = ( uint32 )R_LightListBinIndex( t, L.zMax );
		survivor[numKept] = i;
		keys[numKept] = ( cls << 24 ) | ( farBin << LL_RANK_BITS ) | ( uint32 )numKept;
		numKept++;
	}

	// 2. class, then far bin, then survivor rank: one unique integer each, the same order everywhere
	R_LightListRadixSort( keys, scratch, numKept );

	for( int i = 0; i < numLights; i++ )
	{
		t.recordOfLight[i] = -1;
	}
	t.numRecords = 1 + numKept;
	t.firstWide = 1;
	t.firstFogOnly = 1;
	t.lightOfRecord[0] = sun;
	t.features[0] = ( sun >= 0 ) ? lights[sun].features : 0;
	t.importanceRank[0] = 0;
	t.sunListed = ( sun >= 0 && lights[sun].clustered ) ? 1 : 0;
	t.rect[0].x0 = 1;								// the sun is never binned: no tile bit, no bin
	t.rect[0].x1 = 0;
	t.rect[0].y0 = 1;
	t.rect[0].y1 = 0;
	if( sun >= 0 )
	{
		t.recordOfLight[sun] = 0;
	}
	const int maxTileX = t.tilesX - 1;
	const int maxTileY = t.tilesY - 1;
	for( int r = 1; r < t.numRecords; r++ )
	{
		const uint32 key = keys[r - 1];
		const int rank = ( int )( key & ( ( 1u << LL_RANK_BITS ) - 1 ) );
		const int i = survivor[rank];
		const lightListLight_t& L = lights[i];
		t.lightOfRecord[r] = i;
		t.recordOfLight[i] = r;
		t.features[r] = L.features;
		t.importanceRank[r] = ( uint16 )rank;
		t.rect[r].x0 = ( int16 )idMath::ClampInt( 0, maxTileX, L.x0 / t.parms.tileSize );
		t.rect[r].x1 = ( int16 )idMath::ClampInt( 0, maxTileX, L.x1 / t.parms.tileSize );
		t.rect[r].y0 = ( int16 )idMath::ClampInt( 0, maxTileY, L.y0 / t.parms.tileSize );
		t.rect[r].y1 = ( int16 )idMath::ClampInt( 0, maxTileY, L.y1 / t.parms.tileSize );
		const uint32 cls = key >> 24;
		if( cls == 0 )
		{
			t.firstWide = r + 1;
		}
		if( cls <= 1 )
		{
			t.firstFogOnly = r + 1;
		}
	}
	t.firstWide = Max( t.firstWide, 1 );
	t.firstFogOnly = Max( t.firstFogOnly, t.firstWide );
	t.wordsPerTile = ( t.numRecords + 31 ) / 32;

	// 3. bins: walking records in index order, the first to reach a bin is its lo and the last its hi, per class
	for( int b = 0; b < numBins * LL_BIN_STRIDE; b++ )
	{
		t.bins[b] = ( ( b % LL_BIN_STRIDE ) == 3 ) ? 0u : LL_RANGE_EMPTY;
	}
	for( int r = 1; r < t.numRecords; r++ )
	{
		const lightListLight_t& L = lights[ t.lightOfRecord[r] ];
		const int cls = ( r < t.firstWide ) ? 0 : ( ( r < t.firstFogOnly ) ? 1 : 2 );
		const int b1 = R_LightListBinIndex( t, L.zMax );
		for( int b = R_LightListBinIndex( t, L.zMin ); b <= b1; b++ )
		{
			uint32& bin = t.bins[ b * LL_BIN_STRIDE + cls ];
			bin = Min( bin & 0xFFFFu, ( uint32 )r ) | ( ( uint32 )r << 16 );
		}
	}
}

/*
====================
R_LightListTileRows

One job per band of tile rows ( D79, the R_AddLights pattern ). A job writes only its own rows,
so any split of the rows gives the same bits.
====================
*/
void R_LightListTileRows( const lightListTables_t& t, int row0, int row1, uint32* tileBits )
{
	for( int r = 1; r < t.numRecords; r++ )
	{
		const lightListRect_t& rc = t.rect[r];
		const int y0 = Max( ( int )rc.y0, row0 );
		const int y1 = Min( ( int )rc.y1, row1 - 1 );
		if( rc.x0 > rc.x1 || y0 > y1 )
		{
			continue;
		}
		const uint32 bit = 1u << ( r & 31 );
		const int wordIndex = r >> 5;
		for( int y = y0; y <= y1; y++ )
		{
			uint32* row = tileBits + ( y * t.tilesX ) * t.wordsPerTile + wordIndex;
			for( int x = rc.x0; x <= rc.x1; x++ )
			{
				row[ x * t.wordsPerTile ] |= bit;
			}
		}
	}
}

static ID_INLINE int R_LightListBitCount( uint32 v )
{
	v = v - ( ( v >> 1 ) & 0x55555555u );
	v = ( v & 0x33333333u ) + ( ( v >> 2 ) & 0x33333333u );
	return ( int )( ( ( ( v + ( v >> 4 ) ) & 0x0F0F0F0Fu ) * 0x01010101u ) >> 24 );
}

static ID_INLINE int R_LightListLowBit( uint32 v )		// index of the lowest set bit; v != 0
{
	return R_LightListBitCount( ( v & ( 0u - v ) ) - 1u );
}

/*
====================
R_LightListCandidates
====================
*/
int R_LightListCandidates( const lightListTables_t& t, const uint32* tileBits, const uint32* mask, int px, int py, float viewZ,
						   int* out, int maxOut )
{
	const uint32* bin = t.bins + R_LightListBinIndex( t, viewZ ) * LL_BIN_STRIDE;
	const uint32* words = tileBits + ( ( py / t.parms.tileSize ) * t.tilesX + px / t.parms.tileSize ) * t.wordsPerTile;
	int num = 0;
	if( mask != NULL && ( mask[0] & 1u ) != 0 )
	{
		if( out != NULL && num < maxOut )
		{
			out[num] = 0;							// the sun: never binned, listed through bit 0
		}
		num++;
	}
	for( int range = 0; range < 2; range++ )		// narrow, then wide: ascending record order throughout
	{
		const int lo = ( int )( bin[range] & 0xFFFFu );
		const int hi = ( int )( bin[range] >> 16 );
		for( int w = lo >> 5; w <= ( hi >> 5 ) && lo <= hi; w++ )
		{
			uint32 m = words[w] & R_LightListWordMask( w, lo, hi );
			if( mask != NULL )
			{
				m &= mask[w];
			}
			while( m != 0 )
			{
				if( out != NULL && num < maxOut )
				{
					out[num] = w * 32 + R_LightListLowBit( m );
				}
				num++;
				m &= m - 1;
			}
		}
	}
	return num;
}

/*
==========================================================================================

	The model jobs: the run, and the path

==========================================================================================
*/

/*
====================
R_LightListFinishRun

The job appended the surface's record indices in the order its contacted lights came. This writes
the surface's mask ( wordsPerTile words: bit r set when the job listed r ) and rewrites the run from
the mask, so the run is an ascending set whatever the job appended: both loops then shade in the
same order, and a pair listed twice is shaded once. Returns the run's length.
====================
*/
int R_LightListFinishRun( const lightListTables_t& t, uint16* run, int count, uint32* mask )
{
	for( int w = 0; w < t.wordsPerTile; w++ )
	{
		mask[w] = 0;
	}
	for( int i = 0; i < count; i++ )
	{
		const int r = run[i];
		if( r >= t.firstFogOnly || ( r == 0 && !t.sunListed ) )
		{
			LL_FATAL( "R_LightListFinishRun: the job listed a record that may not be listed" );
			continue;
		}
		mask[ r >> 5 ] |= 1u << ( r & 31 );
	}
	int num = 0;
	for( int w = 0; w < t.wordsPerTile; w++ )
	{
		for( uint32 m = mask[w]; m != 0; m &= m - 1 )
		{
			run[num++] = ( uint16 )( w * 32 + R_LightListLowBit( m ) );
		}
	}
	return num;
}

/*
====================
R_LightListChoosePath

A hand first ( the forced path, then the material's tag ), then 07's rule, n <= F + C, on the tables
the surface is shaded with: C is what the Z-bin loop would evaluate per pixel UNDER THE SURFACE'S OWN
MASK, estimated at up to 64 samples weighted by the pixels each stands for. n <= F needs no sample:
the estimate cannot be negative. The sun is paid on both paths and is left out of both sides.
Pure: tables, shape, run and mask in; the path out, and how it was decided for the log.
====================
*/
int R_LightListChoosePath( const lightListTables_t& t, const uint32* tileBits, const lightListShape_t& shape,
						   const uint16* run, int count, const uint32* mask, int tag, int* how )
{
	const lightListParms_t& p = t.parms;
	if( p.forcePath >= 0 )
	{
		*how = LLH_FORCED;							// for A/B and for timing F; skips the sampling
		return ( p.forcePath == LLP_ZBIN ) ? LLP_ZBIN : LLP_LIST;
	}
	int unknown = 0;
	switch( tag )
	{
		case LLT_LIST:
			*how = LLH_TAG;
			return LLP_LIST;
		case LLT_ZBIN:
			*how = LLH_TAG;
			return LLP_ZBIN;
		default:
			unknown = LLH_UNKNOWN_TAG;				// counted by the pack; the material's parse warned at load
			break;
		case LLT_AUTO:
			break;
	}

	const int n = count - ( ( count > 0 && run[0] == 0 ) ? 1 : 0 );
	if( ( float )n <= p.fetchCost )
	{
		*how = LLH_SHORT | unknown;
		return LLP_LIST;
	}

	// the samples: cell centres of a 4 x 4 grid ( plane ) or 4 x 4 x 4 ( box ), each weighted by its projected area
	idVec3 normal( 0.0f, 0.0f, 0.0f );
	if( shape.numEdges == 2 )
	{
		normal = shape.edges[0].Cross( shape.edges[1] );
		normal.Normalize();
	}
	float sum = 0.0f;
	float weights = 0.0f;
	const int layers = ( shape.numEdges == 3 ) ? 4 : 1;
	for( int k = 0; k < layers; k++ )
	{
		for( int j = 0; j < 4; j++ )
		{
			for( int i = 0; i < 4; i++ )
			{
				idVec3 q = shape.origin + shape.edges[0] * ( ( ( float )i + 0.5f ) * 0.25f ) + shape.edges[1] * ( ( ( float )j + 0.5f ) * 0.25f );
				if( shape.numEdges == 3 )
				{
					q += shape.edges[2] * ( ( ( float )k + 0.5f ) * 0.25f );
				}
				if( q.z <= p.zNear )
				{
					continue;
				}
				const float x = ( q.x / q.z - p.tanLeft ) / ( p.tanRight - p.tanLeft ) * ( float )p.width;
				const float y = ( p.tanTop - q.y / q.z ) / ( p.tanTop - p.tanBottom ) * ( float )p.height;
				if( !( x >= 0.0f && x < ( float )p.width && y >= 0.0f && y < ( float )p.height ) )
				{
					continue;
				}
				const float z3 = q.z * q.z * q.z;
				const float weight = ( ( shape.numEdges == 3 ) ? q.Length() : idMath::Fabs( normal * q ) ) / z3;
				const uint32* bin = t.bins + R_LightListBinIndex( t, q.z ) * LL_BIN_STRIDE;
				const uint32* words = tileBits + ( ( ( int )y / p.tileSize ) * t.tilesX + ( int )x / p.tileSize ) * t.wordsPerTile;
				int candidates = 0;
				for( int range = 0; range < 2; range++ )
				{
					const int lo = ( int )( bin[range] & 0xFFFFu );
					const int hi = ( int )( bin[range] >> 16 );
					for( int w = lo >> 5; w <= ( hi >> 5 ) && lo <= hi; w++ )
					{
						candidates += R_LightListBitCount( words[w] & R_LightListWordMask( w, lo, hi ) & mask[w] );
					}
				}
				sum += weight * ( float )candidates;
				weights += weight;
			}
		}
	}
	if( !( weights > 0.0f ) )
	{
		*how = LLH_UNSAMPLED | unknown;
		return ( n <= p.unsampledMax ) ? LLP_LIST : LLP_ZBIN;
	}
	*how = LLH_SAMPLED | unknown;
	return ( ( float )n <= p.fetchCost + sum / weights ) ? LLP_LIST : LLP_ZBIN;
}

/*
==========================================================================================

	Second hook: the pool

==========================================================================================
*/

/*
====================
R_LightListPack

Serial, in draw order. Pool word 0 holds 0 ( the sun's record index, which the list loop reads on the
Z-bin path ) and words [ 1, 33 ) are the shared all-ones mask. A list-path run is copied after them,
one word per record index; a Z-bin surface gets its mask, wordsPerTile words.
The pool full: a run that does not fit becomes its mask ( at most 32 words ); a mask that does not
fit points at the all-ones mask, where the surface keeps every light it lists and may gain others
( loud: counted, the glue warns, the path view paints it magenta ) - never a dropped light. Every
record a run holds is counted as listed whichever path the surface takes ( its shadows must survive
R_OptimizeViewLightsList, D9 ). A run that is not an ascending set of listable records is a job bug: fatal.
====================
*/
void R_LightListPack( const lightListTables_t& t, lightListSurface_t** surfs, int numSurfs, uint32* pool,
					  int* listedOfRecord, lightListStats_t& stats )
{
	pool[LL_POOL_SUN] = 0;
	for( int w = 0; w < LL_MAX_WORDS; w++ )
	{
		pool[LL_POOL_ALL_ONES + w] = 0xFFFFFFFFu;
	}
	if( !t.sunListed )
	{
		pool[LL_POOL_ALL_ONES] &= ~1u;				// no sun to list: the unmasked surface must not evaluate record 0
	}
	int used = LL_POOL_FIRST;
	for( int s = 0; s < numSurfs; s++ )
	{
		lightListSurface_t& surf = *surfs[s];
		uint32 features = 0;
		for( int i = 0; i < surf.count; i++ )
		{
			const int r = surf.run[i];
			if( r >= t.firstFogOnly || ( r == 0 && !t.sunListed ) || ( i > 0 && surf.run[i - 1] >= r ) )
			{
				LL_FATAL( "R_LightListPack: a run is not an ascending set of listable records" );
				surf.count = ( int16 )i;				// the test build goes on: what was checked is kept
				break;
			}
			listedOfRecord[r]++;
			features |= t.features[r];
		}
		stats.listedPairs += surf.count;
		stats.how[ surf.how & 0x7F ]++;
		stats.unknownTags += ( surf.how & LLH_UNKNOWN_TAG ) ? 1 : 0;
		surf.featureMask = features;
		surf.first = LL_POOL_ALL_ONES;

		if( surf.path == LLP_LIST )
		{
			if( used + surf.count <= t.parms.poolCap )
			{
				surf.first = ( uint32 )used;
				for( int i = 0; i < surf.count; i++ )
				{
					pool[used++] = surf.run[i];
				}
				stats.listSurfs++;
				continue;
			}
			stats.poolOverflow++;					// the run does not fit: its mask may
			surf.path = LLP_ZBIN;
		}
		if( used + t.wordsPerTile <= t.parms.poolCap )
		{
			surf.path = LLP_ZBIN;
			surf.first = ( uint32 )used;
			for( int w = 0; w < t.wordsPerTile; w++ )
			{
				pool[used + w] = 0;
			}
			for( int i = 0; i < surf.count; i++ )
			{
				pool[ used + ( surf.run[i] >> 5 ) ] |= 1u << ( surf.run[i] & 31 );
			}
			used += t.wordsPerTile;
			stats.zbinSurfs++;
			continue;
		}
		surf.path = LLP_ZBIN_ALL;					// nothing fits: every clustered record of the pixel's tile and bin
		surf.first = LL_POOL_ALL_ONES;
		surf.featureMask = LLF_ALL;
		stats.zbinAllSurfs++;
	}
	stats.records = t.numRecords;
	stats.narrow = t.firstWide - 1;
	stats.wide = t.firstFogOnly - t.firstWide;
	stats.fogOnly = t.numRecords - t.firstFogOnly;
	stats.capped = t.numCapped;
	stats.poolUsed = used;
}

static ID_INLINE uint32 R_LightListFnv( uint32 h, uint32 v )
{
	for( int i = 0; i < 4; i++ )
	{
		h = ( h ^ ( ( v >> ( i * 8 ) ) & 0xFFu ) ) * 16777619u;
	}
	return h;
}

/*
====================
R_LightListHash

The view's one number: if two machines, two runs or two builds print the same hash for the same
logged input, they built the same tables. pool and surfs may be NULL ( the tables alone ).
====================
*/
uint32 R_LightListHash( const lightListTables_t& t, const uint32* tileBits, const uint32* pool, int poolUsed,
						lightListSurface_t* const* surfs, int numSurfs )
{
	uint32 h = 2166136261u;
	h = R_LightListFnv( h, ( uint32 )t.numRecords );
	h = R_LightListFnv( h, ( uint32 )t.firstWide );
	h = R_LightListFnv( h, ( uint32 )t.firstFogOnly );
	h = R_LightListFnv( h, ( uint32 )t.sunListed );
	for( int r = 0; r < t.numRecords; r++ )
	{
		h = R_LightListFnv( h, ( uint32 )t.lightOfRecord[r] );
	}
	for( int b = 0; b < t.parms.numBins * LL_BIN_STRIDE; b++ )
	{
		h = R_LightListFnv( h, t.bins[b] );
	}
	const int numWords = t.tilesX * t.tilesY * t.wordsPerTile;
	for( int w = 0; w < numWords; w++ )
	{
		h = R_LightListFnv( h, tileBits[w] );
	}
	for( int s = 0; s < numSurfs; s++ )
	{
		h = R_LightListFnv( h, ( uint32 )surfs[s]->path | ( ( uint32 )surfs[s]->count << 2 ) );
		h = R_LightListFnv( h, surfs[s]->first );
	}
	for( int i = 0; pool != NULL && i < poolUsed; i++ )
	{
		h = R_LightListFnv( h, pool[i] );
	}
	return h;
}

/*
==========================================================================================

THE ENGINE SIDE

viewLight_t into the core's flat input, cvars into its parms, and its tables back into
viewLight_t::recordNumber, the C1 records, the log line and the commands. Two hooks, both on
the frontend's serial path: R_NumberRecordLights after R_AddLights ( the lights are final and
no model job has run ) and R_BuildLightLists after R_AddModels. Until the forward pass exists
( F2 ) no surface lists a light, so nothing here changes what is drawn.

==========================================================================================
*/

idCVar r_useForwardLists( "r_useForwardLists", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "build the light records, Z-bins and tile bits of the hybrid forward pass in the main view ( needs r_useShadowAtlas )" );
idCVar r_forwardLightKinds( "r_forwardLightKinds", "3", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "light kinds that may be listed: 1 = math lights, 2 = default-texture lights, 4 = the primary sun ( not listed yet: the bit is ignored )" );
idCVar r_zbinTileSize( "r_zbinTileSize", "16", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "pixels per Z-bin tile side", 8, 64 );
idCVar r_zbinCount( "r_zbinCount", "8192", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "Z-bins per view", 1, LL_MAX_BINS );
idCVar r_zbinFar( "r_zbinFar", "16384", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "view distance the Z-bins cover; beyond it, the last bin" );
idCVar r_zbinWideDepth( "r_zbinWideDepth", "2048", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "clustered lights whose view-depth extent is at least this take the wide range of the Z-bins" );
idCVar r_surfLightForcePath( "r_surfLightForcePath", "-1", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "-1 = material tags, then the rule; 0 = every surface loops its run; 1 = every surface loops its Z-bins", -1, 1 );
idCVar r_surfLightFetchCost( "r_surfLightFetchCost", "1", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "the Z-bin lookup, in light iterations ( F of the rule )" );
idCVar r_surfLightUnsampledMax( "r_surfLightUnsampledMax", "8", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "longest run a surface with no sample on screen loops" );
idCVar r_showLightLists( "r_showLightLists", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "1 = lights looped per pixel ( a heatmap over the view, drawn before the temporal filter, which smears it in motion ), 2 = path per surface ( green list, blue Z-bins, magenta pool overflow ), 3 = run length", 0, 3 );
idCVar r_lightListLog( "r_lightListLog", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "print one line per listing view: records by class, surfaces by path and by reason, pool, overflow, hash" );
idCVar r_lightListStrict( "r_lightListStrict", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "a full table of the light lists ( record cap, pool ) or an unknown forward tag is a FatalError instead of a warning ( developer machines )" );
idCVar r_lightListCapture( "r_lightListCapture", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "copy the next listing view's builder input for lightListBench and lightListDump, then clear" );
idCVar r_forwardMaterials( "r_forwardMaterials", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "1 = every material the forward pass can draw ( one bump, one diffuse and at most one specular stage, no texture matrix, no vertex colour ) lists its lights, with or without the forward keyword" );
idCVar r_showLightDiff( "r_showLightDiff", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "1 = twin mode: a listed pair also keeps its lit surface, so the legacy pass lights it and the forward pass adds no light ( the legacy image, with the lists built beside it ); 2 = the difference view: the lit passes are drawn twice, forward and legacy, the screen shows their difference times r_lightDiffScale and the HUD its mean and maximum", 0, 2 );

static bool lightListSaidNoAtlas = false;		// the console says once that the lists need the shadow atlas
static int lightListWarned = 0;					// R_LightListLoud: the kinds of warning this map has had

const lightListSurface_t lightListTwinMark = { NULL, 0, 0, LLH_TWIN, 0, 0 };	// see LightLists.h

static void R_LightListCapture( const lightListView_t* llv );

/*
====================
R_LightListRecordFlags

The record rule ( D40, D57 ) and the listing rule as one function per light. Returns 0 when the light has no
record; RLF_CLUSTERED in the result says it may be listed. Computed once per view in the first hook; it travels
in the record's class and flags and is never tested again.
====================
*/
static uint32 R_LightListRecordFlags( const viewLight_t* vLight, int* unknownModes )
{
	const idMaterial* shader = vLight->lightShader;
	if( shader == NULL || shader->IsBlendLight() )
	{
		return 0;
	}
	for( int i = 0; i < shader->GetNumStages(); i++ )
	{
		if( shader->GetStage( i )->texture.hasMatrix )
		{
			return 0;															// D57: no record, legacy path, no fog
		}
	}
	if( shader->IsFogLight() )
	{
		return RLF_FOGLIGHT;													// inject only, binned ( D81 )
	}
	if( shader->IsAmbientLight() )
	{
		return RLF_AMBIENT;
	}

	uint32 flags = ( vLight->pointLight ? RLF_POINT : 0 ) | ( vLight->parallel ? RLF_PARALLEL : 0 ) | ( vLight->isPrimarySun ? RLF_SUN : 0 );
	// a record carries one colour, so only a light with one stage can be expressed by the loop
	const bool oneStage = shader->GetNumStages() == 1;
	const bool math = R_LightUsesMathPath( vLight );
	bool defaults = false;
	if( oneStage )
	{
		// D39: the default textures of the light's own kind, by image pointer, projection and falloff both. The two
		// default shaders share one projection image, so only the falloff tells them apart, and a projected light
		// with the point light's falloff is not a default light. The falloff is the one the light is drawn with
		// ( viewLight_t::falloffImage ): a light shader without a falloff image of its own, which is the usual
		// case, takes the default shader's
		// The forward layout binds ONE projection image, the default point light's ( and the two falloff images ). So
		// the projection must be that image too: a projected light stays a default light only while the two default
		// shaders share their projection image, which is this game's data, not the code
		const idMaterial* defaultShader = vLight->pointLight ? tr.defaultPointLight : tr.defaultProjectedLight;
		defaults = shader->GetStage( 0 )->texture.image == defaultShader->GetStage( 0 )->texture.image && vLight->falloffImage == defaultShader->LightFalloffImage()
				   && shader->GetStage( 0 )->texture.image == tr.defaultPointLight->GetStage( 0 )->texture.image;
	}
	flags |= ( math ? RLF_MATH : 0 ) | ( defaults ? RLF_DEFAULT_TEXTURES : 0 );

	const int kinds = r_forwardLightKinds.GetInteger() & 3;						// bit 4, the primary sun, is F4's: the forward shader has no cascades yet
	bool listable = oneStage && ( ( math && ( kinds & 1 ) != 0 ) || ( defaults && ( kinds & 2 ) != 0 ) );
	switch( vLight->lightDef->parms.forwardMode )
	{
		case LIGHT_FORWARD_AUTO:
			break;
		case LIGHT_FORWARD_LIST:
			listable = oneStage && ( math || defaults );						// the hand forces it; the loop still has to express it
			break;
		case LIGHT_FORWARD_LEGACY:
			listable = false;
			break;
		default:
			( *unknownModes )++;												// counted and warned; read as LIGHT_FORWARD_AUTO
			break;
	}
	if( vLight->parallel && ( !vLight->isPrimarySun || ( kinds & 4 ) == 0 ) )
	{
		listable = false;														// D42: only the primary sun, through record 0, from F4
	}
	return flags | ( listable ? RLF_CLUSTERED : 0 );
}

/*
====================
R_LightListFlattenRecord

One C1 record from one view light, in the frame relative to the view's renderOrigin, which is what viewLight_t's
positional fields already are. The colour is taken once, here ( D58 ): the first stage of the light shader that
passes its condition, times r_lightScale and the light's distance-cull fade, the product the interaction pass
forms per stage. A fog light's record is read the fog pass's way instead ( D81 ): the raw stage colour, and the
fog distance in w. shadowRow stays -1 until R_BuildLightLists gives the light its rows.
====================
*/
static void R_LightListFlattenRecord( const viewLight_t* vLight, uint32 flags, int importanceRank, float viewLightScale, recordLight_t& rec )
{
	memset( &rec, 0, sizeof( rec ) );
	const idRenderLightLocal* light = vLight->lightDef;

	// the binning sphere: centred on the shading origin, holding the light's bounds ( a length: the same in the
	// map's frame, where the light def has both, and in the relative frame ). A parallel light has none.
	float radiusSq = 0.0f;
	if( !vLight->parallel )
	{
		for( int a = 0; a < 3; a++ )
		{
			const float reach = Max( light->globalLightOrigin[a] - light->globalLightBounds[0][a], light->globalLightBounds[1][a] - light->globalLightOrigin[a] );
			radiusSq += reach * reach;
		}
	}
	rec.originInvRadiusSq.Set( vLight->globalLightOrigin.x, vLight->globalLightOrigin.y, vLight->globalLightOrigin.z, ( radiusSq > 0.0f ) ? ( 1.0f / radiusSq ) : 0.0f );

	const idMaterial* shader = vLight->lightShader;
	const float* regs = vLight->shaderRegisters;
	// the first stage that passes its condition; for a fog light the first stage whatever its condition, which is
	// the one the fog pass reads
	int stageNum = 0;
	while( ( flags & RLF_FOGLIGHT ) == 0 && stageNum < shader->GetNumStages() && regs[ shader->GetStage( stageNum )->conditionRegister ] == 0.0f )
	{
		stageNum++;
	}
	if( stageNum < shader->GetNumStages() )
	{
		const int* reg = shader->GetStage( stageNum )->color.registers;
		if( ( flags & RLF_FOGLIGHT ) != 0 )
		{
			const float distance = regs[ reg[3] ];
			rec.color.Set( regs[ reg[0] ], regs[ reg[1] ], regs[ reg[2] ], ( distance <= 1.0f ) ? 500.0f : distance );
		}
		else
		{
			const float lightScale = viewLightScale * vLight->cullFade;
			rec.color.Set( lightScale * regs[ reg[0] ], lightScale * regs[ reg[1] ], lightScale * regs[ reg[2] ], 0.0f );
		}
	}
	else
	{
		stageNum = 0;															// no live stage: a black record
	}
	// no cone term: the projection planes shape a projected light, so scale 0 and bias 1 on every record
	rec.spotDir.Set( 0.0f, 0.0f, 0.0f, 1.0f );
	rec.shadowRow = -1;
	const int spectrum = shader->Spectrum();
	rec.channelMask = ( spectrum > 0 ) ? ( 1u << Min( spectrum + 1, 31 ) ) : 1u;
	// "moved" is any update of the light def in this frame, a change of colour alone included: the def keeps no
	// stamp for a change of shape
	rec.flags = flags | ( light->parms.noSpecular ? RLF_NOSPECULAR : 0 ) | ( ( light->lastModifiedFrameNum == tr.frameCount ) ? RLF_MOVED_THIS_FRAME : 0 );
	rec.uniqueId = light->uniqueId;
	for( int i = 0; i < 4; i++ )
	{
		rec.lightProject[i] = vLight->lightProject[i].ToVec4();
	}
	if( ( flags & RLF_MATH ) != 0 )
	{
		// the interaction pass's own encode and its mode lane: 1 = math on, 2 to 4 = the debug views
		R_MathLightToParms( vLight->mathLight, 1.0f + ( float )r_showMathLights.GetInteger(), rec.mathLight );
	}
	rec.ids.Set( ( float )importanceRank, ( float )stageNum, -1.0f, 0.0f );
}

/*
====================
R_LightListHistory

For the volumetrics' history ( D56, D82 ), in the fog-eligible view only: ids.z of each record becomes the record
index its light had in the previous fog-eligible view ( -1 = none ), and vanishedPrev lists the records of that
view whose lights have no record now. Lights are matched by their def index and uniqueId. Views are told apart by
a serial, not by the frame, because one frame can hold two such views ( the two eyes of a stereo frame ). The
table lives across frames; it restarts with the world and wherever the engine drops its own previous-frame state.
====================
*/
struct lightListPrev_t
{
	int		uniqueId;			// of the light this def index last had a record for
	int		serial;				// of the fog-eligible view that wrote the slot
	int		record;				// the light's record index in that view
};

static idList<lightListPrev_t, TAG_RENDER>	lightListPrevOfDef;				// by idRenderLightLocal::index
static int									lightListPrevDefOfRecord[MAX_RECORD_LIGHTS];	// the previous fog-eligible view: record -> def index, -1 = the empty sun slot
static int									lightListPrevIdOfRecord[MAX_RECORD_LIGHTS];
static int									lightListPrevNumRecords;
static int									lightListPrevSerial;			// of that view; 0 = none
static int									lightListPrevFrame;				// its tr.frameCount; 0 = none
static int									lightListHistorySerial;			// counts the fog-eligible views
static const idRenderWorld*					lightListPrevWorld;

static void R_LightListHistory( const viewDef_t* viewDef, lightListView_t* llv, recordLight_t* records )
{
	const lightListTables_t& t = llv->tables;
	// the serial restarts with the table, so it never wraps: every slot is zero again, which no view's serial is
	if( lightListPrevWorld != viewDef->renderWorld || viewDef->invalidateHistory || lightListHistorySerial == INT_MAX )
	{
		lightListPrevWorld = viewDef->renderWorld;
		lightListPrevOfDef.SetNum( 0 );
		lightListPrevNumRecords = 0;
		lightListPrevSerial = 0;
		lightListPrevFrame = 0;
		lightListHistorySerial = 0;
	}
	// one slot per light def of the world, sized once a view: a def's index never passes the world's list
	const int numDefs = viewDef->renderWorld->lightDefs.Num();
	const int oldNum = lightListPrevOfDef.Num();
	if( numDefs > oldNum )
	{
		lightListPrevOfDef.SetNum( numDefs );
		memset( &lightListPrevOfDef[oldNum], 0, ( numDefs - oldNum ) * sizeof( lightListPrev_t ) );
	}
	llv->prevFrame = lightListPrevFrame;
	const int serial = ++lightListHistorySerial;

	for( int r = 0; r < t.numRecords; r++ )
	{
		const int input = t.lightOfRecord[r];
		if( input < 0 )
		{
			continue;															// the empty sun slot
		}
		const idRenderLightLocal* light = llv->lightOfInput[input]->lightDef;
		assert( light->index >= 0 && light->index < lightListPrevOfDef.Num() );
		lightListPrev_t& prev = lightListPrevOfDef[light->index];
		if( lightListPrevSerial != 0 && prev.serial == lightListPrevSerial && prev.uniqueId == light->uniqueId )
		{
			records[r].ids.z = ( float )prev.record;
		}
		prev.uniqueId = light->uniqueId;
		prev.serial = serial;
		prev.record = r;
	}

	// a previous record vanished when its def index has no record now, or has one for another light
	llv->vanishedPrev = ( int* )R_FrameAlloc( Max( lightListPrevNumRecords, 1 ) * sizeof( int ), FRAME_ALLOC_UNKNOWN );
	llv->numVanishedPrev = 0;
	for( int r = 0; r < lightListPrevNumRecords; r++ )
	{
		const int def = lightListPrevDefOfRecord[r];
		if( def < 0 )
		{
			continue;
		}
		const lightListPrev_t& prev = lightListPrevOfDef[def];
		if( prev.serial != serial || prev.uniqueId != lightListPrevIdOfRecord[r] )
		{
			llv->vanishedPrev[ llv->numVanishedPrev++ ] = r;
		}
	}

	// this view is the previous one of the next
	lightListPrevNumRecords = t.numRecords;
	for( int r = 0; r < t.numRecords; r++ )
	{
		const int input = t.lightOfRecord[r];
		lightListPrevDefOfRecord[r] = ( input >= 0 ) ? llv->lightOfInput[input]->lightDef->index : -1;
		lightListPrevIdOfRecord[r] = ( input >= 0 ) ? llv->lightOfInput[input]->lightDef->uniqueId : 0;
	}
	lightListPrevSerial = serial;
	lightListPrevFrame = tr.frameCount;
}

struct lightListRowJob_t
{
	const lightListTables_t*	tables;
	int							row0;
	int							row1;
	uint32*						tileBits;
};

static void R_LightListRowJob( lightListRowJob_t* job )
{
	R_LightListTileRows( *job->tables, job->row0, job->row1, job->tileBits );
}
REGISTER_PARALLEL_JOB( R_LightListRowJob, "R_LightListRowJob" );

/*
====================
R_NumberRecordLights

The first hook: right after R_AddLights, before any model job. Everything the jobs and the backend need about the
lights is decided here, once: who has a record, its index ( the only numbering ), its class, the bins, the tile
bits and the records themselves. Lights are final now; the jobs read the tables and never write them. Nothing is
projected again: a record's tile rectangle is the light's scissor and its bin range the scissor's depth range,
both from R_AddSingleLight.
====================
*/
void R_NumberRecordLights()
{
	viewDef_t* viewDef = tr.viewDef;
	viewDef->lightListView = NULL;
	viewDef->recordLights = NULL;
	viewDef->numRecordLights = 0;
	viewDef->numShadowRows = 0;
	viewDef->listsBuilt = false;
	for( viewLight_t* vLight = viewDef->viewLights; vLight != NULL; vLight = vLight->next )
	{
		vLight->recordNumber = -1;
		vLight->numListedSurfs = 0;
	}
	if( viewDef->invalidateHistory )
	{
		lightListWarned = 0;													// the first main view of a map: R_LightListLoud warns again
	}
	if( r_useForwardLists.GetBool() && r_useShadowAtlas.GetInteger() == 0 && !lightListSaidNoAtlas )
	{
		lightListSaidNoAtlas = true;
		common->Printf( "lightlists: r_useForwardLists needs r_useShadowAtlas: with the atlas off it reads as 0\n" );
	}
	// No view lists a pair unless the forward pass runs in it, and lists need the atlas; the sky view, mirrors and
	// GUI worlds build nothing ( D40, D79 ). The volumetrics will join their own switch here: records and bins
	// without lists.
	const bool wantLists = r_useForwardLists.GetBool() && r_useShadowAtlas.GetInteger() != 0 && viewDef->isMainView
						   && ( viewDef->renderView.rdflags & ( RDF_IRRADIANCE | RDF_NOAMBIENT ) ) == 0;
	const bool wantRecords = wantLists;
	if( !wantRecords )
	{
		return;
	}

	int numLights = 0;
	for( viewLight_t* vLight = viewDef->viewLights; vLight != NULL; vLight = vLight->next )
	{
		numLights++;
	}

	lightListParms_t parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.width = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	parms.height = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;
	// the projection's side planes as view x / z: ndc x = P[0] * x / z - P[8] ( column-major, the eye looks down -z );
	// the unjittered matrix, so the rule's samples do not move with the TAA jitter
	const float* P = viewDef->unjitteredProjectionMatrix;
	parms.tanLeft = ( -1.0f + P[8] ) / P[0];
	parms.tanRight = ( 1.0f + P[8] ) / P[0];
	parms.tanBottom = ( -1.0f + P[9] ) / P[5];
	parms.tanTop = ( 1.0f + P[9] ) / P[5];
	parms.zNear = viewDef->renderView.cramZNear ? ( r_znear.GetFloat() * 0.25f ) : r_znear.GetFloat();	// as R_SetupProjectionMatrix
	// a tile is as large as r_zbinTileSize asks, and larger where the view would pass the builder's LL_MAX_TILES
	// tiles a side ( 8-pixel tiles in a view wider than 4,096 pixels )
	parms.tileSize = Max( r_zbinTileSize.GetInteger(), ( Max( parms.width, parms.height ) + LL_MAX_TILES - 1 ) / LL_MAX_TILES );
	parms.numBins = r_zbinCount.GetInteger();
	parms.binFar = Max( r_zbinFar.GetFloat(), 1.0f );
	parms.wideDepth = r_zbinWideDepth.GetFloat();
	parms.forcePath = r_surfLightForcePath.GetInteger();
	parms.fetchCost = r_surfLightFetchCost.GetFloat();
	parms.unsampledMax = r_surfLightUnsampledMax.GetInteger();
	parms.poolCap = LL_MAX_POOL;

	lightListView_t* llv = ( lightListView_t* )R_ClearedFrameAlloc( sizeof( lightListView_t ), FRAME_ALLOC_UNKNOWN );
	llv->lights = ( lightListLight_t* )R_FrameAlloc( Max( numLights, 1 ) * sizeof( lightListLight_t ), FRAME_ALLOC_UNKNOWN );
	llv->lightOfInput = ( viewLight_t** )R_FrameAlloc( Max( numLights, 1 ) * sizeof( viewLight_t* ), FRAME_ALLOC_UNKNOWN );
	llv->flagsOfInput = ( uint32* )R_FrameAlloc( Max( numLights, 1 ) * sizeof( uint32 ), FRAME_ALLOC_UNKNOWN );
	llv->lists = wantLists;
	llv->showMode = r_showLightLists.GetInteger();
	llv->allMaterials = r_forwardMaterials.GetBool();							// read here, once: the model jobs read the view, not the cvars
	llv->twin = r_showLightDiff.GetInteger() != 0;
	llv->diff = r_showLightDiff.GetInteger() == 2;
	llv->log = r_lightListLog.GetBool();
	llv->lightScale = r_lightScale.GetFloat();									// the flatten below multiplies the colours by it
	llv->viewFrame = tr.frameCount;

	// viewLight_t -> the builder's flat input
	int n = 0;
	for( viewLight_t* vLight = viewDef->viewLights; vLight != NULL; vLight = vLight->next )
	{
		const uint32 flags = R_LightListRecordFlags( vLight, &llv->unknownLightModes );
		const idScreenRect& sr = vLight->scissorRect;
		if( flags == 0 || sr.IsEmpty() )
		{
			continue;															// no record, or no pixel: lights nothing in this view
		}
		lightListLight_t& L = llv->lights[n];
		// the bin range is linear view distance, zNear / zmax .. zNear / zmin ( reversed-Z: zmax is the nearest );
		// with r_useLightScissors 0 the scissor's depth range is the cleared one and the light takes every bin
#if ID_REVERSED_Z
		L.zMin = parms.zNear / Max( sr.zmax, 1e-7f );
		L.zMax = ( sr.zmin > 0.0f ) ? Min( parms.zNear / sr.zmin, 2.0f * parms.binFar ) : 2.0f * parms.binFar;
#else
		L.zMin = parms.zNear / Max( 1.0f - sr.zmin, 1e-7f );
		L.zMax = ( sr.zmax < 1.0f ) ? Min( parms.zNear / ( 1.0f - sr.zmax ), 2.0f * parms.binFar ) : 2.0f * parms.binFar;
#endif
		// the scissor is GL-style ( y up from the bottom ); tiles count rows from the top. One pixel of padding, so a
		// TAA jitter never moves a light's edge into a tile that lacks its bit
		L.x0 = ( int16 )idMath::ClampInt( 0, parms.width - 1, sr.x1 - 1 );
		L.x1 = ( int16 )idMath::ClampInt( 0, parms.width - 1, sr.x2 + 1 );
		L.y0 = ( int16 )idMath::ClampInt( 0, parms.height - 1, parms.height - 1 - sr.y2 - 1 );
		L.y1 = ( int16 )idMath::ClampInt( 0, parms.height - 1, parms.height - 1 - sr.y1 + 1 );
		L.importance = sr.GetArea();
		// shadowed: exactly the lights R_BuildLightLists gives shadow rows
		L.features = ( uint16 )( ( ( vLight->shadowLOD != -1 ) ? LLF_SHADOWED : 0 )
								 | ( ( flags & RLF_MATH ) ? LLF_MATH : 0 ) | ( ( flags & RLF_POINT ) ? LLF_POINT : LLF_PROJECTED ) );
		L.clustered = ( flags & RLF_CLUSTERED ) ? 1 : 0;
		L.primarySun = vLight->isPrimarySun ? 1 : 0;
		llv->lightOfInput[n] = vLight;
		llv->flagsOfInput[n] = flags;
		n++;
	}
	llv->numLights = n;

	lightListArena_t arena;
	arena.size = R_LightListArenaBytes( parms, n );
	arena.base = ( byte* )R_FrameAlloc( arena.size, FRAME_ALLOC_UNKNOWN );
	arena.used = 0;
	R_LightListSetup( parms, n, llv->tables, arena );
	R_LightListNumber( llv->lights, n, llv->tables, arena );
	for( int i = 0; i < n; i++ )
	{
		llv->lightOfInput[i]->recordNumber = llv->tables.recordOfLight[i];
	}

	// tile bits: one job per band of rows ( D79 ), each writes only its rows
	const lightListTables_t& t = llv->tables;
	llv->tileBits = ( uint32* )R_ClearedFrameAlloc( t.tilesX * t.tilesY * t.wordsPerTile * sizeof( uint32 ), FRAME_ALLOC_UNKNOWN );
	if( t.numRecords > 1 )														// record 0, the sun's, is never binned: a view with no other record sets no bit
	{
		const int bands = Min( t.tilesY, 16 );
		lightListRowJob_t* jobs = ( lightListRowJob_t* )R_FrameAlloc( bands * sizeof( lightListRowJob_t ), FRAME_ALLOC_UNKNOWN );
		for( int i = 0; i < bands; i++ )
		{
			jobs[i].tables = &llv->tables;
			jobs[i].row0 = t.tilesY * i / bands;
			jobs[i].row1 = t.tilesY * ( i + 1 ) / bands;
			jobs[i].tileBits = llv->tileBits;
			tr.frontEndJobList->AddJob( ( jobRun_t )R_LightListRowJob, &jobs[i] );
		}
		tr.frontEndJobList->Submit();
		tr.frontEndJobList->Wait();
	}

	// the C1 records, in record order; record 0 stays empty when the view has no primary sun
	recordLight_t* records = ( recordLight_t* )R_ClearedFrameAlloc( t.numRecords * sizeof( recordLight_t ), FRAME_ALLOC_UNKNOWN );
	llv->rowsOfRecord = ( byte* )R_ClearedFrameAlloc( t.numRecords, FRAME_ALLOC_UNKNOWN );
	for( int r = 0; r < t.numRecords; r++ )
	{
		records[r].shadowRow = -1;
		records[r].ids.z = -1.0f;
		const int input = t.lightOfRecord[r];
		if( input >= 0 )
		{
			R_LightListFlattenRecord( llv->lightOfInput[input], llv->flagsOfInput[input], t.importanceRank[r], llv->lightScale, records[r] );
		}
	}
	const bool fogEligible = viewDef->isMainView && ( viewDef->renderView.rdflags & ( RDF_IRRADIANCE | RDF_NOAMBIENT | RDF_NOFOG ) ) == 0;
	if( fogEligible )
	{
		R_LightListHistory( viewDef, llv, records );
	}
	viewDef->recordLights = records;
	viewDef->numRecordLights = t.numRecords;
	viewDef->lightListView = llv;
	R_LightListCapture( llv );
}

/*
====================
R_LightListTakePair

H3, in R_AddSingleModel's contacted-light loop, where the lit drawSurf_t of ( surface, light ) would be made, on a
model job. LL_PAIR_LISTED: the pair is listed on the base surface and the caller makes no lit surface ( it still
makes the shadow surface ). LL_PAIR_LEGACY: the caller's code runs as it always did. LL_PAIR_TWIN ( the difference
view ): listed, and the caller makes the lit surface too and marks it. baseDrawSurf is NULL when the surface was
not linked to the view ( a deform made other surfaces ). maxLights is the entity's contacted-light count, which
bounds the run. The job reads the view's tables, final since the first hook, and writes only its own surface.

The forward pass draws a surface once, with one bump, one diffuse and one specular image and no texture matrix or
vertex colour: the materials the engine keeps fast-path images for ( idMaterial::SetFastPathImages; the property,
not the cvar r_skipInteractionFastPath ). Every other material keeps the legacy pass, whatever its keyword says.
====================
*/
int R_LightListTakePair( const viewDef_t* viewDef, const viewLight_t* vLight, const idMaterial* shader, drawSurf_t* baseDrawSurf, int maxLights )
{
	const lightListView_t* llv = viewDef->lightListView;
	if( llv == NULL || !llv->lists || baseDrawSurf == NULL )
	{
		return LL_PAIR_LEGACY;
	}
	const int record = vLight->recordNumber;
	if( record < 0 || record >= llv->tables.firstFogOnly || ( record == 0 && !llv->tables.sunListed ) )
	{
		return LL_PAIR_LEGACY;													// no record, a fog-only record, or a sun that is not listed
	}
	if( !( llv->allMaterials || shader->TestMaterialFlag( MF_FORWARD ) ) || shader->GetFastPathBumpImage() == NULL || shader->Coverage() == MC_TRANSLUCENT )
	{
		return LL_PAIR_LEGACY;													// the forward pass does not draw it ( F4 lifts the coverage test )
	}
	lightListSurface_t* s = baseDrawSurf->lightList;
	if( s == NULL )
	{
		// the surface's list and its run in one block ( frame memory comes in whole cache lines, cleared ). The run
		// can hold every light of the entity that can be listed: no more than touch it, no more than have a
		// listable record
		const int maxRun = Min( maxLights, llv->tables.firstFogOnly );
		s = ( lightListSurface_t* )R_FrameAlloc( sizeof( lightListSurface_t ) + maxRun * sizeof( uint16 ), FRAME_ALLOC_DRAW_SURFACE );
		s->run = ( uint16* )( s + 1 );
		s->count = 0;
		baseDrawSurf->lightList = s;
	}
	s->run[ s->count++ ] = ( uint16 )record;
	return llv->twin ? LL_PAIR_TWIN : LL_PAIR_LISTED;
}

/*
====================
R_LightListFinishSurface

H3, after the contacted-light loop of one surface, on the model job: the run becomes an ascending set, and the path
is chosen here, because the tables are final and the samples cost a few hundred nanoseconds that the serial pack
should not pay. The shape is the surface's bounds through the model-view matrix: a chunk ( one bounds axis under a
unit ) as its rectangle, anything else as a box. Nothing about the chunk grid is stored or read.
====================
*/
void R_LightListFinishSurface( const viewDef_t* viewDef, drawSurf_t* baseDrawSurf )
{
	if( baseDrawSurf->lightList == NULL )
	{
		return;
	}
	lightListSurface_t& s = *baseDrawSurf->lightList;
	const lightListView_t* llv = viewDef->lightListView;
	uint32 mask[LL_MAX_WORDS];
	s.count = ( int16 )R_LightListFinishRun( llv->tables, s.run, s.count, mask );

	const idBounds& b = baseDrawSurf->frontEndGeo->bounds;
	const float* m = baseDrawSurf->space->modelViewMatrix;
	const idVec3 size = b[1] - b[0];
	lightListShape_t shape;
	shape.origin.x = m[0] * b[0].x + m[4] * b[0].y + m[8] * b[0].z + m[12];
	shape.origin.y = m[1] * b[0].x + m[5] * b[0].y + m[9] * b[0].z + m[13];
	shape.origin.z = -( m[2] * b[0].x + m[6] * b[0].y + m[10] * b[0].z + m[14] );		// view space: +z forward
	shape.numEdges = 0;
	bool thin = false;
	for( int a = 0; a < 3; a++ )
	{
		if( size[a] < 1.0f && !thin )
		{
			thin = true;														// the chunk's plane: the bounds say it, nothing was stored
			continue;
		}
		shape.edges[shape.numEdges].x = m[a * 4 + 0] * size[a];
		shape.edges[shape.numEdges].y = m[a * 4 + 1] * size[a];
		shape.edges[shape.numEdges].z = -m[a * 4 + 2] * size[a];
		shape.numEdges++;
	}

	int how = 0;
	s.path = ( byte )R_LightListChoosePath( llv->tables, llv->tileBits, shape, s.run, s.count, mask, baseDrawSurf->material->GetForwardTag(), &how );
	s.how = ( byte )how;
}

/*
====================
R_LightListLoud

A full table, an unknown tag, or more forward draws than the render parms' ring is sure to hold
( LL_FORWARD_DRAWS_LOUD ): one console warning of each kind per map, not per frame, and the counters every
frame in the log line. The session keeps one render world for every map, so the kinds a map has had are cleared
where the engine drops its view history ( R_NumberRecordLights ). With r_lightListStrict it stops the author's
machine instead, in any view that has one of them.
====================
*/
static void R_LightListLoud( const lightListView_t* llv )
{
	const lightListStats_t& st = llv->stats;
	const int now = ( st.capped > 0 ? 1 : 0 ) | ( st.zbinAllSurfs > 0 ? 2 : 0 ) | ( ( st.unknownTags + llv->unknownLightModes ) > 0 ? 4 : 0 )
					| ( ( st.listSurfs + st.zbinSurfs ) > LL_FORWARD_DRAWS_LOUD ? 8 : 0 );
	const int fresh = r_lightListStrict.GetBool() ? now : ( now & ~lightListWarned );
	if( fresh == 0 )
	{
		return;
	}
	lightListWarned |= fresh;
	idStr msg;
	if( fresh & 1 )
	{
		msg += va( "%d lights past the %d records of a view keep the legacy path. ", st.capped, LL_MAX_RECORDS );
	}
	if( fresh & 2 )
	{
		msg += va( "the pool is full ( %d words ): %d surfaces are lit through unmasked Z-bins and may gain light ( magenta in r_showLightLists 2 ). ", LL_MAX_POOL, st.zbinAllSurfs );
	}
	if( fresh & 4 )
	{
		msg += va( "%d surfaces and %d lights carry a forward tag the engine does not know, read as 0. ", st.unknownTags, llv->unknownLightModes );
	}
	if( fresh & 8 )
	{
		msg += va( "%d surfaces list lights: past %d forward draws a view the forward layouts' ring of render parms can run out on Vulkan ( at about twice that NVRHI's \"maxVersions\" error stops the engine ). ", st.listSurfs + st.zbinSurfs, LL_FORWARD_DRAWS_LOUD );
	}
	if( r_lightListStrict.GetBool() )
	{
		common->FatalError( "lightlists: %s", msg.c_str() );
	}
	common->Warning( "lightlists: %s", msg.c_str() );
}

/*
====================
R_LightListPrintRecords

"listLightRecords" arms it; the next view that builds records prints them, one line each, and disarms it.
====================
*/
static bool lightListPrintArmed = false;

static void R_LightListPrintRecords( const viewDef_t* viewDef, const lightListView_t* llv )
{
	lightListPrintArmed = false;
	const lightListTables_t& t = llv->tables;
	common->Printf( "lightlists: %d records ( narrow below %d, wide below %d, fog only from there ), %d shadow rows, %d vanished, previous view at frame %d\n",
					t.numRecords, t.firstWide, t.firstFogOnly, viewDef->numShadowRows, llv->numVanishedPrev, llv->prevFrame );
	for( int r = 0; r < t.numRecords; r++ )
	{
		const recordLight_t& rec = viewDef->recordLights[r];
		const lightListRect_t& rect = t.rect[r];
		common->Printf( "  %4d id %5d flags %03x rank %4d row %4d ( %d ) prev %4d tiles x %d-%d y %d-%d color %.3f %.3f %.3f %.1f origin %.1f %.1f %.1f\n",
						r, rec.uniqueId, rec.flags, ( int )rec.ids.x, rec.shadowRow, ( int )llv->rowsOfRecord[r], ( int )rec.ids.z,
						( int )rect.x0, ( int )rect.x1, ( int )rect.y0, ( int )rect.y1,
						rec.color.x, rec.color.y, rec.color.z, rec.color.w, rec.originInvRadiusSq.x, rec.originInvRadiusSq.y, rec.originInvRadiusSq.z );
	}
}

/*
====================
R_BuildLightLists

The second hook: after R_AddModels, before R_OptimizeViewLightsList. Serial, in draw order: the shadow row bases,
the pool, the listed counts ( so a listed light keeps its shadows, D9 ), listsBuilt, the warnings, the log line.
====================
*/
void R_BuildLightLists()
{
	viewDef_t* viewDef = tr.viewDef;
	lightListView_t* llv = viewDef->lightListView;
	if( llv == NULL )
	{
		return;
	}
	const lightListTables_t& t = llv->tables;

	// C2: a record's rows are consecutive, one per map the light can have: six faces, the cascades, or one. Every
	// light that can have a shadow map this frame gets its rows. Whether it has one is not known here: the shadow
	// cache's decide pass runs later in R_RenderView, gives a cached light a map although no caster is in this
	// view's chain, and may still take a light's maps away. The backend fills the rows after the atlas pass and
	// leaves those of a light without a map, or of a map without a place, unplaced
	int numRows = 0;
	for( int r = 0; r < t.numRecords; r++ )
	{
		const int input = t.lightOfRecord[r];
		if( input < 0 )
		{
			continue;
		}
		const viewLight_t* vLight = llv->lightOfInput[input];
		if( vLight->shadowLOD == -1 )
		{
			continue;															// the light casts no shadow
		}
		// parallel first: this engine's parallel lights are point lights too ( any light without a target is )
		const int rows = vLight->parallel ? idMath::ClampInt( 1, 6, r_shadowMapSplits.GetInteger() + 1 ) : ( vLight->pointLight ? 6 : 1 );
		viewDef->recordLights[r].shadowRow = numRows;
		llv->rowsOfRecord[r] = ( byte )rows;
		numRows += rows;
	}
	viewDef->numShadowRows = numRows;

	// the view surfaces the model jobs listed on, in draw order
	lightListSurface_t** surfs = ( lightListSurface_t** )R_FrameAlloc( Max( viewDef->numDrawSurfs, 1 ) * sizeof( lightListSurface_t* ), FRAME_ALLOC_UNKNOWN );
	int numSurfs = 0;
	int poolWords = LL_POOL_FIRST;
	for( int i = 0; i < viewDef->numDrawSurfs; i++ )
	{
		lightListSurface_t* s = viewDef->drawSurfs[i]->lightList;
		if( s != NULL && s->run != NULL )
		{
			surfs[numSurfs++] = s;
			poolWords += Max( ( int )s->count, t.wordsPerTile );				// whichever of the run and the mask it ends up with
		}
	}
	// the pool is sized by what this view can use, not at its cap: R_FrameAlloc clears what it hands out
	llv->pool = ( uint32* )R_FrameAlloc( Min( poolWords, LL_MAX_POOL ) * sizeof( uint32 ), FRAME_ALLOC_UNKNOWN );
	// per record, the surfaces that list it
	int* listed = ( int* )R_ClearedFrameAlloc( LL_MAX_RECORDS * sizeof( int ), FRAME_ALLOC_UNKNOWN );
	memset( &llv->stats, 0, sizeof( llv->stats ) );
	R_LightListPack( t, surfs, numSurfs, llv->pool, listed, llv->stats );
	for( int i = 0; i < llv->numLights; i++ )
	{
		viewLight_t* vLight = llv->lightOfInput[i];
		if( vLight->recordNumber >= 0 && listed[ vLight->recordNumber ] > 0 )
		{
			vLight->numListedSurfs = listed[ vLight->recordNumber ];			// D9: R_ViewLightIsLit counts it, so the light keeps its shadow maps
			tr.pc.c_listedLights++;
		}
	}
	viewDef->listsBuilt = llv->lists;
	R_LightListLoud( llv );
	if( r_lightListLog.GetBool() )
	{
		const lightListStats_t& st = llv->stats;
		llv->stats.hash = R_LightListHash( t, llv->tileBits, llv->pool, st.poolUsed, surfs, numSurfs );
		common->Printf( "lightlists: frame %d records %d ( narrow %d wide %d fog %d ) capped %d | surfaces list %d zbin %d zbinAll %d | by forced %d tag %d short %d "
						"sampled %d unsampled %d unknownTags %d | pairs %d pool %d overflow %d | rows %d vanished %d | hash %08x\n",
						tr.frameCount, st.records, st.narrow, st.wide, st.fogOnly, st.capped, st.listSurfs, st.zbinSurfs, st.zbinAllSurfs,
						st.how[LLH_FORCED], st.how[LLH_TAG], st.how[LLH_SHORT], st.how[LLH_SAMPLED], st.how[LLH_UNSAMPLED], st.unknownTags + llv->unknownLightModes,
						st.listedPairs, st.poolUsed, st.poolOverflow, numRows, llv->numVanishedPrev, st.hash );
	}
	if( lightListPrintArmed )
	{
		R_LightListPrintRecords( viewDef, llv );
	}
}

/*
====================
R_LightListDrawParms
R_LightListViewParms

The backend's lanes ( C18 ). Per draw, rpSurfLights: the first pool word, the run length, the path and the feature
mask. Per view: the tile size, the tiles per row, the words per tile and the bin count; the bins per unit, near,
far, and r_showLightLists in the spare lane.

The integers travel as FLOAT VALUES, which the shader converts back with uint( x ): every one of them is far below
2^24, so the conversion is exact. They cannot travel as bits read back with asuint, as C18 has it: the bits of a
small integer are a denormal float, the backend's thread runs with denormals read as zero ( found by running it ),
and idRenderProgManager::SetUniformValue stores a lane only when it compares unequal to what it holds, so a lane of
such bits was never stored.
====================
*/
void R_LightListDrawParms( const lightListSurface_t& s, float out[4] )
{
	out[0] = ( float )s.first;
	out[1] = ( float )s.count;
	out[2] = ( float )s.path;
	out[3] = ( float )s.featureMask;
}

void R_LightListViewParms( const lightListView_t* llv, float zbinParms[4], float zbinScale[4] )
{
	const lightListTables_t& t = llv->tables;
	zbinParms[0] = ( float )t.parms.tileSize;
	zbinParms[1] = ( float )t.tilesX;
	zbinParms[2] = ( float )t.wordsPerTile;
	zbinParms[3] = ( float )t.parms.numBins;
	zbinScale[0] = t.binScale;
	zbinScale[1] = t.parms.zNear;
	zbinScale[2] = t.parms.binFar;
	zbinScale[3] = ( float )llv->showMode;
}

/*
====================
R_LightListBench_f

"lightListBench [ n ]": rebuilds the captured view's tables n times ( default 100 ) and prints the time per stage
and the hash of the tables. The benchmark ships in the binary.
====================
*/
static idList<lightListLight_t, TAG_RENDER>	lightListCaptureLights;
static lightListParms_t						lightListCaptureParms;

static void R_LightListBench_f( const idCmdArgs& args )
{
	if( lightListCaptureLights.Num() == 0 )
	{
		common->Printf( "lightListBench: nothing captured; set r_lightListCapture 1 first\n" );
		return;
	}
	const int runs = ( args.Argc() > 1 ) ? Max( atoi( args.Argv( 1 ) ), 1 ) : 100;
	const int n = lightListCaptureLights.Num();
	idTempArray<byte> block( R_LightListArenaBytes( lightListCaptureParms, n ) );
	uint64 numberUs = 0, tilesUs = 0;
	uint32 hash = 0;
	for( int k = 0; k < runs; k++ )
	{
		lightListArena_t arena;
		arena.base = block.Ptr();
		arena.size = ( int )block.Size();
		arena.used = 0;
		lightListTables_t t;
		const uint64 t0 = Sys_Microseconds();
		R_LightListSetup( lightListCaptureParms, n, t, arena );
		R_LightListNumber( lightListCaptureLights.Ptr(), n, t, arena );
		const uint64 t1 = Sys_Microseconds();
		idTempArray<uint32> bits( t.tilesX * t.tilesY * t.wordsPerTile );
		bits.Zero();
		R_LightListTileRows( t, 0, t.tilesY, bits.Ptr() );
		const uint64 t2 = Sys_Microseconds();
		numberUs += t1 - t0;
		tilesUs += t2 - t1;
		hash = R_LightListHash( t, bits.Ptr(), NULL, 0, NULL, 0 );
	}
	common->Printf( "lightListBench: %d lights, %d runs: number + bins %.1f us, tile bits %.1f us ( serial ), tables hash %08x\n",
					n, runs, ( double )numberUs / runs, ( double )tilesUs / runs, hash );
}

/*
====================
R_LightListCapture

Called at the end of R_NumberRecordLights while r_lightListCapture is set: keeps the builder's input past the frame.
====================
*/
static void R_LightListCapture( const lightListView_t* llv )
{
	if( !r_lightListCapture.GetBool() || llv == NULL )
	{
		return;
	}
	lightListCaptureParms = llv->tables.parms;
	lightListCaptureLights.SetNum( llv->numLights );
	for( int i = 0; i < llv->numLights; i++ )
	{
		lightListCaptureLights[i] = llv->lights[i];
	}
	r_lightListCapture.SetBool( false );
	common->Printf( "lightlists: captured %d lights\n", llv->numLights );
}

/*
====================
R_LightListDump_f

"lightListDump <file>": the captured input as a versioned binary ( "LLD2", parms, count, lights ): the flat structs
as they are, so an offline harness replays the view with no serializer.
====================
*/
static void R_LightListDump_f( const idCmdArgs& args )
{
	if( args.Argc() < 2 || lightListCaptureLights.Num() == 0 )
	{
		common->Printf( "usage: lightListDump <file> ( after r_lightListCapture 1 )\n" );
		return;
	}
	idFile* f = fileSystem->OpenFileWrite( args.Argv( 1 ) );
	if( f == NULL )
	{
		common->Printf( "lightListDump: cannot write %s\n", args.Argv( 1 ) );
		return;
	}
	const int n = lightListCaptureLights.Num();
	f->Write( "LLD2", 4 );
	f->Write( &lightListCaptureParms, sizeof( lightListCaptureParms ) );
	f->Write( &n, sizeof( n ) );
	f->Write( lightListCaptureLights.Ptr(), n * sizeof( lightListLight_t ) );
	fileSystem->CloseFile( f );
}

static void R_ListLightRecords_f( const idCmdArgs& /* args */ )
{
	lightListPrintArmed = true;
}

/*
====================
R_ListLightTags_f

"listLightTags": every forward tag in use in the loaded level, materials and lights, so a designer sees what the
numbers mean before typing a new one. One line per tag user.
====================
*/
static void R_ListLightTags_f( const idCmdArgs& /* args */ )
{
	static const char* materialTags[LLT_NUM] = { "the rule decides", "always the list", "always the Z-bins" };
	int count = 0;
	for( int i = 0; i < declManager->GetNumDecls( DECL_MATERIAL ); i++ )
	{
		const idMaterial* m = static_cast<const idMaterial*>( declManager->DeclByIndex( DECL_MATERIAL, i, false ) );
		if( m == NULL || !m->TestMaterialFlag( MF_FORWARD ) || m->GetForwardTag() == LLT_AUTO )
		{
			continue;
		}
		const int tag = m->GetForwardTag();
		common->Printf( "material %-48s forward %d ( %s )\n", m->GetName(), tag, ( tag < LLT_NUM ) ? materialTags[tag] : "UNKNOWN, read as 0" );
		count++;
	}
	if( tr.primaryWorld != NULL )
	{
		for( int i = 0; i < tr.primaryWorld->lightDefs.Num(); i++ )
		{
			const idRenderLightLocal* def = tr.primaryWorld->lightDefs[i];
			if( def == NULL || def->parms.forwardMode == LIGHT_FORWARD_AUTO )
			{
				continue;
			}
			const int mode = def->parms.forwardMode;
			common->Printf( "light %5d %-40s forward %d ( %s )\n", i, def->parms.shader ? def->parms.shader->GetName() : "<no shader>", mode,
							mode == LIGHT_FORWARD_LIST ? "always listed" : ( mode == LIGHT_FORWARD_LEGACY ? "never listed" : "UNKNOWN, read as 0" ) );
			count++;
		}
	}
	common->Printf( "%d forward tags in use\n", count );
}

void R_LightListInitCommands()
{
	cmdSystem->AddCommand( "lightListBench", R_LightListBench_f, CMD_FL_RENDERER, "times the light-list builder on the captured view" );
	cmdSystem->AddCommand( "lightListDump", R_LightListDump_f, CMD_FL_RENDERER, "writes the captured light-list input to a file" );
	cmdSystem->AddCommand( "listLightRecords", R_ListLightRecords_f, CMD_FL_RENDERER, "prints the light records of the next main view that builds them" );
	cmdSystem->AddCommand( "listLightTags", R_ListLightTags_f, CMD_FL_RENDERER, "lists the forward tags of materials and lights in the loaded level" );
}
