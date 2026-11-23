//puncho - C1-K: R-C1-1 to R-C1-16. No engine state; both hosts compile this file.
#include "snd_convolver.h"
#include "../libs/pffft/pffft.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
========================
idSoundConvolver::idSoundConvolver
========================
*/
idSoundConvolver::idSoundConvolver() :
	setup( NULL ), irSpectra( NULL ), inputSpectra( NULL ), accumulators( NULL ),
	window( NULL ), timeBuffer( NULL ), work( NULL ), live( NULL ),
	capacity( 0 ), partitions( 0 ), head( 0 ), tail( 0 ), previousInGain( 0.0f ),
	previousEarlyGain( 0.0f ), previousLateGain( 0.0f ), previousLive( false ), gainsValid( false )
#ifndef NDEBUG
	, busy( false )
#endif
{
}

size_t idSoundConvolver::MemoryBytes( int maxPartitions )
{
	if ( maxPartitions < 1 || maxPartitions > SND_CONV_MAX_PARTITIONS )
	{
		return 0;
	}
	// Every float array is a whole FFT; flags come last, rounded to 16 bytes.
	return ( 3 * (size_t)maxPartitions + 7 ) * SND_CONV_FFT * sizeof( float ) + ( ( maxPartitions + 15 ) & ~15 );
}

bool idSoundConvolver::Init( void* memory, int maxPartitions )
{
	assert( !busy );
	assert( setup == NULL );
	if ( setup != NULL || MemoryBytes( maxPartitions ) == 0 || memory == NULL || ( (uintptr_t)memory & 15 ) != 0 )
	{
		return false;
	}
	if ( ( SND_CONV_FFT != 1024 && SND_CONV_FFT != 960 ) || !pffft_is_valid_size( SND_CONV_FFT, PFFFT_REAL ) )
	{
		return false;
	}
#ifndef NDEBUG
	busy = true;
#endif
	setup = pffft_new_setup( SND_CONV_FFT, PFFFT_REAL );
	if ( setup == NULL )
	{
#ifndef NDEBUG
		busy = false;
#endif
		return false;
	}
	capacity = maxPartitions;
	irSpectra = (float*)memory;
	inputSpectra = irSpectra + 2 * capacity * SND_CONV_FFT;
	accumulators = inputSpectra + capacity * SND_CONV_FFT;
	window = accumulators + 4 * SND_CONV_FFT;
	timeBuffer = window + SND_CONV_FFT;
	work = timeBuffer + SND_CONV_FFT;
	live = (unsigned char*)( work + SND_CONV_FFT );
	// Touch every page on the caller's thread so the first audio blocks take no page fault.
	memset( memory, 0, MemoryBytes( maxPartitions ) );
	partitions = 0;
	ClearHistory();
#ifndef NDEBUG
	busy = false;
#endif
	return true;
}

void idSoundConvolver::Shutdown()
{
	assert( !busy );
#ifndef NDEBUG
	busy = true;
#endif
	pffft_destroy_setup( (PFFFT_Setup*)setup );
	setup = NULL;
	irSpectra = inputSpectra = accumulators = window = timeBuffer = work = NULL;
	live = NULL;
	capacity = partitions = head = tail = 0;
	previousInGain = previousEarlyGain = previousLateGain = 0.0f;
	previousLive = gainsValid = false;
#ifndef NDEBUG
	busy = false;
#endif
}

void idSoundConvolver::ClearHistory()
{
	memset( live, 0, capacity );
	memset( window, 0, SND_CONV_FFT * sizeof( float ) );
	head = 0;
	tail = 0;
	previousLive = false;
	gainsValid = false;
	previousInGain = previousEarlyGain = previousLateGain = 0.0f;
}

void idSoundConvolver::BeginIR()
{
	assert( !busy );
	if ( setup == NULL )
	{
		return;
	}
#ifndef NDEBUG
	busy = true;
#endif
	partitions = 0;
	memset( irSpectra, 0, 2 * capacity * SND_CONV_FFT * sizeof( float ) );
	ClearHistory();
#ifndef NDEBUG
	busy = false;
#endif
}

