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
#include <precompiled.h>
#pragma hdrstop

#include "renderer/RenderCommon.h"
#include "../../framework/Common_local.h"	// the probe prints the GPU time of the fog's two blocks

#include "VolumetricFogPass.h"
#include "VolumetricFogPass_cb.h"

idCVar r_volumetricFog( "r_volumetricFog", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "froxel fog in the main view: 0 = off ( no fog pass: the engine's picture without it ), 1 = on" );
static idCVar r_volumetricFogColumnPixels( "r_volumetricFogColumnPixels", "12", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "the side of a froxel column in pixels; the grid follows the render size, and a grid of too many froxels gets wider columns", 4, 64 );
static idCVar r_volumetricFogSlices( "r_volumetricFogSlices", "64", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "the number of depth slices of the fog volume", 8, 256 );
static idCVar r_volumetricFogFar( "r_volumetricFogFar", "3000", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "the far end of the fog volume in units along the view axis; no fog is added beyond it", 64.0f, 65536.0f );
static idCVar r_volumetricFogDistance( "r_volumetricFogDistance", "1500", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "the global fog's distance D in units: the air is 99 percent opaque at D ( sigma_t = 4.632 / D ); 0 = no global fog; a distance above 0 counts as at least 0.1", 0.0f, 1000000.0f );
static idCVar r_volumetricFogAlbedo( "r_volumetricFogAlbedo", "0.9", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "the global fog's scattering albedo: the share of the light the air takes out of a ray that it scatters and does not absorb", 0.0f, 1.0f );
static idCVar r_volumetricFogFurnace( "r_volumetricFogFurnace", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "test: above 0 the fog is the uniform test medium, lit by this radiance from every direction and by nothing else ( cut where the fog is so thick that the half-float volumes cannot hold the in-scatter )", 0.0f, 1000.0f );
static idCVar r_volumetricFogDebug( "r_volumetricFogDebug", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "fog debug view in place of the picture: 1 = the transmittance to each pixel as grey, 2 = the slices as bands over the grid of columns", 0, 2 );

// coefficients are stored times this, which keeps thin, dim fog out of the half floats' subnormal range
static const float FROXEL_STORE_SCALE	= 1024.0f;
// S of the biased logarithmic slices, in units: slices are about linear nearer than S and exponential beyond it
static const float FROXEL_SLICE_BIAS	= 16.0f;
// sigma_t * D: the air is 99 percent opaque at the fog distance D, as Doom 3's fog lights are
static const float FROXEL_OPACITY_AT_D	= 4.632f;
// The volumes are half floats, whose largest value is 65504: what the pass stores stays below this bound. A fog
// distance above 0 counts as at least FROXEL_MIN_FOG_DISTANCE units ( sigma_t times the store scale is then
// 47,432 ), and the stored in-scatter is cut at the bound
static const float FROXEL_STORE_MAX		= 60000.0f;
static const float FROXEL_MIN_FOG_DISTANCE	= 0.1f;
// a bound on the froxels of one volume ( 8 bytes each: 32 MiB ); a grid past it gets wider columns
static const int FROXEL_MAX_CELLS		= 4 << 20;
// the probe's columns: the view's centre, its first column and row ( a corner ), the middle of its left edge
static const int FROXEL_PROBE_COLUMNS	= 3;
// the probe's copy is read this many fog views later: the frame that holds the copy has been handed to the GPU by
// then ( the map itself waits for the GPU )
static const int FROXEL_PROBE_WAIT		= 3;
// the engine framebuffer of the sky mask: the mask as colour, the view's depth image as depth
static const char* const FROXEL_SKYMASK_TARGET = "_fogSkyMask";

// Set by the console command fogProbe, taken by the next fog view. Console commands run on the main thread, where
// the backend runs too, so nothing else touches it
static bool fogProbeRequested = false;

CONSOLE_COMMAND( fogProbe, "prints three columns of the integrated fog volume beside the closed form of the uniform medium, the fogged picture's centre pixel and the GPU time of the fog's two blocks", NULL )
{
	fogProbeRequested = true;
	if( !r_volumetricFog.GetBool() )
	{
		common->Printf( "fogProbe: r_volumetricFog is 0, the probe waits for the first fog view\n" );
	}
}

// Whether a froxel can hold a medium at all: the global fog has a distance, or a debug view wants the pass. In clear
// air the pass does nothing in a view: no sky mask, no kernels, no apply. A medium of another kind than the global
// fog must be asked here too
static bool FroxelHasMedium()
{
	return r_volumetricFogDistance.GetFloat() > 0.0f || r_volumetricFogDebug.GetInteger() != 0;
}

