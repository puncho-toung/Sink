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
//puncho - C3e-C: one main-thread publisher, one gated audio-thread reader.
#ifndef __MX_SOUNDHARDWARE_H__
#define __MX_SOUNDHARDWARE_H__

#if defined( USE_SOUND_MIXER )
#include "../snd_mixlink.h"
#include "../snd_reverbmix.h"
// Included by snd_local.h after xaudio2.h and the sample/voice declarations.
#include "../XAudio2/XA2_SoundSink.h"

class idSoundReverb;
struct reverbCarrierFrame_t;

class idSoundHardware_Mixer
{
public:
						idSoundHardware_Mixer();
	void				Init();
	void				Shutdown();
	void				Update();
	idSoundVoice*		AllocateVoice( const idSoundSample* leadinSample, const idSoundSample* loopingSample );
	void				FreeVoice( idSoundVoice* voice );
	void				FenceSamples( const char* sampleName = NULL );
	void				TracePurgeCandidate( idSoundSample* sample );
	void				PullBlock( float* out, int numOut );
	void				BeginSampleFree();
	void				EndSampleFree();
	void				AssertOutsideSampleFree() const;

	IXAudio2*			GetIXAudio2() const { return sink.GetIXAudio2(); }
	int					GetNumFreeVoices() const { return freeCount; }
	const sndMixStatus_t& GetStatus() const { return lastStatus; }

	// WP-C4 supplies the reverb instances. These entry points retain today's shared face.
	bool				SupportsReverbCarrier() const { return sink.IsOpen(); }
	bool				AcquireReverbCarrier();
	bool				ReleaseIdleReverbGraph() { return false; }
	bool				ReverbCarrierLive() const { return reverbOut[0].live || reverbOut[1].live; }
	int					ReverbBusVoiceCount() const { return ( reverbOut[0].live != 0 ) + ( reverbOut[1].live != 0 ); }
	void				SubmitReverbFrame( const reverbCarrierFrame_t& frame );
	void				BeginReverbMap( idSoundReverb* owner );
	void				EndReverbMap( bool soundCarried );
	void				DetachReverb( const idSoundReverb* owner );
	void				PrintReverbVoices() const;
	bool				GetActiveVoiceCounts( uint32& sourceVoices, uint32& submixVoices ) const;

private:
	// In masterdemo's settings order; the compressor depth has a ceiling-dependent range.
	enum mixerSetting_t { HDR_ON, HDR_CEILING, HDR_RELEASE, EQ_ON, LOW_HZ, LOW_DB, HIGH_HZ, HIGH_DB,
		COMP_ON, COMP_UNDER, COMP_RATIO, COMP_ATTACK, COMP_RELEASE, MAKEUP, LIMIT_CEILING, LIMIT_HOLD, LIMIT_RELEASE };
	inline static idCVar mixerSettings[] =
	{
		{ "s_mixHdr", "0", CVAR_FLOAT, "HDR window: zero off, nonzero on" },
		{ "s_mixHdrCeilingDb", "-12", CVAR_FLOAT, "HDR ceiling in dBFS", -60, 60 },
		{ "s_mixHdrReleaseMs", "500", CVAR_FLOAT, "HDR recovery in milliseconds", 0, 60000 },
		{ "s_mixEq", "0", CVAR_FLOAT, "Tone shelves: zero off, nonzero on" },
		{ "s_mixLowHz", "180", CVAR_FLOAT, "Bass shelf corner in Hz", 60, 400 },
		{ "s_mixLowDb", "0", CVAR_FLOAT, "Bass shelf gain in dB", -12, 12 },
		{ "s_mixHighHz", "2500", CVAR_FLOAT, "Treble shelf corner in Hz", 1000, 8000 },
		{ "s_mixHighDb", "0", CVAR_FLOAT, "Treble shelf gain in dB", -12, 12 },
		{ "s_mixCompress", "0", CVAR_FLOAT, "Compressor: zero off, nonzero on" },
		{ "s_mixCompUnderDb", "6", CVAR_FLOAT, "Threshold depth: ceiling to ceiling+60; cleaned with the ceiling" },
		{ "s_mixCompRatio", "3", CVAR_FLOAT, "Compression ratio", 1, 20 },
		{ "s_mixCompAttackMs", "2", CVAR_FLOAT, "Compressor attack in milliseconds", 0, 50 },
		{ "s_mixCompReleaseMs", "120", CVAR_FLOAT, "Compressor recovery in milliseconds", 10, 2000 },
		{ "s_mixMakeupDb", "0", CVAR_FLOAT, "Post-compressor gain in dB", -12, 12 },
		{ "s_mixLimitCeilingDb", "-1", CVAR_FLOAT, "Final peak ceiling in dBFS", -12, 0 },
		{ "s_mixLimitHoldMs", "30", CVAR_FLOAT, "Limiter hold in milliseconds", 0, 200 },
		{ "s_mixLimitReleaseMs", "50", CVAR_FLOAT, "Limiter recovery in milliseconds", 1, 2000 }
	};

