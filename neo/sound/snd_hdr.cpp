//puncho - H1-W: the same arithmetic is compiled by the standalone acceptance host.
#include "snd_hdr.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

static inline float SndHdr_ReadLevel( float level )
{
	if ( !( level >= SND_HDR_MIN_DB ) )
	{
		return SND_HDR_SILENT;
	}
	return level > SND_HDR_MAX_DB ? SND_HDR_MAX_DB : level;
}

void SndHdr_DefaultParms( sndHdrParms_t& parms )
{
	parms.minTopDb = 55.0f;
	parms.sizeDb = 35.0f;
	parms.releaseTime = 0.5f;
	parms.headroomDb = 12.0f;
	parms.referenceDb = 67.0f;
	parms.priorityBiasDb[0] = 0.0f;
	parms.priorityBiasDb[1] = 3.0f;
	parms.priorityBiasDb[2] = 6.0f;
	parms.priorityBiasDb[3] = 9.0f;
	parms.maxVoices = 48;
}

static float SndHdr_ClampParm( float value, float minimum, float maximum, float fallback )
{
	if ( isnan( value ) )
	{
		return fallback;
	}
	return value < minimum ? minimum : ( value > maximum ? maximum : value );
}

void SndHdr_CleanParms( sndHdrParms_t& parms )
{
	sndHdrParms_t defaults;
	SndHdr_DefaultParms( defaults );
	parms.minTopDb = SndHdr_ClampParm( parms.minTopDb, SND_HDR_MIN_DB, SND_HDR_MAX_DB, defaults.minTopDb );
	parms.referenceDb = SndHdr_ClampParm( parms.referenceDb, SND_HDR_MIN_DB, SND_HDR_MAX_DB, defaults.referenceDb );
	parms.sizeDb = SndHdr_ClampParm( parms.sizeDb, 0.0f, 400.0f, defaults.sizeDb );
	parms.releaseTime = SndHdr_ClampParm( parms.releaseTime, 0.0f, 60.0f, 0.0f );
	parms.headroomDb = SndHdr_ClampParm( parms.headroomDb, -60.0f, 60.0f, defaults.headroomDb );
	for ( int i = 0; i < 4; i++ )
	{
		parms.priorityBiasDb[i] = SndHdr_ClampParm( parms.priorityBiasDb[i], -200.0f, 200.0f, 0.0f );
	}
	parms.maxVoices = parms.maxVoices < 0 ? 0 : ( parms.maxVoices > SND_HDR_MAX_PLAYING ? SND_HDR_MAX_PLAYING : parms.maxVoices );
}

float SndHdr_Perceived( float attenuatedDb, float rms )
{
	attenuatedDb = SndHdr_ReadLevel( attenuatedDb );
	if ( attenuatedDb == SND_HDR_SILENT || !( rms > 0.0f ) )
	{
		return SND_HDR_SILENT;
	}
	rms = rms > 1000.0f ? 1000.0f : rms;
	const float level = (float)( (double)attenuatedDb + 20.0 * log10( (double)rms ) );
	return SndHdr_ReadLevel( level );
}

// Both callers use this reduction, without copying strided voice fields into scratch.
static float SndHdr_AggregateInput( const float* levelsDb, const sndHdrVoice_t* voices, int index )
{
	if ( levelsDb != NULL )
	{
		return SndHdr_ReadLevel( levelsDb[index] );
	}
	const sndHdrVoice_t& voice = voices[index];
	return ( voice.verdict == SND_HDR_REJECTED || voice.verdict == SND_HDR_PENDING ) ?
		SND_HDR_SILENT : SndHdr_ReadLevel( voice.perceivedDb );
}

