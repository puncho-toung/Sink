/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

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
#pragma hdrstop
#include "precompiled.h"
#include "../snd_local.h"

idCVar s_skipHardwareSets( "s_skipHardwareSets", "0", CVAR_BOOL, "Do all calculation, but skip XA2 calls" );
idCVar s_debugHardware( "s_debugHardware", "0", CVAR_BOOL, "Print a message any time a hardware voice changes" );

// The whole system runs at this sample rate
static int SYSTEM_SAMPLE_RATE = SND_MIX_SAMPLE_RATE;	//puncho - R-E6-39: one constant (snd_local.h)
static float ONE_OVER_SYSTEM_SAMPLE_RATE = 1.0f / SYSTEM_SAMPLE_RATE;

//puncho - R-E6-39: 0 only. Measured (M-SB0-9), the highest setting below open does not filter, so the other rule is not
// built; the define stays as the marker of that ruling
#define SND_FILTER_TOP_SETTING 0
#if SND_FILTER_TOP_SETTING != 0
	#error "SND_FILTER_TOP_SETTING 1 is not built (R-E6-39)"
#endif

/*
========================
SoundFilterFrequency

//puncho - R-E6-39, DR-201: XAudio2 filters after the resampler, at the input rate of the voice's destination, independent of
the sample's rate. The one-pole coefficient is 1 - exp( -2 pi fc / R ), clamped to [0, 1].
At fc >= 100,000 Hz return exactly XAUDIO2_MAX_FILTER_FREQUENCY for pass-through.
========================
*/
float SoundFilterFrequency( float cutoffHz, float filterRate )
{
	if( cutoffHz >= 100000.0f )	//puncho - DR-201: one compare before the open return
	{
		return XAUDIO2_MAX_FILTER_FREQUENCY;
	}
	return idMath::ClampFloat( 0.0f, XAUDIO2_MAX_FILTER_FREQUENCY, 1.0f - idMath::Exp( -idMath::TWO_PI * cutoffHz / filterRate ) );
}

/*
========================
idStreamingVoiceContext
========================
*/
class idStreamingVoiceContext : public IXAudio2VoiceCallback
{
public:
	STDMETHOD_( void, OnVoiceProcessingPassStart )( UINT32 BytesRequired ) {}
	STDMETHOD_( void, OnVoiceProcessingPassEnd )() {}
	STDMETHOD_( void, OnStreamEnd )() {}
	STDMETHOD_( void, OnBufferStart )( void* pContext )
	{
		// every buffer queued with this callback comes from idSoundVoice_XAudio2::SubmitBuffer: one of the voice's own contexts (R-E6-18)
		xa2BufferContext_t* bufferContext = ( xa2BufferContext_t* ) pContext;
		bufferContext->voice->OnBufferStart( bufferContext->sample, bufferContext->bufferNumber );
	}
	STDMETHOD_( void, OnLoopEnd )( void* ) {}
	STDMETHOD_( void, OnVoiceError )( void*, HRESULT hr )
	{
		idLib::Warning( "OnVoiceError( %d )", hr );
	}
	//puncho - R-E6-18: nothing to release, the contexts are embedded in the voice; no lock on XAudio2's thread
	STDMETHOD_( void, OnBufferEnd )( void* ) {}
} streamContext;

/*
========================
idSoundVoice_XAudio2::idSoundVoice_XAudio2
========================
*/
idSoundVoice_XAudio2::idSoundVoice_XAudio2()
	:	pSourceVoice( NULL ),
	  leadinSample( NULL ),
	  loopingSample( NULL ),
	  sourceVoiceRate( 0 ),
	  sampleRate( 0 ),
	  pitchShared( 1.0f ),
	  occlusionShared( 0.0f ),
	  lastFreqRatio( -1.0f ),
	  lastFilterFreq( -1.0f ),
	  nextContext( 0 ),
	  paused( true ),
	  hasVUMeter( false ),
	  vuMeterAttached( false )
{
	memset( &voiceFormat, 0, sizeof( voiceFormat ) ); //puncho - R-E6-15
	memset( contexts, 0, sizeof( contexts ) ); //puncho - R-E6-18
}

/*
========================
idSoundVoice_XAudio2::~idSoundVoice_XAudio2
========================
*/
idSoundVoice_XAudio2::~idSoundVoice_XAudio2()
{
	DestroyInternal();
}

