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

  Compile-time decals: every func_decal of the map is a projector that dmap bakes
  into the world geometry, with the same projection maths as the runtime decals
  (idDecalProjection).

  The pass runs pre-optimize (between PutPrimitivesInAreas and Prelight), so the
  light carving, the optimizer and the T-junction fix treat the fragments like any
  other map geometry. Decal materials are discrete (DECAL_MACRO), so the fragments
  never snap to the 1/32 grid and never split the world's edges; a material that is
  not discrete would do both, so the projector warns and bakes nothing.

  Targets are the optimize groups of the world entity whose material allows
  overlays: the brushes, patches and meshes of the world and the inlined
  func_static models. Movers and func_statics that are not inlined are other
  entities and are skipped by construction; the runtime decals cover them.

  Every projector is clipped against the world's tris before any fragment is
  inserted (the dress contract in dress.cpp): AddMapTriToAreas prepends the groups
  it creates to the lists being walked, and a decal must never land on another
  decal's fragments.

  Each fragment is split at the far fade plane only. The band behind it and the
  hard part in front of it get different merge groups, so the optimizer keeps the
  knee vertices; DressDecalsPostTjunction gives the band groups the projector's
  merge group again, so each projector becomes one surface.

  DressDecalsPostTjunction also writes the fade as vertex colour: opaque white with
  alpha from the depth to the far fade plane, a pure function of position, so it
  does not matter how the optimizer re-triangulated the band. It runs after the
  T-junction fix, the last pass that moves or adds vertices, so the colour is final.
  A projector with fade 0 writes no colour and its surface stays without colour in
  the .proc.

*/

typedef struct
{
	int						entityNum;		// into dmapGlobals.uEntities
	decalProjectionParms_t	parms;			// world space
	int						numFragments;
	int						numCorners;		// tri corners coloured by DressDecalsPostTjunction
	int						numBandCorners;	// of those, the ones with alpha below 1
} decalProjector_t;

// the projectors of the map being compiled. Filled by DressDecalsPreOptimize and not
// changed until the next compile resets it there: the addresses of its elements are the
// merge groups of the decal tris, so they live through output like every other merge
// group target (the brush sides and model surfaces of the discrete materials)
static idList<decalProjector_t>	decalProjectors;

/*
====================
DecalName

The entity's name for the messages, "entity <n>" for an unnamed func_decal.
====================
*/
static idStr DecalName( int entityNum )
{
	const char* name = dmapGlobals.uEntities[entityNum].mapEntity->epairs.GetString( "name" );
	if( name[0] != '\0' )
	{
		return name;
	}
	return va( "entity %i", entityNum );
}

/*
====================
DecalProjectorForMergeGroup

The index of the projector whose hard part (band = false) or fade band (band = true)
has this merge group, -1 for every other merge group. O(1): the post-t-junction pass
asks for every group of the world, and the two merge groups of a projector are its
address and the address one byte into it, so the index follows from the offset.
====================
*/
static int DecalProjectorForMergeGroup( const void* mergeGroup, bool& band )
{
	if( decalProjectors.Num() == 0 )
	{
		return -1;
	}

	const byte* first = ( const byte* )&decalProjectors[0];
	const byte* group = ( const byte* )mergeGroup;
	if( group < first || group > ( const byte* )&decalProjectors[decalProjectors.Num() - 1] + 1 )
	{
		return -1;
	}

	const size_t offset = ( size_t )( group - first );
	const size_t inside = offset % sizeof( decalProjector_t );
	if( inside > 1 )
	{
		return -1;
	}
	band = ( inside == 1 );
	return ( int )( offset / sizeof( decalProjector_t ) );
}