void idSoundConvolver::LoadPartition( int k, const float* left, const float* right )
{
	assert( !busy );
	assert( setup != NULL && k >= 0 && k < capacity && left != NULL && right != NULL );
	if ( setup == NULL || k < 0 || k >= capacity || left == NULL || right == NULL )
	{
		return;
	}
#ifndef NDEBUG
	busy = true;
#endif
	for ( int channel = 0; channel < 2; ++channel )
	{
		const float* taps = channel == 0 ? left : right;
		for ( int i = 0; i < SND_CONV_BLOCK; ++i )
		{
			// These comparisons reject both infinities and NaN, without a platform macro.
			timeBuffer[i] = taps[i] >= -FLT_MAX && taps[i] <= FLT_MAX ? taps[i] : 0.0f;
		}
		memset( timeBuffer + SND_CONV_BLOCK, 0, ( SND_CONV_FFT - SND_CONV_BLOCK ) * sizeof( float ) );
		float* spectrum = irSpectra + ( 2 * k + channel ) * SND_CONV_FFT;
		pffft_transform( (PFFFT_Setup*)setup, timeBuffer, spectrum, work, PFFFT_FORWARD );
		// R-C1-7: normalize exactly once, while loading. Multiply-add uses scale 1.
		for ( int i = 0; i < SND_CONV_FFT; ++i )
		{
			spectrum[i] *= 1.0f / SND_CONV_FFT;
		}
	}
#ifndef NDEBUG
	busy = false;
#endif
}

void idSoundConvolver::EndIR( int numPartitions )
{
	assert( !busy );
	assert( setup != NULL && numPartitions >= 0 && numPartitions <= capacity );
	if ( setup == NULL || numPartitions < 0 || numPartitions > capacity )
	{
		return;
	}
#ifndef NDEBUG
	busy = true;
#endif
	partitions = numPartitions;
	ClearHistory();
#ifndef NDEBUG
	busy = false;
#endif
}

void idSoundConvolver::Flush()
{
	assert( !busy );
	if ( setup == NULL )
	{
		return;
	}
#ifndef NDEBUG
	busy = true;
#endif
	ClearHistory();
#ifndef NDEBUG
	busy = false;
#endif
}

int idSoundConvolver::TailBlocksLeft() const
{
	assert( !busy );
	return tail;
}

/*
========================
idSoundConvolver::Process

The 441-sample overlap and 441 new samples precede the zero padding. Circular
wrap can only contaminate the discarded prefix, even with the 960-point FFT.
========================
*/
void idSoundConvolver::Process( const float* in, float inGain, float earlyGain, float lateGain, float* outLeft, float* outRight )
{
	assert( !busy );
	if ( setup == NULL )
	{
		return;
	}
#ifndef NDEBUG
	busy = true;
#endif
	const float oldIn = gainsValid ? previousInGain : inGain;
	const float oldEarly = gainsValid ? previousEarlyGain : earlyGain;
	const float oldLate = gainsValid ? previousLateGain : lateGain;
	previousInGain = inGain;
	previousEarlyGain = earlyGain;
	previousLateGain = lateGain;
	gainsValid = true;
	const bool inputLive = in != NULL && ( inGain != 0.0f || oldIn != 0.0f );
	if ( partitions == 0 || ( !inputLive && tail == 0 ) )
	{
#ifndef NDEBUG
		busy = false;
#endif
		return;
	}

	memcpy( window, window + SND_CONV_BLOCK, SND_CONV_BLOCK * sizeof( float ) );
	if ( !inputLive )
	{
		memset( window + SND_CONV_BLOCK, 0, SND_CONV_BLOCK * sizeof( float ) );
	}
	else if ( oldIn == inGain )
	{
		for ( int i = 0; i < SND_CONV_BLOCK; ++i )
		{
			window[SND_CONV_BLOCK + i] = in[i] * inGain;
		}
	}
	else
	{
		const double step = ( (double)inGain - oldIn ) / SND_CONV_BLOCK;
		for ( int i = 0; i < SND_CONV_BLOCK; ++i )
		{
			const float gain = (float)( oldIn + step * ( i + 1 ) );
			window[SND_CONV_BLOCK + i] = in[i] * gain;
		}
	}
	memset( window + 2 * SND_CONV_BLOCK, 0, ( SND_CONV_FFT - 2 * SND_CONV_BLOCK ) * sizeof( float ) );
	live[head] = (unsigned char)( inputLive || previousLive );
	previousLive = inputLive;
	if ( live[head] )
	{
		pffft_transform( (PFFFT_Setup*)setup, window, inputSpectra + head * SND_CONV_FFT, work, PFFFT_FORWARD );
	}

	bool groupLive[2] = { false, false };
	int slot = head;
	for ( int k = 0; k < partitions; ++k )
	{
		if ( live[slot] )
		{
			const int group = k < SND_CONV_EARLY_PARTITIONS ? 0 : 1;
			float* sum = accumulators + 2 * group * SND_CONV_FFT;
			if ( !groupLive[group] )
			{
				memset( sum, 0, 2 * SND_CONV_FFT * sizeof( float ) );
				groupLive[group] = true;
			}
			const float* input = inputSpectra + slot * SND_CONV_FFT;
			const float* ir = irSpectra + 2 * k * SND_CONV_FFT;
			// PFFFT's API is mono: both calls read input. Adjacency permits cache reuse,
			// but does not meet R-C1-6's literal single-read claim (reported obstacle).
			pffft_zconvolve_accumulate( (PFFFT_Setup*)setup, input, ir, sum, 1.0f );
			pffft_zconvolve_accumulate( (PFFFT_Setup*)setup, input, ir + SND_CONV_FFT, sum + SND_CONV_FFT, 1.0f );
		}
		if ( --slot < 0 )
		{
			slot = capacity - 1;
		}
	}
	for ( int group = 0; group < 2; ++group )
	{
		if ( !groupLive[group] )
		{
			continue;
		}
		const float oldGain = group == 0 ? oldEarly : oldLate;
		const float newGain = group == 0 ? earlyGain : lateGain;
		for ( int channel = 0; channel < 2; ++channel )
		{
			pffft_transform( (PFFFT_Setup*)setup, accumulators + ( 2 * group + channel ) * SND_CONV_FFT, timeBuffer, work, PFFFT_BACKWARD );
			float* out = channel == 0 ? outLeft : outRight;
			if ( oldGain == newGain )
			{
				for ( int i = 0; i < SND_CONV_BLOCK; ++i )
				{
					out[i] += timeBuffer[SND_CONV_BLOCK + i] * newGain;
				}
			}
			else
			{
				const double step = ( (double)newGain - oldGain ) / SND_CONV_BLOCK;
				for ( int i = 0; i < SND_CONV_BLOCK; ++i )
				{
					const float gain = (float)( oldGain + step * ( i + 1 ) );
					out[i] += timeBuffer[SND_CONV_BLOCK + i] * gain;
				}
			}
		}
	}
	if ( ++head == capacity )
	{
		head = 0;
	}
	tail = inputLive ? partitions : tail - 1;
	if ( tail == 0 )
	{
		// No old slot may reappear after the fast idle return stops advancing head.
		memset( live, 0, capacity );
	}
#ifndef NDEBUG
	busy = false;
#endif
}

