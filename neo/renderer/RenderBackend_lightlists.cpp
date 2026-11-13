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

#include "precompiled.h"
#pragma hdrstop

#include "RenderCommon.h"

#include <sys/DeviceManager.h>
extern DeviceManager* deviceManager;

/*
==========================================================================================

THE LIGHT LISTS' BACKEND ( F1 )

The six table buffers, the upload of a view's tables, and the shadow rows ( C2 ). The backend
reads the view's lightListView_t and records, which are frame memory the frontend finished
before the view was queued; it never reads a light def, and no cvar the legacy interaction pass
and the atlas pass do not read as well ( the atlas switch and size, r_skipShadows, the cascade
count and the single side ).

==========================================================================================
*/

// the rows of the view being drawn: the backend's own memory ( the frontend allocates none, the backend never
// calls R_FrameAlloc ). One backend, one view at a time.
static shadowRow_t	lightListRows[ MAX_RECORD_LIGHTS * 6 ];

static nvrhi::BufferHandle R_LightListCreateBuffer( const char* name, int stride, int count )
{
	nvrhi::BufferDesc desc;
	desc.byteSize = ( uint64_t )stride * count;
	desc.structStride = stride;											// a structured buffer: bound as an SRV
	desc.debugName = name;
	desc.initialState = nvrhi::ResourceStates::ShaderResource;
	desc.keepInitialState = true;
	return deviceManager->GetDevice()->createBuffer( desc );
}

/*
====================
idRenderBackend::LightListCreateBuffers

The six table buffers ( D69, C22 ). Called at the start of every frame and before every upload; it creates what is
missing, which is everything after ClearCaches. Five sizes are fixed by the caps. The tile bits depend on the
render size and the tile size: the buffer is made for the render size at 16-pixel tiles and the record cap, and
made again, larger, when a view needs more words than it holds ( tileBitWords ).
====================
*/
void idRenderBackend::LightListCreateBuffers( int tileBitWords )
{
	if( !recordLightsBuffer )
	{
		recordLightsBuffer = R_LightListCreateBuffer( "LightList Records", sizeof( recordLight_t ), MAX_RECORD_LIGHTS );
	}
	if( !shadowRowsBuffer )
	{
		shadowRowsBuffer = R_LightListCreateBuffer( "LightList Shadow Rows", sizeof( shadowRow_t ), MAX_RECORD_LIGHTS * 6 );
	}
	if( !lightPoolBuffer )
	{
		lightPoolBuffer = R_LightListCreateBuffer( "LightList Pool", sizeof( uint32 ), LL_MAX_POOL );
	}
	if( !zBinsBuffer )
	{
		zBinsBuffer = R_LightListCreateBuffer( "LightList Z-Bins", LL_BIN_STRIDE * sizeof( uint32 ), LL_MAX_BINS );
	}
	if( !vanishedPrevBuffer )
	{
		vanishedPrevBuffer = R_LightListCreateBuffer( "LightList Vanished", sizeof( int ), MAX_RECORD_LIGHTS );
	}
	if( !tileBitsBuffer )
	{
		lightListTileBitsWords = 0;
	}
	if( !tileBitsBuffer || tileBitWords > lightListTileBitsWords )
	{
		const int tilesX = ( renderSystem->GetWidth() + 15 ) / 16;
		const int tilesY = ( renderSystem->GetHeight() + 15 ) / 16;
		// at least twice the old size: the old buffer stays alive in the binding cache until ClearCaches
		lightListTileBitsWords = Max( Max( tilesX * tilesY * LL_MAX_WORDS, tileBitWords ), Max( 2 * lightListTileBitsWords, 1 ) );
		tileBitsBuffer = R_LightListCreateBuffer( "LightList Tile Bits", sizeof( uint32 ), lightListTileBitsWords );
	}
}

