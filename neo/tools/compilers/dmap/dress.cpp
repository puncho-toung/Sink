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

/*

  The contract every dress pass keeps:

  A pass walks the areas of the world and collects the tris it generates into a
  local list; it inserts them (AddMapTriToAreas) only after the walk is done.
  AddMapTriToAreas ends in AddTriListToArea, which merges into a matching
  optimizeGroup_t or prepends a new one to the area's group list, so inserting
  while the pass still iterates that list would make it visit its own output.

  Randomness comes from idRandom seeded with DressSeed, never from the order of
  a list.

*/

// the dmap_* material keywords some dress pass reads, NULL terminated. Empty until the
// passes land: the scatter pass adds dmap_surfaceModel, dmap_foliage and dmap_smoothAngle,
// the tessellation pass dmap_tessSize and dmap_colorGen
static const char* const dressKeywords[] = { NULL };

// the classes of map-only (dmapOnly) entities some dress pass bakes, NULL terminated. The
// passes match the classname exactly: a def that inherits one of them is not baked
static const char* const dressEntityClasses[] = { FUNC_DECAL_CLASSNAME, NULL };

// the classes of map-only entities that no dress pass bakes because another reader takes their keys
// straight from the .map: the map graph's landmarks, carry volumes and landings (the game, at map
// load) and the light-grid hints (bakeLightGrids). NULL terminated; the readers match the classname
// exactly too
static const char* const mapReadEntityClasses[] = { "info_landmark", "trigger_transition", "info_landing", "info_lightgrid_hint", NULL };

/*
====================
DressCheckMaterialKeywords

The renderer keeps every dmap_* keyword of a material as text without judging it;
only dmap knows which ones a pass reads. Every other one is ignored, with one warning
per material and keyword.
====================
*/
static void DressCheckMaterialKeywords( uEntity_t* e )
{
	idList<const idMaterial*>	materials;

	for( int i = 0; i < e->numAreas; i++ )
	{
		for( optimizeGroup_t* group = e->areas[i].groups; group; group = group->nextGroup )
		{
			materials.AddUnique( group->material );
		}
	}

	for( int i = 0; i < materials.Num(); i++ )
	{
		const idList<dmapKeyword_t, TAG_MATERIAL>& args = materials[i]->GetDmapArgs();

		for( int j = 0; j < args.Num(); j++ )
		{
			int k;
			for( k = 0; dressKeywords[k] != NULL; k++ )
			{
				if( !args[j].keyword.Icmp( dressKeywords[k] ) )
				{
					break;
				}
			}
			if( dressKeywords[k] != NULL )
			{
				continue;
			}

			// a keyword the material repeats has already warned
			int m;
			for( m = 0; m < j; m++ )
			{
				if( !args[m].keyword.Icmp( args[j].keyword ) )
				{
					break;
				}
			}
			if( m < j )
			{
				continue;
			}

			common->Warning( "material '%s': unknown dmap keyword '%s' ignored", materials[i]->GetName(), args[j].keyword.c_str() );
		}
	}
}

/*
====================
DressCheckMapOnlyEntities

The game inhibits every dmapOnly entity and dmap strips its primitives, so one that
no pass bakes and no other reader takes from the .map is lost without a trace; say so
once per entity.
====================
*/
static void DressCheckMapOnlyEntities()
{
	for( int i = 1; i < dmapGlobals.numEntities; i++ )
	{
		if( !dmapGlobals.uEntities[i].dmapOnly )
		{
			continue;
		}

		const idDict& epairs = dmapGlobals.uEntities[i].mapEntity->epairs;
		const char* classname = epairs.GetString( "classname" );
		int k;
		for( k = 0; dressEntityClasses[k] != NULL; k++ )
		{
			if( !idStr::Icmp( classname, dressEntityClasses[k] ) )
			{
				break;
			}
		}
		if( dressEntityClasses[k] != NULL )
		{
			continue;
		}

		for( k = 0; mapReadEntityClasses[k] != NULL; k++ )
		{
			if( !idStr::Icmp( classname, mapReadEntityClasses[k] ) )
			{
				break;
			}
		}
		if( mapReadEntityClasses[k] == NULL )
		{
			common->Warning( "map-only entity '%s' (%s): no dmap pass handles this class, it bakes nothing", epairs.GetString( "name", va( "entity %i", i ) ), classname );
		}
	}
}

/*
====================
DressAreasPreOptimize

Between PutPrimitivesInAreas and Prelight. Passes collect first and insert after
their walk, see the contract above.
====================
*/
void DressAreasPreOptimize( uEntity_t* e )
{
	// world only, like the inlined static models
	if( dmapGlobals.entityNum != 0 )
	{
		return;
	}

	// first, while the groups still carry only the map's own materials
	DressCheckMaterialKeywords( e );

	DressDecalsPreOptimize( e );

	DressCheckMapOnlyEntities();
}

/*
====================
DressAreasPostOptimize

After OptimizeEntity, before FixGlobalTjunctions. Passes collect first and insert
after their walk, see the contract above.
====================
*/
void DressAreasPostOptimize( uEntity_t* e )
{
	if( dmapGlobals.entityNum != 0 )
	{
		return;
	}

	// no pass is registered here yet
}

/*
====================
DressAreasPostTjunction

After FixGlobalTjunctions: nothing changes the world's tris after this before
WriteOutputFile. Passes collect first and insert after their walk, see the contract
above.
====================
*/
void DressAreasPostTjunction( uEntity_t* e )
{
	if( dmapGlobals.entityNum != 0 )
	{
		return;
	}

	DressDecalsPostTjunction( e );
}

/*
====================
DressSeed

idRandom is a 32 bit LCG that returns the low 15 bits of its state, and those depend
only on the low 15 bits of the seed. A sum or a shift of the indices would leave most
input bits out of them and give neighbouring primitives related sequences, so each
index is folded in with the murmur3 32 bit finalizer, which spreads every input bit
over every output bit.
====================
*/
int DressSeed( int entityNum, int primitiveNum, int side )
{
	const unsigned int keys[3] = { ( unsigned int )entityNum, ( unsigned int )primitiveNum, ( unsigned int )side };

	// a non-zero start, so ( 0, 0, 0 ) does not seed with 0
	unsigned int h = 0x9E3779B9u;
	for( int i = 0; i < 3; i++ )
	{
		h ^= keys[i];
		h ^= h >> 16;
		h *= 0x85EBCA6Bu;
		h ^= h >> 13;
		h *= 0xC2B2AE35u;
		h ^= h >> 16;
	}

	return ( int )h;
}
