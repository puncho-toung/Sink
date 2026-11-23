/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2012-2016 Robert Beckebans
Copyright (C) 2014-2016 Kot in Action Creative Artel

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

#include "precompiled.h"
#pragma hdrstop

#include "snd_local.h"
#include "snd_occlusion.h"	//puncho - SA3-E (DR-193): E1

#if defined( _MSC_VER )	// #pragma warning is MSVC specific; not on _DEBUG: RelWithDebInfo defines it (/MDd) and inlines (/Ob1)
	#pragma warning( 1 : 4714 )	//puncho - SB3-K (K5-1, R-SB3-24): a force-inlined body left out of line is a defect; report it
#endif

idCVar s_singleEmitter( "s_singleEmitter", "0", CVAR_INTEGER, "mute all sounds but this emitter" );
idCVar s_showStartSound( "s_showStartSound", "0", CVAR_BOOL, "print a message every time a sound starts/stops" );
idCVar s_useOcclusion( "s_useOcclusion", "1", CVAR_BOOL, "Attenuate sounds based on walls" );
idCVar s_centerFractionVO( "s_centerFractionVO", "0.75", CVAR_FLOAT, "Portion of VO sounds routed to the center channel" );

extern idCVar s_playDefaultSound;
extern idCVar s_noSound;

//puncho - R-E6-20: declared in snd_local.h; nearest first, so the area found is the one the speaker sits against
const idVec3 soundSolidRetryOffsets[ SOUND_SOLID_RETRIES ] =
{
	idVec3( 0.0f, 0.0f, 8.0f ), idVec3( 0.0f, 0.0f, -8.0f ), idVec3( 8.0f, 0.0f, 0.0f ), idVec3( -8.0f, 0.0f, 0.0f ), idVec3( 0.0f, 8.0f, 0.0f ), idVec3( 0.0f, -8.0f, 0.0f ),
	idVec3( 0.0f, 0.0f, 16.0f ), idVec3( 0.0f, 0.0f, -16.0f ), idVec3( 16.0f, 0.0f, 0.0f ), idVec3( -16.0f, 0.0f, 0.0f ), idVec3( 0.0f, 16.0f, 0.0f ), idVec3( 0.0f, -16.0f, 0.0f ),
	idVec3( 0.0f, 0.0f, 32.0f ), idVec3( 0.0f, 0.0f, -32.0f ), idVec3( 32.0f, 0.0f, 0.0f ), idVec3( -32.0f, 0.0f, 0.0f ), idVec3( 0.0f, 32.0f, 0.0f ), idVec3( 0.0f, -32.0f, 0.0f )
};

/*
================================================================================================

	idSoundFade

================================================================================================
*/

/*
========================
idSoundFade::Clear
========================
*/
void idSoundFade::Clear()
{
	fadeStartTime = 0;
	fadeEndTime = 0;
	fadeStartVolume = 0.0f;
	fadeEndVolume = 0.0f;
}

/*
========================
idSoundFade::SetVolume
========================
*/
void idSoundFade::SetVolume( float to )
{
	fadeStartVolume = to;
	fadeEndVolume = to;
	fadeStartTime = 0;
	fadeEndTime = 0;
}

/*
========================
idSoundFade::Fade
========================
*/
void idSoundFade::Fade( float to, int length, int soundTime )
{
	int startTime = soundTime;
	// if it is already fading to this volume at this rate, don't change it
	if( fadeEndTime == startTime + length && fadeEndVolume == to )
	{
		return;
	}
	fadeStartVolume = GetVolume( soundTime );
	fadeEndVolume = to;
	fadeStartTime = startTime;
	fadeEndTime = startTime + length;
}

/*
========================
idSoundFade::GetVolume
========================
*/
float idSoundFade::GetVolume( const int soundTime ) const
{
	const float fadeDuration = ( fadeEndTime - fadeStartTime );
	const int currentTime = soundTime;
	const float playTime = ( currentTime - fadeStartTime );
	if( fadeDuration <= 0.0f )
	{
		return fadeEndVolume;
	}
	else if( currentTime >= fadeEndTime )
	{
		return fadeEndVolume;
	}
	else if( currentTime > fadeStartTime )
	{
		return fadeStartVolume + ( fadeEndVolume - fadeStartVolume ) * playTime / fadeDuration;
	}
	else
	{
		return fadeStartVolume;
	}
}

/*
================================================================================================

	idSoundSmooth

================================================================================================
*/

/*
========================
idSoundSmooth::Invalidate
========================
*/
void idSoundSmooth::Invalidate()
{
	valid = false;
}

/*
========================
idSoundSmooth::Snap
========================
*/
void idSoundSmooth::Snap( float target )
{
	value = target;
	valid = true;
}

/*
========================
idSoundSmooth::Step

//puncho - R-E6-2, in this order: snap; the fraction alpha of the gap; the cap; the move; the arrival. With alpha 1 and
maxStep = D * dtMs / R it is R-E6-36's linear ramp: the whole gap cut to maxStep, so the value moves exactly maxStep
========================
*/
float idSoundSmooth::Step( float target, float alpha, const soundSmoothParms_t& p, bool snap )
{
	if( !valid || snap )
	{
		Snap( target );
		return value;
	}
	float delta = ( target - value ) * alpha;
	if( p.maxStep >= 0.0f )
	{
		delta = idMath::ClampFloat( -p.maxStep, p.maxStep, delta );
	}
	value += delta;
	if( idMath::Fabs( target - value ) <= p.arrive )
	{
		value = target;
	}
	return value;
}

