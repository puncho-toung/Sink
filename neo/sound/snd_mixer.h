//puncho: Plain, caller-owned mixer interface (WP-C3c section 5.1).
#ifndef __SND_MIXER_H__
#define __SND_MIXER_H__

#include "snd_voice.h"
#include "snd_hdr.h"
#include "snd_master.h"
#include "snd_convolver.h"

#define SND_MIX_RATE				44100
#define SND_MIX_BLOCK				441
#define SND_MIX_MAX_VOICES			48		// MAX_HARDWARE_VOICES
#define SND_MIX_MAX_SRC_CHANNELS	64		// MAX_HARDWARE_CHANNELS
#define SND_MIX_MAX_OUT				8

struct sndMixReverb_t
{
	idSoundConvolver*	convolver;		// NULL: none
	float				inGain;			// the send row's gain into this instance
	float				outGain;		// its return's gain
	unsigned char		live;			// the audio thread owns the instance and may call Process
	unsigned char		reserved[3];
};

struct sndMixFrame_t					// everything one block needs; plain data, copied whole
{
	unsigned long long	sequence;		// the link's (WP-C3d), 64 bits; the mixer copies it into the status
	unsigned int		resetSerial;	// a new value: a new stream. Every voice idle, the window and the chain reset
	unsigned int		hdrSerial;		// a new value: the window reset
	int					numOut;			// 1 to 8; changes only with resetSerial
	int					lfeOut;			// the LFE's output index; -1: none
	int					wetOut[2];		// the outputs the reverb's left and right return to
	unsigned char		hdrOn;
	unsigned char		meters;			// fill the status's per-output levels
	unsigned char		reserved[2];
	unsigned int		publishUs;		// the publisher's clock, for the sink's measure of the wait; the mixer ignores it
	sndHdrParms_t		hdr;			// cleaned, with referenceDb = minTopDb + headroomDb
	sndMasterCoefs_t	master;			// prepared
	sndMixReverb_t		reverb[2];
	sndVoiceCmd_t		voices[SND_MIX_MAX_VOICES];
};

struct sndMixVoiceStatus_t { unsigned int generation; float rms; unsigned char playing; unsigned char ended; unsigned char reserved[2]; };

struct sndMixStatus_t					// what one block did; plain data, copied whole
{
	unsigned long long	sequence;		// the frame's
	unsigned int		blocks;			// blocks since the last reset
	int					voicesMixed;
	int					voicesSkipped;	// voices left out by the source-channel budget, last blocks included
	float				sceneDb;		// the window's reading of this block; SND_HDR_SILENT: nothing
	float				windowTopDb;
	float				windowFactor;
	sndMasterInfo_t		master;
	float				outRms[SND_MIX_MAX_OUT];	// when frame.meters
	float				outPeak[SND_MIX_MAX_OUT];
	int					reverbTail[2];	// TailBlocksLeft of a live instance; -1: not live
	unsigned int		blockUs;		// the caller's fields from here: the mixer does not touch them
	unsigned int		maxBlockUs;
	unsigned int		lateBlocks;
	unsigned int		waitUs;
	sndMixVoiceStatus_t	voices[SND_MIX_MAX_VOICES];
};

class idSoundMixer						// one caller at a time; the calling thread has flush-to-zero and denormals-are-zero set
{
public:
						idSoundMixer();
	static size_t		MemoryBytes();
	bool				Init( void* memory );	// MemoryBytes() bytes, 16-aligned; clears every byte of it
	void				Shutdown();
	void				Block( const sndMixFrame_t& frame, sndMixStatus_t& status, float* out );	// 441 * numOut floats, interleaved

private:
	void*				storage;		//puncho - DR-288: caller-owned state, never a frame or sample pointer
};

#endif // __SND_MIXER_H__
