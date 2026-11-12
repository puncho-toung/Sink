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
Emitter.cpp

Spawn-arg parsing and writing for emitter lights, and the effect clock. The game
(idGameEdit::ParseSpawnArgsToRenderLight, idLight::SharedThink) and the light
editors call these helpers, so the emit_* key contract has exactly one
implementation.
================================================================================
*/
#include "precompiled.h"
#pragma hdrstop

#include "Emitter.h"

// indexed by emitterShape_t, emitterFalloff_t, emitterPattern_t and emitterEffect_t
static const char* const emShapeNames[] =
{
	"omni", "spot", "sun"
};

static const char* const emFalloffNames[] =
{
	"invsquare", "linear", "smooth", "exponential", "invsquare_clamped"
};

static const char* const emPatternNames[] =
{
	"none", "subsurface", "ice", "caustic", "banded", "granular"
};

static const char* const emEffectNames[] =
{
	"none",
	"flicker_a", "slow_strong_pulse", "candle_a", "fast_strobe", "gentle_pulse", "flicker_b",
	"candle_b", "candle_c", "slow_strobe", "fluorescent", "slow_pulse",
	"strobe", "pulse", "flicker"
};

// every key of the block, removed together when the block is deleted
static const char* const emKeys[] =
{
	"emitter",
	"emit_shape",
	"emit_auto_range",
	"emit_falloff",
	"emit_size",
	"emit_start",
	"emit_clamp",
	"emit_cone",
	"emit_penumbra",
	"emit_square",
	"emit_pattern",
	"emit_pattern_strength",
	"emit_pattern_scale",
	"emit_pattern_phase",
	"emit_effect",
	"emit_effect_speed",
	"emit_effect_amount",
	"emit_effect_phase"
};

static const int EM_NUM_SHAPES = EM_SUN + 1;
static const int EM_NUM_FALLOFFS = EMF_INVSQUARE_CLAMPED + 1;
static const int EM_NUM_PATTERNS = EMP_GRANULAR + 1;
static const int EM_NUM_KEYS = sizeof( emKeys ) / sizeof( emKeys[ 0 ] );

static_assert( sizeof( emShapeNames ) / sizeof( emShapeNames[ 0 ] ) == EM_NUM_SHAPES, "one name per emitterShape_t" );
static_assert( sizeof( emFalloffNames ) / sizeof( emFalloffNames[ 0 ] ) == EM_NUM_FALLOFFS, "one name per emitterFalloff_t" );
static_assert( sizeof( emPatternNames ) / sizeof( emPatternNames[ 0 ] ) == EM_NUM_PATTERNS, "one name per emitterPattern_t" );
static_assert( sizeof( emEffectNames ) / sizeof( emEffectNames[ 0 ] ) == EME_NUM_EFFECTS, "one name per emitterEffect_t" );
static_assert( sizeof( predef_lightstyles ) / sizeof( predef_lightstyles[ 0 ] ) == EME_STYLE_LAST + 1, "effects 1..EME_STYLE_LAST index predef_lightstyles" );

// the lengths of predef_lightstyles[ 0 .. EME_STYLE_LAST ], so a style effect walks its string once per
// tic, inside EM_StyleMultiplier; EM_EffectMultiplier checks them in debug builds
static const int emStyleLengths[] = { 1, 23, 51, 34, 12, 33, 17, 28, 42, 16, 25, 36 };
static_assert( sizeof( emStyleLengths ) / sizeof( emStyleLengths[ 0 ] ) == EME_STYLE_LAST + 1, "one length per predefined style" );

// tan( 30 degrees ), the half-angle tangent EM_ParseFromDict computes for emit_cone "60", held by a light
// that is not a spot without a tan ( EC-01 ); le_selfTest compares the bits with the computed value
static const float EM_DEFAULT_TAN_HALF_CONE = 0.577350259f;

/*
================
EM_FindName

The index of a name in a name table, ignoring case; -1 when it is not there.
================
*/
static int EM_FindName( const char* const* names, int numNames, const char* name )
{
	if( name == NULL )
	{
		return -1;
	}

	for( int i = 0; i < numNames; i++ )
	{
		if( idStr::Icmp( names[ i ], name ) == 0 )
		{
			return i;
		}
	}
	return -1;
}