/*
========================
idSoundSmooth::Alpha

//puncho - R-E6-2: once per world update per parameter set, never per value
========================
*/
float idSoundSmooth::Alpha( float dtMs, float tauMs )
{
	if( tauMs <= 0.0f )
	{
		return 1.0f;
	}
	if( dtMs <= 0.0f )
	{
		return 0.0f;
	}
	return 1.0f - idMath::Exp( -dtMs / tauMs );
}

/*
========================
idSoundChannel::idSoundChannel
========================
*/
idSoundChannel::idSoundChannel()
{
	emitter = NULL;
	hardwareVoice = NULL;

	startTime = 0;
	endTime = 0;
	leadinSample = NULL;
	loopingSample = NULL;
	logicalChannel = SCHANNEL_ANY;
	allowSlow = false;
	soundShader = NULL;

	volumeFade.Clear();

	volumeDB = DB_SILENCE;
	currentAmplitude = 0.0f;

	//puncho - R-E6-42: all zero, so every smoother snaps on its first step
	excessTargetDB = 0.0f;
	occTarget = 0.0f;
	memset( &excessDB, 0, sizeof( excessDB ) );
	memset( &occlusion, 0, sizeof( occlusion ) );
	memset( panOffset, 0, sizeof( panOffset ) );
}

/*
========================
idSoundChannel::~idSoundChannel
========================
*/
idSoundChannel::~idSoundChannel()
{
}

/*
========================
idSoundChannel::CanMute
Never actually mute VO because we can't restart them precisely enough for lip syncing to not fuck up
========================
*/
bool idSoundChannel::CanMute() const
{
	return true;
}

/*
========================
idSoundChannel::Mute

A muted sound is considered still running, and can restart when a listener
gets close enough.
========================
*/
void idSoundChannel::Mute()
{
	if( hardwareVoice != NULL )
	{
		soundSystemLocal.FreeVoice( hardwareVoice );
		hardwareVoice = NULL;
	}
}

/*
========================
idSoundChannel::IsLooping
========================
*/
bool idSoundChannel::IsLooping() const
{
	return ( parms.soundShaderFlags & SSF_LOOPING ) != 0;
}

/*
========================
idSoundChannel::CheckForCompletion
========================
*/
bool idSoundChannel::CheckForCompletion( int currentTime )
{
	if( leadinSample == NULL )
	{
		return true;
	}
	// endTime of 0 indicates a sound should loop forever
	if( endTime > 0 && endTime < currentTime )
	{
		return true;
	}
	return false;
}

/*
========================
idSoundChannel::UpdateVolumeBody

//puncho - SB3-K (R-SB3-22, R-SB3-24): today's UpdateVolume, one instantiation per arm; the place V1 is SA3's
========================
*/
template< bool gated >
ID_FORCE_INLINE void idSoundChannel::UpdateVolumeBody( int currentTime )
{
	idSoundWorldLocal* soundWorld = emitter->soundWorld;

	volumeDB = DB_SILENCE;
	currentAmplitude = 0.0f;

	if( leadinSample == NULL )
	{
		return;
	}
	if( startTime > currentTime )
	{
		return;
	}
	if( endTime > 0 && endTime < currentTime )
	{
		return;
	}

	// if you don't want to hear all the beeps from missing sounds
	if( leadinSample->IsDefault() && !s_playDefaultSound.GetBool() )
	{
		return;
	}

	bool emitterIsListener = ( emitter->emitterId == soundWorld->listener.id );

	// if it is a private sound, set the volume to zero unless we match the listener.id
	if( parms.soundShaderFlags & SSF_PRIVATE_SOUND )
	{
		if( !emitterIsListener )
		{
			return;
		}
	}
	if( parms.soundShaderFlags & SSF_ANTI_PRIVATE_SOUND )
	{
		if( emitterIsListener )
		{
			return;
		}
	}

	// volume fading
	float fadeDB = parms.volume;
	fadeDB += volumeFade.GetVolume( currentTime );
	fadeDB += soundWorld->volumeFade.GetVolume( currentTime );
	fadeDB += soundWorld->pauseFade.GetVolume( currentTime );
	const float areaDB = soundWorld->areaFade.GetVolume( currentTime );	//puncho - R-E6-19: the listener held outside every area
	fadeDB += areaDB;
	if( parms.soundClass >= 0 && parms.soundClass < SOUND_MAX_CLASSES )
	{
		fadeDB += soundWorld->soundClassFade[parms.soundClass].GetVolume( currentTime );
	}

	bool global = ( parms.soundShaderFlags & SSF_GLOBAL ) != 0;

	//puncho - R-E6-9: only the occlusion excess is smoothed. The straight distance term follows a moving source at once;
	// the excess the portal walk adds over it glides at a bounded rate, so a door glides and an open room has no lag.
	// (a) the straight term; (b) the excess target; then the place V1; (d) the clamp; (e) the step; (f) the sum
	const bool occluded = !global && !emitterIsListener && ( parms.soundShaderFlags & SSF_NO_OCCLUSION ) == 0;
	float straightDB = 0.0f;
	if( !global && !emitterIsListener )
	{
		straightDB = SoundDistanceDB( emitter->directDistance, parms.minDistance, parms.maxDistance );
	}
	excessTargetDB = 0.0f;
	if( occluded && emitter->spatializedDistance != emitter->directDistance )	// equal in the listener's area: excess 0 (V3-12)
	{
		excessTargetDB = SoundDistanceDB( emitter->spatializedDistance, parms.minDistance, parms.maxDistance ) - straightDB;
	}
	// V1: SA3
	if( gated )
	{
		if( occluded )
		{
			excessTargetDB -= emitter->portalLossDb;	//puncho - SA3-E (R-SA3-20): before E6's clamp, floor and step
		}
	}
	excessTargetDB = idMath::ClampFloat( -120.0f, 0.0f, excessTargetDB );
	// R-E6-4 as amended for the gain (V3-4): the excess snaps only on its first step and on a cut (or s_smooth 0), never
	// for want of a voice, so a sound a shut door silenced rises at the cap when the door opens. Below the floor, the
	// excess at which the channel is exactly silent, nothing is heard, so a falling excess waits there and rises from it.
	// A channel silent last update whose target is still at or below the floor has no audible excess: its excess is the
	// floor itself, with no step and no cap, so a fade-in or a quick approach cannot outrun the glide (V3-4, round 3).
	// "Silent last update" is read as no voice at entry: emitter Update resets volumeDB before this runs, and the world
	// loop muted the channel last update when its volumeDB was at or below DB_SILENCE (or gave it no voice)
	const bool excessSnap = soundWorld->smoothSnap || !excessDB.valid;
	const float floorDB = Min( DB_SILENCE - fadeDB - straightDB, 0.0f );
	if( !excessSnap && hardwareVoice == NULL && excessTargetDB <= floorDB )
	{
		excessDB.value = floorDB;
	}
	else
	{
		if( !excessSnap && hardwareVoice == NULL )
		{
			// V3R-3: a silent channel has nothing to glide down from (its stored excess may be an old floor far above
			// the target); a rise still starts from the floor
			excessDB.value = Min( excessDB.value, excessTargetDB );
		}
		excessDB.Step( excessTargetDB, soundWorld->gainAlpha, soundWorld->gainParms, excessSnap );
		if( !excessSnap && excessDB.value < floorDB )
		{
			excessDB.value = floorDB;
		}
	}
	float newVolumeDB = fadeDB + straightDB + excessDB.value;
	if( !excessSnap && excessDB.value <= floorDB )
	{
		newVolumeDB = Min( newVolumeDB, DB_SILENCE );	// exactly silent, not a rounding above it
	}

	// out of range by the straight distance; an occluded channel keeps its voice while its excess glides (R-E6-10).
	// R-E6-19 (V3-11): once areaFade has reached silence the channel is silent, whatever its other fades add
	if( ( !global && !emitterIsListener && emitter->directDistance >= parms.maxDistance ) || areaDB <= DB_SILENCE )
	{
		newVolumeDB = DB_SILENCE;
	}

	if( soundSystemLocal.musicMuted && ( parms.soundShaderFlags & SSF_MUSIC ) != 0 )
	{
		newVolumeDB = DB_SILENCE;
	}

	// store the new volume on the channel
	volumeDB = newVolumeDB;

	// the voice's amplitude, for the shake (R-E6-27: the unread currentVolumeDB is gone)
	if( hardwareVoice != NULL )
	{
		currentAmplitude = hardwareVoice->GetAmplitude();
	}
}

