/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013 Robert Beckebans

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
#include "snd_acoustics.h"	//puncho - SB3-T: the command and Render's log hook

idCVar s_noSound( "s_noSound", "0", CVAR_BOOL, "returns NULL for all sounds loaded and does not update the sound rendering" );

#ifdef ID_RETAIL
	idCVar s_useCompression( "s_useCompression", "1", CVAR_BOOL, "Use compressed sound files (mp3/xma)" );
	idCVar s_playDefaultSound( "s_playDefaultSound", "0", CVAR_BOOL, "play a beep for missing sounds" );
	idCVar s_maxSamples( "s_maxSamples", "5", CVAR_INTEGER, "max samples to load per shader" );
#else
	idCVar s_useCompression( "s_useCompression", "1", CVAR_BOOL, "Use compressed sound files (mp3/xma)" );
	idCVar s_playDefaultSound( "s_playDefaultSound", "1", CVAR_BOOL, "play a beep for missing sounds" );
	idCVar s_maxSamples( "s_maxSamples", "5", CVAR_INTEGER, "max samples to load per shader" );
#endif

idCVar preLoad_Samples( "preLoad_Samples", "1", CVAR_SYSTEM | CVAR_BOOL, "preload samples during beginlevelload" );

idSoundSystemLocal soundSystemLocal;
idSoundSystem* soundSystem = &soundSystemLocal;

/*
================================================================================================

idSoundSystemLocal

================================================================================================
*/

/*
========================
TestSound_f

This is called from the main thread.
========================
*/
void TestSound_f( const idCmdArgs& args )
{
	if( args.Argc() != 2 )
	{
		idLib::Printf( "Usage: testSound <file>\n" );
		return;
	}
	if( soundSystemLocal.currentSoundWorld )
	{
		soundSystemLocal.currentSoundWorld->PlayShaderDirectly( args.Argv( 1 ) );
	}
}

/*
========================
RestartSound_f
========================
*/
void RestartSound_f( const idCmdArgs& args )
{
	soundSystemLocal.Restart();
}

/*
========================
ListSamples_f

========================
*/
void ListSamples_f( const idCmdArgs& args )
{
	idLib::Printf( "Sound samples\n-------------\n" );
	int totSize = 0;
	for( int i = 0; i < soundSystemLocal.samples.Num(); i++ )
	{
		idLib::Printf( "%05dkb\t%s\n", soundSystemLocal.samples[ i ]->BufferSize() / 1024, soundSystemLocal.samples[ i ]->GetName() );
		totSize += soundSystemLocal.samples[ i ]->BufferSize();
	}
	idLib::Printf( "--------------------------\n" );
	idLib::Printf( "%05dkb total size\n", totSize / 1024 );
}

/*
========================
SmoothSelfTest_Line
========================
*/
static void SmoothSelfTest_Line( int& lines, int& fails, const bool pass, const char* what )
{
	idLib::Printf( "%s  %s\n", pass ? "pass" : "FAIL", what );
	lines++;
	if( !pass )
	{
		fails++;
	}
}

