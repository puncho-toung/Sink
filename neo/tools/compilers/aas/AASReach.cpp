/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.

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

#include "../../../aas/AASFile.h"
#include "../../../aas/AASFile_local.h"
#include "AASReach.h"
//puncho start - R5 speed
#include "AASParallel.h"
#include "AASReach_fast.h"
#include <algorithm>	//puncho - Phase 2C P2.6: std::sort of the point reaches' lists

idCVar aas_threads( "aas_threads", "0", CVAR_TOOL | CVAR_INTEGER, "threads of the AAS compiler's reachability pair loop: 0 = one a logical processor, 1 = no thread is started; changes no output", 0, 64 );
idCVar aas_reachFast( "aas_reachFast", "1", CVAR_TOOL | CVAR_INTEGER, "reachability pair loop: 0 = every area against every area on one thread; 1 = the bounds test first, on a packed copy of the bounds, on aas_threads threads; changes no output", 0, 1 );
idCVar aas_reachCheck( "aas_reachCheck", "1", CVAR_TOOL | CVAR_BOOL, "reachability pair loop with aas_reachFast: 1 = beside the loop the old bounds test is run for every pair of areas, and the compile ends with an error when an area that passes it is not among an area's candidates; 0 = no such check; changes no output" );
//puncho - Phase 2C P2.6
idCVar aas_stepNear( "aas_stepNear", "1", CVAR_TOOL | CVAR_BOOL, "1 = a walk reachability between two floor areas whose floor corners lie within 0.5 of each other seen from above but are not one vertex ( the corner of a step, or corners not welded ), where the floor faces round that point close the whole angle and lie within a step height of each other; 0 = none ( the files of aas_pointReach alone )" );
idCVar aas_pointReach( "aas_pointReach", "1", CVAR_TOOL | CVAR_BOOL, "1 = a walk reachability between two floor areas that meet at one floor vertex and share no floor edge, where the floor faces at that vertex close the whole angle round it ( no wall, ledge or cluster portal at it ); 0 = none ( the files of builds before it )" );
//puncho - Phase 2C P2.6 fix round A1
idCVar aas_reachEndClamp( "aas_reachEndClamp", "1", CVAR_TOOL | CVAR_BOOL, "1 = a point or near-point reachability ends at most half way to the far side of the to-area's floor face along its bisector ( a sliver face narrower than 1 unit keeps the end on it ); 0 = always INSIDEUNITS_WALKEND in ( P2.6's files )" );
#ifndef ID_RETAIL
idCVar aas_fault( "aas_fault", "0", CVAR_TOOL | CVAR_INTEGER, "tests only, not in a retail build: plants a fault that a check of the AAS compiler must refuse. 1 = with aas_reachCheck 1 one candidate of the pair loop is taken away; 2 = one area of every cluster is left out of the flood's list; 3 = every entry of the area to portal table is one too high; 4 = an error is raised inside the pair loop on a thread that is not the calling thread; 5 = on the calling thread while the other threads run; 6 = as 4, a fatal error; 7 = with aas_reachCheck 1 the check's own copy of one area is changed", 0, 7 );
#endif

// Above 0 on a thread while it works in a loop of AAS_ParallelFor: the loop's workers and the thread that started
// it ( AASParallel.h ). On such a thread common->Error and common->FatalError only throw their text
// ( tools/compilers/main.cpp and main_posix.cpp for rbdmap, framework/Common_printf.cpp for the game ); the loop
// joins its threads and then raises the error itself.
thread_local int aasParallelThreadDepth = 0;

bool AAS_ParallelThreadInLoop()
{
	return aasParallelThreadDepth > 0;
}
//puncho end - R5 speed

#define INSIDEUNITS							2.0f
#define INSIDEUNITS_WALKEND					0.5f
#define INSIDEUNITS_WALKSTART				0.1f
#define INSIDEUNITS_SWIMEND					0.5f
#define INSIDEUNITS_FLYEND					0.5f
#define INSIDEUNITS_WATERJUMP				15.0f


/*
================
idAASReach::ReachabilityExists
================
*/
bool idAASReach::ReachabilityExists( int fromAreaNum, int toAreaNum )
{
	aasArea_t* area;
	idReachability* reach;

	area = &file->areas[fromAreaNum];
	for( reach = area->reach; reach; reach = reach->next )
	{
		if( reach->toAreaNum == toAreaNum )
		{
			return true;
		}
	}
	return false;
}

/*
================
idAASReach::CanSwimInArea
================
*/
ID_INLINE bool idAASReach::CanSwimInArea( int areaNum )
{
	return ( file->areas[areaNum].contents & AREACONTENTS_WATER ) != 0;
}

/*
================
idAASReach::AreaHasFloor
================
*/
ID_INLINE bool idAASReach::AreaHasFloor( int areaNum )
{
	return ( file->areas[areaNum].flags & AREA_FLOOR ) != 0;
}

/*
================
idAASReach::AreaIsClusterPortal
================
*/
ID_INLINE bool idAASReach::AreaIsClusterPortal( int areaNum )
{
	return ( file->areas[areaNum].contents & AREACONTENTS_CLUSTERPORTAL ) != 0;
}

/*
================
idAASReach::AddReachabilityToArea
================
*/
void idAASReach::AddReachabilityToArea( idReachability* reach, int areaNum )
{
	aasArea_t* area;

	area = &file->areas[areaNum];
	reach->next = area->reach;
	area->reach = reach;
	//puncho - R5 speed: numReachabilities is no longer counted here ( several threads add ); Build counts the lists at its end
}

/*
================
idAASReach::Reachability_Fly
================
*/
void idAASReach::Reachability_Fly( int areaNum )
{
	int i, faceNum, otherAreaNum;
	aasArea_t* area;
	aasFace_t* face;
	idReachability_Fly* reach;

	area = &file->areas[areaNum];

	for( i = 0; i < area->numFaces; i++ )
	{
		faceNum = file->faceIndex[area->firstFace + i];
		face = &file->faces[abs( faceNum )];

		otherAreaNum = face->areas[INT32_SIGNBITNOTSET( faceNum )];

		if( otherAreaNum == 0 )
		{
			continue;
		}

		if( ReachabilityExists( areaNum, otherAreaNum ) )
		{
			continue;
		}

		// create reachability going through this face
		reach = new idReachability_Fly();
		reach->travelType = TFL_FLY;
		reach->toAreaNum = otherAreaNum;
		reach->fromAreaNum = areaNum;
		reach->edgeNum = 0;
		reach->travelTime = 1;
		reach->start = file->FaceCenter( abs( faceNum ) );
		if( faceNum < 0 )
		{
			reach->end = reach->start + file->planeList[face->planeNum].Normal() * INSIDEUNITS_FLYEND;
		}
		else
		{
			reach->end = reach->start - file->planeList[face->planeNum].Normal() * INSIDEUNITS_FLYEND;
		}
		AddReachabilityToArea( reach, areaNum );
	}
}

/*
================
idAASReach::Reachability_Swim
================
*/
void idAASReach::Reachability_Swim( int areaNum )
{
	int i, faceNum, otherAreaNum;
	aasArea_t* area;
	aasFace_t* face;
	idReachability_Swim* reach;

	if( !CanSwimInArea( areaNum ) )
	{
		return;
	}

	area = &file->areas[areaNum];

	for( i = 0; i < area->numFaces; i++ )
	{
		faceNum = file->faceIndex[area->firstFace + i];
		face = &file->faces[abs( faceNum )];

		otherAreaNum = face->areas[INT32_SIGNBITNOTSET( faceNum )];

		if( otherAreaNum == 0 )
		{
			continue;
		}

		if( !CanSwimInArea( otherAreaNum ) )
		{
			continue;
		}

		if( ReachabilityExists( areaNum, otherAreaNum ) )
		{
			continue;
		}

		// create reachability going through this face
		reach = new idReachability_Swim();
		reach->travelType = TFL_SWIM;
		reach->toAreaNum = otherAreaNum;
		reach->fromAreaNum = areaNum;
		reach->edgeNum = 0;
		reach->travelTime = 1;
		reach->start = file->FaceCenter( abs( faceNum ) );
		if( faceNum < 0 )
		{
			reach->end = reach->start + file->planeList[face->planeNum].Normal() * INSIDEUNITS_SWIMEND;
		}
		else
		{
			reach->end = reach->start - file->planeList[face->planeNum].Normal() * INSIDEUNITS_SWIMEND;
		}
		AddReachabilityToArea( reach, areaNum );
	}
}

/*
================
idAASReach::Reachability_EqualFloorHeight
================
*/
void idAASReach::Reachability_EqualFloorHeight( int areaNum )
{
	int i, k, l, m, n, faceNum, face1Num, face2Num, otherAreaNum, edge1Num = 0, edge2Num;
	aasArea_t* area, *otherArea;
	aasFace_t* face, *face1, *face2;
	idReachability_Walk* reach;

	if( !AreaHasFloor( areaNum ) )
	{
		return;
	}

	area = &file->areas[areaNum];

	for( i = 0; i < area->numFaces; i++ )
	{
		faceNum = file->faceIndex[area->firstFace + i];
		face = &file->faces[abs( faceNum )];

		otherAreaNum = face->areas[INT32_SIGNBITNOTSET( faceNum )];
		if( !AreaHasFloor( otherAreaNum ) )
		{
			continue;
		}

		otherArea = &file->areas[otherAreaNum];

		for( k = 0; k < area->numFaces; k++ )
		{
			face1Num = file->faceIndex[area->firstFace + k];
			face1 = &file->faces[abs( face1Num )];

			if( !( face1->flags & FACE_FLOOR ) )
			{
				continue;
			}
			for( l = 0; l < otherArea->numFaces; l++ )
			{
				face2Num = file->faceIndex[otherArea->firstFace + l];
				face2 = &file->faces[abs( face2Num )];

				if( !( face2->flags & FACE_FLOOR ) )
				{
					continue;
				}

				for( m = 0; m < face1->numEdges; m++ )
				{
					edge1Num = abs( file->edgeIndex[face1->firstEdge + m] );
					for( n = 0; n < face2->numEdges; n++ )
					{
						edge2Num = abs( file->edgeIndex[face2->firstEdge + n] );
						if( edge1Num == edge2Num )
						{
							break;
						}
					}
					if( n < face2->numEdges )
					{
						break;
					}
				}
				if( m < face1->numEdges )
				{
					break;
				}
			}
			if( l < otherArea->numFaces )
			{
				break;
			}
		}
		if( k < area->numFaces )
		{
			// create reachability
			reach = new idReachability_Walk();
			reach->travelType = TFL_WALK;
			reach->toAreaNum = otherAreaNum;
			reach->fromAreaNum = areaNum;
			reach->edgeNum = abs( edge1Num );
			reach->travelTime = 1;
			reach->start = file->EdgeCenter( edge1Num );
			if( faceNum < 0 )
			{
				reach->end = reach->start + file->planeList[face->planeNum].Normal() * INSIDEUNITS_WALKEND;
			}
			else
			{
				reach->end = reach->start - file->planeList[face->planeNum].Normal() * INSIDEUNITS_WALKEND;
			}
			AddReachabilityToArea( reach, areaNum );
		}
	}
}

