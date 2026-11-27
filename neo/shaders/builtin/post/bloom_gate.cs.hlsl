// bloom_gate.cs.hlsl - WP-J21, the bloom's gate: the frame as the bloom may see it. The stages that pass the gate wrote
// their luminance into the glow mask in their own draws ( texture_color's and bink's USE_GLOW twins ); here each frame
// pixel keeps only what the mask vouches for, less the lit floor round it ( plan section 07, "The bloom's first pass" ):
//   F    the frame's luminance where the mask, widened over 3 x 3, is 0: the lit surfaces near an emitter
//   L0   the lit floor: the least F within 5 pixels ( 11 x 11 ); 0 where every pixel there is masked ( a wide panel )
//   m    1.002 x the mask's largest value over the pixel's 3 x 3 ( the widening covers the pixel TAA spreads a lamp by;
//        the factor lets a pure emitter's pixel through the mask's 16-bit rounding )
//   e    min( max( 0, L - L0 ), m ): the floor first, then the cap ( the order the model held steady )
//   est  R where e >= L, else R x e / L: the pixel's colour, its luminance cut to e
// The bloom's first pass ( bloom_down FIRST 3 and 4 ) then reads est in place of the frame, by today's rules.
// One group a tile of 16 x 16 view pixels. A tile with no mask within 8 pixels writes its flag 0 and nothing else: the
// first pass reads est only through a tile whose flag is 1, so what such a tile holds from an earlier frame is never
// read. The floor is per pixel over 11 x 11, the form the section's model measured.
//
// WP-J27, the sources' own blooms and the hot-pixel boost ( the owner, 2026-11-27 ):
//   slot   the mask's green is its red times 2^slot, the style slot of the source that drew it ( texture_color's twin ); a
//          pixel takes the slot of the mask's largest value over its 3 x 3, the pixel its cap came from. A ratio that is
//          not within 0.1 of a power of two in its log ( two slots blended ) is the view's slot ( review R2-F2 )
//   gain   est times the slot's strength ( a light's bloom_intensity; 1 for the view's slot )
//   boost  the alpha written beside est: how much brighter the pixel enters the transform ( the first pass's wide
//          source ), 2^( stops x s ), s rising smoothly from 0 at the threshold to 1 at the hot level in the log of
//          the pixel's exposed brightness; 1 where the slot has no boost. The core ( level 0 ) is not boosted. The same
//          at every distance: the size rule that faded it on emitters wide on screen made a lamp's glow strong far away
//          and gone up close, and the owner had it removed ( 2026-11-27 )
//   share  where the bloom tells sources apart ( params.y 1 ), each 4 x 4 block of the view gets the light of each slot
//          that will pass the threshold, boosted, summed ( a texel of level 1 ): bloom_share.cs.hlsl spreads it into
//          the shares the transform splits its source by
// STYLES 0 is the form for a view with no style of its own in it: every pixel is slot 0, no sums ( the slots' decode, their
// group-shared memory and the sums' barrier are compiled out ); STYLES 1 reads the slots

#include "vulkan.hlsli"

// *INDENT-OFF*
struct BloomGateConstants
{
	int4	rect;		// xy the view rectangle's origin in the frame, zw its size ( pixels )
	float4	params;		// x the cap's factor ( 1.002 ); y 1 = write the blocks' slot sums; z the hot level, w the threshold ( exposed, the bias divided out )
	float4	expo;		// x the exposure scale exp2( r_exposure ), y r_hdrMinLuminance ( the first pass's E ); zw 0
	float4	slotGain;	// each slot's strength
	float4	slotBoost;	// each slot's boost in stops ( 0: none )
};
VK_PUSH_CONSTANT ConstantBuffer<BloomGateConstants> g_Gate : register( b0 );