// Self-test helpers have no state outside the invocation. The reference consumes
// the exact float values supplied to Process/LoadPartition, promoted to double.
struct convolverError_t
{
	double	peakRelative;
	double	rmsRelative;
	int		failed;
};

static float ConvolverTestNoise( uint32_t& seed )
{
	seed = seed * 1664525u + 1013904223u;
	return (float)( seed >> 8 ) * ( 1.0f / 8388608.0f ) - 1.0f;
}

static void ConvolverTestIR( float* left, float* right, int count, int kind )
{
	uint32_t seed = 0x613f27a5u;
	const int length = count * SND_CONV_BLOCK;
	memset( left, 0, length * sizeof( float ) );
	memset( right, 0, length * sizeof( float ) );
	if ( kind < 2 )
	{
		const int tap = kind == 0 ? 0 : 5000;
		left[tap] = 1.0f;
		right[tap] = -0.5f;
		return;
	}
	for ( int i = 0; i < length; ++i )
	{
		if ( kind == 2 )
		{
			left[i] = ConvolverTestNoise( seed );
			right[i] = ConvolverTestNoise( seed );
		}
		else
		{
			left[i] = (float)exp( -6.907755278982137 * i / SND_CONV_RATE );
			right[i] = (float)( -0.75 * exp( -9.210340371976184 * i / SND_CONV_RATE ) );
		}
	}
}

static void ConvolverTestLoad( idSoundConvolver& convolver, const float* left, const float* right, int count )
{
	convolver.BeginIR();
	// Reverse order also checks that loading does not advance an implicit cursor.
	for ( int k = count - 1; k >= 0; --k )
	{
		convolver.LoadPartition( k, left + k * SND_CONV_BLOCK, right + k * SND_CONV_BLOCK );
	}
	convolver.EndIR( count );
}

static void ConvolverTestRender( idSoundConvolver& convolver, const float* input, int inputBlocks, int outputBlocks,
	float early, float late, float* left, float* right )
{
	convolver.Flush();
	memset( left, 0, outputBlocks * SND_CONV_BLOCK * sizeof( float ) );
	memset( right, 0, outputBlocks * SND_CONV_BLOCK * sizeof( float ) );
	for ( int block = 0; block < outputBlocks; ++block )
	{
		convolver.Process( block < inputBlocks ? input + block * SND_CONV_BLOCK : NULL, 1.0f, early, late,
			left + block * SND_CONV_BLOCK, right + block * SND_CONV_BLOCK );
	}
}