VolumetricFogPass::VolumetricFogPass( nvrhi::IDevice* device, CommonRenderPasses* commonPasses )
	: m_Device( device )
	, commonRenderPasses( commonPasses )
	, m_GridWidth( 0 )
	, m_GridHeight( 0 )
	, m_GridSlices( 0 )
	, m_ColumnPixels( 0 )
	, m_ViewGridWidth( 0 )
	, m_ViewGridHeight( 0 )
	, m_SliceStep( 0.0f )
	, m_ApplyColor( NULL )
	, m_RenderedView( NULL )
	, m_SkyMask( NULL )
	, m_SkyMaskView( NULL )
	, m_SkyMaskEmpty( false )
	, m_ProbeWait( 0 )
	, m_ProbeStaged( false )
	, m_ProbeHasPixel( false )
	, m_ProbeSlices( 0 )
	, m_ProbeSliceStep( 0.0f )
	, m_ProbeSigmaT( 0.0f )
	, m_ProbeSource( 0.0f )
{
	nvrhi::BufferDesc constantBufferDesc;
	constantBufferDesc.byteSize = sizeof( froxelConstants_t );
	constantBufferDesc.debugName = "FroxelConstants";
	constantBufferDesc.isConstantBuffer = true;
	constantBufferDesc.isVolatile = true;
	constantBufferDesc.maxVersions = c_MaxRenderPassConstantBufferVersions;
	m_ConstantBuffer = device->createBuffer( constantBufferDesc );

	// the kernels are found here, at the pass's first use ( F17 ): a run with the fog off never loads them
	InitKernel( m_Inject, "builtin/volumetrics/froxel_inject", false );
	InitKernel( m_Resolve, "builtin/volumetrics/froxel_resolve", true );
	InitKernel( m_Integrate, "builtin/volumetrics/froxel_integrate", true );

	const idList<shaderMacro_t> noMacros;
	m_ApplyVS = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/volumetrics/froxel_apply", SHADER_STAGE_VERTEX, "", noMacros, true, LAYOUT_DRAW_VERT ) );
	m_ApplyPS = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/volumetrics/froxel_apply", SHADER_STAGE_FRAGMENT, "", noMacros, true, LAYOUT_DRAW_VERT ) );

	// the fog set's three items at the registers shaders/froxel_fog.inc.hlsl gives them, the scene depth and the
	// sky mask
	nvrhi::BindingLayoutDesc applyLayoutDesc;
	applyLayoutDesc.visibility = nvrhi::ShaderType::All;
	applyLayoutDesc.bindings =
	{
		nvrhi::BindingLayoutItem::VolatileConstantBuffer( 1 ),
		nvrhi::BindingLayoutItem::Texture_SRV( 15 ),
		nvrhi::BindingLayoutItem::Sampler( 5 ),
		nvrhi::BindingLayoutItem::Texture_SRV( 0 ),
		nvrhi::BindingLayoutItem::Texture_SRV( 1 )
	};
	m_ApplyLayout = device->createBindingLayout( applyLayoutDesc );
}

VolumetricFogPass::~VolumetricFogPass()
{
	ReleaseVolume();
}

/*
========================
VolumetricFogPass::ReleaseVolume

Called where the integrated volume is about to die: the grid changed, or the pass goes. Since its last apply the
engine's fog sets name the volume ( backEnd.froxelVolume ), and the engine's binding cache keeps a set, and the
textures the set names, for as long as the cache lives. So the fog set gets the dummy volume back and the cache
starts again. A pass that never applied has handed out nothing and clears nothing.
========================
*/
void VolumetricFogPass::ReleaseVolume()
{
	if( m_Integrated && backEnd.froxelVolume == m_Integrated )
	{
		backEnd.froxelVolume = backEnd.froxelDummyVolume;
		backEnd.bindingCache.Clear();
	}
}

/*
========================
VolumetricFogPass::InitKernel

One compute kernel: its shader, its layout and its pipeline. Every kernel reads the constants at b1 and writes
one volume at u0; a kernel that reads a volume has it at t0.
========================
*/
void VolumetricFogPass::InitKernel( kernel_t& kernel, const char* shaderName, bool readsVolume )
{
	const idList<shaderMacro_t> noMacros;
	kernel.shader = renderProgManager.GetShader( renderProgManager.FindShader( shaderName, SHADER_STAGE_COMPUTE, "", noMacros, true, LAYOUT_DRAW_VERT ) );

	nvrhi::BindingLayoutDesc layoutDesc;
	layoutDesc.visibility = nvrhi::ShaderType::Compute;
	layoutDesc.bindings.push_back( nvrhi::BindingLayoutItem::VolatileConstantBuffer( 1 ) );
	if( readsVolume )
	{
		layoutDesc.bindings.push_back( nvrhi::BindingLayoutItem::Texture_SRV( 0 ) );
	}
	layoutDesc.bindings.push_back( nvrhi::BindingLayoutItem::Texture_UAV( 0 ) );
	kernel.layout = m_Device->createBindingLayout( layoutDesc );

	nvrhi::ComputePipelineDesc pipelineDesc;
	pipelineDesc.bindingLayouts = { kernel.layout };
	pipelineDesc.CS = kernel.shader;
	kernel.pipeline = m_Device->createComputePipeline( pipelineDesc );
}

