/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.
Copyright (C) 2015 Daniel Gibson
Copyright (C) 2020-2023 Robert Beckebans

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#include "precompiled.h"
#pragma hdrstop

#include "LightEditor.h"

#include "../imgui/BFGimgui.h"

#include "renderer/Material.h"
#include "renderer/Image.h"

#include "../../renderer/Emitter.h"

// plan 3.7: easy mode tunables (tool-only: not networked, not saved in savegames)
idCVar le_traceDist( "le_traceDist", "8192", CVAR_TOOL | CVAR_FLOAT, "light editor: aim trace length in units", 64.0f, 131072.0f );
idCVar le_showLegend( "le_showLegend", "1", CVAR_TOOL | CVAR_BOOL | CVAR_ARCHIVE, "light editor: show the easy mode legend (H toggles)" );
idCVar le_startEasy( "le_startEasy", "0", CVAR_TOOL | CVAR_BOOL | CVAR_ARCHIVE, "light editor: editLights opens the easy overlay; 0 shows nothing until a light is picked, then the advanced panel (F switches either way)" );
idCVar le_logInput( "le_logInput", "0", CVAR_TOOL | CVAR_BOOL, "light editor: print every mouse button and wheel key event while the editor is open (WP-LE1 input check)" );
idCVar le_scaleSens( "le_scaleSens", "0.004", CVAR_TOOL | CVAR_FLOAT, "light editor: log-scale per mouse count in the Q and E modals (250 counts ~ x2.7)" );
idCVar le_wheelSens( "le_wheelSens", "0.10", CVAR_TOOL | CVAR_FLOAT, "light editor: log-scale per wheel click in the E modal" );
idCVar le_rotSens( "le_rotSens", "0.25", CVAR_TOOL | CVAR_FLOAT, "light editor: degrees per mouse count in the R modal" );
idCVar le_rotWheelSens( "le_rotWheelSens", "5.0", CVAR_TOOL | CVAR_FLOAT, "light editor: degrees per wheel click in the R modal" );
idCVar le_hueSens( "le_hueSens", "0.0015", CVAR_TOOL | CVAR_FLOAT, "light editor: hue turns per mouse count in the T modal (one wrap ~ 670 counts)" );
idCVar le_intSens( "le_intSens", "0.004", CVAR_TOOL | CVAR_FLOAT, "light editor: log-intensity per mouse count in the T modal" );
idCVar le_maxIntensity( "le_maxIntensity", "8.0", CVAR_TOOL | CVAR_FLOAT, "light editor: upper clamp of max( r, g, b ) in the T modal", 0.01f, 1000.0f );

