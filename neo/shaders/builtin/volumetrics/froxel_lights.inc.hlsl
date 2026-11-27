// froxel_lights.inc.hlsl - one light record at one froxel sample ( track V, package V2 ): what a light sends into
// the air at a point, in the units the inject multiplies by sigma_s. It is the per-light half of
// shaders/lightrecord.inc.hlsl ( LL_EvaluateRecord ) without the surface: the same planes, the same emitter core, the
// same images and the same decoding, so a light scatters what it shines on the walls. Where the surface shader
// multiplies by the surface's colour and the cosine, the air multiplies by pi and the phase function:
//
//   - A Doom 3 light's colour C is the brightness of a white wall that faces it. Such a wall gives back E / pi of
//     the light E that falls on it, so the light that falls through a point is pi * C.
//   - The phase function says what share of it turns toward the camera: Henyey-Greenstein with the fog's g, which
//     integrates to 1 over the sphere. The angle is between the light's travel direction and the direction from
//     the sample to the camera, so a positive g brightens the fog when looking toward the light.
//   - An ambient light is radiance from every direction: no pi, no phase, no shadow.
//   - A fog light is a medium, not a light: V4 adds it to the froxel's medium ( FroxelAddFogLight, below ), never as a light ( FroxelRecordLight ).
//
// The shadow rule ( D80, and the owner's rows 50 and 51 ): FroxelShadowShare. A light's shadow is looked up at the
// sample, one comparison against the shadow atlas ( V3 ), and the fog is not culled by area as surfaces are, so a
// light is taken only where its shadow is known: where the row of the sample's face or cascade has a place in the
// atlas, or where the record says the light needs none ( RLF_FOG_NOSHADOW: the light's own key, or a light the game
// builds without shadows; RLF_FOG_NOCASTER: no caster in the view ). Which records the fog can take at all is R_FogCanTakeRecord's
// ( renderer/VolumetricFog.h ): the pass sends its answer as the take mask, and no other record reaches this file.
//
// The planes' test, a point light's face and an emitter pattern's step are shaders/lightrecord_core.inc.hlsl's,
// which the surface's twin, shaders/lightrecord.inc.hlsl ( LL_EvaluateRecord, LL_RecordShadow ), includes too: a
// compute stage cannot include that file ( it needs the surface's includes and the forward pass's constants ).
//
// The includer has, in front of this file:
//   global_inc.hlsl, emitter.inc.hlsl, lightlists.inc.hlsl and builtin/volumetrics/froxel_grid.inc.hlsl
//   StructuredBuffer<recordLight_t> t_LightRecords      StructuredBuffer<shadowRow_t> t_ShadowRows
//   Texture2D t_DefaultProjection, t_PointFalloff, t_ProjectedFalloff with SamplerState s_LightBorder
//   Texture2D t_ShadowAtlas with SamplerComparisonState s_ShadowCompare
//   StructuredBuffer<recordLight_t> t_PrevRecords ( last frame's records, V3 )
//   StructuredBuffer<float4> t_CasterBounds ( by record, the boxes where a caster that changed shadows, V3 )
// Nothing here takes a screen derivative: the images are read at their first level, and an emitter's pattern gets
// the froxel's own footprint.

#ifndef FROXEL_LIGHTS_INC_HLSL
#define FROXEL_LIGHTS_INC_HLSL

#include "lightrecord_core.inc.hlsl"

// what a light needs of the froxel's sample, set up once per froxel
struct FROXEL_SAMPLE
{
	float3	position;			// the relative frame, as the records
	float3	toEye;				// from the sample to the camera, unit length
	float3	stepX;				// the froxel's three edges in the relative frame: one column to the right, one row down
	float3	stepY;
	float3	stepZ;				// and the slab along the ray
	float	viewDepth;			// the sample's distance along the view axis: the sun's cascade
};

