//puncho - SB2-E (WP-SB2 R-SB2-15 to R-SB2-23, R-SB2-31, R-SB2-32): the bake's estimator, area defaults, zones,
// door losses and the acousticEstimateSelfTest command. Pure arithmetic: no Steam Audio header, no file, no cvar.
#include "precompiled.h"
#pragma hdrstop

#include <float.h>

#include "acoustic_estimate.h"

// the engine with the acoustic bake. rbdmap (DMAP) compiles this file too and gets nothing from it; the engine
// built without USE_STEAMAUDIO_BAKE keeps only the self-test command's stub (C-SB49).
#if defined( USE_STEAMAUDIO_BAKE ) && !defined( DMAP )
	#define ACOUSTIC_ESTIMATE	1
#else
	#define ACOUSTIC_ESTIMATE	0
#endif

#if ACOUSTIC_ESTIMATE

/*
===============================================================================

	The estimator (R-SB2-15 to R-SB2-19, R-SB2-31, R-SB2-32). All arithmetic in double, loops in ascending
	bin unless stated. Band 1 (800-8,000 Hz) is the mid band wherever one band is named.

===============================================================================
*/

static const double	EST_FOUR_PI				= 4.0 * 3.14159265358979323846;
static const double	EST_BIN_SECONDS			= 0.01;			// bin i = delays [i * 10 ms, ( i + 1 ) * 10 ms)
static const double	EST_SPEED_OF_SOUND		= 340.0;		// m/s
static const double	EST_AIR[SND_NUM_BANDS]	= { 0.0002, 0.0017, 0.0182 };	// per metre (air_absorption.cpp:41)
static const uint64	EST_BIN_PATH_MM			= 3400;			// path per bin: 340 m/s * 10 ms
static const double	EST_SAMPLE_MIN			= 1e-30;		// below it, a NaN, a denormal or a negative reads 0
static const double	EST_SAMPLE_MAX			= ( double )FLT_MAX;	// above it, an infinity reads 0 (E3-6, E3R-4)
static const double	EST_EMPTY_ENERGY		= 1e-10 / EST_FOUR_PI;	// L() of it and below is -100 dB
static const double	EST_PEAK_FRACTION		= 1e-4;			// -40 dB under the peak: preDelay, lateDelay, sparse test
static const double	EST_WINDOW_TOP			= -0.5;			// fit window, -5 dB ...
static const double	EST_WINDOW_BOTTOM		= -2.5;			// ... to -25 dB (reverb_estimator.cpp:29-30)
static const double	EST_WINDOW_MIN_SPAN		= 1.0;			// 10 dB
static const double	EST_Y_NONE				= -1000.0;		// y of a zero sum: far under the window, never -inf
static const int	EST_REFITS				= 3;			// always three, no convergence test (section 5.8)
static const int	EST_TAIL_BINS			= 5;			// the tail's block of bins (DR-75)
static const int	EST_EARLY_BINS			= 8;			// bins 0-7: the first 80 ms after the direct sound
static const int	EST_MIN_SEGMENTS		= 64;			// fewer: roomSize from preDelay, bit 2
static const double	EST_DECAY_MIN			= 0.1;			// s
static const double	EST_DECAY_MAX			= 20.0;			// s
static const double	EST_DB_MIN				= -100.0;
static const double	EST_DB_MAX				= 40.0;
static const double	EST_PREDELAY_MAX		= 0.3;			// s
static const double	EST_LATEDELAY_MAX		= 0.4;			// s
static const double	EST_LATEDELAY_NONE		= 0.08;			// s
static const double	EST_ROOM_MIN			= 1.0;			// m
static const double	EST_ROOM_MAX			= 100.0;		// m
static const double	EST_STEP_DB				= 0.01;			// R-SB2-18
static const double	EST_STEP_SECONDS		= 0.0001;
static const double	EST_STEP_METRES			= 0.001;

/*
===============
Estimate_Clamp

a NaN gives lo: no record is ever NaN (DR-26)
===============
*/
static double Estimate_Clamp( double x, double lo, double hi )
{
	if( !( x >= lo ) )
	{
		return lo;
	}
	if( x > hi )
	{
		return hi;
	}
	return x;
}

/*
===============
Estimate_Quantise

R-SB2-18: after clamping, floor( x / step + 0.5 ) * step in double, stored as float
===============
*/
static float Estimate_Quantise( double x, double step )
{
	return ( float )( floor( x / step + 0.5 ) * step );
}

/*
===============
Estimate_Sample

R-SB2-15: written so that a NaN fails the test; an infinity reads 0 too
===============
*/
static double Estimate_Sample( double v )
{
	return ( v >= EST_SAMPLE_MIN && v <= EST_SAMPLE_MAX ) ? v : 0.0;
}

/*
===============
Estimate_Level

R-SB2-15: 0 dB = the direct sound of the same source at 1 m
===============
*/
static double Estimate_Level( double energy )
{
	if( !( energy > EST_EMPTY_ENERGY ) )
	{
		return EST_DB_MIN;
	}
	return Estimate_Clamp( 10.0 * log10( EST_FOUR_PI * energy ), EST_DB_MIN, EST_DB_MAX );
}

/*
===============
Estimate_EmptyRecord

R-SB2-17: a valid dry record; roomSize by its rule with preDelay 0
===============
*/
static uint32 Estimate_EmptyRecord( uint64 segments, uint64 segmentMm, sndReverb_t& reverb )
{
	uint32 flags = ACOUSTIC_PROBE_EMPTY;
	double roomSize = 0.0;
	if( segments >= ( uint64 )EST_MIN_SEGMENTS )
	{
		roomSize = ( double )segmentMm / ( double )segments / 1000.0;
	}
	else
	{
		flags |= ACOUSTIC_PROBE_ROOMSIZE_FALLBACK;
	}

	memset( &reverb, 0, sizeof( reverb ) );
	for( int b = 0; b < SND_NUM_BANDS; b++ )
	{
		reverb.decayTime[b] = Estimate_Quantise( EST_DECAY_MIN, EST_STEP_SECONDS );
	}
	reverb.earlyDb = Estimate_Quantise( EST_DB_MIN, EST_STEP_DB );
	reverb.lateDb = Estimate_Quantise( EST_DB_MIN, EST_STEP_DB );
	reverb.preDelay = 0.0f;
	reverb.lateDelay = 0.0f;
	reverb.roomSize = Estimate_Quantise( Estimate_Clamp( roomSize, EST_ROOM_MIN, EST_ROOM_MAX ), EST_STEP_METRES );
	return flags;
}

struct estimateFit_t
{
	double		decayTime;		// raw, before any clamp
	bool		twoPoint;		// fell back to -6 x_L / y_L
	int			first;			// first window point, -1 none
	int			last;			// last window point, -1 none
	double		yEnd;			// y at the last usable bin, EST_Y_NONE when there is none
};

