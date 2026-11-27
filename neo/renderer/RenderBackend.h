/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2016-2017 Dustin Land
Copyright (C) 2017-2023 Robert Beckebans
Copyright (C) 2022 Stephen Pridham
Copyright (C) 2026 Phillip Melnichenko

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

#ifndef __RENDERER_BACKEND_H__
#define __RENDERER_BACKEND_H__

#include "RenderLog.h"

#include "Passes/CommonPasses.h"
#include "Passes/MipMapGenPass.h"
#include "Passes/SsaoPass.h"
#include "Passes/TonemapPass.h"
// ---- TRACK G: the bloom of the main view; the member beside toneMapPass below ----
#include "Passes/BloomPass.h"
// ---- END TRACK G ----
#include "Passes/TemporalAntiAliasingPass.h"
#include "Passes/VolumetricFogPass.h"

#include "PipelineCache.h"

struct tmu_t
{
	unsigned int	current2DMap;
	unsigned int	current2DArray;
	unsigned int	currentCubeMap;
};

const int MAX_MULTITEXTURE_UNITS = 14;

enum stencilFace_t
{
	STENCIL_FACE_FRONT,
	STENCIL_FACE_BACK,
	STENCIL_FACE_NUM
};

// which surfaces of an interaction chain RenderInteractions draws, by material sort ( X0, D83 )
enum interactionSort_t
{
	INTERACTION_SORT_ALL,			// every surface of the chain
	INTERACTION_SORT_BELOW_FAR,		// sort < SS_FAR: lit decals, which lie on opaque surfaces
	INTERACTION_SORT_FAR_AND_ABOVE	// sort >= SS_FAR: free-standing translucents ( glass ), drawn after the fog apply
};

struct gfxImpParms_t
{
	int		x;				// ignored in fullscreen
	int		y;				// ignored in fullscreen
	int		width;
	int		height;
	int		fullScreen;		// 0 = windowed, otherwise 1 based monitor number to go full screen on
	// -1 = borderless window for spanning multiple displays
	int		displayHz;
	int		multiSamples;
};

#define MAX_DEBUG_LINES		16384
#define MAX_DEBUG_TEXT		512
#define MAX_DEBUG_POLYGONS	8192

struct debugLine_t
{
	idVec4		rgb;
	idVec3		start;
	idVec3		end;
	bool		depthTest;
	int			lifeTime;
};

struct debugText_t
{
	idStr		text;
	idVec3		origin;
	float		scale;
	idVec4		color;
	idMat3		viewAxis;
	int			align;
	int			lifeTime;
	bool		depthTest;
};

struct debugPolygon_t
{
	idVec4		rgb;
	idWinding	winding;
	bool		depthTest;
	int			lifeTime;
};

void RB_SetMVP( const idRenderMatrix& mvp );
void RB_SetVertexColorParms( stageVertexColor_t svc );

// ---- TRACK F: the forward pass's free functions ----
// The three default light images of the forward layouts ( renderer/NVRHI/RenderBackend_NVRHI.cpp ): 0 = the
// projection image the two default light shaders share, 1 = the default point light's falloff, 2 = the default
// projected light's. The froxel fog's inject binds the same three ( VolumetricFogPass::CheckInjectSet ): one
// function for both passes, so that the fog and the walls cannot read different images
nvrhi::ITexture* R_ForwardDefaultLightImage( int which );
// ---- END TRACK F ----

/*
===========================================================================

idRenderBackend

all state modified by the back end is separated from the front end state

===========================================================================
*/
struct ImDrawData;

class IRenderPass;
class ForwardShadingPass;

class idRenderBackend
{
	friend class Framebuffer;
	friend class fhImmediateMode;

public:
	idRenderBackend();
	~idRenderBackend();

	void				Init();
	void				Shutdown();

	void				ExecuteBackEndCommands( const emptyCommand_t* cmds );
	void				StereoRenderExecuteBackEndCommands( const emptyCommand_t* const allCmds );
	void				GL_BlockingSwapBuffers();

	void				CheckCVars();

	void				ClearCaches();

	//puncho start - static index (2R R3.3): when a static set of the vertex cache grows, its buffer is replaced by a larger one. The
	// handles and offsets of the buffers bound last must not outlive the old buffer (idVertexCache::GrowStaticSet calls
	// this, with the back end idle). It is the reset every view starts with
	void				ClearGeometryBufferCache()
	{
		currentVertexBuffer = nullptr;
		currentIndexBuffer = nullptr;
		currentJointBuffer = nullptr;
		currentVertexOffset = 0;
		currentIndexOffset = 0;
		currentJointOffset = 0;
	}
	//puncho end

