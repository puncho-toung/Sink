// froxel_columns.cs.hlsl - the froxel columns ( track V, package V3 ): for each column of the view's grid, the farthest
// distance along the view axis of the surfaces its pixels show. The inject skips the froxels that lie beyond the reach of
// a column and its neighbours ( r_volumetricFogSkip ): no pixel's lookup reads them.
//
// The depth is the one the apply reads ( the Hi-Z pyramid's mip 0, or the depth image where the pyramid is off ), with
// the apply's conversion to a distance. A pixel that shows sky reaches without bound: the apply fogs it as infinitely
// far ( the sky mask, D52 ); so does a pixel that shows no surface. One thread group per column: its threads share the
// column's pixels and take the largest distance together.

#include "vulkan.hlsli"
#include "builtin/volumetrics/froxel_grid.inc.hlsl"

// *INDENT-OFF*
Texture2D<float> t_SceneDepth : register( t0 );
Texture2D<float> t_SkyMask : register( t1 );
VK_IMAGE_FORMAT( "r32f" ) RWTexture2D<float> u_Columns : register( u0 );
// *INDENT-ON*

groupshared float s_Farthest[64];

[numthreads( 8, 8, 1 )]
void main( uint3 groupId : SV_GroupID, uint threadIndex : SV_GroupIndex )
{
	const uint side = uint( froxel.skipParms.y );
	const uint2 viewSize = uint2( froxel.skipParms.zw );
	float farthest = 0.0;
	for( uint i = threadIndex; i < side * side; i += 64 )
	{
		const uint2 inView = groupId.xy * side + uint2( i % side, i / side );
		if( all( inView < viewSize ) )
		{
			const int3 pixel = int3( int2( inView ) + int2( froxel.pixelParms.zw ), 0 );
			const float depth = t_SceneDepth.Load( pixel );
			const float denominator = depth + froxel.pixelParms.y;
			float distance = ( denominator * froxel.pixelParms.x > 0.0 ) ? froxel.pixelParms.x / denominator : 1e30;
			if( t_SkyMask.Load( pixel ) > 0.5 )
			{
				distance = 1e30;
			}
			farthest = max( farthest, distance );
		}
	}
	s_Farthest[ threadIndex ] = farthest;
	GroupMemoryBarrierWithGroupSync();
	for( uint stride = 32; stride > 0; stride >>= 1 )
	{
		if( threadIndex < stride )
		{
			s_Farthest[ threadIndex ] = max( s_Farthest[ threadIndex ], s_Farthest[ threadIndex + stride ] );
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if( threadIndex == 0 )
	{
		u_Columns[ groupId.xy ] = s_Farthest[0];
	}
}
