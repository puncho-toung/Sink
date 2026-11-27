// bloom_fft_compose.ps.hlsl - the bloom convolution's re-entry ( WP-J24 ): drawn over level 0's rectangle with the up
// pass's blend ( ONE, ONE ), it adds the convolution's result to level 0, which holds the crisp core the first pass
// drew. The result is read from the field's cells through the cubic B-spline of bloom.inc.hlsl and times
// ( 1 - w0 ) / w0, so that the tonemap's mix weight, strength x w0, gives the core its w0 and the shaped glow its
// 1 - w0: the Tail bar's split. The mix and bloomProbe read level 0 as they do without the convolution.

#include "vulkan.hlsli"
#include "bloom_fft_cb.h"
#include "bloom.inc.hlsl"

// *INDENT-OFF*
Texture2D t_Result : register( t0 );
SamplerState s_Linear : register( s0 );

cbuffer c_BloomFft : register( b1 )
{
	BloomFftConstants g_Fft;
};
// *INDENT-ON*

void main(
	in float4 pos : SV_Position,
	in float2 uv : UV,
	out float4 o_rgba : SV_Target )
{
	// level 0's texel p covers the view's pixels 2p to 2p + 2, so its centre 2 ( p + 0.5 ) = 2 pos.xy is the field's
	// position 2 pos.xy k_s, in cells from the picture's corner
	const float2 t = 2.0 * pos.xy * g_Fft.shape.z;
	o_rgba.rgb = g_Fft.shape.y * BloomBSpline( t_Result, s_Linear, t, float2( 0.0, 0.0 ), float2( g_Fft.image.xy ), 1.0 / float2( g_Fft.field.xy ) );
	o_rgba.a = 0.0;
}