/*
====================
ParseDecalProjector

Builds the projector of one func_decal: a box 'depth' deep centred on the origin along
the entity's forward axis, 'size' wide and high on the plane through the origin,
projecting toward -forward. Warns and returns false when the entity cannot bake.
====================
*/
static bool ParseDecalProjector( int entityNum, decalProjector_t& projector )
{
	const idMapEntity* mapEnt = dmapGlobals.uEntities[entityNum].mapEntity;
	const idStr name = DecalName( entityNum );

	// the map writes only the keys it changes: the def holds the defaults, as at spawn
	idDict args = mapEnt->epairs;
	const idDeclEntityDef* def = static_cast<const idDeclEntityDef*>( declManager->FindType( DECL_ENTITYDEF, mapEnt->epairs.GetString( "classname" ), false ) );
	if( def != NULL )
	{
		args.SetDefaults( &def->dict );
	}

	const char* materialName = args.GetString( "material" );
	if( materialName[0] == '\0' )
	{
		common->Warning( "func_decal '%s': no material", name.c_str() );
		return false;
	}
	const idMaterial* material = declManager->FindMaterial( materialName );
	if( !material->IsDiscrete() )
	{
		common->Warning( "func_decal '%s': material '%s' is not discrete (use DECAL_MACRO), skipped", name.c_str(), material->GetName() );
		return false;
	}

	const idVec2 size = args.GetVec2( "size", "64 64" );
	if( size.x <= 0.0f || size.y <= 0.0f )
	{
		common->Warning( "func_decal '%s': size %g %g is not positive, skipped", name.c_str(), size.x, size.y );
		return false;
	}
	const float depth = args.GetFloat( "depth", "32" );
	if( depth <= 0.0f )
	{
		common->Warning( "func_decal '%s': depth %g is not positive, skipped", name.c_str(), depth );
		return false;
	}
	const float halfDepth = depth * 0.5f;

	// the far cap is depth / 2 behind the origin, so a longer fade would put the knee in
	// front of the origin plane and dim the decal on the surface it is placed on
	const float mapFade = args.GetFloat( "fade", "8" );
	const float fade = idMath::ClampFloat( 0.0f, halfDepth, mapFade );
	if( fade != mapFade )
	{
		common->Warning( "func_decal '%s': fade %g clamped to %g (0 to depth/2 = %g)", name.c_str(), mapFade, fade, halfDepth );
	}
	const bool parallel = args.GetBool( "parallel", "0" );
	const idVec3 origin = args.GetVector( "origin" );

	// axis[0] points out of the surface the decal goes on; axis[0], axis[1], axis[2] is a
	// right-handed frame, which makes the winding below face +axis[0]
	idMat3 axis;
	bool aimed = false;
	const char* targetName = args.GetString( "target" );
	if( targetName[0] != '\0' )
	{
		const idMapEntity* target = dmapGlobals.dmapFile->FindEntity( targetName );
		if( target == NULL )
		{
			common->Warning( "func_decal '%s': target '%s' not found, using its angles", name.c_str(), targetName );
		}
		else
		{
			idVec3 dir = target->epairs.GetVector( "origin" ) - origin;
			if( dir.Normalize() == 0.0f )
			{
				common->Warning( "func_decal '%s': target '%s' is at its origin, using its angles", name.c_str(), targetName );
			}
			else
			{
				// aimed at the target, roll 0
				idVec3 left, down;
				axis[0] = -dir;
				axis[0].NormalVectors( left, down );
				axis[1] = left;
				axis[2] = -down;
				aimed = true;
			}
		}
	}
	if( !aimed )
	{
		// the order the inlined models read their axis in (usurface.cpp), which is the game's
		if( !args.GetMatrix( "rotation", "1 0 0 0 1 0 0 0 1", axis ) )
		{
			idAngles angles;
			if( args.GetAngles( "angles", "0 0 0", angles ) )
			{
				axis = angles.ToMat3();
			}
			else
			{
				axis = idAngles( 0.0f, args.GetFloat( "angle" ), 0.0f ).ToMat3();
			}
		}
	}

	// the winding is the far cap, depth / 2 behind the origin; perspective projects from
	// depth / 2 in front of it, so the far cap is twice the size and the origin plane
	// still sees 'size'. s runs along +axis[1] and t along -axis[2], so the image reads
	// upright and unmirrored from where the entity faces
	const float capScale = parallel ? 0.5f : 1.0f;
	const idVec3 farCenter = origin - halfDepth * axis[0];
	const idVec3 right = ( size.x * capScale ) * axis[1];
	const idVec3 up = ( size.y * capScale ) * axis[2];
	const idVec3 projectionOrigin = origin + halfDepth * axis[0];

	idFixedWinding winding;
	winding += idVec5( farCenter - right + up, idVec2( 0.0f, 0.0f ) );
	winding += idVec5( farCenter + right + up, idVec2( 1.0f, 0.0f ) );
	winding += idVec5( farCenter + right - up, idVec2( 1.0f, 1.0f ) );
	winding += idVec5( farCenter - right - up, idVec2( 0.0f, 1.0f ) );

	projector.entityNum = entityNum;
	projector.numFragments = 0;
	projector.numCorners = 0;
	projector.numBandCorners = 0;
	if( !idDecalProjection::CreateParms( projector.parms, winding, projectionOrigin, parallel, fade, material, 0 ) )
	{
		// cannot happen with four points, kept so a change to the winding cannot pass silently
		common->Warning( "func_decal '%s': no projection, skipped", name.c_str() );
		return false;
	}

	return true;
}