/*
========================
VolumetricFogPass::CheckVolumes

Makes the three volumes and the kernels' binding sets for a grid, when the pass has none or the grid changed
( the render size's grid: a grid cvar changed ). In every frame the kernels write every froxel the lookup's
filter can fetch ( Render: the view's grid and one column and row more ), so a new volume is not cleared.
Returns false when there is no grid to fill or the volumes could not be made: the view then stays unfogged, and
the same grid is not tried again.
========================
*/
bool VolumetricFogPass::CheckVolumes( int gridWidth, int gridHeight, int gridSlices )
{
	if( gridWidth < 1 || gridHeight < 1 || gridSlices < 1 )
	{
		return false;
	}
	if( gridWidth == m_GridWidth && gridHeight == m_GridHeight && gridSlices == m_GridSlices )
	{
		return m_Integrated != nullptr;		// this grid's volumes exist, or the device could not make them
	}
	// the grid changed: the old volumes go, and the fog set binds the dummy until this view's apply
	ReleaseVolume();

	nvrhi::TextureDesc desc;
	desc.dimension = nvrhi::TextureDimension::Texture3D;
	desc.width = gridWidth;
	desc.height = gridHeight;
	desc.depth = gridSlices;
	desc.mipLevels = 1;
	desc.format = nvrhi::Format::RGBA16_FLOAT;
	desc.isUAV = true;
	desc.initialState = nvrhi::ResourceStates::ShaderResource;
	desc.keepInitialState = true;

	desc.debugName = "Fog/Injected";
	m_Injected = m_Device->createTexture( desc );
	desc.debugName = "Fog/Resolved";
	m_Resolved = m_Device->createTexture( desc );
	desc.debugName = "Fog/Integrated";
	m_Integrated = m_Device->createTexture( desc );
	if( !m_Injected || !m_Resolved || !m_Integrated )
	{
		// The device could not make a volume ( out of memory ): no fog. The kernels' sets let the old volumes go, and
		// the grid is kept, so that it is not tried again in every view
		m_Injected = nullptr;
		m_Resolved = nullptr;
		m_Integrated = nullptr;
		m_Inject.bindingSet = nullptr;
		m_Resolve.bindingSet = nullptr;
		m_Integrate.bindingSet = nullptr;
		m_GridWidth = gridWidth;
		m_GridHeight = gridHeight;
		m_GridSlices = gridSlices;
		idLib::Warning( "VolumetricFogPass: the device could not make the fog's volumes of %d x %d x %d froxels, no fog until the grid changes", gridWidth, gridHeight, gridSlices );
		return false;
	}

	nvrhi::BindingSetDesc injectDesc;
	injectDesc.bindings =
	{
		nvrhi::BindingSetItem::ConstantBuffer( 1, m_ConstantBuffer ),
		nvrhi::BindingSetItem::Texture_UAV( 0, m_Injected )
	};
	m_Inject.bindingSet = m_Device->createBindingSet( injectDesc, m_Inject.layout );

	nvrhi::BindingSetDesc resolveDesc;
	resolveDesc.bindings =
	{
		nvrhi::BindingSetItem::ConstantBuffer( 1, m_ConstantBuffer ),
		nvrhi::BindingSetItem::Texture_SRV( 0, m_Injected ),
		nvrhi::BindingSetItem::Texture_UAV( 0, m_Resolved )
	};
	m_Resolve.bindingSet = m_Device->createBindingSet( resolveDesc, m_Resolve.layout );

	nvrhi::BindingSetDesc integrateDesc;
	integrateDesc.bindings =
	{
		nvrhi::BindingSetItem::ConstantBuffer( 1, m_ConstantBuffer ),
		nvrhi::BindingSetItem::Texture_SRV( 0, m_Resolved ),
		nvrhi::BindingSetItem::Texture_UAV( 0, m_Integrated )
	};
	m_Integrate.bindingSet = m_Device->createBindingSet( integrateDesc, m_Integrate.layout );

	m_ProbeWait = 0;						// a probe under way was of the old volume

	m_GridWidth = gridWidth;
	m_GridHeight = gridHeight;
	m_GridSlices = gridSlices;
	return true;
}

