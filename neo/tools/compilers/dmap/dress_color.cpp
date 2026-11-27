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

  The colour generators (Phase 2C P3, stage 2C-4a WP4.2). A material that says

	dmap_colorGen slope <x> <y> <z> <minDot> <maxDot>
	dmap_colorGen volume

  (any number of them, in any order) is "coloured": dmap writes its tris with vertex
  colour, computed per vertex from the vertex as written:

  - every vertex starts opaque white, the colour the loader gives a surface written
    without one;
  - the generators run in the material's keyword order;
  - slope: rgb = clamp( ( n . dir - minDot ) / ( maxDot - minDot ), 0, 1 ) on the vertex
    normal after the smoothing pass, alpha kept (the interaction reads the vertex
    colour's rgb only, so a blend weight lives in rgb); equal minDot and maxDot make a
    step at that dot;
  - volume: each func_vertexcolor brush of the map, in entity and brush order: depth =
    the least inside distance to its planes, w = falloff > 0 ? clamp( depth / falloff,
    0, 1 ) : ( depth >= 0 ); mode replace: c = lerp( c, _color, w ), add: c = clamp( c +
    _color * w ). The brushes are in map (world) space; a brush model's vertex is moved
    by the entity's origin first. A brush model's rotation is not applied, to the position
    for the volumes nor to the normal for the slope, and a func_vertexcolor's own
    rotation keys are ignored (with a warning).

  The pass runs after the smoothing pass, so after FixGlobalTjunctions, for every entity.
  Every non-discrete group of a coloured material gets vertexColor, so the groups of one
  merge group never mix coloured and plain ones (WriteOutputSurfaces stops with an Error
  if they do). Discrete groups, the decals among them, are left to their own passes.

*/

idCVar dmap_colorGen( "dmap_colorGen", "1", CVAR_TOOL | CVAR_BOOL, "0 = dmap ignores dmap_colorGen and func_vertexcolor and writes the stock surfaces" );

typedef enum
{
	COLORGEN_SLOPE,
	COLORGEN_VOLUME
} colorGenType_t;

typedef struct
{
	colorGenType_t		type;
	idVec3				dir;		// slope: unit length
	float				minDot;		// slope
	float				maxDot;		// slope
} colorGen_t;

typedef struct
{
	const idMaterial*	material;
	int					firstGen;	// into colorGens
	int					numGens;	// 0 = not coloured
} colorMaterial_t;

typedef struct
{
	int					firstPlane;	// into colorVolumePlanes, the brush's planes facing out
	int					numPlanes;
	idBounds			bounds;		// of the brush, a unit larger on every side
	idVec4				color;		// _color, 0 to 1
	float				falloff;	// 0 = hard
	bool				add;		// mode add, else replace
} colorVolume_t;

// a vertex this close outside a hard volume's face (falloff 0) is still inside: a vertex on a
// non-axial face would otherwise go in or out on the rounding of its distance
static const float COLOR_VOLUME_ON_EPSILON = 0.01f;

// the materials and volumes of the map being compiled, so each material and volume warns
// once a compile; the volumes are read at the first entity. DressColorReset clears them
// for every run
static idList<colorMaterial_t>	colorMaterials;
static idHashIndex				colorMaterialHash;
static idList<colorGen_t>		colorGens;
static idList<colorVolume_t>	colorVolumes;
static idList<idPlane>			colorVolumePlanes;
static idList<int>				colorGroupVolumes;	// the volumes whose bounds meet the bounds of the group being coloured
static bool						colorVolumesRead = false;

/*
====================
DressColorReset
====================
*/
void DressColorReset()
{
	colorMaterials.Clear();
	colorMaterialHash.Clear();
	colorGens.Clear();
	colorVolumes.Clear();
	colorVolumePlanes.Clear();
	colorGroupVolumes.Clear();
	colorVolumesRead = false;
}

/*
====================
ColorMaterialHashKey
====================
*/
static int ColorMaterialHashKey( const idMaterial* material )
{
	const uintptr_t bits = ( uintptr_t )material;
	return ( int )( ( bits >> 4 ) ^ ( bits >> 20 ) ) & 0x7fffffff;
}

/*
====================
ColorParseGenerator

One dmap_colorGen of a material. Warns and returns false when it is malformed.
====================
*/
static bool ColorParseGenerator( const idMaterial* material, const char* text, colorGen_t& gen )
{
	// the generator's name is the first word
	const char* rest = text;
	while( *rest != '\0' && *rest != ' ' )
	{
		rest++;
	}
	const idStr name( text, 0, ( int )( rest - text ) );

	float numbers[5];
	const int numNumbers = DressParseNumbers( rest, numbers, 5 );

	if( !name.Icmp( "slope" ) )
	{
		if( numNumbers != 5 )
		{
			common->Warning( "material '%s': dmap_colorGen '%s' needs slope <x> <y> <z> <minDot> <maxDot>, ignored", material->GetName(), text );
			return false;
		}
		gen.type = COLORGEN_SLOPE;
		// in double: idVec3::Normalize overflows or underflows on components far from 1 and returns a
		// non-zero length with an infinite or NaN direction
		const double dx = numbers[0], dy = numbers[1], dz = numbers[2];
		const double length = sqrt( dx * dx + dy * dy + dz * dz );
		if( !( length > 1.0e-6 ) )
		{
			common->Warning( "material '%s': dmap_colorGen '%s' has no direction, ignored", material->GetName(), text );
			return false;
		}
		gen.dir.Set( ( float )( dx / length ), ( float )( dy / length ), ( float )( dz / length ) );
		gen.minDot = numbers[3];
		gen.maxDot = numbers[4];
		return true;
	}

	if( !name.Icmp( "volume" ) )
	{
		if( numNumbers != 0 )
		{
			common->Warning( "material '%s': dmap_colorGen '%s' takes no numbers after volume, ignored", material->GetName(), text );
			return false;
		}
		gen.type = COLORGEN_VOLUME;
		gen.dir.Zero();
		gen.minDot = 0.0f;
		gen.maxDot = 0.0f;
		return true;
	}

	common->Warning( "material '%s': dmap_colorGen '%s' is neither slope nor volume, ignored", material->GetName(), text );
	return false;
}

/*
====================
ColorMaterial

The generators of a material, parsed once a compile; numGens 0 = not coloured. Warns once
when the colour cannot show: the material is discrete (its surfaces are left out), or no
stage of it reads the vertex colour.
====================
*/
static colorMaterial_t ColorMaterial( const idMaterial* material )
{
	const int hash = ColorMaterialHashKey( material );
	for( int i = colorMaterialHash.First( hash ); i != -1; i = colorMaterialHash.Next( i ) )
	{
		if( colorMaterials[i].material == material )
		{
			return colorMaterials[i];
		}
	}

	colorMaterial_t entry;
	entry.material = material;
	entry.firstGen = colorGens.Num();
	entry.numGens = 0;

	const idList<dmapKeyword_t, TAG_MATERIAL>& args = material->GetDmapArgs();
	for( int i = 0; i < args.Num(); i++ )
	{
		colorGen_t gen;
		if( !args[i].keyword.Icmp( "dmap_colorGen" ) && ColorParseGenerator( material, args[i].args.c_str(), gen ) )
		{
			colorGens.Append( gen );
			entry.numGens++;
		}
	}

	if( entry.numGens > 0 )
	{
		bool readsColor = false;
		for( int s = 0; s < material->GetNumStages(); s++ )
		{
			if( material->GetStage( s )->vertexColor != SVC_IGNORE )
			{
				readsColor = true;
			}
		}
		if( material->IsDiscrete() )
		{
			common->Warning( "material '%s': dmap_colorGen on a discrete material, its surfaces are not coloured", material->GetName() );
		}
		else if( !readsColor )
		{
			common->Warning( "material '%s': dmap_colorGen, but no stage reads the vertex colour (vertexColor, inverseVertexColor): it does not show", material->GetName() );
		}
	}

	const int index = colorMaterials.Append( entry );
	colorMaterialHash.Add( hash, index );
	return entry;
}

/*
====================
ColorGroupMaterial

True when the group is written coloured: not discrete, and its material has a generator.
====================
*/
static bool ColorGroupMaterial( const optimizeGroup_t* group, colorMaterial_t& entry )
{
	if( group->material == NULL )
	{
		return false;
	}
	// resolved for discrete materials too, so their one warning that the surfaces are not coloured fires
	entry = ColorMaterial( group->material );
	return entry.numGens > 0 && !group->material->IsDiscrete();
}

/*
====================
ColorParseVolumes

Every brush of every func_vertexcolor, in entity and brush order. The map primitives of a
map-only entity live until dmap strips them after ProcessModels (dmap.cpp).
====================
*/
static void ColorParseVolumes()
{
	for( int i = 1; i < dmapGlobals.numEntities; i++ )
	{
		const uEntity_t* entity = &dmapGlobals.uEntities[i];
		if( !entity->dmapOnly || idStr::Icmp( entity->mapEntity->epairs.GetString( "classname" ), FUNC_VERTEXCOLOR_CLASSNAME ) != 0 )
		{
			continue;
		}

		const idMapEntity* mapEnt = entity->mapEntity;
		// a copy: va() is the text of a temporary, gone at the end of the statement
		const idStr name = mapEnt->epairs.GetString( "name", va( "entity %i", i ) );

		// the map writes only the keys it changes: the def holds the defaults, as at spawn
		idDict args = mapEnt->epairs;
		const idDeclEntityDef* def = static_cast<const idDeclEntityDef*>( declManager->FindType( DECL_ENTITYDEF, mapEnt->epairs.GetString( "classname" ), false ) );
		if( def != NULL )
		{
			args.SetDefaults( &def->dict );
		}

		// "r g b" or "r g b a", 0 to 1; alpha 1 when it is not given
		float rgba[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		const char* colorText = args.GetString( "_color", "1 1 1 1" );
		const int numColor = DressParseNumbers( colorText, rgba, 4 );
		if( numColor != 3 && numColor != 4 )
		{
			common->Warning( "func_vertexcolor '%s': _color '%s' is not r g b or r g b a, skipped", name.c_str(), colorText );
			continue;
		}
		if( numColor == 3 )
		{
			rgba[3] = 1.0f;
		}
		idVec4 color( rgba[0], rgba[1], rgba[2], rgba[3] );
		for( int c = 0; c < 4; c++ )
		{
			if( color[c] < 0.0f || color[c] > 1.0f )
			{
				common->Warning( "func_vertexcolor '%s': _color '%s' clamped to 0 to 1", name.c_str(), colorText );
				break;
			}
		}
		for( int c = 0; c < 4; c++ )
		{
			color[c] = idMath::ClampFloat( 0.0f, 1.0f, color[c] );
		}

		float falloff = args.GetFloat( "falloff", "0" );
		if( !( falloff >= 0.0f && falloff < 1.0e30f ) )
		{
			common->Warning( "func_vertexcolor '%s': falloff %g is not a distance from 0 up, using 0 (hard)", name.c_str(), falloff );
			falloff = 0.0f;
		}

		// the brushes are taken as the map has them: an editor's rotation of the entity is not applied
		if( mapEnt->epairs.FindKey( "rotation" ) != NULL || mapEnt->epairs.FindKey( "angle" ) != NULL || mapEnt->epairs.FindKey( "angles" ) != NULL )
		{
			common->Warning( "func_vertexcolor '%s': its rotation keys are ignored, the brushes colour where they are", name.c_str() );
		}

		const char* mode = args.GetString( "mode", "replace" );
		const bool add = !idStr::Icmp( mode, "add" );
		if( !add && idStr::Icmp( mode, "replace" ) != 0 )
		{
			common->Warning( "func_vertexcolor '%s': mode '%s' is neither replace nor add, using replace", name.c_str(), mode );
		}

		int numBrushes = 0;
		for( int p = 0; p < mapEnt->GetNumPrimitives(); p++ )
		{
			const idMapPrimitive* prim = mapEnt->GetPrimitive( p );
			if( prim->GetType() != idMapPrimitive::TYPE_BRUSH )
			{
				continue;
			}
			const idMapBrush* brush = static_cast<const idMapBrush*>( prim );
			// a Valve 220 origin brush only places the entity
			if( brush->IsOriginBrush() )
			{
				continue;
			}

			colorVolume_t volume;
			volume.firstPlane = colorVolumePlanes.Num();
			volume.numPlanes = brush->GetNumSides();
			volume.color = color;
			volume.falloff = falloff;
			volume.add = add;
			volume.bounds.Clear();
			for( int s = 0; s < brush->GetNumSides(); s++ )
			{
				// the map file holds a brush relative to its entity's "origin" key (idMapBrush::Parse):
				// back to map space
				idPlane plane = brush->GetSide( s )->GetPlane();
				plane[3] -= plane.Normal() * entity->origin;
				colorVolumePlanes.Append( plane );
			}

			// the bounds of the brush from its face windings, for a quick reject (idWinding: a face
			// may have more points than an idFixedWinding holds)
			for( int s = 0; s < volume.numPlanes; s++ )
			{
				idWinding w;
				w.BaseForPlane( colorVolumePlanes[volume.firstPlane + s] );
				for( int o = 0; o < volume.numPlanes && w.GetNumPoints() > 0; o++ )
				{
					if( o != s && !w.ClipInPlace( -colorVolumePlanes[volume.firstPlane + o], 0.0f ) )
					{
						break;
					}
				}
				for( int k = 0; k < w.GetNumPoints(); k++ )
				{
					volume.bounds.AddPoint( w[k].ToVec3() );
				}
			}
			if( volume.bounds.IsCleared() )
			{
				common->Warning( "func_vertexcolor '%s': brush %i has no volume, skipped", name.c_str(), p );
				colorVolumePlanes.SetNum( volume.firstPlane );
				continue;
			}
			volume.bounds.ExpandSelf( 1.0f );

			colorVolumes.Append( volume );
			numBrushes++;
		}
		if( numBrushes == 0 )
		{
			common->Warning( "func_vertexcolor '%s': no brush, it colours nothing", name.c_str() );
		}
	}
}

/*
====================
ColorVertex

The colour of one vertex of a coloured material: opaque white, then each generator in
order. position is in map space.
====================
*/
static dword ColorVertex( const colorMaterial_t& entry, const idVec3& position, const idVec3& normal )
{
	idVec4 c( 1.0f, 1.0f, 1.0f, 1.0f );

	for( int g = 0; g < entry.numGens; g++ )
	{
		const colorGen_t& gen = colorGens[entry.firstGen + g];
		if( gen.type == COLORGEN_SLOPE )
		{
			const float dot = normal * gen.dir;
			float w;
			if( gen.maxDot == gen.minDot )
			{
				w = ( dot >= gen.minDot ) ? 1.0f : 0.0f;
			}
			else
			{
				w = idMath::ClampFloat( 0.0f, 1.0f, ( dot - gen.minDot ) / ( gen.maxDot - gen.minDot ) );
			}
			c.x = w;
			c.y = w;
			c.z = w;
			continue;
		}

		for( int i = 0; i < colorGroupVolumes.Num(); i++ )
		{
			const colorVolume_t& volume = colorVolumes[colorGroupVolumes[i]];
			if( !volume.bounds.ContainsPoint( position ) )
			{
				continue;
			}

			// the least inside distance: negative outside the brush
			float depth = idMath::INFINITUM;
			for( int p = 0; p < volume.numPlanes; p++ )
			{
				depth = Min( depth, -colorVolumePlanes[volume.firstPlane + p].Distance( position ) );
			}

			float w;
			if( volume.falloff > 0.0f )
			{
				w = idMath::ClampFloat( 0.0f, 1.0f, depth / volume.falloff );
			}
			else
			{
				w = ( depth >= -COLOR_VOLUME_ON_EPSILON ) ? 1.0f : 0.0f;
			}
			if( w <= 0.0f )
			{
				continue;
			}

			if( volume.add )
			{
				for( int k = 0; k < 4; k++ )
				{
					c[k] = idMath::ClampFloat( 0.0f, 1.0f, c[k] + volume.color[k] * w );
				}
			}
			else
			{
				c.Lerp( c, volume.color, w );
			}
		}
	}

	return PackColorRound( c );
}

/*
====================
DressColorPostTjunction
====================
*/
void DressColorPostTjunction( uEntity_t* e )
{
	if( !dmap_colorGen.GetBool() )
	{
		return;
	}

	if( !colorVolumesRead )
	{
		colorVolumesRead = true;
		ColorParseVolumes();
	}

	int numGroups = 0;
	int numVerts = 0;
	for( int a = 0; a < e->numAreas; a++ )
	{
		for( optimizeGroup_t* group = e->areas[a].groups; group; group = group->nextGroup )
		{
			colorMaterial_t entry;
			if( !ColorGroupMaterial( group, entry ) )
			{
				continue;
			}

			// only the volumes whose bounds meet the group's can colour one of its vertices (same order, same colours)
			idBounds groupBounds;
			groupBounds.Clear();
			for( const mapTri_t* tri = group->triList; tri; tri = tri->next )
			{
				for( int k = 0; k < 3; k++ )
				{
					groupBounds.AddPoint( tri->v[k].xyz + e->origin );
				}
			}
			colorGroupVolumes.SetNum( 0 );
			for( int v = 0; v < colorVolumes.Num(); v++ )
			{
				if( colorVolumes[v].bounds.IntersectsBounds( groupBounds ) )
				{
					colorGroupVolumes.Append( v );
				}
			}

			for( mapTri_t* tri = group->triList; tri; tri = tri->next )
			{
				for( int k = 0; k < 3; k++ )
				{
					tri->v[k].SetColor( ColorVertex( entry, tri->v[k].xyz + e->origin, tri->v[k].GetNormal() ) );
					numVerts++;
				}
			}
			group->vertexColor = true;
			numGroups++;
		}
	}

	// the world's line always, a brush model's in verbose mode only: a map can hold thousands of them
	if( numGroups > 0 && dmapGlobals.entityNum == 0 )
	{
		common->Printf( "dmap color: %i groups %i verts %i volumes\n", numGroups, numVerts, colorVolumes.Num() );
	}
	else if( numGroups > 0 )
	{
		common->VerbosePrintf( "dmap color (entity %i): %i groups %i verts %i volumes\n", dmapGlobals.entityNum, numGroups, numVerts, colorVolumes.Num() );
	}
}
