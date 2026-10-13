/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013-2020 Robert Beckebans
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

// Math-driven light falloff, shape, and patterns for the math light extension.

//   * Falloff coordinate read from texcoord2.x (the original read .z, which
//     the vertex shaders hardwire to 0.0 — every curve evaluated at t = 0)

//   * Point lights compute a true normalized radial distance

//   * exponential / raw invsquare are windowed to reach 0 at the volume edge

//   * Patterns use a 3D light-local domain, FBM noise, Voronoi caustics, and
//     an fwidth() fade. (fwidth makes this include pixel-shader only)

//   * Energy clamp applies to falloff BEFORE the pattern multiply

//   * flags.x carries a debug mode (r_showMathLights)

//
//   * the energy-clamp-OFF path is bounded below the
//     fp16 render-target ceiling. Without a ceiling, raw invsquare at u->0 (~1e6) overflows the
//     RGBA16F HDR buffer / envprobe  bake to +Inf and NaNs the bloom chain. See MLEnergyClamp below.
//
// Uniform packing (unchanged, RENDERPARM_USER0/1/2):
//   pc.rpUser0 = (mode, type, falloff, pattern)
//                 mode: 0 = legacy path, 1 = math on,
//                       2 = debug falloff t, 3 = debug mask, 4 = debug pattern
//   pc.rpUser1 = (falloffExp, innerRadius, shapeExp, penumbra)
//   pc.rpUser2 = (patternStrength, patternScale, patternPhase, energyClamp)
//
// COORDINATE CONTRACT (verified against RenderWorld_defs.cpp and the stock
// interaction vertex shaders — do not "fix" this back to .z):
//   texcoord2 = ( falloffS, 0.5, 0.0, 1.0 )
//       falloffS = dot4( position, rpLightFalloffS )
//       point/parallel: falloffS = 0.5 + z / (2 * lightRadius.z)   [0.5 at source]
//       spot:           falloffS = ( dAxis - zNear ) / zFar        [linear along beam]
//   texcoord3 = ( s, t, 0.0, q )
//       s/q, t/q in [0,1] across the light footprint
//       point/parallel: s/q = 0.5 + x / (2 * lightRadius.x), q = 1
//

#define ML_POINT  0
#define ML_SPOT   1
#define ML_SUN    2

#define MLF_INVSQUARE          0
#define MLF_LINEAR             1
#define MLF_SMOOTH             2
#define MLF_EXPONENTIAL        3
#define MLF_INVSQUARE_CLAMPED  4

#define MLP_NONE        0
#define MLP_SUBSURFACE  1
#define MLP_ICE         2
#define MLP_CAUSTIC     3
#define MLP_BANDED      4
#define MLP_GRANULAR    5

// Ceiling for the energy-clamp-OFF (HDR) path
// Below fp16 overflow (65504) with generous headroom for the specular term
// that multiplies this. 64 still blooms a hot core to full white; bloom can't
// tell 64 from 65504, but the buffer and the envprobe bake very much can.
#define ML_HDR_CEIL     64.0



