// bloom_down.ps.hlsl - halves the bloom's source into the next level: thirteen bilinear taps of the source's
// rectangle, combined as five boxes ( an inner box of the four ( +-1, +-1 ) taps at weight 0.5 and four outer
// boxes of four taps each at 0.125, the centre tap shared by all four outer boxes ). Its marginal along either
// axis is [ 1 3 4 4 3 1 ] / 16 over six source texels ( the 2D kernel is not that row's outer product, but
// like it is zero at every frequency a halving folds back ). The passes after the first ( FIRST 0 ) read
// already scrubbed levels and just filter. The first pass of the chain ( FIRST 1, the frame into level 0 ) is
// the GLOW's ( WP-G3 ): it reads the footprint's thirty-six frame pixels one by one, scrubs each to the ceiling
// ( at most BLOOM_SCRUB_BOUND ), killing NaNs and infinities before they can spread, and thresholds each by
// itself before the same thirteen taps are formed from them, because a threshold on a bilinear tap ( the mean of
// four pixels ) depends on how a small lamp falls on the taps' grid, and its glow then pulses as it moves; and
// it weights every tap by the firefly knee, so a lone very bright pixel adds only about the knee to the bloom.
// Two shorter forms were measured and dropped: the same footprint in a loop over local arrays ( the chain cost five
// times as much ), and the texel's own four pixels alone ( cheap, but a two-by-two box is no filter: a small
// lamp's halo jumped as it moved ). A first pass on thirteen bilinear taps with no threshold was the MIX of
// v0001.3r, removed by WP-G4.
// The first pass writes a second target, the wide levels' source, which level 1's reduction reads instead of
// level 0: the texel's own four pixels' light under a box. FIRST 2 is FIRST 1 reading the frame's depth too: a
// pixel of the view weapon is black to the bloom ( WP-G4 ), and each of the four pixels' light enters the wide
// levels' source times its own nearness ( the distance tail ).

#include "vulkan.hlsli"
#include "bloom_cb.h"
#include "bloom.inc.hlsl"

// *INDENT-OFF*
Texture2D t_Source : register( t0 );
#if FIRST
Buffer<uint> t_Exposure : register( t1 );
#endif
#if FIRST == 2
Texture2D<float> t_Depth : register( t2 );	// the frame's depth at full size ( the Hi-Z image's mip 0 )
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

// The firefly knee's weight for a tap of mean luminance L: k / ( k + L * E ), written so nothing divides by k. A
// tap of exposed luminance L survives at most about knee/(knee+L*E) of itself, so a lone very bright pixel adds
// at most about a third of the knee. k <= 0 turns the knee off: every weight is then 1, the plain filter on
// scrubbed taps
float BloomKneeWeightL( float L, float k, float E )
{
	return ( k > 0.0 ) ? k / ( k + L * E ) : 1.0;
}

// A box's weighted mean of its four taps. The floor on the weight sum keeps a box whose weights all came out 0
// at 0 instead of a NaN
float3 BloomBox( float3 c0, float w0, float3 c1, float w1, float3 c2, float w2, float3 c3, float w3 )
{
	return ( c0 * w0 + c1 * w1 + c2 * w2 + c3 * w3 ) / max( w0 + w1 + w2 + w3, 1e-30 );
}

// The glow ( WP-G3 ): what of a frame pixel enters the chain. A pixel of exposed brightness B
// ( its largest channel times E, the unit of the tonemapper's scaled luminance ) keeps the share of itself that
// stands above the threshold T, with a soft knee of half-width K round T so that a surface crossing it does not
// pop. The brightness is the largest channel, not the luminance, so that a saturated lamp ( a cyan panel, a red
// light ) glows as readily as a white one of the same strength. The C++ sends T in exposure.w ( above 0 )
// and K in srcInvSize.z. E is finite ( held where it is made ) and the pixel is scrubbed, so B is finite
// and the share lies in 0 .. 1
float3 BloomGlowPixel( float3 c, float E )
{
	const float T = g_Bloom.exposure.w;
	const float K = g_Bloom.srcInvSize.z;
	const float B = max( c.r, max( c.g, c.b ) ) * E;
	float soft = clamp( B - T + K, 0.0, 2.0 * K );
	soft = soft * soft / ( 4.0 * K + 1e-5 );
	const float over = max( soft, B - T );
	return c * saturate( over / max( B, 1e-5 ) );
}
#endif

