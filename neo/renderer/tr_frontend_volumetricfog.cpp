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
#include "VolumetricFog.h"

/*
==========================================================================================

THE FROXEL FOG'S FRONTEND ( track V of the renderer upgrade )

What the frontend does for the froxel fog's inject, which reads the view's light records, Z-bins and tile bits. The
light lists' frontend ( tr_frontend_lightlists.cpp ) builds them and calls each function below from a block fenced
TRACK V ( renderer/VolumetricFog.h ).

==========================================================================================
*/

static bool fogCapWarned = false;				// R_VolumetricFogRecordCap: this map has had the warning

// the casters that changed or went this frame, where each stood before its first change of the frame, in its world
// ( R_VolumetricFogCasterChanged )
struct fogChangedCaster_t
{
	idBounds					bounds;
	const idRenderWorldLocal*	world;
	int							index;		// the def's index in its world
};
static const int			FOG_MAX_CHANGED_CASTERS = 512;
static fogChangedCaster_t	fogChangedCasters[FOG_MAX_CHANGED_CASTERS];
static int					fogNumChangedCasters = 0;
static int			fogChangedFrame = -1;		// the tr.frameCount the list is of
static bool			fogChangedOverflow = false;	// more casters changed this frame than the list holds

/*
====================
R_VolumetricFogBeginView

The fog's switch ( D40 ), at the start of R_NumberRecordLights, for every view: how far along the view axis the fog's
inject reads this view's light records, 0 where it reads none. The fog runs in the main view without
RDF_IRRADIANCE, RDF_NOAMBIENT and RDF_NOFOG, and reads records while R_VolumetricFogRecordReach says so: the fog on,
air that scatters, no furnace. In the view the fog runs in, the records, the bins and the tile bits are then built
with the lists off as well; they list nothing ( lightListView_t::lists is the lists' own switch ). The first main
view of a map clears the fog's warnings.
====================
*/
float R_VolumetricFogBeginView( const viewDef_t* viewDef )
{
	if( viewDef->invalidateHistory )
	{
		fogCapWarned = false;
	}
	return R_LightListFogEligible( viewDef ) ? R_VolumetricFogRecordReach() : 0.0f;
}

/*
====================
R_VolumetricFogRecordFlags

The fog's bits of a light's record, OR'd into it by R_LightListFlattenRecord; recordFlags are the record's flags as
R_LightListRecordFlags decided them. An ambient record carries the kind's bit, which says which of the two default
falloff images the inject reads for it ( the stock ambient light material has the point light's ); the record is the
fog's alone and is never listed. The light's own word on the fog is the spawnarg "volumetric"
( renderLight_t::volumetricMode ); any other value reads as LIGHT_VOLUMETRIC_AUTO.
====================
*/
uint32 R_VolumetricFogRecordFlags( const viewLight_t* vLight, uint32 recordFlags )
{
	uint32 flags = 0;
	if( ( recordFlags & RLF_AMBIENT ) != 0 && vLight->pointLight )
	{
		flags |= RLF_POINT;
	}
	const int mode = vLight->lightDef->parms.volumetricMode;
	if( mode == LIGHT_VOLUMETRIC_ALWAYS )
	{
		flags |= RLF_FOG_NOSHADOW;
	}
	else if( mode == LIGHT_VOLUMETRIC_NEVER )
	{
		flags |= RLF_FOG_OFF;
	}
	return flags;
}

/*
====================
R_VolumetricFogMarkLights

A light the inject can take counts as lit ( R_ViewLightIsLit ), so one whose surfaces are all out of sight keeps its
place in the view and its shadow maps: its fog is in sight. Marked: a light the fog can take ( R_FogCanTakeRecord,
with "can have a shadow map in this view" from its shadow LOD ) that begins nearer than the fog's far end. Called
while the fog reads the view's records; a view light the fog does not mark keeps the false it was made with
( R_ClearedFrameAlloc ).
====================
*/
void R_VolumetricFogMarkLights( lightListView_t* llv, const recordLight_t* records, float fogReach )
{
	const lightListTables_t& t = llv->tables;
	for( int r = 0; r < t.numRecords; r++ )
	{
		const int input = t.lightOfRecord[r];
		if( input < 0 )
		{
			continue;														// the empty sun slot
		}
		viewLight_t* vLight = llv->lightOfInput[input];
		vLight->fogRelevant = llv->lights[input].zMin < fogReach && R_FogCanTakeRecord( records[r], vLight->shadowLOD != -1 );
	}
}

