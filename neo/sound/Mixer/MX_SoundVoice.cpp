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
//puncho - C3e-C: adapt the engine voice face to the plain mixer's command.
#pragma hdrstop
#include "precompiled.h"
#if defined( USE_SOUND_MIXER )
#include "../snd_local.h"

idCVar s_skipHardwareSets( "s_skipHardwareSets", "0", CVAR_BOOL, "Do all calculation, but skip mixer command updates" );
idCVar s_debugHardware( "s_debugHardware", "0", CVAR_BOOL, "Print a message any time a hardware voice changes" );

// Same law and SDK limit as XA2_SoundVoice.cpp, which this build does not compile.
float SoundFilterFrequency( float cutoffHz, float filterRate )
{
	if( cutoffHz >= 100000.0f )
	{
		return XAUDIO2_MAX_FILTER_FREQUENCY;
	}
	return idMath::ClampFloat( 0.0f, XAUDIO2_MAX_FILTER_FREQUENCY, 1.0f - idMath::Exp( -idMath::TWO_PI * cutoffHz / filterRate ) );
}

idSoundVoice_Mixer::idSoundVoice_Mixer()
{
	memset( &command, 0, sizeof( command ) );
	memset( &leadinView, 0, sizeof( leadinView ) );
	memset( &loopView, 0, sizeof( loopView ) );
	slot = -1;
	startOffsetMS = 0;
	lastOcclusion = 0.0f;
	noMeter = false;
	started = false;
}

bool idSoundVoice_Mixer::FormatsMatch( const idWaveFile::waveFmt_t& a, const idWaveFile::waveFmt_t& b )
{
	if( a.basic.formatTag != b.basic.formatTag || a.basic.numChannels != b.basic.numChannels
			|| a.basic.blockSize != b.basic.blockSize || a.basic.bitsPerSample != b.basic.bitsPerSample
			|| a.extraSize != b.extraSize )
	{
		return false;
	}
	const size_t extraBytes = Min( ( size_t )a.extraSize, sizeof( a.extra ) );
	return memcmp( &a.extra, &b.extra, extraBytes ) == 0;
}

static void MixerSampleView( const idSoundSample* sample, sndSampleView_t& view )
{
	memset( &view, 0, sizeof( view ) );
	if( sample == NULL || !sample->IsLoaded() || sample->GetView( view ) )
	{
		return;
	}
	const idSoundSample* noise = soundSystemLocal.defaultSample;
	if( sample->MarkNoiseWarning() )
	{
		idLib::Warning( "sound mixer: unsupported sample '%s', playing '%s'", sample->GetName(), noise != NULL ? noise->GetName() : "<no default>" );
	}
	if( noise == NULL || !noise->GetView( view ) )
	{
		memset( &view, 0, sizeof( view ) );
	}
}

void idSoundVoice_Mixer::Create( const idSoundSample* leadinSample, const idSoundSample* loopingSample )
{
	// CO-10: an intervening fence may publish OFF; it must still carry the old views.
	assert( command.state == SND_VOICE_OFF );
	command.state = SND_VOICE_OFF;
	started = false;
	noMeter = false;
	reverbSend = 0.0f;
	MixerSampleView( leadinSample, leadinView );
	if( loopingSample != NULL && loopingSample == leadinSample )
	{
		loopView = leadinView;	// one warning for a looping shader's single sample
	}
	else
	{
		MixerSampleView( loopingSample, loopView );
	}
	if( leadinView.format != SND_SAMPLE_NONE && loopView.format != SND_SAMPLE_NONE &&
		( loopView.format != leadinView.format || loopView.channels != leadinView.channels ) )
	{
		idLib::Warning( "Leadin/looping format mismatch: %s & %s", leadinSample->GetName(), loopingSample->GetName() );
		memset( &loopView, 0, sizeof( loopView ) );
	}
}