/*
========================
idSoundChannel::UpdateVolume
========================
*/
void idSoundChannel::UpdateVolume( int currentTime )
{
	UpdateVolumeBody< false >( currentTime );
}

/*
========================
idSoundChannel::UpdateVolumeGated

//puncho - SB3-K (R-SB3-22): the baked map's entry point
========================
*/
void idSoundChannel::UpdateVolumeGated( int currentTime )
{
	UpdateVolumeBody< true >( currentTime );
}

/*
========================
idSoundChannel::UpdateHardwareBody

//puncho - SB3-K (R-SB3-22, R-SB3-24): today's UpdateHardware, one instantiation per arm; the place H1 is SA3's and SB4's
========================
*/
template< bool gated >
ID_FORCE_INLINE void idSoundChannel::UpdateHardwareBody( float volumeAdd, int currentTime )
{
	//puncho - R-E6-4: tested before the allocation below; a fresh voice starts at its targets (a shot behind a shut door
	// starts muffled, panned from the portal)
	const bool firstUpdate = ( hardwareVoice == NULL );
	idSoundWorldLocal* soundWorld = emitter->soundWorld;

	if( soundWorld == NULL )
	{
		return;
	}
	if( leadinSample == NULL )
	{
		return;
	}
	if( startTime > currentTime )
	{
		return;
	}
	if( endTime > 0 && endTime < currentTime )
	{
		return;
	}

	// convert volumes from decibels to linear
	float volume = Max( 0.0f, DBtoLinear( volumeDB + volumeAdd ) );

	if( ( parms.soundShaderFlags & SSF_UNCLAMPED ) == 0 )
	{
		volume = Min( 1.0f, volume );
	}

	bool global = ( parms.soundShaderFlags & SSF_GLOBAL ) != 0;
	bool omni = ( parms.soundShaderFlags & SSF_OMNIDIRECTIONAL ) != 0;
	bool emitterIsListener = ( emitter->emitterId == soundWorld->listener.id );

	int startOffset = 0;
	bool issueStart = false;

	if( hardwareVoice == NULL )
	{
		if( volume <= 0.00001f )
		{
			return;
		}

		//puncho start - DR-175: a sample missing (a default) or not loaded when its voice starts plays the noise, named once,
		// instead of a square wave or silence. Here, not at StartSound: a sound started in a level load holds a sample that
		// EndLevelLoad reads before its voice starts. The sample tests come first: a real sound reads no cvar
		idSoundSample* const noise = soundSystemLocal.defaultSample;
		const bool leadinFails = ( leadinSample != noise && ( !leadinSample->IsLoaded() || leadinSample->IsDefault() ) );
		const bool loopingFails = ( loopingSample != NULL && loopingSample != noise && ( !loopingSample->IsLoaded() || loopingSample->IsDefault() ) );
		if( ( leadinFails || loopingFails ) && !soundSystemLocal.insideLevelLoad && s_playDefaultSound.GetBool() )
		{
			//puncho - DR-175 (DR-177): unread but level-load referenced is a sample the load queued (EndLevelLoad's reads, which a
			// carry load renders between): no voice this update, so a later one starts it with its real sample, unswapped
			if( ( leadinFails && !leadinSample->IsLoaded() && leadinSample->GetLevelLoadReferenced() ) || ( loopingFails && !loopingSample->IsLoaded() && loopingSample->GetLevelLoadReferenced() ) )
			{
				return;
			}
			//puncho - DR-175 (DR-178): idLib::Warning prints without developer 1, as the XAudio2 warning this replaces did
			if( loopingFails && loopingSample != leadinSample )	// a looping shader's one sample is both: name it once
			{
				idLib::Warning( "sound sample '%s' %s, playing '%s'", loopingSample->GetName(), loopingSample->IsLoaded() ? "missing" : "not loaded", noise->GetName() );
			}
			if( loopingFails )
			{
				loopingSample = noise;
			}
			if( leadinFails )
			{
				idLib::Warning( "sound sample '%s' %s, playing '%s'", leadinSample->GetName(), leadinSample->IsLoaded() ? "missing" : "not loaded", noise->GetName() );
				leadinSample = noise;
				// StartSound's end came from the old length; endTime 0 is its mark of a channel that loops forever
				if( endTime > 0 )
				{
					endTime = startTime + leadinSample->LengthInMsec() + 100;
				}
			}
		}
		//puncho end

		hardwareVoice = soundSystemLocal.AllocateVoice( leadinSample, loopingSample );

		if( hardwareVoice == NULL )
		{
			return;
		}

		issueStart = true;
		startOffset = currentTime - startTime;
	}

	//puncho - R-E6-11: the portal displacement glides, not the position: a moving source has no lag, a jump between a
	// portal and the origin glides, a head turn is not smoothed. A channel that is not occluded (global, the listener's,
	// SSF_NO_OCCLUSION) pans from its true origin, as its gain already does (E-SA-3)
	const bool snap = soundWorld->smoothSnap || firstUpdate;
	const bool occluded = !global && !emitterIsListener && ( parms.soundShaderFlags & SSF_NO_OCCLUSION ) == 0;
	const idVec3 offsetTarget = ( occluded && !omni ) ? ( emitter->spatializedOrigin - emitter->origin ) : vec3_zero;	// omni: 0 by rule (V3R-7)
	idVec3 offset( vec3_zero );
	if( omni || global || emitterIsListener )
	{
		// placed at the listener whatever the offset: no step (V3-12)
		hardwareVoice->SetPosition( vec3_zero );
	}
	else
	{
		// V3-2, V3R-4: no path this update, as the emitter update recorded it (noPath). The voice is kept while its excess
		// glides down, so its offset holds instead of swinging to the true origin
		if( emitter->noPath && !snap && panOffset[0].valid )
		{
			offset.Set( panOffset[0].value, panOffset[1].value, panOffset[2].value );
		}
		else
		{
			for( int k = 0; k < 3; k++ )
			{
				offset[k] = panOffset[k].Step( offsetTarget[k], soundWorld->panAlpha, soundWorld->panParms, snap );
			}
		}
		hardwareVoice->SetPosition( ( emitter->origin + offset - soundWorld->listener.pos ) * soundWorld->listener.axis.Transpose() );
	}
	if( parms.soundShaderFlags & SSF_VO )
	{
		hardwareVoice->SetCenterChannel( s_centerFractionVO.GetFloat() );
	}
	else
	{
		hardwareVoice->SetCenterChannel( 0.0f );
	}

	extern idCVar timescale;

	hardwareVoice->SetGain( volume );
	hardwareVoice->SetInnerRadius( parms.minDistance * METERS_TO_DOOM );
	hardwareVoice->SetPitch( soundWorld->slowmoSpeed * idMath::ClampFloat( 0.2f, 5.0f, timescale.GetFloat() ) );

	//puncho - R-E6-12, the place H1, one run: the target (0.5 with the enviro suit, else 0), a later lane's line, the
	// step, the set. XAudio2 never ramps a filter change, so the low-pass glides here
	occTarget = soundWorld->enviroSuitActive ? 0.5f : 0.0f;
	//puncho - H1: SA3 takes Max( occTarget, portalOcclusion ) for occluded channels under if ( gated ) below
	// H1: SA3, then SB4
	if( gated )
	{
		if( occluded )
		{
			occTarget = Max( occTarget, emitter->portalOcclusion );	//puncho - SA3-E (R-SA3-20): E6 owns the filter step
		}
		//puncho - R-SB4-5 (SB4-C): the send commits with the start; a voice that does not list the bus returns at once
		if( issueStart )
		{
			hardwareVoice->SetReverbSend( global ? 0.0f : s_acousticReverbSend.GetFloat() );
		}
#if defined( USE_SOUND_MIXER )
		//puncho - C4-R: the whole send uses the cushion-adjusted voice volume, every gated update.
		float roomSend = 0.0f;
		if( !global )
		{
			const float v = volumeDB + volumeAdd;
			const float straight = emitterIsListener ? 0.0f : SoundDistanceDB( emitter->directDistance, parms.minDistance, parms.maxDistance );
			const float w = idMath::ClampFloat( 0.0f, 1.0f, ( v + 60.0f ) / 30.0f );
			roomSend = DBtoLinear( v - ( 1.0f - s_acousticReverbSendDistance.GetFloat() ) * straight * w );
			if( ( parms.soundShaderFlags & SSF_UNCLAMPED ) == 0 ) { roomSend = Min( 1.0f, roomSend ); }
		}
		hardwareVoice->SetReverbSend( roomSend );
#endif
	}
	occlusion.Step( occTarget, soundWorld->filterAlpha, soundWorld->filterParms, snap );
	hardwareVoice->SetOcclusion( occlusion.value );

	if( issueStart )
	{
		hardwareVoice->Start( startOffset, parms.soundShaderFlags | ( parms.shakes == 0.0f ? SSF_NO_FLICKER : 0 ) );
	}
	else
	{
		hardwareVoice->Update();
	}

	//puncho - R-E6-24: E6's own s_smoothLog row, after the voice update; SB3's acoustic V row is written at L1, not here.
	// The world opened the file for this update or left smoothLog NULL
	if( soundWorld->smoothLog != NULL )
	{
		extern idCVar s_volume_dB;
		const float masterDB = soundSystemLocal.muted ? DB_SILENCE : s_volume_dB.GetFloat();	//puncho - R-E6-24: the master has no ramp
		const int written = soundWorld->smoothLog->Printf( "%d,%.3f,%d,%.2f,%d,%d,%.3f,%.3f,%.3f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.4f,%.4f,%d,%d\n",
							currentTime, soundWorld->updateDtMs, soundWorld->listenerCutReasons, masterDB, emitter->index, logicalChannel,
							excessTargetDB, excessDB.value, volumeDB, offsetTarget.x, offsetTarget.y, offsetTarget.z, offset.x, offset.y, offset.z,
							occTarget, occlusion.value, soundWorld->listenerAreaHeld ? 1 : 0, soundWorld->listener.area );
		if( written <= 0 )
		{
			SmoothLog_WriteFailed();	// clears s_smoothLog and every world's smoothLog
		}
	}
}

