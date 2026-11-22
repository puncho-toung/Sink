// froxel_inject.cs.hlsl - the froxel inject ( track V ): what the air of each froxel scatters toward the camera and
// how much it takes out of a ray, sigma_s * L in rgb and sigma_t in a, both times the store scale.
//
// V4: the medium is the froxel's own: the media volume ( froxel_media.cs.hlsl ) where the view has media besides the
// global fog, else the global fog's constant, plus the fog lights the inject takes as media ( FroxelAddFogLight ).
// V2: the medium was the same everywhere ( the global fog ), and it is lit: by the view's ambient light and by
// the light records of the view ( renderer/LightLists.h, C1 ), each evaluated at the froxel's sample as the surface
// shaders evaluate it ( froxel_lights.inc.hlsl ). The sample is the froxel's centre ( its column's centre, half way
// through its slice in the slice coordinate ) moved by the frame's jitter ( V3: one offset for every froxel, and the
// history averages the frames ). V3: each light's shadow is looked up at the sample, one comparison against the shadow
// atlas ( FroxelShadowShare ), and beside what the air scatters and takes the inject writes the state of the history
// ( u_State: how much light the sample gained or lost since last frame, and whether the resolve clips the history here
// or does not read it ).
//
// A froxel's records are found one of three ways ( r_volumetricFogCull ), which give the same sum but for the cases
// named under 2, since a record adds exactly zero outside its light's volume and the records are walked in ascending
// order every time:
//   2  the light lists' tables: the tile bits of the tile under the sample and the three ranges of the sample's
//      Z-bin ( narrow, wide, fog-only ). A light's tile rectangle is its scissor and its bins are its depth range,
//      so a sample inside the light's volume and inside the view finds the light's bit. The tables are the view's,
//      and three things follow. A light's scissor is clipped by the portals its areas are seen through: outside
//      that rectangle ( behind a wall ) this way leaves a light out that the two others take. A column whose centre
//      lies past the view's right or bottom edge reads the view's last tile, where the others light the sample
//      itself. And the slices nearer than the view's near plane take way 1: a light's scissor and bins are those of
//      its volume cut at that plane, and do not hold in front of it
//   1  every record, behind one test of the record's sphere against the thread group's samples: the test's answer is
//      the same for every thread of a group, so the branch on it splits no wave
//   0  every record: the reference
// In all three the bits pass the take mask first: a bit per record that the pass sets where the fog can take the
// record's light at all ( R_FogCanTakeRecord, renderer/VolumetricFog.h ). A record without a bit is not read.
// Under the furnace radiance ( a test input of the pass ) the record count and the ambient lanes are zero, and what is
// written is the uniform medium alone.

#include "global_inc.hlsl"
#include "emitter.inc.hlsl"
#include "lightlists.inc.hlsl"
#include "builtin/volumetrics/froxel_grid.inc.hlsl"

// *INDENT-OFF*
VK_IMAGE_FORMAT( "rgba16f" ) RWTexture3D<float4> u_Injected : register( u0 );

StructuredBuffer<recordLight_t>	t_LightRecords		: register( t0 );
StructuredBuffer<shadowRow_t>	t_ShadowRows		: register( t1 );
StructuredBuffer<uint4>			t_LightBins			: register( t2 );	// per bin: narrow, wide, fog-only range ( lo | hi << 16 ), 0
StructuredBuffer<uint>			t_LightTileBits		: register( t3 );	// tile-major, words per tile each; bit i = record i
Texture2D						t_DefaultProjection	: register( t4 );
Texture2D						t_PointFalloff		: register( t5 );
Texture2D						t_ProjectedFalloff	: register( t6 );

