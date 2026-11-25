/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.
Copyright (C) 2022 Harrie van Ginneken
Copyright (C) 2022 Robert Beckebans

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

#include "AASBuild_local.h"
#include "../compiler_public.h" //puncho - TC1
#include "../dmap/CompilerPhase.h" //puncho - TC0
#include "../../../aas/AASFile_router.h"	//puncho - R2 router

//puncho start - R2 router: an AAS compile also bakes the router's lists
#ifndef _WIN32
#include <sys/wait.h>	// WEXITSTATUS, for the bake's result
#endif
static idCVar aas_routerBake( "aas_routerBake", "1", CVAR_TOOL | CVAR_BOOL, "after an AAS compile, bake the router's walk lists beside each AAS file it wrote (needs the program aasrouterbake in the base path); 0: a compile writes no router file, and the command routerMap writes the router maps only" );
static void RemoveRouterFiles( const idStr& aasName );
static void RouterFilesAfterAAS( const idStr& mapName, const char* aasType );
//puncho end - R2 router

#define BFL_PATCH		0x1000

//===============================================================
//
//	idAASBuild
//
//===============================================================

/*
============
idAASBuild::idAASBuild
============
*/
idAASBuild::idAASBuild()
{
	file = NULL;
	procNodes = NULL;
	numProcNodes = 0;
	numGravitationalSubdivisions = 0;
	numMergedLeafNodes = 0;
	numLedgeSubdivisions = 0;
	ledgeMap = NULL;
}

/*
============
idAASBuild::~idAASBuild
============
*/
idAASBuild::~idAASBuild()
{
	Shutdown();
}

/*
================
idAASBuild::Shutdown
================
*/
void idAASBuild::Shutdown()
{
	aasSettings = NULL;
	if( file )
	{
		delete file;
		file = NULL;
	}
	DeleteProcBSP();
	numGravitationalSubdivisions = 0;
	numMergedLeafNodes = 0;
	numLedgeSubdivisions = 0;
	ledgeList.Clear();
	if( ledgeMap )
	{
		delete ledgeMap;
		ledgeMap = NULL;
	}
}

/*
================
idAASBuild::ParseProcNodes
================
*/
void idAASBuild::ParseProcNodes( idLexer* src )
{
	int i;

	src->ExpectTokenString( "{" );

	idAASBuild::numProcNodes = src->ParseInt();
	if( idAASBuild::numProcNodes < 0 )
	{
		src->Error( "idAASBuild::ParseProcNodes: bad numProcNodes" );
	}
	idAASBuild::procNodes = ( aasProcNode_t* )Mem_ClearedAlloc( idAASBuild::numProcNodes * sizeof( aasProcNode_t ), TAG_TOOLS );

	for( i = 0; i < idAASBuild::numProcNodes; i++ )
	{
		aasProcNode_t* node;

		node = &( idAASBuild::procNodes[i] );

		src->Parse1DMatrix( 4, node->plane.ToFloatPtr() );
		node->children[0] = src->ParseInt();
		node->children[1] = src->ParseInt();
	}

	src->ExpectTokenString( "}" );
}

/*
================
idAASBuild::LoadProcBSP
================
*/
bool idAASBuild::LoadProcBSP( const char* name, ID_TIME_T minFileTime )
{
	idStr fileName;
	idToken token;
	idLexer* src;

	// load it
	fileName = name;
	fileName.SetFileExtension( PROC_FILE_EXT );
	src = new idLexer( fileName, LEXFL_NOSTRINGCONCAT | LEXFL_NODOLLARPRECOMPILE );
	if( !src->IsLoaded() )
	{
		common->Warning( "idAASBuild::LoadProcBSP: couldn't load %s", fileName.c_str() );
		delete src;
		return false;
	}

	// if the file is too old
	if( src->GetFileTime() < minFileTime )
	{
		delete src;
		return false;
	}

	if( !src->ReadToken( &token ) || ProcFileVersion( token ) == 0 )
	{
		common->Warning( "idAASBuild::LoadProcBSP: bad id '%s' instead of '%s', '%s' or '%s'", token.c_str(), PROC_FILE_ID, PROC_FILE_ID_004, PROC_FILE_ID_003 );
		delete src;
		return false;
	}

	// parse the file
	while( 1 )
	{
		if( !src->ReadToken( &token ) )
		{
			break;
		}

		if( token == "model" )
		{
			src->SkipBracedSection();
			continue;
		}

		if( token == "shadowModel" )
		{
			src->SkipBracedSection();
			continue;
		}

		if( token == "interAreaPortals" )
		{
			src->SkipBracedSection();
			continue;
		}

		if( token == "nodes" )
		{
			idAASBuild::ParseProcNodes( src );
			break;
		}

		src->Error( "idAASBuild::LoadProcBSP: bad token \"%s\"", token.c_str() );
	}

	delete src;

	return true;
}

/*
============
idAASBuild::DeleteProcBSP
============
*/
void idAASBuild::DeleteProcBSP()
{
	if( procNodes )
	{
		Mem_Free( procNodes );
		procNodes = NULL;
	}
	numProcNodes = 0;
}

/*
============
idAASBuild::ChoppedAwayByProcBSP
============
*/
bool idAASBuild::ChoppedAwayByProcBSP( int nodeNum, idFixedWinding* w, const idVec3& normal, const idVec3& origin, const float radius )
{
	int res;
	idFixedWinding back;
	aasProcNode_t* node;
	float dist;

	do
	{
		node = idAASBuild::procNodes + nodeNum;
		dist = node->plane.Normal() * origin + node->plane[3];
		if( dist > radius )
		{
			res = SIDE_FRONT;
		}
		else if( dist < -radius )
		{
			res = SIDE_BACK;
		}
		else
		{
			res = w->Split( &back, node->plane, ON_EPSILON );
		}
		if( res == SIDE_FRONT )
		{
			nodeNum = node->children[0];
		}
		else if( res == SIDE_BACK )
		{
			nodeNum = node->children[1];
		}
		else if( res == SIDE_ON )
		{
			// continue with the side the winding faces
			if( node->plane.Normal() * normal > 0.0f )
			{
				nodeNum = node->children[0];
			}
			else
			{
				nodeNum = node->children[1];
			}
		}
		else
		{
			// if either node is not solid
			if( node->children[0] < 0 || node->children[1] < 0 )
			{
				return false;
			}
			// only recurse if the node is not solid
			if( node->children[1] > 0 )
			{
				if( !idAASBuild::ChoppedAwayByProcBSP( node->children[1], &back, normal, origin, radius ) )
				{
					return false;
				}
			}
			nodeNum = node->children[0];
		}
	}
	while( nodeNum > 0 );
	if( nodeNum < 0 )
	{
		return false;
	}
	return true;
}

/*
============
idAASBuild::ClipBrushSidesWithProcBSP
============
*/
void idAASBuild::ClipBrushSidesWithProcBSP( idBrushList& brushList )
{
	int i, clippedSides;
	idBrush* brush;
	idFixedWinding neww;
	idBounds bounds;
	float radius;
	idVec3 origin;

	// if the .proc file has no BSP tree
	if( idAASBuild::procNodes == NULL )
	{
		return;
	}

	clippedSides = 0;
	for( brush = brushList.Head(); brush; brush = brush->Next() )
	{
		for( i = 0; i < brush->GetNumSides(); i++ )
		{

			if( !brush->GetSide( i )->GetWinding() )
			{
				continue;
			}

			// make a local copy of the winding
			neww = *brush->GetSide( i )->GetWinding();
			neww.GetBounds( bounds );
			origin = ( bounds[1] - bounds[0] ) * 0.5f;
			radius = origin.Length() + ON_EPSILON;
			origin = bounds[0] + origin;

			if( ChoppedAwayByProcBSP( 0, &neww, brush->GetSide( i )->GetPlane().Normal(), origin, radius ) )
			{
				brush->GetSide( i )->SetFlag( SFL_USED_SPLITTER );
				clippedSides++;
			}
		}
	}

	common->Printf( "%6d brush sides clipped\n", clippedSides );
}

