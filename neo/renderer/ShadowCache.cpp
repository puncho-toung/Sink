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
idCVar r_shadowCacheDebug( "r_shadowCacheDebug", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "S5: where the shadow cache's texels came from: 1 = the primary sun's cascade maps as stored, in panels at the bottom of the screen ( their depth in red; on D3D12 a grey panel ), the places drawn in the last views coloured ( blue a build in full, yellow a strip, green a repair, a magenta frame an addition, cyan a heal; a white cross at the window's first texel ), 2 = also the cache atlas of the local lights while r_shadowCache is 2 ( this frame's reused maps framed green )", 0, 2 );

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

// S4c: the extent of a slice along gridX ( [0] ) and gridY ( [1] ), in double: the box of its eight corners on the grid.
static void R_ShadowCacheSliceExtent( const shadowLightFrame_t& frame, const shadowCacheSlice_t& s, const idVec3& viewOrigin, const idMat3& viewAxis,
									  double lo[2], double hi[2] )
{
	lo[0] = 1e300;
	lo[1] = 1e300;
	hi[0] = -1e300;
	hi[1] = -1e300;
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
}

// Centre a cascade's window on the middle of its slice's extent along the grid ( R_ShadowCacheSliceExtent ): not on the
// average of the corners, which lies off the middle of a slice seen at an angle.
static void R_ShadowCacheSliceWindow( const double lo[2], const double hi[2], double texelWorld, int res, shadowCacheWindow_t& out )
{
	R_ShadowCacheSnapWindowAt( 0.5 * ( lo[0] + hi[0] ), 0.5 * ( lo[1] + hi[1] ), texelWorld, res, out );
}

/*
==========================================================================================

S4: toroidal storage of the cascades ( 08 section 6, 01 section 5 )

A cascade map holds world-grid texel g at physical texel ( g - base ) mod res, where base is the
window's origin when the map was last drawn in full. The window moves by changing which physical
texel is logical texel 0: no data moves, and only the strips the window gained are cleared and
drawn. A build in full sets base to the window's origin, so such a map has no wrap and is drawn as
before S4. The depth range is fixed to the light volume ( D16 ), so a scroll moves nothing along
the light. res is a power of two ( shadowMapResolutions ). The frontend and the backend both call
these: both halves of the cache live in this file.

==========================================================================================
*/

// The texels of the current window that the previous window did not hold, after the window moved by ( dx, dy ) texels:
// texel ( x, y ) of the current window holds what texel ( x + dx, y + dy ) of the previous one held. The columns that
// came in on one side, and the rows that came in on one side between them: one or two rects, which do not overlap.
// Every static caster that overlaps a strip is drawn into it, not only those new to the window ( 01 section 5: a caster
// that straddled the old window's edge has texels in the strip too ). The caller keeps |dx| and |dy| below res.
static int R_ShadowCacheExposedRects( int dx, int dy, int res, shadowCacheRect_t rects[2] )
{
	// the part of the current window that the previous one covered
	const int keepX0 = Max( 0, -dx );
	const int keepX1 = Min( res, res - dx );
	const int keepY0 = Max( 0, -dy );
	const int keepY1 = Min( res, res - dy );
	int num = 0;
	if( keepX0 >= keepX1 || keepY0 >= keepY1 )
	{
		rects[num++] = { 0, 0, res, res };
		return num;
	}
	if( keepX0 > 0 )
	{
		rects[num++] = { 0, 0, keepX0, res };
	}
	else if( keepX1 < res )
	{
		rects[num++] = { keepX1, 0, res, res };
	}
	if( keepY0 > 0 )
	{
		rects[num++] = { keepX0, 0, keepX1, keepY0 };
	}
	else if( keepY1 < res )
	{
		rects[num++] = { keepX0, keepY1, keepX1, res };
	}
	return num;
}

// The physical texel of the window's logical texel 0 along one axis: ( origin - base ) mod res. Two's complement: the
// right residue for a negative difference too.
static int R_ShadowCacheWrapCoord( int64 origin, int64 base, int res )
{
	return ( int )( ( origin - base ) & ( int64 )( res - 1 ) );
}

// Split a rect of the window into the 1, 2 or 4 physical pieces it occupies. Logical x maps to physical x + wrapX below
// res - wrapX and to x + wrapX - res from there on; the same in y. Draw each piece with the window's own projection, a
// res x res viewport whose corner is at ( viewportX, viewportY ), and the piece as the scissor: set the two apart, since
// a viewport may reach past the map or start at a negative offset and a scissor may not.
static int R_ShadowCacheToroidalRects( int wrapX, int wrapY, int res, const shadowCacheRect_t& logical, shadowCacheToroidalRect_t out[4] )
{
	const int splitX = Max( logical.x0, Min( logical.x1, res - wrapX ) );
	const int splitY = Max( logical.y0, Min( logical.y1, res - wrapY ) );
	const int xs[3] = { logical.x0, splitX, logical.x1 };
	const int ys[3] = { logical.y0, splitY, logical.y1 };
	int num = 0;
	for( int j = 0; j < 2; j++ )
	{
		for( int i = 0; i < 2; i++ )
		{
			if( xs[i] >= xs[i + 1] || ys[j] >= ys[j + 1] )
			{
				continue;
			}
			const int offX = ( i == 0 ) ? wrapX : wrapX - res;
			const int offY = ( j == 0 ) ? wrapY : wrapY - res;
			shadowCacheToroidalRect_t& piece = out[num++];
			piece.physical.x0 = xs[i] + offX;
			piece.physical.x1 = xs[i + 1] + offX;
			piece.physical.y0 = ys[j] + offY;
			piece.physical.y1 = ys[j + 1] + offY;
			piece.viewportX = offX;
			piece.viewportY = offY;
		}
	}
	return num;
}

/*
==========================================================================================

S5: repairs of the cascades ( 08 section 7 )

A static caster that moves, changes or is removed leaves its old depth in every cascade map it was
drawn into, and depth cannot be un-drawn. The decide pass clears the texels its OLD bounds can have
covered, in the window the map holds after this frame's move ( the backend repairs the map as this
frame's copy reads it ), and draws again every static caster that reaches them: the strip rule, with
the same rectangles and bits as the strips. A caster that comes to rest is added without a clear.

==========================================================================================
*/

// The texels of a window that a box in its light view ( lo..hi along the view's x and y ) can have written: columns from 'left'
// along x, rows DOWN from 'top' along y, 'texelX' and 'texelY' the size of a texel, two texels wider on every side ( as the strip
// rule ) and one more on the far side for the half-open edge, clipped to the window's res texels. False when it misses the window.
// The bounds' own texels already hold every texel a caster inside them can write; the margin is against float rounding of the
// light-view boxes ( at most 0.27 texel at 120,000 units from the origin, about 0.01 at a cascade 0 texel: O1-7 ).
static bool R_ShadowCacheTexelRect( const double lo[2], const double hi[2], double left, double top, double texelX, double texelY, int res, shadowCacheRect_t& out )
{
	const double x0 = floor( ( lo[0] - left ) / texelX ) - 2.0;
	const double x1 = floor( ( hi[0] - left ) / texelX ) + 3.0;
	const double y0 = floor( ( top - hi[1] ) / texelY ) - 2.0;
	const double y1 = floor( ( top - lo[1] ) / texelY ) + 3.0;
	// clamped before the conversion: a box far outside the window gives numbers no int holds
	out.x0 = ( int )Min( Max( x0, 0.0 ), ( double )res );
	out.x1 = ( int )Min( Max( x1, 0.0 ), ( double )res );
	out.y0 = ( int )Min( Max( y0, 0.0 ), ( double )res );
	out.y1 = ( int )Min( Max( y1, 0.0 ), ( double )res );
	return out.x0 < out.x1 && out.y0 < out.y1;
}

// The texels of cascade 'c' that a caster inside the absolute world bounds 'b' can have written, in the window's logical texels
// ( R_ShadowCacheTexelRect ). The light view sits at scv.sunViewOrigin, relative to the render origin; its x runs along -axis[1]
// and its y along axis[2] ( idRenderMatrix::CreateViewMatrix, as R_ShadowCacheCasterBits has it ). In double: the bounds are absolute.
static bool R_ShadowCacheFootprint( const shadowCacheView_t& scv, const shadowCacheCascade_t& c, const idVec3d& renderOrigin, const idBounds& b, shadowCacheRect_t& out )
{
	double lo[2] = { 1e300, 1e300 };
	double hi[2] = { -1e300, -1e300 };
	for( int j = 0; j < 8; j++ )
	{
		const double p[3] =
		{
			( double )b[j & 1].x - renderOrigin.x - ( double )scv.sunViewOrigin.x,
			( double )b[( j >> 1 ) & 1].y - renderOrigin.y - ( double )scv.sunViewOrigin.y,
			( double )b[( j >> 2 ) & 1].z - renderOrigin.z - ( double )scv.sunViewOrigin.z
		};
		const double v[2] =
		{
			-( p[0] * ( double )scv.sunAxis[1].x + p[1] * ( double )scv.sunAxis[1].y + p[2] * ( double )scv.sunAxis[1].z ),
			p[0] * ( double )scv.sunAxis[2].x + p[1] * ( double )scv.sunAxis[2].y + p[2] * ( double )scv.sunAxis[2].z
		};
		for( int k = 0; k < 2; k++ )
		{
			lo[k] = Min( lo[k], v[k] );
			hi[k] = Max( hi[k], v[k] );
		}
	}
	return R_ShadowCacheTexelRect( lo, hi, c.left, c.top, ( c.right - c.left ) / ( double )c.res, ( c.top - c.bottom ) / ( double )c.res, c.res, out );
}

