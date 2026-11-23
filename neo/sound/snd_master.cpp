//puncho - M1-K: the engine and standalone hosts compile this same plain code.
#include "snd_master.h"
#include <math.h>
#include <string.h>
#include <float.h>
#include <assert.h>

static float SndMaster_ClampParm( float value, float minimum, float maximum, float fallback )
{
	if ( !isfinite( value ) )
	{
		return fallback;
	}
	return value < minimum ? minimum : ( value > maximum ? maximum : value );
}

static float SndMaster_UserGain( float gain )
{
	return !isfinite( gain ) || gain < 0.0f ? 0.0f : ( gain > 64.0f ? 64.0f : gain );
}

void SndMaster_DefaultParms( sndMasterParms_t& parms )
{
	memset( &parms, 0, sizeof( parms ) );
	parms.lowHz = 180.0f;
	parms.highHz = 2500.0f;
	parms.compThresholdDb = -18.0f;
	parms.compRatio = 3.0f;
	parms.compAttackMs = 2.0f;
	parms.compReleaseMs = 120.0f;
	parms.limitCeilingDb = -1.0f;
	parms.limitHoldMs = 30.0f;
	parms.limitReleaseMs = 50.0f;
}

void SndMaster_CleanParms( sndMasterParms_t& parms )
{
	sndMasterParms_t defaults;
	SndMaster_DefaultParms( defaults );
	parms.lowHz = SndMaster_ClampParm( parms.lowHz, 60.0f, 400.0f, defaults.lowHz );
	parms.lowDb = SndMaster_ClampParm( parms.lowDb, -12.0f, 12.0f, defaults.lowDb );
	parms.highHz = SndMaster_ClampParm( parms.highHz, 1000.0f, 8000.0f, defaults.highHz );
	parms.highDb = SndMaster_ClampParm( parms.highDb, -12.0f, 12.0f, defaults.highDb );
	parms.compThresholdDb = SndMaster_ClampParm( parms.compThresholdDb, -60.0f, 0.0f, defaults.compThresholdDb );
	parms.compRatio = SndMaster_ClampParm( parms.compRatio, 1.0f, 20.0f, defaults.compRatio );
	parms.compAttackMs = SndMaster_ClampParm( parms.compAttackMs, 0.0f, 50.0f, defaults.compAttackMs );
	parms.compReleaseMs = SndMaster_ClampParm( parms.compReleaseMs, 10.0f, 2000.0f, defaults.compReleaseMs );
	parms.makeupDb = SndMaster_ClampParm( parms.makeupDb, -12.0f, 12.0f, defaults.makeupDb );
	parms.limitCeilingDb = SndMaster_ClampParm( parms.limitCeilingDb, -12.0f, 0.0f, defaults.limitCeilingDb );
	parms.limitHoldMs = SndMaster_ClampParm( parms.limitHoldMs, 0.0f, 200.0f, defaults.limitHoldMs );
	parms.limitReleaseMs = SndMaster_ClampParm( parms.limitReleaseMs, 1.0f, 2000.0f, defaults.limitReleaseMs );
	parms.eqOn = parms.eqOn != 0;
	parms.compOn = parms.compOn != 0;
	parms.linkPower = parms.linkPower != 0;
	parms.reserved = 0;
}

// WF-13 / the measured DesignShelf law, evaluated in double before storage.
static void SndMaster_DesignShelf( float* coefs, bool high, double hz, double db, bool enabled )
{
	if ( !enabled || fabs( db ) <= 0.005 )
	{
		coefs[0] = 1.0f;
		coefs[1] = coefs[2] = coefs[3] = coefs[4] = 0.0f;
		return;
	}
	const double a = pow( 10.0, db / 40.0 );
	const double angle = 6.28318530717958647693 * hz / SND_MASTER_RATE;
	const double c = cos( angle );
	const double t = sin( angle ) * sqrt( 2.0 * a );
	double b0, b1, b2, a0, a1, a2;
	if ( high )
	{
		b0 = a * ( ( a + 1.0 ) + ( a - 1.0 ) * c + t );
		b1 = -2.0 * a * ( ( a - 1.0 ) + ( a + 1.0 ) * c );
		b2 = a * ( ( a + 1.0 ) + ( a - 1.0 ) * c - t );
		a0 = ( a + 1.0 ) - ( a - 1.0 ) * c + t;
		a1 = 2.0 * ( ( a - 1.0 ) - ( a + 1.0 ) * c );
		a2 = ( a + 1.0 ) - ( a - 1.0 ) * c - t;
	}
	else
	{
		b0 = a * ( ( a + 1.0 ) - ( a - 1.0 ) * c + t );
		b1 = 2.0 * a * ( ( a - 1.0 ) - ( a + 1.0 ) * c );
		b2 = a * ( ( a + 1.0 ) - ( a - 1.0 ) * c - t );
		a0 = ( a + 1.0 ) + ( a - 1.0 ) * c + t;
		a1 = -2.0 * ( ( a - 1.0 ) + ( a + 1.0 ) * c );
		a2 = ( a + 1.0 ) + ( a - 1.0 ) * c - t;
	}
	coefs[0] = (float)( b0 / a0 );
	coefs[1] = (float)( b1 / a0 );
	coefs[2] = (float)( b2 / a0 );
	coefs[3] = (float)( a1 / a0 );
	coefs[4] = (float)( a2 / a0 );
}

