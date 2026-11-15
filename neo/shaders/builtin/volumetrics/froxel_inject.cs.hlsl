// froxel_inject.cs.hlsl - the froxel inject pass ( D33 ): X0 landed this as a stub that compiles and
// does nothing; track V owns this file and writes the inject compute there ( V1-V5 ).

#include "vulkan.hlsli"

[numthreads( 8, 8, 1 )] void main( uint3 globalIdx : SV_DispatchThreadID )
{
	// X0 stub: nothing is injected yet.
}
