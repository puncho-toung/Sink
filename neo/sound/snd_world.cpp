/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2014-2016 Robert Beckebans
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
#include "snd_acoustics.h"	//puncho - SB3: the destructor deletes the object; SB3-K's body reads it
#include "snd_occlusion.h"	//puncho - SA3-E (DR-193): W1 and the end hook

#if defined( _MSC_VER )	// #pragma warning is MSVC specific; not on _DEBUG: RelWithDebInfo defines it (/MDd) and inlines (/Ob1)
	#pragma warning( 1 : 4714 )	//puncho - SB3-K (K5-1, R-SB3-24): a force-inlined UpdateBody left out of line is a defect; report it
#endif

idCVar s_lockListener( "s_lockListener", "0", CVAR_BOOL, "lock listener updates" );
idCVar s_constantAmplitude( "s_constantAmplitude", "-1", CVAR_FLOAT, "" );
idCVar s_maxEmitterChannels( "s_maxEmitterChannels", "48", CVAR_INTEGER, "Can be set lower than the absolute max of MAX_HARDWARE_VOICES" );
idCVar s_cushionFadeChannels( "s_cushionFadeChannels", "2", CVAR_INTEGER, "Ramp currentCushionDB so this many emitter channels should be silent" );
idCVar s_cushionFadeRate( "s_cushionFadeRate", "60", CVAR_FLOAT, "DB / second change to currentCushionDB" );
idCVar s_cushionFadeLimit( "s_cushionFadeLimit", "-30", CVAR_FLOAT, "Never cushion fade beyond this level" );
idCVar s_cushionFadeOver( "s_cushionFadeOver", "10", CVAR_FLOAT, "DB above s_cushionFadeLimit to start ramp to silence" );
idCVar s_unpauseFadeInTime( "s_unpauseFadeInTime", "250", CVAR_INTEGER, "When unpausing a sound world, milliseconds to fade sounds in over" );
idCVar s_doorDistanceAdd( "s_doorDistanceAdd", "150", CVAR_FLOAT, "reduce sound volume with this distance when going through a door" );
idCVar s_drawSounds( "s_drawSounds", "0", CVAR_INTEGER, "", 0, 2, idCmdSystem::ArgCompletion_Integer<0, 2> );
idCVar s_showVoices( "s_showVoices", "0", CVAR_BOOL, "show active voices" );
idCVar s_transitionCarry( "s_transitionCarry", "0", CVAR_NOCHEAT | CVAR_BOOL, "carry four global speakers; requires com_levelKeep" );

idCVar s_volume_dB( "s_volume_dB", "0", CVAR_ARCHIVE | CVAR_FLOAT, "volume in dB" );

//puncho - R-E6-8, R-E6-19: the listener's distance net and the held area
idCVar s_smoothCutDistance( "s_smoothCutDistance", "1024", CVAR_FLOAT, "a listener move longer than this many units in one placement is a cut" );
idCVar s_smoothAreaHoldMs( "s_smoothAreaHoldMs", "250", CVAR_INTEGER, "ms a listener outside every area keeps its last valid area before areaFade starts" );
idCVar s_smoothAreaFadeMs( "s_smoothAreaFadeMs", "250", CVAR_INTEGER, "ms areaFade takes to reach silence, and to come back in an area" );

//puncho - R-E6-3, R-E6-9 to R-E6-14, R-E6-24, R-E6-38: the smoother's parameters. The gain defaults were measured
// (M-SB0-9): 30 dB/s bounded at 0.75 dB a step takes a 12 dB door in 400 ms at any frame rate down to 40 Hz
idCVar s_smooth( "s_smooth", "1", CVAR_BOOL, "0: every smoothed sound value snaps to its target, as before (A/B)" );
idCVar s_smoothMaxDtMs( "s_smoothMaxDtMs", "100", CVAR_FLOAT, "the measured update time is clamped to this many ms", 1.0f, 1000.0f );
idCVar s_smoothGainTauMs( "s_smoothGainTauMs", "60", CVAR_FLOAT, "time constant in ms of the occlusion excess", 0.0f, 10000.0f );
idCVar s_smoothGainRateDBs( "s_smoothGainRateDBs", "30", CVAR_FLOAT, "largest change of the occlusion excess in dB per second", 1.0f, 10000.0f );
idCVar s_smoothGainMaxStepDB( "s_smoothGainMaxStepDB", "0.75", CVAR_FLOAT, "largest change of the occlusion excess in dB per update", 0.01f, 120.0f );
idCVar s_smoothPanTauMs( "s_smoothPanTauMs", "40", CVAR_FLOAT, "time constant in ms of the portal displacement of the pan", 0.0f, 10000.0f );
idCVar s_smoothFilterTauMs( "s_smoothFilterTauMs", "100", CVAR_FLOAT, "time constant in ms of the low-pass occlusion", 0.0f, 10000.0f );
idCVar s_smoothCutRampMs( "s_smoothCutRampMs", "0", CVAR_INTEGER, "0: a listener cut snaps; else every smoothed value lands linearly over this many ms after a cut", 0.0f, 10000.0f );
idCVar s_smoothLog( "s_smoothLog", "", CVAR_NOCHEAT, "write one row per voiced channel per update to smoothlog/<name>.csv under fs_savepath" );

//puncho - R-E6-37: the s_showVoices overlay handle, at file scope so the world update holds no static state
static idOverlayHandle showVoicesOverlay;

//puncho - R-E6-24: the one open s_smoothLog file, shared by every world, and the name it was opened under
static idFile* smoothLogFile = NULL;
static char smoothLogName[128];

/*
========================
SmoothLog_Open

//puncho - R-E6-24: called only while s_smoothLog is set. Returns the file for this update's rows, opening it on a new
name (the name checked as com_loadTimelineLog's is); NULL, and the cvar cleared, when the name or the open fails
========================
*/
static idFile* SmoothLog_Open( const char* name )
{
	if( smoothLogFile != NULL && idStr::Cmp( name, smoothLogName ) == 0 )
	{
		return smoothLogFile;
	}
	if( smoothLogFile != NULL )
	{
		fileSystem->CloseFile( smoothLogFile );
		smoothLogFile = NULL;
	}
	smoothLogName[0] = '\0';
	if( !ValidDiagnosticName( name ) )
	{
		idLib::Warning( "s_smoothLog requires a plain name" );
		s_smoothLog.SetString( "" );
		return NULL;
	}
	char path[160];
	idStr::snPrintf( path, sizeof( path ), "smoothlog/%s.csv", name );
	smoothLogFile = fileSystem->OpenFileAppend( path, false, "fs_savepath" );	// no flush per row (V3-9)
	if( smoothLogFile == NULL )
	{
		idLib::Warning( "s_smoothLog: could not open %s", path );
		s_smoothLog.SetString( "" );
		return NULL;
	}
	idStr::Copynz( smoothLogName, name, sizeof( smoothLogName ) );
	if( smoothLogFile->Length() == 0 )
	{
		if( smoothLogFile->Printf( "ms,dtMs,cutReasons,masterDB,emitter,channel,excessTargetDB,excessDB,volumeDB,offTx,offTy,offTz,offX,offY,offZ,occTarget,occlusion,held,area\n" ) <= 0 )
		{
			SmoothLog_WriteFailed();
			return NULL;
		}
	}
	return smoothLogFile;
}

/*
========================
SmoothLog_WriteFailed

//puncho - R-E6-24 (V3-9): a row that could not be written stops the log: one console line, the cvar cleared, the file
closed and no world left pointing at it
========================
*/
void SmoothLog_WriteFailed()
{
	idLib::Warning( "s_smoothLog: writing smoothlog/%s.csv failed; the log is off", smoothLogName );
	s_smoothLog.SetString( "" );
	SmoothLog_Close();
}