/*
================
EM_ParseName

Reads a named key. An absent or empty key is the default; an unknown name is the default with one
warning, so a typo in a map shows in the console instead of silently changing the light.
================
*/
static int EM_ParseName( const idDict* args, const char* key, const char* const* names, int numNames, int defaultIndex )
{
	const char* value = args->GetString( key, "" );
	if( value[ 0 ] == '\0' )
	{
		return defaultIndex;
	}

	const int index = EM_FindName( names, numNames, value );
	if( index < 0 )
	{
		idLib::Warning( "light '%s': %s '%s' not recognized; using '%s'", args->GetString( "name" ), key, value, names[ defaultIndex ] );
		return defaultIndex;
	}
	return index;
}

/*
================
EM_FormatFloats

Numbers with at most the given decimals and no trailing zeros, "-0" written "0", as TrenchBroom's
formatIdTech4Float writes them, so both editors write the same values and no key is written "-0".
================
*/
static idStr EM_FormatFloats( const float* values, int count, int precision )
{
	idStr text;
	for( int i = 0; i < count; i++ )
	{
		idStr number = idStr::FloatArrayToString( &values[ i ], 1, precision );
		if( number == "-0" )
		{
			number = "0";
		}
		if( i > 0 )
		{
			text += " ";
		}
		text += number;
	}
	return text;
}

/*
================
EM_SetFloat

At most four decimals and no trailing zeros, as the light editors write floats; a value that rounds
to zero is "0", never "-0" (an unclamped phase can be a tiny negative number).
================
*/
static void EM_SetFloat( idDict* args, const char* key, float value )
{
	args->Set( key, EM_FormatFloats( &value, 1, 4 ) );
}

/*
================
EM_Hash

Integer hash of one flicker cell, 0 to 1. Pure integer arithmetic, so every peer and every editor
gets the same value for the same cell.
================
*/
static float EM_Hash( unsigned int n )
{
	return ( float )( ( double )( ( ( n * 1103515245u + 12345u ) >> 16 ) & 0x7fffu ) / 32767.0 );
}

//puncho start - the light-style clock a style effect plays
/*
================
EM_StyleMultiplier

The brightness a light-style string gives at a game time. The a-z characters play one per frameTimeMs
and are interpolated as idLight::SharedThink interpolates "style", scaled so that 'a' = 0, 'm' = 1 and
'z' = 25/12 (characters outside a-z are clamped to them); an empty string is 1. A pure function of the
clock, so every peer and the editors get the same value.
================
*/
static float EM_StyleMultiplier( const char* style, int timeMs, int frameTimeMs )
{
	const int length = idStr::Length( style );
	if( length <= 0 )
	{
		return 1.0f;
	}

	// ls_frametime comes from the map; zero would divide by zero
	frameTimeMs = Max( frameTimeMs, 1 );
	timeMs = Max( timeMs, 0 );

	const int step = timeMs / frameTimeMs;
	const int frame = step % length;
	const int nextFrame = ( frame + 1 ) % length;
	const float backLerp = ( float )( timeMs - step * frameTimeMs ) / ( float )frameTimeMs;

	const int lastCh = idMath::ClampInt( 0, 25, style[ frame ] - 'a' );
	const int nextCh = idMath::ClampInt( 0, 25, style[ nextFrame ] - 'a' );

	return ( lastCh * ( 1.0f - backLerp ) + nextCh * backLerp ) * ( 1.0f / 12.0f );
}
//puncho end

