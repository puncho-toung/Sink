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

#pragma once

// The fog set's constants: shaders/froxel_fog.inc.hlsl has the same struct, lane for lane. X0 created the buffer
// ( backEnd.froxelFogParms ) and writes zeros into it at the start of every 3D view: all zeros mean "fog off". The
// fog pass writes the real values at the apply slot of the fog view ( D66 ).
struct froxelFogParms_t
{
	idVec4	volumeParms;	// xy = the fog view's screen UV to volume UV, z = the slice count G, w = 1 / G
	idVec4	depthParms;		// xy = the slices' bias S and step, as froxelConstants_t::sliceParms.xy; zw = the projection's depth terms: distance = z / ( depth + w )
	idVec4	fogParms;		// x = 1 where the fog is on, y = the apply's debug view ( r_volumetricFogDebug ), zw = the fog view's first pixel
	idVec4	reserved;		// xy = 1 / the fog view's size in pixels, zw = the volume UV of the view's last column and row: the lookup stays inside the view's grid
	// V8 ( B2 ): where the view's bounded media can be seen. Zeros ( the fog-off parms ) test nothing
	idVec4	boundsRect;		// xy = the top-left, zw = the bottom-right corner, in the fog view's screen UV, of the rectangle outside which the lookup reads only clear air ( the columns and rows a medium's centre can be in and two more each side, BoundsBox )
	idVec4	boundsParms;	// x = 1 where the lookup tests the rectangle and the first slice, y = the slice coordinate nearer than which it reads only clear air ( the box's first slice, less one ), zw = 0
};
compile_time_assert( sizeof( froxelFogParms_t ) == 96 );

// The kernels' constants: shaders/builtin/volumetrics/froxel_grid.inc.hlsl has the same struct, lane for lane.
struct froxelConstants_t
{
	idVec4	gridSize;		// xyz = the columns, rows and slices the kernels fill: the view's grid and, where the volumes have them, one column and row more; w = the light records the inject reads ( 0: the fog is not lit )
	idVec4	sliceParms;		// x = S, y = ln( 1 + farEnd / S ) / slices, z = 1 / the store scale, w = the history's sigma_t times it ( V3 )
	idVec4	rayParms;		// a column's ray: lateral = ( column + 0.5, row + 0.5 ) * xy - zw, forward 1
	idVec4	medium;			// the uniform medium: rgb = sigma_s * L of the furnace radiance, a = sigma_t, both times the store scale
	// V2: the inject's lanes
	idVec4	viewOrigin;		// xyz = the eye in the relative frame ( the light records' frame ), w = sigma_s times the store scale
	idVec4	viewForward;		// xyz = the view's forward axis, w = the phase function's g ( Henyey-Greenstein )
	idVec4	viewRight;		// xyz = its right axis, w = how the inject finds a froxel's records ( r_volumetricFogCull )
	idVec4	viewUp;			// xyz = its up axis, w = the bound of a stored coefficient
	idVec4	tileParms;		// the light lists' tiles: x = a tile's side in pixels, y = the tiles in a row, z = the rows, w = the words of a tile
	idVec4	binParms;		// x = the light lists' Z-bins per unit of view distance, y = the bin count, z = a column's side in pixels, w = the near plane's distance
	idVec4	ambient[9];		// rgb = the view's blended SH radiance, each band times the phase function's weight and the surfaces' ambient scale; ambient[0].w = one texel of the shadow atlas ( V3 )
	// V3: the shadows
	idVec4	shadowParms;	// x = the sun's depth bias scale, y = every other light's ( the legacy pass's: a receiver's depth times it ), z = a parallel light's rows ( its cascades: every parallel light's are the view's splits ), w = 1 where the inject looks shadows up ( r_volumetricFogShadows )
	// V3: the history
	idVec4	jitter;			// xyz = this frame's sample in its froxel ( columns, rows, slices: -0.5 .. 0.5 ), w = 1 where the history holds last frame's fog
	idVec4	historyParms;	// x = the history's weight, y = its weight where it is clipped, z = the colour change that clips ( a share of the larger colour ), w = the lights that vanished since last frame
	idVec4	historyGrid;	// xy = a screen UV to the volume UV of the view's grid, z = 1 / the slices, w = the records of last frame
	idVec4	prevOrigin;		// xyz = this frame's render origin minus last frame's ( a point of this frame's relative frame plus it is one of last frame's ), w = the lights that moved
	idVec4	prevMVP[4];		// last frame's unjittered MVP, relative to its render origin ( H10 ): the rows of an idRenderMatrix
	idVec4	skipParms;		// x = 1 where the inject skips the froxels no pixel's lookup reaches, y = a column's side in pixels, zw = the view's size in pixels
	idVec4	pixelParms;		// xy = the projection's depth terms ( distance = x / ( depth + y ) ), zw = the view's first pixel in the render target
	uint32	takeMask[32];	// a bit per light record, word w in [ w / 4 ][ w % 4 ] of the shader's uint4 lanes: set where the fog can take the record's light at all
	// V4: the media
	idVec4	mediaParms;		// x = 1 where the inject reads the media volume ( the media pass ran ), y = the furnace radiance ( a test input ), z = 1 on V4's path ( r_volumetricFogMedia: the resolve's medium per froxel and the mark volume ), w = the media in the view's list
	idVec4	mediaParms2;	// x = the media that moved, came or went ( four boxes each in m_MediaMoved ), y = the fog lights' rate times the store scale, z = 1 / the fog lights' enter ramp, w = 1 where the inject takes fog lights ( V4b )
	// V8: the fog's cost follows its volumes' bounds ( r_volumetricFogBounds )
	idVec4	boundsParms;	// x = 1 where a froxel with no medium returns early in the resolve and the inject runs its permutation FROXEL_CLEAR_AIR 1 ( B1: the view's media are its fog lights alone, V4's path ), yzw = 0
	// V8 ( B4 ): the box of the view's bounded media in froxels, padded by one ( BoundsBox )
	idVec4	boxMin;			// xyz = its first column, row and slice, w = 1 where the inject runs over it alone and the resolve and the integrate skip its outside
	idVec4	boxMax;			// xyz = its last column, row and slice ( inclusive ), w = 0
};
compile_time_assert( sizeof( froxelConstants_t ) == 688 );

