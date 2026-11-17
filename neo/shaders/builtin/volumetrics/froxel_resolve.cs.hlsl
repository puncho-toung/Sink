// froxel_resolve.cs.hlsl - the froxel resolve ( track V ): this frame's injected volume into the resolved one.
// V1: a copy; the history comes with V3.

#include "vulkan.hlsli"
#include "builtin/volumetrics/froxel_grid.inc.hlsl"

// *INDENT-OFF*
Texture3D<float4> t_Injected : register( t0 );
VK_IMAGE_FORMAT( "rgba16f" ) RWTexture3D<float4> u_Resolved : register( u0 );
// *INDENT-ON*

[numthreads( 8, 8, 1 )]
void main( uint3 froxelId : SV_DispatchThreadID )
{
	if( any( froxelId >= uint3( froxel.gridSize.xyz ) ) )
	{
		return;
	}

	u_Resolved[ froxelId ] = t_Injected[ froxelId ];
}