/*
================
EM_ParseBlock

The body of both entries. allKeys false is the game's parse: EC-01 ( G0 section 3, "It reads only
the keys the light uses" ). allKeys true reads every key whatever the shape, pattern and effect, as
the parse did before batch E: the editors write every key back from the block they parsed ( G0
section 3, "Two entries", DP-EC-01 ), so a key the light does not use must come back as authored.
For any dict both give the same value in every field the light uses.
================
*/
static void EM_ParseBlock( const idDict* args, emitterData_t* e, bool allKeys )
{
	// cleared first, so switching the emitter off leaves no stale state and the pads stay 0
	memset( e, 0, sizeof( *e ) );

	e->enabled = args->GetBool( "emitter", "0" );
	if( !e->enabled )
	{
		return;
	}

	// EC-01: unless allKeys, a key that the light's shape, pattern or effect does not use is not looked
	// up, and its field holds the key's default, as if the key were absent, so an emitter pays at spawn
	// only for the keys it uses

	// an absent, empty or unknown shape follows "parallel", which a sun carries; a recognized emit_shape
	// does not look at it
	const char* shapeName = args->GetString( "emit_shape", "" );
	e->shape = EM_FindName( emShapeNames, EM_NUM_SHAPES, shapeName );
	if( e->shape < 0 )
	{
		e->shape = args->GetBool( "parallel", "0" ) ? EM_SUN : EM_OMNI;
		if( shapeName[ 0 ] != '\0' )
		{
			idLib::Warning( "light '%s': %s '%s' not recognized; using '%s'", args->GetString( "name" ), "emit_shape", shapeName, emShapeNames[ e->shape ] );
		}
	}
	e->pattern = EM_ParseName( args, "emit_pattern", emPatternNames, EM_NUM_PATTERNS, EMP_NONE );

	// the effect through EM_EffectFromName, the lookup G0 freezes for every reader of the key; absent or
	// empty is none, and an unknown name is none with EM_ParseName's one warning
	const char* effectName = args->GetString( "emit_effect", "" );
	e->effect = ( effectName[ 0 ] != '\0' ) ? EM_EffectFromName( effectName ) : EME_NONE;
	if( e->effect < 0 )
	{
		idLib::Warning( "light '%s': %s '%s' not recognized; using '%s'", args->GetString( "name" ), "emit_effect", effectName, emEffectNames[ EME_NONE ] );
		e->effect = EME_NONE;
	}

	e->energyClamp = args->GetBool( "emit_clamp", "1" );

	// a sun has no falloff curve ( G0 section 5 ), so its three curve keys are not read. The start's
	// upper limit (range - 1) is applied where the range is known, in the renderer, because scripts and
	// snapshots change the radius after spawn
	if( allKeys || e->shape != EM_SUN )
	{
		e->falloff = EM_ParseName( args, "emit_falloff", emFalloffNames, EM_NUM_FALLOFFS, EMF_INVSQUARE_CLAMPED );
		e->size = Max( args->GetFloat( "emit_size", "96" ), 1.0f );
		e->start = Max( args->GetFloat( "emit_start", "0" ), 0.0f );
	}
	else
	{
		e->falloff = EMF_INVSQUARE_CLAMPED;
		e->size = 96.0f;
		e->start = 0.0f;
	}

	if( allKeys || e->shape == EM_SPOT )
	{
		// "h" or "h v", full angles in degrees; one number is a round cone
		float coneH = 60.0f;
		float coneV = 60.0f;
		const int numCone = sscanf( args->GetString( "emit_cone", "60" ), "%f %f", &coneH, &coneV );
		if( numCone < 2 )
		{
			coneV = coneH;
		}
		e->coneH = idMath::ClampFloat( 1.0f, 120.0f, coneH );
		e->coneV = idMath::ClampFloat( 1.0f, 120.0f, coneV );
		e->tanHalfH = idMath::Tan( DEG2RAD( e->coneH * 0.5f ) );
		e->tanHalfV = idMath::Tan( DEG2RAD( e->coneV * 0.5f ) );

		e->penumbra = idMath::ClampFloat( 0.0f, 1.0f, args->GetFloat( "emit_penumbra", "0.2" ) );
		e->squareness = idMath::ClampFloat( 1.5f, 32.0f, args->GetFloat( "emit_square", "2" ) );
	}
	else
	{
		// the default cone with no sscanf and no tan
		e->coneH = 60.0f;
		e->coneV = 60.0f;
		e->tanHalfH = EM_DEFAULT_TAN_HALF_CONE;
		e->tanHalfV = EM_DEFAULT_TAN_HALF_CONE;
		e->penumbra = 0.2f;
		e->squareness = 2.0f;
	}

	if( allKeys || e->pattern != EMP_NONE )
	{
		e->patternStrength = idMath::ClampFloat( 0.0f, 1.0f, args->GetFloat( "emit_pattern_strength", "0" ) );
		e->patternScale = args->GetFloat( "emit_pattern_scale", "1" );
		e->patternPhase = args->GetFloat( "emit_pattern_phase", "0" );
	}
	else
	{
		e->patternStrength = 0.0f;
		e->patternScale = 1.0f;
		e->patternPhase = 0.0f;
	}

	if( allKeys || e->effect != EME_NONE )
	{
		e->effectSpeed = idMath::ClampFloat( 0.0f, 100.0f, args->GetFloat( "emit_effect_speed", "1" ) );
		e->effectAmount = idMath::ClampFloat( 0.0f, 1.0f, args->GetFloat( "emit_effect_amount", "1" ) );

		// stored as given; EM_EffectMultiplier uses it modulo 1
		e->effectPhase = args->GetFloat( "emit_effect_phase", "0" );
	}
	else
	{
		e->effectSpeed = 1.0f;
		e->effectAmount = 1.0f;
		e->effectPhase = 0.0f;
	}
}