// pi times the Henyey-Greenstein phase function: pi * ( 1 - g^2 ) / ( 4 pi * ( 1 + g^2 - 2 g cos )^1.5 ). At g = 0
// it is 1 / 4. The pass keeps g within -0.9 .. 0.9, so the bracket is at least 0.01
float FroxelPhaseTimesPi( float cosTheta, float g )
{
	const float g2 = g * g;
	const float bracket = max( 1.0 + g2 - 2.0 * g * cosTheta, 1e-4 );
	return ( 1.0 - g2 ) / ( 4.0 * bracket * sqrt( bracket ) );
}

// The share of a record's light that reaches the sample through its shadow ( D80, as the owner ruled rows 50 and 51 ),
// for a record that is not an ambient light's:
//   - the row of the sample: a point light's face ( LL_PointLightFace ); a parallel light's cascade, the first whose far
//     end ( the row's w ) lies beyond the sample's depth, and past the last one's far end the light is unshadowed, as
//     on the surfaces ( interactionSM.ps.hlsl's beyondCascades ); any other light's first row;
//   - a light with no caster in the view ( RLF_FOG_NOCASTER ): unshadowed, times 1 - fade like its neighbours ( row 50;
//     a parallel light and a keyed light whole ). Its map is not read: only at r_shadowCache 2 does such a light have a
//     place in the atlas, the cache's static map, whose casters' shadows all miss the view;
//   - a row with a place in the atlas: one comparison at the sample, with the legacy pass's receiver depth and bias
//     scale, times 1 - fade ( w, the light's fade to unshadowed on its last shadow LOD ). Not for a parallel light,
//     whose w is its cascade's far end, and not for a keyed light ( RLF_FOG_NOSHADOW ), which takes its map's shadow
//     alone. A sample inside the light's volume but outside the row's frustum reads the edge of the row's own tile, not
//     a neighbour's; one behind the row's projection ( near an emitter spot's bulb ) is not shadowed; and a parallel
//     light's sample nearer than the near plane, where its first cascade begins, is looked up on its ray at the near
//     plane;
//   - a row without a place: a keyed light whole, any other light nothing: its shadow is not known;
//   - no rows ( a light without shadows, or a shadow LOD of -1 ): a keyed light whole, any other nothing.
float FroxelShadowShare( recordLight_t L, FROXEL_SAMPLE s, float4 P, float3 toLight )
{
	const bool keyed = ( L.flags & RLF_FOG_NOSHADOW ) != 0;
	if( L.shadowRow < 0 )
	{
		return keyed ? 1.0 : 0.0;
	}
	const bool parallel = ( L.flags & RLF_PARALLEL ) != 0;
	uint row = uint( L.shadowRow );
	if( parallel )
	{
		const uint cascades = uint( froxel.shadowParms.z );
		if( cascades == 0 || s.viewDepth > t_ShadowRows[ row + cascades - 1 ].atlasRect.w )
		{
			return 1.0;
		}
		uint c = 0;
		while( c + 1 < cascades && s.viewDepth >= t_ShadowRows[ row + c ].atlasRect.w )
		{
			c++;
		}
		row += c;
	}
	else if( ( L.flags & RLF_POINT ) != 0 )
	{
		row += LL_PointLightFace( toLight );
	}
	const shadowRow_t R = t_ShadowRows[ row ];
	const float fade = parallel ? 0.0 : saturate( R.atlasRect.w );
	if( ( L.flags & RLF_FOG_NOCASTER ) != 0 )
	{
		// nothing casts a shadow of the light inside the view
		return keyed ? 1.0 : 1.0 - fade;
	}
	if( R.atlasRect.z >= 0.0 )
	{
		float4 Q = P;
		if( parallel && s.viewDepth < froxel.binParms.w )
		{
			Q.xyz = froxel.viewOrigin.xyz + ( P.xyz - froxel.viewOrigin.xyz ) * ( froxel.binParms.w / max( s.viewDepth, 1e-4 ) );
		}
		if( dot( Q, R.worldToShadow[3] ) <= 0.0 )
		{
			return keyed ? 1.0 : 1.0 - fade;
		}
		float3 coord = LL_ShadowRowCoord( R, Q, parallel ? froxel.shadowParms.x : froxel.shadowParms.y );
		const float2 texel = froxel.ambient[0].ww;
		coord.xy = clamp( coord.xy, R.atlasRect.xy + texel, R.atlasRect.xy + R.atlasRect.z - texel );
		const float lit = ( froxel.shadowParms.w > 0.0 ) ? t_ShadowAtlas.SampleCmpLevelZero( s_ShadowCompare, coord.xy, coord.z ) : 1.0;
		return keyed ? lit : lit * ( 1.0 - fade );
	}
	return keyed ? 1.0 : 0.0;
}