/*
========================
idSoundChannel::UpdateHardware
========================
*/
void idSoundChannel::UpdateHardware( float volumeAdd, int currentTime )
{
	UpdateHardwareBody< false >( volumeAdd, currentTime );
}

/*
========================
idSoundChannel::UpdateHardwareGated

//puncho - SB3-K (R-SB3-22): the baked map's entry point
========================
*/
void idSoundChannel::UpdateHardwareGated( float volumeAdd, int currentTime )
{
	UpdateHardwareBody< true >( volumeAdd, currentTime );
}

/*
================================================================================================

	idSoundEmitterLocal

================================================================================================
*/

/*
========================
idSoundEmitterLocal::idSoundEmitterLocal
========================
*/
idSoundEmitterLocal::idSoundEmitterLocal()
{
	Init( 0, NULL );
}

/*
========================
idSoundEmitterLocal::~idSoundEmitterLocal
========================
*/
idSoundEmitterLocal::~idSoundEmitterLocal()
{
	assert( channels.Num() == 0 );
}

/*
========================
idSoundEmitterLocal::Clear
========================
*/
void idSoundEmitterLocal::Init( int i, idSoundWorldLocal* sw )
{
	index = i;
	soundWorld = sw;

	// Init should only be called on a freshly constructed sound emitter or in a Reset()
	assert( channels.Num() == 0 );

	canFree = false;
	origin.Zero();
	emitterId = 0;

	directDistance = 0.0f;
	lastValidPortalArea = -1;
	spatializedDistance = 0.0f;
	spatializedOrigin.Zero();
	portalLossDb = 0.0f;		//puncho - SA3-E (R-SA3-19): Reset calls Init too
	portalOcclusion = 0.0f;
	inSolid = false;			//puncho - R-E6-20
	noPath = false;				//puncho - V3R-4
	solidOrigin.Zero();

	memset( &parms, 0, sizeof( parms ) );
}

