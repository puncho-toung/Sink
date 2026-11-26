/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2014 Robert Beckebans

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
#ifndef RENDERER_PASSES_BLOOMPASS_H_
#define RENDERER_PASSES_BLOOMPASS_H_

#include "BloomPass_cb.h"		// the bloom draws' constants ( BloomConstants )

/*
================================

BloomPass ( track G of the renderer upgrade )

The bloom of the main view: the HDR picture the tonemap pass is about to read ( the frame ) is reduced into a chain
of half-size levels by full-screen pixel-shader draws: level 0 half the view's size and each further level half the
one before. Only what a frame pixel has above a brightness threshold enters level 0 ( the glow ), and the view
weapon's pixels enter nothing ( WP-G4 ). The levels are enlarged back with additive blending and added together,
and a variant of the tonemap shader adds level 0 to the frame before the exposure. No history, nothing read back
on the CPU ( but by the console's bloomProbe ): every frame redraws the whole chain from the frame's own pixels,
and nothing is kept between frames.

Level i's rectangle starts at ( 0, 0 ) of its texture and has the size ( w[i], h[i] ): the view's size halved i + 1
times with halves rounded up. The level textures are sized by the frame texture the same way, so a view rectangle
of any size up to the frame's fits its levels; texels of a level outside its rectangle hold stale data and are
read only at weight 0 ( a tap on the rectangle's last texel centre ); they are cleared once when the chain is
made, so that what is stale is at least finite.

The pass exists only while r_bloom is on: the backend makes and deletes it in its TRACK G blocks of GL_StartFrame
and ClearCaches.

================================
*/

#define BLOOM_MAX_LEVELS 9

extern idCVar r_bloom;

// what the tonemap draw needs to mix the chain in; valid until the next BloomPass::Render
struct BloomMix
{
	nvrhi::ITexture*	texture;		// the level the mix reads ( level 0; under r_bloomDebug 2..9 that level )
	int					chainSerial;	// counts the chains this pass and every earlier BloomPass made ( a static counter ): a reader that keeps binding sets drops them when it changes
	idVec4				mix;			// x the frame's weight, y the bloom's, z the level read, w 1: one bilinear tap
	idVec4				rect;			// x, y the view rectangle's origin in the frame ( pixels ); z, w the size of the read level's rectangle ( texels )
	idVec4				scale;			// x 0.5 / 2^level: screen pixels ( relative to the view's origin ) to the read level's texels; y 0; z, w 1 / the read level's TEXTURE size
};

class BloomPass
{
public:
	BloomPass( nvrhi::IDevice* device, CommonRenderPasses* commonPasses );
	~BloomPass();

	// true where this view gets bloom: r_bloom, the main view ( viewDef_t::isMainView ), r_renderMode == RENDERMODE_DOOM
	static bool			WantedForView( const viewDef_t* viewDef );

	// builds the chain from the view's rectangle of sourceTexture ( the picture the tonemap draw reads ); exposureBuffer
	// is the tonemap pass's one-value buffer. Returns the mix, or NULL when nothing was built ( the caller then draws
	// the tonemap without bloom ).
	const BloomMix*		Render( nvrhi::ICommandList* commandList, const viewDef_t* viewDef, nvrhi::ITexture* sourceTexture, nvrhi::IBuffer* exposureBuffer );

private:
	void				ReleaseChain();
	bool				CheckChain( int frameWidth, int frameHeight );
	bool				CheckFirstSet( nvrhi::ITexture* sourceTexture, nvrhi::IBuffer* exposureBuffer );
	bool				CheckFirstDepthSet( nvrhi::ITexture* sourceTexture, nvrhi::IBuffer* exposureBuffer, nvrhi::ITexture* depthTexture );
	void				DrawLevel( nvrhi::ICommandList* commandList, nvrhi::IGraphicsPipeline* pipeline, nvrhi::IBindingSet* bindingSet,
								   nvrhi::IFramebuffer* framebuffer, int width, int height, const BloomConstants& constants );
	void				PokeFrame( nvrhi::ICommandList* commandList, nvrhi::ITexture* frame, int centreX, int centreY, int debug );
	bool				ProbeCopy( nvrhi::ICommandList* commandList, nvrhi::ITexture* frame, int numLevels );
	void				ProbeStageUp( nvrhi::ICommandList* commandList );
	void				ProbeRead();

	nvrhi::DeviceHandle				m_Device;
	CommonRenderPasses*				commonRenderPasses;

	// the chain's format, chosen once: R11G11B10_FLOAT where the device can draw into it, blend into it and filter
	// it, else RGBA16_FLOAT. The probe prints its name
	nvrhi::Format					m_ChainFormat;
	bool							m_Dead;			// a shader handle came back null from some cause: Render returns NULL for good ( a missing blob or permutation stops the engine as for every builtin shader, so this guards any other cause )

	nvrhi::ShaderHandle				m_VS;			// the tonemapping program's vertex shader: four vertices, no inputs
	nvrhi::ShaderHandle				m_DownFirstPS;	// builtin/post/bloom_down with FIRST 1: the first pass, the footprint's pixels thresholded one by one
	nvrhi::ShaderHandle				m_DownFirstDepthPS;	// builtin/post/bloom_down with FIRST 2: the first pass reading the frame's depth too ( the distance tail, the view weapon )
	nvrhi::ShaderHandle				m_DownPS;		// builtin/post/bloom_down with FIRST 0
	nvrhi::ShaderHandle				m_UpPS;			// builtin/post/bloom_up

