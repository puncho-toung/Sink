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
#include "renderParmSet13.inc.hlsl"


// *INDENT-OFF*
// BINDING_LAYOUT_POST_PROCESS_FINAL2 ( the retro post-process layout ): only t3 is read
Texture2D t_BaseColor	: register( t0 VK_DESCRIPTOR_SET( 0 ) );
Texture2D t_BlueNoise	: register( t1 VK_DESCRIPTOR_SET( 0 ) );
Texture2D t_Normals		: register( t2 VK_DESCRIPTOR_SET( 0 ) );
Texture2D t_Depth		: register( t3 VK_DESCRIPTOR_SET( 0 ) );

SamplerState s_LinearClamp	: register(s0 VK_DESCRIPTOR_SET( 1 ) );
SamplerState s_LinearWrap	: register(s1 VK_DESCRIPTOR_SET( 1 ) );

struct PS_IN
{
	float4 position : SV_Position;
	float2 texcoord0 : TEXCOORD0_centroid;
};

struct PS_OUT
{
	float4 color : SV_Target0;
};
// *INDENT-ON*

// r_showDepth ( DBG_ShowDepth ): the depth source as linear view distance, a grey ramp of log2( distance )
// from 1 unit ( black ) to 65536 units ( white ), and the depth clear ( sky, nothing drawn ) in red.
// The decode is the generic one every depth reader uses, so it is right in both depth conventions.
void main( PS_IN fragment, out PS_OUT result )
{
	// any source size ( the probe capture is 256 x 256 ): the texel under this screen position, mip 0
	uint width, height;
	t_Depth.GetDimensions( width, height );
	int2 texel = min( int2( fragment.texcoord0 * float2( width, height ) ), int2( width, height ) - 1 );
	float depth = t_Depth.Load( int3( texel, 0 ) ).r;

	// test the clear before dividing: under reversed-Z it is at infinity
	if( depth == DEPTH_CONVENTION_CLEAR )
	{
		result.color = float4( 1.0, 0.0, 0.0, 1.0 );
		return;
	}

	// clipW = -P23 / ( -P22 - ndc.z ) is the view distance: zNear / ( 0.999 - z ) for the forward
	// projection, zNear / z for the reversed one
	float distance = -pc.rpProjectionMatrixZ.w / ( -pc.rpProjectionMatrixZ.z - depth );

	float grey = saturate( log2( max( distance, 1.0 ) ) * ( 1.0 / 16.0 ) );
	result.color = float4( grey, grey, grey, 1.0 );
}