	static void			ImGui_RenderDrawLists( ImDrawData* draw_data );

	void				DrawElementsWithCounters( const drawSurf_t* surf, bool shadowCounter = false );

private:
	//puncho - callers test the flag captured by ExecuteBackEndCommands.
	void				MarkBackendPass( backendPass_t nextPass );
	void				DrawFlickerBox();

	void				GetCurrentBindingLayout( int bindingLayoutType );
	void				ResizeImages();

	void				DrawViewInternal( const viewDef_t* viewDef, const int stereoEye );
	void				DrawView( const void* data, const int stereoEye );
	void				CopyRender( const void* data );

	void				BindVariableStageImage( const textureStage_t* texture, const float* shaderRegisters, nvrhi::ICommandList* commandList );
	void				PrepareStageTexturing( const shaderStage_t* pStage, const drawSurf_t* surf, int fogKind = -1 );	// V5e: fogKind, the stage's blend kind for a fog twin ( rpTexGen0Enabled.y )
	void				FinishStageTexturing( const shaderStage_t* pStage, const drawSurf_t* surf );

	void				ResetViewportAndScissorToDefaultCamera( const viewDef_t* _viewDef );

	void				FillDepthBufferGeneric( const drawSurf_t* const* drawSurfs, int numDrawSurfs );
	void				FillDepthBufferFast( drawSurf_t** drawSurfs, int numDrawSurfs );

	void				T_BlendLight( const drawSurf_t* drawSurfs, const viewLight_t* vLight );
	void				BlendLight( const drawSurf_t* drawSurfs, const drawSurf_t* drawSurfs2, const viewLight_t* vLight );
	void				T_BasicFog( const drawSurf_t* drawSurfs, const idPlane fogPlanes[ 4 ], const idRenderMatrix* inverseBaseLightProject );
	void				FogPass( const drawSurf_t* drawSurfs,  const drawSurf_t* drawSurfs2, const viewLight_t* vLight );
	void				FogAllLights();

	void				SetupInteractionStage( const shaderStage_t* surfaceStage, const float* surfaceRegs, const float lightColor[4],
			idVec4 matrix[2], float color[4] );

	void				DrawInteractions( const viewDef_t* _viewDef );
	void				DrawTranslucentInteractions( const viewDef_t* _viewDef );
	//puncho start
	void				DrawInteractionsPass( const viewDef_t* _viewDef, bool emitterPass, Framebuffer* previousFramebuffer, bool useLightDepthBounds );
	//puncho end
	void				DrawSingleInteraction( drawInteraction_t* din, bool useFastPath, bool useIBL, bool setInteractionShader );
	int					DrawShaderPasses( const drawSurf_t* const* const drawSurfs, const int numDrawSurfs,
										  const float guiStereoScreenOffset, const int stereoEye, const bool stopAtFar );

	void				RenderInteractions( const drawSurf_t* surfList, const viewLight_t* vLight, int depthFunc, bool performStencilTest, bool useLightDepthBounds, const interactionSort_t sortFilter );
	// track F ( F7j ): RenderInteractions' lists of a light's lit surfaces, emptied per call, their memory kept
	idList< const drawSurf_t* >	interactionSurfaces;
	idList< const drawSurf_t* >	interactionComplexSurfaces;

	// RB
	void				AmbientPass( const drawSurf_t* const* drawSurfs, int numDrawSurfs, bool fillGbuffer );

	// ---- X0: renderer upgrade seams ----
	void				ForwardPass( const drawSurf_t* const* drawSurfs, int numDrawSurfs, bool translucent );
	void				ResetGraphicsStateCache();
	// ---- END X0 ----

	void				SetupShadowMapMatrices( viewLight_t* vLight, int side, idRenderMatrix& lightProjectionRenderMatrix, idRenderMatrix& lightViewRenderMatrix );
	void				ShadowMapPassFast( const drawSurf_t* drawSurfs, viewLight_t* vLight, int side, bool atlas, const byte* casterBits = NULL, int casterMask = 0, int casterWant = 0, const struct shadowCacheOp_t* cacheOp = NULL, Framebuffer* cacheTarget = NULL, const struct shadowCacheToroidalRect_t* cachePiece = NULL );
	void				ShadowMapPassPerforated( const drawSurf_t** drawSurfs, int numDrawSurfs, viewLight_t* vLight, int side, const idRenderMatrix& lightProjectionRenderMatrix, const idRenderMatrix& lightViewRenderMatrix );
	void				ShadowMapsForLightNoAtlas( const viewLight_t* vLight, Framebuffer* previousFramebuffer );

