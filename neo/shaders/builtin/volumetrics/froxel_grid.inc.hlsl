// froxel_grid.inc.hlsl - what the froxel kernels share ( track V, packages V1 and V2 ): the constants and the slices.
// The C++ mirror of froxelConstants_t is renderer/Passes/VolumetricFogPass_cb.h, lane for lane. The pass binds the
// constants at b1 of every kernel.

#ifndef FROXEL_GRID_INC_HLSL
#define FROXEL_GRID_INC_HLSL

#include "builtin/volumetrics/froxel_slices.inc.hlsl"

struct froxelConstants_t
{
	float4 gridSize;		// xyz = the columns, rows and slices the kernels fill: the view's grid and, where the volumes have them, one column and row more; w = the light records the inject reads ( 0: the fog is not lit )
	float4 sliceParms;		// x = S, y = ln( 1 + farEnd / S ) / slices, z = 1 / the store scale
	float4 rayParms;		// a column's ray: lateral = ( column + 0.5, row + 0.5 ) * xy - zw, forward 1
	float4 medium;			// the uniform medium: rgb = sigma_s * L of the furnace radiance, a = sigma_t, both times the store scale
	// V2: the inject's lanes
	float4 viewOrigin;		// xyz = the eye in the relative frame ( the light records' frame ), w = sigma_s times the store scale
	float4 viewForward;		// xyz = the view's forward axis, w = the phase function's g ( Henyey-Greenstein )
	float4 viewRight;		// xyz = its right axis, w = how the inject finds a froxel's records ( r_volumetricFogCull )
	float4 viewUp;			// xyz = its up axis, w = the bound of a stored coefficient
	float4 tileParms;		// the light lists' tiles: x = a tile's side in pixels, y = the tiles in a row, z = the rows, w = the words of a tile
	float4 binParms;		// x = the light lists' Z-bins per unit of view distance, y = the bin count, z = a column's side in pixels, w = the near plane's distance
	float4 ambient[9];		// rgb = the view's blended SH radiance, each band times the phase function's weight and the surfaces' ambient scale
	uint4 takeMask[8];		// a bit per light record, word w in [ w / 4 ][ w % 4 ]: set where the fog can take the record's light at all
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
