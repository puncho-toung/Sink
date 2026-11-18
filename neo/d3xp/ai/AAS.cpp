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

#include "precompiled.h"
#pragma hdrstop


#include "AAS_local.h"
#include "AAS_router.h"	//puncho - R2 router

static idCVar aas_router( "aas_router", "0", CVAR_GAME | CVAR_BOOL, "1 = answer RouteToGoalArea from a map's baked router files when it has them; read when a map loads" );	//puncho - R2 router
#include "../Game_local.h"		//puncho - ai-harness: gameLocal.aiStats

/*
============
idAAS::Alloc
============
*/
idAAS* idAAS::Alloc()
{
	return new( TAG_AAS ) idAASLocal;
}

/*
============
idAAS::idAAS
============
*/
idAAS::~idAAS()
{
}

/*
============
idAASLocal::idAASLocal
============
*/
idAASLocal::idAASLocal()
{
	file = NULL;
	router = NULL;	//puncho - R2 router
	totalCacheMemory = 0;
	cacheEvictions = 0;
	travelSaturated = 0;
	//puncho start - ai-avoidance (WP-A2)
	areaWalls = NULL;
	wallEdgeStamp = NULL;
	wallQueryStamp = 0;
	memset( flowFields, 0, sizeof( flowFields ) );	// WP-A6: the cells come with the file (SetupFlowFields)
	flowCells = NULL;
	flowUseCount = 0;
	//puncho end
}

/*
============
idAASLocal::~idAASLocal
============
*/
idAASLocal::~idAASLocal()
{
	Shutdown();
}

/*
============
idAASLocal::Init
============
*/
bool idAASLocal::Init( const idStr& mapName, unsigned int mapFileCRC )
{
	//puncho start - R2 router review: a held file is kept only while the file on disk has the time of the one
	// that was loaded (idAASFile::GetFileTime). The geometry CRC does not see a compile that changed entities,
	// seeds or AAS settings, and the router's lists beside the file are read anew at every map start: they
	// must meet the file they were baked from. The disk is asked for a held file only: an open before a fresh
	// load would take the file's bytes out of the prefetch store (com_prefetch), and the load would then read
	// the disk again.
	bool keep = false;
	if( file && mapName.Icmp( file->GetName() ) == 0 && mapFileCRC == file->GetCRC() )
	{
		ID_TIME_T diskTime = FILE_NOT_FOUND_TIMESTAMP;
		fileSystem->ReadFile( mapName, NULL, &diskTime );	// without a buffer: nothing is kept; the time is what is wanted
		keep = ( diskTime == file->GetFileTime() );
	}
	//puncho end - R2 router review
	if( keep )
	{
		common->Printf( "Keeping %s\n", file->GetName() );
		RemoveAllObstacles();
		FlushFlowFields();	//puncho - ai-avoidance (WP-A6, audit M5): the previous game's slots and cells go (its first asks filled them); the routing cache stays, as stock keeps it
		SetupRouter();	//puncho - R2 router: loaded again on a kept file; it follows the switch and starts from the file's present doors
	}
	else
	{
		Shutdown();

		file = AASFileManager->LoadAAS( mapName, mapFileCRC );
		if( !file )
		{
			common->DWarning( "Couldn't load AAS file: '%s'", mapName.c_str() );
			return false;
		}
		SetupRouting();
		SetupWallCache();	//puncho - ai-avoidance (WP-A2)
		SetupFlowFields();	//puncho - ai-avoidance (WP-A6, audit M4)
		SetupRouter();	//puncho - R2 router
	}
	return true;
}

//puncho start - R2 router
/*
============
idAASLocal::SetupRouter

  the map's baked router files, when it has them and aas_router is set
============
*/
void idAASLocal::SetupRouter()
{
	ShutdownRouter();
	if( !aas_router.GetBool() )
	{
		return;
	}
	router = new( TAG_AAS ) idAASRouter;
	if( router->Init( file, file->GetName() ) == 0 )
	{
		ShutdownRouter();	// the map has none
	}
}

/*
============
idAASLocal::ShutdownRouter
============
*/
void idAASLocal::ShutdownRouter()
{
	delete router;
	router = NULL;
}
//puncho end - R2 router

/*
============
idAASLocal::Shutdown
============
*/
void idAASLocal::Shutdown()
{
	idScopedLoadSpan freeSpan( SPAN_FREE_AAS );
	if( file )
	{
		ShutdownRouting();
		ShutdownWallCache();	//puncho - ai-avoidance (WP-A2)
		ShutdownFlowFields();	//puncho - ai-avoidance (WP-A6)
		RemoveAllObstacles();
		ShutdownRouter();	//puncho - R2 router: it points into the file
		AASFileManager->FreeAAS( file );
		file = NULL;
	}
}

/*
============
idAASLocal::Stats
============
*/
void idAASLocal::Stats() const
{
	if( !file )
	{
		return;
	}
	common->Printf( "[%s]\n", file->GetName() );
	file->PrintInfo();
	RoutingStats();
	//puncho start - R2 router: what the baked router has answered
	if( router != NULL )
	{
		router->PrintStats();
	}
	//puncho end - R2 router
}

