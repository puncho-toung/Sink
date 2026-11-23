/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013 Robert Beckebans
Copyright (C) 1997-2012 Sam Lantinga <slouken@libsdl.org>  (MS ADPCM decoder)
Copyright (c) 2011 Chris Robinson <chris.kcat@gmail.com> (OpenAL helpers)

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/
//puncho - C3b-V. Compile with /fp:precise (MSVC), -ffp-contract=off (GCC/Clang).
// No multiply/add contraction; the scalar expression order is the signal law.
#include "snd_voice.h"
#include <stddef.h>
#include <math.h>
#include <string.h>
#include <assert.h>
#if !defined( SND_VOICE_PLAIN ) && ( defined( _M_X64 ) || defined( __x86_64__ ) )
#define SND_VOICE_SSE2 1
#include <emmintrin.h>
#else
#define SND_VOICE_SSE2 0
#endif

bool SndSample_ViewValid( const sndSampleView_t& view )
{
	if ( view.data == NULL || view.numFrames < 0 || view.rate < 1000 || view.rate > 192000 ||
		( view.channels != 1 && view.channels != 2 ) )
	{
		return false;
	}
	if ( view.format == SND_SAMPLE_PCM16 )
	{
		return true;
	}
	if ( view.format != SND_SAMPLE_MSADPCM || view.blockFrames < 4 || view.blockFrames > SND_VOICE_ADPCM_FRAMES )
	{
		return false;
	}
	const int nibbles = ( view.blockFrames - 2 ) * view.channels;
	return ( nibbles & 1 ) == 0 && view.blockBytes == 7 * view.channels + nibbles / 2 &&
		view.numFrames % view.blockFrames == 0;
}

struct sndAdpcmState_t
{
	short			coef1, coef2;
	unsigned short	iDelta;
	short			iSamp1, iSamp2;
};

// The arithmetic from AL_SoundSample.cpp, including both signed divisions and
// the unsigned delta truncation. Only the engine integer type names changed.
static int SndAdpcm_Nibble( sndAdpcmState_t* state, signed char nybble )
{
	const int max_audioval = ( ( 1 << ( 16 - 1 ) ) - 1 );
	const int min_audioval = -( 1 << ( 16 - 1 ) );
	static const int adaptive[] =
	{
		230, 230, 230, 230, 307, 409, 512, 614,
		768, 614, 512, 409, 307, 230, 230, 230
	};

	int new_sample, delta;

	new_sample = ( ( state->iSamp1 * state->coef1 ) +
				   ( state->iSamp2 * state->coef2 ) ) / 256;

	if( nybble & 0x08 )
	{
		new_sample += state->iDelta * ( nybble - 0x10 );
	}
	else
	{
		new_sample += state->iDelta * nybble;
	}

	if( new_sample < min_audioval )
	{
		new_sample = min_audioval;
	}
	else if( new_sample > max_audioval )
	{
		new_sample = max_audioval;
	}

	delta = ( ( int ) state->iDelta * adaptive[nybble] ) / 256;
	if( delta < 16 )
	{
		delta = 16;
	}

	state->iDelta = ( unsigned short ) delta;
	state->iSamp2 = state->iSamp1;
	state->iSamp1 = ( short ) new_sample;

	return ( new_sample );
}