/*
============
idAASBuild::ContentsForAAS
============
*/
int idAASBuild::ContentsForAAS( int contents )
{
	int c;

	if( contents & ( CONTENTS_SOLID | CONTENTS_AAS_SOLID | CONTENTS_MONSTERCLIP ) )
	{
		return AREACONTENTS_SOLID;
	}
	c = 0;
	if( contents & CONTENTS_WATER )
	{
		c |= AREACONTENTS_WATER;
	}
	if( contents & CONTENTS_AREAPORTAL )
	{
		c |= AREACONTENTS_CLUSTERPORTAL;
	}
	if( contents & CONTENTS_AAS_OBSTACLE )
	{
		c |= AREACONTENTS_OBSTACLE;
	}
	return c;
}

/*
============
idAASBuild::AddBrushForMapBrush
============
*/
idBrushList idAASBuild::AddBrushesForMapBrush( const idMapBrush* mapBrush, const idVec3& origin, const idMat3& axis, int entityNum, int primitiveNum, idBrushList brushList )
{
	int contents, i;
	idMapBrushSide* mapSide;
	const idMaterial* mat;
	idList<idBrushSide*> sideList;
	idBrush* brush;
	idPlane plane;
	const idBrushFrame worldFrame;	//puncho - Phase 2C P2.2: centre 0, the AAS windings are made at world coordinates

	contents = 0;
	for( i = 0; i < mapBrush->GetNumSides(); i++ )
	{
		mapSide = mapBrush->GetSide( i );
		mat = declManager->FindMaterial( mapSide->GetMaterial() );
		contents |= mat->GetContentFlags();
		plane = mapSide->GetPlane();
		plane.FixDegeneracies( DEGENERATE_DIST_EPSILON );
		sideList.Append( new idBrushSide( plane, -1 ) );
		//puncho start - Phase 2C P2.2: a non-axial Valve 220 side whose map points define its plane keeps the plane
		// made from those points in double (dmap's S5b rule, idBrushFrame::PlaneFromPoints): the stored float plane is off
		// by an ulp of its distance far from the origin. The points are less the entity origin the parser took off
		idDPlane dplane;
		if( aas_brushWindingsDouble.GetBool() && mapSide->GetProjectionType() == idMapBrushSide::PROJECTION_VALVE220 && worldFrame.PlaneFromPointsDouble( mapSide->planepts, mapSide->GetOrigin(), plane, dplane ) )
		{
			sideList[sideList.Num() - 1]->SetDPlane( dplane );
		}
		//puncho end
	}

	contents = ContentsForAAS( contents );
	if( !contents )
	{
		for( i = 0; i < sideList.Num(); i++ )
		{
			delete sideList[i];
		}
		return brushList;
	}

	brush = new idBrush();
	brush->SetContents( contents );

	if( !brush->FromSides( sideList ) )
	{
		common->Warning( "brush primitive %d on entity %d is degenerate", primitiveNum, entityNum );
		delete brush;
		return brushList;
	}

	brush->SetEntityNum( entityNum );
	brush->SetPrimitiveNum( primitiveNum );
	brush->Transform( origin, axis );
	brushList.AddToTail( brush );

	return brushList;
}
/*
============
idAASBuild::AddBrushesForMapPolygonMesh
============
*/

idBrushList idAASBuild::AddBrushesForMapPolygonMesh( const MapPolygonMesh* mapMesh, const idVec3& origin, const idMat3& axis, int entityNum, int primitiveNum, idBrushList brushList )
{
	int contents = 0;
	int validBrushes = 0;

	idFixedWinding w;
	idPlane plane;
	idVec3 d1, d2;
	idBrush* brush;
	const idMaterial* mat;

	//per map polygon
	for( int p = 0 ; p < mapMesh->GetNumPolygons(); p++ )
	{
		const MapPolygon& face = mapMesh->GetFace( p );

		mat = declManager->FindMaterial( face.GetMaterial() );
		contents = ContentsForAAS( mat->GetContentFlags() );

		if( !contents )
		{
			return brushList;
		}

		const idList<idDrawVert>& verts = mapMesh->GetDrawVerts();
		const idList<int>& indices = face.GetIndexes();

		//create brush from triangle

		//Front face
		d1 = verts[indices[1]].xyz - verts[indices[0]].xyz;
		d2 = verts[indices[2]].xyz - verts[indices[0]].xyz;
		plane.SetNormal( d1.Cross( d2 ) );
		if( plane.Normalize() != 0.0f )
		{
			plane.FitThroughPoint( verts[indices[2]].xyz );

			w.Clear();
			w += verts[indices[0]].xyz;
			w += verts[indices[1]].xyz;
			w += verts[indices[2]].xyz;

			brush = new idBrush();
			brush->SetContents( contents );
			if( brush->FromWinding( w, plane ) )
			{
				brush->SetEntityNum( entityNum );
				brush->SetPrimitiveNum( primitiveNum );
				brush->SetFlag( BFL_PATCH );
				brush->Transform( origin, axis );
				brushList.AddToTail( brush );
				validBrushes++;
			}
			else
			{
				delete brush;
			}
		}
	}

	if( !validBrushes )
	{
		common->Warning( "map polygon primitive %d on entity %d is completely degenerate", primitiveNum, entityNum );
	}

	return brushList;
}