/*
===============
Estimate_Fit

one pass of R-SB2-16's decayTime row: the compensated Schroeder sum over bins [0, iEnd), the least-squares line
over the window points, or the two-point value. sum and y are scratch of at least iEnd.
===============
*/
static void Estimate_Fit( const double* energy, int iEnd, double tail, double* sum, double* y, estimateFit_t& fit )
{
	// the backward integral, from the last usable bin down, starting from the tail
	double running = tail;
	for( int i = iEnd - 1; i >= 0; i-- )
	{
		running += energy[i];
		sum[i] = running;
	}
	const double d0 = ( iEnd > 0 ) ? sum[0] : 0.0;

	fit.first = -1;
	fit.last = -1;
	int n = 0;
	double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
	double yMax = EST_Y_NONE, yMin = 0.0;
	for( int i = 0; i < iEnd; i++ )
	{
		y[i] = ( d0 > 0.0 && sum[i] > 0.0 ) ? log10( sum[i] / d0 ) : EST_Y_NONE;
		if( y[i] >= EST_WINDOW_BOTTOM && y[i] <= EST_WINDOW_TOP )
		{
			const double x = i * EST_BIN_SECONDS;
			if( fit.first < 0 )
			{
				fit.first = i;
				yMax = y[i];
				yMin = y[i];
			}
			fit.last = i;
			yMax = Max( yMax, y[i] );
			yMin = Min( yMin, y[i] );
			sx += x;
			sy += y[i];
			sxx += x * x;
			sxy += x * y[i];
			n++;
		}
	}
	fit.yEnd = ( iEnd > 0 ) ? y[iEnd - 1] : EST_Y_NONE;

	double slope = 0.0;
	if( n >= 3 )
	{
		const double denom = n * sxx - sx * sx;
		if( denom > 0.0 )
		{
			slope = ( n * sxy - sx * sy ) / denom;
		}
	}

	if( n >= 3 && yMax - yMin >= EST_WINDOW_MIN_SPAN && slope < 0.0 )
	{
		fit.decayTime = -6.0 / slope;
		fit.twoPoint = false;
		return;
	}

	// the two-point value from the last usable bin L; no bin or no fall at all reads as dry (clamps to 0.1 s)
	fit.twoPoint = true;
	fit.decayTime = 0.0;
	const int last = iEnd - 1;
	if( last >= 1 && y[last] < 0.0 )
	{
		fit.decayTime = -6.0 * ( last * EST_BIN_SECONDS ) / y[last];
	}
}

/*
===============
Estimate_Record

the estimator on one histogram of raw channel-0 values, band-major (SND_NUM_BANDS * numBins doubles): a single
probe's, or the mean of several. Fills reverb, clamped then quantised, and returns flag bits 1 to 5.
===============
*/
static uint32 Estimate_Record( const double* values, int numBins, uint64 segments, uint64 segmentMm, uint64 ceilingPathMm, sndReverb_t& reverb )
{
	// no bins: the empty record (E3-5), so a negative count never reaches a size
	if( numBins <= 0 )
	{
		return Estimate_EmptyRecord( segments, segmentMm, reverb );
	}

	// R-SB2-15: omni energy with air, by distance travelled
	idTempArray<double> energy( SND_NUM_BANDS * numBins );
	const double rootFourPi = sqrt( EST_FOUR_PI );
	double midTotal = 0.0;
	for( int b = 0; b < SND_NUM_BANDS; b++ )
	{
		for( int i = 0; i < numBins; i++ )
		{
			const double air = exp( -EST_AIR[b] * EST_SPEED_OF_SOUND * ( i + 0.5 ) * EST_BIN_SECONDS );
			energy[b * numBins + i] = rootFourPi * Estimate_Sample( values[b * numBins + i] ) * air;
		}
	}
	const double* mid = energy.Ptr() + numBins;
	for( int i = 0; i < numBins; i++ )
	{
		midTotal += mid[i];
	}

	// R-SB2-17
	if( !( midTotal > EST_EMPTY_ENERGY ) )
	{
		return Estimate_EmptyRecord( segments, segmentMm, reverb );
	}

	// R-SB2-31: bins at and after iEnd may lack energy from rays the ceiling stopped. floor( t_cut / 10 ms ) in
	// integers: a bin is 340 m/s * 10 ms = 3,400 mm of path (double divisions lose a bin at some multiples, E3-8)
	int iEnd = numBins;
	if( ceilingPathMm != ACOUSTIC_CEILING_NONE )
	{
		const uint64 cutBins = ceilingPathMm / EST_BIN_PATH_MM;
		if( cutBins < ( uint64 )numBins )
		{
			iEnd = ( int )cutBins;
		}
	}

	uint32 flags = 0;
	double decayTime[SND_NUM_BANDS];
	double tail[SND_NUM_BANDS];
	idTempArray<double> sum( Max( numBins, 1 ) );
	idTempArray<double> y( Max( numBins, 1 ) );

	for( int b = 0; b < SND_NUM_BANDS; b++ )
	{
		const double* e = energy.Ptr() + b * numBins;
		tail[b] = 0.0;

		// (1) first pass, no tail
		estimateFit_t fit;
		Estimate_Fit( e, iEnd, 0.0, sum.Ptr(), y.Ptr(), fit );
		// no window point on the first pass: no refit adds a tail (E3-9 b, ruled)
		const bool tailAllowed = ( fit.first >= 0 );

		// (2) R-SB2-32: the sparse test on the first pass's window; an empty window is not tested
		if( fit.first >= 0 )
		{
			double peak = 0.0;
			for( int k = 0; k < numBins; k++ )
			{
				peak = Max( peak, e[k] );
			}
			const double floorEnergy = EST_PEAK_FRACTION * peak;
			int held = 0;
			for( int i = fit.first; i <= fit.last; i++ )
			{
				if( e[i] >= floorEnergy )
				{
					held++;
				}
			}
			if( held * 2 < fit.last - fit.first + 1 )
			{
				decayTime[b] = EST_DECAY_MIN;
				flags |= ACOUSTIC_PROBE_SPARSE;
				continue;
			}
		}

		// (3) always three refits with the tail: the last five bins' mean, continued in blocks of five at the
		// previous pass's rate. A previous raw decay under 0.1 s is R-SB2-16's T = 0, r = 0: no tail (E3-1); the
		// working decay is capped at 20 s so the geometric sum stays finite.
		const int tailFirst = Max( iEnd - EST_TAIL_BINS, 0 );
		double tailMean = 0.0;
		for( int i = tailFirst; i < iEnd; i++ )
		{
			tailMean += e[i];
		}
		if( iEnd > tailFirst )
		{
			tailMean /= ( double )( iEnd - tailFirst );
		}
		for( int pass = 0; pass < EST_REFITS; pass++ )
		{
			tail[b] = 0.0;
			if( tailAllowed && fit.decayTime >= EST_DECAY_MIN )
			{
				const double working = Min( fit.decayTime, EST_DECAY_MAX );
				const double r = pow( 10.0, -0.06 / working );
				const double r5 = r * r * r * r * r;
				tail[b] = EST_TAIL_BINS * tailMean * r5 / ( 1.0 - r5 );
			}
			Estimate_Fit( e, iEnd, tail[b], sum.Ptr(), y.Ptr(), fit );
		}

		decayTime[b] = fit.decayTime;
		if( fit.twoPoint )
		{
			flags |= ACOUSTIC_PROBE_TWO_POINT;
		}
		// bit 3: the ceiling made iEnd < B and the compensated sum had not reached the window's lower edge
		if( iEnd < numBins && ( iEnd == 0 || fit.yEnd > EST_WINDOW_BOTTOM ) )
		{
			flags |= ACOUSTIC_PROBE_CEILING_CUT;
		}
	}

	// levels (mid band)
	double early = 0.0;
	for( int i = 0; i < Min( EST_EARLY_BINS, numBins ); i++ )
	{
		early += mid[i];
	}
	double late = 0.0;
	for( int i = EST_EARLY_BINS; i < iEnd; i++ )
	{
		late += mid[i];
	}
	late += tail[1];

	// delays: -40 dB under the mid band's peak, so one stray ray cannot set them
	double midPeak = 0.0;
	for( int i = 0; i < numBins; i++ )
	{
		midPeak = Max( midPeak, mid[i] );
	}
	const double midFloor = EST_PEAK_FRACTION * midPeak;
	int first = 0;
	while( first < numBins && !( mid[first] >= midFloor ) )
	{
		first++;
	}
	const double preDelay = Estimate_Clamp( first * EST_BIN_SECONDS, 0.0, EST_PREDELAY_MAX );
	int lateFirst = Max( EST_EARLY_BINS, first );
	while( lateFirst < numBins && !( mid[lateFirst] >= midFloor ) )
	{
		lateFirst++;
	}
	double lateDelay = ( lateFirst < numBins ) ? lateFirst * EST_BIN_SECONDS : Max( EST_LATEDELAY_NONE, preDelay );
	lateDelay = Estimate_Clamp( lateDelay, preDelay, EST_LATEDELAY_MAX );

	// roomSize: the mean over segments, not over rays (DR-60)
	double roomSize;
	if( segments >= ( uint64 )EST_MIN_SEGMENTS )
	{
		roomSize = ( double )segmentMm / ( double )segments / 1000.0;
	}
	else
	{
		roomSize = EST_SPEED_OF_SOUND * preDelay;
		flags |= ACOUSTIC_PROBE_ROOMSIZE_FALLBACK;
	}

	// R-SB2-18: clamp, then quantise
	memset( &reverb, 0, sizeof( reverb ) );
	for( int b = 0; b < SND_NUM_BANDS; b++ )
	{
		reverb.decayTime[b] = Estimate_Quantise( Estimate_Clamp( decayTime[b], EST_DECAY_MIN, EST_DECAY_MAX ), EST_STEP_SECONDS );
	}
	reverb.earlyDb = Estimate_Quantise( Estimate_Level( early ), EST_STEP_DB );
	reverb.lateDb = Estimate_Quantise( Estimate_Level( late ), EST_STEP_DB );
	reverb.preDelay = Estimate_Quantise( preDelay, EST_STEP_SECONDS );
	reverb.lateDelay = Estimate_Quantise( lateDelay, EST_STEP_SECONDS );
	reverb.roomSize = Estimate_Quantise( Estimate_Clamp( roomSize, EST_ROOM_MIN, EST_ROOM_MAX ), EST_STEP_METRES );
	return flags;
}