template< int channels > static int SndAdpcm_DecodeChannels( const sndSampleView_t& view, int block, short* out )
{
	const unsigned char* encoded = view.data + (size_t)block * view.blockBytes;
	sndAdpcmState_t states[2] = {};
	for ( int c = 0; c < channels; c++ )
	{
		const int predictor = *encoded > 6 ? 6 : *encoded;
		encoded++;
		states[c].coef1 = view.coef[predictor][0];
		states[c].coef2 = view.coef[predictor][1];
	}
	for ( int c = 0; c < channels; c++ )
	{
		states[c].iDelta = (unsigned short)( encoded[0] | ( encoded[1] << 8 ) );
		encoded += 2;
	}
	for ( int c = 0; c < channels; c++ )
	{
		states[c].iSamp1 = (short)( encoded[0] | ( encoded[1] << 8 ) );
		encoded += 2;
	}
	for ( int c = 0; c < channels; c++ )
	{
		states[c].iSamp2 = (short)( encoded[0] | ( encoded[1] << 8 ) );
		encoded += 2;
	}
	for ( int c = 0; c < channels; c++ )
	{
		*out++ = states[c].iSamp2;
	}
	for ( int c = 0; c < channels; c++ )
	{
		*out++ = states[c].iSamp1;
	}
	for ( int remaining = ( view.blockFrames - 2 ) * channels; remaining > 0; remaining -= 2 )
	{
		*out++ = (short)SndAdpcm_Nibble( &states[0], (signed char)( *encoded >> 4 ) );
		*out++ = (short)SndAdpcm_Nibble( &states[channels - 1], (signed char)( *encoded & 15 ) );
		encoded++;
	}
	return view.blockFrames;
}

int SndAdpcm_DecodeBlock( const sndSampleView_t& view, int block, short* out )
{
	return view.channels == 1 ? SndAdpcm_DecodeChannels< 1 >( view, block, out ) : SndAdpcm_DecodeChannels< 2 >( view, block, out );
}

static bool SndVoice_HasLoop( const sndVoiceCmd_t& cmd )
{
	return cmd.loop.format != SND_SAMPLE_NONE && cmd.loop.numFrames > 0;
}

static float SndVoice_Coef( float value )
{
	return !isfinite( value ) || value < 0.0f || value > 1.0f ? 1.0f : value;
}

static uint64_t SndVoice_Step( float pitch, int rate )
{
	if ( !isfinite( pitch ) || pitch <= 0.0f )
	{
		pitch = 1.0f;
	}
	double step = (double)pitch * (double)rate / 44100.0;
	step = step < 1.0 / 64.0 ? 1.0 / 64.0 : ( step > 8.0 ? 8.0 : step );
	return (uint64_t)( step * 4294967296.0 + 0.5 );
}

void SndVoice_Start( sndVoiceState_t& state, const sndVoiceCmd_t& cmd )
{
	memset( &state, 0, sizeof( state ) );
	state.generation = cmd.generation;
	state.active = true;
	state.edgeIn = true;
	state.cacheBlock = -1;
	state.filterCoef = SndVoice_Coef( cmd.filterCoef );
	const bool loop = SndVoice_HasLoop( cmd );
	assert( cmd.startFrame >= 0 );
	state.inLoop = cmd.startInLoop != 0 && loop;
	state.position = (uint64_t)cmd.startFrame << 32;
	if ( !state.inLoop && loop && cmd.startFrame >= cmd.leadin.numFrames )
	{
		state.position -= (uint64_t)cmd.leadin.numFrames << 32;
		state.inLoop = true;
	}
	if ( state.inLoop )
	{
		state.position %= (uint64_t)cmd.loop.numFrames << 32;
	}
	state.readerInLoop = state.inLoop;
	state.readerFrame = (int)( state.position >> 32 ) - 1;
	if ( !loop && cmd.startFrame >= cmd.leadin.numFrames )
	{
		state.active = false;
		state.ended = true;
	}
}

// At most one rate change occurs in a block. Positions here are unwrapped,
// relative to its first whole source frame; the state stays sample-relative.
struct sndVoiceSpan_t
{
	uint64_t position;
	uint64_t step;
	int first;
	int count;
};

struct sndVoicePlan_t
{
	sndVoiceSpan_t spans[2];
	int numSpans;
	int count;
	int next;
	int live;
};

