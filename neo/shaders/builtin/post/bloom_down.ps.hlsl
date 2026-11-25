// bloom_down.ps.hlsl - halves the bloom's source into the next level: thirteen bilinear taps of the source's
// rectangle, combined as five boxes ( an inner box of the four ( +-1, +-1 ) taps at weight 0.5 and four outer
// boxes of four taps each at 0.125, the centre tap shared by all four outer boxes ). Its marginal along either
// axis is [ 1 3 4 4 3 1 ] / 16 over six source texels ( the 2D kernel is not that row's outer product, but
// like it is zero at every frequency a halving folds back ). The first pass of the chain ( FIRST 1, the frame
// into level 0 ) does two things more: it scrubs every tap to the ceiling ( at most BLOOM_SCRUB_BOUND ),
// killing NaNs and infinities before they can spread through a filtered tap, and it weights every tap by the
// firefly knee, so a lone very bright pixel adds only about the knee to the bloom. Later passes ( FIRST 0 )
// read already scrubbed levels and just filter.

#include "vulkan.hlsli"
#include "bloom_cb.h"
#include "bloom.inc.hlsl"

// *INDENT-OFF*
Texture2D t_Source : register( t0 );
#if FIRST
Buffer<uint> t_Exposure : register( t1 );
#endif
SamplerState s_Linear : register( s0 );

VK_PUSH_CONSTANT ConstantBuffer<BloomConstants> g_Bloom : register( b0 );
// *INDENT-ON*

#if FIRST
// The scrub, per channel, on the value's bits rather than a float compare: a compare against 0 rejects a
// NaN only as long as no compiler or driver rewrites it under a fast-math rule, and a bit test cannot be
// rewritten. A positive finite value and +0 have bits up to 0x7F7FFFFF and take the min; +Inf is
// 0x7F800000 and becomes the ceiling; every NaN, and every negative value, -0 and -Inf with their sign
// bit, is above and becomes 0. The ceiling is what the C++ sends in exposure.z ( the cvar r_bloomCeiling,
// held to 1 .. BLOOM_SCRUB_BOUND there ): a value a few times the brightest emitter, which no light of
// the engine reaches and which holds a firefly of any size, where the knee below bounds only
// a lone bright pixel
float BloomScrub( float v )
{
	return ( asuint( v ) <= 0x7F800000u ) ? min( v, g_Bloom.exposure.z ) : 0.0;
}

float3 BloomScrub3( float3 v )
{
	return float3( BloomScrub( v.r ), BloomScrub( v.g ), BloomScrub( v.b ) );
}

// The firefly knee's weight for a tap: k / ( k + L * E ), written so nothing divides by k. A tap of exposed
// luminance L survives at most about knee/(knee+L*E) of itself, so a lone very bright pixel adds at most about
// a third of the knee. k <= 0 turns the knee off: every weight is then 1, the plain filter on scrubbed taps
float BloomKneeWeight( float3 c, float k, float E )
{
	const float L = dot( c, float3( 0.2126, 0.7152, 0.0722 ) );
	return ( k > 0.0 ) ? k / ( k + L * E ) : 1.0;
}

// A box's weighted mean of its four taps. The floor on the weight sum keeps a box whose weights all came out 0
// at 0 instead of a NaN
float3 BloomBox( float3 c0, float w0, float3 c1, float w1, float3 c2, float w2, float3 c3, float w3 )
{
	return ( c0 * w0 + c1 * w1 + c2 * w2 + c3 * w3 ) / max( w0 + w1 + w2 + w3, 1e-30 );
}
#endif

