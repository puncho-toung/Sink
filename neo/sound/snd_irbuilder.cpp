//puncho: WP-C2. Envelopes carry integrated stereo energy, before band filtering.
#include "snd_irbuilder.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <string.h>

static const uint32_t sndIRSeeds[2][3] =
{
	{ 0x243f6a88u, 0x85a308d3u, 0x13198a2eu },
	{ 0x03707344u, 0xa4093822u, 0x299f31d0u }
};

static float SndIR_Clamp( float value, float lower, float upper )
{
	// Both infinities, as well as NaN, take the lower bound.
	if ( !( value >= -FLT_MAX && value <= FLT_MAX ) || value < lower )
	{
		return lower;
	}
	return value > upper ? upper : value;
}

void SndIR_Butterworth4( bool highPass, double frequency, float b[2][3], float a[2][2] )
{
	assert( frequency > 0.0 && frequency < 0.5 * SND_IR_RATE );
	const double angle = frequency * ( 2.0 * 3.14159265358979323846 / SND_IR_RATE );
	const double cosine = cos( angle );
	const double sine = sin( angle );
	const double sign = highPass ? -1.0 : 1.0;
	const double numerator = 0.5 * ( 1.0 - sign * cosine );
	for ( int section = 0; section < 2; section++ )
	{
		const double quality = section == 0 ? 0.54119610 : 1.30656296;
		const double alpha = sine / ( 2.0 * quality );
		const double normalizer = 1.0 / ( 1.0 + alpha );
		b[section][0] = ( float )( numerator * normalizer );
		b[section][1] = ( float )( 2.0 * sign * numerator * normalizer );
		b[section][2] = b[section][0];
		a[section][0] = ( float )( -2.0 * cosine * normalizer );
		a[section][1] = ( float )( ( 1.0 - alpha ) * normalizer );
	}
}

idSoundIRBuilder::idSoundIRBuilder()
{
	memset( this, 0, sizeof( *this ) );
}

void idSoundIRBuilder::Begin( const sndIRRecord_t& input, unsigned int zoneSeed )
{
	memset( this, 0, sizeof( *this ) );
	for ( int band = 0; band < 3; band++ )
	{
		record.decayTime[band] = SndIR_Clamp( input.decayTime[band], 0.1f, 20.0f );
		decayStep[band] = pow( 10.0, -3.0 / ( ( double )record.decayTime[band] * SND_IR_RATE ) );
		sampleStep[band] = ( float )decayStep[band];
	}
	// Low: sections 0-1; mid: 2-5; high: 6-7. State is separate per channel.
	SndIR_Butterworth4( false, 800.0, filterB, filterA );
	SndIR_Butterworth4( true, 800.0, filterB + 2, filterA + 2 );
	SndIR_Butterworth4( false, 8000.0, filterB + 4, filterA + 4 );
	SndIR_Butterworth4( true, 8000.0, filterB + 6, filterA + 6 );
	record.earlyDb = SndIR_Clamp( input.earlyDb, -100.0f, 40.0f );
	record.lateDb = SndIR_Clamp( input.lateDb, -100.0f, 40.0f );
	record.preDelay = SndIR_Clamp( input.preDelay, 0.0f, 0.3f );
	const float minimumLate = record.preDelay > 0.08f ? record.preDelay : 0.08f;
	record.lateDelay = SndIR_Clamp( input.lateDelay, minimumLate, 0.4f );
	earlyStart = ( int )floor( ( double )record.preDelay * SND_IR_RATE + 0.5 );
	lateStart = ( int )floor( ( double )record.lateDelay * SND_IR_RATE + 0.5 );
	if ( lateStart < SND_IR_EARLY_SAMPLES )
	{
		lateStart = SND_IR_EARLY_SAMPLES;
	}

	const double q = decayStep[1] * decayStep[1];
	if ( earlyStart < SND_IR_EARLY_SAMPLES && record.earlyDb > SND_IR_SILENT_DB )
	{
		const double earlyEnergy = pow( 10.0, ( double )record.earlyDb / 10.0 );
		const double earlySum = 1.0 - pow( q, SND_IR_EARLY_SAMPLES - earlyStart );
		earlyAmplitude = sqrt( 0.5 * earlyEnergy * ( 1.0 - q ) / earlySum );
		numPartitions = ( SND_IR_EARLY_SAMPLES + SND_IR_TAPER_SAMPLES ) / SND_IR_PARTITION;
	}
	if ( record.lateDb > SND_IR_SILENT_DB )
	{
		const double lateEnergy = pow( 10.0, ( double )record.lateDb / 10.0 );
		lateAmplitude = sqrt( 0.5 * lateEnergy * ( 1.0 - q ) );
		float longestDecay = record.decayTime[0];
		for ( int band = 1; band < 3; band++ )
		{
			if ( record.decayTime[band] > longestDecay )
			{
				longestDecay = record.decayTime[band];
			}
		}
		const int samples = lateStart + ( int )ceil( ( double )longestDecay * SND_IR_RATE - 1.0e-3 );
		numPartitions = ( samples + SND_IR_PARTITION - 1 ) / SND_IR_PARTITION;
		if ( numPartitions > SND_IR_MAX_PARTITIONS )
		{
			numPartitions = SND_IR_MAX_PARTITIONS;
		}
	}

	const uint32_t seed = ( uint32_t )zoneSeed * UINT32_C( 2654435761 );
	for ( int channel = 0; channel < 2; channel++ )
	{
		for ( int band = 0; band < 3; band++ )
		{
			const uint32_t mixed = sndIRSeeds[channel][band] ^ seed;
			noiseState[channel][band] = mixed != 0 ? mixed : sndIRSeeds[channel][band];
		}
	}
}

