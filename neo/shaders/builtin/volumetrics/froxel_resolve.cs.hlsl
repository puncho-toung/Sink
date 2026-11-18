// froxel_resolve.cs.hlsl - the froxel resolve ( track V ): this frame's injected volume and last frame's resolved one
// into this frame's resolved volume, the fog's history ( V3; in V1 a copy ).
//
// The rule is the one tools/harness/temporal_harness.py of the V3 run chose ( six scenarios of the plan and two more ):
// the froxel's centre is reprojected into last frame's view with last frame's unjittered MVP and render origin ( H10 ),
// last frame's fog there is read, the light change the inject wrote is added to it, and the result is blended with
// this frame's sample: weight historyParms.x, historyParms.y where the inject asked for a clip ( then the history is
// first clamped to the range of this frame's injected values round the froxel ), and 0 where the history is not valid
// ( a camera cut, another grid, the engine's last-frame state not the history's view's ), where the point was outside
// last frame's view or volume, or inside the
// last volume of a light that vanished. A history value that is not finite is not read, nor one that touched a froxel
// the inject skipped.

#include "vulkan.hlsli"
#include "builtin/volumetrics/froxel_grid.inc.hlsl"

// *INDENT-OFF*
Texture3D<float4> t_Injected : register( t0 );
Texture3D<float4> t_State : register( t1 );		// rgb = the light change, a = 1: clip the history, 2: do not read it
Texture3D<float4> t_History : register( t2 );		// last frame's resolved volume
SamplerState s_History : register( s0 );			// linear, clamped
VK_IMAGE_FORMAT( "rgba16f" ) RWTexture3D<float4> u_Resolved : register( u0 );
// *INDENT-ON*

[numthreads( 8, 8, 1 )]
void main( uint3 froxelId : SV_DispatchThreadID )
{
	const uint3 grid = uint3( froxel.gridSize.xyz );
	if( any( froxelId >= grid ) )
	{
		return;
	}

	const float4 injected = t_Injected[ froxelId ];
	float4 resolved = injected;
	const float4 state = t_State[ froxelId ];
	// a skipped froxel ( state 3 ): no pixel reads it now, and the mark ( alpha minus this frame's sigma_t, which the
	// integrate reads as clear air ) tells a later history read how much of it fell on a skipped froxel
	if( state.a > 2.5 )
	{
		u_Resolved[ froxelId ] = float4( 0.0, 0.0, 0.0, -injected.a );
		return;
	}
	if( froxel.jitter.w > 0.0 && state.a < 1.5 )
	{
		// the froxel's centre in last frame's clip space: last frame's relative frame is this one's plus prevOrigin
		const float4 P = float4( FroxelCentre( froxelId ) + froxel.prevOrigin.xyz, 1.0 );
		const float4 clipPos = float4( dot( P, froxel.prevMVP[0] ), dot( P, froxel.prevMVP[1] ), dot( P, froxel.prevMVP[2] ), dot( P, froxel.prevMVP[3] ) );
		if( clipPos.w > 1e-3 )
		{
			// screen UV with the top-left origin, y down ( the motion vectors' convention ), and the slice coordinate of
			// the point's distance along last frame's view axis ( clip w )
			const float2 screenUV = float2( 0.5 + 0.5 * clipPos.x / clipPos.w, 0.5 - 0.5 * clipPos.y / clipPos.w );
			const float slice = FroxelSliceOf( clipPos.w, froxel.sliceParms.x, froxel.sliceParms.y );
			if( all( screenUV >= 0.0 ) && all( screenUV <= 1.0 ) && slice <= froxel.gridSize.z )
			{
				// a reprojection that lands on a texel's centre ( a still camera, but for rounding ) reads that texel
				// alone: the trilinear weights of the rounding would mix in a little of the neighbours ( a skipped
				// froxel's mark among them ) and blur the history of a still view
				const float3 uvw = float3( screenUV * froxel.historyGrid.xy, slice * froxel.historyGrid.z );
				uint3 size;
				t_History.GetDimensions( size.x, size.y, size.z );
				const float3 texel = uvw * float3( size ) - 0.5;
				const float3 nearest = clamp( round( texel ), float3( 0.0, 0.0, 0.0 ), float3( size ) - 1.0 );
				float4 history = all( abs( texel - nearest ) < 1e-3 ) ? t_History.Load( int4( int3( nearest ), 0 ) ) : t_History.SampleLevel( s_History, uvw, 0.0 );
				// a history read that touched skipped froxels mixes their marks in: a mark's alpha is minus last frame's
				// sigma_t and every other texel's plus it ( a uniform medium: sliceParms.w ), so the read's alpha gives
				// the share of its weight on froxels that were not skipped, ( sigma_t + a ) / ( 2 sigma_t ), and their
				// in-scatter is the read's over that share. A read that is more mark than fog is not used
				const float prevSigma = froxel.sliceParms.w;
				const float fogShare = ( prevSigma > 0.0 ) ? ( prevSigma + history.a ) / ( 2.0 * prevSigma ) : 0.0;
				if( all( abs( history ) < 1e30 ) && fogShare >= 0.5 )
				{
					// the history's in-scatter is of last frame's medium: it goes with sigma_s, sigma_t times the albedo,
					// so this frame's is it times the ratio of the sigma_t
					history.rgb = history.rgb / min( fogShare, 1.0 ) * ( injected.a / prevSigma ) + state.rgb;
					float weight = froxel.historyParms.x;
					if( state.a > 0.5 )
					{
						// this frame's injected values round the froxel: its six neighbours, inside the grid
						float3 lo = injected.rgb;
						float3 hi = injected.rgb;
						const int3 steps[6] = { int3( -1, 0, 0 ), int3( 1, 0, 0 ), int3( 0, -1, 0 ), int3( 0, 1, 0 ), int3( 0, 0, -1 ), int3( 0, 0, 1 ) };
						for( int i = 0; i < 6; i++ )
						{
							const int3 n = clamp( int3( froxelId ) + steps[i], int3( 0, 0, 0 ), int3( grid ) - 1 );
							const float3 v = t_Injected[ uint3( n ) ].rgb;
							lo = min( lo, v );
							hi = max( hi, v );
						}
						history.rgb = clamp( history.rgb, lo, hi );
						weight = froxel.historyParms.y;
					}
					// the in-scatter is blended and kept inside the store's bound ( the inject bounds the history's change
					// and this frame's value each, not their sum ); sigma_t is this frame's medium
					resolved.rgb = clamp( lerp( injected.rgb, history.rgb, weight ), 0.0, froxel.viewUp.w );
				}
			}
		}
	}
	u_Resolved[ froxelId ] = resolved;
}
