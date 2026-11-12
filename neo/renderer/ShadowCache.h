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
#ifndef __SHADOWCACHE_H__
#define __SHADOWCACHE_H__

/*
==========================================================================================

SHADOW CACHE SEAMS ( X0 )

The hooks track S fills; S1 fills R_ShadowCacheDecide for the primary sun's cascades;
S2a makes the decide pass also fill the atlas pass's cvar snapshot, plan the atlas and
write the per-side caster bits; S2b adds the cache's images ( Image.h ) and the constants
of its copy draw, whose backend half lives at the end of ShadowCache.cpp; S2c gives the
decide pass the ops ( shadowCacheOp_t: where each cached unit's static map lives ), the
cacheable tests and the static bit of the caster bits, and the backend the block that
draws the static maps and composites each unit through the cache; S3a fills the two
notify hooks and the publish and gives local lights records, so with r_shadowCacheReuse 1
a static map is drawn once and kept until something in it changes; the debug overlay is
still a no-op; the calls are in R_RenderView,
SwapCommandBuffers_FinishRendering, RenderWorld.cpp and DBG_RenderDebugTools.
Included from RenderCommon.h, so it sees the complete idRenderEntityLocal /
idRenderLightLocal / renderEntity_t / renderLight_t.

==========================================================================================
*/

enum shadowCacheNotify_t
{
	SC_UPDATE,
	SC_FREE
};

// S1's records: the frontend's decide pass places each cascade of the map's primary sun on a
// texel grid anchored to the map origin and hangs the records on viewDef_t::shadowCacheView;
// the backend's SetupShadowMapMatrices obeys the record.
#define SHADOWCACHE_MAX_CASCADES	5		// r_shadowMapSplits is 0..4; the atlas pass draws splits + 1 cascades
#define SHADOWCACHE_MARGIN_TEXELS	4		// the least margin: filter taps reach r_shadowMapJitterScale ( 2.5 ) texels, plus one for the compare

struct shadowCacheCascade_t
{
	int64		originX;		// grid texel under column 0 ( unused when clampedX ); S4's scroll between two windows reads it, today only the log
	int64		originY;		// grid texel under row 0 ( unused when clampedY ); S4's scroll between two windows reads it, today only the log
	double		texelWorld;		// map units per texel of the snapped window; S4's scroll between two windows reads it, today only the log
	double		left;			// orthographic extents in the sun's light view, measured from shadowCacheView_t::sunViewOrigin
	double		right;
	double		bottom;
	double		top;
	int			res;			// texels per side ( shadowMapResolutions[ shadowLOD ] )
	bool		clampedX;		// this axis uses the light's own box, not the window; S4's scroll between two windows reads it, today only the log
	bool		clampedY;		// this axis uses the light's own box, not the window; S4's scroll between two windows reads it, today only the log
};

struct shadowCacheView_t
{
	idVec3					sunViewOrigin;	// where the sun's light view sits, relative to viewDef_t::renderOrigin: the relative eye
	idMat3					sunAxis;		// lightDir.ToMat3() of the primary sun: the rotation of its light view
	double					zNear;			// the light volume's range along the light, measured from sunViewOrigin ( D16: it never scrolls )
	double					zFar;
	int						numCascades;
	shadowCacheCascade_t	cascades[SHADOWCACHE_MAX_CASCADES];
};

// S2: what the decide pass says about one shadow caster of a light, one byte per surface of
// viewLight_t::globalShadows in chain order ( viewLight_t::shadowCasterBits ). The backend must not read
// drawSurf_t::frontEndGeo, so the frontend tests the bounds and the backend tests a bit.
enum shadowCasterBits_t
{
	SCB_SIDE_MASK	= 0x3F,		// bits 0..5: the sides ( cube faces of a point light, cascades of the sun ) the surface's bounds can reach
	SCB_STATIC		= 0x40		// S2c: the surface belongs to its unit's static map
};

// S2b: the constants of the copy draw, laid out as shaders/shadow_cache_copy.cb.h declares them ( 32 bytes )
struct shadowCacheCopyConstants_t
{
	int		tileOrigin[2];		// the rectangle's corner in the target, in texels
	int		sourceOrigin[2];	// the cached map's corner in its own texture
	int		wrap[2];			// the toroidal wrap ( S4 ); ( 0, 0 ) until then
	int		res;				// the cached map's size, a power of two; 0 = write the clear depth instead of copying
	int		tileRes;			// the rectangle's size in the target
};

static_assert( sizeof( shadowCacheCopyConstants_t ) == 32, "laid out as shaders/shadow_cache_copy.cb.h" );

// S3a: the cache atlas is handed out in cells of the smallest shadow map
#define SHADOWCACHE_CELL			128		// shadowMapResolutions[ MAX_SHADOWMAP_RESOLUTIONS - 1 ]
#define SHADOWCACHE_MAX_CELLS		4096	// ( 8192 / 128 ) squared: the cells of the largest cache atlas
#define SHADOWCACHE_MAX_SLOTS		( SHADOWCACHE_MAX_CELLS + SHADOWCACHE_MAX_CASCADES )	// one built stamp per place a static map can start at
#define SHADOWCACHE_MISS_RING		128		// S3a: serials of the last missed copies the backend publishes ( a point light that misses takes six ); idRenderBackend::shadowCacheMissed keeps the literal: RenderBackend.h is included before this header

