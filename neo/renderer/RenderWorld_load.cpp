/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2015-2024 Robert Beckebans

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
#include "Model_local.h"


/*
================
idRenderWorldLocal::FreeWorld
================
*/
void idRenderWorldLocal::FreeWorld()
{
	// this will free all the lightDefs and entityDefs
	FreeDefs();

	// free all the portals and check light/model references
	for( int i = 0; i < numPortalAreas; i++ )
	{
		portalArea_t*	area;
		portal_t*		portal, *nextPortal;

		area = &portalAreas[i];
		for( portal = area->portals; portal; portal = nextPortal )
		{
			nextPortal = portal->next;
			delete portal->w;
			R_StaticFree( portal );
		}

		// SRS - release the lightGridPoints idList or it will leak
		area->lightGrid.lightGridPoints.Clear();

		// there shouldn't be any remaining lightRefs or entityRefs
		if( area->lightRefs.areaNext != &area->lightRefs )
		{
			common->Error( "FreeWorld: unexpected remaining lightRefs" );
		}
		if( area->entityRefs.areaNext != &area->entityRefs )
		{
			common->Error( "FreeWorld: unexpected remaining entityRefs" );
		}
	}

	if( portalAreas )
	{
		R_StaticFree( portalAreas );
		portalAreas = NULL;
		numPortalAreas = 0;
		R_StaticFree( areaScreenRect );
		areaScreenRect = NULL;
	}

	if( doublePortals )
	{
		R_StaticFree( doublePortals );
		doublePortals = NULL;
		numInterAreaPortals = 0;
	}

	if( areaNodes )
	{
		R_StaticFree( areaNodes );
		areaNodes = NULL;
	}

	// free all the inline idRenderModels
	for( int i = 0; i < localModels.Num(); i++ )
	{
		renderModelManager->RemoveModel( localModels[i] );
		delete localModels[i];
	}
	localModels.Clear();

	// the chunk origins outlive FreeDefs, which the retain path runs, but not the map
	areaChunks.Clear();

	areaReferenceAllocator.Shutdown();
	interactionAllocator.Shutdown();

	mapName = "<FREED>";
}

/*
================
idRenderWorldLocal::TouchWorldModels
================
*/
void idRenderWorldLocal::TouchWorldModels()
{
	for( int i = 0; i < localModels.Num(); i++ )
	{
		renderModelManager->CheckModel( localModels[i]->Name() );
	}
}

/*
================
idRenderWorldLocal::ReadBinaryShadowModel
================
*/
idRenderModel* idRenderWorldLocal::ReadBinaryModel( idFile* fileIn )
{
	idStrStatic< MAX_OSPATH > name;
	fileIn->ReadString( name );
	idRenderModel* model = renderModelManager->AllocModel();
	model->InitEmpty( name );

	// RB: declSourceTimeStamp is not important here
	if( model->LoadBinaryModel( fileIn, mapTimeStamp, 0 ) )
	{
		return model;
	}
	return NULL;
}

extern idCVar binaryLoadRenderModels;

/*
================
R_AreaNumForModelName

Returns N for a model named exactly "_area<N>", the name dmap writes and
AddWorldModelEntities looks up, else -1. The "_area" prefix test in
idRenderModelStatic::InitEmpty also takes an inline model a mapper named "_area...",
and only world area models may move to a chunk origin.
================
*/
static int R_AreaNumForModelName( const char* name )
{
	if( idStr::Cmpn( name, "_area", 5 ) != 0 )
	{
		return -1;
	}

	// only what va( "_area%i", N ) prints for N >= 0: digits, no sign, no leading zero,
	// and at most nine of them so the number fits an int
	const char* digits = name + 5;
	const int numDigits = idStr::Length( digits );
	if( numDigits < 1 || numDigits > 9 || ( digits[0] == '0' && numDigits > 1 ) )
	{
		return -1;
	}

	int areaNum = 0;
	for( int i = 0; i < numDigits; i++ )
	{
		if( digits[i] < '0' || digits[i] > '9' )
		{
			return -1;
		}
		areaNum = areaNum * 10 + ( digits[i] - '0' );
	}
	return areaNum;
}

/*
================
idRenderWorldLocal::SetAreaChunk

Areas the list grows past get origin 0 and cleared bounds until their own model
arrives; AddWorldModelEntities reads only areas whose model it found.
================
*/
void idRenderWorldLocal::SetAreaChunk( int areaNum, const idVec3& origin, const idBounds& absBounds )
{
	while( areaChunks.Num() <= areaNum )
	{
		areaChunk_t& chunk = areaChunks.Alloc();
		chunk.origin.Zero();
		chunk.absBounds.Clear();
	}
	areaChunks[areaNum].origin = origin;
	areaChunks[areaNum].absBounds = absBounds;
}

