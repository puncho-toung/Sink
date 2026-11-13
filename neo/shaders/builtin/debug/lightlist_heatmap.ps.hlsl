// lightlist_heatmap.ps.hlsl - pixel stage of the light lists' heatmap ( r_showLightLists 1 ). For each pixel: its
// view distance from the scene depth, the Z-bin that distance falls in, the pixel's tile, and the number of
// clustered light records whose bit is set in the tile and whose index lies in one of the bin's two clustered
// ranges: the lights a forward pixel would loop there. Blended over the image. It reads exactly the tables the
// frontend built ( bins: narrow, wide, fog-only range as lo | hi << 16; tile bits: bit r is record r ), so a
// wrong tile row order, a wrong depth sense or a table that was not uploaded shows at once.

#include <lightlist_heatmap.cb.h>

// *INDENT-OFF*
#ifdef SPIRV
[[vk::push_constant]] ConstantBuffer<LightListHeatmapConstants> g_Heat;
#else
cbuffer g_Heat : register( b0 )
{
	LightListHeatmapConstants g_Heat;
}
#endif

struct ZBin
{
	uint	narrow;
	uint	wide;
	uint	fogOnly;
	uint	pad;
};

Texture2D<float>		t_Depth		: register( t0 );
StructuredBuffer<ZBin>	t_ZBins		: register( t1 );
StructuredBuffer<uint>	t_TileBits	: register( t2 );

struct PS_IN
{
	float4 position	: SV_Position;
};
// *INDENT-ON*

// the records of one range that have their bit in the tile: R_LightListWordMask, word by word
uint CountRange( uint range, uint tileBase )
{
	const uint lo = range & 0xFFFFu;
	const uint hi = range >> 16;
	uint count = 0;
	if( lo <= hi )
	{
		for( uint w = lo / 32; w <= hi / 32; ++w )
		{
			const uint first = max( lo, w * 32 ) - w * 32;
			const uint last = min( hi, w * 32 + 31 ) - w * 32;
			const uint mask = ( 0xFFFFFFFFu >> ( 31 - ( last - first ) ) ) << first;
			count += countbits( t_TileBits[ tileBase + w ] & mask );
		}
	}
	return count;
}

float4 main( PS_IN fragment ) : SV_Target
{
	const int2 pixel = int2( fragment.position.xy );
	const uint2 inView = uint2( clamp( pixel - g_Heat.viewOrigin, int2( 0, 0 ), g_Heat.viewSize - int2( 1, 1 ) ) );

	// linear view distance, as the frontend binned the lights: zNear / depth under reversed-Z
	const float depth = t_Depth.Load( int3( pixel, 0 ) );
#if ID_REVERSED_Z
	const float viewZ = g_Heat.zNear / max( depth, 1e-7 );
#else
	const float viewZ = g_Heat.zNear / max( 1.0 - depth, 1e-7 );
#endif
	const uint bin = uint( clamp( viewZ * g_Heat.binsPerUnit, 0.0, float( g_Heat.numBins ) - 1.0 ) );
	const ZBin ranges = t_ZBins[ bin ];

	const uint2 tile = inView / g_Heat.tileSize;
	const uint tileBase = ( tile.y * g_Heat.tilesX + tile.x ) * g_Heat.wordsPerTile;
	const uint count = CountRange( ranges.narrow, tileBase ) + CountRange( ranges.wide, tileBase );

	// 0 lights: nothing; then blue, cyan, green, yellow, red at 1, 4, 8, 16, 32 and white beyond
	float3 heat = float3( 0.0, 0.0, 0.0 );
	float alpha = 0.0;
	if( count > 0 )
	{
		const float t = float( count );
		heat = ( t <= 4.0 ) ? lerp( float3( 0.0, 0.0, 1.0 ), float3( 0.0, 1.0, 1.0 ), ( t - 1.0 ) / 3.0 )
			   : ( t <= 8.0 ) ? lerp( float3( 0.0, 1.0, 1.0 ), float3( 0.0, 1.0, 0.0 ), ( t - 4.0 ) / 4.0 )
			   : ( t <= 16.0 ) ? lerp( float3( 0.0, 1.0, 0.0 ), float3( 1.0, 1.0, 0.0 ), ( t - 8.0 ) / 8.0 )
			   : ( t <= 32.0 ) ? lerp( float3( 1.0, 1.0, 0.0 ), float3( 1.0, 0.0, 0.0 ), ( t - 16.0 ) / 16.0 )
			   : float3( 1.0, 1.0, 1.0 );
		alpha = g_Heat.opacity;
	}
	// tile borders, faint, so the grid can be read against the scene
	if( ( inView.x % g_Heat.tileSize ) == 0 || ( inView.y % g_Heat.tileSize ) == 0 )
	{
		alpha *= 0.5;
	}
	return float4( heat, alpha );
}