/*
========================
SmoothLog_Close

//puncho - R-E6-24: at sound shutdown; no world keeps a pointer to the closed file
========================
*/
void SmoothLog_Close()
{
	for( int i = 0; i < soundSystemLocal.soundWorlds.Num(); i++ )
	{
		soundSystemLocal.soundWorlds[i]->smoothLog = NULL;
	}
	if( smoothLogFile != NULL )
	{
		fileSystem->CloseFile( smoothLogFile );
		smoothLogFile = NULL;
	}
	smoothLogName[0] = '\0';
}

/*
========================
idSoundWorldLocal::idSoundWorldLocal
========================
*/
idSoundWorldLocal::idSoundWorldLocal()
{
	memset( transitionEmitters, 0, sizeof( transitionEmitters ) );
	memset( transitionExpire, 0, sizeof( transitionExpire ) );
	volumeFade.Clear();
	for( int i = 0; i < SOUND_MAX_CLASSES; i++ )
	{
		soundClassFade[i].Clear();
	}
	renderWorld = NULL;

	listener.axis.Identity();
	listener.pos.Zero();
	listener.id = -1;
	listener.area = 0;

	shakeAmp = 0.0f;
	currentCushionDB = DB_SILENCE;

	localSound = AllocSoundEmitter();

	pauseFade.Clear();
	pausedTime = 0;
	accumulatedPauseTime = 0;
	isPaused = false;

	slowmoSpeed = 1.0f;
	enviroSuitActive = false;

	//puncho - R-E6-5, R-E6-19
	listenerCut = false;
	listenerCutReasons = 0;
	cutPending = 0;
	cutHeld = 0;
	cutOnNextPlace = false;
	lastListenerPosValid = false;
	lastListenerPos.Zero();
	listenerAreaHeld = false;
	lastValidListenerArea = -1;
	areaHeldTime = 0;
	areaFade.Clear();
	//puncho - R-E6-3, R-E6-24, R-E6-38: the parameter sets are filled at the top of every Update
	smoothLastUpdateUs = 0;
	smoothLog = NULL;
	memset( &gainParms, 0, sizeof( gainParms ) );
	memset( &panParms, 0, sizeof( panParms ) );
	memset( &filterParms, 0, sizeof( filterParms ) );
	updateDtMs = 0.0f;
	gainAlpha = 1.0f;
	panAlpha = 1.0f;
	filterAlpha = 1.0f;
	cutRampLeftMs = 0.0f;
	smoothSnap = true;
	smoothStepsSkipped = false;

	//puncho - SB3 (R-SB3-32): no bake until LoadMapAcoustics passes every check
	acoustics = NULL;
	occlusion = NULL;		//puncho - SA3-E (R-SA3-23)
	acousticsLine[0] = '\0';
	acousticsLoadFile = NULL;		//puncho - SB3-W (W4-3): no load in progress
	acousticsLoadBlock = NULL;
	acousticsLoadScratch = NULL;
	reverb.Init( this );		//puncho - SB4-L (C-SB6m): zeroed; nothing of it runs until a baked map's begin hook
}

/*
========================
idSoundWorldLocal::~idSoundWorldLocal
========================
*/
idSoundWorldLocal::~idSoundWorldLocal()
{
	//puncho start - SB3 (R-SB3-32, DR-80): never through FreeMapAcoustics, whose SB4 end hook may start a drain.
	// Before the emitters are freed, so SA3's zeroing walk still finds them; SB4's detach runs whether or not this world
	// holds a bake, so no reverb owner or drain outlives the world
	if( acoustics != NULL )
	{
		idSoundAcoustics* it = acoustics;
		acoustics = NULL;
		idSoundOcclusion::OnMapAcousticsFreed( *this );	//puncho - SA3-E (R-SA3-23)
		DeleteAcoustics( it );
	}
	ReleaseAcousticsLoad();		//puncho - SB3-W (W4-3): a load an Error unwound and nothing released since
	reverb.Detach();			//puncho - SB4-L (R-SB4-31, D-48): after the object's delete above, of which it reads nothing
	if( soundSystemLocal.acousticsGameWorld == this )
	{
		soundSystemLocal.acousticsGameWorld = NULL;
	}
	if( soundSystemLocal.acousticsLogWorld == this )	//puncho - SB3-T (D-37): the log never follows a deleted world
	{
		soundSystemLocal.acousticsLogWorld = NULL;
	}
	//puncho end

	if( soundSystemLocal.currentSoundWorld == this )
	{
		soundSystemLocal.currentSoundWorld = NULL;
	}

	for( int i = 0; i < emitters.Num(); i++ )
	{
		emitters[i]->Reset();
		emitterAllocator.Free( emitters[i] );
	}

	// Make sure we aren't leaking emitters or channels
	assert( emitterAllocator.GetAllocCount() == 0 );
	assert( channelAllocator.GetAllocCount() == 0 );

	emitterAllocator.Shutdown();
	channelAllocator.Shutdown();

	renderWorld = NULL;
	localSound = NULL;
}

/*
========================
idSoundWorldLocal::AllocSoundEmitter

This is called from the main thread.
========================
*/
idSoundEmitter* idSoundWorldLocal::AllocSoundEmitter()
{
	idSoundEmitterLocal* emitter = emitterAllocator.Alloc();
	emitter->Init( emitters.Append( emitter ), this );
	return emitter;
}

/*
========================
idSoundWorldLocal::AllocSoundChannel
========================
*/
idSoundChannel* idSoundWorldLocal::AllocSoundChannel()
{
	return channelAllocator.Alloc();
}

/*
========================
idSoundWorldLocal::FreeSoundChannel
========================
*/
void idSoundWorldLocal::FreeSoundChannel( idSoundChannel* channel )
{
	channel->Mute();
	channelAllocator.Free( channel );
}

/*
========================
idSoundWorldLocal::CurrentShakeAmplitude
========================
*/
float idSoundWorldLocal::CurrentShakeAmplitude()
{
	if( s_constantAmplitude.GetFloat() >= 0.0f )
	{
		return s_constantAmplitude.GetFloat();
	}
	return shakeAmp;
}

/*
========================
idSoundWorldLocal::PlaceListener
========================
*/
void idSoundWorldLocal::PlaceListener( const idVec3& origin, const idMat3& axis, const int id )
{
	if( s_lockListener.GetBool() )
	{
		return;
	}

	listener.axis = axis;
	listener.pos = origin;
	listener.id = id;

	if( renderWorld )
	{
		listener.area = renderWorld->PointInArea( origin );	// where are we?
	}
	else
	{
		listener.area = 0;
	}

	//puncho - R-E6-6, R-E6-8: the first placement after a clear or a restore is the load's cut, with the reasons
	// raised while it was waiting folded in, and skips the net; any other placement that jumps is a cut by distance
	if( cutOnNextPlace )
	{
		cutPending |= LCUT_LOAD | cutHeld;
		cutHeld = 0;
		cutOnNextPlace = false;
	}
	else if( lastListenerPosValid && ( origin - lastListenerPos ).LengthSqr() > Square( s_smoothCutDistance.GetFloat() ) )
	{
		cutPending |= LCUT_DISTANCE;
	}
	lastListenerPos = origin;
	lastListenerPosValid = true;

	//puncho - R-E6-19: outside every area, keep the last valid one for a moment instead of silencing every emitter
	// (snd_emitter.cpp's listener.area == -1 test); after s_smoothAreaHoldMs areaFade takes the world to silence.
	// A cut clears the hold, so no area from before a jump, a clear or a restore is ever held after it.
	if( cutPending != 0 )
	{
		// a cut puts every value at its target: no hold, no fade back in
		lastValidListenerArea = -1;
		listenerAreaHeld = false;
		areaFade.Clear();
	}
	if( listener.area != -1 )
	{
		lastValidListenerArea = listener.area;
		listenerAreaHeld = false;
		if( areaFade.fadeEndVolume != 0.0f )
		{
			areaFade.Fade( 0.0f, s_smoothAreaFadeMs.GetInteger(), GetSoundTime() );
		}
	}
	else if( lastValidListenerArea != -1 )
	{
		listener.area = lastValidListenerArea;
		const int soundTime = GetSoundTime();	// only here and at the fade back: the ordinary path makes no call
		if( !listenerAreaHeld )
		{
			listenerAreaHeld = true;
			areaHeldTime = soundTime;
		}
		else if( soundTime - areaHeldTime >= s_smoothAreaHoldMs.GetInteger() && areaFade.fadeEndVolume != DB_SILENCE )
		{
			areaFade.Fade( DB_SILENCE, s_smoothAreaFadeMs.GetInteger(), soundTime );
		}
	}
	else
	{
		listenerAreaHeld = false;
	}
}

