// froxel_fog.inc.hlsl - the one include every froxel-fogged pipeline uses ( D33, D83 ).
//
// The including shader must #define FROXEL_FOG_SET ( the index of the fog set in its own binding
// layout ) before including this file.
//
// VK_BINDING takes the FINAL Vulkan binding number, and the build shifts the register classes
// ( t +0, s +128, b +256 - compileshaders.cmake ), so b1 / t15 / s5 are written as 257 / 15 / 133.
// VK_DESCRIPTOR_SET must NOT be used with FROXEL_FOG_SET: it pastes its argument ( space##dset )
// and would emit the literal spaceFROXEL_FOG_SET instead of the macro's value.
//
// The engine matches a permutation by its exact define string, so a program row that wants the fog
// permutation passes USE_FROXEL_FOG=1 LAST and a row that does not passes no USE_FROXEL_FOG at all.

#ifndef FROXEL_FOG_INC_HLSL
#define FROXEL_FOG_INC_HLSL

#ifndef FROXEL_FOG_SET
#error froxel_fog.inc.hlsl: define FROXEL_FOG_SET ( the index of the fog set in the including shader's binding layout ) before the include
#endif

#include "vulkan.hlsli"
#include "builtin/volumetrics/froxel_slices.inc.hlsl"

// the C++ mirror is renderer/Passes/VolumetricFogPass_cb.h, lane for lane; all zeros mean "fog off"
struct froxelFogParms_t
{
	float4 volumeParms;		// xy = the fog view's screen UV to volume UV, z = the slice count G, w = 1 / G
	float4 depthParms;		// xy = the slices' bias S and step, as the kernels have them; zw = the projection's depth terms: distance = z / ( depth + w )
	float4 fogParms;		// x = 1 where the fog is on, y = the apply's debug view ( r_volumetricFogDebug ), zw = the fog view's first pixel
	float4 reserved;		// xy = 1 / the fog view's size in pixels, zw = the volume UV of the view's last column and row: the lookup stays inside the view's grid
	// V8 ( B2 ): where the view's bounded media can be seen. Zeros ( the fog-off parms ) test nothing
	float4 boundsRect;		// xy = the top-left, zw = the bottom-right corner, in the fog view's screen UV, of the rectangle outside which the lookup reads only clear air
	float4 boundsParms;		// x = 1 where the lookup tests the rectangle and the first slice, y = the slice coordinate nearer than which it reads only clear air, zw = 0
};

VK_BINDING( 257, FROXEL_FOG_SET )
cbuffer c_FroxelFogParms : register( b1 )
{
	froxelFogParms_t froxelFogParms;
};

VK_BINDING( 15, FROXEL_FOG_SET )
Texture3D<float4> t_FroxelVolume : register( t15 );

VK_BINDING( 133, FROXEL_FOG_SET )
SamplerState s_FroxelLinearClamp : register( s5 );

// the slice coordinate of a distance along the view axis: 0 at the camera, G at the volume's far end and beyond it
// ( froxel_slices.inc.hlsl )
float FroxelFogSlice( float viewDepth )
{
	return min( froxelFogParms.volumeParms.z, FroxelSliceOf( viewDepth, froxelFogParms.depthParms.x, froxelFogParms.depthParms.y ) );
}

// The distance along the view axis of a device depth ( a texel of the scene's depth, or a pixel's own SV_Position.z ),
// from the projection's two depth terms: z / ( depth + w ). Under reversed-Z that is zNear / depth, and a depth of
// 0, the clear value, is infinitely far; a forward-Z build has a negative z and reaches its far plane where
// depth + w is 0. Infinitely far comes back as 1e30
float FroxelFogViewDepth( float deviceDepth )
{
	const float denominator = deviceDepth + froxelFogParms.depthParms.w;
	return ( denominator * froxelFogParms.depthParms.z > 0.0 ) ? froxelFogParms.depthParms.z / denominator : 1e30;
}

// a pixel's place in the fog view, 0..1 with the origin top-left, from its SV_Position
float2 FroxelFogScreenUV( float2 svPosition )
{
	return ( svPosition - froxelFogParms.fogParms.zw ) * froxelFogParms.reserved.xy;
}