/*
====================
EmitDecalWinding

Fans a clipped fragment into tris in its vertex order, which is the source tri's, so
AddMapTriToAreas puts the coplanar fragment on the same side of the source's BSP plane.
====================
*/
static void EmitDecalWinding( const idFixedWinding& w, const idMaterial* material, void* mergeGroup, const idVec3& normal, idList<mapTri_t>& tris )
{
	for( int i = 2; i < w.GetNumPoints(); i++ )
	{
		const int corners[3] = { 0, i - 1, i };

		mapTri_t& tri = tris.Alloc();
		memset( &tri, 0, sizeof( tri ) );
		tri.material = material;
		tri.mergeGroup = mergeGroup;
		for( int k = 0; k < 3; k++ )
		{
			const idVec5& point = w[corners[k]];
			tri.v[k].xyz = point.ToVec3();
			tri.v[k].SetTexCoord( point.s, point.t );
			tri.v[k].SetNormal( normal );
		}
	}
}

/*
====================
DressDecalsPreOptimize
====================
*/
void DressDecalsPreOptimize( uEntity_t* e )
{
	decalProjectors.Clear();

	for( int i = 1; i < dmapGlobals.numEntities; i++ )
	{
		const uEntity_t* entity = &dmapGlobals.uEntities[i];
		if( !entity->dmapOnly || idStr::Icmp( entity->mapEntity->epairs.GetString( "classname" ), FUNC_DECAL_CLASSNAME ) != 0 )
		{
			continue;
		}

		decalProjector_t projector;
		if( ParseDecalProjector( i, projector ) )
		{
			decalProjectors.Append( projector );
		}
	}

	if( decalProjectors.Num() == 0 )
	{
		return;
	}

	// from here on the list does not change: &decalProjectors[p] is the merge group of the
	// hard part, the address one byte into it the merge group of the fade band, an address
	// no other merge group can have
	idList<mapTri_t> decalTris;

	// it grows by Alloc: at the default granularity of 16 every growth copies all the 192-byte
	// tris so far, quadratic in the fragment count
	decalTris.SetGranularity( 4096 );

	for( int p = 0; p < decalProjectors.Num(); p++ )
	{
		decalProjector_t& projector = decalProjectors[p];
		const decalProjectionParms_t& parms = projector.parms;
		void* hardGroup = &projector;
		void* bandGroup = ( byte* )&projector + 1;

		for( int a = 0; a < e->numAreas; a++ )
		{
			for( const optimizeGroup_t* group = e->areas[a].groups; group; group = group->nextGroup )
			{
				if( !group->material->AllowOverlays() )
				{
					continue;
				}

				// never onto an existing decal surface (a DECAL_MACRO patch or brush face): the
				// fragments would stack on it and z-fight, and AllowOverlays alone lets opaque
				// or forceOverlays decal materials through
				if( group->material->IsDiscrete() && group->material->GetSort() == SS_DECAL )
				{
					continue;
				}

				// never onto generated tris: there are none in the areas yet, they are inserted
				// only after every projector is clipped, so no merge group points into the list
				assert( ( const byte* )group->mergeGroup < ( const byte* )&decalProjectors[0] ||
						( const byte* )group->mergeGroup > ( const byte* )&decalProjectors[decalProjectors.Num() - 1] + 1 );

				for( const mapTri_t* tri = group->triList; tri; tri = tri->next )
				{
					// Prelight has not set the group bounds yet
					idBounds triBounds( tri->v[0].xyz );
					triBounds.AddPoint( tri->v[1].xyz );
					triBounds.AddPoint( tri->v[2].xyz );
					if( !parms.projectionBounds.IntersectsBounds( triBounds ) )
					{
						continue;
					}

					idFixedWinding fw;
					if( !idDecalProjection::ClipTriangle( parms, tri->v[0].xyz, tri->v[1].xyz, tri->v[2].xyz, ( 1 << NUM_DECAL_BOUNDING_PLANES ) - 1, fw ) )
					{
						continue;
					}

					idPlane plane;
					PlaneForTri( tri, plane );

					// the fade happens only at the far end: the part behind the far fade
					// plane is the band, the rest is hard
					void* fwGroup = hardGroup;
					if( parms.fadeDepth > 0.0f )
					{
						idFixedWinding back;
						const int side = fw.Split( &back, parms.fadePlanes[0], 0.1f );
						if( side == SIDE_CROSS )
						{
							EmitDecalWinding( back, parms.material, bandGroup, plane.Normal(), decalTris );
							projector.numFragments++;
						}
						else if( side == SIDE_BACK )
						{
							fwGroup = bandGroup;
						}
					}
					EmitDecalWinding( fw, parms.material, fwGroup, plane.Normal(), decalTris );
					projector.numFragments++;
				}
			}
		}
	}

	int totalFragments = 0;
	for( int p = 0; p < decalProjectors.Num(); p++ )
	{
		const decalProjector_t& projector = decalProjectors[p];
		common->Printf( "func_decal '%s': %i fragments\n", DecalName( projector.entityNum ).c_str(), projector.numFragments );
		totalFragments += projector.numFragments;
	}
	common->Printf( "%5i func_decal fragments, %i tris\n", totalFragments, decalTris.Num() );

	// AddMapTriToAreas copies the tri
	for( int i = 0; i < decalTris.Num(); i++ )
	{
		AddMapTriToAreas( &decalTris[i], e );
	}
}

