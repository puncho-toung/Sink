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
#ifndef __XA2_SOUNDVOICE_H__
#define __XA2_SOUNDVOICE_H__

#include <atomic> //puncho - R-E6-17: values the XAudio2 callback reads without a lock

static const int MAX_QUEUED_BUFFERS = 3;

class idSoundVoice_XAudio2;
class idSoundSample_XAudio2;

//puncho - R-E6-18: the context XAudio2 hands back to the voice's callback. Each voice embeds
// MAX_QUEUED_BUFFERS of them, so neither thread takes a lock; the shared pool and
// idSoundSystemLocal::bufferContext_t (declared after this header) stay for OpenAL only
struct xa2BufferContext_t
{
	idSoundVoice_XAudio2*	voice;
	idSoundSample_XAudio2*	sample;
	int						bufferNumber;
};

/*
================================================
idSoundVoice_XAudio2
================================================
*/
class idSoundVoice_XAudio2 : public idSoundVoice_Base
{
public:
	idSoundVoice_XAudio2();
	~idSoundVoice_XAudio2();

	void					Create( const idSoundSample* leadinSample, const idSoundSample* loopingSample );

	// Start playing at a particular point in the buffer.  Does an Update() too
	void					Start( int offsetMS, int ssFlags );

	// Stop playing.
	void					Stop();

	// Stop consuming buffers
	void					Pause();
	// Start consuming buffers again
	void					UnPause();

	// Sends new position/volume/pitch information to the hardware
	bool					Update();

	// returns the RMS levels of the most recently processed block of audio, SSF_FLICKER must have been passed to Start
	float					GetAmplitude();

	// returns true if we can re-use this voice
	bool					CompatibleFormat( idSoundSample_XAudio2* s );

	//puncho - R-E6-15: the field test CompatibleFormat runs, callable without a created voice (E6-S's self-test)
	static bool				FormatsMatch( const idWaveFile::waveFmt_t& a, const idWaveFile::waveFmt_t& b );

	uint32					GetSampleRate() const
	{
		return sampleRate.load( std::memory_order_relaxed );
	}

	//puncho - R-SB4-5: the send into the reverb bus; returns at once when the voice does not list the bus
	void					SetReverbSend( float s );

	// callback function
	void					OnBufferStart( idSoundSample_XAudio2* sample, int bufferNumber );

private:
	friend class idSoundHardware_XAudio2;

	// Returns true when all the buffers are finished processing
	bool					IsPlaying();

	// Called after the voice has been stopped
	void					FlushSourceBuffers();

	// Destroy the internal hardware resource
	void					DestroyInternal();

	// Helper function used by the initial start as well as for looping a streamed buffer
	int						RestartAt( int offsetSamples );

	// Helper function to submit a buffer
	int						SubmitBuffer( idSoundSample_XAudio2* sample, int bufferNumber, int offset );

	// Adjust the voice frequency based on the new sample rate for the buffer
	//puncho - R-E6-17: reads only sampleRate, pitchShared, occlusionShared; onBufferStart issues both calls,
	// the main thread's Update issues the filter and the ratio each only when it differs from the last one it issued
	void					SetSampleRate( uint32 operationSet, bool onBufferStart );

	//puncho start - WP-SB4: the voice's send lists, main thread only (R-SB4-28)
	// {master, bus} or {master}; the master matrix reissued and a bus matrix cut at once (D-30); false when XAudio2
	// refused the list, the flags then left as they were
	bool					SetReverbList( bool toBus );
	// the bus matrix cut at once and reverbSend 0
	void					ClearReverbSend();
	// the two flags, with the hardware's count of voices that have either (D-47)
	void					SetReverbFlags( bool hasBus, bool pending );
	//puncho end

	IXAudio2SourceVoice* 	pSourceVoice;
	idSoundSample_XAudio2* leadinSample;
	idSoundSample_XAudio2* loopingSample;

	//puncho - R-E6-15: the whole format the source voice was created with, copied at CreateSourceVoice
	idWaveFile::waveFmt_t	voiceFormat;

	uint32					sourceVoiceRate;
	std::atomic<uint32>		sampleRate;			//puncho - R-E6-17: written by Create while not playing, then only by the callback

	//puncho - R-E6-17: published by Update on the main thread, read by the callback (relaxed)
	std::atomic<float>		pitchShared;
	std::atomic<float>		occlusionShared;
	// the last values Update issued; main thread only; < 0 = issue on the next Update
	float					lastFreqRatio;
	float					lastFilterFreq;

	//puncho - R-E6-18: at most two buffers are queued at once (RestartAt submits one, each OnBufferStart one more),
	// so a ring of three never overwrites a context XAudio2 still holds
	xa2BufferContext_t		contexts[ MAX_QUEUED_BUFFERS ];
	int						nextContext;

	bool					hasVUMeter;			// this start uses the meter (GetAmplitude reads it)
	bool					vuMeterAttached;	//puncho - R-E6-15: a meter chain is set on pSourceVoice (kept until it is destroyed)
	bool					paused;
	bool					listHasBus = false;		//puncho - R-SB4-5: the voice's XAudio2 list holds the reverb bus
	bool					listPending = false;	//puncho - R-SB4-29: held when its list was set; reset once free
};

/*
================================================
idSoundVoice
================================================
*/
class idSoundVoice : public idSoundVoice_XAudio2
{
};

#endif