/*
========================
idSoundWorldLocal::ListenerCut
========================
*/
void idSoundWorldLocal::ListenerCut( int reasons )
{
	//puncho - R-E6-6: while a clear or a restore waits for its first placement, reasons wait with it (cutHeld)
	if( cutOnNextPlace )
	{
		cutHeld |= reasons;
	}
	else
	{
		cutPending |= reasons;
	}
}

/*
========================
MapVolumeFromFadeDB

Ramp down volumes that are close to fadeDB so that fadeDB is DB_SILENCE
========================
*/
float MapVolumeFromFadeDB( const float volumeDB, const float fadeDB )
{
	if( volumeDB <= fadeDB )
	{
		return DB_SILENCE;
	}

	const float fadeOver = s_cushionFadeOver.GetFloat();
	const float fadeFrom = fadeDB + fadeOver;

	if( volumeDB >= fadeFrom )
	{
		// unchanged
		return volumeDB;
	}
	const float fadeFraction = ( volumeDB - fadeDB ) / fadeOver;

	const float mappedDB = DB_SILENCE + ( fadeFrom - DB_SILENCE ) * fadeFraction;
	return mappedDB;
}

/*
========================
AdjustForCushionChannels

In the very common case of having more sounds that would contribute to the
mix than there are available hardware voices, it can be an audible discontinuity
when a channel initially gets a voice or loses a voice.
To avoid this, make sure that the last few hardware voices are mixed with a volume
of zero, so they won't make a difference as they come and go.
It isn't obvious what the exact best volume ramping method should be, just that
it smoothly change frame to frame.
========================
*/
static float AdjustForCushionChannels( const idStaticList< idActiveChannel, MAX_HARDWARE_VOICES >& activeEmitterChannels,
									   const int uncushionedChannels, const float currentCushionDB, const float driftRate )
{

	float	targetCushionDB;
	if( activeEmitterChannels.Num() <= uncushionedChannels )
	{
		// we should be able to hear all of them
		targetCushionDB = DB_SILENCE;
	}
	else
	{
		// we should be able to hear all of them
		targetCushionDB = activeEmitterChannels[uncushionedChannels].channel->volumeDB;
		if( targetCushionDB < DB_SILENCE )
		{
			targetCushionDB = DB_SILENCE;
		}
		else if( targetCushionDB > s_cushionFadeLimit.GetFloat() )
		{
			targetCushionDB = s_cushionFadeLimit.GetFloat();
		}
	}

	// linearly drift the currentTargetCushionDB towards targetCushionDB
	float	driftedDB = currentCushionDB;
	if( driftedDB < targetCushionDB )
	{
		driftedDB += driftRate;
		if( driftedDB > targetCushionDB )
		{
			driftedDB = targetCushionDB;
		}
	}
	else
	{
		driftedDB -= driftRate;
		if( driftedDB < targetCushionDB )
		{
			driftedDB = targetCushionDB;
		}
	}

	// ramp the lower sound volumes down
	for( int i = 0; i < activeEmitterChannels.Num(); i++ )
	{
		idSoundChannel* chan = activeEmitterChannels[i].channel;
		chan->volumeDB = MapVolumeFromFadeDB( chan->volumeDB, driftedDB );
	}

	return driftedDB;
}

/*
========================
idSoundWorldLocal::ListenerPass

//puncho - SB3-K (R-SB3-11, R-SB3-13, R-SB3-28): the gate of this update, or NULL for the ungated arm. Called by Update
only with the published object (current is acoustics). s_acoustics is read once per update while a bake is loaded: 0
frees it here and prints its line. Until the load's own cut (a cut whose reasons hold LCUT_LOAD) the object is not
armed and nothing of the listener or the render world is read, so a respawn cut raised during the load never arms.
Each update that returns the gate reads every double portal's state; statesChanged says whether any byte moved
========================
*/
idSoundAcoustics* idSoundWorldLocal::ListenerPass( idSoundAcoustics* current )
{
	extern idCVar s_acoustics;

	if( s_acoustics.GetInteger() == 0 )
	{
		// the free deletes the object and leaves "off (unloaded)": the line is formatted from the gate first (K5-2: no copy
		// of the name, a buffer the size of the stored line) and written over it after
		char line[ sizeof( acousticsLine ) ];
		idStr::snPrintf( line, sizeof( line ), "acoustics: %s off (s_acoustics 0)", current->GetMapName() );
		FreeMapAcoustics();
		idStr::Copynz( acousticsLine, line, sizeof( acousticsLine ) );
		common->Printf( "%s\n", acousticsLine );
		return NULL;
	}
	if( !current->armed )
	{
		if( !listenerCut || ( listenerCutReasons & LCUT_LOAD ) == 0 )
		{
			return NULL;
		}
		current->armed = true;
	}

	// C-SB5c: one virtual call and a compare per double portal per armed update
	assert( renderWorld != NULL );		// a bake is published only for a world with a render world (R-SB3-4 step 0)
	bool changed = false;
	for( int p = 0; p < current->numPortals; p++ )
	{
		const uint8 state = ( uint8 )renderWorld->GetPortalState( p + 1 );
		if( state != current->states[p] )
		{
			current->states[p] = state;
			changed = true;
		}
	}
	current->statesChanged = changed;
	return current;
}

