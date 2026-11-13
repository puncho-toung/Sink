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

idCVar r_shadowCache( "r_shadowCache", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "shadow cache: 0 = off ( today's path ), 1 = the primary sun's cascades use a map-anchored snapped window of constant size, 2 = also the frontend's snapshot, the atlas plan, the per-side caster cull, and static casters drawn through the cache images ( kept between frames with r_shadowCacheReuse 1 )", 0, 2 );
idCVar r_shadowCacheLog( "r_shadowCacheLog", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "1 = print the primary sun's cascade windows of the main view whenever one changes, and the shadow cache's counters every 64 frames; 2 = also one line per local light of the view with those counters", 0, 2 );
idCVar r_shadowCacheSize( "r_shadowCacheSize", "4096", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE | CVAR_NEW, "side of the shadow cache atlas in texels: 8192, or 4096 for any other value; read when the cache images are created", 4096, 8192 );

idCVar r_shadowCacheReuse( "r_shadowCacheReuse", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "with r_shadowCache 2: 1 = a local light's static shadow maps are drawn once and kept until something in them changes, 0 = every static map is drawn again every frame ( the comparison image for the cache )" );
idCVar r_shadowCacheStaticFrames( "r_shadowCacheStaticFrames", "300", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "frames an entity must go unchanged before its shadow is drawn into a static shadow map", 1, 100000 );
idCVar r_shadowCacheBudget( "r_shadowCacheBudget", "10000", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "what the static shadow maps built in one frame for lights that wait for them may cost: per side of a light, 100 plus the cells of cache atlas ( 128 x 128 texels ) times the casters drawn into them, so a point light at 256 with 35 casters costs 1440; the first build of a frame is never refused; maps a change made stale are rebuilt at once and do not count", 1, 100000000 );
idCVar r_shadowCacheFlush( "r_shadowCacheFlush", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "set to 1 to throw every cached shadow map and its place away once; the engine sets it back to 0" );

extern idCVar r_useAreasConnectedForShadowCulling;		// tr_frontend_addlights.cpp
extern idCVar r_forceShadowMapsOnAlphaTestedSurfaces;	// tr_frontend_addmodels.cpp

/*
==========================================================================================

The light frame, the window and the slice

==========================================================================================
*/

// The light frame of a directional cache: a rotation and nothing else, a pure function of the light's direction, so that
// every frame and every cascade agree on one texel grid anchored to the world. A sun that has not moved must produce a
// bit-identical direction every frame, or the grid moves and every cached texel is invalid.
// The grid runs along the shadow map's own columns and rows. With a view built by idRenderMatrix::CreateViewMatrix from
// 'axis' (view x = -axis[1], view y = axis[2]) and a target whose row 0 is NDC y = +1 (D3D12, and Vulkan through NVRHI's
// flipped viewport), columns grow along -axis[1] and rows along -axis[2]. Keeping the grid in target space makes every
// shift, rect and wrap below plain integer arithmetic on target texels, with no mirror to remember.
struct shadowLightFrame_t
{
	idVec3			axis[3];		// the rotation the light view is built from; axis[0] is the light's travel direction
	idVec3			gridX;			// world direction in which shadow-map columns grow
	idVec3			gridY;			// world direction in which shadow-map rows grow
};

static void R_ShadowCacheLightFrame( const idVec3& lightTravelDir, shadowLightFrame_t& frame )
{
	const idMat3 m = lightTravelDir.ToMat3();	// expects a unit vector; the same input always gives the same basis
	frame.axis[0] = m[0];
	frame.axis[1] = m[1];
	frame.axis[2] = m[2];
	frame.gridX = -m[1];
	frame.gridY = -m[2];
}

// A res x res window of the light frame's world-anchored texel grid. The origin is an integer: the grid texel under the
// window's column 0 and row 0. Two windows of the same cache differ by whole texels by construction, so a scroll is an
// integer shift, never a resample.
struct shadowCacheWindow_t
{
	int64			originX;
	int64			originY;
	double			texelWorld;		// world units per texel: constant for the life of the cache
	int				res;			// texels per side
};

// Light-frame coordinate of a world point along one axis, in double: in float the sub-texel part is lost far from the
// world origin, and the grid stops lining up from frame to frame.
static double R_ShadowCacheLightCoord( const idVec3& axis, const idVec3& p )
{
	return ( double )p.x * ( double )axis.x + ( double )p.y * ( double )axis.y + ( double )p.z * ( double )axis.z;
}

// Centre a window on light-frame coordinates ( gx, gy ), in whole texels. The floor puts the window's centre up to one
// texel before ( gx, gy ), never after it.
static void R_ShadowCacheSnapWindowAt( double gx, double gy, double texelWorld, int res, shadowCacheWindow_t& out )
{
	out.originX = ( int64 )floor( gx / texelWorld ) - res / 2;
	out.originY = ( int64 )floor( gy / texelWorld ) - res / 2;
	out.texelWorld = texelWorld;
	out.res = res;
}

// The window's extent along gridX and gridY, for building its orthographic projection.
static void R_ShadowCacheWindowExtent( const shadowCacheWindow_t& win, double& minX, double& maxX, double& minY, double& maxY )
{
	minX = ( double )win.originX * win.texelWorld;
	maxX = ( double )( win.originX + win.res ) * win.texelWorld;
	minY = ( double )win.originY * win.texelWorld;
	maxY = ( double )( win.originY + win.res ) * win.texelWorld;
}

// A slice of a perspective view, in the view's own terms: its near and far distances and the tangents of its four
// side planes ( -tanHalfX, tanHalfX, -tanHalfY, tanHalfY for a centred view; an off-centre projection shifts them).
struct shadowCacheSlice_t
{
	double			zNear;
	double			zFar;
	double			tanLeft;
	double			tanRight;
	double			tanBottom;
	double			tanTop;
};

// The slice's diameter: the greatest distance between two of its corners. It depends on the slice alone - not on
// where the camera is or where it looks - so a window sized from it keeps one size while the camera moves and turns.
// Compute it from these numbers, never from corners transformed by this frame's camera, whose rounding changes the
// size, and with it the grid, every frame.
static double R_ShadowCacheSliceDiameter( const shadowCacheSlice_t& s )
{
	double c[8][3];
	for( int i = 0; i < 8; i++ )
	{
		const double z = ( i & 4 ) ? s.zFar : s.zNear;
		c[i][0] = z * ( ( i & 1 ) ? s.tanRight : s.tanLeft );
		c[i][1] = z * ( ( i & 2 ) ? s.tanTop : s.tanBottom );
		c[i][2] = z;
	}
	double best = 0.0;
	for( int i = 0; i < 8; i++ )
	{
		for( int j = i + 1; j < 8; j++ )
		{
			const double dx = c[i][0] - c[j][0];
			const double dy = c[i][1] - c[j][1];
			const double dz = c[i][2] - c[j][2];
			best = Max( best, dx * dx + dy * dy + dz * dz );
		}
	}
	return sqrt( best );
}

// World units per texel of a res x res window that holds the whole slice wherever it lies on the grid, with
// 'marginTexels' to spare on every side (for filter taps that reach past a sampled point). The slice's extent along
// the grid is at most its diameter, and the snap puts the window's centre up to a texel off the slice's: so the window
// needs the diameter, one texel on each side, and the margins.
static double R_ShadowCacheTexelWorld( double diameter, int res, int marginTexels )
{
	return diameter / ( double )( res - 2 - 2 * marginTexels );
}

// Corner i of the slice in world space, in double, in the order idRenderMatrix::GetFrustumCorners uses: x from bit 0,
// y from bit 1, near or far from bit 2. The view axis is Doom's: [0] forward, [1] left, [2] up; the slice's x runs to
// the right. Built from the camera's origin and axis directly: the inverse of a view-projection matrix in float loses
// about a thousandth of the slice's size far from the world origin.
static void R_ShadowCacheSliceCorner( const shadowCacheSlice_t& s, const idVec3& viewOrigin, const idMat3& viewAxis, int i, double out[3] )
{
	const double z = ( i & 4 ) ? s.zFar : s.zNear;
	const double x = z * ( ( i & 1 ) ? s.tanRight : s.tanLeft );
	const double y = z * ( ( i & 2 ) ? s.tanTop : s.tanBottom );
	for( int k = 0; k < 3; k++ )
	{
		out[k] = ( double )viewOrigin[k] + z * ( double )viewAxis[0][k] - x * ( double )viewAxis[1][k] + y * ( double )viewAxis[2][k];
	}
}

// Centre a cascade's window on the middle of its slice's extent along the grid: not on the average of the corners,
// which lies off the middle of a slice seen at an angle.
static void R_ShadowCacheSliceWindow( const shadowLightFrame_t& frame, const shadowCacheSlice_t& s, const idVec3& viewOrigin, const idMat3& viewAxis,
									  double texelWorld, int res, shadowCacheWindow_t& out )
{
	double lo[2] = { 1e300, 1e300 };
	double hi[2] = { -1e300, -1e300 };
	for( int i = 0; i < 8; i++ )
	{
		double p[3];
		R_ShadowCacheSliceCorner( s, viewOrigin, viewAxis, i, p );
		const double g[2] =
		{
			frame.gridX.x * p[0] + frame.gridX.y * p[1] + frame.gridX.z * p[2],
			frame.gridY.x * p[0] + frame.gridY.y * p[1] + frame.gridY.z * p[2]
		};
		for( int k = 0; k < 2; k++ )
		{
			lo[k] = Min( lo[k], g[k] );
			hi[k] = Max( hi[k], g[k] );
		}
	}
	R_ShadowCacheSnapWindowAt( 0.5 * ( lo[0] + hi[0] ), 0.5 * ( lo[1] + hi[1] ), texelWorld, res, out );
}

// What the decide pass keeps between frames for the primary sun. Frontend-owned ( D36 ): read and
// written only by R_ShadowCacheDecide, on the thread that runs the frontend.
struct shadowCacheLogged_t
{
	double		minX;
	double		maxX;
	double		minY;
	double		maxY;
	int			res;
};

struct shadowCacheSunState_t
{
	const idRenderWorldLocal*	world;			// the reference below belongs to one render world
	int							mapHash;		// the game keeps one render world for every map: the map's name and
	ID_TIME_T					mapTimeStamp;	// time stamp tell a new map from the old one
	double						refTanX;		// the largest half-angle tangents the main view has shown ( D46 )
	double						refTanY;
	shadowCacheLogged_t			logged[SHADOWCACHE_MAX_CASCADES];	// what r_shadowCacheLog printed last
};

static shadowCacheSunState_t shadowCacheSun;

/*
=====================
R_ShadowCacheSunWindows

Places the primary sun's cascade windows for this view and hangs them on viewDef->shadowCacheView.
The size of a window comes from the reference field of view, its centre from this frame's slice;
the grid is anchored to the map origin and snapped in double. The light view sits at the relative
eye, and the depth range is the light volume's along the light, from the light def's absolute box.
The extents handed to the backend are measured from that eye.
=====================
*/
static void R_ShadowCacheSunWindows( viewDef_t* viewDef, const viewLight_t* sun )
{
	const renderView_t& renderView = viewDef->renderView;

	// a new map starts a new reference: the game keeps one render world for every map, so its pointer
	// alone never changes
	const int mapHash = viewDef->renderWorld->mapName.FileNameHash();
	if( shadowCacheSun.world != viewDef->renderWorld || shadowCacheSun.mapHash != mapHash || shadowCacheSun.mapTimeStamp != viewDef->renderWorld->mapTimeStamp )
	{
		memset( &shadowCacheSun, 0, sizeof( shadowCacheSun ) );
		shadowCacheSun.world = viewDef->renderWorld;
		shadowCacheSun.mapHash = mapHash;
		shadowCacheSun.mapTimeStamp = viewDef->renderWorld->mapTimeStamp;
	}

	// this view's half-angle tangents. The slice is the frustum the view is DRAWN with: R_SetupProjectionMatrix
	// applies neither r_centerX / r_centerY nor the stereo separation ( R_SetupProjectionMatrix2 does, for the
	// cascade frustums only ), and the shader selects a cascade by view depth alone
	const double tanX = tan( ( double )renderView.fov_x * ( double )idMath::PI / 360.0 );
	const double tanY = tan( ( double )renderView.fov_y * ( double )idMath::PI / 360.0 );

	// a field of view that is not a usable angle ( a first frame, a broken cinematic; NaN fails every test )
	// keeps today's crop this frame and must not reach the reference
	if( !( tanX > 0.0 && tanX < 100.0 && tanY > 0.0 && tanY < 100.0 ) )
	{
		return;
	}

	// the reference field of view only grows ( D46 )
	shadowCacheSun.refTanX = Max( shadowCacheSun.refTanX, tanX );
	shadowCacheSun.refTanY = Max( shadowCacheSun.refTanY, tanY );

	// the light direction, exactly as the parallel branch of SetupShadowMapMatrices computes it
	idVec3 lightDir = -sun->lightCenter;
	if( lightDir.Normalize() == 0.0f )
	{
		lightDir[2] = -1.0f;
	}
	shadowLightFrame_t frame;
	R_ShadowCacheLightFrame( lightDir, frame );

	// the light's own box on the grid and along the light, from the light DEF's absolute matrix ( bit-stable
	// while the light holds still; the frontend may read sun->lightDef )
	ALIGNTYPE16 frustumCorners_t corners;
	idRenderMatrix::GetFrustumCorners( corners, sun->lightDef->inverseBaseLightProject, bounds_zeroOneCube );
	double boxMin[3] = { 1e300, 1e300, 1e300 };
	double boxMax[3] = { -1e300, -1e300, -1e300 };
	for( int j = 0; j < 8; j++ )
	{
		const idVec3 p( corners.x[j], corners.y[j], corners.z[j] );
		const double g[3] = { R_ShadowCacheLightCoord( frame.gridX, p ), R_ShadowCacheLightCoord( frame.gridY, p ), R_ShadowCacheLightCoord( frame.axis[0], p ) };
		for( int k = 0; k < 3; k++ )
		{
			boxMin[k] = Min( boxMin[k], g[k] );
			boxMax[k] = Max( boxMax[k], g[k] );
		}
	}

	// where the backend's light view sits: at the relative eye ( exactly zero with r_cameraRelative 1, the eye
	// itself with 0 ), so every number handed over stays small on both paths. In map coordinates that point is
	// renderOrigin + the relative eye; the window and the depth range are measured from it, in double
	const idVec3 viewOrigin = R_RelativeEye( viewDef );
	const double anchor[3] = { viewDef->renderOrigin.x + ( double )viewOrigin.x, viewDef->renderOrigin.y + ( double )viewOrigin.y, viewDef->renderOrigin.z + ( double )viewOrigin.z };
	const double anchorGX = anchor[0] * ( double )frame.gridX.x + anchor[1] * ( double )frame.gridX.y + anchor[2] * ( double )frame.gridX.z;
	const double anchorGY = anchor[0] * ( double )frame.gridY.x + anchor[1] * ( double )frame.gridY.y + anchor[2] * ( double )frame.gridY.z;
	const double anchorAlong = anchor[0] * ( double )frame.axis[0].x + anchor[1] * ( double )frame.axis[0].y + anchor[2] * ( double )frame.axis[0].z;

	// the cascades R_SetupSplitFrustums gave this view: their far ends are frame data ( frustumSplitDistances
	// and, for the last one, shadowCascadeFar ), so the windows and the shader's cascade select cannot disagree
	int numCascades = 1;
	while( numCascades < SHADOWCACHE_MAX_CASCADES && viewDef->frustumSplitDistances[numCascades - 1] != idMath::INFINITUM )
	{
		numCascades++;
	}

	shadowCacheView_t* scv = ( shadowCacheView_t* )R_FrameAlloc( sizeof( shadowCacheView_t ) );
	scv->sunViewOrigin = viewOrigin;
	scv->sunAxis = idMat3( frame.axis[0], frame.axis[1], frame.axis[2] );
	// the light view looks along axis[0]: its near plane is the box's nearest point along the light
	scv->zNear = boxMin[2] - anchorAlong;
	scv->zFar = boxMax[2] - anchorAlong;
	scv->numCascades = numCascades;

	const int res = shadowMapResolutions[sun->shadowLOD];
	const int marginTexels = Max( SHADOWCACHE_MARGIN_TEXELS, ( int )ceil( r_shadowMapJitterScale.GetFloat() + 1.0f ) );

	float zNear = renderView.cramZNear ? ( r_znear.GetFloat() * 0.25f ) : r_znear.GetFloat();

	for( int i = 0; i < numCascades; i++ )
	{
		const float zFar = ( i < numCascades - 1 ) ? viewDef->frustumSplitDistances[i] : viewDef->shadowCascadeFar;

		// the reference slice ( the size ) and this frame's slice ( the centre )
		shadowCacheSlice_t refSlice;
		refSlice.zNear = zNear;
		refSlice.zFar = zFar;
		refSlice.tanLeft = -shadowCacheSun.refTanX;
		refSlice.tanRight = shadowCacheSun.refTanX;
		refSlice.tanBottom = -shadowCacheSun.refTanY;
		refSlice.tanTop = shadowCacheSun.refTanY;

		shadowCacheSlice_t slice = refSlice;
		slice.tanLeft = -tanX;
		slice.tanRight = tanX;
		slice.tanBottom = -tanY;
		slice.tanTop = tanY;

		const double texelWorld = R_ShadowCacheTexelWorld( R_ShadowCacheSliceDiameter( refSlice ), res, marginTexels );
		shadowCacheWindow_t win;
		R_ShadowCacheSliceWindow( frame, slice, renderView.vieworg, renderView.viewaxis, texelWorld, res, win );

		double minX, maxX, minY, maxY;
		R_ShadowCacheWindowExtent( win, minX, maxX, minY, maxY );

		// the box clamp is per axis and all-or-nothing
		shadowCacheCascade_t& c = scv->cascades[i];
		c.originX = win.originX;
		c.originY = win.originY;
		c.texelWorld = texelWorld;
		c.res = res;
		c.clampedX = ( boxMax[0] - boxMin[0] ) <= ( maxX - minX );
		if( c.clampedX )
		{
			minX = boxMin[0];
			maxX = boxMax[0];
		}
		c.clampedY = ( boxMax[1] - boxMin[1] ) <= ( maxY - minY );
		if( c.clampedY )
		{
			minY = boxMin[1];
			maxY = boxMax[1];
		}

		// the extents measured from the light view's origin. The light view's x is the grid's x; its y is
		// MINUS the grid's y ( row 0 of the window is the top of the view )
		c.left = minX - anchorGX;
		c.right = maxX - anchorGX;
		c.bottom = -( maxY - anchorGY );
		c.top = -( minY - anchorGY );

		if( r_shadowCacheLog.GetBool() )
		{
			shadowCacheLogged_t& prev = shadowCacheSun.logged[i];
			if( prev.minX != minX || prev.maxX != maxX || prev.minY != minY || prev.maxY != maxY || prev.res != res )
			{
				prev.minX = minX;
				prev.maxX = maxX;
				prev.minY = minY;
				prev.maxY = maxY;
				prev.res = res;
				common->Printf( "shadowCache: frame %d cascade %d res %d texel %.9g origin %lld %lld clamp %d %d x %.6f %.6f y %.6f %.6f rel %.6f %.6f %.6f %.6f fold %.6f %.6f depth %.6f %.6f\n",
								tr.frameCount, i, res, texelWorld, ( long long )c.originX, ( long long )c.originY, ( int )c.clampedX, ( int )c.clampedY, minX, maxX, minY, maxY,
								c.left, c.right, c.bottom, c.top, anchorGX, anchorGY, scv->zNear + anchorAlong, scv->zFar + anchorAlong );
			}
		}

		// the next cascade begins half a percent inside this one, as R_SetupSplitFrustums has it
		zNear = zFar - ( zFar * 0.005f );
	}

	viewDef->shadowCacheView = scv;
}