/*
========================
idSoundVoice_XAudio2::CompatibleFormat
========================
*/
bool idSoundVoice_XAudio2::CompatibleFormat( idSoundSample_XAudio2* s )
{
	if( pSourceVoice == NULL )
	{
		// If this voice has never been allocated, then it's compatible with everything
		return true;
	}
	return FormatsMatch( voiceFormat, s->format ); //puncho - R-E6-15: reuse a created voice of the same format
}

/*
========================
idSoundVoice_XAudio2::FormatsMatch

//puncho - R-E6-15: the rate may differ (Create sets it on reuse); every other field
XAudio2 bakes into a source voice must match, including the format's extra bytes
========================
*/
bool idSoundVoice_XAudio2::FormatsMatch( const idWaveFile::waveFmt_t& a, const idWaveFile::waveFmt_t& b )
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

/*
========================
idSoundVoice_XAudio2::Create
========================
*/
void idSoundVoice_XAudio2::Create( const idSoundSample* leadinSample_, const idSoundSample* loopingSample_ )
{
	if( IsPlaying() )
	{
		// This should never hit
		Stop();
		return;
	}
	leadinSample = ( idSoundSample_XAudio2* )leadinSample_;
	loopingSample = ( idSoundSample_XAudio2* )loopingSample_;

	//puncho - R-E6-15: the voice is not playing here, so the callback does not run and this thread may write the rate
	const uint32 newSampleRate = leadinSample->format.basic.samplesPerSec;
	if( pSourceVoice != NULL && CompatibleFormat( leadinSample ) )
	{
		// reuse: nothing is destroyed, so hasVUMeter and its effect chain survive
		sampleRate.store( newSampleRate, std::memory_order_relaxed );
		if( FAILED( pSourceVoice->SetSourceSampleRate( newSampleRate ) ) )
		{
			// a failed rate set on reuse falls back to destroy and create, as before
			DestroyInternal();
		}
	}
	else
	{
		DestroyInternal();
	}
	if( pSourceVoice == NULL )
	{
		voiceFormat = leadinSample->format;
		sampleRate.store( newSampleRate, std::memory_order_relaxed );

		// the one creation site of a source voice
		//puncho - R-SB4-5 (C-SB65b): the off path's one test. While the carrier is live the voice is made on {master, bus}
		// and its bus matrix cut at once (a new destination gets XAudio2's default matrix, not 0); else today's call
		if( soundSystemLocal.hardware.ReverbCarrierLive() )
		{
			XAUDIO2_SEND_DESCRIPTOR sends[ 2 ] = { { 0, soundSystemLocal.hardware.pMasterVoice }, { 0, soundSystemLocal.hardware.pReverbBus } };
			XAUDIO2_VOICE_SENDS sendList = { 2, sends };
			soundSystemLocal.hardware.pXAudio2->CreateSourceVoice( &pSourceVoice, ( const WAVEFORMATEX* )&leadinSample->format, XAUDIO2_VOICE_USEFILTER, 4.0f, &streamContext, &sendList );
			if( pSourceVoice != NULL )
			{
				ClearReverbSend();
				SetReverbFlags( true, false );
			}
		}
		else
		{
			soundSystemLocal.hardware.pXAudio2->CreateSourceVoice( &pSourceVoice, ( const WAVEFORMATEX* )&leadinSample->format, XAUDIO2_VOICE_USEFILTER, 4.0f, &streamContext );
		}
		if( pSourceVoice == NULL )
		{
			// If this hits, then we are most likely passing an invalid sample format, which should have been caught by the loader (and the sample defaulted)
			return;
		}
		soundSystemLocal.hardware.voicesCreated++;
		pSourceVoice->SetSourceSampleRate( newSampleRate );
		if( s_debugHardware.GetBool() )
		{
			if( loopingSample == NULL || loopingSample == leadinSample )
			{
				idLib::Printf( "%dms: %p created for %s\n", Sys_Milliseconds(), pSourceVoice, leadinSample ? leadinSample->GetName() : "<null>" );
			}
			else
			{
				idLib::Printf( "%dms: %p created for %s and %s\n", Sys_Milliseconds(), pSourceVoice, leadinSample ? leadinSample->GetName() : "<null>", loopingSample ? loopingSample->GetName() : "<null>" );
			}
		}
	}
	sourceVoiceRate = newSampleRate;
	// a reused voice keeps the last start's filter and ratio: the next Update issues both
	lastFreqRatio = -1.0f;
	lastFilterFreq = -1.0f;
	pSourceVoice->SetVolume( 0.0f );
}

