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

struct froxelFogParms_t
{
	float4 volumeParms;
	float4 depthParms;
	float4 fogParms;
	float4 reserved;
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

// in-scatter in rgb, transmittance in a; all zero parms mean "fog off" and the function returns ( 0, 0, 0, 1 )
float4 FroxelFogSample( float2 screenUV, float viewDepth )
{
	// X0 stub: track V writes the real sample here ( V5 ). Nothing is sampled yet, so the fog is off.
	return float4( 0.0, 0.0, 0.0, 1.0 );
}

#endif // FROXEL_FOG_INC_HLSL
