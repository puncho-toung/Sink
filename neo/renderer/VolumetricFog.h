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

#ifndef __VOLUMETRICFOG_H__
#define __VOLUMETRICFOG_H__

/*
==========================================================================================

THE FROXEL FOG'S FRONTEND ( track V of the renderer upgrade )

The froxel fog's inject ( renderer/Passes/VolumetricFogPass.cpp ) reads the view's light records, Z-bins and tile
bits ( renderer/LightLists.h ). What the frontend and the pass share about them, and the hooks that the light
lists' frontend ( tr_frontend_lightlists.cpp ) calls from its blocks fenced TRACK V: the fog's rules stand here and
in tr_frontend_volumetricfog.cpp, and the light lists' file holds a call to each. R_VolumetricFogRecordReach is
defined beside the fog's cvars, in the pass. Include this file behind RenderCommon.h.

==========================================================================================
*/

// renderLight_t::volumetricMode ( the spawnarg "volumetric" ): a designer says whether a light lights the froxel fog.
// The fog has no shadow of a light without a shadow map, so by default it takes a light only where the shadow is
// known ( D80 ): where the light's shadow map has a place in the atlas, or where the light has no caster at all
enum lightVolumetricMode_t
{
	LIGHT_VOLUMETRIC_AUTO	= 0,			// only where the light's shadow is known
	LIGHT_VOLUMETRIC_ALWAYS	= 1,			// also where it has no shadow map ( RLF_FOG_NOSHADOW ): the opt-in of a light that casts no shadows; its fog shines through walls
	LIGHT_VOLUMETRIC_NEVER	= 2				// the light adds nothing to the fog ( RLF_FOG_OFF )
};

// How far along the view axis the fog's inject reads a view's light records; 0 where it reads none ( the fog off,
// air that scatters nothing, the furnace ). Defined beside the fog's cvars, in renderer/Passes/VolumetricFogPass.cpp
float	R_VolumetricFogRecordReach();

// Whether the fog can take a record's light at all: not a fog light ( a medium: V4 ), not a light switched off for
// the fog, not a light of another spectrum ( it lights no surface of the default one either ), and a light that
// needs no shadow ( ambient, or flagged ) or can have a shadow map. hasShadowRows is that last fact as the caller
// knows it: the frontend from the light's shadow LOD, the backend from the record's rows ( a light with no caster
// has rows too: its fade is in them ). Whether a light with rows is taken at a sample, and how much of it, is the
// row's to say ( FroxelShadowShare, shaders/builtin/volumetrics/froxel_lights.inc.hlsl )
ID_INLINE bool R_FogCanTakeRecord( const recordLight_t& rec, bool hasShadowRows )
{
	return ( rec.flags & ( RLF_FOGLIGHT | RLF_FOG_OFF ) ) == 0 && ( rec.channelMask & 1u ) != 0
		   && ( ( rec.flags & ( RLF_AMBIENT | RLF_FOG_NOSHADOW ) ) != 0 || hasShadowRows );
}

// V4: a fog light's record ( RLF_FOGLIGHT, read the fog pass's way, D81 ) that the inject can take as a medium: not
// switched off for the fog and of the default spectrum. The inject takes them only while R_VolumetricFogTakesFogLights
// says so ( r_volumetricFogLights on V4's path, defined beside the fog's cvars )
ID_INLINE bool R_FogCanTakeFogLight( const recordLight_t& rec )
{
	return ( rec.flags & RLF_FOGLIGHT ) != 0 && ( rec.flags & RLF_FOG_OFF ) == 0 && ( rec.channelMask & 1u ) != 0;
}
// V4: a fog light's density rises from 0 at its fog plane to full this many units below it: the fog pass's enter ramp,
// 8 texels of its enter image ( RAMP_RANGE ) at FOG_SCALE 0.001 and FOG_ENTER_SIZE 64 texels a unit of the scale. V5:
// also the frontend's portal distance ( R_VolumetricFogPortalDistance )
static const float FROXEL_FOGLIGHT_RAMP	= 125.0f;
bool	R_VolumetricFogTakesFogLights();
bool	R_VolumetricFogSwapsFogLights();		// V5: the cvars under which the froxel fog takes fog lights in place of the fog pass
float	R_VolumetricFogLightRate();				// V5: r_volumetricFogLightRate, the fog light's sigma_t times its distance
// V5 ( D21, D29 ): the fog light of a view light that the froxel fog took in this view: its record, the light's own
// ( viewLight_t::recordNumber; record 0 is the sun's slot ), still on for the fog. FogAllLights skips the fog pass for it
ID_INLINE bool R_FroxelFogTookFogLight( const viewDef_t* viewDef, const viewLight_t* vLight )
{
	return viewDef->recordLights != NULL && vLight->recordNumber > 0 && vLight->recordNumber < Min( viewDef->numRecordLights, MAX_RECORD_LIGHTS )
		   && R_FogCanTakeFogLight( viewDef->recordLights[ vLight->recordNumber ] );
}