/*
====================
R_FogShadowRegion

A box that holds the shadow a caster's box casts inside a light's volume, cleared where it casts none there.
R_ShadowBounds, the cull's box, runs to the volume's far bound on every axis where the light is not beyond the caster,
and for a parallel light, whose origin stands PARALLEL_LIGHT_FAKE_DISTANCE away, on nearly every axis. Here a parallel
light's shadow is the caster's box swept along the light's direction until it has left the volume through some face
( the least sweep over the three axes: a low sun's shadow leaves through the floor ), and any other light's the box
scaled from the light's origin until it has left the volume ( the least scale over the axes on which the box lies wholly
to one side of the origin ); a box that holds the origin shadows the whole volume.
====================
*/
static idBounds R_FogShadowRegion( const idBounds& caster, const idRenderLightLocal* light )
{
	const idBounds& volume = light->globalLightBounds;
	idBounds region = caster;
	if( light->parms.parallel )
	{
		idVec3 away = light->parms.origin - light->globalLightOrigin;		// the way the light's shadows go
		away.Normalize();
		float sweep = idMath::INFINITUM;
		for( int i = 0; i < 3; i++ )
		{
			if( away[i] > 1e-6f )
			{
				sweep = Min( sweep, ( volume[1][i] - caster[0][i] ) / away[i] );
			}
			else if( away[i] < -1e-6f )
			{
				sweep = Min( sweep, ( caster[1][i] - volume[0][i] ) / -away[i] );
			}
		}
		if( sweep > 0.0f && sweep < idMath::INFINITUM )
		{
			region.AddBounds( idBounds( caster[0] + away * sweep, caster[1] + away * sweep ) );
		}
	}
	else
	{
		const idVec3& origin = light->globalLightOrigin;
		float scale = idMath::INFINITUM;
		for( int i = 0; i < 3; i++ )
		{
			if( caster[0][i] > origin[i] )
			{
				scale = Min( scale, ( volume[1][i] - origin[i] ) / ( caster[0][i] - origin[i] ) );
			}
			else if( caster[1][i] < origin[i] )
			{
				scale = Min( scale, ( origin[i] - volume[0][i] ) / ( origin[i] - caster[1][i] ) );
			}
		}
		if( scale == idMath::INFINITUM )
		{
			return volume;
		}
		if( scale > 1.0f )
		{
			region.AddBounds( idBounds( origin + ( caster[0] - origin ) * scale, origin + ( caster[1] - origin ) * scale ) );
		}
	}
	if( !region.IntersectsBounds( volume ) )
	{
		region.Clear();
		return region;
	}
	region.IntersectSelf( volume );
	return region;
}

/*
====================
R_FogAddCasterBox

One more caster's region among a record's boxes: none where the view does not see it, a box of its own while there are
fewer than FOG_CASTER_BOXES, else merged into the box it grows least
====================
*/
static void R_FogAddCasterBox( const viewDef_t* viewDef, idBounds* boxes, int& numBoxes, const idBounds& region )
{
	if( region.IsCleared() || idRenderMatrix::CullBoundsToMVP( viewDef->worldCullMVP, region ) )
	{
		return;			// no shadow in the volume, or none the view sees
	}
	if( numBoxes < FOG_CASTER_BOXES )
	{
		boxes[numBoxes++] = region;
		return;
	}
	int best = 0;
	float bestGrowth = idMath::INFINITUM;
	for( int i = 0; i < FOG_CASTER_BOXES; i++ )
	{
		idBounds merged = boxes[i];
		merged.AddBounds( region );
		const float growth = merged.GetVolume() - boxes[i].GetVolume();
		if( growth < bestGrowth )
		{
			bestGrowth = growth;
			best = i;
		}
	}
	boxes[best].AddBounds( region );
}