/*
========================
idSoundWorldLocal::UpdateBody

//puncho - SB3-K (R-SB3-21, R-SB3-23, R-SB3-24): Update's body, moved whole, one instantiation per arm. The template
constant folds in both whether or not the body is inlined, and every line that exists only for a baked map sits
under if( gated ), so UpdateBody< false > is today's code. gate is ListenerPass's result (NULL on the ungated arm);
logRows: the acoustics log follows this world. The places W1 and W2 are the later lanes', L1 is SB3-T's on both arms
========================
*/
template< bool gated >
ID_FORCE_INLINE void idSoundWorldLocal::UpdateBody( idSoundAcoustics* gate, bool logRows )
{
	// ------------------
	// Update emitters
	//
	// Only loop through the list once to avoid extra cache misses
	// ------------------

	// The naming convention is weird here because we reuse the name "channel"
	// An idSoundChannel is a channel on an emitter, which may have an explicit channel assignment or SND_CHANNEL_ANY
	// A hardware channel is a channel from the sound file itself (IE: left, right, LFE)
	// We only allow MAX_HARDWARE_CHANNELS channels, which may wind up being a smaller number of idSoundChannels
	idStaticList< idActiveChannel, MAX_HARDWARE_VOICES > activeEmitterChannels;
	int	maxEmitterChannels = s_maxEmitterChannels.GetInteger() + 1;	// +1 to leave room for insert-before-sort
	if( maxEmitterChannels > MAX_HARDWARE_VOICES )
	{
		maxEmitterChannels = MAX_HARDWARE_VOICES;
	}

	int activeHardwareChannels = 0;
	int	totalHardwareChannels = 0;
	int	totalEmitterChannels = 0;

	int currentTime = GetSoundTime();

	// W1: SB4, then SA3
	if( gated )
	{
		reverb.Update();		//puncho - SB4-L (R-SB4-8 to R-SB4-14, R-SB4-30): arm, lookup, zone, arm walk; declares nothing here (D-46)
		if( gate->IsArmed() )
		{
			occlusion->BeginFrame( *gate, listener.pos, listener.area, ( soundSystemLocal.acousticsLogWorld == this ) ? emitters.Num() : 0 );	//puncho - SA3-E (R-SA3-26): E6's held area, no W1 local
		}
	}

	for( int e = emitters.Num() - 1; e >= 0; e-- )
	{
		// check for freeing a one-shot emitter that is finished playing
		if( !IsCarriedEmitter( emitters[e] ) && emitters[e]->CheckForCompletion( currentTime ) )
		{
			// do a fast list collapse by swapping the last element into
			// the slot we are deleting
			emitters[e]->Reset();
			emitterAllocator.Free( emitters[e] );
			int lastEmitter = emitters.Num() - 1;
			if( e != lastEmitter )
			{
				emitters[e] = emitters[lastEmitter];
				emitters[e]->index = e;
			}
			emitters.SetNum( lastEmitter );
			continue;
		}

		if( gated )
		{
			emitters[e]->UpdateGated( currentTime );
		}
		else
		{
			emitters[e]->Update( currentTime );
		}

		totalEmitterChannels += emitters[e]->channels.Num();

		// sort the active channels into the hardware list
		for( int i = 0; i < emitters[e]->channels.Num(); i++ )
		{
			idSoundChannel* channel = emitters[e]->channels[i];

			// check if this channel contributes at all
			const bool canMute = channel->CanMute();
			if( canMute && channel->volumeDB <= DB_SILENCE )
			{
#if !defined(USE_OPENAL)
				channel->Mute();
				continue;
#endif
			}

			// Calculate the sort key.
			// VO can't be stopped and restarted accurately, so always keep VO channels by adding a large value to the sort key.
			const int sortKey = idMath::Ftoi( channel->volumeDB * 100.0f + ( canMute ? 0.0f : 100000.0f ) );

			// Keep track of the total number of hardware channels.
			// This is done after calculating the sort key to avoid a load-hit-store that
			// would occur when using the sort key in the loop below after the Ftoi above.
			const int sampleChannels = channel->leadinSample->NumChannels();
			totalHardwareChannels += sampleChannels;

			// Find the location to insert this channel based on the sort key.
			int insertIndex = 0;
			for( insertIndex = 0; insertIndex < activeEmitterChannels.Num(); insertIndex++ )
			{
				if( sortKey > activeEmitterChannels[insertIndex].sortKey )
				{
					break;
				}
			}

			// Only insert at the end if there is room.
			if( insertIndex == activeEmitterChannels.Num() )
			{
				// Always leave one spot free in the 'activeEmitterChannels' so there is room to insert sort a potentially louder sound later.
				if( activeEmitterChannels.Num() + 1 >= activeEmitterChannels.Max() || activeHardwareChannels + sampleChannels > MAX_HARDWARE_CHANNELS )
				{
					// We don't have enough voices to play this, so mute it if it was playing.
					channel->Mute();
					continue;
				}
			}

			// We want to insert the sound at this point.
			activeEmitterChannels.Insert( idActiveChannel( channel, sortKey ), insertIndex );
			activeHardwareChannels += sampleChannels;

			// If we are over our voice limit or at our channel limit, mute sounds until it fits.
			// If activeEmitterChannels is full, always remove the last one so there is room to insert sort a potentially louder sound later.
			while( activeEmitterChannels.Num() == maxEmitterChannels || activeHardwareChannels > MAX_HARDWARE_CHANNELS )
			{
				const int indexToRemove = activeEmitterChannels.Num() - 1;
				idSoundChannel* const channelToMute = activeEmitterChannels[ indexToRemove ].channel;
				channelToMute->Mute();
				activeHardwareChannels -= channelToMute->leadinSample->NumChannels();
				activeEmitterChannels.RemoveIndex( indexToRemove );
			}
		}
	}

	//puncho - R-E6-28: the cushion drifts by the measured time, so it keeps its rate below the engine's Hz
	const float secondsThisUpdate = updateDtMs * 0.001f;

	// ------------------
	// In the very common case of having more sounds that would contribute to the
	// mix than there are available hardware voices, it can be an audible discontinuity
	// when a channel initially gets a voice or loses a voice.
	// To avoid this, make sure that the last few hardware voices are mixed with a volume
	// of zero, so they won't make a difference as they come and go.
	// It isn't obvious what the exact best volume ramping method should be, just that
	// it smoothly change frame to frame.
	// ------------------
	const int uncushionedChannels = maxEmitterChannels - s_cushionFadeChannels.GetInteger();
	currentCushionDB = AdjustForCushionChannels( activeEmitterChannels, uncushionedChannels,
					   currentCushionDB, s_cushionFadeRate.GetFloat() * secondsThisUpdate );

	// ------------------
	// Update Hardware
	// ------------------
	shakeAmp = 0.0f;

	idStr showVoiceTable;
	bool showVoices = s_showVoices.GetBool();
	if( showVoices )
	{
#if !defined(USE_OPENAL) && defined(_MSC_VER) //puncho - R-E6-15, R-E6-18: XAudio2's queued buffers over its embedded contexts; created and destroyed source voices
		showVoiceTable.Format( "currentCushionDB: %5.1f  freeVoices: %i zombieVoices: %i buffers:%i/%i voicesCreated: %i voicesDestroyed: %i\nacoustics: %s", currentCushionDB, //puncho - SB3-T (D-7, TL8-3, DR-213)
							   soundSystemLocal.hardware.GetNumFreeVoices(), soundSystemLocal.hardware.GetNumZombieVoices(),
							   soundSystemLocal.hardware.GetNumQueuedBuffers(), soundSystemLocal.hardware.GetNumBufferContexts(),
							   soundSystemLocal.hardware.GetNumVoicesCreated(), soundSystemLocal.hardware.GetNumVoicesDestroyed(), AcousticsState() );
#else
		showVoiceTable.Format( "currentCushionDB: %5.1f  freeVoices: %i zombieVoices: %i buffers:%i/%i\nacoustics: %s", currentCushionDB, //puncho - SB3-T (D-7, TL8-3, DR-213)
							   soundSystemLocal.hardware.GetNumFreeVoices(), soundSystemLocal.hardware.GetNumZombieVoices(),
							   soundSystemLocal.activeStreamBufferContexts.Num(), soundSystemLocal.freeStreamBufferContexts.Num(), AcousticsState() );
#endif
		//puncho - SB3-T (DR-213): keep the suffix out of Format's outgoing argument area; no new Update local.
		showVoiceTable += acoustics != NULL ? ( acoustics->IsArmed() ? ", armed\n" : ", not armed\n" ) : "\n";
	}
	for( int i = 0; i < activeEmitterChannels.Num(); i++ )
	{
		idSoundChannel* chan = activeEmitterChannels[i].channel;
		if( gated )
		{
			chan->UpdateHardwareGated( 0.0f, currentTime );
		}
		else
		{
			chan->UpdateHardware( 0.0f, currentTime );
		}

		if( showVoices )
		{
			idStr voiceLine;
			voiceLine.Format( "%5.1f db [%3i:%2i] %s", chan->volumeDB, chan->emitter->index, chan->logicalChannel, chan->CanMute() ? "" : " <CANT MUTE>\n" );
			idSoundSample* leadinSample = chan->leadinSample;
			idSoundSample* loopingSample = chan->loopingSample;
			if( loopingSample == NULL )
			{
				voiceLine.Append( va( "%ikhz*%i %s\n", leadinSample->SampleRate() / 1000, leadinSample->NumChannels(), leadinSample->GetName() ) );
			}
			else if( loopingSample == leadinSample )
			{
				voiceLine.Append( va( "%ikhz*%i <LOOPING> %s\n", leadinSample->SampleRate() / 1000, leadinSample->NumChannels(), leadinSample->GetName() ) );
			}
			else
			{
				voiceLine.Append( va( "%ikhz*%i %s | %ikhz*%i %s\n", leadinSample->SampleRate() / 1000, leadinSample->NumChannels(), leadinSample->GetName(), loopingSample->SampleRate() / 1000, loopingSample->NumChannels(), loopingSample->GetName() ) );
			}
			showVoiceTable += voiceLine;
		}

		// Calculate shakes
		if( chan->hardwareVoice == NULL )
		{
			continue;
		}

		shakeAmp += chan->parms.shakes * chan->hardwareVoice->GetGain() * chan->currentAmplitude;
	}
	if( showVoices )
	{
		console->PrintOverlay( showVoicesOverlay, JUSTIFY_LEFT, showVoiceTable.c_str() );	//puncho - R-E6-37: file scope
	}

	// W2: SB3-T
	if( gated )
	{
		if( s_acousticsShow.GetInteger() != 0 )	//puncho - SB3-T (D-46): no local in the inlined body
		{
			DrawAcoustics( s_acousticsShow.GetInteger() );	//puncho - SB3-T (OP8-1): no gate pointer kept for W2
		}
	}

	// L1: SB3-T, both arms
	if( logRows )	//puncho - SB3-T (D-46, DR-210): the row scratch stays out of Update's stack
	{
		AcousticsLog_World( activeEmitterChannels, currentTime );	//puncho - SB3-T (OP8-1, DR-211): no local or count argument
	}
}