/*
========================
SmoothSelfTest_f

//puncho - R-E6-25 (SEAMS D-12): pure checks of WP-E6+, one line each. The listener lines run on a scratch world that is
never played. No pan-law line: the pan law is compiled out (DR-81)
========================
*/
static void SmoothSelfTest_f( const idCmdArgs& args )
{
	extern idCVar s_lockListener;
	extern idCVar s_smoothCutDistance;
	int lines = 0;
	int fails = 0;

	// R-E6-2: snap, the fraction, the cap, the move, the arrival
	const soundSmoothParms_t noCap = { 0.0f, SMOOTH_NO_CAP, 0.0f };
	const soundSmoothParms_t capped = { 0.0f, 2.0f, 0.0f };
	const soundSmoothParms_t still = { 0.0f, 0.0f, 0.0f };
	const soundSmoothParms_t arriving = { 0.0f, SMOOTH_NO_CAP, 0.05f };
	idSoundSmooth s;
	memset( &s, 0, sizeof( s ) );
	s.Step( 10.0f, 0.5f, noCap, false );
	SmoothSelfTest_Line( lines, fails, s.valid && s.value == 10.0f, "R-E6-2 (1) a smoother not yet valid snaps to its target" );
	s.Step( 0.0f, 0.5f, noCap, true );
	SmoothSelfTest_Line( lines, fails, s.value == 0.0f, "R-E6-2 (1) snap = true lands on the target" );
	s.Step( 10.0f, 0.5f, noCap, false );
	SmoothSelfTest_Line( lines, fails, s.value == 5.0f, "R-E6-2 (2) alpha 0.5 with no cap moves half the gap" );
	s.Snap( 0.0f );
	s.Step( 10.0f, 1.0f, capped, false );
	const bool capUp = ( s.value == 2.0f );
	s.Snap( 0.0f );
	s.Step( -10.0f, 1.0f, capped, false );
	SmoothSelfTest_Line( lines, fails, capUp && s.value == -2.0f, "R-E6-2 (3) maxStep 2 cuts a step of 10 to 2, both ways" );
	s.Snap( 0.0f );
	s.Step( 10.0f, 0.5f, capped, false );
	SmoothSelfTest_Line( lines, fails, s.value == 2.0f, "R-E6-2 (2) before (3): alpha 0.5 of a gap of 10 is 5, cut to 2 (capping first would give 1)" );
	s.Snap( 0.0f );
	s.Step( 10.0f, 1.0f, still, false );
	SmoothSelfTest_Line( lines, fails, s.value == 0.0f, "R-E6-2 (3) maxStep 0 does not move" );
	s.Snap( 0.0f );
	s.Step( 0.08f, 0.5f, arriving, false );
	SmoothSelfTest_Line( lines, fails, s.value == 0.08f, "R-E6-2 (5) within arrive after the move, the value lands on the target" );
	s.Invalidate();
	s.Step( -3.0f, 0.5f, noCap, false );
	SmoothSelfTest_Line( lines, fails, s.value == -3.0f, "R-E6-2 Invalidate makes the next step snap" );

	// R-E6-36: a linear ramp of 60 over 40 ms in steps of 10 ms is alpha 1 and maxStep 60 * 10 / 40 = 15
	soundSmoothParms_t ramp = { 0.0f, 60.0f * 10.0f / 40.0f, 0.05f };
	s.Snap( 0.0f );
	s.Step( -60.0f, 1.0f, ramp, false );
	const bool rampFirst = ( s.value == -15.0f );
	for( int i = 0; i < 3; i++ )
	{
		s.Step( -60.0f, 1.0f, ramp, false );
	}
	SmoothSelfTest_Line( lines, fails, rampFirst && s.value == -60.0f, "R-E6-36 a ramp moves exactly maxStep per step and lands at its end" );
	ramp.maxStep = 60.0f * 0.0f / 40.0f;
	s.Snap( 0.0f );
	s.Step( -60.0f, 1.0f, ramp, false );
	const bool rampStill = ( s.value == 0.0f );
	s.Step( -60.0f, 1.0f, ramp, true );
	SmoothSelfTest_Line( lines, fails, rampStill && s.value == -60.0f, "R-E6-36 dtMs 0 gives maxStep 0, no move; snap still snaps" );

	// Alpha's limits
	const float alpha = idSoundSmooth::Alpha( 10.0f, 60.0f );
	SmoothSelfTest_Line( lines, fails, idSoundSmooth::Alpha( 10.0f, 0.0f ) == 1.0f && idSoundSmooth::Alpha( 10.0f, -5.0f ) == 1.0f,
						 "Alpha is 1 when tauMs <= 0" );
	SmoothSelfTest_Line( lines, fails, idSoundSmooth::Alpha( 0.0f, 60.0f ) == 0.0f && idSoundSmooth::Alpha( -1.0f, 60.0f ) == 0.0f,
						 "Alpha is 0 when dtMs <= 0" );
	SmoothSelfTest_Line( lines, fails, idMath::Fabs( alpha - 0.153518f ) <= 0.00001f, va( "Alpha( 10, 60 ) = 1 - exp( -1/6 ) = %.6f (0.153518)", alpha ) );

	// R-E6-6, R-E6-8, R-E6-19 on a scratch world: no render world, so every placement is in area 0
	if( s_lockListener.GetBool() )
	{
		SmoothSelfTest_Line( lines, fails, false, "R-E6-6, R-E6-8, R-E6-19: s_lockListener is set, the listener lines cannot run" );
	}
	else
	{
		idSoundWorldLocal* scratch = new( TAG_AUDIO ) idSoundWorldLocal;
		scratch->ClearAllSoundEmitters();
		scratch->ListenerCut( LCUT_RESPAWN );
		SmoothSelfTest_Line( lines, fails, scratch->cutPending == 0 && scratch->cutHeld == LCUT_RESPAWN,
							 "R-E6-6 a reason raised after a clear waits for the first placement" );
		scratch->PlaceListener( vec3_origin, mat3_identity, 1 );
		SmoothSelfTest_Line( lines, fails, scratch->cutPending == ( LCUT_LOAD | LCUT_RESPAWN ) && scratch->cutHeld == 0 && !scratch->cutOnNextPlace,
							 "R-E6-6 the first placement after a clear is one cut: LCUT_LOAD with the held reasons" );
		scratch->cutPending = 0;	// stands in for the update that consumes the cut
		// the net's distance as set: half of it is no cut, twice it is
		const float cutDistance = Max( 1.0f, s_smoothCutDistance.GetFloat() );
		scratch->PlaceListener( idVec3( 0.5f * cutDistance, 0.0f, 0.0f ), mat3_identity, 1 );
		const bool shortMove = ( scratch->cutPending == 0 );
		scratch->PlaceListener( idVec3( 2.5f * cutDistance, 0.0f, 0.0f ), mat3_identity, 1 );
		SmoothSelfTest_Line( lines, fails, shortMove && scratch->cutPending == LCUT_DISTANCE,
							 va( "R-E6-8 with s_smoothCutDistance %g: a move of half of it is no cut, a move of twice it is LCUT_DISTANCE", cutDistance ) );
		scratch->lastValidListenerArea = 3;
		scratch->listenerAreaHeld = true;
		scratch->areaFade.Fade( DB_SILENCE, 250, scratch->GetSoundTime() );
		scratch->ClearAllSoundEmitters();
		SmoothSelfTest_Line( lines, fails, scratch->lastValidListenerArea == -1 && !scratch->listenerAreaHeld && scratch->areaFade.fadeEndVolume == 0.0f && scratch->areaFade.fadeEndTime == 0,
							 "R-E6-19 a clear holds no area of the old map and clears areaFade" );
		delete scratch;
	}

	// R-E6-20: distance the outer loop, nearest first; +z, -z, +x, -x, +y, -y for each
	const idVec3 retryDirections[6] = { idVec3( 0.0f, 0.0f, 1.0f ), idVec3( 0.0f, 0.0f, -1.0f ), idVec3( 1.0f, 0.0f, 0.0f ),
										idVec3( -1.0f, 0.0f, 0.0f ), idVec3( 0.0f, 1.0f, 0.0f ), idVec3( 0.0f, -1.0f, 0.0f )
									  };
	bool retryOrder = ( SOUND_SOLID_RETRIES == 18 );
	for( int i = 0; i < SOUND_SOLID_RETRIES && retryOrder; i++ )
	{
		retryOrder = ( soundSolidRetryOffsets[i] == retryDirections[i % 6] * ( float )( 8 << ( i / 6 ) ) );
	}
	SmoothSelfTest_Line( lines, fails, retryOrder, "R-E6-20 18 retries: 8, 16, 32 units, each +z -z +x -x +y -y" );

#if !defined(USE_OPENAL) && defined(_MSC_VER)
	//puncho - R-E6-39 (DR-117, DR-201, DR-203): the one-pole filter runs at SND_MIX_SAMPLE_RATE for every sample rate.
	// The suit's 0.5 is a 2,000 Hz cutoff (coefficient 0.2479); occlusion 0 opens exactly, 99,999 Hz does not.
	const float mixRate = ( float )SND_MIX_SAMPLE_RATE;
	const int sampleRates[3] = { 11025, 22050, 44100 };
	for( int i = 0; i < 3; i++ )
	{
		const float suit = SoundFilterFrequency( 1000.0f / 0.5f, mixRate );
		const bool opens = SoundFilterFrequency( 1000.0f / Max( 0.01f, 0.0f ), mixRate ) == XAUDIO2_MAX_FILTER_FREQUENCY;
		const bool below = SoundFilterFrequency( 100000.0f - 1.0f, mixRate ) < XAUDIO2_MAX_FILTER_FREQUENCY;
		SmoothSelfTest_Line( lines, fails, idMath::Fabs( suit - 0.2479f ) <= 0.00005f && opens && below,
							 va( "R-E6-39 a %d Hz sample, filtered at SND_MIX_SAMPLE_RATE %d: 2,000 Hz gives %.4f (0.2479); occlusion 0 opens exactly; %g Hz is not open",
								 sampleRates[i], SND_MIX_SAMPLE_RATE, suit, 100000.0f - 1.0f ) );
	}

	// R-E6-15: everything XAudio2 bakes into a source voice must match; the rate may differ
	idWaveFile::waveFmt_t a;
	memset( &a, 0, sizeof( a ) );
	a.basic.formatTag = idWaveFile::FORMAT_PCM;
	a.basic.numChannels = 1;
	a.basic.samplesPerSec = 22050;
	a.basic.avgBytesPerSec = 44100;
	a.basic.blockSize = 2;
	a.basic.bitsPerSample = 16;
	idWaveFile::waveFmt_t b = a;
	b.basic.samplesPerSec = 44100;
	b.basic.avgBytesPerSec = 88200;
	SmoothSelfTest_Line( lines, fails, idSoundVoice_XAudio2::FormatsMatch( a, b ), "R-E6-15 another rate matches" );
	b = a;
	b.basic.formatTag = idWaveFile::FORMAT_ADPCM;
	SmoothSelfTest_Line( lines, fails, !idSoundVoice_XAudio2::FormatsMatch( a, b ), "R-E6-15 another format tag does not match" );
	b = a;
	b.basic.numChannels = 2;
	SmoothSelfTest_Line( lines, fails, !idSoundVoice_XAudio2::FormatsMatch( a, b ), "R-E6-15 another channel count does not match" );
	b = a;
	b.basic.blockSize = 4;
	SmoothSelfTest_Line( lines, fails, !idSoundVoice_XAudio2::FormatsMatch( a, b ), "R-E6-15 another block size does not match" );
	b = a;
	b.basic.bitsPerSample = 8;
	SmoothSelfTest_Line( lines, fails, !idSoundVoice_XAudio2::FormatsMatch( a, b ), "R-E6-15 another sample width does not match" );
	b = a;
	b.extraSize = 2;
	SmoothSelfTest_Line( lines, fails, !idSoundVoice_XAudio2::FormatsMatch( a, b ), "R-E6-15 another extra size does not match" );
	a.basic.formatTag = idWaveFile::FORMAT_ADPCM;
	a.extraSize = 2;
	a.extra.adpcm.samplesPerBlock = 1012;
	b = a;
	b.extra.adpcm.samplesPerBlock = 500;
	SmoothSelfTest_Line( lines, fails, !idSoundVoice_XAudio2::FormatsMatch( a, b ), "R-E6-15 other extra bytes within extraSize do not match" );
	b = a;
	b.extra.adpcm.numCoef = 7;
	SmoothSelfTest_Line( lines, fails, idSoundVoice_XAudio2::FormatsMatch( a, b ), "R-E6-15 bytes beyond extraSize are not compared" );
#else
	idLib::Printf( "R-E6-39, R-E6-15: not the XAudio2 build, no lines\n" );
#endif

	idLib::Printf( "s_smoothSelfTest: %d of %d lines pass\n", lines - fails, lines );
}

