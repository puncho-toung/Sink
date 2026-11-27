/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#pragma pack_matrix(row_major)

#include "vulkan.hlsli"
#include "tonemapping_cb.h"
// ---- TRACK G: the bloom chain's clamped fetch and B-spline, for the BLOOM variant ----
#if defined( BLOOM )
#include "bloom.inc.hlsl"
#endif
// ---- END TRACK G ----

// *INDENT-OFF*
#if SOURCE_ARRAY
Texture2DArray t_Source : register(t0);
#else
Texture2D t_Source : register(t0);
#endif
Buffer<uint> t_Exposure : register(t1);

Texture2D t_ColorLUT : register(t2);
SamplerState s_ColorLUTSampler : register(s0);
// ---- TRACK G: the bloom level the mix reads, for the BLOOM variant ----
#if defined( BLOOM )
Texture2D t_Bloom : register( t3 );
#endif
// ---- END TRACK G ----

#if USE_PUSH_CONSTANTS
VK_PUSH_CONSTANT ConstantBuffer<ToneMappingConstants> g_ToneMapping : register( b0 );
#else
cbuffer c_ToneMapping : register( b0 )
{
    ToneMappingConstants g_ToneMapping;
};
#endif
// *INDENT-ON*

float3 ACESFilm( float3 x )
{
	float a = 2.51;
	float b = 0.03;
	float c = 2.43;
	float d = 0.59;
	float e = 0.14;
	return saturate( ( x * ( a * x + b ) ) / ( x * ( c * x + d ) + e ) );
}

float Luminance( float3 color )
{
	return 0.2126 * color.r + 0.7152 * color.g + 0.0722 * color.b;
}

float3 ConvertToLDR( float3 color )
{
	float srcLuminance = Luminance( color );

	if( srcLuminance <= 0 )
	{
		return 0;
	}

	float adaptedLuminance = asfloat( t_Exposure[0] );
	if( adaptedLuminance <= 0 )
	{
		adaptedLuminance = g_ToneMapping.minAdaptedLuminance;
	}

	float scaledLuminance = g_ToneMapping.exposureScale * srcLuminance / adaptedLuminance;
	float mappedLuminance = ( scaledLuminance * ( 1 + scaledLuminance * g_ToneMapping.whitePointInvSquared ) ) / ( 1 + scaledLuminance );

	return color * ( mappedLuminance / srcLuminance );
}

float3 ApplyColorLUT( float3 color )
{
	const float size = g_ToneMapping.colorLUTTextureSize.y;

	color = saturate( color );

	float r = color.r * ( size - 1 ) + 0.5;
	float g = color.g * ( size - 1 ) + 0.5;

	float b = color.b * ( size - 1 );

	float2 uv1 = float2( ( floor( b ) + 0 ) * size + r, g ) * g_ToneMapping.colorLUTTextureSizeInv.xy;
	float2 uv2 = float2( ( floor( b ) + 1 ) * size + r, g ) * g_ToneMapping.colorLUTTextureSizeInv.xy;

	float3 c1 = t_ColorLUT.SampleLevel( s_ColorLUTSampler, uv1, 0 ).rgb;
	float3 c2 = t_ColorLUT.SampleLevel( s_ColorLUTSampler, uv2, 0 ).rgb;

	return lerp( c1, c2, frac( b ) );
}


void main(
	in float4 pos : SV_Position,
	in float2 uv : UV,
	out float4 o_rgba : SV_Target )
{
#if SOURCE_ARRAY
	float4 HdrColor = t_Source[uint3( pos.xy, g_ToneMapping.sourceSlice )];
#else
	float4 HdrColor = t_Source[pos.xy];
#endif
	// ---- TRACK G: the bloom chain mixed into the HDR frame before the exposure. The frame's
	// weight and the bloom's are bloomMix.x and bloomMix.y; the screen pixel centre P reads the level at
	// ( P - viewOrigin ) * bloomScale.x, that scale being 0.5 / 2^level, and the fetch's 1 / the level's
	// TEXTURE size is bloomScale.zw: the texture may be larger than the rectangle it holds a view of ( the
	// view can be smaller than the frame ), so the size comes in with the constants instead of being read
	// off the texture. The level's rectangle is bloomRect.zw at the origin ( 0, 0 ) of its own texture, and
	// bloomRect.zw is only the clamp's bound. bloomMix.z, the level, is not read by this shader: the
	// C++ binds the level's texture and has folded 2^level into bloomScale.x. With bloomMix.w, one
	// bilinear tap replaces the B-spline
#if defined( BLOOM )
	const float2 bloomT = ( pos.xy - g_ToneMapping.bloomRect.xy ) * g_ToneMapping.bloomScale.x;
	// a real branch, not a ?: - before HLSL 2021 a ternary evaluates both of its sides, and after it a compiler may
	// still fetch both and select: the point of the switch is to leave one of the two fetch groups out
	float3 bloom;
	if( g_ToneMapping.bloomMix.w != 0.0 )
	{
		bloom = BloomFetch( t_Bloom, s_ColorLUTSampler, bloomT, float2( 0.0, 0.0 ), g_ToneMapping.bloomRect.zw, g_ToneMapping.bloomScale.zw );
	}
	else
	{
		bloom = BloomBSpline( t_Bloom, s_ColorLUTSampler, bloomT, float2( 0.0, 0.0 ), g_ToneMapping.bloomRect.zw, g_ToneMapping.bloomScale.zw );
	}
	HdrColor.rgb = HdrColor.rgb * g_ToneMapping.bloomMix.x + bloom * g_ToneMapping.bloomMix.y;
#endif
	// ---- END TRACK G ----
	o_rgba.rgb = ConvertToLDR( HdrColor.rgb );
	o_rgba.a = HdrColor.a;

	if( g_ToneMapping.colorLUTTextureSize.x > 0 )
	{
		o_rgba.rgb = ApplyColorLUT( o_rgba.rgb );
	}
	else
	{
		// Tonemapping curve is applied after exposure.
		o_rgba.rgb = ACESFilm( o_rgba.rgb );
	}

	// Gamma correction since we are not rendering to an sRGB render target.
	const float hdrGamma = 2.2;
	float gamma = 1.0 / hdrGamma;
	o_rgba.r = pow( o_rgba.r, gamma );
	o_rgba.g = pow( o_rgba.g, gamma );
	o_rgba.b = pow( o_rgba.b, gamma );
}