void idSoundVoice_Mixer::Start( int offsetMS, int ssFlags )
{
#if defined( _DEBUG )
	soundSystemLocal.hardware.AssertOutsideSampleFree();
#endif
	assert( idLib::IsMainThread() );
	assert( offsetMS >= 0 );
	startOffsetMS = offsetMS;
	if( leadinView.format == SND_SAMPLE_NONE )
	{
		return;
	}
	unsigned int startFrame = MsecToSamples( offsetMS, leadinView.rate );
	bool startInLoop = false;
	if( startFrame >= ( unsigned int )leadinView.numFrames )
	{
		if( loopView.format == SND_SAMPLE_NONE || loopView.numFrames == 0 )
		{
			return;
		}
		startFrame = MsecToSamples( offsetMS - SamplesToMsec( leadinView.numFrames, leadinView.rate ), loopView.rate ) % loopView.numFrames;
		startInLoop = true;
	}

	command.leadin = leadinView;
	command.loop = loopView;
	command.startFrame = startFrame;
	command.startInLoop = startInLoop;
	command.flags = ( ssFlags & ( SSF_GLOBAL | SSF_VO | SSF_MUSIC ) ) != 0 ? SND_VOICEFLAG_PROTECTED : 0;
	command.reserved = 0;
	command.generation = soundSystemLocal.hardware.NextGeneration();
	command.state = SND_VOICE_PLAY;
	noMeter = ( ssFlags & SSF_NO_FLICKER ) != 0;
	started = true;
	command.dryGain = 0.0f;
	memset( command.matrix, 0, sizeof( command.matrix ) );
	Update();
	if( s_debugHardware.GetBool() )
	{
		idLib::Printf( "%dms: mixer slot %d generation %u starting @ %dms\n", Sys_Milliseconds(), slot, command.generation, offsetMS );
	}
}

void idSoundVoice_Mixer::Stop()
{
	if( command.state != SND_VOICE_OFF )
	{
		command.state = SND_VOICE_OFF;
		soundSystemLocal.hardware.NoteStop();
	}
}

void idSoundVoice_Mixer::Pause()
{
	if( started && command.state == SND_VOICE_PLAY )
	{
		command.state = SND_VOICE_PAUSED;
	}
}

void idSoundVoice_Mixer::UnPause()
{
	if( started && command.state == SND_VOICE_PAUSED )
	{
		command.state = SND_VOICE_PLAY;
	}
}

bool idSoundVoice_Mixer::Update()
{
	// CO-27: before a successful Start the command still belongs to the slot's old life.
	if( !started )
	{
		return false;
	}
	if( s_skipHardwareSets.GetBool() )
	{
		return true;
	}
	float matrix[MAX_CHANNELS_PER_VOICE * MAX_CHANNELS_PER_VOICE] = { 0 };
	const int srcChannels = leadinView.channels;
	CalculateSurround( srcChannels, matrix, 1.0f );
	memset( command.matrix, 0, sizeof( command.matrix ) );
	for( int s = 0; s < srcChannels; s++ )
	{
		for( int o = 0; o < dstChannels; o++ )
		{
			command.matrix[s][o] = matrix[o * srcChannels + s];	// CO-1, CO-26
		}
	}
	command.dryGain = gain;
	command.pitch = Min( pitch, 4.0f );
	lastOcclusion = occlusion;
	command.filterCoef = SoundFilterFrequency( 1000.0f / Max( 0.01f, lastOcclusion ), ( float )SND_MIX_SAMPLE_RATE );
	command.sendGain = soundSystemLocal.hardware.ReverbCarrierLive() ? reverbSend : 0.0f; //puncho - C4-R
	return true;
}

void idSoundVoice_Mixer::SetReverbSend( float s )
{
	reverbSend = s;
}

float idSoundVoice_Mixer::GetAmplitude()
{
	if( !started )
	{
		return 0.0f;
	}
	if( noMeter )
	{
		return 1.0f;
	}
	const sndMixVoiceStatus_t& status = soundSystemLocal.hardware.GetStatus().voices[slot];
	return status.generation == command.generation ? status.rms : 0.0f;
}

#endif // USE_SOUND_MIXER
