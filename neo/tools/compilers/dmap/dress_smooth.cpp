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

#include "precompiled.h"
#pragma hdrstop

#include "dress.h"
#include "dress_smooth_core.h"

/*

  Smoothed vertex normals (Phase 2C P3, stage 2C-4a WP4.1). dmap gives every vertex of
  a brush face its plane's normal (TriListForSide), so brushwork shades faceted. The tris
  of a material that says dmap_smoothAngle <degrees>, or of every material when the
  worldspawn says it (a material's own keyword wins, 0 turns it off), get at each vertex
  the angle-weighted mean of the face normals that meet there within the angle. The rules
  are the S4.1 core's (dress_smooth_core.h): corners link when an edge ray of each points
  the same way, so a T-junction the fix missed still links; a wedge whose faces share one
  plane keeps the stock bytes; the result does not depend on the order of the lists or on
  how the optimizer triangulated a plane.

  The pass runs after FixGlobalTjunctions, the last pass that moves or adds vertices, for
  every entity. Only normals change: no vertex moves and no tri is added or removed, and
  the collision model and the AAS do not read the .proc's normals. MatchVert welds the
  vertices it gave one normal, where their st agrees too.

  Taking part: the tris of the non-discrete groups (discrete ones are never snapped to the
  grid) made of brush sides alone (optimizeGroup_t::notBrushSides false: no patch, mesh,
  inlined model or dress pass tri, which may carry normals of their own) whose material
  resolves an angle above 0. The core gets the group plane's normal, not the vertices'
  bytes: the T-junction fix and the optimizer decode, lerp, normalize and re-encode the
  normal of every vertex they add, and along an edge with many T-junctions those bytes
  drift by degrees from the plane (the Fable consult, P3/consult). A wedge that keeps the
  stock normal keeps those bytes as they are.

*/

idCVar dmap_smooth( "dmap_smooth", "1", CVAR_TOOL | CVAR_BOOL, "0 = dmap ignores dmap_smoothAngle (the material keyword and the worldspawn key) and writes the stock normals" );

// must equal tritjunction.cpp's SNAP_FRACTIONS: FixGlobalTjunctions has put every vertex of a
// non-discrete group on this grid, so xyz * SMOOTH_SNAP_FRACTIONS is an integer
static const double SMOOTH_SNAP_FRACTIONS = 32.0;

// the core's bound: every position within 2^23 grid steps (262,144 units) on every axis
static const double SMOOTH_MAX_STEPS = 8388608.0;

typedef struct
{
	const idMaterial*	material;
	float				cosAngle;	// of the angle the material resolves to, when on
	bool				on;			// the angle is above 0
} smoothMaterial_t;

// the materials resolved for the map being compiled, so each one warns once a compile, and
// the worldspawn key, read at the first entity; DressSmoothReset clears them for every run
static idList<smoothMaterial_t>	smoothMaterials;
static idHashIndex				smoothMaterialHash;
static bool						smoothWorldRead = false;
static float					smoothWorldAngle = 0.0f;	// the worldspawn key, 0 = none

/*
====================
DressSmoothReset
====================
*/
void DressSmoothReset()
{
	smoothMaterials.Clear();
	smoothMaterialHash.Clear();
	smoothWorldRead = false;
	smoothWorldAngle = 0.0f;
}

/*
====================
SmoothAngleFromText

The degrees of a dmap_smoothAngle, the material keyword's or the worldspawn key's: 0 to
180, 0 = off. Warns and returns 0 for text that is not one number or is below 0, and
clamps above 180.
====================
*/
static float SmoothAngleFromText( const char* text, const char* owner )
{
	float angle;
	if( DressParseNumbers( text, &angle, 1 ) != 1 )
	{
		common->Warning( "%s: dmap_smoothAngle '%s' is not one number, no smoothing", owner, text );
		return 0.0f;
	}
	if( angle < 0.0f )
	{
		common->Warning( "%s: dmap_smoothAngle %g is below 0, no smoothing", owner, angle );
		return 0.0f;
	}
	if( angle > 180.0f )
	{
		common->Warning( "%s: dmap_smoothAngle %g is above 180, using 180", owner, angle );
		return 180.0f;
	}
	return angle;
}