/*
========================
VolumetricFogPass::DrawSkyMask

Marks the pixels of the fog view that show a sky surface ( a material of sort SS_PORTAL_SKY: it writes depth, so by
its depth the sky would stand as near as the brush that carries it ). The apply fogs a marked pixel as infinitely
far ( D52 ). Called right behind the depth prepass: the sky surfaces are drawn once more with the engine's colour
program, depth test EQUAL and no depth write, into a mask that shares the view's depth image, so a mark stands
exactly where the prepass left a sky surface visible. It opens no main block; the caller resets the graphics
state cache behind it.
========================
*/
void VolumetricFogPass::DrawSkyMask( nvrhi::ICommandList* commandList, const viewDef_t* viewDef )
{
	m_SkyMask = NULL;
	m_SkyMaskView = NULL;

	if( !FroxelHasMedium() )
	{
		return;					// clear air: the pass does nothing in this view
	}

	nvrhi::ITexture* depth = globalImages->currentDepthImage->GetTextureHandle().Get();
	if( depth == NULL )
	{
		return;
	}

	// The target is an engine framebuffer, because the engine's draw path keys its pipelines by one. The engine
	// deletes every framebuffer on a resize, behind ClearCaches, so no pointer to it is kept past this call; a pass
	// made later ( a map load, the cvar ) finds and takes over the one an earlier pass made
	Framebuffer* target = Framebuffer::Find( FROXEL_SKYMASK_TARGET );
	if( target == NULL )
	{
		const nvrhi::TextureDesc& depthDesc = depth->getDesc();

		nvrhi::TextureDesc desc;
		desc.width = depthDesc.width;
		desc.height = depthDesc.height;
		desc.format = nvrhi::Format::R8_UNORM;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "Fog/SkyMask";
		nvrhi::TextureHandle mask = m_Device->createTexture( desc );

		target = new Framebuffer( FROXEL_SKYMASK_TARGET, nvrhi::FramebufferDesc().addColorAttachment( mask ).setDepthAttachment( depth ) );
		m_SkyMaskEmpty = false;				// a new texture holds anything
	}
	const nvrhi::FramebufferDesc& targetDesc = target->GetApiObject()->getDesc();
	if( targetDesc.depthAttachment.texture != depth )
	{
		// Made for another depth image: no mask, and the apply then leaves the view unfogged. The engine re-makes
		// the depth image only where it deletes every framebuffer too ( Framebuffer::ResizeFramebuffers ), so this
		// is the fault of a later change: say so, once
		static bool warned = false;
		if( !warned )
		{
			warned = true;
			idLib::Warning( "VolumetricFogPass: the framebuffer %s belongs to another depth image, the view gets no fog", FROXEL_SKYMASK_TARGET );
		}
		return;
	}
	nvrhi::ITexture* mask = targetDesc.colorAttachments[0].texture;

	// cleared only when it holds marks, or when this pass does not know what it holds ( a mask taken over )
	if( !m_SkyMaskEmpty )
	{
		commandList->clearTextureFloat( mask, nvrhi::AllSubresources, nvrhi::Color( 0.0f ) );
		m_SkyMaskEmpty = true;
	}

	Framebuffer* previous = backEnd.currentFrameBuffer;
	bool bound = false;
	for( int i = 0; i < viewDef->numDrawSurfs; i++ )
	{
		const drawSurf_t* surf = viewDef->drawSurfs[i];
		const idMaterial* material = surf->material;
		if( material == NULL || material->GetSort() != SS_PORTAL_SKY || material->Coverage() == MC_TRANSLUCENT )
		{
			continue;			// not a sky surface, or one that left no depth
		}
		if( !bound )
		{
			target->Bind();
			backEnd.GL_State( GLS_DEPTHMASK | GLS_DEPTHFUNC_EQUAL );
			backEnd.GL_Color( 1.0f, 1.0f, 1.0f, 1.0f );
			backEnd.currentSpace = NULL;
			bound = true;
			m_SkyMaskEmpty = false;
		}
		if( surf->space != backEnd.currentSpace )
		{
			RB_SetMVP( surf->space->mvp );
			backEnd.currentSpace = surf->space;
		}
		if( surf->jointCache )
		{
			renderProgManager.BindShader_ColorSkinned();
		}
		else
		{
			renderProgManager.BindShader_Color();
		}
		backEnd.DrawElementsWithCounters( surf );
	}
	if( bound && previous != NULL )
	{
		previous->Bind();
	}

	m_SkyMask = mask;
	m_SkyMaskView = viewDef;
}

