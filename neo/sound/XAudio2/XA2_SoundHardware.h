/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2012 Robert Beckebans

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
#ifndef __XA_SOUNDHARDWARE_H__
#define __XA_SOUNDHARDWARE_H__

class idSoundSample_XAudio2;
class idSoundVoice_XAudio2;
// RB
class idSoundHardware_XAudio2;
//puncho - WP-SB4 (SEAMS section 3): snd_reverb.h, which snd_local.h includes, declares both
class idSoundReverb;
struct reverbCarrierFrame_t;

/*
================================================
idSoundEngineCallback
================================================
*/
class idSoundEngineCallback : public IXAudio2EngineCallback
{
public:
	idSoundHardware_XAudio2* hardware;

private:
	// Called by XAudio2 just before an audio processing pass begins.
	STDMETHOD_( void, OnProcessingPassStart )( THIS ) {}

	// Called just after an audio processing pass ends.
	STDMETHOD_( void, OnProcessingPassEnd )( THIS ) {}

	// Called in the event of a critical system error which requires XAudio2
	// to be closed down and restarted.  The error code is given in Error.
	STDMETHOD_( void, OnCriticalError )( THIS_ HRESULT Error );
};

/*
================================================
idSoundHardware_XAudio2
================================================
*/

class idSoundHardware_XAudio2
{
public:
	idSoundHardware_XAudio2();

	void			Init();
	void			Shutdown();

	void 			Update();

	idSoundVoice* 	AllocateVoice( const idSoundSample* leadinSample, const idSoundSample* loopingSample );
	void			FreeVoice( idSoundVoice* voice );

	// video playback needs this
	IXAudio2* 		GetIXAudio2() const
	{
		return pXAudio2;
	};

	int				GetNumZombieVoices() const
	{
		return zombieVoices.Num();
	}
	int				GetNumFreeVoices() const
	{
		return freeVoices.Num();
	}
	//puncho - R-E6-15: source voices created and destroyed since start; a reused voice counts in neither
	int				GetNumVoicesCreated() const
	{
		return voicesCreated;
	}
	int				GetNumVoicesDestroyed() const
	{
		return voicesDestroyed;
	}
	//puncho - R-E6-18: the s_showVoices buffer column, the buffers XAudio2 holds over all voices and the
	// contexts the voices embed; one GetState per created voice, paid only while s_showVoices is on
	int				GetNumQueuedBuffers() const
	{
		int queued = 0;
		for( int i = 0; i < voices.Num(); i++ )
		{
			if( voices[i].pSourceVoice != NULL )
			{
				XAUDIO2_VOICE_STATE state;
				voices[i].pSourceVoice->GetState( &state );
				queued += ( int )state.BuffersQueued;
			}
		}
		return queued;
	}
	int				GetNumBufferContexts() const
	{
		return voices.Num() * MAX_QUEUED_BUFFERS;
	}