/*
========================
idSoundEmitterLocal::Reset
========================
*/
void idSoundEmitterLocal::Reset()
{
	for( int i = 0; i < channels.Num(); i++ )
	{
		soundWorld->FreeSoundChannel( channels[i] );
	}
	channels.Clear();
	Init( index, soundWorld );
}

/*
==================
idSoundEmitterLocal::OverrideParms
==================
*/
void idSoundEmitterLocal::OverrideParms( const soundShaderParms_t* base, const soundShaderParms_t* over, soundShaderParms_t* out )
{
	if( !over )
	{
		*out = *base;
		return;
	}
	if( over->minDistance )
	{
		out->minDistance = over->minDistance;
	}
	else
	{
		out->minDistance = base->minDistance;
	}
	if( over->maxDistance )
	{
		out->maxDistance = over->maxDistance;
	}
	else
	{
		out->maxDistance = base->maxDistance;
	}
	if( over->shakes )
	{
		out->shakes = over->shakes;
	}
	else
	{
		out->shakes = base->shakes;
	}
	if( over->volume )
	{
		out->volume = over->volume;
	}
	else
	{
		out->volume = base->volume;
	}
	if( over->soundClass )
	{
		out->soundClass = over->soundClass;
	}
	else
	{
		out->soundClass = base->soundClass;
	}
	out->soundShaderFlags = base->soundShaderFlags | over->soundShaderFlags;
}

/*
========================
idSoundEmitterLocal::CheckForCompletion

Checks to see if any of the channels have completed, removing them as they do

This will also play any postSounds on the same channel as their owner.

Returns true if the emitter should be freed.
========================
*/
bool idSoundEmitterLocal::CheckForCompletion( int currentTime )
{
	for( int i = channels.Num() - 1; i >= 0 ; i-- )
	{
		idSoundChannel* chan = channels[i];

		if( chan->CheckForCompletion( currentTime ) )
		{
			channels.RemoveIndex( i );
			soundWorld->FreeSoundChannel( chan );
		}
	}
	return ( canFree && channels.Num() == 0 );
}

