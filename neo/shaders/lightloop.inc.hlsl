// lightloop.inc.hlsl - the forward pass's two light loops ( track F; the F0 merge's lightloop.hlsli, whose text
// from the first constant below to the end of the file is unchanged, so the merge's emulated-wave tests of that
// file are tests of this one: tools/asm/check_loop.py of the run compares the two ).
//
// It is the shader half of the light-list core in renderer/tr_frontend_lightlists.cpp and must agree with it bit
// for bit: LL_BinIndex is R_LightListBinIndex, LL_WordMask is R_LightListWordMask, the bin's .x and .y are the
// narrow and wide clustered ranges, and the pool is laid out as R_LightListPack writes it:
//
//   t_LightPool[ 0 ]            0: the record index of the sun, so the list loop can evaluate it on the Z-bin path
//   t_LightPool[ 1 .. 32 ]      the shared all-ones mask ( LLP_ZBIN_ALL: the pool was full )
//   a list-path surface         count words, one ascending record index each, from rpSurfLights.x
//   a Z-bin surface             words-per-tile words from rpSurfLights.x: bit r set when the model job listed record r
//
// The includer provides:
//   LL_SURFACE                                  the per-pixel surface data, everything hoisted out of the loop
//   float3 LL_EvaluateRecord( uint r, LL_SURFACE s )   one record at this pixel; EXACTLY zero where the light does not
//                                               reach ( the two paths then add the same terms in the same order )
//   StructuredBuffer<uint>  t_LightPool         t18
//   StructuredBuffer<uint4> t_LightBins         t19: per bin narrow, wide, fog-only range ( lo | hi << 16 ), 0
//   StructuredBuffer<uint>  t_LightTileBits     t20: tilesX * tilesY * wordsPerTile words, tile-major; bit i = record i
//   ll_tileSize, ll_tilesX, ll_wordsPerTile, ll_numBins ( uint ), ll_binScale ( float ): rpZBinParms, rpZBinScale.x
//   USE_WAVE_OPS                                1: the scalarized fetch ( SM 6.0 ); 0: each lane its own range
//
// Both loops shade exactly the ( light, surface ) pairs the model job listed, in ascending record order: the run is an
// ascending set, the Z-bin words and bits ascend, the sun ( record 0 ) comes first on both paths and the narrow class
// precedes the wide one. So the loops test no flag, no channel mask and no exclusion per candidate, and a surface whose
// path flips shades the same bits. The path is a per-draw constant: every branch on it is uniform. The includer samples
// with SampleLevel / SampleCmpLevelZero only ( the Z-bin loop's trip count is not uniform, C5 ).

static const uint LLP_LIST = 0;									// lightListPath_t; anything else is the Z-bin loop
static const uint LL_POOL_SUN = 0;								// the pool word that always holds 0, the sun's record index

uint LL_BinIndex( float viewZ )									// R_LightListBinIndex, clamps included
{
	return ( uint )clamp( viewZ * ll_binScale, 0.0f, float( ll_numBins ) - 1.0f );
}

// the bits of word w whose record indices lie in [ lo, hi ]; a full word is a right shift by 31 - ( last - first ),
// never ( 1u << 32 ) - 1, which HLSL reads as 0 ( 03 )
uint LL_WordMask( uint w, uint lo, uint hi )
{
	const int first = max( int( lo ) - int( w * 32 ), 0 );
	const int last = min( int( hi ) - int( w * 32 ), 31 );
	if( first > last )
	{
		return 0;
	}
	return ( 0xFFFFFFFFu >> uint( 31 - ( last - first ) ) ) << uint( first );
}

// The list loop: the run is the same for every pixel of the draw, so the counter, the pool entry and the record are
// wave-uniform ( scalar loads ). Per light: one pool fetch, then the record.
float3 LL_ListLoop( uint first, uint count, LL_SURFACE s )
{
	float3 lit = float3( 0.0f, 0.0f, 0.0f );
	for( uint i = 0; i < count; ++i )
	{
		lit += LL_EvaluateRecord( t_LightPool[ first + i ], s );
	}
	return lit;
}