void SndMaster_Prepare( sndMasterCoefs_t& coefs, const sndMasterParms_t& parms )
{
	SndMaster_DesignShelf( coefs.glide, false, parms.lowHz, parms.lowDb, parms.eqOn != 0 );
	SndMaster_DesignShelf( coefs.glide + 5, true, parms.highHz, parms.highDb, parms.eqOn != 0 );
	coefs.glide[10] = parms.makeupDb;
	coefs.compThresholdDb = parms.compThresholdDb;
	coefs.compSlope = parms.compOn != 0 ? (float)( 1.0 - 1.0 / parms.compRatio ) : 0.0f;
	coefs.detectCoef = (float)exp( -21.0 / ( 44.1 * SND_MASTER_DETECT_MS ) );
	coefs.attackCoef = parms.compAttackMs == 0.0f ? 0.0f : (float)exp( -21.0 / ( 44.1 * parms.compAttackMs ) );
	coefs.releaseCoef = (float)exp( -21.0 / ( 44.1 * parms.compReleaseMs ) );
	coefs.limitCeiling = (float)pow( 10.0, parms.limitCeilingDb / 20.0 );
	coefs.limitReleaseCoef = (float)exp( -1.0 / ( 44.1 * parms.limitReleaseMs ) );
	coefs.limitHoldFrames = (int)( parms.limitHoldMs * 44.1 + 0.5 );
	coefs.linkPower = parms.linkPower;
}

void SndMaster_Reset( sndMasterState_t& state, const sndMasterCoefs_t& coefs, float userGain )
{
	memset( &state, 0, sizeof( state ) );
	memcpy( state.shelf, coefs.glide, sizeof( state.shelf ) );
	state.makeupDb = state.makeupTarget = coefs.glide[10];
	state.compGain = powf( 10.0f, coefs.glide[10] / 20.0f );
	state.limitGain = 1.0f;
	state.userGain = SndMaster_UserGain( userGain );
}

static bool SndMaster_Identity( const float* coefs )
{
	return coefs[0] == 1.0f && coefs[1] == 0.0f && coefs[2] == 0.0f && coefs[3] == 0.0f && coefs[4] == 0.0f;
}

static void SndMaster_Filter( float* row, int count, const float* coefs, float* z )
{
	float z1 = z[0], z2 = z[1];
	for ( int n = 0; n < count; n++ )
	{
		const float x = row[n];
		const float y = coefs[0] * x + z1;
		z1 = coefs[1] * x - coefs[3] * y + z2;
		z2 = coefs[2] * x - coefs[4] * y;
		row[n] = y;
	}
	z[0] = z1;
	z[1] = z2;
}

// The two recurrences overlap on the CPU; measured against separate row passes.
static void SndMaster_TwoShelves( float* row, int count, const float* b, float* z )
{
	float l1 = z[0], l2 = z[1], h1 = z[2], h2 = z[3];
	for ( int n = 0; n < count; n++ )
	{
		const float x = row[n];
		const float low = b[0] * x + l1;
		l1 = b[1] * x - b[3] * low + l2;
		l2 = b[2] * x - b[4] * low;
		const float high = b[5] * low + h1;
		h1 = b[6] * low - b[8] * high + h2;
		h2 = b[7] * low - b[9] * high;
		row[n] = high;
	}
	z[0] = l1;
	z[1] = l2;
	z[2] = h1;
	z[3] = h2;
}