/*
========================
idSoundWorldLocal::Update
========================
*/
void idSoundWorldLocal::Update()
{
	//puncho - R-E6-3: the measured time since this world's last update, clamped; 0 on its first. Every smoother's step
	// and the cushion's drift use it, so a glide takes the same time at any frame rate. Signed, so a clock that steps
	// back gives 0
	const uint64 nowUs = Sys_Microseconds();
	updateDtMs = 0.0f;
	if( smoothLastUpdateUs != 0 )
	{
		updateDtMs = idMath::ClampFloat( 0.0f, s_smoothMaxDtMs.GetFloat(), ( float )( int64 )( nowUs - smoothLastUpdateUs ) * 0.001f );
	}
	smoothLastUpdateUs = nowUs;

	//puncho - R-E6-24: one first-byte test while the log is off; the file is closed on the first update after it empties
	smoothLog = NULL;
	if( s_smoothLog.GetString()[0] != '\0' )
	{
		smoothLog = SmoothLog_Open( s_smoothLog.GetString() );
	}
	else if( smoothLogFile != NULL )
	{
		fileSystem->CloseFile( smoothLogFile );
		smoothLogFile = NULL;
		smoothLogName[0] = '\0';
	}

	//puncho - R-E6-6: the cut raised since the last update is true for this update only; a world that is not the playing
	// one (the old world's extra update in SetPlayingSoundWorld) reports none and leaves the cut for its own next update
	if( soundSystemLocal.currentSoundWorld == this )
	{
		listenerCut = ( cutPending != 0 );
		listenerCutReasons = cutPending;
		cutPending = 0;
	}
	else
	{
		listenerCut = false;
		listenerCutReasons = 0;
	}

	//puncho - R-E6-9, R-E6-11, R-E6-12, R-E6-14: this update's parameter sets, their alphas and whether the steps snap,
	// once for every channel. After the cut block because the snap reads listenerCut. The gain's cap is a rate
	// (s_smoothGainRateDBs) times the measured time, bounded per update (s_smoothGainMaxStepDB)
	gainParms.tauMs = s_smoothGainTauMs.GetFloat();
	gainParms.maxStep = Min( s_smoothGainRateDBs.GetFloat() * updateDtMs * 0.001f, s_smoothGainMaxStepDB.GetFloat() );
	gainParms.arrive = 0.05f;
	panParms.tauMs = s_smoothPanTauMs.GetFloat();
	panParms.maxStep = SMOOTH_NO_CAP;
	panParms.arrive = 0.5f;
	filterParms.tauMs = s_smoothFilterTauMs.GetFloat();
	filterParms.maxStep = SMOOTH_NO_CAP;
	filterParms.arrive = 0.001f;
	gainAlpha = idSoundSmooth::Alpha( updateDtMs, gainParms.tauMs );
	panAlpha = idSoundSmooth::Alpha( updateDtMs, panParms.tauMs );
	filterAlpha = idSoundSmooth::Alpha( updateDtMs, filterParms.tauMs );
	smoothSnap = listenerCut || !s_smooth.GetBool();

	//puncho - R-E6-38: built and off (s_smoothCutRampMs 0, DR-116); off, this test is its whole cost. On, a cut does not
	// snap: for that many ms every value lands linearly, alpha = dtMs / ( dtMs + the ms left after this update)
	if( s_smoothCutRampMs.GetInteger() > 0 )
	{
		bool ramp = listenerCut;
		if( listenerCut )
		{
			cutRampLeftMs = s_smoothCutRampMs.GetFloat();
		}
		else if( cutRampLeftMs > 0.0f )
		{
			cutRampLeftMs = Max( 0.0f, cutRampLeftMs - updateDtMs );
			ramp = true;
		}
		if( ramp )
		{
			const float rampAlpha = ( updateDtMs + cutRampLeftMs > 0.0f ) ? updateDtMs / ( updateDtMs + cutRampLeftMs ) : 1.0f;
			gainAlpha = rampAlpha;
			panAlpha = rampAlpha;
			filterAlpha = rampAlpha;
			gainParms.maxStep = SMOOTH_NO_CAP;
			panParms.maxStep = SMOOTH_NO_CAP;
			filterParms.maxStep = SMOOTH_NO_CAP;
			smoothSnap = !s_smooth.GetBool();
		}
	}
	else
	{
		cutRampLeftMs = 0.0f;
	}

	//puncho - R-E6-4 (V3R-1, V3S-1): every channel's excess snaps on its next step after a cut that snaps, and after an
	// update in which no emitter stepped its gain (the world-wide early returns of the emitter update): nothing was heard
	const bool stepsRun = ( listener.area != -1 && !soundSystemLocal.muted && soundSystemLocal.currentSoundWorld == this );
	const bool resumed = smoothStepsSkipped && stepsRun;
	smoothStepsSkipped = !stepsRun;
	if( ( listenerCut && smoothSnap ) || resumed )
	{
		for( int e = 0; e < emitters.Num(); e++ )
		{
			for( int c = 0; c < emitters[e]->channels.Num(); c++ )
			{
				emitters[e]->channels[c]->excessDB.Invalidate();
			}
		}
	}

	const int now = Sys_Milliseconds();
	for( int i = 0; i < 4; i++ )
	{
		if( transitionEmitters[i] == NULL || transitionExpire[i] == 0 )
		{
			continue;
		}
		if( now >= transitionExpire[i] )
		{
			transitionEmitters[i]->Free( true );
			transitionEmitters[i] = NULL;
		}
		else if( now >= transitionExpire[i] - 500 )
		{
			transitionEmitters[i]->FadeSound( SCHANNEL_ANY, DB_SILENCE, ( transitionExpire[i] - now ) * 0.001f );
		}
	}

	//puncho - SB3-K (R-SB3-21): with no bake, one load and one branch on acoustics (C-SB50) and the log's compare
	// (C-SB51b); ListenerPass returns NULL until the load's cut arms the gate and after a live s_acoustics 0
	const bool logRows = ( soundSystemLocal.acousticsLogWorld == this );
	idSoundAcoustics* gate;
	if( acoustics != NULL && ( gate = ListenerPass( acoustics ) ) != NULL )
	{
		UpdateBody< true >( gate, logRows );
	}
	else
	{
		UpdateBody< false >( NULL, logRows );
	}

	if( s_drawSounds.GetBool() && renderWorld != NULL )
	{
		for( int e = 0; e < emitters.Num(); e++ )
		{
			idSoundEmitterLocal* emitter = emitters[e];
			bool audible = false;
			float maxGain = 0.0f;
			for( int c = 0; c < emitter->channels.Num(); c++ )
			{
				if( emitter->channels[c]->hardwareVoice != NULL )
				{
					audible = true;
					maxGain = Max( maxGain, emitter->channels[c]->hardwareVoice->GetGain() );
				}
			}
			if( !audible )
			{
				continue;
			}

			static const int lifetime = 20;

			idBounds ref;
			ref.Clear();
			ref.AddPoint( idVec3( -10.0f ) );
			ref.AddPoint( idVec3( 10.0f ) );

			// draw a box
			renderWorld->DebugBounds( idVec4( maxGain, maxGain, 1.0f, 1.0f ), ref, emitter->origin, lifetime );
			if( emitter->origin != emitter->spatializedOrigin )
			{
				renderWorld->DebugLine( idVec4( 1.0f, 0.0f, 0.0f, 1.0f ), emitter->origin, emitter->spatializedOrigin, lifetime );
			}

			// draw the index
			idVec3 textPos = emitter->origin;
			textPos.z -= 8;
			renderWorld->DrawText( va( "%i", e ), textPos, 0.1f, idVec4( 1, 0, 0, 1 ), listener.axis, 1, lifetime );
			textPos.z += 8;

			// run through all the channels
			for( int k = 0; k < emitter->channels.Num(); k++ )
			{
				idSoundChannel* chan = emitter->channels[k];
				float	min = chan->parms.minDistance;
				float	max = chan->parms.maxDistance;
				const char* defaulted = chan->leadinSample->IsDefault() ? " *DEFAULTED*" : "";
				idStr text;
				text.Format( "%s (%i %i/%i)%s", chan->soundShader->GetName(), idMath::Ftoi( emitter->spatializedDistance ), idMath::Ftoi( min ), idMath::Ftoi( max ), defaulted );
				renderWorld->DrawText( text, textPos, 0.1f, idVec4( 1, 0, 0, 1 ), listener.axis, 1, lifetime );
				textPos.z += 8;
			}
		}
	}
}

