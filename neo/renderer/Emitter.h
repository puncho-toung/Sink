/*
================================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993 - 2012 id Software LLC, a ZeniMax Media company.
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
#ifndef __EMITTER_H__
#define __EMITTER_H__

#include "../idlib/Dict.h"
/*
================================================================================

       Emitter

================================================================================
*/
enum emitterShape_t : int
{
	EM_OMNI = 0,
	EM_SPOT = 1,
	EM_SUN = 2,
};

// the numbers emitter.inc.hlsl decodes from rpUser0.y (EMF_* there)
enum emitterFalloff_t : int
{
	EMF_INVSQUARE = 0,
	EMF_LINEAR = 1,
	EMF_SMOOTH = 2,
	EMF_EXPONENTIAL = 3,
	EMF_INVSQUARE_CLAMPED = 4,
};

// the numbers emitter.inc.hlsl decodes from rpUser0.y (EMP_* there)
enum emitterPattern_t : int
{
	EMP_NONE = 0,
	EMP_SUBSURFACE = 1,
	EMP_ICE = 2,
	EMP_CAUSTIC = 3,
	EMP_BANDED = 4,
	EMP_GRANULAR = 5,
};

// 0 = none; 1..11 = predef_lightstyles[ n ] of renderer/Material.h; then the three waveforms
enum emitterEffect_t : int
{
	EME_NONE = 0,
	EME_STYLE_LAST = 11,
	EME_STROBE = 12,
	EME_PULSE = 13,
	EME_FLICKER = 14,
	EME_NUM_EFFECTS = 15,
};

// POD, no padding holes. 76 bytes.
struct emitterData_t
{
	int		shape;				// emitterShape_t
	int		falloff;			// emitterFalloff_t
	int		pattern;			// emitterPattern_t
	int		effect;				// emitterEffect_t; game side only, never uploaded

	float	size;				// world units
	float	start;				// world units
	//puncho start - RV2-EA-04: the renderer reads only the two tangents, never the angles. A producer
	// that sets coneH or coneV sets tanHalfH and tanHalfV with them; EM_ParseFromDict is the only one
	// whose block reaches the renderer today. A block with an angle and a zero tangent derives a flat
	// culling frustum and a beam the frontend clamps to tan 1e-4, not the authored cone
	//puncho end
	float	coneH;				// full angle in degrees, as authored
	float	coneV;
	float	tanHalfH;			// derived by EM_ParseFromDict: tan( coneH / 2 )
	float	tanHalfV;
	float	penumbra;
	float	squareness;
	float	patternStrength;
	float	patternScale;
	float	patternPhase;
	float	effectSpeed;
	float	effectAmount;
	float	effectPhase;

	bool	enabled;
	bool	energyClamp;
	bool	_boolPad0;			// always 0
	bool	_boolPad1;			// always 0
};

static_assert( sizeof( emitterData_t ) == 76, "emitterData_t must have no padding" );

void	EM_ParseFromDict( const idDict* args, emitterData_t* e );
void	EM_ParseAllFromDict( const idDict* args, emitterData_t* e );	// every key, for the editors ( DP-EC-01 )
void	EM_WriteToDict( idDict* args, const emitterData_t& e, bool deleteWhenDisabled );
float	EM_Range( const idVec3& lightRadius );
float	EM_EffectMultiplier( const emitterData_t& e, int timeMs, int frameTimeMs );
float	EM_ReachFactor( int falloff, float intensity );
float	EM_AutoRange( const emitterData_t& e, float intensity );
float	EM_AutoSize( const emitterData_t& e, float intensity, float range );
bool	EM_RangeInStep( const emitterData_t& e, float intensity, float range );	// added 2026-11-11, choice 3
const char* EM_EffectName( int effect );
int		EM_EffectFromName( const char* name );

#endif