/*
================
idAASReach::Reachability_Step_Barrier_WaterJump_WalkOffLedge
================
*/
bool idAASReach::Reachability_Step_Barrier_WaterJump_WalkOffLedge( int area1num, int area2num )
{
	int i, j, k, l, edge1Num, edge2Num, areas[10];
	int floor_bestArea1FloorEdgeNum = 0, floor_bestArea2FloorEdgeNum, floor_foundReach;
	int water_bestArea1FloorEdgeNum, water_bestArea2FloorEdgeNum, water_foundReach;
	int side1, faceSide1, floorFace1Num;
	float dist, dist1, dist2, diff, invGravityDot, orthogonalDot;
	float x1, x2, x3, x4, y1, y2, y3, y4, tmp, y;
	float length, floor_bestLength, water_bestLength, floor_bestDist, water_bestDist;
	idVec3 v1, v2, v3, v4, tmpv, p1area1, p1area2, p2area1, p2area2;
	idVec3 normal, orthogonal, edgeVec, start, end;
	idVec3 floor_bestStart, floor_bestEnd, floor_bestNormal;
	idVec3 water_bestStart, water_bestEnd, water_bestNormal;
	idVec3 testPoint;
	idPlane* plane;
	aasArea_t* area1, *area2;
	aasFace_t* floorFace1, *floorFace2, *floor_bestFace1, *water_bestFace1;
	aasEdge_t* edge1, *edge2;
	idReachability_Walk* walkReach;
	idReachability_BarrierJump* barrierJumpReach;
	idReachability_WaterJump* waterJumpReach;
	idReachability_WalkOffLedge* walkOffLedgeReach;
	aasTrace_t trace;

	// must be able to walk or swim in the first area
	if( !AreaHasFloor( area1num ) && !CanSwimInArea( area1num ) )
	{
		return false;
	}

	if( !AreaHasFloor( area2num ) && !CanSwimInArea( area2num ) )
	{
		return false;
	}

	area1 = &file->areas[area1num];
	area2 = &file->areas[area2num];

	// if the areas are not near anough in the x-y direction
	for( i = 0; i < 2; i++ )
	{
		if( area1->bounds[0][i] > area2->bounds[1][i] + 2.0f )
		{
			return false;
		}
		if( area1->bounds[1][i] < area2->bounds[0][i] - 2.0f )
		{
			return false;
		}
	}

	floor_foundReach = false;
	floor_bestDist = 99999;
	floor_bestLength = 0;
	floor_bestArea2FloorEdgeNum = 0;

	water_foundReach = false;
	water_bestDist = 99999;
	water_bestLength = 0;
	water_bestArea2FloorEdgeNum = 0;

	for( i = 0; i < area1->numFaces; i++ )
	{
		floorFace1Num = file->faceIndex[area1->firstFace + i];
		faceSide1 = floorFace1Num < 0;
		floorFace1 = &file->faces[abs( floorFace1Num )];

		// if this isn't a floor face
		if( !( floorFace1->flags & FACE_FLOOR ) )
		{

			// if we can swim in the first area
			if( CanSwimInArea( area1num ) )
			{

				// face plane must be more or less horizontal
				plane = &file->planeList[ floorFace1->planeNum ^ ( !faceSide1 ) ];
				if( plane->Normal() * file->settings.invGravityDir < file->settings.minFloorCos )
				{
					continue;
				}
			}
			else
			{
				// if we can't swim in the area it must be a ground face
				continue;
			}
		}

		for( k = 0; k < floorFace1->numEdges; k++ )
		{
			edge1Num = file->edgeIndex[floorFace1->firstEdge + k];
			side1 = ( edge1Num < 0 );
			// NOTE: for water faces we must take the side area 1 is on into
			// account because the face is shared and doesn't have to be oriented correctly
			if( !( floorFace1->flags & FACE_FLOOR ) )
			{
				side1 = ( side1 == faceSide1 );
			}
			edge1Num = abs( edge1Num );
			edge1 = &file->edges[edge1Num];
			// vertices of the edge
			v1 = file->vertices[edge1->vertexNum[!side1]];
			v2 = file->vertices[edge1->vertexNum[side1]];
			// get a vertical plane through the edge
			// NOTE: normal is pointing into area 2 because the face edges are stored counter clockwise
			edgeVec = v2 - v1;
			normal = edgeVec.Cross( file->settings.invGravityDir );
			normal.Normalize();
			dist = normal * v1;

			// check the faces from the second area
			for( j = 0; j < area2->numFaces; j++ )
			{
				floorFace2 = &file->faces[abs( file->faceIndex[area2->firstFace + j] )];
				// must be a ground face
				if( !( floorFace2->flags & FACE_FLOOR ) )
				{
					continue;
				}
				// check the edges of this ground face
				for( l = 0; l < floorFace2->numEdges; l++ )
				{
					edge2Num = abs( file->edgeIndex[floorFace2->firstEdge + l] );
					edge2 = &file->edges[edge2Num];
					// vertices of the edge
					v3 = file->vertices[edge2->vertexNum[0]];
					v4 = file->vertices[edge2->vertexNum[1]];
					// check the distance between the two points and the vertical plane through the edge of area1
					diff = normal * v3 - dist;
					if( diff < -0.2f || diff > 0.2f )
					{
						continue;
					}
					diff = normal * v4 - dist;
					if( diff < -0.2f || diff > 0.2f )
					{
						continue;
					}

					// project the two ground edges into the step side plane
					// and calculate the shortest distance between the two
					// edges if they overlap in the direction orthogonal to
					// the gravity direction
					orthogonal = file->settings.invGravityDir.Cross( normal );
					invGravityDot = file->settings.invGravityDir * file->settings.invGravityDir;
					orthogonalDot = orthogonal * orthogonal;
					// projection into the step plane
					// NOTE: since gravity is vertical this is just the z coordinate
					y1 = v1[2];//(v1 * file->settings.invGravity) / invGravityDot;
					y2 = v2[2];//(v2 * file->settings.invGravity) / invGravityDot;
					y3 = v3[2];//(v3 * file->settings.invGravity) / invGravityDot;
					y4 = v4[2];//(v4 * file->settings.invGravity) / invGravityDot;

					x1 = ( v1 * orthogonal ) / orthogonalDot;
					x2 = ( v2 * orthogonal ) / orthogonalDot;
					x3 = ( v3 * orthogonal ) / orthogonalDot;
					x4 = ( v4 * orthogonal ) / orthogonalDot;

					if( x1 > x2 )
					{
						tmp = x1;
						x1 = x2;
						x2 = tmp;
						tmp = y1;
						y1 = y2;
						y2 = tmp;
						tmpv = v1;
						v1 = v2;
						v2 = tmpv;
					}
					if( x3 > x4 )
					{
						tmp = x3;
						x3 = x4;
						x4 = tmp;
						tmp = y3;
						y3 = y4;
						y4 = tmp;
						tmpv = v3;
						v3 = v4;
						v4 = tmpv;
					}
					// if the two projected edge lines have no overlap
					if( x2 <= x3 || x4 <= x1 )
					{
						continue;
					}
					// if the two lines fully overlap
					if( ( x1 - 0.5f < x3 && x4 < x2 + 0.5f ) && ( x3 - 0.5f < x1 && x2 < x4 + 0.5f ) )
					{
						dist1 = y3 - y1;
						dist2 = y4 - y2;
						p1area1 = v1;
						p2area1 = v2;
						p1area2 = v3;
						p2area2 = v4;
					}
					else
					{
						// if the points are equal
						if( x1 > x3 - 0.1f && x1 < x3 + 0.1f )
						{
							dist1 = y3 - y1;
							p1area1 = v1;
							p1area2 = v3;
						}
						else if( x1 < x3 )
						{
							y = y1 + ( x3 - x1 ) * ( y2 - y1 ) / ( x2 - x1 );
							dist1 = y3 - y;
							p1area1 = v3;
							p1area1[2] = y;
							p1area2 = v3;
						}
						else
						{
							y = y3 + ( x1 - x3 ) * ( y4 - y3 ) / ( x4 - x3 );
							dist1 = y - y1;
							p1area1 = v1;
							p1area2 = v1;
							p1area2[2] = y;
						}
						// if the points are equal
						if( x2 > x4 - 0.1f && x2 < x4 + 0.1f )
						{
							dist2 = y4 - y2;
							p2area1 = v2;
							p2area2 = v4;
						}
						else if( x2 < x4 )
						{
							y = y3 + ( x2 - x3 ) * ( y4 - y3 ) / ( x4 - x3 );
							dist2 = y - y2;
							p2area1 = v2;
							p2area2 = v2;
							p2area2[2] = y;
						}
						else
						{
							y = y1 + ( x4 - x1 ) * ( y2 - y1 ) / ( x2 - x1 );
							dist2 = y4 - y;
							p2area1 = v4;
							p2area1[2] = y;
							p2area2 = v4;
						}
					}

					// if both distances are pretty much equal then we take the middle of the points
					if( dist1 > dist2 - 1.0f && dist1 < dist2 + 1.0f )
					{
						dist = dist1;
						start = ( p1area1 + p2area1 ) * 0.5f;
						end = ( p1area2 + p2area2 ) * 0.5f;
					}
					else if( dist1 < dist2 )
					{
						dist = dist1;
						start = p1area1;
						end = p1area2;
					}
					else
					{
						dist = dist2;
						start = p2area1;
						end = p2area2;
					}

					// get the length of the overlapping part of the edges of the two areas
					length = ( p2area2 - p1area2 ).Length();

					if( floorFace1->flags & FACE_FLOOR )
					{
						// if the vertical distance is smaller
						if( dist < floor_bestDist ||
								// or the vertical distance is pretty much the same
								// but the overlapping part of the edges is longer
								( dist < floor_bestDist + 1.0f && length > floor_bestLength ) )
						{
							floor_bestDist = dist;
							floor_bestLength = length;
							floor_foundReach = true;
							floor_bestArea1FloorEdgeNum = edge1Num;
							floor_bestArea2FloorEdgeNum = edge2Num;
							floor_bestFace1 = floorFace1;
							floor_bestStart = start;
							floor_bestNormal = normal;
							floor_bestEnd = end;
						}
					}
					else
					{
						// if the vertical distance is smaller
						if( dist < water_bestDist ||
								//or the vertical distance is pretty much the same
								//but the overlapping part of the edges is longer
								( dist < water_bestDist + 1.0f && length > water_bestLength ) )
						{
							water_bestDist = dist;
							water_bestLength = length;
							water_foundReach = true;
							water_bestArea1FloorEdgeNum = edge1Num;
							water_bestArea2FloorEdgeNum = edge2Num;
							water_bestFace1 = floorFace1;
							water_bestStart = start;	// best start point in area1
							water_bestNormal = normal;	// normal is pointing into area2
							water_bestEnd = end;		// best point towards area2
						}
					}
				}
			}
		}
	}
	//
	// NOTE: swim reachabilities should already be filtered out
	//
	// Steps
	//
	//         ---------
	//         |          step height -> TFL_WALK
	// --------|
	//
	//         ---------
	// ~~~~~~~~|          step height and low water -> TFL_WALK
	// --------|
	//
	// ~~~~~~~~~~~~~~~~~~
	//         ---------
	//         |          step height and low water up to the step -> TFL_WALK
	// --------|
	//
	// check for a step reachability
	if( floor_foundReach )
	{
		// if area2 is higher but lower than the maximum step height
		// NOTE: floor_bestDist >= 0 also catches equal floor reachabilities
		if( floor_bestDist >= 0 && floor_bestDist < file->settings.maxStepHeight )
		{
			// create walk reachability from area1 to area2
			walkReach = new idReachability_Walk();
			walkReach->travelType = TFL_WALK;
			walkReach->toAreaNum = area2num;
			walkReach->fromAreaNum = area1num;
			walkReach->start = floor_bestStart + INSIDEUNITS_WALKSTART * floor_bestNormal;
			walkReach->end = floor_bestEnd + INSIDEUNITS_WALKEND * floor_bestNormal;
			walkReach->edgeNum = abs( floor_bestArea1FloorEdgeNum );
			walkReach->travelTime = 0;
			if( area2->flags & AREA_CROUCH )
			{
				walkReach->travelTime += file->settings.tt_startCrouching;
			}
			AddReachabilityToArea( walkReach, area1num );
			return true;
		}
	}
	//
	// Water Jumps
	//
	//         ---------
	//         |
	// ~~~~~~~~|
	//         |
	//         |          higher than step height and water up to waterjump height -> TFL_WATERJUMP
	// --------|
	//
	// ~~~~~~~~~~~~~~~~~~
	//         ---------
	//         |
	//         |
	//         |
	//         |          higher than step height and low water up to the step -> TFL_WATERJUMP
	// --------|
	//
	// check for a waterjump reachability
	if( water_foundReach )
	{
		// get a test point a little bit towards area1
		testPoint = water_bestEnd - INSIDEUNITS * water_bestNormal;
		// go down the maximum waterjump height
		testPoint[2] -= file->settings.maxWaterJumpHeight;
		// if there IS water the sv_maxwaterjump height below the bestend point
		if( area1->flags & AREA_LIQUID )
		{
			// don't create rediculous water jump reachabilities from areas very far below the water surface
			if( water_bestDist < file->settings.maxWaterJumpHeight + 24 )
			{
				// water jumping from or towards a crouch only areas is not possible
				if( !( area1->flags & AREA_CROUCH ) && !( area2->flags & AREA_CROUCH ) )
				{
					// create water jump reachability from area1 to area2
					waterJumpReach = new idReachability_WaterJump();
					waterJumpReach->travelType = TFL_WATERJUMP;
					waterJumpReach->toAreaNum = area2num;
					waterJumpReach->fromAreaNum = area1num;
					waterJumpReach->start = water_bestStart;
					waterJumpReach->end = water_bestEnd + INSIDEUNITS_WATERJUMP * water_bestNormal;
					waterJumpReach->edgeNum = abs( floor_bestArea1FloorEdgeNum );
					waterJumpReach->travelTime = file->settings.tt_waterJump;
					AddReachabilityToArea( waterJumpReach, area1num );
					return true;
				}
			}
		}
	}
	//
	// Barrier Jumps
	//
	//         ---------
	//         |
	//         |
	//         |
	//         |         higher than max step height lower than max barrier height -> TFL_BARRIERJUMP
	// --------|
	//
	//         ---------
	//         |
	//         |
	//         |
	// ~~~~~~~~|         higher than max step height lower than max barrier height
	// --------|         and a thin layer of water in the area to jump from -> TFL_BARRIERJUMP
	//
	// check for a barrier jump reachability
	if( floor_foundReach )
	{
		//if area2 is higher but lower than the maximum barrier jump height
		if( floor_bestDist > 0 && floor_bestDist < file->settings.maxBarrierHeight )
		{
			//if no water in area1 or a very thin layer of water on the ground
			if( !water_foundReach || ( floor_bestDist - water_bestDist < 16 ) )
			{
				// cannot perform a barrier jump towards or from a crouch area
				if( !( area1->flags & AREA_CROUCH ) && !( area2->flags & AREA_CROUCH ) )
				{
					// create barrier jump reachability from area1 to area2
					barrierJumpReach = new idReachability_BarrierJump();
					barrierJumpReach->travelType = TFL_BARRIERJUMP;
					barrierJumpReach->toAreaNum = area2num;
					barrierJumpReach->fromAreaNum = area1num;
					barrierJumpReach->start = floor_bestStart + INSIDEUNITS_WALKSTART * floor_bestNormal;
					barrierJumpReach->end = floor_bestEnd + INSIDEUNITS_WALKEND * floor_bestNormal;
					barrierJumpReach->edgeNum = abs( floor_bestArea1FloorEdgeNum );
					barrierJumpReach->travelTime = file->settings.tt_barrierJump;
					AddReachabilityToArea( barrierJumpReach, area1num );
					return true;
				}
			}
		}
	}
	//
	// Walk and Walk Off Ledge
	//
	// --------|
	//         |          can walk or step back -> TFL_WALK
	//         ---------
	//
	// --------|
	//         |
	//         |
	//         |
	//         |          cannot walk/step back -> TFL_WALKOFFLEDGE
	//         ---------
	//
	// --------|
	//         |
	//         |~~~~~~~~
	//         |
	//         |          cannot step back but can waterjump back -> TFL_WALKOFFLEDGE
	//         ---------  FIXME: create TFL_WALK reach??
	//
	// check for a walk or walk off ledge reachability
	if( floor_foundReach )
	{
		if( floor_bestDist < 0 )
		{
			if( floor_bestDist > -file->settings.maxStepHeight )
			{
				// create walk reachability from area1 to area2
				walkReach = new idReachability_Walk();
				walkReach->travelType = TFL_WALK;
				walkReach->toAreaNum = area2num;
				walkReach->fromAreaNum = area1num;
				walkReach->start = floor_bestStart + INSIDEUNITS_WALKSTART * floor_bestNormal;
				walkReach->end = floor_bestEnd + INSIDEUNITS_WALKEND * floor_bestNormal;
				walkReach->edgeNum = abs( floor_bestArea1FloorEdgeNum );
				walkReach->travelTime = 1;
				AddReachabilityToArea( walkReach, area1num );
				return true;
			}
			// if no maximum fall height set or less than the max
			if( !file->settings.maxFallHeight || idMath::Fabs( floor_bestDist ) < file->settings.maxFallHeight )
			{
				// trace a bounding box vertically to check for solids
				floor_bestEnd += INSIDEUNITS * floor_bestNormal;
				start = floor_bestEnd;
				start[2] = floor_bestStart[2];
				end = floor_bestEnd;
				end[2] += 4;
				trace.areas = areas;
				trace.maxAreas = sizeof( areas ) / sizeof( int );
				file->Trace( trace, start, end );
				// if the trace didn't start in solid and nothing was hit
				if( trace.lastAreaNum && trace.fraction >= 1.0f )
				{
					// the trace end point must be in the goal area
					if( trace.lastAreaNum == area2num )
					{
						// don't create reachability if going through a cluster portal
						for( i = 0; i < trace.numAreas; i++ )
						{
							if( AreaIsClusterPortal( trace.areas[i] ) )
							{
								break;
							}
						}
						if( i >= trace.numAreas )
						{
							// create a walk off ledge reachability from area1 to area2
							walkOffLedgeReach = new idReachability_WalkOffLedge();
							walkOffLedgeReach->travelType = TFL_WALKOFFLEDGE;
							walkOffLedgeReach->toAreaNum = area2num;
							walkOffLedgeReach->fromAreaNum = area1num;
							walkOffLedgeReach->start = floor_bestStart;
							walkOffLedgeReach->end = floor_bestEnd;
							walkOffLedgeReach->edgeNum = abs( floor_bestArea1FloorEdgeNum );
							walkOffLedgeReach->travelTime = file->settings.tt_startWalkOffLedge + idMath::Fabs( floor_bestDist ) * 50 / file->settings.gravityValue;
							AddReachabilityToArea( walkOffLedgeReach, area1num );
							return true;
						}
					}
				}
			}
		}
	}
	return false;
}

