/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

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

#ifndef __INTERACTION_H__
#define __INTERACTION_H__

/*
===============================================================================

	Interaction between static entityDef surfaces and a static lightDef.

	Interactions with no lightTris and no shadowTris are still
	valid, because they show that a given entityDef / lightDef
	do not interact, even though they share one or more areas.

===============================================================================
*/

#define LIGHT_CULL_ALL_FRONT		((byte *)-1)
#define	LIGHT_CLIP_EPSILON			0.1f

// enabling this define allows the precise inside shadow volume test
// to be performed on interaction (static) shadow volumes
#define KEEP_INTERACTION_CPU_DATA

struct srfCullInfo_t
{
	// For each triangle a byte set to 1 if facing the light origin.
	byte* 					facing;

	// For each vertex a byte with the bits [0-5] set if the
	// vertex is at the back side of the corresponding clip plane.
	// If the 'cullBits' pointer equals LIGHT_CULL_ALL_FRONT all
	// vertices are at the front of all the clip planes.
	byte* 					cullBits;

	// Clip planes in surface space used to calculate the cull bits.
	idPlane					localClipPlanes[6];
};


// ---- TRACK F: each surface keeps its own lights ( F7, row 197 ( b ) ) ----
// Until F7 an interaction held one slot for every surface of its entity, so a light that touched one chunk of a world
// area paid a slot for every chunk of it, and the frame's walk visited every surface for every light. Now each surface
// of an entity keeps the list of the lights that reach it ( entitySurfLights_t on the entity, made and freed in
// Interaction.cpp ), and the interaction keeps the numbers of the surfaces it put itself into, to take itself out again.
// Pre-generated shadow volumes from dmap are not present here; they are added separately.
struct surfLightRef_t						// 16 bytes: one light that reaches one surface
{
	int						lightIndex;		// idRenderLightLocal::index
	// The vertexes for light tris will always come from ambient triangles.
	// For interactions created at load time, the indexes will be uniquely
	// generated in static vertex memory.
	int						numLightTrisIndexes;
	vertCacheHandle_t		lightTrisIndexCache;
};

struct surfLightList_t						// 16 bytes: the lights of one surface, in the order they were put in
{
	surfLightRef_t*			refs;			// static memory, grows by doubling from four
	int						num;
	int						max;
};

struct entitySurfLights_t					// idRenderEntityLocal::surfLights, while any surface of the entity has a light
{
	int						numSurfaces;	// of the model the static interactions were made from
	int						numRefs;		// over every list; the table is freed when it falls to zero
	surfLightList_t			lists[1];		// numSurfaces of them
};
// ---- END TRACK F ----


class idRenderEntityLocal;
class idRenderLightLocal;
class RenderEnvprobeLocal;

class idInteraction
{
public:
	// this may be 0 if the light and entity do not actually intersect
	// -1 = an untested interaction
	int						numSurfaces;

	// track F ( F7 ): the surfaces of entityDef this light reaches, ascending; each holds this light in its list of
	// entityDef->surfLights. NULL when the interaction is not static or reaches no surface ( the count first, beside
	// numSurfaces: the class keeps its 72 bytes, F7i, review O1-4 )
	int						numLitSurfaces;
	int*					litSurfaces;

	// get space from here, if NULL, it is a pre-generated shadow volume from dmap
	idRenderEntityLocal* 	entityDef;
	idRenderLightLocal* 	lightDef;

	idInteraction* 			lightNext;				// for lightDef chains
	idInteraction* 			lightPrev;
	idInteraction* 			entityNext;				// for entityDef chains
	idInteraction* 			entityPrev;

	bool					staticInteraction;		// true if the interaction was created at map load time in static buffer space

public:
	idInteraction();

	// because these are generated and freed each game tic for active elements all
	// over the world, we use a custom pool allocater to avoid memory allocation overhead
	// and fragmentation
	static idInteraction* 	AllocAndLink( idRenderEntityLocal* edef, idRenderLightLocal* ldef );

	// unlinks from the entity and light, takes this light out of the entity's surface lists ( F7 ),
	// and puts it back on the free list
	void					UnlinkAndFree();

	// free the interaction surfaces
	void					FreeSurfaces();

	// makes the interaction empty for when the light and entity do not actually intersect
	// all empty interactions are linked at the end of the light's and entity's interaction list
	void					MakeEmpty();

	// returns true if the interaction is empty
	bool					IsEmpty() const
	{
		return ( numSurfaces == 0 );
	}

	// returns true if the interaction is not yet completely created
	bool					IsDeferred() const
	{
		return ( numSurfaces == -1 );
	}

	// returns true if the interaction has shadows
	bool					HasShadows() const;

	// called by GenerateAllInteractions
	void					CreateStaticInteraction( nvrhi::ICommandList* commandList );

private:
	// unlink from entity and light lists
	void					Unlink();
};

void R_ShowInteractionMemory_f( const idCmdArgs& args );

#endif /* !__INTERACTION_H__ */