bool idSoundIRBuilder::NextPartition( float* left, float* right )
{
	if ( partitionsDone == numPartitions )
	{
		return false;
	}
	float* channels[2] = { left, right };
	memset( left, 0, SND_IR_PARTITION * sizeof( *left ) );
	memset( right, 0, SND_IR_PARTITION * sizeof( *right ) );
	const int first = partitionsDone * SND_IR_PARTITION;
	for ( int band = 0; band < 3; band++ )
	{
		// Re-anchor both parts in double, including an onset inside this partition.
		const int earlyOffset = first > earlyStart ? first - earlyStart : 0;
		const int lateOffset = first > lateStart ? first - lateStart : 0;
		float early = ( float )( earlyAmplitude * pow( decayStep[band], earlyOffset ) );
		float late = ( float )( lateAmplitude * pow( decayStep[band], lateOffset ) );
		const int firstSection = band == 0 ? 0 : ( band == 1 ? 2 : 6 );
		const int sectionCount = band == 1 ? 4 : 2;
		for ( int i = 0; i < SND_IR_PARTITION; i++ )
		{
			const int sample = first + i;
			float amplitude = 0.0f;
			if ( sample >= earlyStart && sample < SND_IR_EARLY_SAMPLES )
			{
				amplitude = early;
				early *= sampleStep[band];
			}
			else if ( sample >= lateStart )
			{
				amplitude = late;
				late *= sampleStep[band];
			}
			const float gain = amplitude * 1.7320508075688772935f;
			for ( int channel = 0; channel < 2; channel++ )
			{
				uint32_t& noise = noiseState[channel][band];
				noise ^= noise << 13;
				noise ^= noise >> 17;
				noise ^= noise << 5;
				const float white = ( ( int )( noise >> 8 ) - 8388608 ) * ( 1.0f / 8388608.0f );
				float value = gain * white;
				for ( int section = firstSection; section < firstSection + sectionCount; section++ )
				{
					float* state = filterState[channel][section];
					const float output = filterB[section][0] * value + state[0];
					state[0] = filterB[section][1] * value - filterA[section][0] * output + state[1];
					state[1] = filterB[section][2] * value - filterA[section][1] * output;
					value = output;
				}
				channels[channel][i] += value;
			}
		}
	}

	const int total = numPartitions * SND_IR_PARTITION;
	const int taperStart = total - SND_IR_TAPER_SAMPLES;
	if ( first + SND_IR_PARTITION > taperStart )
	{
		const int start = taperStart > first ? taperStart - first : 0;
		for ( int i = start; i < SND_IR_PARTITION; i++ )
		{
			const float ramp = ( float )( total - 1 - first - i ) / ( SND_IR_TAPER_SAMPLES - 1 );
			left[i] *= ramp;
			right[i] *= ramp;
		}
	}
	partitionsDone++;
	return true;
}