/*
================
idAASReach::Reachability_WalkOffLedge
================
*/
void idAASReach::Reachability_WalkOffLedge( int areaNum )
{
	int i, j, faceNum, edgeNum, side, reachAreaNum, p, areas[10];
	aasArea_t* area;
	aasFace_t* face;
	aasEdge_t* edge;
	idPlane* plane;
	idVec3 v1, v2, mid, dir, testEnd;
	idReachability_WalkOffLedge* reach;
	aasTrace_t trace;

	if( !AreaHasFloor( areaNum ) || CanSwimInArea( areaNum ) )
	{
		return;
	}

	area = &file->areas[areaNum];

	for( i = 0; i < area->numFaces; i++ )
	{
		faceNum = file->faceIndex[area->firstFace + i];
		face = &file->faces[abs( faceNum )];

		// face must be a floor face
		if( !( face->flags & FACE_FLOOR ) )
		{
			continue;
		}

		for( j = 0; j < face->numEdges; j++ )
		{

			edgeNum = file->edgeIndex[face->firstEdge + j];
			edge = &file->edges[abs( edgeNum )];

			//if ( !(edge->flags & EDGE_LEDGE) ) {
			//	continue;
			//}

			side = edgeNum < 0;

			v1 = file->vertices[edge->vertexNum[side]];
			v2 = file->vertices[edge->vertexNum[!side]];

			plane = &file->planeList[face->planeNum ^ INT32_SIGNBITSET( faceNum ) ];

			// get the direction into the other area
			dir = plane->Normal().Cross( v2 - v1 );
			dir.Normalize();

			mid = ( v1 + v2 ) * 0.5f;
			testEnd = mid + INSIDEUNITS_WALKEND * dir;
			testEnd[2] -= file->settings.maxFallHeight + 1.0f;
			trace.areas = areas;
			trace.maxAreas = sizeof( areas ) / sizeof( int );
			file->Trace( trace, mid, testEnd );

			reachAreaNum = trace.lastAreaNum;
			if( !reachAreaNum || reachAreaNum == areaNum )
			{
				continue;
			}
			if( idMath::Fabs( mid[2] - trace.endpos[2] ) > file->settings.maxFallHeight )
			{
				continue;
			}
			if( !AreaHasFloor( reachAreaNum ) && !CanSwimInArea( reachAreaNum ) )
			{
				continue;
			}
			if( ReachabilityExists( areaNum, reachAreaNum ) )
			{
				continue;
			}
			// if not going through a cluster portal
			for( p = 0; p < trace.numAreas; p++ )
			{
				if( AreaIsClusterPortal( trace.areas[p] ) )
				{
					break;
				}
			}
			if( p < trace.numAreas )
			{
				continue;
			}

			reach = new idReachability_WalkOffLedge();
			reach->travelType = TFL_WALKOFFLEDGE;
			reach->toAreaNum = reachAreaNum;
			reach->fromAreaNum = areaNum;
			reach->start = mid;
			reach->end = trace.endpos;
			reach->edgeNum = abs( edgeNum );
			reach->travelTime = file->settings.tt_startWalkOffLedge + idMath::Fabs( mid[2] - trace.endpos[2] ) * 50 / file->settings.gravityValue;
			AddReachabilityToArea( reach, areaNum );
		}
	}
}

/*
================
idAASReach::FlagReachableAreas
================
*/
void idAASReach::FlagReachableAreas( idAASFileLocal* file )
{
	int i, numReachableAreas;

	numReachableAreas = 0;
	for( i = 1; i < file->areas.Num(); i++ )
	{

		if( ( file->areas[i].flags & ( AREA_FLOOR | AREA_LADDER ) ) ||
				( file->areas[i].contents & AREACONTENTS_WATER ) )
		{
			file->areas[i].flags |= AREA_REACHABLE_WALK;
		}
		if( file->GetSettings().allowFlyReachabilities )
		{
			file->areas[i].flags |= AREA_REACHABLE_FLY;
		}
		numReachableAreas++;
	}

	common->Printf( "%6d reachable areas\n", numReachableAreas );
}