namespace ImGuiTools
{

// plan 3.2: the keys the tool owns; easy mode queues and swallows all of them, advanced mode F and H
static const int easyKeys[ LightEditor::NUM_EASY_KEYS ] = { K_Q, K_E, K_R, K_T, K_F, K_H, K_MOUSE1, K_MOUSE2, K_MWHEELUP, K_MWHEELDOWN, K_ENTER, K_KP_ENTER };

static void AutoCaptureBase( const LightInfo& light, LightAutoBase& base );

// an emitter's shape is the light's type, never chosen on its own
static int ShapeForLightType( ELightType type )
{
	if( type == LIGHT_SPOT )
	{
		return EM_SPOT;
	}
	return ( type == LIGHT_SUN ) ? EM_SUN : EM_OMNI;
}

static ELightType LightTypeForShape( int shape )
{
	if( shape == EM_SPOT )
	{
		return LIGHT_SPOT;
	}
	return ( shape == EM_SUN ) ? LIGHT_SUN : LIGHT_POINT;
}

// adds the keys of from that start with prefix to to
static void CopyPrefixKeys( const idDict& from, idDict& to, const char* prefix )
{
	for( const idKeyValue* kv = from.MatchPrefix( prefix ); kv != NULL; kv = from.MatchPrefix( prefix, kv ) )
	{
		to.Set( kv->GetKey(), kv->GetValue() );
	}
}

// applies the prefix keys of d to to, as idGameEdit::EntityChangeSpawnArgs applies them ("" deletes)
static void ApplyPrefixKeys( idDict& to, const idDict& d, const char* prefix )
{
	for( const idKeyValue* kv = d.MatchPrefix( prefix ); kv != NULL; kv = d.MatchPrefix( prefix, kv ) )
	{
		if( kv->GetValue().IsEmpty() )
		{
			to.Delete( kv->GetKey() );
		}
		else
		{
			to.Set( kv->GetKey(), kv->GetValue() );
		}
	}
}

// Cancel means no change. The emit keys stay on a light switched to Texture, so the original's
// ToDict cannot undo a switch: this rewrites the prefix part of d (that ToDict output, bound for a
// light whose keys are current) so the light ends with exactly original's keys
static void RestorePrefixKeys( const idDict& original, const idDict& current, idDict& d, const char* prefix )
{
	idDict touched;
	CopyPrefixKeys( d, touched, prefix );
	CopyPrefixKeys( current, touched, prefix );
	for( int i = 0; i < touched.GetNumKeyVals(); i++ )
	{
		const idKeyValue* kv = touched.GetKeyVal( i );
		d.Set( kv->GetKey(), original.GetString( kv->GetKey(), "" ) );
	}

	// a key the original had that neither ToDict nor the light has now
	for( int i = 0; i < original.GetNumKeyVals(); i++ )
	{
		const idKeyValue* kv = original.GetKeyVal( i );
		d.Set( kv->GetKey(), kv->GetValue() );
	}
}

// RV-C-ET-01: an easy-mode checkpoint is a LightInfo, and its ToDict cannot undo the keys it does not write
// (the emitter "1" an Emitter tab wrote). So a checkpoint also keeps the emit keys the light has once 'undo'
// is applied to it: the live keys with undo's ToDict on top (a gesture between the snapshot and now changes
// only keys that ToDict writes)
static void CheckpointKeys( const idDict& live, const LightInfo& undo, idDict& emitKeys )
{
	LightInfo copy = undo;
	idDict d;
	copy.ToDict( &d );

	emitKeys.Clear();
	CopyPrefixKeys( live, emitKeys, "emit" );
	ApplyPrefixKeys( emitKeys, d, "emit" );
}

// the dict that undoes a checkpoint on a light whose keys are 'live' now: undo's ToDict, its emit keys set
// back to the checkpoint's, as Cancel sets them back to the selection's
static void CheckpointUndoDict( const LightInfo& undo, const idDict& live, const idDict& emitKeys, idDict& d )
{
	LightInfo copy = undo;
	copy.ToDict( &d );
	RestorePrefixKeys( emitKeys, live, d, "emit" );
}

// the intensity auto range reads: the brightest channel of _color
static float LightIntensity( const LightInfo& light )
{
	return Max3( light.color.x, light.color.y, light.color.z );
}

// both full angles clamped to 1..120 degrees, with the half-angle tangents kept in step
static void SetEmitterCone( emitterData_t& e, float coneH, float coneV )
{
	e.coneH = idMath::ClampFloat( 1.0f, 120.0f, coneH );
	e.coneV = idMath::ClampFloat( 1.0f, 120.0f, coneV );
	e.tanHalfH = idMath::Tan( DEG2RAD( e.coneH * 0.5f ) );
	e.tanHalfV = idMath::Tan( DEG2RAD( e.coneV * 0.5f ) );
}

static void SetEmitterConeFromTan( emitterData_t& e, float tanHalfH, float tanHalfV )
{
	SetEmitterCone( e, RAD2DEG( 2.0f * idMath::ATan( tanHalfH ) ), RAD2DEG( 2.0f * idMath::ATan( tanHalfV ) ) );
}

static const float EMITTER_MIN_RANGE = 24.0f;

// a spot's range is written R R R; an omni or sun box is scaled as a whole so that its largest
// component is the range, and no component goes below 1
static void SetEmitterRange( LightInfo& light, float range )
{
	range = Max( range, EMITTER_MIN_RANGE );
	if( light.lightType == LIGHT_SPOT )
	{
		light.lightRadius.Set( range, range, range );
		light.equalRadius = true;
		return;
	}

	const float largest = EM_Range( light.lightRadius );
	if( largest < 1.0f )
	{
		light.lightRadius.Set( range, range, range );
	}
	else
	{
		light.lightRadius *= range / largest;
	}
	for( int i = 0; i < 3; i++ )
	{
		light.lightRadius[ i ] = Max( light.lightRadius[ i ], 1.0f );
	}
}

static void ClampEmitterStart( LightInfo& light )
{
	light.emitter.start = idMath::ClampFloat( 0.0f, Max( EM_Range( light.lightRadius ) - 1.0f, 0.0f ), light.emitter.start );
}

// G0 5, "auto range in force", as the editor holds it: an emitter omni or spot whose autoRange is on
// and whose curve has a reach factor. The last condition, a range in step, is tested when the light
// is read and when a kept block comes back (EmitterAutoRangeOnRead); a light that fails it is held with
// autoRange off, so it is not tested again on every edit, and a falloff edit applies the flag as it
// stands (G0 5)
static bool EmitterAutoRangeInForce( const LightInfo& light )
{
	if( !light.emitter.enabled || light.lightType == LIGHT_SUN || !light.autoRange )
	{
		return false;
	}
	return EM_ReachFactor( light.emitter.falloff, LightIntensity( light ) ) > 0.0f;
}

// G0 5, "a hand-set range wins": the auto range flag as read from the keys ('stored'), turned off when
// the light is an emitter omni or spot whose curve has a reach factor and whose range is out of step
// with it. ToDict then writes emit_auto_range "0". A curve without a reach keeps the stored flag
static void EmitterAutoRangeOnRead( LightInfo& light, bool stored )
{
	light.autoRange = stored;
	if( !stored || !light.emitter.enabled || light.lightType == LIGHT_SUN )
	{
		return;
	}

	const float intensity = LightIntensity( light );
	if( EM_ReachFactor( light.emitter.falloff, intensity ) > 0.0f && !EM_RangeInStep( light.emitter, intensity, EM_Range( light.lightRadius ) ) )
	{
		light.autoRange = false;
	}
}

// an intensity, size, start or falloff edit: under auto range the range follows the reach. A sun
// is not attenuated and keeps its box
static void EmitterReachChanged( LightInfo& light )
{
	if( !light.emitter.enabled || light.lightType == LIGHT_SUN )
	{
		return;
	}

	light.emitter.size = Max( light.emitter.size, 1.0f );
	if( EmitterAutoRangeInForce( light ) )
	{
		SetEmitterRange( light, EM_AutoRange( light.emitter, LightIntensity( light ) ) );
	}
	ClampEmitterStart( light );
}

// the Falloff control: a reach edit like any other. A curve that gains a reach factor (from linear or
// smooth) applies the flag as it stands: on, the range becomes the auto range; off, the range is kept.
// No in-step test here (G0 5, corrected 01:00, F-C-01 / DP-T5-01): a round trip through linear changes
// nothing, and a range set by hand under linear turned the flag off when it was set
static void EmitterSetFalloff( LightInfo& light, int falloff )
{
	light.emitter.falloff = falloff;
	EmitterReachChanged( light );
}

// the range was set (a radius or range drag, Q, the wheel, the gizmo, the aim under auto range):
// the limits apply, and under auto range the size follows so the reach stays the one asked for
static void EmitterRangeSet( LightInfo& light )
{
	if( !light.emitter.enabled || light.lightType == LIGHT_SUN )
	{
		return;
	}

	SetEmitterRange( light, EM_Range( light.lightRadius ) );
	ClampEmitterStart( light );
	if( EmitterAutoRangeInForce( light ) )
	{
		light.emitter.size = EM_AutoSize( light.emitter, LightIntensity( light ), EM_Range( light.lightRadius ) );
	}
}

// a range edit by the mapper (a range or radius field, Q, E's wheel, the gizmo; not the aim, which never
// sets the range of a curve without a reach), 'rangeBefore' being the range the edit started from:
// EmitterRangeSet, and on an emitter omni or spot whose curve has no reach factor (linear, smooth) a
// changed range turns auto range off, because the mapper took it in hand (G0 5, F-C-01)
static void EmitterRangeEdited( LightInfo& light, float rangeBefore )
{
	EmitterRangeSet( light );
	if( !light.emitter.enabled || light.lightType == LIGHT_SUN )
	{
		return;
	}
	if( EM_ReachFactor( light.emitter.falloff, LightIntensity( light ) ) == 0.0f && EM_Range( light.lightRadius ) != rangeBefore )
	{
		light.autoRange = false;
	}
}

// Range, Radius and Size drag at 1 % of their value per pixel, so a long throw is as quick to set as a
// short one; never slower than a quarter unit
static float DragSpeedFor( float value )
{
	return Max( idMath::Fabs( value ) * 0.01f, 0.25f );
}

// the panel shows a colour as an intensity, the largest channel of _color, and the colour divided by
// it. 'hue' is that colour; at intensity 0 (black) it keeps what it held, so the colour shown stays
static float PanelColorSplit( const idVec3& color, idVec3& hue )
{
	const float intensity = Max3( color.x, color.y, color.z );
	if( intensity <= 0.0f )
	{
		return 0.0f;
	}
	hue = color / intensity;
	return intensity;
}

// either control writes _color = hue * intensity. A changed intensity is a reach edit: under auto
// range an emitter's range follows it. A texture light only gets the colour
static void PanelSetColor( LightInfo& light, const idVec3& hue, float intensity )
{
	const float before = LightIntensity( light );
	light.color = hue * Max( intensity, 0.0f );
	if( LightIntensity( light ) != before )
	{
		EmitterReachChanged( light );
	}
}

// an emitter omni's lamp is at origin + axis * light_center (the bulb), so its gizmo is drawn there;
// every other light's at its origin
static idVec3 LightGizmoOrigin( const LightInfo& light )
{
	if( light.emitter.enabled && light.lightType == LIGHT_POINT && light.hasCenter )
	{
		return light.origin + light.angles.ToMat3() * light.lightCenter;
	}
	return light.origin;
}

// the origin a gizmo at 'gizmoOrigin' gives, with the light's angles already updated: the gizmo turns
// an emitter omni about its bulb
static idVec3 LightOriginFromGizmo( const LightInfo& light, const idVec3& gizmoOrigin )
{
	if( light.emitter.enabled && light.lightType == LIGHT_POINT && light.hasCenter )
	{
		return gizmoOrigin - light.angles.ToMat3() * light.lightCenter;
	}
	return gizmoOrigin;
}

// G0 9 (RV-C-ET-08): FromDict ticks Equilateral for any light with equal radii; on an emitter omni the
// mapper's untick lasts the session ('unticked'), so the panel shows three radii again after a re-read.
// equalRadius only picks the panel's rows and how ToDict writes equal radii, so the keys do not change
static void EquilateralForSession( LightInfo& light, bool unticked )
{
	if( unticked && light.emitter.enabled && light.lightType == LIGHT_POINT )
	{
		light.equalRadius = false;
	}
}

// the block a light gets when it first becomes an emitter: every field set, pads cleared
static void SeedEmitter( emitterData_t& e, ELightType type )
{
	memset( &e, 0, sizeof( e ) );
	e.enabled = true;
	e.shape = ShapeForLightType( type );
	e.falloff = EMF_INVSQUARE_CLAMPED;
	e.pattern = EMP_NONE;
	e.effect = EME_NONE;
	e.size = 96.0f;
	e.start = 0.0f;
	SetEmitterCone( e, 60.0f, 60.0f );
	e.penumbra = 0.2f;
	e.squareness = 2.0f;
	e.patternStrength = 0.0f;
	e.patternScale = 1.0f;
	e.patternPhase = 0.0f;
	e.effectSpeed = 1.0f;
	e.effectAmount = 1.0f;
	e.effectPhase = 0.0f;
	e.energyClamp = true;
}

// what the type radio gives an emitter, 'before' being the light before the switch (G0 9, shape
// switch). The rotation is the lamp's and stays, except that a light with none becoming a spot points
// down. With auto range in force the range is the auto range; otherwise an omni and a spot keep it (a
// spot's range is the omni's largest radius and the reverse), a linear or smooth curve and a hand-set
// range included. Coming from a sun, whose radius is a box: the auto range when the flag is on and the
// curve has a reach, else 300. A sun keeps today's sun box
static void ApplyEmitterShapeDefaults( LightInfo& light, const LightInfo& before )
{
	if( !light.emitter.enabled )
	{
		return;
	}

	light.emitter.shape = ShapeForLightType( light.lightType );
	light.angles = before.angles;
	if( light.lightType == LIGHT_SUN )
	{
		return;
	}

	if( light.lightType == LIGHT_SPOT && before.angles.pitch == 0.0f && before.angles.yaw == 0.0f && before.angles.roll == 0.0f )
	{
		light.angles.Set( 90.0f, 0.0f, 0.0f );
	}

	// autoRange and the curve came over with the block, so in force here is in force before the switch
	const float intensity = LightIntensity( light );
	const bool hasReach = ( EM_ReachFactor( light.emitter.falloff, intensity ) > 0.0f );
	if( before.lightType == LIGHT_SUN )
	{
		SetEmitterRange( light, ( light.autoRange && hasReach ) ? EM_AutoRange( light.emitter, intensity ) : 300.0f );
	}
	else if( EmitterAutoRangeInForce( light ) )
	{
		SetEmitterRange( light, EM_AutoRange( light.emitter, intensity ) );
	}
	else
	{
		SetEmitterRange( light, EM_Range( before.lightRadius ) );
	}
	ClampEmitterStart( light );
}

// a texture spot's frustum read as an emitter: the beam is the target, the left axis minus the right
// vector made perpendicular to it, both turned by the light's rotation; the cone from the frustum's
// slopes, the range out to the far plane
static void TextureSpotToEmitter( LightInfo& light )
{
	const float targetLength = light.lightTarget.Length();
	if( targetLength < 1e-3f )
	{
		// no beam to read: the rotation stays and the seed's cone does
		SetEmitterRange( light, 256.0f );
	}
	else
	{
		const idVec3 n = light.lightTarget / targetLength;
		const idVec3 start = light.explicitStartEnd ? light.lightStart : vec3_zero;
		const idVec3 end = light.explicitStartEnd ? light.lightEnd : light.lightTarget;
		const float zNear = Max( start * n, 8.0f );
		const float zFar = Max( end * n, 16.0f );

		idVec3 left = n * ( light.lightRight * n ) - light.lightRight;
		if( left.LengthSqr() < 1e-6f )
		{
			idVec3 unusedUp;
			n.OrthogonalBasis( left, unusedUp );
		}
		left.Normalize();

		const idMat3 beamFrame( n, left, n.Cross( left ) );
		light.angles = ( beamFrame * light.angles.ToMat3() ).ToAngles();

		SetEmitterCone( light.emitter, RAD2DEG( 2.0f * idMath::ATan( light.lightRight.Length() / targetLength ) ),
						RAD2DEG( 2.0f * idMath::ATan( light.lightUp.Length() / targetLength ) ) );
		SetEmitterRange( light, zNear + zFar );

		// an explicit light_start was a lit gap in front of the lamp: the emitter starts at the near
		// plane. Without one the start is what the block had (0 for a new block), so a spot switched
		// to Texture and back keeps it; the caller clamps it to the range
		if( light.explicitStartEnd )
		{
			light.emitter.start = zNear;
		}
	}

	// never read on an emitter, and deleted by ToDict
	light.lightTarget.Zero();
	light.lightRight.Zero();
	light.lightUp.Zero();
	light.lightStart.Zero();
	light.lightEnd.Zero();
	light.explicitStartEnd = false;
}

// the Emitter tab: the emit keys the light kept come back (emitter forced on), a light without them
// gets the seed; a spot's frustum becomes its direction, cone and range
static void SwitchToEmitter( LightInfo& light, const idDict& spawnArgs )
{
	idDict block;
	CopyPrefixKeys( spawnArgs, block, "emit" );
	block.Delete( "emitter" );

	bool keptAutoRange = false;
	if( block.GetNumKeyVals() > 0 )
	{
		block.Set( "emitter", "1" );
		// every kept key, as FromDict reads them ( DP-EC-01 )
		EM_ParseAllFromDict( &block, &light.emitter );
		keptAutoRange = block.GetBool( "emit_auto_range", "1" );
		light.autoRange = keptAutoRange;
	}
	else
	{
		SeedEmitter( light.emitter, light.lightType );
		light.autoRange = true;
	}

	// the light's first emitter block (no emit_effect yet) plays the flicker its style key gave it as a
	// texture light; style stays on the light, so the Texture tab plays it again
	if( spawnArgs.FindKey( "emit_effect" ) == NULL )
	{
		light.emitter.effect = ( light.lightStyle >= 1 && light.lightStyle <= EME_STYLE_LAST ) ? light.lightStyle : EME_NONE;
	}

	light.emitter.shape = ShapeForLightType( light.lightType );
	light.hasEmitterBlock = true;

	if( light.lightType == LIGHT_SPOT )
	{
		TextureSpotToEmitter( light );
	}

	// a kept block is read as FromDict reads one (G0 5, "a hand-set range wins"): against the range the
	// light has now, so a range set while it was a texture light is kept and the size does not move.
	// A seeded block has no stored flag; the size fits the range it finds
	if( block.GetNumKeyVals() > 0 )
	{
		EmitterAutoRangeOnRead( light, keptAutoRange );
	}
	EmitterRangeSet( light );
}

// the Texture tab: a spot gets the pyramid round its cone back, in the rotation it has. The engine puts
// its near plane at 8 and its far plane at 8 + T, so it ends at the range and the Emitter tab reads the
// same range back. ToDict then writes emitter "0" and leaves the emit_* keys on the light
static void SwitchToTexture( LightInfo& light )
{
	if( light.lightType == LIGHT_SPOT )
	{
		const float targetLength = Max( EM_Range( light.lightRadius ) - 8.0f, 16.0f );
		light.lightTarget.Set( targetLength, 0.0f, 0.0f );
		light.lightRight.Set( 0.0f, -targetLength * light.emitter.tanHalfH, 0.0f );
		light.lightUp.Set( 0.0f, 0.0f, targetLength * light.emitter.tanHalfV );
		light.explicitStartEnd = false;

		// as FromDict holds an implicit start and end
		light.lightStart = light.lightTarget * 0.25f;
		light.lightEnd = light.lightTarget;
	}

	light.emitter.enabled = false;
	light.hasEmitterBlock = true;
}

static bool EffectItemsGetter( void* /*data*/, int idx, const char** outText )
{
	static const char* waveNames[] = { "Strobe", "Pulse", "Flicker" };

	if( idx == EME_NONE )
	{
		*outText = "<No Effect>";
		return true;
	}
	if( idx > EME_NONE && idx <= EME_STYLE_LAST )
	{
		*outText = predef_lightstylesinfo[ idx ];
		return true;
	}
	if( idx >= EME_STROBE && idx < EME_NUM_EFFECTS )
	{
		*outText = waveNames[ idx - EME_STROBE ];
		return true;
	}

	*outText = "<Invalid Index!>";
	return false;
}

// the Emitter tab's controls; returns true when they changed the light
static bool DrawEmitterControls( LightInfo& light )
{
	emitterData_t& em = light.emitter;
	bool changed = false;

	static const char* shapeNames[] = { "omni", "spot", "sun" };
	ImGui::TextDisabled( "shape: %s (follows the light type above)", shapeNames[ idMath::ClampInt( 0, 2, em.shape ) ] );

	// a sun is not attenuated: it has no curve and no reach. Range, Auto range, Size and Start sit
	// together in the Light Volume section (DrawEmitterExtentRows); the curve's shape stays here
	if( light.lightType != LIGHT_SUN )
	{
		static const char* falloffOptions[] =
		{
			"Inverse Square (unbounded)",
			"Linear",
			"Smooth",
			"Exponential",
			"Inverse Square (clamped) - PBR safe"
		};
		int falloffIdx = idMath::ClampInt( 0, IM_ARRAYSIZE( falloffOptions ) - 1, em.falloff );
		if( ImGui::Combo( "Falloff", &falloffIdx, falloffOptions, IM_ARRAYSIZE( falloffOptions ) ) )
		{
			EmitterSetFalloff( light, falloffIdx );
			changed = true;
		}
	}

	if( light.lightType == LIGHT_SPOT )
	{
		ImGui::Spacing();
		float cone[2] = { em.coneH, em.coneV };
		if( ImGui::DragFloat2( "Cone (h, v)", cone, 0.25f, 1.0f, 120.0f, "%.1f" ) )
		{
			SetEmitterCone( em, cone[0], cone[1] );
			changed = true;
		}
		// AlwaysClamp: a CTRL+click typed value stays within G0 2's clamps, as the engine parses them (RV-C-ET-12)
		changed |= ImGui::SliderFloat( "Penumbra", &em.penumbra, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp );
		changed |= ImGui::SliderFloat( "Squareness (2=ellipse, 32=rect)", &em.squareness, 1.5f, 32.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp );
	}

	ImGui::Spacing();
	static const char* patternOptions[] =
	{
		"None",
		"Subsurface (warm halo)",
		"Ice (cool noise)",
		"Caustic (sharp noise)",
		"Banded (sine rings)",
		"Granular (stippled)"
	};
	int patternIdx = idMath::ClampInt( 0, IM_ARRAYSIZE( patternOptions ) - 1, em.pattern );
	if( ImGui::Combo( "Pattern", &patternIdx, patternOptions, IM_ARRAYSIZE( patternOptions ) ) )
	{
		em.pattern = patternIdx;
		changed = true;
	}
	if( em.pattern != EMP_NONE )
	{
		changed |= ImGui::SliderFloat( "Pattern Strength", &em.patternStrength, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp );
		// in world units (G0 5, "Pattern position"): the scale does not follow the range or the box
		changed |= ImGui::DragFloat( "Pattern Scale", &em.patternScale, 0.01f, 0.01f, 64.0f, "%.3f", ImGuiSliderFlags_Logarithmic );
		ImGui::SetItemTooltip( "1 is the reference size, fixed in the world: an ice cell is about 32 units across,\na caustic cell 43, a granular cell 8, a banded ring repeats every 80. 2 halves them." );
		changed |= ImGui::SliderFloat( "Pattern Phase", &em.patternPhase, 0.0f, 100.0f, "%.2f" );
	}

	ImGui::Spacing();
	changed |= ImGui::Checkbox( "Energy clamp (PBR safe)", &em.energyClamp );
	if( !em.energyClamp )
	{
		ImGui::SameLine();
		ImGui::TextDisabled( "(may blow out specular)" );
	}

	ImGui::Spacing();
	int effectIdx = idMath::ClampInt( 0, EME_NUM_EFFECTS - 1, em.effect );
	if( ImGui::Combo( "Effect", &effectIdx, EffectItemsGetter, NULL, EME_NUM_EFFECTS ) )
	{
		em.effect = effectIdx;
		changed = true;
	}
	if( em.effect != EME_NONE )
	{
		changed |= ImGui::SliderFloat( "Effect Speed", &em.effectSpeed, 0.0f, 100.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp );
		changed |= ImGui::SliderFloat( "Effect Amount", &em.effectAmount, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp );
		changed |= ImGui::DragFloat( "Effect Phase", &em.effectPhase, 0.01f, 0.0f, 0.0f, "%.3f" );
	}

	return changed;
}

/*
G0 9, "Range and reach in the panel": an emitter omni's or spot's extent, in this order: Range, Auto
range, Size, Start, and one line that says what follows what. Range is the largest radius; on an omni an
edit scales the box as a whole. A range edit only sets 'rangeEdited' (the caller runs EmitterRangeEdited
after the volume rows, as for a radius drag); a reach edit runs EmitterReachChanged here. Returns true
when a reach edit or the Auto range box changed the light. Never called for a sun or a texture light.
*/
static bool DrawEmitterExtentRows( LightInfo& light, bool& rangeEdited )
{
	emitterData_t& em = light.emitter;
	bool reachEdited = false;
	bool changed = false;

	// the label changes, the id after ### does not, so the drag is not dropped while auto range flips
	const bool inForce = EmitterAutoRangeInForce( light );
	float range = EM_Range( light.lightRadius );
	if( ImGui::DragFloat( inForce ? "Range (auto)###emRange" : "Range###emRange", &range, DragSpeedFor( range ), EMITTER_MIN_RANGE, 131072.0f, "%.1f" ) )
	{
		SetEmitterRange( light, range );
		rangeEdited = true;
	}

	// linear and smooth reach zero at the range: they have no size and no reach to follow
	const bool curveHasSize = ( em.falloff != EMF_LINEAR && em.falloff != EMF_SMOOTH );
	ImGui::BeginDisabled( !curveHasSize );
	if( ImGui::Checkbox( "Auto range", &light.autoRange ) )
	{
		// ticking sets the range to the auto range and keeps the size; unticking changes only the flag
		changed = true;
		reachEdited = light.autoRange;
	}
	reachEdited |= ImGui::DragFloat( "Size", &em.size, DragSpeedFor( em.size ), 1.0f, 65536.0f, "%.1f" );
	ImGui::EndDisabled();
	if( !curveHasSize )
	{
		ImGui::SameLine();
		ImGui::TextDisabled( "(unused by this curve)" );
	}

	reachEdited |= ImGui::DragFloat( "Start", &em.start, 1.0f, 0.0f, Max( range - 1.0f, 0.0f ), "%.1f" );

	if( !curveHasSize )
	{
		ImGui::TextDisabled( "This curve ends at the range: Range and Start set it." );
	}
	else if( inForce )
	{
		ImGui::TextDisabled( "Auto range: Range follows Intensity, Size, Start and Falloff; editing Range sets Size." );
	}
	else
	{
		ImGui::TextDisabled( "Auto range off: Range, Size and Start are each kept as set." );
	}

	if( reachEdited )
	{
		EmitterReachChanged( light );
		changed = true;
	}
	return changed;
}

// the one way the light's type changes, so the emitter's shape follows it
void LightInfo::SetLightType( ELightType type )
{
	lightType = type;
	emitter.shape = ShapeForLightType( type );
}

void LightInfo::Defaults()
{
	strTexture = "";
	equalRadius = true;
	explicitStartEnd = false;
	lightStart.Zero();
	lightEnd.Zero();
	lightUp.Zero();
	lightRight.Zero();
	lightTarget.Zero();
	lightCenter.Zero();
	color[0] = color[1] = color[2] = 1.0f;

	lightRadius.Zero();
	castShadows = true;
	skipSpecular = false;
	hasCenter = false;
	lightStyle = -1;

	hasLightOrigin = false;
	angles.Zero();
	scale.Set( 1, 1, 1 );

	memset( &emitter, 0, sizeof( emitter ) );
	autoRange = true;
	hasEmitterBlock = false;

	SetLightType( LIGHT_POINT );
}


// a type switch keeps the colour, light_origin and the emitter block with its editor flags
void LightInfo::RestoreKept( const LightInfo& kept )
{
	color = kept.color;
	hasLightOrigin = kept.hasLightOrigin;
	emitter = kept.emitter;
	autoRange = kept.autoRange;
	hasEmitterBlock = kept.hasEmitterBlock;

	// G0 9 (DP-E3-05): an emitter's shape switch keeps its shadow and specular switches; a texture
	// light's type switch resets them, as it always has
	if( kept.emitter.enabled )
	{
		castShadows = kept.castShadows;
		skipSpecular = kept.skipSpecular;
	}
}

void LightInfo::DefaultPoint()
{
	const LightInfo kept = *this;
	Defaults();
	RestoreKept( kept );

	SetLightType( LIGHT_POINT );
	lightRadius[0] = lightRadius[1] = lightRadius[2] = 300;
	equalRadius = true;
	ApplyEmitterShapeDefaults( *this, kept );
}

void LightInfo::DefaultProjected()
{
	const LightInfo kept = *this;
	Defaults();
	RestoreKept( kept );

	SetLightType( LIGHT_SPOT );
	lightTarget[2] = -256;
	lightUp[1] = -128;
	lightRight[0] = -128;
	ApplyEmitterShapeDefaults( *this, kept );
}

void LightInfo::DefaultSun()
{
	const LightInfo kept = *this;
	Defaults();
	RestoreKept( kept );

	SetLightType( LIGHT_SUN );
	lightCenter.Set( 4, 4, 32 );
	lightRadius[0] = lightRadius[1] = 2048;
	lightRadius[2] = 1024;
	equalRadius = false;
	ApplyEmitterShapeDefaults( *this, kept );
}

void LightInfo::FromDict( const idDict* e )
{
	if( e->GetVector( "light_origin", "", origin ) )
	{
		hasLightOrigin = true;
	}
	else
	{
		e->GetVector( "origin", "", origin );
	}

	lightRadius.Zero();
	lightTarget.Zero();
	lightRight.Zero();
	lightUp.Zero();
	lightStart.Zero();
	lightEnd.Zero();
	lightCenter.Zero();

	castShadows = !e->GetBool( "noshadows" );
	skipSpecular = e->GetBool( "nospecular" );

	strTexture = e->GetString( "texture" );

	bool isParallel = e->GetBool( "parallel" );

	if( !e->GetVector( "_color", "", color ) )
	{
		// NOTE: like the game, imgui uses color values between 0.0 and 1.0
		//       even though it displays them as 0 to 255
		color[0] = color[1] = color[2] = 1.0f;
	}

	if( e->GetVector( "light_right", "", lightRight ) )
	{
		// projected light
		lightType = LIGHT_SPOT;
		e->GetVector( "light_target", "", lightTarget );
		e->GetVector( "light_up", "", lightUp );
		if( e->GetVector( "light_start", "", lightStart ) )
		{
			// explicit start and end points
			explicitStartEnd = true;
			if( !e->GetVector( "light_end", "", lightEnd ) )
			{
				// no end, use target
				lightEnd = lightTarget;
			}
		}
		else
		{
			explicitStartEnd = false;

			// create a start a quarter of the way to the target
			lightStart = lightTarget * 0.25;
			lightEnd = lightTarget;
		}
	}
	else
	{
		lightType = isParallel ? LIGHT_SUN : LIGHT_POINT;

		if( e->GetVector( "light_radius", "", lightRadius ) )
		{
			equalRadius = ( lightRadius.x == lightRadius.y && lightRadius.x == lightRadius.z );
		}
		else
		{
			float radius = e->GetFloat( "light" );
			if( radius == 0 )
			{
				radius = 300;
			}
			lightRadius[0] = lightRadius[1] = lightRadius[2] = radius;
			equalRadius = true;
		}

		if( e->GetVector( "light_center", "", lightCenter ) )
		{
			hasCenter = true;
		}
	}

	// RB: Quake 1 light styles
	lightStyle = e->GetInt( "style", -1 );

	// the emitter block. Every key is read, the ones the shape, pattern or effect does not use too: ToDict
	// writes them all back ( DP-EC-01 )
	EM_ParseAllFromDict( e, &emitter );
	autoRange = e->GetBool( "emit_auto_range", "1" );
	hasEmitterBlock = ( e->MatchPrefix( "emit" ) != NULL );

	if( emitter.enabled )
	{
		// the shape is the light's type; radius and centre are read again because the frustum branch
		// above skips them while stale spot vectors are present, which an emitter never reads
		lightType = LightTypeForShape( emitter.shape );
		lightTarget.Zero();
		lightRight.Zero();
		lightUp.Zero();
		lightStart.Zero();
		lightEnd.Zero();
		explicitStartEnd = false;

		if( !e->GetVector( "light_radius", "", lightRadius ) )
		{
			lightRadius.Set( 300.0f, 300.0f, 300.0f );
		}
		for( int i = 0; i < 3; i++ )
		{
			lightRadius[ i ] = Max( lightRadius[ i ], 1.0f );
		}

		if( lightType == LIGHT_SPOT )
		{
			const float range = EM_Range( lightRadius );
			lightRadius.Set( range, range, range );
			lightCenter.Zero();
			hasCenter = false;
		}
		else
		{
			hasCenter = e->GetVector( "light_center", "", lightCenter );
			if( !hasCenter )
			{
				lightCenter.Zero();
			}
		}
		equalRadius = ( lightRadius.x == lightRadius.y && lightRadius.x == lightRadius.z );

		// G0 5: a range out of step with the auto range was set by hand, and wins
		EmitterAutoRangeOnRead( *this, autoRange );
	}

	// get the rotation matrix in either full form, or single angle form
	idMat3 axis;

	if( !e->GetMatrix( "light_rotation", "1 0 0 0 1 0 0 0 1", axis ) )
	{
		if( !e->GetMatrix( "rotation", "1 0 0 0 1 0 0 0 1", axis ) )
		{
			// RV-17: read light_angles in the engine's position (d3xp/Light.cpp), so the
			// rotation ToDict writes for a light_origin light round-trips
			if( e->GetAngles( "light_angles", "0 0 0", angles ) )
			{
				angles[ 0 ] = idMath::AngleNormalize360( angles[ 0 ] );
				angles[ 1 ] = idMath::AngleNormalize360( angles[ 1 ] );
				angles[ 2 ] = idMath::AngleNormalize360( angles[ 2 ] );

				axis = angles.ToMat3();
			}
			// RB: TrenchBroom interop
			// support "angles" like in Quake 3
			else if( e->GetAngles( "angles", "0 0 0", angles ) )
			{
				angles[ 0 ] = idMath::AngleNormalize360( angles[ 0 ] );
				angles[ 1 ] = idMath::AngleNormalize360( angles[ 1 ] );
				angles[ 2 ] = idMath::AngleNormalize360( angles[ 2 ] );

				axis = angles.ToMat3();
			}
			else
			{
				e->GetFloat( "angle", "0", angles[ 1 ] );

				angles[ 0 ] = 0;
				angles[ 1 ] = idMath::AngleNormalize360( angles[ 1 ] );
				angles[ 2 ] = 0;

				axis = angles.ToMat3();
			}
		}
	}

	// fix degenerate identity matrices
	axis[0].FixDegenerateNormal();
	axis[1].FixDegenerateNormal();
	axis[2].FixDegenerateNormal();

	angles = axis.ToAngles();
	scale.Set( 1, 1, 1 );
}

// the returned idDict is supposed to be used by idGameEdit::EntityChangeSpawnArgs()
// and thus will contain pairs with value "" if the key should be removed from entity
void LightInfo::ToDict( idDict* e )
{
	if( hasLightOrigin )
	{
		e->SetVector( "light_origin", origin );
	}
	else
	{
		e->SetVector( "origin", origin );
	}

	// idGameEdit::EntityChangeSpawnArgs() will delete key/value from entity,
	// if value is "" => use DELETE_VAL for readability
	static const char* DELETE_VAL = "";

	e->Set( "light", DELETE_VAL ); // we always use "light_radius" instead

	e->Set( "noshadows", ( !castShadows ) ? "1" : "0" );
	e->Set( "nospecular", ( skipSpecular ) ? "1" : "0" );

	if( strTexture.Length() > 0 )
	{
		e->Set( "texture", strTexture );
	}
	else
	{
		e->Set( "texture", DELETE_VAL );
	}

	// NOTE: e->SetVector() uses precision of 2, not enough for color
	e->Set( "_color", color.ToString( 4 ) );

	if( emitter.enabled )
	{
		// an emitter's volume is its range: an omni or sun keeps its box, a spot writes R R R; the
		// frustum vectors are never read on an emitter
		if( lightType == LIGHT_SPOT )
		{
			const float range = EM_Range( lightRadius );
			e->SetVector( "light_radius", idVec3( range, range, range ) );
			e->Set( "light_center", DELETE_VAL );
		}
		else
		{
			if( !equalRadius )
			{
				e->SetVector( "light_radius", lightRadius );
			}
			else
			{
				idVec3 tmp( lightRadius[0] ); // x, y and z have the same value
				e->SetVector( "light_radius", tmp );
			}

			if( hasCenter )
			{
				e->SetVector( "light_center", lightCenter );
			}
			else
			{
				e->Set( "light_center", DELETE_VAL );
			}
		}

		e->Set( "parallel", ( lightType == LIGHT_SUN ) ? "1" : DELETE_VAL );

		e->Set( "light_target", DELETE_VAL );
		e->Set( "light_up", DELETE_VAL );
		e->Set( "light_right", DELETE_VAL );
		e->Set( "light_start", DELETE_VAL );
		e->Set( "light_end", DELETE_VAL );
	}
	else if( lightType == LIGHT_POINT || lightType == LIGHT_SUN )
	{
		if( !equalRadius )
		{
			e->SetVector( "light_radius", lightRadius );
		}
		else
		{
			idVec3 tmp( lightRadius[0] ); // x, y and z have the same value
			e->SetVector( "light_radius", tmp );
		}

		if( hasCenter )
		{
			e->SetVector( "light_center", lightCenter );
		}
		else
		{
			e->Set( "light_center", DELETE_VAL );
		}

		e->Set( "parallel", ( lightType == LIGHT_SUN ) ? "1" : DELETE_VAL );

		// get rid of all the projected light specific stuff
		e->Set( "light_target", DELETE_VAL );
		e->Set( "light_up", DELETE_VAL );
		e->Set( "light_right", DELETE_VAL );
		e->Set( "light_start", DELETE_VAL );
		e->Set( "light_end", DELETE_VAL );
	}
	else
	{
		e->SetVector( "light_target", lightTarget );
		e->SetVector( "light_up", lightUp );
		e->SetVector( "light_right", lightRight );
		if( explicitStartEnd )
		{
			e->SetVector( "light_start", lightStart );
			e->SetVector( "light_end", lightEnd );
		}
		else
		{
			e->Set( "light_start", DELETE_VAL );
			e->Set( "light_end", DELETE_VAL );
		}

		// get rid of the pointlight specific stuff
		e->Set( "light_radius", DELETE_VAL );
		e->Set( "light_center", DELETE_VAL );
		e->Set( "parallel", DELETE_VAL );
	}

	// RB: Quake 1 light styles
	if( lightStyle != -1 && lightType != LIGHT_SUN )
	{
		e->SetInt( "style", lightStyle );
	}
	else
	{
		e->Set( "style", DELETE_VAL );
	}

	e->Set( "rotation", DELETE_VAL );
	e->Set( "light_rotation", DELETE_VAL );

	if( hasLightOrigin )
	{
		// "angle" stays: on a light_origin light it orients the fixture, and both the light and
		// FromDict read light_angles before it
		e->SetAngles( "light_angles", angles );
	}
	else
	{
		// V-B13: FromDict has folded a legacy yaw into the angles, so a kept "angle" would
		// resurrect it whenever the rotation is zeroed
		e->Set( "angle", DELETE_VAL );

		// the engine reads light_angles before angles, so a stale one would hide every edit
		e->Set( "light_angles", DELETE_VAL );

		if( angles.yaw != 0.0f || angles.pitch != 0.0f || angles.roll != 0.0f )
		{
			e->SetAngles( "angles", angles );
		}
		else
		{
			e->Set( "angles", DELETE_VAL );
		}
	}
	// only an emitter, or a light that carries an emitter block, writes emit keys: a disabled block
	// writes emitter "0" and leaves its emit_* keys on the light, so the Emitter tab brings them back.
	if( emitter.enabled || hasEmitterBlock )
	{
		emitterData_t block = emitter;
		block.shape = ShapeForLightType( lightType );
		EM_WriteToDict( e, block, /*deleteWhenDisabled=*/ false );
		if( emitter.enabled )
		{
			e->Set( "emit_auto_range", autoRange ? "1" : "0" );
		}
	}
}

LightInfo::LightInfo()
{
	Defaults();
}


// ########### LightEditor #############

LightEditor& LightEditor::Instance()
{
	static LightEditor instance;
	return instance;
}


// static
void LightEditor::ReInit( const idDict* dict, idEntity* light )
{
	Instance().Init( dict, light );
}

void LightEditor::Init( const idDict* dict, idEntity* light )
{
	// D31 / RV-6: every selection (E0 LMB, the panel's FireWeapon pick, Ctrl+D) starts in AUTO with
	// no checkpoint and no modal; mode, the press queue, the key
	// latches, the delta reference and the committed list survive
	easyState = EASY_AUTO;
	modalKey = 0;
	modalX = 0;
	modalY = 0;
	modalWheel = 0;
	checkpointUndo.Defaults();
	checkpointEmit.Clear();
	modalBase.Defaults();
	modalEntry.Defaults();
	lastAimHit = false;

	// MA-8: the selected light picked again (a panel pick held past SelectEntity's 300 ms debounce,
	// or F out of AUTO and a re-pick) keeps its original and is not seeded again: its spawnArgs now
	// hold the preview, and a fresh FromDict would make Cancel restore the preview
	if( light != NULL && light == lightEntity )
	{
		AutoCaptureBase( cur, autoBase );
		return;
	}

	Reset();

	if( textureNames.Num() == 0 )
	{
		LoadLightTextures();
	}

	if( styleNames.Num() == 0 )
	{
		LoadLightStyles();
	}

	if( dict )
	{
		original.FromDict( dict );
		cur.FromDict( dict );
		CopyPrefixKeys( *dict, originalEmit, "emit" );

		gameEdit->EntityGetOrigin( light, entityPos );

		// D24: the locator matches an unnamed map light by the "origin" it was spawned with
		dict->GetVector( "origin", "", spawnOrigin );

		// EA-2 / EB-2: a bound light keeps its master-relative pose, so the preview cannot show it
		// moving (only a map "bind" key is seen here; a script bind is not)
		if( dict->GetString( "bind" )[0] != '\0' )
		{
			common->Warning( "LightEditor: this light is bound to '%s': origin and rotation edits are committed but not previewed", dict->GetString( "bind" ) );
		}

		const char* name = dict->GetString( "name", NULL );
		entityNameFromDict = ( name != NULL );
		if( name )
		{
			entityName = name;
			//title.Format( "Light Editor: %s at (%s)", name, entityPos.ToString() );
		}
		else
		{
			//idassert( 0 && "LightEditor::Init(): Given entity has no 'name' property?!" );
			entityName = gameEdit->GetUniqueEntityName( "light" );

			//title.Format( "Light Editor: <unnamed> light at (%s)", entityPos.ToString() );
		}

		// D6: one ImGui window id with and without a light, so deselecting does not move the panel
		title = "Light Editor###LightEditor";

		currentTextureIndex = 0;
		currentTexture = NULL;
		if( original.strTexture.Length() > 0 )
		{
			const char* curTex = original.strTexture.c_str();
			for( int i = 0; i < textureNames.Num(); ++i )
			{
				if( textureNames[i] == curTex )
				{
					currentTextureIndex = i + 1; // remember, 0 is "<No Texture>"
					LoadCurrentTexture();
					break;
				}
			}
		}

		// RB: light styles
		if( original.lightStyle >= 0 )
		{
			currentStyleIndex = original.lightStyle + 1;
		}
	}
	else
	{
		// no previous light's locator inputs may survive into a commit (D30)
		entityName.Clear();
		entityNameFromDict = false;
		spawnOrigin.Zero();
	}

	// D24: DuplicateLight raises the flag right before calling Init for its copy
	lightIsNew = duplicateIsNew;
	duplicateIsNew = false;

	this->lightEntity = light;

	// plan 3.3: AUTO keeps the endpoint dimensions the light has when it is entered
	AutoCaptureBase( cur, autoBase );
}

void LightEditor::Reset()
{
	title = "Light Editor: no Light selected!###LightEditor";
	entityPos.x = idMath::INFINITUM;
	entityPos.y = idMath::INFINITUM;
	entityPos.z = idMath::INFINITUM;

	original.Defaults();
	originalEmit.Clear();
	cur.Defaults();

	lightEntity = NULL;
	currentTextureIndex = 0;
	currentTexture = NULL;
	currentTextureMaterial = NULL;
	currentStyleIndex = 0;

	mCurrentGizmoOperation = ImGuizmo::TRANSLATE;
	mCurrentGizmoMode = ImGuizmo::WORLD;

	useSnap = false;
	//snap = { 1.f, 1.f, 1.f };
	//bounds[] = { -0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f };
	//boundsSnap[] = { 0.1f, 0.1f, 0.1f };
	boundSizing = false;
	boundSizingSnap = false;

	shortcutSaveMapEnabled = true;
	shortcutDuplicateLightEnabled = true;

	// a light selected black shows white until it is given a colour
	panelHue.Set( 1.0f, 1.0f, 1.0f );
}

namespace
{
class idSort_textureNames : public idSort_Quick< idStr, idSort_textureNames >
{
public:
	int Compare( const idStr& a, const idStr& b ) const
	{
		return a.Icmp( b );
	}
};
} //anon. namespace

void LightEditor::LoadLightTextures()
{
	textureNames.Clear();

	int count = declManager->GetNumDecls( DECL_MATERIAL );

	for( int i = 0; i < count; i++ )
	{
		// just get the name of the light material
		const idMaterial* mat = declManager->MaterialByIndex( i, false );

		idStr matName = mat->GetName();
		matName.ToLower();

		if( matName.Icmpn( "lights/", strlen( "lights/" ) ) == 0 || matName.Icmpn( "fogs/", strlen( "fogs/" ) ) == 0 )
		{
			// actually load the material
			const idMaterial* material = declManager->FindMaterial( matName, false );
			if( material != NULL )
			{
				// check if the material has textures or is just a leftover from the development
				idImage* editorImage = mat->GetLightEditorImage();
				if( !editorImage->IsLoaded() )
				{
					editorImage->DeferredLoadImage();
				}

				if( !editorImage->IsDefaulted() )
				{
					textureNames.Append( matName );
				}
			}
		}
	}

	textureNames.SortWithTemplate( idSort_textureNames() );
}

// static
bool LightEditor::TextureItemsGetter( void* data, int idx, const char** outText )
{
	LightEditor* self = static_cast<LightEditor*>( data );
	if( idx == 0 )
	{
		*outText = "<No Texture>";
		return true;
	}

	// as index 0 has special purpose, the "real" index is one less
	--idx;

	if( idx < 0 || idx >= self->textureNames.Num() )
	{
		*outText = "<Invalid Index!>";
		return false;
	}

	*outText = self->textureNames[idx].c_str();

	return true;
}

void LightEditor::LoadCurrentTexture()
{
	currentTexture = NULL;

	if( currentTextureIndex > 0 && cur.strTexture.Length() > 0 )
	{
		const idMaterial* mat = declManager->FindMaterial( cur.strTexture, false );
		if( mat != NULL )
		{
			currentTexture = mat->GetLightEditorImage();
			if( currentTexture )
			{
				// RB: create extra 2D material of the image for UI rendering

				// HACK that deserves being called a hack
				idStr uiName( "lighteditor/" );
				uiName += currentTexture->GetName();

				currentTextureMaterial = declManager->FindMaterial( uiName, true );
			}
		}
	}
}

void LightEditor::LoadLightStyles()
{
	styleNames.Clear();

	const idDeclEntityDef* decl = static_cast<const idDeclEntityDef*>( declManager->FindType( DECL_ENTITYDEF, "light", false ) );
	if( decl == NULL )
	{
		return;
	}

	int numStyles = decl->dict.GetInt( "num_styles", "0" );
	if( numStyles > 0 )
	{
		for( int i = 0; i < numStyles; i++ )
		{
			idStr style = decl->dict.GetString( va( "light_style%d", i ) );
			styleNames.Append( style );
		}
	}
	else
	{
		// RB: it's not defined in entityDef light so use predefined Quake 1 table
		for( int i = 0; i < 12; i++ )
		{
			idStr style( predef_lightstylesinfo[ i ] );
			styleNames.Append( style );
		}
	}
}

// static
bool LightEditor::StyleItemsGetter( void* data, int idx, const char** outText )
{
	LightEditor* self = static_cast<LightEditor*>( data );
	if( idx == 0 )
	{
		*outText = "<No Lightstyle>";
		return true;
	}

	// as index 0 has special purpose, the "real" index is one less
	--idx;

	if( idx < 0 || idx >= self->styleNames.Num() )
	{
		*outText = "<Invalid Index!>";
		return false;
	}

	*outText = self->styleNames[idx].c_str();

	return true;
}

void LightEditor::TempApplyChanges()
{
	applyPending = false;

	if( lightEntity != NULL )
	{
		idDict d;
		cur.ToDict( &d );

		gameEdit->EntityChangeSpawnArgs( lightEntity, &d );
		gameEdit->EntityUpdateChangeableSpawnArgs( lightEntity, NULL );
	}
}

int LightEditor::FindCommit( const char* name ) const
{
	for( int i = 0; i < committed.Num(); i++ )
	{
		if( committed[i].entityName.Icmp( name ) == 0 )
		{
			return i;
		}
	}
	return -1;
}

// D24: a name no spawned entity, no in-memory map entity and no committed light uses yet;
// GetUniqueEntityName only looks at spawned entities and is deterministic, so it would hand
// out a name already given to an unnamed map light in this session
idStr LightEditor::InventLightName() const
{
	// MAX_GENTITIES is game-private; the bound is far above any map's entity count
	idStr name;
	for( int id = 0; id < ( 1 << 16 ); id++ )
	{
		name.Format( "light_%d", id );
		if( gameEdit->FindEntity( name ) == NULL && gameEdit->MapGetEntityDict( name ) == NULL && FindCommit( name ) < 0 )
		{
			break;
		}
	}
	return name;
}

// D24: resolves once, at the light's first commit, where it lives in the in-memory map. An
// unnamed map light is matched by its spawn origin and given a name, which goes into 'dict' only:
// R6-1, SaveChanges makes it real (live spawnArgs, in-memory map, entityName) once the commit
// stands, so a failed write leaves no half-given name another light could be handed. Returns the
// in-memory index to copy into (-1: copy by name), or -2 when the light cannot be found.
int LightEditor::ResolveLocator( LightCommit& commit, idDict& dict )
{
	const idDict* spawnArgs = gameEdit->EntityGetSpawnArgs( lightEntity );

	commit.entityName = entityName;
	commit.isNew = lightIsNew;
	commit.writeName = false;
	commit.pending = false;
	commit.name = entityName;
	commit.classname = ( spawnArgs != NULL ) ? spawnArgs->GetString( "classname", "light" ) : "light";
	commit.onDiskOrigin.Zero();
	commit.indexHint = -1;

	if( lightIsNew )
	{
		// Ctrl+D put it into the in-memory map under its invented name (MapAddEntity)
		return ( gameEdit->MapGetEntityDict( entityName ) != NULL ) ? -1 : -2;
	}

	// a name Init invented is not the light's own and may belong to another map entity
	if( entityNameFromDict )
	{
		const idDict* mapDict = gameEdit->MapGetEntityDict( entityName );
		if( mapDict != NULL )
		{
			// no index accessor by name exists; indexHint is only a tie-breaker
			mapDict->GetVector( "origin", "", commit.onDiskOrigin );
			return -1;
		}
	}

	// unnamed on disk (or named only at runtime): the unique unnamed map light at the spawn origin
	const int index = gameEdit->MapFindEntityAtOrigin( spawnOrigin, commit.classname );
	if( index < 0 )
	{
		return -2;
	}

	// a runtime name is free in the map (the lookup above failed); otherwise invent one
	const idStr name = entityNameFromDict ? entityName : InventLightName();

	commit.entityName = name;
	commit.name = name;
	commit.writeName = true;
	commit.onDiskOrigin = spawnOrigin;
	commit.indexHint = index;

	dict.Set( "name", name );

	return index;
}

/*
D48 / D49: exactly two things write the .map. Enter in easy mode writes this one light at once
(flushNow, onlyThisLight). The panel's save (Ctrl+S, File > Save Map: flushNow alone) commits
the selected light and writes every pending panel Apply. Apply and Ctrl+D commit in memory only (the
original editor's behaviour), pending until that save. Returns false when nothing was committed.
*/
bool LightEditor::SaveChanges( bool flushNow, bool onlyThisLight )
{
	// D30: a commit needs a light; a stale entityName would overwrite another light's entry
	if( lightEntity == NULL )
	{
		// the panel's save with nothing selected still writes what was applied
		if( flushNow && !onlyThisLight )
		{
			Flush( NULL );
		}
		return false;
	}

	idDict d;
	cur.ToDict( &d );

	// only a light with its own name key can already be in the committed list
	int commitNum = entityNameFromDict ? FindCommit( entityName ) : -1;
	int mapIndex = -1;
	bool resolvedNow = false;
	if( commitNum < 0 )
	{
		LightCommit resolved;
		mapIndex = ResolveLocator( resolved, d );
		if( mapIndex < -1 )
		{
			// D24: never a silent no-op; original stays, nothing is pending
			lastWarning.Format( "light at ( %s ) not found in the map: not committed", cur.origin.ToString() );
			common->Warning( "LightEditor: %s", lastWarning.c_str() );

			// R6-4: the panel's save still writes the other pending Applies
			if( flushNow && !onlyThisLight )
			{
				const idStr failure = lastWarning;
				Flush( NULL );
				lastWarning = failure;
			}
			return false;
		}
		commitNum = committed.Append( resolved );
		resolvedNow = true;
	}

	if( committed[ commitNum ].writeName )
	{
		d.Set( "name", committed[ commitNum ].name );
	}

	if( flushNow )
	{
		// D48: stage this light and write it alone first; on any failure its entry goes back to what
		// it was (or away), so no later save writes it and the light is left as it was (R6-1: an
		// invented name is made real only below, once written)
		const idDict previousDict = committed[ commitNum ].dict;
		const bool previousPending = committed[ commitNum ].pending;
		committed[ commitNum ].dict = d;
		committed[ commitNum ].pending = true;

		const idStr stagedName = committed[ commitNum ].entityName;
		const bool written = ( Flush( stagedName ) == 0 );
		if( !written )
		{
			if( resolvedNow )
			{
				committed.RemoveIndex( commitNum );
			}
			else
			{
				committed[ commitNum ].dict = previousDict;
				committed[ commitNum ].pending = previousPending;
			}
		}

		if( !onlyThisLight )
		{
			// R6-4: the panel's save writes every other pending Apply whatever became of this light;
			// this light's failure stays the warning shown
			const idStr failure = lastWarning;
			Flush( NULL );
			if( !written )
			{
				lastWarning = failure;
			}
		}

		if( !written )
		{
			return false;
		}
	}

	LightCommit& commit = committed[ commitNum ];

	// R6-1: the commit stands (written, or an in-memory Apply), so an invented name becomes real now
	// and only now: live spawnArgs, entityName and (by the copy below) the in-memory map
	if( resolvedNow && commit.writeName )
	{
		idDict nameArgs;
		nameArgs.Set( "name", commit.name );
		gameEdit->EntityChangeSpawnArgs( lightEntity, &nameArgs );
		entityName = commit.name;
		entityNameFromDict = true;
	}

	// U-1: into the in-memory map as before; by index only right after an origin resolution
	if( mapIndex >= 0 )
	{
		gameEdit->MapCopyDictToEntityByIndex( mapIndex, &d );
	}
	else
	{
		gameEdit->MapCopyDictToEntity( commit.name, &d );
	}

	original = cur;

	// Cancel now goes back to this commit: the light's emit keys once d is on it
	originalEmit.Clear();
	CopyPrefixKeys( *gameEdit->EntityGetSpawnArgs( lightEntity ), originalEmit, "emit" );
	ApplyPrefixKeys( originalEmit, d, "emit" );

	if( flushNow )
	{
		// written above (D34 / U-5: the splice is the light editor's only .map writer)
		if( onlyThisLight )
		{
			lastWarning.Clear();
		}
		hintText.Format( "saved %s", entityName.c_str() );
		hintUntilFrame = idLib::frameNumber + 90;
		return true;
	}

	// an in-memory commit (Apply, Ctrl+D): pending until the panel's save
	commit.dict = d;
	commit.pending = true;
	lastWarning.Clear();
	return true;
}

// D48: panel Applies not yet saved
int LightEditor::PendingApplies() const
{
	int pending = 0;
	for( int i = 0; i < committed.Num(); i++ )
	{
		if( committed[ i ].pending )
		{
			pending++;
		}
	}
	return pending;
}

// D48: closing never writes; it says what it drops (one console line, no other engine or ImGui call)
void LightEditor::WarnUnsavedApplies() const
{
	const int pending = PendingApplies();
	if( pending > 0 )
	{
		common->Warning( "LightEditor: %d light(s) applied in the panel were never saved (Ctrl+S); the .map keeps them as they were", pending );
	}
}

void LightEditor::CancelChanges()
{
	if( lightEntity != NULL )
	{
		idDict d;
		original.ToDict( &d );
		RestorePrefixKeys( originalEmit, *gameEdit->EntityGetSpawnArgs( lightEntity ), d, "emit" );

		gameEdit->EntityChangeSpawnArgs( lightEntity, &d );
		gameEdit->EntityUpdateChangeableSpawnArgs( lightEntity, NULL );
	}
}

void LightEditor::DuplicateLight()
{
	if( lightEntity != NULL )
	{
		// store current light properties to game idMapFile (D48: an in-memory commit like Apply,
		// written by the panel's save)
		SaveChanges( false, false );

		// spawn the new light; it keeps the emit block a Texture tab left on the source (ToDict does not
		// write a kept block's values; DeleteEmptyKeys drops the keys ToDict clears)
		idDict d;
		CopyPrefixKeys( *gameEdit->EntityGetSpawnArgs( lightEntity ), d, "emit" );
		cur.ToDict( &d );
		d.DeleteEmptyKeys();

		// D24: the splice appends a new light only under a name no map entity carries; the source
		// light keeps its own name until the copy exists (Init takes the new one from 'd')
		const idStr newName = InventLightName();
		d.Set( "name", newName );
		d.Set( "classname", "light" );

		idEntity* light = NULL;
		gameEdit->SpawnEntityDef( d, &light );

		if( light )
		{
			gameEdit->MapAddEntity( &d );
			gameEdit->ClearEntitySelection();
			gameEdit->AddSelectedEntity( light );

			duplicateIsNew = true;
			Init( &d, light );
		}
	}
}

// ########### map splice (U-5, D34, plan 3.5) #############

namespace
{

// one top-level entity block of the map text
class LightMapBlock
{
public:
	int			start;			// offset of the opening '{'
	int			end;			// offset one past the matching '}'
	int			ordinal;		// entity number in file order
	int			commit;			// the committed light spliced into this block, -1 for none
	bool		nested;			// holds brushes or patches
	bool		broken;			// a key without its value on the same line
	bool		hasName;
	idStr		classname;
	idStr		name;
	idVec3		origin;			// ( 0, 0, 0 ) without an "origin" key, as the engine reads it
};

class LightSpliceResult
{
public:
	bool		spliced;
	idVec3		origin;			// the block's "origin" as written
};

} //anon. namespace

// FindEntityAtOrigin's tolerance, so the splice agrees with the in-memory match
static bool LightMapOriginNear( const idVec3& a, const idVec3& b )
{
	idBounds bo( a );
	bo.ExpandSelf( 0.125f );
	return bo.ContainsPoint( b );
}

/*
plan 3.5 step 2: records every top-level entity block. Accepts exactly: '{' '}' nesting; double-quoted
strings with no escape characters that never cross a line; '//' comments to the end of the line
(no C block comments, which TrenchBroom never writes); an entity's key and value on one line. A block with a
nested '{' (brushes, patches) or a key whose value is not on its line is recorded but marked, and
the splice refuses it. No primitive is parsed. Returns false on unbalanced braces or a broken
string. A file with no blocks scans fine and has nothing to locate.
*/
static bool LightMapScan( const idStr& text, idList<LightMapBlock>& blocks )
{
	const char* s = text.c_str();
	const int len = text.Length();

	// one record per entity; a large granularity keeps a big map from re-copying the list
	blocks.Clear();
	blocks.SetGranularity( 256 );

	LightMapBlock* block = NULL;
	int depth = 0;
	int keyStart = -1;
	int keyEnd = -1;
	idStr value;

	for( int i = 0; i < len; i++ )
	{
		const char c = s[ i ];

		if( c == '/' && i + 1 < len && s[ i + 1 ] == '/' )
		{
			while( i + 1 < len && s[ i + 1 ] != '\n' )
			{
				i++;
			}
			continue;
		}

		if( c == '\n' && depth == 1 && keyStart >= 0 )
		{
			// the engine would read this key's value from the next line (MapFile.cpp ReadTokenOnLine)
			block->broken = true;
			continue;
		}

		if( c == '"' )
		{
			int j = i + 1;
			while( j < len && s[ j ] != '"' && s[ j ] != '\n' )
			{
				j++;
			}
			if( j >= len || s[ j ] != '"' )
			{
				return false;
			}

			// only an entity's own key/value pairs matter, never a primitive's strings
			if( depth == 1 )
			{
				if( keyStart < 0 )
				{
					keyStart = i + 1;
					keyEnd = j;
				}
				else
				{
					// only the three keys the locator reads are copied
					const int keyLen = keyEnd - keyStart;
					if( keyLen == 9 && idStr::Icmpn( s + keyStart, "classname", 9 ) == 0 )
					{
						block->classname.CopyRange( s, i + 1, j );
					}
					else if( keyLen == 4 && idStr::Icmpn( s + keyStart, "name", 4 ) == 0 )
					{
						block->name.CopyRange( s, i + 1, j );
						block->hasName = ( j > i + 1 );
					}
					else if( keyLen == 6 && idStr::Icmpn( s + keyStart, "origin", 6 ) == 0 )
					{
						value.CopyRange( s, i + 1, j );
						block->origin.Zero();
						sscanf( value.c_str(), "%f %f %f", &block->origin.x, &block->origin.y, &block->origin.z );
					}
					keyStart = -1;
				}
			}
			i = j;
			continue;
		}

		if( c == '{' )
		{
			if( depth == 0 )
			{
				block = &blocks.Alloc();
				block->start = i;
				block->end = -1;
				block->ordinal = blocks.Num() - 1;
				block->commit = -1;
				block->nested = false;
				block->broken = false;
				block->hasName = false;
				block->classname.Empty();
				block->name.Empty();
				block->origin.Zero();
			}
			else
			{
				block->nested = true;
			}
			keyStart = -1;
			depth++;
			continue;
		}

		if( c == '}' )
		{
			if( depth == 0 )
			{
				return false;
			}
			if( depth == 1 && keyStart >= 0 )
			{
				// a key with no value before the closing brace
				block->broken = true;
			}
			depth--;
			if( depth == 0 )
			{
				block->end = i + 1;
			}
			keyStart = -1;
		}
	}

	return ( depth == 0 );
}

/*
plan 3.5 step 3: the block a committed light lives in; -1 = append it (a new light), -2 = skip (warned).
Never splices by an invented name alone and never appends an existing light.
*/
static int LightMapLocate( const idList<LightMapBlock>& blocks, const LightCommit& commit )
{
	int named = -1;
	int namedCount = 0;
	for( int b = 0; b < blocks.Num(); b++ )
	{
		if( blocks[ b ].hasName && blocks[ b ].name.Icmp( commit.name ) == 0 )
		{
			named = b;
			namedCount++;
		}
	}

	if( namedCount > 1 )
	{
		common->Warning( "LightEditor: '%s' names %d blocks in the map file; not written", commit.name.c_str(), namedCount );
		return -2;
	}

	if( namedCount == 1 )
	{
		const LightMapBlock& block = blocks[ named ];
		if( commit.isNew )
		{
			// the name was invented in this session, so the block is someone else's
			common->Warning( "LightEditor: the map file already has an entity named '%s'; the new light is not written", commit.name.c_str() );
			return -2;
		}
		if( block.classname.Icmp( commit.classname ) != 0 || !LightMapOriginNear( commit.onDiskOrigin, block.origin ) )
		{
			// TrenchBroom moved or replaced it since the map was loaded
			common->Warning( "LightEditor: '%s' was changed in the map file since it was loaded; not written", commit.name.c_str() );
			return -2;
		}
		return named;
	}

	if( commit.isNew )
	{
		return -1;
	}

	// an existing light whose name is in no block: the unique unnamed block of its class at its
	// on-disk origin; the in-memory index only breaks a tie
	int found = -1;
	int hinted = -1;
	int count = 0;
	for( int b = 0; b < blocks.Num(); b++ )
	{
		const LightMapBlock& block = blocks[ b ];
		if( !block.hasName && block.classname.Icmp( commit.classname ) == 0 && LightMapOriginNear( commit.onDiskOrigin, block.origin ) )
		{
			found = b;
			count++;
			if( block.ordinal == commit.indexHint )
			{
				hinted = b;
			}
		}
	}

	if( count == 1 )
	{
		return found;
	}
	if( count > 1 && hinted >= 0 )
	{
		return hinted;
	}

	common->Warning( "LightEditor: '%s' at ( %s ): %s in the map file; not written", commit.name.c_str(), commit.onDiskOrigin.ToString(),
					 ( count == 0 ) ? "no matching block" : "several matching blocks" );
	return -2;
}

/*
plan 3.5 step 4: appends 'block' rewritten with 'dict' to 'out'. A value is replaced in place only when
its string differs, a "" value removes its line, a key the block lacks goes in before the closing
'}'; every other line, comment and the order are kept.
*/
static void LightMapRewriteBlock( const idStr& text, const LightMapBlock& block, const idDict& dict, const char* eol, idStr& key, idList<bool>& found, idStr& out )
{
	const char* s = text.c_str();
	const int closing = block.end - 1;
	int copyFrom = block.start;
	int lineStart = block.start + 1;
	int keyStart = -1;
	int keyEnd = -1;

	// 'key' and 'found' are the caller's scratch, reused for every block of one splice
	found.SetNum( dict.GetNumKeyVals() );
	for( int k = 0; k < found.Num(); k++ )
	{
		found[ k ] = false;
	}

	for( int i = block.start + 1; i < closing; i++ )
	{
		const char c = s[ i ];

		if( c == '\n' )
		{
			lineStart = i + 1;
			continue;
		}

		if( c == '/' && s[ i + 1 ] == '/' )
		{
			while( i + 1 < closing && s[ i + 1 ] != '\n' )
			{
				i++;
			}
			continue;
		}

		if( c != '"' )
		{
			continue;
		}

		// LightMapScan has validated every string
		int j = i + 1;
		while( s[ j ] != '"' )
		{
			j++;
		}

		if( keyStart < 0 )
		{
			keyStart = i + 1;
			keyEnd = j;
			i = j;
			continue;
		}

		const int pairStart = keyStart - 1;
		const int pairEnd = j + 1;
		const int valueStart = i + 1;
		const int valueEnd = j;
		key.CopyRange( s, keyStart, keyEnd );
		keyStart = -1;
		i = j;

		const int k = dict.FindKeyIndex( key );
		if( k < 0 )
		{
			continue;
		}
		found[ k ] = true;

		const idStr& newValue = dict.GetKeyVal( k )->GetValue();
		if( newValue.IsEmpty() )
		{
			// the whole line goes when it holds nothing but this pair
			int lineEnd = pairEnd;
			while( lineEnd < closing && ( s[ lineEnd ] == ' ' || s[ lineEnd ] == '\t' || s[ lineEnd ] == '\r' ) )
			{
				lineEnd++;
			}
			bool onlyPair = ( lineEnd < closing && s[ lineEnd ] == '\n' );
			for( int p = lineStart; p < pairStart && onlyPair; p++ )
			{
				onlyPair = ( s[ p ] == ' ' || s[ p ] == '\t' );
			}

			if( onlyPair )
			{
				out.Append( s + copyFrom, lineStart - copyFrom );
				copyFrom = lineEnd + 1;
				i = lineEnd;
				lineStart = lineEnd + 1;
			}
			else
			{
				out.Append( s + copyFrom, pairStart - copyFrom );
				copyFrom = pairEnd;
			}
		}
		else if( newValue.Length() != valueEnd - valueStart || idStr::Cmpn( s + valueStart, newValue.c_str(), valueEnd - valueStart ) != 0 )
		{
			out.Append( s + copyFrom, valueStart - copyFrom );
			out.Append( newValue );
			copyFrom = valueEnd;
		}
	}

	// missing keys go on lines of their own right before the closing brace
	int insertAt = closing;
	while( insertAt > copyFrom && ( s[ insertAt - 1 ] == ' ' || s[ insertAt - 1 ] == '\t' ) )
	{
		insertAt--;
	}
	const bool closingOwnsLine = ( s[ insertAt - 1 ] == '\n' );
	if( !closingOwnsLine )
	{
		insertAt = closing;
	}

	out.Append( s + copyFrom, insertAt - copyFrom );

	bool first = true;
	for( int k = 0; k < dict.GetNumKeyVals(); k++ )
	{
		const idKeyValue* kv = dict.GetKeyVal( k );
		if( found[ k ] || kv->GetValue().IsEmpty() )
		{
			continue;
		}
		if( first && !closingOwnsLine )
		{
			out += eol;
		}
		first = false;

		out += "\"";
		out += kv->GetKey();
		out += "\" \"";
		out += kv->GetValue();
		out += "\"";
		out += eol;
	}

	out.Append( s + insertAt, block.end - insertAt );
}

// plan 3.5 step 3c: a light created in this session, as a new block
static void LightMapAppendBlock( const LightCommit& commit, const char* eol, idStr& out )
{
	out += "{";
	out += eol;
	out += "\"classname\" \"";
	out += commit.classname;
	out += "\"";
	out += eol;
	out += "\"name\" \"";
	out += commit.name;
	out += "\"";
	out += eol;

	for( int k = 0; k < commit.dict.GetNumKeyVals(); k++ )
	{
		const idKeyValue* kv = commit.dict.GetKeyVal( k );
		if( kv->GetValue().IsEmpty() || kv->GetKey().Icmp( "classname" ) == 0 || kv->GetKey().Icmp( "name" ) == 0 )
		{
			continue;
		}
		out += "\"";
		out += kv->GetKey();
		out += "\" \"";
		out += kv->GetValue();
		out += "\"";
		out += eol;
	}

	out += "}";
	out += eol;
}

// the "origin" a spliced block ends up with
static idVec3 LightMapWrittenOrigin( const idDict& dict, const idVec3& blockOrigin )
{
	if( dict.FindKey( "origin" ) == NULL )
	{
		return blockOrigin;
	}
	// a "" value deleted the key, which the engine then reads as ( 0, 0, 0 )
	return dict.GetVector( "origin", "0 0 0" );
}

/*
plan 3.5 steps 2-4 on map text already in memory (no file access, so le_selfTest can run it). Fills
'out' with the new text and 'results' with what happened to each commit. With 'onlyEntity' set
only that committed light is considered (D49: Enter in easy mode writes one light). Returns the number
of lights spliced, or -1 when the text cannot be scanned (then 'out' is undefined).
*/
static int LightMapSplice( const idStr& text, const idList<LightCommit>& commits, idStr& out, idList<LightSpliceResult>& results, const char* onlyEntity = NULL )
{
	results.SetNum( commits.Num() );
	for( int i = 0; i < results.Num(); i++ )
	{
		results[ i ].spliced = false;
		results[ i ].origin.Zero();
	}

	idList<LightMapBlock> blocks;
	if( !LightMapScan( text, blocks ) )
	{
		common->Warning( "LightEditor: the map file has unbalanced braces or a broken string; nothing written" );
		return -1;
	}

	if( blocks.Num() == 0 )
	{
		// nothing to locate, and a new light must not become the whole file
		for( int i = 0; i < commits.Num(); i++ )
		{
			if( commits[ i ].pending && ( onlyEntity == NULL || commits[ i ].entityName.Icmp( onlyEntity ) == 0 ) )
			{
				common->Warning( "LightEditor: the map file has no entities; '%s' not written", commits[ i ].name.c_str() );
			}
		}
		out = text;
		return 0;
	}

	const char* eol = ( text.Find( "\r\n" ) >= 0 ) ? "\r\n" : "\n";

	// step 3: one block per light, each block taken once
	idList<int> appended;
	for( int i = 0; i < commits.Num(); i++ )
	{
		const LightCommit& commit = commits[ i ];
		if( !commit.pending || ( onlyEntity != NULL && commit.entityName.Icmp( onlyEntity ) != 0 ) )
		{
			continue;
		}

		const int b = LightMapLocate( blocks, commit );
		if( b == -1 )
		{
			appended.Append( i );
			continue;
		}
		if( b < 0 )
		{
			continue;
		}
		if( blocks[ b ].nested )
		{
			common->Warning( "LightEditor: '%s' is a brush entity in the map file; not written", commit.name.c_str() );
			continue;
		}
		if( blocks[ b ].broken )
		{
			common->Warning( "LightEditor: '%s': its block has a key without a value on its line; not written", commit.name.c_str() );
			continue;
		}
		if( blocks[ b ].commit != -1 )
		{
			common->Warning( "LightEditor: '%s' and '%s' resolve to the same block; '%s' not written", commits[ blocks[ b ].commit ].name.c_str(), commit.name.c_str(), commit.name.c_str() );
			continue;
		}
		blocks[ b ].commit = i;
	}

	// step 4: everything outside the touched blocks is copied byte for byte; the output is
	// reserved once (the input plus the spliced keys)
	out.Fill( ' ', text.Length() + 4096 );
	out.Empty();

	idStr key;
	idList<bool> found;
	int copyFrom = 0;
	int spliced = 0;
	for( int b = 0; b < blocks.Num(); b++ )
	{
		const int i = blocks[ b ].commit;
		if( i < 0 )
		{
			continue;
		}

		out.Append( text.c_str() + copyFrom, blocks[ b ].start - copyFrom );
		LightMapRewriteBlock( text, blocks[ b ], commits[ i ].dict, eol, key, found, out );
		copyFrom = blocks[ b ].end;

		results[ i ].spliced = true;
		results[ i ].origin = LightMapWrittenOrigin( commits[ i ].dict, blocks[ b ].origin );
		spliced++;
	}
	out.Append( text.c_str() + copyFrom, text.Length() - copyFrom );

	for( int a = 0; a < appended.Num(); a++ )
	{
		const int i = appended[ a ];
		if( out.Length() > 0 && out[ out.Length() - 1 ] != '\n' )
		{
			out += eol;
		}
		LightMapAppendBlock( commits[ i ], eol, out );

		results[ i ].spliced = true;
		results[ i ].origin = LightMapWrittenOrigin( commits[ i ].dict, vec3_zero );
		spliced++;
	}

	return spliced;
}

/*
plan 3.5 flush: splices every pending commit into the .map file the game loaded, read fresh from the
fs_basepath copy idMapFile::Write would replace. Never goes through idMapFile::Write (U-5).
With 'onlyEntity' set only that committed light is written (D49: Enter in easy mode); NULL writes
every pending one (the panel's save). Returns how many of those pending lights it did not write (a
refused or unwritable file counts all of them, a skipped block one); 0 when everything was written
or nothing was pending. D48: only SaveChanges( true, ... ) calls it.
*/
int LightEditor::Flush( const char* onlyEntity )
{
	int pending = 0;
	for( int i = 0; i < committed.Num(); i++ )
	{
		if( committed[ i ].pending && ( onlyEntity == NULL || committed[ i ].entityName.Icmp( onlyEntity ) == 0 ) )
		{
			pending++;
		}
	}
	if( pending == 0 )
	{
		return 0;
	}

	// step 1
	idStrStatic< MAX_OSPATH > qpath = gameEdit->MapGetFileName();
	if( qpath.IsEmpty() )
	{
		lastWarning = "no map is loaded: the .map was not written";
		common->Warning( "LightEditor: %s", lastWarning.c_str() );
		return pending;
	}

	if( gameEdit->MapIsGLTF() )
	{
		lastWarning.Format( "%s is a glTF map: the light editor writes only .map files", qpath.c_str() );
		common->Warning( "LightEditor: %s", lastWarning.c_str() );
		return pending;
	}

	// idMapFile::Parse loads a .json beside the .map instead of it, so the .map is not the level
	idStrStatic< MAX_OSPATH > jsonPath = qpath;
	jsonPath.SetFileExtension( "json" );
	if( fileSystem->ReadFile( jsonPath, NULL ) >= 0 )
	{
		lastWarning.Format( "%s exists and is loaded instead of the .map: the .map was not written", jsonPath.c_str() );
		common->Warning( "LightEditor: %s", lastWarning.c_str() );
		return pending;
	}

	qpath.SetFileExtension( "map" );

	// EA-3 / EB-4: write only the file the level was read from. The search path tries fs_savepath
	// before fs_basepath (and .resources first under fs_resourceLoadPriority 1), so a shadowing copy
	// would take the edits away from the file the engine loads
	idStrStatic< MAX_OSPATH > basePath = fileSystem->RelativePathToOSPath( qpath, "fs_basepath" );
	idStrStatic< MAX_OSPATH > loadedPath;
	idFile* loaded = fileSystem->OpenFileRead( qpath );
	if( loaded != NULL )
	{
		loadedPath = loaded->GetFullPath();
		fileSystem->CloseFile( loaded );
	}
	basePath.BackSlashesToSlashes();
	loadedPath.BackSlashesToSlashes();
	if( loadedPath.Icmp( basePath ) != 0 )
	{
		lastWarning.Format( "the level is loaded from '%s', not from '%s': the .map was not written", loadedPath.c_str(), basePath.c_str() );
		common->Warning( "LightEditor: %s", lastWarning.c_str() );
		return pending;
	}

	// a <map>_extra_ents.map overrides keys again at load; written all the same
	idStrStatic< MAX_OSPATH > extraPath = gameEdit->MapGetFileName();
	extraPath += "_extra_ents.map";
	if( fileSystem->ReadFile( extraPath, NULL ) >= 0 )
	{
		common->Warning( "LightEditor: %s exists; keys it sets override the written lights at load", extraPath.c_str() );
	}

	idFile* src = fileSystem->OpenExplicitFileRead( fileSystem->RelativePathToOSPath( qpath, "fs_basepath" ) );
	if( src == NULL )
	{
		lastWarning.Format( "%s not found under fs_basepath: the .map was not written", qpath.c_str() );
		common->Warning( "LightEditor: %s", lastWarning.c_str() );
		return pending;
	}

	idStr text;
	const int length = src->Length();
	text.Fill( ' ', length );
	const int read = ( length > 0 ) ? src->Read( &text[ 0 ], length ) : 0;
	fileSystem->CloseFile( src );
	if( read != length )
	{
		lastWarning.Format( "could not read %s: the .map was not written", qpath.c_str() );
		common->Warning( "LightEditor: %s", lastWarning.c_str() );
		return pending;
	}

	// steps 2-4
	idStr out;
	idList<LightSpliceResult> results;
	const int spliced = LightMapSplice( text, committed, out, results, onlyEntity );
	if( spliced < 0 )
	{
		lastWarning.Format( "%s could not be scanned: the .map was not written", qpath.c_str() );
		return pending;
	}

	// step 5: an unchanged file is not touched. A changed one goes to a temporary file that replaces
	// the map only once every byte is in it (RenameFile replaces an existing target: MoveFileEx with
	// MOVEFILE_REPLACE_EXISTING, rename() elsewhere), so a short write never leaves a cut .map; on
	// failure every light stays pending, so the caller can retry or roll its entry back
	if( out.Length() != text.Length() || memcmp( out.c_str(), text.c_str(), out.Length() ) != 0 )
	{
		idStrStatic< MAX_OSPATH > tmpPath = qpath;
		tmpPath += ".tmp";

		// EA-8: flushed to disk before the rename (idFile::Flush returns nothing, so only the write
		// count can fail the step)
		bool written = false;
		idFile* dst = fileSystem->OpenFileWrite( tmpPath, "fs_basepath" );
		if( dst != NULL )
		{
			written = ( dst->Write( out.c_str(), out.Length() ) == out.Length() );
			dst->Flush();
			fileSystem->CloseFile( dst );
		}

		if( !written || !fileSystem->RenameFile( tmpPath, qpath, "fs_basepath" ) )
		{
			fileSystem->RemoveFile( tmpPath );
			lastWarning.Format( "could not write %s: the .map was left as it was", qpath.c_str() );
			common->Warning( "LightEditor: %s", lastWarning.c_str() );
			return pending;
		}
	}

	// step 6: a light that is now in the file is located there by name and origin from now on
	int skipped = 0;
	for( int i = 0; i < committed.Num(); i++ )
	{
		LightCommit& commit = committed[ i ];
		if( !commit.pending || ( onlyEntity != NULL && commit.entityName.Icmp( onlyEntity ) != 0 ) )
		{
			continue;
		}
		if( results[ i ].spliced )
		{
			commit.isNew = false;
			commit.onDiskOrigin = results[ i ].origin;
		}
		else
		{
			skipped++;
		}
		commit.pending = false;
	}

	common->Printf( "LightEditor: %d light(s) written to %s\n", spliced, qpath.c_str() );
	if( skipped > 0 )
	{
		lastWarning.Format( "%d light(s) not written to %s, see the console", skipped, qpath.c_str() );
	}
	else
	{
		lastWarning.Clear();
	}

	return skipped;
}

// a kinda ugly hack to get a float* (as used by imgui) from idVec3
static float* vecToArr( idVec3& v )
{
	return &v.x;
}

// V-B20: the player's camera matrices for the gizmo and the reticle. The view must be cleared
// (the renderer's own way) and needs a viewport, which the projection divides by.
bool LightEditor::SetupToolView( viewDef_t& vd ) const
{
	memset( &vd, 0, sizeof( vd ) );

	// PlayerGetRenderView dereferences the local player unchecked
	if( !gameEdit->PlayerIsValid() || !gameEdit->PlayerGetRenderView( vd.renderView ) )
	{
		return false;
	}

	vd.viewport.x1 = 0;
	vd.viewport.y1 = 0;
	vd.viewport.x2 = renderSystem->GetWidth() - 1;
	vd.viewport.y2 = renderSystem->GetHeight() - 1;

	// absolute ( a zero origin ): the gizmo and the reticle pair this view with absolute points
	R_SetupViewMatrix( &vd, idVec3d() );
	R_SetupProjectionMatrix( &vd, false );

	return true;
}


void LightEditor::Draw()
{
	// S1: easy mode replaces the panel with the overlay and the reticle, which stay off the Esc menu
	// (ImGui still renders under the shell, while RunFrame does not run)
	if( EasyModeActive() )
	{
		if( game == NULL || !game->Shell_IsActive() )
		{
			DrawEasyOverlay();
		}
		return;
	}

	if( editorOpen )
	{
		// plan 3.6: advanced mode keeps one line of easy-mode state, naming the remembered state
		const char* remembered = "no light";
		if( lightEntity != NULL )
		{
			if( easyState == EASY_CHECKPOINT )
			{
				remembered = "CHECKPOINT paused: advanced";
			}
			else
			{
				// D46: a point light holds instead of following the aim
				remembered = ( cur.lightType == LIGHT_POINT ) ? "HOLD paused: advanced" : "AUTO paused: advanced";
			}
		}
		ImGui::SetNextWindowPos( ImVec2( 10.0f, ImGui::GetIO().DisplaySize.y - 10.0f ), ImGuiCond_Always, ImVec2( 0.0f, 1.0f ) );
		ImGui::Begin( "##lightEditorState", NULL, ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings );
		ImGui::Text( "%s%s", remembered, le_showLegend.GetBool() ? "" : "  H: legend" );
		ImGui::End();
	}

	bool changes = false;
	bool showTool = isShown;
	bool isOpen;

	if( ImGui::Begin( title, &isOpen ) )
	{
		// RB: handle arrow key inputs like in TrenchBroom
		ImGuiIO& io = ImGui::GetIO();

		// FIXME, escape does not work
		if( io.KeysDown[K_ESCAPE] )
		{
			CancelChanges();
			showTool = false;
		}

		// TODO use view direction like just global values
		if( io.KeyCtrl )
		{
			if( io.KeysDown[K_S] && shortcutSaveMapEnabled )
			{
				SaveChanges( true, false );
				shortcutSaveMapEnabled = false;
			}
			else if( io.KeysDown[K_D] && shortcutDuplicateLightEnabled )
			{
				DuplicateLight();
				shortcutDuplicateLightEnabled = false;
			}
		}
		else if( io.KeyAlt )
		{
			if( io.KeysDown[K_R] )
			{
				// reset rotation like in Blender
				cur.angles.Zero();
				changes = true;
			}
			else if( io.KeysDown[K_UPARROW] )
			{
				cur.origin.z += 1;
				changes = true;
			}
			else if( io.KeysDown[K_DOWNARROW] )
			{
				cur.origin.z -= 1;
				changes = true;
			}
		}
		else if( io.KeysDown[K_RIGHTARROW] )
		{
			cur.origin.x += 1;
			changes = true;
		}
		else if( io.KeysDown[K_LEFTARROW] )
		{
			cur.origin.x -= 1;
			changes = true;
		}
		else if( io.KeysDown[K_UPARROW] )
		{
			cur.origin.y += 1;
			changes = true;
		}
		else if( io.KeysDown[K_DOWNARROW] )
		{
			cur.origin.y -= 1;
			changes = true;
		}

		// reenable commands if keys were released
		if( ( !io.KeyCtrl || !io.KeysDown[K_S] ) && !shortcutSaveMapEnabled )
		{
			shortcutSaveMapEnabled = true;
		}

		if( ( !io.KeyCtrl || !io.KeysDown[K_D] ) && !shortcutDuplicateLightEnabled )
		{
			shortcutDuplicateLightEnabled = true;
		}

		// UI-3: a refused commit or save says so here too, not only in easy mode
		if( !lastWarning.IsEmpty() )
		{
			ImGui::TextColored( ImVec4( 1.0f, 0.75f, 0.2f, 1.0f ), "%s", lastWarning.c_str() );
		}

		// UI-4 / D6: with no light the controls do nothing, so they look disabled and say how to pick one
		const bool noLight = ( lightEntity == NULL );
		if( noLight )
		{
			ImGui::TextUnformatted( "No light selected. Pick one: RMB fly + LMB, or F for easy mode." );
		}
		ImGui::BeginDisabled( noLight );

		if( !entityName.IsEmpty() )
		{
			ImGui::SeparatorText( entityName.c_str() );
		}

		ImGui::SeparatorText( "Light Volume" );

		ImGui::Spacing();

		int lightSelectionRadioBtn = cur.lightType;

		changes |= ImGui::RadioButton( "Point Light", &lightSelectionRadioBtn, 0 );
		ImGui::SameLine();
		changes |= ImGui::RadioButton( "Spot Light", &lightSelectionRadioBtn, 1 );
		ImGui::SameLine();
		changes |= ImGui::RadioButton( "Sun Light", &lightSelectionRadioBtn, 2 );

		ImGui::Indent();

		ImGui::Spacing();

		bool rangeEdited = false;
		const float panelRangeBefore = EM_Range( cur.lightRadius );

		if( lightSelectionRadioBtn == LIGHT_POINT || lightSelectionRadioBtn == LIGHT_SUN )
		{
			if( lightSelectionRadioBtn == LIGHT_POINT && lightSelectionRadioBtn != cur.lightType )
			{
				cur.DefaultPoint();
				changes = true;
			}
			else if( lightSelectionRadioBtn == LIGHT_SUN && lightSelectionRadioBtn != cur.lightType )
			{
				cur.DefaultSun();
				changes = true;
			}

			if( cur.emitter.enabled && lightSelectionRadioBtn == LIGHT_POINT )
			{
				// G0 9: an emitter omni shows one Range field; its three radii are behind Equilateral,
				// and ticking it makes the box a sphere of the range. Once unticked it stays so for the
				// session, so a re-read light with equal radii can still be given unequal ones
				EquilateralForSession( cur, equilateralUnticked );
				if( ImGui::Checkbox( "Equilateral", &cur.equalRadius ) )
				{
					equilateralUnticked = !cur.equalRadius;
					if( cur.equalRadius )
					{
						const float range = EM_Range( cur.lightRadius );
						cur.lightRadius.Set( range, range, range );
						rangeEdited = true;
					}
					changes = true;
				}
				if( !cur.equalRadius )
				{
					ImGui::PushItemWidth( -1.0f );
					ImGui::Text( "Radii:" );
					ImGui::Indent();
					rangeEdited |= ImGui::DragVec3( "##radXYZ", cur.lightRadius, DragSpeedFor( EM_Range( cur.lightRadius ) ) );
					ImGui::Unindent();
					ImGui::PopItemWidth();
				}
				changes |= DrawEmitterExtentRows( cur, rangeEdited );
			}

			ImGui::PushItemWidth( -1.0f ); // align end of Drag* with right window border

			if( !cur.emitter.enabled || lightSelectionRadioBtn != LIGHT_POINT )
			{
				changes |= ImGui::Checkbox( "Equilateral Radius", &cur.equalRadius );
				ImGui::Text( "Radius:" );
				ImGui::Indent();
				if( cur.equalRadius )
				{
					if( ImGui::DragFloat( "##radEquil", &cur.lightRadius.x, DragSpeedFor( cur.lightRadius.x ), 0.0f, 10000.0f, "%.1f" ) )
					{
						cur.lightRadius.z = cur.lightRadius.y = cur.lightRadius.x;
						rangeEdited = true;
					}
				}
				else
				{
					rangeEdited |= ImGui::DragVec3( "##radXYZ", cur.lightRadius, DragSpeedFor( EM_Range( cur.lightRadius ) ) );
				}
				ImGui::Unindent();
			}

			ImGui::Spacing();

			//changes |= ImGui::Checkbox( "Parallel", &cur.isParallel );

			//ImGui::Spacing();

			// on an emitter omni light_center is where the bulb sits relative to the origin (the lamp),
			// either way along each axis; elsewhere it is today's light centre
			const bool bulbOffset = ( cur.emitter.enabled && lightSelectionRadioBtn == LIGHT_POINT );
			changes |= ImGui::Checkbox( bulbOffset ? "Bulb offset" : "Center", &cur.hasCenter );
			if( cur.hasCenter )
			{
				ImGui::Indent();
				if( bulbOffset )
				{
					changes |= ImGui::DragVec3( "##bulbXYZ", cur.lightCenter, 1.0f, 0.0f, 0.0f, "%.1f" );
				}
				else
				{
					changes |= ImGui::DragVec3( "##centerXYZ", cur.lightCenter, 1.0f, 0.0f, 10000.0f, "%.1f" );
				}
				ImGui::Unindent();
			}

			// a texture point light with a centre had its lit box on the origin; as an emitter the box is
			// centred on the bulb, so the light lights a different place than it did
			if( bulbOffset && cur.hasCenter && !original.emitter.enabled && original.lightType == LIGHT_POINT && original.hasCenter
					&& original.lightCenter != vec3_zero )
			{
				ImGui::TextDisabled( "As an emitter the lit box is centred on the bulb: it moved by the offset." );
			}
			ImGui::PopItemWidth(); // back to default alignment on right side
		}
		else if( lightSelectionRadioBtn == LIGHT_SPOT )
		{
			if( cur.lightType != lightSelectionRadioBtn )
			{
				cur.DefaultProjected();
				changes = true;
			}

			if( cur.emitter.enabled )
			{
				// an emitter spot has no frustum: its direction is the rotation, its cone is on the Emitter tab
				changes |= DrawEmitterExtentRows( cur, rangeEdited );
			}
			else
			{
				changes |= ImGui::DragVec3( "Target", cur.lightTarget, 1.0f, 0.0f, 0.0f, "%.1f" );
				changes |= ImGui::DragVec3( "Right", cur.lightRight, 1.0f, 0.0f, 0.0f, "%.1f" );
				changes |= ImGui::DragVec3( "Up", cur.lightUp, 1.0f, 0.0f, 0.0f, "%.1f" );

				ImGui::Spacing();

				changes |= ImGui::Checkbox( "Explicit start/end points", &cur.explicitStartEnd );

				ImGui::Spacing();
				if( cur.explicitStartEnd )
				{
					changes |= ImGui::DragVec3( "Start", cur.lightStart, 1.0f, 0.0f, 0.0f, "%.1f" );
					changes |= ImGui::DragVec3( "End", cur.lightEnd, 1.0f, 0.0f, 0.0f, "%.1f" );
				}
			}
		}

		// a radius or range drag sets an emitter's range; on a texture light this is only the change
		if( rangeEdited )
		{
			EmitterRangeEdited( cur, panelRangeBefore );
			changes = true;
		}

		// the one type choice; the emitter's shape follows it
		cur.SetLightType( ELightType( lightSelectionRadioBtn ) );

		ImGui::Unindent();

		ImGui::SeparatorText( "Transform" );

		if( io.KeysDown[K_G] )
		{
			mCurrentGizmoOperation = ImGuizmo::TRANSLATE;
		}

		if( io.KeysDown[K_R] )
		{
			mCurrentGizmoOperation = ImGuizmo::ROTATE;
		}

		//if( ImGui::IsKeyPressed( ImGuiKey_S ) )
		if( io.KeysDown[K_S] )
		{
			mCurrentGizmoOperation = ImGuizmo::SCALE;
		}

		if( mCurrentGizmoOperation != ImGuizmo::SCALE )
		{
			if( ImGui::RadioButton( "Local", mCurrentGizmoMode == ImGuizmo::LOCAL ) )
			{
				mCurrentGizmoMode = ImGuizmo::LOCAL;
			}
			ImGui::SameLine();
			if( ImGui::RadioButton( "World", mCurrentGizmoMode == ImGuizmo::WORLD ) )
			{
				mCurrentGizmoMode = ImGuizmo::WORLD;
			}
		}
		else
		{
			mCurrentGizmoMode = ImGuizmo::LOCAL;
		}

		if( ImGui::RadioButton( "Translate", mCurrentGizmoOperation == ImGuizmo::TRANSLATE ) )
		{
			mCurrentGizmoOperation = ImGuizmo::TRANSLATE;
		}
		ImGui::SameLine();
		if( ImGui::RadioButton( "Rotate", mCurrentGizmoOperation == ImGuizmo::ROTATE ) )
		{
			mCurrentGizmoOperation = ImGuizmo::ROTATE;
		}
		ImGui::SameLine();
		if( ImGui::RadioButton( "Scale", mCurrentGizmoOperation == ImGuizmo::SCALE ) )
		{
			mCurrentGizmoOperation = ImGuizmo::SCALE;
		}
		//if( ImGui::RadioButton( "Universal", mCurrentGizmoOperation == ImGuizmo::UNIVERSAL ) )
		//{
		//	mCurrentGizmoOperation = ImGuizmo::UNIVERSAL;
		//}

		changes |= ImGui::DragVec3( "Origin", cur.origin, 1.0f, 0.0f, 0.0f, "%.1f" );
		changes |= ImGui::InputFloat3( "Angles", cur.angles.ToFloatPtr() );
		//changes |= ImGui::DragVec3( "Angles", cur.origin, 1.0f, 0.0f, 0.0f, "%.1f" );

		ImGui::SeparatorText( "Snapping" );

		ImGui::Checkbox( "Use Snapping", &useSnap );
		//ImGui::SameLine();

		if( useSnap )
		{
			switch( mCurrentGizmoOperation )
			{
				case ImGuizmo::TRANSLATE:
					ImGui::InputFloat3( "Grid Snap", &gridSnap[0] );
					break;
				case ImGuizmo::ROTATE:
					ImGui::InputFloat( "Angle Snap", &angleSnap );
					break;
				case ImGuizmo::SCALE:
					ImGui::InputFloat( "Scale Snap", &scaleSnap );
					break;
			}
		}

#if 0
		ImGui::Checkbox( "Bound Sizing", &boundSizing );
		if( boundSizing )
		{
			ImGui::PushID( 3 );
			ImGui::Checkbox( "##BoundSizing", &boundSizingSnap );
			ImGui::SameLine();
			ImGui::InputFloat3( "Snap", boundsSnap );
			ImGui::PopID();
		}
#endif

		ImGui::SeparatorText( "Color" );

		// the colour and its intensity (the largest channel of _color, as T and auto range read it), so a
		// light brighter than white can be set and recoloured here without being clipped to 1
		float intensity = PanelColorSplit( cur.color, panelHue );
		idVec3 hue = panelHue;
		if( ImGui::ColorEdit3( "Color", vecToArr( hue ) ) )
		{
			panelHue = hue;
			PanelSetColor( cur, hue, intensity );
			changes = true;
		}
		// AlwaysClamp: a typed intensity stays 0 to 8 (G0 9, RV-C-ET-12)
		if( ImGui::DragFloat( "Intensity", &intensity, 0.01f, 0.0f, 8.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp ) )
		{
			// a reach edit: under auto range an emitter's range follows it
			PanelSetColor( cur, panelHue, intensity );
			changes = true;
		}

		ImGui::Spacing();

		// an emitter and a texture light are two tabs, and the tab is the switch. The shown tab follows
		// cur.emitter.enabled: a new light, Cancel or an undo asks for its tab with SetSelected, which
		// ImGui applies a frame later, so nothing flips while a tab is asked for; a tab that opens unasked
		// is the user's click. A new ImGui context (resize) opens on the first tab, so ask on appearing
		const bool tabRequest = ( tabShowsEmitter != cur.emitter.enabled ) || ImGui::IsWindowAppearing();
		if( ImGui::BeginTabBar( "LightKind" ) )
		{
			if( ImGui::BeginTabItem( "Emitter", NULL, ( tabRequest && cur.emitter.enabled ) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None ) )
			{
				if( !tabRequest && !cur.emitter.enabled && lightEntity != NULL )
				{
					// the emit keys a Texture tab left on the light come back; a light without them is seeded
					SwitchToEmitter( cur, *gameEdit->EntityGetSpawnArgs( lightEntity ) );
					changes = true;
				}
				tabShowsEmitter = true;

				changes |= DrawEmitterControls( cur );

				ImGui::EndTabItem();
			}

			if( ImGui::BeginTabItem( "Texture", NULL, ( tabRequest && !cur.emitter.enabled ) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None ) )
			{
				if( !tabRequest && cur.emitter.enabled )
				{
					// ToDict writes emitter "0" and leaves the emit_* keys on the light
					SwitchToTexture( cur );
					changes = true;
				}
				tabShowsEmitter = false;

				if( ImGui::Combo( "Texture", &currentTextureIndex, TextureItemsGetter, this, textureNames.Num() + 1 ) )
				{
					changes = true;

					// -1 because 0 is "<No Texture>"
					cur.strTexture = ( currentTextureIndex > 0 ) ? textureNames[currentTextureIndex - 1] : "";
					LoadCurrentTexture();
				}

				if( currentTextureMaterial != nullptr && currentTexture != nullptr )
				{
					ImVec2 size( currentTexture->GetUploadWidth(), currentTexture->GetUploadHeight() );

					ImGui::Image( ( void* )currentTextureMaterial, size, ImVec2( 0, 0 ), ImVec2( 1, 1 ),
								  ImColor( 255, 255, 255, 255 ), ImColor( 255, 255, 255, 128 ) );
				}

				ImGui::SeparatorText( "Flicker Style" );

				if( ImGui::Combo( "Style", &currentStyleIndex, StyleItemsGetter, this, styleNames.Num() + 1 ) )
				{
					changes = true;

					// -1 because 0 is "<No Lightstyle>"
					cur.lightStyle = ( currentStyleIndex > 0 ) ? currentStyleIndex - 1 : -1;
				}

				ImGui::EndTabItem();
			}

			ImGui::EndTabBar();
		}

		ImGui::SeparatorText( "Misc Options" );

		changes |= ImGui::Checkbox( "Cast Shadows", &cur.castShadows );
		changes |= ImGui::Checkbox( "Skip Specular", &cur.skipSpecular );

		// TODO: allow multiple lights selected at the same time + "apply different" button?
		//       then only the changed attribute (e.g. color) would be set to all lights,
		//       but they'd keep their other individual properties (eg radius)

		ImGui::Spacing();

		// D6 / D30: Apply and Cancel deselect through the one routine, and the panel then hides and
		// the game takes the mouse back, as before easy mode; the editor stays open for the next pick,
		// F, or the editLights command. A refused commit keeps the light and the panel
		if( ImGui::Button( "Apply" ) )
		{
			if( Deselect( true ) )
			{
				showTool = false;
			}
		}
		else if( ImGui::SameLine(), ImGui::Button( "Cancel" ) )
		{
			Deselect( false );
			showTool = false;
		}

		ImGui::EndDisabled();

		viewDef_t viewDef;
		const bool haveView = SetupToolView( viewDef );
		if( haveView )
		{
			ImGui::Separator();

			ImGui::Text( "X: %f Y: %f", io.MousePos.x, io.MousePos.y );
			if( ImGuizmo::IsUsing() )
			{
				ImGui::Text( "Using gizmo" );
			}
			else
			{
				ImGui::Text( ImGuizmo::IsOver() ? "Over gizmo" : "" );
				ImGui::SameLine();
				ImGui::Text( ImGuizmo::IsOver( ImGuizmo::TRANSLATE ) ? "Over translate gizmo" : "" );
				ImGui::SameLine();
				ImGui::Text( ImGuizmo::IsOver( ImGuizmo::ROTATE ) ? "Over rotate gizmo" : "" );
				ImGui::SameLine();
				ImGui::Text( ImGuizmo::IsOver( ImGuizmo::SCALE ) ? "Over scale gizmo" : "" );
			}
		}

		static bool use_work_area = true;
		static ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
										| ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;// | ImGuiWindowFlags_MenuBar;

		// We demonstrate using the full viewport area or the work area (without menu-bars, task-bars etc.)
		// Based on your use case you may want one or the other.
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos( use_work_area ? viewport->WorkPos : viewport->Pos );
		ImGui::SetNextWindowSize( use_work_area ? viewport->WorkSize : viewport->Size );

		if( ImGui::Begin( "Example: Fullscreen window", &showTool, flags ) )
		{
			if( ImGui::BeginMainMenuBar() )
			{
				if( ImGui::BeginMenu( "File" ) )
				{
					//ShowExampleMenuFile();
					if( ImGui::MenuItem( "Save Map", "Ctrl+S" ) )
					{
						SaveChanges( true, false );
					}
					ImGui::EndMenu();
				}
				if( ImGui::BeginMenu( "Edit" ) )
				{
					//if( ImGui::MenuItem( "Undo", "CTRL+Z" ) ) {}
					//if( ImGui::MenuItem( "Redo", "CTRL+Y", false, false ) ) {} // Disabled item

					//ImGui::Separator();

					//if( ImGui::MenuItem( "Cut", "CTRL+X" ) ) {}
					//if( ImGui::MenuItem( "Copy", "CTRL+C" ) ) {}
					//if( ImGui::MenuItem( "Paste", "CTRL+V" ) ) {}

					if( ImGui::MenuItem( "Duplicate", "CTRL+D" ) )
					{
						DuplicateLight();
					}

					//if( ImGui::MenuItem( "Delete", "Backspace" ) )
					//{
					// TODO
					//	goto exitLightEditor;
					//}
					ImGui::EndMenu();
				}
				ImGui::EndMainMenuBar();
			}

			// backup state before moving the light
			if( !ImGuizmo::IsUsing() )
			{
				curNotMoving = cur;
			}

			//
			// GIZMO
			//

			//ImGuiIO& io = ImGui::GetIO();
			ImGuizmo::SetRect( 0, 0, io.DisplaySize.x, io.DisplaySize.y );
			ImGuizmo::SetOrthographic( false );
			ImGuizmo::SetDrawlist();

			ImGuizmo::SetID( 0 );


			// UI-4: no gizmo and no cube at the world origin without a light
			if( haveView && lightEntity != NULL )
			{
				float* cameraView = viewDef.worldSpace.modelViewMatrix;
				float* cameraProjection = viewDef.unjitteredProjectionMatrix;
#if ID_REVERSED_Z
				// ImGuizmo unprojects ndc z = 0 for one end of its mouse ray ( ComputeCameraRay ), which
				// is w = 0 with the exactly infinite reversed projection. The gizmo gets a reversed
				// projection with a finite far plane instead; only the z row differs, so it draws where
				// it did, and its ray runs from the far plane to the near plane
				float gizmoProjection[16];
				memcpy( gizmoProjection, viewDef.unjitteredProjectionMatrix, sizeof( gizmoProjection ) );
				const float gizmoNear = gizmoProjection[3 * 4 + 2];
				const float gizmoFar = 65536.0f;
				gizmoProjection[2 * 4 + 2] = gizmoNear / ( gizmoFar - gizmoNear );
				gizmoProjection[3 * 4 + 2] = gizmoFar * gizmoNear / ( gizmoFar - gizmoNear );
				cameraProjection = gizmoProjection;
#endif

				idMat3 rotateMatrix = cur.angles.ToMat3();
				idMat3 scaleMatrix = mat3_identity;
				scaleMatrix[0][0] = 16;
				scaleMatrix[1][1] = 16;
				scaleMatrix[2][2] = 16;

				// an emitter omni's gizmo sits on its bulb, which may be off the origin
				const idVec3 gizmoOrigin = LightGizmoOrigin( cur );
				idMat4 objectMatrix( scaleMatrix * rotateMatrix,  gizmoOrigin );
				ImGuizmo::DrawCubes( cameraView, cameraProjection, objectMatrix.Transpose().ToFloatPtr(), 1 );

				scaleMatrix[0][0] = 1;
				scaleMatrix[1][1] = 1;
				scaleMatrix[2][2] = 1;

				idMat4 gizmoMatrix( scaleMatrix * rotateMatrix,  gizmoOrigin );
				idMat4 manipMatrix = gizmoMatrix.Transpose();

				const float* snap = NULL;
				if( useSnap )
				{
					switch( mCurrentGizmoOperation )
					{
						case ImGuizmo::TRANSLATE:
							snap = &gridSnap[0];
							break;
						case ImGuizmo::ROTATE:
							snap = &angleSnap;
							break;
						case ImGuizmo::SCALE:
							snap = &scaleSnap;
							break;
					}
				}

				ImGuizmo::Manipulate( cameraView, cameraProjection, mCurrentGizmoOperation, mCurrentGizmoMode, manipMatrix.ToFloatPtr(), NULL, useSnap ? snap : NULL, boundSizing ? bounds : NULL, boundSizingSnap ? boundsSnap : NULL );

				if( ImGuizmo::IsUsing() )
				{
					//if( mCurrentGizmoOperation == ImGuizmo::TRANSLATE )
					{
						gizmoMatrix = manipMatrix.Transpose();
						changes = true;
					}

					if( ( mCurrentGizmoOperation & ImGuizmo::SCALE ) == 0 )
					{
						idMat3 axis = gizmoMatrix.ToMat3();
						cur.angles = axis.ToAngles();

						changes = true;
					}

					// with the new angles: an emitter omni turns about its bulb
					cur.origin = LightOriginFromGizmo( cur, gizmoMatrix.GetTranslation() );

					if( mCurrentGizmoOperation == ImGuizmo::SCALE )
					{
						// Use DecomposeMatrixToComponents just for the scaling
						float matrixTranslation[3], matrixRotation[3], matrixScale[3];
						ImGuizmo::DecomposeMatrixToComponents( &manipMatrix[0][0], matrixTranslation, matrixRotation, matrixScale );

						cur.scale.x = matrixScale[0];
						cur.scale.y = matrixScale[1];
						cur.scale.z = matrixScale[2];

						if( matrixScale[0] != 1.0f || matrixScale[1] != 1.0f || matrixScale[2] != 1.0f )
						{
							if( cur.lightType == LIGHT_SPOT && cur.emitter.enabled )
							{
								// local x is the beam: it scales the range; y and z scale the two cone tangents
								SetEmitterConeFromTan( cur.emitter, curNotMoving.emitter.tanHalfH * matrixScale[1], curNotMoving.emitter.tanHalfV * matrixScale[2] );
								cur.emitter.size = curNotMoving.emitter.size;
								SetEmitterRange( cur, EM_Range( curNotMoving.lightRadius ) * matrixScale[0] );
								EmitterRangeEdited( cur, EM_Range( curNotMoving.lightRadius ) );
							}
							else if( cur.lightType == LIGHT_SPOT )
							{
								cur.lightRight = curNotMoving.lightRight * matrixScale[0];
								cur.lightUp = curNotMoving.lightUp * matrixScale[1];
								cur.lightTarget = curNotMoving.lightTarget * matrixScale[2];
							}
							else //if( cur.lightType == LIGHT_POINT )
							{
								cur.lightRadius.x = curNotMoving.lightRadius.x * matrixScale[0];
								cur.lightRadius.y = curNotMoving.lightRadius.y * matrixScale[1];
								cur.lightRadius.z = curNotMoving.lightRadius.z * matrixScale[2];

								if( matrixScale[0] != matrixScale[1] || matrixScale[1] != matrixScale[2] )
								{
									cur.equalRadius = false;
								}

								// an emitter's radius is its range
								EmitterRangeEdited( cur, EM_Range( curNotMoving.lightRadius ) );
							}

							changes = true;
						}
					}
				}
			}
		}
		ImGui::End();
	}
	ImGui::End();

	if( changes )
	{
		TempApplyChanges();
	}

//exitLightEditor:

	if( isShown && !showTool )
	{
		isShown = showTool;
		SetReleaseToolMouse( false );
	}
}

// ########### easy mode geometry (plan 3.3, frozen) #############
//
// Free functions on LightInfo and plain inputs, so le_selfTest can check them with no map loaded.
// idMat3 rows are the light's basis vectors: local -> world is axis * v, world -> local is
// axis.Transpose() * v (v * axis is the SAME product as axis * v). idRotation turns the other way
// from the textbook: rotation( n, a ) * v = cos( a ) v - sin( a ) ( n x v ) for v perpendicular to n.

static const idVec3 worldUp( 0.0f, 0.0f, 1.0f );

// EB-10: the range of every Q and E factor
static const float MODAL_SCALE_MIN = 0.01f;
static const float MODAL_SCALE_MAX = 100.0f;

// the light's horizontal vector when the aim has none: the previous frame's, else 'first', both
// made perpendicular to n again (V-B F8); any perpendicular as the last resort
static idVec3 AutoFallbackVector( const LightAutoBase& base, const idVec3& first, const idVec3& n )
{
	idVec3 v = base.haveFallback ? base.fallback : first;
	v -= n * ( v * n );
	if( v.LengthSqr() < 1e-6f )
	{
		idVec3 up;
		n.OrthogonalBasis( v, up );
	}
	v.Normalize();
	return v;
}

// plan 3.3 autoBase, at every E1 entry
static void AutoCaptureBase( const LightInfo& light, LightAutoBase& base )
{
	// the stored-to-world frame is taken once for this entry, by RunFrame on its first emitter-spot
	// AUTO frame (this function has no entity); until then, and in le_selfTest, it is the identity
	base.storedFrame.Identity();
	base.haveStoredFrame = false;
	base.farHalfWidth = 0.0f;
	base.farHalfHeight = 0.0f;
	base.handedness = -1.0f;
	base.zNear = 8.0f;
	base.centerLength = Max( light.lightCenter.Length(), 1.0f );
	base.emitterStart = light.emitter.start;
	base.haveFallback = false;
	base.fallback.Zero();

	const float targetLength = light.lightTarget.Length();
	if( light.lightType != LIGHT_SPOT || targetLength < 1e-3f )
	{
		return;
	}

	const idVec3 n = light.lightTarget / targetLength;

	// V-B F7: the far plane sits at max( end . n, 16 ) with explicit start/end, else at the target
	const float zFar = light.explicitStartEnd ? Max( light.lightEnd * n, 16.0f ) : targetLength;
	base.farHalfWidth = light.lightRight.Length() * zFar / targetLength;
	base.farHalfHeight = light.lightUp.Length() * zFar / targetLength;

	// V-B7: 0 counts as -1
	base.handedness = ( light.lightRight.Cross( light.lightUp ) * light.lightTarget > 0.0f ) ? 1.0f : -1.0f;

	if( light.explicitStartEnd )
	{
		base.zNear = light.lightStart * n;
	}
}

// S3 spot: the far plane lands on the reticle with its original size; the apex never moves.
// Returns false when the frame is skipped (aim closer than 16).
static bool AutoAimSpot( LightInfo& light, LightAutoBase& base, const idVec3& aimPoint )
{
	const idVec3 d = aimPoint - light.origin;
	const float length = d.Length();
	if( length < 16.0f )
	{
		return false;
	}

	const idMat3 axis = light.angles.ToMat3();
	const idVec3 t = axis.Transpose() * d;
	idVec3 n = t;
	n.Normalize();

	// r is the viewer's right; straight up or down it has none
	idVec3 r = n.Cross( axis.Transpose() * worldUp );
	if( r.Length() < 0.01f )
	{
		r = AutoFallbackVector( base, axis.Transpose() * idVec3( 1.0f, 0.0f, 0.0f ), n );
	}
	r.Normalize();
	base.fallback = r;
	base.haveFallback = true;

	idVec3 u = r.Cross( n );
	u.Normalize();
	const float side = ( r.Cross( u ) * n > 0.0f ) ? 1.0f : -1.0f;
	if( side != base.handedness )
	{
		u = -u;
	}

	light.lightTarget = t;
	light.lightRight = r * base.farHalfWidth;
	light.lightUp = u * base.farHalfHeight;
	if( light.explicitStartEnd )
	{
		light.lightStart = n * Min( base.zNear, 0.5f * length );
		light.lightEnd = light.lightTarget;
	}
	return true;
}

// D10 sun: only the direction follows, in world space (the renderer does not rotate a parallel
// light's lightCenter); the box stays
static void AutoAimSun( LightInfo& light, const LightAutoBase& base, const idVec3& viewForward )
{
	light.lightCenter = -viewForward * base.centerLength;
	light.hasCenter = true;		// otherwise ToDict deletes the key
}

/*
S3 / D46: what AUTO does to the selected light for the aim point (and the view direction, NULL when
unknown). A spot re-aims its far plane, a sun takes the view direction, and a point light holds where
it is (D46 withdrew D25/D37: it no longer follows the player). Returns true when it wrote the light.
*/
static bool AutoAimLight( LightInfo& light, LightAutoBase& base, const idVec3& aimPoint, const idVec3* viewForward )
{
	switch( light.lightType )
	{
		case LIGHT_SPOT:
			return AutoAimSpot( light, base, aimPoint );
		case LIGHT_SUN:
			if( viewForward != NULL )
			{
				AutoAimSun( light, base, *viewForward );
				return true;
			}
			return false;
		default:
			return false;
	}
}

/*
AUTO for an emitter spot: the beam (axis[0]) through the aim point, computed in the world and stored in
the dict's space through the frame taken at entry (base.storedFrame). Under auto range the range is
max( 24, distance / 0.6 ) every frame, both ways (G0 9, choice 2), so it follows the aim and never
ratchets; otherwise the range, size and start stay. Returns false when the frame is skipped (aim
closer than 16).
*/
static bool AutoAimEmitterSpot( LightInfo& light, LightAutoBase& base, const idVec3& aimPoint )
{
	const LightStoredFrame& frame = base.storedFrame;
	const idVec3 d = aimPoint - frame.ToWorldPoint( light.origin );
	const float distance = d.Length();
	if( distance < 16.0f )
	{
		return false;
	}

	idMat3 worldAxis;
	worldAxis[0] = d / distance;
	worldAxis[1] = worldUp.Cross( worldAxis[0] );
	if( worldAxis[1].Length() < 0.01f )
	{
		// straight up or down: the previous frame's left, else the light's own
		const idMat3 current = light.angles.ToMat3() * frame.axis;
		worldAxis[1] = AutoFallbackVector( base, current[1], worldAxis[0] );
	}
	worldAxis[1].Normalize();
	worldAxis[2] = worldAxis[0].Cross( worldAxis[1] );

	// a world vector here, where a texture spot keeps a light-local one; AutoCaptureBase clears it
	base.fallback = worldAxis[1];
	base.haveFallback = true;

	// at pitch 90 ToAngles folds roll into yaw; the rotation is kept, the three numbers may change
	light.angles = frame.ToStoredAxis( worldAxis ).ToAngles();

	// G0 9: under auto range the aimed surface sits at 0.6 of the range, nearer or further, and the
	// size follows as for any range edit. Otherwise the aim turns the lamp and nothing else. The start
	// is clamped from its value at entry each frame (EmitterRangeSet clamps it to range - 1), so a near
	// aim does not eat it for the rest of the aim (DP-E4-01)
	if( EmitterAutoRangeInForce( light ) )
	{
		light.emitter.start = base.emitterStart;
		SetEmitterRange( light, distance / 0.6f );
		EmitterRangeSet( light );
	}
	return true;
}

/*
R4-3: a held point light that is still a sphere shows no orientation, so at modal entry its frame is
turned to the aim: axis[2] on the aim point, axis[0] horizontal (the viewer's left, the old point
AUTO's fallback when aiming straight up or down), right-handed. R and E are then screen-relative. A
point light with unequal radii (shaped already) keeps its frame. Returns true when it set the angles.
*/
static bool ModalAlignSphere( LightInfo& light, LightAutoBase& base, const idVec3& aimPoint )
{
	if( light.lightType != LIGHT_POINT || light.lightRadius.x != light.lightRadius.y || light.lightRadius.y != light.lightRadius.z )
	{
		return false;
	}

	const idVec3 d = aimPoint - light.origin;
	const float length = d.Length();
	if( length < 1.0f )
	{
		return false;
	}
	const idVec3 n = d / length;

	idMat3 axis;
	axis[2] = n;
	axis[0] = worldUp.Cross( n );
	if( axis[0].Length() < 0.01f )
	{
		axis[0] = AutoFallbackVector( base, idVec3( 1.0f, 0.0f, 0.0f ), n );
	}
	axis[0].Normalize();
	axis[1] = axis[2].Cross( axis[0] );
	base.fallback = axis[0];
	base.haveFallback = true;

	// U-4: stored as angles; axis[0] is horizontal, so ToAngles never takes its gimbal branch. An emitter
	// omni with a bulb offset turns about its bulb, as the gizmo turns it (G0 9, RV-C-ET-02): the lamp
	// and its lit volume stay where they are
	const idVec3 bulb = LightGizmoOrigin( light );
	light.angles = axis.ToAngles();
	light.origin = LightOriginFromGizmo( light, bulb );
	return true;
}

/*
R on an emitter spot is screen-relative from its first frame: at entry the roll follows AUTO's world-up
rule, axis[1] = normalize( worldUp x axis[0] ) in the world, through the stored-to-world frame. A beam
within 0.01 of vertical has no level roll and keeps its own, as AUTO keeps the previous one; a roll
already level is left alone, so a light AUTO has just aimed is not touched. The beam does not move.
Returns true when it set the angles.
*/
static bool ModalLevelSpotRoll( LightInfo& light, const LightStoredFrame& frame )
{
	if( !light.emitter.enabled || light.lightType != LIGHT_SPOT )
	{
		return false;
	}

	idMat3 worldAxis = light.angles.ToMat3() * frame.axis;
	idVec3 left = worldUp.Cross( worldAxis[0] );
	if( left.Length() < 0.01f )
	{
		return false;
	}
	left.Normalize();
	if( left * worldAxis[1] > 1.0f - 1e-5f )
	{
		return false;
	}

	worldAxis[1] = left;
	worldAxis[2] = worldAxis[0].Cross( left );
	light.angles = frame.ToStoredAxis( worldAxis ).ToAngles();
	return true;
}

/*
A gesture's entry (EnterModal). 'entry' takes the light as the gesture found it; then a held sphere
turns its frame to the last aim (R4-3) and, when 'spotFrame' is given (R on an emitter spot), the roll
is levelled (F-E-09). Cancel restores 'entry', so the authored roll and frame come back (G0 9, DP-E3-02);
the gesture itself runs from the prepared light. Returns true when it changed the light.
*/
static bool ModalPrepareEntry( LightInfo& light, LightInfo& entry, LightAutoBase& base, bool aimHit, const idVec3& aimPoint, const LightStoredFrame* spotFrame )
{
	entry = light;

	bool changed = false;
	if( aimHit && ModalAlignSphere( light, base, aimPoint ) )
	{
		changed = true;
	}
	if( spotFrame != NULL && ModalLevelSpotRoll( light, *spotFrame ) )
	{
		changed = true;
	}
	return changed;
}

// centre of a spot frustum's bounds in world orientation, relative to the light origin: the near
// rectangle at zNear and the far one at zFar, half extents right * z / |target| and up * z / |target|
static idVec3 SpotBoundsCenter( const idMat3& axis, const idVec3& target, const idVec3& right, const idVec3& up, float zNear, float zFar )
{
	const float targetLength = target.Length();
	const idVec3 n = target / targetLength;

	idBounds bounds;
	bounds.Clear();
	for( int i = 0; i < 8; i++ )
	{
		const float z = ( i & 1 ) ? zFar : zNear;
		const float sideRight = ( i & 2 ) ? 1.0f : -1.0f;
		const float sideUp = ( i & 4 ) ? 1.0f : -1.0f;
		const idVec3 corner = n * z + right * ( sideRight * z / targetLength ) + up * ( sideUp * z / targetLength );
		bounds.AddPoint( axis * corner );
	}
	return bounds.GetCenter();
}

// S8 Q: uniform scale about the centre of the light's bounds
static void ModalScale( LightInfo& light, const LightInfo& base, float mouseX, float scaleSens )
{
	// EB-10: bounded, so no inf (exp overflows near 22000 counts) can reach cur or the dict
	float s = idMath::ClampFloat( MODAL_SCALE_MIN, MODAL_SCALE_MAX, idMath::Exp( mouseX * scaleSens ) );

	if( base.emitter.enabled && base.lightType != LIGHT_SUN )
	{
		// the lamp scales as a whole: range, size and start together; the cone and the bulb stay
		light.origin = base.origin;
		light.lightRadius = base.lightRadius * s;
		light.emitter.size = Max( base.emitter.size * s, 1.0f );
		light.emitter.start = base.emitter.start * s;
		EmitterRangeEdited( light, EM_Range( base.lightRadius ) );
		return;
	}

	if( base.lightType != LIGHT_SPOT )
	{
		// the box is centred on the origin, which stays
		light.lightRadius = base.lightRadius * s;
		light.origin = base.origin;
		return;
	}

	const float targetLength = base.lightTarget.Length();
	if( targetLength < 1e-3f )
	{
		return;
	}
	const idVec3 n = base.lightTarget / targetLength;

	// D26 / V-B F6: implicit start/end made explicit as the renderer was already clamping them (start
	// at 8 along the aim, end at the target), so the near plane scales too; derived from the base
	// every frame, so a cancel restores the implicit form exactly
	const idVec3 start = base.explicitStartEnd ? base.lightStart : n * 8.0f;
	const idVec3 end = base.explicitStartEnd ? base.lightEnd : base.lightTarget;
	const float zNear = base.explicitStartEnd ? start * n : 8.0f;
	const float zFar = base.explicitStartEnd ? end * n : targetLength;
	if( zFar < 1e-3f )
	{
		return;
	}

	// MA-1: only the far plane's minimum (16) limits the scale; the near plane is pinned at the
	// renderer's 8 rather than stopping it, so Q-left shrinks every spot and nothing jumps at entry.
	// VF-1: a far plane the renderer already holds at 16 never forces s above 1
	s = Max( s, Min( 16.0f / zFar, 1.0f ) );
	const float zNearScaled = Max( zNear * s, 8.0f );

	// both centres with the depths the renderer uses and in world orientation, so the bounds the
	// renderer culls with keep their centre exactly for every s and every axis
	const idMat3 axis = base.angles.ToMat3();
	const idVec3 centerBefore = SpotBoundsCenter( axis, base.lightTarget, base.lightRight, base.lightUp, Max( zNear, 8.0f ), Max( zFar, 16.0f ) );
	const idVec3 centerAfter = SpotBoundsCenter( axis, base.lightTarget * s, base.lightRight * s, base.lightUp * s, zNearScaled, Max( zFar * s, 16.0f ) );

	light.origin = base.origin + centerBefore - centerAfter;
	light.lightTarget = base.lightTarget * s;
	light.lightRight = base.lightRight * s;
	light.lightUp = base.lightUp * s;
	light.lightStart = n * zNearScaled;
	light.lightEnd = end * s;
	light.explicitStartEnd = true;
}

// S9 E: per-axis scale; mouse x widens, mouse up heightens, the wheel stretches along the aim
static void ModalStretch( LightInfo& light, const LightInfo& base, float mouseX, float mouseY, float wheel, float scaleSens, float wheelSens )
{
	// EB-10: bounded like Q
	const float sx = idMath::ClampFloat( MODAL_SCALE_MIN, MODAL_SCALE_MAX, idMath::Exp( mouseX * scaleSens ) );
	const float sy = idMath::ClampFloat( MODAL_SCALE_MIN, MODAL_SCALE_MAX, idMath::Exp( -mouseY * scaleSens ) );
	float sz = idMath::ClampFloat( MODAL_SCALE_MIN, MODAL_SCALE_MAX, idMath::Exp( wheel * wheelSens ) );

	if( base.emitter.enabled && base.lightType == LIGHT_SPOT )
	{
		// mouse x widens cone h, mouse y cone v, the wheel moves the range; the bulb stays
		SetEmitterConeFromTan( light.emitter, base.emitter.tanHalfH * sx, base.emitter.tanHalfV * sy );
		light.emitter.size = base.emitter.size;
		light.origin = base.origin;
		SetEmitterRange( light, EM_Range( base.lightRadius ) * sz );
		EmitterRangeEdited( light, EM_Range( base.lightRadius ) );
		return;
	}

	if( base.lightType == LIGHT_SPOT )
	{
		const float targetLength = base.lightTarget.Length();
		if( base.explicitStartEnd && targetLength > 1e-3f )
		{
			// MA-4: wheel down must not pull the far plane inside the near one (zFar < zNear inverts
			// the falloff); end stays at least one unit beyond max( start . n, 8 ). VF-1: an end that
			// is already closer is left where it was authored, never pushed out at entry
			const idVec3 n = base.lightTarget / targetLength;
			const float startDepth = base.lightStart * n;
			const float endDepth = base.lightEnd * n;
			if( endDepth > 1e-3f )
			{
				sz = Max( sz, Min( ( Max( startDepth, 8.0f ) + 1.0f ) / endDepth, 1.0f ) );
			}
		}

		// the far plane moves through the reticle; start and the apex stay
		light.lightRight = base.lightRight * sx;
		light.lightUp = base.lightUp * sy;
		light.lightTarget = base.lightTarget * sz;
		if( base.explicitStartEnd )
		{
			light.lightEnd = base.lightEnd * sz;
		}
		light.origin = base.origin;
		return;
	}

	light.lightRadius.Set( base.lightRadius.x * sx, base.lightRadius.y * sy, base.lightRadius.z * sz );
	light.equalRadius = ( light.lightRadius.x == light.lightRadius.y && light.lightRadius.y == light.lightRadius.z );

	if( base.emitter.enabled && base.lightType == LIGHT_POINT )
	{
		// an emitter's origin is the lamp: the radii scale about the bulb, which stays in its fixture
		light.origin = base.origin;
	}
	else
	{
		// D11 / Q5: the near (-Z) face stays put, so the origin travels half the stretch along the aim
		const idMat3 axis = base.angles.ToMat3();
		light.origin = base.origin + axis[2] * ( base.lightRadius.z * sz - base.lightRadius.z );
	}

	// an emitter's radius is its range
	light.emitter.size = base.emitter.size;
	EmitterRangeEdited( light, EM_Range( base.lightRadius ) );
}

// S10 R / D12: roll about the aim, pitch about the viewer's right, yaw about the frame's up, by
// on-screen effect (mouse right = clockwise seen from behind, mouse up = aim up, wheel up = aim
// left); the signs are the ones that make le_selfTest's three exact checks pass. The axes are the
// frame at modal entry, which AUTO has aligned with the view.
static void ModalRotate( LightInfo& light, const LightInfo& base, float mouseX, float mouseY, float wheel, float rotSens, float rotWheelSens )
{
	const float rollAngle = -mouseX * rotSens;
	const float pitchAngle = mouseY * rotSens;
	const float yawAngle = -wheel * rotWheelSens;

	if( base.lightType != LIGHT_SPOT || base.emitter.enabled )
	{
		// right-handed frames. A point light aims along axis[2], with axis[0] the viewer's left and
		// axis[1] up; an emitter spot's beam is axis[0], with axis[1] left and axis[2] up
		const bool beamIsX = ( base.lightType == LIGHT_SPOT );
		const int aim = beamIsX ? 0 : 2;
		const int left = beamIsX ? 1 : 0;
		const int up = beamIsX ? 2 : 1;

		const idMat3 axis = base.angles.ToMat3();
		const idRotation roll( vec3_origin, axis[aim], rollAngle );
		const idRotation pitch( vec3_origin, -axis[left], pitchAngle );
		const idRotation yaw( vec3_origin, axis[up], yawAngle );

		idMat3 rotated;
		for( int i = 0; i < 3; i++ )
		{
			rotated[i] = yaw * ( pitch * ( roll * axis[i] ) );
		}
		light.angles = rotated.ToAngles();		// U-4
		return;
	}

	// spot: rotate the five vectors inside light space, apex fixed; angles stay
	const float targetLength = base.lightTarget.Length();
	if( targetLength < 1e-3f )
	{
		return;
	}
	const idVec3 n = base.lightTarget / targetLength;
	idVec3 right = base.lightRight - n * ( base.lightRight * n );
	if( right.LengthSqr() < 1e-6f )
	{
		// MA-6: no right vector (zero, or along the target) gives no frame; Normalize would make NaNs
		return;
	}
	right.Normalize();

	// right x n is the frame's up whatever the spot's handedness (it equals up for Doom's default)
	idVec3 up = right.Cross( n );
	up.Normalize();

	const idRotation roll( vec3_origin, n, rollAngle );
	const idRotation pitch( vec3_origin, right, pitchAngle );
	const idRotation yaw( vec3_origin, up, yawAngle );

	light.lightTarget = yaw * ( pitch * ( roll * base.lightTarget ) );
	light.lightRight = yaw * ( pitch * ( roll * base.lightRight ) );
	light.lightUp = yaw * ( pitch * ( roll * base.lightUp ) );
	light.lightStart = yaw * ( pitch * ( roll * base.lightStart ) );
	light.lightEnd = yaw * ( pitch * ( roll * base.lightEnd ) );
}

// S11 T / D13 / Q7: mouse x walks the hue forever, mouse y the intensity, the saturation stays. A white
// or grey light has no hue to walk, so it starts as gold at 25 % saturation: entering T shows the gold
// at once and a cancel restores the white. The two ImGui converters are pure (no context).
static void ModalColor( LightInfo& light, const LightInfo& base, float mouseX, float mouseY, float hueSens, float intSens, float maxIntensity )
{
	float intensity = Max3( base.color.x, base.color.y, base.color.z );
	if( intensity <= 0.0f )
	{
		intensity = 1.0f;
	}

	float h, s, v;
	ImGui::ColorConvertRGBtoHSV( base.color.x / intensity, base.color.y / intensity, base.color.z / intensity, h, s, v );
	if( s < 0.001f )
	{
		h = 0.125f;		// 45 degrees
		s = 0.25f;
	}

	// wrap before ImGui, whose ImFmod keeps the sign of a negative hue
	h = idMath::Frac( h + mouseX * hueSens );
	intensity = idMath::ClampFloat( 0.01f, maxIntensity, intensity * idMath::Exp( -mouseY * intSens ) );

	float r, g, b;
	ImGui::ColorConvertHSVtoRGB( h, s, 1.0f, r, g, b );
	light.color.Set( r * intensity, g * intensity, b * intensity );
}

// ########### easy mode (plan 3.1, plan 3.2, plan 3.6, plan 3.8, plan 3.9) #############

namespace
{

// the LightInfo fields easy mode changes, as plain bytes, for the panel's apply-on-change rule
class LightPose
{
public:
	idVec3			origin;
	idAngles		angles;
	idVec3			lightRadius;
	idVec3			lightTarget;
	idVec3			lightRight;
	idVec3			lightUp;
	idVec3			lightStart;
	idVec3			lightEnd;
	idVec3			lightCenter;
	idVec3			color;
	emitterData_t	emitter;
	int				flags;

