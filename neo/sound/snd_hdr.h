//puncho - H1-W: standalone HDR control; no engine state.
#ifndef __SND_HDR_H__
#define __SND_HDR_H__

#define SND_HDR_SILENT ( -1.0e30f )
#define SND_HDR_MIN_DB ( -200.0f )
#define SND_HDR_MAX_DB ( 200.0f )
#define SND_HDR_MAX_PLAYING 64

enum sndHdrVerdict_t
{
	SND_HDR_PLAY,
	SND_HDR_BELOW,
	SND_HDR_OVER_BUDGET,
	SND_HDR_REJECTED,
	SND_HDR_PENDING
};

enum sndHdrPriority_t
{
	SND_HDR_PRIORITY_NONE,
	SND_HDR_PRIORITY_LOW,
	SND_HDR_PRIORITY_MEDIUM,
	SND_HDR_PRIORITY_HIGH,
	SND_HDR_PRIORITY_PERMANENT
};

enum sndHdrFlags_t
{
	SND_HDR_RMS_VALID = 1 << 0,
	SND_HDR_NEW = 1 << 1,
	SND_HDR_LOOP = 1 << 2
};

struct sndHdrParms_t
{
	float	minTopDb;
	float	sizeDb;
	float	releaseTime;
	float	headroomDb;
	float	referenceDb;
	float	priorityBiasDb[4];
	int		maxVoices;
};

struct sndHdrWindow_t
{
	float	env;		// The only state: the scene envelope.
	float	top;		// Outputs only; always derived from env and the parameters.
	float	bottom;
};

struct sndHdrVoice_t
{
	float	attenuatedDb;
	float	rms;
	float	rmsBound;
	float	perceivedDb;
	float	gain;
	int		stableId;
	unsigned char	priority;
	unsigned char	flags;
	unsigned char	verdict;
	unsigned char	reserved;	// Explicit tail byte; no implicit padding holes.
};

// One caller at a time; no lock here.
// Parameters must be cleaned once by SndHdr_CleanParms before use by other functions.
void SndHdr_DefaultParms( sndHdrParms_t& parms );
// Call once after changing parameters, outside the block. All other functions
// assume cleaned parameters, including standalone referenceDb/headroomDb arguments.
void SndHdr_CleanParms( sndHdrParms_t& parms );
float SndHdr_Perceived( float attenuatedDb, float rms );
float SndHdr_Aggregate( const float* levelsDb, int num );
void SndHdr_ResetWindow( sndHdrWindow_t& window, const sndHdrParms_t& parms );
void SndHdr_UpdateWindow( sndHdrWindow_t& window, float sceneDb, float dt, const sndHdrParms_t& parms );
float SndHdr_Gain( float attenuatedDb, float top, float headroomDb );
float SndHdr_LevelFromEngineDb( float engineDb, float referenceDb );

// scratch has num ints. Priorities above 4 read as 0; live stableIds are unique.
void SndHdr_Block( sndHdrVoice_t* voices, int num, int* scratch, sndHdrWindow_t& window,
	const sndHdrParms_t& parms, float dt );

#endif