/*
===============
AcousticEstimateProbe
===============
*/
uint32 AcousticEstimateProbe( const float* histogram, int numBins, uint32 segments, uint64 segmentMm, uint64 ceilingPathMm, sndReverb_t& reverb )
{
	if( numBins <= 0 )
	{
		return Estimate_EmptyRecord( segments, segmentMm, reverb );
	}
	idTempArray<double> values( SND_NUM_BANDS * numBins );
	for( int i = 0; i < SND_NUM_BANDS * numBins; i++ )
	{
		values[i] = Estimate_Sample( histogram[i] );
	}
	return Estimate_Record( values.Ptr(), numBins, segments, segmentMm, ceilingPathMm, reverb );
}

/*
===============================================================================

	Running sums (R-SB2-20, R-SB2-22 as amended, E3-12): one per area, double, its probes added in probe
	order. An area's mean is its sum divided by its probe count; a zone's sum is the sum of its areas' sums in
	ascending area order. The estimator takes the double mean with no float round trip.

===============================================================================
*/

/*
===============
AcousticAreaSums_Begin
===============
*/
void AcousticAreaSums_Begin( acousticAreaSums_t& sums, int numAreas, int numBins )
{
	memset( &sums, 0, sizeof( sums ) );
	sums.numAreas = Max( numAreas, 0 );
	sums.numBins = Max( numBins, 0 );
	const size_t areas = ( size_t )Max( sums.numAreas, 1 );
	sums.values = ( double* )Mem_ClearedAlloc( areas * SND_NUM_BANDS * Max( sums.numBins, 1 ) * sizeof( double ), TAG_TOOLS );
	sums.segments = ( uint64* )Mem_ClearedAlloc( areas * sizeof( uint64 ), TAG_TOOLS );
	sums.segmentMm = ( uint64* )Mem_ClearedAlloc( areas * sizeof( uint64 ), TAG_TOOLS );
	sums.ceilingPathMm = ( uint64* )Mem_Alloc( areas * sizeof( uint64 ), TAG_TOOLS );
	sums.probes = ( int* )Mem_ClearedAlloc( areas * sizeof( int ), TAG_TOOLS );
	for( int a = 0; a < sums.numAreas; a++ )
	{
		sums.ceilingPathMm[a] = ACOUSTIC_CEILING_NONE;
	}
}

/*
===============
AcousticAreaSums_AddProbe
===============
*/
void AcousticAreaSums_AddProbe( acousticAreaSums_t& sums, int area, const float* histogram, uint32 segments, uint64 segmentMm, uint64 ceilingPathMm )
{
	assert( area >= 0 && area < sums.numAreas );
	const int values = SND_NUM_BANDS * sums.numBins;
	double* sum = sums.values + ( size_t )area * values;
	for( int i = 0; i < values; i++ )
	{
		sum[i] += Estimate_Sample( histogram[i] );
	}
	sums.segments[area] += segments;
	sums.segmentMm[area] += segmentMm;
	sums.ceilingPathMm[area] = Min( sums.ceilingPathMm[area], ceilingPathMm );
	sums.probes[area]++;
}

/*
===============
AcousticAreaSums_Free
===============
*/
void AcousticAreaSums_Free( acousticAreaSums_t& sums )
{
	Mem_Free( sums.values );
	Mem_Free( sums.segments );
	Mem_Free( sums.segmentMm );
	Mem_Free( sums.ceilingPathMm );
	Mem_Free( sums.probes );
	memset( &sums, 0, sizeof( sums ) );
}

/*
===============
AcousticAreaDefaults
===============
*/
void AcousticAreaDefaults( const acousticAreaSums_t& sums, sndReverb_t* areaReverb )
{
	const int values = SND_NUM_BANDS * sums.numBins;
	idTempArray<double> mean( Max( values, 1 ) );
	for( int a = 0; a < sums.numAreas; a++ )
	{
		const int count = sums.probes[a];
		if( count <= 0 )
		{
			continue;
		}
		// the sums stay whole: the zones add them up later
		const double* sum = sums.values + ( size_t )a * values;
		for( int i = 0; i < values; i++ )
		{
			mean[i] = sum[i] / ( double )count;
		}
		Estimate_Record( mean.Ptr(), sums.numBins, sums.segments[a], sums.segmentMm[a], sums.ceilingPathMm[a], areaReverb[a] );
	}
}

/*
===============================================================================

	Zones (R-SB2-21). A zone is a union of areas whose root is its lowest area index; every merge keeps the
	lower root, so that index is the zone's id until the dense renumbering. Arrays only, no hash table.

===============================================================================
*/

// the merge limits, code constants under revision B (section 10). Every field is compared as an integer count
// of its quantisation step (0.1 ms, 0.01 dB, 1 mm), the ratios by cross-multiplying, so the stored values decide
// exactly and the limit itself merges (E3-7)
static const int64	ZONE_DECAY_RATIO_NUM	= 12;		// max / min of decayTime[1] <= 1.2: 10 max <= 12 min
static const int64	ZONE_DECAY_RATIO_DEN	= 10;
static const int	ZONE_MAX_PREDELAY_STEPS	= 100;		// 10 ms in 0.1 ms
static const int64	ZONE_ROOM_RATIO_NUM		= 5;		// max / min of roomSize <= 1.25: 4 max <= 5 min
static const int64	ZONE_ROOM_RATIO_DEN		= 4;
static const int	ZONE_MAX_DB_STEPS		= 300;		// 3 dB in 0.01 dB, earlyDb and lateDb each
static const int	ZONE_PASS_MAX_PROBES	= 2;		// pass-through: a zone with at most this many probes ...
static const int	ZONE_PASS_MAX_NEIGHBOURS = 2;		// ... and 1 or 2 joinable neighbour zones