/*
============
idAASLocal::FileBytes
============
*/
int idAASLocal::FileBytes() const
{
	if( !file )
	{
		return 0;
	}
	return file->MemorySize();
}

/*
============
idAASLocal::RoutingBytes
============
*/
int64 idAASLocal::RoutingBytes() const
{
	if( !file )
	{
		return 0;
	}
	// Match CalculateAreaTravelTimes and SetupRoutingCache's allocations, not the evictable caches.
	int64 bytes = ( int64 )numAreaTravelTimes * sizeof( unsigned short );
	bytes += ( int64 )file->GetNumClusters() * sizeof( idRoutingCache** );
	bytes += ( int64 )areaCacheIndexSize * sizeof( idRoutingCache* );
	bytes += ( int64 )portalCacheIndexSize * sizeof( idRoutingCache* );
	bytes += ( int64 )file->GetNumAreas() * sizeof( idRoutingUpdate );
	bytes += ( ( int64 )file->GetNumPortals() + 1 ) * sizeof( idRoutingUpdate );
	bytes += ( int64 )file->GetNumAreas() * sizeof( unsigned short );
	return bytes;
}

/*
============
idAASLocal::GetSettings
============
*/
const idAASSettings* idAASLocal::GetSettings() const
{
	if( !file )
	{
		return NULL;
	}
	return &file->GetSettings();
}

/*
============
idAASLocal::PointAreaNum
============
*/
int idAASLocal::PointAreaNum( const idVec3& origin ) const
{
	if( !file )
	{
		return 0;
	}
	return file->PointAreaNum( origin );
}

/*
============
idAASLocal::PointReachableAreaNum
============
*/
int idAASLocal::PointReachableAreaNum( const idVec3& origin, const idBounds& searchBounds, const int areaFlags ) const
{
	gameLocal.aiStats.areaLookups++;	//puncho - ai-harness: counts idAI's wrapper and AI_pathing's direct call alike (00-PLAN.md §2.1)

	if( !file )
	{
		return 0;
	}

	return file->PointReachableAreaNum( origin, searchBounds, areaFlags, TFL_INVALID );
}

/*
============
idAASLocal::BoundsReachableAreaNum
============
*/
int idAASLocal::BoundsReachableAreaNum( const idBounds& bounds, const int areaFlags ) const
{
	if( !file )
	{
		return 0;
	}

	return file->BoundsReachableAreaNum( bounds, areaFlags, TFL_INVALID );
}

/*
============
idAASLocal::PushPointIntoAreaNum
============
*/
void idAASLocal::PushPointIntoAreaNum( int areaNum, idVec3& origin ) const
{
	if( !file )
	{
		return;
	}
	file->PushPointIntoAreaNum( areaNum, origin );
}

/*
============
idAASLocal::AreaCenter
============
*/
idVec3 idAASLocal::AreaCenter( int areaNum ) const
{
	if( !file )
	{
		return vec3_origin;
	}
	return file->GetArea( areaNum ).center;
}

/*
============
idAASLocal::AreaFlags
============
*/
int idAASLocal::AreaFlags( int areaNum ) const
{
	if( !file )
	{
		return 0;
	}
	return file->GetArea( areaNum ).flags;
}

/*
============
idAASLocal::AreaTravelFlags
============
*/
int idAASLocal::AreaTravelFlags( int areaNum ) const
{
	if( !file )
	{
		return 0;
	}
	return file->GetArea( areaNum ).travelFlags;
}

/*
============
idAASLocal::Trace
============
*/
bool idAASLocal::Trace( aasTrace_t& trace, const idVec3& start, const idVec3& end ) const
{
	if( !file )
	{
		trace.fraction = 0.0f;
		trace.lastAreaNum = 0;
		trace.numAreas = 0;
		return true;
	}
	return file->Trace( trace, start, end );
}

/*
============
idAASLocal::GetPlane
============
*/
const idPlane& idAASLocal::GetPlane( int planeNum ) const
{
	if( !file )
	{
		static idPlane dummy;
		return dummy;
	}
	return file->GetPlane( planeNum );
}

/*
============
idAASLocal::GetEdgeVertexNumbers
============
*/
void idAASLocal::GetEdgeVertexNumbers( int edgeNum, int verts[2] ) const
{
	if( !file )
	{
		verts[0] = verts[1] = 0;
		return;
	}
	const int* v = file->GetEdge( abs( edgeNum ) ).vertexNum;
	verts[0] = v[INT32_SIGNBITSET( edgeNum )];
	verts[1] = v[INT32_SIGNBITNOTSET( edgeNum )];
}

/*
============
idAASLocal::GetEdge
============
*/
void idAASLocal::GetEdge( int edgeNum, idVec3& start, idVec3& end ) const
{
	if( !file )
	{
		start.Zero();
		end.Zero();
		return;
	}
	const int* v = file->GetEdge( abs( edgeNum ) ).vertexNum;
	start = file->GetVertex( v[INT32_SIGNBITSET( edgeNum )] );
	end = file->GetVertex( v[INT32_SIGNBITNOTSET( edgeNum )] );
}
