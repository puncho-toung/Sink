// bloom.inc.hlsl - the two fetches every bloom shader shares: the clamped bilinear tap of a level's rectangle,
// and the cubic B-spline that enlarges a level in four such taps. The including shader passes the texture and
// the sampler; every position is in the source's own texels, relative to its rectangle's origin.

#ifndef BLOOM_INC_HLSL
#define BLOOM_INC_HLSL

// The clamped bilinear fetch of a tap at the relative position t of a source whose rectangle has the origin
// rectOrigin and the size rectSize, in the source's texels. The position is clamped half a texel inside the
// rectangle, so the bilinear footprint stays inside it but for a neighbour at weight 0 on the right and bottom
// bounds: a tap clamped to a bound sits on the last texel's centre, and the read still touches the texel
// beyond it at weight 0. That neighbour is stale data, and only its being finite keeps it harmless - the
// levels are cleared when the chain is made, and the first pass scrubs what it fetches - since 0 times an
// Inf is a NaN. The picture continues past its border by its own edge texels ( a source of one colour stays
// one colour ). invTextureSize is 1 / the source TEXTURE's size: the texture may be larger than the
// rectangle it holds a view of, and the origin is added after the clamp, so a rectangle that starts inside
// the texture still reads only its own texels
float3 BloomFetch( Texture2D tex, SamplerState samp, float2 t, float2 rectOrigin, float2 rectSize, float2 invTextureSize )
{
	const float2 clamped = clamp( t, float2( 0.5, 0.5 ), rectSize - float2( 0.5, 0.5 ) );
	return tex.SampleLevel( samp, ( rectOrigin + clamped ) * invTextureSize, 0 ).rgb;
}

// The cubic B-spline of the source at the relative position t, in four bilinear taps. Per axis: tc = t - 0.5
// puts the texel whose centre is nearest at i = floor( tc ) with fraction f; the four spline weights, here
// times 6, pair into s0 = u0 + u1 and s1 = u2 + u3; g0 = s0 / 6 and g1 = 1 - g0 are the pairs' weights, which
// sum to 1 and are never 0 ( g0 runs from 5/6 down to 1/6 on f in 0..1 ). p0 and p1 are the two positions whose
// bilinear reads reproduce the pairs exactly: bilinear at p0 gives u0 / 6 at texel i - 1 and u1 / 6 at texel i,
// bilinear at p1 gives u2 / 6 at texel i and u3 / 6 at i + 1. A linear ramp is reproduced exactly, as any
// B-spline does
float3 BloomBSpline( Texture2D tex, SamplerState samp, float2 t, float2 rectOrigin, float2 rectSize, float2 invTextureSize )
{
	const float2 tc = t - 0.5;
	const float2 i = floor( tc );
	const float2 f = tc - i;
	const float2 f2 = f * f;
	const float2 f3 = f2 * f;
	const float2 omf = 1.0 - f;
	// the four weights times 6: the common 1 / 6 cancels in the two ratios below and is applied once, to g0
	const float2 u0 = omf * omf * omf;
	const float2 u1 = 4.0 - 6.0 * f2 + 3.0 * f3;
	const float2 u2 = 1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3;
	const float2 u3 = f3;
	const float2 s0 = u0 + u1;
	const float2 s1 = u2 + u3;
	const float2 g0 = s0 * ( 1.0 / 6.0 );
	const float2 g1 = 1.0 - g0;				// the four weights sum to 1: g1 is what g0 leaves
	const float2 p0 = i - 1.0 + u1 / s0 + 0.5;
	const float2 p1 = i + 1.0 + u3 / s1 + 0.5;

	float3 sum = BloomFetch( tex, samp, float2( p0.x, p0.y ), rectOrigin, rectSize, invTextureSize ) * ( g0.x * g0.y );
	sum += BloomFetch( tex, samp, float2( p1.x, p0.y ), rectOrigin, rectSize, invTextureSize ) * ( g1.x * g0.y );
	sum += BloomFetch( tex, samp, float2( p0.x, p1.y ), rectOrigin, rectSize, invTextureSize ) * ( g0.x * g1.y );
	sum += BloomFetch( tex, samp, float2( p1.x, p1.y ), rectOrigin, rectSize, invTextureSize ) * ( g1.x * g1.y );
	return sum;
}

#endif // BLOOM_INC_HLSL
