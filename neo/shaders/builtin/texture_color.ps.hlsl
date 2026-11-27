/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#include "global_inc.hlsl"
#include "renderParmSet4.inc.hlsl"


// *INDENT-OFF*
Texture2D	 t_BaseColor	: register( t0 VK_DESCRIPTOR_SET( 1 ) );
SamplerState s_Sampler		: register( s0 VK_DESCRIPTOR_SET( 2 ) );

#if defined( USE_FROXEL_FOG ) && USE_FROXEL_FOG
	// the froxel fog permutation ( D33 ): bound by the stage loop after the fog view's apply ( V5, D20, D35 )
#define FROXEL_FOG_SET 3
#include "froxel_fog.inc.hlsl"
#endif

struct PS_IN {
	float4 position		: SV_POSITION;
	float3 texcoord0	: TEXCOORD0_centroid;
	float4 color		: COLOR0;
};

struct PS_OUT {
	float4 color : SV_Target0;
#if defined( USE_GLOW ) && USE_GLOW
	// WP-J21: the glow twin's second output, the glow mask: the stage's luminance with its alpha, blended as the colour is
	float4 glow : SV_Target1;
#endif
};
// *INDENT-ON*

void main( in PS_IN fragment, out PS_OUT result )
{
	float2 uv = fragment.texcoord0.xy;

	// PSX affine texture mapping
	if( pc.rpPSXDistortions.z > 0.0 )
	{
		uv /= fragment.texcoord0.z;
	}

	float4 color = t_BaseColor.Sample( s_Sampler, uv ) * fragment.color;
	clip( color.a - pc.rpAlphaTest.x );
	result.color = sRGBAToLinearRGBA( color );

#if defined( USE_FROXEL_FOG ) && USE_FROXEL_FOG
	// V5: the stage seen through the fog to its own depth, by the blend kind the stage loop set in rpTexGen0Enabled.y
	result.color = FroxelFogTranslucent( result.color, FroxelFogSamplePixel( fragment.position.xy, FroxelFogPixelViewDepth( fragment.position ) ), pc.rpTexGen0Enabled.y );
#endif

#if defined( USE_GLOW ) && USE_GLOW
	// WP-J21: after the colour is final ( fogged too: the fog in front of a lamp dims its glow as it dims the lamp ). WP-J27: the
	// green is the luminance times 2^slot, the source's style slot ( rpTexGen0Enabled.z, 0 for the view's bloom ), so the gate
	// reads the slot back as log2( green / red ); both blend alike, so the ratio holds under the stage's blend, and two slots
	// blended in a pixel give a ratio between powers of two, which the gate reads as the view's ( review R2-F2 )
	const float glowLuma = dot( result.color.rgb, float3( 0.2126, 0.7152, 0.0722 ) );
	result.glow = float4( glowLuma, glowLuma * exp2( pc.rpTexGen0Enabled.z ), 0.0, result.color.a );
#endif
}