	void				ShadowAtlasPass( const viewDef_t* _viewDef );

	void				DrawMotionVectors();
	void				TemporalAAPass( const viewDef_t* _viewDef );

	void				DrawScreenSpaceAmbientOcclusion( const viewDef_t* _viewDef );
	void				DrawScreenSpaceAmbientOcclusion2( const viewDef_t* _viewDef );

	// Experimental feature
	void				MotionBlur();

	void				PostProcess( const void* data );
	void				CRTPostProcess();

private:
	void				GL_StartFrame();
	void				GL_EndFrame();

public:
	uint64				GL_GetCurrentState() const;
	idVec2				GetCurrentPixelOffset( int frameIndex ) const;

	nvrhi::ICommandList* GL_GetCommandList() const
	{
		return commandList;
	}

private:
	uint64				GL_GetCurrentStateMinusStencil() const;
	void				GL_SetDefaultState();

	void				GL_State( uint64 stateBits, bool forceGlState = false );
//	void				GL_SeparateStencil( stencilFace_t face, uint64 stencilBits );

	void				GL_SelectTexture( int unit );
//	void				GL_BindTexture( idImage* image );

//	void				GL_CopyFrameBuffer( idImage* image, int x, int y, int imageWidth, int imageHeight );
//	void				GL_CopyDepthBuffer( idImage* image, int x, int y, int imageWidth, int imageHeight );

	// RB: HDR parm
	void				GL_Clear( bool color, bool depth, bool stencil, byte stencilValue, float r, float g, float b, float a, bool clearHDR = false, bool clearVR = false, const int stereoEye = 0 );

	void				GL_DepthBoundsTest( const float zmin, const float zmax );
	void				GL_PolygonOffset( float scale, float bias );

	void				GL_Scissor( int x /* left*/, int y /* bottom */, int w, int h );
	void				GL_Viewport( int x /* left */, int y /* bottom */, int w, int h );

	ID_INLINE void		GL_Scissor( const idScreenRect& rect )
	{
		GL_Scissor( rect.x1, rect.y1, rect.x2 - rect.x1 + 1, rect.y2 - rect.y1 + 1 );
	}
	ID_INLINE void		GL_Viewport( const idScreenRect& rect )
	{
		GL_Viewport( rect.x1, rect.y1, rect.x2 - rect.x1 + 1, rect.y2 - rect.y1 + 1 );
	}

	ID_INLINE void	GL_ViewportAndScissor( int x, int y, int w, int h )
	{
		GL_Viewport( x, y, w, h );
		GL_Scissor( x, y, w, h );
	}

	ID_INLINE void	GL_ViewportAndScissor( const idScreenRect& rect )
	{
		GL_Viewport( rect );
		GL_Scissor( rect );
	}

	void				GL_Color( float r, float g, float b, float a );
	ID_INLINE void		GL_Color( float r, float g, float b )
	{
		GL_Color( r, g, b, 1.0f );
	}

	ID_INLINE void		GL_Color( const idVec3& color )
	{
		GL_Color( color[0], color[1], color[2], 1.0f );
	}

	ID_INLINE void		GL_Color( const idVec4& color )
	{
		GL_Color( color[0], color[1], color[2], color[3] );
	}

//	void				GL_Color( float* color );

