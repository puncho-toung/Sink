/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
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

//puncho start
// Emitter lights: shaded from the lamp, not from the volume.
//
// The light's texgens are a box round the bulb for every emitter, so
//   p = ( 2 * tc3.x - 1, 2 * tc3.y - 1, 2 * tc2.x - 1 )
// is the light-local position in units of the box radius: p.x along the beam (axis[0]), p.y along
// axis[1], p.z along axis[2]. Box texgens are affine, so the interpolants are exact and both w are 1,
// which is why the position is not divided by them.
//
// interaction.ps.hlsl and interactionSM.ps.hlsl call EMEvaluate; interactionAmbient.ps.hlsl does not:
// that program is bound only for an ambient light material, and an emitter's material is always
// lights/defaultPointLight (G0 section 8).
//
// Uniform packing (RENDERPARM_USER0/1/2), done once per view light in R_AddSingleLight
// (tr_frontend_addlights.cpp):
//   rpUser0 = ( mode, ids, cotH, cotV )
//       mode: below 8 is not an emitter; 8 + debug view (0 off, 1 t, 2 mask, 3 pattern,
//             4 unlit inside the volume), plus 16 when the early-out is allowed
//       ids:  shape + 8 * falloff + 64 * pattern
//       cotH, cotV: a spot's cotangents of the half beam angles. An omni or a sun has no beam, so
//             there they hold the box's proportions radius.y / radius.x and radius.z / radius.x
//   rpUser1 = ( k, inner, squareness, penumbra )
//   rpUser2 = ( patternStrength, patternScale, patternPhase, energyClamp )
//       patternScale: emit_pattern_scale * radius.x / 256 (radius.x is the range for a spot)
//
// Patterns are in world units round the bulb (G0 section 5, "Pattern position"): the pattern takes
// ( p.x, p.y * ry, p.z * rz ), the position in units of radius.x, so with the scale above a cell is
// the same size in the world whatever the light's range or box. emit_pattern_scale 1 is what a light
// of radius 256 showed before; banded takes the length of that vector, so its rings are spheres round
// the bulb; subsurface keeps t, a tint toward the end of the range.
//
// The falloff curves, the beam mask, the patterns and the energy clamp below are what EMEvaluate
// calls. The EMF_* and EMP_* numbers are those of emitterFalloff_t and emitterPattern_t in
// Emitter.h, which the frontend packs into rpUser0.y.

#define EMF_INVSQUARE          0
#define EMF_LINEAR             1
#define EMF_SMOOTH             2
#define EMF_EXPONENTIAL        3
#define EMF_INVSQUARE_CLAMPED  4

#define EMP_NONE        0
#define EMP_SUBSURFACE  1
#define EMP_ICE         2
#define EMP_CAUSTIC     3
#define EMP_BANDED      4
#define EMP_GRANULAR    5

// Ceiling for the energy-clamp-OFF (HDR) path
// Below fp16 overflow (65504) with generous headroom for the specular term
// that multiplies this. 64 still blooms a hot core to full white; bloom can't
// tell 64 from 65504, but the buffer and the envprobe bake very much can.
#define EM_HDR_CEIL     64.0