/*
====================
idRenderBackend::LightListReleaseBuffers

ClearCaches: the binding sets that named the buffers are gone with the binding cache.
====================
*/
void idRenderBackend::LightListReleaseBuffers()
{
	recordLightsBuffer.Reset();
	shadowRowsBuffer.Reset();
	lightPoolBuffer.Reset();
	zBinsBuffer.Reset();
	tileBitsBuffer.Reset();
	vanishedPrevBuffer.Reset();
	lightListTileBitsWords = 0;
	lightListHeatmapPipeline = nullptr;
	lightListHeatmapTarget = nullptr;
	lightListHeatmapColor = nullptr;
	lightListHeatmapLayout = nullptr;
	lightListHeatmapVS = nullptr;
	lightListHeatmapPS = nullptr;
	// F2: the difference view
	lightDiffImageA = nullptr;
	lightDiffImageB = nullptr;
	lightDiffReduced = nullptr;
	lightDiffReducedTarget = nullptr;
	lightDiffShowTarget = nullptr;
	lightDiffShowColor = nullptr;
	for( int i = 0; i < 3; i++ )
	{
		lightDiffStaging[i] = nullptr;
		lightDiffStagingFrame[i] = -1;
	}
	lightDiffHasReadout = false;
	lightDiffVS = nullptr;
	lightDiffPS = nullptr;
	lightDiffLayout = nullptr;
	lightDiffShowPipeline = nullptr;
	lightDiffReducePipeline = nullptr;
}

