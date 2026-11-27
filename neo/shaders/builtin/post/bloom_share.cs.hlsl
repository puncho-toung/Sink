// bloom_share.cs.hlsl - WP-J27, the sources' own blooms under the Fourier bloom: what share of a texel of the transform's
// source level ( level 1, or level 2 at Quality 0 ) is the light of each style slot. The gate wrote each 4 x 4 block's light
// by slot ( the light that passes the threshold, boosted: bloom_gate.cs.hlsl ); a level texel's light comes from its own
// blocks and from those the chain's filters spread into it, f blocks a side for level 1 and three for level 2 ( the
// first pass's and the down passes' thirteen taps ), so a texel's shares are its blocks' sums over that footprint,
// divided by their total. A texel with no light there is the view's slot's alone. bloom_fft.cs.hlsl's PASS 1 multiplies
// each source texel by its class's share before the transform, one transform a class; the classes' products are summed
// before the one transform back.

#include "vulkan.hlsli"

// *INDENT-OFF*
struct BloomShareConstants
{
	int4	size;		// x, y the level's rectangle ( texels ); z f, the blocks a texel spans an axis ( 1 or 2 ); w r, the spread in blocks
	int4	blocks;		// x, y the blocks' rectangle; zw 0
};
VK_PUSH_CONSTANT ConstantBuffer<BloomShareConstants> g_Share : register( b0 );

Texture2D<float4> t_Sums : register( t0 );
VK_IMAGE_FORMAT( "rgba16f" ) RWTexture2D<float4> u_Shares : register( u0 );
// *INDENT-ON*

[numthreads( 8, 8, 1 )]
void main( uint3 id : SV_DispatchThreadID )
{
	if( any( int2( id.xy ) >= g_Share.size.xy ) )
	{
		return;
	}
	const int f = g_Share.size.z;
	const int r = g_Share.size.w;
	const int2 lo = max( int2( id.xy ) * f - r, int2( 0, 0 ) );
	const int2 hi = min( int2( id.xy ) * f + f - 1 + r, g_Share.blocks.xy - int2( 1, 1 ) );
	float4 sum = float4( 0.0, 0.0, 0.0, 0.0 );
	[loop]
	for( int y = lo.y; y <= hi.y; y++ )
	{
		[loop]
		for( int x = lo.x; x <= hi.x; x++ )
		{
			sum += t_Sums.Load( int3( x, y, 0 ) );
		}
	}
	const float total = sum.x + sum.y + sum.z + sum.w;
	u_Shares[ id.xy ] = ( total > 0.0 ) ? sum / total : float4( 1.0, 0.0, 0.0, 0.0 );
}