/*
========================
idSoundVoice_XAudio2::DestroyInternal
========================
*/
void idSoundVoice_XAudio2::DestroyInternal()
{
	if( pSourceVoice != NULL )
	{
		if( s_debugHardware.GetBool() )
		{
			idLib::Printf( "%dms: %p destroyed\n", Sys_Milliseconds(), pSourceVoice );
		}
		pSourceVoice->DestroyVoice();
		pSourceVoice = NULL;
		hasVUMeter = false;
		vuMeterAttached = false;	//puncho - R-E6-15: the chain went with the source voice
		paused = true;				//puncho - a voice destroyed while playing (hardware Shutdown) must UnPause on its next use
		soundSystemLocal.hardware.voicesDestroyed++; //puncho - R-E6-15
		//puncho - D-47: the send lists went with the source voice; a send is set only on a voice that lists the bus
		if( listHasBus || listPending )
		{
			SetReverbFlags( false, false );
			reverbSend = 0.0f;
		}
	}
}

/*
========================
idSoundVoice_XAudio2::SetReverbFlags

//puncho - D-47: the hardware counts the voices that list the bus or are pending, kept here wherever either flag
changes, so the release at an unbaked load is one compare while the count is 0 (C-SB60b)
========================
*/
void idSoundVoice_XAudio2::SetReverbFlags( bool hasBus, bool pending )
{
	soundSystemLocal.hardware.reverbBusVoices += ( ( hasBus || pending ) ? 1 : 0 ) - ( ( listHasBus || listPending ) ? 1 : 0 );
	listHasBus = hasBus;
	listPending = pending;
}

/*
========================
idSoundVoice_XAudio2::ClearReverbSend

//puncho - R-SB4-5, R-SB4-29: the bus matrix to 0 at once, on a voice that lists the bus
========================
*/
void idSoundVoice_XAudio2::ClearReverbSend()
{
	if( pSourceVoice != NULL && soundSystemLocal.hardware.pReverbBus != NULL )
	{
		float levels[ MAX_CHANNELS_PER_VOICE * 2 ] = { 0 };
		pSourceVoice->SetOutputMatrix( soundSystemLocal.hardware.pReverbBus, voiceFormat.basic.numChannels, 2, levels, XAUDIO2_COMMIT_NOW );
	}
	reverbSend = 0.0f;
}

/*
========================
idSoundVoice_XAudio2::SetReverbList

//puncho - R-SB4-5 step 3, R-SB4-26, R-SB4-29, R-SB4-31: the voice's list, {master, bus} or {master} (NULL).
SetOutputVoices resets every send matrix, so the master matrix is reissued at once from the voice's last values and
a bus matrix is cut to 0 (D-30). Called on a stopped or free voice, the owning world's detach excepted (DR-43, D-47).
A bus that is not there is never listed; with no XAudio2 only the flags change (D-47). A failed call returns false and
leaves the list, the matrices and both flags as they were (C6-1), so the count still holds the voice and the next walk,
release or begin hook tries it again; a voice the bus could not be added to stays dry (section 9)
========================
*/
bool idSoundVoice_XAudio2::SetReverbList( bool toBus )
{
	idSoundHardware_XAudio2& hardware = soundSystemLocal.hardware;
	toBus = toBus && hardware.pReverbBus != NULL;
	if( pSourceVoice != NULL && hardware.pXAudio2 != NULL )
	{
		HRESULT result;
		if( toBus )
		{
			XAUDIO2_SEND_DESCRIPTOR sends[ 2 ] = { { 0, hardware.pMasterVoice }, { 0, hardware.pReverbBus } };
			XAUDIO2_VOICE_SENDS sendList = { 2, sends };
			result = pSourceVoice->SetOutputVoices( &sendList );
		}
		else
		{
			result = pSourceVoice->SetOutputVoices( NULL );
		}
		if( FAILED( result ) )
		{
			common->DWarning( "sound voice %p: SetOutputVoices to %s failed (0x%08x)", pSourceVoice, toBus ? "master and bus" : "master", ( unsigned int )result );
			return false;
		}
		const int srcChannels = voiceFormat.basic.numChannels;
		float levels[ MAX_CHANNELS_PER_VOICE * MAX_CHANNELS_PER_VOICE ] = { 0 };
		CalculateSurround( srcChannels, levels, 1.0f );
		pSourceVoice->SetOutputMatrix( hardware.pMasterVoice, srcChannels, dstChannels, levels, XAUDIO2_COMMIT_NOW );
		if( toBus )
		{
			ClearReverbSend();
		}
	}
	SetReverbFlags( toBus, false );
	reverbSend = 0.0f;
	return true;
}