	void			Capture( const LightInfo& light )
	{
		memset( this, 0, sizeof( *this ) );
		origin = light.origin;
		angles = light.angles;
		lightRadius = light.lightRadius;
		lightTarget = light.lightTarget;
		lightRight = light.lightRight;
		lightUp = light.lightUp;
		lightStart = light.lightStart;
		lightEnd = light.lightEnd;
		lightCenter = light.lightCenter;
		color = light.color;
		memcpy( &emitter, &light.emitter, sizeof( emitter ) );
		flags = ( light.equalRadius ? 1 : 0 ) | ( light.explicitStartEnd ? 2 : 0 ) | ( light.hasCenter ? 4 : 0 ) | ( light.autoRange ? 8 : 0 );
	}
};

} //anon. namespace

static int EasyKeySlot( int keyNum )
{
	for( int i = 0; i < LightEditor::NUM_EASY_KEYS; i++ )
	{
		if( easyKeys[ i ] == keyNum )
		{
			return i;
		}
	}
	return -1;
}

static bool IsModalKey( int keyNum )
{
	return ( keyNum == K_Q || keyNum == K_E || keyNum == K_R || keyNum == K_T );
}

/*
plan 3.2: every SE_KEY, before ImGuiHook::InjectSysEvent's gate. One edge source in both modes: a down
with the latch clear is one press, a down with the latch set is WM_KEYDOWN auto-repeat, an up clears
the latch. The latch is kept whatever the mode; only the queuing depends on it. Returns true to
swallow the event (no binding fires, ImGui never sees it).
*/
bool LightEditor::KeyEvent( int keyNum, bool down )
{
	const int slot = EasyKeySlot( keyNum );
	if( slot < 0 )
	{
		return false;
	}

	// WP-LE1 runtime check: do mouse buttons or the wheel reach SE_KEY twice under DirectInput?
	if( le_logInput.GetBool() && editorOpen && ( keyNum == K_MOUSE1 || keyNum == K_MOUSE2 || keyNum == K_MWHEELUP || keyNum == K_MWHEELDOWN ) )
	{
		common->Printf( "le_logInput: frame %d %s %s\n", idLib::frameNumber, idKeyInput::KeyNumToString( ( keyNum_t )keyNum ), down ? "down" : "up" );
	}

	if( !down )
	{
		// an up is taken exactly when its down was, so ImGui never sees half a press
		const bool swallow = keySwallowed[ slot ];
		keyLatched[ slot ] = false;
		keySwallowed[ slot ] = false;
		return swallow;
	}

	if( keyLatched[ slot ] )
	{
		return keySwallowed[ slot ];
	}
	keyLatched[ slot ] = true;

	// D39: F and H belong to the tool in both modes, the rest only in easy mode
	bool own = editorOpen && ( easyMode || keyNum == K_F || keyNum == K_H );
	if( own && !easyMode && ImGuiHook::IsInitialized() && ImGui::GetIO().WantTextInput )
	{
		// silent while typing: in a focused text field F and H are letters
		own = false;
	}
	keySwallowed[ slot ] = own;

	// a full queue drops the newest press (idStaticList::Append asserts when full, so test first)
	if( own && pressQueue.Num() < pressQueue.Max() )
	{
		pressQueue.Append( keyNum );
	}
	return own;
}

/*
the frame between the light's spawn dict pose and the world, read as idLight::UpdateChangeableSpawnArgs
derives the entity frame from the same dict; nowOrigin and nowAxis are the entity's physics pose.
A light without light_origin gets the identity: its entity frame comes from "angles", the key the
editor writes the light's own rotation to, so E would follow every edit (RV-G4-04). Unbound it is the
identity anyway; bound, the light is aimed in its stored space, which is right while the bind master
is at its spawn pose. Pure, so le_selfTest can call it with no entity
*/
static void LightStoredFrameFromDict( const idDict& args, const idVec3& nowOrigin, const idMat3& nowAxis, LightStoredFrame& frame )
{
	frame.Identity();

	if( args.FindKey( "light_origin" ) == NULL )
	{
		return;
	}

	idVec3 entOrigin;
	idMat3 entAxis;
	args.GetVector( "origin", "0 0 0", entOrigin );
	if( args.GetString( "model" )[0] != '\0' )
	{
		// only entities with a model have origin brushes (BRUSH_ORIGIN_KEY of the game)
		entOrigin += args.GetVector( "__brushOrigin", "0 0 0" );
	}

	if( !args.GetMatrix( "rotation", "1 0 0 0 1 0 0 0 1", entAxis ) )
	{
		idAngles entAngles;

		entAxis.Identity();
		if( args.GetAngles( "angles", "0 0 0", entAngles ) )
		{
			if( entAngles.pitch != 0.0f || entAngles.yaw != 0.0f || entAngles.roll != 0.0f )
			{
				entAxis = entAngles.ToMat3();
			}
		}
		else
		{
			const float angle = args.GetFloat( "angle" );
			if( angle != 0.0f )
			{
				entAxis = idAngles( 0.0f, angle, 0.0f ).ToMat3();
			}
		}
	}

	frame.storedOrigin = entOrigin;
	frame.worldOrigin = nowOrigin;
	frame.axis = entAxis.Transpose() * nowAxis;
}

// the same for a live light entity: its spawn args and its physics pose now
static void LightStoredFrameOf( idEntity* light, LightStoredFrame& frame )
{
	frame.Identity();

	const idDict* args = gameEdit->EntityGetSpawnArgs( light );
	if( args == NULL )
	{
		return;
	}

	idVec3 nowOrigin;
	idMat3 nowAxis;
	gameEdit->EntityGetOrigin( light, nowOrigin );
	gameEdit->EntityGetAxis( light, nowAxis );
	LightStoredFrameFromDict( *args, nowOrigin, nowAxis, frame );
}

/*
plan 3.9: once per frame from the top of idCommonLocal::Draw's map-spawned branch, before the scene is
drawn. Consumes the presses in order, runs the state machine and applies the preview through the
panel's own TempApplyChanges when cur changed. No ImGui frame or draw call.
*/
void LightEditor::RunFrame()
{
	// Draw() also runs from UpdateScreen and StartWipe
	if( !editorOpen || idLib::frameNumber == lastRunFrame )
	{
		return;
	}

	// D27: a skipped frame (the Esc shell, a wipe, a load) drops what piled up meanwhile
	const bool resumed = ( idLib::frameNumber != lastRunFrame + 1 );
	lastRunFrame = idLib::frameNumber;

	// the engine's key table sees every SE_KEY, also those the console or a menu ate
	for( int i = 0; i < NUM_EASY_KEYS; i++ )
	{
		if( keyLatched[ i ] && !idKeyInput::IsDown( easyKeys[ i ] ) )
		{
			keyLatched[ i ] = false;
		}
	}

	int mouseX, mouseY, mouseButton;
	bool mouseDown;
	usercmdGen->MouseState( &mouseX, &mouseY, &mouseButton, &mouseDown );
	if( resumed )
	{
		pressQueue.Clear();
		lastMouseX = mouseX;
		lastMouseY = mouseY;
	}
	if( easyMode && easyState == EASY_MODAL && !console->Active() && !common->Dialog().IsDialogActive() )
	{
		modalX += mouseX - lastMouseX;
		modalY += mouseY - lastMouseY;
	}
	lastMouseX = mouseX;
	lastMouseY = mouseY;

	// presses that write cur (undo, cancel) raise applyPending themselves
	for( int i = 0; i < pressQueue.Num(); i++ )
	{
		if( !HandlePress( pressQueue[ i ] ) )
		{
			break;
		}
	}
	pressQueue.Clear();

	lastAimHit = false;
	if( easyMode && lightEntity != NULL )
	{
		// AUTO and the modals recompute the pose every frame; only a frame whose result differs has
		// written anything, so a still view applies nothing (no sound restart while you look)
		LightPose before;
		before.Capture( cur );

		if( easyState == EASY_AUTO )
		{
			// D8: the light's own clip model is out of this one trace, so a fixture never catches the aim
			idVec3 hitNormal;
			lastAimHit = gameEdit->PlayerTraceFromEye( le_traceDist.GetFloat(), lightEntity, lastAim, hitNormal );

			// S3: on a miss nothing moves this frame; a point light holds anyway (D46), the trace only
			// feeds the reticle
			if( lastAimHit && cur.lightType != LIGHT_POINT )
			{
				if( cur.emitter.enabled && cur.lightType == LIGHT_SPOT )
				{
					// the aim is a world point; a light_origin light on a moved entity stores its pose in the
					// dict's space. The frame is taken once per AUTO entry, before AUTO writes anything, and
					// kept: read back every frame it would follow the rotation AUTO writes (RV-G4-04)
					if( !autoBase.haveStoredFrame )
					{
						LightStoredFrameOf( lightEntity, autoBase.storedFrame );
						autoBase.haveStoredFrame = true;
					}
					AutoAimEmitterSpot( cur, autoBase, lastAim );
				}
				else
				{
					// PlayerGetRenderView dereferences the local player unchecked
					renderView_t view;
					const bool haveView = ( cur.lightType == LIGHT_SUN && gameEdit->PlayerIsValid() && gameEdit->PlayerGetRenderView( view ) );
					AutoAimLight( cur, autoBase, lastAim, haveView ? &view.viewaxis[0] : NULL );
				}
			}
		}
		else if( easyState == EASY_MODAL )
		{
			// D17: a pure function of modalBase and the counts since modal entry, no integration
			const float mouseX = ( float )modalX;
			const float mouseY = ( float )modalY;
			const float wheel = ( float )modalWheel;
			switch( modalKey )
			{
				case K_Q:
					ModalScale( cur, modalBase, mouseX, le_scaleSens.GetFloat() );
					break;
				case K_E:
					ModalStretch( cur, modalBase, mouseX, mouseY, wheel, le_scaleSens.GetFloat(), le_wheelSens.GetFloat() );
					break;
				case K_R:
					ModalRotate( cur, modalBase, mouseX, mouseY, wheel, le_rotSens.GetFloat(), le_rotWheelSens.GetFloat() );
					break;
				case K_T:
					ModalColor( cur, modalBase, mouseX, mouseY, le_hueSens.GetFloat(), le_intSens.GetFloat(), le_maxIntensity.GetFloat() );
					// under auto range an emitter's range follows its intensity
					EmitterReachChanged( cur );
					break;
				default:
					break;
			}
		}

		LightPose after;
		after.Capture( cur );
		if( memcmp( &before, &after, sizeof( before ) ) != 0 )
		{
			applyPending = true;
		}
	}

	// ST-3: the panel's rule, apply only what was written; TempApplyChanges clears the flag
	if( applyPending )
	{
		TempApplyChanges();
	}
}

/*
plan 3.1: one counted press. Returns false when the rest of this frame's presses must be dropped
(a mode switch: F, or RMB out of easy mode).
*/
bool LightEditor::HandlePress( int key )
{
	// UI-5: any press ends the "nothing selected" hint
	hintUntilFrame = 0;

	if( key == K_H )
	{
		le_showLegend.SetBool( !le_showLegend.GetBool() );
		return true;
	}

	if( key == K_F )
	{
		SwitchMode();
		return false;
	}

	// advanced mode queues only F and H
	if( !easyMode )
	{
		return true;
	}

	// D49: Enter saves the light to the .map (with the panel's own save, the only writers). A modal
	// ends first, keeping its result as a checkpoint as LMB does. On success the saved shape gives
	// AUTO its new endpoint dimensions and AUTO resumes; on any failure (no locator, refused file,
	// skipped block, short write) the state stays, a checkpoint keeps its undo, the warning shows,
	// and Enter again retries
	if( key == K_ENTER || key == K_KP_ENTER )
	{
		if( easyState == EASY_MODAL )
		{
			TakeCheckpoint( modalEntry );
			easyState = EASY_CHECKPOINT;
		}
		if( easyState != EASY_NONE && SaveChanges( true, true ) )
		{
			checkpointUndo.Defaults();
			checkpointEmit.Clear();
			AutoCaptureBase( cur, autoBase );
			easyState = EASY_AUTO;
		}
		return true;
	}

	switch( easyState )
	{
		case EASY_NONE:
			if( key == K_MOUSE1 )
			{
				// D30 / D4: AUTO only if LightEditorInit ran during the call; its return value only says
				// the click was consumed (it is true inside SelectEntity's 300 ms debounce)
				gameEdit->EditEntitySelectFromView();
				if( lightEntity != NULL )
				{
					easyState = EASY_AUTO;
				}
				else
				{
					// UI-5: a miss, a non-light or the debounce: say so instead of staying silent
					hintText = "nothing selected: aim at a light's volume (r_showLights 3) and press LMB";
					hintUntilFrame = idLib::frameNumber + 90;
				}
			}
			else if( key == K_MOUSE2 )
			{
				// S4: back out of easy mode to the state editLights opened in (nothing shown, the
				// game's mouse, lights pickable); the editor stays open, only the command closes it
				SwitchMode();
				return false;
			}
			break;

		case EASY_AUTO:
			if( key == K_MOUSE1 )
			{
				// S5: freeze; nothing is applied, cur stays
				TakeCheckpoint( cur );
				easyState = EASY_CHECKPOINT;
			}
			else if( key == K_MOUSE2 )
			{
				// S4 / D7: revert, deselect, and back out of easy mode as from NONE
				Deselect( false );
				SwitchMode();
				return false;
			}
			else if( IsModalKey( key ) )
			{
				EnterModal( key );
			}
			break;

		case EASY_CHECKPOINT:
			if( key == K_MOUSE1 )
			{
				// D49: LMB releases the checkpoint (Enter saves): cur kept, and its shape gives AUTO
				// its new endpoint dimensions
				checkpointUndo.Defaults();
				checkpointEmit.Clear();
				AutoCaptureBase( cur, autoBase );
				easyState = EASY_AUTO;
			}
			else if( key == K_MOUSE2 )
			{
				// D7: undo the checkpoint; AUTO resumes from the state the checkpoint was made from
				UndoCheckpoint();
				checkpointEmit.Clear();
				easyState = EASY_AUTO;
			}
			// S8: Q, E, R and T do nothing here
			break;

		case EASY_MODAL:
			if( key == K_MOUSE1 )
			{
				TakeCheckpoint( modalEntry );
				easyState = EASY_CHECKPOINT;
			}
			else if( key == K_MOUSE2 || key == modalKey )
			{
				// U-2: the same key again, or RMB, cancels exactly (D17), to the light as the gesture found
				// it: before ModalAlignSphere or ModalLevelSpotRoll touched it (G0 9, DP-E3-02)
				cur = modalEntry;
				applyPending = true;
				easyState = EASY_AUTO;
			}
			else if( key == K_MWHEELUP || key == K_MWHEELDOWN )
			{
				modalWheel += ( key == K_MWHEELUP ) ? 1 : -1;
			}
			// another modal key is ignored: one modalBase at a time keeps the undo exact
			break;
	}

	return true;
}

// E2's entry (LMB in AUTO or in a gesture, Enter in a gesture): the light to come back to, with the emit
// keys it has then (RV-C-ET-01), because F may open the panel and its tabs while the checkpoint stands
void LightEditor::TakeCheckpoint( const LightInfo& undo )
{
	checkpointUndo = undo;
	checkpointEmit.Clear();
	if( lightEntity != NULL )
	{
		CheckpointKeys( *gameEdit->EntityGetSpawnArgs( lightEntity ), undo, checkpointEmit );
	}
}

// RMB on a checkpoint: the light as it was, its kind included. Applied here as Cancel applies, since
// TempApplyChanges writes only what ToDict writes and would leave an emitter "1"
void LightEditor::UndoCheckpoint()
{
	cur = checkpointUndo;
	applyPending = false;
	if( lightEntity != NULL )
	{
		idDict d;
		CheckpointUndoDict( cur, *gameEdit->EntityGetSpawnArgs( lightEntity ), checkpointEmit, d );
		gameEdit->EntityChangeSpawnArgs( lightEntity, &d );
		gameEdit->EntityUpdateChangeableSpawnArgs( lightEntity, NULL );
	}
}

// S6: F in both directions, and RMB out of easy mode (NONE, or AUTO after its deselect)
void LightEditor::SwitchMode()
{
	if( easyMode )
	{
		if( easyState == EASY_MODAL )
		{
			cur = modalEntry;
			applyPending = true;
			easyState = EASY_AUTO;
		}
		easyMode = false;
	}
	else
	{
		easyMode = true;
		if( lightEntity == NULL )
		{
			easyState = EASY_NONE;
		}
		else if( easyState == EASY_AUTO )
		{
			// panel edits become AUTO's new endpoint dimensions
			AutoCaptureBase( cur, autoBase );
		}
	}

	// easy mode draws its overlay; advanced mode draws the panel only with a light, else it waits for
	// a pick with the game's mouse, as the editor did before easy mode
	isShown = easyMode || lightEntity != NULL;
	// the panel gets the cursor; easy mode, and advanced mode with no light, keep the grabbed mouse
	SetReleaseToolMouse( !easyMode && lightEntity != NULL );

	// D5: never usercmdGen->Clear() (the grab change runs it already); the key latches stay, the
	// keys are still down. The caller drops the rest of this frame's presses.
	ImGuiHook::ResetInputLatches();

	int mouseButton;
	bool mouseDown;
	usercmdGen->MouseState( &lastMouseX, &lastMouseY, &mouseButton, &mouseDown );
}

// U-2: Q, E, R or T pressed in AUTO
void LightEditor::EnterModal( int key )
{
	// an emitter spot's R works in the light's frame, so its roll is levelled first (in the world, through
	// the frame AUTO took for this entry, else the light's own now)
	LightStoredFrame frame;
	const LightStoredFrame* spotFrame = NULL;
	if( key == K_R && cur.emitter.enabled && cur.lightType == LIGHT_SPOT && lightEntity != NULL )
	{
		if( autoBase.haveStoredFrame )
		{
			frame = autoBase.storedFrame;
		}
		else
		{
			LightStoredFrameOf( lightEntity, frame );
		}
		spotFrame = &frame;
	}

	if( ModalPrepareEntry( cur, modalEntry, autoBase, lastAimHit, lastAim, spotFrame ) )
	{
		applyPending = true;
	}

	modalKey = key;
	modalBase = cur;
	modalX = 0;
	modalY = 0;
	modalWheel = 0;
	easyState = EASY_MODAL;

	// the mouse reference was re-based before this frame's presses ran
}

// D30: the one deselect routine of Apply, Cancel, RMB in AUTO and the editLights off path
bool LightEditor::Deselect( bool commit )
{
	if( commit )
	{
		// D48: Apply is the original editor's in-memory commit, written by the panel's save; a commit
		// that cannot be located keeps the light selected and the warning says why
		if( !SaveChanges( false, false ) )
		{
			return false;
		}
	}
	else
	{
		CancelChanges();
	}

	gameEdit->ClearEntitySelection();
	Reset();
	entityName.Clear();
	easyState = EASY_NONE;
	checkpointEmit.Clear();
	return true;
}

// plan 3.8: editLights on
void LightEditor::Open()
{
	// D48: opened again while open never writes; it says what it drops
	if( editorOpen )
	{
		WarnUnsavedApplies();
	}

	editorOpen = true;
	hintUntilFrame = 0;
	// le_startEasy picks the opening mode; F switches either way (S6). Advanced mode starts as the
	// editor did before easy mode: nothing shown and the game's mouse, until a pick (LightEditorInit)
	// shows the panel and releases the cursor
	easyMode = le_startEasy.GetBool();
	isShown = easyMode;
	easyState = EASY_NONE;

	Reset();
	entityName.Clear();

	committed.Clear();
	lastWarning.Clear();

	// G0 9: a new session; Equilateral shows ticked again for an emitter omni with equal radii
	equilateralUnticked = false;

	// required: the off path never cleared releaseMouse
	SetReleaseToolMouse( false );
	ImGuiHook::ResetInputLatches();

	pressQueue.Clear();
	int mouseButton;
	bool mouseDown;
	usercmdGen->MouseState( &lastMouseX, &lastMouseY, &mouseButton, &mouseDown );
	lastRunFrame = -2;
}

/*
plan 3.8 / D16 / D48: editLights off and map shutdown. Never saves and is idempotent. Pending panel
Applies are dropped after one console warning; that printf is its only engine call (on a quit the
first MapShutdown has already emptied the list, so the second one, after ImGui and the renderer
are gone, prints nothing).
*/
void LightEditor::Close()
{
	WarnUnsavedApplies();

	Reset();
	entityName.Clear();

	committed.Clear();
	lastWarning.Clear();

	easyState = EASY_NONE;
	checkpointEmit.Clear();
	easyMode = true;
	isShown = false;
	editorOpen = false;
	pressQueue.Clear();

	SetReleaseToolMouse( false );
}

/*
D32: from idGameLocal::UnregisterEntity, after ~idLight has run. Compares and assigns only; it also
fires during the second MapShutdown of a quit and for MP predicted entities.
*/
void LightEditor::EntityRemoved( idEntity* ent )
{
	if( ent == NULL || ent != lightEntity )
	{
		return;
	}

	// a light created in this session and removed again is not appended to the file (its pending
	// Apply, if any, goes with it)
	const int commitNum = entityNameFromDict ? FindCommit( entityName ) : -1;
	if( commitNum >= 0 && committed[ commitNum ].isNew )
	{
		committed.RemoveIndex( commitNum );
	}

	Reset();
	entityName.Clear();
	easyState = EASY_NONE;

	// the checkpoint and the modal snapshot belonged to the removed light
	checkpointUndo.Defaults();
	checkpointEmit.Clear();
	modalBase.Defaults();
	modalEntry.Defaults();
	modalKey = 0;
	modalX = 0;
	modalY = 0;
	modalWheel = 0;
}

// plan 3.6: drawn in Draw() (the ImGui pass), never in RunFrame
void LightEditor::DrawEasyOverlay()
{
	ImGuiIO& io = ImGui::GetIO();

	const char* stateText = "NONE";
	ImU32 reticleColor = IM_COL32( 255, 255, 255, 255 );
	switch( easyState )
	{
		case EASY_NONE:
			break;
		case EASY_AUTO:
			// D46: a point light holds where it is; spots and suns follow the aim
			stateText = ( cur.lightType == LIGHT_POINT ) ? "HOLD (LMB checkpoints, ENTER saves)" : "AUTO (LMB checkpoints, ENTER saves)";
			reticleColor = lastAimHit ? IM_COL32( 64, 255, 64, 255 ) : IM_COL32( 128, 128, 128, 255 );
			break;
		case EASY_CHECKPOINT:
			stateText = "CHECKPOINT (ENTER saves, LMB releases, RMB undoes)";
			reticleColor = IM_COL32( 64, 255, 255, 255 );
			break;
		case EASY_MODAL:
			reticleColor = IM_COL32( 255, 160, 0, 255 );
			switch( modalKey )
			{
				case K_Q:
					stateText = "Q: scaling (Q again or RMB cancels, LMB checkpoints, ENTER saves)";
					break;
				case K_E:
					stateText = "E: stretching (E again or RMB cancels, LMB checkpoints, ENTER saves)";
					break;
				case K_R:
					stateText = "R: rotating (R again or RMB cancels, LMB checkpoints, ENTER saves)";
					break;
				default:
					stateText = "T: colour (T again or RMB cancels, LMB checkpoints, ENTER saves)";
					break;
			}
			break;
	}

	// NoInputs is load-bearing: io.MouseDown is fed from the key state even without ImGui input
	ImGui::SetNextWindowPos( ImVec2( 10.0f, 10.0f ) );
	ImGui::Begin( "##lightEditorEasy", NULL, ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings );

	// UI-5 / D48: a transient note ("saved <name>", "nothing selected ...") replaces the subject
	const char* subject = "aim at a light and press LMB";
	if( idLib::frameNumber < hintUntilFrame )
	{
		subject = hintText.c_str();
	}
	else if( lightEntity != NULL )
	{
		subject = entityName.c_str();
	}
	ImGui::Text( "Light editor - EASY | %s | %s%s", stateText, subject, le_showLegend.GetBool() ? "" : "  H: legend" );

	if( lightEntity != NULL )
	{
		static const char* falloffNames[] = { "inverse square", "linear", "smooth", "exponential", "inverse square clamped" };
		const int falloff = idMath::ClampInt( 0, IM_ARRAYSIZE( falloffNames ) - 1, cur.emitter.falloff );
		const char* autoText = cur.autoRange ? ", auto range" : "";
		if( cur.emitter.enabled && cur.lightType == LIGHT_SPOT )
		{
			ImGui::Text( "emitter spot: range %.0f, cone %.0f x %.0f, %s, size %.0f%s", EM_Range( cur.lightRadius ), cur.emitter.coneH, cur.emitter.coneV, falloffNames[ falloff ], cur.emitter.size, autoText );
		}
		else if( cur.emitter.enabled && cur.lightType == LIGHT_POINT )
		{
			ImGui::Text( "emitter omni: range %.0f, %s, size %.0f%s", EM_Range( cur.lightRadius ), falloffNames[ falloff ], cur.emitter.size, autoText );
		}
		else if( cur.emitter.enabled )
		{
			ImGui::TextUnformatted( "emitter sun" );
		}
		else
		{
			ImGui::TextUnformatted( "texture light" );
		}
	}

	if( !lastWarning.IsEmpty() )
	{
		ImGui::TextColored( ImVec4( 1.0f, 0.75f, 0.2f, 1.0f ), "%s", lastWarning.c_str() );
	}

	// D48: only the panel's save writes panel Applies; leaving drops them
	const int pendingApplies = PendingApplies();
	if( pendingApplies > 0 )
	{
		ImGui::Text( "%d applied in the panel, not saved (F, then Ctrl+S)", pendingApplies );
	}

	if( le_showLegend.GetBool() )
	{
		ImGui::Separator();
		ImGui::TextUnformatted( "LMB  select / checkpoint / release checkpoint" );
		ImGui::TextUnformatted( "ENTER  save the light to the map" );
		ImGui::TextUnformatted( "RMB  back: cancel modal / undo checkpoint / leave easy mode" );
		ImGui::TextUnformatted( "Q    scale (mouse x)                 E    scale X/Y (mouse), Z (wheel)" );
		ImGui::TextUnformatted( "R    rotate Z (mouse x) X (mouse y) Y (wheel)        modal keys toggle; same key or RMB cancels" );
		ImGui::TextUnformatted( "T    hue (mouse x), brightness (mouse y)" );
		ImGui::TextUnformatted( "F    advanced panel                  H    hide this legend" );
	}

	ImGui::End();

	// the reticle: a cross at the screen centre, a circle where AUTO aims
	ImDrawList* drawList = ImGui::GetForegroundDrawList();
	const ImVec2 center( io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f );
	drawList->AddLine( ImVec2( center.x - 6.0f, center.y ), ImVec2( center.x + 6.0f, center.y ), reticleColor, 2.0f );
	drawList->AddLine( ImVec2( center.x, center.y - 6.0f ), ImVec2( center.x, center.y + 6.0f ), reticleColor, 2.0f );

	viewDef_t viewDef;
	if( easyState == EASY_AUTO && lastAimHit && SetupToolView( viewDef ) )
	{
		// V-B20: idLib row-vector matrices, translation at 12..14
		const float* m = viewDef.worldSpace.modelViewMatrix;
		const float* p = viewDef.unjitteredProjectionMatrix;

		float eye[4];
		for( int j = 0; j < 4; j++ )
		{
			eye[ j ] = lastAim.x * m[ j ] + lastAim.y * m[ 4 + j ] + lastAim.z * m[ 8 + j ] + m[ 12 + j ];
		}

		float clip[4];
		for( int j = 0; j < 4; j++ )
		{
			clip[ j ] = eye[ 0 ] * p[ j ] + eye[ 1 ] * p[ 4 + j ] + eye[ 2 ] * p[ 8 + j ] + eye[ 3 ] * p[ 12 + j ];
		}

		if( clip[ 3 ] > 0.0f )
		{
			const float sx = ( 0.5f + 0.5f * clip[ 0 ] / clip[ 3 ] ) * io.DisplaySize.x;
			const float sy = ( 0.5f - 0.5f * clip[ 1 ] / clip[ 3 ] ) * io.DisplaySize.y;
			drawList->AddCircle( ImVec2( sx, sy ), 8.0f, reticleColor, 0, 2.0f );
		}
	}
}

// ########### le_selfTest (D22, D38) #############

//puncho - dev guard (RV2-EA-06, RV2-ET-05): test scaffolding stays out of retail builds, as cm_selfTest's
#if !defined( ID_RETAIL )

namespace
{

class LightSelfTest
{
public:
	int			run = 0;
	int			failed = 0;

