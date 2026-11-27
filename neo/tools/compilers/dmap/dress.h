/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.
Copyright (C) 2026 Phillip Melnichenko

This file is part of the Doom 3 GPL Source Code (?Doom 3 Source Code?).

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#ifndef __DRESS_H__
#define __DRESS_H__

#include "dmap.h"

/*
===============================================================================

	Dress passes: geometry that dmap generates from the map content (decals,
	scatter, smoothing, tessellation) and puts into the areas of the world.

	ProcessModel calls the three dispatchers at fixed points of the pass order.
	Each pass lives in its own file (dress_decal.cpp, dress_chunk.cpp, dress_smooth.cpp,
	dress_color.cpp, later dress_scatter.cpp, dress_tess.cpp); dress.cpp holds only the dispatchers,
	the checks that warn about dmap_* material keywords no pass reads and
	map-only entities no pass bakes, DressParseNumbers and DressSeed.

	A pass collects the tris it generates and inserts them (AddMapTriToAreas, or
	whole groups, as dress_chunk.cpp does)
	only after its walk of the areas is done, see the contract in dress.cpp.

	World entity only, except the smoothing pass and the colour generators: they
	only rewrite the normals and colours of tris already there, and DressAreasPostTjunction
	runs them for every entity, so brush models smooth and colour like the world.

===============================================================================
*/

// at the start of every dmap run, before any map is loaded: the passes that keep state over
// the entities of one compile reset it here (DressSmoothReset, DressColorReset)
void	DressBeginCompile();

// between PutPrimitivesInAreas and Prelight: what a pass adds here is carved by the
// static light frustums and optimized together with the brush geometry
void	DressAreasPreOptimize( uEntity_t* e );

// after OptimizeEntity, before FixGlobalTjunctions: skips the optimizer, but is still
// T-junction fixed and grid snapped with the rest of the world
void	DressAreasPostOptimize( uEntity_t* e );

// after FixGlobalTjunctions: written exactly as the pass made it, nothing changes it
// afterwards and it splits no edge of the world
void	DressAreasPostTjunction( uEntity_t* e );

// the class of the compile-time decal projectors (base/def/func.def)
static const char* const FUNC_DECAL_CLASSNAME = "func_decal";

// func_decal (dress_decal.cpp), from DressAreasPreOptimize: clips the world's tris whose
// material allows overlays to every projector and adds the fragments with the decal material
void	DressDecalsPreOptimize( uEntity_t* e );

// func_decal, from DressAreasPostTjunction: writes the fade as vertex colour (the surface is
// written with PROC_VERTEX_FORMAT_COLOR) and gives the fade band groups their projector's
// merge group back, so each projector is written as one surface
void	DressDecalsPostTjunction( uEntity_t* e );

// world-surface chunking (dress_chunk.cpp), from DressAreasPostOptimize: at the grid the option
// -chunkGrid or the worldspawn key chunkGrid names, else at 512 ( F7; 0 for no cut ), cuts every
// world group whose material is lit, not discrete, not translucent and not noFragment on the
// planes k * grid of each axis and gives each cell's piece a merge
// group of its own, so each is written as its own surface; FixGlobalTjunctions, which runs next,
// repairs the seams against the groups not cut
void	DressChunkWorldSurfaces( uEntity_t* e );

// the smoothing pass (dress_smooth.cpp), from DressAreasPostTjunction for every entity: the
// tris of every material whose own dmap_smoothAngle, else the worldspawn key dmap_smoothAngle,
// asks for it get the angle-weighted mean of the face normals that meet at a vertex within
// the angle (dress_smooth_core.h); only normals change. Only groups made of brush sides alone
// take part (optimizeGroup_t::notBrushSides). The cvar dmap_smooth 0 turns it off
void	DressSmoothPostTjunction( uEntity_t* e );
void	DressSmoothReset();

// the class of the compile-time vertex colour volumes (base/def/func.def)
static const char* const FUNC_VERTEXCOLOR_CLASSNAME = "func_vertexcolor";

// the colour generators (dress_color.cpp), from DressAreasPostTjunction for every entity
// after the smoothing pass: every non-discrete group of a material with a dmap_colorGen
// keyword (slope, volume) is written with vertex colour, computed per vertex from its
// written normal and position and the func_vertexcolor brushes; only colours change. The
// cvar dmap_colorGen 0 turns it off
void	DressColorPostTjunction( uEntity_t* e );
void	DressColorReset();

// the numbers of a dmap keyword's argument text (dmapKeyword_t::args: the lexer's tokens
// joined by single spaces, so "-0.5" arrives as "- 0.5"; parentheses around a vector are
// skipped). Returns how many it read into values, or -1 when a word is neither a number
// nor "-", "(" or ")", or when there are more than maxValues numbers
int		DressParseNumbers( const char* text, float* values, int maxValues );

// a seed for idRandom::SetSeed derived from map content only: the map entity index, the
// primitive index inside that entity and a side (brush side, patch or mesh triangle) index.
// The same .map gives the same seed on every run and every machine, whatever order dmap's
// lists are in (they are built by merging and prepending). Passes that scatter or stamp
// seed idRandom from it; never seed from a list position or a pointer, so a compile is
// repeatable.
int		DressSeed( int entityNum, int primitiveNum, int side );

#endif /* !__DRESS_H__ */
