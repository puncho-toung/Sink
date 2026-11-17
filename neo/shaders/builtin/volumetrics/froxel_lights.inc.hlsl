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
//   - A fog light is a medium, not a light: the inject of this package leaves it out ( V4 ).
//
// The shadow rule ( D80 ). The fog has no shadow of a light without a shadow map, and fog is not culled by area as
// surfaces are, so a light is taken only where its shadow is known: where the shadow row of the sample's face has a
// place in the atlas ( then scaled by 1 - fade, the light's fade to unshadowed on its last shadow LOD ), or, whole,
// where the record says the light needs none ( RLF_FOG_NOSHADOW: the light's own key, or no caster in the view ).
// This package looks no shadow up yet ( V3 ). Which records the fog can take at all is R_FogCanTakeRecord's
// ( renderer/LightLists.h ): the pass sends its answer as the take mask, and no other record reaches this file.
//
// The planes' test, the pick of a point light's face and the emitter's call stand here and in
// shaders/lightrecord.inc.hlsl ( LL_EvaluateRecord, LL_RecordShadow ), which a compute stage cannot include ( it
// needs the surface's includes and the atlas ): change both.
//
// The includer has, in front of this file:
//   global_inc.hlsl, emitter.inc.hlsl, lightlists.inc.hlsl and builtin/volumetrics/froxel_grid.inc.hlsl
//   StructuredBuffer<recordLight_t> t_LightRecords      StructuredBuffer<shadowRow_t> t_ShadowRows
//   Texture2D t_DefaultProjection, t_PointFalloff, t_ProjectedFalloff with SamplerState s_LightBorder
// Nothing here takes a screen derivative: the images are read at their first level, and an emitter's pattern gets
// the froxel's own footprint.

#ifndef FROXEL_LIGHTS_INC_HLSL
#define FROXEL_LIGHTS_INC_HLSL

// what a light needs of the froxel's sample, set up once per froxel
struct FROXEL_SAMPLE
{
	float3	position;			// the relative frame, as the records
	float3	toEye;				// from the sample to the camera, unit length
	float3	stepX;				// the froxel's three edges in the relative frame: one column to the right, one row down
	float3	stepY;
	float3	stepZ;				// and the slab along the ray
};

// pi times the Henyey-Greenstein phase function: pi * ( 1 - g^2 ) / ( 4 pi * ( 1 + g^2 - 2 g cos )^1.5 ). At g = 0
// it is 1 / 4. The pass keeps g within -0.9 .. 0.9, so the bracket is at least 0.01
float FroxelPhaseTimesPi( float cosTheta, float g )
{
	const float g2 = g * g;
	const float bracket = max( 1.0 + g2 - 2.0 * g * cosTheta, 1e-4 );
	return ( 1.0 - g2 ) / ( 4.0 * bracket * sqrt( bracket ) );
}

// One record's light at a froxel sample: pi * phase * the light's colour there, or the colour itself for an
// ambient light. Exactly zero outside the light's volume and for a light the inject leaves out, so every way of
// finding a froxel's records adds the same terms
float3 FroxelRecordLight( uint r, FROXEL_SAMPLE s )
{
	// the light's texgens at the sample: projection S, T, Q and the falloff. The four planes are read on their own,
	// in front of the record, so that a sample outside the light's volume reads nothing else of it
	const float4 P = float4( s.position, 1.0 );
	const float projS = dot( P, t_LightRecords[ r ].lightProject[0] );
	const float projT = dot( P, t_LightRecords[ r ].lightProject[1] );
	const float projQ = dot( P, t_LightRecords[ r ].lightProject[2] );
	const float fallS = dot( P, t_LightRecords[ r ].lightProject[3] );
	if( projQ <= 0.0 || projS < 0.0 || projS > projQ || projT < 0.0 || projT > projQ || fallS < 0.0 || fallS > 1.0 )
	{
		return float3( 0.0, 0.0, 0.0 );
	}
	const recordLight_t L = t_LightRecords[ r ];		// a fog light or a light switched off has no bit in the take mask and is not here

	// the shadow rule: the share of the light that is taken, 0 where its shadow is not known
	const float3 toLight = L.originInvRadiusSq.xyz - s.position;
	float taken = 1.0;
	if( ( L.flags & RLF_AMBIENT ) == 0 )
	{
		bool placed = false;
		if( L.shadowRow >= 0 )
		{
			uint row = uint( L.shadowRow );
			if( ( L.flags & ( RLF_POINT | RLF_PARALLEL ) ) == RLF_POINT )
			{
				// a point light's six rows: the face whose axis the direction to the light is largest along, the first
				// on a tie ( LL_RecordShadow ). A parallel light's rows are its cascades: the first stands for the light
				float best = -toLight.x;
				uint face = 0;
				if( toLight.x > best )
				{
					best = toLight.x;
					face = 1;
				}
				if( -toLight.y > best )
				{
					best = -toLight.y;
					face = 2;
				}
				if( toLight.y > best )
				{
					best = toLight.y;
					face = 3;
				}
				if( -toLight.z > best )
				{
					best = -toLight.z;
					face = 4;
				}
				if( toLight.z > best )
				{
					best = toLight.z;
					face = 5;
				}
				row += face;
			}
			const float4 atlasRect = t_ShadowRows[ row ].atlasRect;
			if( atlasRect.z >= 0.0 )
			{
				placed = true;
				// w is the fade on every row but the sun's, where it is the cascade's far end. A record flagged to be
				// taken without a shadow is taken whole here as well: it must not fade out across its last shadow LOD
				// and come back whole where it has no rows at all
				if( ( L.flags & ( RLF_SUN | RLF_FOG_NOSHADOW ) ) == 0 )
				{
					taken = 1.0 - saturate( atlasRect.w );
				}
			}
		}
		if( !placed && ( L.flags & RLF_FOG_NOSHADOW ) == 0 )
		{
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
			const float3 emX = float3( dot( s.stepX, L.lightProject[0].xyz ), dot( s.stepX, L.lightProject[1].xyz ), dot( s.stepX, L.lightProject[3].xyz ) );
			const float3 emY = float3( dot( s.stepY, L.lightProject[0].xyz ), dot( s.stepY, L.lightProject[1].xyz ), dot( s.stepY, L.lightProject[3].xyz ) );
			const float3 emZ = float3( dot( s.stepZ, L.lightProject[0].xyz ), dot( s.stepZ, L.lightProject[1].xyz ), dot( s.stepZ, L.lightProject[3].xyz ) );
			emFootprint = ( abs( emX ) + abs( emY ) + abs( emZ ) ) * emAxis;
		}
		bool emEarlyOut;
		emOn = EMEvaluateFootprint( float4( fallS, 0.5, 0.0, 1.0 ), float4( projS, projT, 0.0, projQ ), emFootprint, L.emitter[0], L.emitter[1], L.emitter[2], lightFalloffRGB, lightProjRGB, emEarlyOut );
		if( emEarlyOut )
		{
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
	const float3 lightColor = L.color.rgb * ( emOn ? ( lightProjRGB * lightFalloffRGB ) : sRGBToLinearRGB( lightProjRGB * lightFalloffRGB ) );

	if( ( L.flags & RLF_AMBIENT ) != 0 )
	{
		return lightColor;
	}
	// the light's travel direction at the sample; a sample on the light's origin has none, and any will do
	const float3 travel = -toLight / max( length( toLight ), 1e-4 );
	return lightColor * ( FroxelPhaseTimesPi( dot( travel, s.toEye ), froxel.viewForward.w ) * taken );
}

#endif // FROXEL_LIGHTS_INC_HLSL