/*
====================
idRenderBackend::R_BuildShadowRows

After ShadowAtlasPass, in a view that has records: the shadow rows ( C2 ), then the upload of every table of the
view. The rows are the legacy interaction pass's per-draw products without the model matrix ( the forward pass
and the volumetrics come with positions relative to the view's renderOrigin, which is what shadowV and shadowP
take ): parallel and point maps clipSpaceToWindowSpace x shadowP x shadowV, a spot map shadowP x shadowV. Every
row of a light without a placed shadow map, and every row while the atlas is off or shadows are skipped, has
atlasRect.z = -1. A face or cascade that was not drawn, of a light whose maps are placed, has -2: the forward loop
keeps the shadow-map body for that light and looks nothing up there. Everything is uploaded here, before the first
pass that reads it ( the fog inject, then the forward pass ).
====================
*/
void idRenderBackend::R_BuildShadowRows( const viewDef_t* viewDef )
{
	const lightListView_t* llv = viewDef->lightListView;
	if( llv == NULL || viewDef->recordLights == NULL )
	{
		return;
	}
	const lightListTables_t& t = llv->tables;

	const int numRows = Min( viewDef->numShadowRows, MAX_RECORD_LIGHTS * 6 );
	memset( lightListRows, 0, numRows * sizeof( shadowRow_t ) );
	for( int i = 0; i < numRows; i++ )
	{
		lightListRows[i].atlasRect.z = -1.0f;
	}
	const bool atlas = r_useShadowAtlas.GetInteger() != 0 && !r_skipShadows.GetBool();
	const float invAtlasSize = 1.0f / ( float )r_shadowMapAtlasSize.GetInteger();
	// what the atlas pass drew, read the way it reads it: a cascade or a face it left out has no map, whatever the
	// frontend gave rows to ( the frontend counted the cascades one frame earlier )
	const shadowSkipSet_t& skipSet = viewDef->shadowSkipSet;
	const int atlasCascades = ( skipSet.valid ? skipSet.shadowMapSplits : r_shadowMapSplits.GetInteger() ) + 1;
	const int atlasSingleSide = skipSet.valid ? skipSet.shadowMapSingleSide : r_shadowMapSingleSide.GetInteger();
	for( const viewLight_t* vLight = viewDef->viewLights; vLight != NULL; vLight = vLight->next )
	{
		const int record = vLight->recordNumber;
		if( record < 0 || record >= t.numRecords )
		{
			continue;
		}
		const int first = viewDef->recordLights[record].shadowRow;
		const int count = llv->rowsOfRecord[record];
		if( first < 0 || first + count > numRows || !R_ViewLightHasShadowMap( vLight ) )
		{
			continue;
		}
		for( int s = 0; s < count; s++ )
		{
			shadowRow_t& row = lightListRows[first + s];
			idRenderMatrix shadowClip;
			idRenderMatrix::Multiply( vLight->shadowP[s], vLight->shadowV[s], shadowClip );
			if( vLight->pointLight || vLight->parallel )
			{
				idRenderMatrix shadowWindow;
				idRenderMatrix::Multiply( renderMatrix_clipSpaceToWindowSpace, shadowClip, shadowWindow );
				memcpy( row.worldToShadow, shadowWindow[0], sizeof( row.worldToShadow ) );
			}
			else
			{
				memcpy( row.worldToShadow, shadowClip[0], sizeof( row.worldToShadow ) );	// a spot map's P carries the window-space step already
			}
			// the sun's rows carry the far end of their cascade; every other row the light's fade to unshadowed ( D47 )
			float w = vLight->shadowFadeOut;
			if( vLight->isPrimarySun )
			{
				w = ( s < count - 1 && s < MAX_FRUSTUMS ) ? viewDef->frustumSplitDistances[s] : viewDef->shadowCascadeFar;
			}
			const bool drawn = vLight->parallel ? ( s < atlasCascades ) : ( !vLight->pointLight || atlasSingleSide == -1 || s == atlasSingleSide );
			const bool placed = atlas && drawn && vLight->ImageAtlasPlaced() && vLight->imageAtlasOffset[s].x != -1 && vLight->imageAtlasOffset[s].y != -1;
			if( placed )
			{
				row.atlasRect.Set( vLight->imageAtlasOffset[s].x * invAtlasSize, vLight->imageAtlasOffset[s].y * invAtlasSize, vLight->imageSize.x * invAtlasSize, w );
			}
			else
			{
				// -2: the light's maps have places and the legacy pass binds its shadow-map program for the whole
				// light, but this face or cascade was not drawn: the forward loop keeps that program's body there,
				// unshadowed. -1: the light has no placed map: the plain body
				row.atlasRect.Set( 0.0f, 0.0f, ( atlas && vLight->ImageAtlasPlaced() ) ? -2.0f : -1.0f, w );
			}
		}
	}

	const int tileBitWords = t.tilesX * t.tilesY * t.wordsPerTile;
	LightListCreateBuffers( tileBitWords );
	commandList->writeBuffer( recordLightsBuffer, viewDef->recordLights, viewDef->numRecordLights * sizeof( recordLight_t ) );
	if( numRows > 0 )
	{
		commandList->writeBuffer( shadowRowsBuffer, lightListRows, numRows * sizeof( shadowRow_t ) );
	}
	commandList->writeBuffer( lightPoolBuffer, llv->pool, llv->stats.poolUsed * sizeof( uint32 ) );
	commandList->writeBuffer( zBinsBuffer, t.bins, t.parms.numBins * LL_BIN_STRIDE * sizeof( uint32 ) );
	if( tileBitWords > 0 )
	{
		commandList->writeBuffer( tileBitsBuffer, llv->tileBits, tileBitWords * sizeof( uint32 ) );
	}
	if( llv->numVanishedPrev > 0 )
	{
		commandList->writeBuffer( vanishedPrevBuffer, llv->vanishedPrev, Min( llv->numVanishedPrev, MAX_RECORD_LIGHTS ) * sizeof( int ) );
	}
	// a write to a buffer ends the render pass on Vulkan: whatever draws next sets its state anew
	ResetGraphicsStateCache();
	pc.c_lightListRecords = viewDef->numRecordLights;
	pc.c_lightListRows = numRows;
	pc.c_lightListUploadBytes = viewDef->numRecordLights * ( int )sizeof( recordLight_t ) + numRows * ( int )sizeof( shadowRow_t )
								+ ( llv->stats.poolUsed + t.parms.numBins * LL_BIN_STRIDE + tileBitWords + llv->numVanishedPrev ) * ( int )sizeof( uint32 );
}

// the constants of the heatmap pass, laid out as shaders/lightlist_heatmap.cb.h declares them
struct lightListHeatmapConstants_t
{
	uint32	tileSize;
	uint32	tilesX;
	uint32	wordsPerTile;
	uint32	numBins;
	float	binsPerUnit;
	float	zNear;
	float	opacity;
	uint32	pad;
	int		viewOrigin[2];
	int		viewSize[2];
};
static_assert( sizeof( lightListHeatmapConstants_t ) == 48, "laid out as shaders/lightlist_heatmap.cb.h" );