/*
========================
idSoundVoice_XAudio2::SetReverbSend

//puncho - R-SB4-5: at H1 with the start, and by the arm walk. Ms: mono 0.7071 s to L and R; stereo identity s; 3 to 8
channels 0.5 s each to L and R. The send is after the voice's SetVolume, so it follows the gain, pauseFade and the
cushion with no per-frame call. OPERATION_SET: it commits with the start
========================
*/
void idSoundVoice_XAudio2::SetReverbSend( float s )
{
	if( !listHasBus )
	{
		return;
	}
	assert( pSourceVoice != NULL && soundSystemLocal.hardware.pReverbBus != NULL );
	const int srcChannels = voiceFormat.basic.numChannels;
	assert( srcChannels >= 1 && srcChannels <= MAX_CHANNELS_PER_VOICE );
	// XAudio2's matrix: source channel c to destination channel d at [ d * srcChannels + c ]
	float levels[ MAX_CHANNELS_PER_VOICE * 2 ] = { 0 };
	if( srcChannels == 1 )
	{
		levels[ 0 ] = idMath::SQRT_1OVER2 * s;
		levels[ 1 ] = idMath::SQRT_1OVER2 * s;
	}
	else if( srcChannels == 2 )
	{
		levels[ 0 ] = s;		// L to L
		levels[ 3 ] = s;		// R to R
	}
	else
	{
		for( int i = 0; i < srcChannels * 2; i++ )
		{
			levels[ i ] = 0.5f * s;
		}
	}
	pSourceVoice->SetOutputMatrix( soundSystemLocal.hardware.pReverbBus, srcChannels, 2, levels, OPERATION_SET );
	idSoundVoice_Base::SetReverbSend( s );
}

/*
========================
idSoundVoice_XAudio2::Start
========================
*/
void idSoundVoice_XAudio2::Start( int offsetMS, int ssFlags )
{

	if( s_debugHardware.GetBool() )
	{
		idLib::Printf( "%dms: %p starting %s @ %dms\n", Sys_Milliseconds(), pSourceVoice, leadinSample ? leadinSample->GetName() : "<null>", offsetMS );
	}

	if( !leadinSample )
	{
		return;
	}
	if( !pSourceVoice )
	{
		return;
	}

	//puncho - DR-175: no defaulted-sample warning here: the channel's voice allocation names the real sample it replaced
	bool flicker = ( ssFlags & SSF_NO_FLICKER ) == 0;

	//puncho - R-E6-15: on a reused voice the meter, once attached, stays attached for the life of the source
	// voice and is switched with Enable/DisableEffect, so a start never changes the voice graph. hasVUMeter
	// still means "this start uses the meter"; the channel count of any later sample matches (FormatsMatch)
	if( flicker && !vuMeterAttached )
	{
		IUnknown* vuMeter = NULL;
		if( XAudio2CreateVolumeMeter( &vuMeter, 0 ) == S_OK )
		{

			XAUDIO2_EFFECT_DESCRIPTOR descriptor;
			descriptor.InitialState = true;
			descriptor.OutputChannels = leadinSample->NumChannels();
			descriptor.pEffect = vuMeter;

			XAUDIO2_EFFECT_CHAIN chain;
			chain.EffectCount = 1;
			chain.pEffectDescriptors = &descriptor;

			if( SUCCEEDED( pSourceVoice->SetEffectChain( &chain ) ) )
			{
				vuMeterAttached = true;
			}

			vuMeter->Release();
		}
	}
	else if( vuMeterAttached && flicker != hasVUMeter )
	{
		if( flicker )
		{
			pSourceVoice->EnableEffect( 0 );
		}
		else
		{
			pSourceVoice->DisableEffect( 0 );
		}
	}
	hasVUMeter = flicker;

	assert( offsetMS >= 0 );
	int offsetSamples = MsecToSamples( offsetMS, leadinSample->SampleRate() );
	if( loopingSample == NULL && offsetSamples >= leadinSample->playLength )
	{
		return;
	}

	RestartAt( offsetSamples );
	Update();
	UnPause();
}