#if FIRST == 2
// WP-G3, the distance tail. The first pass writes two targets: level 0 with all of the light ( the core, which
// every lamp keeps ) and the wide levels' source, which level 1's reduction reads instead of level 0. In FIRST 2 a
// pixel's light enters that source times its NEARNESS: a lamp nearer than the near distance keeps the whole tail the
// player set, one beyond the far distance the share in tailDist.z of it ( 0: its core alone ). The nearness is that
// of the OPAQUE surface at each lit pixel ( the depth prepass's depth: a translucent emitter, a flame or a flare,
// writes none and takes the distance of what is behind it ): a small solid lamp in front of a far wall is near, and
// the wall behind it is not lit. The distance along the view axis is the froxel fog's conversion
// ( FroxelFogViewDepth ), z / ( depth + w ) with the projection's two depth terms; the clear value comes back
// infinitely far ( the sky where nothing wrote depth; a lamp seen in a mirror or on a camera screen takes the
// distance of that surface, which is drawn into the depth prepass )
float BloomViewDistance( float deviceDepth )
{
	const float denominator = deviceDepth + g_Bloom.tailDepth.y;
	return ( denominator * g_Bloom.tailDepth.x > 0.0 ) ? g_Bloom.tailDepth.x / denominator : 1e30;
}

// the nearness of a device depth: 1 up to the near distance, the far share from the far distance on. Finite for
// every depth, so a pixel without light times it is still 0
float BloomNearness( float deviceDepth )
{
	const float d = BloomViewDistance( deviceDepth );
	return lerp( g_Bloom.tailDist.z, 1.0, saturate( ( g_Bloom.tailDist.x - d ) * g_Bloom.tailDist.y ) );
}

// WP-G4, the view weapon ( the owner, 2026-11-26: the bloom was "haloing the weapons in total darkness" ): 1 for a
// pixel of the world, 0 for one of the first-person weapon, which is then black to the bloom ( no light into any
// level, no luminance into a tap's knee weight ). The weapon is drawn with idRenderMatrix::ApplyDepthHack, which
// puts all of it in the nearest quarter of the depth range; the C++ sends that bound in tailDepth.z and the depth
// convention's sign in tailDepth.w ( z 1 and w 0: no pixel is the weapon's, and the weapon glows like the world ).
// A translucent surface in front of the weapon ( a muzzle flash over the barrel ) writes no depth and is left out
// with it; over the world it glows
float BloomWorldPixel( float deviceDepth )
{
	return ( deviceDepth * g_Bloom.tailDepth.w > g_Bloom.tailDepth.z ) ? 0.0 : 1.0;
}
#endif

void main(
	in float4 pos : SV_Position,
	in float2 uv : UV,
#if FIRST
	out float4 o_rgba : SV_Target,
	out float4 o_wide : SV_Target1 )
#else
	out float4 o_rgba : SV_Target )
#endif
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
	// a tap of luminance 0 would be a NaN. The factor is held to a finite bound too, since r_exposure has no
	// enforced range and an infinite factor times a tap of 0 would be a NaN in the knee's weight and in the
	// glow's share
	float adapted = asfloat( t_Exposure[0] );
	if( !( adapted > 0.0 ) )
	{
		adapted = g_Bloom.exposure.y;
	}
	const float E = min( g_Bloom.exposure.x / max( adapted, 0.0001 ), 1.0e9 );
	const float knee = g_Bloom.srcInvSize.w;

	// The first pass. Each of the thirteen taps is the mean of four frame pixels, and the footprint's thirty-six frame
	// pixels are read one by one: pixel ( i, j ), named gij, is the one at c + ( i - 3, j - 3 ) in the rectangle's pixel
	// indices ( the tap at c + d is the mean of the pixels c + d - 1 and c + d along either axis ), and a pixel beyond
	// the rectangle is the rectangle's edge pixel. Each is scrubbed and thresholded BY ITSELF ( BloomGlowPixel ), and the
	// thirteen taps and five boxes are then formed from the results: what passes the threshold is a sum over pixels,
	// the same wherever a small lamp stands, behind the same filter. Written out as thirty-six named values, with no
	// loop and no array: the loop over local arrays this first was cost five times as much
	const int2 base = int2( c ) - int2( 3, 3 );
	const int2 last = int2( size ) - int2( 1, 1 );
	const int2 org = int2( origin );
	// The thirty-six pixels, each loaded and scrubbed, and the largest channel among them
	float top = 0.0;
