/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013-2025 Robert Beckebans
Copyright (C) 2026 Phillip Melnichenko

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

// forward.ps.hlsl - pixel stage of the lit forward pass ( D33 ). X0 landed this as a stub: a copy of
// ambient_lighting_IBL.ps.hlsl in which every place that differs from ambient_lightgrid_IBL.ps.hlsl is
// written as #if USE_LIGHTGRID ( the light-grid code ) #else ( the IBL code ) #endif, so USE_LIGHTGRID=0
// compiles to the IBL shader and USE_LIGHTGRID=1 to the light-grid one.
// Track F ( F2 ): after the ambient term the surface's listed lights are added by the light loop of
// lightloop.inc.hlsl, from the tables of the FORWARD image set. LL_EvaluateRecord gives for one record what the
// legacy interaction shaders add for that light: the body of interaction.ps.hlsl for a light without a placed
// shadow map, the body of interactionSM.ps.hlsl for a light with one ( D48 ). USE_TRANSLUCENT is F4's and is not
// tested here.

#include "global_inc.hlsl"
#include "renderParmSet9.inc.hlsl"

#include "BRDF.inc.hlsl"
#include "mathlight.inc.hlsl"


// *INDENT-OFF*
Texture2D t_Normal				: register( t0 VK_DESCRIPTOR_SET( 1 ) );
Texture2D t_Specular			: register( t1 VK_DESCRIPTOR_SET( 1 ) );
Texture2D t_BaseColor			: register( t2 VK_DESCRIPTOR_SET( 1 ) );

Texture2D t_BrdfLut				: register( t3 VK_DESCRIPTOR_SET( 2 ) );
Texture2D t_Ssao				: register( t4 VK_DESCRIPTOR_SET( 2 ) );
#if USE_LIGHTGRID
Texture2D t_IrradianceCubeMap	: register( t7 VK_DESCRIPTOR_SET( 2 ) );
#else
//Texture2D t_IrradianceCubeMap	: register( t7 VK_DESCRIPTOR_SET( 2 ) );    //no longer used
#endif
TextureCube t_RadianceCubeMap1	: register( t8 VK_DESCRIPTOR_SET( 2 ) );
TextureCube t_RadianceCubeMap2	: register( t9 VK_DESCRIPTOR_SET( 2 ) );
TextureCube t_RadianceCubeMap3	: register( t10 VK_DESCRIPTOR_SET( 2 ) );
TextureCube t_DepthCubeMap		: register (t12 VK_DESCRIPTOR_SET( 2 ) );
TextureCube t_DepthCubeMap2		: register( t13 VK_DESCRIPTOR_SET( 2 ) );	// inter-probe distance 1
TextureCube t_DepthCubeMap3		: register( t14 VK_DESCRIPTOR_SET( 2 ) );	// inter-probe distance 2
#define NUM_RADIANCE_MIPS 10

SamplerState s_Material			: register( s0 VK_DESCRIPTOR_SET( 3 ) ); // (Wrap) Anisotropic sampler: normal sampler & specular sampler
SamplerState s_LinearClamp		: register( s1 VK_DESCRIPTOR_SET( 3 ) ); // (Clamp) Linear sampler: brdf lut sampler & ssao sampler
//SamplerState s_Light			: register( s2 VK_DESCRIPTOR_SET( 3 )); // (Clamp) Anisotropic sampler: irradiance, radiance 1, 2 and 3.

// ---- track F ( F2 ): the tail of the FORWARD image set and its three more samplers ( C4; the order is the layout's ) ----
#include "lightlists.inc.hlsl"

Texture2D						t_ShadowAtlas		: register( t5 VK_DESCRIPTOR_SET( 2 ) );
Texture2D						t_Jitter			: register( t6 VK_DESCRIPTOR_SET( 2 ) );
StructuredBuffer<recordLight_t>	t_LightRecords		: register( t16 VK_DESCRIPTOR_SET( 2 ) );
StructuredBuffer<shadowRow_t>	t_ShadowRows		: register( t17 VK_DESCRIPTOR_SET( 2 ) );
StructuredBuffer<uint>			t_LightPool			: register( t18 VK_DESCRIPTOR_SET( 2 ) );
StructuredBuffer<uint4>			t_LightBins			: register( t19 VK_DESCRIPTOR_SET( 2 ) );
StructuredBuffer<uint>			t_LightTileBits		: register( t20 VK_DESCRIPTOR_SET( 2 ) );
Texture2D						t_DefaultProjection	: register( t21 VK_DESCRIPTOR_SET( 2 ) );
Texture2D						t_PointFalloff		: register( t22 VK_DESCRIPTOR_SET( 2 ) );
Texture2D						t_ProjectedFalloff	: register( t23 VK_DESCRIPTOR_SET( 2 ) );

SamplerComparisonState			s_Shadow			: register( s2 VK_DESCRIPTOR_SET( 3 ) );	// the shadow atlas, compared
SamplerState					s_LightBorder		: register( s3 VK_DESCRIPTOR_SET( 3 ) );	// the light images, black beyond their edge
SamplerState					s_PointWrap			: register( s4 VK_DESCRIPTOR_SET( 3 ) );	// the blue noise