/*
================
idRenderWorldLocal::ParseModel
================
*/
idRenderModel* idRenderWorldLocal::ParseModel( idLexer* src, const char* mapName, ID_TIME_T mapTimeStamp, idFile* fileOut, const int procVersion )
{
	idToken token;

	src->ExpectTokenString( "{" );

	// parse the name
	src->ExpectAnyToken( &token );

	idRenderModel* model = renderModelManager->AllocModel();
	model->InitEmpty( token );

	if( fileOut != NULL )
	{
		// write out the type so the binary reader knows what to instantiate
		fileOut->WriteString( "model" );
		fileOut->WriteString( token );
	}

	int numSurfaces = src->ParseInt();
	if( numSurfaces < 0 )
	{
		src->Error( "R_ParseModel: bad numSurfaces" );
	}

	for( int i = 0; i < numSurfaces; i++ )
	{
		src->ExpectTokenString( "{" );

		src->ExpectAnyToken( &token );

		modelSurface_t surf;
		surf.shader = declManager->FindMaterial( token );

		( ( idMaterial* )surf.shader )->AddReference();

		srfTriangles_t* tri = R_AllocStaticTriSurf();
		surf.geometry = tri;

		tri->numVerts = src->ParseInt();
		tri->numIndexes = src->ParseInt();

		// 004 names the vertex layout of each surface, 003 is always the basic one
		int vertexFormat = PROC_VERTEX_FORMAT_BASIC;
		if( procVersion >= 4 )
		{
			vertexFormat = src->ParseInt();
			if( vertexFormat != PROC_VERTEX_FORMAT_BASIC && vertexFormat != PROC_VERTEX_FORMAT_COLOR )
			{
				src->Error( "idRenderWorldLocal::ParseModel: model '%s' surface %i '%s': bad vertexFormat %i", model->Name(), i, surf.shader->GetName(), vertexFormat );
			}
		}
		const int floatsPerVert = ( vertexFormat == PROC_VERTEX_FORMAT_COLOR ) ? PROC_VERTEX_FLOATS_COLOR : PROC_VERTEX_FLOATS_BASIC;

		// parse the vertices
		idTempArray<float> verts( tri->numVerts * floatsPerVert );
		for( int j = 0; j < tri->numVerts; j++ )
		{
			src->Parse1DMatrix( floatsPerVert, &verts[j * floatsPerVert] );
		}

		// parse the indices; triIndex_t would wrap a bad one silently, and the island
		// search below indexes vertIslands with them
		idTempArray<triIndex_t> indexes( tri->numIndexes );
		for( int j = 0; j < tri->numIndexes; j++ )
		{
			const int index = src->ParseInt();
			if( index < 0 || index >= tri->numVerts )
			{
				src->Error( "idRenderWorldLocal::ParseModel: model '%s' surface %i '%s': index %i out of range (%i verts)", model->Name(), i, surf.shader->GetName(), index, tri->numVerts );
			}
			indexes[j] = ( triIndex_t )index;
		}

#if 1
		// find the island that each vertex belongs to
		idTempArray<int> vertIslands( tri->numVerts );
		idTempArray<bool> trisVisited( tri->numIndexes );
		vertIslands.Zero();
		trisVisited.Zero();
		int numIslands = 0;
		for( int j = 0; j < tri->numIndexes; j += 3 )
		{
			if( trisVisited[j] )
			{
				continue;
			}

			int islandNum = ++numIslands;
			vertIslands[indexes[j + 0]] = islandNum;
			vertIslands[indexes[j + 1]] = islandNum;
			vertIslands[indexes[j + 2]] = islandNum;
			trisVisited[j] = true;

			idList<int> queue;
			queue.Append( j );
			for( int n = 0; n < queue.Num(); n++ )
			{
				int t = queue[n];
				for( int k = 0; k < tri->numIndexes; k += 3 )
				{
					if( trisVisited[k] )
					{
						continue;
					}
					bool connected =	indexes[t + 0] == indexes[k + 0] || indexes[t + 0] == indexes[k + 1] || indexes[t + 0] == indexes[k + 2] ||
										indexes[t + 1] == indexes[k + 0] || indexes[t + 1] == indexes[k + 1] || indexes[t + 1] == indexes[k + 2] ||
										indexes[t + 2] == indexes[k + 0] || indexes[t + 2] == indexes[k + 1] || indexes[t + 2] == indexes[k + 2];
					if( connected )
					{
						vertIslands[indexes[k + 0]] = islandNum;
						vertIslands[indexes[k + 1]] = islandNum;
						vertIslands[indexes[k + 2]] = islandNum;
						trisVisited[k] = true;
						queue.Append( k );
					}
				}
			}
		}

		// center the texture coordinates for each island for maximum 16-bit precision
		for( int j = 1; j <= numIslands; j++ )
		{
			float minS = idMath::INFINITUM;
			float minT = idMath::INFINITUM;
			float maxS = -idMath::INFINITUM;
			float maxT = -idMath::INFINITUM;
			for( int k = 0; k < tri->numVerts; k++ )
			{
				if( vertIslands[k] == j )
				{
					minS = Min( minS, verts[k * floatsPerVert + 3] );
					maxS = Max( maxS, verts[k * floatsPerVert + 3] );
					minT = Min( minT, verts[k * floatsPerVert + 4] );
					maxT = Max( maxT, verts[k * floatsPerVert + 4] );
				}
			}
			const float averageS = idMath::Ftoi( ( minS + maxS ) * 0.5f );
			const float averageT = idMath::Ftoi( ( minT + maxT ) * 0.5f );
			for( int k = 0; k < tri->numVerts; k++ )
			{
				if( vertIslands[k] == j )
				{
					verts[k * floatsPerVert + 3] -= averageS;
					verts[k * floatsPerVert + 4] -= averageT;
				}
			}
		}
#endif

		// the verts come from an uncleared allocation, so a surface without colour used to draw and
		// cache whatever color and color2 bytes were left in it; the basic format is opaque white
		const dword opaqueWhite = PackColor( colorWhite );

		R_AllocStaticTriSurfVerts( tri, tri->numVerts );
		for( int j = 0; j < tri->numVerts; j++ )
		{
			tri->verts[j].Clear();
			tri->verts[j].xyz[0] = verts[j * floatsPerVert + 0];
			tri->verts[j].xyz[1] = verts[j * floatsPerVert + 1];
			tri->verts[j].xyz[2] = verts[j * floatsPerVert + 2];
			tri->verts[j].SetTexCoord( verts[j * floatsPerVert + 3], verts[j * floatsPerVert + 4] );
			tri->verts[j].SetNormal( verts[j * floatsPerVert + 5], verts[j * floatsPerVert + 6], verts[j * floatsPerVert + 7] );
			if( vertexFormat == PROC_VERTEX_FORMAT_COLOR )
			{
				// rounded: dmap wrote byte / 255 with six decimals, which can parse a hair below the byte
				tri->verts[j].SetColor( PackColorRound( idVec4( verts[j * floatsPerVert + 8], verts[j * floatsPerVert + 9], verts[j * floatsPerVert + 10], verts[j * floatsPerVert + 11] ) ) );
			}
			else
			{
				tri->verts[j].SetColor( opaqueWhite );
			}
		}

		R_AllocStaticTriSurfIndexes( tri, tri->numIndexes );
		for( int j = 0; j < tri->numIndexes; j++ )
		{
			tri->indexes[j] = indexes[j];
		}
		src->ExpectTokenString( "}" );

		// add the completed surface to the model
		model->AddSurface( surf );
	}

	src->ExpectTokenString( "}" );

	// a world area model moves to its chunk origin here, before FinishSurfaces derives
	// the bounds, the MOC triangles and the .bproc blob from its vertices
	idVec3 chunkOrigin = vec3_origin;
	idBounds absBounds;
	absBounds.Clear();
	const int areaNum = R_AreaNumForModelName( model->Name() );
	if( areaNum >= 0 )
	{
		// the bounds FinishSurfaces would give the absolute vertices: R_BoundTriSurf as
		// R_CleanupTriangles runs it, then the shared pass. The back-side copies and mirrored
		// vertices FinishSurfaces adds repeat existing positions and cannot change the result
		const int numAreaSurfaces = model->NumSurfaces();
		idTempArray<modelSurface_t> areaSurfaces( numAreaSurfaces );
		for( int i = 0; i < numAreaSurfaces; i++ )
		{
			areaSurfaces[i] = *model->Surface( i );
			R_BoundTriSurf( areaSurfaces[i].geometry );
		}
		R_BoundModelSurfaces( areaSurfaces.Ptr(), numAreaSurfaces, absBounds );

		// snap the centre to 1024 in double: an integer origin comes off dmap's 1/32 grid
		// exactly, and 1024 is a multiple of every light grid size
		if( !absBounds.IsCleared() )
		{
			for( int i = 0; i < 3; i++ )
			{
				const double center = ( ( double )absBounds[0][i] + ( double )absBounds[1][i] ) * 0.5;
				chunkOrigin[i] = ( float )( 1024.0 * floor( center / 1024.0 + 0.5 ) );
			}
		}

		for( int i = 0; i < numAreaSurfaces; i++ )
		{
			srfTriangles_t* tri = areaSurfaces[i].geometry;
			for( int j = 0; j < tri->numVerts; j++ )
			{
				tri->verts[j].xyz -= chunkOrigin;
			}
		}

		SetAreaChunk( areaNum, chunkOrigin, absBounds );
	}

	// RB: FIXME add check for mikktspace
	model->FinishSurfaces( false );

	if( fileOut != NULL && model->SupportsBinaryModel() && binaryLoadRenderModels.GetBool() )
	{
		model->WriteBinaryModel( fileOut, &mapTimeStamp );

		// .bproc v3: every model blob is followed by its chunk origin and absolute bounds,
		// zero and cleared for a model that is not a world area, so read and write mirror
		fileOut->WriteVec3( chunkOrigin );
		fileOut->WriteBigArray( absBounds.ToFloatPtr(), 6 );
	}

	return model;
}

