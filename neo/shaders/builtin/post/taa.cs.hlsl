/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#pragma pack_matrix(row_major)

#include "vulkan.hlsli"

//#include <donut/shaders/taa_cb.h>

struct TemporalAntiAliasingConstants
{
	float4x4 reprojectionMatrix;

	float2 inputViewOrigin;
	float2 inputViewSize;

	float2 outputViewOrigin;
	float2 outputViewSize;

	float2 inputPixelOffset;
	float2 outputTextureSizeInv;

	float2 inputOverOutputViewSize;
	float2 outputOverInputViewSize;

	float clampingFactor;
	float newFrameWeight;
	float pqC;
	float invPqC;

	uint stencilMask;

	// ---- TAA REPAIR ----
	float motionWeightAdd;		// added to the new frame's weight where the history is read half way between two of its pixels; 0 = as before
	uint repairFlags;			// bit 0: the clamp's box takes in this pixel's own colour; bit 1: five history taps for nine
	float sharpen;				// how far what is SHOWN is pushed from the mean of its nine; 0 = none
	// ---- END TAA REPAIR ----
};

#if USE_PUSH_CONSTANTS
VK_PUSH_CONSTANT ConstantBuffer<TemporalAntiAliasingConstants> g_TemporalAA :
register( b0 );
#else
cbuffer c_TemporalAA :
register( b0 )
{
	TemporalAntiAliasingConstants g_TemporalAA;
};
#endif

#ifndef SAMPLE_COUNT
	#define SAMPLE_COUNT 1
#endif

// TAA REPAIR: the permutation with the sharpening of what is shown ( the pass uses it while r_taaSharpen is above 0 )
#ifndef USE_SHARPEN
	#define USE_SHARPEN 0
#endif

#if SAMPLE_COUNT == 1
Texture2D<float4> t_UnfilteredRT :
register( t0 );
Texture2D<float2> t_MotionVectors :
register( t1 );
#else
Texture2DMS<float4> t_UnfilteredRT :
register( t0 );
Texture2DMS<float2> t_MotionVectors :
register( t1 );
#endif
Texture2D<float4> t_FeedbackInput :
register( t2 );
SamplerState s_Sampler :
register( s0 );
VK_IMAGE_FORMAT( "rgba16f" ) RWTexture2D<float4> u_ColorOutput :
register( u0 );
VK_IMAGE_FORMAT( "rgba16f" ) RWTexture2D<float4> u_FeedbackOutput :
register( u1 );

#define GROUP_X 16
#define GROUP_Y 16
#define BUFFER_X (GROUP_X + 3)
#define BUFFER_Y (GROUP_Y + 3)
#define RENAMED_GROUP_Y ((GROUP_X * GROUP_Y) / BUFFER_X)

groupshared float4 s_ColorsAndLengths[BUFFER_Y][BUFFER_X];
groupshared float2 s_MotionVectors[BUFFER_Y][BUFFER_X];

static const float pq_m1 = 0.1593017578125;
static const float pq_m2 = 78.84375;
static const float pq_c1 = 0.8359375;
static const float pq_c2 = 18.8515625;
static const float pq_c3 = 18.6875;

float3 PQDecode( float3 image )
{
	float3 Np = pow( max( image, 0.0 ), 1.0 / pq_m2 );
	float3 L = Np - pq_c1;
	L = L / ( pq_c2 - pq_c3 * Np );
	L = pow( max( L, 0.0 ), 1.0 / pq_m1 );

	return L * g_TemporalAA.pqC; // returns cd/m^2
}

float3 PQEncode( float3 image )
{
	float3 L = image * g_TemporalAA.invPqC;
	float3 Lm = pow( max( L, 0.0 ), pq_m1 );
	float3 N = ( pq_c1 + pq_c2 * Lm ) / ( 1.0 + pq_c3 * Lm );
	image = pow( N, pq_m2 );

	return saturate( image );
}