// Independent rows overlap the recurrences without changing their arithmetic.
static void SndMaster_FilterPair( float* row0, float* row1, const float* b, float* z0, float* z1 )
{
	const float b0 = b[0], b1 = b[1], b2 = b[2], a1 = b[3], a2 = b[4];
	float l1 = z0[0], l2 = z0[1], r1 = z1[0], r2 = z1[1];
	for ( int n = 0; n < SND_MASTER_BLOCK; n++ )
	{
		const float x = row0[n], u = row1[n];
		const float y = b0 * x + l1, v = b0 * u + r1;
		l1 = b1 * x - a1 * y + l2;
		l2 = b2 * x - a2 * y;
		r1 = b1 * u - a1 * v + r2;
		r2 = b2 * u - a2 * v;
		row0[n] = y;
		row1[n] = v;
	}
	z0[0] = l1;
	z0[1] = l2;
	z1[0] = r1;
	z1[1] = r2;
}

static void SndMaster_TwoShelvesPair( float* row0, float* row1, const float* b, float* z0, float* z1 )
{
	const float b0 = b[0], b1 = b[1], b2 = b[2], b3 = b[3], b4 = b[4];
	const float b5 = b[5], b6 = b[6], b7 = b[7], b8 = b[8], b9 = b[9];
	float l1 = z0[0], l2 = z0[1], h1 = z0[2], h2 = z0[3];
	float m1 = z1[0], m2 = z1[1], k1 = z1[2], k2 = z1[3];
	for ( int n = 0; n < SND_MASTER_BLOCK; n++ )
	{
		const float x = row0[n];
		const float low = b0 * x + l1;
		l1 = b1 * x - b3 * low + l2;
		l2 = b2 * x - b4 * low;
		const float high = b5 * low + h1;
		h1 = b6 * low - b8 * high + h2;
		h2 = b7 * low - b9 * high;
		const float u = row1[n];
		const float lowU = b0 * u + m1;
		m1 = b1 * u - b3 * lowU + m2;
		m2 = b2 * u - b4 * lowU;
		const float highU = b5 * lowU + k1;
		k1 = b6 * lowU - b8 * highU + k2;
		k2 = b7 * lowU - b9 * highU;
		row0[n] = high;
		row1[n] = highU;
	}
	z0[0] = l1;
	z0[1] = l2;
	z0[2] = h1;
	z0[3] = h2;
	z1[0] = m1;
	z1[1] = m2;
	z1[2] = k1;
	z1[3] = k2;
}

static void SndMaster_CleanHistory( float* z )
{
	if ( !isfinite( z[0] ) || !isfinite( z[1] ) || !isfinite( z[2] ) || !isfinite( z[3] ) )
	{
		memset( z, 0, 4 * sizeof( float ) );
	}
	for ( int j = 0; j < 4; j++ )
	{
		z[j] = fabsf( z[j] ) < 1.0e-30f ? 0.0f : z[j];
	}
}

