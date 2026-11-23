//puncho - C3b-V: plain voice DSP, one caller at a time, no engine state.
// The calling thread has flush-to-zero and denormals-are-zero set.
#ifndef __SND_VOICE_H__
#define __SND_VOICE_H__

#include <stdint.h>

#define SND_VOICE_RATE			44100
#define SND_VOICE_BLOCK			441
#define SND_VOICE_MAX_OUT		8
#define SND_VOICE_MAX_STEP		8		// source frames per output frame, at most
#define SND_VOICE_FETCH_FRAMES	( SND_VOICE_BLOCK * SND_VOICE_MAX_STEP + 8 )
#define SND_VOICE_ADPCM_FRAMES	128		// maximum frames in one MS-ADPCM block
#ifndef SND_VOICE_EDGE_FRAMES
#define SND_VOICE_EDGE_FRAMES	40		// first/last gain edges; a test may use 0 for immediate steps
#endif

enum { SND_SAMPLE_NONE, SND_SAMPLE_PCM16, SND_SAMPLE_MSADPCM };
enum { SND_VOICE_OFF, SND_VOICE_PLAY, SND_VOICE_PAUSED };
enum { SND_VOICEFLAG_PROTECTED = 1 };	// read by the mixer only

struct sndSampleView_t			// audio-thread sample view; plain data, copied whole
{
	const unsigned char*	data;		// interleaved native-endian PCM16, or whole MS-ADPCM blocks
	int				numFrames;		// frames that play
	int				rate;			// Hz
	short			channels;		// 1 or 2
	short			format;			// SND_SAMPLE_*
	short			blockBytes;		// MS-ADPCM bytes per block
	short			blockFrames;	// MS-ADPCM frames per block
	short			coef[7][2];		// MS-ADPCM's seven predictor pairs
};

struct sndVoiceCmd_t			// main-thread request; plain data, copied whole
{
	unsigned int	generation;		// new value at every start; 0 means never started
	unsigned char	state;			// SND_VOICE_*; caller scheduling only, ignored by these functions
	unsigned char	flags;			// SND_VOICEFLAG_*; mixer only
	unsigned char	startInLoop;	// start lies in the loop sample
	unsigned char	reserved;		// reserved command byte
	int				startFrame;		// first frame in the sample the start lies in
	sndSampleView_t	leadin;			// sample played before the loop
	sndSampleView_t	loop;			// SND_SAMPLE_NONE means none; same channels/format as leadin
	float			pitch;			// playback-rate multiplier
	float			filterCoef;		// one-pole coefficient; 1 is open
	float			dryGain;		// dry gain before factor and matrix
	float			sendGain;		// mono-send gain before factor
	float			matrix[2][SND_VOICE_MAX_OUT];	// [ source channel ][ output ]
};

// Audio-thread state, one per slot; no pointers; all zero is idle.
// The caller may read generation, active and ended; the remaining fields are private to this path.
struct sndVoiceState_t
{
	uint64_t		position;		// 32.32, relative to the current sample
	unsigned int	generation;		// caller-readable generation taken at start
	int				readerFrame;	// next frame not yet fetched
	int				cacheBlock;		// decoded block index, or -1 when invalid
	int				carryCount;		// frames carried per channel; 0 at start, then 1 through 4
	bool			active;			// caller-readable: timeline has not ended this started voice
	bool			ended;			// caller-readable: timeline ended, including a lost loop
	bool			inLoop;			// playback position is in the loop sample
	bool			readerInLoop;	// forward reader has reached the loop sample
	bool			edgeIn;			// next mix starts from zero, after Start or a last mix
	short			decoded[SND_VOICE_ADPCM_FRAMES * 2];	// one interleaved decoded block
	float			carry[2][4];		// planar frames retained for the next fetch window
	float			z[2];			// last one-pole output per source channel
	float			filterCoef;		// coefficient last applied
	float			dryGain[2][SND_VOICE_MAX_OUT];	// last applied dry products, zero after last
	float			sendGain;		// last applied send product, zero after last
};

// Structural validation only: reads no sample bytes.
bool	SndSample_ViewValid( const sndSampleView_t& view );
// A valid block writes blockFrames * channels interleaved shorts to out.
int		SndAdpcm_DecodeBlock( const sndSampleView_t& view, int block, short* out );
// Clears the whole state, including padding, so states can be compared by their bytes.
void	SndVoice_Start( sndVoiceState_t& state, const sndVoiceCmd_t& cmd );
// Caller-owned fetch: 2 * SND_VOICE_FETCH_FRAMES floats, reusable across voices.
// Caller-owned scratch: 441 * channels floats, planar; returns the block's mean square.
// The caller schedules calls; Render and Mix ignore cmd.state. No buffer alignment is assumed.
float	SndVoice_Render( sndVoiceState_t& state, const sndVoiceCmd_t& cmd, float* fetch, float* scratch );
// rows: numOut rows of 441 floats; send: one row of 441 floats, or NULL.
// last applies the edge out and arms the next edge in; active, position and filter history are preserved.
void	SndVoice_Mix( sndVoiceState_t& state, const sndVoiceCmd_t& cmd, const float* scratch, float factor,
			bool last, float* const* rows, int numOut, float* send );

#endif
