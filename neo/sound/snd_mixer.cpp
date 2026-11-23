//puncho - C3c-M: caller-owned, bounded block mixing; no engine state.
#include "snd_mixer.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

struct sndMixSlot_t
{
	sndVoiceState_t	voice;
	float			previousSend;
	unsigned char	played;
};

struct sndMixMemory_t
{
	float			scratch[SND_MIX_MAX_SRC_CHANNELS][SND_MIX_BLOCK];
	float			fetch[2 * SND_VOICE_FETCH_FRAMES];
	float			world[SND_MIX_MAX_OUT][SND_MIX_BLOCK];
	float			protectedRows[SND_MIX_MAX_OUT][SND_MIX_BLOCK];
	float			send[SND_MIX_BLOCK];
	sndMixSlot_t		slots[SND_MIX_MAX_VOICES];
	sndHdrWindow_t	window;
	sndMasterState_t	master;
	unsigned int	resetSerial;
	unsigned int	hdrSerial;
	unsigned int	blocks;
	unsigned char	hdrOn;
	unsigned char	initialized;
};

idSoundMixer::idSoundMixer()
{
	storage = NULL;
}

size_t idSoundMixer::MemoryBytes()
{
	return ( sizeof( sndMixMemory_t ) + 15 ) & ~(size_t)15;
}

bool idSoundMixer::Init( void* memory )
{
	storage = NULL;
	if ( memory == NULL || ( (uintptr_t)memory & 15 ) != 0 )
	{
		return false;
	}
	memset( memory, 0, MemoryBytes() );
	sndHdrParms_t parms;
	SndHdr_DefaultParms( parms );
	SndHdr_CleanParms( parms );
	SndHdr_ResetWindow( ( (sndMixMemory_t*)memory )->window, parms );
	storage = memory;
	return true;
}

void idSoundMixer::Shutdown()
{
	storage = NULL;
}