	nvrhi::BindingLayoutHandle		m_FirstLayout;	// push constants, the frame at t0, the exposure buffer at t1, a sampler
	nvrhi::BindingLayoutHandle		m_FirstDepthLayout;	// m_FirstLayout and the frame's depth at t2
	nvrhi::BindingLayoutHandle		m_DownUpLayout;	// push constants, one level at t0, a sampler: the down and the up passes

	// the chain: made by CheckChain for the frame texture's size, released when that size changes or the pass goes
	int								m_ChainWidth;	// the frame size the chain was made for ( 0 until the first Render )
	int								m_ChainHeight;
	bool							m_ChainFresh;	// a chain was just made: Render clears every level once, before any draw touches it
	int								m_TexW[ BLOOM_MAX_LEVELS ];		// the level textures' sizes
	int								m_TexH[ BLOOM_MAX_LEVELS ];
	nvrhi::TextureHandle			m_Levels[ BLOOM_MAX_LEVELS ];
	nvrhi::FramebufferHandle		m_Frames[ BLOOM_MAX_LEVELS ];
	nvrhi::GraphicsPipelineHandle	m_DownFirstPipeline;	// the first pass ( m_FirstLayout, m_FirstSet ) into m_FirstFrame's two targets
	nvrhi::GraphicsPipelineHandle	m_DownPipeline;
	nvrhi::GraphicsPipelineHandle	m_UpPipeline;	// the only one that blends: ONE, ONE
	nvrhi::BindingSetHandle			m_DownSets[ BLOOM_MAX_LEVELS ];	// the down pass into level i, reading level i - 1 ( i >= 2: level 1 reads m_Wide )
	nvrhi::BindingSetHandle			m_UpSets[ BLOOM_MAX_LEVELS ];		// the up pass into level i, reading level i + 1
	nvrhi::BindingSetHandle			m_FirstSet;		// made again whenever the frame texture or the exposure buffer changes
	nvrhi::ITexture*				m_FirstSource;
	nvrhi::IBuffer*					m_FirstExposure;

	// The first pass writes two targets, level 0 and the wide levels' source, which level 1 is reduced from. Made
	// with the chain and part of it; the pipeline and set of the permutation that reads the frame's depth may be
	// missing: the tail is then the same at every distance and the view weapon glows
	nvrhi::TextureHandle			m_Wide;			// level 0's size and format: the wide levels' source ( level 1 is reduced from it )
	nvrhi::FramebufferHandle		m_FirstFrame;	// level 0 and m_Wide
	nvrhi::GraphicsPipelineHandle	m_DownFirstDepthPipeline;
	nvrhi::BindingSetHandle			m_DownWideSet;	// the down pass into level 1, reading m_Wide
	nvrhi::BindingSetHandle			m_FirstDepthSet;	// made again whenever the frame texture, the exposure buffer or the depth image changes
	nvrhi::ITexture*				m_FirstDepthSource;
	nvrhi::IBuffer*					m_FirstDepthExposure;
	nvrhi::ITexture*				m_FirstDepthImage;

	BloomMix						m_Mix;			// what Render returns

	// the debug pokes ( r_bloomDebug 11, 12, 13 ): one 1 x 1 texture of the frame's format each, made on first use
	nvrhi::TextureHandle			m_Pokes[3];	// r_bloomDebug 11, 12, 13: a NaN, a +Inf, 65504
	bool							m_PokeWarned;	// the frame was not RGBA16_FLOAT: warned once

	// the probe ( the console command bloomProbe ): each level copied whole after the down passes, level 0 again
	// after the up passes, the frame too, read back and printed
	nvrhi::StagingTextureHandle		m_ProbeStagings[ BLOOM_MAX_LEVELS ];
	nvrhi::StagingTextureHandle		m_ProbeUpStaging;
	nvrhi::StagingTextureHandle		m_ProbeFrameStaging;	// the whole frame, for the view rectangle's mean; only ever made when the frame is RGBA16_FLOAT
	int								m_ProbeWait;	// bloom views until the copies are read; 0 = no probe under way
	int								m_ProbeView[4];	// the probe's view rectangle and level count, captured with the copies
	int								m_ProbeLevels;
	float							m_ProbeTail;
	float							m_ProbeW0;
	float							m_ProbeExposureScale;
	float							m_ProbeKnee;
	float							m_ProbeCeiling;
	int								m_ProbeDebug;
	float							m_ProbeGlowGain;
	float							m_ProbeThreshold;
	float							m_ProbeThresholdKnee;
	int								m_ProbeTailDistance;	// 1: the probed view was drawn with the distance tail
	int								m_ProbeWeaponOut;		// 1: the probed view's first pass left the view weapon out
	float							m_ProbeTailNear;
	float							m_ProbeTailFar;
	float							m_ProbeTailFarShare;
	float							m_ProbeMixFrame;
	float							m_ProbeMixBloom;
	int								m_ProbeFrame[2];	// the frame texture's size
	int								m_ProbeLevelW[ BLOOM_MAX_LEVELS ];	// the probe's level rectangles
	int								m_ProbeLevelH[ BLOOM_MAX_LEVELS ];
};

#endif // RENDERER_PASSES_BLOOMPASS_H_