/*
========================
idSoundWorldLocal::OnReloadSound
========================
*/
void idSoundWorldLocal::OnReloadSound( const idDecl* shader )
{
	for( int i = 0; i < emitters.Num(); i++ )
	{
		emitters[i]->OnReloadSound( shader );
	}
}

/*
========================
idSoundWorldLocal::EmitterForIndex
========================
*/
idSoundEmitter* idSoundWorldLocal::EmitterForIndex( int index )
{
	// This is only used by save/load code which assumes index = 0 is invalid
	// Which is fine since we use index 0 for the local sound emitter anyway
	if( index <= 0 )
	{
		return NULL;
	}
	if( index >= emitters.Num() )
	{
		idLib::Error( "idSoundWorldLocal::EmitterForIndex: %i >= %i", index, emitters.Num() );
	}
	return emitters[index];
}

/*
========================
idSoundWorldLocal::ClearAllSoundEmitters
========================
*/
bool idSoundWorldLocal::IsCarriedEmitter( const idSoundEmitter* emitter ) const
{
	for( int i = 0; i < 4; i++ )
	{
		if( transitionEmitters[i] == emitter )
		{
			return true;
		}
	}
	return false;
}

bool idSoundWorldLocal::CarryEmitter( idSoundEmitter* emitter, const char* shader )
{
	if( !s_transitionCarry.GetBool() || common->GetLevelKeep() < 1 || emitter == NULL || idStr::Length( shader ) >= 256 )
	{
		return false;
	}
	for( int i = 0; i < 4; i++ )
	{
		if( transitionEmitters[i] == emitter )
		{
			return true;
		}
		if( transitionEmitters[i] == NULL )
		{
			transitionEmitters[i] = static_cast<idSoundEmitterLocal*>( emitter );
			idStr::Copynz( transitionShaders[i], shader, sizeof( transitionShaders[i] ) );
			transitionExpire[i] = 0;
			common->Printf( "transition_carried_emitters slot=%d shader=%s channels=%d start_ms=%d sound_ms=%d\n", i, shader, transitionEmitters[i]->channels.Num(), transitionEmitters[i]->channels.Num() ? transitionEmitters[i]->channels[0]->startTime : -1, GetSoundTime() );
			return true;
		}
	}
	common->Warning( "transition speaker cap 4 reached" );
	return false;
}

idSoundEmitter* idSoundWorldLocal::AdoptCarriedEmitter( const char* shader )
{
	if( !common->IsGameDrivenLoad() || !s_transitionCarry.GetBool() )
	{
		return NULL;
	}
	for( int i = 0; i < 4; i++ )
	{
		if( transitionEmitters[i] != NULL && !idStr::Icmp( transitionShaders[i], shader ) )
		{
			idSoundEmitterLocal* result = transitionEmitters[i];
			transitionEmitters[i] = NULL;
			common->Printf( "transition speaker adopted slot=%d shader=%s channels=%d start_ms=%d sound_ms=%d\n", i, shader, result->channels.Num(), result->channels.Num() ? result->channels[0]->startTime : -1, GetSoundTime() );
			return result;
		}
	}
	return NULL;
}

void idSoundWorldLocal::TransitionArrival()
{
	for( int i = 0; i < 4; i++ )
	{
		if( transitionEmitters[i] != NULL && transitionExpire[i] == 0 )
		{
			transitionExpire[i] = Sys_Milliseconds() + 2500;
		}
	}
}

void idSoundWorldLocal::ClearAllSoundEmitters()
{
	if( !common->IsGameDrivenLoad() || !s_transitionCarry.GetBool() || common->GetLevelKeep() < 1 )
	{
		memset( transitionEmitters, 0, sizeof( transitionEmitters ) );
	}
	idSoundEmitterLocal* carried[4];
	int kept = 0;
	for( int i = 0; i < emitters.Num(); i++ )
	{
		if( kept < 4 && IsCarriedEmitter( emitters[i] ) )
		{
			carried[kept++] = emitters[i];
		}
		else
		{
			emitters[i]->Reset();
			emitterAllocator.Free( emitters[i] );
		}
	}
	//puncho - transitions (stage 3 review): localSound first, so it keeps index 0, which save/restore skip as the local
	// emitter (WriteToSaveGame starts at 1, EmitterForIndex( 0 ) is NULL); the carried emitters follow it
	emitters.SetNum( 0 );
	localSound = AllocSoundEmitter();
	for( int i = 0; i < kept; i++ )
	{
		carried[i]->index = emitters.Append( carried[i] );
	}

	//puncho - R-E6-6: the next placement is the new map's cut; nothing of the old map's listener carries over
	cutOnNextPlace = true;
	lastListenerPosValid = false;
	lastValidListenerArea = -1;
	listenerAreaHeld = false;
	areaFade.Clear();
}

/*
========================
idSoundWorldLocal::StopAllSounds

This is called from the main thread.
========================
*/
void idSoundWorldLocal::StopAllSounds()
{
	for( int i = 0; i < emitters.Num(); i++ )
	{
		emitters[i]->Reset();
	}
}