/*
============
idAASBuild::AddBrushesForPatch
============
*/
idBrushList idAASBuild::AddBrushesForMapPatch( const idMapPatch* mapPatch, const idVec3& origin, const idMat3& axis, int entityNum, int primitiveNum, idBrushList brushList )
{
	int i, j, contents, validBrushes;
	float dot;
	int v1, v2, v3, v4;
	idFixedWinding w;
	idPlane plane;
	idVec3 d1, d2;
	idBrush* brush;
	idSurface_Patch mesh;
	const idMaterial* mat;

	mat = declManager->FindMaterial( mapPatch->GetMaterial() );
	contents = ContentsForAAS( mat->GetContentFlags() );

	if( !contents )
	{
		return brushList;
	}

	mesh = idSurface_Patch( *mapPatch );

	// if the patch has an explicit number of subdivisions use it to avoid cracks
	if( mapPatch->GetExplicitlySubdivided() )
	{
		mesh.SubdivideExplicit( mapPatch->GetHorzSubdivisions(), mapPatch->GetVertSubdivisions(), false, true );
	}
	else
	{
		mesh.Subdivide( DEFAULT_CURVE_MAX_ERROR_CD, DEFAULT_CURVE_MAX_ERROR_CD, DEFAULT_CURVE_MAX_LENGTH_CD, false );
	}

	validBrushes = 0;

	for( i = 0; i < mesh.GetWidth() - 1; i++ )
	{
		for( j = 0; j < mesh.GetHeight() - 1; j++ )
		{

			v1 = j * mesh.GetWidth() + i;
			v2 = v1 + 1;
			v3 = v1 + mesh.GetWidth() + 1;
			v4 = v1 + mesh.GetWidth();

			d1 = mesh[v2].xyz - mesh[v1].xyz;
			d2 = mesh[v3].xyz - mesh[v1].xyz;
			plane.SetNormal( d1.Cross( d2 ) );
			if( plane.Normalize() != 0.0f )
			{
				plane.FitThroughPoint( mesh[v1].xyz );
				dot = plane.Distance( mesh[v4].xyz );
				// if we can turn it into a quad
				if( idMath::Fabs( dot ) < 0.1f )
				{
					w.Clear();
					w += mesh[v1].xyz;
					w += mesh[v2].xyz;
					w += mesh[v3].xyz;
					w += mesh[v4].xyz;

					brush = new idBrush();
					brush->SetContents( contents );
					if( brush->FromWinding( w, plane ) )
					{
						brush->SetEntityNum( entityNum );
						brush->SetPrimitiveNum( primitiveNum );
						brush->SetFlag( BFL_PATCH );
						brush->Transform( origin, axis );
						brushList.AddToTail( brush );
						validBrushes++;
					}
					else
					{
						delete brush;
					}
					continue;
				}
				else
				{
					// create one of the triangles
					w.Clear();
					w += mesh[v1].xyz;
					w += mesh[v2].xyz;
					w += mesh[v3].xyz;

					brush = new idBrush();
					brush->SetContents( contents );
					if( brush->FromWinding( w, plane ) )
					{
						brush->SetEntityNum( entityNum );
						brush->SetPrimitiveNum( primitiveNum );
						brush->SetFlag( BFL_PATCH );
						brush->Transform( origin, axis );
						brushList.AddToTail( brush );
						validBrushes++;
					}
					else
					{
						delete brush;
					}
				}
			}
			// create the other triangle
			d1 = mesh[v3].xyz - mesh[v1].xyz;
			d2 = mesh[v4].xyz - mesh[v1].xyz;
			plane.SetNormal( d1.Cross( d2 ) );
			if( plane.Normalize() != 0.0f )
			{
				plane.FitThroughPoint( mesh[v1].xyz );

				w.Clear();
				w += mesh[v1].xyz;
				w += mesh[v3].xyz;
				w += mesh[v4].xyz;

				brush = new idBrush();
				brush->SetContents( contents );
				if( brush->FromWinding( w, plane ) )
				{
					brush->SetEntityNum( entityNum );
					brush->SetPrimitiveNum( primitiveNum );
					brush->SetFlag( BFL_PATCH );
					brush->Transform( origin, axis );
					brushList.AddToTail( brush );
					validBrushes++;
				}
				else
				{
					delete brush;
				}
			}
		}
	}

	if( !validBrushes )
	{
		common->Warning( "patch primitive %d on entity %d is completely degenerate", primitiveNum, entityNum );
	}

	return brushList;
}

/*
============
idAASBuild::AddBrushesForMapEntity
============
*/
idBrushList idAASBuild::AddBrushesForMapEntity( const idMapEntity* mapEnt, int entityNum, idBrushList brushList )
{
	int i;
	idVec3 origin;
	idMat3 axis;

	if( mapEnt->GetNumPrimitives() < 1 )
	{
		return brushList;
	}

	mapEnt->epairs.GetVector( "origin", "0 0 0", origin );
	if( !mapEnt->epairs.GetMatrix( "rotation", "1 0 0 0 1 0 0 0 1", axis ) )
	{
		float angle = mapEnt->epairs.GetFloat( "angle" );
		if( angle != 0.0f )
		{
			axis = idAngles( 0.0f, angle, 0.0f ).ToMat3();
		}
		else
		{
			axis.Identity();
		}
	}

	for( i = 0; i < mapEnt->GetNumPrimitives(); i++ )
	{
		idMapPrimitive*	mapPrim;

		mapPrim = mapEnt->GetPrimitive( i );
		if( mapPrim->GetType() == idMapPrimitive::TYPE_BRUSH )
		{
			brushList = AddBrushesForMapBrush( static_cast<idMapBrush*>( mapPrim ), origin, axis, entityNum, i, brushList );
			continue;
		}
		if( mapPrim->GetType() == idMapPrimitive::TYPE_PATCH )
		{
			if( aasSettings->usePatches )
			{
				brushList = AddBrushesForMapPatch( static_cast<idMapPatch*>( mapPrim ), origin, axis, entityNum, i, brushList );
			}
			continue;
		}
		//HVG: Map polygon mesh support
		if( mapPrim->GetType() == idMapPrimitive::TYPE_MESH )
		{
			brushList = AddBrushesForMapPolygonMesh( static_cast< MapPolygonMesh* >( mapPrim ), origin, axis, entityNum, i, brushList );
		}
	}

	return brushList;
}

/*
============
idAASBuild::AddBrushesForMapFile
============
*/
idBrushList idAASBuild::AddBrushesForMapFile( const idMapFile* mapFile, idBrushList brushList )
{
	int i;

	common->Printf( "[Brush Load]\n" );

	brushList = AddBrushesForMapEntity( mapFile->GetEntity( 0 ), 0, brushList );

	for( i = 1; i < mapFile->GetNumEntities(); i++ )
	{
		const char* classname = mapFile->GetEntity( i )->epairs.GetString( "classname" );

		if( idStr::Icmp( classname, "func_aas_obstacle" ) == 0 )
		{
			brushList = AddBrushesForMapEntity( mapFile->GetEntity( i ), i, brushList );
		}
	}

	common->Printf( "%6d brushes\n", brushList.Num() );

	return brushList;
}

/*
============
idAASBuild::CheckForEntities
============
*/
//puncho - P2b.3.8: explicit type selection enables generic flood seeds.
bool idAASBuild::CheckForEntities( const idMapFile* mapFile, idStrList& entityClassNames, bool useSeeds ) const
{
	int		i;
	idStr	classname;

	com_editors |= EDITOR_AAS;

	for( i = 0; i < mapFile->GetNumEntities(); i++ )
	{
		if( !mapFile->GetEntity( i )->epairs.GetString( "classname", "", classname ) )
		{
			continue;
		}

		if( ( useSeeds && classname.Icmp( "aas_seed" ) == 0 ) || aasSettings->ValidEntity( classname ) )
		{
			entityClassNames.AddUnique( classname );
		}
	}

	com_editors &= ~EDITOR_AAS;

	return ( entityClassNames.Num() != 0 );
}

/*
============
MergeAllowed
============
*/
bool MergeAllowed( idBrush* b1, idBrush* b2 )
{
	return ( b1->GetContents() == b2->GetContents() && !( ( b1->GetFlags() | b2->GetFlags() ) & BFL_PATCH ) );
}

/*
============
ExpandedChopAllowed
============
*/
bool ExpandedChopAllowed( idBrush* b1, idBrush* b2 )
{
	return ( b1->GetContents() == b2->GetContents() );
}

/*
============
ExpandedMergeAllowed
============
*/
bool ExpandedMergeAllowed( idBrush* b1, idBrush* b2 )
{
	return ( b1->GetContents() == b2->GetContents() );
}

/*
============
idAASBuild::ChangeMultipleBoundingBoxContents
============
*/
void idAASBuild::ChangeMultipleBoundingBoxContents_r( idBrushBSPNode* node, int mask )
{
	while( node )
	{
		if( !( node->GetContents() & mask ) )
		{
			node->SetContents( node->GetContents() & ~AREACONTENTS_SOLID );
		}
		ChangeMultipleBoundingBoxContents_r( node->GetChild( 0 ), mask );
		node = node->GetChild( 1 );
	}
}