float3 ParallaxCorrectAndOcclude(float3 worldPos, float3 reflDir, float3 probeCentre,
								  float probeFarDist, float roughness, out float occlusion, out float hitDist)
{
    occlusion = 1.0;
    hitDist = probeFarDist;

	// the lookup is blurry anyway
    const int steps = (int) lerp(16.0, 4.0, roughness);
    const float stepLen = probeFarDist / max((float) steps, 1.0);

    float3 rayPos = worldPos + reflDir * (stepLen * 0.5);

    float3 dSeed = rayPos - probeCentre;
    float prevDiff = length(dSeed) - t_DepthCubeMap.SampleLevel(s_LinearClamp, normalize(dSeed), 0).r;

	[loop]
    for (int i = 0; i < steps; i++)
    {
        float3 nextPos = rayPos + reflDir * stepLen;
        float3 d = nextPos - probeCentre;
        float rayDist = length(d);
        float scnDist = t_DepthCubeMap.SampleLevel(s_LinearClamp, normalize(d), 0).r;
        float diff = rayDist - scnDist; 

        if (diff > 0.0)
        {
            float t = saturate(prevDiff / min(prevDiff - diff, -1e-4));
            float3 hitPos = lerp(rayPos, nextPos, t);

            hitDist = length(hitPos - worldPos);
            occlusion = saturate(hitDist / (probeFarDist * 0.5));

            return normalize(hitPos - probeCentre);
        }

        rayPos = nextPos;
        prevDiff = diff;
    }
    return reflDir;
}



// coarse (distances are from the probe centre, not the shading point)
// returns a bent normal 
#define NUM_AO_TAPS 6
float LargeScaleDepthAO(float3 worldPos, float3 N, float3 probeCentre, float probeFarDist, out float3 bentNormal)
{
	// tangent basis around N
    float3 up = (abs(N.z) < 0.999) ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 T = normalize(cross(up, N));
    float3 Bt = cross(N, T);

	// tangent-plane offsets; index 0 points straight up the normal
    const float2 kernel[NUM_AO_TAPS] =
    {
        float2(0.00, 0.00),
		float2(0.66, 0.00),
		float2(-0.33, 0.57),
		float2(-0.33, -0.57),
		float2(0.30, 0.52),
		float2(-0.60, -0.20),
    };

    const float radius = probeFarDist * 0.25;			// occlusion reach    (tunable)
    const float falloff = probeFarDist * 0.25;			// transition softness (tunable)

    float visSum = 0.0;
    float wSum = 0.0;
    float3 bent = float3(0, 0, 0);

	[unroll]
    for (int i = 0; i < NUM_AO_TAPS; i++)
    {
        float3 dir = normalize(N + T * kernel[i].x + Bt * kernel[i].y);
        float w = saturate(dot3(dir, N));				// cosine weight

        float3 p = worldPos + dir * radius;
        float3 fromC = p - probeCentre;
        float scnD = t_DepthCubeMap.SampleLevel(s_LinearClamp, normalize(fromC), 0).r;
        float behind = length(fromC) - scnD;			// > 0 => p is past the surface => occluded
        float vis = saturate(-behind / falloff);		// open space => ~1; behind geometry => 0

        visSum += vis * w;
        wSum += w;
        bent += dir * (vis * w);
    }

    bentNormal = (wSum > 0.0) ? normalize(bent + N * 1e-4) : N;
    return (wSum > 0.0) ? saturate(visSum / wSum) : 1.0;
}

// can this probe see worldPos? 1 = fully visible, -> 0 = behind geometry.
// Bias keeps points lying ON a surface visible; fade softens the wall transition.
float ProbeVisibility(TextureCube depthCube, float3 worldPos, float3 probePos, float probeFarDist)
{
    float3 toP = worldPos - probePos;
    float dP = length(toP);
    float scn = depthCube.SampleLevel(s_LinearClamp, normalize(toP), 0).r;

    const float bias = probeFarDist * 0.02;				// tunable
    const float fade = probeFarDist * 0.10;				// tunable
    return saturate(1.0 - (dP - scn - bias) / fade);
}

struct PS_IN
{
	float4 position		: SV_Position;
	float4 texcoord0	: TEXCOORD0_centroid;
	float4 texcoord1	: TEXCOORD1_centroid;
	float4 texcoord2	: TEXCOORD2_centroid;
	float4 texcoord3	: TEXCOORD3_centroid;
	float4 texcoord4	: TEXCOORD4_centroid;
	float4 texcoord5	: TEXCOORD5_centroid;
	float4 texcoord6	: TEXCOORD6_centroid;
	float4 texcoord7	: TEXCOORD7_centroid;
	float4 texcoord8	: TEXCOORD8_centroid;
	float4 texcoord9	: TEXCOORD9_centroid;
	float4 color		: COLOR0;
};

struct PS_OUT
{
	float4 color : SV_Target0;
};
// *INDENT-ON*



#if USE_LIGHTGRID
	// no SH helpers in the light-grid variant: its diffuse block samples the lightgrid atlas instead
#else
void shEvaluateL2(float3 d, out float y[9])
{
    float x = d.x, yv = d.y, z = d.z;
    float x2 = x * x, y2 = yv * yv, z2 = z * z;

    y[0] = 1.0 / ( 2.0 * sqrt( 3.1415927 ) );
    y[1] = -sqrt( 3.0 / ( 4.0 * 3.1415927 ) ) * yv;
    y[2] = sqrt( 3.0 / ( 4.0 * 3.1415927 ) ) * z;
    y[3] = -sqrt( 3.0 / ( 4.0 * 3.1415927 ) ) * x;
    y[4] = sqrt( 15.0 / ( 4.0 * 3.1415927 ) ) * yv * x;
    y[5] = -sqrt( 15.0 / ( 4.0 * 3.1415927 ) ) * yv * z;
    y[6] = sqrt( 5.0 / ( 16.0 * 3.1415927 ) ) * (3.0 * z2 - 1.0);
    y[7] = -sqrt( 15.0 / ( 4.0 * 3.1415927 ) ) * x * z;
    y[8] = sqrt( 15.0 / ( 16.0 * 3.1415927 ) ) * (x2 - y2);
}

// c[9] = the stored RGB coefficients (first 9 / L2). Returns irradiance E(n).
float3 shEvaluateDiffuseL2RGB(float3 c[9], float3 n)
{
    float b[9];
    shEvaluateL2(n, b);

    const float A0 = 3.1415927;              // SphericalHarmonics
    const float A1 = 3.1415927 * 2.0 / 3.0;
    const float A2 = 3.1415927 * 1.0 / 4.0;

    float3 r = c[0] * b[0] * A0;
    r += (c[1] * b[1] + c[2] * b[2] + c[3] * b[3]) * A1;
    r += (c[4] * b[4] + c[5] * b[5] + c[6] * b[6] + c[7] * b[7] + c[8] * b[8]) * A2;
    return r;
}
#endif