//puncho start - Phase 2C P2.6: a walk reachability across a single floor vertex
static const double POINT_REACH_TWO_PI = 6.283185307179586;
static const double POINT_REACH_MAX_GAP = 0.5 * POINT_REACH_TWO_PI / 360.0;		// the largest gap allowed round an open vertex
static const double POINT_REACH_MAX_OVER = 5.0 * POINT_REACH_TWO_PI / 360.0;	// the most the faces' angles may add up over 2 pi

//puncho start - Phase 2C P2.6 fix round A1
/*
================
AAS_FaceRayExit

  Seen from above, how far a ray from ( x, y ) along ( dx, dy ) ( unit ) runs inside the floor face before it meets an
  edge of the face that does not pass through ( x, y ) itself; INFINITY when it meets none.
================
*/
static double AAS_FaceRayExit( const idAASFileLocal* file, const int faceNum, const double x, const double y, const double dx, const double dy )
{
	const aasFace_t& face = file->GetFace( faceNum );
	double best = idMath::INFINITUM;
	for( int i = 0; i < face.numEdges; i++ )
	{
		const aasEdge_t& edge = file->GetEdge( abs( file->GetEdgeIndex( face.firstEdge + i ) ) );
		const idVec3& a = file->GetVertex( edge.vertexNum[0] );
		const idVec3& b = file->GetVertex( edge.vertexNum[1] );
		const double ex = ( double )b.x - a.x, ey = ( double )b.y - a.y;
		const double wx = ( double )a.x - x, wy = ( double )a.y - y;
		const double den = dx * ey - dy * ex;
		if( fabs( den ) < 1e-12 )
		{
			continue;	// parallel to the ray
		}
		const double t = ( wx * ey - wy * ex ) / den;	// along the ray
		const double s = ( wx * dy - wy * dx ) / den;	// along the edge
		if( t > 1e-6 && s >= -1e-9 && s <= 1.0 + 1e-9 && t < best )
		{
			best = t;
		}
	}
	return best;
}
//puncho end

/*
================
idAASReach::FloorFaceSector

  The angle the floor face faceNum takes round its corner vertexNum, seen from above: start ( 0 to 2 pi ) and span
  ( counter-clockwise ). Of the two arcs between the face's two edges at the corner it is the one holding the
  direction to the middle of the face's corners ( the face is convex; a corner of 180 degrees, the vertex in the
  middle of a straight side, has no shorter way round ). The edges are read without their sign. false when the face
  does not have exactly two edges at the vertex or one of them has no length seen from above.
================
*/
bool idAASReach::FloorFaceSector( int faceNum, int vertexNum, double& start, double& span ) const
{
	const aasFace_t& face = file->faces[faceNum];
	const idVec3& v = file->vertices[vertexNum];
	int i, numAt = 0, other[2] = { 0, 0 };
	double cx = 0.0, cy = 0.0;

	for( i = 0; i < face.numEdges; i++ )
	{
		const aasEdge_t& edge = file->edges[abs( file->edgeIndex[face.firstEdge + i] )];
		cx += ( double )file->vertices[edge.vertexNum[0]].x + ( double )file->vertices[edge.vertexNum[1]].x;
		cy += ( double )file->vertices[edge.vertexNum[0]].y + ( double )file->vertices[edge.vertexNum[1]].y;
		if( edge.vertexNum[0] == vertexNum || edge.vertexNum[1] == vertexNum )
		{
			if( numAt < 2 )
			{
				other[numAt] = ( edge.vertexNum[0] == vertexNum ) ? edge.vertexNum[1] : edge.vertexNum[0];
			}
			numAt++;
		}
	}
	if( numAt != 2 || face.numEdges < 3 )
	{
		return false;
	}
	// every corner is the end of two edges: the mean of all the ends is the mean of the corners
	cx /= 2.0 * face.numEdges;
	cy /= 2.0 * face.numEdges;

	const idVec3& q = file->vertices[other[0]];
	const idVec3& r = file->vertices[other[1]];
	const double ux = ( double )q.x - v.x, uy = ( double )q.y - v.y;
	const double wx = ( double )r.x - v.x, wy = ( double )r.y - v.y;
	if( ux * ux + uy * uy < 1e-12 || wx * wx + wy * wy < 1e-12 )
	{
		return false;
	}
	const double au = atan2( uy, ux ), aw = atan2( wy, wx ), ac = atan2( cy - v.y, cx - v.x );
	double s1 = aw - au, sc = ac - au;
	while( s1 < 0.0 )
	{
		s1 += POINT_REACH_TWO_PI;
	}
	while( sc < 0.0 )
	{
		sc += POINT_REACH_TWO_PI;
	}
	if( sc <= s1 )
	{
		start = au;
		span = s1;
	}
	else
	{
		start = aw;
		span = POINT_REACH_TWO_PI - s1;
	}
	while( start < 0.0 )
	{
		start += POINT_REACH_TWO_PI;
	}
	while( start >= POINT_REACH_TWO_PI )
	{
		start -= POINT_REACH_TWO_PI;
	}
	return true;
}

/*
================
idAASReach::PointReachInit

  Before the pair loop, on the calling thread, read-only for the loop's workers: every floor area's floor vertices
  and floor edges ( ascending ), and for every vertex whether it is OPEN: the floor faces at it ( of every area with
  floor ) close the whole angle round it, seen from above, so the floor goes on all round the vertex and a box can
  cross it from any of these areas to any other. A vertex is not open when the faces leave a gap of more than
  POINT_REACH_MAX_GAP ( a wall, a ledge, a drop, steep ground, a pinch between two solids: the faces of the areas
  beyond do not reach the vertex ), when they overlap by more than POINT_REACH_MAX_OVER in all ( a fold: floor over
  floor at the vertex ), when a face at it has not exactly two edges at it, or when every face at it is axial
  ( level, its edges along x or y: the floors of an all-axial map, whose files stay as they were ). The small
  allowances take the welding of StoreFile: corners up to VERTEX_EPSILON apart are one vertex.
  A cluster portal's floor face at a vertex counts as floor for the angle but cuts the faces round the vertex into
  RUNS ( pointVertexRun: the faces between two portal faces, in the order of their angles; -1 on a portal's face ):
  a point reachability joins two areas of one run only, so it never goes round a portal, and the areas of a run are
  joined already by the edge reachabilities between neighbouring faces: clustering ( which floods along every
  reachability ) puts them in one cluster with or without it. The faces at the vertex are kept for the pair loop
  ( pointVertexFirst, pointVertexFaces, pointFaceArea ) and pointOpen holds 1 = open, 2 = a portal face at it.
================
*/
void idAASReach::PointReachInit()
{
	const int numAreas = file->areas.Num();
	const int numVertices = file->vertices.Num();
	const int numFaces = file->faces.Num();
	int i, j, k, e, numVertexRefs, numEdgeRefs;
	idList<int> scratch;
	idList<int>& vertexFirst = pointVertexFirst;
	idList<int>& vertexFaces = pointVertexFaces;
	idList<byte> faceSeen, facePortal, faceAxial;

	// how many floor corners and floor edges, so every list below is sized once
	numVertexRefs = 0;
	numEdgeRefs = 0;
	for( i = 1; i < numAreas; i++ )
	{
		const aasArea_t& area = file->areas[i];
		if( !( area.flags & AREA_FLOOR ) )
		{
			continue;
		}
		for( j = 0; j < area.numFaces; j++ )
		{
			const aasFace_t& face = file->faces[abs( file->faceIndex[area.firstFace + j] )];
			if( face.flags & FACE_FLOOR )
			{
				numEdgeRefs += face.numEdges;
				numVertexRefs += 2 * face.numEdges;
			}
		}
	}

	// every floor area's floor vertices and floor edges, ascending, each once
	pointFirstVertex.SetNum( numAreas + 1 );
	pointFirstEdge.SetNum( numAreas + 1 );
	pointVertices.Resize( Max( 1, numVertexRefs ) );
	pointVertices.SetNum( 0 );
	pointEdges.Resize( Max( 1, numEdgeRefs ) );
	pointEdges.SetNum( 0 );
	faceSeen.SetNum( numFaces );
	facePortal.SetNum( numFaces );
	faceAxial.SetNum( numFaces );
	pointFaceArea.SetNum( numFaces );
	memset( pointFaceArea.Ptr(), 0, numFaces * sizeof( int ) );
	memset( faceSeen.Ptr(), 0, numFaces * sizeof( byte ) );
	memset( facePortal.Ptr(), 0, numFaces * sizeof( byte ) );
	memset( faceAxial.Ptr(), 0, numFaces * sizeof( byte ) );
	for( i = 0; i < numAreas; i++ )
	{
		const int firstVertex = pointVertices.Num();
		const int firstEdge = pointEdges.Num();
		pointFirstVertex[i] = firstVertex;
		pointFirstEdge[i] = firstEdge;
		const aasArea_t& area = file->areas[i];
		if( i == 0 || !( area.flags & AREA_FLOOR ) )
		{
			continue;
		}
		for( j = 0; j < area.numFaces; j++ )
		{
			const int faceNum = abs( file->faceIndex[area.firstFace + j] );
			const aasFace_t& face = file->faces[faceNum];
			if( !( face.flags & FACE_FLOOR ) )
			{
				continue;
			}
			if( !faceSeen[faceNum] )
			{
				pointFaceArea[faceNum] = i;	// the first area that lists it
			}
			faceSeen[faceNum] = 1;
			if( AreaIsClusterPortal( i ) )
			{
				facePortal[faceNum] = 1;
			}
			// axial: level, and every edge along x or y ( what an all-axial map's floors are )
			const idVec3& normal = file->planeList[face.planeNum].Normal();
			bool axial = ( normal.x == 0.0f && normal.y == 0.0f );
			for( k = 0; k < face.numEdges && axial; k++ )
			{
				const aasEdge_t& edge = file->edges[abs( file->edgeIndex[face.firstEdge + k] )];
				const idVec3& a = file->vertices[edge.vertexNum[0]];
				const idVec3& b = file->vertices[edge.vertexNum[1]];
				axial = ( a.x == b.x || a.y == b.y );
			}
			faceAxial[faceNum] = axial ? 1 : 0;
			for( k = 0; k < face.numEdges; k++ )
			{
				e = abs( file->edgeIndex[face.firstEdge + k] );
				pointEdges.Append( e );
				pointVertices.Append( file->edges[e].vertexNum[0] );
				pointVertices.Append( file->edges[e].vertexNum[1] );
			}
		}
		std::sort( pointVertices.Ptr() + firstVertex, pointVertices.Ptr() + pointVertices.Num() );
		pointVertices.SetNum( ( int )( std::unique( pointVertices.Ptr() + firstVertex, pointVertices.Ptr() + pointVertices.Num() ) - pointVertices.Ptr() ) );
		std::sort( pointEdges.Ptr() + firstEdge, pointEdges.Ptr() + pointEdges.Num() );
		pointEdges.SetNum( ( int )( std::unique( pointEdges.Ptr() + firstEdge, pointEdges.Ptr() + pointEdges.Num() ) - pointEdges.Ptr() ) );
	}
	pointFirstVertex[numAreas] = pointVertices.Num();
	pointFirstEdge[numAreas] = pointEdges.Num();

	// the floor faces at every vertex ( each face once, also when two areas list it )
	vertexFirst.SetNum( numVertices + 1 );
	memset( vertexFirst.Ptr(), 0, ( numVertices + 1 ) * sizeof( int ) );
	for( i = 1; i < numFaces; i++ )
	{
		if( !faceSeen[i] )
		{
			continue;
		}
		const aasFace_t& face = file->faces[i];
		for( k = 0; k < face.numEdges; k++ )
		{
			// the vertex that starts the edge in the face's order: every corner once ( StoreFile chains the edges )
			const int edgeNum = file->edgeIndex[face.firstEdge + k];
			vertexFirst[file->edges[abs( edgeNum )].vertexNum[INT32_SIGNBITSET( edgeNum )] + 1]++;
		}
	}
	for( i = 0; i < numVertices; i++ )
	{
		vertexFirst[i + 1] += vertexFirst[i];
	}
	vertexFaces.SetNum( Max( 1, vertexFirst[numVertices] ) );
	scratch.SetNum( numVertices + 1 );
	memcpy( scratch.Ptr(), vertexFirst.Ptr(), ( numVertices + 1 ) * sizeof( int ) );
	for( i = 1; i < numFaces; i++ )
	{
		if( !faceSeen[i] )
		{
			continue;
		}
		const aasFace_t& face = file->faces[i];
		for( k = 0; k < face.numEdges; k++ )
		{
			const int edgeNum = file->edgeIndex[face.firstEdge + k];
			vertexFaces[scratch[file->edges[abs( edgeNum )].vertexNum[INT32_SIGNBITSET( edgeNum )]]++] = i;
		}
	}

	// which vertices are open, and the runs between portal faces
	pointOpen.SetNum( Max( 1, numVertices ) );
	memset( pointOpen.Ptr(), 0, pointOpen.Num() * sizeof( byte ) );
	pointVertexRun.SetNum( vertexFaces.Num() );
	memset( pointVertexRun.Ptr(), 0, pointVertexRun.Num() * sizeof( int ) );
	idList<double> sectorStart, sectorSpan;
	idList<int> order;
	for( i = 0; i < numVertices; i++ )
	{
		const int first = vertexFirst[i], num = vertexFirst[i + 1] - first;
		if( num < 2 )
		{
			continue;	// one floor face at it: no two areas meet there
		}
		bool open = true, axialOnly = true, portal = false;
		double sum = 0.0;
		sectorStart.SetNum( num );
		sectorSpan.SetNum( num );
		order.SetNum( num );
		for( k = 0; k < num && open; k++ )
		{
			const int faceNum = vertexFaces[first + k];
			if( !FloorFaceSector( faceNum, i, sectorStart[k], sectorSpan[k] ) )
			{
				open = false;
			}
			if( !faceAxial[faceNum] )
			{
				axialOnly = false;
			}
			if( facePortal[faceNum] )
			{
				portal = true;
			}
			sum += sectorSpan[k];
			order[k] = k;
		}
		// a vertex whose floor faces are all axial is left as it was: an all-axial map's file does not change
		if( !open || axialOnly || sum > POINT_REACH_TWO_PI + POINT_REACH_MAX_OVER )
		{
			continue;
		}
		// the largest gap: the sectors by their start, walked once round
		const double* starts = sectorStart.Ptr();
		std::sort( order.Ptr(), order.Ptr() + num, [starts]( const int a, const int b )
		{
			return starts[a] < starts[b] || ( starts[a] == starts[b] && a < b );
		} );
		double covered = sectorStart[order[0]] + sectorSpan[order[0]];
		double gap = 0.0;
		for( k = 1; k < num; k++ )
		{
			const int s = order[k];
			if( sectorStart[s] - covered > gap )
			{
				gap = sectorStart[s] - covered;
			}
			if( sectorStart[s] + sectorSpan[s] > covered )
			{
				covered = sectorStart[s] + sectorSpan[s];
			}
		}
		if( sectorStart[order[0]] + POINT_REACH_TWO_PI - covered > gap )
		{
			gap = sectorStart[order[0]] + POINT_REACH_TWO_PI - covered;
		}
		if( gap > POINT_REACH_MAX_GAP )
		{
			continue;
		}
		pointOpen[i] = 1;
		if( portal )
		{
			// the runs: round from the first portal face in the order of the angles, a new run after each portal face
			pointOpen[i] |= 2;
			int p = 0, run = 0;
			while( !facePortal[vertexFaces[first + order[p]]] )
			{
				p++;
			}
			for( k = 0; k < num; k++ )
			{
				const int s = order[( p + k ) % num];
				if( facePortal[vertexFaces[first + s]] )
				{
					pointVertexRun[first + s] = -1;
					run++;
				}
				else
				{
					pointVertexRun[first + s] = run;
				}
			}
		}
	}

	//puncho - Phase 2C P2.6: the groups of near corners, while the face flags are here
	if( aas_stepNear.GetBool() )
	{
		StepNearInit( facePortal, faceAxial );
	}
}