	void		Check( bool ok, const char* what )
	{
		run++;
		if( !ok )
		{
			failed++;
		}
		common->Printf( "%s %s\n", ok ? "PASS" : "FAIL", what );
	}
};

} //anon. namespace

static LightCommit SelfTestCommit( const char* name, bool isNew, bool writeName, const idVec3& onDiskOrigin, int indexHint )
{
	LightCommit commit;
	commit.entityName = name;
	commit.isNew = isNew;
	commit.writeName = writeName;
	commit.pending = true;
	commit.name = name;
	commit.classname = "light";
	commit.onDiskOrigin = onDiskOrigin;
	commit.indexHint = indexHint;
	return commit;
}

// prints where two texts part, so a FAIL can be read without a debugger
static void SelfTestShowMismatch( const idStr& got, const idStr& want )
{
	int i = 0;
	while( i < got.Length() && i < want.Length() && got[ i ] == want[ i ] )
	{
		i++;
	}
	common->Printf( "     first difference at byte %d: got \"%s\" want \"%s\"\n", i, got.Mid( i, 40 ).c_str(), want.Mid( i, 40 ).c_str() );
}

// WP-LE0b: the splice on a Valve 220 sample in memory
static void SelfTestSplice( LightSelfTest& test )
{
	const char* header = "// Game: Doom 3 BFG\r\n// Format: Doom3 (Valve)\r\n";
	const char* world = "// entity 0\r\n{\r\n\"mapversion\" \"220\"\r\n\"classname\" \"worldspawn\"\r\n// brush 0\r\n{\r\n"
						"( -64 64 0 ) ( -64 -64 0 ) ( -64 -64 -16 ) tb_test/wall [ 0 1 0 0 ] [ 0 0 -1 0 ] 0 0.5 0.5 0 0 0\r\n"
						"( 64 -64 0 ) ( 64 64 0 ) ( 64 64 -16 ) tb_test/wall [ 0 1 0 0 ] [ 0 0 -1 0 ] 0 0.5 0.5 0 0 0\r\n}\r\n}\r\n";
	const char* group = "// entity 1\r\n{\r\n\"classname\" \"func_group\"\r\n\"_tb_type\" \"_tb_layer\"\r\n\"_tb_name\" \"lights\"\r\n\"_tb_id\" \"2\"\r\n"
						"// brush 0\r\n{\r\n( 0 0 0 ) ( 0 1 0 ) ( 1 0 0 ) tb_test/wall [ 1 0 0 0 ] [ 0 -1 0 0 ] 0 0.5 0.5 0 0 0\r\n}\r\n}\r\n";
	const char* named = "// entity 2\r\n{\r\n\"classname\" \"light\"\r\n\"name\" \"light_named\"\r\n\"origin\" \"10 20 30\"\r\n\"_color\" \"1 1 1\"\r\n"
						"\"light_radius\" \"300 300 300\"\r\n\"_tb_layer\" \"2\"\r\n}\r\n";
	const char* namedOut = "// entity 2\r\n{\r\n\"classname\" \"light\"\r\n\"name\" \"light_named\"\r\n\"origin\" \"10 20 30\"\r\n\"_color\" \"0.25 0.5 1\"\r\n"
						   "\"_tb_layer\" \"2\"\r\n\"light_center\" \"0 0 8\"\r\n}\r\n";
	const char* unnamed = "// entity 3\r\n{\r\n\"classname\" \"light\"\r\n\"origin\" \"-100 50 64\"\r\n\"_color\" \"0.5 0.5 0.5\"\r\n\"light_radius\" \"200 200 200\"\r\n}\r\n";
	const char* unnamedOut = "// entity 3\r\n{\r\n\"classname\" \"light\"\r\n\"origin\" \"-100 50 64\"\r\n\"_color\" \"0.1 0.2 0.3\"\r\n\"name\" \"light_7\"\r\n}\r\n";
	const char* commented = "// entity 4\r\n{\r\n\"classname\" \"light\"\r\n\"name\" \"light_commented\"\r\n// a comment inside the block\r\n"
							"// a \"quoted\" { comment\r\n\"origin\" \"256 0 128\"\r\n\"noshadows\" \"0\"\r\n\"_color\" \"1 0 0\"\r\n}\r\n";
	const char* commentedOut = "// entity 4\r\n{\r\n\"classname\" \"light\"\r\n\"name\" \"light_commented\"\r\n// a comment inside the block\r\n"
							   "// a \"quoted\" { comment\r\n\"origin\" \"256 0 128\"\r\n\"_color\" \"0 1 0\"\r\n\"nospecular\" \"1\"\r\n}\r\n";
	const char* memOnly = "// entity 5\r\n{\r\n\"classname\" \"light\"\r\n\"origin\" \"300 300 300\"\r\n\"light_radius\" \"100 100 100\"\r\n\"_color\" \"1 1 1\"\r\n}\r\n";
	const char* memOnlyOut = "// entity 5\r\n{\r\n\"classname\" \"light\"\r\n\"origin\" \"300 300 300\"\r\n\"_color\" \"0.5 0.5 1\"\r\n\"light_center\" \"0 0 16\"\r\n}\r\n";
	const char* moved = "// entity 6\r\n{\r\n\"classname\" \"light\"\r\n\"name\" \"light_moved\"\r\n\"origin\" \"512 512 0\"\r\n}\r\n";
	const char* brushLight = "// entity 7\r\n{\r\n\"classname\" \"light\"\r\n\"name\" \"light_brush\"\r\n\"origin\" \"0 0 0\"\r\n// brush 0\r\n{\r\n"
							 "( 0 0 0 ) ( 0 1 0 ) ( 1 0 0 ) tb_test/wall [ 1 0 0 0 ] [ 0 -1 0 0 ] 0 0.5 0.5 0 0 0\r\n}\r\n}\r\n";
	const char* tieA = "// entity 8\r\n{\r\n\"classname\" \"light\"\r\n\"origin\" \"700 0 0\"\r\n\"_color\" \"1 1 1\"\r\n}\r\n";
	const char* tieB = "// entity 9\r\n{\r\n\"classname\" \"light\"\r\n\"origin\" \"700 0 0\"\r\n\"_color\" \"1 1 1\"\r\n}\r\n";
	const char* tieBOut = "// entity 9\r\n{\r\n\"classname\" \"light\"\r\n\"origin\" \"700 0 0\"\r\n\"_color\" \"0 0 1\"\r\n}\r\n";
	const char* player = "// entity 10\r\n{\r\n\"classname\" \"info_player_start\"\r\n\"name\" \"info_player_start_0\"\r\n\"origin\" \"0 0 16\"\r\n}\r\n";
	const char* appended = "{\r\n\"classname\" \"light\"\r\n\"name\" \"light_new\"\r\n\"origin\" \"1 2 3\"\r\n\"_color\" \"1 1 0\"\r\n\"light_radius\" \"64 64 64\"\r\n}\r\n";

	idStr text;
	text += header;
	text += world;
	text += group;
	text += named;
	text += unnamed;
	text += commented;
	text += memOnly;
	text += moved;
	text += brushLight;
	text += tieA;
	text += tieB;
	text += player;

	idStr want;
	want += header;
	want += world;
	want += group;
	want += namedOut;
	want += unnamedOut;
	want += commentedOut;
	want += memOnlyOut;
	want += moved;
	want += brushLight;
	want += tieA;
	want += tieBOut;
	want += player;
	want += appended;

	// one changed key, one new key and one deleted key per light, plus keys that already match
	idList<LightCommit> commits;
	LightCommit& cNamed = commits.Alloc();
	cNamed = SelfTestCommit( "light_named", false, false, idVec3( 10, 20, 30 ), -1 );
	cNamed.dict.Set( "origin", "10 20 30" );
	cNamed.dict.Set( "_color", "0.25 0.5 1" );
	cNamed.dict.Set( "light_radius", "" );
	cNamed.dict.Set( "light_center", "0 0 8" );

	LightCommit& cUnnamed = commits.Alloc();
	cUnnamed = SelfTestCommit( "light_7", false, true, idVec3( -100, 50, 64 ), 3 );
	cUnnamed.dict.Set( "name", "light_7" );
	cUnnamed.dict.Set( "origin", "-100 50 64" );
	cUnnamed.dict.Set( "_color", "0.1 0.2 0.3" );
	cUnnamed.dict.Set( "light_radius", "" );

	LightCommit& cCommented = commits.Alloc();
	cCommented = SelfTestCommit( "light_commented", false, false, idVec3( 256, 0, 128 ), -1 );
	cCommented.dict.Set( "origin", "256 0 128" );
	cCommented.dict.Set( "_color", "0 1 0" );
	cCommented.dict.Set( "noshadows", "" );
	cCommented.dict.Set( "nospecular", "1" );

	LightCommit& cMemOnly = commits.Alloc();
	cMemOnly = SelfTestCommit( "light_mem", false, false, idVec3( 300, 300, 300 ), -1 );
	cMemOnly.dict.Set( "_color", "0.5 0.5 1" );
	cMemOnly.dict.Set( "light_radius", "" );
	cMemOnly.dict.Set( "light_center", "0 0 16" );

	LightCommit& cMoved = commits.Alloc();
	cMoved = SelfTestCommit( "light_moved", false, false, idVec3( 0, 0, 0 ), -1 );
	cMoved.dict.Set( "_color", "0 1 1" );

	LightCommit& cBrush = commits.Alloc();
	cBrush = SelfTestCommit( "light_brush", false, false, idVec3( 0, 0, 0 ), -1 );
	cBrush.dict.Set( "_color", "0 1 1" );

	LightCommit& cTie = commits.Alloc();
	cTie = SelfTestCommit( "light_tie", false, false, idVec3( 700, 0, 0 ), 9 );
	cTie.dict.Set( "_color", "0 0 1" );

	LightCommit& cNew = commits.Alloc();
	cNew = SelfTestCommit( "light_new", true, false, vec3_zero, -1 );
	cNew.dict.Set( "origin", "1 2 3" );
	cNew.dict.Set( "_color", "1 1 0" );
	cNew.dict.Set( "light_radius", "64 64 64" );
	cNew.dict.Set( "light_center", "" );

	idStr out;
	idList<LightSpliceResult> results;
	const int spliced = LightMapSplice( text, commits, out, results );

	const bool same = ( out.Length() == want.Length() && memcmp( out.c_str(), want.c_str(), out.Length() ) == 0 );
	test.Check( same, "splice: byte-identical outside the touched blocks; values replaced in place, \"\" lines removed, new keys before '}', comment and other lines kept in order" );
	if( !same )
	{
		SelfTestShowMismatch( out, want );
	}
	test.Check( spliced == 6, "splice: six of the eight committed lights written" );
	test.Check( results[ 0 ].spliced, "splice: named light located by name and origin" );
	test.Check( results[ 1 ].spliced && results[ 1 ].origin.Compare( idVec3( -100, 50, 64 ) ), "splice: unnamed light found by classname and origin, name written" );
	test.Check( results[ 2 ].spliced && out.Find( "\"origin\" \"256 0 128\"\r\n\"_color\" \"0 1 0\"" ) >= 0, "splice: comment lines kept (one with a quote and a brace), matching-value line untouched" );
	test.Check( results[ 3 ].spliced, "splice: a name in no block with an existing locator falls to the origin match" );
	test.Check( !results[ 4 ].spliced, "splice: named light whose origin moved in the file is skipped with a warning" );
	test.Check( !results[ 5 ].spliced, "splice: block with a nested '{' (brush entity) refused" );
	test.Check( results[ 6 ].spliced, "splice: two candidate blocks, the in-memory index breaks the tie" );
	test.Check( results[ 7 ].spliced && out.Find( appended ) == out.Length() - idStr::Length( appended ), "splice: new light appended as one block at the end" );

	// a second flush of the same commits after step 6 changes nothing
	for( int i = 0; i < commits.Num(); i++ )
	{
		if( results[ i ].spliced )
		{
			commits[ i ].isNew = false;
			commits[ i ].onDiskOrigin = results[ i ].origin;
		}
	}
	idStr again;
	idList<LightSpliceResult> resultsAgain;
	LightMapSplice( out, commits, again, resultsAgain );
	test.Check( again.Length() == out.Length() && memcmp( again.c_str(), out.c_str(), out.Length() ) == 0, "splice: second flush with no change is byte-identical" );

	// D49: Enter writes one light; every other pending light in the list stays as it is
	idStr filteredWant = text;
	filteredWant.Replace( named, namedOut );
	idStr filtered;
	idList<LightSpliceResult> filteredResults;
	const int filteredCount = LightMapSplice( text, commits, filtered, filteredResults, "light_named" );
	bool othersUntouched = true;
	for( int i = 1; i < filteredResults.Num(); i++ )
	{
		if( filteredResults[ i ].spliced )
		{
			othersUntouched = false;
		}
	}
	test.Check( filteredCount == 1 && filteredResults[ 0 ].spliced && othersUntouched && filtered.Cmp( filteredWant ) == 0,
				"splice: a filter on one of several pending lights changes exactly that block" );

	// LF files get LF lines
	idList<LightCommit> lfCommits;
	LightCommit& cLf = lfCommits.Alloc();
	cLf = SelfTestCommit( "a", false, false, vec3_zero, -1 );
	cLf.dict.Set( "k", "v" );
	idStr lfOut;
	LightMapSplice( "{\n\"classname\" \"light\"\n\"name\" \"a\"\n}\n", lfCommits, lfOut, results );
	test.Check( lfOut.Cmp( "{\n\"classname\" \"light\"\n\"name\" \"a\"\n\"k\" \"v\"\n}\n" ) == 0, "splice: inserted lines use the file's line ending" );

	test.Check( LightMapSplice( "{\r\n\"classname\" \"light\"\r\n", lfCommits, lfOut, results ) == -1, "splice: unbalanced text refused" );

	// MA-7 / ST-6: a key whose value is on the next line marks the block, which is refused
	const idStr brokenText = "{\n\"classname\" \"light\"\n\"name\" \"a\"\n\"k\"\n\"old\"\n}\n";
	test.Check( LightMapSplice( brokenText, lfCommits, lfOut, results ) == 0 && !results[ 0 ].spliced && lfOut.Cmp( brokenText ) == 0,
				"splice: a block with a key whose value is not on its line is refused" );

	// an empty file has nothing to locate: a new light is warned about, never made the whole file
	idList<LightCommit> newCommits;
	LightCommit& cNewOnly = newCommits.Alloc();
	cNewOnly = SelfTestCommit( "light_new", true, false, vec3_zero, -1 );
	cNewOnly.dict.Set( "origin", "1 2 3" );
	test.Check( LightMapSplice( "", newCommits, lfOut, results ) == 0 && !results[ 0 ].spliced && lfOut.IsEmpty(), "splice: an empty file gets nothing, the new light is warned about" );
}

static bool SelfTestFinite( const idVec3& v )
{
	for( int i = 0; i < 3; i++ )
	{
		if( v[ i ] != v[ i ] || idMath::Fabs( v[ i ] ) > 1e30f )
		{
			return false;
		}
	}
	return true;
}

static bool SelfTestNear( float a, float b, float epsilon )
{
	return idMath::Fabs( a - b ) <= epsilon;
}

// finite and inside the world bounds
static bool SelfTestInWorld( const idVec3& v )
{
	const float limit = ( float )MAX_WORLD_COORD;
	return SelfTestFinite( v ) && idMath::Fabs( v.x ) < limit && idMath::Fabs( v.y ) < limit && idMath::Fabs( v.z ) < limit;
}

// world centre of a spot's bounds with the renderer's depth rules (absent start = ( 0, 0, 0 ),
// absent end = the target, zNear >= 8, zFar >= 16)
static idVec3 SelfTestSpotCenter( const LightInfo& light, float* zNearOut, float* zFarOut )
{
	const idVec3 n = light.lightTarget / light.lightTarget.Length();
	const idVec3 start = light.explicitStartEnd ? light.lightStart : vec3_zero;
	const idVec3 end = light.explicitStartEnd ? light.lightEnd : light.lightTarget;
	const float zNear = Max( start * n, 8.0f );
	const float zFar = Max( end * n, 16.0f );
	if( zNearOut )
	{
		*zNearOut = zNear;
	}
	if( zFarOut )
	{
		*zFarOut = zFar;
	}
	return light.origin + SpotBoundsCenter( light.angles.ToMat3(), light.lightTarget, light.lightRight, light.lightUp, zNear, zFar );
}

static bool SelfTestSamePose( const LightInfo& a, const LightInfo& b )
{
	LightPose pa;
	LightPose pb;
	pa.Capture( a );
	pb.Capture( b );
	return memcmp( &pa, &pb, sizeof( pa ) ) == 0;
}

// WP-LE2: AUTO and the Q, E, R modals on synthetic lights (the cvar defaults as sensitivities)
static void SelfTestGeometry( LightSelfTest& test )
{
	const float scaleSens = 0.004f;
	const float wheelSens = 0.10f;
	const float rotSens = 0.25f;
	const float rotWheelSens = 5.0f;

	// ---- spot AUTO: Doom's default spot (left-handed), turned 30 degrees
	LightInfo spot;
	spot.DefaultProjected();
	spot.origin.Set( 100.0f, 200.0f, 50.0f );
	spot.angles.Set( 0.0f, 30.0f, 0.0f );
	const idMat3 spotAxis = spot.angles.ToMat3();

	LightAutoBase spotBase;
	AutoCaptureBase( spot, spotBase );

	LightInfo spotAuto = spot;
	const idVec3 spotAim = spot.origin + idVec3( 300.0f, 100.0f, -50.0f );
	const float spotL = ( spotAim - spot.origin ).Length();
	const bool spotMoved = AutoAimSpot( spotAuto, spotBase, spotAim );
	const idVec3 sn = spotAuto.lightTarget / spotAuto.lightTarget.Length();

	test.Check( spotMoved && SelfTestNear( spotAuto.lightTarget.Length(), spotL, 1e-2f ), "spot AUTO: |target| = L" );
	test.Check( ( spot.origin + spotAxis * spotAuto.lightTarget ).Compare( spotAim, 1e-2f ), "spot AUTO: the far plane centre is the aim point" );
	test.Check( SelfTestNear( spotAuto.lightRight.Length(), spotBase.farHalfWidth, 1e-3f ) && SelfTestNear( spotAuto.lightUp.Length(), spotBase.farHalfHeight, 1e-3f )
				&& SelfTestNear( spotBase.farHalfWidth, 128.0f, 1e-3f ), "spot AUTO: far half sizes W0 / H0 kept" );
	test.Check( idMath::Fabs( spotAuto.lightRight * sn ) < 1e-3f && idMath::Fabs( spotAuto.lightUp * sn ) < 1e-3f, "spot AUTO: r and u perpendicular to n" );
	const float spotSide = ( spotAuto.lightRight.Cross( spotAuto.lightUp ) * spotAuto.lightTarget > 0.0f ) ? 1.0f : -1.0f;
	test.Check( spotBase.handedness == -1.0f && spotSide == spotBase.handedness, "spot AUTO: handedness preserved" );

	// MA-2 (a): r x n is always left-handed, so a right-handed spot must have its up flipped back
	LightInfo spotRightHanded = spot;
	spotRightHanded.lightUp = -spot.lightUp;
	LightAutoBase spotRightHandedBase;
	AutoCaptureBase( spotRightHanded, spotRightHandedBase );
	AutoAimSpot( spotRightHanded, spotRightHandedBase, spotAim );
	const float rightHandedSide = ( spotRightHanded.lightRight.Cross( spotRightHanded.lightUp ) * spotRightHanded.lightTarget > 0.0f ) ? 1.0f : -1.0f;
	test.Check( spotRightHandedBase.handedness == 1.0f && rightHandedSide == 1.0f, "spot AUTO: a right-handed spot (up negated) keeps h0 = +1" );
	test.Check( spotAuto.origin == spot.origin && spotAuto.angles == spot.angles, "spot AUTO: apex and axis unchanged" );

	LightInfo spotUp = spot;
	LightAutoBase spotUpBase;
	AutoCaptureBase( spot, spotUpBase );
	AutoAimSpot( spotUp, spotUpBase, spot.origin + idVec3( 0.0f, 0.0f, 500.0f ) );
	LightInfo spotDown = spot;
	AutoAimSpot( spotDown, spotUpBase, spot.origin + idVec3( 0.0f, 0.0f, -500.0f ) );
	test.Check( SelfTestFinite( spotUp.lightRight ) && SelfTestFinite( spotUp.lightUp ) && SelfTestFinite( spotDown.lightRight ) && SelfTestFinite( spotDown.lightUp )
				&& SelfTestNear( spotUp.lightRight.Length(), 128.0f, 1e-2f ) && SelfTestNear( spotDown.lightRight.Length(), 128.0f, 1e-2f ),
				"spot AUTO: straight up and straight down give no NaN" );

	LightInfo spotExplicit = spot;
	spotExplicit.explicitStartEnd = true;
	spotExplicit.lightStart.Set( 0.0f, 0.0f, -32.0f );
	spotExplicit.lightEnd = spotExplicit.lightTarget;
	LightAutoBase spotExplicitBase;
	AutoCaptureBase( spotExplicit, spotExplicitBase );
	AutoAimSpot( spotExplicit, spotExplicitBase, spotAim );
	const idVec3 sen = spotExplicit.lightTarget / spotExplicit.lightTarget.Length();
	test.Check( spotExplicit.lightStart.Compare( sen * 32.0f, 1e-3f ) && spotExplicit.lightEnd == spotExplicit.lightTarget, "spot AUTO: explicit start = n * min( zNear0, L / 2 ), end = target" );

	// ---- point lights hold in AUTO (D46): the dispatch RunFrame uses writes nothing for them
	LightInfo point;
	point.DefaultPoint();
	point.origin.Set( -50.0f, 20.0f, 100.0f );
	LightAutoBase pointBase;
	AutoCaptureBase( point, pointBase );

	LightInfo pointHeld = point;
	const idVec3 pointAim = point.origin + idVec3( 200.0f, -300.0f, 120.0f );
	const idVec3 someView( 0.0f, 1.0f, 0.0f );
	const bool pointWritten = AutoAimLight( pointHeld, pointBase, pointAim, &someView );
	LightInfo spotDispatched = spot;
	LightAutoBase spotDispatchedBase;
	AutoCaptureBase( spot, spotDispatchedBase );
	const bool spotWritten = AutoAimLight( spotDispatched, spotDispatchedBase, spotAim, &someView );
	test.Check( !pointWritten && SelfTestSamePose( pointHeld, point ) && spotWritten && SelfTestSamePose( spotDispatched, spotAuto ),
				"AUTO: a point light holds (D46), a spot still re-aims" );

	// R expects a frame the user has aimed: A[2] on the aim, A[0] horizontal to the viewer's left,
	// right-handed; built here, since AUTO no longer aims point lights
	LightInfo pointAimed = point;
	idVec3 pn = pointAim - point.origin;
	pn.Normalize();
	idMat3 aimFrame;
	aimFrame[2] = pn;
	aimFrame[0] = worldUp.Cross( pn );
	aimFrame[0].Normalize();
	aimFrame[1] = aimFrame[2].Cross( aimFrame[0] );
	pointAimed.angles = aimFrame.ToAngles();
	const idMat3 pa = pointAimed.angles.ToMat3();

	// R4-3: at modal entry a sphere turns its frame to the aim, a shaped point light keeps its own
	LightInfo sphereEntry = point;
	LightAutoBase sphereBase;
	AutoCaptureBase( point, sphereBase );
	const bool sphereAligned = ModalAlignSphere( sphereEntry, sphereBase, pointAim );
	const idMat3 sa = sphereEntry.angles.ToMat3();
	test.Check( sphereAligned && sa[2].Compare( pn, 1e-3f ) && idMath::Fabs( sa[0].z ) < 1e-3f && sa[0].Cross( sa[1] ) * sa[2] > 0.999f,
				"modal entry: a point-light sphere gets axis[2] on the aim, A[0] horizontal, right-handed" );

	LightInfo hotdog = point;
	hotdog.lightRadius.Set( 300.0f, 300.0f, 600.0f );
	hotdog.equalRadius = false;
	hotdog.angles.Set( 10.0f, 40.0f, 0.0f );
	LightInfo hotdogEntry = hotdog;
	const bool hotdogAligned = ModalAlignSphere( hotdogEntry, sphereBase, pointAim );
	test.Check( !hotdogAligned && hotdogEntry.angles == hotdog.angles, "modal entry: a shaped point light (unequal radii) keeps its angles" );

	// ---- sun AUTO
	LightInfo sun;
	sun.DefaultSun();
	LightAutoBase sunBase;
	AutoCaptureBase( sun, sunBase );
	LightInfo sunAuto = sun;
	idVec3 view( 1.0f, 2.0f, -0.5f );
	view.Normalize();
	AutoAimSun( sunAuto, sunBase, view );
	test.Check( sunAuto.lightCenter.Compare( -view * sun.lightCenter.Length(), 1e-3f ) && sunAuto.hasCenter && sunAuto.lightRadius == sun.lightRadius,
				"sun AUTO: lightCenter = -view * |lc0| in world space, box unchanged" );

	// MA-2 (b): a sun without light_center still gets a direction
	LightInfo sunNoCenter = sun;
	sunNoCenter.lightCenter.Zero();
	sunNoCenter.hasCenter = false;
	LightAutoBase sunNoCenterBase;
	AutoCaptureBase( sunNoCenter, sunNoCenterBase );
	AutoAimSun( sunNoCenter, sunNoCenterBase, view );
	test.Check( sunNoCenter.lightCenter.Compare( -view, 1e-4f ) && sunNoCenter.hasCenter, "sun AUTO: a sun without light_center gets |lightCenter| = 1 along -view" );

	// ---- Q
	float zNearBefore, zFarBefore, zNearNeutral, zFarNeutral;
	const idVec3 centerBefore = SelfTestSpotCenter( spot, &zNearBefore, &zFarBefore );

	LightInfo spotNeutral = spot;
	ModalScale( spotNeutral, spot, 0.0f, scaleSens );
	const idVec3 centerNeutral = SelfTestSpotCenter( spotNeutral, &zNearNeutral, &zFarNeutral );
	test.Check( spotNeutral.explicitStartEnd && centerNeutral.Compare( centerBefore, 1e-3f ) && SelfTestNear( zNearNeutral, zNearBefore, 1e-3f ) && SelfTestNear( zFarNeutral, zFarBefore, 1e-3f ),
				"Q spot: implicit start/end made explicit render-neutrally" );

	LightInfo spotScaled = spot;
	const float s = idMath::Exp( 100.0f * scaleSens );
	ModalScale( spotScaled, spot, 100.0f, scaleSens );
	const idVec3 n0 = spot.lightTarget / spot.lightTarget.Length();
	test.Check( ( SelfTestSpotCenter( spotScaled, NULL, NULL ) - centerBefore ).Length() < 0.01f, "Q spot: world bounds centre invariant to 0.01" );

	// a frustum not aligned with its light's axes, on a pitched and rolled light: a local-space
	// bounds centre would drift here, the world one does not
	LightInfo spotTilted = spotAuto;
	spotTilted.angles.Set( 20.0f, 30.0f, 10.0f );
	const idVec3 tiltedBefore = SelfTestSpotCenter( spotTilted, NULL, NULL );
	LightInfo tiltedScaled = spotTilted;
	ModalScale( tiltedScaled, spotTilted, 150.0f, scaleSens );
	test.Check( ( SelfTestSpotCenter( tiltedScaled, NULL, NULL ) - tiltedBefore ).Length() < 0.01f, "Q spot: centre invariant for a skewed frustum on a pitched and rolled light" );
	test.Check( spotScaled.lightTarget.Compare( spot.lightTarget * s, 1e-3f ) && spotScaled.lightRight.Compare( spot.lightRight * s, 1e-3f ) && spotScaled.lightUp.Compare( spot.lightUp * s, 1e-3f )
				&& spotScaled.lightStart.Compare( n0 * 8.0f * s, 1e-3f ) && spotScaled.lightEnd.Compare( spot.lightTarget * s, 1e-3f ), "Q spot: the five vectors scaled by s" );

	LightInfo pointScaled = point;
	ModalScale( pointScaled, point, 100.0f, scaleSens );
	test.Check( pointScaled.lightRadius.Compare( point.lightRadius * s, 1e-3f ) && pointScaled.origin == point.origin, "Q point: radius r0 * s, origin fixed" );

	// MA-1: mouse left shrinks an implicit-start spot; the near plane stays pinned at the renderer's 8
	LightInfo spotShrunk = spot;
	const float sLeft = idMath::Exp( -100.0f * scaleSens );
	ModalScale( spotShrunk, spot, -100.0f, scaleSens );
	test.Check( spotShrunk.lightTarget.Compare( spot.lightTarget * sLeft, 1e-3f ) && spotShrunk.lightStart.Compare( n0 * 8.0f, 1e-3f )
				&& ( SelfTestSpotCenter( spotShrunk, NULL, NULL ) - centerBefore ).Length() < 0.01f, "Q spot: mouse left (s < 1) shrinks an implicit-start spot, near plane at 8, centre kept" );

	// an explicit start at depth 2 (the renderer reads 8) on the skewed, tilted spot: entering Q
	// renders the identical light, and scaling down keeps the world centre
	LightInfo spotShallow = spotTilted;
	const idVec3 nt = spotTilted.lightTarget / spotTilted.lightTarget.Length();
	spotShallow.explicitStartEnd = true;
	spotShallow.lightStart = nt * 2.0f;
	spotShallow.lightEnd = spotShallow.lightTarget;
	float zNearShallow, zFarShallow, zNearEntry, zFarEntry;
	const idVec3 shallowBefore = SelfTestSpotCenter( spotShallow, &zNearShallow, &zFarShallow );
	LightInfo shallowEntry = spotShallow;
	ModalScale( shallowEntry, spotShallow, 0.0f, scaleSens );
	const idVec3 shallowAtEntry = SelfTestSpotCenter( shallowEntry, &zNearEntry, &zFarEntry );
	test.Check( shallowEntry.origin.Compare( spotShallow.origin, 1e-4f ) && shallowEntry.lightTarget.Compare( spotShallow.lightTarget, 1e-4f )
				&& shallowEntry.lightRight.Compare( spotShallow.lightRight, 1e-4f ) && shallowEntry.lightUp.Compare( spotShallow.lightUp, 1e-4f )
				&& SelfTestNear( zNearEntry, zNearShallow, 1e-3f ) && SelfTestNear( zFarEntry, zFarShallow, 1e-3f ) && shallowAtEntry.Compare( shallowBefore, 1e-3f ),
				"Q spot: explicit start at depth 2, entering Q (s = 1) renders the identical light" );
	LightInfo shallowScaled = spotShallow;
	ModalScale( shallowScaled, spotShallow, -80.0f, scaleSens );
	test.Check( ( SelfTestSpotCenter( shallowScaled, NULL, NULL ) - shallowBefore ).Length() < 0.01f && shallowScaled.lightTarget.Length() < spotShallow.lightTarget.Length(),
				"Q spot: explicit start at depth 2, scaling down keeps the world centre" );

	// VF-1: a far plane the renderer already holds at 16 (end depth 10, start depth 9.5): entering
	// Q (s = 1) or E (no counts) renders the identical light
	LightInfo spotShort = spot;
	spotShort.explicitStartEnd = true;
	spotShort.lightStart = n0 * 9.5f;
	spotShort.lightEnd = n0 * 10.0f;
	float zNearShort, zFarShort, zNearQ, zFarQ;
	const idVec3 shortBefore = SelfTestSpotCenter( spotShort, &zNearShort, &zFarShort );
	LightInfo shortQ = spotShort;
	ModalScale( shortQ, spotShort, 0.0f, scaleSens );
	const idVec3 shortAfterQ = SelfTestSpotCenter( shortQ, &zNearQ, &zFarQ );
	LightInfo shortE = spotShort;
	ModalStretch( shortE, spotShort, 0.0f, 0.0f, 0.0f, scaleSens, wheelSens );
	test.Check( shortQ.origin.Compare( spotShort.origin, 1e-4f ) && shortQ.lightTarget.Compare( spotShort.lightTarget, 1e-4f ) && shortQ.lightEnd.Compare( spotShort.lightEnd, 1e-4f )
				&& shortQ.lightStart.Compare( spotShort.lightStart, 1e-4f ) && shortAfterQ.Compare( shortBefore, 1e-3f ) && SelfTestNear( zNearQ, zNearShort, 1e-3f )
				&& SelfTestNear( zFarQ, zFarShort, 1e-3f ) && SelfTestSamePose( shortE, spotShort ), "Q and E: an explicit spot with end depth 10 renders the identical light on entry" );

	// ---- E
	const float sx = idMath::Exp( 50.0f * scaleSens );
	const float sy = idMath::Exp( 30.0f * scaleSens );
	const float sz = idMath::Exp( 3.0f * wheelSens );

	LightInfo spotStretched = spot;
	ModalStretch( spotStretched, spot, 50.0f, -30.0f, 3.0f, scaleSens, wheelSens );
	test.Check( spotStretched.lightRight.Compare( spot.lightRight * sx, 1e-3f ) && spotStretched.lightUp.Compare( spot.lightUp * sy, 1e-3f )
				&& spotStretched.lightTarget.Compare( spot.lightTarget * sz, 1e-3f ) && spotStretched.origin == spot.origin, "E spot: per-axis factors, apex fixed" );

	LightInfo pointTurned = point;
	pointTurned.angles.Set( 10.0f, 40.0f, 0.0f );
	const idMat3 pointTurnedAxis = pointTurned.angles.ToMat3();
	LightInfo pointStretched = pointTurned;
	ModalStretch( pointStretched, pointTurned, 50.0f, -30.0f, 3.0f, scaleSens, wheelSens );
	const idVec3 nearFaceBefore = pointTurned.origin - pointTurnedAxis[2] * pointTurned.lightRadius.z;
	const idVec3 nearFaceAfter = pointStretched.origin - pointTurnedAxis[2] * pointStretched.lightRadius.z;
	test.Check( pointStretched.lightRadius.Compare( idVec3( 300.0f * sx, 300.0f * sy, 300.0f * sz ), 1e-2f ) && !pointStretched.equalRadius
				&& pointStretched.origin.Compare( pointTurned.origin + pointTurnedAxis[2] * ( 300.0f * ( sz - 1.0f ) ), 1e-2f ) && nearFaceAfter.Compare( nearFaceBefore, 1e-2f ),
				"E point: per-axis factors, origin moved r0.z * ( sz - 1 ) along the aim, -Z face fixed" );

	LightInfo sunStretched = sun;
	ModalStretch( sunStretched, sun, 0.0f, 0.0f, 3.0f, scaleSens, wheelSens );
	test.Check( sunStretched.origin.Compare( sun.origin + sun.angles.ToMat3()[2] * ( sun.lightRadius.z * ( sz - 1.0f ) ), 1e-2f ), "E sun: origin moved along the aim like a point light" );

	// MA-2 (c): an explicit spot (start = n * 32, end = target after AUTO)
	LightInfo explicitStretched = spotExplicit;
	ModalStretch( explicitStretched, spotExplicit, 0.0f, 0.0f, 3.0f, scaleSens, wheelSens );
	test.Check( explicitStretched.lightStart == spotExplicit.lightStart && explicitStretched.lightEnd.Compare( spotExplicit.lightEnd * sz, 1e-3f ), "E explicit spot: start kept, end = end0 * sz" );

	// MA-4: wheel far down stops the far plane one unit beyond the near one
	LightInfo explicitSquashed = spotExplicit;
	ModalStretch( explicitSquashed, spotExplicit, 0.0f, 0.0f, -50.0f, scaleSens, wheelSens );
	const idVec3 explicitN = spotExplicit.lightTarget / spotExplicit.lightTarget.Length();
	test.Check( SelfTestNear( explicitSquashed.lightEnd * explicitN, Max( spotExplicit.lightStart * explicitN, 8.0f ) + 1.0f, 1e-2f ), "E explicit spot: wheel down keeps end one unit beyond max( start, 8 )" );

	LightInfo explicitRotated = spotExplicit;
	ModalRotate( explicitRotated, spotExplicit, 60.0f, -40.0f, 1.0f, rotSens, rotWheelSens );
	idVec3 rotatedAim = explicitRotated.lightTarget;
	rotatedAim.Normalize();
	test.Check( explicitRotated.lightStart.Compare( rotatedAim * spotExplicit.lightStart.Length(), 1e-2f ) && explicitRotated.lightEnd.Compare( explicitRotated.lightTarget, 1e-2f )
				&& !explicitRotated.lightTarget.Compare( spotExplicit.lightTarget, 1.0f ), "R explicit spot: start and end turn with the target" );

	// EB-10: a runaway mouse saturates at the clamp, finite and inside the world
	LightInfo pointHuge = point;
	ModalScale( pointHuge, point, 1e6f, scaleSens );
	LightInfo spotHuge = spot;
	ModalScale( spotHuge, spot, 1e6f, scaleSens );
	LightInfo stretchHuge = pointTurned;
	ModalStretch( stretchHuge, pointTurned, 1e6f, -1e6f, 1e6f, scaleSens, wheelSens );
	test.Check( SelfTestInWorld( pointHuge.lightRadius ) && SelfTestInWorld( spotHuge.origin ) && SelfTestInWorld( spotHuge.lightTarget ) && SelfTestInWorld( spotHuge.lightRight )
				&& SelfTestInWorld( spotHuge.lightUp ) && SelfTestInWorld( spotHuge.lightStart ) && SelfTestInWorld( spotHuge.lightEnd )
				&& SelfTestInWorld( stretchHuge.lightRadius ) && SelfTestInWorld( stretchHuge.origin )
				&& pointHuge.lightRadius.Compare( point.lightRadius * 100.0f, 1e-2f ) && spotHuge.lightTarget.Compare( spot.lightTarget * 100.0f, 1e-2f )
				&& stretchHuge.lightRadius.Compare( pointTurned.lightRadius * 100.0f, 1e-2f ), "Q and E: a million counts give finite values inside the world, factor = the clamp (100)" );

	// ---- R, on the aimed point and the AUTO-aligned spot from above
	LightInfo pointRolled = pointAimed;
	ModalRotate( pointRolled, pointAimed, 90.0f / rotSens, 0.0f, 0.0f, rotSens, rotWheelSens );
	test.Check( pointRolled.angles.ToMat3()[0].Compare( pa[1], 1e-3f ), "R point: 90 degree mouse-right roll gives A[0]' = +A[1] (clockwise seen from behind)" );

	LightInfo spotRolled = spotAuto;
	ModalRotate( spotRolled, spotAuto, 90.0f / rotSens, 0.0f, 0.0f, rotSens, rotWheelSens );
	idVec3 rolledRight = spotRolled.lightRight;
	rolledRight.Normalize();
	idVec3 baseUp = spotAuto.lightUp;
	baseUp.Normalize();
	test.Check( rolledRight.Compare( -baseUp, 1e-3f ), "R spot: 90 degree mouse-right roll gives r' = -u" );

	LightInfo pointPitched = pointAimed;
	ModalRotate( pointPitched, pointAimed, 0.0f, -10.0f / rotSens, 0.0f, rotSens, rotWheelSens );
	const idVec3 spotUpLocal = spotAxis.Transpose() * worldUp;
	LightInfo spotPitched = spotAuto;
	ModalRotate( spotPitched, spotAuto, 0.0f, -10.0f / rotSens, 0.0f, rotSens, rotWheelSens );
	idVec3 pitchedAim = spotPitched.lightTarget;
	pitchedAim.Normalize();
	test.Check( pointPitched.angles.ToMat3()[2] * worldUp > pa[2] * worldUp && pitchedAim * spotUpLocal > sn * spotUpLocal, "R: 10 degree mouse-up pitch raises the aim (point and spot)" );

	LightInfo pointYawed = pointAimed;
	ModalRotate( pointYawed, pointAimed, 0.0f, 0.0f, 10.0f / rotWheelSens, rotSens, rotWheelSens );
	LightInfo spotYawed = spotAuto;
	ModalRotate( spotYawed, spotAuto, 0.0f, 0.0f, 10.0f / rotWheelSens, rotSens, rotWheelSens );
	idVec3 yawedAim = spotYawed.lightTarget;
	yawedAim.Normalize();
	idVec3 spotRight = spotAuto.lightRight;
	spotRight.Normalize();
	test.Check( pointYawed.angles.ToMat3()[2] * -pa[0] < pa[2] * -pa[0] && yawedAim * spotRight < sn * spotRight, "R: 10 degree wheel-up yaw turns the aim to the viewer's left (point and spot)" );

	// ---- D17: results depend only on modalBase and the counts (an earlier frame's counts leave no
	// trace); cancel is the assignment cur = modalBase, exact by construction
	LightInfo twice = spot;
	ModalScale( twice, spot, 40.0f, scaleSens );
	ModalScale( twice, spot, 100.0f, scaleSens );
	LightInfo pointTwice = pointAimed;
	ModalRotate( pointTwice, pointAimed, 30.0f, 20.0f, 0.0f, rotSens, rotWheelSens );
	ModalRotate( pointTwice, pointAimed, 120.0f, -60.0f, 1.0f, rotSens, rotWheelSens );
	LightInfo pointOnce = pointAimed;
	ModalRotate( pointOnce, pointAimed, 120.0f, -60.0f, 1.0f, rotSens, rotWheelSens );
	test.Check( SelfTestSamePose( twice, spotScaled ) && SelfTestSamePose( pointTwice, pointOnce ), "modals: Q and R, no per-frame integration" );

	LightInfo stretchTwice = pointTurned;
	ModalStretch( stretchTwice, pointTurned, 20.0f, 10.0f, 1.0f, scaleSens, wheelSens );
	ModalStretch( stretchTwice, pointTurned, 50.0f, -30.0f, 3.0f, scaleSens, wheelSens );
	LightInfo spotStretchTwice = spotExplicit;
	ModalStretch( spotStretchTwice, spotExplicit, -20.0f, 15.0f, -2.0f, scaleSens, wheelSens );
	ModalStretch( spotStretchTwice, spotExplicit, 0.0f, 0.0f, 3.0f, scaleSens, wheelSens );
	test.Check( SelfTestSamePose( stretchTwice, pointStretched ) && SelfTestSamePose( spotStretchTwice, explicitStretched ), "modals: E, no per-frame integration (point and explicit spot)" );
}

// hue, saturation and max( r, g, b ) of a colour, through the same pure ImGui converter
static void SelfTestHsv( const idVec3& color, float& h, float& s, float& intensity )
{
	intensity = Max3( color.x, color.y, color.z );
	float v;
	ImGui::ColorConvertRGBtoHSV( color.x / intensity, color.y / intensity, color.z / intensity, h, s, v );
}

// WP-LE3: the T modal
static void SelfTestColor( LightSelfTest& test )
{
	const float hueSens = 0.0015f;
	const float intSens = 0.004f;
	const float maxIntensity = 8.0f;
	float h, s, intensity;

	LightInfo red;
	red.color.Set( 1.0f, 0.0f, 0.0f );

	LightInfo forward = red;
	ModalColor( forward, red, 1.25f / hueSens, 0.0f, hueSens, intSens, maxIntensity );
	SelfTestHsv( forward.color, h, s, intensity );
	test.Check( SelfTestNear( h, 0.25f, 1e-3f ) && SelfTestNear( s, 1.0f, 1e-3f ), "T: hue wraps forward through 1.0" );

	LightInfo backward = red;
	ModalColor( backward, red, -0.25f / hueSens, 0.0f, hueSens, intSens, maxIntensity );
	SelfTestHsv( backward.color, h, s, intensity );
	test.Check( SelfTestNear( h, 0.75f, 1e-3f ), "T: hue wraps backward through 0" );

	LightInfo bright = red;
	ModalColor( bright, red, 0.0f, -100000.0f, hueSens, intSens, maxIntensity );
	LightInfo dim = red;
	ModalColor( dim, red, 0.0f, 100000.0f, hueSens, intSens, maxIntensity );
	LightInfo up = red;
	ModalColor( up, red, 0.0f, -100.0f, hueSens, intSens, maxIntensity );
	test.Check( SelfTestNear( Max3( bright.color.x, bright.color.y, bright.color.z ), maxIntensity, 1e-3f ) && SelfTestNear( Max3( dim.color.x, dim.color.y, dim.color.z ), 0.01f, 1e-5f )
				&& SelfTestNear( up.color.x, idMath::Exp( 100.0f * intSens ), 1e-3f ), "T: mouse up brightens, intensity clamps to [ 0.01, le_maxIntensity ]" );

	LightInfo white;
	white.color.Set( 1.0f, 1.0f, 1.0f );
	LightInfo gold = white;
	ModalColor( gold, white, 0.0f, 0.0f, hueSens, intSens, maxIntensity );
	SelfTestHsv( gold.color, h, s, intensity );
	test.Check( SelfTestNear( h, 0.125f, 1e-3f ) && SelfTestNear( s, 0.25f, 1e-3f ) && SelfTestNear( intensity, 1.0f, 1e-4f ), "T: a white light starts as gold, hue 0.125 / saturation 0.25" );

	LightInfo goldWalked = white;
	ModalColor( goldWalked, white, 300.0f, 0.0f, hueSens, intSens, maxIntensity );
	SelfTestHsv( goldWalked.color, h, s, intensity );
	test.Check( SelfTestNear( h, 0.125f + 300.0f * hueSens, 1e-3f ) && SelfTestNear( s, 0.25f, 1e-3f ), "T: the white light keeps saturation 0.25 while the hue walks" );

	LightInfo colored;
	colored.color.Set( 2.0f, 1.0f, 0.5f );
	float h0, s0, i0;
	SelfTestHsv( colored.color, h0, s0, i0 );
	LightInfo coloredWalked = colored;
	ModalColor( coloredWalked, colored, 200.0f, 0.0f, hueSens, intSens, maxIntensity );
	SelfTestHsv( coloredWalked.color, h, s, intensity );
	test.Check( SelfTestNear( s, s0, 1e-3f ) && SelfTestNear( intensity, i0, 1e-3f ) && SelfTestNear( h, idMath::Frac( h0 + 200.0f * hueSens ), 1e-3f ),
				"T: a coloured light keeps its own saturation and intensity" );

	// MA-2 (d): the intensity is the largest channel, whichever it is
	LightInfo blue;
	blue.color.Set( 0.2f, 0.2f, 1.0f );
	LightInfo blueEntry = blue;
	ModalColor( blueEntry, blue, 0.0f, 0.0f, hueSens, intSens, maxIntensity );
	test.Check( SelfTestNear( Max3( blueEntry.color.x, blueEntry.color.y, blueEntry.color.z ), 1.0f, 1e-4f ) && blueEntry.color.Compare( blue.color, 1e-3f ),
				"T: a light whose largest channel is blue keeps max = 1 at entry" );

	// D17: an earlier frame's counts leave no trace
	LightInfo colorTwice = colored;
	ModalColor( colorTwice, colored, 80.0f, 20.0f, hueSens, intSens, maxIntensity );
	ModalColor( colorTwice, colored, 200.0f, 0.0f, hueSens, intSens, maxIntensity );
	test.Check( colorTwice.color == coloredWalked.color, "T: no per-frame integration" );
}

// a ToDict dict merged onto a light as idGameEdit::EntityChangeSpawnArgs merges it ("" deletes the key)
static void SelfTestMerge( idDict& light, const idDict& d )
{
	for( int i = 0; i < d.GetNumKeyVals(); i++ )
	{
		const idKeyValue* kv = d.GetKeyVal( i );
		if( kv->GetValue().IsEmpty() )
		{
			light.Delete( kv->GetKey() );
		}
		else
		{
			light.Set( kv->GetKey(), kv->GetValue() );
		}
	}
}

static bool SelfTestDeletes( const idDict& written, const char* key )
{
	return written.FindKey( key ) != NULL && written.GetString( key )[ 0 ] == '\0';
}

// parse, write, merge and parse again: the second parse gives the first; rotations compared as matrices
static bool SelfTestRoundTrip( const idDict& spawnArgs, LightInfo& first, LightInfo& second, idDict& written )
{
	first.FromDict( &spawnArgs );
	written.Clear();
	first.ToDict( &written );
	idDict merged = spawnArgs;
	SelfTestMerge( merged, written );
	second.FromDict( &merged );

	return memcmp( &first.emitter, &second.emitter, sizeof( first.emitter ) ) == 0 && first.lightType == second.lightType
		   && first.autoRange == second.autoRange && first.lightRadius.Compare( second.lightRadius, 1e-2f )
		   && first.angles.ToMat3().Compare( second.angles.ToMat3(), 1e-4f );
}

// the emitter keys of each shape, a texture light and the Texture tab
static void SelfTestEmitterKeys( LightSelfTest& test )
{
	LightInfo first;
	LightInfo second;
	idDict written;

	idDict omni;
	omni.Set( "classname", "light" );
	omni.Set( "emitter", "1" );
	omni.Set( "emit_shape", "omni" );
	omni.Set( "light_radius", "288 144 288" );
	omni.Set( "light_center", "0 0 16" );
	omni.Set( "emit_falloff", "exponential" );
	omni.Set( "emit_size", "64" );
	omni.Set( "emit_start", "8" );
	omni.Set( "emit_effect", "candle_a" );
	omni.Set( "emit_auto_range", "0" );
	omni.Set( "angles", "0 45 0" );
	const bool omniSame = SelfTestRoundTrip( omni, first, second, written );
	test.Check( omniSame && first.lightType == LIGHT_POINT && first.emitter.enabled && first.emitter.effect == 3 && !first.autoRange
				&& second.hasCenter && second.lightCenter.Compare( idVec3( 0.0f, 0.0f, 16.0f ), 1e-3f )
				&& SelfTestDeletes( written, "parallel" ) && SelfTestDeletes( written, "light_target" ),
				"emitter omni: parse, write, parse again; parallel and the frustum deleted" );

	idDict spot;
	spot.Set( "classname", "light" );
	spot.Set( "emitter", "1" );
	spot.Set( "emit_shape", "spot" );
	spot.Set( "light_radius", "400 100 50" );
	spot.Set( "emit_cone", "40 20" );
	spot.Set( "angles", "90 0 0" );
	spot.Set( "light_target", "0 0 -256" );
	spot.Set( "light_right", "-128 0 0" );
	spot.Set( "light_up", "0 -128 0" );
	const bool spotSame = SelfTestRoundTrip( spot, first, second, written );
	test.Check( spotSame && first.lightType == LIGHT_SPOT && first.lightRadius.Compare( idVec3( 400.0f, 400.0f, 400.0f ), 1e-3f )
				&& written.GetVector( "light_radius" ).Compare( idVec3( 400.0f, 400.0f, 400.0f ), 1e-3f ) && SelfTestNear( first.emitter.coneV, 20.0f, 1e-3f )
				&& SelfTestDeletes( written, "light_target" ) && SelfTestDeletes( written, "light_right" ) && SelfTestDeletes( written, "light_up" )
				&& SelfTestDeletes( written, "parallel" ) && SelfTestDeletes( written, "light_center" ),
				"emitter spot: stale frustum keys ignored and deleted, light_radius written R R R, the cone kept" );
	test.Check( first.angles.ToMat3()[ 0 ].Compare( idVec3( 0.0f, 0.0f, -1.0f ), 1e-4f ), "emitter spot: angles \"90 0 0\" points axis[0] straight down" );

	idDict sun;
	sun.Set( "classname", "light" );
	sun.Set( "emitter", "1" );
	sun.Set( "emit_shape", "sun" );
	sun.Set( "parallel", "1" );
	sun.Set( "light_radius", "2048 2048 1024" );
	sun.Set( "light_center", "4 4 32" );
	sun.Set( "emit_effect", "pulse" );
	const bool sunSame = SelfTestRoundTrip( sun, first, second, written );
	test.Check( sunSame && first.lightType == LIGHT_SUN && idStr::Cmp( written.GetString( "parallel" ), "1" ) == 0
				&& written.GetVector( "light_center" ).Compare( idVec3( 4.0f, 4.0f, 32.0f ), 1e-3f ) && first.emitter.effect == EME_PULSE,
				"emitter sun: parallel 1, its box and light_center written as a sun writes them today" );

	idDict texture;
	texture.Set( "classname", "light" );
	texture.Set( "light_radius", "300 300 300" );
	texture.Set( "texture", "lights/squarelight1" );
	LightInfo textureLight;
	textureLight.FromDict( &texture );
	idDict textureWritten;
	textureLight.ToDict( &textureWritten );
	test.Check( !textureLight.emitter.enabled && textureWritten.MatchPrefix( "emit" ) == NULL,
				"texture light: no emitter key is written for a light that has none" );

	// the Texture tab on an emitter: emitter "0", every emit_* key left in place
	LightInfo switched;
	switched.FromDict( &omni );
	SwitchToTexture( switched );
	idDict switchedWritten;
	switched.ToDict( &switchedWritten );
	test.Check( idStr::Cmp( switchedWritten.GetString( "emitter" ), "0" ) == 0 && switchedWritten.MatchPrefix( "emit_" ) == NULL,
				"Texture tab: an emitter writes emitter 0 and leaves its emit_* keys on the light" );

	// RV-C-ET-14: a phase that rounds to zero is written "0", never "-0"
	emitterData_t tinyPhase;
	EM_ParseAllFromDict( &omni, &tinyPhase );
	tinyPhase.effectPhase = -0.00001f;
	tinyPhase.patternPhase = -0.00004f;
	idDict tinyDict;
	EM_WriteToDict( &tinyDict, tinyPhase, false );
	test.Check( idStr::Cmp( tinyDict.GetString( "emit_effect_phase" ), "0" ) == 0 && idStr::Cmp( tinyDict.GetString( "emit_pattern_phase" ), "0" ) == 0,
				"RV-C-ET-14: EM_WriteToDict writes a phase of -0.00001 \"0\", never \"-0\"" );
}

// a spot emitter at a known pose; auto range off, so the gestures alone are measured
static LightInfo SelfTestEmitterSpot()
{
	LightInfo spot;
	spot.DefaultProjected();
	SeedEmitter( spot.emitter, LIGHT_SPOT );
	spot.hasEmitterBlock = true;
	spot.autoRange = false;
	spot.origin.Set( 100.0f, 200.0f, 50.0f );
	spot.angles.Set( 0.0f, 30.0f, 0.0f );
	spot.emitter.start = 16.0f;
	SetEmitterRange( spot, 512.0f );
	return spot;
}

// AUTO, Q, E, R and auto range on emitters (the cvar defaults as sensitivities)
static void SelfTestEmitterGestures( LightSelfTest& test )
{
	const float scaleSens = 0.004f;
	const float wheelSens = 0.10f;
	const float rotSens = 0.25f;
	const float rotWheelSens = 5.0f;

	// ---- AUTO (AutoCaptureBase leaves the identity frame in the base, as for an unbound light): the beam through the aim.
	// Auto range is off on this spot, so since choice 2 (G0 9) no aim changes its range: before it, a far
	// aim set the range to farDistance / 0.6 here, and this check expected that
	const LightInfo spot = SelfTestEmitterSpot();
	LightAutoBase base;
	AutoCaptureBase( spot, base );
	LightInfo aimed = spot;
	const idVec3 farAim = spot.origin + idVec3( 900.0f, 300.0f, -200.0f );
	const float farDistance = ( farAim - spot.origin ).Length();
	const bool farMoved = AutoAimEmitterSpot( aimed, base, farAim );
	test.Check( farMoved && aimed.angles.ToMat3()[ 0 ].Compare( ( farAim - spot.origin ) / farDistance, 1e-4f )
				&& SelfTestNear( EM_Range( aimed.lightRadius ), 512.0f, 1e-3f ),
				"emitter AUTO: axis[0] runs through the aim; auto range off, a far aim keeps the range" );

	AutoAimEmitterSpot( aimed, base, spot.origin + idVec3( 100.0f, 0.0f, -50.0f ) );
	test.Check( SelfTestNear( EM_Range( aimed.lightRadius ), 512.0f, 1e-3f ) && SelfTestNear( aimed.emitter.size, spot.emitter.size, 1e-4f )
				&& SelfTestNear( aimed.emitter.start, spot.emitter.start, 1e-4f ),
				"emitter AUTO: auto range off, a near aim keeps the range, the size and the start" );

	LightInfo down = spot;
	down.angles.Zero();
	LightAutoBase downBase;
	AutoCaptureBase( down, downBase );
	AutoAimEmitterSpot( down, downBase, down.origin + idVec3( 0.0f, 0.0f, -300.0f ) );
	const idMat3 downAxis = down.angles.ToMat3();
	test.Check( downAxis[ 0 ].Compare( idVec3( 0.0f, 0.0f, -1.0f ), 1e-4f ) && SelfTestFinite( downAxis[ 1 ] ) && SelfTestFinite( downAxis[ 2 ] )
				&& downAxis[ 0 ].Cross( downAxis[ 1 ] ) * downAxis[ 2 ] > 0.999f,
				"emitter AUTO: aimed straight down, the frame stays finite and right-handed" );

	// an entity turned 90 degrees and moved since its dict pose: aimed in the world, stored in the dict's space
	LightStoredFrame turned;
	turned.storedOrigin.Zero();
	turned.worldOrigin.Set( 1000.0f, 0.0f, 0.0f );
	turned.axis = idAngles( 0.0f, 90.0f, 0.0f ).ToMat3();
	LightInfo bound = spot;
	bound.origin.Set( 100.0f, 0.0f, 0.0f );
	LightAutoBase boundBase;
	AutoCaptureBase( bound, boundBase );
	boundBase.storedFrame = turned;		// what RunFrame takes once for the entry
	boundBase.haveStoredFrame = true;
	const idVec3 boundBulb = turned.ToWorldPoint( bound.origin );
	const idVec3 boundAim = boundBulb + idVec3( 0.0f, 400.0f, -300.0f );
	AutoAimEmitterSpot( bound, boundBase, boundAim );
	test.Check( boundBulb.Compare( idVec3( 1000.0f, 100.0f, 0.0f ), 1e-2f ) && ( bound.angles.ToMat3() * turned.axis )[ 0 ].Compare( idVec3( 0.0f, 0.8f, -0.6f ), 1e-4f ),
				"emitter AUTO: a moved entity's light is aimed in the world and stored in its dict's space" );

	// RV-G4-04: the frame is the entry's, so a second frame at the same aim stores the same rotation
	const idMat3 boundFirst = bound.angles.ToMat3();
	AutoAimEmitterSpot( bound, boundBase, boundAim );
	test.Check( bound.angles.ToMat3().Compare( boundFirst, 1e-4f ), "emitter AUTO: two frames at the same aim store the same rotation (no feedback)" );

	// RV-G4-04: a light without light_origin is aimed in its stored space whatever its physics pose;
	// a light_origin light gets E^T * P (here E is the identity, P the turned and moved pose)
	idDict plainArgs;
	plainArgs.Set( "origin", "100 0 0" );
	plainArgs.Set( "angles", "0 30 0" );
	LightStoredFrame plainFrame;
	LightStoredFrameFromDict( plainArgs, turned.worldOrigin, turned.axis, plainFrame );
	idDict fixtureArgs;
	fixtureArgs.Set( "origin", "0 0 0" );
	fixtureArgs.Set( "light_origin", "100 0 0" );
	LightStoredFrame fixtureFrame;
	LightStoredFrameFromDict( fixtureArgs, turned.worldOrigin, turned.axis, fixtureFrame );
	test.Check( plainFrame.axis.Compare( mat3_identity, 1e-6f ) && plainFrame.ToWorldPoint( idVec3( 100.0f, 0.0f, 0.0f ) ).Compare( idVec3( 100.0f, 0.0f, 0.0f ), 1e-4f )
				&& fixtureFrame.axis.Compare( turned.axis, 1e-6f ) && fixtureFrame.worldOrigin.Compare( turned.worldOrigin, 1e-4f ),
				"stored frame: identity without light_origin (bound or not); E-transpose * P with light_origin" );

	// ---- Q
	const float s = idMath::Exp( 250.0f * scaleSens );
	LightInfo scaled = spot;
	ModalScale( scaled, spot, 250.0f, scaleSens );
	test.Check( SelfTestNear( EM_Range( scaled.lightRadius ), 512.0f * s, 1e-1f ) && SelfTestNear( scaled.emitter.size, spot.emitter.size * s, 1e-2f )
				&& SelfTestNear( scaled.emitter.start, 16.0f * s, 1e-2f ) && SelfTestNear( scaled.emitter.coneH, spot.emitter.coneH, 1e-4f )
				&& scaled.origin == spot.origin && scaled.angles == spot.angles,
				"emitter Q: range, size and start times s; the cone, the bulb and the rotation stay" );

	// ---- E
	LightInfo widened = spot;
	ModalStretch( widened, spot, 250.0f, 0.0f, 0.0f, scaleSens, wheelSens );
	test.Check( SelfTestNear( widened.emitter.tanHalfH, spot.emitter.tanHalfH * idMath::Exp( 1.0f ), 1e-3f ) && SelfTestNear( widened.emitter.coneV, spot.emitter.coneV, 1e-3f )
				&& SelfTestNear( EM_Range( widened.lightRadius ), 512.0f, 1e-3f ),
				"emitter E: mouse x widens cone h only" );

	LightInfo longer = spot;
	ModalStretch( longer, spot, 0.0f, 0.0f, 5.0f, scaleSens, wheelSens );
	test.Check( SelfTestNear( EM_Range( longer.lightRadius ), 512.0f * idMath::Exp( 0.5f ), 1e-1f )
				&& SelfTestNear( longer.emitter.coneH, spot.emitter.coneH, 1e-3f ) && SelfTestNear( longer.emitter.coneV, spot.emitter.coneV, 1e-3f ),
				"emitter E: the wheel changes the range only" );

	// ---- R
	const idMat3 spotAxis = spot.angles.ToMat3();
	LightInfo rolled = spot;
	ModalRotate( rolled, spot, 360.0f, 0.0f, 0.0f, rotSens, rotWheelSens );
	const idMat3 rolledAxis = rolled.angles.ToMat3();
	test.Check( rolledAxis[ 0 ].Compare( spotAxis[ 0 ], 1e-4f ) && idMath::Fabs( rolledAxis[ 1 ] * spotAxis[ 1 ] ) < 1e-3f,
				"emitter R: mouse x rolls 90 degrees about the beam; axis[0] stays" );

	LightInfo pitched = spot;
	ModalRotate( pitched, spot, 0.0f, -40.0f, 0.0f, rotSens, rotWheelSens );
	test.Check( pitched.angles.ToMat3()[ 0 ].z > spotAxis[ 0 ].z, "emitter R: mouse up raises the beam" );

	// ---- auto range
	LightInfo omni;
	omni.DefaultPoint();
	SeedEmitter( omni.emitter, LIGHT_POINT );
	omni.autoRange = true;
	omni.color.Set( 1.0f, 1.0f, 1.0f );
	EmitterReachChanged( omni );
	test.Check( omni.lightRadius.Compare( idVec3( 288.0f, 288.0f, 288.0f ), 1e-2f ), "auto range: intensity 1, size 96, start 0 gives 288" );

	LightInfo shaped = omni;
	shaped.lightRadius.Set( 300.0f, 150.0f, 300.0f );
	shaped.equalRadius = false;
	EmitterReachChanged( shaped );
	test.Check( shaped.lightRadius.Compare( idVec3( 288.0f, 144.0f, 288.0f ), 1e-2f ), "auto range: an omni with unequal radii is scaled as a whole" );

	LightInfo ranged = omni;
	SetEmitterRange( ranged, 576.0f );
	EmitterRangeSet( ranged );
	test.Check( SelfTestNear( ranged.emitter.size, 192.0f, 1e-2f ), "auto range: setting the range to 576 sets the size to 192" );

	LightInfo linear = omni;
	linear.emitter.falloff = EMF_LINEAR;
	linear.lightRadius.Set( 500.0f, 500.0f, 500.0f );
	EmitterReachChanged( linear );
	test.Check( linear.lightRadius.Compare( idVec3( 500.0f, 500.0f, 500.0f ), 1e-3f ), "auto range: a linear curve has no reach, the range stays" );

	LightInfo newSpot = omni;
	newSpot.DefaultProjected();
	test.Check( newSpot.emitter.enabled && newSpot.lightType == LIGHT_SPOT && newSpot.emitter.shape == EM_SPOT
				&& newSpot.angles.ToMat3()[ 0 ].Compare( idVec3( 0.0f, 0.0f, -1.0f ), 1e-4f ) && newSpot.lightRadius.Compare( idVec3( 288.0f, 288.0f, 288.0f ), 1e-2f ),
				"new emitter spot from the type radio: pitch 90, range from auto range" );
}

// the kind switch both ways, compared as rotations, and Cancel after it
static void SelfTestKindSwitch( LightSelfTest& test )
{
	LightInfo texture;
	texture.DefaultProjected();
	texture.angles.Set( 0.0f, 30.0f, 20.0f );
	const idVec3 beam = texture.angles.ToMat3() * ( texture.lightTarget / texture.lightTarget.Length() );

	const idDict noKeys;
	LightInfo asEmitter = texture;
	SwitchToEmitter( asEmitter, noKeys );
	const float expectedCone = RAD2DEG( 2.0f * idMath::ATan( 0.5f ) );
	test.Check( asEmitter.emitter.enabled && asEmitter.angles.ToMat3()[ 0 ].Compare( beam, 1e-4f ) && SelfTestNear( asEmitter.emitter.coneH, expectedCone, 1e-2f )
				&& SelfTestNear( asEmitter.emitter.coneV, expectedCone, 1e-2f ) && SelfTestNear( EM_Range( asEmitter.lightRadius ), 264.0f, 1e-2f )
				&& asEmitter.origin == texture.origin,
				"kind switch: texture spot to emitter keeps the beam; cone 2 atan( |right| / |target| ); range zNear + zFar" );

	LightInfo back = asEmitter;
	SwitchToTexture( back );
	// RV-C-ET-13: read back from the keys the panel writes, so the rotation is tested where it can be lost
	idDict backDict;
	back.ToDict( &backDict );
	backDict.DeleteEmptyKeys();		// ToDict's "" means delete, as on a light
	LightInfo backRead;
	backRead.FromDict( &backDict );
	const float backCone = RAD2DEG( 2.0f * idMath::ATan( backRead.lightRight.Length() / backRead.lightTarget.Length() ) );
	// 1e-3: the keys hold angles and vectors with two decimals
	test.Check( !backRead.emitter.enabled && backRead.lightType == LIGHT_SPOT && backRead.angles.ToMat3().Compare( asEmitter.angles.ToMat3(), 1e-3f )
				&& ( backRead.angles.ToMat3() * ( backRead.lightTarget / backRead.lightTarget.Length() ) ).Compare( beam, 1e-3f ) && SelfTestNear( backCone, asEmitter.emitter.coneH, 1e-2f ),
				"kind switch: emitter spot to texture keeps the rotation, the beam and the cone, as written and read back" );

	idDict keptKeys;
	asEmitter.ToDict( &keptKeys );
	LightInfo again = back;
	SwitchToEmitter( again, keptKeys );
	const idMat3 a = asEmitter.angles.ToMat3();
	const idMat3 b = again.angles.ToMat3();
	test.Check( b.Compare( a, 1e-4f ) && again.emitter.falloff == asEmitter.emitter.falloff,
				"kind switch: emitter to texture to emitter gives the same rotation and keeps the kept block" );
	test.Check( SelfTestNear( again.emitter.coneH, asEmitter.emitter.coneH, 1e-2f ) && SelfTestNear( again.emitter.coneV, asEmitter.emitter.coneV, 1e-2f )
				&& SelfTestNear( EM_Range( again.lightRadius ), EM_Range( asEmitter.lightRadius ), 1e-2f ),
				"kind switch: the round trip gives both cone angles and the range back unchanged" );

	// Cancel after the Emitter tab: the light ends with exactly the emit keys it had (none)
	idDict live;
	live.Set( "classname", "light" );
	live.Set( "light_radius", "300 300 300" );
	idDict originalEmit;
	CopyPrefixKeys( live, originalEmit, "emit" );
	LightInfo original;
	original.FromDict( &live );
	LightInfo edited = original;
	SwitchToEmitter( edited, live );
	idDict editedDict;
	edited.ToDict( &editedDict );
	SelfTestMerge( live, editedDict );
	const bool becameEmitter = ( idStr::Cmp( live.GetString( "emitter" ), "1" ) == 0 );
	idDict cancelDict;
	original.ToDict( &cancelDict );
	RestorePrefixKeys( originalEmit, live, cancelDict, "emit" );
	SelfTestMerge( live, cancelDict );
	test.Check( becameEmitter && live.MatchPrefix( "emit" ) == NULL, "Cancel: a texture light switched to Emitter is left with no emit key" );
}

// fix batch B (impl/B-FIXES.md): intensity, E on an omni, the bulb gizmo, the shape switch, drag speed,
// R's roll, the effect seeded from style and the start of a texture spot
static void SelfTestFixBatchB( LightSelfTest& test )
{
	const float scaleSens = 0.004f;
	const float wheelSens = 0.10f;
	const idDict noKeys;

	// ---- F-E-04 intensity beside the colour
	LightInfo tinted;
	tinted.DefaultPoint();
	tinted.color.Set( 2.0f, 1.0f, 0.5f );
	idVec3 hue( 1.0f, 1.0f, 1.0f );
	const float tintedIntensity = PanelColorSplit( tinted.color, hue );
	const bool splitOk = SelfTestNear( tintedIntensity, 2.0f, 1e-6f ) && hue.Compare( idVec3( 1.0f, 0.5f, 0.25f ), 1e-6f );
	PanelSetColor( tinted, hue, 0.0f );
	const float blackIntensity = PanelColorSplit( tinted.color, hue );
	const bool blackOk = tinted.color == vec3_zero && blackIntensity == 0.0f && hue.Compare( idVec3( 1.0f, 0.5f, 0.25f ), 1e-6f );
	PanelSetColor( tinted, hue, 3.0f );
	test.Check( splitOk && blackOk && tinted.color.Compare( idVec3( 3.0f, 1.5f, 0.75f ), 1e-5f ),
				"intensity: the largest channel, the colour over it; intensity 0 keeps the colour shown; _color = colour * intensity" );

	LightInfo textureA;
	textureA.DefaultPoint();
	textureA.color.Set( 2.0f, 1.0f, 0.5f );
	LightInfo textureB;
	textureB.DefaultPoint();
	PanelSetColor( textureB, idVec3( 1.0f, 0.5f, 0.25f ), 2.0f );
	idDict keysA;
	idDict keysB;
	textureA.ToDict( &keysA );
	textureB.ToDict( &keysB );
	test.Check( idStr::Cmp( keysA.GetString( "_color" ), keysB.GetString( "_color" ) ) == 0 && keysA.GetVector( "light_radius" ) == keysB.GetVector( "light_radius" ),
				"intensity: a texture light set through colour and intensity writes the keys of the same colour" );

	LightInfo reach;
	reach.DefaultPoint();
	SeedEmitter( reach.emitter, LIGHT_POINT );
	reach.autoRange = true;
	reach.color.Set( 1.0f, 1.0f, 1.0f );
	EmitterReachChanged( reach );
	PanelSetColor( reach, idVec3( 1.0f, 1.0f, 1.0f ), 4.0f );
	test.Check( SelfTestNear( EM_Range( reach.lightRadius ), EM_AutoRange( reach.emitter, 4.0f ), 1e-2f ) && EM_Range( reach.lightRadius ) > 288.0f,
				"intensity: an edit on an auto-range emitter is a reach edit" );

	// ---- F-E-05 E on an emitter omni keeps the lamp
	LightInfo bulbOmni;
	bulbOmni.DefaultPoint();
	SeedEmitter( bulbOmni.emitter, LIGHT_POINT );
	bulbOmni.hasEmitterBlock = true;
	bulbOmni.autoRange = false;
	bulbOmni.origin.Set( 10.0f, 20.0f, 30.0f );
	bulbOmni.angles.Set( 30.0f, 45.0f, 0.0f );
	bulbOmni.hasCenter = true;
	bulbOmni.lightCenter.Set( 0.0f, 0.0f, 16.0f );
	bulbOmni.lightRadius.Set( 200.0f, 200.0f, 200.0f );
	const idVec3 bulb = bulbOmni.origin + bulbOmni.angles.ToMat3() * bulbOmni.lightCenter;
	LightInfo stretched = bulbOmni;
	ModalStretch( stretched, bulbOmni, 0.0f, 0.0f, 5.0f, scaleSens, wheelSens );
	test.Check( stretched.origin == bulbOmni.origin && SelfTestNear( stretched.lightRadius.z, 200.0f * idMath::Exp( 0.5f ), 1e-2f )
				&& SelfTestNear( stretched.lightRadius.x, 200.0f, 1e-3f ) && LightGizmoOrigin( stretched ).Compare( bulb, 1e-3f ),
				"emitter E: an omni's radii scale about the bulb; the origin stays" );

	LightInfo texturePoint;
	texturePoint.DefaultPoint();
	texturePoint.origin = bulbOmni.origin;
	texturePoint.angles = bulbOmni.angles;
	LightInfo textureStretched = texturePoint;
	ModalStretch( textureStretched, texturePoint, 0.0f, 0.0f, 5.0f, scaleSens, wheelSens );
	const float sz = idMath::Exp( 0.5f );
	test.Check( textureStretched.origin.Compare( texturePoint.origin + texturePoint.angles.ToMat3()[ 2 ] * ( 300.0f * sz - 300.0f ), 1e-2f ),
				"E: a texture point light keeps its near face, as before" );

	// ---- F-E-06 the gizmo of an emitter omni sits on the bulb and turns about it
	LightInfo turnedOmni = bulbOmni;
	turnedOmni.angles.Set( 0.0f, 90.0f, 60.0f );
	const idVec3 turnedOrigin = LightOriginFromGizmo( turnedOmni, bulb );
	turnedOmni.origin = turnedOrigin;
	test.Check( LightGizmoOrigin( bulbOmni ).Compare( bulb, 1e-4f ) && LightOriginFromGizmo( bulbOmni, bulb ).Compare( bulbOmni.origin, 1e-3f )
				&& LightGizmoOrigin( turnedOmni ).Compare( bulb, 1e-3f ) && LightGizmoOrigin( texturePoint ) == texturePoint.origin
				&& LightOriginFromGizmo( texturePoint, bulb ) == bulb,
				"bulb gizmo: an emitter omni's gizmo is at origin + axis * light_center and turns about it; other lights' at the origin" );

	// ---- F-E-07 the type radio on an emitter keeps the rotation, and the range with auto range off
	LightInfo shapeOmni = bulbOmni;
	shapeOmni.hasCenter = false;
	shapeOmni.lightCenter.Zero();
	shapeOmni.angles.Set( 10.0f, 45.0f, 5.0f );
	shapeOmni.lightRadius.Set( 400.0f, 200.0f, 300.0f );
	shapeOmni.equalRadius = false;
	LightInfo shapeSpot = shapeOmni;
	shapeSpot.DefaultProjected();
	test.Check( shapeSpot.emitter.shape == EM_SPOT && shapeSpot.angles.ToMat3().Compare( shapeOmni.angles.ToMat3(), 1e-5f )
				&& shapeSpot.lightRadius.Compare( idVec3( 400.0f, 400.0f, 400.0f ), 1e-3f ),
				"shape switch: omni to spot keeps the rotation; auto range off, the range is the largest radius" );

	LightInfo shapeBack = shapeSpot;
	shapeBack.DefaultPoint();
	test.Check( shapeBack.emitter.shape == EM_OMNI && shapeBack.angles.ToMat3().Compare( shapeOmni.angles.ToMat3(), 1e-5f )
				&& shapeBack.lightRadius.Compare( idVec3( 400.0f, 400.0f, 400.0f ), 1e-3f ),
				"shape switch: spot to omni keeps the rotation; auto range off, the radii are the range" );

	LightInfo shapeAuto = shapeOmni;
	shapeAuto.autoRange = true;
	shapeAuto.color.Set( 1.0f, 1.0f, 1.0f );
	shapeAuto.DefaultProjected();
	test.Check( shapeAuto.angles.ToMat3().Compare( shapeOmni.angles.ToMat3(), 1e-5f ) && shapeAuto.lightRadius.Compare( idVec3( 288.0f, 288.0f, 288.0f ), 1e-2f ),
				"shape switch: auto range on, the spot's range is the auto range; the rotation stays" );

	LightInfo shapeSun = shapeOmni;
	shapeSun.DefaultSun();
	LightInfo textureSpot;
	textureSpot.DefaultPoint();
	textureSpot.angles.Set( 10.0f, 45.0f, 5.0f );
	textureSpot.DefaultProjected();
	test.Check( shapeSun.emitter.shape == EM_SUN && shapeSun.angles.ToMat3().Compare( shapeOmni.angles.ToMat3(), 1e-5f )
				&& shapeSun.lightRadius.Compare( idVec3( 2048.0f, 2048.0f, 1024.0f ), 1e-3f ) && textureSpot.angles == ang_zero,
				"shape switch: to sun keeps the rotation and gets the sun box; a texture light resets as before" );

	// ---- F-E-08 drag speed
	test.Check( DragSpeedFor( 10.0f ) == 0.25f && SelfTestNear( DragSpeedFor( 1000.0f ), 10.0f, 1e-5f ) && SelfTestNear( DragSpeedFor( -2000.0f ), 20.0f, 1e-5f ),
				"drag speed: 1 % of the value per pixel, at least 0.25" );

	// ---- F-E-09 R's roll levelled at entry, the beam kept
	LightInfo tilted = SelfTestEmitterSpot();
	tilted.angles.Set( -30.0f, 40.0f, 25.0f );
	const idMat3 tiltedBefore = tilted.angles.ToMat3();
	LightStoredFrame identityFrame;
	identityFrame.Identity();
	const bool levelled = ModalLevelSpotRoll( tilted, identityFrame );
	const idMat3 tiltedAfter = tilted.angles.ToMat3();
	idVec3 levelLeft = worldUp.Cross( tiltedAfter[ 0 ] );
	levelLeft.Normalize();
	const bool levelledAgain = ModalLevelSpotRoll( tilted, identityFrame );
	test.Check( levelled && !levelledAgain && tiltedAfter[ 0 ].Compare( tiltedBefore[ 0 ], 1e-4f ) && tiltedAfter[ 1 ].Compare( levelLeft, 1e-4f )
				&& tiltedAfter[ 0 ].Cross( tiltedAfter[ 1 ] ) * tiltedAfter[ 2 ] > 0.999f,
				"emitter R entry: the roll is levelled (axis[1] = worldUp x axis[0]), the beam kept; a level roll is left alone" );

	LightInfo vertical = SelfTestEmitterSpot();
	vertical.angles.Set( 90.0f, 0.0f, 0.0f );
	const idAngles verticalAngles = vertical.angles;
	LightStoredFrame turnedFrame;
	turnedFrame.Identity();
	turnedFrame.axis = idAngles( 0.0f, 90.0f, 0.0f ).ToMat3();
	LightInfo turnedTilted = SelfTestEmitterSpot();
	turnedTilted.angles.Set( -30.0f, 40.0f, 25.0f );
	const idVec3 turnedBeam = ( turnedTilted.angles.ToMat3() * turnedFrame.axis )[ 0 ];
	ModalLevelSpotRoll( turnedTilted, turnedFrame );
	const idMat3 turnedWorld = turnedTilted.angles.ToMat3() * turnedFrame.axis;
	test.Check( !ModalLevelSpotRoll( vertical, identityFrame ) && vertical.angles == verticalAngles
				&& turnedWorld[ 0 ].Compare( turnedBeam, 1e-4f ) && idMath::Fabs( turnedWorld[ 1 ].z ) < 1e-4f,
				"emitter R entry: a vertical beam keeps its roll; a moved entity's roll is levelled in the world" );

	// ---- F-E-10 the first emitter block plays the light's style
	LightInfo styled;
	styled.DefaultPoint();
	styled.lightStyle = 3;
	SwitchToEmitter( styled, noKeys );
	idDict styledKeys;
	styled.ToDict( &styledKeys );
	LightInfo unstyled;
	unstyled.DefaultPoint();
	unstyled.lightStyle = 0;
	SwitchToEmitter( unstyled, noKeys );
	idDict partialKeys;
	partialKeys.Set( "emit_size", "64" );
	LightInfo partial;
	partial.DefaultPoint();
	partial.lightStyle = 10;
	SwitchToEmitter( partial, partialKeys );
	test.Check( styled.emitter.effect == 3 && idStr::Cmp( styledKeys.GetString( "emit_effect" ), "candle_a" ) == 0 && styledKeys.GetInt( "style", "-1" ) == 3
				&& unstyled.emitter.effect == EME_NONE && partial.emitter.effect == 10,
				"first emitter block: style 1..11 seeds emit_effect with its name, style 0 seeds none; style stays" );

	idDict effectKeys;
	effectKeys.Set( "emit_effect", "pulse" );
	LightInfo effected;
	effected.DefaultPoint();
	effected.lightStyle = 3;
	SwitchToEmitter( effected, effectKeys );
	test.Check( effected.emitter.effect == EME_PULSE, "first emitter block: a light that has emit_effect keeps it" );

	// ---- F-E-11 a texture spot's explicit light_start becomes the emitter's start
	LightInfo startSpot;
	startSpot.DefaultProjected();
	startSpot.explicitStartEnd = true;
	startSpot.lightStart.Set( 0.0f, 0.0f, -32.0f );
	startSpot.lightEnd = startSpot.lightTarget;
	SwitchToEmitter( startSpot, noKeys );
	LightInfo nearSpot;
	nearSpot.DefaultProjected();
	nearSpot.explicitStartEnd = true;
	nearSpot.lightStart.Set( 0.0f, 0.0f, -2.0f );
	nearSpot.lightEnd = nearSpot.lightTarget;
	SwitchToEmitter( nearSpot, noKeys );
	test.Check( SelfTestNear( startSpot.emitter.start, 32.0f, 1e-3f ) && SelfTestNear( EM_Range( startSpot.lightRadius ), 288.0f, 1e-2f )
				&& SelfTestNear( nearSpot.emitter.start, 8.0f, 1e-3f ),
				"kind switch: a texture spot with light_start starts at its near plane, max( start . n, 8 )" );

	LightInfo implicitSpot;
	implicitSpot.DefaultProjected();
	SwitchToEmitter( implicitSpot, noKeys );
	test.Check( implicitSpot.emitter.start == 0.0f, "kind switch: a texture spot without light_start starts at 0" );
}

// batch C (impl/C-CHOICES.md, engine part 1): the aim under auto range, a hand-set range, the panel's
// in-force test, the entry of a gesture and the shape switch's shadow switches
static void SelfTestChoicesC( LightSelfTest& test )
{
	// ---- C-E-01 the aim sets the range both ways under auto range, through a re-captured base
	LightInfo autoSpot = SelfTestEmitterSpot();
	autoSpot.autoRange = true;
	EmitterReachChanged( autoSpot );
	const bool autoSpotInStep = SelfTestNear( EM_Range( autoSpot.lightRadius ), 16.0f + 96.0f * 3.0f, 1e-2f );

	LightAutoBase autoBase;
	AutoCaptureBase( autoSpot, autoBase );
	const idVec3 farAim = autoSpot.origin + idVec3( 900.0f, 300.0f, -200.0f );
	const float farDistance = ( farAim - autoSpot.origin ).Length();
	AutoAimEmitterSpot( autoSpot, autoBase, farAim );
	const float farRange = EM_Range( autoSpot.lightRadius );
	const bool farOk = SelfTestNear( farRange, farDistance / 0.6f, 1e-1f ) && SelfTestNear( autoSpot.emitter.size, ( farRange - 16.0f ) / 3.0f, 1e-2f );

	// a checkpoint, Enter or a re-pick captures the base again
	AutoCaptureBase( autoSpot, autoBase );
	const idVec3 nearAim = autoSpot.origin + idVec3( 100.0f, 0.0f, -50.0f );
	const float nearDistance = ( nearAim - autoSpot.origin ).Length();
	AutoAimEmitterSpot( autoSpot, autoBase, nearAim );
	const float nearRange = EM_Range( autoSpot.lightRadius );
	test.Check( autoSpotInStep && farOk && SelfTestNear( nearRange, nearDistance / 0.6f, 1e-2f )
				&& SelfTestNear( autoSpot.emitter.size, ( nearRange - 16.0f ) / 3.0f, 1e-2f )
				&& EM_RangeInStep( autoSpot.emitter, 1.0f, nearRange ) && autoSpot.autoRange,
				"C-E-01: under auto range a far aim then a near aim, base captured between, ends at the near aim's range and size" );

	// ---- C-E-02 EM_RangeInStep: max( 1, 5 % ) about the auto range, false without a reach
	emitterData_t stepBlock;
	SeedEmitter( stepBlock, LIGHT_POINT );
	emitterData_t tinyBlock = stepBlock;
	tinyBlock.size = 2.0f;
	emitterData_t linearBlock = stepBlock;
	linearBlock.falloff = EMF_LINEAR;
	test.Check( EM_RangeInStep( stepBlock, 1.0f, 288.0f ) && EM_RangeInStep( stepBlock, 1.0f, 302.0f ) && !EM_RangeInStep( stepBlock, 1.0f, 303.0f )
				&& EM_RangeInStep( tinyBlock, 1.0f, 25.0f ) && !EM_RangeInStep( tinyBlock, 1.0f, 25.5f ) && !EM_RangeInStep( linearBlock, 1.0f, 288.0f ),
				"C-E-02: EM_RangeInStep holds within max( 1, 5 % ) of the auto range, never for a curve without reach" );

	// a hand-set range wins: no emit_auto_range, range 500 where the auto range is 288
	idDict handSet;
	handSet.Set( "classname", "light" );
	handSet.Set( "emitter", "1" );
	handSet.Set( "emit_shape", "omni" );
	handSet.Set( "light_radius", "500 500 500" );
	handSet.Set( "_color", "1 1 1" );
	LightInfo handLight;
	handLight.FromDict( &handSet );
	const bool readOff = !handLight.autoRange;
	PanelSetColor( handLight, idVec3( 1.0f, 1.0f, 1.0f ), 2.0f );
	idDict handWritten;
	handLight.ToDict( &handWritten );
	test.Check( readOff && SelfTestNear( EM_Range( handLight.lightRadius ), 500.0f, 1e-3f ) && SelfTestNear( handLight.emitter.size, 96.0f, 1e-4f )
				&& idStr::Cmp( handWritten.GetString( "emit_auto_range" ), "0" ) == 0,
				"C-E-02: a range of 500 against an auto range of 288 reads with auto range off; an intensity edit keeps 500; emit_auto_range \"0\" is written" );

	idDict inStepKeys = handSet;
	inStepKeys.Set( "light_radius", "288 288 288" );
	LightInfo inStepLight;
	inStepLight.FromDict( &inStepKeys );
	test.Check( inStepLight.autoRange && EmitterAutoRangeInForce( inStepLight ), "C-E-02: the same light at its auto range reads in force" );

	// a dim lamp written under auto range reads back in force, at the editor's precision and at two decimals
	LightInfo dim;
	dim.DefaultPoint();
	SeedEmitter( dim.emitter, LIGHT_POINT );
	dim.hasEmitterBlock = true;
	dim.autoRange = true;
	PanelSetColor( dim, idVec3( 1.0f, 0.8f, 0.6f ), 0.234f );
	idDict dimKeys;
	dim.ToDict( &dimKeys );
	LightInfo dimRead;
	dimRead.FromDict( &dimKeys );
	idDict dimTwo = dimKeys;
	dimTwo.Set( "_color", "0.23 0.19 0.14" );
	LightInfo dimTwoRead;
	dimTwoRead.FromDict( &dimTwo );
	test.Check( dimRead.autoRange && dimTwoRead.autoRange && idStr::Cmp( dimKeys.GetString( "emit_auto_range" ), "1" ) == 0,
				"C-E-02: a dim lamp (intensity 0.234, size 96) written under auto range reads back in force, _color at 4 and at 2 decimals" );

	// a falloff edit that gives the curve a reach applies the flag as it stands (G0 5, corrected 01:00,
	// F-C-01): with the flag on the range becomes the auto range, in step or not; no in-step test here
	LightInfo linearIn;
	linearIn.DefaultPoint();
	SeedEmitter( linearIn.emitter, LIGHT_POINT );
	linearIn.hasEmitterBlock = true;
	linearIn.emitter.falloff = EMF_LINEAR;
	linearIn.autoRange = true;
	SetEmitterRange( linearIn, 288.0f );
	LightInfo linearOut = linearIn;
	SetEmitterRange( linearOut, 500.0f );
	EmitterSetFalloff( linearIn, EMF_INVSQUARE_CLAMPED );
	EmitterSetFalloff( linearOut, EMF_INVSQUARE_CLAMPED );
	test.Check( linearIn.autoRange && SelfTestNear( EM_Range( linearIn.lightRadius ), 288.0f, 1e-2f )
				&& linearOut.autoRange && SelfTestNear( EM_Range( linearOut.lightRadius ), 288.0f, 1e-2f ) && SelfTestNear( linearOut.emitter.size, 96.0f, 1e-4f ),
				"F-C-01: linear to inverse square with the flag on: the range becomes the auto range, in step before or not; auto range stays on" );

	// a range the mapper sets under linear turns the flag off; the curve's later reach keeps that range
	LightInfo linearHand = linearIn;
	EmitterSetFalloff( linearHand, EMF_LINEAR );
	const bool handStillOn = linearHand.autoRange;
	SetEmitterRange( linearHand, 500.0f );
	EmitterRangeEdited( linearHand, 288.0f );
	const bool handOff = !linearHand.autoRange;
	EmitterSetFalloff( linearHand, EMF_INVSQUARE_CLAMPED );
	idDict handKeys;
	linearHand.ToDict( &handKeys );
	test.Check( handStillOn && handOff && !linearHand.autoRange && SelfTestNear( EM_Range( linearHand.lightRadius ), 500.0f, 1e-3f )
				&& SelfTestNear( linearHand.emitter.size, 96.0f, 1e-4f ) && idStr::Cmp( handKeys.GetString( "emit_auto_range" ), "0" ) == 0,
				"F-C-01: a range set under linear turns auto range off; inverse square clamped then keeps 500 with auto range off" );

	// a round trip through every curve changes nothing
	LightInfo roundTrip;
	roundTrip.DefaultPoint();
	SeedEmitter( roundTrip.emitter, LIGHT_POINT );
	roundTrip.hasEmitterBlock = true;
	roundTrip.autoRange = true;
	EmitterReachChanged( roundTrip );
	const float roundStart = EM_Range( roundTrip.lightRadius );
	EmitterSetFalloff( roundTrip, EMF_EXPONENTIAL );
	const bool expMoved = !SelfTestNear( EM_Range( roundTrip.lightRadius ), roundStart, 1.0f );
	EmitterSetFalloff( roundTrip, EMF_LINEAR );
	EmitterSetFalloff( roundTrip, EMF_SMOOTH );
	EmitterSetFalloff( roundTrip, EMF_INVSQUARE_CLAMPED );
	test.Check( SelfTestNear( roundStart, 288.0f, 1e-2f ) && expMoved && roundTrip.autoRange && SelfTestNear( EM_Range( roundTrip.lightRadius ), roundStart, 1e-2f )
				&& SelfTestNear( roundTrip.emitter.size, 96.0f, 1e-4f ),
				"F-C-01: inverse square clamped, exponential, linear, smooth, inverse square clamped ends at the starting range with auto range on" );

	// ticking Auto range sets the range to the auto range and keeps the size (the box's own path)
	LightInfo ticked = linearHand;
	ticked.autoRange = true;
	EmitterReachChanged( ticked );
	test.Check( SelfTestNear( EM_Range( ticked.lightRadius ), 288.0f, 1e-2f ) && SelfTestNear( ticked.emitter.size, 96.0f, 1e-4f ),
				"C-E-02: ticking Auto range sets the range to the auto range; the size is kept" );

	// the shape switch: a linear light keeps its range though its flag is on; from a sun, the flag decides
	LightInfo linearOmni;
	linearOmni.DefaultPoint();
	SeedEmitter( linearOmni.emitter, LIGHT_POINT );
	linearOmni.hasEmitterBlock = true;
	linearOmni.emitter.falloff = EMF_LINEAR;
	linearOmni.autoRange = true;
	SetEmitterRange( linearOmni, 800.0f );
	LightInfo linearSpot = linearOmni;
	linearSpot.DefaultProjected();
	LightInfo sunAuto = inStepLight;
	sunAuto.DefaultSun();
	LightInfo sunOff = sunAuto;
	sunOff.autoRange = false;
	sunAuto.DefaultPoint();
	sunOff.DefaultPoint();
	test.Check( SelfTestNear( EM_Range( linearSpot.lightRadius ), 800.0f, 1e-3f ) && SelfTestNear( EM_Range( sunAuto.lightRadius ), 288.0f, 1e-2f )
				&& SelfTestNear( EM_Range( sunOff.lightRadius ), 300.0f, 1e-3f ),
				"C-E-02: shape switch: a linear light keeps its range; from a sun, the auto range with the flag on, else 300" );

	// ---- C-E-03 the panel's "Range (auto)" label and its line follow EmitterAutoRangeInForce
	LightInfo sunLight = inStepLight;
	sunLight.DefaultSun();
	test.Check( EmitterAutoRangeInForce( inStepLight ) && !EmitterAutoRangeInForce( handLight ) && !EmitterAutoRangeInForce( linearOmni )
				&& !EmitterAutoRangeInForce( sunLight ),
				"C-E-03: in force for an omni at its auto range; not for a hand-set range, a linear curve or a sun" );

	// ---- C-E-05 a gesture's entry keeps the authored roll for Cancel; the gesture runs on the levelled one
	LightInfo tilted = SelfTestEmitterSpot();
	tilted.angles.Set( -30.0f, 40.0f, 25.0f );
	const idMat3 authored = tilted.angles.ToMat3();
	LightStoredFrame identityFrame;
	identityFrame.Identity();
	LightAutoBase entryBase;
	AutoCaptureBase( tilted, entryBase );
	LightInfo entry;
	const bool prepared = ModalPrepareEntry( tilted, entry, entryBase, false, vec3_zero, &identityFrame );
	const bool levelledNow = !tilted.angles.ToMat3().Compare( authored, 1e-3f );
	// RV-C-ET-13: the gesture runs from the levelled light and moves it; the entry Cancel restores (HandlePress
	// sets cur = modalEntry, which needs an editor and a light, so it is not run here) keeps the authored roll
	LightInfo gestured = tilted;
	ModalRotate( gestured, tilted, 40.0f, 0.0f, 0.0f, 0.25f, 5.0f );
	const idMat3 gesturedAxis = gestured.angles.ToMat3();
	test.Check( prepared && levelledNow && !gesturedAxis.Compare( tilted.angles.ToMat3(), 1e-3f ) && !gesturedAxis.Compare( authored, 1e-3f )
				&& entry.angles.ToMat3().Compare( authored, 1e-5f ),
				"C-E-05: R entry levels the roll for the gesture, which turns the levelled light; the entry Cancel restores keeps the authored roll" );

	// ---- C-E-06 a shape switch keeps an emitter's noshadows and nospecular; a texture light resets as before
	LightInfo quiet = inStepLight;
	quiet.castShadows = false;
	quiet.skipSpecular = true;
	quiet.DefaultProjected();
	LightInfo quietSun = quiet;
	quietSun.DefaultSun();
	LightInfo quietTexture;
	quietTexture.DefaultPoint();
	quietTexture.castShadows = false;
	quietTexture.skipSpecular = true;
	quietTexture.DefaultProjected();
	idDict quietKeys;
	quiet.ToDict( &quietKeys );
	test.Check( !quiet.castShadows && quiet.skipSpecular && !quietSun.castShadows && quietSun.skipSpecular
				&& quietTexture.castShadows && !quietTexture.skipSpecular
				&& idStr::Cmp( quietKeys.GetString( "noshadows" ), "1" ) == 0 && idStr::Cmp( quietKeys.GetString( "nospecular" ), "1" ) == 0,
				"C-E-06: an emitter's shape switch keeps noshadows and nospecular; a texture light's resets them as before" );
}

//puncho start - batch C, engine part 2 (impl/C-CHOICES.md, C-E-10 to C-E-12, F-E4-01)
// F-E4-01: within one aim the start clamps to the range and comes back
static void SelfTestAimStart( LightSelfTest& test )
{
	// ---- F-E4-01: within one aim under auto range a near aim clamps the start and a far aim gives it back
	LightInfo aimSpot = SelfTestEmitterSpot();
	aimSpot.emitter.start = 100.0f;
	aimSpot.autoRange = true;
	EmitterReachChanged( aimSpot );
	LightAutoBase aimBase;
	AutoCaptureBase( aimSpot, aimBase );
	const idVec3 aimDir( 0.6f, 0.0f, -0.8f );
	AutoAimEmitterSpot( aimSpot, aimBase, aimSpot.origin + aimDir * 40.0f );
	const float nearStart = aimSpot.emitter.start;
	const float nearAimRange = EM_Range( aimSpot.lightRadius );
	AutoAimEmitterSpot( aimSpot, aimBase, aimSpot.origin + aimDir * 600.0f );
	test.Check( SelfTestNear( nearAimRange, 40.0f / 0.6f, 1e-2f ) && SelfTestNear( nearStart, nearAimRange - 1.0f, 1e-3f ) && aimSpot.emitter.start == 100.0f
				&& SelfTestNear( EM_Range( aimSpot.lightRadius ), 1000.0f, 1e-1f ) && SelfTestNear( aimSpot.emitter.size, 300.0f, 1e-1f ),
				"F-E4-01: under auto range a near aim clamps the start to range - 1; a far aim in the same entry gives the entry start back" );
}

// batch D (impl/D-REVIEW-FIXES.md), the editor's findings: the checkpoint undo across a kind switch, the
// bulb at a gesture's entry, the session's Equilateral
static void SelfTestReviewFixesD( LightSelfTest& test )
{
	const idDict noKeys;

	// ---- RV-C-ET-01, a texture light: (1) LMB in AUTO takes a checkpoint, (2) F opens the panel, (3) the
	// Emitter tab writes the emitter to the light, (4) F back and RMB: the light is the texture light again
	idDict live;
	live.Set( "classname", "light" );
	live.Set( "name", "et01_texture" );
	live.Set( "origin", "0 0 0" );
	live.Set( "light_radius", "300 300 300" );
	live.Set( "texture", "lights/squarelight1" );
	LightInfo cur;
	cur.FromDict( &live );
	const LightInfo undo = cur;
	idDict checkpointEmit;
	CheckpointKeys( live, undo, checkpointEmit );
	SwitchToEmitter( cur, live );
	idDict tabDict;
	cur.ToDict( &tabDict );
	SelfTestMerge( live, tabDict );
	const bool tabMadeEmitter = ( idStr::Cmp( live.GetString( "emitter" ), "1" ) == 0 );
	// what the undo wrote before the fix: the snapshot's ToDict alone, which has no emitter key
	idDict unfixed = live;
	idDict snapshotDict;
	LightInfo snapshot = undo;
	snapshot.ToDict( &snapshotDict );
	SelfTestMerge( unfixed, snapshotDict );
	idDict undoDict;
	CheckpointUndoDict( undo, live, checkpointEmit, undoDict );
	SelfTestMerge( live, undoDict );
	LightInfo back;
	back.FromDict( &live );
	test.Check( tabMadeEmitter && idStr::Cmp( unfixed.GetString( "emitter" ), "1" ) == 0 && live.MatchPrefix( "emit" ) == NULL && !back.emitter.enabled
				&& idStr::Cmp( live.GetString( "texture" ), "lights/squarelight1" ) == 0,
				"RV-C-ET-01: a checkpoint, the Emitter tab on a texture light, RMB: the light is a texture light again with no emit key (the snapshot alone left emitter 1)" );

	// ---- RV-C-ET-02: a gesture's entry turns a held emitter omni about its bulb
	LightInfo bulbOmni;
	bulbOmni.DefaultPoint();
	SwitchToEmitter( bulbOmni, noKeys );
	bulbOmni.origin.Set( 100.0f, 0.0f, 0.0f );
	bulbOmni.hasCenter = true;
	bulbOmni.lightCenter.Set( 16.0f, 0.0f, 32.0f );
	const idVec3 bulbBefore = LightGizmoOrigin( bulbOmni );
	LightAutoBase bulbBase;
	AutoCaptureBase( bulbOmni, bulbBase );
	LightInfo bulbAligned = bulbOmni;
	const bool bulbTurned = ModalAlignSphere( bulbAligned, bulbBase, idVec3( 300.0f, 120.0f, 80.0f ) );
	LightInfo plainPoint;
	plainPoint.DefaultPoint();
	plainPoint.origin.Set( 100.0f, 0.0f, 0.0f );
	plainPoint.hasCenter = true;
	plainPoint.lightCenter.Set( 16.0f, 0.0f, 32.0f );
	const bool plainTurned = ModalAlignSphere( plainPoint, bulbBase, idVec3( 300.0f, 120.0f, 80.0f ) );
	test.Check( bulbOmni.emitter.enabled && bulbTurned && LightGizmoOrigin( bulbAligned ).Compare( bulbBefore, 1e-3f ) && !bulbAligned.origin.Compare( bulbOmni.origin, 1e-1f )
				&& plainTurned && plainPoint.origin.Compare( idVec3( 100.0f, 0.0f, 0.0f ), 1e-6f ),
				"RV-C-ET-02: entering a gesture turns an emitter omni with a bulb offset about its bulb; a texture point light keeps its origin" );

	// ---- RV-C-ET-08: Equilateral once unticked stays so for a re-read emitter omni; the keys do not change
	idDict equalKeys;
	bulbOmni.ToDict( &equalKeys );
	equalKeys.DeleteEmptyKeys();
	LightInfo reread;
	reread.FromDict( &equalKeys );
	const bool tickedOnRead = reread.equalRadius;
	EquilateralForSession( reread, false );
	const bool tickedWithoutUntick = reread.equalRadius;
	EquilateralForSession( reread, true );
	idDict untickedKeys;
	reread.ToDict( &untickedKeys );
	LightInfo textureRead;
	textureRead.DefaultPoint();
	EquilateralForSession( textureRead, true );
	test.Check( tickedOnRead && tickedWithoutUntick && !reread.equalRadius && idStr::Cmp( untickedKeys.GetString( "light_radius" ), equalKeys.GetString( "light_radius" ) ) == 0
				&& textureRead.equalRadius,
				"RV-C-ET-08: after an untick a re-read emitter omni with equal radii shows three radii and writes the same light_radius; a texture light is not touched" );
}

// batch E ( impl/E-EMITTER-COSTS.md ): EC-01, the keys an emitter reads at spawn, and EC-02, a spot's
// shadow projection from the culling apex's inverse frame. EC-03 draws nothing headless and is not here
static void SelfTestEmitterCosts( LightSelfTest& test )
{
	// ---- EC-01, EM_ParseFromDict: a key the shape, pattern or effect does not use holds its default
	idDict coneKeys;
	coneKeys.Set( "emitter", "1" );
	coneKeys.Set( "emit_shape", "omni" );
	coneKeys.Set( "emit_cone", "30" );
	coneKeys.Set( "emit_penumbra", "0.9" );
	coneKeys.Set( "emit_square", "8" );
	emitterData_t omniCone;
	EM_ParseFromDict( &coneKeys, &omniCone );
	coneKeys.Set( "emit_shape", "spot" );
	emitterData_t spotCone;
	EM_ParseFromDict( &coneKeys, &spotCone );
	test.Check( omniCone.coneH == 60.0f && omniCone.coneV == 60.0f && omniCone.penumbra == 0.2f && omniCone.squareness == 2.0f
				&& spotCone.coneH == 30.0f && spotCone.coneV == 30.0f && spotCone.penumbra == 0.9f && spotCone.squareness == 8.0f,
				"EC-01: an omni reads no emit_cone, emit_penumbra, emit_square (defaults); the same keys on a spot are read" );

	// the default tangents are a constant: the bits the parse computes for emit_cone "60"
	idDict sixty;
	sixty.Set( "emitter", "1" );
	sixty.Set( "emit_shape", "spot" );
	sixty.Set( "emit_cone", "60" );
	emitterData_t spotSixty;
	EM_ParseFromDict( &sixty, &spotSixty );
	test.Check( memcmp( &omniCone.tanHalfH, &spotSixty.tanHalfH, sizeof( float ) ) == 0 && memcmp( &omniCone.tanHalfV, &spotSixty.tanHalfV, sizeof( float ) ) == 0,
				"EC-01: the default half-cone tangent constant has the bits the parse computes for emit_cone \"60\"" );

	idDict patternKeys;
	patternKeys.Set( "emitter", "1" );
	patternKeys.Set( "emit_pattern", "none" );
	patternKeys.Set( "emit_pattern_strength", "0.6" );
	patternKeys.Set( "emit_pattern_scale", "3" );
	patternKeys.Set( "emit_pattern_phase", "0.25" );
	patternKeys.Set( "emit_effect", "none" );
	patternKeys.Set( "emit_effect_speed", "4" );
	patternKeys.Set( "emit_effect_amount", "0.5" );
	patternKeys.Set( "emit_effect_phase", "0.75" );
	emitterData_t plain;
	EM_ParseFromDict( &patternKeys, &plain );
	patternKeys.Set( "emit_pattern", "ice" );
	patternKeys.Set( "emit_effect", "pulse" );
	emitterData_t patterned;
	EM_ParseFromDict( &patternKeys, &patterned );
	test.Check( plain.patternStrength == 0.0f && plain.patternScale == 1.0f && plain.patternPhase == 0.0f
				&& plain.effectSpeed == 1.0f && plain.effectAmount == 1.0f && plain.effectPhase == 0.0f
				&& patterned.pattern == EMP_ICE && patterned.patternStrength == 0.6f && patterned.patternScale == 3.0f && patterned.patternPhase == 0.25f
				&& patterned.effect == EME_PULSE && patterned.effectSpeed == 4.0f && patterned.effectAmount == 0.5f && patterned.effectPhase == 0.75f,
				"EC-01: pattern none and effect none read none of their three keys (defaults); ice and pulse read them" );

	idDict sunKeys;
	sunKeys.Set( "emitter", "1" );
	sunKeys.Set( "emit_shape", "sun" );
	sunKeys.Set( "emit_falloff", "linear" );
	sunKeys.Set( "emit_size", "40" );
	sunKeys.Set( "emit_start", "12" );
	emitterData_t sun;
	EM_ParseFromDict( &sunKeys, &sun );
	test.Check( sun.shape == EM_SUN && sun.falloff == EMF_INVSQUARE_CLAMPED && sun.size == 96.0f && sun.start == 0.0f,
				"EC-01: a sun reads no emit_falloff, emit_size, emit_start (defaults)" );

	idDict parallelKeys;
	parallelKeys.Set( "emitter", "1" );
	parallelKeys.Set( "parallel", "1" );
	emitterData_t inferred;
	EM_ParseFromDict( &parallelKeys, &inferred );
	parallelKeys.Set( "emit_shape", "omni" );
	emitterData_t named;
	EM_ParseFromDict( &parallelKeys, &named );
	test.Check( inferred.shape == EM_SUN && named.shape == EM_OMNI,
				"EC-01: emit_shape absent with parallel \"1\" is a sun; emit_shape \"omni\" with parallel \"1\" is an omni" );

	// ---- EC-01, ParseSpawnArgsToRenderLight: an emitter reads none of the five vectors, nor texture or
	// light, light_radius once and light_center only on an omni or a sun
	idDict stale;
	stale.Set( "classname", "light" );
	stale.Set( "origin", "10 20 30" );
	stale.Set( "emitter", "1" );
	stale.Set( "emit_shape", "omni" );
	stale.Set( "light_radius", "200 100 50" );
	stale.Set( "light_center", "4 5 6" );
	stale.Set( "light_target", "0 0 -256" );
	stale.Set( "light_up", "0 128 0" );
	stale.Set( "light_right", "128 0 0" );
	stale.Set( "light_start", "0 0 -8" );
	stale.Set( "light_end", "0 0 -300" );
	stale.Set( "texture", "lights/squarelight1" );
	renderLight_t omniLight;
	gameEdit->ParseSpawnArgsToRenderLight( &stale, &omniLight );
	stale.Set( "emit_shape", "spot" );
	renderLight_t spotLight;
	gameEdit->ParseSpawnArgsToRenderLight( &stale, &spotLight );
	const bool vectorsZero = omniLight.target == vec3_zero && omniLight.up == vec3_zero && omniLight.right == vec3_zero
							 && omniLight.start == vec3_zero && omniLight.end == vec3_zero && spotLight.target == vec3_zero && spotLight.end == vec3_zero;
	test.Check( vectorsZero && omniLight.pointLight && !omniLight.parallel && omniLight.lightRadius == idVec3( 200.0f, 100.0f, 50.0f )
				&& omniLight.lightCenter == idVec3( 4.0f, 5.0f, 6.0f ) && !spotLight.pointLight && spotLight.lightRadius == idVec3( 200.0f, 200.0f, 200.0f )
				&& spotLight.lightCenter == vec3_zero && omniLight.shader == spotLight.shader && omniLight.shader != NULL
				&& omniLight.shader == declManager->FindMaterial( "lights/defaultPointLight", false ),
				"EC-01: an emitter with stale light_target, light_up, light_right spawns with the five vectors zero; a spot has ( R, R, R ) and no centre, an omni keeps light_center" );

	// G0 section 4: a sun spawns as a parallel point light, whether emit_shape names it or parallel "1"
	// stands for an absent shape ( DP-G1E-04: no other check asserted it )
	idDict spawnSunKeys;
	spawnSunKeys.Set( "classname", "light" );
	spawnSunKeys.Set( "emitter", "1" );
	spawnSunKeys.Set( "emit_shape", "sun" );
	spawnSunKeys.Set( "light_radius", "512 512 1024" );
	spawnSunKeys.Set( "light_center", "0 0 2000" );
	renderLight_t namedSun;
	gameEdit->ParseSpawnArgsToRenderLight( &spawnSunKeys, &namedSun );
	spawnSunKeys.Delete( "emit_shape" );
	spawnSunKeys.Set( "parallel", "1" );
	renderLight_t inferredSun;
	gameEdit->ParseSpawnArgsToRenderLight( &spawnSunKeys, &inferredSun );
	test.Check( namedSun.emitter.enabled && namedSun.emitter.shape == EM_SUN && namedSun.parallel && namedSun.pointLight
				&& inferredSun.emitter.enabled && inferredSun.emitter.shape == EM_SUN && inferredSun.parallel && inferredSun.pointLight
				&& namedSun.lightRadius == idVec3( 512.0f, 512.0f, 1024.0f ) && namedSun.lightCenter == idVec3( 0.0f, 0.0f, 2000.0f )
				&& inferredSun.lightRadius == namedSun.lightRadius && inferredSun.lightCenter == namedSun.lightCenter,
				"G0 section 4: an emitter sun ( emit_shape \"sun\", or no shape and parallel \"1\" ) spawns with parallel and pointLight both set, light_radius its box and light_center its direction" );

	idDict lightKey;
	lightKey.Set( "classname", "light" );
	lightKey.Set( "emitter", "1" );
	lightKey.Set( "light", "200" );
	renderLight_t lightKeyLight;
	gameEdit->ParseSpawnArgsToRenderLight( &lightKey, &lightKeyLight );
	lightKey.Delete( "emitter" );
	renderLight_t texturePoint;
	gameEdit->ParseSpawnArgsToRenderLight( &lightKey, &texturePoint );
	test.Check( lightKeyLight.lightRadius == idVec3( 300.0f, 300.0f, 300.0f ) && texturePoint.lightRadius == idVec3( 200.0f, 200.0f, 200.0f ) && !texturePoint.emitter.enabled,
				"EC-01: an emitter with no light_radius and light \"200\" gets 300 300 300; the same light as a texture light still reads light \"200\"" );

	// a texture spot still reads its frustum
	stale.Delete( "emitter" );
	renderLight_t textureSpot;
	gameEdit->ParseSpawnArgsToRenderLight( &stale, &textureSpot );
	test.Check( !textureSpot.emitter.enabled && !textureSpot.pointLight && textureSpot.target == idVec3( 0.0f, 0.0f, -256.0f ) && textureSpot.up == idVec3( 0.0f, 128.0f, 0.0f )
				&& textureSpot.right == idVec3( 128.0f, 0.0f, 0.0f ) && textureSpot.start == idVec3( 0.0f, 0.0f, -8.0f ) && textureSpot.end == idVec3( 0.0f, 0.0f, -300.0f ),
				"EC-01: a texture spot reads its five vectors as before" );

	// DP-EC-01: the editor writes every emit_* key back from the block it parsed ( G0 section 2, "Writers" ), so it
	// reads them all ( EM_ParseAllFromDict ): editing a light leaves a key its shape, pattern or effect does not
	// use exactly as it was, so switching back brings the authored value
	static const char* const keptOmniKeys[][ 2 ] =
	{
		{ "emit_cone", "30" }, { "emit_penumbra", "0.9" }, { "emit_square", "8" },
		{ "emit_pattern_strength", "0.6" }, { "emit_pattern_scale", "3" }, { "emit_pattern_phase", "0.25" },
		{ "emit_effect_speed", "4" }, { "emit_effect_amount", "0.5" }, { "emit_effect_phase", "0.75" },
	};
	static const char* const keptSunKeys[][ 2 ] =
	{
		{ "emit_falloff", "linear" }, { "emit_size", "40" }, { "emit_start", "12" },
	};
	const int numKeptOmni = sizeof( keptOmniKeys ) / sizeof( keptOmniKeys[ 0 ] );
	const int numKeptSun = sizeof( keptSunKeys ) / sizeof( keptSunKeys[ 0 ] );
	idDict keptOmni;
	keptOmni.Set( "classname", "light" );
	keptOmni.Set( "origin", "0 0 0" );
	keptOmni.Set( "emitter", "1" );
	keptOmni.Set( "emit_shape", "omni" );
	keptOmni.Set( "light_radius", "300 300 300" );
	keptOmni.Set( "emit_pattern", "none" );
	keptOmni.Set( "emit_effect", "none" );
	for( int i = 0; i < numKeptOmni; i++ )
	{
		keptOmni.Set( keptOmniKeys[ i ][ 0 ], keptOmniKeys[ i ][ 1 ] );
	}
	idDict keptSun;
	keptSun.Set( "classname", "light" );
	keptSun.Set( "origin", "0 0 0" );
	keptSun.Set( "emitter", "1" );
	keptSun.Set( "emit_shape", "sun" );
	keptSun.Set( "parallel", "1" );
	keptSun.Set( "light_radius", "2048 2048 1024" );
	keptSun.Set( "light_center", "4 4 32" );
	for( int i = 0; i < numKeptSun; i++ )
	{
		keptSun.Set( keptSunKeys[ i ][ 0 ], keptSunKeys[ i ][ 1 ] );
	}

	// the edit: a colour change, then the editor's write
	LightInfo keptOmniLight;
	keptOmniLight.FromDict( &keptOmni );
	keptOmniLight.color.Set( 0.5f, 0.25f, 1.0f );
	idDict keptOmniWritten;
	keptOmniLight.ToDict( &keptOmniWritten );
	LightInfo keptSunLight;
	keptSunLight.FromDict( &keptSun );
	keptSunLight.color.Set( 0.5f, 0.25f, 1.0f );
	idDict keptSunWritten;
	keptSunLight.ToDict( &keptSunWritten );

	bool keptAll = true;
	for( int i = 0; i < numKeptOmni; i++ )
	{
		const char* written = keptOmniWritten.GetString( keptOmniKeys[ i ][ 0 ], keptOmniKeys[ i ][ 1 ] );
		if( idStr::Cmp( written, keptOmniKeys[ i ][ 1 ] ) != 0 )
		{
			common->Printf( "le_selfTest DP-EC-01 omni: %s written \"%s\", was \"%s\"\n", keptOmniKeys[ i ][ 0 ], written, keptOmniKeys[ i ][ 1 ] );
			keptAll = false;
		}
	}
	for( int i = 0; i < numKeptSun; i++ )
	{
		const char* written = keptSunWritten.GetString( keptSunKeys[ i ][ 0 ], keptSunKeys[ i ][ 1 ] );
		if( idStr::Cmp( written, keptSunKeys[ i ][ 1 ] ) != 0 )
		{
			common->Printf( "le_selfTest DP-EC-01 sun: %s written \"%s\", was \"%s\"\n", keptSunKeys[ i ][ 0 ], written, keptSunKeys[ i ][ 1 ] );
			keptAll = false;
		}
	}
	test.Check( keptAll, "DP-EC-01: editing an omni leaves its cone keys and a none pattern's and a none effect's three keys as they were; editing a sun leaves its falloff, size and start" );

	// the two entries agree in every field the light uses ( G0 section 3, "Two entries" ): an omni, a spot with a
	// pattern and an effect, a sun; the dicts carry every key, so the full read has something to differ on
	idDict both;
	both.Set( "emitter", "1" );
	both.Set( "emit_falloff", "exponential" );
	both.Set( "emit_size", "40" );
	both.Set( "emit_start", "12" );
	both.Set( "emit_clamp", "0" );
	both.Set( "emit_cone", "50 30" );
	both.Set( "emit_penumbra", "0.4" );
	both.Set( "emit_square", "6" );
	both.Set( "emit_pattern_strength", "0.6" );
	both.Set( "emit_pattern_scale", "3" );
	both.Set( "emit_pattern_phase", "0.25" );
	both.Set( "emit_effect_speed", "4" );
	both.Set( "emit_effect_amount", "0.5" );
	both.Set( "emit_effect_phase", "0.75" );
	bool entriesAgree = true;
	for( int c = 0; c < 3; c++ )
	{
		both.Set( "emit_shape", ( c == 0 ) ? "omni" : ( c == 1 ) ? "spot" : "sun" );
		both.Set( "emit_pattern", ( c == 1 ) ? "caustic" : "none" );
		both.Set( "emit_effect", ( c == 1 ) ? "pulse" : "none" );
		emitterData_t game;
		emitterData_t all;
		EM_ParseFromDict( &both, &game );
		EM_ParseAllFromDict( &both, &all );
		bool same = game.enabled == all.enabled && game.shape == all.shape && game.pattern == all.pattern && game.effect == all.effect
					&& game.energyClamp == all.energyClamp;
		if( game.shape != EM_SUN )
		{
			same = same && game.falloff == all.falloff && game.size == all.size && game.start == all.start;
		}
		if( game.shape == EM_SPOT )
		{
			same = same && game.coneH == all.coneH && game.coneV == all.coneV && game.tanHalfH == all.tanHalfH && game.tanHalfV == all.tanHalfV
				   && game.penumbra == all.penumbra && game.squareness == all.squareness;
		}
		if( game.pattern != EMP_NONE )
		{
			same = same && game.patternStrength == all.patternStrength && game.patternScale == all.patternScale && game.patternPhase == all.patternPhase;
		}
		if( game.effect != EME_NONE )
		{
			same = same && game.effectSpeed == all.effectSpeed && game.effectAmount == all.effectAmount && game.effectPhase == all.effectPhase;
		}
		entriesAgree = entriesAgree && same && ( c != 1 || ( game.pattern == EMP_CAUSTIC && game.effect == EME_PULSE && game.coneH == 50.0f ) );
	}
	test.Check( entriesAgree, "DP-EC-01: EM_ParseFromDict and EM_ParseAllFromDict give the same block in every field an omni, a spot with a pattern and an effect, and a sun use" );

	// ---- EC-02: the shadow projection from the apex's inverse against one built with a general inverse of
	// the bulb's frame ( what the derive did before ), relative to the largest element
	struct ec02Spot_t
	{
		idVec3		origin;
		idAngles	angles;
		float		range;
	};
	const ec02Spot_t spots[] =
	{
		{ idVec3( 0.0f, 0.0f, 0.0f ), idAngles( 0.0f, 0.0f, 0.0f ), 256.0f },
		{ idVec3( 120.0f, -64.0f, 32.0f ), idAngles( 90.0f, 0.0f, 0.0f ), 512.0f },
		{ idVec3( 300.0f, 200.0f, -100.0f ), idAngles( 33.0f, 127.0f, -41.0f ), 300.0f },
		{ idVec3( 100000.0f, 0.0f, 0.0f ), idAngles( 0.0f, 0.0f, 0.0f ), 256.0f },
		{ idVec3( 100000.0f, -70000.0f, 2000.0f ), idAngles( 17.0f, 211.0f, 63.0f ), 1024.0f },
	};
	idDict ec02Keys;
	ec02Keys.Set( "emitter", "1" );
	ec02Keys.Set( "emit_shape", "spot" );
	ec02Keys.Set( "emit_cone", "50 30" );
	emitterData_t ec02Spot;
	EM_ParseFromDict( &ec02Keys, &ec02Spot );
	float worstRelative = 0.0f;
	const int numSpots = sizeof( spots ) / sizeof( spots[ 0 ] );
	for( int s = 0; s < numSpots; s++ )
	{
		const idMat3 axis = spots[ s ].angles.ToMat3();
		const float pullBack = Max( 8.0f, spots[ s ].range / 512.0f );

		idRenderMatrix apexMatrix;
		idRenderMatrix::CreateFromOriginAxis( spots[ s ].origin - axis[0] * pullBack, axis, apexMatrix );
		idRenderMatrix inverseApex;
		idRenderMatrix::Inverse( apexMatrix, inverseApex );
		idRenderMatrix got;
		R_EmitterSpotShadowProject( ec02Spot, spots[ s ].range, pullBack, inverseApex, got );

		idRenderMatrix bulbMatrix;
		idRenderMatrix::CreateFromOriginAxis( spots[ s ].origin, axis, bulbMatrix );
		idRenderMatrix inverseBulb;
		idRenderMatrix::Inverse( bulbMatrix, inverseBulb );
		idRenderMatrix want;
		R_EmitterSpotShadowProject( ec02Spot, spots[ s ].range, 0.0f, inverseBulb, want );

		float largest = 0.0f;
		float difference = 0.0f;
		for( int i = 0; i < 4; i++ )
		{
			for( int j = 0; j < 4; j++ )
			{
				largest = Max( largest, idMath::Fabs( want[ i ][ j ] ) );
				difference = Max( difference, idMath::Fabs( got[ i ][ j ] - want[ i ][ j ] ) );
			}
		}
		const float relative = ( largest > 0.0f ) ? difference / largest : difference;
		common->Printf( "le_selfTest EC-02 spot %d: origin %s, largest element %g, largest difference %g, relative %g\n", s,
						spots[ s ].origin.ToString( 0 ), largest, difference, relative );
		worstRelative = Max( worstRelative, relative );
	}
	common->Printf( "le_selfTest EC-02: largest shadow-projection difference relative to the largest element: %g\n", worstRelative );
	test.Check( worstRelative < 1e-5f, "EC-02: a spot's shadow projection from the apex's inverse matches a general inverse of the bulb's frame" );
}
//puncho end

// the opening mode follows le_startEasy; runs Open and Close on the tool, so only while it is closed
static void SelfTestOpenMode( LightSelfTest& test )
{
	LightEditor& editor = LightEditor::Instance();
	if( editor.IsOpen() )
	{
		common->Printf( "SKIP Open: the editor is open, le_startEasy not checked\n" );
		return;
	}

	const bool startEasy = le_startEasy.GetBool();

	le_startEasy.SetBool( false );
	editor.Open();
	const bool opensHidden = editor.IsOpen() && !editor.EasyModeActive() && !editor.IsShown();
	editor.Close();

	le_startEasy.SetBool( true );
	editor.Open();
	const bool opensEasy = editor.EasyModeActive() && editor.IsShown();
	editor.Close();

	le_startEasy.SetBool( startEasy );
	test.Check( opensHidden && opensEasy && !editor.IsOpen(), "Open: le_startEasy 0 shows nothing until a pick, 1 opens easy mode" );
}

#endif // !ID_RETAIL

} //namespace ImGuiTools

#if !defined( ID_RETAIL )
CONSOLE_COMMAND( le_selfTest, "checks the easy-mode light geometry contracts and the map splice", NULL )
{
	ImGuiTools::LightSelfTest test;

	ImGuiTools::SelfTestSplice( test );
	ImGuiTools::SelfTestGeometry( test );
	ImGuiTools::SelfTestColor( test );
	ImGuiTools::SelfTestEmitterKeys( test );
	ImGuiTools::SelfTestEmitterGestures( test );
	ImGuiTools::SelfTestKindSwitch( test );
	ImGuiTools::SelfTestFixBatchB( test );
	ImGuiTools::SelfTestChoicesC( test );
	//puncho start
	ImGuiTools::SelfTestAimStart( test );
	ImGuiTools::SelfTestReviewFixesD( test );
	ImGuiTools::SelfTestEmitterCosts( test );
	//puncho end
	ImGuiTools::SelfTestOpenMode( test );

	common->Printf( "le_selfTest: %d checks, %d passed, %d failed\n", test.run, test.run - test.failed, test.failed );
}
#endif // !ID_RETAIL