/*
========================
VolumetricFogPass::Render

The three kernels of the fog view, in the backend's MRB_VOLUMETRICS block. The medium is the same everywhere
( the global fog, lit by the furnace radiance alone ), and the resolve copies.
========================
*/
void VolumetricFogPass::Render( nvrhi::ICommandList* commandList, const viewDef_t* viewDef )
{
	m_RenderedView = NULL;

	// a probe's copy is read some fog views after it was made
	if( m_ProbeWait > 0 && --m_ProbeWait == 0 )
	{
		ProbeRead();
	}

	// clear air: no medium in any froxel, so the kernels and the apply have nothing to do and the view stays unfogged
	if( !FroxelHasMedium() )
	{
		if( fogProbeRequested )
		{
			fogProbeRequested = false;
			common->Printf( "fogprobe: the air is clear ( r_volumetricFogDistance 0 ), there is nothing to read\n" );
		}
		return;
	}
	// no sky mask for this view ( DrawSkyMask could not make one ): the apply will not run, so nothing is filled
	if( m_SkyMaskView != viewDef || m_SkyMask == NULL )
	{
		return;
	}

	// The global fog: sigma_t from its distance, sigma_s from the albedo, the in-scatter per unit from the furnace.
	// What goes into the half-float volumes stays finite ( FROXEL_STORE_MAX ): a distance above 0 counts as at least
	// FROXEL_MIN_FOG_DISTANCE, and the in-scatter of a furnace too bright for the store is cut
	const float fogDistance = r_volumetricFogDistance.GetFloat();
	const float sigmaT = ( fogDistance > 0.0f ) ? FROXEL_OPACITY_AT_D / Max( fogDistance, FROXEL_MIN_FOG_DISTANCE ) : 0.0f;
	const float sigmaS = sigmaT * idMath::ClampFloat( 0.0f, 1.0f, r_volumetricFogAlbedo.GetFloat() );
	const float source = Min( sigmaS * Max( 0.0f, r_volumetricFogFurnace.GetFloat() ), FROXEL_STORE_MAX / FROXEL_STORE_SCALE );

	// The volumes are sized by the render size, which is the sky mask's, not by the view: a view that changes its
	// size ( the engine's dynamic resolution ) fills a part of them and nothing is made again. A grid of more than
	// FROXEL_MAX_CELLS froxels gets wider columns
	const nvrhi::TextureDesc& renderDesc = m_SkyMask->getDesc();
	const int gridSlices = idMath::ClampInt( 8, 256, r_volumetricFogSlices.GetInteger() );
	const int wantedColumnPixels = idMath::ClampInt( 4, 64, r_volumetricFogColumnPixels.GetInteger() );
	int columnPixels = wantedColumnPixels;
	while( columnPixels < 64 && ( ( ( int )renderDesc.width + columnPixels - 1 ) / columnPixels ) * ( ( ( int )renderDesc.height + columnPixels - 1 ) / columnPixels ) * gridSlices > FROXEL_MAX_CELLS )
	{
		columnPixels *= 2;
	}
	if( columnPixels != wantedColumnPixels )
	{
		static bool warned = false;
		if( !warned )
		{
			warned = true;
			idLib::Warning( "VolumetricFogPass: r_volumetricFogColumnPixels %d with %d slices is more than %d froxels at this render size, the columns are %d pixels", wantedColumnPixels, gridSlices, FROXEL_MAX_CELLS, columnPixels );
		}
	}
	if( !CheckVolumes( ( ( int )renderDesc.width + columnPixels - 1 ) / columnPixels, ( ( int )renderDesc.height + columnPixels - 1 ) / columnPixels, gridSlices ) )
	{
		return;
	}
	m_ColumnPixels = columnPixels;
	const float farEnd = idMath::ClampFloat( 64.0f, 65536.0f, r_volumetricFogFar.GetFloat() );
	m_SliceStep = idMath::Log( 1.0f + farEnd / FROXEL_SLICE_BIAS ) / gridSlices;

	// the view's grid: the part of the volumes this view's kernels fill and its lookup reads
	const int viewWidth = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	const int viewHeight = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;
	const int gridWidth = Min( ( viewWidth + columnPixels - 1 ) / columnPixels, m_GridWidth );
	const int gridHeight = Min( ( viewHeight + columnPixels - 1 ) / columnPixels, m_GridHeight );
	m_ViewGridWidth = gridWidth;
	m_ViewGridHeight = gridHeight;
	// The kernels fill one column and one row more, where the volumes have them: at the view's last column and row
	// the lookup's filter also fetches the next froxel, at weight 0, and a froxel that nothing wrote may hold anything
	const int fillWidth = Min( gridWidth + 1, m_GridWidth );
	const int fillHeight = Min( gridHeight + 1, m_GridHeight );

	// the tangents of the view's half angles; these two elements of the projection carry no jitter
	const float tanHalfX = 1.0f / viewDef->projectionMatrix[0 * 4 + 0];
	const float tanHalfY = 1.0f / idMath::Fabs( viewDef->projectionMatrix[1 * 4 + 1] );

	froxelConstants_t constants;
	constants.gridSize.Set( ( float )fillWidth, ( float )fillHeight, ( float )gridSlices, 0.0f );
	constants.sliceParms.Set( FROXEL_SLICE_BIAS, m_SliceStep, 1.0f / FROXEL_STORE_SCALE, 0.0f );
	// a column's centre in the view's -1..1, times the tangent: how far the ray moves sideways per unit forward
	constants.rayParms.Set( 2.0f * columnPixels / viewWidth * tanHalfX, 2.0f * columnPixels / viewHeight * tanHalfY, tanHalfX, tanHalfY );
	constants.medium.Set( source * FROXEL_STORE_SCALE, source * FROXEL_STORE_SCALE, source * FROXEL_STORE_SCALE, sigmaT * FROXEL_STORE_SCALE );
	commandList->writeBuffer( m_ConstantBuffer, &constants, sizeof( constants ) );

	const int groupsX = ( fillWidth + 7 ) / 8;
	const int groupsY = ( fillHeight + 7 ) / 8;
	nvrhi::ComputeState state;
	{
		OPTICK_GPU_EVENT( "Fog_Inject" );
		state.pipeline = m_Inject.pipeline;
		state.bindings = { m_Inject.bindingSet };
		commandList->setComputeState( state );
		commandList->dispatch( groupsX, groupsY, gridSlices );
	}
	{
		OPTICK_GPU_EVENT( "Fog_Resolve" );
		state.pipeline = m_Resolve.pipeline;
		state.bindings = { m_Resolve.bindingSet };
		commandList->setComputeState( state );
		commandList->dispatch( groupsX, groupsY, gridSlices );
	}
	{
		// one thread per column: it walks the slices from the camera outward
		OPTICK_GPU_EVENT( "Fog_Integrate" );
		state.pipeline = m_Integrate.pipeline;
		state.bindings = { m_Integrate.bindingSet };
		commandList->setComputeState( state );
		commandList->dispatch( groupsX, groupsY, 1 );
	}

	// the probe: the console command asks for one, and it starts when none is under way
	if( fogProbeRequested && m_ProbeWait == 0 )
	{
		fogProbeRequested = false;
		m_ProbeSlices = gridSlices;
		m_ProbeColumn[0][0] = gridWidth / 2;
		m_ProbeColumn[0][1] = gridHeight / 2;
		m_ProbeColumn[1][0] = 0;
		m_ProbeColumn[1][1] = 0;
		m_ProbeColumn[2][0] = 0;
		m_ProbeColumn[2][1] = gridHeight / 2;
		m_ProbeRayParms = constants.rayParms;
		m_ProbeSliceStep = m_SliceStep;
		m_ProbeSigmaT = sigmaT;
		m_ProbeSource = source;
		ProbeCopy( commandList );
		m_ProbeWait = FROXEL_PROBE_WAIT;
	}

	m_RenderedView = viewDef;
}