/*
================
idAASReach::PointReachFree
================
*/
void idAASReach::PointReachFree()
{
	pointFirstVertex.Clear();
	pointVertices.Clear();
	pointFirstEdge.Clear();
	pointEdges.Clear();
	pointOpen.Clear();
	pointVertexFirst.Clear();
	pointVertexFaces.Clear();
	pointVertexRun.Clear();
	pointFaceArea.Clear();
	stepGroup.Clear();
	stepGroupOpen.Clear();
	stepFirstGroup.Clear();
	stepGroups.Clear();
}

/*
================
idAASReach::PointSameRun

  At a vertex with a portal's face: whether a face of area1 and a face of area2 at it lie in one run ( PointReachInit ).
================
*/
bool idAASReach::PointSameRun( int area1num, int area2num, int vertexNum ) const
{
	const int first = pointVertexFirst[vertexNum], last = pointVertexFirst[vertexNum + 1];
	for( int a = first; a < last; a++ )
	{
		if( pointFaceArea[pointVertexFaces[a]] != area1num || pointVertexRun[a] < 0 )
		{
			continue;
		}
		for( int b = first; b < last; b++ )
		{
			if( pointFaceArea[pointVertexFaces[b]] == area2num && pointVertexRun[b] == pointVertexRun[a] )
			{
				return true;
			}
		}
	}
	return false;
}

/*
================
idAASReach::Reachability_FloorPoint

  A walk reachability from area1 to area2 when both have floor, neither is a cluster portal, their floor faces share
  no edge and they share a floor vertex that is open ( PointReachInit ), in one run where a portal's face is at it:
  the lowest such vertex. Every such pair at the vertex gets one, also two areas side by side ( not opposite ): the
  router and the game's paths go from reachability to reachability, and the edge reachabilities between neighbours
  start at the middle of their edge, often far from the vertex ( the corner-route gate's p95 rose from 1.04 to 1.47
  when only opposite areas got one ).
  It starts at the vertex and ends INSIDEUNITS_WALKEND into area2, along the middle of the angle of area2's first
  floor face at the vertex, on that face's plane; travel time 1, as a walk across an edge. Its edge number is 0, no
  edge: the game takes a walk reachability's edge as a floor edge that is no wall ( AAS_pathing.cpp GetWallEdges,
  BuildAreaWalls compare it with the from-area's own floor edges, and an AAS file has no edge 0: StoreFile's first
  edge is a dummy and GetFaceForPortal keeps no edge 0 ), so no wall goes; FlowFieldExit takes edge 0 as the point
  reach->end, so the exit is FLOW_EXIT_STEP past the vertex along this reachability. Optimize keeps 0 as 0 ( an edge
  of area2 would turn into 0 or a real number depending on the order of the areas ).
  Reads only what PointReachInit made and the two areas: safe on the pair loop's workers.
  1 = made, -1 = they meet at floor vertices and none is open ( refused ), 0 = no point contact.
================
*/
int idAASReach::Reachability_FloorPoint( int area1num, int area2num )
{
	if( !AreaHasFloor( area1num ) || !AreaHasFloor( area2num ) )
	{
		return 0;
	}

	// a shared floor edge is the other reachabilities' case
	const int* e1 = pointEdges.Ptr() + pointFirstEdge[area1num];
	const int* e1End = pointEdges.Ptr() + pointFirstEdge[area1num + 1];
	const int* e2 = pointEdges.Ptr() + pointFirstEdge[area2num];
	const int* e2End = pointEdges.Ptr() + pointFirstEdge[area2num + 1];
	while( e1 < e1End && e2 < e2End )
	{
		if( *e1 == *e2 )
		{
			return 0;
		}
		if( *e1 < *e2 )
		{
			e1++;
		}
		else
		{
			e2++;
		}
	}

	// the lowest shared floor vertex that is open
	const int* v1 = pointVertices.Ptr() + pointFirstVertex[area1num];
	const int* v1End = pointVertices.Ptr() + pointFirstVertex[area1num + 1];
	const int* v2 = pointVertices.Ptr() + pointFirstVertex[area2num];
	const int* v2End = pointVertices.Ptr() + pointFirstVertex[area2num + 1];
	int vertexNum = -1;
	bool shared = false;
	const bool portalArea = AreaIsClusterPortal( area1num ) || AreaIsClusterPortal( area2num );
	while( v1 < v1End && v2 < v2End )
	{
		if( *v1 == *v2 )
		{
			shared = true;
			if( !portalArea && ( pointOpen[*v1] & 1 ) && ( !( pointOpen[*v1] & 2 ) || PointSameRun( area1num, area2num, *v1 ) ) )
			{
				vertexNum = *v1;
				break;
			}
			v1++;
			v2++;
		}
		else if( *v1 < *v2 )
		{
			v1++;
		}
		else
		{
			v2++;
		}
	}
	if( vertexNum < 0 )
	{
		return shared ? -1 : 0;
	}

	// area2's first floor face at the vertex ( in its face list ): the reachability ends along the middle of its angle,
	// on its plane
	const aasArea_t* area2 = &file->areas[area2num];
	const idVec3& vertex = file->vertices[vertexNum];
	idVec3 end = vertex;
	int i;
	for( i = 0; i < area2->numFaces; i++ )
	{
		const int faceNum = abs( file->faceIndex[area2->firstFace + i] );
		const aasFace_t& face = file->faces[faceNum];
		double start, span;
		if( !( face.flags & FACE_FLOOR ) || !FloorFaceSector( faceNum, vertexNum, start, span ) )
		{
			continue;
		}
		const double a = start + 0.5 * span;
		//puncho start - Phase 2C P2.6 fix round A1: on a sliver face the end stays on the face
		double inside = INSIDEUNITS_WALKEND;
		if( aas_reachEndClamp.GetBool() )
		{
			inside = Min( inside, 0.5 * AAS_FaceRayExit( file, faceNum, vertex.x, vertex.y, cos( a ), sin( a ) ) );
		}
		end.x = ( float )( vertex.x + inside * cos( a ) );
		end.y = ( float )( vertex.y + inside * sin( a ) );
		//puncho end
		const idPlane& plane = file->planeList[face.planeNum];
		if( idMath::Fabs( plane.Normal().z ) > 0.001f )
		{
			end.z = ( plane.Dist() - plane.Normal().x * end.x - plane.Normal().y * end.y ) / plane.Normal().z;
		}
		break;
	}
	if( i >= area2->numFaces )
	{
		return -1;	// an open vertex of area2 whose face did not give its angle: cannot happen, refused all the same
	}

	idReachability_Walk* walkReach = new idReachability_Walk();
	walkReach->travelType = TFL_WALK;
	walkReach->toAreaNum = area2num;
	walkReach->fromAreaNum = area1num;
	walkReach->start = vertex;
	walkReach->end = end;
	walkReach->edgeNum = 0;
	walkReach->travelTime = 1;
	AddReachabilityToArea( walkReach, area1num );
	return 1;
}
/*
================
idAASReach::GroupFaceSector

  As FloorFaceSector for a group of near corners ( StepNearInit ) taken as one point ( ax, ay ): the angle of the face
  between its two edges that leave the group, each seen from its own corner in the group, the arc holding the
  direction to the face's middle. false when the face has not exactly two edges leaving the group ( all its corners in
  the group: a sliver smaller than the group ).
================
*/
bool idAASReach::GroupFaceSector( int faceNum, int group, double ax, double ay, double& start, double& span ) const
{
	const aasFace_t& face = file->faces[faceNum];
	int i, numOut = 0;
	double dir[2] = { 0.0, 0.0 }, cx = 0.0, cy = 0.0;

	for( i = 0; i < face.numEdges; i++ )
	{
		const aasEdge_t& edge = file->edges[abs( file->edgeIndex[face.firstEdge + i] )];
		const idVec3& a = file->vertices[edge.vertexNum[0]];
		const idVec3& b = file->vertices[edge.vertexNum[1]];
		cx += ( double )a.x + ( double )b.x;
		cy += ( double )a.y + ( double )b.y;
		const bool inA = ( stepGroup[edge.vertexNum[0]] == group );
		const bool inB = ( stepGroup[edge.vertexNum[1]] == group );
		if( inA == inB )
		{
			continue;
		}
		const idVec3& from = inA ? a : b;
		const idVec3& to = inA ? b : a;
		if( numOut < 2 )
		{
			dir[numOut] = atan2( ( double )to.y - from.y, ( double )to.x - from.x );
		}
		numOut++;
	}
	if( numOut != 2 )
	{
		return false;
	}
	cx /= 2.0 * face.numEdges;
	cy /= 2.0 * face.numEdges;
	const double ac = atan2( cy - ay, cx - ax );
	double s1 = dir[1] - dir[0], sc = ac - dir[0];
	while( s1 < 0.0 )
	{
		s1 += POINT_REACH_TWO_PI;
	}
	while( sc < 0.0 )
	{
		sc += POINT_REACH_TWO_PI;
	}
	if( sc <= s1 )
	{
		start = dir[0];
		span = s1;
	}
	else
	{
		start = dir[1];
		span = POINT_REACH_TWO_PI - s1;
	}
	while( start < 0.0 )
	{
		start += POINT_REACH_TWO_PI;
	}
	while( start >= POINT_REACH_TWO_PI )
	{
		start -= POINT_REACH_TWO_PI;
	}
	return true;
}

