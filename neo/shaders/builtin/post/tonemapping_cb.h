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

#ifndef DEPTH_CB_H
#define DEPTH_CB_H

struct ToneMappingConstants
{
	uint2 viewOrigin;
	uint2 viewSize;

	float logLuminanceScale;
	float logLuminanceBias;
	float histogramLowPercentile;
	float histogramHighPercentile;

	float eyeAdaptationSpeedUp;
	float eyeAdaptationSpeedDown;
	float minAdaptedLuminance;
	float maxAdaptedLuminance;

	float frameTime;
	float exposureScale;
	float whitePointInvSquared;
	uint sourceSlice;

	float2 colorLUTTextureSize;
	float2 colorLUTTextureSizeInv;
	// ---- TRACK G: the bloom mix, sent by the tonemap pass's bloom variant ( its C++ twin appends these three
	// to ToneMappingConstants as ToneMappingBloomConstants, 128 bytes ) ----
#if defined( BLOOM )
	float4 bloomMix;		// x the frame's weight, y the bloom's, z the level read, w 1: one bilinear tap
	float4 bloomRect;		// xy the view rectangle's origin in the frame ( pixels ); zw the read level's rectangle size ( texels )
	float4 bloomScale;		// x 0.5 / 2^level: screen pixels ( relative to the view's origin ) to the read level's texels; y 0; zw 1 / the read level's TEXTURE size
#endif
	// ---- END TRACK G ----
};

#endif // DEPTH_CB_H