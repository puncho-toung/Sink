// lightdiff.ps.hlsl - pixel stage of the light-difference view ( track F; r_showLightDiff 2; D10, D49, D79 ). The
// backend's LightDiffPass hands it the view's lit passes twice: t_A lit by the forward pass's loop, t_B by the legacy
// pass. Mode 0 draws | A - B | times a scale into the view. Mode 1 draws into a small target, one texel per block of
// LIGHTDIFF_BLOCK x LIGHTDIFF_BLOCK view pixels: the sum over the block of the difference ( its three channels
// averaged ), the block's largest channel difference, the sum of the legacy picture, and the number of the block's
// pixels inside the view. The backend sums that small picture two frames later.

#include <lightdiff.cb.h>

// *INDENT-OFF*
#ifdef SPIRV
[[vk::push_constant]] ConstantBuffer<LightDiffConstants> g_Diff;
#else
cbuffer g_Diff : register( b0 )
{
	LightDiffConstants g_Diff;
}
#endif

Texture2D<float4> t_A : register( t0 );
Texture2D<float4> t_B : register( t1 );

struct PS_IN
{
	float4 position	: SV_Position;
	float2 uv		: TEXCOORD0;
};
// *INDENT-ON*

float4 main( PS_IN fragment ) : SV_Target
{
	const int2 pixel = int2( fragment.position.xy );
	if( g_Diff.mode == 0 )
	{
		const int3 p = int3( pixel, 0 );
		return float4( abs( t_A.Load( p ).rgb - t_B.Load( p ).rgb ) * g_Diff.scale, 1.0 );
	}

	const int2 first = g_Diff.viewOrigin + pixel * LIGHTDIFF_BLOCK;
	const int2 end = g_Diff.viewOrigin + g_Diff.viewSize;
	float sum = 0.0;
	float largest = 0.0;
	float sumLegacy = 0.0;
	float count = 0.0;
	for( int y = 0; y < LIGHTDIFF_BLOCK; y++ )
	{
		for( int x = 0; x < LIGHTDIFF_BLOCK; x++ )
		{
			const int2 q = first + int2( x, y );
			if( q.x < end.x && q.y < end.y )
			{
				const float3 a = t_A.Load( int3( q, 0 ) ).rgb;
				const float3 b = t_B.Load( int3( q, 0 ) ).rgb;
				const float3 d = abs( a - b );
				sum += ( d.r + d.g + d.b ) * ( 1.0 / 3.0 );
				largest = max( largest, max( d.r, max( d.g, d.b ) ) );
				sumLegacy += ( b.r + b.g + b.b ) * ( 1.0 / 3.0 );
				count += 1.0;
			}
		}
	}
	return float4( sum, largest, sumLegacy, count );
}
