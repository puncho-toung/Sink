// froxel_inject.cs.hlsl - the froxel inject ( track V ): what the air of each froxel scatters toward the camera and
// how much it takes out of a ray, sigma_s * L in rgb and sigma_t in a, both times the store scale.
// V1: the medium is the same everywhere ( the global fog under the furnace radiance ); the lights come with V2.

#include "vulkan.hlsli"
#include "builtin/volumetrics/froxel_grid.inc.hlsl"

// *INDENT-OFF*
VK_IMAGE_FORMAT( "rgba16f" ) RWTexture3D<float4> u_Injected : register( u0 );
// *INDENT-ON*

[numthreads( 8, 8, 1 )]
void main( uint3 froxelId : SV_DispatchThreadID )
{
	if( any( froxelId >= uint3( froxel.gridSize.xyz ) ) )
	{
		return;
	}

	u_Injected[ froxelId ] = froxel.medium;
}