/*
================
idRenderWorldLocal::SetupAreaRefs
================
*/
void idRenderWorldLocal::SetupAreaRefs()
{
	connectedAreaNum = 0;
	for( int i = 0; i < numPortalAreas; i++ )
	{
		portalAreas[i].areaNum = i;

		portalAreas[i].lightRefs.areaNext =
			portalAreas[i].lightRefs.areaPrev = &portalAreas[i].lightRefs;

		portalAreas[i].entityRefs.areaNext =
			portalAreas[i].entityRefs.areaPrev = &portalAreas[i].entityRefs;

		portalAreas[i].envprobeRefs.areaNext =
			portalAreas[i].envprobeRefs.areaPrev = &portalAreas[i].envprobeRefs;
	}
}

/*
================
idRenderWorldLocal::ParseInterAreaPortals
================
*/
void idRenderWorldLocal::ParseInterAreaPortals( idLexer* src, idFile* fileOut )
{
	src->ExpectTokenString( "{" );

	numPortalAreas = src->ParseInt();
	if( numPortalAreas < 0 )
	{
		src->Error( "R_ParseInterAreaPortals: bad numPortalAreas" );
		return;
	}

	if( fileOut != NULL )
	{
		// write out the type so the binary reader knows what to instantiate
		fileOut->WriteString( "interAreaPortals" );
	}


	portalAreas = ( portalArea_t* )R_ClearedStaticAlloc( numPortalAreas * sizeof( portalAreas[0] ) );
	areaScreenRect = ( idScreenRect* ) R_ClearedStaticAlloc( numPortalAreas * sizeof( idScreenRect ) );

	// set the doubly linked lists
	SetupAreaRefs();

	numInterAreaPortals = src->ParseInt();
	if( numInterAreaPortals < 0 )
	{
		src->Error( "R_ParseInterAreaPortals: bad numInterAreaPortals" );
		return;
	}

	if( fileOut != NULL )
	{
		fileOut->WriteBig( numPortalAreas );
		fileOut->WriteBig( numInterAreaPortals );
	}

	doublePortals = ( doublePortal_t* )R_ClearedStaticAlloc( numInterAreaPortals *
					sizeof( doublePortals [0] ) );

	for( int i = 0; i < numInterAreaPortals; i++ )
	{
		int		numPoints, a1, a2;
		idWinding*	w;
		portal_t*	p;

		numPoints = src->ParseInt();
		a1 = src->ParseInt();
		a2 = src->ParseInt();

		if( fileOut != NULL )
		{
			fileOut->WriteBig( numPoints );
			fileOut->WriteBig( a1 );
			fileOut->WriteBig( a2 );
		}

		w = new( TAG_RENDER_WINDING ) idWinding( numPoints );
		w->SetNumPoints( numPoints );
		for( int j = 0; j < numPoints; j++ )
		{
			src->Parse1DMatrix( 3, ( *w )[j].ToFloatPtr() );

			if( fileOut != NULL )
			{
				fileOut->WriteBig( ( *w )[j].x );
				fileOut->WriteBig( ( *w )[j].y );
				fileOut->WriteBig( ( *w )[j].z );
			}
			// no texture coordinates
			( *w )[j][3] = 0;
			( *w )[j][4] = 0;
		}

		// add the portal to a1
		p = ( portal_t* )R_ClearedStaticAlloc( sizeof( *p ) );
		p->intoArea = a2;
		p->doublePortal = &doublePortals[i];
		p->w = w;
		p->w->GetPlane( p->plane );

		p->next = portalAreas[a1].portals;
		portalAreas[a1].portals = p;

		doublePortals[i].portals[0] = p;

		// reverse it for a2
		p = ( portal_t* )R_ClearedStaticAlloc( sizeof( *p ) );
		p->intoArea = a1;
		p->doublePortal = &doublePortals[i];
		p->w = w->Reverse();
		p->w->GetPlane( p->plane );

		p->next = portalAreas[a2].portals;
		portalAreas[a2].portals = p;

		doublePortals[i].portals[1] = p;
	}

	src->ExpectTokenString( "}" );
}

