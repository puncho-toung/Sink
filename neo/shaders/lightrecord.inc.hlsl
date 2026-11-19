// lightrecord.inc.hlsl - one light record at one surface point ( track F, package F3 ): what a light loop adds for a
// pair ( light, surface ). It was part of builtin/lighting/forward.ps.hlsl and is a file of its own so that other
// passes that walk the light lists can include it ( the translucent pass, the decals ). The froxel inject cannot: a
// compute stage has none of what this file needs. What both need ( the planes, a point light's face, an emitter
// pattern's step ) is shaders/lightrecord_core.inc.hlsl's, which both include. It holds
// the per-light half only. FILLING an LL_SURFACE decides as much of the picture ( both bodies' normals and energy
// terms, the material, the stage colours, the shadow filter's set-up ) and is still the end of forward.ps.hlsl's
// main: a second includer has to fill it the same way to light alike.
//
// The includer has, in front of this file:
//   global_inc.hlsl, BRDF.inc.hlsl, emitter.inc.hlsl and lightlists.inc.hlsl ( recordLight_t, shadowRow_t, RLF_* )
//   pc.rpShadowBias and pc.rpShadowFilter ( render parm set 9 )
//   StructuredBuffer<recordLight_t> t_LightRecords      StructuredBuffer<shadowRow_t> t_ShadowRows
//   Texture2D t_ShadowAtlas with SamplerComparisonState s_Shadow
//   Texture2D t_DefaultProjection, t_PointFalloff, t_ProjectedFalloff with SamplerState s_LightBorder
// and fills one LL_SURFACE per pixel, outside every loop. lightloop.inc.hlsl, included behind this file, calls
// LL_EvaluateRecord. Nothing here takes a screen derivative or samples with an implicit one ( SampleGrad and
// SampleCmpLevelZero only ): the Z-bin loop's trip count differs between the pixels of a quad.

#ifndef LIGHTRECORD_INC_HLSL
#define LIGHTRECORD_INC_HLSL

#include "lightrecord_core.inc.hlsl"

// what a light needs of the pixel, set up once in main and not once per light
struct LL_SURFACE
{
	float3	position;				// the relative frame, as the records
	float3	dPdx;					// its screen derivatives, for the mip of the light images and for an emitter pattern's footprint
	float3	dPdy;
	float3	tangent;				// the vertex frame in the relative frame, interpolated and not normalised: the frame the legacy vertex stages multiply by
	float3	bitangent;
	float3	normal;
	float3	positionTS;				// frame . position, formed per vertex and interpolated ( forward.vs.hlsl's texcoord9 )
	float3	viewTS;					// the view vector in that frame, normalised
	float3	nPlain;					// the bump normal after the contrast of interaction.ps.hlsl
	float3	nShadowed;				// and after the contrast of interactionSM.ps.hlsl
	float	vdotNPlain;
	float	vdotNShadowed;
	float3	energyPlain;			// each body's energy compensation ( it depends on the body's vdotN )
	float3	energyShadowed;
	float3	diffuseColor;			// the interaction shaders' material
	float3	diffuseNoSpecular;		// its diffuse colour under a light with nospecular ( see LL_EvaluateRecord )
	float3	specularColor;
	float	rr;						// roughness^2
	float	rrrr;
	float3	diffuseStage;			// the surface's stage colours: the legacy modifiers without the light's colour
	float3	specularStage;
	float3	vertexColor;
	float	vogelPhi;				// the rotation of the shadow filter's disk at this pixel
	float	vogelSqrtSamples;		// the square root of the filter's sample count
	float	viewDepth;				// F4: the distance along the view axis ( the vertex stage's clip w ), which picks the sun's cascade
};

// interactionSM.ps.hlsl's VogelDiskSample with the square root of the sample count coming in formed
// ( LL_SURFACE::vogelSqrtSamples: once per pixel, not once per tap ): the same expression as that file's, the root of
// the same value and the same division. That the taps come out equal to the bit is the compiler's to keep
float2 LL_VogelDiskSample( float sampleIndex, float sqrtSamplesCount, float phi )
{
	float goldenAngle = 2.4f;

	float r = sqrt( sampleIndex + 0.5f ) / sqrtSamplesCount;
	float theta = sampleIndex * goldenAngle + phi;

	float sine = sin( theta );
	float cosine = cos( theta );

	return float2( r * cosine, r * sine );
}