struct zonePair_t
{
	int			a;			// a < b
	int			b;
	int64		decayHigh;	// the larger decayTime[1] of the two, in 0.1 ms steps
	int64		decayLow;	// the smaller; the score is decayHigh / decayLow, the order of |ln( a / b )|
};

// the union's spread, kept at the root, in quantisation steps
struct zoneSpread_t
{
	int			decayMin, decayMax;
	int			roomMin, roomMax;
	int			preMin, preMax;
	int			earlyMin, earlyMax;
	int			lateMin, lateMax;
	int			probes;
	int			lastMember;		// the end of the root's member chain
};

class idSortZonePairByArea : public idSort_Quick< zonePair_t, idSortZonePairByArea >
{
public:
	int Compare( const zonePair_t& x, const zonePair_t& y ) const
	{
		if( x.a != y.a )
		{
			return ( x.a < y.a ) ? -1 : 1;
		}
		if( x.b != y.b )
		{
			return ( x.b < y.b ) ? -1 : 1;
		}
		return 0;
	}
};

// by score, compared by cross-multiplying the integer steps in int64 (E3R-2): equal ratios tie exactly whatever
// the values, and ties fall to ( a, b ). No float is compared anywhere in the zone code.
class idSortZonePairByScore : public idSort_Quick< zonePair_t, idSortZonePairByScore >
{
public:
	int Compare( const zonePair_t& x, const zonePair_t& y ) const
	{
		const int64 left = x.decayHigh * y.decayLow;
		const int64 right = y.decayHigh * x.decayLow;
		if( left != right )
		{
			return ( left < right ) ? -1 : 1;
		}
		if( x.a != y.a )
		{
			return ( x.a < y.a ) ? -1 : 1;
		}
		if( x.b != y.b )
		{
			return ( x.b < y.b ) ? -1 : 1;
		}
		return 0;
	}
};

static int Zone_Steps( float value, double step )
{
	return ( int )floor( value / step + 0.5 );
}

static int Zone_Find( int* parent, int area )
{
	while( parent[area] != area )
	{
		parent[area] = parent[parent[area]];
		area = parent[area];
	}
	return area;
}

// the lower root survives; its spread and member chain take the other's
static void Zone_Union( int* parent, int* nextMember, zoneSpread_t* spread, int rootA, int rootB )
{
	const int keep = Min( rootA, rootB );
	const int gone = Max( rootA, rootB );
	zoneSpread_t& k = spread[keep];
	const zoneSpread_t& g = spread[gone];
	k.decayMin = Min( k.decayMin, g.decayMin );
	k.decayMax = Max( k.decayMax, g.decayMax );
	k.roomMin = Min( k.roomMin, g.roomMin );
	k.roomMax = Max( k.roomMax, g.roomMax );
	k.preMin = Min( k.preMin, g.preMin );
	k.preMax = Max( k.preMax, g.preMax );
	k.earlyMin = Min( k.earlyMin, g.earlyMin );
	k.earlyMax = Max( k.earlyMax, g.earlyMax );
	k.lateMin = Min( k.lateMin, g.lateMin );
	k.lateMax = Max( k.lateMax, g.lateMax );
	k.probes += g.probes;
	nextMember[k.lastMember] = gone;
	k.lastMember = g.lastMember;
	parent[gone] = keep;
}

