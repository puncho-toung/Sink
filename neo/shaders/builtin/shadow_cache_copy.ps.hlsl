// shadow_cache_copy.ps.hlsl - pixel stage of the shadow cache's copy draw ( track S, D34 ). A rectangle of a
// depth target ( viewport and scissor = the rectangle ) is drawn with depth test ALWAYS and depth write on;
// each pixel loads one texel of the cached map and returns it as its depth. On D3D12 a copy command cannot
// write one tile of a depth atlas; this draw can, on both APIs. The same draw unwraps a toroidal map ( S4 ),
// reduces a map into a smaller tile by point sampling ( D43 ), and with res 0 clears the rectangle to 1.0.

#include <shadow_cache_copy.cb.h>

// *INDENT-OFF*
#ifdef SPIRV

[[vk::push_constant]] ConstantBuffer<ShadowCacheCopyConstants> g_Copy;

#else

cbuffer g_Copy : register( b0 )
{
	ShadowCacheCopyConstants g_Copy;
}

#endif

Texture2D<float> t_CacheDepth : register( t0 );

struct PS_IN
{
	float4 position	: SV_Position;
	float2 uv		: TEXCOORD0;
};
// *INDENT-ON*

float main( PS_IN fragment ) : SV_Depth
{
	if( g_Copy.res == 0 )
	{
		return 1.0;
	}

	// the pixel inside the rectangle, then the texel of the cached map it shows
	const int2 inTile = int2( fragment.position.xy ) - g_Copy.tileOrigin;
	// the texel under the pixel's centre. A tile of the map's own size ( every tile, until a reduced copy is
	// used: D43 ) needs no arithmetic; a reduced one takes the middle of each block
	int2 logical = inTile;
	if( g_Copy.res != g_Copy.tileRes )
	{
		logical = ( ( 2 * inTile + 1 ) * g_Copy.res ) / ( 2 * g_Copy.tileRes );
	}
	const int2 physical = g_Copy.sourceOrigin + int2( ( logical.x + g_Copy.wrap.x ) & ( g_Copy.res - 1 ), ( logical.y + g_Copy.wrap.y ) & ( g_Copy.res - 1 ) );
	return t_CacheDepth.Load( int3( physical, 0 ) );
}