#define BLOOM_PIXEL( i, j ) const float3 v##i##j = BloomScrub3( t_Source.Load( int3( org + clamp( base + int2( i, j ), int2( 0, 0 ), last ), 0 ) ).rgb ); top = max( top, max( v##i##j.r, max( v##i##j.g, v##i##j.b ) ) )
	BLOOM_PIXEL( 0, 0 ); BLOOM_PIXEL( 1, 0 ); BLOOM_PIXEL( 2, 0 ); BLOOM_PIXEL( 3, 0 ); BLOOM_PIXEL( 4, 0 ); BLOOM_PIXEL( 5, 0 );
	BLOOM_PIXEL( 0, 1 ); BLOOM_PIXEL( 1, 1 ); BLOOM_PIXEL( 2, 1 ); BLOOM_PIXEL( 3, 1 ); BLOOM_PIXEL( 4, 1 ); BLOOM_PIXEL( 5, 1 );
	BLOOM_PIXEL( 0, 2 ); BLOOM_PIXEL( 1, 2 ); BLOOM_PIXEL( 2, 2 ); BLOOM_PIXEL( 3, 2 ); BLOOM_PIXEL( 4, 2 ); BLOOM_PIXEL( 5, 2 );
	BLOOM_PIXEL( 0, 3 ); BLOOM_PIXEL( 1, 3 ); BLOOM_PIXEL( 2, 3 ); BLOOM_PIXEL( 3, 3 ); BLOOM_PIXEL( 4, 3 ); BLOOM_PIXEL( 5, 3 );
	BLOOM_PIXEL( 0, 4 ); BLOOM_PIXEL( 1, 4 ); BLOOM_PIXEL( 2, 4 ); BLOOM_PIXEL( 3, 4 ); BLOOM_PIXEL( 4, 4 ); BLOOM_PIXEL( 5, 4 );
	BLOOM_PIXEL( 0, 5 ); BLOOM_PIXEL( 1, 5 ); BLOOM_PIXEL( 2, 5 ); BLOOM_PIXEL( 3, 5 ); BLOOM_PIXEL( 4, 5 ); BLOOM_PIXEL( 5, 5 );
#undef BLOOM_PIXEL

	// Review O2-4: most of a frame is under the threshold, and such a texel has nothing to add. BloomGlowPixel keeps
	// a share above 0 only of a pixel whose B - T + K is above 0 ( the knee's lower end ); the test below is that
	// expression for the brightest of the thirty-six, so where it fails every pixel's share is exactly 0, every tap
	// and box is exactly 0, and the zeros written here are the bits the arithmetic below would write. A NaN in E
	// fails the test too and writes zeros
	[branch]
	if( !( top * E - g_Bloom.exposure.w + g_Bloom.srcInvSize.z > 0.0 ) )
	{
		o_rgba = float4( 0.0, 0.0, 0.0, 1.0 );
		o_wide = float4( 0.0, 0.0, 0.0, 1.0 );
		return;
	}

#if FIRST == 2
	// WP-G4: the thirty-six pixels' depths, read behind the test above, so only where the footprint holds light. A
	// pixel of the view weapon is black to the bloom: its light and its luminance are taken times 0, a world pixel's
	// times 1 ( the same bits )
#define BLOOM_DEPTH( i, j ) const float z##i##j = t_Depth.Load( int3( org + clamp( base + int2( i, j ), int2( 0, 0 ), last ), 0 ) )
	BLOOM_DEPTH( 0, 0 ); BLOOM_DEPTH( 1, 0 ); BLOOM_DEPTH( 2, 0 ); BLOOM_DEPTH( 3, 0 ); BLOOM_DEPTH( 4, 0 ); BLOOM_DEPTH( 5, 0 );
	BLOOM_DEPTH( 0, 1 ); BLOOM_DEPTH( 1, 1 ); BLOOM_DEPTH( 2, 1 ); BLOOM_DEPTH( 3, 1 ); BLOOM_DEPTH( 4, 1 ); BLOOM_DEPTH( 5, 1 );
	BLOOM_DEPTH( 0, 2 ); BLOOM_DEPTH( 1, 2 ); BLOOM_DEPTH( 2, 2 ); BLOOM_DEPTH( 3, 2 ); BLOOM_DEPTH( 4, 2 ); BLOOM_DEPTH( 5, 2 );
	BLOOM_DEPTH( 0, 3 ); BLOOM_DEPTH( 1, 3 ); BLOOM_DEPTH( 2, 3 ); BLOOM_DEPTH( 3, 3 ); BLOOM_DEPTH( 4, 3 ); BLOOM_DEPTH( 5, 3 );
	BLOOM_DEPTH( 0, 4 ); BLOOM_DEPTH( 1, 4 ); BLOOM_DEPTH( 2, 4 ); BLOOM_DEPTH( 3, 4 ); BLOOM_DEPTH( 4, 4 ); BLOOM_DEPTH( 5, 4 );
	BLOOM_DEPTH( 0, 5 ); BLOOM_DEPTH( 1, 5 ); BLOOM_DEPTH( 2, 5 ); BLOOM_DEPTH( 3, 5 ); BLOOM_DEPTH( 4, 5 ); BLOOM_DEPTH( 5, 5 );