static float SndHdr_AggregateInputs( const float* levelsDb, const sndHdrVoice_t* voices, int num )
{
	float maximum = SND_HDR_SILENT;
	for ( int i = 0; i < num; i++ )
	{
		const float level = SndHdr_AggregateInput( levelsDb, voices, i );
		if ( level > maximum )
		{
			maximum = level;
		}
	}
	if ( maximum == SND_HDR_SILENT )
	{
		return SND_HDR_SILENT;
	}
	double sum = 0.0;
	for ( int i = 0; i < num; i++ )
	{
		const float level = SndHdr_AggregateInput( levelsDb, voices, i );
		if ( level != SND_HDR_SILENT )
		{
			sum += pow( 10.0, ( (double)level - maximum ) / 10.0 );
		}
	}
	return SndHdr_ReadLevel( (float)( (double)maximum + 10.0 * log10( sum ) ) );
}

float SndHdr_Aggregate( const float* levelsDb, int num )
{
	return SndHdr_AggregateInputs( levelsDb, NULL, num );
}

static inline float SndHdr_Bottom( float env, const sndHdrParms_t& parms )
{
	return env > parms.minTopDb ? (float)( (double)env - parms.sizeDb ) : SND_HDR_SILENT;
}

static void SndHdr_DeriveWindow( sndHdrWindow_t& window, const sndHdrParms_t& parms )
{
	window.top = window.env > parms.minTopDb ? window.env : parms.minTopDb;
	window.bottom = SndHdr_Bottom( window.env, parms );
}

void SndHdr_ResetWindow( sndHdrWindow_t& window, const sndHdrParms_t& parms )
{
	window.env = (float)( (double)parms.minTopDb - parms.sizeDb );
	SndHdr_DeriveWindow( window, parms );
}

static void SndHdr_CheckWindow( sndHdrWindow_t& window, const sndHdrParms_t& parms )
{
	const float env = SndHdr_ReadLevel( window.env );
	if ( env == SND_HDR_SILENT || env != window.env )
	{
		SndHdr_ResetWindow( window, parms );
	}
}

void SndHdr_UpdateWindow( sndHdrWindow_t& window, float sceneDb, float dt, const sndHdrParms_t& parms )
{
	SndHdr_CheckWindow( window, parms );
	sceneDb = SndHdr_ReadLevel( sceneDb );
	const float floorDb = (float)( (double)parms.minTopDb - parms.sizeDb );
	const float target = sceneDb > floorDb ? sceneDb : floorDb;
	if ( target >= window.env )
	{
		window.env = target;
	}
	else if ( isfinite( dt ) && dt > 0.0f )
	{
		window.env = parms.releaseTime == 0.0f ? target :
			(float)( (double)target + ( (double)window.env - target ) * exp( -(double)dt / parms.releaseTime ) );
	}
	SndHdr_DeriveWindow( window, parms );
}

float SndHdr_Gain( float attenuatedDb, float top, float headroomDb )
{
	attenuatedDb = SndHdr_ReadLevel( attenuatedDb );
	top = SndHdr_ReadLevel( top );
	if ( attenuatedDb == SND_HDR_SILENT || top == SND_HDR_SILENT )
	{
		return 0.0f;
	}
	return (float)pow( 10.0, ( (double)attenuatedDb - top - headroomDb ) / 20.0 );
}

float SndHdr_LevelFromEngineDb( float engineDb, float referenceDb )
{
	engineDb = SndHdr_ReadLevel( engineDb );
	if ( engineDb == SND_HDR_SILENT )
	{
		return SND_HDR_SILENT;
	}
	// DBtoLinear in snd_local.h uses 2^(db/6). log10(2) is constant here.
	const double engineDbScale = 20.0 * 0.30102999566398119521 / 6.0;
	return SndHdr_ReadLevel( (float)( (double)referenceDb + (double)engineDb * engineDbScale ) );
}

static inline int SndHdr_ReadPriority( unsigned char priority )
{
	return priority > SND_HDR_PRIORITY_PERMANENT ? SND_HDR_PRIORITY_NONE : priority;
}