static void SndVoice_Plan( sndVoiceState_t& state, const sndVoiceCmd_t& cmd, sndVoicePlan_t& plan )
{
	const bool loop = SndVoice_HasLoop( cmd );
	const sndSampleView_t& sample = state.inLoop ? cmd.loop : cmd.leadin;
	const uint64_t step = SndVoice_Step( cmd.pitch, sample.rate );
	const uint64_t end = (uint64_t)sample.numFrames << 32;
	const uint64_t fraction = (uint32_t)state.position;
	plan.numSpans = 1;
	plan.live = SND_VOICE_BLOCK;
	plan.spans[0].position = fraction;
	plan.spans[0].step = step;
	plan.spans[0].first = 0;
	plan.spans[0].count = SND_VOICE_BLOCK;
	if ( !state.inLoop )
	{
		const uint64_t beforeEnd = state.position >= end ? 0 : ( end - state.position + step - 1 ) / step;
		if ( beforeEnd < SND_VOICE_BLOCK )
		{
			if ( loop )
			{
				plan.spans[0].count = (int)beforeEnd;
				plan.numSpans = 2;
				plan.spans[1].position = fraction + beforeEnd * step;
				plan.spans[1].step = SndVoice_Step( cmd.pitch, cmd.loop.rate );
				plan.spans[1].first = (int)beforeEnd;
				plan.spans[1].count = SND_VOICE_BLOCK - (int)beforeEnd;
			}
			else
			{
				plan.live = (int)beforeEnd;
				state.ended = true;
				state.active = false;
			}
		}
	}
	const sndVoiceSpan_t& tail = plan.spans[plan.numSpans - 1];
	const uint64_t next = tail.position + tail.step * tail.count;
	const uint64_t last = next - tail.step;
	plan.next = (int)( next >> 32 );
	plan.count = (int)( last >> 32 ) + 4;
	if ( plan.count < plan.next + 1 )
	{
		plan.count = plan.next + 1;
	}
	assert( plan.count <= SND_VOICE_FETCH_FRAMES );
	state.position += next - fraction;
	if ( !state.inLoop && loop && state.position >= end )
	{
		state.position -= end;
		state.inLoop = true;
	}
	if ( state.inLoop )
	{
		state.position %= (uint64_t)cmd.loop.numFrames << 32;
	}
}

static void SndVoice_Convert( const short* input, int channels, int count, float* left, float* right )
{
	int n = 0;
#if SND_VOICE_SSE2
	const __m128 scale = _mm_set1_ps( 1.0f / 32768.0f );
	if ( channels == 1 )
	{
		for ( ; n + 8 <= count; n += 8 )
		{
			const __m128i x = _mm_loadu_si128( (const __m128i*)( input + n ) );
			const __m128i sign = _mm_srai_epi16( x, 15 );
			_mm_storeu_ps( left + n, _mm_mul_ps( _mm_cvtepi32_ps( _mm_unpacklo_epi16( x, sign ) ), scale ) );
			_mm_storeu_ps( left + n + 4, _mm_mul_ps( _mm_cvtepi32_ps( _mm_unpackhi_epi16( x, sign ) ), scale ) );
		}
	}
	else
	{
		for ( ; n + 4 <= count; n += 4 )
		{
			const __m128i x = _mm_loadu_si128( (const __m128i*)( input + 2 * n ) );
			_mm_storeu_ps( left + n, _mm_mul_ps( _mm_cvtepi32_ps( _mm_srai_epi32( _mm_slli_epi32( x, 16 ), 16 ) ), scale ) );
			_mm_storeu_ps( right + n, _mm_mul_ps( _mm_cvtepi32_ps( _mm_srai_epi32( x, 16 ) ), scale ) );
		}
	}
#endif
	if ( channels == 1 )
	{
		for ( ; n < count; n++ )
		{
			left[n] = input[n] * ( 1.0f / 32768.0f );
		}
	}
	else
	{
		for ( ; n < count; n++ )
		{
			left[n] = input[n * 2] * ( 1.0f / 32768.0f );
			right[n] = input[n * 2 + 1] * ( 1.0f / 32768.0f );
		}
	}
}