	void				SetBuffer( const void* data );

private:
	void				DBG_SimpleSurfaceSetup( const drawSurf_t* drawSurf );
	void				DBG_SimpleWorldSetup();
	void				DBG_PolygonClear();
	void				DBG_ShowDestinationAlpha();
	void				DBG_ScanStencilBuffer();
	void				DBG_CountStencilBuffer();
	void				DBG_ColorByStencilBuffer();
	void				DBG_ShowOverdraw();
	void				DBG_ShowIntensity();
	void				DBG_ShowDepth();
	void				DBG_ShowLightCount();
//	void				DBG_EnterWeaponDepthHack();
//	void				DBG_EnterModelDepthHack( float depth );
//	void				DBG_LeaveDepthHack();
	void				DBG_RenderDrawSurfListWithFunction( drawSurf_t** drawSurfs, int numDrawSurfs );
	void				DBG_ShowSilhouette();
	void				DBG_ShowTris( drawSurf_t** drawSurfs, int numDrawSurfs );
	void				DBG_ShowSurfaceInfo( drawSurf_t** drawSurfs, int numDrawSurfs );
	void				DBG_ShowViewEntitys( viewEntity_t* vModels );
	void				DBG_ShowTexturePolarity( drawSurf_t** drawSurfs, int numDrawSurfs );
	void				DBG_ShowUnsmoothedTangents( drawSurf_t** drawSurfs, int numDrawSurfs );
	void				DBG_ShowTangentSpace( drawSurf_t** drawSurfs, int numDrawSurfs );
	void				DBG_ShowVertexColor( drawSurf_t** drawSurfs, int numDrawSurfs );
	void				DBG_ShowNormals( drawSurf_t** drawSurfs, int numDrawSurfs );
	void				DBG_ShowTextureVectors( drawSurf_t** drawSurfs, int numDrawSurfs );
	void				DBG_ShowDominantTris( drawSurf_t** drawSurfs, int numDrawSurfs );
	void				DBG_ShowEdges( drawSurf_t** drawSurfs, int numDrawSurfs );
	void				DBG_ShowLights();
	void				DBG_ShowLightGrid(); // RB
	void				DBG_ShowViewEnvprobes(); // RB
	void				DBG_ShowShadowMapLODs(); // RB
	void				DBG_ShowPortals();
	void				DBG_ShowDebugText();
	void				DBG_ShowDebugLines();
	void				DBG_ShowDebugPolygons();
	void				DBG_ShowCenterOfProjection();
	void				DBG_ShowLines();
	void				DBG_TestGamma();
	void				DBG_TestGammaBias();
	void				DBG_TestImage();
	void				DBG_ShowShadowMaps(); // RB
	void				DBG_ShowTrace( drawSurf_t** drawSurfs, int numDrawSurfs );
	void				DBG_RenderDebugTools( drawSurf_t** drawSurfs, int numDrawSurfs );

public:
	backEndCounters_t	pc;
	int					shadowAtlasFailedHighWater;

	// surfaces used for code-based drawing
	drawSurf_t			unitSquareSurface;
	drawSurf_t			zeroOneCubeSurface;
	drawSurf_t			zeroOneSphereSurface; // RB
	drawSurf_t			testImageSurface;

	float				slopeScaleBias;
	float				depthBias;

private:
	//puncho - recording-thread state, no timer or counter access at disabled marks.
	uint64				passStartMicroSec;
	int					passStartDraws;
	backendPass_t		currentPass;
	bool				concurrencyStats;

	uint64				glStateBits;			// for all render APIs

	const viewDef_t* 	viewDef;

	const viewEntity_t*	currentSpace;			// for detecting when a matrix must change
	idScreenRect		currentScissor;			// for scissor clipping, local inside renderView viewport

	bool				currentRenderCopied;	// true if any material has already referenced _currentRender

	idRenderMatrix		prevMVP[2];				// world MVP from previous frame for motion blur
	idVec3d				prevRenderOrigin[2];	// the render origin prevMVP is relative to, per eye like prevMVP
	bool				prevViewsValid;

	// RB begin
	// TODO remove
	float				hdrTime;

	// quad-tree for managing tiles within tiled shadow map
	TileMap				tileMap;

private:
	idScreenRect					stateViewport;
	idScreenRect					stateScissor;

	idScreenRect					currentViewport;
	nvrhi::BufferHandle				currentVertexBuffer;
	uint							currentVertexOffset;
	nvrhi::BufferHandle				currentIndexBuffer;
	uint							currentIndexOffset;
	nvrhi::BindingLayoutHandle		currentBindingLayout;
	nvrhi::IBuffer*					currentJointBuffer;
	uint							currentJointOffset;
	nvrhi::GraphicsPipelineHandle	currentPipeline;

	idStaticList<nvrhi::BindingSetHandle, nvrhi::c_MaxBindingLayouts> currentBindingSets;
	idStaticList<idStaticList<nvrhi::BindingSetDesc, nvrhi::c_MaxBindingLayouts>, NUM_BINDING_LAYOUTS> pendingBindingSetDescs;

