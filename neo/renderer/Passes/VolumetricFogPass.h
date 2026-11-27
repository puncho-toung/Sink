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

#include "VolumetricFogPass_cb.h"		// V4: the media list's entry ( fogMedium_t ) and the kernels' constants

/*
================================

VolumetricFogPass ( track V of the renderer upgrade, packages V1 and V2 )

Froxel fog: a 3D texture aligned with the fog view's frustum, a column per block of screen pixels and a slice per
step in depth. Three compute kernels fill it in every frame ( inject, resolve, integrate ), and the apply fogs the
opaque picture with one lookup per pixel at the pixel's depth, before the first translucent surface is drawn.

The inject lights the fog ( V2 ) with the view's ambient light and with the view's light records
( renderer/LightLists.h ), which the frontend builds in the fog view while r_volumetricFog is on and the backend
uploads right before this pass. V3: a light's shadow is looked up in the shadow atlas, one comparison per froxel.

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

// V6 ( row 182 ): the zones, GPU timer queries the pass keeps round its sky mask and each kernel ( r_volumetricFogZones ).
// The render log's main blocks are nearly all taken ( RenderLog.h ) and Optick is off in the build, so the pass times
// itself. backEndCounters_t::gpuFogZoneMicroSec is indexed by these
enum fogZone_t
{
	FOG_ZONE_SKY,			// DrawSkyMask's clear and draws, in no main block
	FOG_ZONE_COLUMNS,		// the kernels, in the block MRB_VOLUMETRICS
	FOG_ZONE_MEDIA,
	FOG_ZONE_INJECT,
	FOG_ZONE_RESOLVE,
	FOG_ZONE_INTEGRATE,
	FOG_ZONE_COUNT
};

class VolumetricFogPass
{
public:
	VolumetricFogPass( nvrhi::IDevice* device, CommonRenderPasses* commonPasses );
	~VolumetricFogPass();

	void	DrawSkyMask( nvrhi::ICommandList* commandList, const viewDef_t* viewDef );	// opens no main block
	void	Render( nvrhi::ICommandList* commandList, const viewDef_t* viewDef );		// inject, resolve, integrate
	void	Apply( nvrhi::ICommandList* commandList, const viewDef_t* viewDef );		// the composite
	void	DebugTools( nvrhi::ICommandList* commandList, const viewDef_t* viewDef );	// track V's debug overlay, called from DBG_RenderDebugTools
	bool	ProbeStageTranslucent( nvrhi::ICommandList* commandList, const viewDef_t* viewDef );	// V5: a probe's centre pixel behind the translucents; true where it copied

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
	void	InitResolve();		// V3: the resolve reads three volumes and a sampler
	void	InitColumns();		// V3: the columns read the scene's depth and the sky mask
	void	InitMedia();		// V4: the media pass reads the media list and the columns
	int		BuildMedia( nvrhi::ICommandList* commandList, const viewDef_t* viewDef, int& numMovedBoxes );	// V4: the view's media list
	void	ProbeMedia( const froxelConstants_t& constants );				// V4: the media of the probe's columns, on the CPU
	bool	CheckVolumes( int gridWidth, int gridHeight, int gridSlices );
	bool	CheckInjectSet();
	void	ReleaseVolume();
	void	ProbeCopy( nvrhi::ICommandList* commandList );
	void	ProbeStage( nvrhi::ICommandList* commandList, nvrhi::ITexture* color, const viewDef_t* viewDef );
	void	ProbeRead();
	void	ProbeReadMedia( const byte* data, size_t rowPitch );	// V4: ProbeRead's columns where the media pass ran
	void	ZonesRead();												// V6: once a frame, the zones of the slot this frame reuses, into backEnd.pc
	void	ZoneBegin( nvrhi::ICommandList* commandList, fogZone_t zone );	// V6: a zone's two timestamps, while r_volumetricFogZones is on
	void	ZoneEnd( nvrhi::ICommandList* commandList, fogZone_t zone );
	void	BoundsBox( const viewDef_t* viewDef, const froxelConstants_t& constants, int viewWidth, int viewHeight, int fillWidth, int fillHeight );	// V8 ( B0, B2, B4 ): the box of the view's bounded media

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
	nvrhi::TextureHandle			m_Resolved[2];		// the same behind the temporal resolve: this frame's and last frame's by turns ( V3; m_ResolvedIndex is this frame's )
	nvrhi::TextureHandle			m_State;			// V3: per froxel the light change since last frame and the history's use there ( the inject writes it, the resolve reads it )
	nvrhi::TextureHandle			m_Columns;			// V3: per column of the grid the farthest surface its pixels show ( the skip )
	nvrhi::TextureHandle			m_Integrated;		// in-scatter in rgb, transmittance in a: camera to the slice's far face
	nvrhi::TextureHandle			m_Media;			// V4: sigma_s in rgb, sigma_t in a, times the store scale: the media pass's sum, which the inject reads
	nvrhi::TextureHandle			m_Mark[2];			// V4: 1 where the inject skipped the froxel, in step with m_Resolved ( the resolve writes mark i in turn i, reads the other )

	kernel_t						m_Inject;			// its layout is its own ( InitInject ) and its set is made by CheckInjectSet
	// V8 ( B1 ): the inject's permutation FROXEL_CLEAR_AIR 1, which returns early in clear air: m_Inject's layout and set
	nvrhi::ShaderHandle				m_InjectClearAirShader;
	nvrhi::ComputePipelineHandle	m_InjectClearAir;
	kernel_t						m_Resolve;			// V3: its set is one of m_ResolveSets, by turns
	kernel_t						m_Integrate;		// V3: its set is one of m_IntegrateSets, by turns
	kernel_t						m_ColumnsKernel;	// V3: the farthest surface of each column; its set comes from the engine's binding cache
	kernel_t						m_MediaKernel;		// V4: the media pass; its set is made with the volumes
	nvrhi::BufferHandle				m_MediaList;		// V4: the view's media, fogMedium_t[ FOG_MEDIA_MAX ]
	nvrhi::BufferHandle				m_MediaMoved;		// V4: by medium that moved, came or went, its last and its present bounds: 4 * FOG_MEDIA_MAX float4 ( min, max, min, max )
	fogMedium_t						m_MediaCopy[ FOG_MEDIA_MAX ];	// V4: what the list holds this view, for the probe
	int								m_NumMedia;
	bool							m_PrevBoxValid;		// V4: the test box of the last fog view, in map units: the inject clips the history where it moved
	idVec3d							m_PrevBoxMins;
	idVec3d							m_PrevBoxMaxs;
	// V3: the history. The resolved volumes take turns: this frame's is m_Resolved[ m_ResolvedIndex ], last frame's the
	// other, and set i of each kernel is the one for m_ResolvedIndex i
	nvrhi::BindingSetHandle			m_ResolveSets[2];
	nvrhi::BindingSetHandle			m_IntegrateSets[2];
	int								m_ResolvedIndex;
	// ---- WP-J21: the fog's glow ( the bloom's gate ). The inject's and the integrate's USE_GLOW forms also keep, per
	// froxel, the luminance of the in-scatter's part that never glows, and its integral; the bloom's level-1 pass reads it
	// beside the integrated volume ( backEnd.froxelNoGlowVolume ). Made with the volumes; the forms run only in a view under
	// the gate with r_bloomFogGlow ( BloomPass::FogGlowWantedForView ) ----
	nvrhi::TextureHandle			m_NoGlow;
	nvrhi::TextureHandle			m_NoGlowIntegrated;
	nvrhi::ShaderHandle				m_InjectGlowShader;
	nvrhi::ShaderHandle				m_InjectGlowClearAirShader;
	nvrhi::BindingLayoutHandle		m_InjectGlowLayout;
	nvrhi::ComputePipelineHandle	m_InjectGlow;
	nvrhi::ComputePipelineHandle	m_InjectGlowClearAir;
	nvrhi::BindingSetHandle			m_InjectGlowSet;
	kernel_t						m_IntegrateGlow;
	nvrhi::BindingSetHandle			m_IntegrateGlowSets[2];
	bool							m_GlowDead;			// a part could not be made: the fog gives no glow under the gate
	bool							m_NoGlowFresh;		// the volumes were just made: the first glow view clears m_NoGlow
	bool							m_GlowRendered;		// the last Render ran the glow forms ( Apply hands the volume to the bloom )
	// ---- END WP-J21 ----
	nvrhi::BufferHandle				m_PrevRecords;		// last fog view's light records, copied after its inject: their colours, flags and volumes
	nvrhi::BufferHandle				m_MovedRecords;		// the indices in m_PrevRecords of this view's lights that moved, or whose share jumped
	nvrhi::BufferHandle				m_CasterBounds;		// by record, 2 * FOG_CASTER_BOXES float4: where a caster that changed shadows ( froxelView_t::casterBoxes: up to FOG_CASTER_BOXES by record )
	int								m_PrevNumRecords;
	bool							m_HistoryValid;		// the other resolved volume holds the last fog view's fog, and m_PrevRecords its records
	int								m_HistoryFrame;		// that view's frame ( lightListView_t::viewFrame; 0 = it had no records )
	int								m_HistoryGrid[8];	// and what its fog was made with: the grid's columns and rows, the column side, the slices, the furnace, the view's width and height, V4's path
	float							m_HistorySliceStep;
	idRenderMatrix					m_HistoryMVP;		// and the view's unjittered MVP and render origin: the engine's last-frame
	idVec3d							m_HistoryOrigin;	// state ( H10 ) for the eye must be these
	float							m_HistoryMedium;	// and its sigma_t times the store scale: the alpha of its resolved froxels
	byte							m_PrevFogBits[ MAX_RECORD_LIGHTS ];	// of each of its records: 0x80 the fog took it, bits 0 .. 5 its rows that had a place
	int								m_JitterIndex;		// the fog views so far: the jitter's place in its sequence

	// what the inject's set was made for besides the pass's own objects: the light lists' four tables, the three
	// light images, the shadow atlas and the list of vanished lights, which belong to the engine and can be made
	// again under a living pass
	nvrhi::IResource*				m_InjectSetKey[9];

	// the apply: one triangle over the view, blended into the view's colour target
	nvrhi::ShaderHandle				m_ApplyVS;
	nvrhi::ShaderHandle				m_ApplyPS;
	nvrhi::BindingLayoutHandle		m_ApplyLayout;
	nvrhi::FramebufferHandle		m_ApplyTarget;		// the colour attachment of the view's framebuffer alone
	nvrhi::ITexture*				m_ApplyColor;		// the texture that target was made for
	nvrhi::GraphicsPipelineHandle	m_ApplyPipeline;

	const viewDef_t*				m_RenderedView;		// the view Render() filled the volumes for: its apply alone may read them
	bool							m_RenderedFogLights;	// V5: and whether its inject took fog lights
	// V8 ( B0, B2 ): and where its bounded media can be seen ( BoundsBox ): the fog set's rectangle and first slice, for its apply
	bool							m_BoundsOn;			// the rectangle and the first slice hold ( no map-wide medium, every fog light taken has its entry )
	idVec4							m_BoundsRect;		// froxelFogParms_t::boundsRect
	float							m_BoundsFirstSlice;	// froxelFogParms_t::boundsParms.y
	int								m_BoxMin[3];		// V8 ( B4 ): the box in froxels ( columns, rows, slices; inclusive ), padded by one; empty where the first passes the last
	int								m_BoxMax[3];
	bool							m_FogLightPath;		// V5f: a view since the last camera cut took fog lights: V4's path holds

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
	// V5: the same pixel once more behind the view's translucent shader passes ( the translucents fogged by their shaders )
	nvrhi::TextureHandle			m_ProbePixelLate;
	nvrhi::StagingTextureHandle		m_ProbePixelLateStaging;
	nvrhi::ITexture*				m_ProbeLateColor;	// the colour target ProbeStage read, until the late copy is made
	const viewDef_t*				m_ProbeLateView;	// and its view
	bool							m_ProbeHasPixelLate;
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
	// V4: the probe of the media: the media volume's three columns, and the medium the CPU finds at the same samples
	nvrhi::TextureHandle			m_ProbeMediaStrip;
	nvrhi::StagingTextureHandle		m_ProbeMediaStaging;
	bool							m_ProbeMedia;		// the media pass ran in the probe's view
	bool							m_ProbeMediaClosed;	// and its media have a closed form here ( no noise slot )
	bool							m_ProbeFogLights;	// V5: the probe's view took fog lights: the plain read has no closed form for them
	float							m_ProbeFurnace;		// the furnace radiance of the probe's view
	idVec4							m_ProbeMedium[3][256];	// by column and slice: sigma_s ( rgb ) and sigma_t per unit at the sample
	idVec3							m_ProbeSample[256];		// V4c: the centre column's samples in the relative frame, printed for every media probe ( review QR4 )
	idVec3d							m_ProbeRenderOrigin;	// the probe's view's render origin: a sample plus it is the sample's place in the map
	bool							m_ProbeHistory;		// the probe's view read the history ( false: it was cut )
	fogMedium_t						m_ProbeMediaList[ FOG_MEDIA_MAX ];	// the media list of the probe's view, and its count: the list of the view it reads in is another
	int								m_ProbeNumMedia;
	bool							m_ProbeSkip;		// the skip was on: past a column's reach the GPU holds the global fog alone, and there is no closed form

	// V6 ( row 182 ): the zones. A slot per frame of NUM_FRAME_DATA, by tr.frameCount ( the swap moves it before the backend
	// runs, so it stays the same through one backend frame ); a slot's queries are read when the slot comes round again,
	// NUM_FRAME_DATA frames later, as the render log reads its blocks
	nvrhi::TimerQueryHandle			m_ZoneQueries[ NUM_FRAME_DATA ][ FOG_ZONE_COUNT ];
	bool							m_ZoneUsed[ NUM_FRAME_DATA ][ FOG_ZONE_COUNT ];	// ended and not read yet ( the render log's timerUsed )
	bool							m_ZoneOpen[ FOG_ZONE_COUNT ];	// begun in this frame and not ended yet
	int								m_ZoneFrame[ NUM_FRAME_DATA ];	// the frame each slot's queries were made in
	int								m_ZoneReadFrame;	// the frame ZonesRead last ran in
};

#endif // RENDERER_PASSES_VOLUMETRICFOGPASS_H_