// V4: one medium of the media pass ( shaders/builtin/volumetrics/froxel_media.inc.hlsl has the same struct, lane for lane ):
// the height fog, the dust, the test box, and track M's volumes later. The pass writes at most FOG_MEDIA_MAX a view into
// its media list; the media pass tests each one's sphere once per thread group and sums the density of those left
const int FOG_MEDIA_MAX = 64;
enum fogMediumShape_t
{
	FOG_SHAPE_ALL		= 0,		// everywhere
	FOG_SHAPE_HEIGHT	= 1,		// full below a height of the map, falling off above it
	FOG_SHAPE_BOX		= 2		// inside a box of the medium's frame
};
struct fogMedium_t
{
	idVec4	sphere;			// xyz = the bounding sphere's centre in the relative frame, w = 1 / r^2; 0 = unbounded ( no group test )
	idVec4	sigma;			// rgb = sigma_s at full density ( the albedo times sigma_t ), a = sigma_t at full density, per unit times the store scale
	idVec4	shape;			// x = the shape ( fogMediumShape_t ); HEIGHT: y = the base height in the medium's frame ( its z ), z = 1 / the falloff; BOX: yzw = the half extents
	idVec4	frame[3];		// the medium's frame: coordinate i of a relative point p is dot( frame[i].xyz, p ) + frame[i].w
	idVec4	noiseOffset;	// xyz = added to the frame's coordinates where the slots read them ( the wind's drift, the map's wrapped offset ), w = the slots in use
	idVec4	slot[6];		// noise slots 1 and 2, three lanes each ( V4c )
};
compile_time_assert( sizeof( fogMedium_t ) == 208 );
// V4c: a noise slot's kind and its op ( shaders/builtin/volumetrics/froxel_media.inc.hlsl, the same numbers )
enum fogNoiseType_t
{
	FOG_NOISE_NONE			= 0,
	FOG_NOISE_FBM			= 1,		// gradient-noise fBm: soft lumps
	FOG_NOISE_WORLEY		= 2,		// cellular noise: round billows
	FOG_NOISE_PERLINWORLEY	= 3,		// fBm remapped by Worley's fBm: puffy lumps
	FOG_NOISE_CURL			= 4		// the curl of a noise potential: a warp
};
enum fogNoiseOp_t
{
	FOG_OP_MULTIPLY		= 0,
	FOG_OP_ADD			= 1,
	FOG_OP_ERODE		= 2,
	FOG_OP_REMAP		= 3,
	FOG_OP_WARP			= 4
};