/*
====================
R_VolumetricFogShadowFlags

The fog's shadow bits of a light's record, OR'd into it by R_BuildLightLists once the view's chains are final. A
light that casts shadows and has no caster in this view's chain, which holds every caster whose shadow can reach the
view ( R_AddSingleLight leaves out one whose shadow bounds miss the view's frustum ), gets no shadow map
( R_ViewLightHasShadowMap ): its rows read "not placed". Nothing shadows it inside the view, so the inject takes it
unshadowed; and like every other light that casts shadows it takes it times 1 - fade, the fade to unshadowed on its
last shadow LOD, and not at all at a shadow LOD of -1, where it has no rows ( the owner's row 50: a light with no
caster fades like its neighbours, so a lamp's fog does not change when a caster enters its box ). R_BuildShadowRows
writes the fade into its rows. Only at r_shadowCache 2 does such a light have a place in the atlas: the cache's
static map, whose casters' shadows all miss the view; the inject does not read it. The chain is gathered for every
light that casts shadows, whatever its shadow LOD ( R_AddSingleModel ), and it is final when this is called: the model
jobs have run, and the optimize pass, which empties the chain of a light that is not lit, runs behind it.
RLF_FOG_NOCASTER is a fact about the record, set whether or not the fog is on; RLF_FOG_CASTERMOVED and the record's
caster boxes ( froxelView_t ) are made only while the fog reads records.
====================
*/
uint32 R_VolumetricFogShadowFlags( viewDef_t* viewDef, int record, int numRecords, const viewLight_t* vLight )
{
	if( vLight->shadowLOD == -1 || !vLight->lightDef->LightCastsShadows() )
	{
		return 0;			// no shadow in the fog: what a caster does changes nothing of it
	}
	if( vLight->globalShadows == NULL )
	{
		return RLF_FOG_NOCASTER;
	}
	if( !R_LightListFogEligible( viewDef ) || R_VolumetricFogRecordReach() <= 0.0f )
	{
		return 0;			// the fog does not run in this view, or reads no records: no history to clip
	}
	// V3: a caster that changed this frame ( an entity that moved or animated, or went ) changes the light's shadow,
	// which the history cannot follow by the light's colour. The inject clips the history where that caster shadows:
	// the region of its shadow where it stands now ( the chain's casters whose def changed this frame; a light's chain
	// holds an entity's surfaces in a run, R_AddModels ) and where it stood ( R_VolumetricFogCasterChanged )
	const idRenderLightLocal* light = vLight->lightDef;
	idBounds boxes[FOG_CASTER_BOXES];
	int numBoxes = 0;
	const idRenderEntityLocal* last = NULL;
	for( const drawSurf_t* surf = vLight->globalShadows; surf != NULL; surf = surf->nextOnLight )
	{
		const idRenderEntityLocal* def = ( surf->space != NULL ) ? surf->space->entityDef : NULL;
		if( def == NULL || def == last )
		{
			continue;
		}
		last = def;
		if( def->lastModifiedFrameNum == tr.frameCount )
		{
			R_FogAddCasterBox( viewDef, boxes, numBoxes, R_FogShadowRegion( def->globalReferenceBounds, light ) );
		}
	}
	if( fogChangedFrame == tr.frameCount )
	{
		if( fogChangedOverflow )
		{
			numBoxes = 1;
			boxes[0] = light->globalLightBounds;			// more casters changed than the list holds: the whole volume
		}
		else
		{
			for( int i = 0; i < fogNumChangedCasters; i++ )
			{
				if( fogChangedCasters[i].world == viewDef->renderWorld && fogChangedCasters[i].bounds.IntersectsBounds( light->globalLightBounds ) )
				{
					R_FogAddCasterBox( viewDef, boxes, numBoxes, R_FogShadowRegion( fogChangedCasters[i].bounds, light ) );
				}
			}
		}
	}
	if( numBoxes == 0 )
	{
		return 0;
	}
	// the record's boxes, in the relative frame, made for the view when its first record has any ( frame memory,
	// cleared )
	if( viewDef->froxelView == NULL )
	{
		viewDef->froxelView = ( froxelView_t* )R_ClearedFrameAlloc( sizeof( froxelView_t ), FRAME_ALLOC_UNKNOWN );
	}
	if( viewDef->froxelView->casterBoxes == NULL )
	{
		viewDef->froxelView->casterBoxes = ( idVec4* )R_ClearedFrameAlloc( numRecords * 2 * FOG_CASTER_BOXES * sizeof( idVec4 ), FRAME_ALLOC_UNKNOWN );
	}
	idVec4* out = viewDef->froxelView->casterBoxes + record * 2 * FOG_CASTER_BOXES;
	for( int i = 0; i < numBoxes; i++ )
	{
		for( int j = 0; j < 2; j++ )
		{
			out[2 * i + j].Set( ( float )( boxes[i][j].x - viewDef->renderOrigin.x ), ( float )( boxes[i][j].y - viewDef->renderOrigin.y ),
								( float )( boxes[i][j].z - viewDef->renderOrigin.z ), ( i == 0 && j == 0 ) ? ( float )numBoxes : 0.0f );
		}
	}
	return RLF_FOG_CASTERMOVED;
}