// One record's shadow at a point: the legacy pass's lookup of the atlas ( interactionSM.ps.hlsl's atlas branch ) from
// the record's rows. Returns the lit share, 1 where the light is unshadowed at the point; shadowBody says whether the
// light is on the shadow-map body. shadowRow and flags are the record's; P is the point in the relative frame;
// toLight the direction itself ( origin - point ), which picks a point light's face; viewDepth the point's distance
// along the view axis, which picks the sun's cascade; vogelPhi and vogelSqrtSamples are the filter's set-up at the
// pixel ( LL_SURFACE ). A function of its own so that a pass without a surface can take a light's shadow alone ( not
// the froxel inject, which cannot include this file )
float LL_RecordShadow( int shadowRow, uint flags, float4 P, float3 toLight, float viewDepth, float vogelPhi, float vogelSqrtSamples, out bool shadowBody )
{
	// the shadow rows: one for a projected light, six for a point light, one per cascade for the sun. A row without a
	// place leaves the light unshadowed there; a light none of whose maps has a place is on the plain body, as the
	// legacy pass binds the plain program then
	float shadow = 1.0;
	shadowBody = false;
	if( shadowRow >= 0 )
	{
		uint row = uint( shadowRow );
		const bool parallel = ( flags & RLF_PARALLEL ) != 0;
		bool beyondCascades = false;
		if( parallel )
		{
			// F4: the sun, the one parallel light the lists hold ( record 0 ). interactionSM.ps.hlsl's LIGHT_PARALLEL
			// select: the first cascade whose far end lies beyond the point, else the last; past the last one's far end
			// the light is unshadowed, on the shadow-map body. A sun's row holds its cascade's far end in w ( C2; the
			// last row the view's shadowCascadeFar ), and rpShadowBias.z is the count of its rows. Tested before the
			// point light's face: a parallel light is a point light too in this engine
			const uint cascades = uint( pc.rpShadowBias.z );
			uint c = 0;
			while( c + 1 < cascades && viewDepth >= t_ShadowRows[ row + c ].atlasRect.w )
			{
				c++;
			}
			row += c;
			beyondCascades = viewDepth > t_ShadowRows[ row ].atlasRect.w;		// only the last cascade can be passed
		}
		else if( ( flags & RLF_POINT ) != 0 )
		{
			row += LL_PointLightFace( toLight );
		}
		const shadowRow_t R = t_ShadowRows[ row ];
		// The body follows the LIGHT, as the legacy pass binds its program per light: -2 marks a row whose own face
		// or cascade was not drawn while the light's maps have places. The shadow-map body then, unshadowed
		shadowBody = ( R.atlasRect.z >= 0.0 || R.atlasRect.z < -1.5 );
		if( R.atlasRect.z >= 0.0 && !beyondCascades )
		{
			// the receiver's depth times the legacy pass's bias scale, the sun's or every other light's
			const float3 shadowCoord = LL_ShadowRowCoord( R, P, parallel ? pc.rpShadowBias.x : pc.rpShadowBias.y );
			const float receiver = shadowCoord.z;
			const float2 shadowTexcoordAtlas = shadowCoord.xy;
			const float numSamples = pc.rpShadowFilter.x;
			const float stepSize = 1.0 / numSamples;
			shadow = 0.0;
			for( float si = 0.0; si < numSamples; si += 1.0 )
			{
				const float2 jitter = LL_VogelDiskSample( si, vogelSqrtSamples, vogelPhi );
				shadow += t_ShadowAtlas.SampleCmpLevelZero( s_Shadow, shadowTexcoordAtlas + jitter * pc.rpShadowFilter.y, receiver );
			}
			shadow *= stepSize;
			// the fade of a light on its last shadow LOD: a row's w, but a sun's rows hold their cascade's far end
			// there, and the sun's fade comes in rpShadowBias.w ( the legacy pass's rpJitterTexScale.z )
			shadow = saturate( max( shadow, parallel ? pc.rpShadowBias.w : R.atlasRect.w ) );
		}
	}
	return shadow;
}