/*
================
EM_ParseFromDict

The game's parse: only the keys the light uses ( EC-01 ). ParseSpawnArgsToRenderLight and every
caller that only draws or simulates the light.
================
*/
void EM_ParseFromDict( const idDict* args, emitterData_t* e )
{
	EM_ParseBlock( args, e, false );
}

/*
================
EM_ParseAllFromDict

Every key, for a caller that writes the block back ( the light editors, DP-EC-01 ).
================
*/
void EM_ParseAllFromDict( const idDict* args, emitterData_t* e )
{
	EM_ParseBlock( args, e, true );
}

/*
================
EM_WriteToDict
================
*/
void EM_WriteToDict( idDict* args, const emitterData_t& e, bool deleteWhenDisabled )
{
	// the light editors mark a key for removal with an empty value (their DELETE_VAL); following
	// that idiom keeps editor-side and game-side calls identical
	static const char* const DELETE_VAL = "";

	if( !e.enabled )
	{
		if( deleteWhenDisabled )
		{
			for( int i = 0; i < EM_NUM_KEYS; i++ )
			{
				args->Set( emKeys[ i ], DELETE_VAL );
			}
		}
		else
		{
			// the tab is the switch; the other emit_* keys stay as they are, so switching back is free
			args->Set( "emitter", "0" );
		}
		return;
	}

	// every key is written, defaults included, so the block describes itself
	args->Set( "emitter", "1" );
	args->Set( "emit_shape", emShapeNames[ idMath::ClampInt( 0, EM_NUM_SHAPES - 1, e.shape ) ] );
	args->Set( "emit_falloff", emFalloffNames[ idMath::ClampInt( 0, EM_NUM_FALLOFFS - 1, e.falloff ) ] );

	EM_SetFloat( args, "emit_size", e.size );
	EM_SetFloat( args, "emit_start", e.start );
	args->SetBool( "emit_clamp", e.energyClamp );

	if( e.coneH == e.coneV )
	{
		EM_SetFloat( args, "emit_cone", e.coneH );
	}
	else
	{
		const float cone[ 2 ] = { e.coneH, e.coneV };
		args->Set( "emit_cone", idStr::FloatArrayToString( cone, 2, 4 ) );
	}
	EM_SetFloat( args, "emit_penumbra", e.penumbra );
	EM_SetFloat( args, "emit_square", e.squareness );

	args->Set( "emit_pattern", emPatternNames[ idMath::ClampInt( 0, EM_NUM_PATTERNS - 1, e.pattern ) ] );
	EM_SetFloat( args, "emit_pattern_strength", e.patternStrength );
	EM_SetFloat( args, "emit_pattern_scale", e.patternScale );
	EM_SetFloat( args, "emit_pattern_phase", e.patternPhase );

	args->Set( "emit_effect", EM_EffectName( e.effect ) );
	EM_SetFloat( args, "emit_effect_speed", e.effectSpeed );
	EM_SetFloat( args, "emit_effect_amount", e.effectAmount );
	EM_SetFloat( args, "emit_effect_phase", e.effectPhase );
}

/*
================
EM_Range

The emitter's range: the largest radius. A spot carries it three times; an omni's curve is defined
along its largest axis.
================
*/
float EM_Range( const idVec3& lightRadius )
{
	return Max( Max( lightRadius.x, lightRadius.y ), lightRadius.z );
}