/*
================
idRenderWorldLocal::ParseInterAreaPortals
================
*/
void idRenderWorldLocal::ReadBinaryAreaPortals( idFile* file )
{

	file->ReadBig( numPortalAreas );
	file->ReadBig( numInterAreaPortals );

	portalAreas = ( portalArea_t* )R_ClearedStaticAlloc( numPortalAreas * sizeof( portalAreas[0] ) );
	areaScreenRect = ( idScreenRect* ) R_ClearedStaticAlloc( numPortalAreas * sizeof( idScreenRect ) );

	// set the doubly linked lists
	SetupAreaRefs();

	doublePortals = ( doublePortal_t* )R_ClearedStaticAlloc( numInterAreaPortals * sizeof( doublePortals [0] ) );

	for( int i = 0; i < numInterAreaPortals; i++ )
	{
		int		numPoints, a1, a2;
		idWinding*	w;
		portal_t*	p;

		file->ReadBig( numPoints );
		file->ReadBig( a1 );
		file->ReadBig( a2 );

		w = new( TAG_RENDER_WINDING ) idWinding( numPoints );
		w->SetNumPoints( numPoints );

		for( int j = 0; j < numPoints; j++ )
		{
			file->ReadBig( ( *w )[ j ][ 0 ] );
			file->ReadBig( ( *w )[ j ][ 1 ] );
			file->ReadBig( ( *w )[ j ][ 2 ] );

			// no texture coordinates
			( *w )[ j ][ 3 ] = 0;
			( *w )[ j ][ 4 ] = 0;
		}

		// add the portal to a1
		p = ( portal_t* )R_ClearedStaticAlloc( sizeof( *p ) );
		p->intoArea = a2;
		p->doublePortal = &doublePortals[i];
		p->w = w;
		p->w->GetPlane( p->plane );

		p->next = portalAreas[a1].portals;
		portalAreas[a1].portals = p;

		doublePortals[i].portals[0] = p;

		// reverse it for a2
		p = ( portal_t* )R_ClearedStaticAlloc( sizeof( *p ) );
		p->intoArea = a1;
		p->doublePortal = &doublePortals[i];
		p->w = w->Reverse();
		p->w->GetPlane( p->plane );

		p->next = portalAreas[a2].portals;
		portalAreas[a2].portals = p;

		doublePortals[i].portals[1] = p;
	}
}