/*
================
idAASReach::StepNearInit

  From PointReachInit, when aas_stepNear is on: floor corners closer than STEP_NEAR_XY seen from above are joined into
  GROUPS ( stepGroup per vertex, -1 = none; a group has two vertices at least ), and a group is OPEN when:
  - it is at most STEP_NEAR_SIZE across, seen from above, and its corners lie within the file's maxStepHeight of each
    other in height: every floor round the point is a step from every other, so a box can step from any of them onto
    any other there ( the floors are the box's own positions: the AAS world is expanded by the box, so where they are
    the box fits );
  - the floor faces at its corners close the whole angle round it ( GroupFaceSector; the gap and overlap allowances of
    the point reachability ): no wall, ledge, drop or pinch reaches the point, so the step does not end at a wall or
    over a drop;
  - no face at it belongs to a cluster portal, not every face at it is axial ( all-axial files stay as they were ),
    and every face has exactly two edges leaving the group.
  Per area: the groups its floor corners are in ( stepFirstGroup, stepGroups, ascending ).
================
*/
static const double STEP_NEAR_XY = 0.5;		// corners this close seen from above are one point
static const double STEP_NEAR_SIZE = 1.0;	// the widest group used, seen from above
static const int STEP_NEAR_MAX_CORNERS = 16;	// the most corners a group used may have

void idAASReach::StepNearInit( const idList<byte>& facePortal, const idList<byte>& faceAxial )
{
	const int numVertices = file->vertices.Num();
	const int numAreas = file->areas.Num();
	const float maxStep = file->GetSettings().maxStepHeight;
	int i, k, n;

	// the floor corners, by their cell of STEP_NEAR_XY seen from above ( cell x and y offset by 2^22 and packed in one
	// number, so a neighbouring cell is that number +- 1 and +- 2^23 )
	idList<int> parent;
	parent.SetNum( Max( 1, numVertices ) );
	for( i = 0; i < numVertices; i++ )
	{
		parent[i] = i;
	}
	idList<int> sorted;
	sorted.Resize( Max( 1, numVertices ) );
	sorted.SetNum( 0 );
	idList<long long> cellOf;
	cellOf.SetNum( Max( 1, numVertices ) );
	for( i = 0; i < numVertices; i++ )
	{
		if( pointVertexFirst[i + 1] > pointVertexFirst[i] )
		{
			const long long cx = ( long long )floor( file->vertices[i].x / STEP_NEAR_XY ) + ( 1 << 22 );
			const long long cy = ( long long )floor( file->vertices[i].y / STEP_NEAR_XY ) + ( 1 << 22 );
			cellOf[i] = ( cx << 23 ) | cy;
			sorted.Append( i );
		}
	}
	const long long* cells = cellOf.Ptr();
	std::sort( sorted.Ptr(), sorted.Ptr() + sorted.Num(), [cells]( const int a, const int b )
	{
		return cells[a] < cells[b] || ( cells[a] == cells[b] && a < b );
	} );
	auto findRoot = [&parent]( int v )
	{
		while( parent[v] != v )
		{
			parent[v] = parent[parent[v]];
			v = parent[v];
		}
		return v;
	};
	// every pair of corners closer than STEP_NEAR_XY: in the same cell or a neighbouring one
	for( k = 0; k < sorted.Num(); k++ )
	{
		const int v = sorted[k];
		const idVec3& p = file->vertices[v];
		for( int dx = -1; dx <= 1; dx++ )
		{
			for( int dy = -1; dy <= 1; dy++ )
			{
				const long long want = cells[v] + ( ( long long )dx << 23 ) + dy;
				// the first corner of that cell
				int lo = 0, hi = sorted.Num();
				while( lo < hi )
				{
					const int mid = ( lo + hi ) / 2;
					if( cells[sorted[mid]] < want )
					{
						lo = mid + 1;
					}
					else
					{
						hi = mid;
					}
				}
				for( n = lo; n < sorted.Num() && cells[sorted[n]] == want; n++ )
				{
					const int w = sorted[n];
					if( w <= v )
					{
						continue;
					}
					const double ddx = ( double )file->vertices[w].x - p.x, ddy = ( double )file->vertices[w].y - p.y;
					if( ddx * ddx + ddy * ddy <= STEP_NEAR_XY * STEP_NEAR_XY )
					{
						const int rv = findRoot( v ), rw = findRoot( w );
						if( rv != rw )
						{
							parent[Max( rv, rw )] = Min( rv, rw );
						}
					}
				}
			}
		}
	}

	// the groups, numbered in the order of their lowest vertex; their members
	idList<int> size, groupOfRoot;
	size.SetNum( Max( 1, numVertices ) );
	groupOfRoot.SetNum( Max( 1, numVertices ) );
	memset( size.Ptr(), 0, size.Num() * sizeof( int ) );
	for( i = 0; i < numVertices; i++ )
	{
		groupOfRoot[i] = -1;
		if( pointVertexFirst[i + 1] > pointVertexFirst[i] )
		{
			size[findRoot( i )]++;
		}
	}
	int numGroups = 0;
	stepGroup.SetNum( Max( 1, numVertices ) );
	for( i = 0; i < numVertices; i++ )
	{
		stepGroup[i] = -1;
		if( pointVertexFirst[i + 1] == pointVertexFirst[i] )
		{
			continue;
		}
		const int r = findRoot( i );
		if( size[r] < 2 )
		{
			continue;
		}
		if( groupOfRoot[r] < 0 )
		{
			groupOfRoot[r] = numGroups++;
		}
		stepGroup[i] = groupOfRoot[r];
	}
	idList<int> groupFirst, groupMembers;
	groupFirst.SetNum( numGroups + 1 );
	memset( groupFirst.Ptr(), 0, ( numGroups + 1 ) * sizeof( int ) );
	for( i = 0; i < numVertices; i++ )
	{
		if( stepGroup[i] >= 0 )
		{
			groupFirst[stepGroup[i] + 1]++;
		}
	}
	for( k = 0; k < numGroups; k++ )
	{
		groupFirst[k + 1] += groupFirst[k];
	}
	groupMembers.SetNum( Max( 1, groupFirst[numGroups] ) );
	idList<int> fill;
	fill.SetNum( numGroups + 1 );
	memcpy( fill.Ptr(), groupFirst.Ptr(), ( numGroups + 1 ) * sizeof( int ) );
	for( i = 0; i < numVertices; i++ )
	{
		if( stepGroup[i] >= 0 )
		{
			groupMembers[fill[stepGroup[i]]++] = i;	// ascending: the vertices are walked in order
		}
	}

	// which groups are open
	stepGroupOpen.SetNum( Max( 1, numGroups ) );
	memset( stepGroupOpen.Ptr(), 0, stepGroupOpen.Num() * sizeof( byte ) );
	idList<int> faces, order;
	idList<double> sectorStart, sectorSpan;
	for( k = 0; k < numGroups; k++ )
	{
		const int first = groupFirst[k], num = groupFirst[k + 1] - first;
		if( num > STEP_NEAR_MAX_CORNERS )
		{
			continue;
		}
		double ax = 0.0, ay = 0.0;
		float zMin = idMath::INFINITUM, zMax = -idMath::INFINITUM;
		bool open = true;
		for( n = 0; n < num; n++ )
		{
			const idVec3& p = file->vertices[groupMembers[first + n]];
			ax += p.x;
			ay += p.y;
			zMin = Min( zMin, p.z );
			zMax = Max( zMax, p.z );
			for( int m = 0; m < n; m++ )
			{
				const idVec3& q = file->vertices[groupMembers[first + m]];
				const double ddx = ( double )p.x - q.x, ddy = ( double )p.y - q.y;
				if( ddx * ddx + ddy * ddy > STEP_NEAR_SIZE * STEP_NEAR_SIZE )
				{
					open = false;
				}
			}
		}
		if( !open || zMax - zMin > maxStep )
		{
			continue;
		}
		ax /= num;
		ay /= num;
		// the floor faces at its corners, each once
		faces.SetNum( 0 );
		for( n = 0; n < num; n++ )
		{
			const int v = groupMembers[first + n];
			for( int e = pointVertexFirst[v]; e < pointVertexFirst[v + 1]; e++ )
			{
				faces.Append( pointVertexFaces[e] );
			}
		}
		std::sort( faces.Ptr(), faces.Ptr() + faces.Num() );
		faces.SetNum( ( int )( std::unique( faces.Ptr(), faces.Ptr() + faces.Num() ) - faces.Ptr() ) );
		const int numFacesHere = faces.Num();
		sectorStart.SetNum( numFacesHere );
		sectorSpan.SetNum( numFacesHere );
		order.SetNum( numFacesHere );
		bool axialOnly = true;
		double sum = 0.0;
		for( n = 0; n < numFacesHere && open; n++ )
		{
			if( facePortal[faces[n]] || !GroupFaceSector( faces[n], k, ax, ay, sectorStart[n], sectorSpan[n] ) )
			{
				open = false;
				break;
			}
			if( !faceAxial[faces[n]] )
			{
				axialOnly = false;
			}
			sum += sectorSpan[n];
			order[n] = n;
		}
		if( !open || numFacesHere < 2 || axialOnly || sum > POINT_REACH_TWO_PI + POINT_REACH_MAX_OVER )
		{
			continue;
		}
		const double* starts = sectorStart.Ptr();
		std::sort( order.Ptr(), order.Ptr() + numFacesHere, [starts]( const int a, const int b )
		{
			return starts[a] < starts[b] || ( starts[a] == starts[b] && a < b );
		} );
		double covered = sectorStart[order[0]] + sectorSpan[order[0]];
		double gap = 0.0;
		for( n = 1; n < numFacesHere; n++ )
		{
			const int s = order[n];
			gap = Max( gap, sectorStart[s] - covered );
			covered = Max( covered, sectorStart[s] + sectorSpan[s] );
		}
		gap = Max( gap, sectorStart[order[0]] + POINT_REACH_TWO_PI - covered );
		if( gap <= POINT_REACH_MAX_GAP )
		{
			stepGroupOpen[k] = 1;
		}
	}

	// every floor area's groups, ascending
	stepFirstGroup.SetNum( numAreas + 1 );
	stepGroups.SetNum( 0 );
	stepGroups.Resize( pointVertices.Num() );	//puncho - Phase 2C P2.6 (opus/B MINOR 1): sized once ( an area's groups are at most its floor corners ); Append alone grew it 16 at a time, copying it whole each time
	for( i = 0; i < numAreas; i++ )
	{
		const int firstGroup = stepGroups.Num();
		stepFirstGroup[i] = firstGroup;
		for( n = pointFirstVertex[i]; n < pointFirstVertex[i + 1]; n++ )
		{
			if( stepGroup[pointVertices[n]] >= 0 )
			{
				stepGroups.Append( stepGroup[pointVertices[n]] );
			}
		}
		std::sort( stepGroups.Ptr() + firstGroup, stepGroups.Ptr() + stepGroups.Num() );
		stepGroups.SetNum( ( int )( std::unique( stepGroups.Ptr() + firstGroup, stepGroups.Ptr() + stepGroups.Num() ) - stepGroups.Ptr() ) );
	}
	stepFirstGroup[numAreas] = stepGroups.Num();
}

