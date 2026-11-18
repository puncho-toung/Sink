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
};
compile_time_assert( sizeof( froxelFogParms_t ) == 64 );

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
};
compile_time_assert( sizeof( froxelConstants_t ) == 608 );