/*
========================
idSoundVoice_XAudio2::RestartAt
========================
*/
int idSoundVoice_XAudio2::RestartAt( int offsetSamples )
{
	offsetSamples &= ~127;

	idSoundSample_XAudio2* sample = leadinSample;
	if( offsetSamples >= leadinSample->playLength )
	{
		if( loopingSample != NULL )
		{
			offsetSamples %= loopingSample->playLength;
			sample = loopingSample;
		}
		else
		{
			return 0;
		}
	}

	int previousNumSamples = 0;
	for( int i = 0; i < sample->buffers.Num(); i++ )
	{
		if( sample->buffers[i].numSamples > sample->playBegin + offsetSamples )
		{
			return SubmitBuffer( sample, i, sample->playBegin + offsetSamples - previousNumSamples );
		}
		previousNumSamples = sample->buffers[i].numSamples;
	}

	return 0;
}

/*
========================
idSoundVoice_XAudio2::SubmitBuffer
========================
*/
int idSoundVoice_XAudio2::SubmitBuffer( idSoundSample_XAudio2* sample, int bufferNumber, int offset )
{

	if( sample == NULL || ( bufferNumber < 0 ) || ( bufferNumber >= sample->buffers.Num() ) )
	{
		return 0;
	}
	//puncho - R-E6-18: the next of the voice's own contexts, no lock. Callers: RestartAt from Start (main thread,
	// nothing queued, before UnPause) and OnBufferStart (XAudio2's thread, once per started buffer), never both at
	// once, so at most two buffers are queued and the ring of three never reuses one XAudio2 still holds
	xa2BufferContext_t* bufferContext = &contexts[ nextContext ];
	nextContext = ( nextContext + 1 ) % MAX_QUEUED_BUFFERS;

	bufferContext->voice = this;
	bufferContext->sample = sample;
	bufferContext->bufferNumber = bufferNumber;

	XAUDIO2_BUFFER buffer = { 0 };
	if( offset > 0 )
	{
		int previousNumSamples = 0;
		if( bufferNumber > 0 )
		{
			previousNumSamples = sample->buffers[bufferNumber - 1].numSamples;
		}
		buffer.PlayBegin = offset;
		buffer.PlayLength = sample->buffers[bufferNumber].numSamples - previousNumSamples - offset;
	}
	buffer.AudioBytes = sample->buffers[bufferNumber].bufferSize;
	buffer.pAudioData = ( BYTE* )sample->buffers[bufferNumber].buffer;
	buffer.pContext = bufferContext;
	if( ( loopingSample == NULL ) && ( bufferNumber == sample->buffers.Num() - 1 ) )
	{
		buffer.Flags = XAUDIO2_END_OF_STREAM;
	}
	pSourceVoice->SubmitSourceBuffer( &buffer );

	return buffer.AudioBytes;
}

/*
========================
idSoundVoice_XAudio2::Update
========================
*/
bool idSoundVoice_XAudio2::Update()
{
	if( pSourceVoice == NULL || leadinSample == NULL )
	{
		return false;
	}

	const int srcChannels = leadinSample->NumChannels();

	float pLevelMatrix[ MAX_CHANNELS_PER_VOICE * MAX_CHANNELS_PER_VOICE ] = { 0 };
	CalculateSurround( srcChannels, pLevelMatrix, 1.0f );

	//puncho - R-E6-17: publish what the callback reads; it read pitch and occlusion live before, skipped sets or not
	pitchShared.store( pitch, std::memory_order_relaxed );
	occlusionShared.store( occlusion, std::memory_order_relaxed );

	if( s_skipHardwareSets.GetBool() )
	{
		return true;
	}

	pSourceVoice->SetOutputMatrix( soundSystemLocal.hardware.pMasterVoice, srcChannels, dstChannels, pLevelMatrix, OPERATION_SET );

	assert( idMath::Fabs( gain ) <= XAUDIO2_MAX_VOLUME_LEVEL );
	pSourceVoice->SetVolume( gain, OPERATION_SET );

	// filter and ratio only on a change (R-E6-17)
	SetSampleRate( OPERATION_SET, false );

	// we don't do this any longer because we pause and unpause explicitly when the soundworld is paused or unpaused
	// UnPause();
	return true;
}