/*
=====================
R_ShadowCacheParallelMatrices

The parallel-light matrices for one cascade record; called by the backend. The light view sits at
the relative eye, the depth range and the extents come finished from the frontend, and the
projection is MatrixOrthogonalProjectionRH's matrix transposed, computed in double.
=====================
*/
void R_ShadowCacheParallelMatrices( const shadowCacheView_t& scv, const shadowCacheCascade_t& cascade,
									idRenderMatrix& lightViewRenderMatrix, idRenderMatrix& lightProjectionRenderMatrix )
{
	idRenderMatrix::CreateViewMatrix( scv.sunViewOrigin, scv.sunAxis, lightViewRenderMatrix );

	lightProjectionRenderMatrix = idRenderMatrix(
									  ( float )( 2.0 / ( cascade.right - cascade.left ) ), 0.0f, 0.0f, ( float )( ( cascade.left + cascade.right ) / ( cascade.left - cascade.right ) ),
									  0.0f, ( float )( 2.0 / ( cascade.top - cascade.bottom ) ), 0.0f, ( float )( ( cascade.top + cascade.bottom ) / ( cascade.bottom - cascade.top ) ),
									  0.0f, 0.0f, ( float )( 1.0 / ( scv.zNear - scv.zFar ) ), ( float )( scv.zNear / ( scv.zNear - scv.zFar ) ),
									  0.0f, 0.0f, 0.0f, 1.0f );
}

/*
==========================================================================================

S3a: the records, the change history and reuse ( local lights )

Frontend-owned ( D36 ). The decide pass and the two notify hooks write this state on the
frontend's thread, between frames or while it is stopped; the light jobs only read it
( R_ShadowCacheWantsGather, R_ShadowCacheSunWantsGather ); R_ShadowCachePublish copies the
backend's miss count and its ring of missed serials while the frontend is idle.

==========================================================================================
*/

#define SHADOWCACHE_LONG_AGO		( -0x40000000 )		// the "last change" of what has not changed since its map began

// Static or active, for the cache: has this object held still long enough to belong in a static map? It is
// static from the frame it has not changed for 'staticFrames' frames, and active again the frame it changes,
// so the active-to-static transition is rare and the cache is touched rarely. The interval is at least 1:
// with 0, an object changed after a frame's decisions would count as already in the map. What has not
// changed since its map began ( SHADOWCACHE_LONG_AGO ) is static always. Only cacheable kinds are asked:
// geometry that is the same every frame until the game changes it ( no skinning, particles, per-frame
// callbacks or self-deforming materials ).
static bool R_ShadowCacheIsStatic( int frameCount, int lastChangedFrame, int staticFrames )
{
	return lastChangedFrame == SHADOWCACHE_LONG_AGO || frameCount - lastChangedFrame >= staticFrames;
}

// Is the object in a unit's static map? Each unit records 'heldThrough', the frame its map was last brought
// up to date: that update drew every caster that was static by then. An object that turned static later, or
// has changed since ( its lastChangedFrame moved forward ), is not in the map, and is drawn with the unit's
// active casters until the unit's next update adds it. Membership is a fact about the unit, not about the
// frame, so a unit skipped for any number of frames ( out of view, out of budget, a view not drawn ) loses
// nothing. A surface that cannot be cached even on a cacheable object ( a deforming material ) is drawn with
// the active casters whatever this returns.
static bool R_ShadowCacheInUnit( int lastChangedFrame, int staticFrames, int heldThrough )
{
	return lastChangedFrame == SHADOWCACHE_LONG_AGO || heldThrough - lastChangedFrame >= staticFrames;
}

// The history of changes to statics: the old bounds of every static that moved, changed or was removed, recorded where
// the engine tears an entity's derived data down ( one choke point ). A ring, read by sequence number: each unit remembers
// how many changes it had seen when it was last brought up to date ( 'changesSeen' ), and next time repairs every change
// recorded since, however many frames it was skipped. Sequence numbers, not frames: a change recorded after this frame's
// decisions ( a console command runs between the frontend and the next frame ) still counts as unseen. Recording never
// fails and never blocks.
#define SHADOWCACHE_MAX_CHANGES		1024		// a power of two

struct shadowCacheChanges_t
{
	idBounds		oldBounds[SHADOWCACHE_MAX_CHANGES];	// world space: where each static was when it was drawn into the caches
	int				total;								// changes ever recorded ( 0 at load ); change n is oldBounds[ n % SHADOWCACHE_MAX_CHANGES ]
};

static void R_ShadowCacheRecordChange( shadowCacheChanges_t& c, const idBounds& oldWorldBounds )
{
	c.oldBounds[ c.total & ( SHADOWCACHE_MAX_CHANGES - 1 ) ] = oldWorldBounds;
	c.total++;
}

// The changes a unit has not seen: numbers [begin, end). False when some of them have already been overwritten: the unit
// cannot know what changed, and rebuilds. Either way the unit's changesSeen becomes 'end' once its update is emitted.
static bool R_ShadowCacheChangesSince( const shadowCacheChanges_t& c, int changesSeen, int& begin, int& end )
{
	const int oldest = Max( 0, c.total - SHADOWCACHE_MAX_CHANGES );
	end = c.total;
	begin = Max( changesSeen, oldest );
	return changesSeen >= oldest;
}

#define SHADOWCACHE_MAX_LIGHTS		4096				// records are indexed by idRenderLightLocal::index; a light beyond this is drawn uncached
#define SHADOWCACHE_RETRY_FRAMES	64					// a light the atlas has no room for asks for a place again on this frame
#define SHADOWCACHE_CAP_RETRY_FRAMES	16				// a frame in which the contacted-light cap was hit: no full gather for this many frames
#define SHADOWCACHE_VALUE_MARGIN_NUM	5				// S3b: a light of this view gives its place up only to one whose maps are worth
#define SHADOWCACHE_VALUE_MARGIN_DEN	4				// 5 / 4 of its own, so that two lights of nearly equal value do not take turns
#define SHADOWCACHE_DEFAULT_CASTERS		32				// S3b: the casters a turn is priced with while a light's chain has never been counted
#define SHADOWCACHE_RECOUNT_FRAMES		256				// S3b: a count at least this old, and as old as the static interval, is taken again before it refuses a light a turn
#define SHADOWCACHE_PICK_HOLD_FRAMES	30				// S3b: frames the engine's pick of a size must hold before maps of another size are built again at it
#define SHADOWCACHE_SIDE_COST			100				// S3b: what one side of a build costs before any caster, in the budget's units ( its clear, its pass, the state changes around them )
#define SHADOWCACHE_KEY_FREE			( ( int64 ) - 1 )		// S3b: the keys of R_ShadowCacheVictimKey and R_ShadowCachePlace: a block nobody holds,
#define SHADOWCACHE_KEY_KEPT			( ( int64 ) - 2 )		// a record that may not give way,
#define SHADOWCACHE_KEY_IN_USE			( ( int64 )1 << 40 )	// and the base of the keys of maps in use: above every frame stamp

static_assert( SHADOWCACHE_MAX_LIGHTS < 65536 && SHADOWCACHE_MAX_CELLS <= 32768, "shadowCacheState_t::cellOwner holds a record's index plus one, shadowCacheLightRec_t::cell a cell" );

// One per light of the map's world that has been a cache candidate, indexed by idRenderLightLocal::index.
struct shadowCacheLightRec_t
{
	int				uniqueId;		// idRenderLightLocal::uniqueId of the light the record is for; 0 = none
	unsigned int	serial;			// side i's static map is build serial + i
	int				heldThrough;	// tr.frameCount of the decide pass that last ordered the maps built: decides which entities they hold ( R_ShadowCacheInUnit )
	int				changesSeen;	// shadowCacheState.changes.total when a decide pass last looked at this light
	int				shapeFrame;		// tr.frameCount of the light's last change of shape ( the light hook ); SHADOWCACHE_LONG_AGO = none seen
	int				lastUsedFrame;	// tr.frameCount of the last decide pass that had the light as a cache candidate
	int				wantSince;		// tr.frameCount since which the maps wait to be built or brought up to date; 0 = they do not wait
	int				retryFrame;		// no place was free for maps of the engine's pick, or the contacted-light cap was hit: no full gather and no build until this frame
	int				epoch;			// shadowCacheState.epoch the maps were built under
	unsigned int	doorSig;		// R_ShadowCacheDoorSignature when the maps were built
	int				casters;		// S3b: surfaces of the chain that were static when the light last gathered in full ( R_ShadowCacheCountStatic ): what its maps save each frame and what a build of them costs; -1 = never counted
	int				countFrame;		// S3b: tr.frameCount of that count. An old count is taken again before it can keep the light without a place: what has come to rest since is static now ( pass 4 of R_ShadowCacheEmitOps )
	int				pickFrame;		// S3b: tr.frameCount of the decide pass that first saw the engine pick the size in 'pick', or SHADOWCACHE_PICK_HOLD_FRAMES before it for a light that had been out of the main view that long: maps of another size are built again only when the pick has held ( SHADOWCACHE_PICK_HOLD_FRAMES )
	short			cell[6];		// S3b: Z-order cell of each side's block in the cache atlas; meaningful for the first numSides while size != 0
	short			size;			// side of each map in texels; 0 = no place
	byte			numSides;
	bool			valid;			// the maps hold what the record says
	bool			repair;			// a change made valid maps stale: their rebuild does not wait for the budget ( S83 )
	bool			granted;		// S3b: the decide pass gave the light a turn: its job gathers in full in the next frame
	short			pick;			// S3b: the size the engine picked for the light when it was last in a main view ( pass 1 of R_ShadowCacheEmitOps ): maps of another size are not shown, so their place saves nothing
};

// S3a: the primary sun's static maps, one per cascade. WHICH entities they hold is decided for the
// light, not per cascade ( heldThrough ): the caster bits carry one static bit per surface for all cascades,
// so every cascade map must hold the same entities. A cascade whose window moved is drawn again with that
// membership; when the membership itself must be brought up to date ( an unseen change touches the light,
// an entity turned static ), all cascades are drawn again.
struct shadowCacheSunCascadeRec_t
{
	int64			originX;		// the window the map was drawn with ( shadowCacheCascade_t ); not compared on a clamped axis
	int64			originY;
	double			texelWorld;
	int				res;
	unsigned int	serial;			// the build of this cascade's map
	bool			clampedX;
	bool			clampedY;
	bool			valid;
	byte			pad;
};

struct shadowCacheSunRec_t
{
	int							uniqueId;		// the sun light's idRenderLightLocal::uniqueId; 0 = none
	int							heldThrough;	// tr.frameCount of the decide pass that last drew every cascade: which entities the maps hold
	int							changesSeen;	// shadowCacheState.changes.total when a decide pass last looked
	int							epoch;			// shadowCacheState.epoch the membership was set under
	int							wantSince;		// tr.frameCount since which an entity that turned static waits to be drawn into the maps; 0 = none
	int							retryFrame;		// the contacted-light cap was hit: no full gather until this frame
	int							numCascades;
	bool						valid;			// every cascade holds a usable map and the membership stands
	// the main view the windows were last placed for: while the view is the same, so are the windows, and
	// the light job need not gather in full
	idVec3						viewOrigin;
	idMat3						viewAxis;
	float						fovX;
	float						fovY;
	shadowCacheSunCascadeRec_t	cascades[SHADOWCACHE_MAX_CASCADES];
};