/*
========================
idSoundSystemLocal::Restart
========================
*/
void idSoundSystemLocal::Restart()
{

	// Mute all channels in all worlds
	for( int i = 0; i < soundWorlds.Num(); i++ )
	{
		idSoundWorldLocal* sw = soundWorlds[i];
		for( int e = 0; e < sw->emitters.Num(); e++ )
		{
			idSoundEmitterLocal* emitter = sw->emitters[e];
			for( int c = 0; c < emitter->channels.Num(); c++ )
			{
				emitter->channels[c]->Mute();
			}
		}
	}
	// Shutdown sound hardware
	hardware.Shutdown();
	// Reinitialize sound hardware
	if( !s_noSound.GetBool() )
	{
		hardware.Init();
	}

	InitStreamBuffers();
}

/*
========================
idSoundSystemLocal::Init

Initialize the SoundSystem.
========================
*/
void idSoundSystemLocal::Init()
{

	idLib::Printf( "----- Initializing Sound System ------\n" );

	soundTime = Sys_Milliseconds();
	random.SetSeed( soundTime );

	if( !s_noSound.GetBool() )
	{
		hardware.Init();
	}

	InitStreamBuffers();

	//puncho - DR-175: here, outside any level load and before levelSeen, LoadSample marks it never purge and reads it with no mid-game warning
	defaultSample = LoadSample( "_default" );

	cmdSystem->AddCommand( "testSound", TestSound_f, 0, "tests a sound", idCmdSystem::ArgCompletion_SoundName );
	cmdSystem->AddCommand( "s_restart", RestartSound_f, 0, "restart sound system" );
	cmdSystem->AddCommand( "listSamples", ListSamples_f, 0, "lists all loaded sound samples" );
	cmdSystem->AddCommand( "listAcoustics", ListAcoustics_f, 0, "lists baked acoustics [areas|zones|doors|portals]" );	//puncho - SB3-T (R-SB3-17)
	cmdSystem->AddCommand( "s_smoothSelfTest", SmoothSelfTest_f, CMD_FL_SYSTEM, "pure checks of the sound smoother, the listener cut and the voice rules, pass or fail" );	//puncho - R-E6-25

	idLib::Printf( "sound system initialized.\n" );
	idLib::Printf( "--------------------------------------\n" );
}