void SndMaster_World( sndMasterState_t& state, const sndMasterCoefs_t& coefs, float* const* w, int numW,
	sndMasterInfo_t* info )
{
	assert( numW >= 0 && numW <= SND_MASTER_MAX_CHANNELS );
	if ( state.fadeLeft == 0 )
	{
		bool changed = false;
		for ( int j = 0; j < 10; j++ )
		{
			changed |= coefs.glide[j] != state.shelf[j];
		}
		if ( changed )
		{
			memcpy( state.shelfNew, coefs.glide, sizeof( state.shelfNew ) );
			memset( state.zNew, 0, sizeof( state.zNew ) );
			state.fadeLeft = SND_MASTER_FADE_BLOCKS;
		}
	}
	if ( coefs.glide[10] != state.makeupTarget )
	{
		state.makeupTarget = coefs.glide[10];
		state.makeupStep = ( state.makeupTarget - state.makeupDb ) / SND_MASTER_RAMP_PIECES;
		state.makeupLeft = SND_MASTER_RAMP_PIECES;
	}
	float gains[SND_MASTER_BLOCK];
	const bool fading = state.fadeLeft > 0;
	const bool low = !SndMaster_Identity( state.shelf );
	const bool high = !SndMaster_Identity( state.shelf + 5 );
	if ( fading )
	{
		// The unfiltered row feeds both banks. Only one temporary row is needed.
		float scratch[SND_MASTER_BLOCK];
		const int offset = ( SND_MASTER_FADE_BLOCKS - state.fadeLeft ) * SND_MASTER_BLOCK;
		for ( int n = 0; n < SND_MASTER_BLOCK; n++ )
		{
			gains[n] = (float)( offset + n + 1 ) / ( SND_MASTER_FADE_BLOCKS * SND_MASTER_BLOCK );
		}
		if ( state.fadeLeft == 1 )
		{
			gains[SND_MASTER_BLOCK - 1] = 1.0f;
		}
		const bool newLow = !SndMaster_Identity( state.shelfNew );
		const bool newHigh = !SndMaster_Identity( state.shelfNew + 5 );
		for ( int c = 0; c < numW; c++ )
		{
			memcpy( scratch, w[c], sizeof( scratch ) );
			if ( low && high )
			{
				SndMaster_TwoShelves( w[c], SND_MASTER_BLOCK, state.shelf, state.z[c] );
			}
			else if ( low || high )
			{
				SndMaster_Filter( w[c], SND_MASTER_BLOCK, state.shelf + ( low ? 0 : 5 ), state.z[c] + ( low ? 0 : 2 ) );
			}
			if ( newLow && newHigh )
			{
				SndMaster_TwoShelves( scratch, SND_MASTER_BLOCK, state.shelfNew, state.zNew[c] );
			}
			else if ( newLow || newHigh )
			{
				SndMaster_Filter( scratch, SND_MASTER_BLOCK, state.shelfNew + ( newLow ? 0 : 5 ), state.zNew[c] + ( newLow ? 0 : 2 ) );
			}
			for ( int n = 0; n < SND_MASTER_BLOCK; n++ )
			{
				const float old = w[c][n];
				w[c][n] = old + gains[n] * ( scratch[n] - old );
			}
		}
	}
	else if ( low || high )
	{
		int c = 0;
		for ( ; c + 1 < numW; c += 2 )
		{
			if ( low && high )
			{
				SndMaster_TwoShelvesPair( w[c], w[c + 1], state.shelf, state.z[c], state.z[c + 1] );
			}
			else
			{
				const int b = low ? 0 : 5, z = low ? 0 : 2;
				SndMaster_FilterPair( w[c], w[c + 1], state.shelf + b, state.z[c] + z, state.z[c + 1] + z );
			}
		}
		if ( c < numW )
		{
			if ( low && high )
			{
				SndMaster_TwoShelves( w[c], SND_MASTER_BLOCK, state.shelf, state.z[c] );
			}
			else
			{
				SndMaster_Filter( w[c], SND_MASTER_BLOCK, state.shelf + ( low ? 0 : 5 ), state.z[c] + ( low ? 0 : 2 ) );
			}
		}
	}
	for ( int c = 0; c < numW; c++ )
	{
		SndMaster_CleanHistory( state.z[c] );
		if ( fading )
		{
			SndMaster_CleanHistory( state.zNew[c] );
		}
	}
	if ( fading )
	{
		state.fadeLeft--;
		if ( state.fadeLeft == 0 )
		{
			memcpy( state.shelf, state.shelfNew, sizeof( state.shelf ) );
			memcpy( state.z, state.zNew, sizeof( state.z ) );
		}
	}
	if ( state.makeupLeft == 0 && coefs.compSlope == 0.0f && state.makeupDb == 0.0f &&
		state.reductionDb == 0.0f && state.compGain == 1.0f )
	{
		state.detect = 0.0f;
		if ( info != NULL )
		{
			info->compMinGain = 1.0f;
			info->compSumReductionDb = 0.0f;
		}
		return;
	}
	float measure[SND_MASTER_BLOCK] = {};
	for ( int c = 0; c < numW; c++ )
	{
		const float* row = w[c];
		if ( coefs.linkPower != 0 )
		{
			for ( int n = 0; n < SND_MASTER_BLOCK; n++ )
			{
				measure[n] += row[n] * row[n];
			}
		}
		else
		{
			for ( int n = 0; n < SND_MASTER_BLOCK; n++ )
			{
				measure[n] += row[n];
			}
		}
	}
	if ( coefs.linkPower == 0 )
	{
		for ( int n = 0; n < SND_MASTER_BLOCK; n++ )
		{
			measure[n] *= measure[n];
		}
	}
	float minGain = FLT_MAX, sumReduction = 0.0f;
	bool unity = true;
	for ( int piece = 0; piece < SND_MASTER_PIECE; piece++ )
	{
		if ( state.makeupLeft > 0 )
		{
			state.makeupLeft--;
			state.makeupDb = state.makeupLeft == 0 ? state.makeupTarget : state.makeupDb + state.makeupStep;
		}
		const int start = piece * SND_MASTER_PIECE;
		// Independent accumulators let /fp:precise vectorize this reduction.
		float q0 = 0.0f, q1 = 0.0f, q2 = 0.0f, q3 = 0.0f;
		for ( int i = 0; i < SND_MASTER_PIECE - 1; i += 4 )
		{
			q0 += measure[start + i];
			q1 += measure[start + i + 1];
			q2 += measure[start + i + 2];
			q3 += measure[start + i + 3];
		}
		q0 += measure[start + SND_MASTER_PIECE - 1];
		float q = ( ( q0 + q1 ) + ( q2 + q3 ) ) * ( 1.0f / SND_MASTER_PIECE );
		if ( isfinite( q ) )
		{
			q = q > 1.0e4f ? 1.0e4f : q;
			state.detect = q + coefs.detectCoef * ( state.detect - q );
			state.detect = state.detect < 1.0e-30f ? 0.0f : state.detect;
			const float level = 10.0f * log10f( state.detect > 1.0e-12f ? state.detect : 1.0e-12f );
			const float d = level - coefs.compThresholdDb;
			float reduction = 0.0f;
			if ( d >= SND_MASTER_KNEE_DB * 0.5f )
			{
				reduction = coefs.compSlope * d;
			}
			else if ( d > -SND_MASTER_KNEE_DB * 0.5f )
			{
				const float knee = d + SND_MASTER_KNEE_DB * 0.5f;
				reduction = coefs.compSlope * knee * knee / ( 2.0f * SND_MASTER_KNEE_DB );
			}
			const float smoothing = reduction > state.reductionDb ? coefs.attackCoef : coefs.releaseCoef;
			state.reductionDb = reduction + smoothing * ( state.reductionDb - reduction );
			if ( reduction == 0.0f && state.reductionDb < 0.0001f )
			{
				state.reductionDb = 0.0f;
			}
		}
		const float exponent = ( state.makeupDb - state.reductionDb ) / 20.0f;
		const float endGain = exponent == 0.0f ? 1.0f : powf( 10.0f, exponent );
		const float step = ( endGain - state.compGain ) / SND_MASTER_PIECE;
		for ( int i = 0; i < SND_MASTER_PIECE - 1; i++ )
		{
			gains[start + i] = state.compGain + step * (float)( i + 1 );
		}
		gains[start + SND_MASTER_PIECE - 1] = endGain;
		const float pieceMin = gains[start] < endGain ? gains[start] : endGain;
		minGain = pieceMin < minGain ? pieceMin : minGain;
		unity &= state.compGain == 1.0f && endGain == 1.0f;
		state.compGain = endGain;
		sumReduction += state.reductionDb;
	}
	if ( !unity )
	{
		for ( int c = 0; c < numW; c++ )
		{
			for ( int n = 0; n < SND_MASTER_BLOCK; n++ )
			{
				w[c][n] *= gains[n];
			}
		}
	}
	if ( info != NULL )
	{
		info->compMinGain = minGain;
		info->compSumReductionDb = sumReduction;
	}
}