struct shadowCacheState_t
{
	const idRenderWorldLocal*	world;				// the records belong to one map of one render world
	int							mapHash;
	ID_TIME_T					mapTimeStamp;
	int							mapStartFrame;		// tr.frameCount of the map's first decide pass ( R_ShadowCacheLastChanged )
	int							mapStartId;			// the world's nextUniqueId at that pass: a light with a smaller id was there when the map began
	int							epoch;				// bumped when every map must be built again
	unsigned int				settings;			// R_ShadowCacheSettings at the last decide pass
	int							materialFrees;		// c_materialFrees at the last decide pass
	unsigned int				nextSerial;			// builds ordered so far; never reset. It wraps after 2^32 builds; 0 is also the empty stamp, so the backend treats an op with serial 0 as missed and the record is built again with the next serial
	unsigned int				missesPublished;	// written by R_ShadowCachePublish
	unsigned int				missesSeen;
	unsigned int				missedPublished[SHADOWCACHE_MISS_RING];	// the backend's ring of missed serials, as R_ShadowCachePublish copied it
	int							recsHigh;			// one more than the highest record index ever claimed in this map
	int							atlasCells;			// cells of the cache atlas the places were handed out in; 0 = none yet
	// for r_shadowCacheLog: this view's lights by decision, and totals since the map began
	int							reused;
	int							rebuilt;
	int							repaired;
	int							waiting;
	int							moving;
	int							sunReused;		// the sun's cascades of this view that kept their map
	int							sunRebuilt;		// the sun's cascades of this view drawn again
	int							totalRebuilt;
	int							totalRepaired;
	int							totalEvicted;
	int							evicted;		// S3b: records that gave their place up in this view's decide pass
	int							capHit;			// S3b: this view: an entity that can be in a static map touched more lights than the engine keeps per entity ( nothing is built )
	int							turns;			// S3b: lights given a turn for the next frame
	int							costBuilt;		// S3b: what this view's budgeted builds cost ( R_ShadowCacheCost )
	int							cellsUsed;		// S3b: cells of the cache atlas inside a record's place
	shadowCacheChanges_t		changes;
	unsigned short				cellOwner[SHADOWCACHE_MAX_CELLS];	// the cache atlas, by Z-order cell: the index + 1 of the record whose map covers the cell, 0 = free
	shadowCacheLightRec_t		recs[SHADOWCACHE_MAX_LIGHTS];
	shadowCacheSunRec_t			sun;
};

static shadowCacheState_t shadowCacheState;

// The frame an entity last changed, for the cache. An entity not updated since the first decide pass of its
// map counts as unchanged since ever: otherwise every entity of a new map would turn static in one frame.
static int R_ShadowCacheLastChanged( const idRenderEntityLocal* def )
{
	return ( def->lastModifiedFrameNum <= shadowCacheState.mapStartFrame ) ? SHADOWCACHE_LONG_AGO : def->lastModifiedFrameNum;
}

// S3b: marks every block of the record's place as the record's own, or as free, in the cell map
static void R_ShadowCacheMarkPlace( const shadowCacheLightRec_t& rec, bool used )
{
	shadowCacheState_t& s = shadowCacheState;
	const int span = rec.size / SHADOWCACHE_CELL;
	const int cells = span * span;
	const unsigned short owner = ( unsigned short )( used ? ( &rec - s.recs + 1 ) : 0 );
	for( int i = 0; i < rec.numSides; i++ )
	{
		for( int c = 0; c < cells; c++ )
		{
			s.cellOwner[rec.cell[i] + c] = owner;
		}
	}
	s.cellsUsed += used ? rec.numSides * cells : -( rec.numSides * cells );
}

// Gives the record's place in the cache atlas back; its maps are gone with it.
static void R_ShadowCacheFreePlace( shadowCacheLightRec_t& rec )
{
	if( rec.size >= SHADOWCACHE_CELL )
	{
		R_ShadowCacheMarkPlace( rec, false );
	}
	rec.size = 0;
	rec.numSides = 0;
	rec.valid = false;
	rec.retryFrame = 0;		// a light that gave its place back asks for a new one at once ( the flush and an
							// atlas resize free every place through here; a new map zeroes the records; a
							// rebuilt or evicted record tries again now; a claimed slot is another light's )
}

// S3b: what a record's maps are worth ( D77: eviction by value ): the static caster surfaces a cached copy
// saves from being drawn each frame, per cell of the cache atlas the maps take, times 256 so that integers
// compare. A count that is not known ( -1 ) is worth 0.
static int R_ShadowCacheValue( int casters, int size, int sides )
{
	const int span = size / SHADOWCACHE_CELL;
	return ( Max( 0, casters ) * 256 ) / ( sides * span * span );
}

// S3b: the a-priori price of building a light's static maps ( D77: texels times casters; the frame's GPU
// time is five frames old when the decide pass could read it ): per side, the cells of cache atlas drawn
// into times the surfaces drawn into them, and a fixed part for what a side costs before any caster ( its
// clear, its pass, the state changes around them ).
static int R_ShadowCacheCost( int casters, int size, int sides )
{
	const int span = size / SHADOWCACHE_CELL;
	return sides * ( SHADOWCACHE_SIDE_COST + span * span * Max( 1, casters ) );
}

// S3b: the order in which records give their places up to a newcomer whose maps would be worth 'value': the
// smaller key first, or SHADOWCACHE_KEY_KEPT for a record that may not give way. Maps that save nothing now
// always may, the longest unused first: those of a light that is not in this view, and maps that are not
// the size the engine picks now, which are not shown. Then the maps in use, by value, and those only while
// they are worth less than the newcomer by a margin, so that two lights of nearly equal value do not take
// turns. Never a map built this frame ( its build is ordered; with 'nextFrame' the question is about the
// frame after, when that no longer holds ).
static int64 R_ShadowCacheVictimKey( const shadowCacheLightRec_t& victim, int value, bool nextFrame )
{
	if( victim.lastUsedFrame != tr.frameCount || victim.size != victim.pick )
	{
		return ( int64 )victim.lastUsedFrame;
	}
	if( !nextFrame && victim.valid && victim.heldThrough == tr.frameCount )
	{
		return SHADOWCACHE_KEY_KEPT;
	}
	const int victimValue = R_ShadowCacheValue( victim.casters, victim.size, victim.numSides );
	if( victimValue * SHADOWCACHE_VALUE_MARGIN_NUM >= value * SHADOWCACHE_VALUE_MARGIN_DEN )
	{
		return SHADOWCACHE_KEY_KEPT;
	}
	return SHADOWCACHE_KEY_IN_USE + victimValue;
}

/*
=====================
R_ShadowCachePlace

S3b: a place in the cache atlas for the record's maps: 'sides' blocks of 'size' texels, for maps
worth 'value'. The cells of a place the record already has count as free: its maps are drawn
again in any case. With 'commit' false nothing is changed: the answer says whether the place
could be had ( pass 4 of R_ShadowCacheEmitOps asks so, with 'nextFrame', before it gives a
light a turn ).

The atlas is handed out in square blocks of 1, 4, 16 or 64 cells, each at a multiple of its own
cell count in Z order: the nodes of a quadtree, so a block never straddles a larger one. This
is a buddy allocator kept as the cell map alone: a node is free when all of its cells are, and
four free siblings are a free parent with no bookkeeping.
The map holds, per cell, the record whose map covers it.

One walk over the blocks of the size asked for finds the free ones, the lowest first ( on an
empty atlas this places the maps exactly as S2's per-frame assignment did ), and, where those
do not suffice, the occupied blocks that cost least: a block costs what its dearest occupant
is worth ( R_ShadowCacheVictimKey: maps that save nothing now before any in use, then by
value ), and is no candidate when one of its occupants may not give its place up ( a light
of this view only when it is worth less than the newcomer by the margin; S84, D77: no light
keeps a place because it came first, or because it is in view; frames in flight copy from a
freed place before any later frame draws into it ). Only the occupants of the blocks taken
lose their places, and only when all the blocks were found. False = no place, and nothing has
changed.
=====================
*/
static bool R_ShadowCachePlace( shadowCacheLightRec_t& rec, int size, int sides, int value, bool nextFrame, bool commit )
{
	shadowCacheState_t& s = shadowCacheState;
	const int span = size / SHADOWCACHE_CELL;
	const int cells = span * span;
	const unsigned short self = ( unsigned short )( &rec - s.recs + 1 );

	// the 'sides' cheapest blocks so far, in order of cost ( SHADOWCACHE_KEY_FREE first ), then of position
	short block[6];
	int64 blockKey[6];
	int numBlocks = 0;
	for( int first = 0; first + cells <= s.atlasCells; first += cells )
	{
		int64 key = SHADOWCACHE_KEY_FREE;
		unsigned short lastOwner = 0;		// a map's cells follow one another: its record is asked once
		for( int c = 0; c < cells && key != SHADOWCACHE_KEY_KEPT; c++ )
		{
			const unsigned short owner = s.cellOwner[first + c];
			if( owner == 0 || owner == self || owner == lastOwner )
			{
				continue;
			}
			lastOwner = owner;
			const int64 ownerKey = R_ShadowCacheVictimKey( s.recs[owner - 1], value, nextFrame );
			key = ( ownerKey == SHADOWCACHE_KEY_KEPT ) ? SHADOWCACHE_KEY_KEPT : Max( key, ownerKey );
		}
		if( key == SHADOWCACHE_KEY_KEPT )
		{
			continue;		// one of its occupants may not give way
		}
		int at = numBlocks;
		while( at > 0 && blockKey[at - 1] > key )
		{
			at--;
		}
		if( at >= sides )
		{
			continue;		// dearer than all that are kept
		}
		for( int k = Min( numBlocks, sides - 1 ); k > at; k-- )
		{
			block[k] = block[k - 1];
			blockKey[k] = blockKey[k - 1];
		}
		block[at] = ( short )first;
		blockKey[at] = key;
		numBlocks = Min( numBlocks + 1, sides );
		if( numBlocks == sides && blockKey[sides - 1] == SHADOWCACHE_KEY_FREE )
		{
			break;		// free blocks for every side: nothing cheaper follows
		}
	}
	if( numBlocks < sides )
	{
		return false;		// nothing has changed
	}
	if( !commit )
	{
		return true;		// nothing has changed either
	}

	// the record's own old place first ( its cells may lie in the blocks taken ), then the occupants of
	// the blocks taken, and nobody else
	if( rec.size != 0 )
	{
		R_ShadowCacheMarkPlace( rec, false );
	}
	for( int k = 0; k < sides; k++ )
	{
		if( blockKey[k] == SHADOWCACHE_KEY_FREE )
		{
			continue;
		}
		for( int c = 0; c < cells; c++ )
		{
			const unsigned short owner = s.cellOwner[block[k] + c];
			if( owner != 0 )
			{
				R_ShadowCacheFreePlace( s.recs[owner - 1] );
				s.evicted++;
				s.totalEvicted++;
			}
		}
	}
	for( int k = 0; k < sides; k++ )
	{
		rec.cell[k] = block[k];
	}
	rec.size = ( short )size;
	rec.numSides = ( byte )sides;
	R_ShadowCacheMarkPlace( rec, true );
	return true;
}

// The record of a light of the map's world; NULL when the light can have none. A slot that still holds
// another light's record ( that light is gone: ids are never reused ) is claimed for this one.
static shadowCacheLightRec_t* R_ShadowCacheClaimRec( const idRenderLightLocal* light )
{
	shadowCacheState_t& s = shadowCacheState;
	if( s.world == NULL || light->world != s.world || light->index < 0 || light->index >= SHADOWCACHE_MAX_LIGHTS || light->uniqueId == 0 )
	{
		return NULL;
	}
	shadowCacheLightRec_t& rec = s.recs[light->index];
	if( rec.uniqueId != light->uniqueId )
	{
		if( rec.uniqueId != 0 )
		{
			R_ShadowCacheFreePlace( rec );
		}
		memset( &rec, 0, sizeof( rec ) );		// size 0: no place
		rec.uniqueId = light->uniqueId;
		rec.casters = -1;		// never counted
		// no hook fires when a light is made: one made during play ( a projectile's, a muzzle flash ) counts as
		// changing shape from the frame it is first met, so nothing is built for it until it has held still
		rec.shapeFrame = ( light->uniqueId < s.mapStartId ) ? SHADOWCACHE_LONG_AGO : tr.frameCount;
		s.recsHigh = Max( s.recsHigh, light->index + 1 );
	}
	return &rec;
}

// Which of the light's areas are connected to its own. R_AddSingleLight gathers shadow casters only from
// those ( r_useAreasConnectedForShadowCulling 2 ), so a door that opens or closes changes what a static map
// must hold. The sum of a mixed value per area does not depend on the order of the list.
static unsigned int R_ShadowCacheDoorSignature( const idRenderLightLocal* light )
{
	if( light->areaNum == -1 || r_useAreasConnectedForShadowCulling.GetInteger() != 2 )
	{
		return 0;
	}
	unsigned int sig = 1;
	for( const areaReference_t* ref = light->references; ref != NULL; ref = ref->ownerNext )
	{
		if( light->world->AreasAreConnected( light->areaNum, ref->area->areaNum, PS_BLOCK_VIEW ) )
		{
			unsigned int h = ( unsigned int )( ref->area->areaNum + 1 ) * 2654435761u;
			h ^= h >> 15;
			h *= 2246822519u;
			h ^= h >> 13;
			sig += h;
		}
	}
	return sig;
}

// The cvars that shape stored depth or decide which casters a static map holds, as one number: a change
// starts a new epoch. r_shadowCacheReuse is one of them: without reuse a map holds every cacheable caster,
// with it only those that had held still, and the entity hook relies on the second rule.
static unsigned int R_ShadowCacheSettings()
{
	const float f[] = { r_shadowMapFrustumFOV.GetFloat(), r_shadowMapPolygonFactor.GetFloat(), r_dxShadowMapPolygonOffset.GetFloat(),
						r_vkShadowMapPolygonOffset.GetFloat(), r_znear.GetFloat(), r_shadowMapSplitWeight.GetFloat(), r_shadowMapSplitFar.GetFloat()
					  };
	const int n[] = { r_shadowMapOccluderFacing.GetInteger(), r_forceShadowMapsOnAlphaTestedSurfaces.GetInteger(), r_useConstantMaterials.GetInteger(),
					  r_distanceCull.GetInteger(), r_shadowMapSingleSide.GetInteger(), r_useAreasConnectedForShadowCulling.GetInteger(),
					  r_shadowCacheStaticFrames.GetInteger(), r_shadowCacheReuse.GetInteger(), r_shadowMapSplits.GetInteger(),
					  r_singleEntity.GetInteger(), r_singleSurface.GetInteger(), r_cameraRelative.GetInteger()
					};
	unsigned int hash = 2166136261u;
	for( int i = 0; i < ( int )( sizeof( f ) / sizeof( f[0] ) ); i++ )
	{
		unsigned int bits;
		memcpy( &bits, &f[i], sizeof( bits ) );
		hash = ( hash ^ bits ) * 16777619u;
	}
	for( int i = 0; i < ( int )( sizeof( n ) / sizeof( n[0] ) ); i++ )
	{
		hash = ( hash ^ ( unsigned int )n[i] ) * 16777619u;
	}
	return hash;
}

