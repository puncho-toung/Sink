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
Texture3D<float> t_PrevMark : register( t3 );		// V4: last frame's marks: 1 where the inject skipped the froxel, in step with t_History
VK_IMAGE_FORMAT( "r8" ) RWTexture3D<float> u_Mark : register( u1 );		// V4: this frame's
Texture2D<float> t_Columns : register( t4 );		// V8 ( B4 ): the farthest surface of each column ( froxel_columns.cs.hlsl )
// *INDENT-ON*

// V4: on V4's path a history read is not used where last frame's sigma_t there ( times the store scale ) was below this:
// in clear air the history's light per unit of medium is not known
static const float FROXEL_HISTORY_MIN_SIGMA = 1e-4;

[numthreads( 8, 8, 1 )]
void main( uint3 froxelId : SV_DispatchThreadID )
{
	const uint3 grid = uint3( froxel.gridSize.xyz );
	if( any( froxelId >= grid ) )
	{
		return;
	}

	// V8 ( B4 ): outside the box of the view's bounded media the inject did not run this frame: clear air there, and the mark of
	// the inject's own skip test ( state 3 ), which are what the whole work gives ( a skipped froxel is marked and holds clear air
	// on V4's path; clear air resolves to clear air whatever its history holds ). Nothing is read but the columns
	if( FroxelOutsideBox( froxelId ) )
	{
		const bool skipped = froxel.skipParms.x > 0.0 && float( froxelId.z ) > FroxelColumnReach( t_Columns, froxelId.xy );
		u_Mark[ froxelId ] = skipped ? 1.0 : 0.0;
		u_Resolved[ froxelId ] = float4( 0.0, 0.0, 0.0, 0.0 );
		return;
	}
	const float4 injected = t_Injected[ froxelId ];
	float4 resolved = injected;
	const float4 state = t_State[ froxelId ];
	// a skipped froxel ( state 3 ): no pixel reads it now, and a mark tells a later history read how much of it fell on a
	// skipped froxel. On V3's path the mark is the alpha, minus this frame's sigma_t, which the integrate reads as clear air
	// V4: on V4's path ( r_volumetricFogMedia ) the mark goes into the mark volume instead and the skipped froxel holds
	// clear air: the medium is no longer one number that the sign of alpha could stand for
	const bool mediaPath = froxel.mediaParms.z > 0.0;
	if( mediaPath )
	{
		u_Mark[ froxelId ] = ( state.a > 2.5 ) ? 1.0 : 0.0;
	}
	if( state.a > 2.5 )
	{
		u_Resolved[ froxelId ] = mediaPath ? float4( 0.0, 0.0, 0.0, 0.0 ) : float4( 0.0, 0.0, 0.0, -injected.a );
		return;
	}
	// V8 ( B1 ): clear air ( r_volumetricFogBounds, where the view's media are its fog lights alone, on V4's path ). Where nothing
	// was injected this frame the result is the injected zeros whatever the history holds: its light is carried per unit of
	// this frame's sigma_t ( 0 ), and the inject's light change is 0 where nothing scatters or glows; the clip's range starts
	// at 0. So the reprojection and the history read are skipped; the mark above is 0 here either way. It leans on
	// sigma_s <= sigma_t in every medium ( review CN1-2: the inject's own return is then the one way a froxel stores
	// ( 0, 0, 0, 0 ) but the half store's flush of a sigma_t below 2^-25, where the state's change flushes with it, review O1-7 ):
	// a medium of track M must keep it
	if( froxel.boundsParms.x > 0.0 && all( injected == 0.0 ) )
	{
		u_Resolved[ froxelId ] = injected;
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
				const bool exact = all( abs( texel - nearest ) < 1e-3 );
				float4 history = exact ? t_History.Load( int4( int3( nearest ), 0 ) ) : t_History.SampleLevel( s_History, uvw, 0.0 );
				// a history read that touched skipped froxels mixes their marks in: a mark's alpha is minus last frame's
				// sigma_t and every other texel's plus it ( a uniform medium: sliceParms.w ), so the read's alpha gives
				// the share of its weight on froxels that were not skipped, ( sigma_t + a ) / ( 2 sigma_t ), and their
				// in-scatter is the read's over that share. A read that is more mark than fog is not used
				const float prevSigma = froxel.sliceParms.w;
				float fogShare = ( prevSigma > 0.0 ) ? ( prevSigma + history.a ) / ( 2.0 * prevSigma ) : 0.0;
				// V4: on V4's path the share of the read's weight on froxels that were not skipped is one less last frame's
				// marks read through the same filter. A skipped froxel holds clear air, so the read's in-scatter and its sigma_t
				// are diluted by the same share, and their quotient is last frame's in-scatter per unit of its medium: times this
				// frame's sigma_t it is that light in this frame's medium, whatever the medium did between the frames ( drifted,
				// thickened, gained a fog light ). Where the history's medium was clear air its light is not known
				if( mediaPath )
				{
					const float mark = exact ? t_PrevMark.Load( int4( int3( nearest ), 0 ) ) : t_PrevMark.SampleLevel( s_History, uvw, 0.0 );
					fogShare = ( history.a > FROXEL_HISTORY_MIN_SIGMA ) ? 1.0 - mark : 0.0;
				}
				if( all( abs( history ) < 1e30 ) && fogShare >= 0.5 )
				{
					// the history's in-scatter is of last frame's medium: it goes with sigma_s, sigma_t times the albedo,
					// so this frame's is it times the ratio of the sigma_t
					// V4: V3's path keeps V3's statement ( review O1-9 )
					if( mediaPath )
					{
						history.rgb = history.rgb * ( injected.a / history.a ) + state.rgb;
					}
					else
					{
						history.rgb = history.rgb / min( fogShare, 1.0 ) * ( injected.a / prevSigma ) + state.rgb;
					}
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