/*
========================
idSoundSystemLocal::InitStreamBuffers
========================
*/
void idSoundSystemLocal::InitStreamBuffers()
{
	streamBufferMutex.Lock();
	const bool empty = ( bufferContexts.Num() == 0 );
	if( empty )
	{
		bufferContexts.SetNum( MAX_SOUND_BUFFERS );
		for( int i = 0; i < MAX_SOUND_BUFFERS; i++ )
		{
			freeStreamBufferContexts.Append( &( bufferContexts[ i ] ) );
		}
	}
	else
	{
		for( int i = 0; i < activeStreamBufferContexts.Num(); i++ )
		{
			freeStreamBufferContexts.Append( activeStreamBufferContexts[ i ] );
		}
		activeStreamBufferContexts.Clear();
	}
	assert( bufferContexts.Num() == MAX_SOUND_BUFFERS );
	assert( freeStreamBufferContexts.Num() == MAX_SOUND_BUFFERS );
	assert( activeStreamBufferContexts.Num() == 0 );
	streamBufferMutex.Unlock();
}

/*
========================
idSoundSystemLocal::FreeStreamBuffers
========================
*/
void idSoundSystemLocal::FreeStreamBuffers()
{
	streamBufferMutex.Lock();
	bufferContexts.Clear();
	freeStreamBufferContexts.Clear();
	activeStreamBufferContexts.Clear();
	streamBufferMutex.Unlock();
}