// Read only, called from the light jobs ( through R_ShadowCacheFullGather ): must this local light's chain
// hold every static caster of its volume this frame, because its static maps may be built?
static bool R_ShadowCacheWantsGather( const idRenderLightLocal* light )
{
	const shadowCacheState_t& s = shadowCacheState;
	if( s.world == NULL || light->world != s.world || light->index < 0 || light->index >= SHADOWCACHE_MAX_LIGHTS || light->uniqueId == 0 )
	{
		return false;		// it can have no record: drawn uncached
	}
	if( light->transitionInteractionPending )
	{
		return false;		// its interactions are still being made, past every hook: drawn uncached until they are
	}
	const shadowCacheLightRec_t& rec = s.recs[light->index];
	if( rec.uniqueId != light->uniqueId )
	{
		return false;		// S3b: never a candidate so far: the decide pass gives it a record and, in its turn, a full gather
	}
	if( tr.frameCount - rec.shapeFrame < r_shadowCacheStaticFrames.GetInteger() )
	{
		return false;		// a light that is changing shape is drawn uncached
	}
	if( tr.frameCount < rec.retryFrame )
	{
		return false;		// no place was free: it waits, and need not gather in full while it waits
	}
	// S3b: maps a change made stale are built again at once; every other build waits for the turn the decide
	// pass gives ( R_ShadowCacheEmitOps, pass 4 ), so the lights that wait do not all gather in full every frame
	// ( a turn is for the frame after the decide pass that gave it: a light that then is not in the view,
	// or has no shadow to draw, must not keep gathering in full on an old turn )
	if( rec.repair || ( rec.granted && rec.lastUsedFrame == tr.frameCount - 1 ) )
	{
		return true;
	}
	if( !rec.valid || rec.epoch != s.epoch )
	{
		return false;		// its maps wait for a turn
	}
	// the changes this record has not seen: only one whose old bounds touch the light can make its maps
	// stale ( the decide pass makes the same test and latches it; this is the job's read-only copy )
	int begin, end;
	if( !R_ShadowCacheChangesSince( s.changes, rec.changesSeen, begin, end ) )
	{
		return true;
	}
	for( int n = begin; n < end; n++ )
	{
		if( light->globalLightBounds.IntersectsBounds( s.changes.oldBounds[n & ( SHADOWCACHE_MAX_CHANGES - 1 )] ) )
		{
			return true;
		}
	}
	return false;
}

// Read only, called from the light jobs: must the sun's chain hold every cacheable caster this frame? Yes
// when the membership must be brought up to date, and whenever the main view is not the one the windows
// were last placed for: a window may then have moved, and a moved cascade is drawn again.
static bool R_ShadowCacheSunWantsGather( const viewDef_t* viewDef, const idRenderLightLocal* light )
{
	if( light->transitionInteractionPending )
	{
		return false;		// its interactions are still being made, past every hook: drawn uncached until they are
	}
	const shadowCacheState_t& s = shadowCacheState;
	const shadowCacheSunRec_t& sun = s.sun;
	if( s.world == NULL || light->world != s.world || sun.uniqueId != light->uniqueId || !sun.valid
			|| sun.epoch != s.epoch || sun.changesSeen != s.changes.total || sun.wantSince != 0 )
	{
		return true;
	}
	if( tr.frameCount < sun.retryFrame )
	{
		return false;		// the cap was hit: it waits, and need not gather in full while it waits
	}
	const renderView_t& rv = viewDef->renderView;
	return rv.vieworg != sun.viewOrigin || rv.viewaxis != sun.viewAxis || rv.fov_x != sun.fovX || rv.fov_y != sun.fovY;
}

// The backend found no map, or another one, where an op expected build serial ( the frame that drew it was
// never executed, or the cache images were lost ). The record that still expects that build is stale; one
// that has been rebuilt since has another serial and is left alone, so a miss reported late costs nothing.
// The sun: a missed cascade loses its map, and the emitter draws every cascade again when the sun is not
// valid ( the membership is one stamp for all of them ), so a missed cascade is rebuilt with the others.
static void R_ShadowCacheMissed( unsigned int serial )
{
	shadowCacheState_t& s = shadowCacheState;
	for( int i = 0; i < s.recsHigh; i++ )
	{
		shadowCacheLightRec_t& rec = s.recs[i];
		if( rec.uniqueId != 0 && rec.valid && serial - rec.serial < ( unsigned int )rec.numSides )
		{
			rec.valid = false;
			rec.repair = true;		// built again at once: a lost frame costs one rebuild ( D74 )
			return;
		}
	}
	for( int i = 0; i < s.sun.numCascades; i++ )
	{
		if( s.sun.cascades[i].valid && s.sun.cascades[i].serial == serial )
		{
			s.sun.cascades[i].valid = false;
			s.sun.valid = false;	// the light job reads this flag and gathers in full
		}
	}
}

// The start of a main view's cache decisions: a new map throws every record away; a setting that shapes the
// maps or a flush starts a new epoch, which no built map passes. Misses the backend reported mark only the
// records that expect the missed builds; only more misses than the ring holds start a new epoch.
static void R_ShadowCacheBeginView( const viewDef_t* viewDef )
{
	shadowCacheState_t& s = shadowCacheState;
	const idRenderWorldLocal* world = viewDef->renderWorld;
	const int mapHash = world->mapName.FileNameHash();
	if( s.world != world || s.mapHash != mapHash || s.mapTimeStamp != world->mapTimeStamp )
	{
		s.world = world;
		s.mapHash = mapHash;
		s.mapTimeStamp = world->mapTimeStamp;
		s.mapStartFrame = tr.frameCount;
		s.mapStartId = world->nextUniqueId;
		s.epoch++;
		s.settings = R_ShadowCacheSettings();
		s.recsHigh = 0;
		s.totalRebuilt = 0;
		s.totalRepaired = 0;
		s.totalEvicted = 0;
		s.changes.total = 0;
		s.atlasCells = 0;
		s.cellsUsed = 0;
		memset( s.cellOwner, 0, sizeof( s.cellOwner ) );
		memset( s.recs, 0, sizeof( s.recs ) );
		memset( &s.sun, 0, sizeof( s.sun ) );
	}

	// the cache atlas may have been created again at another size ( r_shadowCacheSize, an image reload ): no
	// place handed out in the old one stands
	const int atlasSide = globalImages->shadowCacheAtlasImage->GetUploadWidth() / SHADOWCACHE_CELL;
	const int atlasCells = Min( atlasSide * atlasSide, SHADOWCACHE_MAX_CELLS );
	if( atlasCells != s.atlasCells )
	{
		for( int i = 0; i < s.recsHigh; i++ )
		{
			if( s.recs[i].uniqueId != 0 )
			{
				R_ShadowCacheFreePlace( s.recs[i] );
			}
		}
		memset( s.cellOwner, 0, sizeof( s.cellOwner ) );
		s.cellsUsed = 0;
		s.atlasCells = atlasCells;
		s.epoch++;
		if( r_shadowCacheLog.GetBool() )
		{
			// S3b: the capacity, stated ( D77 ): six faces of a point light take 96 cells at 512, 24 at 256, 6 at 128
			common->Printf( "shadowCache: cache atlas %d x %d, %d cells: room for the six faces of %d point lights at 512, %d at 256, %d at 128\n",
							atlasSide * SHADOWCACHE_CELL, atlasSide * SHADOWCACHE_CELL, atlasCells, atlasCells / 96, atlasCells / 24, atlasCells / 6 );
		}
	}

	const unsigned int settings = R_ShadowCacheSettings();
	if( settings != s.settings )
	{
		s.settings = settings;
		s.epoch++;
	}
	if( c_materialFrees != s.materialFrees )
	{
		// a material was parsed again ( reloadDecls ) or purged: what it casts may have changed
		s.materialFrees = c_materialFrees;
		s.epoch++;
	}
	if( r_shadowCacheFlush.GetBool() )
	{
		// every place is given up as well, so that the next frames place the maps as a first run does
		r_shadowCacheFlush.SetBool( false );
		for( int i = 0; i < s.recsHigh; i++ )
		{
			if( s.recs[i].uniqueId != 0 )
			{
				R_ShadowCacheFreePlace( s.recs[i] );
			}
		}
		s.epoch++;
	}
	if( s.missesPublished != s.missesSeen )
	{
		if( s.missesPublished - s.missesSeen > SHADOWCACHE_MISS_RING )
		{
			s.epoch++;		// more misses than the ring holds ( the cache images were lost ): every map again
		}
		else
		{
			for( unsigned int m = s.missesSeen; m != s.missesPublished; m++ )
			{
				R_ShadowCacheMissed( s.missedPublished[m & ( SHADOWCACHE_MISS_RING - 1 )] );
			}
		}
		s.missesSeen = s.missesPublished;
	}

	s.reused = 0;
	s.rebuilt = 0;
	s.repaired = 0;
	s.waiting = 0;
	s.moving = 0;
	s.sunReused = 0;
	s.sunRebuilt = 0;
	s.evicted = 0;
	s.turns = 0;
	s.costBuilt = 0;
}

/*
==========================================================================================

S2a: the snapshot, the atlas plan and the caster bits

==========================================================================================
*/

// What the plan and the caster bits did, for r_shadowCacheLog. Frontend-owned ( D36 ).
struct shadowCachePlanStats_t
{
	int		demoted;			// lights the plan put one size down
	int		dropped;			// lights the plan left without a shadow
	int		casters;			// surfaces that got a byte
	int		casterSides;		// ( surface, side ) pairs the backend will draw; not the static surfaces of a
								// light that draws no static map this frame, which get no box work ( R_ShadowCacheCasterBits )
	int		casterSidesAll;		// the same without the cull
	int		ops;				// cached units of the view
	int		uncachedLights;		// lights the cache atlas had no room for
	int		staticSides;		// ( surface, side ) pairs drawn into static maps; the skipped surfaces above are not counted
	int		activeSides;		// ( surface, side ) pairs drawn on top, into the atlas
};

static shadowCachePlanStats_t shadowCachePlan;

// The lights ShadowAtlasPass gives tiles to, by the same four tests in the same order;
// they must stay in step ( the three loops of idRenderBackend::ShadowAtlasPass ). The plan and the op
// emission call this while every light's shadowOp is still 0, so "has a shadow map" means "has a caster in
// this view's chain". A cached light's chain keeps its static surfaces ( a missed copy is then drawn as the
// engine draws the chain without the cache ), so the test stays what it is.
static bool R_ShadowCacheLightHasUnits( const viewLight_t* vLight )
{
	if( vLight->lightShader->IsFogLight() || vLight->lightShader->IsBlendLight() )
	{
		return false;
	}
	return R_ViewLightIsLit( vLight ) && R_ViewLightHasShadowMap( vLight );
}

// How many maps the atlas pass draws for the light; the same sides as its tile
// collection, and the two must stay in step.
static int R_ShadowCacheNumSides( const viewLight_t* vLight, const shadowSkipSet_t& skipSet )
{
	if( vLight->parallel )
	{
		return skipSet.shadowMapSplits + 1;
	}
	if( vLight->pointLight )
	{
		return ( skipSet.shadowMapSingleSide != -1 ) ? 1 : 6;
	}
	return 1;
}

/*
=====================
R_ShadowCachePlanAtlas

Fits the view's shadow maps into the per-frame atlas before anything is drawn ( S86, D77 ).
While the demand exceeds the atlas, the least important light that is still above the
smallest size goes down one size; only when every light is at the smallest size is the
least important one dropped. Importance is the light's scissor area, ties go against the
larger map and then against the light later in the view's list. Every size is a power of
two, so a demand at or below the atlas's area always packs largest first ( with the
engine's atlas of 8192 and tiles of 128 to 1024 ). It writes
viewLight_t::shadowLOD ( D43 ).
=====================
*/
static void R_ShadowCachePlanAtlas( viewDef_t* viewDef )
{
	const shadowSkipSet_t& skipSet = viewDef->shadowSkipSet;
	const int64 atlasSize = r_shadowMapAtlasSize.GetInteger();
	const int64 capacity = atlasSize * atlasSize;
	const int smallest = shadowMapResolutions[MAX_SHADOWMAP_RESOLUTIONS - 1];

	int64 demand = 0;
	for( const viewLight_t* vLight = viewDef->viewLights; vLight != NULL; vLight = vLight->next )
	{
		if( R_ShadowCacheLightHasUnits( vLight ) )
		{
			const int64 res = shadowMapResolutions[vLight->shadowLOD];
			demand += R_ShadowCacheNumSides( vLight, skipSet ) * res * res;
		}
	}

	while( demand > capacity )
	{
		// the victim: among the lights above the smallest size, the smallest scissor area; a tie goes to the
		// larger map, then to the later light. When none is above the smallest size, the same choice among all.
		viewLight_t* victim = NULL;
		bool victimShrinks = false;
		int victimArea = 0;
		int victimRes = 0;
		for( viewLight_t* vLight = viewDef->viewLights; vLight != NULL; vLight = vLight->next )
		{
			if( !R_ShadowCacheLightHasUnits( vLight ) )
			{
				continue;
			}
			const int res = shadowMapResolutions[vLight->shadowLOD];
			const bool shrinks = res > smallest;
			const int area = Max( 0, vLight->scissorRect.x2 - vLight->scissorRect.x1 + 1 ) * Max( 0, vLight->scissorRect.y2 - vLight->scissorRect.y1 + 1 );
			bool better;
			if( victim == NULL || shrinks != victimShrinks )
			{
				better = ( victim == NULL ) || shrinks;
			}
			else if( area != victimArea )
			{
				better = area < victimArea;
			}
			else
			{
				better = res >= victimRes;
			}
			if( better )
			{
				victim = vLight;
				victimShrinks = shrinks;
				victimArea = area;
				victimRes = res;
			}
		}
		if( victim == NULL )
		{
			break;
		}

		const int64 sides = R_ShadowCacheNumSides( victim, skipSet );
		if( victimShrinks )
		{
			// one size down: the next level with a smaller map ( two levels share one size )
			int level = victim->shadowLOD;
			while( shadowMapResolutions[level] == victimRes )
			{
				level++;
			}
			const int64 res = shadowMapResolutions[level];
			demand -= sides * ( ( int64 )victimRes * victimRes - res * res );
			victim->shadowLOD = level;
			shadowCachePlan.demoted++;
		}
		else
		{
			demand -= sides * ( int64 )victimRes * victimRes;
			victim->shadowLOD = -1;
			shadowCachePlan.dropped++;
		}
	}
}

/*
=====================
R_ShadowCacheFullGather

Does this light draw its static shadow maps this frame, so that its chain must hold every
cacheable caster of its volume? S3a: without reuse, every shadow-casting light of the main view
that could be cached; with reuse, a light only while its static maps may be built this frame
( R_ShadowCacheWantsGather, and for the sun R_ShadowCacheSunWantsGather ). Called from the light
job ( R_AddSingleLight ), which may run in parallel: it only reads; the model job reads the flag it sets.
=====================
*/
bool R_ShadowCacheFullGather( const viewDef_t* viewDef, const idRenderLightLocal* light )
{
	if( r_shadowCache.GetInteger() < 2 || !viewDef->isMainView || ( viewDef->renderView.rdflags & RDF_NOSHADOWS ) != 0 )
	{
		return false;
	}
	if( r_skipShadows.GetBool() || !r_useShadowAtlas.GetBool() )
	{
		return false;
	}
	// a point light restricted to one face is drawn uncached ( the same test is in pass 1 of R_ShadowCacheEmitOps )
	if( !light->parms.parallel && light->parms.pointLight && r_shadowMapSingleSide.GetInteger() != -1 )
	{
		return false;
	}
	// of the parallel lights only the primary sun is cached ( D42 )
	if( !light->LightCastsShadows() || ( light->parms.parallel && !light->isPrimarySun ) )
	{
		return false;
	}
	// S3a: without reuse every static map is drawn every frame; with it a light gathers in full only while
	// its static maps may be built this frame ( the sun: its windows may have moved with the view )
	if( !r_shadowCacheReuse.GetBool() )
	{
		return true;
	}
	if( light->parms.parallel )
	{
		return R_ShadowCacheSunWantsGather( viewDef, light );
	}
	return R_ShadowCacheWantsGather( light );
}