/*
====================
SmoothMaterialHashKey
====================
*/
static int SmoothMaterialHashKey( const idMaterial* material )
{
	const uintptr_t bits = ( uintptr_t )material;
	return ( int )( ( bits >> 4 ) ^ ( bits >> 20 ) ) & 0x7fffffff;
}

/*
====================
SmoothMaterial

The angle a material resolves to: its own dmap_smoothAngle (the last one when it repeats),
else the worldspawn key. Resolved once a compile; warns when the material says
unsmoothedTangents, which rebuilds the normals at load (tr_trisurf.cpp) and so undoes the
smoothing.
====================
*/
static smoothMaterial_t SmoothMaterial( const idMaterial* material )
{
	const int hash = SmoothMaterialHashKey( material );
	for( int i = smoothMaterialHash.First( hash ); i != -1; i = smoothMaterialHash.Next( i ) )
	{
		if( smoothMaterials[i].material == material )
		{
			return smoothMaterials[i];
		}
	}

	float angle = smoothWorldAngle;
	const idList<dmapKeyword_t, TAG_MATERIAL>& args = material->GetDmapArgs();
	for( int i = args.Num() - 1; i >= 0; i-- )
	{
		if( !args[i].keyword.Icmp( "dmap_smoothAngle" ) )
		{
			angle = SmoothAngleFromText( args[i].args.c_str(), va( "material '%s'", material->GetName() ) );
			break;
		}
	}

	smoothMaterial_t entry;
	entry.material = material;
	entry.on = ( angle > 0.0f );
	entry.cosAngle = entry.on ? ( float )cos( ( double )angle * ( idMath::PI / 180.0 ) ) : 1.0f;
	if( entry.on && material->UseUnsmoothedTangents() )
	{
		common->Warning( "material '%s': unsmoothedTangents rebuilds the normals at load, so dmap_smoothAngle does not show on it", material->GetName() );
	}

	const int index = smoothMaterials.Append( entry );
	smoothMaterialHash.Add( hash, index );
	return entry;
}

/*
====================
SmoothGroupCosine

False when the tris of the group cannot take part: a discrete group (never snapped to
the grid, and each its own surface), a group without a plane, a group with tris that did
not come from a brush side, or a material that resolves no angle. Else the cosine of the
material's angle.
====================
*/
static bool SmoothGroupCosine( const optimizeGroup_t* group, float& cosAngle )
{
	if( group->material == NULL || group->material->IsDiscrete() || group->planeNum < 0 || group->notBrushSides )
	{
		return false;
	}

	const smoothMaterial_t entry = SmoothMaterial( group->material );
	cosAngle = entry.cosAngle;
	return entry.on;
}

/*
====================
SmoothTriInput

The core's input for one tri of a group that takes part, with the group plane's normal;
false when a position is beyond the core's bound.
====================
*/
static bool SmoothTriInput( const mapTri_t* tri, const idVec3& planeNormal, float cosAngle, dmapsmooth::Tri& out )
{
	for( int k = 0; k < 3; k++ )
	{
		for( int axis = 0; axis < 3; axis++ )
		{
			// on the grid already; the rounding only makes the integer exact
			const double step = floor( ( double )tri->v[k].xyz[axis] * SMOOTH_SNAP_FRACTIONS + 0.5 );
			if( !( step <= SMOOTH_MAX_STEPS && step >= -SMOOTH_MAX_STEPS ) )
			{
				return false;
			}
			out.p[k][axis] = ( int64_t )step;
		}
	}

	for( int axis = 0; axis < 3; axis++ )
	{
		out.n[axis] = planeNormal[axis];
	}
	out.cosAngle = cosAngle;
	out.smooth = true;
	out.hard = 0;	// creases are the terrain package's (2C-4b)
	return true;
}