// ---- track F ( F2 ): the light loop ----
// the view's lanes as the loop names them. The integers arrive as float values ( all far below 2^24, so uint( x )
// is exact ), not as bits: see R_LightListViewParms
#define ll_tileSize			uint( pc.rpZBinParms.x )
#define ll_tilesX			uint( pc.rpZBinParms.y )
#define ll_wordsPerTile		uint( pc.rpZBinParms.z )
#define ll_numBins			uint( pc.rpZBinParms.w )
#define ll_binScale			pc.rpZBinScale.x

// F2 compiles the loop without wave intrinsics in every permutation: the scalarized fetch and its run-time
// capability check are F3's ( D51 )
#undef USE_WAVE_OPS
#define USE_WAVE_OPS 0

// what a light needs of the pixel, set up once in main and not once per light
struct LL_SURFACE
{
	float3	position;				// the relative frame, as the records
	float3	dPdx;					// its screen derivatives, for the mip of the light images
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
};

// interactionSM.ps.hlsl's VogelDiskSample, unchanged: the same taps to the bit
float2 LL_VogelDiskSample( float sampleIndex, float samplesCount, float phi )
{
	float goldenAngle = 2.4f;

	float r = sqrt( sampleIndex + 0.5f ) / sqrt( samplesCount );
	float theta = sampleIndex * goldenAngle + phi;

	float sine = sin( theta );
	float cosine = cos( theta );

	return float2( r * cosine, r * sine );
}