static void SndVoice_Read( sndVoiceState_t& state, const sndVoiceCmd_t& cmd, const sndVoicePlan_t& plan, float* fetch )
{
	const int channels = cmd.leadin.channels;
	for ( int c = 0; c < channels; c++ )
	{
		memcpy( fetch + c * SND_VOICE_FETCH_FRAMES, state.carry[c], state.carryCount * sizeof( float ) );
	}
	int at = state.carryCount;
	while ( at < plan.count )
	{
		const sndSampleView_t& sample = state.readerInLoop ? cmd.loop : cmd.leadin;
		if ( state.readerFrame >= sample.numFrames || sample.numFrames == 0 )
		{
			if ( SndVoice_HasLoop( cmd ) )
			{
				state.readerFrame = 0;
				state.readerInLoop = true;
				state.cacheBlock = -1;
				continue;
			}
			for ( int c = 0; c < channels; c++ )
			{
				memset( fetch + c * SND_VOICE_FETCH_FRAMES + at, 0, ( plan.count - at ) * sizeof( float ) );
			}
			break;
		}
		const int frame = state.readerFrame < 0 ? 0 : state.readerFrame;
		int count = sample.numFrames - frame;
		if ( count > plan.count - at )
		{
			count = plan.count - at;
		}
		if ( state.readerFrame < 0 )
		{
			count = 1;
		}
		const short* input;
		if ( sample.format == SND_SAMPLE_MSADPCM )
		{
			const int block = frame / sample.blockFrames;
			const int offset = frame % sample.blockFrames;
			if ( count > sample.blockFrames - offset )
			{
				count = sample.blockFrames - offset;
			}
			if ( state.cacheBlock != block )
			{
				SndAdpcm_DecodeBlock( sample, block, state.decoded );
				state.cacheBlock = block;
			}
			input = state.decoded + offset * channels;
		}
		else
		{
			input = (const short*)sample.data + (size_t)frame * channels;
		}
		SndVoice_Convert( input, channels, count, fetch + at, fetch + SND_VOICE_FETCH_FRAMES + at );
		state.readerFrame += count;
		at += count;
	}
	state.carryCount = plan.count - plan.next;
	assert( state.carryCount >= 1 && state.carryCount <= 4 );
	for ( int c = 0; c < channels; c++ )
	{
		memcpy( state.carry[c], fetch + c * SND_VOICE_FETCH_FRAMES + plan.next, state.carryCount * sizeof( float ) );
	}
}