// One record's light at a froxel sample, in two factors ( V3: the history needs the light's change, which is its colour's
// change times the same factors ): the return is the product of the light's images there ( its colour without the
// record's colour ), and weight is pi * phase times the share that reaches the sample through its shadow, 1 for an
// ambient light. The light is ( L.color.rgb * images ) * weight. Exactly zero outside the light's volume ( weight -1 ),
// and where the light does not reach the sample or the inject leaves it out ( weight 0 ), so every way of finding a
// froxel's records adds the same terms. L is the record, read only inside the light's volume
float3 FroxelRecordLight( uint r, FROXEL_SAMPLE s, out recordLight_t L, out float weight )
{
	// the light's texgens at the sample: projection S, T, Q and the falloff
	const float4 P = float4( s.position, 1.0 );
	float projS, projT, projQ, fallS;
	if( !LL_RecordPlanes( r, P, projS, projT, projQ, fallS ) )
	{
		L = ( recordLight_t )0;
		weight = -1.0;
		return float3( 0.0, 0.0, 0.0 );
	}
	L = t_LightRecords[ r ];		// a light switched off has no bit in the take mask, and a fog light goes to FroxelAddFogLight: neither is here

	// the share of the light that reaches the sample through its shadow ( FroxelShadowShare ); an ambient light has none
	const float3 toLight = L.originInvRadiusSq.xyz - s.position;
	float taken = 1.0;
	if( ( L.flags & RLF_AMBIENT ) == 0 )
	{
		taken = FroxelShadowShare( L, s, P, toLight );
		if( taken <= 0.0 )
		{
			weight = 0.0;
			return float3( 0.0, 0.0, 0.0 );
		}
	}

	// The light's colour at the sample, as LL_EvaluateRecord takes it: an emitter's curve from the core with an
	// explicit footprint ( F3, D61 ), any other light's from the default projection image and the falloff image of
	// its kind through the black-bordered sampler. An emitter's pattern is filtered over the froxel: the footprint is
	// how far the pattern's position moves across the froxel's three edges
	float3 lightFalloffRGB;
	float3 lightProjRGB;
	bool emOn = false;
	if( ( L.flags & RLF_EMITTER ) != 0 )
	{
		float3 emFootprint = float3( 0.0, 0.0, 0.0 );
		if( EMHasPattern( L.emitter[0], L.emitter[2] ) )			// only a pattern reads the footprint
		{
			const float3 emAxis = 2.0 * EMPatternAxisScale( L.emitter[0] );
			const float3 emX = LL_EmitterPatternStep( L, s.stepX );
			const float3 emY = LL_EmitterPatternStep( L, s.stepY );
			const float3 emZ = LL_EmitterPatternStep( L, s.stepZ );
			emFootprint = ( abs( emX ) + abs( emY ) + abs( emZ ) ) * emAxis;
		}
		bool emEarlyOut;
		emOn = EMEvaluateFootprint( float4( fallS, 0.5, 0.0, 1.0 ), float4( projS, projT, 0.0, projQ ), emFootprint, L.emitter[0], L.emitter[1], L.emitter[2], lightFalloffRGB, lightProjRGB, emEarlyOut );
		if( emEarlyOut )
		{
			weight = 0.0;
			return float3( 0.0, 0.0, 0.0 );							// the light is zero there
		}
	}
	if( !emOn )
	{
		lightProjRGB = t_DefaultProjection.SampleLevel( s_LightBorder, float2( projS, projT ) / projQ, 0.0 ).xyz;
		if( ( L.flags & RLF_POINT ) != 0 )
		{
			lightFalloffRGB = t_PointFalloff.SampleLevel( s_LightBorder, float2( fallS, 0.5 ), 0.0 ).xyz;
		}
		else
		{
			lightFalloffRGB = t_ProjectedFalloff.SampleLevel( s_LightBorder, float2( fallS, 0.5 ), 0.0 ).xyz;
		}
	}
	// an emitter's output is linear; the images are decoded as a product, as the interaction shaders decode them
	const float3 images = emOn ? ( lightProjRGB * lightFalloffRGB ) : sRGBToLinearRGB( lightProjRGB * lightFalloffRGB );

	if( ( L.flags & RLF_AMBIENT ) != 0 )
	{
		weight = 1.0;
		return images;
	}
	// the light's travel direction at the sample; a sample on the light's origin has none, and any will do
	const float3 travel = -toLight / max( length( toLight ), 1e-4 );
	weight = FroxelPhaseTimesPi( dot( travel, s.toEye ), froxel.viewForward.w ) * taken;
	return images;
}