float3 BicubicSampleCatmullRom( Texture2D tex, SamplerState samp, float2 samplePos, float2 invTextureSize )
{
	float2 tc = floor( samplePos - 0.5 ) + 0.5;
	float2 f = saturate( samplePos - tc );
	float2 f2 = f * f;
	float2 f3 = f2 * f;

	float2 w0 = f2 - 0.5 * ( f3 + f );
	float2 w1 = 1.5 * f3 - 2.5 * f2 + 1;
	float2 w3 = 0.5 * ( f3 - f2 );
	float2 w2 = 1 - w0 - w1 - w3;

	float2 w12 = w1 + w2;

	float2 tc0 = ( tc - 1 ) * invTextureSize;
	float2 tc12 = ( tc + w2 / w12 ) * invTextureSize;
	float2 tc3 = ( tc + 2 ) * invTextureSize;

	float3 result =
		tex.SampleLevel( samp, float2( tc0.x,  tc0.y ), 0 ).rgb  * ( w0.x  * w0.y ) +
		tex.SampleLevel( samp, float2( tc0.x,  tc12.y ), 0 ).rgb * ( w0.x  * w12.y ) +
		tex.SampleLevel( samp, float2( tc0.x,  tc3.y ), 0 ).rgb  * ( w0.x  * w3.y ) +
		tex.SampleLevel( samp, float2( tc12.x, tc0.y ), 0 ).rgb  * ( w12.x * w0.y ) +
		tex.SampleLevel( samp, float2( tc12.x, tc12.y ), 0 ).rgb * ( w12.x * w12.y ) +
		tex.SampleLevel( samp, float2( tc12.x, tc3.y ), 0 ).rgb  * ( w12.x * w3.y ) +
		tex.SampleLevel( samp, float2( tc3.x,  tc0.y ), 0 ).rgb  * ( w3.x  * w0.y ) +
		tex.SampleLevel( samp, float2( tc3.x,  tc12.y ), 0 ).rgb * ( w3.x  * w12.y ) +
		tex.SampleLevel( samp, float2( tc3.x,  tc3.y ), 0 ).rgb  * ( w3.x  * w3.y );

	return max( 0, result );
}

// ---- TAA REPAIR: the same read without its four corner taps. A corner's weight is the product of two of the
// filter's negative side lobes, at most 0.0055 of the sum each; the five taps left are divided by their own sum ----
float3 BicubicSampleCatmullRom5( Texture2D tex, SamplerState samp, float2 samplePos, float2 invTextureSize )
{
	float2 tc = floor( samplePos - 0.5 ) + 0.5;
	float2 f = saturate( samplePos - tc );
	float2 f2 = f * f;
	float2 f3 = f2 * f;

	float2 w0 = f2 - 0.5 * ( f3 + f );
	float2 w1 = 1.5 * f3 - 2.5 * f2 + 1;
	float2 w3 = 0.5 * ( f3 - f2 );
	float2 w2 = 1 - w0 - w1 - w3;

	float2 w12 = w1 + w2;

	float2 tc0 = ( tc - 1 ) * invTextureSize;
	float2 tc12 = ( tc + w2 / w12 ) * invTextureSize;
	float2 tc3 = ( tc + 2 ) * invTextureSize;

	float wUp = w12.x * w0.y;
	float wLeft = w0.x * w12.y;
	float wMid = w12.x * w12.y;
	float wRight = w3.x * w12.y;
	float wDown = w12.x * w3.y;

	float3 result =
		tex.SampleLevel( samp, float2( tc12.x, tc0.y ), 0 ).rgb  * wUp +
		tex.SampleLevel( samp, float2( tc0.x,  tc12.y ), 0 ).rgb * wLeft +
		tex.SampleLevel( samp, float2( tc12.x, tc12.y ), 0 ).rgb * wMid +
		tex.SampleLevel( samp, float2( tc3.x,  tc12.y ), 0 ).rgb * wRight +
		tex.SampleLevel( samp, float2( tc12.x, tc3.y ), 0 ).rgb  * wDown;

	return max( 0, result / ( wUp + wLeft + wMid + wRight + wDown ) );
}
// ---- END TAA REPAIR ----

void Preload( int2 sharedID, int2 globalID )
{
#if SAMPLE_COUNT == 1
	float3 color = PQEncode( t_UnfilteredRT[globalID].rgb );
	//float3 color = ( t_UnfilteredRT[globalID].rgb );
	float2 motion = t_MotionVectors[globalID].rg;
	float motionLength = dot( motion, motion );
#else
	float3 color = 0;
	float2 motion = 0;
	float motionLength = -1;

	// Resolve MSAA color using average filter, motion vectors using max filter

	[unroll]
	for( int nSample = 0; nSample < SAMPLE_COUNT; nSample++ )
	{
		float3 sampleColor = PQEncode( t_UnfilteredRT.Load( globalID, nSample ).rgb );
		float2 sampleMotion = t_MotionVectors.Load( globalID, nSample ).rg;
		float sampleMotionLength = dot( sampleMotion, sampleMotion );

		color += sampleColor;

		if( sampleMotionLength > motionLength )
		{
			motion = sampleMotion;
			motionLength = sampleMotionLength;
		}
	}

	color /= float( SAMPLE_COUNT );
#endif

	s_ColorsAndLengths[sharedID.y][sharedID.x] = float4( color.rgb, motionLength );
	s_MotionVectors[sharedID.y][sharedID.x] = motion;
}