// One clustered range of the Z-bin loop, under the surface's mask. With wave ops the word range and each word's bits
// are the wave's union, so every record index the wave visits is uniform; each lane's own bits decide whether it
// evaluates ( the correctness: the wave ops are only the speed ). A word the surface lists nothing in is skipped on a
// uniform test before its tile word is read.
float3 LL_ZBinRange( uint maskFirst, uint tileBase, uint bin, LL_SURFACE s, float3 lit )
{
	const uint lo = bin & 0xFFFFu;
	const uint hi = bin >> 16;
#if USE_WAVE_OPS
	const uint waveLo = WaveReadLaneFirst( WaveActiveMin( lo ) );
	const uint waveHi = WaveReadLaneFirst( WaveActiveMax( hi ) );
#else
	const uint waveLo = lo;
	const uint waveHi = hi;
#endif
	if( waveLo > waveHi )
	{
		return lit;												// every lane's range is empty ( uniform with wave ops )
	}
	for( uint w = waveLo / 32; w <= waveHi / 32; ++w )
	{
		const uint surfMask = t_LightPool[ maskFirst + w ];		// a uniform index: one scalar load per word
		if( surfMask != 0 )
		{
			const uint mine = t_LightTileBits[ tileBase + w ] & LL_WordMask( w, lo, hi ) & surfMask;
#if USE_WAVE_OPS
			uint wave = WaveReadLaneFirst( WaveActiveBitOr( mine ) );
#else
			uint wave = mine;
#endif
			while( wave != 0 )
			{
				const uint bit = firstbitlow( wave );
				wave &= wave - 1;
				if( ( mine & ( 1u << bit ) ) != 0 )
				{
					lit += LL_EvaluateRecord( w * 32 + bit, s );
				}
			}
		}
	}
	return lit;
}

// The Z-bin loop: one bin read, then the narrow range and the wide range ( a wide light never stretches the narrow
// range, so each range spans few words ). The fog-only range ( .z ) is the volumetrics' and is never read here.
float3 LL_ZBinLoop( uint maskFirst, uint2 pixel, float viewZ, LL_SURFACE s, float3 lit )
{
	const uint2 tile = pixel / ll_tileSize;
	const uint tileBase = ( tile.y * ll_tilesX + tile.x ) * ll_wordsPerTile;
	const uint4 bin = t_LightBins[ LL_BinIndex( viewZ ) ];
	for( uint range = 0; range < 2; ++range )					// a loop, not two calls: one inlined copy of the light
	{
		lit = LL_ZBinRange( maskFirst, tileBase, ( range == 0 ) ? bin.x : bin.y, s, lit );
	}
	return lit;
}

// rpSurfLights: x = first, y = count, z = path ( asuint ). The list loop runs on both paths, so the light is inlined
// twice, not three times: on the list path it walks the run; on the Z-bin path it walks nothing, or the one pool word
// that holds 0 when bit 0 of the surface's mask says the sun is listed. Then the Z-bin loop adds its candidates.
float3 LL_AccumulateLights( uint path, uint first, uint count, uint2 pixel, float viewZ, LL_SURFACE s )
{
	uint listFirst = first;
	uint listCount = count;
	if( path != LLP_LIST )
	{
		listFirst = LL_POOL_SUN;
		listCount = t_LightPool[ first ] & 1u;
	}
	float3 lit = LL_ListLoop( listFirst, listCount, s );
	if( path != LLP_LIST )
	{
		lit = LL_ZBinLoop( first, pixel, viewZ, s, lit );
	}
	return lit;
}

// r_showLightLists 1: the lights this pixel loops ( its own, not the wave's union ). Counted again here from the same
// tables instead of in the loops, so the view costs the loops nothing while it is off.
uint LL_CountLights( uint path, uint first, uint count, uint2 pixel, float viewZ )
{
	if( path == LLP_LIST )
	{
		return count;
	}
	const uint2 tile = pixel / ll_tileSize;
	const uint tileBase = ( tile.y * ll_tilesX + tile.x ) * ll_wordsPerTile;
	const uint4 bin = t_LightBins[ LL_BinIndex( viewZ ) ];
	uint num = t_LightPool[ first ] & 1u;
	for( uint range = 0; range < 2; ++range )
	{
		const uint bits = ( range == 0 ) ? bin.x : bin.y;
		const uint lo = bits & 0xFFFFu;
		const uint hi = bits >> 16;
		for( uint w = lo / 32; w <= hi / 32 && lo <= hi; ++w )
		{
			num += countbits( t_LightTileBits[ tileBase + w ] & LL_WordMask( w, lo, hi ) & t_LightPool[ first + w ] );
		}
	}
	return num;
}