/*
====================
DressSmoothPostTjunction
====================
*/
void DressSmoothPostTjunction( uEntity_t* e )
{
	if( !dmap_smooth.GetBool() )
	{
		return;
	}

	if( !smoothWorldRead )
	{
		smoothWorldRead = true;
		const char* worldText = dmapGlobals.uEntities[0].mapEntity->epairs.GetString( "dmap_smoothAngle", "" );
		if( worldText[0] != '\0' )
		{
			smoothWorldAngle = SmoothAngleFromText( worldText, "worldspawn" );
		}
	}

	// count first, so a map that asks for no smoothing allocates nothing; and the groups of a
	// material that asks whose tris are not brush sides alone, which are left out
	int numTris = 0;
	int numOtherGroups = 0;
	for( int a = 0; a < e->numAreas; a++ )
	{
		for( const optimizeGroup_t* group = e->areas[a].groups; group; group = group->nextGroup )
		{
			float cosAngle;
			if( !SmoothGroupCosine( group, cosAngle ) )
			{
				if( group->notBrushSides && group->material != NULL && !group->material->IsDiscrete() && SmoothMaterial( group->material ).on )
				{
					numOtherGroups++;
				}
				continue;
			}
			for( const mapTri_t* tri = group->triList; tri; tri = tri->next )
			{
				numTris++;
			}
		}
	}
	if( numTris == 0 && numOtherGroups == 0 )
	{
		return;
	}

	std::vector<dmapsmooth::Tri>	tris;
	std::vector<mapTri_t*>			sources;
	tris.reserve( numTris );
	sources.reserve( numTris );

	int numFar = 0;
	for( int a = 0; a < e->numAreas; a++ )
	{
		for( optimizeGroup_t* group = e->areas[a].groups; group; group = group->nextGroup )
		{
			float cosAngle;
			if( !SmoothGroupCosine( group, cosAngle ) )
			{
				continue;
			}
			const idVec3& planeNormal = dmapGlobals.mapPlanes[group->planeNum].Normal();
			for( mapTri_t* tri = group->triList; tri; tri = tri->next )
			{
				dmapsmooth::Tri input;
				if( !SmoothTriInput( tri, planeNormal, cosAngle, input ) )
				{
					numFar++;
					continue;
				}
				tris.push_back( input );
				sources.push_back( tri );
			}
		}
	}

	dmapsmooth::Stats stats = {};
	if( !tris.empty() )
	{
		std::vector<dmapsmooth::Corner> corners( tris.size() * 3 );
		dmapsmooth::Smooth( tris.data(), ( uint32_t )tris.size(), corners.data(), &stats );

		// only the corners of wedges with more than one plane: the others keep their bytes
		for( size_t i = 0; i < sources.size(); i++ )
		{
			for( int k = 0; k < 3; k++ )
			{
				const dmapsmooth::Corner& corner = corners[i * 3 + k];
				if( corner.changed )
				{
					sources[i]->v[k].SetNormal( corner.n[0], corner.n[1], corner.n[2] );
				}
			}
		}
	}

	// the world's line always, a brush model's in verbose mode only: a map can hold thousands of them
	const char* format = "dmap smooth%s: %u tris %u corners %u wedges %u smoothed %u hard %u planarKept, %i tris beyond the bound, %i groups not brush sides left out\n";
	if( dmapGlobals.entityNum == 0 )
	{
		common->Printf( format, "", stats.tris, stats.corners, stats.wedges, stats.smoothed, stats.hardRays, stats.planarKept, numFar, numOtherGroups );
	}
	else
	{
		common->VerbosePrintf( format, va( " (entity %i)", dmapGlobals.entityNum ), stats.tris, stats.corners, stats.wedges, stats.smoothed, stats.hardRays, stats.planarKept, numFar, numOtherGroups );
	}
}