/*
========================
idSoundSystemLocal::Shutdown
========================
*/
void idSoundSystemLocal::Shutdown()
{
	SmoothLog_Close();	//puncho - R-E6-24
	AcousticsLog_Close();	//puncho - SB3-T (DR-210): the log survives Restart, but not Shutdown
	hardware.Shutdown();
	FreeStreamBuffers();
	samples.DeleteContents( true );
	defaultSample = NULL;	//puncho - DR-175: deleted with the samples above
	sampleHash.Free();
}

/*
========================
idSoundSystemLocal::ObtainStreamBuffer

Get a stream buffer from the free pool, returns NULL if none are available
========================
*/
idSoundSystemLocal::bufferContext_t* idSoundSystemLocal::ObtainStreamBufferContext()
{
	bufferContext_t* bufferContext = NULL;
	streamBufferMutex.Lock();
	if( freeStreamBufferContexts.Num() != 0 )
	{
		bufferContext = freeStreamBufferContexts[ freeStreamBufferContexts.Num() - 1 ];
		freeStreamBufferContexts.SetNum( freeStreamBufferContexts.Num() - 1 );
		activeStreamBufferContexts.Append( bufferContext );
	}
	streamBufferMutex.Unlock();
	return bufferContext;
}

/*
========================
idSoundSystemLocal::ReleaseStreamBuffer

Releases a stream buffer back to the free pool
========================
*/
void idSoundSystemLocal::ReleaseStreamBufferContext( bufferContext_t* bufferContext )
{
	streamBufferMutex.Lock();
	if( activeStreamBufferContexts.Remove( bufferContext ) )
	{
		freeStreamBufferContexts.Append( bufferContext );
	}
	streamBufferMutex.Unlock();
}

/*
========================
idSoundSystemLocal::AllocSoundWorld
========================
*/
idSoundWorld* idSoundSystemLocal::AllocSoundWorld( idRenderWorld* rw )
{
	idSoundWorldLocal* local = new( TAG_AUDIO ) idSoundWorldLocal;
	local->renderWorld = rw;
	soundWorlds.Append( local );
	return local;
}

/*
========================
idSoundSystemLocal::FreeSoundWorld
========================
*/
void idSoundSystemLocal::FreeSoundWorld( idSoundWorld* sw )
{
	idSoundWorldLocal* local = static_cast<idSoundWorldLocal*>( sw );
	soundWorlds.Remove( local );
	delete local;
}

/*
========================
idSoundSystemLocal::SetPlayingSoundWorld

Specifying NULL will cause silence to be played.
========================
*/
void idSoundSystemLocal::SetPlayingSoundWorld( idSoundWorld* soundWorld )
{
	if( currentSoundWorld == soundWorld )
	{
		return;
	}
	idSoundWorldLocal* oldSoundWorld = currentSoundWorld;

	currentSoundWorld = static_cast<idSoundWorldLocal*>( soundWorld );

	if( oldSoundWorld != NULL )
	{
		oldSoundWorld->Update();
	}
}

/*
========================
idSoundSystemLocal::GetPlayingSoundWorld
========================
*/
idSoundWorld* idSoundSystemLocal::GetPlayingSoundWorld()
{
	return currentSoundWorld;
}