static void SndVoice_Cubic( const sndVoicePlan_t& plan, int channels, const float* fetch, float* scratch )
{
	for ( int c = 0; c < channels; c++ )
	{
		const float* input = fetch + c * SND_VOICE_FETCH_FRAMES;
		float* output = scratch + c * SND_VOICE_BLOCK;
		for ( int s = 0; s < plan.numSpans; s++ )
		{
			const sndVoiceSpan_t& span = plan.spans[s];
			uint64_t position = span.position;
			const int end = span.first + span.count < plan.live ? span.first + span.count : plan.live;
			int n = span.first;
			if ( span.step == UINT64_C( 4294967296 ) && (uint32_t)position == 0 )
			{
				if ( end > n )
				{
					memcpy( output + n, input + ( position >> 32 ) + 1, ( end - n ) * sizeof( float ) );
				}
				continue;
			}
#if SND_VOICE_SSE2
			const __m128 half = _mm_set1_ps( 0.5f ), two = _mm_set1_ps( 2.0f ), three = _mm_set1_ps( 3.0f );
			const __m128 four = _mm_set1_ps( 4.0f ), five = _mm_set1_ps( 5.0f );
			for ( ; n + 4 <= end; n += 4, position += span.step * 4 )
			{
				const uint64_t q0 = position, q1 = q0 + span.step, q2 = q1 + span.step, q3 = q2 + span.step;
				const __m128 t = _mm_mul_ps( _mm_cvtepi32_ps( _mm_setr_epi32( (uint32_t)q0 >> 8, (uint32_t)q1 >> 8,
					(uint32_t)q2 >> 8, (uint32_t)q3 >> 8 ) ), _mm_set1_ps( 1.0f / 16777216.0f ) );
				__m128 a = _mm_loadu_ps( input + ( q0 >> 32 ) ), b = _mm_loadu_ps( input + ( q1 >> 32 ) );
				__m128 d = _mm_loadu_ps( input + ( q3 >> 32 ) ), cc = _mm_loadu_ps( input + ( q2 >> 32 ) );
				_MM_TRANSPOSE4_PS( a, b, cc, d );
				const __m128 inner = _mm_sub_ps( _mm_add_ps( _mm_mul_ps( three, _mm_sub_ps( b, cc ) ), d ), a );
				const __m128 middle = _mm_add_ps( _mm_sub_ps( _mm_add_ps( _mm_sub_ps( _mm_mul_ps( two, a ), _mm_mul_ps( five, b ) ),
					_mm_mul_ps( four, cc ) ), d ), _mm_mul_ps( t, inner ) );
				const __m128 x = _mm_add_ps( b, _mm_mul_ps( _mm_mul_ps( half, t ), _mm_add_ps( _mm_sub_ps( cc, a ), _mm_mul_ps( t, middle ) ) ) );
				_mm_storeu_ps( output + n, x );
			}
#endif
			// Scalar twin and tail: every bracket is also a rounding point above.
			for ( ; n < end; n++, position += span.step )
			{
				const int i = (int)( position >> 32 );
				const float t = (float)( (uint32_t)position >> 8 ) * ( 1.0f / 16777216.0f );
				const float p0 = input[i], p1 = input[i + 1], p2 = input[i + 2], p3 = input[i + 3];
				output[n] = p1 + 0.5f * t * ( p2 - p0 + t * ( 2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * ( 3.0f * ( p1 - p2 ) + p3 - p0 ) ) );
			}
		}
		memset( output + plan.live, 0, ( SND_VOICE_BLOCK - plan.live ) * sizeof( float ) );
	}
}

static void SndVoice_Filter( sndVoiceState_t& state, float target, int channels, int live, float* scratch )
{
	const float before = state.filterCoef;
	const float delta = ( target - before ) / 441.0f;
	for ( int c = 0; c < channels; c++ )
	{
		float* row = scratch + c * SND_VOICE_BLOCK;
		if ( before == 1.0f && target == 1.0f )
		{
			state.z[c] = row[SND_VOICE_BLOCK - 1];
			continue;
		}
		float z = state.z[c];
		for ( int n = 0; n < live; n++ )
		{
			const float coef = n == SND_VOICE_BLOCK - 1 ? target : before + (float)( n + 1 ) * delta;
			z += coef * ( row[n] - z );
			row[n] = z;
		}
		state.z[c] = z;
	}
	state.filterCoef = target;
}

static float SndVoice_Measure( const float* scratch, int channels )
{
	float sum = 0.0f;
	for ( int n = 0; n < SND_VOICE_BLOCK * channels; n++ )
	{
		sum += scratch[n] * scratch[n];
	}
	return sum / (float)( SND_VOICE_BLOCK * channels );
}

float SndVoice_Render( sndVoiceState_t& state, const sndVoiceCmd_t& cmd, float* fetch, float* scratch )
{
	if ( state.inLoop && !SndVoice_HasLoop( cmd ) )
	{
		state.ended = true;
		state.active = false;
	}
	if ( !state.active || state.ended )
	{
		memset( scratch, 0, cmd.leadin.channels * SND_VOICE_BLOCK * sizeof( float ) );
		return 0.0f;
	}
	sndVoicePlan_t plan;
	SndVoice_Plan( state, cmd, plan );
	SndVoice_Read( state, cmd, plan, fetch );
	SndVoice_Cubic( plan, cmd.leadin.channels, fetch, scratch );
	SndVoice_Filter( state, SndVoice_Coef( cmd.filterCoef ), cmd.leadin.channels, plan.live, scratch );
	return SndVoice_Measure( scratch, cmd.leadin.channels );
}