static float SndMaster_Peak( const float* row )
{
	float m0 = 0.0f, m1 = 0.0f, m2 = 0.0f, m3 = 0.0f;
	for ( int n = 0; n < SND_MASTER_BLOCK - 1; n += 4 )
	{
		m0 = row[n] > m0 ? row[n] : m0;
		m1 = row[n + 1] > m1 ? row[n + 1] : m1;
		m2 = row[n + 2] > m2 ? row[n + 2] : m2;
		m3 = row[n + 3] > m3 ? row[n + 3] : m3;
	}
	m0 = row[SND_MASTER_BLOCK - 1] > m0 ? row[SND_MASTER_BLOCK - 1] : m0;
	m0 = m1 > m0 ? m1 : m0;
	m2 = m3 > m2 ? m3 : m2;
	return m2 > m0 ? m2 : m0;
}

void SndMaster_Final( sndMasterState_t& state, const sndMasterCoefs_t& coefs, const float* const* w,
	const int* wToOut, int numW, float* const* p, int numOut, float userGain, float* out, sndMasterInfo_t* info )
{
	assert( numW >= 0 && numW <= SND_MASTER_MAX_CHANNELS );
	assert( numOut >= 1 && numOut <= SND_MASTER_MAX_CHANNELS );
	for ( int c = 0; c < numW; c++ )
	{
		assert( wToOut[c] >= 0 && wToOut[c] < numOut );
		for ( int j = 0; j < c; j++ )
		{
			assert( wToOut[j] != wToOut[c] );
		}
		float* dst = p[wToOut[c]];
		const float* src = w[c];
		for ( int n = 0; n < SND_MASTER_BLOCK; n++ )
		{
			dst[n] += src[n];
		}
	}
	userGain = SndMaster_UserGain( userGain );
	const bool unitUser = state.userGain == 1.0f && userGain == 1.0f;
	float gains[SND_MASTER_BLOCK];
	float peaks[SND_MASTER_BLOCK] = {};
	if ( !unitUser )
	{
		const float step = ( userGain - state.userGain ) / SND_MASTER_BLOCK;
		for ( int n = 0; n < SND_MASTER_BLOCK - 1; n++ )
		{
			gains[n] = state.userGain + step * (float)( n + 1 );
		}
		gains[SND_MASTER_BLOCK - 1] = userGain;
	}
	state.userGain = userGain;
	for ( int c = 0; c < numOut; c++ )
	{
		float* row = p[c];
		if ( !unitUser )
		{
			for ( int n = 0; n < SND_MASTER_BLOCK; n++ )
			{
				row[n] *= gains[n];
			}
		}
		for ( int n = 0; n < SND_MASTER_BLOCK; n++ )
		{
			const float magnitude = fabsf( row[n] );
			peaks[n] = magnitude > peaks[n] ? magnitude : peaks[n];
		}
		int nans = 0;
		for ( int n = 0; n < SND_MASTER_BLOCK; n++ )
		{
			nans += row[n] != row[n];
		}
		// All previous rows are clean: an infinite shared peak belongs to this row.
		if ( nans != 0 || !isfinite( SndMaster_Peak( peaks ) ) )
		{
			for ( int n = 0; n < SND_MASTER_BLOCK; n++ )
			{
				row[n] = fabsf( row[n] ) <= FLT_MAX ? row[n] : 0.0f;
			}
			// An infinity may have hidden a finite peak in a previous row.
			memset( peaks, 0, sizeof( peaks ) );
			for ( int j = 0; j <= c; j++ )
			{
				for ( int n = 0; n < SND_MASTER_BLOCK; n++ )
				{
					const float magnitude = fabsf( p[j][n] );
					peaks[n] = magnitude > peaks[n] ? magnitude : peaks[n];
				}
			}
		}
	}
	const float inPeak = SndMaster_Peak( peaks );
	const float ceiling = coefs.limitCeiling;
	if ( state.limitGain == 1.0f && inPeak <= ceiling )
	{
		for ( int c = 0; c < numOut; c++ )
		{
			for ( int n = 0; n < SND_MASTER_BLOCK; n++ )
			{
				out[n * numOut + c] = p[c][n];
			}
		}
		if ( info != NULL )
		{
			info->limitInPeak = inPeak;
			info->limitMinGain = 1.0f;
			info->outPeak = inPeak;
		}
		return;
	}
	float gain = state.limitGain, minGain = 1.0f;
	// Carry the distance to unity so small release steps cannot round away.
	float distance = 1.0f - gain;
	int hold = state.limitHold;
	for ( int n = 0; n < SND_MASTER_BLOCK; n++ )
	{
		const float over = peaks[n] > ceiling ? peaks[n] : ceiling;
		const float target = ceiling / over;
		const float releasedDistance = hold > 0 ? distance : distance * coefs.limitReleaseCoef;
		const float released = hold > 0 ? gain : 1.0f - releasedDistance;
		hold = hold > 0 ? hold - 1 : 0;
		const bool fall = target < released;
		gain = fall ? target : released;
		distance = fall ? 1.0f - target : releasedDistance;
		hold = fall ? coefs.limitHoldFrames : hold;
		gains[n] = gain;
		minGain = gain < minGain ? gain : minGain;
	}
	state.limitGain = gain > 0.9999f ? 1.0f : gain;
	state.limitHold = gain > 0.9999f ? 0 : hold;
	for ( int c = 0; c < numOut; c++ )
	{
		for ( int n = 0; n < SND_MASTER_BLOCK; n++ )
		{
			float value = p[c][n] * gains[n];
			value = value > ceiling ? ceiling : value;
			value = value < -ceiling ? -ceiling : value;
			out[n * numOut + c] = value;
		}
	}
	if ( info != NULL )
	{
		for ( int n = 0; n < SND_MASTER_BLOCK; n++ )
		{
			const float peak = peaks[n] * gains[n];
			peaks[n] = peak > ceiling ? ceiling : peak;
		}
		info->limitInPeak = inPeak;
		info->limitMinGain = minGain;
		info->outPeak = SndMaster_Peak( peaks );
	}
}