static void ConvolverTestReference( const float* input, int inputLength, const float* irLeft, const float* irRight,
	int firstTap, int lastTap, double* left, double* right, int outputLength )
{
	memset( left, 0, outputLength * sizeof( double ) );
	memset( right, 0, outputLength * sizeof( double ) );
	for ( int i = 0; i < inputLength; ++i )
	{
		const double sample = input[i];
		if ( sample == 0.0 )
		{
			continue;
		}
		for ( int tap = firstTap; tap < lastTap; ++tap )
		{
			left[i + tap] += sample * irLeft[tap];
			right[i + tap] += sample * irRight[tap];
		}
	}
}

static void ConvolverTestError( const float* actual, const double* reference, int length, convolverError_t& worst )
{
	double peak = 0.0;
	double maxError = 0.0;
	double referenceSquares = 0.0;
	double errorSquares = 0.0;
	for ( int i = 0; i < length; ++i )
	{
		const double error = fabs( actual[i] - reference[i] );
		const double amplitude = fabs( reference[i] );
		if ( !( error <= DBL_MAX ) )
		{
			++worst.failed;
			worst.peakRelative = worst.rmsRelative = DBL_MAX;
			return;
		}
		if ( amplitude > peak )
		{
			peak = amplitude;
		}
		if ( error > maxError )
		{
			maxError = error;
		}
		referenceSquares += reference[i] * reference[i];
		errorSquares += error * error;
	}
	const double peakRelative = peak > 0.0 ? maxError / peak : ( maxError == 0.0 ? 0.0 : DBL_MAX );
	const double rmsRelative = referenceSquares > 0.0 ? sqrt( errorSquares / referenceSquares ) : ( errorSquares == 0.0 ? 0.0 : DBL_MAX );
	if ( peakRelative > worst.peakRelative )
	{
		worst.peakRelative = peakRelative;
	}
	if ( rmsRelative > worst.rmsRelative )
	{
		worst.rmsRelative = rmsRelative;
	}
	worst.failed += peakRelative > 2e-5 || rmsRelative > 1e-5;
}

static int ConvolverTestNonzeroBits( const float* samples, int length )
{
	int count = 0;
	for ( int i = 0; i < length; ++i )
	{
		uint32_t bits;
		memcpy( &bits, samples + i, sizeof( bits ) );
		count += bits != 0;
	}
	return count;
}

static double ConvolverTestRampError( const float* left, const float* right, double from, double to )
{
	double peak = 0.0;
	double maxError = 0.0;
	for ( int i = 0; i < SND_CONV_BLOCK; ++i )
	{
		const double expected = from + ( to - from ) * ( i + 1 ) / SND_CONV_BLOCK;
		const double leftError = fabs( left[i] - expected );
		const double rightError = fabs( right[i] - expected );
		if ( !( leftError <= DBL_MAX && rightError <= DBL_MAX ) )
		{
			return DBL_MAX;
		}
		if ( fabs( expected ) > peak )
		{
			peak = fabs( expected );
		}
		if ( leftError > maxError )
		{
			maxError = leftError;
		}
		if ( rightError > maxError )
		{
			maxError = rightError;
		}
	}
	return peak > 0.0 ? maxError / peak : ( maxError == 0.0 ? 0.0 : DBL_MAX );
}