// The fog between the camera and a point: the in-scatter in rgb and the transmittance in a, so that the point's
// colour seen through the fog is rgb + colour * a. screenUV is the point's place in the fog view ( 0..1, top-left
// origin, y down ), viewDepth its distance along the view axis in units; beyond the volume's far end no fog is
// added. All zero parms mean "fog off": the function returns ( 0, 0, 0, 1 ) without a sample
float4 FroxelFogSample( float2 screenUV, float viewDepth )
{
	if( froxelFogParms.fogParms.x <= 0.0 )
	{
		return float4( 0.0, 0.0, 0.0, 1.0 );
	}
	// V8 ( B2 ): beside every bounded medium of the view, or in front of the nearest, the lookup would read only clear
	// columns or slices, which integrate to exactly ( 0, 0, 0, 1 ): it is not made. The rectangle holds the columns and rows
	// a medium's centre can be in and two more on each side: floor and ceil there already keep the lookup's footprint ( a
	// column and a row on each side ) off them, so the two are margin ( review O1-4 ); the first slice is the box's, less one
	// ( the lookup reads no texel beyond the point's own slice )
	const bool bounded = froxelFogParms.boundsParms.x > 0.0;
	if( bounded && ( any( screenUV < froxelFogParms.boundsRect.xy ) || any( screenUV > froxelFogParms.boundsRect.zw ) ) )
	{
		return float4( 0.0, 0.0, 0.0, 1.0 );
	}

	// Texel k of the volume holds the fog from the camera to the FAR face of slice k, and its centre is at
	// ( k + 0.5 ) / G. The fog to slice coordinate s is therefore read half a texel nearer, at ( s - 0.5 ) / G: at
	// s = k that is texel k - 1 alone, the fog to the face the point lies on. Read at s / G it would be half a
	// slice too much: light in front of walls. In front of the first far face the volume has no texel: there the
	// fog runs from none at the camera to texel 0
	// Sideways the lookup stays inside the view's grid: the volume may hold more columns and rows than this view
	// fills, and nothing wrote those. At the view's last column and row this is what the sampler's clamp does at the
	// edge of a volume that is the view's size
	const float s = FroxelFogSlice( viewDepth );
	if( bounded && s < froxelFogParms.boundsParms.y )
	{
		return float4( 0.0, 0.0, 0.0, 1.0 );
	}
	const float3 uvw = float3( min( screenUV * froxelFogParms.volumeParms.xy, froxelFogParms.reserved.zw ), ( max( s, 1.0 ) - 0.5 ) * froxelFogParms.volumeParms.w );
	const float4 fog = t_FroxelVolume.SampleLevel( s_FroxelLinearClamp, uvw, 0.0 );
	return lerp( float4( 0.0, 0.0, 0.0, 1.0 ), fog, saturate( s ) );
}

// V5 ( D20 ): a translucent stage's colour ( linear ) seen through the fog between the camera and its own surface, by
// the kind of blend the stage draws with ( RenderBackend.cpp, FroxelFogBlendKind ); fog is FroxelFogSample's. The
// picture behind the stage was fogged by the apply to its own depth: what the blend keeps of it keeps that fog, and
// what the stage adds is seen through the fog to the stage.
//   0 adds: rgb T ( exact where the blend's source factor reads no colour; otherwise, and for a darkening ZERO,
//     ONE_MINUS_SRC_COLOR, the stage fades out in the fog: not exact )
//   1 lays over ( SRC_ALPHA, ONE_MINUS_SRC_ALPHA, or replaces ): rgb T + the in-scatter ( exact )
//   2 lays over premultiplied ( ONE, ONE_MINUS_SRC_ALPHA ): rgb T + the in-scatter times alpha ( exact )
//   3 filters ( the destination times rgb ): rgb T + 1 - T, the filter fading out in the fog. Not exact: the exact
//     filter darkens only the light behind the stage and keeps the in-scatter in front of it, which a multiplying
//     blend cannot
//   4 filters twice ( the destination times 2 rgb ): rgb T + ( 1 - T ) / 2, the same
// alpha stays as the stage made it: the blend's factors read it. Fog off ( T 1, no in-scatter ) returns the colour
float4 FroxelFogTranslucent( float4 color, float4 fog, float kind )
{
	const int k = ( int )( kind + 0.5 );
	float3 rgb = color.rgb * fog.a;
	if( k == 1 )
	{
		rgb += fog.rgb;
	}
	else if( k == 2 )
	{
		rgb += fog.rgb * color.a;
	}
	else if( k == 3 )
	{
		rgb += 1.0 - fog.a;
	}
	else if( k == 4 )
	{
		rgb += 0.5 * ( 1.0 - fog.a );
	}
	return float4( rgb, color.a );
}

// the same for a pixel of the fog view, from its SV_Position: what a fogged pixel shader calls
float4 FroxelFogSamplePixel( float2 svPosition, float viewDepth )
{
	return FroxelFogSample( FroxelFogScreenUV( svPosition ), viewDepth );
}

#endif // FROXEL_FOG_INC_HLSL