void main(
	in float4 pos : SV_Position,
	in float2 uv : UV,
	out float4 o_rgba : SV_Target )
{
	// the tap centre: level pixel p reads the source at 2 * p, in source texels relative to its rectangle
	const float2 c = 2.0 * pos.xy;
	const float2 origin = g_Bloom.srcRect.xy;
	const float2 size = g_Bloom.srcRect.zw;
	const float2 invSize = g_Bloom.srcInvSize.xy;

#if FIRST
	// The exposure factor of the knee, once per pixel: the same factor ConvertToLDR applies to a luminance.
	// The tonemap shader's own rule for a non-positive adapted luminance, written so that a NaN fails the test
	// too and takes the fallback, and a floor under it: r_hdrMinLuminance has no range, and an E of Inf times
	// a tap of luminance 0 would be a NaN
	float adapted = asfloat( t_Exposure[0] );
	if( !( adapted > 0.0 ) )
	{
		adapted = g_Bloom.exposure.y;
	}
	const float E = g_Bloom.exposure.x / max( adapted, 0.0001 );
	const float knee = g_Bloom.srcInvSize.w;

	// The thirteen taps, each fetched once, scrubbed right after its fetch, and weighted by the knee
	float3 ctr = BloomFetch( t_Source, s_Linear, c, origin, size, invSize );
	ctr = BloomScrub3( ctr );
	const float wctr = BloomKneeWeight( ctr, knee, E );
	float3 i00 = BloomFetch( t_Source, s_Linear, c + float2( -1.0, -1.0 ), origin, size, invSize );
	i00 = BloomScrub3( i00 );
	const float wi00 = BloomKneeWeight( i00, knee, E );
	float3 i10 = BloomFetch( t_Source, s_Linear, c + float2( 1.0, -1.0 ), origin, size, invSize );
	i10 = BloomScrub3( i10 );
	const float wi10 = BloomKneeWeight( i10, knee, E );
	float3 i01 = BloomFetch( t_Source, s_Linear, c + float2( -1.0, 1.0 ), origin, size, invSize );
	i01 = BloomScrub3( i01 );
	const float wi01 = BloomKneeWeight( i01, knee, E );
	float3 i11 = BloomFetch( t_Source, s_Linear, c + float2( 1.0, 1.0 ), origin, size, invSize );
	i11 = BloomScrub3( i11 );
	const float wi11 = BloomKneeWeight( i11, knee, E );
	float3 xm2 = BloomFetch( t_Source, s_Linear, c + float2( -2.0, 0.0 ), origin, size, invSize );
	xm2 = BloomScrub3( xm2 );
	const float wxm2 = BloomKneeWeight( xm2, knee, E );
	float3 xp2 = BloomFetch( t_Source, s_Linear, c + float2( 2.0, 0.0 ), origin, size, invSize );
	xp2 = BloomScrub3( xp2 );
	const float wxp2 = BloomKneeWeight( xp2, knee, E );
	float3 ym2 = BloomFetch( t_Source, s_Linear, c + float2( 0.0, -2.0 ), origin, size, invSize );
	ym2 = BloomScrub3( ym2 );
	const float wym2 = BloomKneeWeight( ym2, knee, E );
	float3 yp2 = BloomFetch( t_Source, s_Linear, c + float2( 0.0, 2.0 ), origin, size, invSize );
	yp2 = BloomScrub3( yp2 );
	const float wyp2 = BloomKneeWeight( yp2, knee, E );
	float3 xm2ym2 = BloomFetch( t_Source, s_Linear, c + float2( -2.0, -2.0 ), origin, size, invSize );
	xm2ym2 = BloomScrub3( xm2ym2 );
	const float wxm2ym2 = BloomKneeWeight( xm2ym2, knee, E );
	float3 xp2ym2 = BloomFetch( t_Source, s_Linear, c + float2( 2.0, -2.0 ), origin, size, invSize );
	xp2ym2 = BloomScrub3( xp2ym2 );
	const float wxp2ym2 = BloomKneeWeight( xp2ym2, knee, E );
	float3 xm2yp2 = BloomFetch( t_Source, s_Linear, c + float2( -2.0, 2.0 ), origin, size, invSize );
	xm2yp2 = BloomScrub3( xm2yp2 );
	const float wxm2yp2 = BloomKneeWeight( xm2yp2, knee, E );
	float3 xp2yp2 = BloomFetch( t_Source, s_Linear, c + float2( 2.0, 2.0 ), origin, size, invSize );
	xp2yp2 = BloomScrub3( xp2yp2 );
	const float wxp2yp2 = BloomKneeWeight( xp2yp2, knee, E );

	// The five boxes: the inner one of the four diagonals, and the four outer ones of two corners, two axis
	// taps and the centre, whose single weight serves all four
	const float3 inner = BloomBox( i00, wi00, i10, wi10, i01, wi01, i11, wi11 );
	const float3 boxA = BloomBox( xm2ym2, wxm2ym2, ym2, wym2, xm2, wxm2, ctr, wctr );
	const float3 boxB = BloomBox( ym2, wym2, xp2ym2, wxp2ym2, ctr, wctr, xp2, wxp2 );
	const float3 boxC = BloomBox( xm2, wxm2, ctr, wctr, xm2yp2, wxm2yp2, yp2, wyp2 );
	const float3 boxD = BloomBox( ctr, wctr, xp2, wxp2, yp2, wyp2, xp2yp2, wxp2yp2 );
	o_rgba.rgb = 0.5 * inner + 0.125 * ( boxA + boxB + boxC + boxD );
#else
	// The plain filter: the same thirteen taps with the boxes' means folded into their weights: the centre and
	// the four diagonals 0.125 each, the four axis taps 0.0625 each ( two boxes ), the four corners 0.03125
	// each. The weights sum to 1
	const float3 ctr = BloomFetch( t_Source, s_Linear, c, origin, size, invSize );
	const float3 i00 = BloomFetch( t_Source, s_Linear, c + float2( -1.0, -1.0 ), origin, size, invSize );
	const float3 i10 = BloomFetch( t_Source, s_Linear, c + float2( 1.0, -1.0 ), origin, size, invSize );
	const float3 i01 = BloomFetch( t_Source, s_Linear, c + float2( -1.0, 1.0 ), origin, size, invSize );
	const float3 i11 = BloomFetch( t_Source, s_Linear, c + float2( 1.0, 1.0 ), origin, size, invSize );
	const float3 xm2 = BloomFetch( t_Source, s_Linear, c + float2( -2.0, 0.0 ), origin, size, invSize );
	const float3 xp2 = BloomFetch( t_Source, s_Linear, c + float2( 2.0, 0.0 ), origin, size, invSize );
	const float3 ym2 = BloomFetch( t_Source, s_Linear, c + float2( 0.0, -2.0 ), origin, size, invSize );
	const float3 yp2 = BloomFetch( t_Source, s_Linear, c + float2( 0.0, 2.0 ), origin, size, invSize );
	const float3 xm2ym2 = BloomFetch( t_Source, s_Linear, c + float2( -2.0, -2.0 ), origin, size, invSize );
	const float3 xp2ym2 = BloomFetch( t_Source, s_Linear, c + float2( 2.0, -2.0 ), origin, size, invSize );
	const float3 xm2yp2 = BloomFetch( t_Source, s_Linear, c + float2( -2.0, 2.0 ), origin, size, invSize );
	const float3 xp2yp2 = BloomFetch( t_Source, s_Linear, c + float2( 2.0, 2.0 ), origin, size, invSize );
	o_rgba.rgb = 0.125 * ( ctr + i00 + i10 + i01 + i11 )
	           + 0.0625 * ( xm2 + xp2 + ym2 + yp2 )
	           + 0.03125 * ( xm2ym2 + xp2ym2 + xm2yp2 + xp2yp2 );
#endif
	o_rgba.a = 1.0;
}
