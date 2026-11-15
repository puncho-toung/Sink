// froxel_resolve.cs.hlsl - the froxel resolve pass ( D33 ): X0 landed this as a stub that compiles and
// does nothing; track V owns this file and writes the resolve compute there ( V1-V5 ).

#include "vulkan.hlsli"

[numthreads( 8, 8, 1 )] void main( uint3 globalIdx : SV_DispatchThreadID )
{
	// X0 stub: nothing is resolved yet.
}
