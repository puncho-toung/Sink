// froxel_media.cs.hlsl - the froxel media pass ( track V, package V4; the base of track M ): sigma_s and sigma_t of every
// froxel the inject fills, the global fog's constant plus the view's media ( the height fog, the dust, the test box;
// track M's volumes later ), into the media volume that the inject reads where it read the global fog's constant.
//
// The sample is the froxel's centre ( V4g: not the inject's sample, which the frame's jitter moves: the jitter is one offset
// for the whole screen and the history does not average sigma_t, so a medium taken there moved the view's T from frame
// to frame, review F1-1 ). Each thread group tests each medium's
// bounding sphere against its samples once ( the answer is the same for every thread of the group, so the branch splits
// no wave ), and the media left are summed in registers: overlaps add, in no order and with no atomics. A sum past the
// store's bound ( the half floats' end ) is scaled down whole, so that the medium keeps its albedo: the froxel's sigma_t is
// capped at the bound over the store scale, about 58 per unit, opaque in a fraction of a unit. A froxel the inject skips
// ( no pixel's lookup reaches it ) is not written: nothing reads it.
// The pass is not issued while the view has no medium but the global fog ( and the fog lights, which the inject adds ).

#include "vulkan.hlsli"
#include "builtin/volumetrics/froxel_grid.inc.hlsl"
#include "builtin/volumetrics/froxel_media.inc.hlsl"

// *INDENT-OFF*
StructuredBuffer<fogMedium_t> t_Media : register( t0 );	// the view's media ( mediaParms.w of them )
Texture2D<float> t_Columns : register( t1 );				// the farthest surface of each column ( froxel_columns.cs.hlsl )
VK_IMAGE_FORMAT( "rgba16f" ) RWTexture3D<float4> u_Media : register( u0 );
// *INDENT-ON*

[numthreads( 8, 8, 1 )]
void main( uint3 froxelId : SV_DispatchThreadID, uint3 groupId : SV_GroupID )
{
	if( any( froxelId >= uint3( froxel.gridSize.xyz ) ) )
	{
		return;
	}

	// the global fog: grey sigma_s and sigma_t, the constant the inject reads where this pass does not run
	float4 medium = float4( froxel.viewOrigin.www, froxel.medium.a );
	if( froxel.skipParms.x > 0.0 && float( froxelId.z ) > FroxelColumnReach( t_Columns, froxelId.xy ) )
	{
		return;		// nothing reads a skipped froxel's medium ( review O2-5 )
	}

	// the froxel's centre ( one slice is one depth: the same for the whole thread group; V4g, review F1-1 )
	const float2 lateral = FroxelColumnLateral( float2( froxelId.xy ) + 0.5 );
	const float3 ray = froxel.viewForward.xyz + lateral.x * froxel.viewRight.xyz - lateral.y * froxel.viewUp.xyz;
	const float viewDepth = FroxelSliceFar( float( froxelId.z ) - 0.5 );
	const float3 position = froxel.viewOrigin.xyz + viewDepth * ray;
	// the froxel's width across the view at its centre: the noise slots leave out the octaves finer than two of it
	const float width = viewDepth * max( froxel.rayParms.x, froxel.rayParms.y );

	const uint count = uint( froxel.mediaParms.w );
	for( uint i = 0; i < count; i++ )
	{
		if( FroxelGroupMissesSphere( t_Media[ i ].sphere, groupId.xy, viewDepth, float2( 0.0, 0.0 ) ) )
		{
			continue;
		}
		const fogMedium_t m = t_Media[ i ];
		medium += m.sigma * FroxelMediumDensity( m, position, width );
	}

	// the store's bound: a sum past it is scaled down whole, so that the medium keeps its albedo
	const float largest = max( medium.a, max( medium.r, max( medium.g, medium.b ) ) );
	u_Media[ froxelId ] = medium * min( 1.0, froxel.viewUp.w / max( largest, 1e-20 ) );
}