/*
========================
idSoundVoice_XAudio2::IsPlaying
========================
*/
bool idSoundVoice_XAudio2::IsPlaying()
{
	if( pSourceVoice == NULL )
	{
		return false;
	}
	XAUDIO2_VOICE_STATE state;
	pSourceVoice->GetState( &state );
	return ( state.BuffersQueued != 0 );
}

/*
========================
idSoundVoice_XAudio2::FlushSourceBuffers
========================
*/
void idSoundVoice_XAudio2::FlushSourceBuffers()
{
	if( pSourceVoice != NULL )
	{
		pSourceVoice->FlushSourceBuffers();
	}
}

/*
========================
idSoundVoice_XAudio2::Pause
========================
*/
void idSoundVoice_XAudio2::Pause()
{
	if( !pSourceVoice || paused )
	{
		return;
	}
	if( s_debugHardware.GetBool() )
	{
		idLib::Printf( "%dms: %p pausing %s\n", Sys_Milliseconds(), pSourceVoice, leadinSample ? leadinSample->GetName() : "<null>" );
	}
	pSourceVoice->Stop( 0, OPERATION_SET );
	paused = true;
}

/*
========================
idSoundVoice_XAudio2::UnPause
========================
*/
void idSoundVoice_XAudio2::UnPause()
{
	if( !pSourceVoice || !paused )
	{
		return;
	}
	if( s_debugHardware.GetBool() )
	{
		idLib::Printf( "%dms: %p unpausing %s\n", Sys_Milliseconds(), pSourceVoice, leadinSample ? leadinSample->GetName() : "<null>" );
	}
	pSourceVoice->Start( 0, OPERATION_SET );
	paused = false;
}

/*
========================
idSoundVoice_XAudio2::Stop
========================
*/
void idSoundVoice_XAudio2::Stop()
{
	if( !pSourceVoice )
	{
		return;
	}
	if( !paused )
	{
		if( s_debugHardware.GetBool() )
		{
			idLib::Printf( "%dms: %p stopping %s\n", Sys_Milliseconds(), pSourceVoice, leadinSample ? leadinSample->GetName() : "<null>" );
		}
		pSourceVoice->Stop( 0, OPERATION_SET );
		paused = true;
	}
}

/*
========================
idSoundVoice_XAudio2::GetAmplitude
========================
*/
float idSoundVoice_XAudio2::GetAmplitude()
{
	if( !hasVUMeter )
	{
		return 1.0f;
	}

	float peakLevels[ MAX_CHANNELS_PER_VOICE ];
	float rmsLevels[ MAX_CHANNELS_PER_VOICE ];

	XAUDIO2FX_VOLUMEMETER_LEVELS levels;
	levels.ChannelCount = leadinSample->NumChannels();
	levels.pPeakLevels = peakLevels;
	levels.pRMSLevels = rmsLevels;

	if( levels.ChannelCount > MAX_CHANNELS_PER_VOICE )
	{
		levels.ChannelCount = MAX_CHANNELS_PER_VOICE;
	}

	if( pSourceVoice->GetEffectParameters( 0, &levels, sizeof( levels ) ) != S_OK )
	{
		return 0.0f;
	}

	if( levels.ChannelCount == 1 )
	{
		return rmsLevels[0];
	}

	float rms = 0.0f;
	for( uint32 i = 0; i < levels.ChannelCount; i++ )
	{
		rms += rmsLevels[i];
	}

	return rms / ( float )levels.ChannelCount;
}