// V5 ( row 66 ): a light that lights only the fog in this view: kept lit for its fog ( fogRelevant ) with no surface that
// lists it ( the forward pass ) and no interaction of the legacy pass. The shadow atlas counts its places. A fog light is
// never one ( it keeps its interactions for the fog pass, and the atlas gives fog lights no place )
ID_INLINE bool R_ViewLightLightsOnlyFog( const viewLight_t* vLight )
{
	return vLight->fogRelevant && vLight->numListedSurfs == 0 && vLight->localInteractions == NULL
		   && vLight->globalInteractions == NULL && vLight->translucentInteractions == NULL;
}
bool	R_VolumetricFogMediaScatter();		// V4: the global fog or a medium of the media pass scatters light ( defined beside the fog's cvars )
// V4: whether a fog light the inject takes scatters light ( its albedo, ids.w, above 0 ). Where nothing scatters, the lights
// are not taken at all: a fog light at albedo 0 only darkens and shines ( review O1-2 ). Record 0, the sun's slot, is never a
// fog light the inject takes ( review QR3-1 )
ID_INLINE bool R_FogLightsScatter( const recordLight_t* records, int numRecords )
{
	for( int r = 1; r < numRecords; r++ )
	{
		if( R_FogCanTakeFogLight( records[r] ) && records[r].ids.w > 0.0f )
		{
			return true;
		}
	}
	return false;
}

// The fog's own data of a view ( viewDef_t::froxelView, X0's seam: frame memory, NULL until a hook needs it ). The
// caster boxes: where a caster of a record's light that changed this frame shadows, up to FOG_CASTER_BOXES boxes by
// record ( the casters past them merge into the box they grow least ), in the relative frame. Box i of record r is
// ( min, max ) at casterBoxes[ 2 * ( r * FOG_CASTER_BOXES + i ) ], and the w of the record's first min is its count;
// R_VolumetricFogShadowFlags fills them, the pass uploads them, and the inject reads them where the record has
// RLF_FOG_CASTERMOVED ( FroxelInCasterBoxes )
const int FOG_CASTER_BOXES = 4;			// FROXEL_CASTER_BOXES of shaders/builtin/volumetrics/froxel_lights.inc.hlsl must be the same
struct froxelView_t
{
	idVec4*		casterBoxes;		// [ records * 2 * FOG_CASTER_BOXES ] or NULL
};

// The hooks, in the order the light lists' frontend calls them ( tr_frontend_volumetricfog.cpp )
float	R_VolumetricFogBeginView( const viewDef_t* viewDef );									// R_NumberRecordLights: the fog's switch; returns the reach, 0 = the fog reads no records in this view
uint32	R_VolumetricFogRecordFlags( const viewLight_t* vLight, uint32 recordFlags );			// R_LightListFlattenRecord: the fog's bits of a record
void	R_VolumetricFogLightRecord( const viewLight_t* vLight, uint32 recordFlags, recordLight_t& rec );	// R_LightListFlattenRecord, V4: a fog light's albedo ( ids.w ), its fog plane and its alpha's share ( the emitter lanes )
void	R_VolumetricFogMarkLights( lightListView_t* llv, recordLight_t* records, float fogReach );	// R_NumberRecordLights: viewLight_t::fogRelevant; V5: a fog light beyond the fog's reach keeps the fog pass
bool	R_VolumetricFogMayTakeFogLight( const idRenderLightLocal* ldef, float alpha );			// V5: the verdict but for the reach, in the view being built ( tr.viewDef )
float	R_VolumetricFogPortalDistance( const idRenderLightLocal* ldef, const idWinding* w, float alpha );	// V5: PortalIsFoggedOut's distance for a fog light; 0 = never cull
uint32	R_VolumetricFogShadowFlags( viewDef_t* viewDef, int record, int numRecords, const viewLight_t* vLight );	// R_BuildLightLists: the fog's shadow bits of a record, once the chains are final; fills froxelView_t::casterBoxes
void	R_VolumetricFogCasterChanged( const idRenderEntityLocal* def );						// idRenderWorldLocal::UpdateEntityDef, FreeEntityDef: where a caster stood before it changed or went
void	R_VolumetricFogRecordCap( const lightListView_t* llv );									// R_BuildLightLists: the record cap, in the fog's words

#endif // __VOLUMETRICFOG_H__