/*
===============
AcousticBuildZones
===============
*/
int AcousticBuildZones( const acousticAreaSums_t& sums, const sndReverb_t* areaReverb, const sndPortal_t* portals, int numPortals, int* areaZone )
{
	// the probe counts are the sums' own, as in AcousticAreaDefaults and AcousticZoneRecords (E3R-1)
	const int numAreas = sums.numAreas;
	if( numAreas <= 0 )
	{
		return 0;
	}

	// 1. every area is its own zone
	idTempArray<int> parent( numAreas );
	idTempArray<int> nextMember( numAreas );
	idTempArray<zoneSpread_t> spread( numAreas );
	for( int a = 0; a < numAreas; a++ )
	{
		parent[a] = a;
		nextMember[a] = -1;
		zoneSpread_t& s = spread[a];
		memset( &s, 0, sizeof( s ) );
		s.probes = Max( sums.probes[a], 0 );
		s.lastMember = a;
		if( s.probes > 0 )
		{
			const sndReverb_t& r = areaReverb[a];
			s.decayMin = s.decayMax = Zone_Steps( r.decayTime[1], EST_STEP_SECONDS );
			s.roomMin = s.roomMax = Zone_Steps( r.roomSize, EST_STEP_METRES );
			s.preMin = s.preMax = Zone_Steps( r.preDelay, EST_STEP_SECONDS );
			s.earlyMin = s.earlyMax = Zone_Steps( r.earlyDb, EST_STEP_DB );
			s.lateMin = s.lateMax = Zone_Steps( r.lateDb, EST_STEP_DB );
		}
	}

	// the joinable pairs, once each: a portal with door -1 joins; a door portal never does
	idList<zonePair_t> pairs;
	pairs.Resize( Max( numPortals, 1 ) );
	for( int p = 0; p < numPortals; p++ )
	{
		const sndPortal_t& portal = portals[p];
		if( portal.door >= 0 || portal.area0 == portal.area1 )
		{
			continue;
		}
		if( portal.area0 < 0 || portal.area0 >= numAreas || portal.area1 < 0 || portal.area1 >= numAreas )
		{
			continue;
		}
		zonePair_t pair;
		pair.a = Min( portal.area0, portal.area1 );
		pair.b = Max( portal.area0, portal.area1 );
		pair.decayHigh = 0;
		pair.decayLow = 0;
		pairs.Append( pair );
	}
	pairs.SortWithTemplate( idSortZonePairByArea() );
	int unique = 0;
	for( int i = 0; i < pairs.Num(); i++ )
	{
		if( unique == 0 || pairs[i].a != pairs[unique - 1].a || pairs[i].b != pairs[unique - 1].b )
		{
			pairs[unique++] = pairs[i];
		}
	}
	pairs.SetNum( unique );

	// 2. similar merge: pairs with probes on both sides, by ( score, a, b ); the union's spread is tested, not
	// the pair's, so chains cannot drift
	idList<zonePair_t> similar;
	similar.Resize( Max( pairs.Num(), 1 ) );
	for( int i = 0; i < pairs.Num(); i++ )
	{
		zonePair_t pair = pairs[i];
		if( spread[pair.a].probes <= 0 || spread[pair.b].probes <= 0 )
		{
			continue;
		}
		// each area is still its own zone here, so its spread holds its own steps
		const int64 decayA = spread[pair.a].decayMin;
		const int64 decayB = spread[pair.b].decayMin;
		pair.decayHigh = Max( decayA, decayB );
		pair.decayLow = Min( decayA, decayB );
		similar.Append( pair );
	}
	similar.SortWithTemplate( idSortZonePairByScore() );
	for( int i = 0; i < similar.Num(); i++ )
	{
		const int rootA = Zone_Find( parent.Ptr(), similar[i].a );
		const int rootB = Zone_Find( parent.Ptr(), similar[i].b );
		if( rootA == rootB )
		{
			continue;
		}
		const zoneSpread_t& x = spread[rootA];
		const zoneSpread_t& y = spread[rootB];
		const int64 decayMin = Min( x.decayMin, y.decayMin );
		const int64 decayMax = Max( x.decayMax, y.decayMax );
		const int64 roomMin = Min( x.roomMin, y.roomMin );
		const int64 roomMax = Max( x.roomMax, y.roomMax );
		if( ZONE_DECAY_RATIO_DEN * decayMax > ZONE_DECAY_RATIO_NUM * decayMin || ZONE_ROOM_RATIO_DEN * roomMax > ZONE_ROOM_RATIO_NUM * roomMin )
		{
			continue;
		}
		if( Max( x.preMax, y.preMax ) - Min( x.preMin, y.preMin ) > ZONE_MAX_PREDELAY_STEPS
				|| Max( x.earlyMax, y.earlyMax ) - Min( x.earlyMin, y.earlyMin ) > ZONE_MAX_DB_STEPS
				|| Max( x.lateMax, y.lateMax ) - Min( x.lateMin, y.lateMin ) > ZONE_MAX_DB_STEPS )
		{
			continue;
		}
		Zone_Union( parent.Ptr(), nextMember.Ptr(), spread.Ptr(), rootA, rootB );
	}

	// the joinable neighbours of every area, both directions, from the pairs in ( a, b ) order
	idTempArray<int> adjacencyFirst( numAreas + 1 );
	idTempArray<int> adjacency( Max( pairs.Num() * 2, 1 ) );
	for( int a = 0; a <= numAreas; a++ )
	{
		adjacencyFirst[a] = 0;
	}
	for( int i = 0; i < pairs.Num(); i++ )
	{
		adjacencyFirst[pairs[i].a + 1]++;
		adjacencyFirst[pairs[i].b + 1]++;
	}
	for( int a = 0; a < numAreas; a++ )
	{
		adjacencyFirst[a + 1] += adjacencyFirst[a];
	}
	{
		idTempArray<int> fill( numAreas );
		for( int a = 0; a < numAreas; a++ )
		{
			fill[a] = adjacencyFirst[a];
		}
		for( int i = 0; i < pairs.Num(); i++ )
		{
			adjacency[fill[pairs[i].a]++] = pairs[i].b;
			adjacency[fill[pairs[i].b]++] = pairs[i].a;
		}
	}

	// 3. pass-through merge, one pass in ascending order of each zone's lowest area index (its root). A zone
	// merged away is no longer a root and is skipped; the zone it joined has a lower root, already passed, or
	// keeps this one's root, so no zone that grew is tested again.
	for( int zone = 0; zone < numAreas; zone++ )
	{
		if( parent[zone] != zone || spread[zone].probes > ZONE_PASS_MAX_PROBES )
		{
			continue;
		}
		int neighbours[ZONE_PASS_MAX_NEIGHBOURS + 1];
		int numNeighbours = 0;
		for( int member = zone; member != -1 && numNeighbours <= ZONE_PASS_MAX_NEIGHBOURS; member = nextMember[member] )
		{
			for( int j = adjacencyFirst[member]; j < adjacencyFirst[member + 1] && numNeighbours <= ZONE_PASS_MAX_NEIGHBOURS; j++ )
			{
				const int other = Zone_Find( parent.Ptr(), adjacency[j] );
				if( other == zone )
				{
					continue;
				}
				bool seen = false;
				for( int k = 0; k < numNeighbours; k++ )
				{
					seen |= ( neighbours[k] == other );
				}
				if( !seen )
				{
					neighbours[numNeighbours++] = other;
				}
			}
		}
		if( numNeighbours < 1 || numNeighbours > ZONE_PASS_MAX_NEIGHBOURS )
		{
			continue;
		}
		// the neighbour with more probes; tie: the lower zone id
		int best = neighbours[0];
		for( int k = 1; k < numNeighbours; k++ )
		{
			const int other = neighbours[k];
			if( spread[other].probes > spread[best].probes || ( spread[other].probes == spread[best].probes && other < best ) )
			{
				best = other;
			}
		}
		Zone_Union( parent.Ptr(), nextMember.Ptr(), spread.Ptr(), zone, best );
	}

	// 4. dense ids in ascending order of the lowest area index: a zone is first met at its root
	idTempArray<int> denseId( numAreas );
	for( int a = 0; a < numAreas; a++ )
	{
		denseId[a] = -1;
	}
	int numZones = 0;
	for( int a = 0; a < numAreas; a++ )
	{
		const int root = Zone_Find( parent.Ptr(), a );
		if( denseId[root] < 0 )
		{
			denseId[root] = numZones++;
		}
		areaZone[a] = denseId[root];
	}
	return numZones;
}

/*
===============
AcousticZoneRecords
===============
*/
void AcousticZoneRecords( const acousticAreaSums_t& sums, const int* areaZone, int numZones, sndReverb_t* zoneReverb, sndReverb_t* areaReverb, idList<int>& isolatedAreas )
{
	isolatedAreas.Clear();
	if( numZones <= 0 )
	{
		return;
	}

	// the areas of each zone, ascending (a counting sort)
	const int numAreas = sums.numAreas;
	idTempArray<int> zoneFirst( numZones + 1 );
	idTempArray<int> zoneAreas( Max( numAreas, 1 ) );
	idTempArray<int> zoneProbes( numZones );
	for( int z = 0; z <= numZones; z++ )
	{
		zoneFirst[z] = 0;
	}
	for( int a = 0; a < numAreas; a++ )
	{
		assert( areaZone[a] >= 0 && areaZone[a] < numZones );
		zoneFirst[areaZone[a] + 1]++;
	}
	for( int z = 0; z < numZones; z++ )
	{
		zoneFirst[z + 1] += zoneFirst[z];
	}
	{
		idTempArray<int> fill( numZones );
		for( int z = 0; z < numZones; z++ )
		{
			fill[z] = zoneFirst[z];
		}
		for( int a = 0; a < numAreas; a++ )
		{
			zoneAreas[fill[areaZone[a]]++] = a;
		}
	}

	// a zone's sum is the sum of its areas' sums in ascending area order, divided by its probe count
	const int values = SND_NUM_BANDS * sums.numBins;
	idTempArray<double> mean( Max( values, 1 ) );
	for( int z = 0; z < numZones; z++ )
	{
		memset( mean.Ptr(), 0, Max( values, 1 ) * sizeof( double ) );
		uint64 segments = 0;
		uint64 segmentMm = 0;
		uint64 ceilingPathMm = ACOUSTIC_CEILING_NONE;
		int count = 0;
		for( int j = zoneFirst[z]; j < zoneFirst[z + 1]; j++ )
		{
			const int a = zoneAreas[j];
			if( sums.probes[a] <= 0 )
			{
				continue;
			}
			const double* sum = sums.values + ( size_t )a * values;
			for( int i = 0; i < values; i++ )
			{
				mean[i] += sum[i];
			}
			segments += sums.segments[a];
			segmentMm += sums.segmentMm[a];
			ceilingPathMm = Min( ceilingPathMm, sums.ceilingPathMm[a] );
			count += sums.probes[a];
		}
		zoneProbes[z] = count;
		if( count == 0 )
		{
			// a zone with no probe takes the empty record
			Estimate_EmptyRecord( 0, 0, zoneReverb[z] );
			continue;
		}
		for( int i = 0; i < values; i++ )
		{
			mean[i] /= ( double )count;
		}
		Estimate_Record( mean.Ptr(), sums.numBins, segments, segmentMm, ceilingPathMm, zoneReverb[z] );
	}

	// an area with no probe takes its zone's record; listed when that zone has no probe (the empty record, E3-3)
	for( int a = 0; a < numAreas; a++ )
	{
		if( sums.probes[a] > 0 )
		{
			continue;
		}
		const int z = areaZone[a];
		areaReverb[a] = zoneReverb[z];
		if( zoneProbes[z] == 0 )
		{
			isolatedAreas.Append( a );
		}
	}
}