SamplerState					s_LightBorder		: register( s0 );	// the light images, black beyond their edge
Texture2D						t_ShadowAtlas		: register( t7 );	// the shadow atlas ( V3 )
SamplerComparisonState			s_ShadowCompare		: register( s1 );	// compared, as the forward pass's s_Shadow
VK_IMAGE_FORMAT( "rgba16f" ) RWTexture3D<float4> u_State : register( u1 );		// V3: the light change and the history's use here
StructuredBuffer<recordLight_t>	t_PrevRecords		: register( t8 );	// V3: last frame's records, the pass's copy
StructuredBuffer<int>			t_Vanished			: register( t9 );	// V3: last frame's records whose lights have none now ( vanishedPrev, D82 )
StructuredBuffer<int>			t_Moved				: register( t10 );	// V3: last frame's records of the lights that moved
Texture2D<float>				t_Columns			: register( t11 );	// V3: the farthest surface of each column ( froxel_columns.cs.hlsl )
StructuredBuffer<float4>		t_CasterBounds		: register( t12 );	// V3: by record, the boxes where a caster that changed shadows ( froxelView_t; RLF_FOG_CASTERMOVED )
Texture3D<float4>				t_Media				: register( t13 );	// V4: sigma_s ( rgb ) and sigma_t per froxel, times the store scale ( froxel_media.cs.hlsl )
StructuredBuffer<float4>		t_MediaMoved		: register( t14 );	// V4: by medium that moved, its last and its present bounds in this view's relative frame ( min, max, min, max )
// *INDENT-ON*

#include "builtin/volumetrics/froxel_lights.inc.hlsl"

// The ambient light that the air scatters toward the camera along a ray of direction d ( from the camera to the
// sample ). The lanes hold the view's blended SH of the radiance around it, in the basis of the engine's probes
// ( shEvaluateL2 of builtin/lighting/ambient_lighting_IBL.ps.hlsl: the same order and signs ), each band already
// multiplied by the phase function's weight and by the scale of the surfaces' ambient ( the pass does both ). The
// radiance from every direction, weighted by a phase function that peaks for light travelling toward the camera, is
// that weighted SH read in the direction d. An SH of two bands can dip below zero: cut there
float3 FroxelAmbient( float3 d )
{
	float3 a = froxel.ambient[0].rgb * 0.2820948;
	a += froxel.ambient[1].rgb * ( -0.4886025 * d.y );
	a += froxel.ambient[2].rgb * ( 0.4886025 * d.z );
	a += froxel.ambient[3].rgb * ( -0.4886025 * d.x );
	a += froxel.ambient[4].rgb * ( 1.0925484 * d.y * d.x );
	a += froxel.ambient[5].rgb * ( -1.0925484 * d.y * d.z );
	a += froxel.ambient[6].rgb * ( 0.3153916 * ( 3.0 * d.z * d.z - 1.0 ) );
	a += froxel.ambient[7].rgb * ( -1.0925484 * d.x * d.z );
	a += froxel.ambient[8].rgb * ( 0.5462742 * ( d.x * d.x - d.y * d.y ) );
	return max( a, 0.0 );
}

// the bits of word w whose record indices lie in [ lo, hi ]: LL_WordMask of shaders/lightloop.inc.hlsl, the same text
uint FroxelWordMask( uint w, uint lo, uint hi )
{
	const int first = max( int( lo ) - int( w * 32 ), 0 );
	const int last = min( int( hi ) - int( w * 32 ), 31 );
	if( first > last )
	{
		return 0;
	}
	return ( 0xFFFFFFFFu >> uint( 31 - ( last - first ) ) ) << uint( first );
}

// r_volumetricFogCull 1: whether a record's sphere misses every sample of this thread group: its binning sphere, which
// holds the light's volume ( FroxelGroupMissesSphere, froxel_grid.inc.hlsl )
bool FroxelGroupMisses( uint r, uint2 groupId, float viewDepth )
{
	return FroxelGroupMissesSphere( t_LightRecords[ r ].originInvRadiusSq, groupId, viewDepth, froxel.jitter.xy );
}