/*
============
idAASBuild::Build
============
*/
//puncho - P2b.3.8: useSeeds; TC1: the result
aasBuildResult_t idAASBuild::Build( const idStr& fileName, const idAASSettings* settings, bool useSeeds )
{
	int i, bit, mask, startTime;
	idMapFile* mapFile;
	idBrushList brushList;
	idList<idBrushList*> expandedBrushes;
	idBrush* b;
	idBrushBSP bsp;
	idStr name;
	idAASReach reach;
	idAASCluster cluster;
	idStrList entityClassNames;

	startTime = Sys_Milliseconds();
	//puncho - TC0: stages accumulate across all selected AAS types.
	const bool phaseTimes = dmap_phaseTimes.GetBool();
	int phaseStart = phaseTimes ? Sys_Milliseconds() : 0;

	Shutdown();

	aasSettings = settings;

	name = fileName;
	name.SetFileExtension( "map" );

	mapFile = new idMapFile;
	if( !mapFile->Parse( name ) )
	{
		delete mapFile;
		common->Error( "Couldn't load map file: '%s'", name.c_str() );
		return AAS_BUILD_SKIPPED;
	}

	//puncho - TC0
	if( phaseTimes )
	{
		const int now = Sys_Milliseconds();
		DmapPhaseAdd( DP_AAS_LOAD, now - phaseStart );
		phaseStart = now;
	}

	// check if this map has any entities that use this AAS file
	if( !CheckForEntities( mapFile, entityClassNames, useSeeds ) )
	{
//puncho start - P2b.3.8
		if( useSeeds )
		{
			common->Warning( "aas %s: no using entities or seeds; 0 areas", settings->fileExtension.c_str() );
			// The loader starts at node 1; an empty AAS file is not loadable.
			name.SetFileExtension( aasSettings->fileExtension );
			fileSystem->RemoveFile( name );
			RemoveRouterFiles( name );	//puncho - R2 router: what was made from the removed file goes with it
			common->Printf( "aas %s: areas 0\n", settings->fileExtension.c_str() );
		}
//puncho end - P2b.3.8
		delete mapFile;
		common->Printf( "no entities in map that use %s\n", settings->fileExtension.c_str() );
		return AAS_BUILD_UNUSED;	//puncho - R2 router: nothing was written (with seeds: the file was removed)
	}

	name.SetFileExtension( aasSettings->fileExtension );
	common->DmapPacifierFilename( name, "Compiling AAS" );
	name.SetFileExtension( "map" );

	// load map file brushes
	brushList = AddBrushesForMapFile( mapFile, brushList );

	// if empty map
	if( brushList.Num() == 0 )
	{
		delete mapFile;
		common->Error( "%s is empty", name.c_str() );
		return AAS_BUILD_SKIPPED;
	}

	// merge as many brushes as possible before expansion
	brushList.Merge( MergeAllowed );

	// if there is a .proc file newer than the .map file
	if( LoadProcBSP( fileName, mapFile->GetFileTime() ) )
	{
		ClipBrushSidesWithProcBSP( brushList );
		DeleteProcBSP();
	}

	//puncho - TC0
	if( phaseTimes )
	{
		const int now = Sys_Milliseconds();
		DmapPhaseAdd( DP_AAS_BRUSHES, now - phaseStart );
		phaseStart = now;
	}

	// make copies of the brush list
	expandedBrushes.Append( &brushList );
	for( i = 1; i < aasSettings->numBoundingBoxes; i++ )
	{
		expandedBrushes.Append( brushList.Copy() );
	}

	// expand brushes for the axial bounding boxes
	mask = AREACONTENTS_SOLID;
	for( i = 0; i < expandedBrushes.Num(); i++ )
	{
		for( b = expandedBrushes[i]->Head(); b; b = b->Next() )
		{
			b->ExpandForAxialBox( aasSettings->boundingBoxes[i] );
			bit = 1 << ( i + AREACONTENTS_BBOX_BIT );
			mask |= bit;
			b->SetContents( b->GetContents() | bit );
		}
	}

	// move all brushes back into the original list
	for( i = 1; i < aasSettings->numBoundingBoxes; i++ )
	{
		brushList.AddToTail( *expandedBrushes[i] );
		delete expandedBrushes[i];
	}

	if( aasSettings->writeBrushMap )
	{
		bsp.WriteBrushMap( fileName, "_" + aasSettings->fileExtension, AREACONTENTS_SOLID );
	}

	//puncho - TC0
	if( phaseTimes )
	{
		const int now = Sys_Milliseconds();
		DmapPhaseAdd( DP_AAS_EXPAND, now - phaseStart );
		phaseStart = now;
	}

	// build BSP tree from brushes
	bsp.Build( brushList, AREACONTENTS_SOLID, ExpandedChopAllowed, ExpandedMergeAllowed );

	// only solid nodes with all bits set for all bounding boxes need to stay solid
	ChangeMultipleBoundingBoxContents_r( bsp.GetRootNode(), mask );

	// portalize the bsp tree
	bsp.Portalize();

	// remove subspaces not reachable by entities
	if( !bsp.RemoveOutside( mapFile, AREACONTENTS_SOLID, entityClassNames ) )
	{
		bsp.LeakFile( name );
		delete mapFile;
		common->Printf( "%s has no outside", name.c_str() );
		return AAS_BUILD_SKIPPED;
	}

	//puncho - TC0
	if( phaseTimes )
	{
		const int now = Sys_Milliseconds();
		DmapPhaseAdd( DP_AAS_BSP, now - phaseStart );
		phaseStart = now;
	}

	// gravitational subdivision
	GravitationalSubdivision( bsp );

	// merge portals where possible
	bsp.MergePortals( AREACONTENTS_SOLID );

	// melt portal windings
	bsp.MeltPortals( AREACONTENTS_SOLID );

	if( aasSettings->writeBrushMap )
	{
		WriteLedgeMap( fileName, "_" + aasSettings->fileExtension + "_ledge" );
	}

	// ledge subdivisions
	LedgeSubdivision( bsp );

	// merge leaf nodes
	MergeLeafNodes( bsp );

	// merge portals where possible
	bsp.MergePortals( AREACONTENTS_SOLID );

	// melt portal windings
	bsp.MeltPortals( AREACONTENTS_SOLID );

	//puncho - TC0
	if( phaseTimes )
	{
		const int now = Sys_Milliseconds();
		DmapPhaseAdd( DP_AAS_SUBDIVIDE, now - phaseStart );
		phaseStart = now;
	}

	// store the file from the bsp tree
	StoreFile( bsp );
	file->settings = *aasSettings;

	//puncho - TC0
	if( phaseTimes )
	{
		const int now = Sys_Milliseconds();
		DmapPhaseAdd( DP_AAS_STORE, now - phaseStart );
		phaseStart = now;
	}

	// calculate reachability
	reach.Build( mapFile, file );

	//puncho - TC0
	if( phaseTimes )
	{
		const int now = Sys_Milliseconds();
		DmapPhaseAdd( DP_AAS_REACH, now - phaseStart );
		phaseStart = now;
	}

	// build clusters
	cluster.Build( file );

	//puncho - TC0
	if( phaseTimes )
	{
		const int now = Sys_Milliseconds();
		DmapPhaseAdd( DP_AAS_CLUSTER, now - phaseStart );
		phaseStart = now;
	}

	// optimize the file
	if( !aasSettings->noOptimize )
	{
		file->Optimize();
	}

	//puncho - TC0
	if( phaseTimes )
	{
		const int now = Sys_Milliseconds();
		DmapPhaseAdd( DP_AAS_OPTIMIZE, now - phaseStart );
		phaseStart = now;
	}

	// write the file
	name.SetFileExtension( aasSettings->fileExtension );
	RemoveRouterFiles( name );	//puncho - R2 router: what was made from the old file goes before the file changes
	//puncho - TC1: propagate checked completion.
	const bool written = file->Write( name, mapFile->GetGeometryCRC() );

	//puncho - TC0
	if( phaseTimes )
	{
		const int now = Sys_Milliseconds();
		DmapPhaseAdd( DP_AAS_WRITE, now - phaseStart );
	}

	// delete the map file
	delete mapFile;
	if( !written )
	{
		return AAS_BUILD_WRITE_FAILED; //puncho - TC1: distinguish output failure from a skipped type
	}

	common->Printf( "%6d seconds to create AAS\n", ( Sys_Milliseconds() - startTime ) / 1000 );
	common->DmapPacifierInfo( "%6d seconds to create AAS\n", ( Sys_Milliseconds() - startTime ) / 1000 );

	return AAS_BUILD_OK;
}