/*
=====================
R_ShadowCacheEntityCacheable

Can this entity's shadow go into a static map at all ( D44 as amended )? Its
geometry must stay the same until the game updates the entity: a static model, no callback,
no joints. A model with LOD siblings draws another model per view, and an entity with a
camera-dependent shadow cull comes and goes with the camera: both stay active.
An entity whose shadow is left out of one view or one light stays active too. Whether it
has held still long enough is S3a's test.
=====================
*/
bool R_ShadowCacheEntityCacheable( const idRenderEntityLocal* def )
{
	const renderEntity_t& parms = def->parms;
	const idRenderModel* model = parms.hModel;
	if( model == NULL || model->IsDynamicModel() != DM_STATIC )
	{
		return false;
	}
	if( parms.callback != NULL || parms.joints != NULL || parms.noShadow )
	{
		return false;
	}
	if( model->NumLods() > 0 )
	{
		return false;
	}
	// the cullShadow distance cull ( R_CullEntityDistance ) depends on the camera: an entity it can
	// remove is not cacheable, so it stays an active caster
	if( parms.cullShadow && r_distanceCull.GetBool() && !model->IsStaticWorldModel() && !parms.weaponDepthHack && parms.modelDepthHack == 0.0f )
	{
		return false;
	}
	if( parms.suppressShadowInViewID != 0 || parms.suppressShadowInLightID != 0 )
	{
		return false;
	}
	return true;
}

// a material that moves its own geometry ( a deform ) or whose alpha test can change by itself
// ( perforated with registers that are not constant, or perforated with a cinematic stage ) is
// drawn with the active casters even on a static entity
static bool R_ShadowCacheSurfaceCacheable( const idMaterial* shader )
{
	// a material with LOD keywords is skipped by its distance from the camera ( R_AddSingleModel ): it comes
	// and goes with the view, like a model with LOD siblings ( D44 )
	if( shader->IsLOD() )
	{
		return false;
	}
	if( shader->Deform() != DFRM_NONE )
	{
		return false;
	}
	if( shader->Coverage() == MC_PERFORATED )
	{
		if( shader->ConstantRegisters() == NULL )
		{
			return false;
		}
		// a stage whose texture is a cinematic ( a videoMap ) changes the alpha test every frame without any
		// register: the static map would be stale the frame after it was built
		for( int i = 0; i < shader->GetNumStages(); i++ )
		{
			if( shader->GetStage( i )->texture.cinematic != NULL )
			{
				return false;
			}
		}
	}
	return true;
}

/*
=====================
R_ShadowCacheLightCacheable

May this light's units be cached in this view? S3a: local lights are decided by their
records ( R_ShadowCacheEmitOps ), so this is reached for the parallel lights only: the
primary sun, and only with its windows placed. Whether its chain holds every static caster
is the emitter's question: it draws the sun without the cache when a cascade must be drawn
again and the chain was not gathered in full.
=====================
*/
static bool R_ShadowCacheLightCacheable( const viewDef_t* viewDef, const viewLight_t* vLight )
{
	return vLight->isPrimarySun && viewDef->shadowCacheView != NULL;
}

// cell ( x, y ) of a Z-order ( Morton ) index: x from the even bits, y from the odd ones
static void R_ShadowCacheMortonDecode( int index, int& x, int& y )
{
	x = 0;
	y = 0;
	for( int bit = 0; bit < 15; bit++ )
	{
		x |= ( ( index >> ( 2 * bit ) ) & 1 ) << bit;
		y |= ( ( index >> ( 2 * bit + 1 ) ) & 1 ) << bit;
	}
}

enum shadowCacheDecision_t
{
	SCD_UNCACHED,		// drawn without the cache this frame
	SCD_REUSE,			// its maps are copied
	SCD_REBUILD,		// its maps are drawn, then copied
	SCD_WANTS			// a build if the budget reaches it, else the pick's fallback
};

// what the decide pass settled for one local light of the view ( frame memory )
struct shadowCachePick_t
{
	viewLight_t*			vLight;
	shadowCacheLightRec_t*	rec;
	unsigned int			doorSig;
	int						casters;		// S3b: static surfaces of the chain: counted this frame where a build is possible, else the record's last count
	int						since;			// S3b: the frame its maps began to wait ( the record's wantSince ), or this frame: the order of the builds and of the turns
	short					size;			// the engine's pick ( after the atlas plan )
	byte					sides;
	byte					decision;		// shadowCacheDecision_t
	byte					fallback;		// decision when the budget does not reach it: SCD_REUSE or SCD_UNCACHED
	bool					waits;			// S3b: pass 4: it waits for a turn
};

// S3b: how many surfaces of the light's chain would be in static maps built this frame: the casters a cached
// copy saves from being drawn every frame. The measure of what the maps are worth ( R_ShadowCacheValue ) and
// of what a build costs ( R_ShadowCacheCost ), D77. The same tests, in the same order, as the static bit of
// R_ShadowCacheCasterBits for a map built this frame. Only asked of a chain gathered in full.
static int R_ShadowCacheCountStatic( const viewLight_t* vLight, int staticFrames, bool reuse )
{
	int count = 0;
	const viewEntity_t* lastSpace = NULL;		// the surfaces of one entity follow one another in the chain
	bool lastSpaceStatic = false;
	for( const drawSurf_t* surf = vLight->globalShadows; surf != NULL; surf = surf->nextOnLight )
	{
		if( surf->frontEndGeo == NULL || surf->space == NULL )
		{
			continue;
		}
		if( surf->space != lastSpace )
		{
			lastSpace = surf->space;
			const idRenderEntityLocal* def = surf->space->entityDef;
			lastSpaceStatic = def != NULL && R_ShadowCacheEntityCacheable( def )
							  && ( !reuse || R_ShadowCacheIsStatic( tr.frameCount, R_ShadowCacheLastChanged( def ), staticFrames ) );
		}
		if( lastSpaceStatic && surf->material != NULL && R_ShadowCacheSurfaceCacheable( surf->material ) )
		{
			count++;
		}
	}
	return count;
}

/*
=====================
R_ShadowCacheBuild

S3b: orders the static maps of a pick's light built this frame, at the engine's pick: a cached
map is shown only at the size the engine picks ( a smaller one would lower the shadow's
resolution; a larger one copied reduced keeps the polygon offset of its own texels and shows
acne on surfaces the light grazes ). Where the record's maps have another size, or it has no
place, a place is searched ( R_ShadowCachePlace ). False = none was found and nothing is built:
the light is drawn without the cache, keeps the maps it has when they are for as many sides
( the pick may come back to them ), and asks again in a while.
=====================
*/
static bool R_ShadowCacheBuild( shadowCachePick_t& pick )
{
	shadowCacheState_t& s = shadowCacheState;
	shadowCacheLightRec_t& rec = *pick.rec;
	if( rec.size != 0 && rec.numSides != pick.sides )
	{
		R_ShadowCacheFreePlace( rec );		// maps for another number of sides are of no use
	}
	if( rec.size != pick.size
			&& !R_ShadowCachePlace( rec, pick.size, pick.sides, R_ShadowCacheValue( pick.casters, pick.size, pick.sides ), false, true ) )
	{
		rec.retryFrame = tr.frameCount + SHADOWCACHE_RETRY_FRAMES;		// no place for maps of the pick's size: ask again in a while
		shadowCachePlan.uncachedLights++;
		return false;
	}
	if( rec.repair )
	{
		s.repaired++;
		s.totalRepaired++;
	}
	else
	{
		s.rebuilt++;
		s.totalRebuilt++;
	}
	rec.casters = pick.casters;
	rec.serial = s.nextSerial + 1;
	s.nextSerial += rec.numSides;
	rec.heldThrough = tr.frameCount;
	rec.epoch = s.epoch;
	rec.doorSig = pick.doorSig;
	rec.wantSince = 0;
	rec.repair = false;
	rec.valid = true;
	return true;
}