/*
================
idRenderWorldLocal::ParseNodes
================
*/
void idRenderWorldLocal::ParseNodes( idLexer* src, idFile* fileOut )
{
	src->ExpectTokenString( "{" );

	numAreaNodes = src->ParseInt();
	if( numAreaNodes < 0 )
	{
		src->Error( "R_ParseNodes: bad numAreaNodes" );
	}
	areaNodes = ( areaNode_t* )R_ClearedStaticAlloc( numAreaNodes * sizeof( areaNodes[0] ) );

	if( fileOut != NULL )
	{
		// write out the type so the binary reader knows what to instantiate
		fileOut->WriteString( "nodes" );
	}

	if( fileOut != NULL )
	{
		fileOut->WriteBig( numAreaNodes );
	}

	for( int i = 0; i < numAreaNodes; i++ )
	{
		areaNode_t*	node;

		node = &areaNodes[i];

		src->Parse1DMatrix( 4, node->plane.ToFloatPtr() );

		node->children[0] = src->ParseInt();
		node->children[1] = src->ParseInt();

		if( fileOut != NULL )
		{
			fileOut->WriteBig( node->plane[ 0 ] );
			fileOut->WriteBig( node->plane[ 1 ] );
			fileOut->WriteBig( node->plane[ 2 ] );
			fileOut->WriteBig( node->plane[ 3 ] );
			fileOut->WriteBig( node->children[ 0 ] );
			fileOut->WriteBig( node->children[ 1 ] );
		}

	}

	src->ExpectTokenString( "}" );
}

/*
================
idRenderWorldLocal::ReadBinaryNodes
================
*/
void idRenderWorldLocal::ReadBinaryNodes( idFile* file )
{
	file->ReadBig( numAreaNodes );
	areaNodes = ( areaNode_t* )R_ClearedStaticAlloc( numAreaNodes * sizeof( areaNodes[0] ) );
	for( int i = 0; i < numAreaNodes; i++ )
	{
		areaNode_t* node = &areaNodes[ i ];
		file->ReadBig( node->plane[ 0 ] );
		file->ReadBig( node->plane[ 1 ] );
		file->ReadBig( node->plane[ 2 ] );
		file->ReadBig( node->plane[ 3 ] );
		file->ReadBig( node->children[ 0 ] );
		file->ReadBig( node->children[ 1 ] );
	}
}

/*
================
idRenderWorldLocal::CommonChildrenArea_r
================
*/
int idRenderWorldLocal::CommonChildrenArea_r( areaNode_t* node )
{
	int	nums[2];

	for( int i = 0; i < 2; i++ )
	{
		if( node->children[i] <= 0 )
		{
			nums[i] = -1 - node->children[i];
		}
		else
		{
			nums[i] = CommonChildrenArea_r( &areaNodes[ node->children[i] ] );
		}
	}

	// solid nodes will match any area
	if( nums[0] == AREANUM_SOLID )
	{
		nums[0] = nums[1];
	}
	if( nums[1] == AREANUM_SOLID )
	{
		nums[1] = nums[0];
	}

	int	common;
	if( nums[0] == nums[1] )
	{
		common = nums[0];
	}
	else
	{
		common = CHILDREN_HAVE_MULTIPLE_AREAS;
	}

	node->commonChildrenArea = common;

	return common;
}

/*
=================
idRenderWorldLocal::ClearWorld

Sets up for a single area world
=================
*/
void idRenderWorldLocal::ClearWorld()
{
	numPortalAreas = 1;
	portalAreas = ( portalArea_t* )R_ClearedStaticAlloc( sizeof( portalAreas[0] ) );
	areaScreenRect = ( idScreenRect* ) R_ClearedStaticAlloc( sizeof( idScreenRect ) );

	SetupAreaRefs();

	// even though we only have a single area, create a node
	// that has both children pointing at it so we don't need to
	//
	areaNodes = ( areaNode_t* )R_ClearedStaticAlloc( sizeof( areaNodes[0] ) );
	areaNodes[0].plane[3] = 1;
	areaNodes[0].children[0] = -1;
	areaNodes[0].children[1] = -1;
}