	//puncho - stack (2R R4.3): FillDepthBufferFast and ShadowMapPassFast each set aside one pointer for every surface
	// they are given and draw the perforated ones last. The array is this list; it was an _alloca on the main thread's
	// stack. Num() is the list's capacity: it grows by doubling and never shrinks. The two passes never run inside each
	// other, and each has used the array before it returns
	idList<const drawSurf_t*, TAG_RENDER>	perforatedScratch;

	Framebuffer*					currentFrameBuffer;
	Framebuffer*					lastFrameBuffer;
	nvrhi::CommandListHandle		commandList;
	CommonRenderPasses				commonPasses;
	SsaoPass*						ssaoPass;
	// ---- X0: renderer upgrade seams ----
	VolumetricFogPass*				volumetricFogPass;		// created by track V; X0 only nulls and guards it
	nvrhi::BufferHandle				froxelFogParms;			// volatile, sizeof( froxelFogParms_t ); zeroed at the start of every 3D view
	nvrhi::TextureHandle			froxelDummyVolume;		// the 1x1x1 "no fog" volume; owned here for the backend's lifetime
	nvrhi::TextureHandle			froxelVolume;			// the volume the fog set binds: the dummy until track V swaps in its own at a pass boundary
	// WP-J21: the fog's glow. Set by the fog's apply where its Render ran the glow forms in this view, false again at the
	// start of every 3D view; the bloom's level-1 pass reads froxelNoGlowVolume beside froxelVolume
	bool							froxelGlowView;
	nvrhi::ITexture*				froxelNoGlowVolume;
	// ResetGraphicsStateCache() after the swap, because a binding set is re-patched only when GetCurrentBindingLayout runs again
	bool							viewHistoryCut;			// H10: this main view's camera cut, decided once at the start of its draw
	// ---- END X0 ----
	// ---- TRACK S: shadow cache members ----
	nvrhi::ShaderHandle				shadowCacheCopyVS;
	nvrhi::ShaderHandle				shadowCacheCopyPS;
	nvrhi::BindingLayoutHandle		shadowCacheCopyLayout;
	nvrhi::GraphicsPipelineHandle	shadowCacheCopyPipeline[2];		// by the kind of target: 0 the per-frame atlas, 1 a cache image; the two kinds differ only in the framebuffer they were made for
	nvrhi::FramebufferInfo			shadowCacheCopyPipelineInfo[2];	// what each pipeline was made for
	nvrhi::TextureHandle			shadowCacheCopySource;			// S3b: the source image of the last copy draw, and its binding set, kept while the source stays the same
	nvrhi::BindingSetHandle			shadowCacheCopyBindingSet;
	int								shadowCacheTargetSerial;		// in the names of the cache's framebuffers, so a stale one is never found again
	// S3a: the serial of the static map last drawn at each place of the cache images ( R_ShadowCacheSlot ),
	// 0 = none; only the backend writes it ( D74 )
	unsigned int					shadowCacheBuilt[4096 + 5];
	// S5: r_shadowCacheDebug's marks: the places of the cache images this backend drew into ( ShadowCacheDrawStaticMaps ), in
	// the image's own texels, kept for the panels' fading; written only while the cvar is on
	struct shadowCacheMark_t
	{
		int		frame;		// shadowCacheDebugFrame when it was drawn
		short	target;		// -1 the cache atlas, 0..4 a cascade map
		short	kind;		// what drew it ( shadowCacheDebugKind_t in ShadowCache.cpp )
		short	x0;
		short	y0;
		short	x1;
		short	y1;
	};
	static const int				SHADOWCACHE_NUM_MARKS = 2048;	// a power of two: a sun that scrolls makes about 32 marks a view
	shadowCacheMark_t				shadowCacheMarks[SHADOWCACHE_NUM_MARKS];
	unsigned int					shadowCacheNumMarks;			// marks ever made: mark n is at index n & ( SHADOWCACHE_NUM_MARKS - 1 )
	int								shadowCacheDebugFrame;			// main views r_shadowCacheDebug drew, the marks' clock
	bool							shadowCacheDebugWasOn;			// r_shadowCacheDebug was on at the last static maps' pass
public:
	// S3a: the frontend reads these ( R_ShadowCachePublish takes the miss count, the ring of missed serials and, S4,
	// the count of heals ); the rest of the cache stays the backend's
	unsigned int					shadowCacheMisses;				// S3a: copies that found no map, or another one, where the frontend expected one; R_ShadowCachePublish hands the count to the frontend
	unsigned int					shadowCacheHeals;				// S4: scrolls drawn as a whole window because the build they expected was not in the map; R_ShadowCachePublish hands the count to the frontend's log
	unsigned int					shadowCacheMissed[128];			// S3a: the serials of the last 128 missed copies: miss n is at index n & 127 ( a point light that misses takes six ); the frontend's copy is checked against this width by a static_assert in ShadowCache.cpp
	void							ShadowCacheDebugDraw();			// S5: r_shadowCacheDebug's panels, drawn with the debug tools ( R_ShadowCacheDebugTools calls it )
private:

	void				ShadowCacheCopyDraw( nvrhi::IFramebuffer* target, int targetKind, nvrhi::ITexture* source, const struct shadowCacheCopyConstants_t& constants, const struct shadowCacheRect_t* clearRect = NULL );
	Framebuffer*		ShadowCacheTarget( int which, bool* recreated );		// which: -1 the cache atlas, 0..4 a cascade map
	void				ShadowCacheDrawStaticMaps();
	void				ShadowCacheClearCaches();
	// ---- END TRACK S ----
	// ---- TRACK F: light lists ----
	void							R_BuildShadowRows( const viewDef_t* viewDef );
	void							DBG_LightLists();		// track F's debug overlay, called from DBG_RenderDebugTools
	nvrhi::BufferHandle				recordLightsBuffer;		// declared by X0, created by F1 ( D69 )
	nvrhi::BufferHandle				shadowRowsBuffer;
	nvrhi::BufferHandle				lightPoolBuffer;
	nvrhi::BufferHandle				zBinsBuffer;
	nvrhi::BufferHandle				tileBitsBuffer;
	nvrhi::BufferHandle				vanishedPrevBuffer;
	void							LightListCreateBuffers( int tileBitWords, int poolWords );	// F1: creates what is missing, and the tile bits again when a view needs more words; the pool too //puncho - R5 join
	void							LightListReleaseBuffers();					// F1: with ClearCaches
	int								lightListTileBitsWords;						// F1: the uint32 words tileBitsBuffer holds; valid while the buffer exists
	int								lightListPoolWords;							//puncho - R5 join: the uint32 words lightPoolBuffer holds; valid while the buffer exists
	nvrhi::ShaderHandle				lightListHeatmapVS;							// F1: the heatmap pass ( r_showLightLists 1 ), made at first use
	nvrhi::ShaderHandle				lightListHeatmapPS;
	nvrhi::BindingLayoutHandle		lightListHeatmapLayout;
	nvrhi::GraphicsPipelineHandle	lightListHeatmapPipeline;
	nvrhi::FramebufferHandle		lightListHeatmapTarget;						// the colour attachment of the view's framebuffer alone
	nvrhi::ITexture*				lightListHeatmapColor;						// the texture that target was made for; valid while the target exists
	void							LightDiffPass( const drawSurf_t* const* drawSurfs, int numDrawSurfs );	// F2: the difference view ( r_showLightDiff 2 ), after DrawInteractions
	bool							lightDiffPassA;								// F2: true while LightDiffPass draws the forward picture: the loop lights and the legacy pass leaves the twins out
	bool							forwardTranslucent;							// F4: true while ForwardPass( translucent ) draws the listed glass through AmbientPass's loop
	bool							lightDiffHasReadout;						// F2: a reduced picture of the present size has been read
	nvrhi::TextureHandle			lightDiffImageA;							// the forward picture of the view's lit passes
	nvrhi::TextureHandle			lightDiffImageB;							// the legacy picture
	nvrhi::TextureHandle			lightDiffReduced;							// per block of 16 x 16 pixels: the sum and the maximum of their difference
	nvrhi::FramebufferHandle		lightDiffReducedTarget;
	nvrhi::FramebufferHandle		lightDiffShowTarget;						// the colour attachment of the view's framebuffer alone
	nvrhi::ITexture*				lightDiffShowColor;							// the texture that target was made for; valid while the target exists
	nvrhi::StagingTextureHandle		lightDiffStaging[3];						// a ring: a reduced picture is read two frames after it was drawn ( D79 )
	int								lightDiffStagingFrame[3];					// lightDiffFrame when the slot was written; -1 = never
	int								lightDiffFrame;								// counts the passes
	int								lightDiffViewFrame;							// lightListView_t::viewFrame of the last pass; a gap empties the ring
	float							lightDiffMean;								// the last read-out: the mean and the maximum of the difference, the legacy picture's mean
	float							lightDiffMax;
	float							lightDiffLegacyMean;
	nvrhi::ShaderHandle				lightDiffVS;
	nvrhi::ShaderHandle				lightDiffPS;
	nvrhi::BindingLayoutHandle		lightDiffLayout;
	nvrhi::GraphicsPipelineHandle	lightDiffShowPipeline;
	nvrhi::GraphicsPipelineHandle	lightDiffReducePipeline;
	// ---- END TRACK F ----
	// ---- TRACK V: volumetric fog members ----
	// the pass writes froxelFogParms and swaps froxelVolume at its apply ( D27, D66 ), puts the dummy back and clears
	// bindingCache where its volume dies, and draws its sky mask through the backend's draw path
	friend class VolumetricFogPass;
	// V5: true from the fog view's apply ( which wrote froxelFogParms ) to the end of its translucent shader passes;
	// while it is true the translucent draws bind the fog twins ( D35, D66, B22 ). Every view starts false
	bool							froxelFogTranslucent;
	// V5: the fog view's apply ran with fog lights its inject took ( D21 ): its FogAllLights leaves them to the froxel fog
	bool							froxelFogLightsTaken;
	// ---- END TRACK V ----
	MipMapGenPass*					hiZGenPass;
	TonemapPass*					toneMapPass;
	// ---- TRACK G: the bloom of the main view; made and deleted by the backend's GL_StartFrame and ClearCaches ----
	// the bloom's direct NVRHI draws count themselves in the concurrency stats, as the fog's do
	friend class BloomPass;
	BloomPass*						bloomPass;
	// ---- END TRACK G ----
	// ---- WP-J21: the bloom's gate in the stage loop. glowGateView: this view's stage loops write the glow mask ( a main
	// view drawn into _hdr, the gate on, _hdrGlow there ); glowMaskWritten: an emitting stage drew this view, so the mask
	// was cleared and holds what passes the gate ( the bloom reads it ). Both are set where the view binds _hdr ----
	bool							glowGateView;
	bool							glowMaskWritten;
	bool							GlowStagePasses( const idMaterial* shader, const shaderStage_t* pStage, const drawSurf_t* surf, uint64 stageGLState ) const;
	// ---- END WP-J21 ----
	// ---- WP-J27: the sources' own blooms ( renderBloomStyle_t ). An emitting stage of an entity with a style of its own writes
	// its slot into the glow mask ( texture_color's glow twin: green = the luminance times the slot ); the bloom gives each slot
	// its strength, its boost and, under the Fourier bloom, its kernel. Slot 0 is the view's. A style keeps its slot across
	// frames while it is in view, so the bloom keeps its kernel; a style that finds every slot taken in this view takes the
	// view's ( glowStyleOverflow counts them for bloomProbe ) ----
	renderBloomStyle_t				glowStyles[ BLOOM_STYLE_SLOTS ];		// [0] unused ( the view's )
	int							glowStyleView[ BLOOM_STYLE_SLOTS ];		// the glowStyleSerial of the last view that drew with the slot; 0 = never
	int							glowStyleSerial;						// one a view under the gate
	int							glowStyleOverflow;						// this view's styles that took the view's slot for want of one
	float							glowStyleDrawSlot;						// the stage being drawn: its slot, sent in rpTexGen0Enabled.z ( 0 for every other draw )
	int							GlowStyleSlot( const viewEntity_t* space, uint64 stageGLState );
	// ---- END WP-J27 ----
	TemporalAntiAliasingPass*		taaPass;

	BindingCache					bindingCache;
	SamplerCache					samplerCache;
	PipelineCache					pipelineCache;

	nvrhi::InputLayoutHandle		inputLayout;

	nvrhi::ShaderHandle             vertexShader;
	nvrhi::ShaderHandle             pixelShader;

	int								prevBindingLayoutType;

public:

	void				ResetPipelineCache();

	void				SetCurrentImage( idImage* image );
	idImage*			GetCurrentImage();
	idImage*			GetImageAt( int index );
	CommonRenderPasses& GetCommonPasses()
	{
		return commonPasses;
	}
};

#endif