/*
========================
SndConvolver_SelfTest

A-C1-1: inputs are 4 blocks (40 ms), retaining every input kind, IR kind,
partition boundary and the complete tail.
========================
*/
int SndConvolver_SelfTest( void ( *print )( const char* line ), void* ( *alloc16 )( size_t ), void ( *free16 )( void* ) )
{
	if ( print == NULL || alloc16 == NULL || free16 == NULL )
	{
		return 6;
	}
	const int inputBlocks = 4;
	const int inputLength = inputBlocks * SND_CONV_BLOCK;
	const int maxTaps = SND_CONV_MAX_PARTITIONS * SND_CONV_BLOCK;
	const int maxFrames = ( inputBlocks + SND_CONV_MAX_PARTITIONS + 2 ) * SND_CONV_BLOCK;
	const size_t floatCount = inputLength + 2 * maxTaps + 6 * maxFrames;
	float* data = (float*)alloc16( floatCount * sizeof( float ) );
	double* reference = (double*)alloc16( 2 * maxFrames * sizeof( double ) );
	void* memoryA = alloc16( idSoundConvolver::MemoryBytes( SND_CONV_MAX_PARTITIONS ) );
	void* memoryB = alloc16( idSoundConvolver::MemoryBytes( SND_CONV_MAX_PARTITIONS ) );
	idSoundConvolver a, b;
	if ( data == NULL || reference == NULL || memoryA == NULL || memoryB == NULL ||
		!a.Init( memoryA, SND_CONV_MAX_PARTITIONS ) || !b.Init( memoryB, SND_CONV_MAX_PARTITIONS ) )
	{
		a.Shutdown();
		b.Shutdown();
		if ( memoryA != NULL )
		{
			free16( memoryA );
		}
		if ( memoryB != NULL )
		{
			free16( memoryB );
		}
		if ( data != NULL )
		{
			free16( data );
		}
		if ( reference != NULL )
		{
			free16( reference );
		}
		print( "A-C1-1..6 FAIL self-test allocation/Init failed" );
		return 6;
	}
	float* input = data;
	float* irLeft = input + inputLength;
	float* irRight = irLeft + maxTaps;
	float* outputs[6];
	for ( int i = 0; i < 6; ++i )
	{
		outputs[i] = irRight + maxTaps + i * maxFrames;
	}
	double* refLeft = reference;
	double* refRight = reference + maxFrames;
	char line[512];
	int failures = 0;
	int repeatedDifferences = 0;
	int cases = 0;
	const int lengths[] = { 1, 7, 8, 9, 50, 100 };
	convolverError_t convolution = {};
	for ( int signal = 0; signal < 3; ++signal )
	{
		uint32_t seed = 0xc3a59b17u;
		for ( int i = 0; i < inputLength; ++i )
		{
			if ( signal == 0 )
			{
				input[i] = i == 220 ? 1.0f : 0.0f;
			}
			else if ( signal == 1 )
			{
				input[i] = ConvolverTestNoise( seed );
			}
			else
			{
				const double sweepRate = log( 1000.0 ) / ( (double)inputLength / SND_CONV_RATE );
				input[i] = (float)sin( 6.283185307179586 * 20.0 * ( exp( sweepRate * i / SND_CONV_RATE ) - 1.0 ) / sweepRate );
			}
		}
		for ( int irCase = 0; irCase < 14; ++irCase )
		{
			const int count = irCase == 0 ? 1 : ( irCase == 1 ? 12 : lengths[( irCase - 2 ) % 6] );
			const int kind = irCase < 2 ? irCase : ( irCase < 8 ? 2 : 3 );
			const int blocks = inputBlocks + count + 2;
			const int frames = blocks * SND_CONV_BLOCK;
			ConvolverTestIR( irLeft, irRight, count, kind );
			ConvolverTestLoad( a, irLeft, irRight, count );
			ConvolverTestReference( input, inputLength, irLeft, irRight, 0, count * SND_CONV_BLOCK, refLeft, refRight, frames );
			ConvolverTestRender( a, input, inputBlocks, blocks, 1.0f, 1.0f, outputs[0], outputs[1] );
			ConvolverTestError( outputs[0], refLeft, frames, convolution );
			ConvolverTestError( outputs[1], refRight, frames, convolution );
			ConvolverTestRender( a, input, inputBlocks, blocks, 1.0f, 1.0f, outputs[2], outputs[3] );
			repeatedDifferences += memcmp( outputs[0], outputs[2], frames * sizeof( float ) ) != 0;
			repeatedDifferences += memcmp( outputs[1], outputs[3], frames * sizeof( float ) ) != 0;
			++cases;
		}
	}
	snprintf( line, sizeof( line ), "A-C1-1 %s cases=%d input_frames=%d max_error/peak=%.17g error_rms/ref_rms_db=%.17g failed_channels=%d",
		convolution.failed == 0 ? "PASS" : "FAIL", cases, inputLength, convolution.peakRelative,
		20.0 * log10( convolution.rmsRelative ), convolution.failed );
	print( line );
	failures += convolution.failed != 0;

	const int offsets[] = { 0, 1, 220, 440 };
	double delayError = 0.0;
	ConvolverTestIR( irLeft, irRight, 1, 0 );
	ConvolverTestLoad( a, irLeft, irRight, 1 );
	for ( int test = 0; test < 4; ++test )
	{
		a.Flush();
		memset( input, 0, SND_CONV_BLOCK * sizeof( float ) );
		input[offsets[test]] = 1.0f;
		for ( int i = 0; i < SND_CONV_BLOCK; ++i )
		{
			outputs[0][i] = 0.25f;
			outputs[1][i] = -0.125f;
		}
		a.Process( input, 1.0f, 1.0f, 1.0f, outputs[0], outputs[1] );
		for ( int i = 0; i < SND_CONV_BLOCK; ++i )
		{
			const double e0 = fabs( outputs[0][i] - ( 0.25 + ( i == offsets[test] ? 1.0 : 0.0 ) ) );
			const double e1 = fabs( outputs[1][i] - ( -0.125 - ( i == offsets[test] ? 0.5 : 0.0 ) ) );
			if ( e0 > delayError )
			{
				delayError = e0;
			}
			if ( e1 > delayError )
			{
				delayError = e1;
			}
		}
	}
	snprintf( line, sizeof( line ), "A-C1-2 %s j=0,1,220,440 delay_samples=0 additive_max_error/peak=%.17g", delayError <= 2e-5 ? "PASS" : "FAIL", delayError );
	print( line );
	failures += delayError > 2e-5;

	int countErrors = 0;
	int lastTailNonzero = 0;
	int afterTailBits = 0;
	int flushBits = 0;
	const int tailLengths[] = { 1, 8, 9, 100 };
	for ( int test = 0; test < 4; ++test )
	{
		const int count = tailLengths[test];
		memset( irLeft, 0, count * SND_CONV_BLOCK * sizeof( float ) );
		memset( irRight, 0, count * SND_CONV_BLOCK * sizeof( float ) );
		irLeft[count * SND_CONV_BLOCK - 1] = 1.0f;
		irRight[count * SND_CONV_BLOCK - 1] = -0.5f;
		ConvolverTestLoad( a, irLeft, irRight, count );
		for ( int mode = 0; mode < 2; ++mode )
		{
			a.Flush();
			for ( int i = 0; i < SND_CONV_BLOCK; ++i )
			{
				input[i] = 1.0f;
			}
			memset( outputs[0], 0, SND_CONV_BLOCK * sizeof( float ) );
			memset( outputs[1], 0, SND_CONV_BLOCK * sizeof( float ) );
			a.Process( input, 1.0f, 1.0f, 1.0f, outputs[0], outputs[1] );
			countErrors += a.TailBlocksLeft() != count;
			if ( mode == 1 )
			{
				// The transition to zero is still a non-silent ramp block (R-C1-13).
				a.Process( input, 0.0f, 1.0f, 1.0f, outputs[0], outputs[1] );
				countErrors += a.TailBlocksLeft() != count;
			}
			for ( int block = 1; block <= count + 2; ++block )
			{
				memset( outputs[0], 0, SND_CONV_BLOCK * sizeof( float ) );
				memset( outputs[1], 0, SND_CONV_BLOCK * sizeof( float ) );
				a.Process( mode == 0 ? NULL : input, mode == 0 ? 1.0f : 0.0f, 1.0f, 1.0f, outputs[0], outputs[1] );
				countErrors += a.TailBlocksLeft() != ( block < count ? count - block : 0 );
				if ( block == count )
				{
					// A-C1-3: this call emits the final valid tail before the count is zero.
					lastTailNonzero += ConvolverTestNonzeroBits( outputs[0], SND_CONV_BLOCK );
					lastTailNonzero += ConvolverTestNonzeroBits( outputs[1], SND_CONV_BLOCK );
				}
				else if ( block > count )
				{
					afterTailBits += ConvolverTestNonzeroBits( outputs[0], SND_CONV_BLOCK );
					afterTailBits += ConvolverTestNonzeroBits( outputs[1], SND_CONV_BLOCK );
				}
			}
			a.Process( input, 1.0f, 1.0f, 1.0f, outputs[0], outputs[1] );
			a.Flush();
			memset( outputs[0], 0, SND_CONV_BLOCK * sizeof( float ) );
			memset( outputs[1], 0, SND_CONV_BLOCK * sizeof( float ) );
			a.Process( NULL, 1.0f, 1.0f, 1.0f, outputs[0], outputs[1] );
			flushBits += ConvolverTestNonzeroBits( outputs[0], SND_CONV_BLOCK );
			flushBits += ConvolverTestNonzeroBits( outputs[1], SND_CONV_BLOCK );
			countErrors += a.TailBlocksLeft() != 0;
		}
	}
	const bool tailFailed = countErrors != 0 || afterTailBits != 0 || flushBits != 0;
	snprintf( line, sizeof( line ), "A-C1-3 %s count_errors=%d last_valid_tail_nonzero_samples=%d after_final_tail=%d after_flush=%d",
		tailFailed ? "FAIL" : "PASS", countErrors, lastTailNonzero, afterTailBits, flushBits );
	print( line );
	failures += tailFailed;

	convolverError_t splitError = {};
	double splitRelative = 0.0;
	uint32_t seed = 0xf6913a27u;
	for ( int i = 0; i < inputLength; ++i )
	{
		input[i] = ConvolverTestNoise( seed );
	}
	for ( int test = 0; test < 6; ++test )
	{
		const int count = lengths[test];
		const int taps = count * SND_CONV_BLOCK;
		const int split = count < SND_CONV_EARLY_PARTITIONS ? taps : SND_CONV_EARLY_PARTITIONS * SND_CONV_BLOCK;
		const int blocks = inputBlocks + count + 2;
		const int frames = blocks * SND_CONV_BLOCK;
		ConvolverTestIR( irLeft, irRight, count, 2 );
		ConvolverTestLoad( a, irLeft, irRight, count );
		for ( int path = 0; path < 3; ++path )
		{
			ConvolverTestRender( a, input, inputBlocks, blocks, path == 2 ? 0.0f : 1.0f, path == 1 ? 0.0f : 1.0f, outputs[2 * path], outputs[2 * path + 1] );
			ConvolverTestReference( input, inputLength, irLeft, irRight, path == 2 ? split : 0, path == 1 ? split : taps, refLeft, refRight, frames );
			ConvolverTestError( outputs[2 * path], refLeft, frames, splitError );
			ConvolverTestError( outputs[2 * path + 1], refRight, frames, splitError );
		}
		for ( int channel = 0; channel < 2; ++channel )
		{
			double peak = 0.0;
			double error = 0.0;
			for ( int i = 0; i < frames; ++i )
			{
				const double amplitude = fabs( outputs[channel][i] );
				const double difference = fabs( (double)outputs[channel][i] - ( outputs[2 + channel][i] + outputs[4 + channel][i] ) );
				if ( amplitude > peak )
				{
					peak = amplitude;
				}
				if ( difference > error )
				{
					error = difference;
				}
			}
			if ( error / peak > splitRelative )
			{
				splitRelative = error / peak;
			}
		}
	}
	const bool splitFailed = splitError.failed != 0 || splitRelative > 1e-6;
	snprintf( line, sizeof( line ), "A-C1-4 %s max_error/peak=%.17g error_rms/ref_rms_db=%.17g split_sum_error/peak=%.17g failed_channels=%d",
		splitFailed ? "FAIL" : "PASS", splitError.peakRelative, 20.0 * log10( splitError.rmsRelative ), splitRelative, splitError.failed );
	print( line );
	failures += splitFailed;

	double rampError[3] = {};
	double flushError = 0.0;
	for ( int control = 0; control < 3; ++control )
	{
		const int count = control == 2 ? 9 : 1;
		memset( irLeft, 0, count * SND_CONV_BLOCK * sizeof( float ) );
		memset( irRight, 0, count * SND_CONV_BLOCK * sizeof( float ) );
		irLeft[( count - 1 ) * SND_CONV_BLOCK] = irRight[( count - 1 ) * SND_CONV_BLOCK] = 1.0f;
		ConvolverTestLoad( a, irLeft, irRight, count );
		for ( int i = 0; i < SND_CONV_BLOCK; ++i )
		{
			input[i] = 1.0f;
		}
		float gains[3] = { 1.0f, 1.0f, 1.0f };
		gains[control] = 0.0f;
		for ( int block = 0; block < 10; ++block )
		{
			memset( outputs[0], 0, SND_CONV_BLOCK * sizeof( float ) );
			memset( outputs[1], 0, SND_CONV_BLOCK * sizeof( float ) );
			a.Process( input, gains[0], gains[1], gains[2], outputs[0], outputs[1] );
		}
		for ( int step = 0; step < 2; ++step )
		{
			const double old = gains[control];
			gains[control] = step == 0 ? 1.0f : 0.25f;
			memset( outputs[0], 0, SND_CONV_BLOCK * sizeof( float ) );
			memset( outputs[1], 0, SND_CONV_BLOCK * sizeof( float ) );
			a.Process( input, gains[0], gains[1], gains[2], outputs[0], outputs[1] );
			const double error = ConvolverTestRampError( outputs[0], outputs[1], old, gains[control] );
			if ( error > rampError[control] )
			{
				rampError[control] = error;
			}
		}
		a.Flush();
		for ( int block = 0; block < count; ++block )
		{
			memset( outputs[0], 0, SND_CONV_BLOCK * sizeof( float ) );
			memset( outputs[1], 0, SND_CONV_BLOCK * sizeof( float ) );
			a.Process( input, gains[0], gains[1], gains[2], outputs[0], outputs[1] );
		}
		const double error = ConvolverTestRampError( outputs[0], outputs[1], 0.25, 0.25 );
		if ( error > flushError )
		{
			flushError = error;
		}
	}
	const bool rampFailed = rampError[0] > 2e-5 || rampError[1] > 2e-5 || rampError[2] > 2e-5 || flushError > 2e-5;
	snprintf( line, sizeof( line ), "A-C1-5 %s max_error/peak_input=%.17g early=%.17g late=%.17g after_flush=%.17g bound=2e-5",
		rampFailed ? "FAIL" : "PASS", rampError[0], rampError[1], rampError[2], flushError );
	print( line );
	failures += rampFailed;

	// Fill the old IR and its history before replacing it with a sparse, dirty IR.
	ConvolverTestIR( irLeft, irRight, 100, 2 );
	ConvolverTestLoad( a, irLeft, irRight, 100 );
	a.Process( input, 1.0f, 1.0f, 1.0f, outputs[0], outputs[1] );
	a.BeginIR();
	b.Shutdown();
	const bool freshOK = b.Init( memoryB, 9 );
	b.BeginIR();
	ConvolverTestIR( irLeft, irRight, 9, 2 );
	const int sparse[] = { 8, 0, 3 };
	for ( int index = 0; index < 3; ++index )
	{
		const int k = sparse[index];
		const uint32_t bad[] = { 0x7fc00001u, 0x7f800000u, 0xff800000u };
		for ( int n = 0; n < 3; ++n )
		{
			memcpy( irLeft + k * SND_CONV_BLOCK + n, bad + n, sizeof( float ) );
			memcpy( irRight + k * SND_CONV_BLOCK + n + 4, bad + n, sizeof( float ) );
		}
		a.LoadPartition( k, irLeft + k * SND_CONV_BLOCK, irRight + k * SND_CONV_BLOCK );
		for ( int n = 0; n < 3; ++n )
		{
			irLeft[k * SND_CONV_BLOCK + n] = 0.0f;
			irRight[k * SND_CONV_BLOCK + n + 4] = 0.0f;
		}
		b.LoadPartition( k, irLeft + k * SND_CONV_BLOCK, irRight + k * SND_CONV_BLOCK );
	}
	a.EndIR( 9 );
	b.EndIR( 9 );
	seed = 0x71343275u;
	for ( int i = 0; i < inputLength; ++i )
	{
		input[i] = ConvolverTestNoise( seed );
	}
	ConvolverTestRender( a, input, inputBlocks, inputBlocks + 11, 1.0f, 1.0f, outputs[0], outputs[1] );
	ConvolverTestRender( b, input, inputBlocks, inputBlocks + 11, 1.0f, 1.0f, outputs[2], outputs[3] );
	int reloadDifferences = !freshOK;
	for ( int channel = 0; channel < 2; ++channel )
	{
		reloadDifferences += memcmp( outputs[channel], outputs[channel + 2], ( inputBlocks + 11 ) * SND_CONV_BLOCK * sizeof( float ) ) != 0;
	}
	idSoundConvolver invalid;
	int invalidErrors = 0;
	invalidErrors += invalid.Init( NULL, 1 );
	invalidErrors += invalid.Init( (char*)memoryA + 1, 1 );
	invalidErrors += invalid.Init( memoryA, 0 );
	invalidErrors += invalid.Init( memoryA, 101 );
	invalidErrors += idSoundConvolver::MemoryBytes( 0 ) != 0 || idSoundConvolver::MemoryBytes( 101 ) != 0;
	invalid.Shutdown();
	const bool repeatFailed = repeatedDifferences != 0 || reloadDifferences != 0 || invalidErrors != 0;
	snprintf( line, sizeof( line ), "A-C1-6 %s repeat_channel_mismatches=%d reload_nonfinite_channel_mismatches=%d invalid_init_errors=%d capacities=100,9 sparse=8,0,3",
		repeatFailed ? "FAIL" : "PASS", repeatedDifferences, reloadDifferences, invalidErrors );
	print( line );
	failures += repeatFailed;
	a.Shutdown();
	b.Shutdown();
	free16( memoryA );
	free16( memoryB );
	free16( data );
	free16( reference );
	return failures;
}