/*
========================
idSoundWorldLocal::PlayShaderDirectly
========================
*/
int idSoundWorldLocal::PlayShaderDirectly( const char* name, int channel )
{
	if( name == NULL || name[0] == 0 )
	{
		localSound->StopSound( channel );
		return 0;
	}
	const idSoundShader* shader = declManager->FindSound( name );
	if( shader == NULL )
	{
		localSound->StopSound( channel );
		return 0;
	}
	else
	{
		return localSound->StartSound( shader, channel, soundSystemLocal.random.RandomFloat(), SSF_GLOBAL, true );
	}
}

/*
========================
idSoundWorldLocal::Skip
========================
*/
void idSoundWorldLocal::Skip( int time )
{
	accumulatedPauseTime -= time;
	pauseFade.SetVolume( DB_SILENCE );
	pauseFade.Fade( 0.0f, s_unpauseFadeInTime.GetInteger(), GetSoundTime() );
}

/*
========================
idSoundWorldLocal::Pause
========================
*/
void idSoundWorldLocal::Pause()
{
	if( !isPaused )
	{
		pausedTime = soundSystemLocal.SoundTime();
		isPaused = true;
		// just pause all unmutable voices (normally just voice overs)
		for( int e = emitters.Num() - 1; e > 0; e-- )
		{
			for( int i = 0; i < emitters[e]->channels.Num(); i++ )
			{
				idSoundChannel* channel = emitters[e]->channels[i];
				if( !channel->CanMute() && channel->hardwareVoice != NULL )
				{
					channel->hardwareVoice->Pause();
				}
			}
		}
	}
}

/*
========================
idSoundWorldLocal::UnPause
========================
*/
void idSoundWorldLocal::UnPause()
{
	if( isPaused )
	{
		isPaused = false;
		accumulatedPauseTime += soundSystemLocal.SoundTime() - pausedTime;
		pauseFade.SetVolume( DB_SILENCE );
		pauseFade.Fade( 0.0f, s_unpauseFadeInTime.GetInteger(), GetSoundTime() );

		// just unpause all unmutable voices (normally just voice overs)
		for( int e = emitters.Num() - 1; e > 0; e-- )
		{
			for( int i = 0; i < emitters[e]->channels.Num(); i++ )
			{
				idSoundChannel* channel = emitters[e]->channels[i];
				if( !channel->CanMute() && channel->hardwareVoice != NULL )
				{
					channel->hardwareVoice->UnPause();
				}
			}
		}
	}
}

/*
========================
idSoundWorldLocal::GetSoundTime
========================
*/
int idSoundWorldLocal::GetSoundTime()
{
	if( isPaused )
	{
		return pausedTime - accumulatedPauseTime;
	}
	else
	{
		return soundSystemLocal.SoundTime() - accumulatedPauseTime;
	}
}

/*
===================
idSoundWorldLocal::ResolveOrigin

Find out of the sound is completely occluded by a closed door portal, or
the virtual sound origin position at the portal closest to the listener.
  this is called by the main thread

dist is the distance from the orignial sound origin to the current portal that enters soundArea
def->distance is the distance we are trying to reduce.

If there is no path through open portals from the sound to the listener, def->spatializedDistance will remain
set at maxDistance
===================
*/
static const int MAX_PORTAL_TRACE_DEPTH = 10;

void idSoundWorldLocal::ResolveOrigin( const int stackDepth, const soundPortalTrace_t* prevStack, const int soundArea, const float dist, const idVec3& soundOrigin, idSoundEmitterLocal* def )
{

	if( dist >= def->spatializedDistance )
	{
		// we can't possibly hear the sound through this chain of portals
		return;
	}

	if( soundArea == listener.area )
	{
		float fullDist = dist + ( soundOrigin - listener.pos ).LengthFast();
		if( fullDist < def->spatializedDistance )
		{
			def->spatializedDistance = fullDist;
			def->spatializedOrigin = soundOrigin;
		}
		return;
	}

	if( stackDepth == MAX_PORTAL_TRACE_DEPTH )
	{
		// don't spend too much time doing these calculations in big maps
		return;
	}

	soundPortalTrace_t newStack;
	newStack.portalArea = soundArea;
	newStack.prevStack = prevStack;

	int numPortals = renderWorld->NumPortalsInArea( soundArea );
	for( int p = 0; p < numPortals; p++ )
	{
		exitPortal_t re = renderWorld->GetPortal( soundArea, p );

		float occlusionDistance = 0;

		// air blocking windows will block sound like closed doors
		if( ( re.blockingBits & ( PS_BLOCK_VIEW | PS_BLOCK_AIR ) ) )
		{
			// we could just completely cut sound off, but reducing the volume works better
			// continue;
			occlusionDistance = s_doorDistanceAdd.GetFloat();
		}

		// what area are we about to go look at
		int otherArea = re.areas[0];
		if( re.areas[0] == soundArea )
		{
			otherArea = re.areas[1];
		}

		// if this area is already in our portal chain, don't bother looking into it
		const soundPortalTrace_t* prev;
		for( prev = prevStack ; prev ; prev = prev->prevStack )
		{
			if( prev->portalArea == otherArea )
			{
				break;
			}
		}
		if( prev )
		{
			continue;
		}

		// pick a point on the portal to serve as our virtual sound origin
		idVec3	source;

		idPlane	pl;
		re.w->GetPlane( pl );

		float	scale;
		idVec3	dir = listener.pos - soundOrigin;
		if( !pl.RayIntersection( soundOrigin, dir, scale ) )
		{
			source = re.w->GetCenter();
		}
		else
		{
			source = soundOrigin + scale * dir;

			// if this point isn't inside the portal edges, slide it in
			for( int i = 0 ; i < re.w->GetNumPoints() ; i++ )
			{
				int j = ( i + 1 ) % re.w->GetNumPoints();
				idVec3	edgeDir = ( *( re.w ) )[j].ToVec3() - ( *( re.w ) )[i].ToVec3();
				idVec3	edgeNormal;

				edgeNormal.Cross( pl.Normal(), edgeDir );

				idVec3	fromVert = source - ( *( re.w ) )[j].ToVec3();

				float d = edgeNormal * fromVert;
				if( d > 0 )
				{
					// move it in
					float div = edgeNormal.Normalize();
					d /= div;

					source -= d * edgeNormal;
				}
			}
		}

		idVec3 tlen = source - soundOrigin;
		float tlenLength = tlen.LengthFast();

		ResolveOrigin( stackDepth + 1, &newStack, otherArea, dist + tlenLength + occlusionDistance, source, def );
	}
}

