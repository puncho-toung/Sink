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

/*
================================================================================
MathLight.cpp

Shared spawn-arg parsing for math-driven lights. Both the renderer
(idGameEdit::ParseSpawnArgsToRenderLight) and the ImGui light editor
(LightInfo::FromDict / ToDict) call into these helpers so the math_*
key contract has exactly one implementation.
================================================================================
*/
#include "precompiled.h"
#pragma hdrstop

#include "MathLight.h"

void ML_ParseFromDict(const idDict* args, mathLightData_t* m)
{
    // The renderer seeds from renderLight_t::parallel/pointLight
    // (Light.cpp, ParseSpawnArgsToRenderLight); the editor seeds from
    // LightInfo::lightType (LightEditor.cpp, FromDict). An explicit
    // "math_type" key still overrides the seed below.
    const int seededType = idMath::ClampInt(0, 2, m->type);

    // Clear first so toggling math_enabled off leaves no stale state
    memset(m, 0, sizeof(*m));

    // a cleared style would read as style 0 ("Normal"), so a disabled block has none
    m->style = -1;

    m->enabled = args->GetBool("math_enabled", "0");
    if (!m->enabled)
    {
        return;
    }

    
    idStr typeStr = args->GetString("math_type", "");
    if (typeStr.Icmp("spot") == 0) { m->type = ML_SPOT; }
    else if (typeStr.Icmp("sun") == 0 || typeStr.Icmp("parallel") == 0) { m->type = ML_SUN; }
    else if (typeStr.Icmp("point") == 0) { m->type = ML_POINT; }
    else
    {
        if (typeStr.Length() > 0)
        {
            idLib::Warning("math_type '%s' not recognized; falling back to inferred light type", typeStr.c_str());
        }
        m->type = seededType;
    }
    

    // falloff curve
    idStr falloffStr = args->GetString("math_falloff", "invsquare_clamped");
    if (falloffStr.Icmp("linear") == 0) { m->falloff = MLF_LINEAR; }
    else if (falloffStr.Icmp("smooth") == 0) { m->falloff = MLF_SMOOTH; }
    else if (falloffStr.Icmp("exponential") == 0) { m->falloff = MLF_EXPONENTIAL; }
    else if (falloffStr.Icmp("invsquare") == 0) { m->falloff = MLF_INVSQUARE; }
    else { m->falloff = MLF_INVSQUARE_CLAMPED; }

    args->GetFloat("math_falloff_exp", "2.0", m->falloffExp);
    args->GetFloat("math_inner_radius", "0.0", m->innerRadius);
    args->GetFloat("math_shape_exp", "2.0", m->shapeExp);
    args->GetFloat("math_penumbra", "0.15", m->penumbra);

    m->shapeExp = idMath::ClampFloat(1.5f, 32.0f, m->shapeExp);
    m->penumbra = idMath::ClampFloat(0.0f, 1.0f, m->penumbra);
    m->falloffExp = idMath::ClampFloat(0.1f, 16.0f, m->falloffExp);
    m->innerRadius = idMath::ClampFloat(0.0f, 1.0f, m->innerRadius);

    // pattern
    idStr patternStr = args->GetString("math_pattern", "none");
    if (patternStr.Icmp("subsurface") == 0) { m->pattern = MLP_SUBSURFACE; }
    else if (patternStr.Icmp("ice") == 0) { m->pattern = MLP_ICE; }
    else if (patternStr.Icmp("caustic") == 0) { m->pattern = MLP_CAUSTIC; }
    else if (patternStr.Icmp("banded") == 0) { m->pattern = MLP_BANDED; }
    else if (patternStr.Icmp("granular") == 0) { m->pattern = MLP_GRANULAR; }
    else { m->pattern = MLP_NONE; }

    args->GetFloat("math_pattern_strength", "0.0", m->patternStrength);
    args->GetFloat("math_pattern_scale", "1.0", m->patternScale);
    args->GetFloat("math_pattern_phase", "0.0", m->patternPhase);

    m->patternStrength = idMath::ClampFloat(0.0f, 1.0f, m->patternStrength);

    m->energyClamp = args->GetBool("math_energy_clamp", "1");

    // D62: an index outside the table is no style, as an unknown falloff or pattern name is the default
    const int style = args->GetInt("math_style", "-1");
    m->style = (style >= 0 && style < ML_NUM_STYLES) ? style : -1;
}