// One record's light at this pixel: what the legacy pass adds for the pair ( surface, light ). Two bodies ( D48 ):
// a light whose shadow map has a place in the atlas takes interactionSM.ps.hlsl's ( contrast up to 1.0, the horizon
// bend, a math light's output undecoded, the shadow ), every other light interaction.ps.hlsl's ( contrast up to 1.2,
// no bend, everything decoded ). The two vectors are the legacy vertex stages' interpolants: the view vector comes
// interpolated from the forward vertex stage, and the light vector is frame . origin - frame . position, whose two
// halves interpolate exactly as the legacy product does. Outside the light's volume the result is exactly zero, so
// the two loops add the same terms.
float3 LL_EvaluateRecord( uint r, LL_SURFACE s )
{
	const recordLight_t L = t_LightRecords[ r ];

	// the light's texgens at the pixel: projection S, T, Q and the falloff ( the legacy vertex stages' texcoord3 and texcoord2 )
	const float4 P = float4( s.position, 1.0 );
	const float projS = dot( P, L.lightProject[0] );
	const float projT = dot( P, L.lightProject[1] );
	const float projQ = dot( P, L.lightProject[2] );
	const float fallS = dot( P, L.lightProject[3] );
	// A math light's pattern takes a screen derivative ( fwidth in MLPattern ), which is defined only where every
	// pixel of the draw reaches the call. So the call stands IN FRONT of the volume's return: on the list path a
	// draw's pixels all walk the same records. ( On the Z-bin path the whole function sits under a test per pixel;
	// the footprint form that ends that is F3's, D61. )
	float3 lightFalloffRGB;
	float3 lightProjRGB;
	bool mlMathOn = false;
	if( ( L.flags & RLF_MATH ) != 0 )
	{
		mlMathOn = MLEvaluate( float4( fallS, 0.5, 0.0, 1.0 ), float4( projS, projT, 0.0, projQ ), L.mathLight[0], L.mathLight[1], L.mathLight[2], lightFalloffRGB, lightProjRGB );
	}
	if( projQ <= 0.0 || projS < 0.0 || projS > projQ || projT < 0.0 || projT > projQ || fallS < 0.0 || fallS > 1.0 )
	{
		return float3( 0.0, 0.0, 0.0 );
	}

	// the light vector in the vertex frame: interaction.vs.hlsl's frame . ( origin - position ), interpolated. The
	// frame is linear in the pixel and the origin is one for the draw, so frame . origin interpolates as the product
	// does; frame . position was formed per vertex. Normalised per pixel, as the legacy pixel stage does
	const float3 toLight = L.originInvRadiusSq.xyz - s.position;		// the direction itself, for the shadow face
	const float3 lightOrigin = L.originInvRadiusSq.xyz;
	const float3 lightVector = normalize( float3( dot( s.tangent, lightOrigin ), dot( s.bitangent, lightOrigin ), dot( s.normal, lightOrigin ) ) - s.positionTS );

	// the shadow rows: one for a projected light, six for a point light. A row without a place leaves the light
	// unshadowed there; a light none of whose maps has a place is on the plain body, as the legacy pass binds the
	// plain program then
	float shadow = 1.0;
	bool shadowBody = false;
	if( L.shadowRow >= 0 )
	{
		uint row = uint( L.shadowRow );
		if( ( L.flags & RLF_POINT ) != 0 )
		{
			// interactionSM.ps.hlsl: the face whose axis the direction to the light is largest along, the first on a tie
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
		const shadowRow_t R = t_ShadowRows[ row ];
		// The body follows the LIGHT, as the legacy pass binds its program per light: -2 marks a row whose own face
		// or cascade was not drawn while the light's maps have places. The shadow-map body then, unshadowed
		shadowBody = ( R.atlasRect.z >= 0.0 || R.atlasRect.z < -1.5 );
		if( R.atlasRect.z >= 0.0 )
		{
			float4 shadowTexcoord;
			shadowTexcoord.x = dot( P, R.worldToShadow[0] );
			shadowTexcoord.y = dot( P, R.worldToShadow[1] );
			shadowTexcoord.z = dot( P, R.worldToShadow[2] );
			shadowTexcoord.w = dot( P, R.worldToShadow[3] );
			shadowTexcoord.xyz /= shadowTexcoord.w;
			const float receiver = shadowTexcoord.z * pc.rpShadowBias.y;
			const float2 shadowTexcoordAtlas = shadowTexcoord.xy * R.atlasRect.z + R.atlasRect.xy;
			const float numSamples = pc.rpShadowFilter.x;
			const float stepSize = 1.0 / numSamples;
			shadow = 0.0;
			for( float si = 0.0; si < numSamples; si += 1.0 )
			{
				const float2 jitter = LL_VogelDiskSample( si, numSamples, s.vogelPhi );
				shadow += t_ShadowAtlas.SampleCmpLevelZero( s_Shadow, shadowTexcoordAtlas + jitter * pc.rpShadowFilter.y, receiver );
			}
			shadow *= stepSize;
			shadow = saturate( max( shadow, R.atlasRect.w ) );		// the fade of a light on its last shadow LOD
		}
	}

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

	// the light's colour at the pixel: a math light's curve ( taken at the head of the function ), or the default
	// projection and falloff images of the light's kind through the black-bordered sampler. The images' mip is chosen
	// from the texgens' own derivatives ( the loop may not take implicit ones )
	if( !mlMathOn )
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
	// interactionSM.ps.hlsl leaves a math light's output as it is; interaction.ps.hlsl decodes it like the images
	const float3 lightColor = ( mlMathOn && shadowBody ) ? ( lightProjRGB * lightFalloffRGB ) : sRGBToLinearRGB( lightProjRGB * lightFalloffRGB );

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
	// light alike ( on a material that is not PBR the two colours are the same )
	const float3 diffuseColor = ( ( L.flags & RLF_NOSPECULAR ) != 0 ) ? s.diffuseNoSpecular : s.diffuseColor;
	const float3 diffuseLight = diffuseColor * ldotN * diffuseModifier;

	return ( diffuseLight + specularLight ) * lightColor * s.vertexColor * shadow;
}

#include "lightloop.inc.hlsl"

// r_showLightLists 2 and 3, through the spare lane rpZBinScale.w ( 1 is the heatmap pass's, over every pixel ):
// 2 = the surface's path ( green the list, blue the Z-bins, magenta the all-ones mask of a full pool ), 3 = the
// lights this pixel loops on that path ( the run's length on the list path ). A surface the forward pass does not
// draw keeps its picture
float3 LL_Heat( float n )			// the heatmap pass's ramp: none black, then blue, cyan, green, yellow, red at 1, 4, 8, 16, 32, white beyond
{
	if( n <= 0.0 )
	{
		return float3( 0.0, 0.0, 0.0 );
	}
	return ( n <= 4.0 ) ? lerp( float3( 0.0, 0.0, 1.0 ), float3( 0.0, 1.0, 1.0 ), ( n - 1.0 ) / 3.0 )
		   : ( n <= 8.0 ) ? lerp( float3( 0.0, 1.0, 1.0 ), float3( 0.0, 1.0, 0.0 ), ( n - 4.0 ) / 4.0 )
		   : ( n <= 16.0 ) ? lerp( float3( 0.0, 1.0, 0.0 ), float3( 1.0, 1.0, 0.0 ), ( n - 8.0 ) / 8.0 )
		   : ( n <= 32.0 ) ? lerp( float3( 1.0, 1.0, 0.0 ), float3( 1.0, 0.0, 0.0 ), ( n - 16.0 ) / 16.0 )
		   : float3( 1.0, 1.0, 1.0 );
}

float3 LL_DebugView( uint show, uint path, uint first, uint count, uint2 pixel, float viewZ )
{
	if( show == 2 )
	{
		return ( path == 0 ) ? float3( 0.0, 1.0, 0.0 ) : ( ( path == 1 ) ? float3( 0.0, 0.4, 1.0 ) : float3( 1.0, 0.0, 1.0 ) );
	}
	return LL_Heat( float( LL_CountLights( path, first, count, pixel, viewZ ) ) );
}
// ---- end track F ----

void main( PS_IN fragment, out PS_OUT result )
{
	float2 baseUV = fragment.texcoord1.xy;
	float2 bumpUV = fragment.texcoord0.xy;
	float2 specUV = fragment.texcoord2.xy;

	// PSX affine texture mapping
	if( pc.rpPSXDistortions.z > 0.0 )
	{
		baseUV /= fragment.texcoord0.z;
		bumpUV /= fragment.texcoord0.z;
		specUV /= fragment.texcoord0.z;
	}

	float4 bumpMap =		t_Normal.Sample( s_Material, bumpUV );
	float4 YCoCG =		t_BaseColor.Sample( s_Material, baseUV );
	float4 specMapSRGB =	t_Specular.Sample( s_Material, specUV );
	float4 specMap =		sRGBAToLinearRGBA( specMapSRGB );

	float3 diffuseMap = sRGBToLinearRGB( ConvertYCoCgToRGB( YCoCG ) );

	float3 localNormal;
#if defined(USE_NORMAL_FMT_RGB8)
	localNormal.xy = bumpMap.rg - 0.5;
#else
	localNormal.xy = bumpMap.wy - 0.5;
#endif
	localNormal.z = sqrt( abs( dot( localNormal.xy, localNormal.xy ) - 0.25 ) );
	localNormal = normalize( localNormal );
	const float3 llLocalNormal = localNormal;				// track F: the decoded normal, before the ambient term's contrast below changes it

	// globalPosition, rpGlobalEyePos and the probe parms (rpWobbleSky*, rpTexGen0*) are relative
	// to the render origin (eye-centred world axes)
    float3 globalPosition = fragment.texcoord7.xyz;
    float3 globalView = normalize(pc.rpGlobalEyePos.xyz - globalPosition);

	// View-dependent normal contrast
#if USE_LIGHTGRID
    const float vdContrastMin = 0.8;
    const float vdContrastMax = 1.2;
#else
    const float vdContrastMin = 1.0;
    const float vdContrastMax = 1.4;
#endif
	{
		
        float3 globalNormalPre;
        globalNormalPre.x = dot3(localNormal, fragment.texcoord4);
        globalNormalPre.y = dot3(localNormal, fragment.texcoord5);
        globalNormalPre.z = dot3(localNormal, fragment.texcoord6);
        globalNormalPre = normalize(globalNormalPre);

        float NdotV = saturate(dot3(globalNormalPre, globalView));
        float grazing = 1.0 - NdotV;
        float contrast = lerp(vdContrastMin, vdContrastMax, grazing);
        localNormal.xy *= contrast;
        localNormal = normalize(localNormal);
    }

    float3 globalNormal;
    globalNormal.x = dot3(localNormal, fragment.texcoord4);
    globalNormal.y = dot3(localNormal, fragment.texcoord5);
    globalNormal.z = dot3(localNormal, fragment.texcoord6);
    globalNormal = normalize(globalNormal);

	float3 reflectionVector = globalNormal * dot3( globalView, globalNormal );
	reflectionVector = normalize( ( reflectionVector * 2.0f ) - globalView );

#if 0
	// parallax box correction using portal area bounds
	float hitScale = 0.0;
	float3 bounds[2];
	bounds[0].x = pc.rpWobbleSkyX.x;
	bounds[0].y = pc.rpWobbleSkyX.y;
	bounds[0].z = pc.rpWobbleSkyX.z;

	bounds[1].x = pc.rpWobbleSkyY.x;
	bounds[1].y = pc.rpWobbleSkyY.y;
	bounds[1].z = pc.rpWobbleSkyY.z;

	// global fragment position
	float3 rayStart = fragment.texcoord7.xyz;

	// we can't start inside the box so move this outside and use the reverse path
	rayStart += reflectionVector * 10000.0;

	// only do a box <-> ray intersection test if we use a local cubemap
	if( ( pc.rpWobbleSkyX.w > 0.0 ) && AABBRayIntersection( bounds, rayStart, -reflectionVector, hitScale ) )
	{
		float3 hitPoint = rayStart - reflectionVector * hitScale;

		// pc.rpWobbleSkyZ is cubemap center
		reflectionVector = hitPoint - pc.rpWobbleSkyZ.xyz;
	}
#endif

	float vDotN = saturate( dot3( globalView, globalNormal ) );

#if USE_PBR
	const float metallic = specMapSRGB.g;
	const float roughness = specMapSRGB.r;
	const float glossiness = 1.0 - roughness;
	float ao = specMapSRGB.b;

	// the vast majority of real-world materials (anything not metal or gems) have F(0)
	// values in a very narrow range (~0.02 - 0.08)

	// approximate non-metals with linear RGB 0.04 which is 0.08 * 0.5 (default in UE4)
	const float3 dielectricColor = _float3( 0.04 );

	// derive diffuse and specular from albedo(m) base color
	const float3 baseColor = diffuseMap;

	float3 diffuseColor = baseColor * ( 1.0 - metallic );
	float3 specularColor = lerp( dielectricColor, baseColor, metallic );

#if defined( DEBUG_PBR )
	diffuseColor = float3( 0.0, 0.0, 0.0 );
	specularColor = float3( 0.0, 1.0, 0.0 );
#endif

	float3 kS = Fresnel_SchlickRoughness( specularColor, vDotN, roughness );
	float3 kD = ( float3( 1.0, 1.0, 1.0 ) - kS );

#else

	float ao = 1.0;

// legacy fudgefactor spec+bump wrangling into "PBR", leave it alone unless touching backwards compat
#if KENNY_PBR
	float3 diffuseColor = diffuseMap;
	float3 specularColor;
	float roughness;

	PBRFromSpecmap( specMapSRGB.rgb, specularColor, roughness );
#else
	const float roughness = EstimateLegacyRoughness( specMapSRGB.rgb );

	float3 diffuseColor = diffuseMap;
	float3 specularColor = specMap.rgb;
#endif

#if defined( DEBUG_PBR )
	diffuseColor = float3( 0.0, 0.0, 0.0 );
	specularColor = float3( 1.0, 0.0, 0.0 );
#endif

	float3 kS = Fresnel_SchlickRoughness( specularColor, vDotN, roughness );

	// NOTE: metalness is missing
	float3 kD = ( float3( 1.0, 1.0, 1.0 ) - kS );

#endif

	//diffuseColor = float3( 1.0, 1.0, 1.0 );
	//diffuseColor = float3( 0.0, 0.0, 0.0 );

	// calculate the screen texcoord in the 0.0 to 1.0 range
	//float2 screenTexCoord = vposToScreenPosTexCoord( fragment.position.xy );
	float2 screenTexCoord = fragment.position.xy * pc.rpWindowCoord.xy;

	ao = min( ao,  t_Ssao.Sample( s_LinearClamp, screenTexCoord ).r );

	// evaluate diffuse IBL
#if USE_LIGHTGRID
	float2 normalizedOctCoord = octEncode( globalNormal );
	float2 normalizedOctCoordZeroOne = ( normalizedOctCoord + float2( 1.0, 1.0 ) ) * 0.5;

// lightgrid atlas

	//float3 lightGridOrigin = float3( -192.0, -128.0, 0 );
	//float3 lightGridSize = float3( 64.0, 64.0, 128.0 );
	//int3 lightGridBounds = int3( 7, 7, 3 );

	// grid origin relative to the render origin (eye-centred world axes); size and counts are not positions
	float3 lightGridOrigin = pc.rpGlobalLightOrigin.xyz;
	float3 lightGridSize = pc.rpJitterTexScale.xyz;
	int3 lightGridBounds = int3( pc.rpJitterTexOffset.x, pc.rpJitterTexOffset.y, pc.rpJitterTexOffset.z );

	float invXZ = ( 1.0 / ( lightGridBounds[0] * lightGridBounds[2] ) );
	float invY = ( 1.0 / lightGridBounds[1] );

	normalizedOctCoordZeroOne.x *= invXZ;
	normalizedOctCoordZeroOne.y *= invY;

	int3 gridCoord = int3( 0, 0, 0 );
	float3 frac;
	float3 lightOrigin = globalPosition - lightGridOrigin;

	for( int j = 0; j < 3; j++ )
	{
		float           v;

		// walls can be sampled behind the grid sometimes so avoid negative weights
		v = max( 0, lightOrigin[j] * ( 1.0 / lightGridSize[j] ) );
		gridCoord[j] = int( floor( v ) );
		frac[ j ] = v - gridCoord[ j ];

		if( gridCoord[j] < 0 )
		{
			gridCoord[j] = 0;
		}
		else if( gridCoord[j] >= lightGridBounds[j] - 1 )
		{
			gridCoord[j] = lightGridBounds[j] - 1;
		}
	}

	// trilerp the light value
	int3 gridStep;

	gridStep[0] = 1;
	gridStep[1] = lightGridBounds[0];
	gridStep[2] = lightGridBounds[0] * lightGridBounds[1];

	float totalFactor = 0.0;
	float3 irradiance = float3( 0.0, 0.0, 0.0 );

	/*
	for( int i = 0; i < 8; i++ )
	{
		for( int j = 0; j < 3; j++ )
		{
			if( i & ( 1 << j ) )

		results in these offsets
	*/
	const float3 cornerOffsets[8] =
	{
		float3( 0.0, 0.0, 0.0 ),
		float3( 1.0, 0.0, 0.0 ),
		float3( 0.0, 2.0, 0.0 ),
		float3( 1.0, 2.0, 0.0 ),
		float3( 0.0, 0.0, 4.0 ),
		float3( 1.0, 0.0, 4.0 ),
		float3( 0.0, 2.0, 4.0 ),
		float3( 1.0, 2.0, 4.0 )
	};

	for( int i = 0; i < 8; i++ )
	{
		float factor = 1.0;

		int3 gridCoord2 = gridCoord;

		for( int j = 0; j < 3; j++ )
		{
			if( cornerOffsets[ i ][ j ] > 0.0 )
			{
				factor *= frac[ j ];

				gridCoord2[ j ] += 1;
			}
			else
			{
				factor *= ( 1.0 - frac[ j ] );
			}
		}

		// build P
		//float3 P = lightGridOrigin + ( gridCoord2[0] * gridStep[0] + gridCoord2[1] * gridStep[1] + gridCoord2[2] * gridStep[2] );

		float2 atlasOffset;

		atlasOffset.x = ( gridCoord2[0] * gridStep[0] + gridCoord2[2] * gridStep[1] ) * invXZ;
		atlasOffset.y = ( gridCoord2[1] * invY );

		// offset by one pixel border bleed size for linear filtering
#if 1
		// pc.rpScreenCorrectionFactor.w = probeSize factor accounting account offset border, e.g = ( 16 / 18 ) = 0.8888
		float2 octCoordNormalizedToTextureDimensions = ( normalizedOctCoordZeroOne + atlasOffset ) * pc.rpScreenCorrectionFactor.w;

		// skip by default 2 pixels for each grid cell and offset the start position by (1,1)
		// pc.rpScreenCorrectionFactor.z = borderSize e.g = 2
		float2 probeTopLeftPosition;
		probeTopLeftPosition.x = ( gridCoord2[0] * gridStep[0] + gridCoord2[2] * gridStep[1] ) * pc.rpScreenCorrectionFactor.z + pc.rpScreenCorrectionFactor.z * 0.5;
		probeTopLeftPosition.y = ( gridCoord2[1] ) * pc.rpScreenCorrectionFactor.z + pc.rpScreenCorrectionFactor.z * 0.5;

		float2 normalizedProbeTopLeftPosition = probeTopLeftPosition * pc.rpCascadeDistances.zw;

		float2 atlasCoord = normalizedProbeTopLeftPosition + octCoordNormalizedToTextureDimensions;
#else
		float2 atlasCoord = normalizedOctCoordZeroOne + atlasOffset;
#endif

		float3 color = t_IrradianceCubeMap.Sample( s_LinearClamp, atlasCoord, 0 ).rgb;

		if( ( color.r + color.g + color.b ) < 0.0001 )
		{
			// ignore samples in walls
			continue;
		}

		irradiance += color * factor;
		totalFactor += factor;
	}

	if( totalFactor > 0.0 && totalFactor < 0.9999 )
	{
		totalFactor = 1.0 / totalFactor;

		irradiance *= totalFactor;
	}

// lightgrid atlas
#else
    // Matches the bake's normalization: it stored shEvaluateDiffuse(...) / PI
	// (RenderWorld_envprobes), so divide by PI here too or be PI x too bright.
    float3 sh[9];
	[unroll]
    for (int i = 0; i < 9; i++)
    {
        sh[i] = pc.rpSHIrradiance[i].rgb;
    }
    
    float3 irradiance = max(shEvaluateDiffuseL2RGB(sh, globalNormal) / 3.14159265, 0.0);
#endif
    
    float2 envBRDF = t_BrdfLut.Sample(s_LinearClamp, float2(max(vDotN, 0.0), roughness)).rg;

	// multiple-scattering energy compensation - Fdez-Aguera
    float3 singleScatterSpec = specularColor * envBRDF.x + envBRDF.y;
    float singleScatterEnergy = envBRDF.x + envBRDF.y;
    float multiScatterEnergy = 1.0 - singleScatterEnergy;
    float3 averageFresnel = specularColor + (1.0 - specularColor) * (1.0 / 21.0);
    float3 multiScatterSpec = singleScatterSpec * averageFresnel / (1.0 - multiScatterEnergy * averageFresnel);
    float3 specularScatter = singleScatterSpec + multiScatterSpec * multiScatterEnergy;

	// energy left for diffuse after specular takes its full (multiscatter) share - replaces kD
    float3 diffuseEnergy = float3(1.0, 1.0, 1.0) - specularScatter;

    float3 diffuseLight = (diffuseEnergy * irradiance * diffuseColor) * ao * (pc.rpDiffuseModifier.xyz * 1.0);
	
	// large-scale AO folded into diffuse
    if (pc.rpWobbleSkyX.w > 0.0)
    {
        float3 bentN;
        float aoFarDist = length(pc.rpWobbleSkyY.xyz - pc.rpWobbleSkyZ.xyz);
		// the depth cube was captured at the nearest probe's origin (rpTexGen0S), not at the box centre
        float largeAO = LargeScaleDepthAO(fragment.texcoord7.xyz, globalNormal, pc.rpTexGen0S.xyz, aoFarDist, bentN);
        diffuseLight *= lerp(1.0, largeAO, 0.50); //third is tunable
    }

	// cube: full chain, hardware seam filtering. No octahedral mip cap
    const float MAX_REFLECTION_LOD = float(NUM_RADIANCE_MIPS - 1);
    float mip = roughness * MAX_REFLECTION_LOD;

	// parallax-correct march, per-probe reprojection, visibility-weighted blend
	// Gated on probe validity (rpWobbleSkyX.w == 1)
    float3 reflDir = reflectionVector;
    float reflOcclusion = 1.0;
    float3 blend = pc.rpLocalLightOrigin.xyz; // barycentric weights from the frontend
    float3 dir1 = reflectionVector;
    float3 dir2 = reflectionVector;
    float3 dir3 = reflectionVector;

    if ( pc.rpWobbleSkyX.w > 0.0 )
    {
        float probeFarDist = length( pc.rpWobbleSkyY.xyz - pc.rpWobbleSkyZ.xyz );
        float reflHitDist;
		// march the depth cube from its capture origin (rpTexGen0S); the box only sizes the march
        reflDir = ParallaxCorrectAndOcclude( fragment.texcoord7.xyz, reflectionVector,
											 pc.rpTexGen0S.xyz, probeFarDist, roughness, reflOcclusion, reflHitDist );

		// distant reflected geometry blurs with roughness (optional)
        mip = min( MAX_REFLECTION_LOD, mip + roughness * saturate( reflHitDist / probeFarDist ) * 2.0 );

		// The march walked along reflectionVector from the fragment, so hitPos sits on that ray. re-project it into each probe. On a miss,
		// reflHitDist == probeFarDist and this becomes correct finite-distance parallax.
        float3 hitPos = fragment.texcoord7.xyz + reflectionVector * reflHitDist;
        dir1 = ( pc.rpTexGen0S.w > 0.0 ) ? normalize(hitPos - pc.rpTexGen0S.xyz ) : reflDir;
        dir2 = ( pc.rpTexGen0T.w > 0.0 ) ? normalize(hitPos - pc.rpTexGen0T.xyz ) : reflDir;
        dir3 = ( pc.rpTexGen0Q.w > 0.0 ) ? normalize(hitPos - pc.rpTexGen0Q.xyz ) : reflDir;

        float v1 = ( pc.rpTexGen0S.w > 0.0 ) ? ProbeVisibility( t_DepthCubeMap, fragment.texcoord7.xyz, pc.rpTexGen0S.xyz, probeFarDist ) : 1.0;
        float v2 = ( pc.rpTexGen0T.w > 0.0 ) ? ProbeVisibility( t_DepthCubeMap2, fragment.texcoord7.xyz, pc.rpTexGen0T.xyz, probeFarDist ) : 1.0;
        float v3 = ( pc.rpTexGen0Q.w > 0.0 ) ? ProbeVisibility( t_DepthCubeMap3, fragment.texcoord7.xyz, pc.rpTexGen0Q.xyz, probeFarDist ) : 1.0;

        float3 w = blend * float3(v1, v2, v3);
        float s = w.x + w.y + w.z;
        blend = (s > 0.02) ? (w / s) : blend;
    }

	// per-probe directions and visibility-weighted blend replace the shared lookup
    float3 radiance = t_RadianceCubeMap1.SampleLevel( s_LinearClamp, dir1, mip ).rgb * blend.x;
    radiance += t_RadianceCubeMap2.SampleLevel( s_LinearClamp, dir2, mip ).rgb * blend.y;
    radiance += t_RadianceCubeMap3.SampleLevel( s_LinearClamp, dir3, mip ).rgb * blend.z;


#if 0
	result.color.rgb = float3( envBRDF.x, envBRDF.y, 0.0 );
	result.color.w = fragment.color.a;
	return;
#endif

	// Gate specular IBL by local lightgrid irradiance independent of ambient slider,
    float irradianceLum = dot(irradiance, float3(0.2126, 0.7152, 0.0722));

	// Tunable: irradiance level below which specular IBL begins to fade.
    const float specGateThreshold = 0.005;
    float specGate = smoothstep(0.0, specGateThreshold, irradianceLum);

	// Gate dielectric IBL specular to kill bleed in dim areas, 
#if USE_PBR
	float specVisibility = lerp( specGate, 1.0, metallic );
#else
    float metalnessSignal = saturate((max(max(specularColor.r, specularColor.g), specularColor.b) - 0.04) / 0.96);
    float specVisibility = lerp(specGate, 1.0, metalnessSignal);
#endif

    float specAO = ComputeSpecularAO(vDotN, ao, roughness);
    float3 specularLight = radiance * specularScatter * specAO * specVisibility * reflOcclusion * pc.rpSpecularModifier.xyz;

	// Marmoset Horizon Fade trick
    const half horizonFade = 1.3;
    half horiz = saturate( 1.0 + horizonFade * dot3(reflectionVector, globalNormal)); // signed dot
    horiz *= horiz;
	//horiz = clamp( horiz, 0.0, 1.0 );

	//float3 lightColor = sRGBToLinearRGB( pc.rpAmbientColor.rgb );
	float3 lightColor = ( pc.rpAmbientColor.rgb );

	//result.color.rgb = diffuseLight;
	//result.color.rgb = diffuseLight * lightColor;
	//result.color.rgb = specularLight;
    result.color.rgb = (diffuseLight * lightColor + specularLight * horiz) * fragment.color.rgb;
	//result.color.rgb = localNormal.xyz * 0.5 + 0.5;
	//result.color.rgb = float3( ao );
	result.color.w = fragment.color.a;

	// ---- track F ( F2 ): the surface's listed lights, after the ambient term. rpSurfLights says which ----
	const uint llFirst = uint( pc.rpSurfLights.x );				// the first pool word, the run length, the path
	const uint llCount = uint( pc.rpSurfLights.y );
	const uint llPath = uint( pc.rpSurfLights.z );
	const uint2 llPixel = uint2( max( fragment.position.xy - pc.rpForwardView.xy, float2( 0.0, 0.0 ) ) );	// the tiles count from the view's corner
	const float llViewZ = fragment.texcoord7.w;					// the linear view depth from the vertex stage ( D54 )
	LL_SURFACE llSurface;
	llSurface.position = fragment.texcoord7.xyz;
	llSurface.dPdx = ddx( fragment.texcoord7.xyz );
	llSurface.dPdy = ddy( fragment.texcoord7.xyz );
	// the vertex frame: texcoord4, 5 and 6 hold its x, y and z components ( tangent, bitangent, normal in .x, .y, .z )
	llSurface.tangent = float3( fragment.texcoord4.x, fragment.texcoord5.x, fragment.texcoord6.x );
	llSurface.bitangent = float3( fragment.texcoord4.y, fragment.texcoord5.y, fragment.texcoord6.y );
	llSurface.normal = float3( fragment.texcoord4.z, fragment.texcoord5.z, fragment.texcoord6.z );
	// the view vector: normalised per vertex and taken into the frame there ( texcoord8 ), normalised again per
	// pixel, as the legacy interaction stages do; and the position in the frame ( texcoord9 )
	llSurface.positionTS = fragment.texcoord9.xyz;
	llSurface.viewTS = normalize( fragment.texcoord8.xyz );
	// the two bodies' contrast: 0.8 head-on to 1.2 ( interaction.ps.hlsl ) or to 1.0 ( interactionSM.ps.hlsl ) at
	// grazing, against the vertex normal
	const float llGrazing = 1.0 - saturate( llSurface.viewTS.z );
	float3 llNormal = llLocalNormal;
	llNormal.xy *= lerp( 0.8, 1.2, llGrazing );
	llSurface.nPlain = normalize( llNormal );
	llNormal = llLocalNormal;
	llNormal.xy *= lerp( 0.8, 1.0, llGrazing );
	llSurface.nShadowed = normalize( llNormal );
	llSurface.vdotNPlain = clamp( dot( llSurface.viewTS, llSurface.nPlain ), 0.0, 1.0 );
	llSurface.vdotNShadowed = clamp( dot( llSurface.viewTS, llSurface.nShadowed ), 0.0, 1.0 );
	// the interaction shaders' material ( their roughness is not the ambient term's under USE_PBR )
#if USE_PBR
	const float llRoughness = max( 0.045, specMapSRGB.r );
	const float llMetallic = specMapSRGB.g;
	llSurface.diffuseColor = diffuseMap * ( 1.0 - llMetallic );
	llSurface.diffuseNoSpecular = diffuseMap;
	llSurface.specularColor = lerp( _float3( 0.04 ), diffuseMap, llMetallic );
#else
	float llRoughness;
	llSurface.diffuseColor = diffuseMap;
	llSurface.diffuseNoSpecular = diffuseMap;
	PBRFromSpecmap( specMapSRGB.rgb, llSurface.specularColor, llRoughness );
#endif
	llSurface.rr = llRoughness * llRoughness;
	llSurface.rrrr = llSurface.rr * llSurface.rr;
	llSurface.energyPlain = 1.0 + llSurface.specularColor * ( 1.0 / max( EnvBRDFEnergyApprox( llRoughness, llSurface.vdotNPlain ), 1e-3 ) - 1.0 );
	llSurface.energyShadowed = 1.0 + llSurface.specularColor * ( 1.0 / max( EnvBRDFEnergyApprox( llRoughness, llSurface.vdotNShadowed ), 1e-3 ) - 1.0 );
	// the ambient pass's modifiers hold the surface's stage colours times r_lightScale, which a record's colour holds too
	llSurface.diffuseStage = pc.rpDiffuseModifier.xyz * pc.rpForwardView.z;
	llSurface.specularStage = pc.rpSpecularModifier.xyz * pc.rpForwardView.z;
	llSurface.vertexColor = fragment.color.rgb;
	// interactionSM.ps.hlsl's BlueNoise: the shadow disk's rotation, from the pixel alone. Its frac( x ) is written
	// x - floor( x ) here: main has a local variable named frac
	const float llNoise = t_Jitter.Sample( s_PointWrap, fragment.position.xy * pc.rpShadowFilter.z ).r + c_goldenRatioConjugate * pc.rpShadowFilter.w;
	llSurface.vogelPhi = llNoise - floor( llNoise );
	if( pc.rpForwardView.w == 0.0 )								// 1: the twin mode, the legacy pass lights these pairs
	{
		result.color.rgb += LL_AccumulateLights( llPath, llFirst, llCount, llPixel, llViewZ, llSurface );
	}
	const uint llShow = uint( pc.rpZBinScale.w );
	if( llShow >= 2 )											// uniform: off, the views cost one branch per pixel
	{
		result.color.rgb = LL_DebugView( llShow, llPath, llFirst, llCount, llPixel, llViewZ );
	}
	// ---- end track F ----
}