/*
================
EM_EffectMultiplier

The brightness an effect gives at a game time: 1 + amount * ( w - 1 ), w from 0 to the style's peak.
A pure function of its arguments, so every peer and every editor preview agree. frameTimeMs is the
light's ls_frametime.
================
*/
float EM_EffectMultiplier( const emitterData_t& e, int timeMs, int frameTimeMs )
{
	if( e.effect <= EME_NONE || e.effect >= EME_NUM_EFFECTS )
	{
		return 1.0f;
	}

	// in double: timeMs * speed passes a float's whole-number range within hours of game time
	const double phase = ( double )e.effectPhase - floor( ( double )e.effectPhase );
	const double seconds = ( double )timeMs * 0.001 * ( double )e.effectSpeed;

	float w;
	if( e.effect <= EME_STYLE_LAST )
	{
		const char* style = predef_lightstyles[ e.effect ];

		// EM_StyleMultiplier clamps the frame time the same way; the phase is a fraction of the string
		const int frameTime = Max( frameTimeMs, 1 );
		assert( idStr::Length( style ) == emStyleLengths[ e.effect ] );
		const double period = ( double )emStyleLengths[ e.effect ] * ( double )frameTime;

		// the string repeats every period, so wrapping first gives the same frame and lerp as the
		// unwrapped time, and keeps the time inside an int at any speed
		double t = ( double )timeMs * ( double )e.effectSpeed + phase * period;
		t -= floor( t / period ) * period;

		w = EM_StyleMultiplier( style, ( int )t, frameTime );
	}
	else if( e.effect == EME_STROBE )
	{
		const double cycles = seconds + phase;
		w = ( ( cycles - floor( cycles ) ) < 0.5 ) ? 1.0f : 0.0f;
	}
	else if( e.effect == EME_PULSE )
	{
		// only the fraction matters to the cosine, and a small argument keeps it exact in float
		const double cycles = seconds + phase;
		const float x = ( float )( cycles - floor( cycles ) );
		w = 0.5f + 0.5f * idMath::Cos( idMath::TWO_PI * x );
	}
	else
	{
		// EME_FLICKER: smooth value noise, eight cells a second; the phase moves it by up to 64 cells
		const double x = seconds * 8.0 + phase * 64.0;
		const double cell = floor( x );
		float f = ( float )( x - cell );
		f = f * f * ( 3.0f - 2.0f * f );

		// through int64, so a long session wraps the cell number instead of overflowing an int
		const unsigned int n = ( unsigned int )( int64 )cell;
		const float h0 = EM_Hash( n );
		const float h1 = EM_Hash( n + 1u );
		w = h0 + ( h1 - h0 ) * f;
	}

	return 1.0f + e.effectAmount * ( w - 1.0f );
}

/*
================
EM_ReachFactor

How many sizes past the start a curve stays above one tenth of the intensity; 0 for the curves that
have no auto range (linear, smooth). intensity is the largest component of _color.
================
*/
float EM_ReachFactor( int falloff, float intensity )
{
	// a negative colour has no reach; without this the square roots below would return NaN
	const float ratio = Max( intensity, 0.0f ) / 0.1f;

	switch( falloff )
	{
		case EMF_INVSQUARE_CLAMPED:
			return Max( 1.0f, idMath::Sqrt( Max( ratio - 1.0f, 0.0f ) ) );

		case EMF_INVSQUARE:
			return Max( 1.0f, idMath::Sqrt( ratio ) );

		case EMF_EXPONENTIAL:
			return Max( 1.0f, idMath::Log( Max( ratio, 1.0f ) ) );

		default:
			return 0.0f;
	}
}

/*
================
EM_AutoRange

The range that auto range gives; 0 when the curve has no reach factor.
================
*/
float EM_AutoRange( const emitterData_t& e, float intensity )
{
	const float c = EM_ReachFactor( e.falloff, intensity );
	if( c == 0.0f )
	{
		return 0.0f;
	}
	return Max( 24.0f, e.start + e.size * c );
}

/*
================
EM_AutoSize

The size that keeps a range under auto range; the size unchanged when the curve has no reach factor.
================
*/
float EM_AutoSize( const emitterData_t& e, float intensity, float range )
{
	const float c = EM_ReachFactor( e.falloff, intensity );
	if( c == 0.0f )
	{
		return e.size;
	}
	return Max( 1.0f, ( range - e.start ) / c );
}

/*
================
EM_RangeInStep

The last condition of "auto range in force": the curve has a reach factor and range is within
max( 1, 5 % ) of the auto range. The tolerance is wide on purpose: _color is stored with two
decimals, which moves a dim lamp's auto range by up to 2.5 %, and a light an editor wrote under auto
range must read back in step. It does not look at emit_auto_range or the shape; the editors do.
TrenchBroom ports it as emitterRangeInStep.
================
*/
bool EM_RangeInStep( const emitterData_t& e, float intensity, float range )
{
	const float autoRange = EM_AutoRange( e, intensity );
	if( autoRange == 0.0f )
	{
		return false;
	}
	return idMath::Fabs( range - autoRange ) <= Max( 1.0f, 0.05f * autoRange );
}

/*
================
EM_EffectName

The key value of an effect; "none" for a number outside the table.
================
*/
const char* EM_EffectName( int effect )
{
	if( effect < 0 || effect >= EME_NUM_EFFECTS )
	{
		return emEffectNames[ EME_NONE ];
	}
	return emEffectNames[ effect ];
}

/*
================
EM_EffectFromName

The effect a key value names, ignoring case; -1 for an unknown name, so the caller decides what an
unknown name means. EM_ParseFromDict reads emit_effect through it, and warns and uses none.
================
*/
int EM_EffectFromName( const char* name )
{
	return EM_FindName( emEffectNames, EME_NUM_EFFECTS, name );
}