/*
=================
idRenderWorldLocal::FreeDefs

dump all the interactions
=================
*/
void idRenderWorldLocal::FreeDefs()
{
	generateAllInteractionsCalled = false;

	if( interactionTable )
	{
		R_StaticFree( interactionTable );
		interactionTable = NULL;
	}

	// free all lightDefs
	for( int i = 0; i < lightDefs.Num(); i++ )
	{
		idRenderLightLocal* light = lightDefs[i];
		if( light != NULL && light->world == this )
		{
			FreeLightDef( i );
			lightDefs[i] = NULL;
		}
	}

	// free all entityDefs
	for( int i = 0; i < entityDefs.Num(); i++ )
	{
		idRenderEntityLocal*	 mod = entityDefs[i];
		if( mod != NULL && mod->world == this )
		{
			FreeEntityDef( i );
			entityDefs[i] = NULL;
		}
	}

	// RB: free all envprobeDefs
	for( int i = 0; i < envprobeDefs.Num(); i++ )
	{
		RenderEnvprobeLocal* ep = envprobeDefs[i];
		if( ep != NULL && ep->world == this )
		{
			FreeEnvprobeDef( i );
			envprobeDefs[i] = NULL;
		}
	}
	// RB end

	// Reset decals and overlays
	for( int i = 0; i < decals.Num(); i++ )
	{
		decals[i].entityHandle = -1;
		decals[i].lastStartTime = 0;
	}
	for( int i = 0; i < overlays.Num(); i++ )
	{
		overlays[i].entityHandle = -1;
		overlays[i].lastStartTime = 0;
	}
}