/*
============
idAASBuild::BuildReachability
============
*/
aasBuildResult_t idAASBuild::BuildReachability( const idStr& fileName, const idAASSettings* settings )
{
	int startTime;
	idMapFile* mapFile;
	idStr name;
	idAASReach reach;
	idAASCluster cluster;

	startTime = Sys_Milliseconds();

	aasSettings = settings;

	name = fileName;
	name.SetFileExtension( "map" );

	mapFile = new idMapFile;
	if( !mapFile->Parse( name ) )
	{
		delete mapFile;
		common->Error( "Couldn't load map file: '%s'", name.c_str() );
		return AAS_BUILD_SKIPPED;
	}

	file = new idAASFileLocal();

	name.SetFileExtension( aasSettings->fileExtension );
	if( !file->Load( name, 0 ) )
	{
		delete mapFile;
		common->Error( "Couldn't load AAS file: '%s'", name.c_str() );
		return AAS_BUILD_SKIPPED;
	}

	file->settings = *aasSettings;

	// calculate reachability
	reach.Build( mapFile, file );

	// build clusters
	cluster.Build( file );

	// write the file
	RemoveRouterFiles( name );	//puncho - R2 router: what was made from the old file goes before the file changes
	//puncho - TC1
	const bool written = file->Write( name, mapFile->GetGeometryCRC() );

	// delete the map file
	delete mapFile;
	if( !written )
	{
		return AAS_BUILD_WRITE_FAILED; //puncho - TC1: distinguish output failure from a skipped type
	}

	common->Printf( "%6d seconds to calculate reachability\n", ( Sys_Milliseconds() - startTime ) / 1000 );

	return AAS_BUILD_OK;
}

/*
============
ParseOptions
============
*/
int ParseOptions( const idCmdArgs& args, idAASSettings& settings )
{
	int i;
	idStr str;

	for( i = 1; i < args.Argc(); i++ )
	{

		str = args.Argv( i );
		str.StripLeading( '-' );

//puncho start - P2b.3.8
		if( str.Icmp( "aasType" ) == 0 || str.Icmp( "aasTypes" ) == 0 )
		{
			i++;
			continue;
		}
//puncho end - P2b.3.8
		if( str.Icmp( "usePatches" ) == 0 )
		{
			settings.usePatches = true;
			common->Printf( "usePatches = true\n" );
		}
		else if( str.Icmp( "writeBrushMap" ) == 0 )
		{
			settings.writeBrushMap = true;
			common->Printf( "writeBrushMap = true\n" );
		}
		else if( str.Icmp( "playerFlood" ) == 0 )
		{
			settings.playerFlood = true;
			common->Printf( "playerFlood = true\n" );
		}
		else if( str.Icmp( "noOptimize" ) == 0 )
		{
			settings.noOptimize = true;
			common->Printf( "noOptimize = true\n" );
		}
	}
	return args.Argc() - 1;
}

// RB begin
static const idDict* FindEntityDefDict( const char* name, bool makeDefault )
{
#if defined( DMAP )
	const idDeclEntityDef* decl = static_cast<const idDeclEntityDef*>( declManager->FindType( DECL_ENTITYDEF, name, makeDefault ) );
	return decl ? &decl->dict : NULL;
#else
	return gameEdit->FindEntityDefDict( name, makeDefault );
#endif
}
// RB end

