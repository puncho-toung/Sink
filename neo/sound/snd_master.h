//puncho - M1-K: plain code, no engine state; one caller at a time.
// The calling thread has flush-to-zero and denormals-are-zero set.
// Prepare takes parameters cleaned by SndMaster_CleanParms.
// World and Final read a coefficient set no other thread is writing: hand the audio thread a copy.
#ifndef __SND_MASTER_H__
#define __SND_MASTER_H__

#define SND_MASTER_RATE			44100
#define SND_MASTER_BLOCK		441
#define SND_MASTER_PIECE		21		// frames in a piece, and pieces in a block
#define SND_MASTER_MAX_CHANNELS	8
#define SND_MASTER_FADE_BLOCKS	5		// a changed shelf setting crossfades for five blocks
#define SND_MASTER_RAMP_PIECES	105		// a changed make-up glides for the same five blocks
#define SND_MASTER_KNEE_DB		( 6.0f )
#define SND_MASTER_DETECT_MS	( 2.0f )

struct sndMasterParms_t				// what a preset holds, in the user's units
{
	float	lowHz;					// low shelf corner
	float	lowDb;					// low shelf gain; 0 is no filter
	float	highHz;					// high shelf corner
	float	highDb;					// high shelf gain; 0 is no filter
	float	compThresholdDb;		// dBFS of the detector's measure
	float	compRatio;
	float	compAttackMs;
	float	compReleaseMs;
	float	makeupDb;				// a trim of W after the compressor, on or off
	float	limitCeilingDb;
	float	limitHoldMs;
	float	limitReleaseMs;
	unsigned char	eqOn;
	unsigned char	compOn;
	unsigned char	linkPower;		// 0: the square of the channels' sum; 1: the sum of their squares
	unsigned char	reserved;
};

struct sndMasterCoefs_t				// what SndMaster_Prepare makes of the parameters; plain data, copied whole
{
	float	glide[11];				// low b0 b1 b2 a1 a2, high b0 b1 b2 a1 a2, make-up in dB
	float	compThresholdDb;
	float	compSlope;				// 1 - 1 / ratio; 0 with the compressor off
	float	detectCoef;				// per piece
	float	attackCoef;				// per piece
	float	releaseCoef;			// per piece
	float	limitCeiling;			// linear
	float	limitReleaseCoef;		// per frame
	int		limitHoldFrames;
	int		linkPower;
};

struct sndMasterState_t
{
	float	shelf[10];				// the shelves in use: low b0 b1 b2 a1 a2, high b0 b1 b2 a1 a2
	float	shelfNew[10];			// the shelves fading in, while fadeLeft is above 0
	int		fadeLeft;				// blocks left of a shelf crossfade; 0 at rest
	float	makeupDb;				// the make-up in use
	float	makeupTarget;
	float	makeupStep;				// per piece
	int		makeupLeft;				// pieces left of a make-up glide; 0 at rest
	float	z[SND_MASTER_MAX_CHANNELS][4];		// per W row: low z1 z2, high z1 z2
	float	zNew[SND_MASTER_MAX_CHANNELS][4];	// the same for the shelves fading in
	float	detect;					// the compressor's smoothed power
	float	reductionDb;			// the compressor's smoothed reduction, never negative
	float	compGain;				// the gain at the end of the last piece
	float	limitGain;
	int		limitHold;
	float	userGain;				// the user's gain at the end of the last block
};

struct sndMasterInfo_t				// what one block did
{
	float	compMinGain;			// smallest compressor gain of the block, make-up included
	float	compSumReductionDb;		// the 21 pieces' reductions in dB, summed
	float	limitInPeak;			// largest magnitude that reached the limiter
	float	limitMinGain;			// smallest limiter gain of the block
	float	outPeak;				// largest magnitude written to out
};

void SndMaster_DefaultParms( sndMasterParms_t& parms );
void SndMaster_CleanParms( sndMasterParms_t& parms );
void SndMaster_Prepare( sndMasterCoefs_t& coefs, const sndMasterParms_t& parms );
void SndMaster_Reset( sndMasterState_t& state, const sndMasterCoefs_t& coefs, float userGain );
void SndMaster_World( sndMasterState_t& state, const sndMasterCoefs_t& coefs, float* const* w, int numW,
	sndMasterInfo_t* info );
void SndMaster_Final( sndMasterState_t& state, const sndMasterCoefs_t& coefs, const float* const* w,
	const int* wToOut, int numW, float* const* p, int numOut, float userGain, float* out, sndMasterInfo_t* info );

#endif