/*
========================
idSoundEmitterLocal::UpdateBody

//puncho - SB3-K (R-SB3-22, R-SB3-24): today's Update, one instantiation per arm; the place E1 is SA3's
========================
*/
template< bool gated >
ID_FORCE_INLINE void idSoundEmitterLocal::UpdateBody( int currentTime )
{
	if( channels.Num() == 0 )
	{
		return;
	}

	directDistance = ( soundWorld->listener.pos - origin ).LengthFast() * DOOM_TO_METERS;

	spatializedDistance = directDistance;
	spatializedOrigin = origin;
	noPath = false;	//puncho - V3R-4: true only when the walk below runs and finds no way

	// Initialize all channels to silence
	for( int i = 0; i < channels.Num(); i++ )
	{
		channels[i]->volumeDB = DB_SILENCE;
	}

	if( s_singleEmitter.GetInteger() > 0 && s_singleEmitter.GetInteger() != index )
	{
		return;
	}
	if( soundWorld->listener.area == -1 )
	{
		// listener is outside the world
		return;
	}
	if( soundSystemLocal.muted || soundWorld != soundSystemLocal.currentSoundWorld )
	{
		return;
	}
	float maxDistance = 0.0f;
	bool maxDistanceValid = false;
	bool useOcclusion = false;
	if( emitterId != soundWorld->listener.id )
	{
		for( int i = 0; i < channels.Num(); i++ )
		{
			idSoundChannel* chan = channels[i];
			if( ( chan->parms.soundShaderFlags & SSF_GLOBAL ) != 0 )
			{
				continue;
			}
			useOcclusion = useOcclusion || ( ( chan->parms.soundShaderFlags & SSF_NO_OCCLUSION ) == 0 );
			maxDistanceValid = true;
			if( maxDistance < channels[i]->parms.maxDistance )
			{
				maxDistance = channels[i]->parms.maxDistance;
			}
		}
	}
	if( maxDistanceValid && directDistance >= maxDistance )
	{
		// too far away to possibly hear it
		return;
	}
	//puncho - R-E6-20: the area block. soundInArea stays -1 for an emitter that does not use occlusion, so the resolve
	// below runs exactly when it did before. A speaker a little inside a wall is placed in the nearest area found around
	// its own origin (never from the listener, so a new map and a loaded save agree); one that finds none plays as today.
	int soundInArea = -1;
	if( useOcclusion && s_useOcclusion.GetBool() && soundWorld->renderWorld != NULL )
	{
		soundInArea = soundWorld->renderWorld->PointInArea( origin );
		if( soundInArea == -1 )
		{
			soundInArea = lastValidPortalArea;
		}
		else
		{
			lastValidPortalArea = soundInArea;
			inSolid = false;
		}
		if( soundInArea == -1 && ( !inSolid || ( origin - solidOrigin ).LengthSqr() > 1.0f ) )
		{
			for( int i = 0; i < SOUND_SOLID_RETRIES; i++ )
			{
				soundInArea = soundWorld->renderWorld->PointInArea( origin + soundSolidRetryOffsets[i] );
				if( soundInArea != -1 )
				{
					break;
				}
			}
			if( soundInArea != -1 )
			{
				lastValidPortalArea = soundInArea;
				inSolid = false;
			}
			else
			{
				if( !inSolid )
				{
					const idSoundShader* shader = channels[0]->soundShader;
					common->DWarning( "sound emitter %d at (%s) is in solid, no area within 32 units; plays direct: %s", index, origin.ToString( 0 ), ( shader != NULL ) ? shader->GetName() : "<no shader>" );
				}
				inSolid = true;
				solidOrigin = origin;
			}
		}
	}

	// E1: SA3
	if( gated )
	{
		soundWorld->occlusion->ResolveEmitter( *this, soundInArea, maxDistance );	//puncho - SA3-E (R-SA3-12 to R-SA3-15)
	}
	else
	{
		// work out virtual origin and distance, which may be from a portal instead of the actual origin
		if( soundInArea != -1 && soundInArea != soundWorld->listener.area )
		{
			spatializedDistance = maxDistance * METERS_TO_DOOM;
			soundWorld->ResolveOrigin( 0, NULL, soundInArea, 0.0f, origin, this );
			noPath = ( spatializedDistance >= maxDistance * METERS_TO_DOOM );	//puncho - V3R-4: ResolveOrigin lowers it only on a path
			spatializedDistance *= DOOM_TO_METERS;
		}
	}

	for( int j = 0; j < channels.Num(); j++ )
	{
		if( gated )
		{
			channels[j]->UpdateVolumeGated( currentTime );
		}
		else
		{
			channels[j]->UpdateVolume( currentTime );
		}
	}

	return;
}

/*
========================
idSoundEmitterLocal::Update
========================
*/
void idSoundEmitterLocal::Update( int currentTime )
{
	UpdateBody< false >( currentTime );
}

/*
========================
idSoundEmitterLocal::UpdateGated

//puncho - SB3-K (R-SB3-22): the baked map's entry point
========================
*/
void idSoundEmitterLocal::UpdateGated( int currentTime )
{
	UpdateBody< true >( currentTime );
}

/*
========================
idSoundEmitterLocal::Index
========================
*/
int idSoundEmitterLocal::Index() const
{
	assert( soundWorld );
	assert( soundWorld->emitters[this->index] == this );

	return index;
}

/*
========================
idSoundEmitterLocal::Free

Doesn't free it until the next update.
========================
*/
void idSoundEmitterLocal::Free( bool immediate )
{
	assert( soundWorld );
	assert( soundWorld->emitters[this->index] == this );

	if( canFree )
	{
		// Double free
		return;
	}

	if( immediate )
	{
		Reset();
	}

	canFree = true;
}