/*
================
idAASReach::Reachability_StepNear

  A walk reachability from area1 to area2 when both have floor, neither is a cluster portal, their floor faces share
  no edge, and both have floor corners in a group of near corners that is open ( StepNearInit ): the lowest such
  group. The pair loop asks only when the point reachability made none; two areas that share a vertex whose own faces
  leave a gap can still meet in an open group ( unwelded corners round it fill the gap ). It starts at area1's first corner in the group ( its
  first floor face with one, in its face list ) and ends INSIDEUNITS_WALKEND into area2 from area2's corner, along the
  middle of the angle of area2's first floor face at the group, on that face's plane. The height between them is a
  step at most ( the group's height limit ); travel time 1, edge number 0, as the point reachability ( and for the same
  reasons: no wall goes, the flow-field exit is past the point ).
  1 = made, -1 = they have corners in a group and none is open ( refused ), 0 = no near contact.
================
*/
int idAASReach::Reachability_StepNear( int area1num, int area2num )
{
	if( !AreaHasFloor( area1num ) || !AreaHasFloor( area2num ) )
	{
		return 0;
	}
	// a shared floor edge is another reachability's case
	const int* a = pointEdges.Ptr() + pointFirstEdge[area1num];
	const int* aEnd = pointEdges.Ptr() + pointFirstEdge[area1num + 1];
	const int* b = pointEdges.Ptr() + pointFirstEdge[area2num];
	const int* bEnd = pointEdges.Ptr() + pointFirstEdge[area2num + 1];
	while( a < aEnd && b < bEnd )
	{
		if( *a == *b )
		{
			return 0;
		}
		( *a < *b ) ? a++ : b++;
	}
	// the lowest common group that is open
	a = stepGroups.Ptr() + stepFirstGroup[area1num];
	aEnd = stepGroups.Ptr() + stepFirstGroup[area1num + 1];
	b = stepGroups.Ptr() + stepFirstGroup[area2num];
	bEnd = stepGroups.Ptr() + stepFirstGroup[area2num + 1];
	int group = -1;
	bool common = false;
	while( a < aEnd && b < bEnd )
	{
		if( *a == *b )
		{
			common = true;
			if( stepGroupOpen[*a] )
			{
				group = *a;
				break;
			}
			a++;
			b++;
		}
		else
		{
			( *a < *b ) ? a++ : b++;
		}
	}
	if( group < 0 )
	{
		return common ? -1 : 0;
	}
	if( AreaIsClusterPortal( area1num ) || AreaIsClusterPortal( area2num ) )
	{
		return -1;
	}

	// the group's point seen from above, for the angles
	double ax = 0.0, ay = 0.0;
	int num = 0;
	idVec3 start = vec3_origin, corner2 = vec3_origin;
	bool haveStart = false, haveEnd = false;
	double endAngle = 0.0;
	int endFace = 0;
	for( int pass = 0; pass < 2; pass++ )
	{
		const aasArea_t& area = file->areas[pass == 0 ? area1num : area2num];
		for( int f = 0; f < area.numFaces; f++ )
		{
			const int faceNum = abs( file->faceIndex[area.firstFace + f] );
			const aasFace_t& face = file->faces[faceNum];
			if( !( face.flags & FACE_FLOOR ) )
			{
				continue;
			}
			for( int e = 0; e < face.numEdges; e++ )
			{
				const int edgeNum = file->edgeIndex[face.firstEdge + e];
				const int v = file->edges[abs( edgeNum )].vertexNum[INT32_SIGNBITSET( edgeNum )];
				if( stepGroup[v] != group )
				{
					continue;
				}
				ax += file->vertices[v].x;
				ay += file->vertices[v].y;
				num++;
				if( pass == 0 && !haveStart )
				{
					start = file->vertices[v];
					haveStart = true;
				}
				if( pass == 1 && !haveEnd )
				{
					corner2 = file->vertices[v];
					endFace = faceNum;
					haveEnd = true;
				}
			}
		}
	}
	if( !haveStart || !haveEnd )
	{
		return -1;	// cannot happen: both areas have corners in the group
	}
	ax /= num;
	ay /= num;
	double sectorStart, sectorSpan;
	if( !GroupFaceSector( endFace, group, ax, ay, sectorStart, sectorSpan ) )
	{
		return -1;	// cannot happen in an open group
	}
	endAngle = sectorStart + 0.5 * sectorSpan;
	idVec3 end = corner2;
	//puncho start - Phase 2C P2.6 fix round A1: on a sliver face the end stays on the face
	double inside = INSIDEUNITS_WALKEND;
	if( aas_reachEndClamp.GetBool() )
	{
		inside = Min( inside, 0.5 * AAS_FaceRayExit( file, endFace, corner2.x, corner2.y, cos( endAngle ), sin( endAngle ) ) );
	}
	end.x = ( float )( corner2.x + inside * cos( endAngle ) );
	end.y = ( float )( corner2.y + inside * sin( endAngle ) );
	//puncho end
	const idPlane& plane = file->planeList[file->faces[endFace].planeNum];
	if( idMath::Fabs( plane.Normal().z ) > 0.001f )
	{
		end.z = ( plane.Dist() - plane.Normal().x * end.x - plane.Normal().y * end.y ) / plane.Normal().z;
	}

	idReachability_Walk* walkReach = new idReachability_Walk();
	walkReach->travelType = TFL_WALK;
	walkReach->toAreaNum = area2num;
	walkReach->fromAreaNum = area1num;
	walkReach->start = start;
	walkReach->end = end;
	walkReach->edgeNum = 0;
	walkReach->travelTime = 1;
	AddReachabilityToArea( walkReach, area1num );
	return 1;
}
//puncho end - Phase 2C P2.6