// One record's light at this pixel: what the legacy pass adds for the pair ( surface, light ). Two bodies ( D48 ):
// a light whose shadow map has a place in the atlas takes interactionSM.ps.hlsl's ( contrast up to 1.0, the horizon
// bend, the shadow ), every other light interaction.ps.hlsl's ( contrast up to 1.2, no bend ); on both an emitter's
// output stays linear and the images are decoded ( //puncho - reconcile: emitter in place of the math light ).
// The two vectors are the legacy vertex stages' interpolants: the view vector comes
// interpolated from the forward vertex stage, and the light vector is frame . origin - frame . position, whose two
// halves interpolate exactly as the legacy product does. Outside the light's volume the result is exactly zero, so
// the two loops add the same terms.
float3 LL_EvaluateRecord( uint r, LL_SURFACE s )
{
	// the light's texgens at the pixel: projection S, T, Q and the falloff ( the legacy vertex stages' texcoord3 and
	// texcoord2 ). Outside the light's volume the light is exactly zero ( the two loops then add the same terms )
	const float4 P = float4( s.position, 1.0 );
	float projS, projT, projQ, fallS;
	if( !LL_RecordPlanes( r, P, projS, projT, projQ, fallS ) )
	{
		return float3( 0.0, 0.0, 0.0 );
	}
	const recordLight_t L = t_LightRecords[ r ];

	//puncho start - reconcile: emitter in place of the math light. The interaction shaders' call, the same
	// interpolants: texcoord2 = ( falloff S, 0.5, 0, 1 ) and texcoord3 = ( S, T, 0, Q ), from the same planes ( an
	// emitter's are its box, so Q is 1 ) and the same lanes ( viewEmitter_t::parms, r_showEmitters included ).
	// F3 ( D61 ): the core with an explicit footprint. The interaction shaders take the pattern's footprint as
	// fwidth of its position; a screen derivative is defined only where every pixel of a quad reaches it, which
	// the Z-bin loop cannot promise ( each pixel walks its own bits ). The pattern's position is affine in the
	// pixel's position ( 2 * texgen - 1, scaled per axis ), so its screen step follows from the position's
	// derivatives, which main takes once outside every loop, and the directions of the three planes. Nothing
	// here takes a derivative, so the call stands BEHIND the volume's return: a pixel outside an emitter's
	// volume pays nothing for it
	float3 lightFalloffRGB;
	float3 lightProjRGB;
	bool emOn = false;
	if( ( L.flags & RLF_EMITTER ) != 0 )
	{
		float3 emFootprint = float3( 0.0, 0.0, 0.0 );
		if( EMHasPattern( L.emitter[0], L.emitter[2] ) )			// only a pattern reads the footprint
		{
			const float3 emAxis = 2.0 * EMPatternAxisScale( L.emitter[0] );
			const float3 emDx = LL_EmitterPatternStep( L, s.dPdx ) * emAxis;
			const float3 emDy = LL_EmitterPatternStep( L, s.dPdy ) * emAxis;
			emFootprint = abs( emDx ) + abs( emDy );
		}
		bool emEarlyOut;
		emOn = EMEvaluateFootprint( float4( fallS, 0.5, 0.0, 1.0 ), float4( projS, projT, 0.0, projQ ), emFootprint, L.emitter[0], L.emitter[1], L.emitter[2], lightFalloffRGB, lightProjRGB, emEarlyOut );
		// emEarlyOut: interactionSM.ps.hlsl returns black there before its shadow taps ( r_emitterEarlyOut ); the
		// light is zero there on both bodies, so leaving here skips the taps and changes nothing
		if( emEarlyOut )
		{
			return float3( 0.0, 0.0, 0.0 );
		}
	}
	//puncho end

	// the light vector in the vertex frame: interaction.vs.hlsl's frame . ( origin - position ), interpolated. The
	// frame is linear in the pixel and the origin is one for the draw, so frame . origin interpolates as the product
	// does; frame . position was formed per vertex. Normalised per pixel, as the legacy pixel stage does
	const float3 toLight = L.originInvRadiusSq.xyz - s.position;		// the direction itself, for the shadow face
	const float3 lightOrigin = L.originInvRadiusSq.xyz;
	const float3 lightVector = normalize( float3( dot( s.tangent, lightOrigin ), dot( s.bitangent, lightOrigin ), dot( s.normal, lightOrigin ) ) - s.positionTS );

	// the light's shadow at the pixel and, with it, the body the light is on ( LL_RecordShadow )
	bool shadowBody;
	const float shadow = LL_RecordShadow( L.shadowRow, L.flags, P, toLight, s.viewDepth, s.vogelPhi, s.vogelSqrtSamples, shadowBody );

	// the body's normal; the shadow-map body bends it towards the vertex normal near the horizon, for ldotN only
	const float3 n = shadowBody ? s.nShadowed : s.nPlain;
	const float vdotN = shadowBody ? s.vdotNShadowed : s.vdotNPlain;
	float ldotN;
	if( shadowBody )
	{
		const float horizonBlendStrength = 2.5;
		const float horizonBlendOffset = 0.15;
		const float NbL = dot( n, lightVector );
		const float NgL = lightVector.z;
		float3 N_lit = n;
		if( ( NbL < horizonBlendOffset ) && ( NgL > 0.0 ) )
		{
			const float blend = saturate( ( horizonBlendOffset - NbL ) * horizonBlendStrength / max( NgL, 0.001 ) );
			N_lit = normalize( lerp( n, float3( 0.0, 0.0, 1.0 ), blend ) );
		}
		ldotN = saturate( dot( N_lit, lightVector ) );
	}
	else
	{
		ldotN = saturate( dot( n, lightVector ) );
	}
	const float3 halfAngleVector = normalize( lightVector + s.viewTS );
	const float hdotN = clamp( dot( halfAngleVector, n ), 0.0, 1.0 );
	const float vdotH = clamp( dot( s.viewTS, halfAngleVector ), 0.0, 1.0 );

	// the light's colour at the pixel: an emitter's curve ( taken above ), or the default
	// projection and falloff images of the light's kind through the black-bordered sampler. The images' mip is chosen
	// from the texgens' own derivatives ( the loop may not take implicit ones )
	if( !emOn )	//puncho - reconcile: emitter in place of the math light
	{
		const float invQ = 1.0 / projQ;
		const float2 projUV = float2( projS, projT ) * invQ;
		const float dQdx = dot( s.dPdx, L.lightProject[2].xyz );
		const float dQdy = dot( s.dPdy, L.lightProject[2].xyz );
		const float2 projDx = ( float2( dot( s.dPdx, L.lightProject[0].xyz ), dot( s.dPdx, L.lightProject[1].xyz ) ) - projUV * dQdx ) * invQ;
		const float2 projDy = ( float2( dot( s.dPdy, L.lightProject[0].xyz ), dot( s.dPdy, L.lightProject[1].xyz ) ) - projUV * dQdy ) * invQ;
		const float2 fallUV = float2( fallS, 0.5 );
		const float2 fallDx = float2( dot( s.dPdx, L.lightProject[3].xyz ), 0.0 );
		const float2 fallDy = float2( dot( s.dPdy, L.lightProject[3].xyz ), 0.0 );
		lightProjRGB = t_DefaultProjection.SampleGrad( s_LightBorder, projUV, projDx, projDy ).xyz;
		if( ( L.flags & RLF_POINT ) != 0 )
		{
			lightFalloffRGB = t_PointFalloff.SampleGrad( s_LightBorder, fallUV, fallDx, fallDy ).xyz;
		}
		else
		{
			lightFalloffRGB = t_ProjectedFalloff.SampleGrad( s_LightBorder, fallUV, fallDx, fallDy ).xyz;
		}
	}
	//puncho start - reconcile: emitter in place of the math light. Both interaction shaders leave an emitter's
	// output linear ( the math light was decoded by interaction.ps.hlsl ); the images are decoded as a product
	const float3 lightColor = emOn ? ( lightProjRGB * lightFalloffRGB ) : sRGBToLinearRGB( lightProjRGB * lightFalloffRGB );
	//puncho end

	// the modifiers of the legacy draw: the light stage's colour ( the record's ) times the surface stage's
	const float3 diffuseModifier = L.color.rgb * s.diffuseStage;
	const float3 specularModifier = ( ( L.flags & RLF_NOSPECULAR ) != 0 ) ? float3( 0.0, 0.0, 0.0 ) : L.color.rgb * s.specularStage;

	const float D_denom = ( hdotN * hdotN ) * ( s.rrrr - 1.0 ) + 1.0;
	const float D = shadowBody ? s.rrrr / ( PI * D_denom * D_denom ) : s.rrrr / ( 3.14159f * D_denom * D_denom );
	const float Vis = Visibility_SmithGGXHammon( vdotN, ldotN, s.rr );
	const float3 F = Fresnel_Schlick( s.specularColor, vdotH );
#if KENNY_PBR
	float3 specularLight = D * Vis * F * ldotN * PI * specularModifier;
#else
	float3 specularLight = D * Vis * F * ldotN * specularModifier;
#endif
	specularLight *= shadowBody ? s.energyShadowed : s.energyPlain;
	// A light with nospecular: the legacy pass skips the surface's specular stage for it, draws the interaction with
	// the black image as its specular map and so, that image's usage not being a PBR one, with the program that is
	// NOT PBR, whose diffuse colour is the base colour with no metal share taken out. Kept, so that the two passes
	// light alike ( on a material that is not PBR the two colours are the same ). Only on the legacy pass's slow path,
	// the default ( r_skipInteractionFastPath 1 ): its fast path keeps the PBR image and program and zeroes the
	// specular modifier, so there a metallic PBR material is brighter here by 1 / ( 1 - metal ) ( review O1-2 of F4 )
	const float3 diffuseColor = ( ( L.flags & RLF_NOSPECULAR ) != 0 ) ? s.diffuseNoSpecular : s.diffuseColor;
	const float3 diffuseLight = diffuseColor * ldotN * diffuseModifier;

	return ( diffuseLight + specularLight ) * lightColor * s.vertexColor * shadow;
}

#endif // LIGHTRECORD_INC_HLSL