/*
===============
AcousticDoorLoss
===============
*/
void AcousticDoorLoss( const float transmission[SND_NUM_BANDS], float lossDb[SND_NUM_BANDS] )
{
	for( int b = 0; b < SND_NUM_BANDS; b++ )
	{
		const double t = transmission[b];
		const double loss = -10.0 * log10( ( t >= 1e-6 ) ? t : 1e-6 );
		lossDb[b] = Estimate_Quantise( Estimate_Clamp( loss, 0.0, 60.0 ), EST_STEP_DB );
	}
}

/*
===============================================================================

	acousticEstimateSelfTest (A-SB2-7, SEAMS D-13): synthetic histograms built in code. The constants of
	section 5.5 are typed again here, not shared, so a slip in the estimator's copy fails a case. Each case runs
	twice and must give the same bytes; a second run of the command must print the same lines.

===============================================================================
*/

static const int	TEST_BINS			= 100;		// 1.0 s
static const double	TEST_FOUR_PI		= 12.566370614359172;
static const double	TEST_AIR[3]			= { 0.0002, 0.0017, 0.0182 };
static const uint32	TEST_SEGMENTS		= 1000;
static const uint64	TEST_SEGMENT_MM		= 4210000;	// roomSize 4.21 m

// the raw channel-0 value that gives omni energy e with air at bin i of band b
static float Test_Value( int band, int bin, double e )
{
	const double air = exp( -TEST_AIR[band] * 340.0 * ( bin + 0.5 ) * 0.01 );
	return ( float )( e / ( sqrt( TEST_FOUR_PI ) * air ) );
}

// an exponential decay of T seconds (60 dB), energy density 1 at the direct sound, each bin its integral
static void Test_Exponential( float* histogram, double decayTime )
{
	const double k = 6.0 * log( 10.0 ) / decayTime;
	for( int b = 0; b < SND_NUM_BANDS; b++ )
	{
		for( int i = 0; i < TEST_BINS; i++ )
		{
			const double e = ( exp( -k * 0.01 * i ) - exp( -k * 0.01 * ( i + 1 ) ) ) / k;
			histogram[b * TEST_BINS + i] = Test_Value( b, i, e );
		}
	}
}

static void Test_Energies( float* histogram, const double* energy )
{
	for( int b = 0; b < SND_NUM_BANDS; b++ )
	{
		for( int i = 0; i < TEST_BINS; i++ )
		{
			histogram[b * TEST_BINS + i] = Test_Value( b, i, energy[i] );
		}
	}
}

static double Test_Level( double e )
{
	return floor( 10.0 * log10( TEST_FOUR_PI * e ) / 0.01 + 0.5 ) * 0.01;
}

static bool Test_Near( double got, double want, double fraction )
{
	return fabs( got - want ) <= fraction * want;
}

static bool Test_DecayNear( const sndReverb_t& r, double want, double fraction )
{
	return Test_Near( r.decayTime[0], want, fraction ) && Test_Near( r.decayTime[1], want, fraction ) && Test_Near( r.decayTime[2], want, fraction );
}

static bool Test_IsEmptyRecord( const sndReverb_t& r )
{
	return r.decayTime[0] == 0.1f && r.decayTime[1] == 0.1f && r.decayTime[2] == 0.1f && r.earlyDb == -100.0f && r.lateDb == -100.0f
		   && r.preDelay == 0.0f && r.lateDelay == 0.0f;
}

// runs the estimator twice on one histogram; false when the two runs differ
static bool Test_Estimate( const float* histogram, uint32 segments, uint64 ceilingPathMm, sndReverb_t& r, uint32& flags )
{
	sndReverb_t again;
	flags = AcousticEstimateProbe( histogram, TEST_BINS, segments, ( uint64 )segments * ( TEST_SEGMENT_MM / TEST_SEGMENTS ), ceilingPathMm, r );
	const uint32 flagsAgain = AcousticEstimateProbe( histogram, TEST_BINS, segments, ( uint64 )segments * ( TEST_SEGMENT_MM / TEST_SEGMENTS ), ceilingPathMm, again );
	return flags == flagsAgain && memcmp( &r, &again, sizeof( r ) ) == 0;
}

static bool Test_Report( const char* name, bool pass, const sndReverb_t& r, uint32 flags )
{
	common->Printf( "acousticEstimateSelfTest: %s %-14s decay %.4f %.4f %.4f early %.2f late %.2f pre %.4f lateDelay %.4f room %.3f flags 0x%02x bits %08x\n",
					pass ? "PASS" : "FAIL", name, r.decayTime[0], r.decayTime[1], r.decayTime[2], r.earlyDb, r.lateDb, r.preDelay, r.lateDelay,
					r.roomSize, flags, CRC32_BlockChecksum( &r, sizeof( r ) ) );
	return pass;
}

static bool Test_ReportZones( const char* name, bool pass, const int* areaZone, int numAreas, int numZones, int numIsolated )
{
	idStr zones;
	for( int a = 0; a < numAreas; a++ )
	{
		if( a > 0 )
		{
			zones += " ";
		}
		zones += va( "%d", areaZone[a] );
	}
	common->Printf( "acousticEstimateSelfTest: %s %-14s zones %d: %s isolated %d\n", pass ? "PASS" : "FAIL", name, numZones, zones.c_str(), numIsolated );
	return pass;
}

static const int TEST_MAX_AREAS = 6;

struct testZoneCase_t
{
	const char*		name;
	int				numAreas;
	int				probeCount[TEST_MAX_AREAS];
	double			decayTime[TEST_MAX_AREAS];
	int				numPortals;
	sndPortal_t		portals[TEST_MAX_AREAS + 2];
	int				wantZone[TEST_MAX_AREAS];
	int				wantZones;
	int				wantIsolated;
};

// feeds the areas' probes through the running sums as SB2-B will, then defaults, zones and records
static bool Test_Zones( const testZoneCase_t& test )
{
	float histogram[SND_NUM_BANDS * TEST_BINS];
	acousticAreaSums_t sums;
	AcousticAreaSums_Begin( sums, test.numAreas, TEST_BINS );
	for( int a = 0; a < test.numAreas; a++ )
	{
		Test_Exponential( histogram, test.decayTime[a] );
		for( int j = 0; j < test.probeCount[a]; j++ )
		{
			AcousticAreaSums_AddProbe( sums, a, histogram, TEST_SEGMENTS, TEST_SEGMENT_MM, ACOUSTIC_CEILING_NONE );
		}
	}

	sndReverb_t areaReverb[TEST_MAX_AREAS];
	sndReverb_t zoneReverb[TEST_MAX_AREAS];
	int areaZone[TEST_MAX_AREAS];
	memset( areaReverb, 0, sizeof( areaReverb ) );
	AcousticAreaDefaults( sums, areaReverb );
	const int numZones = AcousticBuildZones( sums, areaReverb, test.portals, test.numPortals, areaZone );
	idList<int> isolated;
	AcousticZoneRecords( sums, areaZone, numZones, zoneReverb, areaReverb, isolated );
	AcousticAreaSums_Free( sums );

	bool pass = ( numZones == test.wantZones && isolated.Num() == test.wantIsolated );
	for( int a = 0; a < test.numAreas; a++ )
	{
		pass &= ( areaZone[a] == test.wantZone[a] );
		// an isolated area takes the empty record
		if( isolated.FindIndex( a ) >= 0 )
		{
			pass &= ( test.probeCount[a] == 0 && Test_IsEmptyRecord( areaReverb[a] ) );
		}
		// an area with no probe fed holds its zone's record, byte for byte
		if( test.probeCount[a] == 0 )
		{
			pass &= ( memcmp( &areaReverb[a], &zoneReverb[areaZone[a]], sizeof( sndReverb_t ) ) == 0 );
		}
	}
	return Test_ReportZones( test.name, pass, areaZone, test.numAreas, numZones, isolated.Num() );
}