#undef BLOOM_DEPTH
	// each pixel's light ( the share above the threshold ) and its luminance BEFORE the threshold
#define BLOOM_LIT( i, j ) const float m##i##j = BloomWorldPixel( z##i##j ); const float3 g##i##j = BloomGlowPixel( v##i##j, E ) * m##i##j; const float l##i##j = dot( v##i##j, float3( 0.2126, 0.7152, 0.0722 ) ) * m##i##j
#else
	// each pixel's light ( the share above the threshold ) and its luminance BEFORE the threshold
#define BLOOM_LIT( i, j ) const float3 g##i##j = BloomGlowPixel( v##i##j, E ); const float l##i##j = dot( v##i##j, float3( 0.2126, 0.7152, 0.0722 ) )
#endif
	BLOOM_LIT( 0, 0 ); BLOOM_LIT( 1, 0 ); BLOOM_LIT( 2, 0 ); BLOOM_LIT( 3, 0 ); BLOOM_LIT( 4, 0 ); BLOOM_LIT( 5, 0 );
	BLOOM_LIT( 0, 1 ); BLOOM_LIT( 1, 1 ); BLOOM_LIT( 2, 1 ); BLOOM_LIT( 3, 1 ); BLOOM_LIT( 4, 1 ); BLOOM_LIT( 5, 1 );
	BLOOM_LIT( 0, 2 ); BLOOM_LIT( 1, 2 ); BLOOM_LIT( 2, 2 ); BLOOM_LIT( 3, 2 ); BLOOM_LIT( 4, 2 ); BLOOM_LIT( 5, 2 );
	BLOOM_LIT( 0, 3 ); BLOOM_LIT( 1, 3 ); BLOOM_LIT( 2, 3 ); BLOOM_LIT( 3, 3 ); BLOOM_LIT( 4, 3 ); BLOOM_LIT( 5, 3 );
	BLOOM_LIT( 0, 4 ); BLOOM_LIT( 1, 4 ); BLOOM_LIT( 2, 4 ); BLOOM_LIT( 3, 4 ); BLOOM_LIT( 4, 4 ); BLOOM_LIT( 5, 4 );
	BLOOM_LIT( 0, 5 ); BLOOM_LIT( 1, 5 ); BLOOM_LIT( 2, 5 ); BLOOM_LIT( 3, 5 ); BLOOM_LIT( 4, 5 ); BLOOM_LIT( 5, 5 );