// Whether a sample lies where a caster of record r that changed this frame shadows: in one of the record's boxes ( V3:
// up to FROXEL_CASTER_BOXES, the count in the w of the first box's min; froxelView_t, R_VolumetricFogShadowFlags )
static const uint FROXEL_CASTER_BOXES = 4;			// FOG_CASTER_BOXES of renderer/VolumetricFog.h must be the same
bool FroxelInCasterBoxes( uint r, float3 position )
{
	const uint first = r * 2 * FROXEL_CASTER_BOXES;
	const uint count = min( uint( t_CasterBounds[ first ].w ), FROXEL_CASTER_BOXES );
	for( uint i = 0; i < count; i++ )
	{
		if( all( position >= t_CasterBounds[ first + 2 * i ].xyz ) && all( position <= t_CasterBounds[ first + 2 * i + 1 ].xyz ) )
		{
			return true;
		}
	}
	return false;
}

// One record at the sample ( V3 ): its light into light, and, while the history holds last frame's fog, its change
// into change ( this frame's colour against last frame's, which the copy of last frame's records holds at the index
// the record's ids.z names; a new light counts from nothing ) and whether the history is clipped here: near a light
// that moved, that is new, that lost or found its caster, or whose colour changed by more than historyParms.z of the
// larger colour ( the harness's rule ), and where a caster of the light that changed shadows ( its region, by record )
void FroxelAddRecord( uint r, FROXEL_SAMPLE s, inout float3 light, inout float3 change, inout bool clip )
{
	recordLight_t L;
	float weight;
	const float3 images = FroxelRecordLight( r, s, L, weight );
	// outside the light's volume nothing of it is here. Inside, where it does not reach the sample ( in its shadow, or
	// where an emitter is zero ), its light and its change are zero, and its history is clipped as anywhere in its
	// volume: a froxel that a moving caster has just put in shadow holds last frame's lit fog
	if( weight < 0.0 )
	{
		return;
	}
	light += ( L.color.rgb * images ) * weight;
	if( froxel.jitter.w > 0.0 )
	{
		const int prev = int( L.ids.z );
		float3 prevColor = float3( 0.0, 0.0, 0.0 );
		uint prevFlags = 0;
		const bool isNew = prev < 0 || prev >= int( froxel.historyGrid.w );
		if( !isNew )
		{
			prevColor = t_PrevRecords[ prev ].color.rgb;
			prevFlags = t_PrevRecords[ prev ].flags;
		}
		change += ( ( L.color.rgb - prevColor ) * images ) * weight;
		const bool casterHere = ( L.flags & RLF_FOG_CASTERMOVED ) != 0 && FroxelInCasterBoxes( r, s.position );
		if( isNew || casterHere || ( L.flags & RLF_MOVED_THIS_FRAME ) != 0 || ( ( L.flags ^ prevFlags ) & RLF_FOG_NOCASTER ) != 0
				|| any( abs( L.color.rgb - prevColor ) > froxel.historyParms.z * max( L.color.rgb, prevColor ) ) )
		{
			clip = true;
		}
	}
}

