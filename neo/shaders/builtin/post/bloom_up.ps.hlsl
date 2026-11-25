// bloom_up.ps.hlsl - enlarges one bloom level into the level below it: a cubic B-spline of the source level
// read at half its own scale, times the tail, this level's weight against the one below. The pass draws with
// the blend ONE, ONE, so the level below becomes itself plus this enlargement; run from the top level down,
// level 0 ends as D0 + tail * up( D1 + tail * up( D2 + ... ) ). The alpha is 0 because the blend adds the
// alpha too and the chain's alpha is never read

#include "vulkan.hlsli"
#include "bloom_cb.h"
#include "bloom.inc.hlsl"

// *INDENT-OFF*
Texture2D t_Source : register( t0 );
SamplerState s_Linear : register( s0 );

VK_PUSH_CONSTANT ConstantBuffer<BloomConstants> g_Bloom : register( b0 );
// *INDENT-ON*

void main(
	in float4 pos : SV_Position,
	in float2 uv : UV,
	out float4 o_rgba : SV_Target )
{
	// level pixel p reads the level above at 0.5 * p, in its texels relative to its rectangle; the tail rides
	// in srcInvSize.z
	o_rgba.rgb = g_Bloom.srcInvSize.z * BloomBSpline( t_Source, s_Linear, 0.5 * pos.xy,
		g_Bloom.srcRect.xy, g_Bloom.srcRect.zw, g_Bloom.srcInvSize.xy );
	o_rgba.a = 0.0;
}