/*
idSoundWorldLocal::WriteToSaveGame
=================
*/
void idSoundWorldLocal::WriteToSaveGame( idFile* savefile )
{
	struct helper
	{
		static void WriteSoundFade( idFile* savefile, idSoundFade& sf )
		{
			savefile->WriteInt( sf.fadeStartTime );
			savefile->WriteInt( sf.fadeEndTime );
			savefile->WriteFloat( sf.fadeStartVolume );
			savefile->WriteFloat( sf.fadeEndVolume );
		}
		static void WriteShaderParms( idFile* savefile, soundShaderParms_t& parms )
		{
			savefile->WriteFloat( parms.minDistance );
			savefile->WriteFloat( parms.maxDistance );
			savefile->WriteFloat( parms.volume );
			savefile->WriteFloat( parms.shakes );
			savefile->WriteInt( parms.soundShaderFlags );
			savefile->WriteInt( parms.soundClass );
		}
	};
	savefile->WriteInt( GetSoundTime() );

	helper::WriteSoundFade( savefile, volumeFade );
	for( int c = 0; c < SOUND_MAX_CLASSES; c++ )
	{
		helper::WriteSoundFade( savefile, soundClassFade[c] );
	}
	savefile->WriteFloat( slowmoSpeed );
	savefile->WriteBool( enviroSuitActive );

	savefile->WriteMat3( listener.axis );
	savefile->WriteVec3( listener.pos );
	savefile->WriteInt( listener.id );
	savefile->WriteInt( listener.area );

	savefile->WriteFloat( shakeAmp );

	int num = emitters.Num();
	savefile->WriteInt( num );
	// Start at 1 because the local sound emitter is not saved
	for( int e = 1; e < emitters.Num(); e++ )
	{
		idSoundEmitterLocal* emitter = emitters[e];
		//puncho - transitions (stage 3 review): a carried emitter no speaker has adopted belongs only to its transition
		// slot, which is not saved; it is written as a finished one-shot (freeable, no channels) so a restore frees it
		// instead of bringing back an unowned loop with no expiry. Same layout, no version change
		const bool carried = IsCarriedEmitter( emitter );
		const int numChannels = carried ? 0 : emitter->channels.Num();
		savefile->WriteBool( emitter->canFree || carried );
		savefile->WriteVec3( emitter->origin );
		savefile->WriteInt( emitter->emitterId );
		helper::WriteShaderParms( savefile, emitter->parms );
		savefile->WriteInt( numChannels );
		for( int c = 0; c < numChannels; c++ )
		{
			idSoundChannel* channel = emitter->channels[c];
			savefile->WriteInt( channel->startTime );
			savefile->WriteInt( channel->endTime );
			savefile->WriteInt( channel->logicalChannel );
			savefile->WriteBool( channel->allowSlow );
			helper::WriteShaderParms( savefile, channel->parms );
			helper::WriteSoundFade( savefile, channel->volumeFade );
			savefile->WriteString( channel->soundShader->GetName() );
			int leadin = -1;
			int looping = -1;
			for( int i = 0; i < channel->soundShader->entries.Num(); i++ )
			{
				if( channel->soundShader->entries[i] == channel->leadinSample )
				{
					leadin = i;
				}
				if( channel->soundShader->entries[i] == channel->loopingSample )
				{
					looping = i;
				}
			}
			savefile->WriteInt( leadin );
			savefile->WriteInt( looping );
		}
	}
}

/*
=================
idSoundWorldLocal::ReadFromSaveGame
=================
*/
void idSoundWorldLocal::ReadFromSaveGame( idFile* savefile )
{
	struct helper
	{
		static void ReadSoundFade( idFile* savefile, idSoundFade& sf, int timeDelta )
		{
			savefile->ReadInt( sf.fadeStartTime );
			savefile->ReadInt( sf.fadeEndTime );
			savefile->ReadFloat( sf.fadeStartVolume );
			savefile->ReadFloat( sf.fadeEndVolume );
			if( sf.fadeEndTime > 0 )
			{
				sf.fadeStartTime += timeDelta;
				sf.fadeEndTime += timeDelta;
			}
		}
		static void ReadShaderParms( idFile* savefile, soundShaderParms_t& parms )
		{
			savefile->ReadFloat( parms.minDistance );
			savefile->ReadFloat( parms.maxDistance );
			savefile->ReadFloat( parms.volume );
			savefile->ReadFloat( parms.shakes );
			savefile->ReadInt( parms.soundShaderFlags );
			savefile->ReadInt( parms.soundClass );
		}
	};
	int oldSoundTime = 0;
	savefile->ReadInt( oldSoundTime );
	int timeDelta = GetSoundTime() - oldSoundTime;

	helper::ReadSoundFade( savefile, volumeFade, timeDelta );
	for( int c = 0; c < SOUND_MAX_CLASSES; c++ )
	{
		helper::ReadSoundFade( savefile, soundClassFade[c], timeDelta );
	}
	savefile->ReadFloat( slowmoSpeed );
	savefile->ReadBool( enviroSuitActive );

	savefile->ReadMat3( listener.axis );
	savefile->ReadVec3( listener.pos );
	savefile->ReadInt( listener.id );
	savefile->ReadInt( listener.area );

	//puncho - R-E6-6: after the saved area is read; the next placement is the restore's cut (nothing is saved)
	cutOnNextPlace = true;
	lastListenerPosValid = false;
	lastValidListenerArea = -1;

	savefile->ReadFloat( shakeAmp );

	int numEmitters = 0;
	savefile->ReadInt( numEmitters );
	ClearAllSoundEmitters();
	idStr shaderName;
	// Start at 1 because the local sound emitter is not saved
	for( int e = 1; e < numEmitters; e++ )
	{
		idSoundEmitterLocal* emitter = ( idSoundEmitterLocal* )AllocSoundEmitter();
		assert( emitter == emitters[e] );
		assert( emitter->index == e );
		assert( emitter->soundWorld == this );
		assert( emitter->channels.Num() == 0 );
		savefile->ReadBool( emitter->canFree );
		savefile->ReadVec3( emitter->origin );
		savefile->ReadInt( emitter->emitterId );
		helper::ReadShaderParms( savefile, emitter->parms );
		int numChannels = 0;
		savefile->ReadInt( numChannels );
		emitter->channels.SetNum( numChannels );
		for( int c = 0; c < numChannels; c++ )
		{
			idSoundChannel* channel = AllocSoundChannel();
			emitter->channels[c] = channel;
			channel->emitter = emitter;
			savefile->ReadInt( channel->startTime );
			savefile->ReadInt( channel->endTime );
			savefile->ReadInt( channel->logicalChannel );
			savefile->ReadBool( channel->allowSlow );
			helper::ReadShaderParms( savefile, channel->parms );
			helper::ReadSoundFade( savefile, channel->volumeFade, timeDelta );
			savefile->ReadString( shaderName );
			channel->soundShader = declManager->FindSound( shaderName );
			int leadin = 0;
			int looping = 0;
			savefile->ReadInt( leadin );
			savefile->ReadInt( looping );
			// If the leadin entry is not valid (possible if the shader changed after saving) then the looping entry can't be valid either
			if( leadin >= 0 && leadin < channel->soundShader->entries.Num() )
			{
				channel->leadinSample = channel->soundShader->entries[ leadin ];
				if( looping >= 0 && looping < channel->soundShader->entries.Num() )
				{
					channel->loopingSample = channel->soundShader->entries[ looping ];
				}
			}
			else
			{
				channel->leadinSample = NULL;
				channel->loopingSample = NULL;
			}
			channel->startTime += timeDelta;
			if( channel->endTime == 0 )
			{
				// Do nothing, endTime == 0 means loop forever
			}
			else if( channel->endTime <= oldSoundTime )
			{
				// Channel already stopped
				channel->endTime = 1;
			}
			else
			{
				channel->endTime += timeDelta;
			}
		}
	}
}

/*
=================
idSoundWorldLocal::FadeSoundClasses

fade all sounds in the world with a given shader soundClass
to is in Db, over is in seconds
=================
*/
void idSoundWorldLocal::FadeSoundClasses( const int soundClass, const float to, const float over )
{
	if( soundClass < 0 || soundClass >= SOUND_MAX_CLASSES )
	{
		common->Error( "idSoundWorldLocal::FadeSoundClasses: bad soundClass %i", soundClass );
		return;
	}
	soundClassFade[ soundClass ].Fade( to, SEC2MS( over ), GetSoundTime() );
}

/*
=================
idSoundWorldLocal::SetSlowmoSpeed
=================
*/
void idSoundWorldLocal::SetSlowmoSpeed( float speed )
{
	slowmoSpeed = speed;
}

/*
=================
idSoundWorldLocal::SetEnviroSuit
=================
*/
void idSoundWorldLocal::SetEnviroSuit( bool active )
{
	enviroSuitActive = active;
}

void idSoundWorldLocal::TouchTransitionMedia()
{
	for( int i = 0; i < 4; i++ )
	{
		if( transitionEmitters[i] != NULL )
		{
			const_cast<idSoundShader*>( declManager->FindSound( transitionShaders[i] ) )->TouchMedia();
		}
	}
}