/*
====================
DressDecalsPostTjunction
====================
*/
void DressDecalsPostTjunction( uEntity_t* e )
{
	if( decalProjectors.Num() == 0 )
	{
		return;
	}

	int numBandGroups = 0;
	for( int a = 0; a < e->numAreas; a++ )
	{
		for( optimizeGroup_t* group = e->areas[a].groups; group; group = group->nextGroup )
		{
			bool band;
			const int p = DecalProjectorForMergeGroup( group->mergeGroup, band );
			if( p < 0 )
			{
				continue;
			}

			// every group of the projector, band or hard, so the surface they merge into is
			// coloured throughout; far fade plane only, the near edge is hard; clamped as the
			// runtime decals clamp it (idDecalProjection::ClampDepthFade)
			decalProjector_t& projector = decalProjectors[p];
			const decalProjectionParms_t& parms = projector.parms;
			if( parms.fadeDepth > 0.0f )
			{
				for( mapTri_t* tri = group->triList; tri; tri = tri->next )
				{
					for( int k = 0; k < 3; k++ )
					{
						const float depthFade = idDecalProjection::ClampDepthFade( idDecalProjection::DepthFade( parms.fadePlanes[0], parms.fadeDepth, tri->v[k].xyz ) );
						// rounded, so a vertex on the knee is exactly opaque
						const dword color = PackColorRound( idVec4( 1.0f, 1.0f, 1.0f, 1.0f - depthFade ) );
						tri->v[k].SetColor( color );
						projector.numCorners++;
						if( ( color >> 24 ) != 255 )
						{
							projector.numBandCorners++;
						}
					}
				}
				group->vertexColor = true;
			}

			// the band was kept apart only for the optimizer: GroupsAreSurfaceCompatible
			// compares merge groups, so this makes one surface per projector
			if( band )
			{
				group->mergeGroup = &decalProjectors[p];
				numBandGroups++;
			}
		}
	}
	common->VerbosePrintf( "%5i func_decal fade band groups\n", numBandGroups );

	for( int p = 0; p < decalProjectors.Num(); p++ )
	{
		const decalProjector_t& projector = decalProjectors[p];
		if( projector.parms.fadeDepth > 0.0f )
		{
			common->Printf( "func_decal '%s': %i tri corners coloured, %i in the fade band\n", DecalName( projector.entityNum ).c_str(), projector.numCorners, projector.numBandCorners );
		}
	}
}
