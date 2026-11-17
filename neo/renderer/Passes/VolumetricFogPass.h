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
#ifndef RENDERER_PASSES_VOLUMETRICFOGPASS_H_
#define RENDERER_PASSES_VOLUMETRICFOGPASS_H_

/*
================================

VolumetricFogPass ( track V of the renderer upgrade, packages V1 and V2 )

Froxel fog: a 3D texture aligned with the fog view's frustum, a column per block of screen pixels and a slice per
step in depth. Three compute kernels fill it in every frame ( inject, resolve, integrate ), and the apply fogs the
opaque picture with one lookup per pixel at the pixel's depth, before the first translucent surface is drawn.

The inject lights the fog ( V2 ) with the view's ambient light and with the view's light records
( renderer/LightLists.h ), which the frontend builds in the fog view while r_volumetricFog is on and the backend
uploads right before this pass. No shadow is looked up yet ( V3 ): a light is taken where its shadow is known.

The conventions, the same in every kernel and in the apply ( shaders/builtin/volumetrics/froxel_grid.inc.hlsl,
shaders/froxel_fog.inc.hlsl ):
- the volumes are sized by the render size; a view fills the part its own size needs ( the view's grid ) and
  reads no froxel outside it.
- volume UV is the fog view's screen UV ( top-left origin, y down ) times volumeParms.xy: the view's last column
  and row may reach past the view's edge.
- slices are slabs of constant distance along the view axis. Slice k spans FroxelSliceFar( k - 1 ) to
  FroxelSliceFar( k ), biased logarithmic, and slice 0 begins at the camera.
- the injected and the resolved volume hold coefficients per unit times FROXEL_STORE_SCALE. Texel k of the
  integrated volume holds the in-scatter and the transmittance from the camera to the FAR face of slice k, so a
  point at slice coordinate s reads it at w = ( s - 0.5 ) / G.

The pass exists only while r_volumetricFog is on: the backend makes and deletes it in its TRACK V blocks of
GL_StartFrame and ClearCaches.

================================
*/

extern idCVar r_volumetricFog;

struct froxelView_t;			// opaque; track V defines it later

class VolumetricFogPass
{
public:
	VolumetricFogPass( nvrhi::IDevice* device, CommonRenderPasses* commonPasses );
	~VolumetricFogPass();

	void	DrawSkyMask( nvrhi::ICommandList* commandList, const viewDef_t* viewDef );	// opens no main block
	void	Render( nvrhi::ICommandList* commandList, const viewDef_t* viewDef );		// inject, resolve, integrate
	void	Apply( nvrhi::ICommandList* commandList, const viewDef_t* viewDef );		// the composite
	void	DebugTools( nvrhi::ICommandList* commandList, const viewDef_t* viewDef );	// track V's debug overlay, called from DBG_RenderDebugTools

private:
	struct kernel_t
	{
		nvrhi::ShaderHandle				shader;
		nvrhi::BindingLayoutHandle		layout;
		nvrhi::ComputePipelineHandle	pipeline;
		nvrhi::BindingSetHandle			bindingSet;		// made with the volumes
	};

	void	InitKernel( kernel_t& kernel, const char* shaderName, bool readsVolume );
	void	InitInject();
	bool	CheckVolumes( int gridWidth, int gridHeight, int gridSlices );
	bool	CheckInjectSet();
	void	ReleaseVolume();
	void	ProbeCopy( nvrhi::ICommandList* commandList );
	void	ProbeStage( nvrhi::ICommandList* commandList, nvrhi::ITexture* color, const viewDef_t* viewDef );
	void	ProbeRead();

	nvrhi::DeviceHandle				m_Device;
	CommonRenderPasses*				commonRenderPasses;

	nvrhi::BufferHandle				m_ConstantBuffer;	// froxelConstants_t, volatile: one write per fog view