/*
========================
VolumetricFogPass::ProbeCopy

Copies the probe's columns out of the integrated volume into a strip: a row per column, a texel per slice. The
strip is a 3D texture of depth 1, so that every copy is between textures of one kind ( 3D to 3D ) and the staging
texture, of depth 1 too, is mapped as one block by both APIs. ProbeStage, behind the apply of the same view,
sends the strip to its staging texture.
========================
*/
void VolumetricFogPass::ProbeCopy( nvrhi::ICommandList* commandList )
{
	if( !m_ProbeStrip || ( int )m_ProbeStrip->getDesc().width != m_ProbeSlices )
	{
		nvrhi::TextureDesc desc;
		desc.dimension = nvrhi::TextureDimension::Texture3D;
		desc.width = m_ProbeSlices;
		desc.height = FROXEL_PROBE_COLUMNS;
		desc.depth = 1;
		desc.format = nvrhi::Format::RGBA16_FLOAT;
		desc.initialState = nvrhi::ResourceStates::CopyDest;
		desc.keepInitialState = true;
		desc.debugName = "Fog/ProbeStrip";
		m_ProbeStrip = m_Device->createTexture( desc );

		nvrhi::TextureDesc stagingDesc;
		stagingDesc.dimension = nvrhi::TextureDimension::Texture3D;
		stagingDesc.width = m_ProbeSlices;
		stagingDesc.height = FROXEL_PROBE_COLUMNS;
		stagingDesc.depth = 1;
		stagingDesc.format = nvrhi::Format::RGBA16_FLOAT;
		stagingDesc.debugName = "Fog/ProbeStaging";
		m_ProbeStaging = m_Device->createStagingTexture( stagingDesc, nvrhi::CpuAccessMode::Read );
	}
	if( !m_ProbePixel )
	{
		nvrhi::TextureDesc desc;
		desc.width = 1;
		desc.height = 1;
		desc.format = nvrhi::Format::RGBA16_FLOAT;
		desc.initialState = nvrhi::ResourceStates::CopyDest;
		desc.keepInitialState = true;
		desc.debugName = "Fog/ProbePixel";
		m_ProbePixel = m_Device->createTexture( desc );

		nvrhi::TextureDesc stagingDesc;
		stagingDesc.width = 1;
		stagingDesc.height = 1;
		stagingDesc.format = nvrhi::Format::RGBA16_FLOAT;
		stagingDesc.debugName = "Fog/ProbePixelStaging";
		m_ProbePixelStaging = m_Device->createStagingTexture( stagingDesc, nvrhi::CpuAccessMode::Read );
	}

	for( int c = 0; c < FROXEL_PROBE_COLUMNS; c++ )
	{
		for( int k = 0; k < m_ProbeSlices; k++ )
		{
			commandList->copyTexture( m_ProbeStrip, nvrhi::TextureSlice().setOrigin( k, c, 0 ).setSize( 1, 1, 1 ),
									  m_Integrated, nvrhi::TextureSlice().setOrigin( m_ProbeColumn[c][0], m_ProbeColumn[c][1], k ).setSize( 1, 1, 1 ) );
		}
	}
	m_ProbeStaged = false;
	m_ProbeHasPixel = false;
}

/*
========================
VolumetricFogPass::ProbeStage

The probe's second half, behind the apply of the view the probe started in: the centre pixel of the view's
colour target as the apply left it ( under r_volumetricFogDebug 1 that is the transmittance to the pixel ) goes
through a texture of one pixel into a staging texture of its own, and the strip into its staging texture.
========================
*/
void VolumetricFogPass::ProbeStage( nvrhi::ICommandList* commandList, nvrhi::ITexture* color, const viewDef_t* viewDef )
{
	if( color->getDesc().format == nvrhi::Format::RGBA16_FLOAT )
	{
		const int centreX = ( viewDef->viewport.x1 + viewDef->viewport.x2 + 1 ) / 2;
		const int centreY = ( viewDef->viewport.y1 + viewDef->viewport.y2 + 1 ) / 2;
		commandList->copyTexture( m_ProbePixel, nvrhi::TextureSlice(),
								  color, nvrhi::TextureSlice().setOrigin( centreX, centreY, 0 ).setSize( 1, 1, 1 ) );
		commandList->copyTexture( m_ProbePixelStaging, nvrhi::TextureSlice(), m_ProbePixel, nvrhi::TextureSlice() );
		m_ProbeHasPixel = true;
	}
	commandList->copyTexture( m_ProbeStaging, nvrhi::TextureSlice(), m_ProbeStrip, nvrhi::TextureSlice() );
	m_ProbeStaged = true;
}