/*
=====================
R_ShadowCacheEmitOps

Gives every cached unit of this view its place in the cache and emits its op ( D74 ). The sun
keeps one map per cascade, drawn again when its window moved or its membership must be brought
up to date. A local light is drawn through the cache when its record's maps are usable or may be
built this frame ( S3a ): the place stays with the record; a light whose maps cannot be built is
drawn uncached. S3b: a place is blocks of the cell map, one per side ( R_ShadowCachePlace ),
and when the atlas is full the records worth least give theirs up ( D77 ); the budget rations
the builds of lights that wait, in a-priori units, the longest wait first ( repairs and the sun
do not count against it ); a light that waits gathers in full only in the turn pass 4 gives
it; maps are shown only at the size the engine picks, and are built again at another size
only when the pick has held ( D43 ).
=====================
*/
static void R_ShadowCacheEmitOps( viewDef_t* viewDef )
{
	shadowCacheState_t& s = shadowCacheState;
	const shadowSkipSet_t& skipSet = viewDef->shadowSkipSet;
	const bool reuse = r_shadowCacheReuse.GetBool();
	const int staticFrames = r_shadowCacheStaticFrames.GetInteger();

	// An entity that touches more than MAX_CONTACTED_LIGHTS lights of the view is left out of the chains of
	// the lights past the cap ( R_AddSingleModel ), by the order of the view's light list, which depends on
	// the camera. A map built from such a chain would lack a caster the membership rule later takes to be
	// in it. So with reuse nothing is built in a frame in which the cap was hit: a light that needs a build
	// keeps its usable maps or is drawn without the cache ( D78: track F removes the cap ). S3b: only an
	// entity of THIS view that can be in a static map counts. One that never can ( the game's smoke particles
	// are one model with bounds as large as the world, which touches every light of the view ) is an active
	// caster in every frame, so its place past the cap takes nothing out of a static map. The frame's
	// counter says whether any entity of any view hit the cap; the walk over this view's entities runs only then
	bool capHit = false;
	if( reuse && tr.pc.c_contactedCapHits > 0 )
	{
		for( const viewEntity_t* vEntity = viewDef->viewEntitys; vEntity != NULL && !capHit; vEntity = vEntity->next )
		{
			capHit = vEntity->contactedCapHits != 0 && vEntity->entityDef != NULL && R_ShadowCacheEntityCacheable( vEntity->entityDef );
		}
	}
	s.capHit = capHit;

	if( shadowMapResolutions[MAX_SHADOWMAP_RESOLUTIONS - 1] != SHADOWCACHE_CELL )
	{
		return;		// the cells and the backend's stamps are laid out for that size: no light is cached
	}

	int maxOps = 0;
	int numLights = 0;
	for( const viewLight_t* vLight = viewDef->viewLights; vLight != NULL; vLight = vLight->next )
	{
		if( R_ShadowCacheLightHasUnits( vLight ) )
		{
			maxOps += R_ShadowCacheNumSides( vLight, skipSet );
			numLights++;
		}
	}
	if( maxOps == 0 )
	{
		return;
	}
	shadowCacheOp_t* ops = ( shadowCacheOp_t* )R_FrameAlloc( maxOps * sizeof( shadowCacheOp_t ) );
	shadowCachePick_t* picks = ( shadowCachePick_t* )R_FrameAlloc( numLights * sizeof( shadowCachePick_t ) );
	int numEmitted = 0;
	int numPicks = 0;

	// 1. every local light that could be cached: what its record says
	for( viewLight_t* vLight = viewDef->viewLights; vLight != NULL; vLight = vLight->next )
	{
		if( !R_ShadowCacheLightHasUnits( vLight ) || vLight->parallel )
		{
			continue;
		}
		if( vLight->pointLight && skipSet.shadowMapSingleSide != -1 )
		{
			continue;		// a point light restricted to one face is drawn uncached
		}
		const idRenderLightLocal* light = vLight->lightDef;
		shadowCacheLightRec_t* rec = R_ShadowCacheClaimRec( light );
		if( rec == NULL )
		{
			continue;
		}
		if( reuse && light->transitionInteractionPending )
		{
			continue;		// its interactions are still being made, past every hook: drawn uncached until they are
		}
		const bool wasRecent = rec->lastUsedFrame > tr.frameCount - SHADOWCACHE_PICK_HOLD_FRAMES;		// S3b: in the main view within the hold
		rec->lastUsedFrame = tr.frameCount;
		rec->granted = false;		// S3b: a turn lasts one frame; pass 4 gives the next ones

		shadowCachePick_t& pick = picks[numPicks++];
		pick.vLight = vLight;
		pick.rec = rec;
		pick.doorSig = reuse ? R_ShadowCacheDoorSignature( light ) : 0;
		pick.casters = rec->casters;
		pick.since = tr.frameCount;
		pick.size = ( short )shadowMapResolutions[vLight->shadowLOD];
		if( rec->pick != pick.size )
		{
			rec->pick = pick.size;		// what its place must hold to be of use ( R_ShadowCacheVictimKey )
			// the hold is for a pick that hovers while the light is in view, or goes in and out of it: only
			// a light that comes back at another size after an absence as long as the hold has served it
			rec->pickFrame = wasRecent ? tr.frameCount : tr.frameCount - SHADOWCACHE_PICK_HOLD_FRAMES;
		}
		pick.sides = ( byte )R_ShadowCacheNumSides( vLight, skipSet );
		pick.decision = SCD_UNCACHED;
		pick.fallback = SCD_UNCACHED;
		pick.waits = false;

		if( !reuse )
		{
			// S2's behaviour: every map again, every frame. S3b: a light the atlas had no room for asks
			// again in a while, and a light that needs a place is counted: what its maps would be worth
			if( vLight->shadowFullGather && tr.frameCount >= rec->retryFrame )
			{
				pick.decision = SCD_REBUILD;
				if( rec->size != pick.size || rec->numSides != pick.sides )
				{
					pick.casters = R_ShadowCacheCountStatic( vLight, staticFrames, false );
				}
			}
			continue;
		}
		if( tr.frameCount - rec->shapeFrame < staticFrames )
		{
			// its maps are stale and it is drawn uncached while it keeps changing: the place is free for
			// another light ( frames in flight copy from it before any later frame draws into it )
			R_ShadowCacheFreePlace( *rec );
			s.moving++;
			continue;		// changing shape: uncached
		}

		// the changes this record has not seen: one whose old bounds touch the light makes its maps stale
		int begin, end;
		bool touched = !R_ShadowCacheChangesSince( s.changes, rec->changesSeen, begin, end );
		for( int n = begin; n < end && !touched; n++ )
		{
			touched = light->globalLightBounds.IntersectsBounds( s.changes.oldBounds[n & ( SHADOWCACHE_MAX_CHANGES - 1 )] );
		}
		rec->changesSeen = end;

		// do the record's maps hold what it says? ( S3b: at whatever size they were built )
		const bool built = rec->valid && rec->epoch == s.epoch && rec->size != 0 && rec->numSides == pick.sides;
		if( built && ( touched || rec->doorSig != pick.doorSig ) )
		{
			rec->valid = false;
			rec->repair = true;
		}
		const bool good = built && rec->valid;
		if( !good && !rec->repair )
		{
			rec->valid = false;		// latch it: the light jobs read this flag
		}
		// S3b ( D43 ): maps are shown only at the size the engine picks. A smaller map would lower the
		// shadow's resolution; a larger one copied reduced keeps the polygon offset of its own texels and
		// shows acne on surfaces the light grazes. With maps of another size the light is drawn without
		// the cache. The maps keep their place meanwhile ( the pick may come back to them ), and maps of the
		// new size are built only when the pick has held for a while, so a pick that hovers between two
		// sizes costs no build ( a light that was out of the main view for as long has served the hold )
		const bool usable = good && rec->size == pick.size;
		const bool held = good && rec->size != pick.size && tr.frameCount - rec->pickFrame < SHADOWCACHE_PICK_HOLD_FRAMES;

		// can a map of this light be drawn this frame? Only from a chain gathered in full, not in a frame
		// in which the contacted-light cap was hit, not while the light waits for a place, and not while
		// its pick has yet to hold
		const bool mayBuild = vLight->shadowFullGather && !capHit && tr.frameCount >= rec->retryFrame && !held;
		if( usable && rec->wantSince == 0 )
		{
			pick.decision = SCD_REUSE;
		}
		else if( !mayBuild )
		{
			pick.decision = usable ? SCD_REUSE : SCD_UNCACHED;
			if( capHit && vLight->shadowFullGather )
			{
				rec->retryFrame = tr.frameCount + SHADOWCACHE_CAP_RETRY_FRAMES;		// the light job stops gathering in full for a while
			}
		}
		else if( !good && rec->repair )
		{
			pick.decision = SCD_REBUILD;		// a repair does not wait for the budget
		}
		else
		{
			pick.decision = SCD_WANTS;
			pick.fallback = usable ? SCD_REUSE : SCD_UNCACHED;
		}
		// good maps of another size wait without a stamp: they queue behind every light that has none
		if( !good && rec->wantSince == 0 )
		{
			rec->wantSince = tr.frameCount;
		}
		if( rec->wantSince != 0 )
		{
			pick.since = rec->wantSince;
		}
		// S3b: a build is priced, and its maps are valued, by the static casters of the chain gathered in full
		if( pick.decision == SCD_REBUILD || pick.decision == SCD_WANTS )
		{
			pick.casters = R_ShadowCacheCountStatic( vLight, staticFrames, true );
			rec->casters = pick.casters;
			rec->countFrame = tr.frameCount;
		}
	}

	// 2. the builds. First the maps that do not wait for the budget ( a repair; without reuse, every map ),
	// the largest first and then in the view's order ( on an empty cache atlas this places the maps exactly
	// as S2's per-frame assignment did ). Then the lights that wait, the longest wait first,
	// then the view's order, while the frame's budget lasts ( S3b: in a-priori units, R_ShadowCacheCost ). A
	// build that finds no place costs nothing. Every place is settled here, before any op is emitted: a
	// record of this view that gives its place up in this pass gets no op in pass 3, so no copy of this
	// frame reads a place another light's map is drawn into ( S84 )
	for( int size = shadowMapResolutions[0]; size >= SHADOWCACHE_CELL; size >>= 1 )
	{
		for( int p = 0; p < numPicks; p++ )
		{
			if( picks[p].size == size && picks[p].decision == SCD_REBUILD && !R_ShadowCacheBuild( picks[p] ) )
			{
				picks[p].decision = SCD_UNCACHED;
			}
		}
	}
	int budget = r_shadowCacheBudget.GetInteger();
	bool spent = false;
	for( ;; )
	{
		shadowCachePick_t* oldest = NULL;
		for( int i = 0; i < numPicks; i++ )
		{
			if( picks[i].decision == SCD_WANTS && ( oldest == NULL || picks[i].since < oldest->since ) )
			{
				oldest = &picks[i];
			}
		}
		if( oldest == NULL )
		{
			break;
		}
		// the first build of a frame is never refused, so a light dearer than the whole budget cannot starve
		if( spent && R_ShadowCacheCost( oldest->casters, oldest->size, oldest->sides ) > budget )
		{
			break;
		}
		if( R_ShadowCacheBuild( *oldest ) )
		{
			const int cost = R_ShadowCacheCost( oldest->casters, oldest->rec->size, oldest->rec->numSides );
			budget -= cost;
			s.costBuilt += cost;
			spent = true;
			oldest->decision = SCD_REBUILD;
		}
		else
		{
			oldest->decision = oldest->fallback;
		}
	}
	for( int i = 0; i < numPicks; i++ )
	{
		if( picks[i].decision == SCD_WANTS )
		{
			picks[i].decision = picks[i].fallback;
		}
	}

	// 3. the ops: the primary sun's, then the local lights' in the view's order

	// the primary sun: cascade i lives in cascade map i, kept while its window is the one its map was drawn with
	for( viewLight_t* vLight = viewDef->viewLights; vLight != NULL; vLight = vLight->next )
	{
		if( !vLight->parallel || !R_ShadowCacheLightHasUnits( vLight ) || !R_ShadowCacheLightCacheable( viewDef, vLight )
				|| ( reuse && vLight->lightDef->transitionInteractionPending ) )		// its interactions are still being made, past every hook: drawn uncached until they are
		{
			continue;
		}
		const int size = shadowMapResolutions[vLight->shadowLOD];
		const int sides = R_ShadowCacheNumSides( vLight, skipSet );
		const idRenderLightLocal* light = vLight->lightDef;
		const shadowCacheView_t* scv = viewDef->shadowCacheView;
		shadowCacheSunRec_t& sun = s.sun;
		if( sun.uniqueId != light->uniqueId )
		{
			memset( &sun, 0, sizeof( sun ) );
			sun.uniqueId = light->uniqueId;
		}

		// the membership: must every cascade be drawn again?
		int begin, end;
		bool touched = !R_ShadowCacheChangesSince( s.changes, sun.changesSeen, begin, end );
		for( int n = begin; n < end && !touched; n++ )
		{
			touched = light->globalLightBounds.IntersectsBounds( s.changes.oldBounds[n & ( SHADOWCACHE_MAX_CHANGES - 1 )] );
		}
		sun.changesSeen = end;
		if( touched )
		{
			sun.valid = false;		// latched: the light job reads it
		}
		const bool all = !reuse || !sun.valid || sun.epoch != s.epoch || sun.wantSince != 0 || sun.numCascades != sides;

		// the cascades whose window is not the one their map was drawn with
		bool moved[SHADOWCACHE_MAX_CASCADES];
		bool anyRebuild = false;
		for( int i = 0; i < sides; i++ )
		{
			const shadowCacheCascade_t& c = scv->cascades[i];
			const shadowCacheSunCascadeRec_t& crec = sun.cascades[i];
			moved[i] = all || !crec.valid || crec.res != c.res || crec.texelWorld != c.texelWorld
					   || crec.clampedX != c.clampedX || crec.clampedY != c.clampedY
					   || ( !c.clampedX && crec.originX != c.originX ) || ( !c.clampedY && crec.originY != c.originY );
			anyRebuild = anyRebuild || moved[i];
		}
		if( anyRebuild && ( !vLight->shadowFullGather || capHit ) )
		{
			// the chain does not hold every static caster ( or the cap of 128 makes that unknowable ), so no
			// map can be drawn this frame: the sun is drawn without the cache ( S1's path ). The stored view is
			// left alone, so the next frame's light job sees another view and gathers in full
			if( capHit && vLight->shadowFullGather )
			{
				sun.retryFrame = tr.frameCount + SHADOWCACHE_CAP_RETRY_FRAMES;		// the light job stops gathering in full for a while
			}
			sun.valid = false;
			continue;
		}
		if( all )
		{
			sun.heldThrough = tr.frameCount;
			sun.epoch = s.epoch;
			sun.wantSince = 0;
			sun.numCascades = sides;
		}
		sun.valid = true;
		sun.viewOrigin = viewDef->renderView.vieworg;
		sun.viewAxis = viewDef->renderView.viewaxis;
		sun.fovX = viewDef->renderView.fov_x;
		sun.fovY = viewDef->renderView.fov_y;

		vLight->shadowOp = numEmitted + 1;
		for( int i = 0; i < sides; i++ )
		{
			shadowCacheSunCascadeRec_t& crec = sun.cascades[i];
			if( moved[i] )
			{
				const shadowCacheCascade_t& c = scv->cascades[i];
				crec.originX = c.originX;
				crec.originY = c.originY;
				crec.texelWorld = c.texelWorld;
				crec.res = c.res;
				crec.clampedX = c.clampedX;
				crec.clampedY = c.clampedY;
				crec.serial = ++s.nextSerial;
				crec.valid = true;
				s.sunRebuilt++;
			}
			else
			{
				s.sunReused++;
			}
			shadowCacheOp_t& op = ops[numEmitted++];
			op.serial = crec.serial;
			op.target = ( short )i;
			op.size = ( short )size;
			op.x = 0;
			op.y = 0;
			op.flags = ( short )( moved[i] ? SCOP_REBUILD : 0 );
			op.pad = 0;
		}
	}

	for( int p = 0; p < numPicks; p++ )
	{
		shadowCachePick_t& pick = picks[p];
		shadowCacheLightRec_t& rec = *pick.rec;
		if( ( pick.decision != SCD_REBUILD && pick.decision != SCD_REUSE ) || rec.size == 0 )
		{
			// drawn without the cache ( also a light of this view that gave its place up in pass 2 ). A light
			// that is changing shape was counted as moving in pass 1: do not count a light twice
			if( !reuse || tr.frameCount - rec.shapeFrame >= staticFrames )
			{
				s.waiting++;
			}
			continue;
		}
		if( pick.decision == SCD_REUSE )
		{
			s.reused++;
		}
		assert( rec.size == pick.size );		// the backend's copy loop refuses a map of any other size

		// S3b: the maps are the size the engine picked, which is the tile's ( shadowLOD )
		pick.vLight->shadowOp = numEmitted + 1;
		for( int i = 0; i < rec.numSides; i++ )
		{
			int cellX, cellY;
			R_ShadowCacheMortonDecode( rec.cell[i], cellX, cellY );

			shadowCacheOp_t& op = ops[numEmitted++];
			op.serial = rec.serial + i;
			op.target = -1;
			op.size = rec.size;
			op.x = ( short )( cellX * SHADOWCACHE_CELL );
			op.y = ( short )( cellY * SHADOWCACHE_CELL );
			op.flags = ( short )( ( pick.decision == SCD_REBUILD ) ? SCOP_REBUILD : 0 );
			op.pad = 0;
		}
	}

	// 4. S3b: the turns of the next frame. A light whose maps wait to be built gathers in full only in its
	// turn ( R_ShadowCacheWantsGather ), so the lights that wait do not all pay for a full gather every
	// frame. The longest wait first, as many as the budget would pay for, each priced by what its last full
	// gather counted ( a light never counted is priced with a default, and its first turn counts it ). A
	// light that needs a place and by its last count could find none gets no turn and asks again in a
	// while. An old count is not trusted with that: entities that have come to rest since it was taken
	// are static now, and only a full gather counts them, so the light gets a turn like one never counted
	if( reuse )
	{
		for( int p = 0; p < numPicks; p++ )
		{
			shadowCachePick_t& pick = picks[p];
			const shadowCacheLightRec_t& rec = *pick.rec;
			if( tr.frameCount - rec.shapeFrame < staticFrames || pick.decision == SCD_REBUILD || tr.frameCount + 1 < rec.retryFrame )
			{
				pick.waits = false;		// changing shape, built this frame, or waiting out a retry
			}
			else if( rec.size == 0 || !rec.valid )
			{
				pick.waits = !rec.repair;		// no good maps; a repair needs no turn: its light job gathers in full by itself
			}
			else if( rec.size == pick.size )
			{
				pick.waits = rec.wantSince != 0;		// an addition waits
			}
			else
			{
				pick.waits = tr.frameCount + 1 - rec.pickFrame >= SHADOWCACHE_PICK_HOLD_FRAMES;		// maps of another size: once the pick has held
			}
		}
		int budgetAhead = r_shadowCacheBudget.GetInteger();
		bool any = false;
		for( ;; )
		{
			shadowCachePick_t* oldest = NULL;
			for( int i = 0; i < numPicks; i++ )
			{
				if( picks[i].waits && ( oldest == NULL || picks[i].since < oldest->since ) )
				{
					oldest = &picks[i];
				}
			}
			if( oldest == NULL )
			{
				break;
			}
			oldest->waits = false;
			shadowCacheLightRec_t& rec = *oldest->rec;
			const bool counted = rec.casters >= 0 && tr.frameCount - rec.countFrame < Max( staticFrames, SHADOWCACHE_RECOUNT_FRAMES );
			if( rec.size != oldest->size && counted
					&& !R_ShadowCachePlace( rec, oldest->size, oldest->sides, R_ShadowCacheValue( rec.casters, oldest->size, oldest->sides ), true, false ) )
			{
				// by its last count its maps would find no place. The placement itself is asked, about the
				// next frame, and changes nothing: a count of cells would let through a light whose blocks
				// do not exist, which then gathers in full and fails to place every time it asks. The lights
				// refused in one frame ask again spread over sixteen
				rec.retryFrame = tr.frameCount + SHADOWCACHE_RETRY_FRAMES + ( int )( ( &rec - s.recs ) & 15 );
				continue;
			}
			const int cost = R_ShadowCacheCost( ( rec.casters >= 0 ) ? rec.casters : SHADOWCACHE_DEFAULT_CASTERS, oldest->size, oldest->sides );
			if( any && cost > budgetAhead )
			{
				break;
			}
			rec.granted = true;
			budgetAhead -= cost;
			any = true;
			s.turns++;
		}
	}

	if( r_shadowCacheLog.GetInteger() >= 2 && ( tr.frameCount & 63 ) == 0 )
	{
		for( int p = 0; p < numPicks; p++ )
		{
			const shadowCachePick_t& pick = picks[p];
			const shadowCacheLightRec_t& rec = *pick.rec;
			common->Printf( "shadowCache: frame %d light %d pick %d cached %d sides %d casters %d value %d decision %d turn %d\n",
							tr.frameCount, pick.vLight->lightDef->index, ( int )pick.size, ( int )rec.size, ( int )pick.sides, rec.casters,
							R_ShadowCacheValue( rec.casters, ( rec.size != 0 ) ? rec.size : pick.size, pick.sides ), ( int )pick.decision, ( int )rec.granted );
		}
	}

	viewDef->shadowOps = ops;
	viewDef->numShadowOps = numEmitted;
	shadowCachePlan.ops = numEmitted;
}