float2 OutputToInput( int2 pixelPosRelativeToOrigin )
{
	return ( float2( pixelPosRelativeToOrigin ) + 0.5 ) * g_TemporalAA.inputOverOutputViewSize
		   - 0.5 + g_TemporalAA.inputViewOrigin + g_TemporalAA.inputPixelOffset;
}

[numthreads( GROUP_X, GROUP_Y, 1 )]
void main(
	in int2 i_groupIdx : SV_GroupID,
	in int2 i_threadIdx : SV_GroupThreadID,
	in int2 i_globalIdx : SV_DispatchThreadID
)
{
	// Rename the 16x16 group into a 19x13 group + 9 idle threads in the end

	int2 newID;
	float linearID = i_threadIdx.y * GROUP_X + i_threadIdx.x;
	linearID = ( linearID + 0.5 ) / float( BUFFER_X );
	newID.y = int( floor( linearID ) );
	newID.x = int( floor( frac( linearID ) * BUFFER_X ) );

	int2 groupBase = int2( OutputToInput( i_groupIdx * int2( GROUP_X, GROUP_Y ) ) - 1 );

	// Preload the colors and motion vectors into shared memory

	if( newID.y < RENAMED_GROUP_Y )
	{
		Preload( newID, groupBase + newID );
	}

	newID.y += RENAMED_GROUP_Y;

	if( newID.y < BUFFER_Y )
	{
		Preload( newID, groupBase + newID );
	}

	GroupMemoryBarrierWithGroupSync();

	// Calculate the color distribution and find the longest MV in the neighbourhood

	int2 outputPixelPosition = i_globalIdx + int2( g_TemporalAA.outputViewOrigin );
	float2 inputPos = OutputToInput( i_globalIdx );
	int2 inputPosInt = int2( round( inputPos ) );
	int2 inputPosShared = inputPosInt - groupBase - 1;

	float3 colorMoment1 = 0;
	float3 colorMoment2 = 0;
	float longestMVLength = -1;
	int2 longestMVPos = 0;
	float3 thisPixelColor = 0;

	[unroll]
	for( int dy = 0; dy <= 2; dy++ )
	{
		[unroll]
		for( int dx = 0; dx <= 2; dx++ )
		{
			int2 pos = inputPosShared + int2( dx, dy );

			float4 colorAndLength = s_ColorsAndLengths[pos.y][pos.x];
			float3 color = colorAndLength.rgb;
			float motionLength = colorAndLength.a;

			if( dx == 1 && dy == 1 )
			{
				thisPixelColor = color;
			}

			colorMoment1 += color;
			colorMoment2 += color * color;

			if( motionLength > longestMVLength )
			{
				longestMVPos = pos;
				longestMVLength = motionLength;
			}
		}
	}

	float2 longestMV = s_MotionVectors[longestMVPos.y][longestMVPos.x];

	colorMoment1 /= 9.0;
	colorMoment2 /= 9.0;
	float3 colorVariance = colorMoment2 - colorMoment1 * colorMoment1;
	float3 colorSigma = sqrt( max( 0, colorVariance ) ) * g_TemporalAA.clampingFactor;
	float3 colorMin = colorMoment1 - colorSigma;
	float3 colorMax = colorMoment1 + colorSigma;

	// ---- TAA REPAIR: the box takes in this pixel's own colour. A pixel that stands more than one deviation from
	// the mean of its nine ( a line or a light one pixel wide, a texel of a fine texture ) lies outside the box of
	// before, and its history was pulled to the box's edge every frame: it kept 0.9 of a deviation and a tenth of
	// its own. A history between the edge and this frame's colour now stays. The result is never further from
	// this frame's colour than before, channel by channel ----
	if( ( g_TemporalAA.repairFlags & 1u ) != 0u )
	{
		colorMin = min( colorMin, thisPixelColor );
		colorMax = max( colorMax, thisPixelColor );
	}
	// ---- END TAA REPAIR ----

	// Sample the previous frame using the longest MV

	longestMV *= g_TemporalAA.outputOverInputViewSize;
	float2 sourcePos = float2( outputPixelPosition.xy ) + longestMV + 0.5;

	float3 resultPQ;
	if( g_TemporalAA.newFrameWeight < 1.0 && all( sourcePos.xy > g_TemporalAA.outputViewOrigin )
			&& all( sourcePos.xy < g_TemporalAA.outputViewOrigin + g_TemporalAA.outputViewSize ) )
	{
#if USE_CATMULL_ROM_FILTER
		// ---- TAA REPAIR: five taps for nine ( bit 1 ) ----
		float3 history;
		if( ( g_TemporalAA.repairFlags & 2u ) != 0u )
		{
			history = BicubicSampleCatmullRom5( t_FeedbackInput, s_Sampler, sourcePos, g_TemporalAA.outputTextureSizeInv );
		}
		else
		{
			history = BicubicSampleCatmullRom( t_FeedbackInput, s_Sampler, sourcePos, g_TemporalAA.outputTextureSizeInv );
		}
		// ---- END TAA REPAIR ----
#else
		float3 history = t_FeedbackInput.SampleLevel( s_Sampler, sourcePos * g_TemporalAA.outputTextureSizeInv, 0 ).rgb;
#endif

		// Clamp the old color to the new color distribution

		float3 historyClamped = history;
		if( g_TemporalAA.clampingFactor >= 0 )
		{
			historyClamped = min( colorMax, max( colorMin, history ) );
		}

		// Blend the old color with the new color and store output

		float motionWeight = smoothstep( 0, 1, length( longestMV ) );
		float2 distanceToLowResPixel = inputPos - float2( inputPosInt );
		float upscalingFactor = g_TemporalAA.outputOverInputViewSize.x;
		float sampleWeight = saturate( 1.0 - upscalingFactor * dot( distanceToLowResPixel, distanceToLowResPixel ) );
		// ---- TAA REPAIR: a history read BETWEEN its pixels is softened by the filter, and nine tenths of the pixel
		// are that read, frame after frame. The loss is none where the read falls on a pixel ( a still camera, a
		// motion of whole pixels ) and greatest half way between two; the new frame's weight rises by that
		// measure ( the larger of the two axes ) times motionWeightAdd. 0 = the weight of before, bit for bit ----
		float2 readPlace = frac( longestMV );
		float2 readLoss = 4.0 * readPlace * ( 1.0 - readPlace );
		float blendWeight = saturate( max( motionWeight, sampleWeight ) * g_TemporalAA.newFrameWeight + max( readLoss.x, readLoss.y ) * g_TemporalAA.motionWeightAdd );
		// ---- END TAA REPAIR ----

		resultPQ = lerp( historyClamped, thisPixelColor, blendWeight );
	}
	else
	{
		resultPQ = thisPixelColor;
	}

	float3 result = PQDecode( resultPQ );
	//float3 result = resultPQ;

	// ---- TAA REPAIR: a slight sharpening of what is SHOWN, at the owner's word. Where the pixel has a history, the
	// resolved colour is pushed away from the mean of this frame's nine by the share `sharpen`, and held within
	// what the nine and the resolved colour span, so no edge gets a rim brighter or darker than the frame itself
	// has there. The HISTORY keeps the colour as resolved ( resultPQ below ): a sharpening that went back into the
	// history would be sharpened again every frame.
	// It is a permutation of its own ( USE_SHARPEN ), which the pass runs only while r_taaSharpen is above 0. Compiled
	// into the one shader, even behind a branch that was not taken, these lines changed the rounding of the lines
	// above in a few pixels of a frame ( one level, at most 24 pixels of two million ): without them in the
	// program, r_taaSharpen 0 is the resolve of before bit for bit ----
#if USE_SHARPEN
	if( g_TemporalAA.newFrameWeight < 1.0 && all( sourcePos.xy > g_TemporalAA.outputViewOrigin )
			&& all( sourcePos.xy < g_TemporalAA.outputViewOrigin + g_TemporalAA.outputViewSize ) )
	{
		float3 nineMean = 0;
		float3 nineMin = 1e30;
		float3 nineMax = -1e30;

		[unroll]
		for( int sy = 0; sy <= 2; sy++ )
		{
			[unroll]
			for( int sx = 0; sx <= 2; sx++ )
			{
				float3 nineColor = s_ColorsAndLengths[inputPosShared.y + sy][inputPosShared.x + sx].rgb;
				nineMean += nineColor;
				nineMin = min( nineMin, nineColor );
				nineMax = max( nineMax, nineColor );
			}
		}
		nineMean /= 9.0;

		float3 sharpened = resultPQ + ( resultPQ - nineMean ) * g_TemporalAA.sharpen;
		result = PQDecode( clamp( sharpened, min( nineMin, resultPQ ), max( nineMax, resultPQ ) ) );
	}
#endif
	// ---- END TAA REPAIR ----

#if 0
	// Show motion vectors
	result = float3( longestMV, 0.0 );
	resultPQ = PQEncode( result );

#endif

	u_ColorOutput[outputPixelPosition] = float4( result, 1.0 );
	u_FeedbackOutput[outputPixelPosition] = float4( resultPQ, 0.0 );
}