static bool SndHdr_Precedes( const sndHdrVoice_t& a, const sndHdrVoice_t& b, const sndHdrParms_t& parms )
{
	const int priorityA = SndHdr_ReadPriority( a.priority );
	const int priorityB = SndHdr_ReadPriority( b.priority );
	const bool permanentA = priorityA == SND_HDR_PRIORITY_PERMANENT;
	const bool permanentB = priorityB == SND_HDR_PRIORITY_PERMANENT;
	if ( permanentA != permanentB )
	{
		return permanentA;
	}
	const double scoreA = permanentA ? a.perceivedDb : (double)a.perceivedDb + parms.priorityBiasDb[priorityA];
	const double scoreB = permanentB ? b.perceivedDb : (double)b.perceivedDb + parms.priorityBiasDb[priorityB];
	return scoreA == scoreB ? a.stableId < b.stableId : scoreA > scoreB;
}

void SndHdr_Block( sndHdrVoice_t* voices, int num, int* scratch, sndHdrWindow_t& window,
	const sndHdrParms_t& parms, float dt )
{
	SndHdr_CheckWindow( window, parms );
	const float previousBottom = SndHdr_Bottom( window.env, parms );
	for ( int i = 0; i < num; i++ )
	{
		sndHdrVoice_t& voice = voices[i];
		voice.perceivedDb = SndHdr_Perceived( voice.attenuatedDb,
			( voice.flags & SND_HDR_RMS_VALID ) != 0 ? voice.rms : voice.rmsBound );
		voice.gain = 0.0f;
		voice.verdict = SND_HDR_BELOW;
	}
	for ( int i = 0; i < num; i++ )
	{
		sndHdrVoice_t& voice = voices[i];
		if ( ( voice.flags & SND_HDR_NEW ) != 0 &&
			SndHdr_Perceived( voice.attenuatedDb, voice.rmsBound ) < previousBottom )
		{
			voice.verdict = (unsigned char)( ( voice.flags & SND_HDR_LOOP ) != 0 ? SND_HDR_PENDING : SND_HDR_REJECTED );
		}
	}
	const float sceneDb = SndHdr_AggregateInputs( NULL, voices, num );
	SndHdr_UpdateWindow( window, sceneDb, dt, parms );
	const float bottom = SndHdr_Bottom( window.env, parms );
	int kept = 0;
	for ( int i = 0; i < num; i++ )
	{
		sndHdrVoice_t& voice = voices[i];
		if ( voice.verdict == SND_HDR_REJECTED || voice.verdict == SND_HDR_PENDING ||
			voice.perceivedDb == SND_HDR_SILENT || voice.perceivedDb < bottom )
		{
			continue;
		}
		voice.verdict = SND_HDR_OVER_BUDGET;
		if ( parms.maxVoices == 0 || ( kept == parms.maxVoices &&
			!SndHdr_Precedes( voice, voices[scratch[kept - 1]], parms ) ) )
		{
			continue;
		}
		// Keep only the best K: at most num*K index writes, including placements.
		int moveEnd = kept;
		if ( kept == parms.maxVoices )
		{
			moveEnd--;
		}
		else
		{
			kept++;
		}
		int low = 0;
		int high = moveEnd;
		while ( low < high )
		{
			const int middle = low + ( high - low ) / 2;
			if ( SndHdr_Precedes( voice, voices[scratch[middle]], parms ) )
			{
				high = middle;
			}
			else
			{
				low = middle + 1;
			}
		}
		if ( low < moveEnd )
		{
			memmove( &scratch[low + 1], &scratch[low], (size_t)( moveEnd - low ) * sizeof( scratch[0] ) );
		}
		scratch[low] = i;
	}
	for ( int i = 0; i < kept; i++ )
	{
		sndHdrVoice_t& voice = voices[scratch[i]];
		voice.verdict = SND_HDR_PLAY;
		voice.gain = SndHdr_Gain( voice.attenuatedDb, window.top, parms.headroomDb );
	}
}