/*
========================
idSoundEmitterLocal::UpdateEmitter
========================
*/
void idSoundEmitterLocal::UpdateEmitter( const idVec3& origin, int listenerId, const soundShaderParms_t* parms )
{
	assert( soundWorld != NULL );
	assert( soundWorld->emitters[this->index] == this );

	this->origin = origin;
	this->emitterId = listenerId;
	this->parms = *parms;
}

/*
========================
idSoundEmitterLocal::StartSound

in most cases play sounds immediately, however
  intercept sounds using SSF_FINITE_SPEED_OF_SOUND
  and schedule them for playback later

return: int	- the length of the started sound in msec.
========================
*/
int idSoundEmitterLocal::StartSound( const idSoundShader* shader, const s_channelType channel, float diversity, int shaderFlags, bool allowSlow )
{
	assert( soundWorld != NULL );
	assert( soundWorld->emitters[this->index] == this );

	if( shader == NULL )
	{
		return 0;
	}

	if( s_noSound.GetBool() )
	{
		return 0;
	}

	int currentTime = soundWorld->GetSoundTime();

	bool showStartSound = s_showStartSound.GetBool();
	if( showStartSound )
	{
		idLib::Printf( "%dms: StartSound(%d:%d): %s: ", currentTime, index, channel, shader->GetName() );
	}

	// build the channel parameters by taking the shader parms and optionally overriding
	soundShaderParms_t	chanParms;
	chanParms = shader->parms;
	OverrideParms( &chanParms, &parms, &chanParms );
	chanParms.soundShaderFlags |= shaderFlags;

	if( shader->entries.Num() == 0 )
	{
		if( showStartSound )
		{
			idLib::Printf( S_COLOR_RED "No Entries\n" );
		}
		return 0;
	}

	// PLAY_ONCE sounds will never be restarted while they are running
	if( chanParms.soundShaderFlags & SSF_PLAY_ONCE )
	{
		for( int i = 0; i < channels.Num(); i++ )
		{
			idSoundChannel* chan = channels[i];
			if( chan->soundShader == shader && !chan->CheckForCompletion( currentTime ) )
			{
				if( showStartSound )
				{
					idLib::Printf( S_COLOR_YELLOW "Not started because of playOnce\n" );
				}
				return 0;
			}
		}
	}

	// never play the same sound twice with the same starting time, even
	// if they are on different channels
	for( int i = 0; i < channels.Num(); i++ )
	{
		idSoundChannel* chan = channels[i];
		if( chan->soundShader == shader && chan->startTime == currentTime && chan->endTime != 1 )
		{
			if( showStartSound )
			{
				idLib::Printf( S_COLOR_RED "Already started this frame\n" );
			}
			return 0;
		}
	}

	// kill any sound that is currently playing on this channel
	if( channel != SCHANNEL_ANY )
	{
		for( int i = 0; i < channels.Num(); i++ )
		{
			idSoundChannel* chan = channels[i];
			if( chan->soundShader && chan->logicalChannel == channel )
			{
				if( showStartSound )
				{
					idLib::Printf( S_COLOR_YELLOW "OVERRIDE %s: ", chan->soundShader->GetName() );
				}
				channels.RemoveIndex( i );
				soundWorld->FreeSoundChannel( chan );
				break;
			}
		}
	}

	idSoundSample* leadinSample = NULL;
	idSoundSample* loopingSample = NULL;

	if( shader->leadin && ( chanParms.soundShaderFlags & SSF_LOOPING ) )
	{
		leadinSample = shader->entries[0];
		loopingSample = shader->entries.Num() > 1 ? shader->entries[1] : NULL;
	}
	else
	{
		if( shader->entries.Num() == 1 )
		{
			leadinSample = shader->entries[0];
		}
		else
		{
			int choice;
			if( chanParms.soundShaderFlags & SSF_NO_DUPS )
			{
				// Don't select the most recently played entry
				int mostRecentTime = 0;
				int mostRecent = 0;
				for( int i = 0; i < shader->entries.Num(); i++ )
				{
					int entryTime = shader->entries[i]->GetLastPlayedTime();
					if( entryTime > mostRecentTime )
					{
						mostRecentTime = entryTime;
						mostRecent = i;
					}
				}
				choice = ( int )( diversity * ( shader->entries.Num() - 1 ) );
				if( choice >= mostRecent )
				{
					choice++;
				}
			}
			else
			{
				// pick a sound from the list based on the passed diversity
				choice = ( int )( diversity * shader->entries.Num() );
			}
			choice = idMath::ClampInt( 0, shader->entries.Num() - 1, choice );
			leadinSample = shader->entries[choice];
			leadinSample->SetLastPlayedTime( soundWorld->GetSoundTime() );
		}
		if( chanParms.soundShaderFlags & SSF_LOOPING )
		{
			loopingSample = leadinSample;
		}
	}

	// set all the channel parameters here,
	// a hardware voice will be allocated next update if the volume is high enough to be audible
	if( channels.Num() == channels.Max() )
	{
		CheckForCompletion( currentTime );	// as a last chance try to release finished sounds here
		if( channels.Num() == channels.Max() )
		{
			if( showStartSound )
			{
				idLib::Printf( S_COLOR_RED "No free emitter channels!\n" );
			}
			return 0;
		}
	}
	idSoundChannel* chan = soundWorld->AllocSoundChannel();
	if( chan == NULL )
	{
		if( showStartSound )
		{
			idLib::Printf( S_COLOR_RED "No free global channels!\n" );
		}
		return 0;
	}
	channels.Append( chan );
	chan->emitter = this;
	chan->parms = chanParms;
	chan->soundShader = shader;
	chan->logicalChannel = channel;
	chan->leadinSample = leadinSample;
	chan->loopingSample = loopingSample;
	chan->allowSlow = allowSlow;

	// return length of sound in milliseconds
	int length = chan->leadinSample->LengthInMsec();

	// adjust the start time based on diversity for looping sounds, so they don't all start at the same point
	int startOffset = 0;

	if( chan->IsLooping() && !shader->leadin )
	{
		// looping sounds start at a random point...
		startOffset = soundSystemLocal.random.RandomInt( length );
	}

	chan->startTime = currentTime - startOffset;

	if( ( chanParms.soundShaderFlags & SSF_LOOPING ) != 0 )
	{
		// This channel will never end!
		chan->endTime = 0;
	}
	else
	{
		// This channel will automatically end at this time
		chan->endTime = chan->startTime + length + 100;
	}
	if( showStartSound )
	{
		if( loopingSample == NULL || leadinSample == loopingSample )
		{
			idLib::Printf( "Playing %s @ %d\n", leadinSample->GetName(), startOffset );
		}
		else
		{
			idLib::Printf( "Playing %s then looping %s\n", leadinSample->GetName(), loopingSample->GetName() );
		}
	}

	return length;
}