	//puncho start - WP-SB4: the reverb carrier (R-SB4-1, SEAMS section 3), main thread only (R-SB4-28). The graph is
	// reverb A, B and the bus that feeds them, made at the first gated load and kept until Shutdown or the owning
	// world's DetachReverb (R-SB4-25). With no XAudio2 or no graph a body skips only its XAudio2 calls (D-47)
	bool			SupportsReverbCarrier() const		// SB3's step 3: creates nothing
	{
		return pXAudio2 != NULL && pMasterVoice != NULL;
	}
	bool			AcquireReverbCarrier();				// SB3's step 11: the graph made if absent, else reused
	void			SubmitReverbFrame( const reverbCarrierFrame_t& frame );
	bool			ReverbCarrierLive() const			// C-SB65b: the one test at a source voice's creation
	{
		return reverbCarrierLive;
	}
	bool			ReleaseIdleReverbGraph()			// C-SB60b: one compare while no voice lists the bus or is pending; destroys nothing
	{
		if( reverbBusVoices == 0 )
		{
			return false;
		}
		ResetIdleReverbLists();
		return true;
	}
	void			PrintReverbVoices() const;
	//puncho - SB3-T (DR-209): paid only by listAcoustics
	bool			GetActiveVoiceCounts( uint32& sourceVoices, uint32& submixVoices ) const
	{
		if( pXAudio2 == NULL )
		{
			return false;
		}
		XAUDIO2_PERFORMANCE_DATA data;
		pXAudio2->GetPerformanceData( &data );
		sourceVoices = data.ActiveSourceVoiceCount;
		submixVoices = data.ActiveSubmixVoiceCount;
		return true;
	}
	void			BeginReverbMap( idSoundReverb* owner );			// R-SB4-5's hardware part; sets reverbOwner
	void			EndReverbMap( bool soundCarried );				// R-SB4-26's hardware part
	void			DetachReverb( const idSoundReverb* owner );		// R-SB4-31's hardware part
	int				ReverbBusVoiceCount() const		// voices that list the bus or are pending a reset (DR-172, DR-173)
	{
		return reverbBusVoices;
	}
	//puncho end

protected:
	friend class idSoundSample_XAudio2;
	friend class idSoundVoice_XAudio2;

private:
	//puncho start - WP-SB4: the carrier's steps
	void			FlushZombieVoices();
	void			ResetIdleReverbLists();
	void			UpdateReverbFreeVoices();
	bool			CreateReverbSubmix( IXAudio2SubmixVoice** submix );
	bool			RemakeReverbSubmixes();
	void			DestroyReverbGraph();
	// each setter records in the cache what XAudio2 took, or unknown when it refused the call (C6R-2)
	void			SetReverbReturn( int submix, float gain, uint32 operationSet );
	void			SetReverbBusLevel( int submix, float level, uint32 operationSet );
	void			SetReverbEnabled( int submix, bool enable, uint32 operationSet );
	void			SilenceReverbGraph();
	void			ResetReverbSent( int submix );
	//puncho end

	IXAudio2* pXAudio2;
	IXAudio2MasteringVoice* pMasterVoice;
	//puncho - R-SB4-3: the submix made in every process that nothing sent to is gone

	idSoundEngineCallback	soundEngineCallback;
	int					lastResetTime;

	int					outputChannels;
	int					channelMask;

	idDebugGraph* 		vuMeterRMS;
	idDebugGraph* 		vuMeterPeak;
	int					vuMeterPeakTimes[ 8 ];

	// Can't stop and start a voice on the same frame, so we have to double this to handle the worst case scenario of stopping all voices and starting a full new set
	idStaticList<idSoundVoice_XAudio2, MAX_HARDWARE_VOICES * 2 > voices;
	idStaticList<idSoundVoice_XAudio2*, MAX_HARDWARE_VOICES * 2 > zombieVoices;
	idStaticList<idSoundVoice_XAudio2*, MAX_HARDWARE_VOICES * 2 > freeVoices;

	int					voicesCreated = 0;		//puncho - R-E6-15: written by idSoundVoice_XAudio2 on the main thread
	int					voicesDestroyed = 0;

	//puncho start - WP-SB4 (R-SB4-2): reverb A and B, and the bus that feeds them; all three made or all three NULL
	IXAudio2SubmixVoice*	pReverbVoices[ 2 ];
	IXAudio2SubmixVoice*	pReverbBus;
	idSoundReverb*		reverbOwner;			// survives Shutdown, for the reacquire at the end of Init (R-SB4-32)
	uint32				reverbSampleRate;		// the master's rate, kept at the end of Init (R-SB4-2)
	int					reverbBusVoices;		// voices with listHasBus or listPending set (D-47); written by idSoundVoice_XAudio2
	bool				reverbCarrierLive;		// C-SB65b: a source voice is made on {master, bus}
	bool				reverbDisabledMidTail;	// A and B are made again at the next begin (R-SB4-5)
	bool				reverbBusAttached;		// the bus sends to A and B; false after a failed attach left its list empty (C6S-1)
	// what A and B were last sent, so a value goes to XAudio2 only when it moved (R-SB4-7). A call XAudio2 refused
	// leaves its entry unknown, which compares unequal to every value, so it is sent again (C6R-2)
	int					reverbSentEnabled[ 2 ];		// 1 on, 0 off, -1 unknown
	bool				reverbSentParamsValid[ 2 ];
	float				reverbSentBus[ 2 ];
	float				reverbSentReturn[ 2 ];
	XAUDIO2FX_REVERB_PARAMETERS	reverbSentParams[ 2 ];
	//puncho end
};

/*
================================================
idSoundHardware
================================================
*/
class idSoundHardware : public idSoundHardware_XAudio2
{
};

#endif