// S3a: what the backend does with a cached unit's static map this frame
enum shadowCacheOpFlags_t
{
	SCOP_REBUILD	= 1		// clear the unit's region of its cache image and draw its static casters into it, before any copy
};

// S2c: one cached unit of this view ( a spot light's map, a face of a point light, a cascade of the
// primary sun ): where its static map lives. Decided by the frontend, executed by the backend ( D36 ).
// A light's ops are contiguous, one per side in side order; viewLight_t::shadowOp is the index of the
// light's first op plus one, 0 = the light is drawn uncached.
// S3a: serial names the build the op expects in the unit's region; the backend stamps the region when it draws a static map there and copies only where the stamp matches ( D74 ).
struct shadowCacheOp_t
{
	unsigned int	serial;		// S3a: the build of the static map this op draws ( SCOP_REBUILD ) or expects to find
	short			target;		// the cache image: -1 the cache atlas, 0..4 a cascade map
	short			size;		// the unit's side in texels, in the cache and in its atlas tile
	short			x;			// the unit's corner in its cache image
	short			y;
	short			flags;		// SCOP_*
	short			pad;
};

// S3a: the backend's built stamp for the place this unit's static map starts at: its cell of the cache
// atlas ( rows of 64 cells ), or one of the cascade maps
ID_INLINE int R_ShadowCacheSlot( const shadowCacheOp_t& op )
{
	if( op.target >= 0 )
	{
		return SHADOWCACHE_MAX_CELLS + op.target;
	}
	return ( op.y / SHADOWCACHE_CELL ) * 64 + ( op.x / SHADOWCACHE_CELL );
}

// frontend, also called from the R_AddSingleLight / R_AddSingleModel jobs: they only read
bool	R_ShadowCacheFullGather( const viewDef_t* viewDef, const idRenderLightLocal* light );
bool	R_ShadowCacheEntityCacheable( const idRenderEntityLocal* def );

extern idCVar r_shadowCache;
extern idCVar r_shadowCacheLog;
extern idCVar r_shadowCacheSize;
extern idCVar r_shadowCacheReuse;
extern idCVar r_shadowCacheStaticFrames;
extern idCVar r_shadowCacheBudget;
extern idCVar r_shadowCacheFlush;

void	R_ShadowCacheParallelMatrices( const shadowCacheView_t& scv, const shadowCacheCascade_t& cascade,
									   idRenderMatrix& lightViewRenderMatrix, idRenderMatrix& lightProjectionRenderMatrix );

void	R_ShadowCacheDecide();		// frontend, called from R_RenderView just before R_AddDrawViewCmd
void	R_ShadowCachePublish();		// called from SwapCommandBuffers_FinishRendering after GL_BlockingSwapBuffers

// re / rl is the NEW description on SC_UPDATE ( the hook compares it with def->parms /
// light->parms ) and NULL on SC_FREE
void	R_ShadowCacheNotifyEntity( const idRenderEntityLocal* def, const renderEntity_t* re, shadowCacheNotify_t op );
void	R_ShadowCacheNotifyLight( const idRenderLightLocal* light, const renderLight_t* rl, shadowCacheNotify_t op );

// called where derived data is freed past every notify call ( R_FreeDerivedData,
// R_CheckForEntityDefsUsingModel ): every static map is built again
void	R_ShadowCacheInvalidateAll();

void	R_ShadowCacheDebugTools( nvrhi::ICommandList* commandList, const viewDef_t* viewDef );	// track S's debug overlay, called from DBG_RenderDebugTools

// C3: the one test of "this view light has a shadow map to sample". The body is track S's and lives in
// this header so that it inlines at its call sites in the backend ( once per interaction draw ).
// S2c: a cached light has a shadow map even when no caster is in this view's chain ( its casters are in
// its static map ): shadowOp says so. With r_shadowCache below 2 shadowOp is always 0,
// so the test is what it was.
ID_INLINE bool R_ViewLightHasShadowMap( const viewLight_t* vLight )
{
	return vLight->shadowLOD != -1 && ( vLight->globalShadows != NULL || vLight->shadowOp != 0 );
}

// S1: the cascade record SetupShadowMapMatrices obeys for this light and side; NULL = today's crop
ID_INLINE const shadowCacheCascade_t* R_ShadowCacheCascade( const viewDef_t* viewDef, const viewLight_t* vLight, int side )
{
	const shadowCacheView_t* scv = viewDef->shadowCacheView;
	if( scv == NULL || !vLight->isPrimarySun || side < 0 || side >= scv->numCascades )
	{
		return NULL;
	}
	return &scv->cascades[side];
}

#endif // __SHADOWCACHE_H__