/*
========================
idSoundEmitterLocal::OnReloadSound

This is a shortened version of StartSound, called whenever a sound shader is reloaded.
If the emitter is currently playing the given sound shader, restart it so
a change in the sound sample used for a given sound shader will be picked up.
========================
*/
void idSoundEmitterLocal::OnReloadSound( const idDecl* decl )
{
}

/*
========================
idSoundEmitterLocal::StopSound

Can pass SCHANNEL_ANY.
========================
*/
void idSoundEmitterLocal::StopSound( const s_channelType channel )
{
	assert( soundWorld != NULL );
	assert( soundWorld->emitters[this->index] == this );

	for( int i = 0; i < channels.Num(); i++ )
	{
		idSoundChannel* chan = channels[i];

		if( channel != SCHANNEL_ANY && chan->logicalChannel != channel )
		{
			continue;
		}
		if( s_showStartSound.GetBool() )
		{
			idLib::Printf( "%dms: StopSound(%d:%d): %s\n", soundWorld->GetSoundTime(), index, channel, chan->soundShader->GetName() );
		}

		// This forces CheckForCompletion to return true
		chan->endTime = 1;
	}
}

/*
========================
idSoundEmitterLocal::ModifySound
========================
*/
void idSoundEmitterLocal::ModifySound( const s_channelType channel, const soundShaderParms_t* parms )
{
	assert( soundWorld != NULL );
	assert( soundWorld->emitters[this->index] == this );

	for( int i = channels.Num() - 1; i >= 0; i-- )
	{
		idSoundChannel* chan = channels[i];
		if( channel != SCHANNEL_ANY && chan->logicalChannel != channel )
		{
			continue;
		}

		if( s_showStartSound.GetBool() )
		{
			idLib::Printf( "%dms: ModifySound(%d:%d): %s\n", soundWorld->GetSoundTime(), index, channel, chan->soundShader->GetName() );
		}

		OverrideParms( &chan->parms, parms, &chan->parms );
	}
}

/*
========================
idSoundEmitterLocal::FadeSound
========================
*/
void idSoundEmitterLocal::FadeSound( const s_channelType channel, float to, float over )
{
	assert( soundWorld != NULL );
	assert( soundWorld->emitters[this->index] == this );

	int overMSec = SEC2MS( over );

	for( int i = 0; i < channels.Num(); i++ )
	{
		idSoundChannel* chan = channels[i];

		if( channel != SCHANNEL_ANY && chan->logicalChannel != channel )
		{
			continue;
		}
		if( s_showStartSound.GetBool() )
		{
			idLib::Printf( "%dms: FadeSound(%d:%d): %s to %.2fdb over %.2f seconds\n", soundWorld->GetSoundTime(), index, channel, chan->soundShader->GetName(), to, over );
		}

		// fade it
		chan->volumeFade.Fade( to - chan->parms.volume, overMSec, soundWorld->GetSoundTime() );
	}
}

/*
========================
idSoundEmitterLocal::CurrentlyPlaying
========================
*/
bool idSoundEmitterLocal::CurrentlyPlaying( const s_channelType channel ) const
{

	if( channel == SCHANNEL_ANY )
	{
		return ( channels.Num() > 0 );
	}

	for( int i = 0; i < channels.Num(); ++i )
	{
		if( channels[i] != NULL && channels[i]->logicalChannel == channel )
		{
			if( channels[i]->endTime == 1 )
			{
				return false;
			}
			else
			{
				return true;
			}
		}
	}

	return false;
}

/*
========================
idSoundEmitterLocal::CurrentAmplitude
========================
*/
float idSoundEmitterLocal::CurrentAmplitude()
{
	float amplitude = 0.0f;
	int currentTime = soundWorld->GetSoundTime();
	for( int i = 0; i < channels.Num(); i++ )
	{
		idSoundChannel* chan = channels[i];
		if( chan == NULL || currentTime < chan->startTime || ( chan->endTime > 0 && currentTime >= chan->endTime ) )
		{
			continue;
		}
		int relativeTime = currentTime - chan->startTime;
		int leadinLength = chan->leadinSample->LengthInMsec();
		if( relativeTime < leadinLength )
		{
			amplitude = Max( amplitude, chan->leadinSample->GetAmplitude( relativeTime ) );
		}
		else if( chan->loopingSample != NULL )
		{
			amplitude = Max( amplitude, chan->loopingSample->GetAmplitude( ( relativeTime - leadinLength ) % chan->loopingSample->LengthInMsec() ) );
		}
	}
	return amplitude;
}