// The same ramp operates on one dry channel or the send's mono sum.
static void SndVoice_Add( const float* left, const float* right, float* row, float before, float target, bool edgeIn, bool last )
{
	if ( last )
	{
		target = 0.0f;
	}
	if ( edgeIn )
	{
		before = 0.0f;
	}
	if ( before == 0.0f && target == 0.0f )
	{
		return;
	}
	const float delta = ( target - before ) / 441.0f;
	const int count = last ? SND_VOICE_EDGE_FRAMES : SND_VOICE_BLOCK;
	int n = 0;
#if SND_VOICE_SSE2
	// Edges keep their division form in the scalar twin below. They are rare;
	// steady blocks vectorize without changing the final frame's explicit target.
	if ( !last && !edgeIn )
	{
		const __m128 start = _mm_set1_ps( before ), change = _mm_set1_ps( delta );
		const __m128 finish = _mm_set1_ps( target );
		__m128 at = _mm_setr_ps( 1, 2, 3, 4 );
		for ( ; n + 4 < count; n += 4, at = _mm_add_ps( at, _mm_set1_ps( 4 ) ) )
		{
			const __m128 gain = before == target ? finish : _mm_add_ps( start, _mm_mul_ps( at, change ) );
			__m128 sample = _mm_loadu_ps( left + n );
			if ( right != NULL )
			{
				sample = _mm_mul_ps( _mm_set1_ps( 0.5f ), _mm_add_ps( sample, _mm_loadu_ps( right + n ) ) );
			}
			_mm_storeu_ps( row + n, _mm_add_ps( _mm_loadu_ps( row + n ), _mm_mul_ps( sample, gain ) ) );
		}
	}
#endif
	for ( ; n < count; n++ )
	{
		float gain;
#if SND_VOICE_EDGE_FRAMES > 0
		if ( last )
		{
			gain = before * ( (float)( SND_VOICE_EDGE_FRAMES - 1 - n ) / (float)SND_VOICE_EDGE_FRAMES );
		}
		else if ( edgeIn && n < SND_VOICE_EDGE_FRAMES )
		{
			gain = target * ( (float)( n + 1 ) / (float)SND_VOICE_EDGE_FRAMES );
		}
		else
#endif
		if ( edgeIn || before == target || n == SND_VOICE_BLOCK - 1 )
		{
			gain = target;
		}
		else
		{
			gain = before + (float)( n + 1 ) * delta;
		}
		const float sample = right == NULL ? left[n] : 0.5f * ( left[n] + right[n] );
		row[n] += sample * gain;
	}
}

void SndVoice_Mix( sndVoiceState_t& state, const sndVoiceCmd_t& cmd, const float* scratch, float factor,
	bool last, float* const* rows, int numOut, float* send )
{
	for ( int c = 0; c < cmd.leadin.channels; c++ )
	{
		for ( int o = 0; o < numOut; o++ )
		{
			float target = cmd.dryGain * factor * cmd.matrix[c][o];
			if ( !isfinite( target ) )
			{
				target = 0.0f;
			}
			SndVoice_Add( scratch + c * SND_VOICE_BLOCK, NULL, rows[o], state.dryGain[c][o], target, state.edgeIn, last );
			state.dryGain[c][o] = last ? 0.0f : target;
		}
	}
	float target = cmd.sendGain * factor;
	if ( !isfinite( target ) )
	{
		target = 0.0f;
	}
	if ( send != NULL )
	{
		SndVoice_Add( scratch, cmd.leadin.channels == 2 ? scratch + SND_VOICE_BLOCK : NULL,
			send, state.sendGain, target, state.edgeIn, last );
	}
	state.sendGain = last ? 0.0f : target;
	state.edgeIn = last;
	if ( last )
	{
		memset( state.dryGain, 0, sizeof( state.dryGain ) );
	}
}