/*
====================
idRenderBackend::DBG_LightLists

r_showLightLists 1: the light-count heatmap, over the view. For every pixel the number of clustered records a
forward pixel would loop there, from the scene depth, the Z-bins and the tile bits as they were uploaded for
this view. The pass draws into the colour attachment of the view's framebuffer alone ( a framebuffer of its
own, made for that texture ), because it samples the depth that framebuffer has attached. The engine's
pipelines know nothing of it: its own shader pair, binding layout and pipeline, found and made at first use
and released with ClearCaches.
====================
*/
void idRenderBackend::DBG_LightLists()
{
	const lightListView_t* llv = viewDef->lightListView;
	if( llv == NULL || llv->showMode != 1 || !tileBitsBuffer || !zBinsBuffer )
	{
		return;
	}
	Framebuffer* active = Framebuffer::GetActiveFramebuffer();
	if( active == NULL )
	{
		return;
	}
	const nvrhi::FramebufferDesc& activeDesc = active->GetApiObject()->getDesc();
	if( activeDesc.colorAttachments.empty() || !activeDesc.colorAttachments[0].valid() )
	{
		return;
	}
	nvrhi::ITexture* color = activeDesc.colorAttachments[0].texture;
	nvrhi::ITexture* depth = globalImages->currentDepthImage->GetTextureHandle().Get();
	if( depth == NULL || depth->getDesc().sampleCount != 1 )
	{
		return;
	}
	nvrhi::IDevice* device = deviceManager->GetDevice();

	if( !lightListHeatmapVS )
	{
		lightListHeatmapVS = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/debug/lightlist_heatmap", SHADER_STAGE_VERTEX, "", idList<shaderMacro_t>(), true, LAYOUT_DRAW_VERT ) );
		lightListHeatmapPS = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/debug/lightlist_heatmap", SHADER_STAGE_FRAGMENT, "", idList<shaderMacro_t>(), true, LAYOUT_DRAW_VERT ) );

		nvrhi::BindingLayoutDesc layoutDesc;
		layoutDesc.visibility = nvrhi::ShaderType::All;
		layoutDesc.bindings =
		{
			nvrhi::BindingLayoutItem::PushConstants( 0, sizeof( lightListHeatmapConstants_t ) ),
			nvrhi::BindingLayoutItem::Texture_SRV( 0 ),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 1 ),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 2 )
		};
		lightListHeatmapLayout = device->createBindingLayout( layoutDesc );
	}
	if( !lightListHeatmapTarget || lightListHeatmapColor != color )
	{
		lightListHeatmapTarget = device->createFramebuffer( nvrhi::FramebufferDesc().addColorAttachment( color ) );
		lightListHeatmapColor = color;
		lightListHeatmapPipeline = nullptr;									// a pipeline is made for a framebuffer's formats
	}
	if( !lightListHeatmapPipeline )
	{
		nvrhi::GraphicsPipelineDesc psoDesc;
		psoDesc.bindingLayouts = { lightListHeatmapLayout };
		psoDesc.VS = lightListHeatmapVS;
		psoDesc.PS = lightListHeatmapPS;
		psoDesc.primType = nvrhi::PrimitiveType::TriangleList;
		psoDesc.renderState.rasterState.setCullNone();
		psoDesc.renderState.depthStencilState.depthTestEnable = false;
		psoDesc.renderState.depthStencilState.depthWriteEnable = false;
		psoDesc.renderState.depthStencilState.stencilEnable = false;
		psoDesc.renderState.blendState.targets[0].blendEnable = true;
		psoDesc.renderState.blendState.targets[0].srcBlend = nvrhi::BlendFactor::SrcAlpha;
		psoDesc.renderState.blendState.targets[0].destBlend = nvrhi::BlendFactor::InvSrcAlpha;
		psoDesc.renderState.blendState.targets[0].srcBlendAlpha = nvrhi::BlendFactor::Zero;
		psoDesc.renderState.blendState.targets[0].destBlendAlpha = nvrhi::BlendFactor::One;
		lightListHeatmapPipeline = device->createGraphicsPipeline( psoDesc, lightListHeatmapTarget );
	}

	nvrhi::BindingSetDesc bindingSetDesc;
	bindingSetDesc.bindings =
	{
		nvrhi::BindingSetItem::PushConstants( 0, sizeof( lightListHeatmapConstants_t ) ),
		nvrhi::BindingSetItem::Texture_SRV( 0, depth ),
		nvrhi::BindingSetItem::StructuredBuffer_SRV( 1, zBinsBuffer ),
		nvrhi::BindingSetItem::StructuredBuffer_SRV( 2, tileBitsBuffer )
	};
	nvrhi::BindingSetHandle bindingSet = bindingCache.GetOrCreateBindingSet( bindingSetDesc, lightListHeatmapLayout );

	const lightListTables_t& t = llv->tables;
	lightListHeatmapConstants_t constants;
	constants.tileSize = ( uint32 )t.parms.tileSize;
	constants.tilesX = ( uint32 )t.tilesX;
	constants.wordsPerTile = ( uint32 )t.wordsPerTile;
	constants.numBins = ( uint32 )t.parms.numBins;
	constants.binsPerUnit = t.binScale;
	constants.zNear = t.parms.zNear;
	constants.opacity = 0.6f;
	constants.pad = 0;
	constants.viewOrigin[0] = viewDef->viewport.x1;
	constants.viewOrigin[1] = viewDef->viewport.y1;
	constants.viewSize[0] = t.parms.width;
	constants.viewSize[1] = t.parms.height;

	const nvrhi::Viewport viewport( ( float )constants.viewOrigin[0], ( float )( constants.viewOrigin[0] + constants.viewSize[0] ),
									( float )constants.viewOrigin[1], ( float )( constants.viewOrigin[1] + constants.viewSize[1] ), 0.0f, 1.0f );

	nvrhi::GraphicsState state;
	state.indexBuffer = {};		// the fork leaves this one member uninitialized
	state.pipeline = lightListHeatmapPipeline;
	state.framebuffer = lightListHeatmapTarget;
	state.bindings = { bindingSet };
	state.viewport.addViewport( viewport );
	state.viewport.addScissorRect( nvrhi::Rect( viewport ) );

	commandList->setGraphicsState( state );
	commandList->setPushConstants( &constants, sizeof( constants ) );

	nvrhi::DrawArguments args;
	args.instanceCount = 1;
	args.vertexCount = 3;
	commandList->draw( args );

	// the engine's own state cache knows nothing of this draw
	ResetGraphicsStateCache();
}