// zones from given defaults: three probes of zeros fed per area, so the sums hold the counts and pass-through
// takes no zone; decayTime as given in every band, the other fields equal
static bool Test_GivenZones( const char* name, int numAreas, const double* decayTime, const sndPortal_t* portals, int numPortals, const int* wantZone, int wantZones )
{
	float histogram[SND_NUM_BANDS * TEST_BINS];
	memset( histogram, 0, sizeof( histogram ) );
	sndReverb_t areaReverb[TEST_MAX_AREAS];
	int areaZone[TEST_MAX_AREAS];
	memset( areaReverb, 0, sizeof( areaReverb ) );
	acousticAreaSums_t sums;
	AcousticAreaSums_Begin( sums, numAreas, TEST_BINS );
	for( int a = 0; a < numAreas; a++ )
	{
		for( int j = 0; j < 3; j++ )
		{
			AcousticAreaSums_AddProbe( sums, a, histogram, TEST_SEGMENTS, TEST_SEGMENT_MM, ACOUSTIC_CEILING_NONE );
		}
		for( int b = 0; b < SND_NUM_BANDS; b++ )
		{
			areaReverb[a].decayTime[b] = ( float )decayTime[a];
		}
		areaReverb[a].lateDelay = 0.08f;
		areaReverb[a].roomSize = 4.21f;
	}
	const int numZones = AcousticBuildZones( sums, areaReverb, portals, numPortals, areaZone );
	AcousticAreaSums_Free( sums );
	bool pass = ( numZones == wantZones );
	for( int a = 0; a < numAreas; a++ )
	{
		pass &= ( areaZone[a] == wantZone[a] );
	}
	return Test_ReportZones( name, pass, areaZone, numAreas, numZones, 0 );
}

#endif // ACOUSTIC_ESTIMATE

// the command is the engine's (Common.cpp registers it); rbdmap has no console and never bakes
#if !defined( DMAP )

