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
#ifndef __LIGHTLISTS_H__
#define __LIGHTLISTS_H__

/*
==========================================================================================

LIGHT LIST SEAMS ( X0 )

The GPU light record ( C1 ) and the shadow rows ( C2 ), with their HLSL mirrors, and
the frontend hooks that will build them. In X0 nothing fills them yet.
Included from RenderCommon.h, so it sees idVec4, uint32 and the assert macros.

==========================================================================================
*/

// flags of recordLight_t::flags
enum
{
	RLF_SUN					= BIT( 0 ),		// the map's primary sun; record 0 from F4 on ( D42, D73 )
	RLF_POINT				= BIT( 1 ),		// point light
	RLF_PARALLEL			= BIT( 2 ),		// parallel ( sun-like ) light
	RLF_MATH				= BIT( 3 ),		// math light; mathLight lanes carry the encode
	RLF_DEFAULT_TEXTURES	= BIT( 4 ),		// light has the default diffuse/specular textures ( D39 )
	RLF_CLUSTERED			= BIT( 5 ),		// the light may be listed ( D40 )
	RLF_AMBIENT				= BIT( 6 ),		// inject-only record ( D26, D40 )
	RLF_FOGLIGHT			= BIT( 7 ),		// inject-only fog record, read the FogPass way ( D26, D40, D81 )
	RLF_NOSPECULAR			= BIT( 8 ),		// no specular contribution
	RLF_MOVED_THIS_FRAME	= BIT( 9 )		// the light moved this frame
};

const int MAX_RECORD_LIGHTS = 1024;

struct lightListView_t;			// opaque; track F defines it later

/*
====================
recordLight_t

The GPU light record ( C1 ), 192 bytes, the relative frame throughout: everything
minus viewDef_t::renderOrigin. HLSL mirror shaders/lightlists.inc.hlsl.
====================
*/
struct alignas( 16 ) recordLight_t
{
	idVec4		originInvRadiusSq;	// xyz = relative shading origin ( globalLightOrigin, already relative, rev 4.2 ), w = 1 / r^2 of the sphere centred there that encloses the light's bounds ( the binning sphere ); RLF_PARALLEL records: w = 0, no sphere ( rev 4.2, see below )
	idVec4		color;				// rgb = evaluated stage colour x r_lightScale x cullFade, applied ONCE ( linear; D58 as amended, rev 4.2 ), w = spot scale ( 0 = omni ). RLF_FOGLIGHT records ( D81 ): rgb = the RAW stage colour, no light scale, w = the fog distance D ( colour alpha, 500 when alpha <= 1 ), as FogPass reads them
	idVec4		spotDir;			// xyz = cone axis ( a direction: the same in the map frame and the relative frame ), w = spot bias ( 1 = omni )
	int			shadowRow;			// first row of this light's shadow rows; -1 = unshadowed
	uint32		channelMask;		// bit 0 = default; spectrum lights set bit ( 1 + spectrum )
	uint32		flags;				// RLF_SUN, RLF_POINT, RLF_PARALLEL, RLF_MATH, RLF_DEFAULT_TEXTURES ( D39 ), RLF_CLUSTERED ( may be listed, D40 ), RLF_AMBIENT and RLF_FOGLIGHT ( inject only, D26, D40 ), RLF_NOSPECULAR, RLF_MOVED_THIS_FRAME
	int			uniqueId;			// idRenderLightLocal::uniqueId ( world-monotonic, D44 ), copied by the frontend; the volumetrics history key
	idVec4		lightProject[4];	// relative planes S, T, Q, falloff: the viewLight_t planes, unchanged ( already relative ) ( hasMatrix lights have no record, D57 )
	idVec4		mathLight[3];		// rpUser0..2 exactly as R_MathLightToParms() encodes mathLightData_t ( the one encode, factored out of RenderInteractions by X0, S7 ); zero when !RLF_MATH
	idVec4		ids;				// x = number ( importance rank ), y = stage, z = record index in the fog-eligible view's previous frame ( -1; keyed by the frame stamp, D56 ), w = scattering albedo for RLF_FOGLIGHT records ( default 0, D81 ), else 0
};
compile_time_assert( sizeof( recordLight_t ) == 192 );
assert_offsetof( recordLight_t, originInvRadiusSq, 0 );	// every HLSL-mirrored field, not only the size ( S82(i); idlib/sys/sys_assert.h:117 )
assert_offsetof( recordLight_t, color, 16 );
assert_offsetof( recordLight_t, spotDir, 32 );
assert_offsetof( recordLight_t, shadowRow, 48 );
assert_offsetof( recordLight_t, channelMask, 52 );
assert_offsetof( recordLight_t, flags, 56 );
assert_offsetof( recordLight_t, uniqueId, 60 );
assert_offsetof( recordLight_t, lightProject, 64 );
assert_offsetof( recordLight_t, mathLight, 128 );
assert_offsetof( recordLight_t, ids, 176 );

/*
====================
shadowRow_t

The shadow rows ( C2 ), 80 bytes, one per spot map, per point face ( 6 ), per
cascade; filled by the backend in R_BuildShadowRows after ShadowAtlasPass.
====================
*/
struct alignas( 16 ) shadowRow_t
{
	idVec4		worldToShadow[4];	// parallel and point units: renderMatrix_clipSpaceToWindowSpace x shadowP[s] x shadowV[s]; spot units: shadowP[0] x shadowV[0] ( its P already carries the window-space and clip-to-UV fix-ups, F6 ). This is the legacy per-draw product of RenderBackend.cpp:2087-2093 ( parallel ), :2103-2109 ( point ), :2119-2124 ( spot ) WITHOUT the model matrix the legacy path folds in at :2087/:2103/:2119: interactionSM's texcoord7 is model space, the forward VS writes the world position ( N9, F40 ). ( rev 4.2: 'world' here is the eye-relative frame; the rows are built from vLight->shadowV and shadowP, which already take relative coordinates )
	idVec4		atlasRect;			// xy = imageAtlasOffset[s] / atlasSize, z = imageSize / atlasSize ( < 0: not placed - the forward loop treats the light as unshadowed, the inject SKIPS it, D80 ), w = cascade far distance ( rpCascadeDistances[s] ) for RLF_SUN rows, shadowFadeOut for every other row ( D47 )
};
compile_time_assert( sizeof( shadowRow_t ) == 80 );
assert_offsetof( shadowRow_t, worldToShadow, 0 );		// S82(i)
assert_offsetof( shadowRow_t, atlasRect, 64 );

void	R_NumberRecordLights();		// frontend, called from R_RenderView after R_AddLights; body in tr_frontend_lightlists.cpp
void	R_BuildLightLists();		// frontend, after R_AddModels and before R_OptimizeViewLightsList; same file

#endif // __LIGHTLISTS_H__