void idSoundMixer::Block( const sndMixFrame_t& frame, sndMixStatus_t& status, float* out )
{
	// The timing tool reads these stage markers; keep each before its stage.
	// Host stage: adoption
	// The timing fields belong to the sink, including on an invalid frame.
	memset( &status, 0, offsetof( sndMixStatus_t, blockUs ) );
	memset( status.voices, 0, sizeof( status.voices ) );
	status.sequence = frame.sequence;
	status.sceneDb = SND_HDR_SILENT;
	status.windowFactor = 1.0f;
	status.reverbTail[0] = status.reverbTail[1] = -1;
	const bool outputsValid = frame.numOut >= 1 && frame.numOut <= SND_MIX_MAX_OUT;
	if ( !outputsValid || storage == NULL || frame.lfeOut < -1 || frame.lfeOut >= frame.numOut ||
		( frame.numOut == 1 && frame.lfeOut != -1 ) ||
		frame.wetOut[0] < 0 || frame.wetOut[0] >= frame.numOut ||
		frame.wetOut[1] < 0 || frame.wetOut[1] >= frame.numOut ||
		frame.wetOut[0] == frame.lfeOut || frame.wetOut[1] == frame.lfeOut )
	{
		if ( storage != NULL )
		{
			for ( int v = 0; v < SND_MIX_MAX_VOICES; v++ )
			{
				( (sndMixMemory_t*)storage )->slots[v].played = 0;
			}
		}
		if ( outputsValid )
		{
			memset( out, 0, frame.numOut * SND_MIX_BLOCK * sizeof( float ) );
		}
		return;
	}
	sndMixMemory_t& memory = *(sndMixMemory_t*)storage;
	if ( !memory.initialized || memory.resetSerial != frame.resetSerial )
	{
		memset( memory.slots, 0, sizeof( memory.slots ) );
		SndHdr_ResetWindow( memory.window, frame.hdr );
		SndMaster_Reset( memory.master, frame.master, 1.0f );
		for ( int r = 0; r < 2; r++ )
		{
			if ( frame.reverb[r].live && frame.reverb[r].convolver != NULL )
			{
				frame.reverb[r].convolver->Flush();
			}
		}
		memory.blocks = 0;
		memory.resetSerial = frame.resetSerial;
		memory.initialized = 1;
	}
	if ( memory.hdrSerial != frame.hdrSerial || ( frame.hdrOn && !memory.hdrOn ) )
	{
		SndHdr_ResetWindow( memory.window, frame.hdr );
	}
	memory.hdrSerial = frame.hdrSerial;
	memory.hdrOn = frame.hdrOn;

	int scratchRow[SND_MIX_MAX_VOICES];
	float meanSquare[SND_MIX_MAX_VOICES] = {};
	unsigned char last[SND_MIX_MAX_VOICES] = {};
	int channels = 0;
	for ( int v = 0; v < SND_MIX_MAX_VOICES; v++ )
	{
		sndMixSlot_t& slot = memory.slots[v];
		const sndVoiceCmd_t& cmd = frame.voices[v];
		const bool same = cmd.generation == slot.voice.generation;
		const bool played = slot.played != 0;
		slot.played = 0;
		scratchRow[v] = -1;
		if ( !same && ( cmd.state == SND_VOICE_PLAY || cmd.state == SND_VOICE_PAUSED ) )
		{
			SndVoice_Start( slot.voice, cmd );
			slot.previousSend = 0.0f;
		}
		// active means a started timeline, never "played last block". Ended voices
		// keep their generation/end flag until Start or a stream reset replaces them.
		if ( !slot.voice.active || slot.voice.ended )
		{
			continue;
		}
		if ( cmd.state != SND_VOICE_PLAY )
		{
			// R-C3c-4: OFF/PAUSED with same generation + played gets last; other
			// OFF/PAUSED rows idle/hold. A value outside the enum holds without a call.
			if ( !same || !played || ( cmd.state != SND_VOICE_OFF && cmd.state != SND_VOICE_PAUSED ) )
			{
				continue;
			}
			last[v] = 1;
		}
		if ( cmd.leadin.channels != 1 && cmd.leadin.channels != 2 )
		{
			continue;
		}
		if ( channels + cmd.leadin.channels > SND_MIX_MAX_SRC_CHANNELS )
		{
			status.voicesSkipped++;
			continue;
		}
		scratchRow[v] = channels;
		channels += cmd.leadin.channels;
	}
	// Host stage: render
	for ( int v = 0; v < SND_MIX_MAX_VOICES; v++ )
	{
		if ( scratchRow[v] >= 0 )
		{
			meanSquare[v] = SndVoice_Render( memory.slots[v].voice, frame.voices[v], memory.fetch,
				memory.scratch[scratchRow[v]] );
		}
	}
	// Host stage: window
	float sum = 0.0f;
	for ( int v = 0; v < SND_MIX_MAX_VOICES; v++ )
	{
		const sndVoiceCmd_t& cmd = frame.voices[v];
		if ( scratchRow[v] >= 0 && !( cmd.flags & SND_VOICEFLAG_PROTECTED ) )
		{
			const float term = cmd.dryGain * cmd.dryGain * meanSquare[v];
			if ( isfinite( term ) )
			{
				sum += term;
			}
		}
	}
	status.sceneDb = sum > 0.0f && isfinite( sum ) ? frame.hdr.referenceDb + 10.0f * log10f( sum ) : SND_HDR_SILENT;
	if ( frame.hdrOn )
	{
		SndHdr_UpdateWindow( memory.window, status.sceneDb, 0.01f, frame.hdr );
		if ( memory.window.top != frame.hdr.minTopDb )
		{
			status.windowFactor = SndHdr_Gain( frame.hdr.referenceDb, memory.window.top, frame.hdr.headroomDb );
		}
	}
	status.windowTopDb = memory.window.top;
	// Host stage: clear
	float* world[SND_MIX_MAX_OUT];
	const float* readWorld[SND_MIX_MAX_OUT];
	float* protectedRows[SND_MIX_MAX_OUT];
	float* mixWorld[SND_MIX_MAX_OUT];
	int wToOut[SND_MIX_MAX_OUT];
	int outToW[SND_MIX_MAX_OUT];
	int numW = 0;
	for ( int o = 0; o < frame.numOut; o++ )
	{
		protectedRows[o] = memory.protectedRows[o];
		memset( protectedRows[o], 0, SND_MIX_BLOCK * sizeof( float ) );
		outToW[o] = -1;
		if ( o == frame.lfeOut )
		{
			mixWorld[o] = protectedRows[o];
		}
		else
		{
			outToW[o] = numW;
			wToOut[numW] = o;
			world[numW] = memory.world[numW];
			readWorld[numW] = world[numW];
			mixWorld[o] = world[numW];
			memset( world[numW], 0, SND_MIX_BLOCK * sizeof( float ) );
			numW++;
		}
	}
	memset( memory.send, 0, sizeof( memory.send ) );
	// Host stage: mix
	bool sendLive = false;
	for ( int v = 0; v < SND_MIX_MAX_VOICES; v++ )
	{
		if ( scratchRow[v] < 0 )
		{
			continue;
		}
		const sndVoiceCmd_t& cmd = frame.voices[v];
		sndMixSlot_t& slot = memory.slots[v];
		const bool protectedVoice = ( cmd.flags & SND_VOICEFLAG_PROTECTED ) != 0;
		const float factor = protectedVoice ? 1.0f : status.windowFactor;
		float targetSend = last[v] ? 0.0f : cmd.sendGain * factor;
		targetSend = isfinite( targetSend ) ? targetSend : 0.0f;
		sendLive |= slot.previousSend != 0.0f || targetSend != 0.0f;
		SndVoice_Mix( slot.voice, cmd, memory.scratch[scratchRow[v]], factor, last[v] != 0,
			protectedVoice ? protectedRows : mixWorld, frame.numOut, memory.send );
		slot.previousSend = targetSend;
		//puncho - R-C3c-4 defines ended as not played; adoption already holds ended voices, so this changes no call.
		slot.played = !last[v] && !slot.voice.ended;
		status.voicesMixed++;
		status.voices[v].playing = 1;
		status.voices[v].rms = sqrtf( meanSquare[v] );
	}
	// Host stage: reverb
	for ( int r = 0; r < 2; r++ )
	{
		const sndMixReverb_t& reverb = frame.reverb[r];
		if ( !reverb.live || reverb.convolver == NULL )
		{
			continue;
		}
		reverb.convolver->Process( sendLive ? memory.send : NULL, reverb.inGain, reverb.outGain, reverb.outGain,
			world[outToW[frame.wetOut[0]]], world[outToW[frame.wetOut[1]]] );
		status.reverbTail[r] = reverb.convolver->TailBlocksLeft();
	}
	// Host stage: world
	SndMaster_World( memory.master, frame.master, world, numW, &status.master );
	// Host stage: final
	SndMaster_Final( memory.master, frame.master, readWorld, wToOut, numW, protectedRows, frame.numOut, 1.0f, out, &status.master );
	// Host stage: status
	status.blocks = ++memory.blocks;
	for ( int v = 0; v < SND_MIX_MAX_VOICES; v++ )
	{
		status.voices[v].generation = memory.slots[v].voice.generation;
		status.voices[v].ended = memory.slots[v].voice.ended;
	}
	if ( frame.meters )
	{
		for ( int o = 0; o < frame.numOut; o++ )
		{
			double squares = 0.0;
			float peak = 0.0f;
			for ( int n = 0; n < SND_MIX_BLOCK; n++ )
			{
				const float sample = out[n * frame.numOut + o];
				squares += (double)sample * sample;
				peak = fabsf( sample ) > peak ? fabsf( sample ) : peak;
			}
			status.outRms[o] = (float)sqrt( squares / SND_MIX_BLOCK );
			status.outPeak[o] = peak;
		}
	}
}