Texture2D t_Frame : register( t0 );
Texture2D t_Mask : register( t1 );
Buffer<uint> t_Exposure : register( t2 );
VK_IMAGE_FORMAT( "rgba16f" ) RWTexture2D<float4> u_Gated : register( u0 );
VK_IMAGE_FORMAT( "r32ui" ) RWTexture2D<uint> u_Flags : register( u1 );
VK_IMAGE_FORMAT( "rgba32f" ) RWTexture2D<float4> u_Share : register( u2 );
// *INDENT-ON*

#define GATE_TILE 16
#define GATE_APRON 8
#define GATE_REGION 32		// the tile and its apron on either side
#define GATE_BLOCK 4		// a block of the share sums: a texel of level 1
#define GATE_SLOTS 4		// BLOOM_STYLE_SLOTS

static const float3 GATE_LUMA = float3( 0.2126, 0.7152, 0.0722 );
static const float GATE_NO_FLOOR = 65504.0;

groupshared float s_Mask[GATE_REGION][GATE_REGION];		// [y][x], the mask over the tile +- 8
groupshared float s_F[GATE_REGION][GATE_REGION];		// F over the tile +- 6 ( GATE_NO_FLOOR where masked )
groupshared float s_RowMin[GATE_REGION][GATE_TILE];		// F's least over +- 5 along x, for the tile's columns
#if STYLES
groupshared uint s_Slot[GATE_REGION][GATE_REGION];		// the slot each mask value was drawn with
groupshared float s_W[GATE_TILE * GATE_TILE];			// each pixel's light into the shares
groupshared uint s_WSlot[GATE_TILE * GATE_TILE];		// and its slot
#endif
groupshared uint s_Found;

// the frame's value made finite: a NaN or a negative to 0, an infinity to the largest half float ( the first pass
// scrubs again to its ceiling )
float3 GateScrub( float3 v )
{
	return float3( ( asuint( v.r ) <= 0x7F800000u ) ? min( v.r, 65504.0 ) : 0.0,
				   ( asuint( v.g ) <= 0x7F800000u ) ? min( v.g, 65504.0 ) : 0.0,
				   ( asuint( v.b ) <= 0x7F800000u ) ? min( v.b, 65504.0 ) : 0.0 );
}

// a view-relative pixel, held to the view rectangle as the first pass holds its footprint, in the frame's pixels
int3 GatePixel( int2 q )
{
	return int3( g_Gate.rect.xy + clamp( q, int2( 0, 0 ), g_Gate.rect.zw - int2( 1, 1 ) ), 0 );
}

// the slot a mask texel was drawn with: log2( green / red ) where it is within 0.1 of a whole slot; 0 where there is no mask,
// no green, a value that is not finite or a blend of two slots
uint GateSlot( float2 mask )
{
	if( !( mask.x > 0.0 ) || !( mask.y > 0.0 ) )
	{
		return 0u;
	}
	const float l = log2( mask.y / mask.x );
	const float s = round( l );
	return ( abs( l - s ) < 0.1 && s >= 1.0 && s <= ( float )( GATE_SLOTS - 1 ) ) ? ( uint )s : 0u;
}

float GateSlotValue( float4 v, uint slot )
{
	return ( slot == 0u ) ? v.x : ( slot == 1u ) ? v.y : ( slot == 2u ) ? v.z : v.w;
}