/*
========================
VolumetricFogPass::ProbeRead

Prints the probe: per column the transmittance and the in-scatter of some slices, and how far every slice is
from the closed form of a uniform medium along the column's ray, worked out here in double precision:
T = exp( -sigma_t * far * | dir | ) and in-scatter = sigma_s * L / sigma_t * ( 1 - T ). With the furnace radiance
L and albedo 1 the in-scatter must be L * ( 1 - T ): the fog furnace. Then the picture's centre pixel as the
apply left it, and the GPU time of the fog's two blocks as the engine last measured them.
========================
*/
void VolumetricFogPass::ProbeRead()
{
	if( !m_ProbeStaged )
	{
		common->Printf( "fogprobe: the probe's view had no apply, nothing was read\n" );
		return;
	}

	size_t rowPitch = 0;
	const byte* data = ( const byte* )m_Device->mapStagingTexture( m_ProbeStaging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch );
	if( data == NULL )
	{
		common->Printf( "fogprobe: the read-back failed\n" );
		return;
	}

	const double step = m_ProbeSliceStep;
	common->Printf( "fogprobe: %d slices, far end %.1f, sigma_t %.6g, sigma_s * L %.6g\n", m_ProbeSlices,
					FROXEL_SLICE_BIAS * ( exp( m_ProbeSlices * step ) - 1.0 ), m_ProbeSigmaT, m_ProbeSource );
	for( int c = 0; c < FROXEL_PROBE_COLUMNS; c++ )
	{
		const halfFloat_t* row = ( const halfFloat_t* )( data + c * rowPitch );
		const double lateralX = ( m_ProbeColumn[c][0] + 0.5 ) * m_ProbeRayParms.x - m_ProbeRayParms.z;
		const double lateralY = ( m_ProbeColumn[c][1] + 0.5 ) * m_ProbeRayParms.y - m_ProbeRayParms.w;
		const double rayLength = sqrt( 1.0 + lateralX * lateralX + lateralY * lateralY );

		double maxErrorT = 0.0;
		double maxErrorS = 0.0;
		for( int k = 0; k < m_ProbeSlices; k++ )
		{
			const double farFace = FROXEL_SLICE_BIAS * ( exp( ( k + 1 ) * step ) - 1.0 );
			const double length = farFace * rayLength;
			const double wantT = exp( -m_ProbeSigmaT * length );
			const double wantS = ( m_ProbeSigmaT > 0.0f ) ? m_ProbeSource / m_ProbeSigmaT * ( 1.0 - wantT ) : m_ProbeSource * length;
			const double gotS = F16toF32( row[k * 4 + 0] );
			const double gotT = F16toF32( row[k * 4 + 3] );
			maxErrorT = Max( maxErrorT, fabs( gotT - wantT ) );
			maxErrorS = Max( maxErrorS, fabs( gotS - wantS ) );
			if( k < 2 || ( ( k + 1 ) & k ) == 0 || k == m_ProbeSlices - 1 )
			{
				common->Printf( "fogprobe:   column %d,%d slice %2d far %8.2f  T %.5f ( closed form %.5f )  in-scatter %.5f ( %.5f )\n",
								m_ProbeColumn[c][0], m_ProbeColumn[c][1], k, farFace, gotT, wantT, gotS, wantS );
			}
		}
		common->Printf( "fogprobe: column %d,%d ray length %.5f: largest error of T %.6f, of the in-scatter %.6f\n",
						m_ProbeColumn[c][0], m_ProbeColumn[c][1], rayLength, maxErrorT, maxErrorS );
	}
	m_Device->unmapStagingTexture( m_ProbeStaging );

	if( m_ProbeHasPixel )
	{
		const halfFloat_t* pixel = ( const halfFloat_t* )m_Device->mapStagingTexture( m_ProbePixelStaging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch );
		if( pixel != NULL )
		{
			common->Printf( "fogprobe: the picture's centre pixel behind the apply: %.5f %.5f %.5f\n", F16toF32( pixel[0] ), F16toF32( pixel[1] ), F16toF32( pixel[2] ) );
			m_Device->unmapStagingTexture( m_ProbePixelStaging );
		}
	}

	// the GPU time of the fog's two blocks as the engine last measured them ( the HUD's "Volumetrics" and "Fog apply" )
	common->Printf( "fogprobe: GPU microseconds: Volumetrics %d, Fog apply %d\n", ( int )commonLocal.GetRendererGpuVolumetricsMicroseconds(), ( int )commonLocal.GetRendererGpuVolumetricsApplyMicroseconds() );
}