/*
===============
AcousticEstimateSelfTest_f

one PASS or FAIL line per case with the values, flags and the CRC of the record's bytes, then a count
===============
*/
void AcousticEstimateSelfTest_f( const idCmdArgs& args )
{
#if ACOUSTIC_ESTIMATE
	float histogram[SND_NUM_BANDS * TEST_BINS];
	sndReverb_t r;
	uint32 flags;
	int passed = 0, cases = 0;
	bool same;

	// exponentials truncated at 1.0 s (A-SB2-7 as amended): 0.3, 1 and 2 s within 5 % with bit 4 clear; 4 s
	// within 10 %, bit 4 not tested (it sits at the flip); 4.5 and 6 s within 10 % with bit 4 set; bits 3 and 5
	// clear in all
	static const double decays[6] = { 0.3, 1.0, 2.0, 4.0, 4.5, 6.0 };
	for( int c = 0; c < 6; c++ )
	{
		Test_Exponential( histogram, decays[c] );
		same = Test_Estimate( histogram, TEST_SEGMENTS, ACOUSTIC_CEILING_NONE, r, flags );
		bool pass = same && Test_Near( r.roomSize, 4.21, 0.0001 ) && ( flags & ( ACOUSTIC_PROBE_CEILING_CUT | ACOUSTIC_PROBE_SPARSE ) ) == 0;
		if( decays[c] < 3.0 )
		{
			pass &= Test_DecayNear( r, decays[c], 0.05 ) && ( flags & ACOUSTIC_PROBE_TWO_POINT ) == 0;
		}
		else if( decays[c] < 4.25 )
		{
			pass &= Test_DecayNear( r, decays[c], 0.10 );
		}
		else
		{
			pass &= Test_DecayNear( r, decays[c], 0.10 ) && ( flags & ACOUSTIC_PROBE_TWO_POINT ) != 0;
		}
		passed += Test_Report( va( "exp %.1f s", decays[c] ), pass, r, flags );
		cases++;
	}

	// the 1.0 s decay cut by the bounce ceiling at 0.8 s (window reached: within 10 %, no bit 3) and 0.3 s (bit 3)
	Test_Exponential( histogram, 1.0 );
	same = Test_Estimate( histogram, TEST_SEGMENTS, ( uint64 )( 0.8 * 340.0 * 1000.0 + 0.5 ), r, flags );
	passed += Test_Report( "cut 0.8 s", same && Test_DecayNear( r, 1.0, 0.10 ) && ( flags & ACOUSTIC_PROBE_CEILING_CUT ) == 0, r, flags );
	same = Test_Estimate( histogram, TEST_SEGMENTS, ( uint64 )( 0.3 * 340.0 * 1000.0 + 0.5 ), r, flags );
	passed += Test_Report( "cut 0.3 s", same && ( flags & ACOUSTIC_PROBE_CEILING_CUT ) != 0, r, flags );
	cases += 2;

	// empty: R-SB2-17's record, bit 1 only
	memset( histogram, 0, sizeof( histogram ) );
	same = Test_Estimate( histogram, TEST_SEGMENTS, ACOUSTIC_CEILING_NONE, r, flags );
	passed += Test_Report( "empty", same && flags == ACOUSTIC_PROBE_EMPTY && Test_IsEmptyRecord( r ) && Test_Near( r.roomSize, 4.21, 0.0001 ), r, flags );
	cases++;

	// a NaN, a negative value and +inf in every band, nothing else: each reads 0, so the empty record
	{
		static const uint32 nanBits = 0x7fc00000;
		static const uint32 infBits = 0x7f800000;
		float nan, inf;
		memcpy( &nan, &nanBits, sizeof( nan ) );
		memcpy( &inf, &infBits, sizeof( inf ) );
		memset( histogram, 0, sizeof( histogram ) );
		for( int b = 0; b < SND_NUM_BANDS; b++ )
		{
			histogram[b * TEST_BINS + 3] = nan;
			histogram[b * TEST_BINS + 10] = -1.0f;
			histogram[b * TEST_BINS + 20] = inf;
		}
		same = Test_Estimate( histogram, TEST_SEGMENTS, ACOUSTIC_CEILING_NONE, r, flags );
		passed += Test_Report( "nan neg inf", same && flags == ACOUSTIC_PROBE_EMPTY && Test_IsEmptyRecord( r ), r, flags );
		cases++;
	}

	// one spike of omni energy 1 / 4 pi at bin 5: earlyDb 0 dB (the 0 dB rule and the air term), preDelay
	// 50 ms, decayTime 0.1 s and bit 4; 10 segments: roomSize 340 * 0.05 = 17 m and bit 2
	{
		double energy[TEST_BINS];
		memset( energy, 0, sizeof( energy ) );
		energy[5] = 1.0 / TEST_FOUR_PI;
		Test_Energies( histogram, energy );
		same = Test_Estimate( histogram, 10, ACOUSTIC_CEILING_NONE, r, flags );
		passed += Test_Report( "spike 5", same && flags == ( ACOUSTIC_PROBE_TWO_POINT | ACOUSTIC_PROBE_ROOMSIZE_FALLBACK )
							   && r.decayTime[0] == 0.1f && r.decayTime[1] == 0.1f && r.decayTime[2] == 0.1f && r.earlyDb == 0.0f && r.lateDb == -100.0f
							   && Test_Near( r.preDelay, 0.05, 0.0001 ) && Test_Near( r.lateDelay, 0.08, 0.0001 ) && Test_Near( r.roomSize, 17.0, 0.0001 ), r, flags );
		cases++;
	}

	// the same spike at bin 97, inside the tail's last five bins: no decay found, so no tail (E3-1, E3-9 b):
	// decayTime 0.1 s, bit 4, lateDb 0 dB from the spike alone, preDelay and lateDelay at their caps
	{
		double energy[TEST_BINS];
		memset( energy, 0, sizeof( energy ) );
		energy[97] = 1.0 / TEST_FOUR_PI;
		Test_Energies( histogram, energy );
		same = Test_Estimate( histogram, TEST_SEGMENTS, ACOUSTIC_CEILING_NONE, r, flags );
		passed += Test_Report( "spike 97", same && flags == ACOUSTIC_PROBE_TWO_POINT
							   && r.decayTime[0] == 0.1f && r.decayTime[1] == 0.1f && r.decayTime[2] == 0.1f && r.earlyDb == -100.0f && r.lateDb == 0.0f
							   && Test_Near( r.preDelay, 0.3, 0.0001 ) && Test_Near( r.lateDelay, 0.4, 0.0001 ), r, flags );
		cases++;
	}

	// sparse: early returns, silence, one echo 20 dB under the peak at 0.6 s: decayTime 0.1 s and bit 5, the
	// levels as computed from the energies
	{
		double energy[TEST_BINS];
		memset( energy, 0, sizeof( energy ) );
		energy[0] = 1.0;
		energy[1] = 0.5;
		energy[2] = 0.25;
		energy[3] = 0.125;
		energy[4] = 0.0625;
		energy[60] = 0.01;
		Test_Energies( histogram, energy );
		same = Test_Estimate( histogram, TEST_SEGMENTS, ACOUSTIC_CEILING_NONE, r, flags );
		const double wantEarly = Test_Level( 1.0 + 0.5 + 0.25 + 0.125 + 0.0625 );
		const double wantLate = Test_Level( 0.01 );
		passed += Test_Report( "sparse", same && ( flags & ACOUSTIC_PROBE_SPARSE ) != 0 && r.decayTime[1] == 0.1f
							   && fabs( r.earlyDb - wantEarly ) <= 0.011 && fabs( r.lateDb - wantLate ) <= 0.011, r, flags );
		cases++;
	}

	// door losses: 1, 0.1, 0.001 give 0, 10, 30 dB; 0, 2, 1e-7 give 60, 0, 60 dB
	{
		static const float open[3] = { 1.0f, 0.1f, 0.001f };
		static const float edge[3] = { 0.0f, 2.0f, 1e-7f };
		float loss[3], lossEdge[3];
		AcousticDoorLoss( open, loss );
		AcousticDoorLoss( edge, lossEdge );
		const bool pass = loss[0] == 0.0f && loss[1] == 10.0f && loss[2] == 30.0f && lossEdge[0] == 60.0f && lossEdge[1] == 0.0f && lossEdge[2] == 60.0f;
		common->Printf( "acousticEstimateSelfTest: %s %-14s loss %.2f %.2f %.2f, %.2f %.2f %.2f\n", pass ? "PASS" : "FAIL", "door loss",
						loss[0], loss[1], loss[2], lossEdge[0], lossEdge[1], lossEdge[2] );
		passed += pass;
		cases++;
	}

	// zones through the running sums. Two equal rooms of three probes (more than pass-through takes) joined
	// only by a door portal stay apart; joined by an open portal they merge by similarity. Area 2 has no probe
	// and no portal: its zone has no probe, so it is listed and takes the empty record.
	// "zones unfed" (E3R-1): area 1 has no probe fed; it is an area with no probe in all three functions, joins
	// room 0 by pass-through and holds the zone's record.
	static const testZoneCase_t zoneCases[5] =
	{
		{
			"zones door", 3, { 3, 3, 0 }, { 1.0, 1.0, 1.0 },
			1, { { 0, 1, 0 } },
			{ 0, 1, 2 }, 3, 1
		},
		{
			"zones open", 3, { 3, 3, 0 }, { 1.0, 1.0, 1.0 },
			1, { { 0, 1, -1 } },
			{ 0, 0, 1 }, 2, 1
		},
		// A-SB2-12's shape: rooms 0 and 1 joined by a door and by a corridor 2-3-4 of one probe each, unlike the
		// rooms and each other; pass-through puts the corridor in room 0, the rooms stay apart
		{
			"zones corridor", 5, { 4, 4, 1, 1, 1 }, { 1.0, 1.0, 0.3, 0.5, 0.8 },
			5, { { 0, 1, 0 }, { 0, 2, -1 }, { 2, 3, -1 }, { 3, 4, -1 }, { 4, 1, -1 } },
			{ 0, 1, 0, 0, 0 }, 2, 0
		},
		// a chain whose neighbours are each within 1.2 but whose union is not: the union's spread, not the pair's
		{
			"zones chain", 4, { 3, 3, 3, 3 }, { 1.0, 1.1, 1.25, 1.375 },
			3, { { 0, 1, -1 }, { 1, 2, -1 }, { 2, 3, -1 } },
			{ 0, 0, 1, 1 }, 2, 0
		},
		{
			"zones unfed", 2, { 3, 0 }, { 1.0, 1.0 },
			1, { { 0, 1, -1 } },
			{ 0, 0 }, 1, 0
		},
	};
	for( int c = 0; c < 5; c++ )
	{
		passed += Test_Zones( zoneCases[c] );
		cases++;
	}

	// given defaults (no estimate) for the tie order, three probes fed per area so pass-through takes none.
	// "zones tie": decayTime[1] 1.5625, 1.875, 2.25 s (25, 30, 36 sixteenths) along a chain of five open portals,
	// a repeated portal and a door: more than four pairs, every max / min exactly 1.2, so the ties fall to
	// ( a, b ): 0-1 merges at the limit, 1-2 is refused by the union, 2-3 merges, 3-4 is refused, 4-5 merges (in
	// the reverse order 3-4 would merge). "zones step tie" (E3R-2): 1.0, 1.1, 1.21 s, 10000, 11000, 12100 steps,
	// whose two ratios are equal in steps though not in floats: ( 0, 1 ) goes first and merges, then 1-2 is
	// refused by the union; ordered by the floats, 1-2 would go first and merge.
	{
		static const double tieDecay[6] = { 1.5625, 1.875, 2.25, 1.875, 1.5625, 1.875 };
		static const sndPortal_t tiePortals[7] = { { 0, 1, -1 }, { 1, 2, -1 }, { 2, 3, -1 }, { 3, 4, -1 }, { 4, 5, -1 }, { 1, 0, -1 }, { 0, 5, 0 } };
		static const int tieWant[6] = { 0, 0, 1, 1, 2, 2 };
		passed += Test_GivenZones( "zones tie", 6, tieDecay, tiePortals, 7, tieWant, 3 );
		static const double stepDecay[3] = { 1.0, 1.1, 1.21 };
		static const sndPortal_t stepPortals[2] = { { 0, 1, -1 }, { 1, 2, -1 } };
		static const int stepWant[3] = { 0, 0, 1 };
		passed += Test_GivenZones( "zones step tie", 3, stepDecay, stepPortals, 2, stepWant, 2 );
		cases += 2;
	}

	common->Printf( "acousticEstimateSelfTest: %d of %d cases passed\n", passed, cases );
#else
	common->Printf( "acousticEstimateSelfTest: built without the acoustic bake\n" );
#endif
}

#endif // !DMAP