// Adds rectangle 'r' to a cascade's list of rectangles to clear and draw again ( at most 'max' ) and keeps them apart: a rectangle
// that overlaps one of the list is merged with it into their bounding rectangle, which may then overlap another, and so on; when the
// list is full, 'r' is merged with the one whose bounding rectangle with it grows least. The rectangles cover every texel the
// separate ones did. Returns the new count.
static int R_ShadowCacheAddRect( shadowCacheRect_t* rects, int num, int max, shadowCacheRect_t r )
{
	assert( max > 0 && num <= max );
	for( ;; )
	{
		int hit = -1;
		for( int i = 0; i < num && hit < 0; i++ )
		{
			if( r.x0 < rects[i].x1 && rects[i].x0 < r.x1 && r.y0 < rects[i].y1 && rects[i].y0 < r.y1 )
			{
				hit = i;
			}
		}
		if( hit < 0 && num == max )
		{
			int64 best = 0;
			for( int i = 0; i < num; i++ )
			{
				const int64 grow = ( int64 )( Max( r.x1, rects[i].x1 ) - Min( r.x0, rects[i].x0 ) ) * ( Max( r.y1, rects[i].y1 ) - Min( r.y0, rects[i].y0 ) )
								   - ( int64 )( rects[i].x1 - rects[i].x0 ) * ( rects[i].y1 - rects[i].y0 );
				if( hit < 0 || grow < best )
				{
					hit = i;
					best = grow;
				}
			}
		}
		if( hit < 0 )
		{
			rects[num++] = r;
			return num;
		}
		// the two become one, taken out of the list: it is added again by the next round, which looks for overlaps anew
		r.x0 = Min( r.x0, rects[hit].x0 );
		r.y0 = Min( r.y0, rects[hit].y0 );
		r.x1 = Max( r.x1, rects[hit].x1 );
		r.y1 = Max( r.y1, rects[hit].y1 );
		rects[hit] = rects[--num];
	}
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

// S4c: the texels a kept window must hold beyond the margin: the froxel fog ( V3 ) looks a cascade up at its columns'
// centres, up to 11 pixels past the view's right and bottom edges, about 8 texels at cascade 0's far end
#define SHADOWCACHE_KEEP_SLACK		8

// S4c: defined with the records below
static bool R_ShadowCacheKeepWindow( const viewDef_t* viewDef, const viewLight_t* sun, int cascade, int res, double texelWorld, int margin,
									 const double lo[2], const double hi[2], int64& originX, int64& originY );

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
		double lo[2];
		double hi[2];
		R_ShadowCacheSliceExtent( frame, slice, renderView.vieworg, renderView.viewaxis, lo, hi );
		shadowCacheWindow_t win;
		R_ShadowCacheSliceWindow( lo, hi, texelWorld, res, win );

		// S4c: in mode 2 a cascade keeps the window its cached map was drawn with while this frame's slice, with its
		// margins, still lies inside it. The texels are anchored to the world, so the picture does not change; the window
		// moves ( to the slice's centre, as above ) only when the slice would leave it, so a walking camera scrolls the
		// map now and then by many texels instead of every frame by a few
		int64 keptX, keptY;
		if( R_ShadowCacheKeepWindow( viewDef, sun, i, res, texelWorld, marginTexels, lo, hi, keptX, keptY ) )
		{
			win.originX = keptX;
			win.originY = keptY;
		}

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
	int				retryFrame;		// no place was free for maps of the engine's pick: no full gather and no build until this frame
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
// membership ( S4: only the strips its window gained, while it moved by less than a map, R_ShadowCacheEmitOps ).
// S5: an unseen change that touches the light is repaired where its old bounds reach, and the entities that
// turned static join the membership drawn on top of the maps; all cascades are drawn again only when the
// history of changes has run out, the epoch changed, or a map was lost.
struct shadowCacheSunCascadeRec_t
{
	int64			originX;		// the window the map was drawn with ( shadowCacheCascade_t ); not compared on a clamped axis
	int64			originY;
	double			texelWorld;
	int				res;
	int64			baseX;			// S4: the window's origin when the map was last drawn in full: world-grid texel g lives at physical
	int64			baseY;			// texel ( g - base ) mod res ( toroidal storage ); not used on a clamped axis
	unsigned int	serial;			// the build of this cascade's map ( S4: its last build in full or its last scroll )
	bool			clampedX;
	bool			clampedY;
	bool			valid;
	byte			pad;
};

struct shadowCacheSunRec_t
{
	int							uniqueId;		// the sun light's idRenderLightLocal::uniqueId; 0 = none
	int							heldThrough;	// tr.frameCount of the decide pass that last drew every cascade ( S5: or added the entities that turned static ): which entities the maps hold
	int							changesSeen;	// shadowCacheState.changes.total when a decide pass last looked
	int							epoch;			// shadowCacheState.epoch the membership was set under
	int							wantSince;		// tr.frameCount since which an entity that turned static waits to be drawn into the maps; 0 = none
	int							numCascades;
	int							pick;			// S4 ( D43 ): the size the engine picked for the cascades when the sun was last a cache candidate
	int							pickFrame;		// S4: tr.frameCount of the decide pass that first saw that pick: maps of another size are built again only when it has held ( SHADOWCACHE_PICK_HOLD_FRAMES )
	int							lastUsedFrame;	// S4: tr.frameCount of the last decide pass that had the sun as a cache candidate: a sun that comes back after an absence as long as the hold has served it
	int							addedFrom;		// S5: the membership stamp before the entities that turned static joined it ( heldThrough until then ), and
	int							addedFrame;		// the decide pass that let them join: that pass's R_ShadowCacheCasterBits marks them ( SCB_ADDED )
	bool						valid;			// every cascade holds a usable map and the membership stands
	int							gatherAskedFrame;	// S5: tr.frameCount of a decide pass that had a map to draw and no chain gathered in full: the next frame's light job, and only it, gathers in full
	// the main view the windows were last placed for: while the view is the same, so are the windows, and
	// the light job need not gather in full
	idVec3						viewOrigin;
	idMat3						viewAxis;
	float						fovX;
	float						fovY;
	idVec3						gridX;			// S5: the grid's axes in the world ( -sunAxis[1], -sunAxis[2] ), for the light job's look at
	idVec3						gridY;			// the windows ( R_ShadowCacheSunWindowsKept )
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
	int							sunScrolled;	// S4: the sun's cascades of this view that kept their map and drew the strips their window gained
	int							sunRepaired;	// S5: the sun's cascades of this view that cleared and drew again what moved or removed casters left
	int							sunAdded;		// S5: the sun's cascades of this view that drew the casters that came to rest on top of their map
	unsigned int				healsPublished;	// S4: written by R_ShadowCachePublish: scrolls the backend drew as a whole window ( the build they expected was not in the map )
	int							totalRebuilt;
	int							totalRepaired;
	int							totalSunRebuilt;	// S4: the sun's cascades drawn in full since the map began
	int							totalSunScrolled;	// S4: the sun's cascades that drew only the strips their window gained, since the map began
	int							totalSunRepaired;	// S5: the sun's cascades repaired, since the map began
	int							totalSunAdded;		// S5: the sun's cascades that drew casters that came to rest, since the map began
	int							totalSunLost;		// S5: the sun's builds in full of every cascade because the history of changes had run out, since the map began
	int							totalSunGathers;	// S5: the decide passes that had the sun as a cache candidate with its chain gathered in full, since the map began
	int							totalEvicted;
	int							evicted;		// S3b: records that gave their place up in this view's decide pass
	int							turns;			// S3b: lights given a turn for the next frame
	int							costBuilt;		// S3b: what this view's budgeted builds cost ( R_ShadowCacheCost )
	int							cellsUsed;		// S3b: cells of the cache atlas inside a record's place
	int							lost;			// S5: lights of this view ( the sun among them ) whose history of changes had run out
	int							changesShown;	// S5: changes.total when the last main view's counters were written
	shadowCacheChanges_t		changes;
	unsigned short				cellOwner[SHADOWCACHE_MAX_CELLS];	// the cache atlas, by Z-order cell: the index + 1 of the record whose map covers the cell, 0 = free
	shadowCacheLightRec_t		recs[SHADOWCACHE_MAX_LIGHTS];
	shadowCacheSunRec_t			sun;
};

static shadowCacheState_t shadowCacheState;

/*
=====================
R_ShadowCacheKeepWindow

S4c: the window of cascade i's cached map, when this frame's slice ( its extent along the grid, lo..hi )
still lies inside it with 'margin' texels and SHADOWCACHE_KEEP_SLACK more to spare on every side of each
axis the map does not clamp, and the map is of this grid ( size and texel ): the window then stays where it
is. A window that fails this test is centred again as S1 centres it; a slice too wide to leave the slack in
any window is centred every frame, as before S4c. Centring a still slice gives the same window again, so the
window never thrashes.
Only with the cache's records ( mode 2 ), for the sun whose records they are; reads frontend state only.
=====================
*/
static bool R_ShadowCacheKeepWindow( const viewDef_t* viewDef, const viewLight_t* sun, int cascade, int res, double texelWorld, int margin,
									 const double lo[2], const double hi[2], int64& originX, int64& originY )
{
	assert( sun->lightDef != NULL );		// R_ShadowCacheSunWindows has read it already
	const shadowCacheState_t& s = shadowCacheState;
	if( s.world == NULL || s.world != viewDef->renderWorld || s.sun.uniqueId != sun->lightDef->uniqueId
			|| cascade < 0 || cascade >= SHADOWCACHE_MAX_CASCADES )
	{
		return false;
	}
	const shadowCacheSunCascadeRec_t& crec = s.sun.cascades[cascade];
	if( !crec.valid || crec.res != res || crec.texelWorld != texelWorld )
	{
		return false;
	}
	// the slice's texels on the grid, and the margin and the fog's slack on every side of them; tested only on an axis
	// the map does not clamp: on a clamped axis it shows the light's box wherever the window stands
	const int keep = margin + SHADOWCACHE_KEEP_SLACK;
	const int64 x0 = ( int64 )floor( lo[0] / texelWorld ) - keep;
	const int64 x1 = ( int64 )floor( hi[0] / texelWorld ) + keep;
	const int64 y0 = ( int64 )floor( lo[1] / texelWorld ) - keep;
	const int64 y1 = ( int64 )floor( hi[1] / texelWorld ) + keep;
	if( ( !crec.clampedX && ( x0 < crec.originX || x1 >= crec.originX + res ) ) || ( !crec.clampedY && ( y0 < crec.originY || y1 >= crec.originY + res ) ) )
	{
		return false;
	}
	originX = crec.originX;
	originY = crec.originY;
	return true;
}

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

/*
=====================
R_ShadowCacheSunWindowsKept

S5 ( row 78 ( a ) ): read only, from the light job. Would every cascade of the sun keep its map's window this frame, so
that nothing is drawn for a move and the chain need not hold every static caster? This frame's slices, as
R_ShadowCacheSunWindows will place them, against the windows of the cascade maps ( R_ShadowCacheKeepWindow's test ), at
the records' size and texel. False whenever it cannot tell. A wrong yes costs one frame: the decide pass then finds a
window to move without a chain gathered in full, draws the sun without the cache, and asks the next frame's light job
for a full gather ( gatherAskedFrame ).
=====================
*/
static bool R_ShadowCacheSunWindowsKept( const viewDef_t* viewDef )
{
	const shadowCacheSunRec_t& sun = shadowCacheState.sun;
	const renderView_t& renderView = viewDef->renderView;
	if( shadowCacheSun.world != viewDef->renderWorld )
	{
		return false;		// the reference field of view belongs to another world
	}
	const double tanX = tan( ( double )renderView.fov_x * ( double )idMath::PI / 360.0 );
	const double tanY = tan( ( double )renderView.fov_y * ( double )idMath::PI / 360.0 );
	if( !( tanX > 0.0 && tanX < 100.0 && tanY > 0.0 && tanY < 100.0 ) )
	{
		return false;
	}
	// the reference the decide pass will size the windows with: it only grows ( D46 )
	const double refTanX = Max( shadowCacheSun.refTanX, tanX );
	const double refTanY = Max( shadowCacheSun.refTanY, tanY );

	int numCascades = 1;
	while( numCascades < SHADOWCACHE_MAX_CASCADES && viewDef->frustumSplitDistances[numCascades - 1] != idMath::INFINITUM )
	{
		numCascades++;
	}
	if( numCascades != sun.numCascades )
	{
		return false;
	}
	shadowLightFrame_t frame;
	memset( &frame, 0, sizeof( frame ) );
	frame.gridX = sun.gridX;		// R_ShadowCacheSliceExtent reads only the grid's axes
	frame.gridY = sun.gridY;
	const int marginTexels = Max( SHADOWCACHE_MARGIN_TEXELS, ( int )ceil( r_shadowMapJitterScale.GetFloat() + 1.0f ) );
	const int keep = marginTexels + SHADOWCACHE_KEEP_SLACK;
	float zNear = renderView.cramZNear ? ( r_znear.GetFloat() * 0.25f ) : r_znear.GetFloat();
	for( int i = 0; i < numCascades; i++ )
	{
		const shadowCacheSunCascadeRec_t& crec = sun.cascades[i];
		const float zFar = ( i < numCascades - 1 ) ? viewDef->frustumSplitDistances[i] : viewDef->shadowCascadeFar;
		if( !crec.valid )
		{
			return false;
		}
		shadowCacheSlice_t refSlice;
		refSlice.zNear = zNear;
		refSlice.zFar = zFar;
		refSlice.tanLeft = -refTanX;
		refSlice.tanRight = refTanX;
		refSlice.tanBottom = -refTanY;
		refSlice.tanTop = refTanY;
		if( R_ShadowCacheTexelWorld( R_ShadowCacheSliceDiameter( refSlice ), crec.res, marginTexels ) != crec.texelWorld )
		{
			return false;		// another grid: the maps are drawn in full
		}
		shadowCacheSlice_t slice = refSlice;
		slice.tanLeft = -tanX;
		slice.tanRight = tanX;
		slice.tanBottom = -tanY;
		slice.tanTop = tanY;
		double lo[2];
		double hi[2];
		R_ShadowCacheSliceExtent( frame, slice, renderView.vieworg, renderView.viewaxis, lo, hi );
		// R_ShadowCacheKeepWindow's test, on the axes the map does not clamp
		const int64 x0 = ( int64 )floor( lo[0] / crec.texelWorld ) - keep;
		const int64 x1 = ( int64 )floor( hi[0] / crec.texelWorld ) + keep;
		const int64 y0 = ( int64 )floor( lo[1] / crec.texelWorld ) - keep;
		const int64 y1 = ( int64 )floor( hi[1] / crec.texelWorld ) + keep;
		if( ( !crec.clampedX && ( x0 < crec.originX || x1 >= crec.originX + crec.res ) ) || ( !crec.clampedY && ( y0 < crec.originY || y1 >= crec.originY + crec.res ) ) )
		{
			return false;		// the window would move
		}
		// the next cascade begins half a percent inside this one, as R_ShadowCacheSunWindows has it
		zNear = zFar - ( zFar * 0.005f );
	}
	return true;
}

// Read only, called from the light jobs: must the sun's chain hold every cacheable caster this frame? Yes
// when the membership must be brought up to date ( S5: a change to repair, entities that turned static to
// add ), when the last decide pass asked for it ( S5 ), and when a window would
// move ( S5: in a view that is not the one the windows were last placed for, R_ShadowCacheSunWindowsKept ): a
// moved cascade is drawn again.
static bool R_ShadowCacheSunWantsGather( const viewDef_t* viewDef, const idRenderLightLocal* light )
{
	if( light->transitionInteractionPending )
	{
		return false;		// its interactions are still being made, past every hook: drawn uncached until they are
	}
	const shadowCacheState_t& s = shadowCacheState;
	const shadowCacheSunRec_t& sun = s.sun;
	if( s.world == NULL || light->world != s.world || sun.uniqueId != light->uniqueId || !sun.valid
			|| sun.epoch != s.epoch || sun.changesSeen != s.changes.total || sun.wantSince != 0
			|| ( sun.gatherAskedFrame != 0 && sun.gatherAskedFrame == tr.frameCount - 1 ) )
	{
		return true;
	}
	const renderView_t& rv = viewDef->renderView;
	if( rv.vieworg == sun.viewOrigin && rv.viewaxis == sun.viewAxis && rv.fov_x == sun.fovX && rv.fov_y == sun.fovY )
	{
		return false;		// the view the windows were last placed for: they stand
	}
	// S5 ( row 78 ( a ) ): another view. S4c keeps a window while its slice fits, so most such frames draw nothing:
	// gather in full only when a window would move
	return !R_ShadowCacheSunWindowsKept( viewDef );
}

// The backend found no map, or another one, where an op expected build serial ( the frame that drew it was
// never executed, or the cache images were lost ). The record that still expects that build is stale; one
// that has been rebuilt since has another serial and is left alone, so a miss reported late costs nothing.
// The sun: a missed cascade loses its map, and the emitter draws every cascade again when the sun is not
// valid ( the membership is one stamp for all of them ), so a missed cascade is rebuilt with the others.
// S4: a scroll whose base build is not in the map is drawn as a whole window by the backend itself
// ( ShadowCacheDrawStaticMaps, the heal ), so a lost frame that scrolled reports no miss when the next op of its
// cascade the backend executes scrolls too. When that op reuses the map ( S4c: the usual case ), its copy misses
// and every cascade is drawn again.
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
		s.totalSunRebuilt = 0;
		s.totalSunScrolled = 0;
		s.totalSunRepaired = 0;
		s.totalSunAdded = 0;
		s.totalSunLost = 0;
		s.totalSunGathers = 0;
		s.totalEvicted = 0;
		s.changes.total = 0;
		s.changesShown = 0;
		s.atlasCells = 0;
		s.cellsUsed = 0;
		memset( s.cellOwner, 0, sizeof( s.cellOwner ) );
		memset( s.recs, 0, sizeof( s.recs ) );
		memset( &s.sun, 0, sizeof( s.sun ) );
	}

	// the cache atlas may have been created again at another size ( r_shadowCacheSize, an image reload ): no
	// place handed out in the old one stands. ( This thread may read the image: it is written only between
	// two frames, by the engine's start, a reloadImages or R_ShadowCachePublish )
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
	s.sunScrolled = 0;
	s.sunRepaired = 0;
	s.sunAdded = 0;
	s.lost = 0;
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
	int		staticDrawn;		// S5: static casters drawn into static maps this frame: once per side built in full, and per rectangle
								// and addition of a patched cascade ( the backend's heals not counted )
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
up to date ( S4: in toroidal storage, so a window that moved by less than a map draws only the
strips it gained; S5: a change is repaired where it reaches, and entities that turned static are
drawn on top ). A local light is drawn through the cache when its record's maps are usable or may be
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

	// S4: no contacted-light cap any more ( F1 removed it, D78 as amended ): a chain gathered in full holds every
	// caster of the light, so S3b's walk over the view's entities, which looked for an entity past the cap, is gone

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
		if( touched && rec->valid && rec->epoch == s.epoch && rec->size != 0 )
		{
			s.lost++;		// S5: what changed is not known: the maps are drawn again ( a record without maps lost nothing )
		}
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

		// can a map of this light be drawn this frame? Only from a chain gathered in full, not while the
		// light waits for a place, and not while its pick has yet to hold
		const bool mayBuild = vLight->shadowFullGather && tr.frameCount >= rec->retryFrame && !held;
		if( usable && rec->wantSince == 0 )
		{
			pick.decision = SCD_REUSE;
		}
		else if( !mayBuild )
		{
			pick.decision = usable ? SCD_REUSE : SCD_UNCACHED;
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

	// the primary sun: cascade i lives in cascade map i, kept while its window is the one its map was drawn with.
	// S4: in toroidal storage. A window that moved by less than a map keeps its map and draws the strips it gained; a
	// map is drawn in full when its grid changed ( size, texel, clamps ), when its window jumped by a map or more, or
	// when the membership must be brought up to date
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
		// S4 ( D43 ): the cascades are the size the engine picks ( shadowLOD, after the atlas plan ). Maps of another
		// size are not shown, and maps of a new size are built only when the pick has held, so a pick that hovers
		// between two sizes ( the atlas plan near its capacity, r_shadowMapLodBias ) costs no build
		const bool wasRecent = sun.lastUsedFrame > tr.frameCount - SHADOWCACHE_PICK_HOLD_FRAMES;		// a cache candidate within the hold
		sun.lastUsedFrame = tr.frameCount;
		if( vLight->shadowFullGather )
		{
			s.totalSunGathers++;		// S5: for r_shadowCacheLog
		}
		if( sun.pick != size )
		{
			sun.pick = size;
			// as a local light's: the hold is for a pick that hovers while the sun is a candidate; a sun that comes back
			// at another size after an absence as long as the hold has served it
			sun.pickFrame = wasRecent ? tr.frameCount : tr.frameCount - SHADOWCACHE_PICK_HOLD_FRAMES;
		}

		// the membership: must every cascade be drawn again? S5: not for a change. Each unseen change whose old bounds touch
		// the light is repaired, below, in every cascade its bounds reach; only a history that has run out ( what changed is
		// not known ) draws every cascade again. The changes count as seen only once this frame's ops are emitted: a frame
		// that cannot draw leaves them for the next
		int begin, end;
		const bool lost = !R_ShadowCacheChangesSince( s.changes, sun.changesSeen, begin, end );
		// A door needs no more than its footprint: a parallel light has no area ( R_CreateLightRefs sets its areaNum to -1 ),
		// so the connected-area culls of R_AddSingleLight never apply to the sun, and a portal that opens or closes
		// changes no caster of its chain ( a local light's record keeps a door signature for that, the sun needs none )
		const bool all = !reuse || lost || !sun.valid || sun.epoch != s.epoch || sun.numCascades != sides;
		// S5: entities that turned static since the membership was set ( R_ShadowCacheCasterBits saw them wait ) join it
		// this frame and are drawn on top of every map that is not drawn in full ( SCOP_ADD )
		const bool adds = !all && sun.wantSince != 0;

		// S4 ( D43 ): good maps of another size wait while the pick has yet to hold. The sun is drawn without the
		// cache meanwhile and keeps its maps and its stored view: should the pick come back to their size, they
		// scroll on to the window of the moment; once it has held, they are drawn in full at the new size
		if( !all && sun.cascades[0].valid && sun.cascades[0].res != size && tr.frameCount - sun.pickFrame < SHADOWCACHE_PICK_HOLD_FRAMES )
		{
			continue;
		}

		// S4: what each cascade needs: its map as it is, the strips its window gained, or a build in full. S5: a patch is a
		// map kept, moved or not, with the strips its window gained, its repairs and the entities that turned static
		enum { SUN_REUSE, SUN_PATCH, SUN_REBUILD };
		int need[SHADOWCACHE_MAX_CASCADES];
		int scrollX[SHADOWCACHE_MAX_CASCADES];
		int scrollY[SHADOWCACHE_MAX_CASCADES];
		for( int i = 0; i < sides; i++ )
		{
			const shadowCacheCascade_t& c = scv->cascades[i];
			const shadowCacheSunCascadeRec_t& crec = sun.cascades[i];
			// a clamped axis shows the light's own box, which does not move: nothing scrolls along it
			const int64 dx = c.clampedX ? 0 : c.originX - crec.originX;
			const int64 dy = c.clampedY ? 0 : c.originY - crec.originY;
			const bool sameGrid = !all && crec.valid && crec.res == c.res && crec.texelWorld == c.texelWorld
								  && crec.clampedX == c.clampedX && crec.clampedY == c.clampedY;
			scrollX[i] = 0;
			scrollY[i] = 0;
			if( !sameGrid || dx <= -c.res || dx >= c.res || dy <= -c.res || dy >= c.res )
			{
				need[i] = SUN_REBUILD;
			}
			else if( dx == 0 && dy == 0 )
			{
				need[i] = SUN_REUSE;
			}
			else if( 2 * ( ( int64 )c.res - ( ( dx < 0 ) ? -dx : dx ) ) * ( ( int64 )c.res - ( ( dy < 0 ) ? -dy : dy ) ) < ( int64 )c.res * c.res )
			{
				// more than half the map would be strips: one build in full costs less ( one clear and one pass over the
				// chain, where a scroll draws each strip in up to four pieces and sends a caster once per piece )
				need[i] = SUN_REBUILD;
			}
			else
			{
				need[i] = SUN_PATCH;
				scrollX[i] = ( int )dx;
				scrollY[i] = ( int )dy;
			}
		}

		// S5: the rectangles each patched cascade clears and draws again: the strips its window gained, then the repairs. A
		// change whose old bounds touch the light is repaired in every cascade that is not drawn in full and that its bounds
		// reach, in this frame's window ( after the move: the backend repairs the map as this frame's copy reads it ); every
		// static caster that reaches a rectangle is drawn into it again ( the strip rule, R_ShadowCacheCasterBits ). Merged
		// where they overlap, at most SHADOWCACHE_MAX_RECTS a cascade; a cascade they would cover more than half of is drawn
		// in full instead, as a scroll is
		shadowCacheRect_t rects[SHADOWCACHE_MAX_CASCADES][SHADOWCACHE_MAX_RECTS];
		int numRects[SHADOWCACHE_MAX_CASCADES];
		bool repaired[SHADOWCACHE_MAX_CASCADES];
		shadowCacheRect_t strips[SHADOWCACHE_MAX_CASCADES][2];		// S5: the strips alone, for the ops' repairs bits
		int numStrips[SHADOWCACHE_MAX_CASCADES];
		for( int i = 0; i < sides; i++ )
		{
			numRects[i] = ( need[i] == SUN_PATCH ) ? R_ShadowCacheExposedRects( scrollX[i], scrollY[i], scv->cascades[i].res, rects[i] ) : 0;
			repaired[i] = false;
			numStrips[i] = numRects[i];
			for( int r = 0; r < numRects[i]; r++ )
			{
				strips[i][r] = rects[i][r];
			}
		}
		for( int n = begin; n < end && !all; n++ )
		{
			const idBounds& oldBounds = s.changes.oldBounds[n & ( SHADOWCACHE_MAX_CHANGES - 1 )];
			if( !light->globalLightBounds.IntersectsBounds( oldBounds ) )
			{
				continue;
			}
			for( int i = 0; i < sides; i++ )
			{
				shadowCacheRect_t r;
				if( need[i] != SUN_REBUILD && R_ShadowCacheFootprint( *scv, scv->cascades[i], viewDef->renderOrigin, oldBounds, r ) )
				{
					numRects[i] = R_ShadowCacheAddRect( rects[i], numRects[i], SHADOWCACHE_MAX_RECTS, r );
					repaired[i] = true;
				}
			}
		}
		bool anyDraw = false;
		for( int i = 0; i < sides; i++ )
		{
			if( need[i] != SUN_REBUILD )
			{
				int64 area = 0;
				for( int r = 0; r < numRects[i]; r++ )
				{
					area += ( int64 )( rects[i][r].x1 - rects[i][r].x0 ) * ( rects[i][r].y1 - rects[i][r].y0 );
				}
				const int64 res = scv->cascades[i].res;
				if( 2 * area > res * res )
				{
					need[i] = SUN_REBUILD;		// more than half the map: one build in full costs less
				}
				else if( need[i] == SUN_REUSE && ( numRects[i] > 0 || adds ) )
				{
					need[i] = SUN_PATCH;		// kept in place: its repairs, or the entities that turned static
				}
			}
			anyDraw = anyDraw || need[i] != SUN_REUSE;
		}
		if( anyDraw && !vLight->shadowFullGather )
		{
			// the chain does not hold every static caster, so no map can be drawn this frame: the sun is drawn without the
			// cache ( S1's path ). Nothing counts as done: the changes stay unseen, the entities that turned static still
			// wait, the cascades' records stay, and the next frame's light job gathers in full because it is asked to
			// ( S5; until S5 a map to be built in full was marked invalid for that, which would now draw every cascade
			// again for one repair grown past half a map )
			// the ask holds for the next frame only ( the local lights' lastUsedFrame rule ): a sun that then leaves the
			// cache, waits in the hold or keeps an unusable view stops asking; one that still has a map to draw asks again
			sun.gatherAskedFrame = tr.frameCount;
			continue;
		}
		sun.gatherAskedFrame = 0;
		sun.changesSeen = end;
		if( all )
		{
			sun.heldThrough = tr.frameCount;
			sun.epoch = s.epoch;
			sun.wantSince = 0;
			sun.numCascades = sides;
			if( lost && sun.valid )		// sun.valid is still the old record's here: a sun without maps lost nothing
			{
				s.totalSunLost++;
				s.lost++;
			}
		}
		else if( adds )
		{
			// S5: the membership grows to every entity that is static now. R_ShadowCacheCasterBits tells the casters that join
			// it ( in the maps by the new stamp, not by the old one ) and marks them in each patched cascade's strip bits
			sun.addedFrom = sun.heldThrough;
			sun.addedFrame = tr.frameCount;
			sun.heldThrough = tr.frameCount;
			sun.wantSince = 0;
		}
		sun.valid = true;
		sun.viewOrigin = viewDef->renderView.vieworg;
		sun.viewAxis = viewDef->renderView.viewaxis;
		sun.fovX = viewDef->renderView.fov_x;
		sun.fovY = viewDef->renderView.fov_y;
		sun.gridX = -scv->sunAxis[1];		// S5: as R_ShadowCacheLightFrame has them
		sun.gridY = -scv->sunAxis[2];

		vLight->shadowOp = numEmitted + 1;
		for( int i = 0; i < sides; i++ )
		{
			shadowCacheSunCascadeRec_t& crec = sun.cascades[i];
			const shadowCacheCascade_t& c = scv->cascades[i];
			shadowCacheOp_t& op = ops[numEmitted++];
			memset( &op, 0, sizeof( op ) );
			if( need[i] == SUN_REBUILD )
			{
				crec.originX = c.originX;
				crec.originY = c.originY;
				crec.texelWorld = c.texelWorld;
				crec.res = c.res;
				crec.clampedX = c.clampedX;
				crec.clampedY = c.clampedY;
				crec.baseX = c.originX;		// S4: a map drawn in full has no wrap
				crec.baseY = c.originY;
				crec.serial = ++s.nextSerial;
				crec.valid = true;
				op.flags = SCOP_REBUILD;
				s.sunRebuilt++;
				s.totalSunRebuilt++;
			}
			else if( need[i] == SUN_PATCH )
			{
				// S4: the strips the window gained, S5: and the repairs, cleared and drawn on top of the map's last build ( the
				// backend checks that it is there, and draws the whole window when it is not ); S5: then the entities that
				// turned static, on top of the whole window
				op.baseSerial = crec.serial;
				op.numRects = numRects[i];
				for( int r = 0; r < numRects[i]; r++ )
				{
					op.rects[r] = rects[i][r];
					// S5: a rectangle that is not one of the strips holds a repair ( merged into a strip or not ): for the
					// debug view's colours only
					bool strip = false;
					for( int k = 0; k < numStrips[i]; k++ )
					{
						strip = strip || ( rects[i][r].x0 == strips[i][k].x0 && rects[i][r].y0 == strips[i][k].y0
										   && rects[i][r].x1 == strips[i][k].x1 && rects[i][r].y1 == strips[i][k].y1 );
					}
					op.repairs |= strip ? 0 : ( 1 << r );
				}
				crec.originX = c.originX;
				crec.originY = c.originY;
				crec.serial = ++s.nextSerial;
				op.flags = ( short )( SCOP_SCROLL | ( adds ? SCOP_ADD : 0 ) );
				if( scrollX[i] != 0 || scrollY[i] != 0 )
				{
					s.sunScrolled++;
					s.totalSunScrolled++;
				}
				if( repaired[i] )
				{
					s.sunRepaired++;
					s.totalSunRepaired++;
				}
				if( adds )
				{
					s.sunAdded++;
					s.totalSunAdded++;
				}
			}
			else
			{
				s.sunReused++;
			}
			op.serial = crec.serial;
			op.target = ( short )i;
			op.size = ( short )size;
			op.x = 0;
			op.y = 0;
			// S4: where the window's logical texel ( 0, 0 ) lives in the map; a clamped axis shows the light's box and never wraps
			op.wrap[0] = ( short )( crec.clampedX ? 0 : R_ShadowCacheWrapCoord( crec.originX, crec.baseX, size ) );
			op.wrap[1] = ( short )( crec.clampedY ? 0 : R_ShadowCacheWrapCoord( crec.originY, crec.baseY, size ) );
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
			memset( &op, 0, sizeof( op ) );		// S4: no wrap, no strips ( not left to the frame allocator's zero fill )
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
S4: for each cascade of the sun that scrolls this frame, one more byte per surface ( the op's
stripBits ): bit r = a static caster of the cascade whose box reaches the op's strip r, two
texels wider on every side as the cascade's own test is ( the strip rule, 01 section 5: every
static that overlaps the strip, not only those new to the window ). S5: the same for the repairs,
the other rectangles of a patched cascade's op; and with SCOP_ADD, SCB_ADDED marks the casters
of entities that join the sun's maps this frame ( in them by the new membership stamp, not by
the old one ), drawn on top of the whole window.
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

	// does the backend draw a static map of this light this frame? ( any of its ops is a rebuild, or S4 a scroll )
	bool rebuilds = false;
	byte builtSides = 0;		// S5: the sides drawn in full, for the HUD's count of static casters drawn
	// S4: the strips of the sun's cascades that scroll this frame, in the sun's light view: x0, x1, y0, y1, two texels
	// wider on every side, as the cascade's own test below is. Columns run along the light view's x from 'left'; rows
	// run DOWN its y from 'top'. S5: every rectangle of a patched cascade's op, strips and repairs
	byte* stripBits[SHADOWCACHE_MAX_CASCADES] = {};
	int stripRects[SHADOWCACHE_MAX_CASCADES] = {};
	double stripBox[SHADOWCACHE_MAX_CASCADES][SHADOWCACHE_MAX_RECTS][4];
	// S5: the casters that join the sun's maps this frame ( R_ShadowCacheEmitOps moved the membership stamp and kept the old
	// one ): in the maps by the new stamp and not by the old; marked SCB_ADDED for each cascade whose op adds
	const bool sunAdds = sun && byRecord && shadowCacheState.sun.addedFrame == tr.frameCount;
	const int addedFrom = shadowCacheState.sun.addedFrom;
	bool stripAdds[SHADOWCACHE_MAX_CASCADES] = {};
	if( cached )
	{
		shadowCacheOp_t* ops = &viewDef->shadowOps[vLight->shadowOp - 1];
		const int numSides = R_ShadowCacheNumSides( vLight, viewDef->shadowSkipSet );
		for( int i = 0; i < numSides; i++ )
		{
			rebuilds = rebuilds || ( ops[i].flags & ( SCOP_REBUILD | SCOP_SCROLL ) ) != 0;
			if( ( ops[i].flags & SCOP_REBUILD ) != 0 )
			{
				builtSides |= ( byte )( 1 << i );
			}
			if( sun && ( ops[i].flags & SCOP_SCROLL ) != 0 && i < scv->numCascades )
			{
				stripBits[i] = ( byte* )R_FrameAlloc( numSurfs );		// zero-filled: a surface that reaches no strip
				stripRects[i] = ops[i].numRects;
				stripAdds[i] = sunAdds && ( ops[i].flags & SCOP_ADD ) != 0;
				assert( ops[i].numRects >= 0 && ops[i].numRects <= SHADOWCACHE_MAX_RECTS );
				ops[i].stripBits = stripBits[i];
				const shadowCacheCascade_t& cascade = scv->cascades[i];
				const double texelX = ( cascade.right - cascade.left ) / ( double )cascade.res;
				const double texelY = ( cascade.top - cascade.bottom ) / ( double )cascade.res;
				for( int r = 0; r < ops[i].numRects; r++ )
				{
					const shadowCacheRect_t& rect = ops[i].rects[r];
					stripBox[i][r][0] = cascade.left + ( double )( rect.x0 - 2 ) * texelX;
					stripBox[i][r][1] = cascade.left + ( double )( rect.x1 + 2 ) * texelX;
					stripBox[i][r][2] = cascade.top - ( double )( rect.y1 + 2 ) * texelY;
					stripBox[i][r][3] = cascade.top - ( double )( rect.y0 - 2 ) * texelY;
				}
			}
		}
	}

	bool waits = false;				// a caster turned static after the maps were built: they want an update
	bool lastSpaceWaits = false;
	bool lastSpaceAdded = false;	// S5: its entity joins the sun's maps this frame
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
			lastSpaceAdded = false;
			if( lastSpaceCacheable && byRecord )
			{
				// in the maps: unchanged for the static interval when the maps were last built
				const int lastChanged = R_ShadowCacheLastChanged( def );
				lastSpaceCacheable = R_ShadowCacheInUnit( lastChanged, staticFrames, heldThrough );
				lastSpaceWaits = !lastSpaceCacheable && R_ShadowCacheIsStatic( tr.frameCount, lastChanged, staticFrames );
				lastSpaceAdded = sunAdds && lastSpaceCacheable && !R_ShadowCacheInUnit( lastChanged, staticFrames, addedFrom );
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

		// S4: a static caster of a scrolling cascade is drawn into each of its strips that its box reaches. S5: and into each
		// repair; one whose entity joins the maps this frame is also drawn over the whole window ( SCB_ADDED )
		if( sun && isStatic )
		{
			for( int c = 0; c < scv->numCascades; c++ )
			{
				if( stripBits[c] == NULL || ( ( reach >> c ) & 1 ) == 0 )
				{
					continue;
				}
				byte strips = 0;
				for( int r = 0; r < stripRects[c]; r++ )
				{
					const double* strip = stripBox[c][r];
					if( viewMax[0] >= strip[0] && viewMin[0] <= strip[1] && viewMax[1] >= strip[2] && viewMin[1] <= strip[3] )
					{
						strips |= ( byte )( 1 << r );
					}
				}
				if( lastSpaceAdded && stripAdds[c] )
				{
					strips |= ( byte )SCB_ADDED;
				}
				// with SCB_STATIC, as the pass's masks ask: a caster past the end of the chain's bits, which the pass reads
				// as SCB_SIDE_MASK ( no static bit ), then reaches no strip, as it reaches no static map in a build in full
				stripBits[c][index] = ( strips != 0 ) ? ( byte )( SCB_STATIC | strips ) : 0;
				// S5: one draw per rectangle it reaches and one for an addition
				for( int k = 0; k < 8; k++ )
				{
					shadowCachePlan.staticDrawn += ( k != 6 && ( ( strips >> k ) & 1 ) != 0 ) ? 1 : 0;
				}
			}
		}
		if( isStatic )
		{
			// S5: the sides it is drawn into in full ( reach's static bit is not a side: the mask removes it )
			const byte built = ( byte )( reach & builtSides & SCB_SIDE_MASK );
			for( int k = 0; k < 6; k++ )
			{
				shadowCachePlan.staticDrawn += ( built >> k ) & 1;
			}
		}

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
		// the backend disagree with itself; without a plan and bits it then simply draws as it did before S2a.
		// Nor is there anything to plan without the cache images: they get their textures only at mode 2
		// ( Image_intrinsic.cpp ), from R_ShadowCachePublish, between two frames. A view that finds none ( the
		// mode was raised inside this frame, after the publish ) is drawn without the cache; the next has them
		if( !skipSet.skipShadows && skipSet.useShadowAtlas && globalImages->shadowCacheAtlasImage->IsLoaded() )
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

	// S5: the HUD's decision line. Each cached unit of the view by its op, and the units of the lights drawn without the cache
	// ( N15: counted for the main view; reused + patched + rebuilt = the cached units above )
	int unitsReused = 0;
	int unitsPatched = 0;
	int unitsRebuilt = 0;
	int unitsUncached = 0;
	for( const viewLight_t* vLight = viewDef->viewLights; vLight != NULL; vLight = vLight->next )
	{
		if( !R_ShadowCacheLightHasUnits( vLight ) )
		{
			continue;
		}
		const int sides = R_ShadowCacheNumSides( vLight, viewDef->shadowSkipSet );
		if( vLight->shadowOp == 0 )
		{
			unitsUncached += sides;
			continue;
		}
		const shadowCacheOp_t* ops = &viewDef->shadowOps[vLight->shadowOp - 1];
		for( int i = 0; i < sides; i++ )
		{
			if( ( ops[i].flags & SCOP_REBUILD ) != 0 )
			{
				unitsRebuilt++;
			}
			else if( ( ops[i].flags & SCOP_SCROLL ) != 0 )
			{
				unitsPatched++;
			}
			else
			{
				unitsReused++;
			}
		}
	}
	tr.pc.c_shadowCacheUnitsReused = unitsReused;
	tr.pc.c_shadowCacheUnitsPatched = unitsPatched;
	tr.pc.c_shadowCacheUnitsRebuilt = unitsRebuilt;
	tr.pc.c_shadowCacheUnitsUncached = unitsUncached;
	tr.pc.c_shadowCacheCastersDrawn = shadowCachePlan.staticDrawn;
	tr.pc.c_shadowCacheChanges = shadowCacheState.changes.total - shadowCacheState.changesShown;
	tr.pc.c_shadowCacheLost = shadowCacheState.lost;
	tr.pc.c_shadowCacheBudgetUsed = shadowCacheState.costBuilt;
	tr.pc.c_shadowCacheBudget = r_shadowCacheBudget.GetInteger();
	shadowCacheState.changesShown = shadowCacheState.changes.total;

	if( r_shadowCacheLog.GetBool() && ( tr.frameCount & 63 ) == 0 )
	{
		common->Printf( "shadowCache: frame %d plan demoted %d dropped %d casters %d sides %d of %d ops %d uncached %d static %d active %d\n",
						tr.frameCount, shadowCachePlan.demoted, shadowCachePlan.dropped, shadowCachePlan.casters, shadowCachePlan.casterSides, shadowCachePlan.casterSidesAll,
						shadowCachePlan.ops, shadowCachePlan.uncachedLights, shadowCachePlan.staticSides, shadowCachePlan.activeSides );
		common->Printf( "shadowCache: frame %d lights reused %d rebuilt %d repaired %d waiting %d moving %d sun reused %d rebuilt %d scrolled %d | totals rebuilt %d repaired %d evicted %d changes %d epoch %d misses %d heals %d sun rebuilt %d scrolled %d\n",
						tr.frameCount, shadowCacheState.reused, shadowCacheState.rebuilt, shadowCacheState.repaired, shadowCacheState.waiting, shadowCacheState.moving,
						shadowCacheState.sunReused, shadowCacheState.sunRebuilt, shadowCacheState.sunScrolled,
						shadowCacheState.totalRebuilt, shadowCacheState.totalRepaired, shadowCacheState.totalEvicted, shadowCacheState.changes.total,
						shadowCacheState.epoch, ( int )shadowCacheState.missesSeen, ( int )shadowCacheState.healsPublished,
						shadowCacheState.totalSunRebuilt, shadowCacheState.totalSunScrolled );
		common->Printf( "shadowCache: frame %d places evicted %d turns %d cost %d cells %d of %d\n",
						tr.frameCount, shadowCacheState.evicted, shadowCacheState.turns, shadowCacheState.costBuilt, shadowCacheState.cellsUsed, shadowCacheState.atlasCells );
		// S5: the sun's repairs and additions; the totals since the map began
		common->Printf( "shadowCache: frame %d sun repaired %d added %d gather asked %d | totals sun repaired %d added %d lost %d gathers %d\n",
						tr.frameCount, shadowCacheState.sunRepaired, shadowCacheState.sunAdded, ( int )( shadowCacheState.sun.gatherAskedFrame == tr.frameCount ),
						shadowCacheState.totalSunRepaired, shadowCacheState.totalSunAdded, shadowCacheState.totalSunLost, shadowCacheState.totalSunGathers );
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
	shadowCacheState.healsPublished = backEnd.shadowCacheHeals;		// S4: for r_shadowCacheLog only
	// The cache images get their textures here when the mode is 2 and they have none ( the mode was raised, or
	// a reloadImages below mode 2 had given them back ): on the thread that makes every texture and between two
	// frames, so the frontend never reads an image while it is written. A render target is allocated without a
	// command list. Each of the six is asked: a debug bind ( testImage ) can have made one alone. The backend's
	// build stamps need no clearing: they are zero at the engine's start, and the reload that gives the images
	// back clears them ( ClearCaches )
	if( r_shadowCache.GetInteger() >= 2 )
	{
		int made = 0;
		const uint64 start = Sys_Microseconds();
		for( int i = -1; i < 5; i++ )
		{
			idImage* image = ( i < 0 ) ? globalImages->shadowCacheAtlasImage : globalImages->shadowCacheCascadeImage[i];
			if( !image->IsLoaded() )
			{
				image->ActuallyLoadImage( false, NULL );
				made++;
			}
		}
		if( made > 0 && r_shadowCacheLog.GetInteger() > 0 )
		{
			common->Printf( "shadowCache: cache images made ( %d of 6; an atlas of %d, cascade maps of %d ) in %d microseconds\n", made,
							globalImages->shadowCacheAtlasImage->GetUploadWidth(), globalImages->shadowCacheCascadeImage[0]->GetUploadWidth(), ( int )( Sys_Microseconds() - start ) );
		}
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

// The shadow cache's debug-tools overlay; X0 lands it as a no-op and track S fills the body. S5: r_shadowCacheDebug, drawn by
// the backend's member, which holds the marks ( this runs on the backend's thread, inside its debug tools )
void R_ShadowCacheDebugTools( nvrhi::ICommandList* commandList, const viewDef_t* viewDef )
{
	if( r_shadowCacheDebug.GetInteger() > 0 )
	{
		backEnd.ShadowCacheDebugDraw();
	}
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
again ( D34 ). S4: a clear may cover any rectangle, clearRect, in place of the square ( a piece
of a cascade's strip ).
=============
*/
void idRenderBackend::ShadowCacheCopyDraw( nvrhi::IFramebuffer* target, int targetKind, nvrhi::ITexture* source, const shadowCacheCopyConstants_t& constants, const shadowCacheRect_t* clearRect )
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

	assert( clearRect == NULL || constants.res == 0 );		// a rectangle that is not the tile is only ever cleared
	const nvrhi::Viewport viewport = ( clearRect != NULL )
									 ? nvrhi::Viewport( ( float )clearRect->x0, ( float )clearRect->x1, ( float )clearRect->y0, ( float )clearRect->y1, 0.0f, 1.0f )
									 : nvrhi::Viewport( ( float )constants.tileOrigin[0], ( float )( constants.tileOrigin[0] + constants.tileRes ),
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
	//puncho - reconcile: direct NVRHI draws bypass DrawElementsWithCounters.
	if( concurrencyStats )
	{
		pc.passDraws[currentPass]++;
	}
}

// S5: what drew a place of a cache image ( idRenderBackend::shadowCacheMark_t::kind ), and its colour in r_shadowCacheDebug
enum shadowCacheDebugKind_t
{
	SCDM_BUILT,			// a build in full: blue
	SCDM_STRIP,			// a strip a cascade window gained: yellow
	SCDM_REPAIR,		// a rectangle that holds a repair: green
	SCDM_ADDED,			// casters drawn on top of the whole map: a magenta frame inside the panel's edge
	SCDM_HEALED,		// a scroll drawn as a whole window: cyan
	SCDM_NUM_KINDS
};

#define SHADOWCACHE_DEBUG_FADE		120		// S5: main views a mark stays on r_shadowCacheDebug's panels, fading

/*
=============
idRenderBackend::ShadowCacheDebugDraw

r_shadowCacheDebug ( S5; 08 section 11: "show where texels came from" ). The main view's cache maps as the cache stores
them, in panels along the bottom of the screen: 1 the primary sun's cascade maps, at the size this view's ops have them,
2 also the cache atlas of the local lights, whole. Over them the marks of the last SHADOWCACHE_DEBUG_FADE main views,
fading: the places ShadowCacheDrawStaticMaps drew into, coloured by what drew them ( shadowCacheDebugKind_t ); a white
cross where a cascade window's logical texel 0 lies in its map ( toroidal storage, S4 ); this frame's reused units of the
atlas framed green. A screenshot draws a fresh frame, whose ops mostly reuse: the fading marks show what came before.
Called from R_ShadowCacheDebugTools inside the debug tools; sets its own state, as they do.
=============
*/
void idRenderBackend::ShadowCacheDebugDraw()
{
	const int mode = r_shadowCacheDebug.GetInteger();
	if( mode <= 0 || viewDef == NULL || !viewDef->isMainView || viewDef->isSubview || renderSystem->GetWidth() <= 0 || renderSystem->GetHeight() <= 0 )
	{
		return;
	}

	struct panel_t
	{
		idImage*	image;
		int			target;		// as a mark's: -1 the cache atlas, 0..4 a cascade map
		int			res;		// the texels the panel shows, from the image's corner ( 0, 0 )
		float		x;			// the panel on the screen, in 0..1 with y down
		float		y;
		float		w;
		float		h;
		int			wrap[2];	// a cascade map's logical texel 0; -1 = none
	};
	panel_t panels[SHADOWCACHE_MAX_CASCADES + 1];
	int numPanels = 0;
	const float aspect = ( float )renderSystem->GetHeight() / ( float )renderSystem->GetWidth();		// panels square in pixels

	// the sun's cascade maps, at the size this view's ops have them
	for( const viewLight_t* vLight = viewDef->viewLights; vLight != NULL; vLight = vLight->next )
	{
		if( !vLight->isPrimarySun || !vLight->parallel || vLight->shadowOp == 0 )
		{
			continue;
		}
		const shadowCacheOp_t* ops = &viewDef->shadowOps[vLight->shadowOp - 1];
		const int numCascades = Min( viewDef->shadowSkipSet.shadowMapSplits + 1, ( int )SHADOWCACHE_MAX_CASCADES );
		for( int i = 0; i < numCascades; i++ )
		{
			if( ops[i].target < 0 || ops[i].target >= SHADOWCACHE_MAX_CASCADES || ops[i].size <= 0 )
			{
				continue;
			}
			panel_t& p = panels[numPanels++];
			p.image = globalImages->shadowCacheCascadeImage[ops[i].target];
			p.target = ops[i].target;
			p.res = ops[i].size;
			p.h = 0.19f;
			p.w = p.h * aspect;
			p.x = 0.01f + ( float )i * ( p.w + 0.005f );
			p.y = 0.8f;
			p.wrap[0] = ops[i].wrap[0];
			p.wrap[1] = ops[i].wrap[1];
		}
		break;
	}
	// 2: the cache atlas, whole
	if( mode >= 2 && r_shadowCache.GetInteger() >= 2 && globalImages->shadowCacheAtlasImage->IsLoaded() )		// below 2 the atlas is stale
	{
		panel_t& p = panels[numPanels++];
		p.image = globalImages->shadowCacheAtlasImage;
		p.target = -1;
		p.res = p.image->GetUploadWidth();
		p.h = 0.36f;
		p.w = p.h * aspect;
		p.x = 0.99f - p.w;
		p.y = 0.63f;
		p.wrap[0] = -1;
		p.wrap[1] = -1;
	}

	// the unit quad ( testImageSurface: x, y and s, t in 0..1 ) over x..x + w, y..y + h of the screen ( 0..1, y down ), with the
	// texture coordinates s0..s1, t0..t1
	auto quad = [this]( float x, float y, float w, float h, float s0, float t0, float s1, float t1 )
	{
		float texS[4] = { s1 - s0, 0.0f, 0.0f, s0 };
		float texT[4] = { 0.0f, t1 - t0, 0.0f, t0 };
		renderProgManager.SetRenderParm( RENDERPARM_TEXTUREMATRIX_S, texS );
		renderProgManager.SetRenderParm( RENDERPARM_TEXTUREMATRIX_T, texT );
		const idRenderMatrix mvp(
			2.0f * w, 0.0f, 0.0f, 2.0f * x - 1.0f,
			0.0f, -2.0f * h, 0.0f, 1.0f - 2.0f * y,
			0.0f, 0.0f, 0.0f, 0.5f,
			0.0f, 0.0f, 0.0f, 1.0f );
		RB_SetMVP( mvp );
		DrawElementsWithCounters( &testImageSurface );
	};
	// a frame two pixels wide just inside x..x + w, y..y + h
	const float lineX = 2.0f / ( float )renderSystem->GetWidth();
	const float lineY = 2.0f / ( float )renderSystem->GetHeight();
	auto frame = [&quad, lineX, lineY]( float x, float y, float w, float h )
	{
		quad( x, y, w, lineY, 0.0f, 0.0f, 0.0f, 0.0f );
		quad( x, y + h - lineY, w, lineY, 0.0f, 0.0f, 0.0f, 0.0f );
		quad( x, y, lineX, h, 0.0f, 0.0f, 0.0f, 0.0f );
		quad( x + w - lineX, y, lineX, h, 0.0f, 0.0f, 0.0f, 0.0f );
	};

	renderProgManager.SetRenderParm( RENDERPARM_ALPHA_TEST, vec4_zero.ToFloatPtr() );
	float texGenEnabled[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	renderProgManager.SetRenderParm( RENDERPARM_TEXGEN_0_ENABLED, texGenEnabled );
	RB_SetVertexColorParms( SVC_IGNORE );

	// the maps as the cache stores them. A depth image samples as ( depth, 0, 0 ): far is bright red. Drawn at half
	// strength so that the marks over them stand out. Only on Vulkan: a depth image carries a comparison sampler, which a
	// plain Sample may not use on D3D12 ( O2-1 ); there a panel is grey and the marks are drawn over it
	const bool sampleMaps = deviceManager->GetGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN;
	GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO | GLS_DEPTHMASK | GLS_DEPTHFUNC_ALWAYS | GLS_CULL_TWOSIDED );
	for( int n = 0; n < numPanels; n++ )
	{
		const panel_t& p = panels[n];
		if( p.image == NULL || !p.image->IsLoaded() || p.res <= 0 )
		{
			continue;
		}
		if( !sampleMaps )
		{
			renderProgManager.BindShader_Color();
			GL_Color( 0.15f, 0.15f, 0.15f, 1.0f );
			quad( p.x, p.y, p.w, p.h, 0.0f, 0.0f, 0.0f, 0.0f );
			continue;
		}
		GL_Color( 0.5f, 0.5f, 0.5f, 1.0f );
		const float side = ( float )p.image->GetUploadWidth();
		GL_SelectTexture( 0 );
		p.image->Bind();
		renderProgManager.BindShader_TextureVertexColor();
		quad( p.x, p.y, p.w, p.h, 0.0f, 0.0f, ( float )p.res / side, ( float )p.res / side );
	}

	// over them, the marks, fading; this frame's reused units of the atlas; a cascade's first texel
	GL_State( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHMASK | GLS_DEPTHFUNC_ALWAYS | GLS_CULL_TWOSIDED );
	renderProgManager.BindShader_Color();
	static const float colors[SCDM_NUM_KINDS][3] =
	{
		{ 0.2f, 0.4f, 1.0f },		// SCDM_BUILT
		{ 1.0f, 0.9f, 0.1f },		// SCDM_STRIP
		{ 0.15f, 1.0f, 0.25f },		// SCDM_REPAIR ( not red: the maps are red )
		{ 1.0f, 0.2f, 1.0f },		// SCDM_ADDED
		{ 0.1f, 1.0f, 1.0f }		// SCDM_HEALED
	};
	const unsigned int first = ( shadowCacheNumMarks > ( unsigned int )SHADOWCACHE_NUM_MARKS ) ? shadowCacheNumMarks - SHADOWCACHE_NUM_MARKS : 0;
	for( int n = 0; n < numPanels; n++ )
	{
		const panel_t& p = panels[n];
		if( p.res <= 0 )
		{
			continue;
		}
		const float sx = p.w / ( float )p.res;
		const float sy = p.h / ( float )p.res;
		for( unsigned int k = first; k < shadowCacheNumMarks; k++ )
		{
			const shadowCacheMark_t& m = shadowCacheMarks[k & ( SHADOWCACHE_NUM_MARKS - 1 )];
			const int age = shadowCacheDebugFrame - m.frame;
			if( m.target != p.target || age < 0 || age >= SHADOWCACHE_DEBUG_FADE || m.kind < 0 || m.kind >= SCDM_NUM_KINDS )
			{
				continue;
			}
			GL_Color( colors[m.kind][0], colors[m.kind][1], colors[m.kind][2], 0.5f * ( 1.0f - ( float )age / ( float )SHADOWCACHE_DEBUG_FADE ) );
			const float x = p.x + ( float )m.x0 * sx;
			const float y = p.y + ( float )m.y0 * sy;
			const float w = ( float )( m.x1 - m.x0 ) * sx;
			const float h = ( float )( m.y1 - m.y0 ) * sy;
			if( m.kind == SCDM_ADDED )
			{
				// an addition covers the whole window: its frame is drawn two lines inside the panel's edge
				frame( x + 2.0f * lineX, y + 2.0f * lineY, w - 4.0f * lineX, h - 4.0f * lineY );
			}
			else
			{
				quad( x, y, w, h, 0.0f, 0.0f, 0.0f, 0.0f );
			}
		}
		if( p.target < 0 )
		{
			// this frame's reused units of the cache atlas
			GL_Color( 0.2f, 1.0f, 0.3f, 0.8f );
			for( int i = 0; i < viewDef->numShadowOps; i++ )
			{
				const shadowCacheOp_t& op = viewDef->shadowOps[i];
				if( op.target == -1 && ( op.flags & ( SCOP_REBUILD | SCOP_SCROLL ) ) == 0 )
				{
					frame( p.x + ( float )op.x * sx, p.y + ( float )op.y * sy, ( float )op.size * sx, ( float )op.size * sy );
				}
			}
		}
		else if( p.wrap[0] >= 0 && p.wrap[1] >= 0 )
		{
			// where the window's logical texel 0 lies: the seams of toroidal storage
			GL_Color( 1.0f, 1.0f, 1.0f, 0.7f );
			quad( p.x + ( float )p.wrap[0] * sx, p.y, lineX, p.h, 0.0f, 0.0f, 0.0f, 0.0f );
			quad( p.x, p.y + ( float )p.wrap[1] * sy, p.w, lineY, 0.0f, 0.0f, 0.0f, 0.0f );
		}
	}

	shadowCacheDebugFrame++;		// the marks' clock: one tick per main view drawn with the view on
}

/*
=============
idRenderBackend::ShadowCacheDrawStaticMaps

The static maps of this view's cached units ( S2c ): each unit's region of its cache image is cleared and
its static casters are drawn into it, with the state ShadowMapPassFast draws any caster with. S3a: only
the ops flagged SCOP_REBUILD are drawn ( a reused unit's map is already in its cache image ); each region
drawn is stamped with the op's serial ( D74 ), the stamp the copy pass checks. Runs in its own main block,
MRB_SHADOW_CACHE, before the atlas block opens ( D70 ); every rebuild of the view precedes every copy.
S4: an op flagged SCOP_SCROLL clears and draws only the strips its cascade's window gained, in toroidal
storage, on top of the build it names; when that build is not in the map, the whole window instead.
S5: its rectangles are the strips and the repairs; with SCOP_ADD the casters that joined the static
maps this frame are then drawn over the whole window, without a clear. With r_shadowCacheDebug on,
every place drawn into is marked for the debug view.
=============
*/
void idRenderBackend::ShadowCacheDrawStaticMaps()
{
	// S5: r_shadowCacheDebug's marks, in the cache image's own texels
	const bool debugMarks = r_shadowCacheDebug.GetInteger() > 0;
	if( debugMarks && !shadowCacheDebugWasOn )
	{
		shadowCacheNumMarks = 0;		// marks made before the view went off would look fresh: their clock stood still
	}
	shadowCacheDebugWasOn = debugMarks;
	auto mark = [this, debugMarks]( int target, int kind, int x0, int y0, int x1, int y1 )
	{
		if( debugMarks )
		{
			shadowCacheMark_t& m = shadowCacheMarks[shadowCacheNumMarks++ & ( SHADOWCACHE_NUM_MARKS - 1 )];
			m.frame = shadowCacheDebugFrame;
			m.target = ( short )target;
			m.kind = ( short )kind;
			m.x0 = ( short )x0;
			m.y0 = ( short )y0;
			m.x1 = ( short )x1;
			m.y1 = ( short )y1;
		}
	};

	// S3a: a view whose units are all reused has nothing to draw here, and opens no block
	bool anyRebuild = false;
	for( int i = 0; i < viewDef->numShadowOps && !anyRebuild; i++ )
	{
		anyRebuild = ( viewDef->shadowOps[i].flags & ( SCOP_REBUILD | SCOP_SCROLL ) ) != 0;
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
			if( ( op.flags & ( SCOP_REBUILD | SCOP_SCROLL ) ) == 0 )
			{
				continue;		// the map is in the cache from an earlier frame
			}
			Framebuffer* target = targets[op.target + 1];
			if( target == NULL )
			{
				continue;
			}
			const int bit = 1 << i;

			if( ( op.flags & SCOP_SCROLL ) != 0 )
			{
				// S4: a cascade whose window moved by whole texels. Its strips are cleared and drawn, each in the 1, 2 or 4 pieces
				// it occupies in toroidal storage, on top of the build the frontend names. When that build is not in the map ( a
				// frame that drew it was never executed, or the images were lost ) the whole window is drawn instead, from the same
				// chain: a frame that scrolls has gathered in full ( the heal ). S5: the op's rectangles are its strips and its
				// repairs, up to SHADOWCACHE_MAX_RECTS, all cleared before any is drawn; then, with SCOP_ADD, the casters that
				// joined the static maps this frame are drawn over the whole window without a clear: a depth map keeps the
				// nearest depth, so the map then holds what it would drawn again with them. The heal draws them with the rest
				shadowCacheRect_t rects[SHADOWCACHE_MAX_RECTS];
				int masks[SHADOWCACHE_MAX_RECTS];
				int numRects = op.numRects;
				const byte* bits = op.stripBits;
				const bool heal = op.baseSerial == 0 || shadowCacheBuilt[R_ShadowCacheSlot( op )] != op.baseSerial;
				if( heal )
				{
					rects[0] = { 0, 0, op.size, op.size };
					masks[0] = bit | SCB_STATIC;
					numRects = 1;
					bits = vLight->shadowCasterBits;
					shadowCacheHeals++;
				}
				else
				{
					assert( numRects >= 0 && numRects <= SHADOWCACHE_MAX_RECTS );
					numRects = Min( Max( numRects, 0 ), SHADOWCACHE_MAX_RECTS );
					for( int r = 0; r < numRects; r++ )
					{
						rects[r] = op.rects[r];
						masks[r] = SCB_STATIC | ( 1 << r );		// a rectangle's bit and the static bit ( R_ShadowCacheCasterBits )
					}
				}
				shadowCacheToroidalRect_t pieces[SHADOWCACHE_MAX_RECTS][4];
				int numPieces[SHADOWCACHE_MAX_RECTS];
				for( int r = 0; r < numRects; r++ )
				{
					numPieces[r] = R_ShadowCacheToroidalRects( op.wrap[0], op.wrap[1], op.size, rects[r], pieces[r] );
					for( int p = 0; p < numPieces[r]; p++ )
					{
						const shadowCacheRect_t& ph = pieces[r][p].physical;
						mark( op.target, heal ? SCDM_HEALED : ( ( ( op.repairs >> r ) & 1 ) ? SCDM_REPAIR : SCDM_STRIP ), ph.x0, ph.y0, ph.x1, ph.y1 );
						// the piece back to the far depth ( res 0: a clear, as below )
						shadowCacheCopyConstants_t constants;
						memset( &constants, 0, sizeof( constants ) );
						ShadowCacheCopyDraw( target->GetApiObject(), 1, globalImages->blackImage->GetTextureHandle().Get(), constants, &pieces[r][p].physical );
					}
				}
				ResetGraphicsStateCache();

				// the static casters that reach each rectangle; with no caster in the chain the rectangles stay clear
				for( int r = 0; r < numRects && bits != NULL; r++ )
				{
					for( int p = 0; p < numPieces[r]; p++ )
					{
						ShadowMapPassFast( vLight->globalShadows, vLight, i, false, bits, masks[r], masks[r], &op, target, &pieces[r][p] );
					}
				}

				// S5: the casters that joined the static maps this frame, on top of the whole window
				if( !heal && ( op.flags & SCOP_ADD ) != 0 && bits != NULL )
				{
					mark( op.target, SCDM_ADDED, 0, 0, op.size, op.size );
					const shadowCacheRect_t whole = { 0, 0, op.size, op.size };
					shadowCacheToroidalRect_t wholePieces[4];
					const int numWhole = R_ShadowCacheToroidalRects( op.wrap[0], op.wrap[1], op.size, whole, wholePieces );
					for( int p = 0; p < numWhole; p++ )
					{
						ShadowMapPassFast( vLight->globalShadows, vLight, i, false, bits, SCB_STATIC | SCB_ADDED, SCB_STATIC | SCB_ADDED, &op, target, &wholePieces[p] );
					}
				}

				shadowCacheBuilt[R_ShadowCacheSlot( op )] = op.serial;		// D74: only the backend knows that the map was drawn
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
			ShadowMapPassFast( vLight->globalShadows, vLight, spot ? -1 : i, false, vLight->shadowCasterBits, bit | SCB_STATIC, bit | SCB_STATIC, &op, target );
			mark( op.target, SCDM_BUILT, op.x, op.y, op.x + op.size, op.y + op.size );

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
