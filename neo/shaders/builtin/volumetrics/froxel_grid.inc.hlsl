// froxel_grid.inc.hlsl - what the froxel kernels share ( track V, package V1 ): the constants and the slices.
// The C++ mirror of froxelConstants_t is renderer/Passes/VolumetricFogPass_cb.h, lane for lane. The pass binds the
// constants at b1 of every kernel.

#ifndef FROXEL_GRID_INC_HLSL
#define FROXEL_GRID_INC_HLSL

#include "builtin/volumetrics/froxel_slices.inc.hlsl"

struct froxelConstants_t
{
	float4 gridSize;		// xyz = the columns, rows and slices the kernels fill: the view's grid and, where the volumes have them, one column and row more
	float4 sliceParms;		// x = S, y = ln( 1 + farEnd / S ) / slices, z = 1 / the store scale
	float4 rayParms;		// a column's ray: lateral = ( column + 0.5, row + 0.5 ) * xy - zw, forward 1
	float4 medium;			// the uniform medium: rgb = sigma_s * L, a = sigma_t, both times the store scale
};

cbuffer c_Froxel : register( b1 )
{
	froxelConstants_t froxel;
};

// A column's ray in the view frame, for a column coordinate ( column + 0.5 and row + 0.5 at a froxel's centre ): how
// far the ray moves sideways per unit forward. x grows to the RIGHT of the screen and y DOWN it ( volume UV is
// screen UV, row 0 is the top row ), so the ray's direction is forward + x * right - y * up
float2 FroxelColumnLateral( float2 columnCoord )
{
	return columnCoord * froxel.rayParms.xy - froxel.rayParms.zw;
}

// the far face of slice k of this frame's grid, in units along the view axis ( froxel_slices.inc.hlsl )
float FroxelSliceFar( float k )
{
	return FroxelSliceFarOf( k, froxel.sliceParms.x, froxel.sliceParms.y );
}

#endif // FROXEL_GRID_INC_HLSL