void ML_WriteToDict(idDict* args, const mathLightData_t& m, bool deleteWhenDisabled)
{
    // LightEditor.cpp uses the convention `e->Set( key, "" )` (empty string)
    // to mark a key for removal (DELETE_VAL at LightEditor.cpp)
    // We follow that idiom rather than calling idDict::Delete directly so that
    // editor-side and renderer-side calls behave identically
    static const char* const DELETE_VAL = "";

    if (!m.enabled)
    {
        if (deleteWhenDisabled)
        {
            args->Set("math_enabled", DELETE_VAL);
            args->Set("math_type", DELETE_VAL);
            args->Set("math_falloff", DELETE_VAL);
            args->Set("math_falloff_exp", DELETE_VAL);
            args->Set("math_inner_radius", DELETE_VAL);
            args->Set("math_shape_exp", DELETE_VAL);
            args->Set("math_penumbra", DELETE_VAL);
            args->Set("math_pattern", DELETE_VAL);
            args->Set("math_pattern_strength", DELETE_VAL);
            args->Set("math_pattern_scale", DELETE_VAL);
            args->Set("math_pattern_phase", DELETE_VAL);
            args->Set("math_energy_clamp", DELETE_VAL);
            args->Set("math_style", DELETE_VAL);
        }
        else
        {
            // D51: the tab is the switch; the other math keys stay as they are, so switching back is free
            args->Set("math_enabled", "0");
        }
        return;
    }

    args->Set("math_enabled", "1");

    static const char* const typeNames[] = { "point", "spot", "sun" };
    static const char* const falloffNames[] = {
        "invsquare", "linear", "smooth", "exponential", "invsquare_clamped"
    };
    static const char* const patternNames[] = {
        "none", "subsurface", "ice", "caustic", "banded", "granular"
    };

    args->Set("math_type", typeNames[idMath::ClampInt(0, 2, m.type)]);
    args->Set("math_falloff", falloffNames[idMath::ClampInt(0, 4, m.falloff)]);
    args->Set("math_pattern", patternNames[idMath::ClampInt(0, 5, m.pattern)]);

    args->SetFloat("math_falloff_exp", m.falloffExp);
    args->SetFloat("math_inner_radius", m.innerRadius);
    args->SetFloat("math_shape_exp", m.shapeExp);
    args->SetFloat("math_penumbra", m.penumbra);
    args->SetFloat("math_pattern_strength", m.patternStrength);
    args->SetFloat("math_pattern_scale", m.patternScale);
    args->SetFloat("math_pattern_phase", m.patternPhase);

    args->SetBool("math_energy_clamp", m.energyClamp);

    // D54: written like "style", only when there is one
    if (m.style != -1)
    {
        args->SetInt("math_style", m.style);
    }
    else
    {
        args->Set("math_style", DELETE_VAL);
    }
}

/*
================
ML_StyleMultiplier

D62: the brightness a light-style string gives at a game time. The a-z characters play one per
frameTimeMs and are interpolated as idLight::SharedThink interpolates "style", scaled so that
'a' = 0, 'm' = 1 and 'z' = 25/12 (characters outside a-z are clamped to them); an empty string is 1.
A pure function of the clock, so every peer and le_selfTest get the same value.
================
*/
float ML_StyleMultiplier(const char* style, int timeMs, int frameTimeMs)
{
    const int length = idStr::Length(style);
    if (length <= 0)
    {
        return 1.0f;
    }

    // ls_frametime comes from the map; zero would divide by zero
    frameTimeMs = Max(frameTimeMs, 1);
    timeMs = Max(timeMs, 0);

    const int step = timeMs / frameTimeMs;
    const int frame = step % length;
    const int nextFrame = (frame + 1) % length;
    const float backLerp = (float)(timeMs - step * frameTimeMs) / (float)frameTimeMs;

    const int lastCh = idMath::ClampInt(0, 25, style[frame] - 'a');
    const int nextCh = idMath::ClampInt(0, 25, style[nextFrame] - 'a');

    return (lastCh * (1.0f - backLerp) + nextCh * backLerp) * (1.0f / 12.0f);
}