/*
========================
idSoundSystemLocal::Render
========================
*/
void idSoundSystemLocal::Render()
{
	if( needsRestart )
	{
		needsRestart = false;
		Restart();
	}

	SCOPED_PROFILE_EVENT( "SoundSystem::Render" );

	if( currentSoundWorld != NULL )
	{
		currentSoundWorld->Update();
	}

	if( !s_noSound.GetBool() )
	{
		hardware.Update();
	}

	if( s_acousticsLog.GetString()[0] != '\0' || acousticsLog != NULL )	//puncho - SB3-T (R-SB3-31): paused and draining too
	{
		AcousticsLog_Frame();
	}

	// The sound system doesn't use game time or anything like that because the sounds are decoded in real time.
	//puncho - R-E6-29 withdrawn: stays after the update, so StartSound and the voice allocation read one stamp (start offset 0)
	soundTime = Sys_Milliseconds();
}

/*
========================
idSoundSystemLocal::OnReloadSound
========================
*/
void idSoundSystemLocal::OnReloadSound( const idDecl* sound )
{
	for( int i = 0; i < soundWorlds.Num(); i++ )
	{
		soundWorlds[i]->OnReloadSound( sound );
	}
}

/*
========================
idSoundSystemLocal::StopAllSounds
========================
*/
void idSoundSystemLocal::StopAllSounds()
{
	for( int i = 0; i < soundWorlds.Num(); i++ )
	{
		idSoundWorld* sw = soundWorlds[i];
		if( sw )
		{
			sw->StopAllSounds();
		}
	}

	if( !s_noSound.GetBool() )
	{
		hardware.Update();
	}
}

/*
========================
idSoundSystemLocal::GetIXAudio2
========================
*/
void* idSoundSystemLocal::GetIXAudio2() const
{
	// RB begin
#if defined(USE_OPENAL)
	return NULL;
#else
	return ( void* )hardware.GetIXAudio2();
#endif
	// RB end
}

/*
========================
idSoundSystemLocal::GetOpenALDevice
========================
*/
// RB begin
void* idSoundSystemLocal::GetOpenALDevice() const
{
#if defined(USE_OPENAL)
	return ( void* )hardware.GetOpenALDevice();
#else
	return ( void* )hardware.GetIXAudio2();
#endif
}
// RB end

/*
========================
idSoundSystemLocal::SoundTime
========================
*/
int idSoundSystemLocal::SoundTime() const
{
	return soundTime;
}

/*
========================
idSoundSystemLocal::AllocateVoice
========================
*/
idSoundVoice* idSoundSystemLocal::AllocateVoice( const idSoundSample* leadinSample, const idSoundSample* loopingSample )
{
	return hardware.AllocateVoice( leadinSample, loopingSample );
}

/*
========================
idSoundSystemLocal::FreeVoice
========================
*/
void idSoundSystemLocal::FreeVoice( idSoundVoice* voice )
{
	hardware.FreeVoice( voice );
}

/*
========================
idSoundSystemLocal::IsSampleResident

Resident-only lookup: no load or reference side effects.
========================
*/
bool idSoundSystemLocal::IsSampleResident( const char* name ) const
{
	idStrStatic<MAX_OSPATH> canonical = name;
	canonical.ToLower();
	canonical.BackSlashesToSlashes();
	canonical.StripFileExtension();
	const int key = idStr::Hash( canonical );
	for( int i = sampleHash.First( key ); i != -1; i = sampleHash.Next( i ) )
	{
		const idSoundSample* sample = samples[i];
		if( !idStr::Cmp( sample->GetName(), canonical ) )
		{
			return sample->IsLoaded();
		}
	}
	return false;
}

/*
========================
idSoundSystemLocal::LoadSample
========================
*/
idSoundSample* idSoundSystemLocal::LoadSample( const char* name )
{
	DeclMediaLookup();
	idStrStatic< MAX_OSPATH > canonical = name;
	canonical.ToLower();
	canonical.BackSlashesToSlashes();
	canonical.StripFileExtension();
	int hashKey = idStr::Hash( canonical );
	idSoundSample* sample = NULL;	//puncho - R-E6-23: new and hashed-but-unloaded samples share the read below
	for( int i = sampleHash.First( hashKey ); i != -1; i = sampleHash.Next( i ) )
	{
		if( idStr::Cmp( samples[i]->GetName(), canonical ) == 0 )
		{
			samples[i]->SetLevelLoadReferenced();
			//puncho - R-E6-23: an unloaded cache hit must be read when it is first used mid-game.
			if( insideLevelLoad || samples[i]->IsLoaded() )
			{
				return samples[i];
			}
			sample = samples[i];
			break;
		}
	}
	//puncho - R-E6-23: preserve preload-manifest registration for newly allocated samples only.
	const bool newSample = ( sample == NULL );
	if( newSample )
	{
		sample = new( TAG_AUDIO ) idSoundSample;
		sample->SetName( canonical );
		sampleHash.Add( hashKey, samples.Append( sample ) );
	}
	if( !insideLevelLoad )
	{
		//puncho - R-E6-23 (DR-150): every read outside a level load keeps today's never-purge lifetime.
		sample->SetNeverPurge();
		if( levelSeen )
		{
			const int start = Sys_Milliseconds();
			sample->LoadResource();
			const int readMs = Sys_Milliseconds() - start;
			//puncho - C-SB79: one console print, without adding a stored warning to warningList.
			common->Printf( S_COLOR_YELLOW "WARNING: " S_COLOR_RED "sound sample '%s' read mid-game in %d ms; precache it\n", sample->GetName(), readMs );
		}
		else
		{
			sample->LoadResource();
		}
	}
	else
	{
		sample->SetLevelLoadReferenced();
	}

	if( newSample && cvarSystem->GetCVarBool( "fs_buildgame" ) )	//puncho - R-E6-23
	{
		fileSystem->AddSamplePreload( canonical );
	}

	return sample;
}