/*
============
RunAAS_f
============
*/
bool RunAAS( const idCmdArgs& args ) //puncho - TC1: return before command Error
{
	bool succeeded = true;
	int i;
	idAASBuild aas;
	idAASSettings settings;
	idStr mapName;

	if( args.Argc() <= 1 )
	{
		common->Printf( "runAAS [options] <mapfile>\n"
						"options:\n"
//puncho start - P2b.3.8
						"  -aasType <type>    = build only this AAS type and enable generic seeds.\n"
						"  -aasTypes <list>   = enable listed types (spaces or +) and generic seeds.\n"
//puncho end - P2b.3.8
						"  -usePatches        = use bezier patches for collision detection.\n"
						"  -writeBrushMap     = write a brush map with the AAS geometry.\n"
						"  -playerFlood       = use player spawn points as valid AAS positions.\n" );
		return true;
	}

	common->ClearWarnings( "compiling AAS" );

	common->SetRefreshOnPrint( true );

	// get the aas settings definitions
	const idDict* dict = FindEntityDefDict( "aas_types", false );
	if( !dict )
	{
		common->Error( "Unable to find entityDef for 'aas_types'" );
	}

//puncho start - P2b.3.8
	idStr selectedType;
	idStrList listedTypes;
	bool hasList = false;
	for( i = 1; i < args.Argc() - 1; i++ )
	{
		idStr option = args.Argv( i );
		option.StripLeading( '-' );
		if( option.Icmp( "aasType" ) && option.Icmp( "aasTypes" ) )
		{
			continue;
		}
		if( i + 1 >= args.Argc() - 1 )
		{
			common->Error( "runAAS: %s needs a type argument and a map", option.c_str() );
		}
		if( !option.Icmp( "aasType" ) )
		{
			i++;
			selectedType = args.Argv( i );
			if( selectedType.IsEmpty() )
			{
				common->Error( "runAAS: aasType needs a nonempty type" );
			}
		}
		else
		{
			hasList = true;
			i++;
			idStr list = args.Argv( i );
			list.Replace( "+", " " );
			idLexer src( list, list.Length(), "aasTypes", LEXFL_ONLYSTRINGS );
			idToken token;
			while( src.ReadToken( &token ) )
			{
				listedTypes.AddUnique( token );
			}
		}
	}
	// every named type must be an aas_types entry, and a -aasType must also be in the aasTypes list
	for( i = -1; i < listedTypes.Num(); i++ )
	{
		const idStr& type = ( i < 0 ) ? selectedType : listedTypes[i];
		if( type.IsEmpty() )
		{
			continue;
		}
		const idKeyValue* known = dict->MatchPrefix( "type" );
		while( known != NULL && known->GetValue() != type )
		{
			known = dict->MatchPrefix( "type", known );
		}
		if( known == NULL || ( i < 0 && hasList && listedTypes.Find( type ) == NULL ) )
		{
			common->Error( "runAAS: refused aas type %s", type.c_str() );
		}
	}
	const bool useSeeds = hasList || !selectedType.IsEmpty();
//puncho end - P2b.3.8
	const idKeyValue* kv = dict->MatchPrefix( "type" );
	while( kv != NULL )
	{
//puncho start - P2b.3.8
		if( ( hasList && !listedTypes.Find( kv->GetValue() ) ) ||
				( !selectedType.IsEmpty() && selectedType != kv->GetValue() ) )
		{
			common->Printf( "aas: type %s not listed, skipped\n", kv->GetValue().c_str() );
			kv = dict->MatchPrefix( "type", kv );
			continue;
		}
//puncho end - P2b.3.8
		const idDict* settingsDict = FindEntityDefDict( kv->GetValue(), false );
		if( !settingsDict )
		{
			common->Warning( "Unable to find '%s' in def/aas.def", kv->GetValue().c_str() );
		}
		else
		{
			settings.FromDict( kv->GetValue(), settingsDict );
			i = ParseOptions( args, settings );
			mapName = args.Argv( i );
			mapName.BackSlashesToSlashes();
			if( mapName.Icmpn( "maps/", 4 ) != 0 )
			{
				mapName = "maps/" + mapName;
			}
//puncho start - P2b.3.8, TC1
			//puncho start - R5 speed: this type's phase times; Build adds them to sums that only dmap printed
			int phaseBefore[DP_AAS_WRITE - DP_AAS_LOAD + 1];
			for( int phase = DP_AAS_LOAD; phase <= DP_AAS_WRITE; phase++ )
			{
				phaseBefore[phase - DP_AAS_LOAD] = DmapPhaseMilliseconds( ( compilerPhase_t )phase );
			}
			//puncho end - R5 speed
			const aasBuildResult_t result = aas.Build( mapName, &settings, useSeeds );
			//puncho start - R5 speed
			if( dmap_phaseTimes.GetBool() )
			{
				static const char* phaseNames[] = { "load", "brushes", "expand", "bsp", "subdivide", "store", "reach", "cluster", "optimize", "write" };
				static_assert( sizeof( phaseNames ) / sizeof( phaseNames[0] ) == DP_AAS_WRITE - DP_AAS_LOAD + 1, "one name a phase" );
				for( int phase = DP_AAS_LOAD; phase <= DP_AAS_WRITE; phase++ )
				{
					common->Printf( "aas phase %s %s %i\n", kv->GetValue().c_str(), phaseNames[phase - DP_AAS_LOAD], DmapPhaseMilliseconds( ( compilerPhase_t )phase ) - phaseBefore[phase - DP_AAS_LOAD] );
				}
			}
			//puncho end - R5 speed
			if( result == AAS_BUILD_WRITE_FAILED || ( useSeeds && result != AAS_BUILD_OK && result != AAS_BUILD_UNUSED ) )	//puncho - R2 router: unused is no failure, as before
			{
				if( useSeeds )
				{
					common->Printf( "aas %s failed: %s\n", kv->GetValue().c_str(), mapName.c_str() );
				}
				succeeded = false;
				break;
			}
//puncho end - P2b.3.8, TC1
			//puncho start - R2 router: the router's lists follow each AAS file as soon as it is written, so a
			// later type that ends the command cannot leave this one without them
			if( result == AAS_BUILD_OK )
			{
				aas.Shutdown();		// the builder's own AAS file is not needed any more; the router step loads the file by its name, as the game does
				RouterFilesAfterAAS( mapName, kv->GetValue().c_str() );
			}
			//puncho end - R2 router
		}

		kv = dict->MatchPrefix( "type", kv );
		if( kv )
		{
			common->Printf( "=======================================================\n" );
		}
	}
	common->SetRefreshOnPrint( false );
	common->PrintWarnings();
	return succeeded;
}

//puncho - TC1: the builder and command temporaries are gone before Error can exit.
void RunAAS_f( const idCmdArgs& args )
{
	if( !RunAAS( args ) )
	{
		common->Error( "RunAAS failed" );
	}
}

/*
============
RunAASDir_f
============
*/
static bool RunAASDir( const idCmdArgs& args ) //puncho - TC1: return before command Error
{
	bool succeeded = true;
	int i;
	idAASBuild aas;
	idAASSettings settings;
	idFileList* mapFiles;

	if( args.Argc() <= 1 )
	{
		common->Printf( "runAASDir <folder>\n" );
		return true;
	}

	common->ClearWarnings( "compiling AAS" );

	common->SetRefreshOnPrint( true );

	// get the aas settings definitions
	const idDict* dict = FindEntityDefDict( "aas_types", false );
	if( !dict )
	{
		common->Error( "Unable to find entityDef for 'aas_types'" );
	}

	// scan for .map files
	mapFiles = fileSystem->ListFiles( idStr( "maps/" ) + args.Argv( 1 ), ".map" );

	// create AAS files for all the .map files
	for( i = 0; succeeded && i < mapFiles->GetNumFiles(); i++ )
	{
		if( i )
		{
			common->Printf( "=======================================================\n" );
		}

		const idStr dirMapName = idStr( "maps/" ) + args.Argv( 1 ) + "/" + mapFiles->GetFile( i );	//puncho - R2 router
		const idKeyValue* kv = dict->MatchPrefix( "type" );
		while( kv != NULL )
		{
			const idDict* settingsDict = FindEntityDefDict( kv->GetValue(), false );
			if( !settingsDict )
			{
				common->Warning( "Unable to find '%s' in def/aas.def", kv->GetValue().c_str() );
			}
			else
			{
				settings.FromDict( kv->GetValue(), settingsDict );
				const aasBuildResult_t result = aas.Build( dirMapName, &settings ); //puncho - TC1
				if( result == AAS_BUILD_WRITE_FAILED )
				{
					succeeded = false;
					break;
				}
				//puncho start - R2 router: the router's lists follow each AAS file as soon as it is written
				if( result == AAS_BUILD_OK )
				{
					aas.Shutdown();
					RouterFilesAfterAAS( dirMapName, kv->GetValue().c_str() );
				}
				//puncho end - R2 router
			}

			kv = dict->MatchPrefix( "type", kv );
			if( kv )
			{
				common->Printf( "=======================================================\n" );
			}
		}
	}

	fileSystem->FreeFileList( mapFiles );

	common->SetRefreshOnPrint( false );
	common->PrintWarnings();
	return succeeded;
}

//puncho - TC1: the builder and command temporaries are gone before Error can exit.
void RunAASDir_f( const idCmdArgs& args )
{
	if( !RunAASDir( args ) )
	{
		common->Error( "RunAASDir failed" );
	}
}

