// froxel_apply.ps.hlsl - pixel stage of the froxel fog apply ( track V, package V1 ): the fog between the camera and
// the opaque surface of each pixel of the fog view. It returns the in-scatter in rgb and the transmittance in a; the
// pass blends source * ONE + destination * SRC_ALPHA, which is picture = in-scatter + picture * transmittance.

// the fog set is this shader's only set: its three items, the scene depth and the sky mask
#define FROXEL_FOG_SET 0
#include "froxel_fog.inc.hlsl"

// *INDENT-OFF*
Texture2D<float> t_SceneDepth : register( t0 );		// mip 0 of the Hi-Z pyramid; the depth image where the pyramid is off
Texture2D<float> t_SkyMask : register( t1 );		// 1 where the view shows a sky surface

struct PS_IN
{
	float4 posClip	: SV_Position;
	float2 uv		: TEXCOORD0;
};
// *INDENT-ON*

float4 main( PS_IN fragment ) : SV_Target
{
	const int3 pixel = int3( int2( fragment.posClip.xy ), 0 );
	const float depth = t_SceneDepth.Load( pixel ).r;

	float viewDepth = FroxelFogViewDepth( depth );
	// a sky surface writes depth, but what it shows is infinitely far ( the pass's sky mask )
	if( t_SkyMask.Load( pixel ).r > 0.5 )
	{
		viewDepth = 1e30;
	}

	const float4 fog = FroxelFogSamplePixel( fragment.posClip.xy, viewDepth );

	// the debug views replace the picture: their alpha, the factor of the destination, is 0
	if( froxelFogParms.fogParms.y == 1.0 )
	{
		// the transmittance to the pixel as grey
		return float4( fog.a, fog.a, fog.a, 0.0 );
	}
	if( froxelFogParms.fogParms.y == 2.0 )
	{
		// The slices as bands, every eighth one red, over the volume's columns as a checkerboard. The bands show the
		// depth distribution. The checkerboard is of the volume's own texels, found the way the lookup finds them
		// ( the fog view's screen UV times volumeParms.xy, times the volume's size ): its squares are the cvar's
		// number of pixels wide from the view's first pixel on only if the volume and that scale are what the pass
		// says they are
		uint volumeWidth, volumeHeight, volumeDepth;
		t_FroxelVolume.GetDimensions( volumeWidth, volumeHeight, volumeDepth );
		const float slice = floor( FroxelFogSlice( viewDepth ) );
		const float2 column = floor( FroxelFogScreenUV( fragment.posClip.xy ) * froxelFogParms.volumeParms.xy * float2( volumeWidth, volumeHeight ) );
		const float band = ( fmod( slice, 2.0 ) < 1.0 ) ? 0.5 : 0.2;
		const float checker = ( fmod( column.x + column.y, 2.0 ) < 1.0 ) ? 1.0 : 0.6;
		const float3 tint = ( fmod( slice, 8.0 ) < 1.0 ) ? float3( 1.0, 0.25, 0.25 ) : float3( 1.0, 1.0, 1.0 );
		return float4( tint * ( band * checker ), 0.0 );
	}
	return fog;
}