/*
========================
VolumetricFogPass::Apply

Fogs the opaque picture of the fog view, in the backend's MRB_VOLUMETRICS_APPLY block: one triangle over the
view, picture = in-scatter + picture * transmittance by blending, so the picture is not read. It writes the fog
set's real constants ( D66 ) and hands the integrated volume to the backend's fog set ( D27 ); the caller resets
the graphics state cache behind it.
========================
*/
void VolumetricFogPass::Apply( nvrhi::ICommandList* commandList, const viewDef_t* viewDef )
{
	if( m_RenderedView != viewDef || m_SkyMaskView != viewDef || m_SkyMask == NULL )
	{
		return;					// Render() did not fill the volumes for this view, or the view has no sky mask
	}
	m_RenderedView = NULL;
	m_SkyMaskView = NULL;

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

	// The scene depth comes from mip 0 of the Hi-Z pyramid, a copy the backend makes right behind the depth prepass:
	// the depth attachment itself is neither sampled nor rebound here ( D83 ). Without the pyramid the depth image
	// is sampled, which costs its two transitions.
	nvrhi::ITexture* depth = R_UseHiZ() ? globalImages->hierarchicalZbufferImage->GetTextureHandle().Get() : globalImages->currentDepthImage->GetTextureHandle().Get();
	if( depth == NULL )
	{
		return;
	}

	if( !m_ApplyTarget || m_ApplyColor != color )
	{
		m_ApplyTarget = m_Device->createFramebuffer( nvrhi::FramebufferDesc().addColorAttachment( color ) );
		m_ApplyColor = color;
		m_ApplyPipeline = nullptr;			// a pipeline is made for a framebuffer's formats
	}
	if( !m_ApplyPipeline )
	{
		nvrhi::GraphicsPipelineDesc psoDesc;
		psoDesc.bindingLayouts = { m_ApplyLayout };
		psoDesc.VS = m_ApplyVS;
		psoDesc.PS = m_ApplyPS;
		psoDesc.primType = nvrhi::PrimitiveType::TriangleList;
		psoDesc.renderState.rasterState.setCullNone();
		psoDesc.renderState.depthStencilState.depthTestEnable = false;
		psoDesc.renderState.depthStencilState.depthWriteEnable = false;
		psoDesc.renderState.depthStencilState.stencilEnable = false;
		// source * ONE + destination * SRC_ALPHA: the shader returns the in-scatter and the transmittance. The
		// target's own alpha stays
		psoDesc.renderState.blendState.targets[0].blendEnable = true;
		psoDesc.renderState.blendState.targets[0].srcBlend = nvrhi::BlendFactor::One;
		psoDesc.renderState.blendState.targets[0].destBlend = nvrhi::BlendFactor::SrcAlpha;
		psoDesc.renderState.blendState.targets[0].srcBlendAlpha = nvrhi::BlendFactor::Zero;
		psoDesc.renderState.blendState.targets[0].destBlendAlpha = nvrhi::BlendFactor::One;
		m_ApplyPipeline = m_Device->createGraphicsPipeline( psoDesc, m_ApplyTarget );
	}

	const int viewWidth = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	const int viewHeight = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;

	froxelFogParms_t parms;
	parms.volumeParms.Set( ( float )viewWidth / ( m_GridWidth * m_ColumnPixels ), ( float )viewHeight / ( m_GridHeight * m_ColumnPixels ), ( float )m_GridSlices, 1.0f / m_GridSlices );
	parms.depthParms.Set( FROXEL_SLICE_BIAS, m_SliceStep, viewDef->projectionMatrix[3 * 4 + 2], viewDef->projectionMatrix[2 * 4 + 2] );
	parms.fogParms.Set( 1.0f, ( float )r_volumetricFogDebug.GetInteger(), ( float )viewDef->viewport.x1, ( float )viewDef->viewport.y1 );
	parms.reserved.Set( 1.0f / viewWidth, 1.0f / viewHeight, ( m_ViewGridWidth - 0.5f ) / m_GridWidth, ( m_ViewGridHeight - 0.5f ) / m_GridHeight );
	commandList->writeBuffer( backEnd.froxelFogParms, &parms, sizeof( parms ) );

	// from here on the engine's fog set binds this view's volume in place of the dummy
	backEnd.froxelVolume = m_Integrated;

	// the set comes from the engine's cache, which ReleaseVolume() clears where the volume dies
	nvrhi::BindingSetDesc bindingSetDesc;
	bindingSetDesc.bindings =
	{
		nvrhi::BindingSetItem::ConstantBuffer( 1, backEnd.froxelFogParms ),
		nvrhi::BindingSetItem::Texture_SRV( 15, m_Integrated ),
		nvrhi::BindingSetItem::Sampler( 5, commonRenderPasses->m_LinearClampSampler ),
		nvrhi::BindingSetItem::Texture_SRV( 0, depth ),
		nvrhi::BindingSetItem::Texture_SRV( 1, m_SkyMask )
	};
	nvrhi::BindingSetHandle bindingSet = backEnd.bindingCache.GetOrCreateBindingSet( bindingSetDesc, m_ApplyLayout );

	const nvrhi::Viewport viewport( ( float )viewDef->viewport.x1, ( float )( viewDef->viewport.x2 + 1 ), ( float )viewDef->viewport.y1, ( float )( viewDef->viewport.y2 + 1 ), 0.0f, 1.0f );
	nvrhi::GraphicsState state;
	state.indexBuffer = {};					// the fork leaves this one member uninitialized
	state.pipeline = m_ApplyPipeline;
	state.framebuffer = m_ApplyTarget;
	state.bindings = { bindingSet };
	state.viewport.addViewport( viewport );
	state.viewport.addScissorRect( nvrhi::Rect( viewport ) );
	commandList->setGraphicsState( state );

	nvrhi::DrawArguments args;
	args.instanceCount = 1;
	args.vertexCount = 3;
	commandList->draw( args );
	// a direct NVRHI draw goes past DrawElementsWithCounters
	if( backEnd.concurrencyStats )
	{
		backEnd.pc.passDraws[backEnd.currentPass]++;
	}

	// a probe that this view's Render() started reads the picture behind the apply
	if( m_ProbeWait == FROXEL_PROBE_WAIT )
	{
		ProbeStage( commandList, color, viewDef );
	}
}

// The volumetric fog pass's debug-tools overlay; X0 lands it as a no-op and track V fills the body.
void VolumetricFogPass::DebugTools( nvrhi::ICommandList* commandList, const viewDef_t* viewDef )
{
}
