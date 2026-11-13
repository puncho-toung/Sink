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
}

/*
====================
idRenderBackend::R_BuildShadowRows

After ShadowAtlasPass, in a view that has records: the shadow rows ( C2 ), then the upload of every table of the
view. The rows are the legacy interaction pass's per-draw products without the model matrix ( the forward pass
and the volumetrics come with positions relative to the view's renderOrigin, which is what shadowV and shadowP
take ): parallel and point maps clipSpaceToWindowSpace x shadowP x shadowV, a spot map shadowP x shadowV. A row
whose map has no place in the atlas, every row of a light without a shadow map, and every row while the atlas is
off or shadows are skipped, has atlasRect.z = -1. Everything is uploaded here, before the first pass that reads
it ( the fog inject, then the forward pass ).
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
				row.atlasRect.Set( 0.0f, 0.0f, -1.0f, w );
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
