/*
================================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993 - 2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013 - 2021 Robert Beckebans
Copyright (C) 2016 - 2017 Dustin Land
Copyright (C) 2022 Stephen Pridham
Copyright (C) 2026 Phillip Melnichenko

This file is part of the Doom 3 BFG Edition GPL Source Code("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software : you can redistribute it and /or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.If not, see < http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms.You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c / o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

================================================================================
*/
#ifndef __MATHLIGHT_H__
#define __MATHLIGHT_H__

#include "../idlib/Dict.h"
/*
================================================================================

       Math Light

================================================================================
*/
enum mathLightType_t : int
{
    ML_POINT = 0,
    ML_SPOT = 1,
    ML_SUN = 2,
};

enum mathFalloff_t : int
{
    MLF_INVSQUARE = 0,
    MLF_LINEAR = 1,
    MLF_SMOOTH = 2,
    MLF_EXPONENTIAL = 3,
    MLF_INVSQUARE_CLAMPED = 4,
};

enum mathPattern_t : int
{
    MLP_NONE = 0,
    MLP_SUBSURFACE = 1,
    MLP_ICE = 2,
    MLP_CAUSTIC = 3,
    MLP_BANDED = 4,
    MLP_GRANULAR = 5,
};

// D62: math_style indexes the same table as the light's "style" key, idLight's light_styles (by
// default the twelve predef_lightstyles of renderer/Material.h); -1 is no style
const int ML_NUM_STYLES = 12;


// LAYOUT IS LOAD-BEARING. id Software relies on a memcmp of renderLight_t
// to detect redundant light updates

struct mathLightData_t
{
    int     type;               // mathLightType_t
    int     falloff;            // mathFalloff_t
    int     pattern;            // mathPattern_t
    int     style;              // math_style, -1 or 0..ML_NUM_STYLES-1; game side only, never uploaded (D62)

    float   falloffExp;
    float   innerRadius;
    float   shapeExp;
    float   penumbra;
    float   patternStrength;
    float   patternScale;
    float   patternPhase;

    bool    enabled;           
    bool    energyClamp;        // clamp falloff
    bool    _boolPad0;          // unused; init to 0
    bool    _boolPad1;          // unused; init to 0
};

static_assert(sizeof(mathLightData_t) == 48,
    "mathLightData_t must be tightly packed for renderLight_t memcmp safety");
static_assert(sizeof(mathLightData_t) <= 64,
    "mathLightData_t must fit in one cache line");
 
// Shared helpers for MathLight.cpp
void ML_ParseFromDict(const idDict* args, mathLightData_t* m);
void ML_WriteToDict(idDict* args, const mathLightData_t& m, bool deleteWhenDisabled);
float ML_StyleMultiplier(const char* style, int timeMs, int frameTimeMs);

#endif 