/*
=================
idRenderWorldLocal::InitFromMap

A NULL or empty name will make a world without a map model, which
is still useful for displaying a bare model
=================
*/
bool idRenderWorldLocal::InitFromMap( const char* name )
{
	idLexer* 		src;
	idToken			token;
	idRenderModel* 	lastModel;

	// if this is an empty world, initialize manually
	if( !name || !name[0] )
	{
		FreeWorld();
		mapName.Clear();
		ClearWorld();
		return true;
	}

	// load it
	idStrStatic< MAX_OSPATH > filename = name;
	filename.SetFileExtension( PROC_FILE_EXT );

	// check for generated file
	idStrStatic< MAX_OSPATH > generatedFileName = filename;
	generatedFileName.Insert( "generated/", 0 );
	generatedFileName.SetFileExtension( "bproc" );

	// if we are reloading the same map, check the timestamp
	// and try to skip all the work
	ID_TIME_T currentTimeStamp = fileSystem->GetTimestamp( filename );

	if( name == mapName )
	{
		if( fileSystem->InProductionMode() || ( currentTimeStamp != FILE_NOT_FOUND_TIMESTAMP && currentTimeStamp == mapTimeStamp ) )
		{
			common->Printf( "idRenderWorldLocal::InitFromMap: retaining existing map\n" );
			FreeDefs();
			TouchWorldModels();
			AddWorldModelEntities();
			ClearPortalStates();
			SetupLightGrid();

			// a map restart is a camera cut too: the next main view drops the TAA history
			tr.invalidateViewHistory = true;
			return true;
		}
		common->Printf( "idRenderWorldLocal::InitFromMap: timestamp has changed, reloading.\n" );
	}

	FreeWorld();

	// see if we have a generated version of this
	// versions: 1 BFG, 2 MOC data, 3 vertex colour (opaque white for a surface without), 4 the same
	// plus a chunk origin and absolute bounds after every model blob. Camera-relative builds made
	// before the vertex colour change wrote their chunk data as a 3 too, so a 3 is never read: it
	// falls through to the .proc parse, which rewrites it
	static const byte BPROC_VERSION_BFG = 1;
	static const byte BPROC_VERSION_MOC_DATA = 2;
	static const byte BPROC_VERSION_AREA_CHUNKS = 4;
	static const byte BPROC_VERSION = BPROC_VERSION_AREA_CHUNKS;


	static const unsigned int BPROC_MAGIC_BFG = ( 'P' << 24 ) | ( 'R' << 16 ) | ( 'O' << 8 ) | BPROC_VERSION_BFG;
	static const unsigned int BPROC_MAGIC_MOC_DATA = ( 'P' << 24 ) | ( 'R' << 16 ) | ( 'O' << 8 ) | BPROC_VERSION_MOC_DATA;
	static const unsigned int BPROC_MAGIC = ( 'P' << 24 ) | ( 'R' << 16 ) | ( 'O' << 8 ) | BPROC_VERSION;
	bool loaded = false;
	idFileLocal file( fileSystem->OpenFileReadMemory( generatedFileName ) );
	if( file != NULL )
	{
		int numEntries = 0;
		int magic = 0;
		file->ReadBig( magic );

		// version 1 or 2 caches ship in the retail .resources without a .proc; in production mode the cache
		// is trusted like every other cache (the BRM and BCM loaders). Otherwise an old cache beside a .proc
		// is stale, the version bump is what makes it stale: the .proc is parsed again (opaque white verts)
		// and the cache rewritten
		const bool oldCache = ( magic == BPROC_MAGIC_BFG || magic == BPROC_MAGIC_MOC_DATA );
		if( magic == BPROC_MAGIC || ( oldCache && ( fileSystem->InProductionMode() || currentTimeStamp == FILE_NOT_FOUND_TIMESTAMP ) ) )
		{
			file->ReadBig( numEntries );
			file->ReadString( mapName );
			file->ReadBig( mapTimeStamp );
			loaded = true;
			for( int i = 0; i < numEntries; i++ )
			{
				idStrStatic< MAX_OSPATH > type;
				file->ReadString( type );
				type.ToLower();
				if( type == "model" )
				{
					idRenderModel* lastModel = ReadBinaryModel( file );
					if( lastModel == NULL )
					{
						loaded = false;
						break;
					}
					renderModelManager->AddModel( lastModel );
					localModels.Append( lastModel );

					// the chunk origin and absolute bounds ParseModel wrote after the blob; an old cache has none
					if( magic == BPROC_MAGIC )
					{
						idVec3 chunkOrigin;
						idBounds absBounds;
						const int originBytes = file->ReadVec3( chunkOrigin );
						const size_t boundsBytes = file->ReadBigArray( absBounds.ToFloatPtr(), 6 );
						if( originBytes != ( int )sizeof( idVec3 ) || boundsBytes != 6 * sizeof( float ) )
						{
							loaded = false;
							break;
						}
						const int areaNum = R_AreaNumForModelName( lastModel->Name() );
						if( areaNum >= 0 )
						{
							SetAreaChunk( areaNum, chunkOrigin, absBounds );
						}
					}
				}
				else if( type == "shadowmodel" && magic == BPROC_MAGIC_BFG )
				{
					// RB: the original BFG .bproc just saved all models as "shadowmodel"
					idRenderModel* lastModel = ReadBinaryModel( file );
					if( lastModel == NULL )
					{
						loaded = false;
						break;
					}
					renderModelManager->AddModel( lastModel );
					localModels.Append( lastModel );
				}
				else if( type == "interareaportals" )
				{
					ReadBinaryAreaPortals( file );
				}
				else if( type == "nodes" )
				{
					ReadBinaryNodes( file );
				}
				else
				{
					idLib::Error( "Binary proc file failed, unexpected type %s\n", type.c_str() );
				}
			}

			// an old cache's area models kept their absolute vertices: chunk origin 0, and the absolute
			// bounds are the model's own, as before chunk origins
			if( loaded && magic != BPROC_MAGIC )
			{
				for( int i = 0; i < localModels.Num(); i++ )
				{
					const int areaNum = R_AreaNumForModelName( localModels[i]->Name() );
					if( areaNum >= 0 )
					{
						SetAreaChunk( areaNum, vec3_origin, localModels[i]->Bounds() );
					}
				}
			}

			if( loaded )
			{
				common->Printf( "idRenderWorldLocal::InitFromMap: loaded %s\n", generatedFileName.c_str() );
			}
		}
		else
		{
			common->Printf( "idRenderWorldLocal::InitFromMap: %s is not a version %i .bproc, parsing %s\n", generatedFileName.c_str(), BPROC_VERSION, filename.c_str() );
		}
	}

	if( !loaded )
	{
		// a .bproc that failed part way may have recorded some areas; the parse records them all
		areaChunks.Clear();

		src = new( TAG_RENDER ) idLexer( filename, LEXFL_NOSTRINGCONCAT | LEXFL_NODOLLARPRECOMPILE );
		if( !src->IsLoaded() )
		{
			common->Printf( "idRenderWorldLocal::InitFromMap: %s not found\n", filename.c_str() );
			ClearWorld();
			return false;
		}


		mapName = name;
		mapTimeStamp = currentTimeStamp;

		if( !src->ReadToken( &token ) || ProcFileVersion( token ) == 0 )
		{
			common->Printf( "idRenderWorldLocal::InitFromMap: bad id '%s' instead of '%s' or '%s'\n", token.c_str(), PROC_FILE_ID, PROC_FILE_ID_003 );
			delete src;
			return false;
		}
		const int procVersion = ProcFileVersion( token );

		int numEntries = 0;
		idFileLocal outputFile( fileSystem->OpenFileWrite( generatedFileName, "fs_basepath" ) );
		if( outputFile != NULL )
		{
			int magic = BPROC_MAGIC;
			outputFile->WriteBig( magic );
			outputFile->WriteBig( numEntries );
			outputFile->WriteString( mapName );
			outputFile->WriteBig( mapTimeStamp );
		}

		// parse the file
		while( 1 )
		{
			if( !src->ReadToken( &token ) )
			{
				break;
			}

			common->UpdateLevelLoadPacifier();


			if( token == "model" )
			{
				lastModel = ParseModel( src, name, currentTimeStamp, outputFile, procVersion );

				// add it to the model manager list
				renderModelManager->AddModel( lastModel );

				// save it in the list to free when clearing this map
				localModels.Append( lastModel );

				numEntries++;

				continue;
			}

			if( token == "shadowModel" )
			{
				// RB: just parse the model but don't do anything with it
				//lastModel = ParseShadowModel( src, outputFile );
				src->SkipBracedSection();
				lastModel = NULL;
				continue;
			}

			if( token == "interAreaPortals" )
			{
				ParseInterAreaPortals( src, outputFile );

				numEntries++;
				continue;
			}

			if( token == "nodes" )
			{
				ParseNodes( src, outputFile );

				numEntries++;
				continue;
			}

			src->Error( "idRenderWorldLocal::InitFromMap: bad token \"%s\"", token.c_str() );
		}

		delete src;

		if( outputFile != NULL )
		{
			outputFile->Seek( 0, FS_SEEK_SET );
			int magic = BPROC_MAGIC;
			outputFile->WriteBig( magic );
			outputFile->WriteBig( numEntries );
		}

	}



	// if it was a trivial map without any areas, create a single area
	if( !numPortalAreas )
	{
		ClearWorld();
	}

	// find the points where we can early-our of reference pushing into the BSP tree
	CommonChildrenArea_r( &areaNodes[0] );

	AddWorldModelEntities();
	ClearPortalStates();
	SetupLightGrid();

	// the next main view drops the previous map's motion vector and TAA history; the loader never
	// writes backend state, so this frontend flag rides to the backend on that view's viewDef
	tr.invalidateViewHistory = true;

	// done!
	return true;
}