// V4: what the fog lights add to a froxel ( FroxelAddFogLight ): their sigma_t, their grey sigma_s, their emission and the
// emission's change since last frame, all times the store scale, as the inject's other terms
struct FROXEL_FOG
{
	float	sigmaT;
	float	sigmaS;
	float3	emission;
	float3	emissionChange;
};

// V4: a Doom 3 fog light as a medium inside its volume ( D29, D81 ), its record read the fog pass's way: color.rgb the
// stage's raw colour, color.w the fog distance D ( the stage's alpha, or 500 ), the scattering albedo in ids.w, and in the
// emitter lanes, which a fog light never uses ( R_VolumetricFogLightRecord, renderer/tr_frontend_volumetricfog.cpp ): the fog
// plane itself in emitter[0] ( vLight->fogPlane: a projected light's planes do not give it, consultant CN4-1 ) and in
// emitter[1].x the stage alpha's share, 0 to 1, by which the fog pass scales its opacity where the alpha is not a
// distance ( CN4-2: the fog pass's opacity is then at most Linear( share ) at any depth; the medium reaches that opacity at
// D and goes on thickening past it, review O1-1 ). The density rises from 0 at the fog plane to full mediaParms2.z^-1 units below it ( the fog
// pass's enter ramp: 8 texels of its enter image at FOG_SCALE 0.001, 125 units ); sigma_t is the rate over D
// ( mediaParms2.y, times the store scale ). The albedo's share scatters the lights and the ambient; the fog also shines
// with the light's colour as fog.ps.hlsl draws it ( clamped, decoded from sRGB ), times its glow ( V7, row 156: the
// spawnarg "fog_emission" in emitter[1].y, 1 by default, independent of the albedo ), so at glow 1 a deep, long ray
// reads colour * ( 1 - T ) plus what it scatters, the fog pass's blend at albedo 0. The fog plane is the light's far
// clip plane, unit length, in the relative frame, negative inside. While the history holds last frame's fog: a fog light
// that is new, moved or changed its colour or its glow by more than historyParms.z of the larger clips it, and its
// emission's change is the change of colour x glow in this frame's medium. position is the froxel's centre in the
// relative frame ( V4g: the medium is taken there, not at the inject's jittered sample, review F1-1 )
void FroxelAddFogLight( uint r, float3 position, inout FROXEL_FOG fog, inout bool clip )
{
	const float4 P = float4( position, 1.0 );
	float projS, projT, projQ, fallS;
	if( !LL_RecordPlanes( r, P, projS, projT, projQ, fallS ) )
	{
		return;
	}
	const recordLight_t L = t_LightRecords[ r ];
	if( L.color.w <= 0.0 )
	{
		return;			// a record with no fog distance ( no stage ) is no medium ( V4g, review F1-9 )
	}
	const float height = dot( P, L.emitter[0] );
	// the alpha's share: the fraction of sigma_t that makes the medium as opaque at D as the fog pass at its most, Linear( share );
	// 1 for a share of 1 ( a distance alpha ). mediaParms2.y * sliceParms.z is the rate ( the store scale cancels )
	const float shareOpacity = sRGBToLinearRGB( L.emitter[1].xxx ).x;
	const float share = min( 1.0, -log( max( 1.0 - shareOpacity, 1e-6 ) ) / ( froxel.mediaParms2.y * froxel.sliceParms.z ) );
	float density = saturate( -height * froxel.mediaParms2.z ) * share;
	// V7 ( row 241 ): soft sides, track M's edge rule: within emitter[1].z units of the faces but the fog plane the density
	// falls to 0 at the face, smoothstep( 0, 1, d / width ), d / width the distance to the nearest of them in widths ( the plane
	// values times the record's lanes, the planes' inverse lengths over the width, R_VolumetricFogLightRecord ); inside the
	// volume, so the bounds hold
	const float edgeWidth = L.emitter[1].z;
	if( edgeWidth > 0.0 )
	{
		const float faceS = min( projS * L.emitter[2].x, ( projQ - projS ) * L.emitter[2].y );
		const float faceT = min( projT * L.emitter[2].z, ( projQ - projT ) * L.emitter[2].w );
		density *= smoothstep( 0.0, 1.0, min( min( faceS, faceT ), fallS * L.emitter[1].w ) );	// the lanes are in widths ( review O1-8 )
	}
	// review CN2-1: with soft sides a froxel on a face has no density, and must still reach the clip below when the width
	// changed; it adds zeros
	if( density <= 0.0 && edgeWidth <= 0.0 )
	{
		return;
	}
	const float sigmaT = froxel.mediaParms2.y / max( L.color.w, 1.0 ) * density;		// D is 500 or an alpha above 1: the max guards a record nobody filled
	const float albedo = saturate( L.ids.w );
	const float glow = max( L.emitter[1].y, 0.0 );
	const float3 colour = sRGBToLinearRGB( L.color.rgb );
	fog.sigmaT += sigmaT;
	fog.sigmaS += albedo * sigmaT;
	fog.emission += colour * ( sigmaT * glow );
	if( froxel.jitter.w > 0.0 )
	{
		const int prev = int( L.ids.z );
		const bool isNew = prev < 0 || prev >= int( froxel.historyGrid.w );
		const float3 prevColour = isNew ? float3( 0.0, 0.0, 0.0 ) : sRGBToLinearRGB( t_PrevRecords[ prev ].color.rgb );
		const float prevDistance = isNew ? L.color.w : t_PrevRecords[ prev ].color.w;
		const float prevShare = isNew ? L.emitter[1].x : t_PrevRecords[ prev ].emitter[1].x;
		const float prevAlbedo = isNew ? L.ids.w : t_PrevRecords[ prev ].ids.w;
		// a previous record of another kind ( an emitter that became a fog light ) holds another lane here: the colour's and
		// the glow's clips below cut its history ( reviews CN1-2, QR1 F2 )
		const float prevGlow = isNew ? glow : max( t_PrevRecords[ prev ].emitter[1].y, 0.0 );
		const float prevEdge = isNew ? edgeWidth : t_PrevRecords[ prev ].emitter[1].z;
		fog.emissionChange += ( colour * glow - prevColour * prevGlow ) * sigmaT;
		// a fog light's D or alpha share can change without a def update ( a material's expression ): it clips as a colour does
		// ( review O1-5 ); so does a changed albedo ( the light editor's key: V4g, review F1-10 ), a glow changed by more
		// than a colour may change and a changed soft side ( V7 )
		if( isNew || ( L.flags & RLF_MOVED_THIS_FRAME ) != 0 || any( abs( colour - prevColour ) > froxel.historyParms.z * max( colour, prevColour ) )
				|| abs( glow - prevGlow ) > froxel.historyParms.z * max( glow, prevGlow ) || edgeWidth != prevEdge
				|| abs( L.color.w - prevDistance ) > froxel.historyParms.z * max( L.color.w, prevDistance ) || abs( L.emitter[1].x - prevShare ) > froxel.historyParms.z || L.ids.w != prevAlbedo )
		{
			clip = true;
		}
	}
}

// Whether a point of last frame's relative frame lies inside the volume of record r of last frame ( V3: the lights that
// vanished or moved since )
bool FroxelInPrevVolume( int r, float3 position )
{
	if( r < 0 || r >= int( froxel.historyGrid.w ) )
	{
		return false;
	}
	const float4 P = float4( position, 1.0 );
	return LL_PlanesHold( dot( P, t_PrevRecords[ r ].lightProject[0] ), dot( P, t_PrevRecords[ r ].lightProject[1] ),
						  dot( P, t_PrevRecords[ r ].lightProject[2] ), dot( P, t_PrevRecords[ r ].lightProject[3] ) );
}

#endif // FROXEL_LIGHTS_INC_HLSL
