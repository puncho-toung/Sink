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
	idVec4	gridSize;		// xyz = the columns, rows and slices the kernels fill: the view's grid and, where the volumes have them, one column and row more
	idVec4	sliceParms;		// x = S, y = ln( 1 + farEnd / S ) / slices, z = 1 / the store scale
	idVec4	rayParms;		// a column's ray: lateral = ( column + 0.5, row + 0.5 ) * xy - zw, forward 1
	idVec4	medium;			// the uniform medium: rgb = sigma_s * L, a = sigma_t, both times the store scale
};
compile_time_assert( sizeof( froxelConstants_t ) == 64 );