/*
=====================
idRenderWorldLocal::ClearPortalStates
=====================
*/
void idRenderWorldLocal::ClearPortalStates()
{
	// all portals start off open
	for( int i = 0; i < numInterAreaPortals; i++ )
	{
		doublePortals[i].blockingBits = PS_BLOCK_NONE;
	}

	// flood fill all area connections
	for( int i = 0; i < numPortalAreas; i++ )
	{
		for( int j = 0; j < NUM_PORTAL_ATTRIBUTES; j++ )
		{
			connectedAreaNum++;
			FloodConnectedAreas( &portalAreas[i], j );
		}
	}
}

/*
=====================
idRenderWorldLocal::AddWorldModelEntities
=====================
*/
void idRenderWorldLocal::AddWorldModelEntities()
{
	// add the world model for each portal area
	// we can't just call AddEntityDef, because that would place the references
	// based on the bounding box, rather than explicitly into the correct area
	for( int i = 0; i < numPortalAreas; i++ )
	{
		common->UpdateLevelLoadPacifier();

		idRenderEntityLocal*	 def = new( TAG_RENDER_ENTITY ) idRenderEntityLocal;

		// try and reuse a free spot
		int index = entityDefs.FindNull();
		if( index == -1 )
		{
			index = entityDefs.Append( def );
		}
		else
		{
			entityDefs[index] = def;
		}

		def->index = index;
		def->world = this;

		def->parms.hModel = renderModelManager->FindModel( va( "_area%i", i ) );
		if( def->parms.hModel->IsDefaultModel() || !def->parms.hModel->IsStaticWorldModel() )
		{
			common->Error( "idRenderWorldLocal::InitFromMap: bad area model lookup" );
		}

		idRenderModel* hModel = def->parms.hModel;

		for( int j = 0; j < hModel->NumSurfaces(); j++ )
		{
			const modelSurface_t* surf = hModel->Surface( j );

			if( surf->shader->GetName() == idStr( "textures/smf/portal_sky" ) ||
					surf->shader->IsPortalSky() )
			{
				def->needsPortalSky = true;
			}
		}

		if( i >= areaChunks.Num() )
		{
			common->Error( "idRenderWorldLocal::InitFromMap: no chunk origin for area %i", i );
		}
		const areaChunk_t& chunk = areaChunks[i];

		// ParseModel moved the area model to its chunk origin, so the model bounds are local
		// to it; R_DeriveEntityData derives the global ones from them and the origin
		def->localReferenceBounds = def->parms.hModel->Bounds();

		def->parms.origin = chunk.origin;
		def->parms.axis[0][0] = 1.0f;
		def->parms.axis[1][1] = 1.0f;
		def->parms.axis[2][2] = 1.0f;

		// in case an explicit shader is used on the world, we don't
		// want it to have a 0 alpha or color
		def->parms.shaderParms[0] = 1.0f;
		def->parms.shaderParms[1] = 1.0f;
		def->parms.shaderParms[2] = 1.0f;
		def->parms.shaderParms[3] = 1.0f;

		R_DeriveEntityData( def );

		portalArea_t* area = &portalAreas[i];
		AddEntityRefToArea( def, area );

		// RB: remember BSP area AABB for quick lookup later
		// derived as R_DeriveEntityData did it when area models sat at origin 0, from the absolute
		// bounds and never from the relocated def: the light grid origin and the auto-placed probe
		// bake keys come from globalBounds, so it must not move by an ulp
		idRenderMatrix areaModelMatrix;
		idRenderMatrix::CreateFromOriginAxis( vec3_origin, mat3_identity, areaModelMatrix );
		idRenderMatrix areaInverseBaseModelProject;
		idRenderMatrix::OffsetScaleForBounds( areaModelMatrix, chunk.absBounds, areaInverseBaseModelProject );
		idRenderMatrix::ProjectedBounds( area->globalBounds, areaInverseBaseModelProject, bounds_unitCube, false );
	}
}

/*
=====================
CheckAreaForPortalSky
=====================
*/
bool idRenderWorldLocal::CheckAreaForPortalSky( int areaNum )
{
	assert( areaNum >= 0 && areaNum < numPortalAreas );

	for( areaReference_t* ref = portalAreas[areaNum].entityRefs.areaNext; ref->entity; ref = ref->areaNext )
	{
		assert( ref->area == &portalAreas[areaNum] );

		if( ref->entity && ref->entity->needsPortalSky )
		{
			return true;
		}
	}

	return false;
}

/*
=====================
ResetLocalRenderModels
=====================
*/
void idRenderWorldLocal::ResetLocalRenderModels()
{
	localModels.Clear();	// Clear out the list when switching between expansion packs, so InitFromMap doesn't try to delete the list whose content has already been deleted by the model manager being re-started
}