#undef BLOOM_LIT

	// the thirteen taps, each the mean of its four pixels, and its knee weight from the pixels' luminance BEFORE the
	// threshold, so the firefly bound is the knee's own for level 0 ( review O3-9: the wide levels' source
	// below weighs each of its four pixels by itself, so a lone pixel brighter than the knee is cut harder in the
	// tail than in the core )
	const float3 ctr = 0.25 * ( g22 + g32 + g23 + g33 );
	const float wctr = BloomKneeWeightL( 0.25 * ( l22 + l32 + l23 + l33 ), knee, E );
	const float3 i00 = 0.25 * ( g11 + g21 + g12 + g22 );
	const float wi00 = BloomKneeWeightL( 0.25 * ( l11 + l21 + l12 + l22 ), knee, E );
	const float3 i10 = 0.25 * ( g31 + g41 + g32 + g42 );
	const float wi10 = BloomKneeWeightL( 0.25 * ( l31 + l41 + l32 + l42 ), knee, E );
	const float3 i01 = 0.25 * ( g13 + g23 + g14 + g24 );
	const float wi01 = BloomKneeWeightL( 0.25 * ( l13 + l23 + l14 + l24 ), knee, E );
	const float3 i11 = 0.25 * ( g33 + g43 + g34 + g44 );
	const float wi11 = BloomKneeWeightL( 0.25 * ( l33 + l43 + l34 + l44 ), knee, E );
	const float3 xm2 = 0.25 * ( g02 + g12 + g03 + g13 );
	const float wxm2 = BloomKneeWeightL( 0.25 * ( l02 + l12 + l03 + l13 ), knee, E );
	const float3 xp2 = 0.25 * ( g42 + g52 + g43 + g53 );
	const float wxp2 = BloomKneeWeightL( 0.25 * ( l42 + l52 + l43 + l53 ), knee, E );
	const float3 ym2 = 0.25 * ( g20 + g30 + g21 + g31 );
	const float wym2 = BloomKneeWeightL( 0.25 * ( l20 + l30 + l21 + l31 ), knee, E );
	const float3 yp2 = 0.25 * ( g24 + g34 + g25 + g35 );
	const float wyp2 = BloomKneeWeightL( 0.25 * ( l24 + l34 + l25 + l35 ), knee, E );
	const float3 xm2ym2 = 0.25 * ( g00 + g10 + g01 + g11 );
	const float wxm2ym2 = BloomKneeWeightL( 0.25 * ( l00 + l10 + l01 + l11 ), knee, E );
	const float3 xp2ym2 = 0.25 * ( g40 + g50 + g41 + g51 );
	const float wxp2ym2 = BloomKneeWeightL( 0.25 * ( l40 + l50 + l41 + l51 ), knee, E );
	const float3 xm2yp2 = 0.25 * ( g04 + g14 + g05 + g15 );
	const float wxm2yp2 = BloomKneeWeightL( 0.25 * ( l04 + l14 + l05 + l15 ), knee, E );
	const float3 xp2yp2 = 0.25 * ( g44 + g54 + g45 + g55 );
	const float wxp2yp2 = BloomKneeWeightL( 0.25 * ( l44 + l54 + l45 + l55 ), knee, E );

	// the five boxes
	const float3 inner = BloomBox( i00, wi00, i10, wi10, i01, wi01, i11, wi11 );
	const float3 boxA = BloomBox( xm2ym2, wxm2ym2, ym2, wym2, xm2, wxm2, ctr, wctr );
	const float3 boxB = BloomBox( ym2, wym2, xp2ym2, wxp2ym2, ctr, wctr, xp2, wxp2 );
	const float3 boxC = BloomBox( xm2, wxm2, ctr, wctr, xm2yp2, wxm2yp2, yp2, wyp2 );
	const float3 boxD = BloomBox( ctr, wctr, xp2, wxp2, yp2, wyp2, xp2yp2, wxp2yp2 );
	o_rgba.rgb = 0.5 * inner + 0.125 * ( boxA + boxB + boxC + boxD );
	// The wide levels' source ( level 1 is reduced from it, not from level 0 ): the knee-weighted mean of the texel's OWN
	// four pixels' light ( the centre block of the footprint ), each times its own nearness. Every frame pixel is in
	// exactly one texel's four ( on an odd last row or column the four are two distinct pixels, the centre tap's
	// clamped ones ), so the wide levels hold each pixel's light times that pixel's nearness wherever a lamp
	// stands; this box is no filter by itself and is followed by level 1's thirteen taps. With every lit pixel near
	// and none the view weapon's ( and in FIRST 1, where the nearness is 1 ) the two permutations write the same bits
	float3 s22 = g22;
	float3 s32 = g32;
	float3 s23 = g23;
	float3 s33 = g33;
#if FIRST == 2
	// each of the four pixels' light times its own nearness ( its depth was read above ). A pixel without light is 0
	// times a finite nearness, 0
	s22 = g22 * BloomNearness( z22 );
	s32 = g32 * BloomNearness( z32 );
	s23 = g23 * BloomNearness( z23 );
	s33 = g33 * BloomNearness( z33 );
#endif
	o_wide.rgb = BloomBox( s22, BloomKneeWeightL( l22, knee, E ), s32, BloomKneeWeightL( l32, knee, E ), s23, BloomKneeWeightL( l23, knee, E ), s33, BloomKneeWeightL( l33, knee, E ) );
	o_wide.a = 1.0;
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