[numthreads( GATE_TILE, GATE_TILE, 1 )]
void main( uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID )
{
	const uint tid = gtid.y * GATE_TILE + gtid.x;
	const int2 tileOrigin = int2( gid.xy ) * GATE_TILE;
#if STYLES
	const bool shares = g_Gate.params.y > 0.5;
#endif
	if( tid == 0u )
	{
		s_Found = 0u;
	}
	GroupMemoryBarrierWithGroupSync();

	// 1. the mask over the tile +- 8, four texels a thread; whether any is lit
	uint found = 0u;
	for( uint k = tid; k < GATE_REGION * GATE_REGION; k += GATE_TILE * GATE_TILE )
	{
		const int rx = ( int )( k % GATE_REGION );
		const int ry = ( int )( k / GATE_REGION );
		const float2 m = t_Mask.Load( GatePixel( tileOrigin + int2( rx, ry ) - GATE_APRON ) ).rg;
		s_Mask[ry][rx] = m.x;
#if STYLES
		s_Slot[ry][rx] = GateSlot( m );
#endif
		found |= ( m.x > 0.0 ) ? 1u : 0u;
	}
	if( found != 0u )
	{
		InterlockedOr( s_Found, 1u );
	}
	GroupMemoryBarrierWithGroupSync();
	if( s_Found == 0u )
	{
		if( tid == 0u )
		{
			u_Flags[ gid.xy ] = 0u;
		}
#if STYLES
		// WP-J27: the tile's blocks hold no light ( the share pass reads every block of the view )
		if( shares && tid < ( GATE_TILE / GATE_BLOCK ) * ( GATE_TILE / GATE_BLOCK ) )
		{
			const int2 block = ( tileOrigin / GATE_BLOCK ) + int2( tid % ( GATE_TILE / GATE_BLOCK ), tid / ( GATE_TILE / GATE_BLOCK ) );
			uint sw, sh;
			u_Share.GetDimensions( sw, sh );
			if( all( uint2( block ) < uint2( sw, sh ) ) )		// review R2-N1: the last tiles' blocks beyond the texture
			{
				u_Share[ uint2( block ) ] = float4( 0.0, 0.0, 0.0, 0.0 );
			}
		}
#endif
		return;
	}
	if( tid == 0u )
	{
		u_Flags[ gid.xy ] = 1u;
	}

	// 2. F over the tile +- 6 ( region 2 to 29 ): the frame's luminance where the mask widened over 3 x 3 is 0
	for( uint k = tid; k < 28u * 28u; k += GATE_TILE * GATE_TILE )
	{
		const int rx = ( int )( k % 28u ) + 2;
		const int ry = ( int )( k / 28u ) + 2;
		float wide = 0.0;
		[unroll]
		for( int dy = -1; dy <= 1; dy++ )
		{
			[unroll]
			for( int dx = -1; dx <= 1; dx++ )
			{
				wide = max( wide, s_Mask[ry + dy][rx + dx] );
			}
		}
		const float3 c = GateScrub( t_Frame.Load( GatePixel( tileOrigin + int2( rx, ry ) - GATE_APRON ) ).rgb );
		s_F[ry][rx] = ( wide > 0.0 ) ? GATE_NO_FLOOR : dot( c, GATE_LUMA );
	}
	GroupMemoryBarrierWithGroupSync();

	// 3. the least F over +- 5 along x, rows 3 to 28 ( the tile +- 5 ), the tile's sixteen columns
	for( uint k = tid; k < 26u * GATE_TILE; k += GATE_TILE * GATE_TILE )
	{
		const int cx = ( int )( k % GATE_TILE );
		const int ry = ( int )( k / GATE_TILE ) + 3;
		float v = GATE_NO_FLOOR;
		[unroll]
		for( int dx = -5; dx <= 5; dx++ )
		{
			v = min( v, s_F[ry][cx + GATE_APRON + dx] );
		}
		s_RowMin[ry][cx] = v;
	}
	GroupMemoryBarrierWithGroupSync();

	// 4. this thread's pixel: the floor over +- 5 along y, the cap, est. A pixel beyond the view writes nothing and adds
	// nothing to its block ( every thread reaches the barrier below )
	const int2 own = int2( gtid.xy );
	const int2 p = tileOrigin + own;
	const bool inside = all( p < g_Gate.rect.zw );
	float weight = 0.0;
	uint slot = 0u;
	if( inside )
	{
		float floorL = GATE_NO_FLOOR;
		[unroll]
		for( int dy = -5; dy <= 5; dy++ )
		{
			floorL = min( floorL, s_RowMin[own.y + GATE_APRON + dy][own.x] );
		}
		if( floorL >= GATE_NO_FLOOR )
		{
			floorL = 0.0;
		}
		float wide = 0.0;
		[unroll]
		for( int wy = -1; wy <= 1; wy++ )
		{
			[unroll]
			for( int wx = -1; wx <= 1; wx++ )
			{
				const float mv = s_Mask[own.y + GATE_APRON + wy][own.x + GATE_APRON + wx];
#if STYLES
				if( mv > wide )
				{
					wide = mv;
					slot = s_Slot[own.y + GATE_APRON + wy][own.x + GATE_APRON + wx];
				}
#else
				wide = max( wide, mv );
#endif
			}
		}
		const float m = g_Gate.params.x * wide;
		const float3 R = GateScrub( t_Frame.Load( int3( g_Gate.rect.xy + p, 0 ) ).rgb );
		const float L = dot( R, GATE_LUMA );
		const float e = min( max( 0.0, L - floorL ), m );
		float3 est = ( e >= L ) ? R : R * ( e / max( L, 1e-30 ) );

		// WP-J27: the slot's strength, then its boost from the pixel's exposed brightness ( its largest channel times E, the
		// first pass's measure ) against the threshold and the hot level
		est *= GateSlotValue( g_Gate.slotGain, slot );
		const float stops = GateSlotValue( g_Gate.slotBoost, slot );
		float boost = 1.0;
#if STYLES
		const bool measure = true;		// the sums want every pixel's light above the threshold
#else
		const bool measure = stops > 0.0;
#endif
		if( measure )
		{
			float adapted = asfloat( t_Exposure[0] );
			if( !( adapted > 0.0 ) )
			{
				adapted = g_Gate.expo.y;
			}
			const float E = min( g_Gate.expo.x / max( adapted, 0.0001 ), 1.0e9 );
			const float B = max( est.r, max( est.g, est.b ) ) * E;
			const float T = g_Gate.params.w;
			if( stops > 0.0 && B > T )
			{
				float s = saturate( log2( B / T ) / log2( g_Gate.params.z / T ) );
				s = s * s * ( 3.0 - 2.0 * s );
				boost = exp2( stops * s );
			}
			weight = max( 0.0, B - T ) * boost;
		}
		u_Gated[ uint2( g_Gate.rect.xy + p ) ] = float4( est, boost );
	}

#if STYLES
	// 5. ( WP-J27 ) each block's light by slot: sixteen threads, one a block ( the barrier outside the branch )
	s_W[tid] = weight;
	s_WSlot[tid] = slot;
	GroupMemoryBarrierWithGroupSync();
	if( shares )
	{
		if( tid < ( GATE_TILE / GATE_BLOCK ) * ( GATE_TILE / GATE_BLOCK ) )
		{
			const uint bx = tid % ( GATE_TILE / GATE_BLOCK );
			const uint by = tid / ( GATE_TILE / GATE_BLOCK );
			float4 sum = float4( 0.0, 0.0, 0.0, 0.0 );
			[unroll]
			for( uint y = 0; y < GATE_BLOCK; y++ )
			{
				[unroll]
				for( uint x = 0; x < GATE_BLOCK; x++ )
				{
					const uint i = ( by * GATE_BLOCK + y ) * GATE_TILE + bx * GATE_BLOCK + x;
					const float w = s_W[i];
					const uint sl = s_WSlot[i];
					sum += float4( sl == 0u ? w : 0.0, sl == 1u ? w : 0.0, sl == 2u ? w : 0.0, sl == 3u ? w : 0.0 );
				}
			}
			const uint2 block = uint2( ( tileOrigin / GATE_BLOCK ) + int2( bx, by ) );
			uint sw, sh;
			u_Share.GetDimensions( sw, sh );
			if( all( block < uint2( sw, sh ) ) )		// review R2-N1
			{
				u_Share[ block ] = sum;		// RGBA32F: a block of boosted lamps does not saturate ( review R2-N2 )
			}
		}
	}
#endif
}
