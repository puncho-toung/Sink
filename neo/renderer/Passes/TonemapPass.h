/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
* Copyright (C) 2022 Stephen Pridham (id Tech 4x integration)
* Copyright (C) 2022-2025 Robert Beckebans (id Tech 4x integration)
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

#ifndef RENDERER_PASSES_TONEMAPPASS
#define RENDERER_PASSES_TONEMAPPASS

#include "CommonPasses.h"

// ---- TRACK G: the bloom pass and what its chain hands the tonemap draw ( Passes/BloomPass.h ) ----
class BloomPass;
struct BloomMix;
// ---- END TRACK G ----

struct ToneMappingParameters
{
	float histogramLowPercentile = 0.8f;
	float histogramHighPercentile = 0.95f;
	float eyeAdaptationSpeedUp = 1.f;
	float eyeAdaptationSpeedDown = 0.5f;
	float minAdaptedLuminance = 0.02f;
	float maxAdaptedLuminance = 0.5f;
	float exposureBias = -0.5f;
	float whitePoint = 3.f;
	bool enableColorLUT = true;
};

class TonemapPass
{
public:
	struct CreateParameters
	{
		bool isTextureArray = false;
		uint32_t histogramBins = 256;
		uint32_t numConstantBufferVersions = 32;
		nvrhi::IBuffer* exposureBufferOverride = nullptr;
		idImage* colorLUT = nullptr;
	};

	TonemapPass();

	void Init( nvrhi::DeviceHandle deviceHandle, CommonRenderPasses* commonPasses, const CreateParameters& params, nvrhi::IFramebuffer* _sampleFramebuffer );

	void Render( nvrhi::ICommandList* commandList, const ToneMappingParameters& params, const viewDef_t* viewDef, nvrhi::ITexture* sourceTexture, nvrhi::FramebufferHandle _targetFb, const BloomMix* bloom = NULL );	// TRACK G: with a mix the bloom variant of the pixel shader draws; NULL is the draw without bloom

	void SimpleRender( nvrhi::ICommandList* commandList, const ToneMappingParameters& params, const viewDef_t* viewDef, nvrhi::ITexture* sourceTexture, nvrhi::FramebufferHandle _fbHandle );

	bool IsLoaded() const
	{
		return isLoaded;
	}

	// ---- TRACK G: the bloom pass whose chain this pass's bloom variant mixes in. The pointer is not owned:
	// the backend sets it before every tonemap block, NULL for a view without bloom ----
	void SetBloomPass( BloomPass* pass )
	{
		bloomPass = pass;
	}

	// empty the bloom variant's binding-set list and its hash: a set must never outlive the chain whose level
	// it names, nor keep a dead level alive; the backend calls this where it deletes the bloom pass on the cvar
	void ReleaseBloomSets();
	// ---- END TRACK G ----

private:
	void ResetExposure( nvrhi::ICommandList* commandList, float initialExposure );
	void ResetHistogram( nvrhi::ICommandList* commandList );
	void AddFrameToHistogram( nvrhi::ICommandList* commandList, const viewDef_t* viewDef, nvrhi::ITexture* sourceTexture );
	void ComputeExposure( nvrhi::ICommandList* commandList, const ToneMappingParameters& params );

	// ---- TRACK G: the bloom variant of the draw, made at the first draw that has a mix ( a run with
	// r_bloom 0 makes none ) ----
	void InitBloom( nvrhi::IFramebuffer* sampleFramebuffer );
	void RenderBloom( nvrhi::ICommandList* commandList, const ToneMappingParameters& params, const viewDef_t* viewDef, nvrhi::ITexture* sourceTexture, nvrhi::FramebufferHandle _targetFb, const BloomMix* bloom );
	// ---- END TRACK G ----

	bool                            isLoaded;
	idImage*                        colorLut;
	int                             colorLutSize;
	CommonRenderPasses*             commonPasses;
	nvrhi::DeviceHandle             device;
	nvrhi::ShaderHandle             histogramShader;
	nvrhi::ShaderHandle             exposureShader;
	nvrhi::ShaderHandle             tonemapShader;
	nvrhi::BufferHandle             toneMappingCb;
	nvrhi::BufferHandle             histogramBuffer;
	nvrhi::BufferHandle             exposureBuffer;
	nvrhi::BindingLayoutHandle      renderBindingLayout;
	nvrhi::BindingLayoutHandle      histogramBindingLayout;
	nvrhi::ComputePipelineHandle    histogramPipeline;
	nvrhi::ComputePipelineHandle    exposurePipeline;
	nvrhi::GraphicsPipelineHandle   renderPipeline;
	nvrhi::BindingSetHandle         exposureBindingSet;
	nvrhi::BindingLayoutHandle	    exposureBindingLayout;
	idList<nvrhi::BindingSetHandle> histogramBindingSets;
	idHashIndex                     histogramBindingHash;
	idList<nvrhi::BindingSetHandle> renderBindingSets;
	idHashIndex                     renderBindingHash;
	bool                            pcEnabledHistogram = false;	// true if push constants are used for Histogram
	bool                            pcEnabledExposure = false;	// true if push constants are used for Exposure
	bool                            pcEnabledTonemap = false;	// true if push constants are used for Tonemap
	// ---- TRACK G: the bloom variant: the pass whose chain is mixed in ( not owned, NULL until the backend
	// gives one ), its shader, layout, pipeline and 128-byte constants, and its binding sets looked up by the
	// source texture and the level texture together ----
	BloomPass*                      bloomPass;
	nvrhi::ShaderHandle             bloomShader;
	nvrhi::BindingLayoutHandle      bloomBindingLayout;
	nvrhi::GraphicsPipelineHandle   bloomRenderPipeline;
	nvrhi::BufferHandle             toneMappingBloomCb;         // the 128-byte constants, when push constants are off
	idList<nvrhi::BindingSetHandle> bloomBindingSets;
	idHashIndex                     bloomBindingHash;
	int                             bloomChainSerial;           // the chain the set list was filled under; a new one empties the list
	bool                            pcEnabledBloom = false;     // true if push constants are used for the bloom variant
	uint32_t                        numConstantBufferVersions;  // _params' count, kept for the bloom variant's buffer made later
	// ---- END TRACK G ----
};

#endif