	// the grid the volumes were made for, from the render size ( 0 until the first fog view ), and what the last
	// Render() used with it: the part of that grid the view's own size needs
	int								m_GridWidth;
	int								m_GridHeight;
	int								m_GridSlices;
	int								m_ColumnPixels;
	int								m_ViewGridWidth;
	int								m_ViewGridHeight;
	float							m_SliceStep;		// ln( 1 + farEnd / S ) / slices: the kernels and the fog set get the same number

	nvrhi::TextureHandle			m_Injected;			// sigma_s * L in rgb, sigma_t in a, times the store scale
	nvrhi::TextureHandle			m_Resolved;			// the same behind the temporal resolve ( V1: a copy )
	nvrhi::TextureHandle			m_Integrated;		// in-scatter in rgb, transmittance in a: camera to the slice's far face

	kernel_t						m_Inject;			// its layout is its own ( InitInject ) and its set is made by CheckInjectSet
	kernel_t						m_Resolve;
	kernel_t						m_Integrate;

	// what the inject's set was made for besides the pass's own objects: the light lists' four tables and the three
	// light images, which belong to the engine and can be made again under a living pass
	nvrhi::IResource*				m_InjectSetKey[7];

	// the apply: one triangle over the view, blended into the view's colour target
	nvrhi::ShaderHandle				m_ApplyVS;
	nvrhi::ShaderHandle				m_ApplyPS;
	nvrhi::BindingLayoutHandle		m_ApplyLayout;
	nvrhi::FramebufferHandle		m_ApplyTarget;		// the colour attachment of the view's framebuffer alone
	nvrhi::ITexture*				m_ApplyColor;		// the texture that target was made for
	nvrhi::GraphicsPipelineHandle	m_ApplyPipeline;

	const viewDef_t*				m_RenderedView;		// the view Render() filled the volumes for: its apply alone may read them

	// the sky mask: 1 where the fog view shows a sky surface, which the apply fogs as infinitely far ( D52 ). The
	// texture belongs to the engine framebuffer "_fogSkyMask"; the pointer is good for the frame of m_SkyMaskView
	nvrhi::ITexture*				m_SkyMask;
	const viewDef_t*				m_SkyMaskView;
	bool							m_SkyMaskEmpty;		// the mask is known to hold no mark: no clear is needed

	// the probe ( the console command fogProbe ): three columns of the integrated volume and the centre pixel of the
	// fogged picture, read back and printed
	nvrhi::TextureHandle			m_ProbeStrip;		// a row per column with a texel per slice; 3D of depth 1, as the volume it is copied from is 3D
	nvrhi::StagingTextureHandle		m_ProbeStaging;
	nvrhi::TextureHandle			m_ProbePixel;		// the picture's centre pixel, 1 x 1
	nvrhi::StagingTextureHandle		m_ProbePixelStaging;
	int								m_ProbeWait;		// fog views until the copy is read; 0 = no probe under way
	bool							m_ProbeStaged;		// the strip went into the staging texture ( the probe's frame had its apply )
	bool							m_ProbeHasPixel;	// and the picture's centre pixel is in it
	int								m_ProbeSlices;		// the frame the copy was made in: its grid, its constants
	int								m_ProbeColumn[3][2];
	idVec4							m_ProbeRayParms;
	float							m_ProbeSliceStep;
	float							m_ProbeSigmaT;
	float							m_ProbeSource;		// sigma_s * L
	bool							m_ProbeLit;			// the fog was lit ( no furnace ): its in-scatter has no closed form
	int								m_ProbeRecords;		// the light records of the view
	int								m_ProbeTakeable;	// and how many of them the fog could take ( the take mask's bits )
	int								m_ProbeCull;		// and how it found them
	float							m_ProbeSigmaS;		// the lit fog's sigma_s ( m_ProbeSource is the furnace's share and 0 then )
};

#endif // RENDERER_PASSES_VOLUMETRICFOGPASS_H_