/*
========================
idSoundSystemLocal::StopVoicesWithSample

A sample is about to be freed, make sure the hardware isn't mixing from it.
========================
*/
void idSoundSystemLocal::StopVoicesWithSample( const idSoundSample* const sample )
{
	for( int w = 0; w < soundWorlds.Num(); w++ )
	{
		idSoundWorldLocal* sw = soundWorlds[w];
		if( sw == NULL )
		{
			continue;
		}
		for( int e = 0; e < sw->emitters.Num(); e++ )
		{
			idSoundEmitterLocal* emitter = sw->emitters[e];
			if( emitter == NULL )
			{
				continue;
			}
			for( int i = 0; i < emitter->channels.Num(); i++ )
			{
				if( emitter->channels[i]->leadinSample == sample || emitter->channels[i]->loopingSample == sample )
				{
					emitter->channels[i]->Mute();
				}
			}
		}
	}
}

/*
========================
idSoundSystemLocal::FreeVoice
========================
*/
cinData_t idSoundSystemLocal::ImageForTime( const int milliseconds, const bool waveform )
{
	cinData_t cd;
	cd.imageY = NULL;
	cd.imageCr = NULL;
	cd.imageCb = NULL;
	cd.image = NULL;
	cd.imageWidth = 0;
	cd.imageHeight = 0;
	cd.status = FMV_IDLE;
	return cd;
}

/*
========================
idSoundSystemLocal::BeginLevelLoad
========================
*/
static int soundLevelLoadCount;

void idSoundSystemLocal::BeginLevelLoad()
{
	soundLevelLoadCount++;
	insideLevelLoad = true;
	for( int i = 0; i < samples.Num(); i++ )
	{
		if( samples[i]->GetNeverPurge() )
		{
			continue;
		}
		if( common->LevelKeepGrace() == 0 )
		{
			samples[i]->FreeData();
		}
		samples[i]->ResetLevelLoadReferenced();
	}
}



/*
========================
idSoundSystemLocal::Preload
========================
*/
void idSoundSystemLocal::Preload( idPreloadManifest& manifest )
{

	idStrStatic< MAX_OSPATH > filename;

	int	start = Sys_Milliseconds();
	int numLoaded = 0;

	idList< preloadSort_t > preloadSort;
	preloadSort.Resize( manifest.NumResources() );
	for( int i = 0; i < manifest.NumResources(); i++ )
	{
		const preloadEntry_s& p = manifest.GetPreloadByIndex( i );
		idResourceCacheEntry rc;
		// FIXME: write these out sorted
		if( p.resType == PRELOAD_SAMPLE )
		{
			if( p.resourceName.Find( "/vo/", false ) >= 0 )
			{
				continue;
			}
			filename  = "generated/";
			filename += p.resourceName;
			filename.SetFileExtension( "idwav" );
			if( fileSystem->GetResourceCacheEntry( filename, rc ) )
			{
				preloadSort_t ps = {};
				ps.idx = i;
				ps.ofs = rc.offset;
				preloadSort.Append( ps );
			}
		}
	}

	preloadSort.SortWithTemplate( idSort_Preload() );

	for( int i = 0; i < preloadSort.Num(); i++ )
	{
		const preloadSort_t& ps = preloadSort[ i ];
		const preloadEntry_s& p = manifest.GetPreloadByIndex( ps.idx );
		filename = p.resourceName;
		filename.Replace( "generated/", "" );
		numLoaded++;
		idSoundSample* sample = LoadSample( filename );
		if( sample != NULL && !sample->IsLoaded() )
		{
			sample->LoadResource();
			sample->SetLevelLoadReferenced();
		}
	}

	int	end = Sys_Milliseconds();
	common->Printf( "%05d sounds preloaded in %5.1f seconds\n", numLoaded, ( end - start ) * 0.001 );
	common->Printf( "----------------------------------------\n" );
}