	static constexpr int NUM_SETTINGS = sizeof( mixerSettings ) / sizeof( mixerSettings[0] );
	static_assert( LIMIT_RELEASE + 1 == NUM_SETTINGS, "setting indices must cover the table" );

	friend class idSoundVoice_Mixer;
	void				ShutAndWait();
	void				PublishFrame( bool tickRoom = true );
	void				TickReverb();
	void				PrepareSettings( const float* values );
#ifndef ID_RETAIL
	static void			TestStall_f( const idCmdArgs& args );
	static void			TestPurge_f( const idCmdArgs& args );
	static void			TestViews_f( const idCmdArgs& args );
	void				ReportKeptCandidates();
#endif
	void				NoteStop();
	unsigned int		NextGeneration();

	idSoundSink_XAudio2	sink;
	idSoundMixer		mixer;
	idSoundMixLink		link;
	void*				mixerMemory;
	idSoundVoice		voices[SND_MIX_MAX_VOICES];
	int					freeSlots[SND_MIX_MAX_VOICES];
	int					freeHead;
	int					freeCount;
	unsigned int		generation;
	unsigned int		resetSerial;
	unsigned int		hdrSerial;
	unsigned long long	stopSequence;
	sndHdrParms_t		hdrParms;
	sndMasterCoefs_t		masterCoefs;
	sndMixStatus_t		lastStatus;
	idSoundReverb*		reverbOwner;
	//puncho - C4-R: main-thread state, kept entries alone cross the link.
	idSoundReverbMix		reverbMix;
	void*				reverbMemory[2];
	sndMixReverb_t		reverbOut[2];
	unsigned int		reverbMapSerial;
	uint64				reverbTickUs;
	unsigned int		reverbMaxTickUs;
	idDebugGraph*		vuMeterRMS;
	idDebugGraph*		vuMeterPeak;
	int					lastResetTime;
	float				settingValues[NUM_SETTINGS];
	bool				hdrOn;
	bool				meters;
	unsigned long long	firstMeterSequence;
	int					meterPeakTimes[SND_MIX_MAX_OUT];
	int					lastPerfPrintTime;
	int					showMixer;
	unsigned int		publishUs;
	unsigned int		reportedMaxPublishWaitUs; //puncho - DR-318: last second, reused by the overlay.
#ifndef ID_RETAIL
	unsigned int		testLoadKept;
	unsigned int		testLoadWaits;
	uint64				testLoadWaitUs;
	bool				testLoadActive;
	int					testStallMS;
	bool				testHold;
	std::atomic<unsigned long long> testZeroUntilUs;
	std::atomic<unsigned int> testHoldMS;
	std::atomic<unsigned long long> testHoldStartUs;
	std::atomic<unsigned long long> testHoldEndUs;
#endif

	std::atomic<unsigned int>	gate;
	std::atomic<unsigned int>	inBlock;
	std::atomic<unsigned int>	pulls;
	std::atomic<unsigned int>	maxPublishWaitUs; //puncho - DR-318: audio raises, main exchanges to zero.
	int					streamChannels;	// set behind the gate, immutable while open
	// Only the audio thread writes these after the gate opens.
	unsigned int		blockTimes[100];
	unsigned int		blockTimeIndex;
	unsigned int		lateBlocks;
	unsigned long long	lastSequence;
	// Main-thread fence counters, kept across restarts.
	unsigned int		fences;
	unsigned int		fenceWaits;
	uint64				maxFenceWaitUs;
	uint64				fenceWaitUs;
	unsigned int		stalls;
#if defined( _DEBUG )
	bool				insideSampleFree;
#endif
};

class idSoundHardware : public idSoundHardware_Mixer
{
};

#endif // USE_SOUND_MIXER
#endif