[numthreads( 8, 8, 1 )]
void main( uint3 froxelId : SV_DispatchThreadID, uint3 groupId : SV_GroupID )
{
	if( any( froxelId >= uint3( froxel.gridSize.xyz ) ) )
	{
		return;
	}
	// V3: a froxel no pixel's lookup reaches is not lit: it keeps the medium, and its state tells the resolve so
	if( froxel.skipParms.x > 0.0 && float( froxelId.z ) > FroxelColumnReach( t_Columns, froxelId.xy ) )
	{
		// V4: on V4's path the medium is the froxel's own ( the furnace leaves no froxel out, so there is no furnace share )
		// ( review O2-5: on V4's path the resolve stores clear air for it and reads only its neighbours' in-scatter )
		u_Injected[ froxelId ] = ( froxel.mediaParms.z > 0.0 ) ? float4( 0.0, 0.0, 0.0, 0.0 ) : froxel.medium;
		u_State[ froxelId ] = float4( 0.0, 0.0, 0.0, 3.0 );
		return;
	}

	// the froxel's sample and its three edges
	// V3: the sample is moved by the frame's jitter, the same for every froxel ( the group test of way 1 moves with it )
	const float2 columnCoord = float2( froxelId.xy ) + 0.5 + froxel.jitter.xy;
	const float2 lateral = FroxelColumnLateral( columnCoord );
	const float3 ray = froxel.viewForward.xyz + lateral.x * froxel.viewRight.xyz - lateral.y * froxel.viewUp.xyz;
	const float viewDepth = FroxelSliceFar( float( froxelId.z ) - 0.5 + froxel.jitter.z );
	FROXEL_SAMPLE s;
	s.position = froxel.viewOrigin.xyz + viewDepth * ray;
	s.toEye = -normalize( ray );
	s.stepX = ( viewDepth * froxel.rayParms.x ) * froxel.viewRight.xyz;
	s.stepY = ( -viewDepth * froxel.rayParms.y ) * froxel.viewUp.xyz;
	s.stepZ = ( FroxelSliceFar( float( froxelId.z ) ) - FroxelSliceFar( float( froxelId.z ) - 1.0 ) ) * ray;
	s.viewDepth = viewDepth;
	// V4g: the froxel's centre, where the fog lights' medium is taken as the media pass takes the others ( review F1-1: the
	// jitter is one offset for the whole screen and the history does not average sigma_t ); the lights' in-scatter keeps
	// the jittered sample, which the history averages
	const float3 centre = FroxelCentre( froxelId );
	const float centreDepth = FroxelSliceFar( float( froxelId.z ) - 0.5 );

	// what falls through the sample to be scattered toward the camera: the ambient light, then the records
	float3 light = FroxelAmbient( -s.toEye );
	float3 change = float3( 0.0, 0.0, 0.0 );		// V3: the light change since last frame, in the same units
	bool clip = false;
	FROXEL_FOG fog = ( FROXEL_FOG )0;				// V4: the fog lights' medium ( FroxelAddFogLight )

	const uint numRecords = uint( froxel.gridSize.w );
	if( numRecords > 0 && ( froxel.takeMask[0].x & 1u ) != 0 )
	{
		// record 0 is the sun's and is in no bin and no tile word; the mask has no bit for an empty slot
		FroxelAddRecord( 0, s, light, change, clip );
	}
	if( numRecords > 1 )
	{
		// the slices nearer than the view's near plane ( one slice is one depth: the same for a whole thread group )
		uint cullMode = uint( froxel.viewRight.w );
		if( cullMode == 2 && viewDepth < froxel.binParms.w )
		{
			cullMode = 1;
		}
		const uint wordsPerTile = uint( froxel.tileParms.w );
		uint wordFirst = 0;
		uint wordLast = wordsPerTile - 1;
		uint tileBase = 0;
		uint3 ranges = uint3( 0, 0, 0 );
		bool hasRange = true;
		if( cullMode == 2 )
		{
			// the tile under the sample; a column that reaches past the view's edge reads the view's last tile
			const uint2 pixel = uint2( columnCoord * froxel.binParms.z );
			const uint2 tile = min( pixel / uint( froxel.tileParms.x ), uint2( froxel.tileParms.yz ) - 1 );
			tileBase = ( tile.y * uint( froxel.tileParms.y ) + tile.x ) * wordsPerTile;
			// the sample's Z-bin ( LL_BinIndex ): one range of record indices per class, lo | hi << 16, lo > hi where empty
			ranges = t_LightBins[ uint( clamp( viewDepth * froxel.binParms.x, 0.0, froxel.binParms.y - 1.0 ) ) ].xyz;
			uint lo = 0xFFFFu;
			uint hi = 0;
			for( uint c = 0; c < 3; c++ )
			{
				const uint classLo = ranges[ c ] & 0xFFFFu;
				const uint classHi = ranges[ c ] >> 16;
				if( classLo <= classHi )
				{
					lo = min( lo, classLo );
					hi = max( hi, classHi );
				}
			}
			hasRange = ( lo <= hi );
			wordFirst = lo / 32;
			wordLast = hi / 32;
		}
		if( hasRange )
		{
			for( uint w = wordFirst; w <= wordLast; w++ )
			{
				uint bits;
				if( cullMode == 2 )
				{
					bits = t_LightTileBits[ tileBase + w ] & ( FroxelWordMask( w, ranges.x & 0xFFFFu, ranges.x >> 16 )
							| FroxelWordMask( w, ranges.y & 0xFFFFu, ranges.y >> 16 ) | FroxelWordMask( w, ranges.z & 0xFFFFu, ranges.z >> 16 ) );
				}
				else
				{
					bits = FroxelWordMask( w, 1, numRecords - 1 );
				}
				bits &= froxel.takeMask[ w >> 2 ][ w & 3 ];
				while( bits != 0 )
				{
					const uint r = w * 32 + firstbitlow( bits );
					bits &= bits - 1;
					// V4: a fog light is a medium, not a light ( D81 ): only the inject that takes fog lights has its bit.
					// V4g: its medium is taken at the froxel's centre, and its group test is the centres' ( review F1-1 ); under the
					// Z-bins the loop below finds it through the centre's tile and bin ( review G-1 )
					if( ( t_LightRecords[ r ].flags & RLF_FOGLIGHT ) != 0 )
					{
						if( cullMode == 0 || ( cullMode == 1 && !FroxelGroupMissesSphere( t_LightRecords[ r ].originInvRadiusSq, groupId.xy, centreDepth, float2( 0.0, 0.0 ) ) ) )
						{
							FroxelAddFogLight( r, centre, fog, clip );
						}
						continue;
					}
					if( cullMode == 1 && FroxelGroupMisses( r, groupId.xy, viewDepth ) )
					{
						continue;
					}
					FroxelAddRecord( r, s, light, change, clip );
				}
			}
		}
		// V4g: under the Z-bins the fog lights are found through the tile and the Z-bin of the froxel's centre, where their
		// medium is taken ( review G-1: through the jittered sample's, a fog light whose z-range or scissor ends between the
		// two dropped out of the froxel for a frame ). The loop above leaves them to this one. A fog light is never clustered:
		// it is a fog-only record, the bins' third range
		if( cullMode == 2 && froxel.mediaParms2.w > 0.0 )
		{
			const uint2 centrePixel = uint2( ( float2( froxelId.xy ) + 0.5 ) * froxel.binParms.z );
			const uint2 centreTile = min( centrePixel / uint( froxel.tileParms.x ), uint2( froxel.tileParms.yz ) - 1 );
			const uint centreBase = ( centreTile.y * uint( froxel.tileParms.y ) + centreTile.x ) * wordsPerTile;
			const uint fogRange = t_LightBins[ uint( clamp( centreDepth * froxel.binParms.x, 0.0, froxel.binParms.y - 1.0 ) ) ].z;
			const uint fogLo = fogRange & 0xFFFFu;
			const uint fogHi = fogRange >> 16;
			if( fogLo <= fogHi )
			{
				for( uint w = fogLo / 32; w <= fogHi / 32; w++ )
				{
					uint bits = t_LightTileBits[ centreBase + w ] & FroxelWordMask( w, fogLo, fogHi ) & froxel.takeMask[ w >> 2 ][ w & 3 ];
					while( bits != 0 )
					{
						const uint r = w * 32 + firstbitlow( bits );
						bits &= bits - 1;
						if( ( t_LightRecords[ r ].flags & RLF_FOGLIGHT ) != 0 )
						{
							FroxelAddFogLight( r, centre, fog, clip );
						}
					}
				}
			}
		}
	}

	// sigma_s * L: the furnace's share comes in formed, the lit share is sigma_s times the light. Both are times the
	// store scale, and what is stored stays below the half floats' end ( the pass's bound ): a value past it is
	// scaled down as a whole, so that it keeps its colour.
	// V4: on V4's path ( r_volumetricFogMedia ) sigma_s ( rgb ) and sigma_t are the froxel's own, and where the media pass
	// ran the furnace's share is sigma_s times the furnace radiance; V3's path and a view without the media pass take the
	// global fog's constant as V3 did
	float3 sigmaS = froxel.viewOrigin.www;
	float sigmaT = froxel.medium.a;
	float3 inScatter;
	if( froxel.mediaParms.z > 0.0 && froxel.mediaParms.x > 0.0 )
	{
		const float4 m = t_Media[ froxelId ];
		sigmaS = m.rgb;
		sigmaT = m.a;
		inScatter = sigmaS * ( froxel.mediaParms.y + light );
	}
	else
	{
		inScatter = froxel.medium.rgb + froxel.viewOrigin.w * light;
	}
	// V4: the fog lights' medium ( D81 ): their sigma_t, their grey sigma_s, which scatters the light, and their colour's
	// emission; nothing unless the inject takes fog lights ( mediaParms2.w, on V4's path alone )
	float cap = 1.0;		// V4g: the store bound's scale, which the emission's change takes too ( review F1-8 )
	if( froxel.mediaParms2.w > 0.0 )
	{
		sigmaS += fog.sigmaS;
		inScatter += fog.sigmaS * light + fog.emission;
		// past the store's bound sigma_t is scaled down, and sigma_s and the in-scatter with it, so that the albedo stays at most 1
		// ( review O1-12 )
		cap = min( 1.0, froxel.viewUp.w / max( sigmaT + fog.sigmaT, 1e-20 ) );
		sigmaT = ( sigmaT + fog.sigmaT ) * cap;
		sigmaS *= cap;
		inScatter *= cap;
	}
	const float largest = max( inScatter.r, max( inScatter.g, inScatter.b ) );
	const float storeScale = min( 1.0, froxel.viewUp.w / max( largest, 1e-20 ) );
	u_Injected[ froxelId ] = float4( inScatter * storeScale, sigmaT );

	// V3: the lights of last frame whose fog the history holds where this frame's records cannot say how it changed,
	// tested at the froxel's centre in last frame's relative frame: inside the last volume of a light that vanished the
	// history is not read ( D82: its shadow is gone, so its fog cannot be taken out ), inside the last volume of a light
	// that moved it is clipped ( no trail )
	float use = clip ? 1.0 : 0.0;
	if( froxel.jitter.w > 0.0 )
	{
		const float3 here = FroxelCentre( froxelId );			// V4: this frame's relative frame
		const float3 centre = here + froxel.prevOrigin.xyz;
		for( uint m = 0; m < uint( froxel.prevOrigin.w ); m++ )
		{
			if( FroxelInPrevVolume( t_Moved[ m ], centre ) )
			{
				use = max( use, 1.0 );
			}
		}
		for( uint v = 0; v < uint( froxel.historyParms.w ); v++ )
		{
			if( FroxelInPrevVolume( t_Vanished[ v ], centre ) )
			{
				use = 2.0;
			}
		}
		// V4: where a medium that moved, came or went was and is not, or is and was not, the history is clipped, as near a
		// light that moved: no trail. Where it was and is, the history is kept: the resolve carries its light per unit of
		// medium. Each such medium has its last and its present bounds, in this frame's relative frame ( an empty box where
		// it had or has none )
		for( uint b = 0; b < uint( froxel.mediaParms2.x ); b++ )
		{
			const bool was = all( here >= t_MediaMoved[ 4 * b ].xyz ) && all( here <= t_MediaMoved[ 4 * b + 1 ].xyz );
			const bool is = all( here >= t_MediaMoved[ 4 * b + 2 ].xyz ) && all( here <= t_MediaMoved[ 4 * b + 3 ].xyz );
			if( was != is )
			{
				use = max( use, 1.0 );
			}
		}
	}
	// the change in the store's units and scale, as the injected value; past the store's bound it is cut
	// V4: V3's path keeps V3's statement ( review O1-9: a compiler may contract the other differently )
	if( froxel.mediaParms.z > 0.0 )
	{
		u_State[ froxelId ] = float4( clamp( ( sigmaS * change + fog.emissionChange * cap ) * storeScale, -froxel.viewUp.w, froxel.viewUp.w ), use );
	}
	else
	{
		u_State[ froxelId ] = float4( clamp( froxel.viewOrigin.w * change * storeScale, -froxel.viewUp.w, froxel.viewUp.w ), use );
	}
}