/*
====================
R_VolumetricFogCasterChanged

idRenderWorldLocal::UpdateEntityDef and FreeEntityDef call it, beside the shadow cache's notify, before an entity's
def changes or goes: on the game thread, in the frame whose frontend then builds the records ( both see the same
tr.frameCount ). It keeps where a caster stood, whose shadow the fog's history holds: a caster that went, or moved out
of a light's volume, has no surface left in the light's chain. UpdateEntityDef calls it as well where an entity changes
in place ( an animation whose bounds, origin and axis stay: the shortcut that does not mark the def ). A def is kept
once a frame, where it stood first, and with its world, so that a GUI's world adds nothing to a view's. A def whose
shadow the main view does not draw is left out ( noShadow; the player's own body, suppressShadowInViewID ).
====================
*/
void R_VolumetricFogCasterChanged( const idRenderEntityLocal* def )
{
	if( fogChangedFrame != tr.frameCount )
	{
		fogChangedFrame = tr.frameCount;
		fogNumChangedCasters = 0;
		fogChangedOverflow = false;
	}
	if( fogChangedOverflow || def->parms.noShadow || def->parms.suppressShadowInViewID != 0 || def->globalReferenceBounds.IsCleared()
			|| R_VolumetricFogRecordReach() <= 0.0f )
	{
		return;
	}
	for( int i = 0; i < fogNumChangedCasters; i++ )
	{
		if( fogChangedCasters[i].index == def->index && fogChangedCasters[i].world == def->world )
		{
			return;
		}
	}
	if( fogNumChangedCasters == FOG_MAX_CHANGED_CASTERS )
	{
		fogChangedOverflow = true;
		return;
	}
	fogChangedCaster_t& caster = fogChangedCasters[fogNumChangedCasters++];
	caster.bounds = def->globalReferenceBounds;
	caster.world = def->world;
	caster.index = def->index;
}

/*
====================
R_VolumetricFogRecordCap

The light lists' warnings speak of the lists ( R_LightListLoud ). A view that builds its tables for the fog alone
lists nothing; of the record cap it says its own word, once a map: the lights past the cap have no record, so they
add nothing to the fog.
====================
*/
void R_VolumetricFogRecordCap( const lightListView_t* llv )
{
	if( !llv->lists && llv->stats.capped > 0 && !fogCapWarned )
	{
		fogCapWarned = true;
		common->Warning( "lightlists: %d lights past the %d records of a view add nothing to the froxel fog", llv->stats.capped, LL_MAX_RECORDS );
	}
}
