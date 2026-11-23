// froxel_grid.inc.hlsl - what the froxel kernels share ( track V, packages V1 and V2 ): the constants and the slices.
// The C++ mirror of froxelConstants_t is renderer/Passes/VolumetricFogPass_cb.h, lane for lane. The pass binds the
// constants at b1 of every kernel.

#ifndef FROXEL_GRID_INC_HLSL
#define FROXEL_GRID_INC_HLSL

#include "builtin/volumetrics/froxel_slices.inc.hlsl"

struct froxelConstants_t
{
	float4 gridSize;		// xyz = the columns, rows and slices the kernels fill: the view's grid and, where the volumes have them, one column and row more; w = the light records the inject reads ( 0: the fog is not lit )
	float4 sliceParms;		// x = S, y = ln( 1 + farEnd / S ) / slices, z = 1 / the store scale, w = the history's sigma_t times it ( V3 )
	float4 rayParms;		// a column's ray: lateral = ( column + 0.5, row + 0.5 ) * xy - zw, forward 1
	float4 medium;			// the uniform medium: rgb = sigma_s * L of the furnace radiance, a = sigma_t, both times the store scale
	// V2: the inject's lanes
	float4 viewOrigin;		// xyz = the eye in the relative frame ( the light records' frame ), w = sigma_s times the store scale
	float4 viewForward;		// xyz = the view's forward axis, w = the phase function's g ( Henyey-Greenstein )
	float4 viewRight;		// xyz = its right axis, w = how the inject finds a froxel's records ( r_volumetricFogCull )
	float4 viewUp;			// xyz = its up axis, w = the bound of a stored coefficient
	float4 tileParms;		// the light lists' tiles: x = a tile's side in pixels, y = the tiles in a row, z = the rows, w = the words of a tile
	float4 binParms;		// x = the light lists' Z-bins per unit of view distance, y = the bin count, z = a column's side in pixels, w = the near plane's distance
	float4 ambient[9];		// rgb = the view's blended SH radiance, each band times the phase function's weight and the surfaces' ambient scale; ambient[0].w = one texel of the shadow atlas ( V3 )
	// V3: the shadows
	float4 shadowParms;		// x = the sun's depth bias scale, y = every other light's ( the legacy pass's: a receiver's depth times it ), z = a parallel light's rows ( its cascades: every parallel light's are the view's splits ), w = 1 where the inject looks shadows up ( r_volumetricFogShadows )
	// V3: the history
	float4 jitter;			// xyz = this frame's sample in its froxel ( columns, rows, slices: -0.5 .. 0.5 ), w = 1 where the history holds last frame's fog
	float4 historyParms;	// x = the history's weight, y = its weight where it is clipped, z = the colour change that clips ( a share of the larger colour ), w = the lights that vanished since last frame
	float4 historyGrid;		// xy = a screen UV to the volume UV of the view's grid, z = 1 / the slices, w = the records of last frame
	float4 prevOrigin;		// xyz = this frame's render origin minus last frame's ( a point of this frame's relative frame plus it is one of last frame's ), w = the lights that moved
	float4 prevMVP[4];		// last frame's unjittered MVP, relative to its render origin ( H10 ): the rows of an idRenderMatrix
	float4 skipParms;		// x = 1 where the inject skips the froxels no pixel's lookup reaches, y = a column's side in pixels, zw = the view's size in pixels
	float4 pixelParms;		// xy = the projection's depth terms ( distance = x / ( depth + y ) ), zw = the view's first pixel in the render target
	uint4 takeMask[8];		// a bit per light record, word w in [ w / 4 ][ w % 4 ]: set where the fog can take the record's light at all
	// V4: the media
	float4 mediaParms;		// x = 1 where the inject reads the media volume ( the media pass ran ), y = the furnace radiance ( a test input ), z = 1 on V4's path ( r_volumetricFogMedia: the resolve's medium per froxel and the mark volume ), w = the media in the view's list
	float4 mediaParms2;		// x = the media that moved, came or went ( four boxes each in t_MediaMoved ), y = the fog lights' rate times the store scale, z = 1 / the fog lights' enter ramp, w = 1 where the inject takes fog lights ( V4b )
	// V8: the fog's cost follows its volumes' bounds ( r_volumetricFogBounds )
	float4 boundsParms;		// x = 1 where a froxel with no medium returns early in the resolve and the inject runs its permutation FROXEL_CLEAR_AIR 1 ( B1: the view's media are its fog lights alone, V4's path ), yzw = 0
	// V8 ( B4 ): the box of the view's bounded media in froxels, padded by one ( the pass's BoundsBox )
	float4 boxMin;			// xyz = its first column, row and slice, w = 1 where the inject runs over it alone and the resolve and the integrate skip its outside
	float4 boxMax;			// xyz = its last column, row and slice ( inclusive ), w = 0
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

// the centre of a froxel in the relative frame: its column's centre, half way through its slice in the slice coordinate
// ( V3: the inject's sample is this moved by the frame's jitter; the resolve reprojects the centre; V4g: the media pass and
// the fog lights take the medium here, review F1-1 )
float3 FroxelCentre( uint3 froxelId )
{
	const float2 lateral = FroxelColumnLateral( float2( froxelId.xy ) + 0.5 );
	const float3 ray = froxel.viewForward.xyz + lateral.x * froxel.viewRight.xyz - lateral.y * froxel.viewUp.xyz;
	return froxel.viewOrigin.xyz + FroxelSliceFar( float( froxelId.z ) - 0.5 ) * ray;
}

// Whether a sphere ( xyz = its centre in the relative frame, w = 1 / r^2 ) misses every sample of a thread group of the
// inject or the media pass. The group's samples lie in one plane across the view axis ( one slice, one depth ), in the
// rectangle its 8 x 8 column centres span. A sphere with w = 0 ( a parallel light, an unbounded medium ) is never missed.
// Every thread of the group computes the same answer ( V4: the inject's test of a record, shared with the media pass ).
// offset is the samples' place in their columns: the frame's jitter for the inject's lights, 0 for the froxels' centres
// ( V4g: where the media pass and the fog lights take the medium, review F1-1 )
bool FroxelGroupMissesSphere( float4 sphere, uint2 groupId, float viewDepth, float2 offset )
{
	const float3 d = sphere.xyz - froxel.viewOrigin.xyz;
	// the sphere's centre in the view frame: right, down, forward
	const float3 centre = float3( dot( d, froxel.viewRight.xyz ), -dot( d, froxel.viewUp.xyz ), dot( d, froxel.viewForward.xyz ) );
	const float2 first = FroxelColumnLateral( float2( groupId ) * 8.0 + 0.5 + offset ) * viewDepth;
	const float2 last = FroxelColumnLateral( float2( groupId ) * 8.0 + 7.5 + offset ) * viewDepth;
	const float2 gap = max( max( first - centre.xy, centre.xy - last ), 0.0 );
	const float along = centre.z - viewDepth;
	return ( dot( gap, gap ) + along * along ) * sphere.w > 1.0;
}

// V8 ( B4 ): whether the kernels follow the box of the view's bounded media and a froxel lies outside it ( the resolve's test;
// the inject's dispatch origin and the integrate's read range spell the same box: keep the three in step, review CN4-5 )
bool FroxelOutsideBox( uint3 froxelId )
{
	return froxel.boxMin.w > 0.0 && ( any( float3( froxelId ) < froxel.boxMin.xyz ) || any( float3( froxelId ) > froxel.boxMax.xyz ) );
}

// V3: the last slice a pixel's lookup reads in a froxel's column: the farthest surface of the column and its eight
// neighbours ( the apply's filter reads the neighbouring columns ), and one slice more ( it reads the two slices round a
// pixel's depth ). columns is the columns kernel's texture ( froxel_columns.cs.hlsl; V4: shared by the inject and the
// media pass )
float FroxelColumnReach( Texture2D<float> columns, uint2 column )
{
	float farthest = 0.0;
	for( int dy = -1; dy <= 1; dy++ )
	{
		for( int dx = -1; dx <= 1; dx++ )
		{
			const int2 c = clamp( int2( column ) + int2( dx, dy ), int2( 0, 0 ), int2( froxel.gridSize.xy ) - 1 );
			farthest = max( farthest, columns.Load( int3( c, 0 ) ) );
		}
	}
	return ceil( FroxelSliceOf( farthest, froxel.sliceParms.x, froxel.sliceParms.y ) - 0.5 ) + 1.0;
}

#endif // FROXEL_GRID_INC_HLSL