float MLHash31(float3 p)
{
    p = frac(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return frac((p.x + p.y) * p.z);
}

float3 MLHash33(float3 p)
{
    p = frac(p * float3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return frac((p.xxy + p.yxx) * p.zyx);
}

float MLNoise3D(float3 p)
{
    float3 i = floor(p);
    float3 f = frac(p);
    float3 u = f * f * (3.0 - 2.0 * f);

    float n000 = MLHash31(i);
    float n100 = MLHash31(i + float3(1.0, 0.0, 0.0));
    float n010 = MLHash31(i + float3(0.0, 1.0, 0.0));
    float n110 = MLHash31(i + float3(1.0, 1.0, 0.0));
    float n001 = MLHash31(i + float3(0.0, 0.0, 1.0));
    float n101 = MLHash31(i + float3(1.0, 0.0, 1.0));
    float n011 = MLHash31(i + float3(0.0, 1.0, 1.0));
    float n111 = MLHash31(i + float3(1.0, 1.0, 1.0));

    float nx00 = lerp(n000, n100, u.x);
    float nx10 = lerp(n010, n110, u.x);
    float nx01 = lerp(n001, n101, u.x);
    float nx11 = lerp(n011, n111, u.x);

    float nxy0 = lerp(nx00, nx10, u.y);
    float nxy1 = lerp(nx01, nx11, u.y);

    return lerp(nxy0, nxy1, u.z);
}

// 3 octaves, normalized to [0,1]
float MLFbm3(float3 p)
{
    float a = 0.5;
    float v = 0.0;
    for (int i = 0; i < 3; i++)
    {
        v += a * MLNoise3D(p);
        p = p * 2.17 + float3(11.5, 7.3, 5.1);
        a *= 0.5;
    }
    return v * (1.0 / 0.875);
}

// 3D Voronoi: distance to nearest (f1) and second-nearest (f2) feature point
// 27-cell search — acceptable for hand-placed effect lights
void MLVoronoiF12(float3 p, out float f1, out float f2)
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
                float3 feature = o + MLHash33(cell + o) - fp;
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
//   kind   - one of MLF_*
//   k      - free exponent / sharpness (falloffExp)
//   inner  - soft inner edge (0..1); below this, atten is ~1
//
// Every curve returns exactly 0.0 at t = 1.0. linear / smooth /
// invsquare_clamped terminate by construction; exponential and raw
// invsquare are multiplied by the Karis window so the light can never
// pop against the engine's hard volume clip (triangle culling + scissor).
float MLFalloffCurve(float t, int kind, float k, float inner)
{
    float u = saturate((t - inner) / max(1.0 - inner, 1e-3));

    if (kind == MLF_LINEAR)
    {
        return 1.0 - u;
    }
    else if (kind == MLF_SMOOTH)
    {
        return 1.0 - smoothstep(0.0, 1.0, u);
    }
    else if (kind == MLF_EXPONENTIAL)
    {
        float w = saturate(1.0 - u * u * u * u);
        return (w * w) * exp(-k * u);
    }
    else if (kind == MLF_INVSQUARE_CLAMPED)
    {
        // Karis-style windowed inverse square: bounded at u=0, zero at u=1.
        float w = saturate(1.0 - u * u * u * u);
        float d = u * k + 1e-3;
        return (w * w) / (d * d + 1.0);
    }
    else 
    {   // MLF_INVSQUARE
        // True inverse-square near-source behavior (blows up at u=0; only
        // safe with energyClamp — or the ML_HDR_CEIL bound when clamp is off),
        // but windowed so it still reaches 0 at the volume edge instead of
        // popping at the clip boundary
        float w = saturate(1.0 - u * u * u * u);
        float d = u * k + 1e-3;
        return (w * w) / (d * d);
    }
}



// SPOT SHAPE (superellipse / Lamé curve) — unchanged, this was correct.
float MLSpotMask(float2 uv, float k, float penumbra)
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
float3 MLPattern(float3 p, float t, int kind, float strength, float scale, float phase)
{
    if (kind == MLP_NONE || strength <= 0.0)
    {
        return float3(1.0, 1.0, 1.0);
    }

    if (kind == MLP_SUBSURFACE)
    {
        // low frequency — no anti-alias fade needed
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

    if (kind == MLP_ICE)
    {
        float n = MLFbm3(p * scale * 8.0 + phase);
        float mod = lerp(1.0, 0.55 + 0.9 * n, strength);
        result = float3(mod, mod * 1.02, mod * 1.08);
    }
    else if (kind == MLP_CAUSTIC)
    {
        // bright filament web on Voronoi cell borders (f2 - f1 -> 0 at borders),
        // broken up by a layer of fine value noise
        float f1, f2;
        MLVoronoiF12(p * scale * 6.0 + phase, f1, f2);

        float web = pow(saturate(1.0 - (f2 - f1) * 2.5), 6.0);
        float n = MLNoise3D(p * scale * 17.0 + phase * 1.3);
        float c = saturate(web * (0.7 + 0.6 * n));

        float mod = 1.0 + (c - 0.25) * 1.6 * strength;
        result = float3(mod, mod, mod);
    }
    else if (kind == MLP_BANDED)
    {
        float ring = sin(t * scale * 20.0 + phase);
        float mod = 1.0 + ring * strength * 0.4;
        result = float3(mod, mod, mod);
    }
    else if (kind == MLP_GRANULAR)
    {
        float n = MLHash31(floor(p * scale * 32.0) + phase);
        float mod = lerp(1.0, n * 1.4 + 0.3, strength);
        result = float3(mod, mod, mod);
    }

    return lerp(float3(1.0, 1.0, 1.0), result, fade);
}



// ENERGY CLAMP for PBR compatibility.
float3 MLEnergyClamp(float3 rgb, bool clampOn)
{
    if (!clampOn)
    {
        return min(rgb, _float3(ML_HDR_CEIL));
    }
    // Per-channel clamp to [0, 1] matches the legacy texture-sampled range
    // exactly, so a math light behavior = texture light
    return saturate(rgb);
}



// TOP-LEVEL ENTRY.
// Returns true when math mode is active; the interaction shaders run the
// legacy texture path on false. Outputs are LINEAR — the caller must NOT
// run them through sRGBToLinearRGB
bool MLEvaluate(
    float4 tc2, float4 tc3,
    float4 flags, float4 shape, float4 pattern,
    out float3 lightFalloffRGB,
    out float3 lightProjRGB)
{
    if (flags.x < 0.5)
    {
        lightFalloffRGB = float3(1.0, 1.0, 1.0);
        lightProjRGB = float3(1.0, 1.0, 1.0);
        return false;
    }

    int debugMode = int(flags.x + 0.5) - 1; // 0 = off, 1 = t, 2 = mask, 3 = pattern

    int type = int(flags.y + 0.5);
    int falloffId = int(flags.z + 0.5);
    int patternId = int(flags.w + 0.5);

    float falloffExp = shape.x;
    float innerRadius = shape.y;
    float shapeExp = shape.z;
    float penumbra = shape.w;

    float pStrength = pattern.x;
    float pScale = pattern.y;
    float pPhase = pattern.z;
    bool energyClamp = pattern.w > 0.5;

    // COORDINATES
    // The falloff texgen lives in tc2.x. tc2.z is a hardwired 0.0 in the
    // vertex shaders 
    float falloffS = saturate(tc2.x / max(tc2.w, 1e-4));

    float2 uv;
    uv.x = (tc3.x / max(tc3.w, 1e-4)) * 2.0 - 1.0;
    uv.y = (tc3.y / max(tc3.w, 1e-4)) * 2.0 - 1.0;

    float axial = falloffS * 2.0 - 1.0; // [-1,1] along the volume axis

    float t;
    if (type == ML_POINT)
    {
      
        // genuine spherical falloff 
        t = saturate(length(float3(uv, axial)));
    }
    else
    {
        // Spot: linear ramp along the beam, light_start -> light_end.
        // Sun: t is unused by the falloff but still drives banded/subsurface.
        t = falloffS;
    }

    // 3D light-local pattern domain
    float3 patternDomain = float3(uv, axial);

    // ---- FALLOFF AND MASK
    float falloff = 1.0;
    float mask = 1.0;

    if (type == ML_SUN)
    {
        falloff = 1.0;
        mask = 1.0;
    }
    else if (type == ML_SPOT)
    {
        falloff = MLFalloffCurve(t, falloffId, falloffExp, innerRadius);
        mask = MLSpotMask(uv, shapeExp, penumbra);
    }
    else // ML_POINT
    {
        falloff = MLFalloffCurve(t, falloffId, falloffExp, innerRadius);
        mask = 1.0;
    }

    // COMBINE 
    // With the old order, saturate(1e6 * 0.7) == 1.0 — darkening patterns vanished anywhere the
    // raw curve exceeded 1. New order: saturate(1e6) * 0.7 == 0.7.
    float3 falloffRGB = MLEnergyClamp(float3(falloff, falloff, falloff), energyClamp);

    float3 patternRGB = MLPattern(patternDomain, t, patternId, pStrength, pScale, pPhase);

    lightFalloffRGB = MLEnergyClamp(falloffRGB * patternRGB, energyClamp);
    lightProjRGB = float3(mask, mask, mask);

    // ---- DEBUG (r_showMathLights) ------------------------------------------
    // Output still passes through the BRDF/shadow/color pipeline, so this is
    // a field visualization over lit surfaces, not a flat overlay
    if (debugMode == 1)
    {
        lightFalloffRGB = float3(t, t, t);
        lightProjRGB = float3(1.0, 1.0, 1.0);
    }
    else if (debugMode == 2)
    {
        lightFalloffRGB = float3(mask, mask, mask);
        lightProjRGB = float3(1.0, 1.0, 1.0);
    }
    else if (debugMode == 3)
    {
        lightFalloffRGB = patternRGB;
        lightProjRGB = float3(1.0, 1.0, 1.0);
    }

    return true;
}