/*
================
idAASReach::BuildPairsFast

//puncho - R5 speed: the pair loop of Build with the areas spread over threads and, for each area, the loop's
body run only for the area's candidates ( AASReach_fast.h ). Every reachability an area gets is made by the one
worker that has the area, in ascending order of the other area, as the loop over all areas made them; no worker
reads or writes another area's list. So every list is the list the old loop builds, for any thread count.
aas_reachCheck ( on by default ) runs the old bounds test for every pair of areas beside the loop and ends the
compile with an error when an area that passes it is not among an area's candidates.
aas_fault ( tests only, not in a retail build ): 1 takes one candidate away, which aas_reachCheck must miss; 4, 5
and 6 raise an error inside the loop ( 4 on a worker that is not the calling thread, 5 on the calling thread, 6 a
fatal error on a worker ); 7 changes the check's own copy of the areas, which the comparison after the loop must
refuse. 2 and 3 are clustering's ( AASCluster.cpp ).
================
*/
void idAASReach::BuildPairsFast()
{
	const int numAreas = file->areas.Num();
	const aasArea_t* areas = file->areas.Ptr();
	const bool check = aas_reachCheck.GetBool();
#ifndef ID_RETAIL
	const int fault = aas_fault.GetInteger();
#else
	const int fault = 0;	// a retail build plants no fault: it has no cvar aas_fault
#endif
	const int numThreads = AAS_NumThreads( aas_threads.GetInteger() );
	int n, reported = 0;
	long long numCandidates = 0;
	std::atomic<int> badArea( 0 ), badOther( 0 ), faultArea( 0 );
	std::atomic<int> pointRefused( 0 );	//puncho - Phase 2C P2.6
	const bool pointReach = aas_pointReach.GetBool();	//puncho - Phase 2C P2.6
	std::atomic<int> stepRefused( 0 ), stepMade( 0 );	//puncho - Phase 2C P2.6
	const bool stepNear = aas_stepNear.GetBool();	//puncho - Phase 2C P2.6
	aasReachPack_t pack;
	idList<int*> buffers;
	idList<long long> counts;
	aasReachRaw_t raw;

	memset( &raw, 0, sizeof( raw ) );
	// Mem_Alloc16 returns NULL when the system has no memory: every scratch array is checked before the loop starts
	bool haveMemory = AAS_ReachPackBuild( pack, areas, numAreas );
	// frees the scratch memory when this function ends, also when the loop ends with an error ( in the game
	// common->Error throws; the lists made so far go with the file, as after any error )
	struct aasReachScratch_t
	{
		idList<int*>* 		buffers;
		aasReachPack_t* 	pack;
		aasReachRaw_t* 		raw;
		~aasReachScratch_t()
		{
			for( int b = 0; b < buffers->Num(); b++ )
			{
				Mem_Free16( ( *buffers )[b] );
			}
			buffers->Clear();
			AAS_ReachPackFree( *pack );
			AAS_ReachRawFree( *raw );
		}
	} scratch = { &buffers, &pack, &raw };
	if( haveMemory && check )
	{
		// what the check reads of every area ( AASReach_fast.h ); it is compared with the areas again after the loop
		haveMemory = AAS_ReachRawBuild( raw, areas, numAreas );
		if( haveMemory && fault == 7 && numAreas > 1 )
		{
			// the planted fault of that comparison, tests only: in the copy the last area's walk flag is turned over
			raw.walk[numAreas - 1] = raw.walk[numAreas - 1] ? 0 : 1;
			faultArea.store( numAreas - 1 );
		}
	}
	// every worker's candidates, and how many it ran
	for( n = 0; haveMemory && n < numThreads; n++ )
	{
		buffers.Append( ( int* )Mem_Alloc16( ( size_t )numAreas * sizeof( int ), TAG_AAS ) );
		counts.Append( 0 );
		haveMemory = ( buffers[n] != NULL );
	}
	if( !haveMemory )
	{
		// no thread is started yet: a plain error; `scratch` frees what there is
		common->Error( "aas reach: no memory for the pair loop's scratch arrays ( %d areas, %d threads )", numAreas, numThreads );
	}
	int* const* workerBuffers = buffers.Ptr();
	long long* workerCounts = counts.Ptr();

	const aasParallelRun_t ran = AAS_ParallelFor( numAreas, 64, numThreads,
								  [&]( const int first, const int last, const int worker )
	{
		int i, j, k, num, missing;
		int* candidates = workerBuffers[worker];
		long long numRan = 0;	// the candidates this chunk ran
		int numRefused = 0;		//puncho - Phase 2C P2.6: the point contacts this chunk refused
		int numStepRefusedHere = 0, numStepMadeHere = 0;	//puncho - Phase 2C P2.6: its near contacts refused and made

		for( i = ( first > 1 ) ? first : 1; i < last; i++ )
		{
			if( !( areas[i].flags & AREA_REACHABLE_WALK ) )
			{
				continue;
			}

			num = AAS_ReachCandidates( pack, &areas[i], candidates );
			numRan += num;

			if( fault == 1 && check && faultArea.load() == 0 )
			{
				// the planted fault of the check: one area, once, loses a candidate the check asks for
				int none = 0;
				if( faultArea.compare_exchange_strong( none, i ) )
				{
					const int kept = AAS_ReachFaultDrop( areas, i, candidates, num );
					if( kept == num )
					{
						faultArea.store( 0 );	// nothing to take away here: another area takes the fault
					}
					num = kept;
				}
			}
			else if( fault >= 4 && fault <= 6 && faultArea.load() == 0 )
			{
				// the planted errors, tests only: 4 is raised on a worker that is not the calling thread, 5 on the calling
				// thread ( worker 0 ) while the others run, 6 is 4 as a fatal error. The side that must not raise it waits
				// here, at most 20 seconds, until the other side has: so the error comes from the wanted thread also on a
				// busy machine and a small map
				const bool raises = ( numThreads == 1 ) || ( ( fault != 5 ) == ( worker != 0 ) );
				if( !raises )
				{
					const int giveUp = Sys_Milliseconds() + 20000;
					while( faultArea.load() == 0 && Sys_Milliseconds() < giveUp )
					{
						std::this_thread::yield();
					}
				}
				else
				{
					int none = 0;
					if( faultArea.compare_exchange_strong( none, i ) )
					{
						if( fault == 6 )
						{
							common->FatalError( "aas_fault 6 planted 1: a fatal error raised in the pair loop at area %d on worker %d", i, worker );
						}
						common->Error( "aas_fault %d planted 1: an error raised in the pair loop at area %d on worker %d", fault, i, worker );
					}
				}
			}

			if( check )
			{
				missing = AAS_ReachCheckArea( raw, &areas[i], candidates, num );
				if( missing != -1 )
				{
					int none = 0;
					if( badArea.compare_exchange_strong( none, i ) )
					{
						badOther.store( missing );
					}
				}
			}

			for( k = 0; k < num; k++ )
			{
				j = candidates[k];

				// the body of the loop in Build
				if( i == j )
				{
					continue;
				}

				if( !( areas[j].flags & AREA_REACHABLE_WALK ) )
				{
					continue;
				}

				if( ReachabilityExists( i, j ) )
				{
					continue;
				}
				if( Reachability_Step_Barrier_WaterJump_WalkOffLedge( i, j ) )
				{
					continue;
				}
				//puncho start - Phase 2C P2.6: no edge contact made a reachability: a contact at one floor vertex, or at
				// near corners
				const int pointMade = pointReach ? Reachability_FloorPoint( i, j ) : 0;
				if( pointMade < 0 )
				{
					numRefused++;
				}
				if( stepNear && pointMade <= 0 )
				{
					const int stepMade = Reachability_StepNear( i, j );
					if( stepMade < 0 )
					{
						numStepRefusedHere++;
					}
					else if( stepMade > 0 )
					{
						numStepMadeHere++;
					}
				}
				//puncho end
			}
		}
		workerCounts[worker] += numRan;	// once a chunk: the slots of the workers lie in one cache line
		pointRefused += numRefused;	//puncho - Phase 2C P2.6
		stepRefused += numStepRefusedHere;
		stepMade += numStepMadeHere;
	},
	[&]( const int done )
	{
		// on the calling thread: the pacifier gets one step an area from area 1, as the loop gave it. Nothing is printed
		// from here: in the game a print draws a frame, and the workers are running. ( The game's pacifier functions
		// are empty, framework/Common_local.h; rbdmap's show the progress as they did. )
		const int steps = ( done > 1 ) ? done - 1 : 0;
		if( steps > reported )
		{
			common->DmapPacifierCompileProgressIncrement( steps - reported );
			reported = steps;
		}
	} );

	// ( the buffers and the pack are freed by `scratch`, when this function ends in whatever way )
	for( n = 0; n < counts.Num(); n++ )
	{
		numCandidates += counts[n];
	}
	numPointRefused += pointRefused.load();	//puncho - Phase 2C P2.6
	numStepRefused += stepRefused.load();
	numStepMade += stepMade.load();

	// which path ran: the tests ask for this line. In the game it is also the one line this loop prints ( no percent
	// while the workers run )
	common->Printf( "\raas reach: fast %d, threads %d of %d, check %d, fault %d planted %d, %lld candidates\n", aas_reachFast.GetInteger(), ran.threads, numThreads, check ? 1 : 0, fault, ( faultArea.load() != 0 ) ? 1 : 0, numCandidates );

	if( ran.controlWords != 0 )
	{
		common->Error( "aas reach: %d threads ran with another floating point control word than the calling thread", ran.controlWords );
	}
	if( badArea.load() != 0 )
	{
		// the area named is the first one a worker found, not the lowest: it can differ from run to run
		if( badOther.load() == -2 )
		{
			common->Error( "aas_reachCheck: area %d: its candidates are not ascending, or hold an area twice, or hold a number that is no area", badArea.load() );
		}
		common->Error( "aas_reachCheck: area %d: area %d has AREA_REACHABLE_WALK and passes the bounds test and is not among the candidates", badArea.load(), badOther.load() );
	}
	if( check )
	{
		// the check read the other area's bounds and flag from its own copy: the copy must still hold, bit for bit,
		// what the areas hold
		const int changed = AAS_ReachRawSame( raw, areas, numAreas );
		if( changed != -1 )
		{
			common->Error( "aas_reachCheck: the check's copy of the bounds and the walk flag differs from the areas at area %d", changed );
		}
	}
}

/*
================
idAASReach::Build
================
*/
bool idAASReach::Build( const idMapFile* mapFile, idAASFileLocal* file )
{
	int i, j, lastPercent;

	this->mapFile = mapFile;
	this->file = file;
	numReachabilities = 0;

	common->Printf( "[Reachability]\n" );
	common->DmapPacifierInfo( "[Reachability]\n" );

	// delete all existing reachabilities
	file->DeleteReachabilities();

	FlagReachableAreas( file );

	//puncho start - Phase 2C P2.6: the floor vertices and edges of every area, and the open vertices, for the pair loop
	numPointRefused = 0;
	numStepRefused = 0;
	numStepMade = 0;
	if( aas_pointReach.GetBool() || aas_stepNear.GetBool() )
	{
		PointReachInit();
	}
	//puncho end

	for( i = 1; i < file->areas.Num(); i++ )
	{
		if( !( file->areas[i].flags & AREA_REACHABLE_WALK ) )
		{
			continue;
		}
		if( file->GetSettings().allowSwimReachabilities )
		{
			Reachability_Swim( i );
		}
		Reachability_EqualFloorHeight( i );
	}

	common->DmapPacifierCompileProgressTotal( file->areas.Num() - 1 );

	lastPercent = -1;
	//puncho start - R5 speed: aas_reachFast 0 keeps the loop below as it was, every area against every area on one thread
	if( aas_reachFast.GetInteger() != 0 )
	{
		BuildPairsFast();
	}
	else
	{
		common->Printf( "aas reach: fast 0, the loop over every pair of areas on one thread\n" );
	}
	if( aas_reachFast.GetInteger() == 0 )
	{
		//puncho end - R5 speed
	for( i = 1; i < file->areas.Num(); i++, common->DmapPacifierCompileProgressIncrement( 1 ) )
	{

		if( !( file->areas[i].flags & AREA_REACHABLE_WALK ) )
		{
			continue;
		}

		for( j = 0; j < file->areas.Num(); j++ )
		{
			if( i == j )
			{
				continue;
			}

			if( !( file->areas[j].flags & AREA_REACHABLE_WALK ) )
			{
				continue;
			}

			if( ReachabilityExists( i, j ) )
			{
				continue;
			}
			if( Reachability_Step_Barrier_WaterJump_WalkOffLedge( i, j ) )
			{
				continue;
			}
			//puncho start - Phase 2C P2.6: as in BuildPairsFast
			const int pointMade = aas_pointReach.GetBool() ? Reachability_FloorPoint( i, j ) : 0;
			if( pointMade < 0 )
			{
				numPointRefused++;
			}
			if( aas_stepNear.GetBool() && pointMade <= 0 )
			{
				const int stepMade = Reachability_StepNear( i, j );
				if( stepMade < 0 )
				{
					numStepRefused++;
				}
				else if( stepMade > 0 )
				{
					numStepMade++;
				}
			}
			//puncho end
		}

		//Reachability_WalkOffLedge( i );

#if !defined( DMAP )
		int percent = 100 * i / file->areas.Num();
		if( percent > lastPercent )
		{
			common->Printf( "\r%6d%%", percent );
			lastPercent = percent;
		}
#endif
	}
	//puncho - R5 speed: the end of `if( aas_reachFast.GetInteger() == 0 )` above; the loop's own lines stay where they were
	}

	if( file->GetSettings().allowFlyReachabilities )
	{
		for( i = 1; i < file->areas.Num(); i++ )
		{
			Reachability_Fly( i );
		}
	}

	PointReachFree();	//puncho - Phase 2C P2.6

	file->LinkReversedReachability();

	int numPointReach = 0;	//puncho - Phase 2C P2.6
	//puncho start - R2 widening: the game refuses a file with an area over the cap (d3xp/ai/AAS_routing.cpp,
	// CalculateAreaTravelTimes). The file is written all the same; every such area is named here
	for( i = 1; i < file->areas.Num(); i++ )
	{
		int numAreaReach = 0;
		for( const idReachability* areaReach = file->areas[i].reach; areaReach; areaReach = areaReach->next )
		{
			numAreaReach++;
			//puncho - Phase 2C P2.6: only a point or near-point reachability is a walk across edge 0
			if( areaReach->travelType == TFL_WALK && areaReach->edgeNum == 0 )
			{
				numPointReach++;
			}
		}
		numReachabilities += numAreaReach;	//puncho - R5 speed: counted here from the lists; AddReachabilityToArea no longer counts
		if( numAreaReach > MAX_REACH_PER_AREA )
		{
			common->Warning( "area %d has %d reachabilities, MAX_REACH_PER_AREA is %d: the game will not load this AAS file", i, numAreaReach, MAX_REACH_PER_AREA );
		}
	}
	//puncho end - R2 widening

	common->Printf( "\r%6d reachabilities\n", numReachabilities );
	//puncho - Phase 2C P2.6
	if( aas_pointReach.GetBool() )
	{
		common->Printf( "%6d point reachabilities, %d point contacts refused\n", numPointReach - numStepMade, numPointRefused );
	}
	if( aas_stepNear.GetBool() )
	{
		common->Printf( "%6d near-point reachabilities, %d near contacts refused\n", numStepMade, numStepRefused );
	}

	return true;
}