/*
========================
idSoundVoice_XAudio2::ResetSampleRate
========================
*/
void idSoundVoice_XAudio2::SetSampleRate( uint32 operationSet, bool onBufferStart )
{
	if( pSourceVoice == NULL || leadinSample == NULL )
	{
		return;
	}

	//puncho - R-E6-17: only the shared values; the callback runs on XAudio2's thread and takes no lock
	const uint32 rate = sampleRate.load( std::memory_order_relaxed );
	const float pitchNow = pitchShared.load( std::memory_order_relaxed );
	const float occlusionNow = occlusionShared.load( std::memory_order_relaxed );

	XAUDIO2_FILTER_PARAMETERS filter;
	filter.Type = LowPassOnePoleFilter;	//puncho - DR-201: every voice, including the enviro suit
	filter.OneOverQ = 1.0f;			//puncho - DR-201: ignored by one-pole filters
	float cutoffFrequency = 1000.0f / Max( 0.01f, occlusionNow );
	//puncho - R-E6-39: at the rate the filter runs at, not the sample's; rate is read for the frequency ratio only
	filter.Frequency = SoundFilterFrequency( cutoffFrequency, ( float )SND_MIX_SAMPLE_RATE );
	assert( filter.Frequency >= 0.0f && filter.Frequency <= XAUDIO2_MAX_FILTER_FREQUENCY );
	filter.Frequency = idMath::ClampFloat( 0.0f, XAUDIO2_MAX_FILTER_FREQUENCY, filter.Frequency );

	// the callback issues both at a buffer start, a rate change included; it never writes
	// lastFilterFreq or lastFreqRatio, so those stay the main thread's alone. Both hold what the main thread
	// issued, computed from the rate it read: if its deferred set carried a rate the callback had just replaced,
	// the next Update computes a different value from the new rate and issues once, so a stale set lasts one frame
	if( onBufferStart )
	{
		pSourceVoice->SetFilterParameters( &filter, operationSet );
	}
	else if( filter.Frequency != lastFilterFreq )
	{
		pSourceVoice->SetFilterParameters( &filter, operationSet );
		lastFilterFreq = filter.Frequency;
	}

	float freqRatio = pitchNow * ( float )rate / ( float )sourceVoiceRate;
	assert( freqRatio >= XAUDIO2_MIN_FREQ_RATIO && freqRatio <= XAUDIO2_MAX_FREQ_RATIO );
	freqRatio = idMath::ClampFloat( XAUDIO2_MIN_FREQ_RATIO, XAUDIO2_MAX_FREQ_RATIO, freqRatio );

	if( !onBufferStart && freqRatio == lastFreqRatio )
	{
		// unchanged since the main thread last issued it. A lead-in-to-loop rate change is applied by the callback
		// at once; the next Update then issues the same ratio once more, because it differs from its last one
		return;
	}

	// if the value specified for maxFreqRatio is too high for the specified format, the call to CreateSourceVoice will fail
	if( voiceFormat.basic.numChannels == 1 )
	{
		assert( freqRatio * ( float )SYSTEM_SAMPLE_RATE <= XAUDIO2_MAX_RATIO_TIMES_RATE_XMA_MONO );
	}
	else
	{
		assert( freqRatio * ( float )SYSTEM_SAMPLE_RATE <= XAUDIO2_MAX_RATIO_TIMES_RATE_XMA_MULTICHANNEL );
	}
	pSourceVoice->SetFrequencyRatio( freqRatio, operationSet );
	if( !onBufferStart )
	{
		lastFreqRatio = freqRatio;
	}
}

/*
========================
idSoundVoice_XAudio2::OnBufferStart
========================
*/
void idSoundVoice_XAudio2::OnBufferStart( idSoundSample_XAudio2* sample, int bufferNumber )
{
	//puncho - R-E6-17: the callback is the only writer of sampleRate once the voice plays
	sampleRate.store( ( uint32 )sample->SampleRate(), std::memory_order_relaxed );
	SetSampleRate( XAUDIO2_COMMIT_NOW, true );

	idSoundSample_XAudio2* nextSample = sample;
	int nextBuffer = bufferNumber + 1;
	if( nextBuffer == sample->buffers.Num() )
	{
		if( sample == leadinSample )
		{
			if( loopingSample == NULL )
			{
				return;
			}
			nextSample = loopingSample;
		}
		nextBuffer = 0;
	}

	SubmitBuffer( nextSample, nextBuffer, 0 );
}