float EMHash31(float3 p)
{
    p = frac(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return frac((p.x + p.y) * p.z);
}

float3 EMHash33(float3 p)
{
    p = frac(p * float3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return frac((p.xxy + p.yxx) * p.zyx);
}

float EMNoise3D(float3 p)
{
    float3 i = floor(p);
    float3 f = frac(p);
    float3 u = f * f * (3.0 - 2.0 * f);

    float n000 = EMHash31(i);
    float n100 = EMHash31(i + float3(1.0, 0.0, 0.0));
    float n010 = EMHash31(i + float3(0.0, 1.0, 0.0));
    float n110 = EMHash31(i + float3(1.0, 1.0, 0.0));
    float n001 = EMHash31(i + float3(0.0, 0.0, 1.0));
    float n101 = EMHash31(i + float3(1.0, 0.0, 1.0));
    float n011 = EMHash31(i + float3(0.0, 1.0, 1.0));
    float n111 = EMHash31(i + float3(1.0, 1.0, 1.0));

    float nx00 = lerp(n000, n100, u.x);
    float nx10 = lerp(n010, n110, u.x);
    float nx01 = lerp(n001, n101, u.x);
    float nx11 = lerp(n011, n111, u.x);

    float nxy0 = lerp(nx00, nx10, u.y);
    float nxy1 = lerp(nx01, nx11, u.y);

    return lerp(nxy0, nxy1, u.z);
}

// 3 octaves, normalized to [0,1]
float EMFbm3(float3 p)
{
    float a = 0.5;
    float v = 0.0;
    for (int i = 0; i < 3; i++)
    {
        v += a * EMNoise3D(p);
        p = p * 2.17 + float3(11.5, 7.3, 5.1);
        a *= 0.5;
    }
    return v * (1.0 / 0.875);
}

// 3D Voronoi: distance to nearest (f1) and second-nearest (f2) feature point
// 27-cell search -- acceptable for hand-placed effect lights
void EMVoronoiF12(float3 p, out float f1, out float f2)
{
    float3 cell = floor(p);
    float3 fp = frac(p);

    f1 = 8.0;
    f2 = 8.0;

    for (int z = -1; z <= 1; z++)
    {
        for (int y = -1; y <= 1; y++)
        {
            for (int x = -1; x <= 1; x++)
            {
                float3 o = float3(x, y, z);
                float3 feature = o + EMHash33(cell + o) - fp;
                float d = dot(feature, feature);
                if (d < f1)
                {
                    f2 = f1;
                    f1 = d;
                }
                else if (d < f2)
                {
                    f2 = d;
                }
            }
        }
    }

    f1 = sqrt(f1);
    f2 = sqrt(f2);
}



// FALLOFF CURVES
//   t      - normalized distance, 0 at the source, 1 at the volume edge
//   kind   - one of EMF_*
//   k      - free exponent / sharpness (falloffExp)
//   inner  - soft inner edge (0..1); below this, atten is ~1
//
// Every curve returns exactly 0.0 at t = 1.0. linear / smooth /
// invsquare_clamped terminate by construction; exponential and raw
// invsquare are multiplied by the Karis window so the light can never
// pop against the engine's hard volume clip (triangle culling + scissor).
float EMFalloffCurve(float t, int kind, float k, float inner)
{
    float u = saturate((t - inner) / max(1.0 - inner, 1e-3));

    if (kind == EMF_LINEAR)
    {
        return 1.0 - u;
    }
    else if (kind == EMF_SMOOTH)
    {
        return 1.0 - smoothstep(0.0, 1.0, u);
    }
    else if (kind == EMF_EXPONENTIAL)
    {
        float w = saturate(1.0 - u * u * u * u);
        return (w * w) * exp(-k * u);
    }
    else if (kind == EMF_INVSQUARE_CLAMPED)
    {
        // Karis-style windowed inverse square: bounded at u=0, zero at u=1.
        float w = saturate(1.0 - u * u * u * u);
        float d = u * k + 1e-3;
        return (w * w) / (d * d + 1.0);
    }
    else 
    {   // EMF_INVSQUARE
        // True inverse-square near-source behavior (blows up at u=0; only
        // safe with energyClamp -- or the EM_HDR_CEIL bound when clamp is off),
        // but windowed so it still reaches 0 at the volume edge instead of
        // popping at the clip boundary
        float w = saturate(1.0 - u * u * u * u);
        float d = u * k + 1e-3;
        return (w * w) / (d * d);
    }
}



// SPOT SHAPE (superellipse / Lame curve) -- unchanged, this was correct.
float EMSpotMask(float2 uv, float k, float penumbra)
{
    float r = pow(pow(abs(uv.x), k) + pow(abs(uv.y), k), 1.0 / k);
    return 1.0 - smoothstep(1.0 - penumbra, 1.0, r);
}


// PATTERN MODULATORS
//==================================================================================
//   p        - 3D light-local domain: (uv.x, uv.y, axial), each in [-1,1].
//              Varies in all three directions, so patterns no longer extrude
//              as streaks down surfaces parallel to the projection axis
//----------------------------------------------------------------------------------
//   t        - normalized distance from the source (radial for point lights)
//==================================================================================
float3 EMPattern(float3 p, float t, int kind, float strength, float scale, float phase)
{
    if (kind == EMP_NONE || strength <= 0.0)
    {
        return float3(1.0, 1.0, 1.0);
    }

    if (kind == EMP_SUBSURFACE)
    {
        // low frequency -- no anti-alias fade needed
        float edge = smoothstep(0.5, 1.0, t);
        float3 warm = float3(1.0, 0.62, 0.45);
        return lerp(float3(1.0, 1.0, 1.0), warm, edge * strength);
    }

    // frequency fade: as pattern features shrink toward the pixel footprint,
    // blend back to flat 1.0 instead of letting the pattern alias and shimmer.
    // length(fwidth(p)) is the screen-space sampling step in domain units
    float fade = saturate(1.0 - length(fwidth(p)) * scale * 12.0);
    if (fade <= 0.0)
    {
        return float3(1.0, 1.0, 1.0);
    }

    float3 result = float3(1.0, 1.0, 1.0);

    if (kind == EMP_ICE)
    {
        float n = EMFbm3(p * scale * 8.0 + phase);
        float mod = lerp(1.0, 0.55 + 0.9 * n, strength);
        result = float3(mod, mod * 1.02, mod * 1.08);
    }
    else if (kind == EMP_CAUSTIC)
    {
        // bright filament web on Voronoi cell borders (f2 - f1 -> 0 at borders),
        // broken up by a layer of fine value noise
        float f1, f2;
        EMVoronoiF12(p * scale * 6.0 + phase, f1, f2);

        float web = pow(saturate(1.0 - (f2 - f1) * 2.5), 6.0);
        float n = EMNoise3D(p * scale * 17.0 + phase * 1.3);
        float c = saturate(web * (0.7 + 0.6 * n));

        float mod = 1.0 + (c - 0.25) * 1.6 * strength;
        result = float3(mod, mod, mod);
    }
    else if (kind == EMP_BANDED)
    {
        float ring = sin(t * scale * 20.0 + phase);
        float mod = 1.0 + ring * strength * 0.4;
        result = float3(mod, mod, mod);
    }
    else if (kind == EMP_GRANULAR)
    {
        float n = EMHash31(floor(p * scale * 32.0) + phase);
        float mod = lerp(1.0, n * 1.4 + 0.3, strength);
        result = float3(mod, mod, mod);
    }

    return lerp(float3(1.0, 1.0, 1.0), result, fade);
}



// ENERGY CLAMP for PBR compatibility.
float3 EMEnergyClamp(float3 rgb, bool clampOn)
{
    if (!clampOn)
    {
        return min(rgb, _float3(EM_HDR_CEIL));
    }
    // Per-channel clamp to [0, 1] matches the legacy texture-sampled range
    // exactly, so a clamped emitter stays in a light texture's range
    return saturate(rgb);
}

#define EM_OMNI		0
#define EM_SPOT		1
#define EM_SUN		2

// true when the emitter path ran. Outputs are linear.
// earlyOut is true when the caller may return black before the shadow taps.
bool EMEvaluate(
	float4 tc2, float4 tc3,
	float4 flags, float4 shape, float4 pattern,
	out float3 lightFalloffRGB,
	out float3 lightProjRGB,
	out bool earlyOut )
{
	int mode = int( flags.x + 0.5 );
	if( mode < 8 )
	{
		lightFalloffRGB = float3( 1.0, 1.0, 1.0 );
		lightProjRGB = float3( 1.0, 1.0, 1.0 );
		earlyOut = false;
		return false;
	}

	mode -= 8;
	bool earlyOutAllowed = false;
	if( mode >= 16 )
	{
		earlyOutAllowed = true;
		mode -= 16;
	}
	int debugMode = mode; // 0 off, 1 t, 2 mask, 3 pattern, 4 unlit inside the volume

	int ids = int( flags.y + 0.5 );
	int emShape = ids & 7;
	int falloffId = ( ids >> 3 ) & 7;
	int patternId = ids >> 6;

	float cotH = flags.z;
	float cotV = flags.w;

	float k = shape.x;
	float inner = shape.y;
	float squareness = shape.z;
	float penumbra = shape.w;

	float pStrength = pattern.x;
	float pScale = pattern.y;
	float pPhase = pattern.z;
	bool energyClamp = pattern.w > 0.5;

	// POSITION. An emitter's texgens are a box, which is affine, so tc3.w and tc2.w are exactly 1 and
	// the position needs no divide (G0 section 5)
	float3 p;
	p.x = 2.0 * tc3.x - 1.0;
	p.y = 2.0 * tc3.y - 1.0;
	p.z = 2.0 * tc2.x - 1.0;

	float t = saturate( length( p ) );

	// PATTERN first: EMPattern takes screen derivatives of its position, which are only defined while
	// every pixel of the quad is still running, so it comes before any select or return.
	// patternId and pStrength are uniform, so both branches below are taken the same way by every
	// pixel of the draw: a light without a pattern pays for none of it, only banded pays for the
	// length, and the derivatives inside EMPattern stay defined (DP-E4-03).
	float3 patternRGB = float3( 1.0, 1.0, 1.0 );
	[branch]
	if( patternId != EMP_NONE && pStrength > 0.0 )
	{
		// the position scaled to radius.x units on all three axes; a spot's radii are equal
		float ry = ( emShape == EM_SPOT ) ? 1.0 : flags.z;
		float rz = ( emShape == EM_SPOT ) ? 1.0 : flags.w;
		float3 patternPos = float3( p.x, p.y * ry, p.z * rz );
		float patternT = t;
		[branch]
		if( patternId == EMP_BANDED )
		{
			patternT = length( patternPos );
		}
		patternRGB = EMPattern( patternPos, patternT, patternId, pStrength, pScale, pPhase );
	}

	// FALLOFF AND MASK
	float falloff = 1.0;
	float mask = 1.0;

	if( emShape != EM_SUN )
	{
		falloff = EMFalloffCurve( t, falloffId, k, inner );
	}

	if( emShape == EM_SPOT )
	{
		// the divide is guarded and the zero behind the bulb is a select, never a branch on a per-pixel
		// value, so the pattern's derivatives above stay defined
		float a = p.x;
		float safeA = max( a, 1e-4 );
		float2 uv = float2( p.y * cotH / safeA, p.z * cotV / safeA );

		float beam;
		if( abs( squareness - 2.0 ) < 1e-3 )
		{
			// a round beam needs no pow()
			beam = 1.0 - smoothstep( 1.0 - penumbra, 1.0, length( uv ) );
		}
		else
		{
			beam = EMSpotMask( uv, squareness, penumbra );
		}

		mask = ( a > 0.0 ) ? beam : 0.0;
	}

	// COMBINE: clamp the curve, then the patterned curve
	float3 falloffRGB = EMEnergyClamp( float3( falloff, falloff, falloff ), energyClamp );

	lightFalloffRGB = EMEnergyClamp( falloffRGB * patternRGB, energyClamp );
	lightProjRGB = float3( mask, mask, mask );

	// the early-out is a debug cvar, off by default: only an allowed early-out pays for its test, and
	// both conditions are uniform
	earlyOut = false;
	[branch]
	if( earlyOutAllowed && debugMode == 0 )
	{
		earlyOut = ( falloff * mask <= 0.0 );
	}

	// DEBUG (r_showEmitters). Output still passes through the BRDF, shadow and colour, so this is a
	// field over lit surfaces, not a flat overlay
	if( debugMode == 1 )
	{
		lightFalloffRGB = float3( t, t, t );
		lightProjRGB = float3( 1.0, 1.0, 1.0 );
	}
	else if( debugMode == 2 )
	{
		lightFalloffRGB = float3( mask, mask, mask );
		lightProjRGB = float3( 1.0, 1.0, 1.0 );
	}
	else if( debugMode == 3 )
	{
		lightFalloffRGB = patternRGB;
		lightProjRGB = float3( 1.0, 1.0, 1.0 );
	}
	else if( debugMode == 4 )
	{
		// red where the volume was drawn but the lamp gives nothing: the price of the square
		// culling volume round a round beam
		if( falloff * mask <= 0.0 )
		{
			lightFalloffRGB = float3( 1.0, 0.0, 0.0 );
			lightProjRGB = float3( 1.0, 1.0, 1.0 );
		}
	}

	return true;
}
//puncho end