/*
============
RunReach_f
============
*/
static bool RunReach( const idCmdArgs& args ) //puncho - TC1: return before command Error
{
	bool succeeded = true;
	int i;
	idAASBuild aas;
	idAASSettings settings;
	idStr reachMapName;		//puncho - R2 router

	if( args.Argc() <= 1 )
	{
		common->Printf( "runReach [options] <mapfile>\n" );
		return true;
	}

	common->ClearWarnings( "calculating AAS reachability" );

	common->SetRefreshOnPrint( true );

	// get the aas settings definitions
	const idDict* dict = FindEntityDefDict( "aas_types", false );
	if( !dict )
	{
		common->Error( "Unable to find entityDef for 'aas_types'" );
	}

	const idKeyValue* kv = dict->MatchPrefix( "type" );
	while( kv != NULL )
	{
		const idDict* settingsDict = FindEntityDefDict( kv->GetValue(), false );
		if( !settingsDict )
		{
			common->Warning( "Unable to find '%s' in def/aas.def", kv->GetValue().c_str() );
		}
		else
		{
			settings.FromDict( kv->GetValue(), settingsDict );
			i = ParseOptions( args, settings );
			reachMapName = idStr( "maps/" ) + args.Argv( i );	//puncho - R2 router
			const aasBuildResult_t result = aas.BuildReachability( reachMapName, &settings ); //puncho - TC1
			if( result == AAS_BUILD_WRITE_FAILED )
			{
				succeeded = false;
				break;
			}
			//puncho start - R2 router: the reachabilities changed, so this AAS file's router lists are baked again
			if( result == AAS_BUILD_OK )
			{
				aas.Shutdown();
				RouterFilesAfterAAS( reachMapName, kv->GetValue().c_str() );
			}
			//puncho end - R2 router
		}

		kv = dict->MatchPrefix( "type", kv );
		if( kv )
		{
			common->Printf( "=======================================================\n" );
		}
	}

	common->SetRefreshOnPrint( false );
	common->PrintWarnings();
	return succeeded;
}

//puncho - TC1: the builder and command temporaries are gone before Error can exit.
void RunReach_f( const idCmdArgs& args )
{
	if( !RunReach( args ) )
	{
		common->Error( "RunReach failed" );
	}
}

//puncho start - R2 router
/*
============
RemoveRouterLists

  removes a lists file and the certificate the bake writes beside it, through the file system (every path the
  engine can write, in the base path and in the save path); says nothing when they are not there, and warns
  when the lists are still there afterwards
============
*/
static void RemoveRouterLists( const char* listsName )
{
	fileSystem->RemoveFile( listsName );
	fileSystem->RemoveFile( va( "%s.certificate.txt", listsName ) );
	if( fileSystem->GetFileLength( listsName ) >= 0 )
	{
		common->Warning( "routerMap: cannot remove %s (the game refuses lists that are older than their AAS file)", listsName );
	}
}

/*
============
RemoveRouterFiles

  An AAS file was rewritten or removed: everything that was made from the old one goes, for both masks: the
  lists with their certificate, the door layer and the router map. The door list (<aas file>.walk.rtd) is
  written by hand and stays; it names areas of the old file, so it is pointed out.
============
*/
static void RemoveRouterFiles( const idStr& aasName )
{
	static const char* labels[2] = { "walk", "fly" };
	for( int p = 0; p < 2; p++ )
	{
		const idStr stem = aasName + "." + labels[p];
		RemoveRouterLists( ( stem + ".nlw4" ).c_str() );
		fileSystem->RemoveFile( ( stem + ".nld4" ).c_str() );
		if( fileSystem->GetFileLength( ( stem + ".nld4" ).c_str() ) >= 0 )
		{
			// the game refuses a door layer that was not baked with the lists beside it, so this costs the map
			// its router, not its routes
			common->Warning( "routerMap: cannot remove %s.nld4: the game would find the door layer of another AAS file", stem.c_str() );
		}
		fileSystem->RemoveFile( ( stem + ".rtg" ).c_str() );
		if( fileSystem->GetFileLength( ( stem + ".rtd" ).c_str() ) >= 0 )
		{
			common->Warning( "routerMap: %s.rtd was written for the AAS file as it was before this compile; check its area numbers", stem.c_str() );
		}
	}
}

/*
============
BakeRouterLists

  Runs the bake program on one router map and has it write the lists file the game reads. The bake is the
  standalone program the router was proven with (its sources are in neo/libs/aasrouterbake, and its build
  copies it to the root of the tree); it is looked for as "aasrouterbake" in the base path. It proves every
  ordered pair of areas, so its time grows
  with the square of the map: about a second at 10,000 areas, ten to twenty at the 32,767 areas an AAS file
  can hold.
  Lists that are there go before the bake starts, and whatever a failed bake left goes too: the game must
  never read lists that do not belong to the AAS file beside them.
  False when a bake was started and did not leave lists, and when a path holds a character the shell could
  take amiss (nothing is started then, and lists that are there stay). A missing bake program and a map with
  a door list are said and are no failure: nothing is baked and lists that are there stay.
============
*/
static bool BakeRouterLists( const char* rtgName, const char* listsName )
{
	// the levels the lists were tested with (walk)
	static const char* levels = "128:4:512,256:8:3072,1024:8:0:4096";
#ifdef _WIN32
	static const char* toolName = "aasrouterbake.exe";
	static const char* unsafe = "\"%!";		// cmd: a quote ends the quoting; percent and exclamation signs can be expanded inside it
	static const char* unsafeWords = "a double quote, a percent sign or an exclamation mark";
#else
	static const char* toolName = "aasrouterbake";
	static const char* unsafe = "'";		// sh: the paths are in single quotes
	static const char* unsafeWords = "a single quote";
#endif
	idStr tool = cvarSystem->GetCVarString( "fs_basepath" );
	tool.AppendPath( toolName );
	FILE* test = fopen( tool.c_str(), "rb" );
	if( test == NULL )
	{
		common->Printf( "routerMap: no bake program %s: %s not baked\n", tool.c_str(), listsName );
		return true;
	}
	fclose( test );

	// A map with a door list (<aas file>.walk.rtd, written by hand; the game reads it beside the AAS file)
	// needs lists that were baked with that list, and a door layer for its wide doors. That bake is still done
	// by hand: the bake program and the game do not read a door list the same way.
	idStr doorsName = rtgName;
	doorsName.StripFileExtension();
	doorsName += ".rtd";
	if( fileSystem->GetFileLength( doorsName ) >= 0 )
	{
		common->Warning( "routerMap: %s is there: the lists of a map with a door list are baked by hand (the command routerMap writes the router map for it), %s not baked", doorsName.c_str(), listsName );
		return true;
	}

	const idStr rtgOS = fileSystem->RelativePathToOSPath( rtgName, "fs_basepath" );
	const idStr outOS = fileSystem->RelativePathToOSPath( listsName, "fs_basepath" );
	// the command goes through the shell: a path that could end the quoting is refused
	if( strpbrk( tool.c_str(), unsafe ) != NULL || strpbrk( outOS.c_str(), unsafe ) != NULL )
	{
		common->Warning( "routerMap: a path holds %s: %s not baked", unsafeWords, listsName );
		return false;
	}

	// The bake takes its map from the environment. Everything else it would read there is taken out, so that
	// nothing left in the environment by a bake run by hand changes this one.
	idStr command;
#ifdef _WIN32
	_putenv_s( "RT_MAP", rtgOS.c_str() );
	_putenv_s( "OD_DOORS", "bridge" );
	_putenv_s( "RT_DOORS", "" );		// an empty value takes the variable out of the environment
	_putenv_s( "RT_FLY", "" );
	_putenv_s( "OD_WAYPOINTS", "" );
	_putenv_s( "OD_COVER", "" );		//puncho - R5 speed: the bake's test switches ( census.cpp, nl_bake.cpp )
	_putenv_s( "NL_PROOF", "" );
	_putenv_s( "NL_FAULT", "" );
	// the outer pair of quotes is cmd's own; inside it every path is quoted
	command = va( "\"\"%s\" 2 \"%s\" FB 1 0 \"%s\"\"", tool.c_str(), levels, outOS.c_str() );
#else
	setenv( "RT_MAP", rtgOS.c_str(), 1 );
	setenv( "OD_DOORS", "bridge", 1 );
	unsetenv( "RT_DOORS" );
	unsetenv( "RT_FLY" );
	unsetenv( "OD_WAYPOINTS" );
	unsetenv( "OD_COVER" );		//puncho - R5 speed: the bake's test switches ( census.cpp, nl_bake.cpp )
	unsetenv( "NL_PROOF" );
	unsetenv( "NL_FAULT" );
	command = va( "'%s' 2 '%s' FB 1 0 '%s'", tool.c_str(), levels, outOS.c_str() );
#endif
	RemoveRouterLists( listsName );
	common->Printf( "routerMap: baking %s\n", listsName );
	fflush( NULL );		// what this program has printed goes out before the bake prints
	int result = system( command.c_str() );
#ifndef _WIN32
	if( result != -1 && WIFEXITED( result ) )
	{
		result = WEXITSTATUS( result );		// sh hands back a wait status
	}
#endif
	if( result == 0 && fileSystem->GetFileLength( listsName ) < 0 )
	{
		RemoveRouterLists( listsName );		// the certificate, when the bake left one
		common->Warning( "routerMap: the bake ended without an error and left no %s", listsName );
		return false;
	}
	if( result != 0 )
	{
		RemoveRouterLists( listsName );
		if( result == -1 )
		{
			common->Warning( "routerMap: the bake program could not be started: %s not baked", listsName );
		}
		else
		{
			common->Warning( "routerMap: the bake of %s failed (%d)", listsName, result );
		}
		return false;
	}
	common->Printf( "routerMap: baked %s\n", listsName );
	return true;
}