idCVar r_lightDiffScale( "r_lightDiffScale", "16", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "r_showLightDiff 2 shows the difference of the forward and the legacy light times this" );

struct lightDiffConstants_t
{
	int		viewOrigin[2];		// the view's top-left pixel in the target
	int		viewSize[2];
	uint32	mode;				// 0 = the picture, 1 = the reduction
	float	scale;
	uint32	pad0;
	uint32	pad1;
};
static_assert( sizeof( lightDiffConstants_t ) == 32, "laid out as shaders/lightdiff.cb.h" );

static const int LIGHTDIFF_BLOCK = 16;		// shaders/lightdiff.cb.h: LIGHTDIFF_BLOCK

static bool lightDiffSaidMultisampled = false;	// LightDiffPass: said once

/*
====================
idRenderBackend::LightDiffPass

The difference view ( r_showLightDiff 2; D10, D49, D79 ): how far the forward pass's light is from the legacy
pass's, in this view, this frame. Called after DrawInteractions. In the twin mode every listed pair also has
its lit surface, so the frame up to here is the LEGACY picture of the view's lit passes: the forward program
drew the listing surfaces with the loop off and the legacy pass lit everything. This pass
  1. keeps that picture ( B ),
  2. clears the colour and draws the lit passes again as the FORWARD picture ( A ): the loop on, the legacy
     pass without the twins ( lightDiffPassA ), over the same depth,
  3. shows | A - B | times r_lightDiffScale in the view,
  4. reduces | A - B | on the GPU to one texel per block of 16 x 16 pixels and copies that small picture into
     a ring of three staging textures; the one written two frames ago is mapped and summed here, so the frame
     never waits for the GPU ( D79 ). The read-out goes to the HUD and, with r_lightListLog, to the console.
The engine's pipelines know nothing of it: its own shader pair ( builtin/debug/lightdiff ), layout and two
pipelines, made at first use and released with ClearCaches ( they stay after the view is switched off ).
====================
*/
void idRenderBackend::LightDiffPass( const drawSurf_t* const* drawSurfs, int numDrawSurfs )
{
	const lightListView_t* llv = viewDef->lightListView;
	if( llv == NULL || !viewDef->listsBuilt || !llv->twin || !llv->diff || lightDiffPassA )
	{
		return;
	}
	Framebuffer* active = Framebuffer::GetActiveFramebuffer();
	if( active == NULL )
	{
		return;
	}
	const nvrhi::FramebufferDesc& activeDesc = active->GetApiObject()->getDesc();
	if( activeDesc.colorAttachments.empty() || !activeDesc.colorAttachments[0].valid() )
	{
		return;
	}
	nvrhi::ITexture* color = activeDesc.colorAttachments[0].texture;
	const nvrhi::TextureDesc& colorDesc = color->getDesc();
	if( colorDesc.sampleCount != 1 )
	{
		// a multisampled target cannot be copied this way
		if( !lightDiffSaidMultisampled )
		{
			lightDiffSaidMultisampled = true;
			common->Printf( "lightdiff: the view's target is multisampled: no difference view ( the twin mode stays on )\n" );
		}
		return;
	}
	nvrhi::IDevice* device = deviceManager->GetDevice();
	const int reducedWidth = ( ( int )colorDesc.width + LIGHTDIFF_BLOCK - 1 ) / LIGHTDIFF_BLOCK;
	const int reducedHeight = ( ( int )colorDesc.height + LIGHTDIFF_BLOCK - 1 ) / LIGHTDIFF_BLOCK;

	// the two pictures in the view's own format, the reduced picture and its ring: made for the target's size
	if( !lightDiffImageA || lightDiffImageA->getDesc().width != colorDesc.width || lightDiffImageA->getDesc().height != colorDesc.height
			|| lightDiffImageA->getDesc().format != colorDesc.format )
	{
		nvrhi::TextureDesc desc;
		desc.width = colorDesc.width;
		desc.height = colorDesc.height;
		desc.format = colorDesc.format;
		desc.dimension = nvrhi::TextureDimension::Texture2D;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "lightDiffA";
		lightDiffImageA = device->createTexture( desc );
		desc.debugName = "lightDiffB";
		lightDiffImageB = device->createTexture( desc );

		nvrhi::TextureDesc reduced;
		reduced.width = reducedWidth;
		reduced.height = reducedHeight;
		reduced.format = nvrhi::Format::RGBA32_FLOAT;
		reduced.dimension = nvrhi::TextureDimension::Texture2D;
		reduced.isRenderTarget = true;
		reduced.initialState = nvrhi::ResourceStates::ShaderResource;
		reduced.keepInitialState = true;
		reduced.debugName = "lightDiffReduced";
		lightDiffReduced = device->createTexture( reduced );
		lightDiffReducedTarget = device->createFramebuffer( nvrhi::FramebufferDesc().addColorAttachment( lightDiffReduced ) );
		lightDiffReducePipeline = nullptr;

		nvrhi::TextureDesc staging;
		staging.width = reducedWidth;
		staging.height = reducedHeight;
		staging.format = nvrhi::Format::RGBA32_FLOAT;
		staging.dimension = nvrhi::TextureDimension::Texture2D;
		staging.debugName = "lightDiffStaging";
		for( int i = 0; i < 3; i++ )
		{
			lightDiffStaging[i] = device->createStagingTexture( staging, nvrhi::CpuAccessMode::Read );
			lightDiffStagingFrame[i] = -1;
		}
		lightDiffHasReadout = false;										// the numbers of another size are not this view's
	}
	if( !lightDiffVS )
	{
		lightDiffVS = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/debug/lightdiff", SHADER_STAGE_VERTEX, "", idList<shaderMacro_t>(), true, LAYOUT_DRAW_VERT ) );
		lightDiffPS = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/debug/lightdiff", SHADER_STAGE_FRAGMENT, "", idList<shaderMacro_t>(), true, LAYOUT_DRAW_VERT ) );

		nvrhi::BindingLayoutDesc layoutDesc;
		layoutDesc.visibility = nvrhi::ShaderType::All;
		layoutDesc.bindings =
		{
			nvrhi::BindingLayoutItem::PushConstants( 0, sizeof( lightDiffConstants_t ) ),
			nvrhi::BindingLayoutItem::Texture_SRV( 0 ),
			nvrhi::BindingLayoutItem::Texture_SRV( 1 )
		};
		lightDiffLayout = device->createBindingLayout( layoutDesc );
		lightDiffShowPipeline = nullptr;
		lightDiffReducePipeline = nullptr;
	}
	if( !lightDiffShowTarget || lightDiffShowColor != color )
	{
		lightDiffShowTarget = device->createFramebuffer( nvrhi::FramebufferDesc().addColorAttachment( color ) );
		lightDiffShowColor = color;
		lightDiffShowPipeline = nullptr;										// a pipeline is made for a framebuffer's formats
	}
	if( !lightDiffShowPipeline || !lightDiffReducePipeline )
	{
		nvrhi::GraphicsPipelineDesc psoDesc;
		psoDesc.bindingLayouts = { lightDiffLayout };
		psoDesc.VS = lightDiffVS;
		psoDesc.PS = lightDiffPS;
		psoDesc.primType = nvrhi::PrimitiveType::TriangleList;
		psoDesc.renderState.rasterState.setCullNone();
		psoDesc.renderState.depthStencilState.depthTestEnable = false;
		psoDesc.renderState.depthStencilState.depthWriteEnable = false;
		psoDesc.renderState.depthStencilState.stencilEnable = false;
		if( !lightDiffShowPipeline )
		{
			lightDiffShowPipeline = device->createGraphicsPipeline( psoDesc, lightDiffShowTarget );
		}
		if( !lightDiffReducePipeline )
		{
			lightDiffReducePipeline = device->createGraphicsPipeline( psoDesc, lightDiffReducedTarget );
		}
	}

	renderLog.OpenBlock( "Render_LightDiff", colorBlue );

	// 1. the legacy picture
	commandList->copyTexture( lightDiffImageB, nvrhi::TextureSlice(), color, nvrhi::TextureSlice() );

	// 2. the forward picture: the same two passes over the same depth, the loop on and the twins out. The legacy
	//    pass left its last light's scissor behind
	ResetGraphicsStateCache();
	ResetViewportAndScissorToDefaultCamera( viewDef );
	GL_Clear( true, false, false, 0, 0.0f, 0.0f, 0.0f, 0.0f );
	lightDiffPassA = true;
	ForwardPass( drawSurfs, numDrawSurfs, false );
	DrawInteractions( viewDef );
	lightDiffPassA = false;
	commandList->copyTexture( lightDiffImageA, nvrhi::TextureSlice(), color, nvrhi::TextureSlice() );

	nvrhi::BindingSetDesc bindingSetDesc;
	bindingSetDesc.bindings =
	{
		nvrhi::BindingSetItem::PushConstants( 0, sizeof( lightDiffConstants_t ) ),
		nvrhi::BindingSetItem::Texture_SRV( 0, lightDiffImageA ),
		nvrhi::BindingSetItem::Texture_SRV( 1, lightDiffImageB )
	};
	nvrhi::BindingSetHandle bindingSet = bindingCache.GetOrCreateBindingSet( bindingSetDesc, lightDiffLayout );

	lightDiffConstants_t constants;
	constants.viewOrigin[0] = viewDef->viewport.x1;
	constants.viewOrigin[1] = viewDef->viewport.y1;
	constants.viewSize[0] = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	constants.viewSize[1] = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;
	constants.mode = 0;
	constants.scale = r_lightDiffScale.GetFloat();
	constants.pad0 = 0;
	constants.pad1 = 0;

	nvrhi::DrawArguments args;
	args.instanceCount = 1;
	args.vertexCount = 3;

	// 3. the difference in the view
	{
		const nvrhi::Viewport viewport( ( float )constants.viewOrigin[0], ( float )( constants.viewOrigin[0] + constants.viewSize[0] ),
										( float )constants.viewOrigin[1], ( float )( constants.viewOrigin[1] + constants.viewSize[1] ), 0.0f, 1.0f );
		nvrhi::GraphicsState state;
		state.indexBuffer = {};		// the fork leaves this one member uninitialized
		state.pipeline = lightDiffShowPipeline;
		state.framebuffer = lightDiffShowTarget;
		state.bindings = { bindingSet };
		state.viewport.addViewport( viewport );
		state.viewport.addScissorRect( nvrhi::Rect( viewport ) );
		commandList->setGraphicsState( state );
		commandList->setPushConstants( &constants, sizeof( constants ) );
		commandList->draw( args );
	}

	// 4. the reduction: one texel per block of the view, then into this frame's slot of the ring
	{
		constants.mode = 1;
		const int blocksX = ( constants.viewSize[0] + LIGHTDIFF_BLOCK - 1 ) / LIGHTDIFF_BLOCK;
		const int blocksY = ( constants.viewSize[1] + LIGHTDIFF_BLOCK - 1 ) / LIGHTDIFF_BLOCK;
		const nvrhi::Viewport viewport( 0.0f, ( float )Min( blocksX, reducedWidth ), 0.0f, ( float )Min( blocksY, reducedHeight ), 0.0f, 1.0f );
		nvrhi::GraphicsState state;
		state.indexBuffer = {};
		state.pipeline = lightDiffReducePipeline;
		state.framebuffer = lightDiffReducedTarget;
		state.bindings = { bindingSet };
		state.viewport.addViewport( viewport );
		state.viewport.addScissorRect( nvrhi::Rect( viewport ) );
		commandList->setGraphicsState( state );
		commandList->setPushConstants( &constants, sizeof( constants ) );
		commandList->draw( args );

		// a gap ( the view or the lists were off for a frame or more ): the slots written before it are not "two
		// passes ago" any more, and the last read-out is not this stretch's
		const int sinceLast = llv->viewFrame - lightDiffViewFrame;
		if( sinceLast < 0 || sinceLast > 1 )
		{
			for( int i = 0; i < 3; i++ )
			{
				lightDiffStagingFrame[i] = -1;
			}
			lightDiffHasReadout = false;
		}
		lightDiffViewFrame = llv->viewFrame;

		const int slot = lightDiffFrame % 3;
		commandList->copyTexture( lightDiffStaging[slot], nvrhi::TextureSlice(), lightDiffReduced, nvrhi::TextureSlice() );
		lightDiffStagingFrame[slot] = lightDiffFrame;

		// the slot written two passes ago: the GPU has long finished it
		const int read = ( lightDiffFrame + 1 ) % 3;
		if( lightDiffStagingFrame[read] >= 0 && lightDiffStagingFrame[read] == lightDiffFrame - 2 )
		{
			size_t rowPitch = 0;
			const byte* data = ( const byte* )device->mapStagingTexture( lightDiffStaging[read], nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch );
			if( data != NULL )
			{
				double sum = 0.0;
				double sumLegacy = 0.0;
				double count = 0.0;
				float maxDiff = 0.0f;
				for( int y = 0; y < Min( blocksY, reducedHeight ); y++ )
				{
					const float* row = ( const float* )( data + y * rowPitch );
					for( int x = 0; x < Min( blocksX, reducedWidth ); x++ )
					{
						sum += row[x * 4 + 0];
						maxDiff = Max( maxDiff, row[x * 4 + 1] );
						sumLegacy += row[x * 4 + 2];
						count += row[x * 4 + 3];
					}
				}
				device->unmapStagingTexture( lightDiffStaging[read] );
				if( count > 0.0 )
				{
					lightDiffMean = ( float )( sum / count );
					lightDiffMax = maxDiff;
					lightDiffLegacyMean = ( float )( sumLegacy / count );
					lightDiffHasReadout = true;
					if( llv->log )
					{
						common->Printf( "lightdiff: frame %d mean %.7f max %.6f legacy mean %.5f\n", llv->viewFrame, lightDiffMean, lightDiffMax, lightDiffLegacyMean );
					}
				}
			}
		}
		lightDiffFrame++;
	}
	pc.c_lightDiffValid = lightDiffHasReadout ? 1 : 0;
	pc.c_lightDiffMean = lightDiffMean;
	pc.c_lightDiffMax = lightDiffMax;
	pc.c_lightDiffLegacyMean = lightDiffLegacyMean;

	renderLog.CloseBlock();
	// the engine's own state cache knows nothing of these draws
	ResetGraphicsStateCache();
}