/*
========================
idSoundSystemLocal::EndLevelLoad
========================
*/
void idSoundSystemLocal::EndLevelLoad()
{
	const uint64 transitionMetricStart = Sys_Microseconds();

	insideLevelLoad = false;

	common->Printf( "----- idSoundSystemLocal::EndLevelLoad -----\n" );
	int		start = Sys_Milliseconds();
	int		keepCount = 0;
	int purgeCount = 0, pinnedCount = 0;
	int		loadCount = 0;

	idList< preloadSort_t > preloadSort;
	preloadSort.Resize( samples.Num() );

	for( int i = 0; i < samples.Num(); i++ )
	{
		common->UpdateLevelLoadPacifier();


		if( samples[i]->GetNeverPurge() )
		{
			continue;
		}
		const bool pinned = samples[i]->IsLoaded() && common->IsLevelAssetPinned( PRELOAD_SAMPLE, samples[i]->GetName() );
		pinnedCount += pinned ? 1 : 0;
		if( samples[i]->GetLevelLoadReferenced() || pinned )
		{
			samples[i]->lastUseLoad = soundLevelLoadCount;
		}
		else if( samples[i]->IsLoaded() && !pinned && soundLevelLoadCount - samples[i]->lastUseLoad >= common->LevelKeepGrace() )
		{
			samples[i]->FreeData();
			purgeCount++;
		}
		if( samples[i]->IsLoaded() )
		{
			keepCount++;
			continue;
		}
		if( samples[i]->GetLevelLoadReferenced() )
		{
			idStrStatic< MAX_OSPATH > filename  = "generated/";
			filename += samples[ i ]->GetName();
			filename.SetFileExtension( "idwav" );
			preloadSort_t ps = {};
			ps.idx = i;
			idResourceCacheEntry rc;
			if( fileSystem->GetResourceCacheEntry( filename, rc ) )
			{
				ps.ofs = rc.offset;
			}
			else
			{
				ps.ofs = 0;
			}
			preloadSort.Append( ps );
			loadCount++;
		}
	}
	preloadSort.SortWithTemplate( idSort_Preload() );
	for( int i = 0; i < preloadSort.Num(); i++ )
	{
		common->UpdateLevelLoadPacifier();


		if( !samples[ preloadSort[ i ].idx ]->IsLoaded() )	//puncho - R-E6-23 (P5-2): the pacifier may have loaded and started this sample
		{
			samples[ preloadSort[ i ].idx ]->LoadResource();
		}
	}
	int	end = Sys_Milliseconds();

	int resident = 0;
	int64 bytes = 0;
	for( int i = 0; i < samples.Num(); i++ )
	{
		if( samples[i]->IsLoaded() )
		{
			resident++;
			bytes += samples[i]->BufferSize();
		}
	}
	common->ReportLevelCache( 2, loadCount, resident, purgeCount, pinnedCount, bytes );
	common->Printf( "%5i sounds loaded in %5.1f seconds\n", loadCount, ( end - start ) * 0.001 );
	common->Printf( "----------------------------------------\n" );
	common->ReportTransitionMetric( TRANSITION_SOUND_US, Sys_Microseconds() - transitionMetricStart );
	levelSeen = common->IsInitialized();	//puncho - R-E6-23 (DR-150): the startup menu's load must not enable warnings
}



/*
========================
idSoundSystemLocal::FreeVoice
========================
*/
void idSoundSystemLocal::PrintMemInfo( MemInfo_t* mi )
{
}

void idSoundSystemLocal::PurgeUnusedLevelCache()
{
	int purged = 0, resident = 0, pins = 0;
	int64 bytes = 0;
	for( int i = 0; i < samples.Num(); i++ )
	{
		idSoundSample* sample = samples[i];
		const bool pinned = sample->IsLoaded() && common->IsLevelAssetPinned( PRELOAD_SAMPLE, sample->GetName() );
		if( sample->IsLoaded() && !sample->GetNeverPurge() && !sample->GetLevelLoadReferenced() && !pinned && soundLevelLoadCount - sample->lastUseLoad >= 1 )
		{
			sample->FreeData();
			purged++;
		}
		pins += pinned ? 1 : 0;
		if( sample->IsLoaded() )
		{
			resident++; bytes += sample->BufferSize();
		}
	}
	common->ReportLevelCache( 2, -1, resident, purged, pins, bytes );
}