/*
=====================
R_ShadowCacheCasterBits

One byte per surface of the light's globalShadows chain, in chain order: the sides the
surface's bounds can reach ( the per-face caster cull of D77 ). Conservative: a surface is
left out of a side only when its box cannot touch that side's frustum. A point light's
faces look down the world axes from the light's origin with r_shadowMapFrustumFOV ( the
point-light branch of SetupShadowMapMatrices: side 0 is +x, 1 is -x, 2 is +y, 3 is -y,
4 is +z, 5 is -z ); a cascade of the primary sun is its window in the sun's light view
( shadowCacheCascade_t ).
S2c: a cached light's chain is walked too, and a surface that may be cached gains the
static bit: it is drawn into the unit's static map instead of into the atlas. The sides an
ACTIVE caster of a cached light reaches are also ORed into vLight->shadowActiveSides, so
the backend draws no active pass into a side none of them reaches.
S3a: with reuse, a local light's entity is static only when it is in the record's maps:
unchanged for the static interval when the maps were last built ( R_ShadowCacheInUnit against
the heldThrough stamp: the light's own record, or for the primary sun its one stamp, the same
for every cascade ). An entity reaches the chain only through the light's own area walk, so no
separate area test is needed. One that turned static since is an active caster this frame and
marks the record as wanting an update.
=====================
*/
static void R_ShadowCacheCasterBits( viewDef_t* viewDef, viewLight_t* vLight )
{
	const shadowCacheView_t* scv = viewDef->shadowCacheView;
	const bool sun = vLight->parallel && vLight->isPrimarySun && scv != NULL;
	const bool cube = !vLight->parallel && vLight->pointLight && viewDef->shadowSkipSet.shadowMapSingleSide == -1;
	const bool cached = vLight->shadowOp != 0;
	if( !sun && !cube && !cached )
	{
		return;		// one side and no cache, or a crop only the backend knows: every surface is drawn
	}

	int numSurfs = 0;
	for( const drawSurf_t* surf = vLight->globalShadows; surf != NULL; surf = surf->nextOnLight )
	{
		numSurfs++;
	}
	if( numSurfs == 0 )
	{
		return;
	}
	byte* bits = ( byte* )R_FrameAlloc( numSurfs );

	// one percent wider than the faces the backend draws ( it draws them with the same snapshot value )
	const float faceTan = idMath::Tan( viewDef->shadowSkipSet.shadowMapFrustumFOV * idMath::PI / 360.0f ) * 1.01f;

	byte activeSides = 0;
	const viewEntity_t* lastSpace = NULL;		// the surfaces of one entity follow one another in the chain:
	bool lastSpaceCacheable = false;			// its test runs once
	// S3a: which entities the static maps hold is decided by a stamp ( the frame the maps were last built ):
	// a local light's own record, the sun one stamp for all its cascades ( the caster bits carry a single
	// static bit per surface ); without reuse, the maps are built this frame from every cacheable caster
	const idRenderLightLocal* light = vLight->lightDef;
	shadowCacheLightRec_t* rec = NULL;
	bool byRecord = false;			// the static maps hold what a record says, not every cacheable caster
	int heldThrough = tr.frameCount;
	int* wantSince = NULL;
	if( cached && r_shadowCacheReuse.GetBool() )
	{
		if( vLight->parallel )
		{
			byRecord = true;
			heldThrough = shadowCacheState.sun.heldThrough;
			wantSince = &shadowCacheState.sun.wantSince;
		}
		else
		{
			rec = R_ShadowCacheClaimRec( light );
			if( rec != NULL )
			{
				byRecord = true;
				heldThrough = rec->heldThrough;
				wantSince = &rec->wantSince;
			}
		}
	}
	const int staticFrames = r_shadowCacheStaticFrames.GetInteger();

	// does the backend draw a static map of this light this frame? ( any of its ops is a rebuild )
	bool rebuilds = false;
	if( cached )
	{
		const shadowCacheOp_t* ops = &viewDef->shadowOps[vLight->shadowOp - 1];
		const int numSides = R_ShadowCacheNumSides( vLight, viewDef->shadowSkipSet );
		for( int i = 0; i < numSides; i++ )
		{
			rebuilds = rebuilds || ( ops[i].flags & SCOP_REBUILD ) != 0;
		}
	}

	bool waits = false;				// a caster turned static after the maps were built: they want an update
	bool lastSpaceWaits = false;
	int index = 0;
	for( const drawSurf_t* surf = vLight->globalShadows; surf != NULL; surf = surf->nextOnLight, index++ )
	{
		const srfTriangles_t* tri = surf->frontEndGeo;
		if( tri == NULL || surf->space == NULL )
		{
			bits[index] = SCB_SIDE_MASK;
			activeSides |= SCB_SIDE_MASK;
			continue;
		}

		const idRenderEntityLocal* def = surf->space->entityDef;
		if( surf->space != lastSpace )
		{
			lastSpace = surf->space;
			lastSpaceCacheable = cached && def != NULL && R_ShadowCacheEntityCacheable( def );
			lastSpaceWaits = false;
			if( lastSpaceCacheable && byRecord )
			{
				// in the maps: unchanged for the static interval when the maps were last built
				const int lastChanged = R_ShadowCacheLastChanged( def );
				lastSpaceCacheable = R_ShadowCacheInUnit( lastChanged, staticFrames, heldThrough );
				lastSpaceWaits = !lastSpaceCacheable && R_ShadowCacheIsStatic( tr.frameCount, lastChanged, staticFrames );
			}
		}
		const bool isStatic = lastSpaceCacheable && surf->material != NULL && R_ShadowCacheSurfaceCacheable( surf->material );
		if( lastSpaceWaits && surf->material != NULL && R_ShadowCacheSurfaceCacheable( surf->material ) )
		{
			waits = true;
		}

		// A static surface of a light that draws no static map this frame is drawn by nobody: its map is
		// copied. Its side bits are read only if the backend misses the copy and draws the chain without
		// the cache, and then it is drawn into every side, as the engine does without the bits. So the
		// box work is left out for it
		if( isStatic && !rebuilds )
		{
			bits[index] = SCB_STATIC | SCB_SIDE_MASK;
			shadowCachePlan.casters++;
			continue;
		}

		// the surface's box in the frame relative to the render origin, and in the sun's light view
		// ( idRenderMatrix::CreateViewMatrix: view x runs along -axis[1], view y along axis[2] )
		idVec3 corners[8];
		tri->bounds.ToPoints( corners );
		idBounds box;
		box.Clear();
		float viewMin[2] = { idMath::INFINITUM, idMath::INFINITUM };
		float viewMax[2] = { -idMath::INFINITUM, -idMath::INFINITUM };
		for( int j = 0; j < 8; j++ )
		{
			idVec3 p;
			R_LocalPointToGlobal( surf->space->modelMatrixRel, corners[j], p );
			box.AddPoint( p );
			if( sun )
			{
				const idVec3 d = p - scv->sunViewOrigin;	// the sun's light view sits at the relative eye
				const float v[2] = { -( d * scv->sunAxis[1] ), d * scv->sunAxis[2] };
				for( int k = 0; k < 2; k++ )
				{
					viewMin[k] = Min( viewMin[k], v[k] );
					viewMax[k] = Max( viewMax[k], v[k] );
				}
			}
		}

		byte reach = 0;
		if( sun )
		{
			for( int c = 0; c < scv->numCascades; c++ )
			{
				const shadowCacheCascade_t& cascade = scv->cascades[c];
				const double padX = 2.0 * ( cascade.right - cascade.left ) / ( double )cascade.res;	// two texels
				const double padY = 2.0 * ( cascade.top - cascade.bottom ) / ( double )cascade.res;
				if( viewMax[0] >= cascade.left - padX && viewMin[0] <= cascade.right + padX
						&& viewMax[1] >= cascade.bottom - padY && viewMin[1] <= cascade.top + padY )
				{
					reach |= ( byte )( 1 << c );
				}
			}
		}
		else if( cube )
		{
			const idVec3 lo = box[0] - vLight->globalLightOrigin;
			const idVec3 hi = box[1] - vLight->globalLightOrigin;
			for( int axis = 0; axis < 3; axis++ )
			{
				const int u = ( axis + 1 ) % 3;
				const int v = ( axis + 2 ) % 3;
				// faces 2 * axis ( +axis ) and 2 * axis + 1 ( -axis ): how far the box reaches along the face's direction
				const float along[2] = { hi[axis], -lo[axis] };
				for( int s = 0; s < 2; s++ )
				{
					const float halfWidth = along[s] * faceTan;	// the frustum's half width where the box reaches farthest
					if( along[s] > 0.0f && lo[u] <= halfWidth && hi[u] >= -halfWidth && lo[v] <= halfWidth && hi[v] >= -halfWidth )
					{
						reach |= ( byte )( 1 << ( axis * 2 + s ) );
					}
				}
			}
		}
		else
		{
			reach = 1;		// a spot light's one map
		}

		// S2c: the surface belongs to the static maps of the sides it reaches when its entity and its
		// material may be cached; everything else is an active caster, drawn every frame on top
		if( isStatic )
		{
			reach |= SCB_STATIC;
		}
		if( !isStatic )
		{
			activeSides |= ( byte )( reach & SCB_SIDE_MASK );		// reach may carry SCB_STATIC: the mask removes it
		}
		bits[index] = reach;

		shadowCachePlan.casters++;
		shadowCachePlan.casterSidesAll += sun ? scv->numCascades : ( cube ? 6 : 1 );
		for( int k = 0; k < 6; k++ )
		{
			const int hit = ( reach >> k ) & 1;
			shadowCachePlan.casterSides += hit;
			if( cached )
			{
				shadowCachePlan.staticSides += isStatic ? hit : 0;
				shadowCachePlan.activeSides += isStatic ? 0 : hit;
			}
		}
	}

	if( waits && wantSince != NULL && *wantSince == 0 )
	{
		*wantSince = tr.frameCount;
	}

	vLight->shadowCasterBits = bits;
	vLight->numShadowCasterBits = numSurfs;
	vLight->shadowActiveSides = activeSides;
}

/*
==========================================================================================

The shadow cache hooks. R_ShadowCacheDecide fills the atlas pass's cvar snapshot, places
the primary sun's cascade windows, plans the atlas, emits the ops and the caster bits;
S3a fills the two notify hooks ( the change history and the light's change of shape ) and
the publish ( the backend's miss count ); the debug overlay is still a no-op.

==========================================================================================
*/

void R_ShadowCacheDecide()
{
	viewDef_t* viewDef = tr.viewDef;
	const int mode = r_shadowCache.GetInteger();

	// S3a: below mode 2 nothing is cached. The records are dropped ( the next mode-2 frame starts as a new map
	// does ), so the notify hooks return at once in modes 0 and 1
	if( mode < 2 && viewDef->isMainView && shadowCacheState.world != NULL )
	{
		shadowCacheState.world = NULL;
	}

	// only the main view places windows, plans and caches; every other view keeps today's path ( D41, D43 )
	if( mode < 1 || !viewDef->isMainView || ( viewDef->renderView.rdflags & RDF_NOSHADOWS ) != 0 )
	{
		return;
	}

	// S2a: the snapshot the atlas pass obeys ( D36 ), then the plan ( it may change a light's shadowLOD )
	bool planned = false;
	if( mode >= 2 )
	{
		shadowSkipSet_t& skipSet = viewDef->shadowSkipSet;
		// the cascade count is the one R_SetupSplitFrustums gave this view ( frame data ), not a second read of the cvar
		int numSplits = 0;
		while( numSplits < SHADOWCACHE_MAX_CASCADES - 1 && viewDef->frustumSplitDistances[numSplits] != idMath::INFINITUM )
		{
			numSplits++;
		}
		skipSet.shadowMapSplits = numSplits;
		skipSet.shadowMapSplitFar = r_shadowMapSplitFar.GetFloat();
		skipSet.shadowMapSingleSide = r_shadowMapSingleSide.GetInteger();
		skipSet.shadowMapFrustumFOV = r_shadowMapFrustumFOV.GetFloat();
		skipSet.skipShadows = r_skipShadows.GetBool();
		skipSet.useShadowAtlas = r_useShadowAtlas.GetBool();
		skipSet.valid = true;

		// with shadows or the atlas off there is nothing to plan. The atlas pass itself reads these two cvars
		// live, like every other backend reader of them, so a toggle between the frontend and the backend cannot make
		// the backend disagree with itself; without a plan and bits it then simply draws as it did before S2a
		if( !skipSet.skipShadows && skipSet.useShadowAtlas )
		{
			R_ShadowCacheBeginView( viewDef );
			memset( &shadowCachePlan, 0, sizeof( shadowCachePlan ) );
			R_ShadowCachePlanAtlas( viewDef );
			planned = true;
		}
	}

	// S1: the primary sun's cascade windows
	for( const viewLight_t* vLight = viewDef->viewLights; vLight != NULL; vLight = vLight->next )
	{
		if( vLight->isPrimarySun && vLight->parallel && vLight->shadowLOD >= 0 )
		{
			R_ShadowCacheSunWindows( viewDef, vLight );
			break;
		}
	}

	if( !planned )
	{
		return;
	}

	// S2c: which units are cached this frame, and where their static maps live ( after the windows:
	// the sun is cached only with them; before the bits: they mark the static casters of cached lights )
	R_ShadowCacheEmitOps( viewDef );

	// S2a: the per-side bits of every caster ( after the windows: the sun's bits test them )
	for( viewLight_t* vLight = viewDef->viewLights; vLight != NULL; vLight = vLight->next )
	{
		if( R_ShadowCacheLightHasUnits( vLight ) )
		{
			R_ShadowCacheCasterBits( viewDef, vLight );
		}
	}

	// S3b: the HUD's capacity line ( the TRACK S rows of Console.cpp )
	tr.pc.c_shadowCacheUnits = shadowCachePlan.ops;
	tr.pc.c_shadowCacheLightsBuilt = shadowCacheState.rebuilt + shadowCacheState.repaired;
	tr.pc.c_shadowCacheLightsWaiting = shadowCacheState.waiting;
	tr.pc.c_shadowCacheEvicted = shadowCacheState.evicted;
	tr.pc.c_shadowCacheCellsUsed = shadowCacheState.cellsUsed;
	tr.pc.c_shadowCacheCells = shadowCacheState.atlasCells;

	if( r_shadowCacheLog.GetBool() && ( tr.frameCount & 63 ) == 0 )
	{
		common->Printf( "shadowCache: frame %d plan demoted %d dropped %d casters %d sides %d of %d ops %d uncached %d static %d active %d\n",
						tr.frameCount, shadowCachePlan.demoted, shadowCachePlan.dropped, shadowCachePlan.casters, shadowCachePlan.casterSides, shadowCachePlan.casterSidesAll,
						shadowCachePlan.ops, shadowCachePlan.uncachedLights, shadowCachePlan.staticSides, shadowCachePlan.activeSides );
		common->Printf( "shadowCache: frame %d lights reused %d rebuilt %d repaired %d waiting %d moving %d sun reused %d rebuilt %d cap %d | totals rebuilt %d repaired %d evicted %d changes %d epoch %d misses %d\n",
						tr.frameCount, shadowCacheState.reused, shadowCacheState.rebuilt, shadowCacheState.repaired, shadowCacheState.waiting, shadowCacheState.moving,
						shadowCacheState.sunReused, shadowCacheState.sunRebuilt, shadowCacheState.capHit,
						shadowCacheState.totalRebuilt, shadowCacheState.totalRepaired, shadowCacheState.totalEvicted, shadowCacheState.changes.total,
						shadowCacheState.epoch, ( int )shadowCacheState.missesSeen );
		common->Printf( "shadowCache: frame %d places evicted %d turns %d cost %d cells %d of %d\n",
						tr.frameCount, shadowCacheState.evicted, shadowCacheState.turns, shadowCacheState.costBuilt, shadowCacheState.cellsUsed, shadowCacheState.atlasCells );
	}
}