/*
============
WriteRouterFiles

  For every AAS file the map has (with onlyTypes: for the AAS types it names): with bake, the walk lists
  the game reads (<aas file name>.walk.nlw4). The bake program takes its map from a file, the walk router
  map (<aas file name>.walk.rtg), and writes a certificate beside the lists.
  With keepMaps (the command routerMap) both router maps (.walk.rtg and .fly.rtg) are written and stay, and
  so does the certificate. Without it (after an AAS compile) the lists are the only file that stays: the
  fly map is not written, and the walk map and the certificate are removed after the bake, however it ended.
  The fly lists are not baked here: no flying monster has used the router yet, and their bake takes other
  levels.
  False when a map could not be built or written, a bake failed, or no AAS file was found.
============
*/
static bool WriteRouterFiles( const idStr& mapName, bool bake, bool keepMaps, const idStrList* onlyTypes )
{
	static const int masks[2] = { TFL_WALK | TFL_AIR, TFL_WALK | TFL_AIR | TFL_FLY };
	static const char* labels[2] = { "walk", "fly" };
	bool succeeded = true;
	int aasFiles = 0;

	const idDict* dict = FindEntityDefDict( "aas_types", false );
	if( !dict )
	{
		common->Warning( "routerMap: unable to find entityDef for 'aas_types'" );
		return false;
	}

	const idKeyValue* kv = dict->MatchPrefix( "type" );
	while( kv != NULL )
	{
		if( onlyTypes != NULL && onlyTypes->Find( kv->GetValue() ) == NULL )
		{
			kv = dict->MatchPrefix( "type", kv );
			continue;
		}

		idStr name = mapName;
		name.SetFileExtension( kv->GetValue().c_str() );

		idAASFileLocal* file = new idAASFileLocal();
		if( !file->Load( name, 0 ) )
		{
			common->Printf( "routerMap: no AAS file %s\n", name.c_str() );
		}
		else
		{
			aasFiles++;
			// after an AAS compile only the walk map is made: the bake reads it
			const int numMasks = keepMaps ? 2 : 1;
			for( int p = 0; p < numMasks; p++ )
			{
				idAASRouterMap map;
				idStr error;
				if( !map.Build( file, masks[p], error ) )
				{
					common->Warning( "routerMap: %s (%s): %s", name.c_str(), labels[p], error.c_str() );
					succeeded = false;
					continue;
				}

				idStr outName = name + "." + labels[p] + ".rtg";
				idFile* f = fileSystem->OpenFileWrite( outName, "fs_basepath" );
				const bool wrote = ( f != NULL && map.Write( f ) );
				if( f )
				{
					fileSystem->CloseFile( f );
				}
				if( !wrote )
				{
					common->Warning( "routerMap: cannot write %s", outName.c_str() );
					if( !keepMaps )
					{
						fileSystem->RemoveFile( outName.c_str() );		// what was written of it
					}
					succeeded = false;
					continue;
				}
				common->Printf( "routerMap: wrote %s: %d areas, %d reaches\n", outName.c_str(), map.numAreas, map.edges.Num() );

				const idStr listsName = name + "." + labels[p] + ".nlw4";
				if( bake && p == 0 )
				{
					if( !BakeRouterLists( outName.c_str(), listsName.c_str() ) )
					{
						succeeded = false;
					}
				}
				if( !keepMaps )
				{
					// The lists are the only router file an AAS compile leaves. The map was the bake's input
					// (routerMap writes it again), and the certificate repeats what the bake printed.
					fileSystem->RemoveFile( outName.c_str() );
					fileSystem->RemoveFile( va( "%s.certificate.txt", listsName.c_str() ) );
					common->Printf( "routerMap: removed %s again: only the bake reads it\n", outName.c_str() );
				}
			}
		}
		delete file;

		kv = dict->MatchPrefix( "type", kv );
	}

	if( aasFiles == 0 )
	{
		common->Warning( "routerMap: %s has no AAS file", mapName.c_str() );
		succeeded = false;
	}
	return succeeded;
}

/*
============
RouterFilesAfterAAS

  One AAS file was written. Unless aas_routerBake is 0 its walk lists are baked, and they are the only
  router file that stays beside it (WriteRouterFiles without keepMaps); with aas_routerBake 0 nothing is
  written. The files made from the file as it was before are gone already: idAASBuild removes them just
  before it writes (RemoveRouterFiles). A failure here is printed and does not fail the AAS compile: without
  lists the game routes the stock way.
============
*/
static void RouterFilesAfterAAS( const idStr& mapName, const char* aasType )
{
	if( !aas_routerBake.GetBool() )
	{
		return;
	}
	idStrList onlyType;
	onlyType.Append( aasType );
	WriteRouterFiles( mapName, true, false, &onlyType );
}

/*
============
RunRouterMap

  rbdmap -nogui routerMap <mapfile>
  The router's files for a map whose AAS files exist already: for every AAS file both router maps, and,
  unless aas_routerBake is 0, the walk lists with their certificate (WriteRouterFiles with keepMaps). An
  AAS compile (runAAS, runAASDir, runReach, dmap) bakes the lists by itself for the AAS files it wrote and
  leaves only them.
============
*/
bool RunRouterMap( const idCmdArgs& args )
{
	idStr mapName;

	if( args.Argc() <= 1 )
	{
		common->Printf( "routerMap <mapfile>\n" );
		return true;
	}

	common->ClearWarnings( "writing router maps" );

	common->SetRefreshOnPrint( true );

	// the map's name as runAAS takes it
	mapName = args.Argv( 1 );
	mapName.BackSlashesToSlashes();
	if( mapName.Icmpn( "maps/", 4 ) != 0 )
	{
		mapName = "maps/" + mapName;
	}

	const bool succeeded = WriteRouterFiles( mapName, aas_routerBake.GetBool(), true, NULL );

	common->SetRefreshOnPrint( false );
	common->PrintWarnings();
	return succeeded;
}
//puncho end - R2 router
