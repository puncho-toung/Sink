// froxel_integrate.cs.hlsl - the froxel integrate ( track V ): one thread per column walks the slices from the
// camera outward and writes, per slice, the light the air adds in front of the slice's FAR face ( rgb ) and the share
// of a surface there that survives ( a ).
//
// Along the ray, not along the view axis: a slice of thickness dz is crossed over dz * | dir |, where dir is the
// column's ray with forward component 1. Each slab is integrated in closed form: with a constant sigma_t and a
// constant in-scatter S per unit, a slab of length l adds S * ( 1 - exp( -sigma_t * l ) ) / sigma_t behind the
// transmittance in front of it.

#include "vulkan.hlsli"
#include "builtin/volumetrics/froxel_grid.inc.hlsl"

// *INDENT-OFF*
Texture3D<float4> t_Resolved : register( t0 );
VK_IMAGE_FORMAT( "rgba16f" ) RWTexture3D<float4> u_Integrated : register( u0 );
// *INDENT-ON*

[numthreads( 8, 8, 1 )]
void main( uint3 columnId : SV_DispatchThreadID )
{
	const uint3 grid = uint3( froxel.gridSize.xyz );
	if( columnId.x >= grid.x || columnId.y >= grid.y )
	{
		return;
	}

	const float2 lateral = FroxelColumnLateral( float2( columnId.xy ) + 0.5 );
	const float rayLength = sqrt( 1.0 + dot( lateral, lateral ) );

	// V8 ( B4 ): outside the box of the view's bounded media every froxel holds clear air ( the resolve wrote it ), so only the
	// box's slices of the box's columns are read; an empty range where the column lies beside the box
	const bool boxed = froxel.boxMin.w > 0.0;
	const bool besideBox = boxed && ( any( float2( columnId.xy ) < froxel.boxMin.xy ) || any( float2( columnId.xy ) > froxel.boxMax.xy ) );
	const uint readFirst = besideBox ? grid.z : ( boxed ? uint( froxel.boxMin.z ) : 0 );
	const uint readLast = boxed ? uint( froxel.boxMax.z ) : grid.z - 1;

	float3 inScatter = float3( 0.0, 0.0, 0.0 );
	float transmittance = 1.0;
	float nearFace = 0.0;
	// V8f ( review O1-1 ): three loops, not a branch per slice ( which cost the views that are not bounded 1 to 2 us ). A clear
	// slab adds nothing ( + 0 ) and passes everything ( exp( -0 ) = 1 ), so in front of the box the slices hold ( 0, 0, 0, 1 )
	// and behind it the box's last sums; the loop between is the loop before V8, the whole of it where the view is not bounded
	uint k = 0;
	for( ; k < min( readFirst, grid.z ); k++ )
	{
		u_Integrated[ uint3( columnId.xy, k ) ] = float4( 0.0, 0.0, 0.0, 1.0 );
	}
	if( k > 0 )
	{
		nearFace = FroxelSliceFar( float( k - 1 ) );
	}
	for( ; k <= readLast; k++ )
	{
		// sigma_s * L and sigma_t, per unit
		const float4 slice = t_Resolved[ uint3( columnId.xy, k ) ] * froxel.sliceParms.z;

		const float farFace = FroxelSliceFar( float( k ) );
		const float slabLength = ( farFace - nearFace ) * rayLength;
		nearFace = farFace;

		const float sigmaT = max( slice.a, 0.0 );
		const float tau = sigmaT * slabLength;
		const float slabTransmittance = exp( -tau );
		// ( 1 - exp( -tau ) ) / sigma_t, which goes to the slab's length where the air is clear: three terms of the
		// series there, where the quotient loses its digits
		const float slabWeight = ( tau > 1e-2 ) ? ( 1.0 - slabTransmittance ) / sigmaT : slabLength * ( 1.0 - tau * ( 0.5 - tau / 6.0 ) );

		inScatter += transmittance * slice.rgb * slabWeight;
		transmittance *= slabTransmittance;

		u_Integrated[ uint3( columnId.xy, k ) ] = float4( inScatter, transmittance );
	}
	for( ; k < grid.z; k++ )
	{
		u_Integrated[ uint3( columnId.xy, k ) ] = float4( inScatter, transmittance );
	}
}