void R_ShadowCachePublish()
{
	// called by the thread that executed the frame's commands, while the frontend's thread is idle ( D74 ):
	// the miss count and the ring of missed serials, so that the decide pass rebuilds only the maps that
	// were missed
	if( shadowCacheState.missesPublished != backEnd.shadowCacheMisses )
	{
		shadowCacheState.missesPublished = backEnd.shadowCacheMisses;
		memcpy( shadowCacheState.missedPublished, backEnd.shadowCacheMissed, sizeof( shadowCacheState.missedPublished ) );
	}
}

void R_ShadowCacheNotifyEntity( const idRenderEntityLocal* def, const renderEntity_t* re, shadowCacheNotify_t op )
{
	shadowCacheState_t& s = shadowCacheState;
	if( s.world == NULL || def->world != s.world )
	{
		return;
	}
	// What the entity was until now ( def->parms is still the old description ): one that casts no shadow,
	// or is skinned, is in no static map. Only fields are read, never the model. An entity with a callback
	// is NOT left out: one that gained its callback on the fast path of UpdateEntityDef ( which reaches no
	// hook ) may still be in a map, and this call is then the first to know
	if( def->parms.noShadow || def->parms.joints != NULL )
	{
		return;
	}
	// An entity that was not static is in no static map: nothing to repair. Whether it could be cached at all
	// is NOT asked here: that test calls into the entity's model, which a FreeModel may already have deleted
	// ( R_CheckForEntityDefsUsingModel leaves the pointer ). EVERY update of an entity that was static is
	// recorded, also one that changes only a colour or a shader parameter: the engine moves
	// lastModifiedFrameNum on any update, and membership in a map is read from that stamp, so an update this
	// hook let pass would make the entity's next change invisible to it ( and every update frees the entity's
	// interactions, which alone can drop a noDynamicInteractions entity from the light's walk ). The history
	// cannot flood: after a recorded change the entity is not static again for the static interval
	if( !R_ShadowCacheIsStatic( tr.frameCount, R_ShadowCacheLastChanged( def ), r_shadowCacheStaticFrames.GetInteger() ) )
	{
		return;
	}
	// the bounds it was drawn into the maps with: after this call the engine forgets them
	R_ShadowCacheRecordChange( s.changes, def->globalReferenceBounds );
}

void R_ShadowCacheNotifyLight( const idRenderLightLocal* light, const renderLight_t* rl, shadowCacheNotify_t op )
{
	// the primary sun changed shape or is freed: none of its cascade maps stands
	if( shadowCacheState.world != NULL && light->world == shadowCacheState.world && shadowCacheState.sun.uniqueId != 0
			&& shadowCacheState.sun.uniqueId == light->uniqueId )
	{
		memset( &shadowCacheState.sun, 0, sizeof( shadowCacheState.sun ) );
	}
	shadowCacheLightRec_t* rec = R_ShadowCacheClaimRec( light );
	if( rec == NULL )
	{
		return;
	}
	if( op == SC_FREE )
	{
		// the place is free at once: the frames in flight copy from it before any later frame draws into it
		// ( one queue, frames executed in order: S84 )
		R_ShadowCacheFreePlace( *rec );
		memset( rec, 0, sizeof( *rec ) );
		return;
	}
	// a change of shape ( the engine's own test sits above this call ): the maps are stale, and a light that
	// keeps changing is drawn uncached
	if( rec->valid )
	{
		rec->repair = true;
	}
	rec->valid = false;
	rec->shapeFrame = tr.frameCount;
}

void R_ShadowCacheInvalidateAll()
{
	// runs wherever R_FreeDerivedData and R_CheckForEntityDefsUsingModel run: a console command, a level load,
	// and game code freeing a model during a tic ( FreeModel ), always on the frontend's thread or while it is
	// stopped: one int, compared by every record and by the light jobs of the next frame
	shadowCacheState.epoch++;
}

// The shadow cache's debug-tools overlay; X0 lands it as a no-op and track S fills the body.
void R_ShadowCacheDebugTools( nvrhi::ICommandList* commandList, const viewDef_t* viewDef )
{
}

/*
==========================================================================================

The backend half ( S2b ). Members of idRenderBackend, declared in the TRACK S block of
RenderBackend.h. They run on the thread that executes the frame's commands and touch no
frontend state.

==========================================================================================
*/

/*
=============
idRenderBackend::ShadowCacheClearCaches

Release what the copy draw created. Called by ClearCaches, which runs on a framebuffer resize,
an image reload, a level load, a vid_restart and at shutdown, where the device's caches go too.
=============
*/
void idRenderBackend::ShadowCacheClearCaches()
{
	// a member sees its own private members: the layout checks live here
	static_assert( sizeof( shadowCacheBuilt ) == sizeof( unsigned int ) * SHADOWCACHE_MAX_SLOTS, "one stamp per slot" );
	static_assert( sizeof( shadowCacheMissed ) == sizeof( unsigned int ) * SHADOWCACHE_MISS_RING, "the frontend's copy is as wide as the backend's ring" );

	shadowCacheCopyVS = nullptr;
	shadowCacheCopyPS = nullptr;
	shadowCacheCopyLayout = nullptr;
	shadowCacheCopySource = nullptr;
	shadowCacheCopyBindingSet = nullptr;
	for( int i = 0; i < 2; i++ )
	{
		shadowCacheCopyPipeline[i] = nullptr;
	}
	// S3a: the cache images may be new ones: no stamp may match until a map is drawn again
	memset( shadowCacheBuilt, 0, sizeof( shadowCacheBuilt ) );
}

/*
=============
idRenderBackend::ShadowCacheTarget

The render target of one cache image, as an engine Framebuffer found by name; never keep the
pointer past the frame: Framebuffer::ResizeFramebuffers deletes every Framebuffer object ( D45 ).
A new one is made when there is none ( every resize and image reload deletes the engine's
Framebuffer objects ); *recreated then tells the caller that the image's contents are lost.
=============
*/
Framebuffer* idRenderBackend::ShadowCacheTarget( int which, bool* recreated )
{
	idImage* image = ( which < 0 ) ? globalImages->shadowCacheAtlasImage : globalImages->shadowCacheCascadeImage[which];
	nvrhi::ITexture* texture = image->GetTextureHandle().Get();
	if( texture == NULL )
	{
		return NULL;
	}

	// the name in one idStr: va() returns the buffer of a temporary idStr that dies at the end of the
	// statement, so each piece is copied into kind and name right away
	const idStr kind = ( which < 0 ) ? "Atlas" : va( "Cascade%i", which );
	const idStr name = va( "_shadowCache%s_%i", kind.c_str(), shadowCacheTargetSerial );

	Framebuffer* fb = Framebuffer::Find( name );
	if( fb != NULL && fb->GetApiObject()->getDesc().depthAttachment.texture == texture )
	{
		return fb;
	}

	// none ( a resize deleted it ), or a stale one ( the image has a new texture ): a new name, so that the
	// stale one is never found again; the engine's list owns the object and deletes it at the next resize.
	// All six cache images share the serial: after a bump the other five are not found under the new name
	// and are made again, and the event that replaced this texture threw their contents away too.
	if( fb != NULL )
	{
		shadowCacheTargetSerial++;
	}
	if( recreated != NULL )
	{
		*recreated = true;
	}
	return new Framebuffer( va( "_shadowCache%s_%i", kind.c_str(), shadowCacheTargetSerial ),
							nvrhi::FramebufferDesc().setDepthAttachment( texture ) );
}

/*
=============
idRenderBackend::ShadowCacheCopyDraw

One depth-writing copy ( or, with constants.res 0, a clear to 1.0 ) of the rectangle
( tileOrigin, tileRes ) of target; source is the cached map's texture and must not be the
target's own depth texture; the caller calls ResetGraphicsStateCache() before the engine draws
again ( D34 ).
=============
*/
void idRenderBackend::ShadowCacheCopyDraw( nvrhi::IFramebuffer* target, int targetKind, nvrhi::ITexture* source, const shadowCacheCopyConstants_t& constants )
{
	nvrhi::IDevice* device = deviceManager->GetDevice();

	if( !shadowCacheCopyVS )
	{
		// found at first use, as the engine's own passes do ( no builtins[] row, no engine binding layout: D34 )
		shadowCacheCopyVS = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/shadow_cache_copy", SHADER_STAGE_VERTEX, "", idList<shaderMacro_t>(), true, LAYOUT_DRAW_VERT ) );
		shadowCacheCopyPS = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/shadow_cache_copy", SHADER_STAGE_FRAGMENT, "", idList<shaderMacro_t>(), true, LAYOUT_DRAW_VERT ) );

		nvrhi::BindingLayoutDesc layoutDesc;
		layoutDesc.visibility = nvrhi::ShaderType::All;
		layoutDesc.bindings =
		{
			nvrhi::BindingLayoutItem::PushConstants( 0, sizeof( shadowCacheCopyConstants_t ) ),
			nvrhi::BindingLayoutItem::Texture_SRV( 0 )
		};
		shadowCacheCopyLayout = device->createBindingLayout( layoutDesc );
	}

	// the pipeline must agree with the framebuffer's formats
	const nvrhi::FramebufferInfo& info = target->getFramebufferInfo();
	if( !shadowCacheCopyPipeline[targetKind] || !( shadowCacheCopyPipelineInfo[targetKind] == info ) )
	{
		nvrhi::GraphicsPipelineDesc psoDesc;
		psoDesc.bindingLayouts = { shadowCacheCopyLayout };
		psoDesc.VS = shadowCacheCopyVS;
		psoDesc.PS = shadowCacheCopyPS;
		psoDesc.primType = nvrhi::PrimitiveType::TriangleList;
		psoDesc.renderState.rasterState.setCullNone();
		psoDesc.renderState.depthStencilState.depthTestEnable = true;
		psoDesc.renderState.depthStencilState.depthWriteEnable = true;
		psoDesc.renderState.depthStencilState.depthFunc = nvrhi::ComparisonFunc::Always;
		psoDesc.renderState.depthStencilState.stencilEnable = false;

		shadowCacheCopyPipeline[targetKind] = device->createGraphicsPipeline( psoDesc, target );
		shadowCacheCopyPipelineInfo[targetKind] = info;
	}

	// S3b: one binding set per source image, kept while the source stays the same: the copies of a view
	// follow one another, and the local lights share one cache image
	if( !shadowCacheCopyBindingSet || shadowCacheCopySource.Get() != source )
	{
		nvrhi::BindingSetDesc bindingSetDesc;
		bindingSetDesc.bindings =
		{
			nvrhi::BindingSetItem::PushConstants( 0, sizeof( shadowCacheCopyConstants_t ) ),
			nvrhi::BindingSetItem::Texture_SRV( 0, source )
		};
		shadowCacheCopyBindingSet = bindingCache.GetOrCreateBindingSet( bindingSetDesc, shadowCacheCopyLayout );
		shadowCacheCopySource = source;
	}

	const nvrhi::Viewport viewport( ( float )constants.tileOrigin[0], ( float )( constants.tileOrigin[0] + constants.tileRes ),
									( float )constants.tileOrigin[1], ( float )( constants.tileOrigin[1] + constants.tileRes ), 0.0f, 1.0f );

	nvrhi::GraphicsState state;
	state.indexBuffer = {};		// the fork leaves this one member uninitialized
	state.pipeline = shadowCacheCopyPipeline[targetKind];
	state.framebuffer = target;
	state.bindings = { shadowCacheCopyBindingSet };
	state.viewport.addViewport( viewport );
	state.viewport.addScissorRect( nvrhi::Rect( viewport ) );

	commandList->setGraphicsState( state );
	commandList->setPushConstants( &constants, sizeof( constants ) );

	nvrhi::DrawArguments args;
	args.instanceCount = 1;
	args.vertexCount = 3;
	commandList->draw( args );
}

/*
=============
idRenderBackend::ShadowCacheDrawStaticMaps

The static maps of this view's cached units ( S2c ): each unit's region of its cache image is cleared and
its static casters are drawn into it, with the state ShadowMapPassFast draws any caster with. S3a: only
the ops flagged SCOP_REBUILD are drawn ( a reused unit's map is already in its cache image ); each region
drawn is stamped with the op's serial ( D74 ), the stamp the copy pass checks. Runs in its own main block,
MRB_SHADOW_CACHE, before the atlas block opens ( D70 ); every rebuild of the view precedes every copy.
=============
*/
void idRenderBackend::ShadowCacheDrawStaticMaps()
{
	// S3a: a view whose units are all reused has nothing to draw here, and opens no block
	bool anyRebuild = false;
	for( int i = 0; i < viewDef->numShadowOps && !anyRebuild; i++ )
	{
		anyRebuild = ( viewDef->shadowOps[i].flags & SCOP_REBUILD ) != 0;
	}
	if( !anyRebuild )
	{
		return;
	}

	renderLog.OpenMainBlock( MRB_SHADOW_CACHE );
	renderLog.OpenBlock( "Render_ShadowCache", colorYellow );

	Framebuffer* previousFramebuffer = Framebuffer::GetActiveFramebuffer();

	// the six cache targets, found once for this view: the atlas at index 0, cascade map i at i + 1
	// ( Framebuffer::Find is a string search; never keep these past the view )
	Framebuffer* targets[6];
	for( int i = 0; i < 6; i++ )
	{
		targets[i] = ShadowCacheTarget( i - 1, NULL );
	}

	for( viewLight_t* vLight = viewDef->viewLights; vLight != NULL; vLight = vLight->next )
	{
		if( vLight->shadowOp == 0 )
		{
			continue;
		}
		const shadowCacheOp_t* ops = &viewDef->shadowOps[vLight->shadowOp - 1];
		const bool spot = !vLight->parallel && !vLight->pointLight;
		const int numSides = vLight->parallel ? ( viewDef->shadowSkipSet.shadowMapSplits + 1 ) : ( spot ? 1 : 6 );

		for( int i = 0; i < numSides; i++ )
		{
			const shadowCacheOp_t& op = ops[i];
			if( ( op.flags & SCOP_REBUILD ) == 0 )
			{
				continue;		// the map is in the cache from an earlier frame
			}
			Framebuffer* target = targets[op.target + 1];
			if( target == NULL )
			{
				continue;
			}

			shadowCacheCopyConstants_t constants;
			memset( &constants, 0, sizeof( constants ) );
			constants.tileOrigin[0] = op.x;
			constants.tileOrigin[1] = op.y;
			constants.tileRes = op.size;
			// the unit's region back to the far depth. The clear mode reads no texel, but the layout needs a
			// texture: one that is never a render target, so the clear causes no state change anywhere
			ShadowCacheCopyDraw( target->GetApiObject(), 1, globalImages->blackImage->GetTextureHandle().Get(), constants );
			ResetGraphicsStateCache();

			// the static casters that reach this side
			const int bit = 1 << i;
			ShadowMapPassFast( vLight->globalShadows, vLight, spot ? -1 : i, false, vLight->shadowCasterBits, bit | SCB_STATIC, bit | SCB_STATIC, &op, target );

			shadowCacheBuilt[R_ShadowCacheSlot( op )] = op.serial;		// D74: only the backend knows that the map was drawn
		}
	}

	if( previousFramebuffer != NULL )
	{
		previousFramebuffer->Bind();
	}
	else
	{
		Framebuffer::Unbind();
	}

	renderLog.CloseBlock();
	renderLog.CloseMainBlock();

	// X0's rule for a main block ( the doc comment of ResetGraphicsStateCache ): a main-block open or
	// close ends the Vulkan render pass, so the engine's state cache must forget what it thinks is bound
	ResetGraphicsStateCache();
}
