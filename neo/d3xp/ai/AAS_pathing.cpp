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
#include "../Game_local.h"		//puncho - ai-harness: gameLocal.aiStats

#define SUBSAMPLE_WALK_PATH		1
#define SUBSAMPLE_FLY_PATH		0

const int		maxWalkPathIterations		= 10;
const float		maxWalkPathDistance			= 500.0f;
const float		walkPathSampleDistance		= 8.0f;

const int		maxFlyPathIterations		= 10;
const float		maxFlyPathDistance			= 500.0f;

//puncho start - Phase 2C P2.6 fix round A1 ( opus/A MINOR 1 )
idCVar ai_edge0Walls( "ai_edge0Walls", "2", CVAR_GAME | CVAR_INTEGER, "a walk reachability without an edge ( P2.6's point and near-point reachabilities, edge 0 ) crosses at a corner that the wall edges round it close for the obstacle avoidance: 0 = they stay walls ( as before this switch ); 1 = a floor edge with an end within 1 unit ( and a step ) of such a reachability's start or end in the area is no wall ( ai_edge0WallProbe 1: only where floor lies beyond it, else it is cut back from the corner ); 2 = 1, and a flow field leads a body to that start before it steps across. Set before the first monster moves ( the wall cache keeps what it built )", 0, 2 );

/*
============
EdgeEndsAtEdge0Reach

  Which ends of the floor edge ( bit 0 = vertexNum[0], bit 1 = vertexNum[1] ) lie within 1 unit seen from above ( and
  maxStepHeight in height ) of the start of one of the area's edge-0 walk reachabilities or the end of one that enters
  the area ( ai_edge0Walls ). Non-zero is P2.6's EdgeAtEdge0Reach.
============
*/
static int EdgeEndsAtEdge0Reach( const idAASFile* file, const aasArea_t* area, const int edgeNum )
{
	const aasEdge_t& edge = file->GetEdge( abs( edgeNum ) );
	const float step = file->GetSettings().maxStepHeight;
	int ends = 0;
	for( int e = 0; e < 2; e++ )
	{
		const idVec3& v = file->GetVertex( edge.vertexNum[e] );
		for( const idReachability* reach = area->reach; reach && !( ends & ( 1 << e ) ); reach = reach->next )
		{
			if( reach->travelType == TFL_WALK && reach->edgeNum == 0 && ( reach->start.ToVec2() - v.ToVec2() ).LengthSqr() <= 1.0f && idMath::Fabs( reach->start.z - v.z ) <= step )
			{
				ends |= ( 1 << e );
			}
		}
		for( const idReachability* reach = area->rev_reach; reach && !( ends & ( 1 << e ) ); reach = reach->rev_next )
		{
			if( reach->travelType == TFL_WALK && reach->edgeNum == 0 && ( reach->end.ToVec2() - v.ToVec2() ).LengthSqr() <= 1.0f && idMath::Fabs( reach->end.z - v.z ) <= step )
			{
				ends |= ( 1 << e );
			}
		}
	}
	return ends;
}
//puncho end

//puncho start - Phase 2C P2.7 ( P2.6 final review MINOR 1 and NOTE 3 ): the walls at edge-0 corners and shared edges, built with the wall cache only
idCVar ai_wallSharedEdges( "ai_wallSharedEdges", "1", CVAR_GAME | CVAR_BOOL, "a floor edge that a floor face of an area this area has a walk reachability to also has ( by edge number ) is no wall for the obstacle avoidance; 0 = it stays a wall unless a reachability names it ( v0001.7o ). Set before the first monster moves ( the wall cache keeps what it built )" );
idCVar ai_edge0WallProbe( "ai_edge0WallProbe", "1", CVAR_GAME | CVAR_BOOL, "with ai_edge0Walls >= 1, a floor edge at an edge-0 corner is no wall only where the AAS has floor within a step beyond it all along ( probed every 8 units or less when the area's walls are built ); otherwise it stays a wall, cut back from the corner by ai_edge0WallCut or by the floor that runs on beyond the edge from the corner, the cut going on into the walls chained to it in the area; an edge the cuts leave less than 8 units of is no wall. 0 = every such edge is no wall ( v0001.7o ). Set before the first monster moves ( the wall cache keeps what it built )" );
idCVar ai_edge0WallCut( "ai_edge0WallCut", "0", CVAR_GAME | CVAR_FLOAT, "ai_edge0WallProbe's cut: no wall within this many units of the corner; 0 = the half width of the AAS file's box plus 8 ( 32 for aas48, 55 for aas96 ): the widest body the file serves plus WallRepulsion's margin. Set before the first monster moves", 0.0f, 1024.0f );

const float	EDGE0_WALL_MARGIN		= 8.0f;		// AI_crowd.cpp FLOW_WALL_MARGIN: a wall closer than the radius plus this pushes
const float	EDGE0_WALL_PROBE_STEP	= 8.0f;		// the far-side samples along an edge are at most this far apart
const float	EDGE0_WALL_PROBE_OUT	= 2.0f;		// and this far out of it
const float	EDGE0_WALL_MIN_LEFT		= 8.0f;		// an edge the cuts leave less of is no wall: a stub shorter than the margin guards nothing	//puncho - Phase 2C P2.7 ( Fable reviewer 2 )

/*
============
EdgeSharedWithWalkArea

  Whether a floor face of an area this area has a walk reachability to has the edge ( ai_wallSharedEdges ). Two floor
  faces with one edge meet along all of it at its height: the far side is floor the area walks to. The stock rule frees
  only the one edge a reachability names; on organic maps the next area's floor often meets this one along several.
============
*/
static bool EdgeSharedWithWalkArea( idAASFile* file, const aasArea_t* area, const int absEdgeNum, const int travelFlags )
{
	for( const idReachability* reach = area->reach; reach; reach = reach->next )
	{
		if( !( reach->travelType & travelFlags & TFL_WALK ) )
		{
			continue;
		}
		const aasArea_t& toArea = file->GetArea( reach->toAreaNum );
		for( int i = 0; i < toArea.numFaces; i++ )
		{
			const aasFace_t& face = file->GetFace( abs( file->GetFaceIndex( toArea.firstFace + i ) ) );
			if( !( face.flags & FACE_FLOOR ) )
			{
				continue;
			}
			for( int j = 0; j < face.numEdges; j++ )
			{
				if( abs( file->GetEdgeIndex( face.firstEdge + j ) ) == absEdgeNum )
				{
					return true;
				}
			}
		}
	}
	return false;
}

/*
============
Edge0WallCutLength

  ai_edge0WallCut, or the half width of the file's box plus WallRepulsion's margin: the file's widest body, so one
  cache built at zero width serves every size ( GetWallQuads scales only the back corners ).
============
*/
static float Edge0WallCutLength( const idAASFile* file )
{
	if( ai_edge0WallCut.GetFloat() > 0.0f )
	{
		return ai_edge0WallCut.GetFloat();
	}
	const idBounds& box = file->GetSettings().boundingBoxes[0];
	return Max( Max( -box[0].x, box[1].x ), Max( -box[0].y, box[1].y ) ) + EDGE0_WALL_MARGIN;
}

/*
============
FloorBeyondEdge

  Whether the AAS has floor within maxStepHeight of p ( a point of a floor edge ) EDGE0_WALL_PROBE_OUT out along out:
  a trace down from 1 unit above p's height to a step below it, or, where that start is solid, from a step above p's
  height down to 1 unit above it ( a step up ), that ends on a plane the AAS stands on. Solid a step up, open a step
  down or a steeper plane is no floor.
============
*/
static bool FloorBeyondEdge( const idAASFile* file, const idVec3& p, const idVec2& out )
{
	const idAASSettings& settings = file->GetSettings();
	idVec3 start( p.x + out.x * EDGE0_WALL_PROBE_OUT, p.y + out.y * EDGE0_WALL_PROBE_OUT, p.z + 1.0f );
	idVec3 end = start;
	if( file->PointAreaNum( start ) != 0 )
	{
		end.z = p.z - settings.maxStepHeight - 1.0f;
	}
	else
	{
		start.z = p.z + settings.maxStepHeight + 1.0f;
		if( file->PointAreaNum( start ) == 0 )
		{
			return false;
		}
	}
	aasTrace_t trace;
	if( !file->Trace( trace, start, end ) )
	{
		return false;
	}
	//puncho - Phase 2C P2.7 ( Opus reviewer A ): PointAreaNum puts a start less than ON_EPSILON in front of a plane in
	// its front area, the trace on the plane's back: a start in solid hands back plane 0 unflipped. The solid is then
	// right under the start: trace again from a little higher
	if( trace.lastAreaNum == 0 )
	{
		start.z += 0.25f;
		if( file->PointAreaNum( start ) == 0 || !file->Trace( trace, start, end ) || trace.lastAreaNum == 0 )
		{
			return false;
		}
	}
	// the trace hands back the plane facing its start
	return -( file->GetPlane( trace.planeNum ).Normal() * settings.gravityDir ) >= settings.minFloorCos;
}

/*
============
Edge0WallCuts

  ai_edge0WallProbe: the far side of a floor edge of floorFace with corner ends ( EdgeEndsAtEdge0Reach's bits ),
  sampled at most EDGE0_WALL_PROBE_STEP apart from 1 unit in from each end. Floor at every sample: no wall, false.
  Otherwise true and the cut of the wall at each corner end, cuts[0] at the signed edge's start and cuts[1] at its end:
  cutLength, or the floor run from that end ( to its last floor sample ) where that is longer; 0 at an end that is no
  corner. A corner end probes only until its run ends; an edge with one corner end is probed only until it is a wall.
============
*/
static bool Edge0WallCuts( const idAASFile* file, const aasFace_t& floorFace, const int edgeNum, const int ends, const float cutLength, float cuts[2] )
{
	const aasEdge_t& edge = file->GetEdge( abs( edgeNum ) );
	const idVec3& v0 = file->GetVertex( edge.vertexNum[0] );
	const idVec3& v1 = file->GetVertex( edge.vertexNum[1] );

	// out of the face: away from the middle of its corners, seen from above
	idVec2 middle( 0.0f, 0.0f );
	int numCorners = 0;
	for( int i = 0; i < floorFace.numEdges; i++ )
	{
		const aasEdge_t& faceEdge = file->GetEdge( abs( file->GetEdgeIndex( floorFace.firstEdge + i ) ) );
		middle += file->GetVertex( faceEdge.vertexNum[0] ).ToVec2() + file->GetVertex( faceEdge.vertexNum[1] ).ToVec2();
		numCorners += 2;
	}
	middle /= ( float )Max( 1, numCorners );
	idVec2 dir = v1.ToVec2() - v0.ToVec2();
	const float length = dir.Normalize();
	idVec2 out( dir.y, -dir.x );
	if( ( middle - ( v0.ToVec2() + v1.ToVec2() ) * 0.5f ) * out > 0.0f )
	{
		out = -out;
	}

	const int numSamples = Max( 2, ( int )idMath::Ceil( ( length - 2.0f ) / EDGE0_WALL_PROBE_STEP ) + 1 );
	auto samplePos = [&]( int k ) -> float
	{
		return ( length <= 2.0f ) ? 0.5f * length : 1.0f + ( length - 2.0f ) * ( float )k / ( float )( numSamples - 1 );
	};
	auto floorAt = [&]( int k ) -> bool
	{
		const float t = ( length > 0.0f ) ? samplePos( k ) / length : 0.5f;
		return FloorBeyondEdge( file, v0 + ( v1 - v0 ) * t, out );
	};

	// from vertexNum[0]: the floor run, and the first sample without floor ( numSamples: floor all along )
	int first = 0;
	float run0 = 0.0f;
	while( first < numSamples && floorAt( first ) )
	{
		run0 = samplePos( first );
		first++;
	}
	if( first == numSamples )
	{
		return false;
	}
	// from vertexNum[1], down to that sample
	float run1 = 0.0f;
	if( ends & 2 )
	{
		for( int k = numSamples - 1; k > first && floorAt( k ); k-- )
		{
			run1 = length - samplePos( k );
		}
	}
	const float cut0 = ( ends & 1 ) ? Max( cutLength, run0 ) : 0.0f;
	const float cut1 = ( ends & 2 ) ? Max( cutLength, run1 ) : 0.0f;
	cuts[0] = ( edgeNum < 0 ) ? cut1 : cut0;
	cuts[1] = ( edgeNum < 0 ) ? cut0 : cut1;
	return true;
}

/*
============
CircleExit

  How far along dir from p ( inside the circle of radius r around c ) a point leaves the circle.
============
*/
static float CircleExit( const idVec2& p, const idVec2& dir, const idVec2& c, const float r )
{
	const idVec2 w = p - c;
	const float b = w * dir;
	return -b + idMath::Sqrt( Max( 0.0f, b * b - w.LengthSqr() + r * r ) );
}
//puncho end
const float		flyPathSampleDistance		= 8.0f;


/*
============
idAASLocal::EdgeSplitPoint

  calculates split point of the edge with the plane
  returns true if the split point is between the edge vertices
============
*/
bool idAASLocal::EdgeSplitPoint( idVec3& split, int edgeNum, const idPlane& plane ) const
{
	const aasEdge_t* edge;
	idVec3 v1, v2;
	float d1, d2;

	edge = &file->GetEdge( edgeNum );
	v1 = file->GetVertex( edge->vertexNum[0] );
	v2 = file->GetVertex( edge->vertexNum[1] );
	d1 = v1 * plane.Normal() - plane.Dist();
	d2 = v2 * plane.Normal() - plane.Dist();

	//if ( (d1 < CM_CLIP_EPSILON && d2 < CM_CLIP_EPSILON) || (d1 > -CM_CLIP_EPSILON && d2 > -CM_CLIP_EPSILON) ) {
	if( IEEE_FLT_SIGNBITSET( d1 ) == IEEE_FLT_SIGNBITSET( d2 ) )
	{
		return false;
	}
	split = v1 + ( d1 / ( d1 - d2 ) ) * ( v2 - v1 );
	return true;
}

/*
============
idAASLocal::FloorEdgeSplitPoint

  calculates either the closest or furthest point on the floor of the area which also lies on the pathPlane
  the point has to be on the front side of the frontPlane to be valid
============
*/
bool idAASLocal::FloorEdgeSplitPoint( idVec3& bestSplit, int areaNum, const idPlane& pathPlane, const idPlane& frontPlane, bool closest ) const
{
	int i, j, faceNum, edgeNum;
	const aasArea_t* area;
	const aasFace_t* face;
	idVec3 split;
	float dist, bestDist;

	if( closest )
	{
		bestDist = maxWalkPathDistance;
	}
	else
	{
		bestDist = -0.1f;
	}

	area = &file->GetArea( areaNum );

	for( i = 0; i < area->numFaces; i++ )
	{
		faceNum = file->GetFaceIndex( area->firstFace + i );
		face = &file->GetFace( abs( faceNum ) );

		if( !( face->flags & FACE_FLOOR ) )
		{
			continue;
		}

		for( j = 0; j < face->numEdges; j++ )
		{
			edgeNum = file->GetEdgeIndex( face->firstEdge + j );

			if( !EdgeSplitPoint( split, abs( edgeNum ), pathPlane ) )
			{
				continue;
			}
			dist = frontPlane.Distance( split );
			if( closest )
			{
				if( dist >= -0.1f && dist < bestDist )
				{
					bestDist = dist;
					bestSplit = split;
				}
			}
			else
			{
				if( dist > bestDist )
				{
					bestDist = dist;
					bestSplit = split;
				}
			}
		}
	}

	if( closest )
	{
		return ( bestDist < maxWalkPathDistance );
	}
	else
	{
		return ( bestDist > -0.1f );
	}
}

/*
============
idAASLocal::WalkPathValid

  returns true if one can walk in a straight line between origin and goalOrigin
============
*/
bool idAASLocal::WalkPathValid( int areaNum, const idVec3& origin, int goalAreaNum, const idVec3& goalOrigin, int travelFlags, idVec3& endPos, int& endAreaNum ) const
{
	int curAreaNum, lastAreaNum, lastAreas[4], lastAreaIndex;
	idPlane pathPlane, frontPlane, farPlane;
	idReachability* reach;
	const aasArea_t* area;
	idVec3 p, dir;

	gameLocal.aiStats.walkPathValid++;	//puncho - ai-harness: the calls inside WalkPathToGoal included (00-PLAN.md §2.1)

	if( file == NULL )
	{
		endPos = goalOrigin;
		endAreaNum = 0;
		return true;
	}

	lastAreas[0] = lastAreas[1] = lastAreas[2] = lastAreas[3] = areaNum;
	lastAreaIndex = 0;

	pathPlane.SetNormal( ( goalOrigin - origin ).Cross( file->GetSettings().gravityDir ) );
	pathPlane.Normalize();
	pathPlane.FitThroughPoint( origin );

	frontPlane.SetNormal( goalOrigin - origin );
	frontPlane.Normalize();
	frontPlane.FitThroughPoint( origin );

	farPlane.SetNormal( frontPlane.Normal() );
	farPlane.FitThroughPoint( goalOrigin );

	curAreaNum = areaNum;
	lastAreaNum = curAreaNum;

	while( 1 )
	{

		// find the furthest floor face split point on the path
		if( !FloorEdgeSplitPoint( endPos, curAreaNum, pathPlane, frontPlane, false ) )
		{
			endPos = origin;
		}

		// if we found a point near or further than the goal we're done
		if( farPlane.Distance( endPos ) > -0.5f )
		{
			break;
		}

		// if we reached the goal area we're done
		if( curAreaNum == goalAreaNum )
		{
			break;
		}

		frontPlane.SetDist( frontPlane.Normal() * endPos );

		area = &file->GetArea( curAreaNum );

		for( reach = area->reach; reach; reach = reach->next )
		{
			if( reach->travelType != TFL_WALK )
			{
				continue;
			}

			// if the reachability goes back to a previous area
			if( reach->toAreaNum == lastAreas[0] || reach->toAreaNum == lastAreas[1] ||
					reach->toAreaNum == lastAreas[2] || reach->toAreaNum == lastAreas[3] )
			{
				continue;
			}

			// if undesired travel flags are required to travel through the area
			if( file->GetArea( reach->toAreaNum ).travelFlags & ~travelFlags )
			{
				continue;
			}

			// don't optimize through an area near a ledge
			if( file->GetArea( reach->toAreaNum ).flags & AREA_LEDGE )
			{
				continue;
			}

			// find the closest floor face split point on the path
			if( !FloorEdgeSplitPoint( p, reach->toAreaNum, pathPlane, frontPlane, true ) )
			{
				continue;
			}

			// direction parallel to gravity
			dir = ( file->GetSettings().gravityDir * endPos * file->GetSettings().gravityDir ) -
				  ( file->GetSettings().gravityDir * p * file->GetSettings().gravityDir );
			if( dir.LengthSqr() > Square( file->GetSettings().maxStepHeight ) )
			{
				continue;
			}

			// direction orthogonal to gravity
			dir = endPos - p - dir;
			if( dir.LengthSqr() > Square( 0.2f ) )
			{
				continue;
			}

			break;
		}

		if( !reach )
		{
			return false;
		}

		lastAreas[lastAreaIndex] = curAreaNum;
		lastAreaIndex = ( lastAreaIndex + 1 ) & 3;

		curAreaNum = reach->toAreaNum;
	}

	endAreaNum = curAreaNum;

	return true;
}

/*
============
idAASLocal::SubSampleWalkPath
============
*/
idVec3 idAASLocal::SubSampleWalkPath( int areaNum, const idVec3& origin, const idVec3& start, const idVec3& end, int travelFlags, int& endAreaNum ) const
{
	int i, numSamples, curAreaNum;
	idVec3 dir, point, nextPoint, endPos;

	dir = end - start;
	numSamples = ( int )( dir.Length() / walkPathSampleDistance ) + 1;

	point = start;
	for( i = 1; i < numSamples; i++ )
	{
		nextPoint = start + dir * ( ( float ) i / numSamples );
		if( ( point - nextPoint ).LengthSqr() > Square( maxWalkPathDistance ) )
		{
			return point;
		}
		if( !idAASLocal::WalkPathValid( areaNum, origin, 0, nextPoint, travelFlags, endPos, curAreaNum ) )
		{
			return point;
		}
		point = nextPoint;
		endAreaNum = curAreaNum;
	}
	return point;
}

/*
============
idAASLocal::WalkPathToGoal

  FIXME: don't stop optimizing on first failure ?
============
*/
bool idAASLocal::WalkPathToGoal( aasPath_t& path, int areaNum, const idVec3& origin, int goalAreaNum, const idVec3& goalOrigin, int travelFlags ) const
{
	int i, travelTime, curAreaNum, lastAreas[4], lastAreaIndex, endAreaNum;
	idReachability* reach = NULL;
	idVec3 endPos;

	path.type = PATHTYPE_WALK;
	path.moveGoal = origin;
	path.moveAreaNum = areaNum;
	path.secondaryGoal = origin;
	path.reachability = NULL;

	if( file == NULL || areaNum == goalAreaNum )
	{
		path.moveGoal = goalOrigin;
		return true;
	}

	lastAreas[0] = lastAreas[1] = lastAreas[2] = lastAreas[3] = areaNum;
	lastAreaIndex = 0;

	curAreaNum = areaNum;

	for( i = 0; i < maxWalkPathIterations; i++ )
	{

		if( !idAASLocal::RouteToGoalArea( curAreaNum, path.moveGoal, goalAreaNum, travelFlags, travelTime, &reach ) )
		{
			break;
		}

		if( !reach )
		{
			return false;
		}

		// no need to check through the first area
		if( areaNum != curAreaNum )
		{
			// only optimize a limited distance ahead
			if( ( reach->start - origin ).LengthSqr() > Square( maxWalkPathDistance ) )
			{
#if SUBSAMPLE_WALK_PATH
				path.moveGoal = SubSampleWalkPath( areaNum, origin, path.moveGoal, reach->start, travelFlags, path.moveAreaNum );
#endif
				return true;
			}

			if( !idAASLocal::WalkPathValid( areaNum, origin, 0, reach->start, travelFlags, endPos, endAreaNum ) )
			{
#if SUBSAMPLE_WALK_PATH
				path.moveGoal = SubSampleWalkPath( areaNum, origin, path.moveGoal, reach->start, travelFlags, path.moveAreaNum );
#endif
				return true;
			}
		}

		path.moveGoal = reach->start;
		path.moveAreaNum = curAreaNum;

		if( reach->travelType != TFL_WALK )
		{
			break;
		}

		if( !idAASLocal::WalkPathValid( areaNum, origin, 0, reach->end, travelFlags, endPos, endAreaNum ) )
		{
			return true;
		}

		path.moveGoal = reach->end;
		path.moveAreaNum = reach->toAreaNum;

		if( reach->toAreaNum == goalAreaNum )
		{
			if( !idAASLocal::WalkPathValid( areaNum, origin, 0, goalOrigin, travelFlags, endPos, endAreaNum ) )
			{
#if SUBSAMPLE_WALK_PATH
				path.moveGoal = SubSampleWalkPath( areaNum, origin, path.moveGoal, goalOrigin, travelFlags, path.moveAreaNum );
#endif
				return true;
			}
			path.moveGoal = goalOrigin;
			path.moveAreaNum = goalAreaNum;
			return true;
		}

		lastAreas[lastAreaIndex] = curAreaNum;
		lastAreaIndex = ( lastAreaIndex + 1 ) & 3;

		curAreaNum = reach->toAreaNum;

		if( curAreaNum == lastAreas[0] || curAreaNum == lastAreas[1] ||
				curAreaNum == lastAreas[2] || curAreaNum == lastAreas[3] )
		{
			common->Warning( "idAASLocal::WalkPathToGoal: local routing minimum going from area %d to area %d", areaNum, goalAreaNum );
			break;
		}
	}

	if( reach == NULL )
	{
		return false;
	}

	switch( reach->travelType )
	{
		case TFL_WALKOFFLEDGE:
			path.type = PATHTYPE_WALKOFFLEDGE;
			path.secondaryGoal = reach->end;
			path.reachability = reach;
			break;
		case TFL_BARRIERJUMP:
			path.type |= PATHTYPE_BARRIERJUMP;
			path.secondaryGoal = reach->end;
			path.reachability = reach;
			break;
		case TFL_JUMP:
			path.type |= PATHTYPE_JUMP;
			path.secondaryGoal = reach->end;
			path.reachability = reach;
			break;
		default:
			break;
	}

	return true;
}

/*
============
idAASLocal::FlyPathValid

  returns true if one can fly in a straight line between origin and goalOrigin
============
*/
bool idAASLocal::FlyPathValid( int areaNum, const idVec3& origin, int goalAreaNum, const idVec3& goalOrigin, int travelFlags, idVec3& endPos, int& endAreaNum ) const
{
	aasTrace_t trace;

	if( file == NULL )
	{
		endPos = goalOrigin;
		endAreaNum = 0;
		return true;
	}

	file->Trace( trace, origin, goalOrigin );

	endPos = trace.endpos;
	endAreaNum = trace.lastAreaNum;

	if( trace.fraction >= 1.0f )
	{
		return true;
	}

	return false;
}

/*
============
idAASLocal::SubSampleFlyPath
============
*/
idVec3 idAASLocal::SubSampleFlyPath( int areaNum, const idVec3& origin, const idVec3& start, const idVec3& end, int travelFlags, int& endAreaNum ) const
{
	int i, numSamples, curAreaNum;
	idVec3 dir, point, nextPoint, endPos;

	dir = end - start;
	numSamples = ( int )( dir.Length() / flyPathSampleDistance ) + 1;

	point = start;
	for( i = 1; i < numSamples; i++ )
	{
		nextPoint = start + dir * ( ( float ) i / numSamples );
		if( ( point - nextPoint ).LengthSqr() > Square( maxFlyPathDistance ) )
		{
			return point;
		}
		if( !idAASLocal::FlyPathValid( areaNum, origin, 0, nextPoint, travelFlags, endPos, curAreaNum ) )
		{
			return point;
		}
		point = nextPoint;
		endAreaNum = curAreaNum;
	}
	return point;
}

/*
============
idAASLocal::FlyPathToGoal

  FIXME: don't stop optimizing on first failure ?
============
*/
bool idAASLocal::FlyPathToGoal( aasPath_t& path, int areaNum, const idVec3& origin, int goalAreaNum, const idVec3& goalOrigin, int travelFlags ) const
{
	int i, travelTime, curAreaNum, lastAreas[4], lastAreaIndex, endAreaNum;
	idReachability* reach = NULL;
	idVec3 endPos;

	path.type = PATHTYPE_WALK;
	path.moveGoal = origin;
	path.moveAreaNum = areaNum;
	path.secondaryGoal = origin;
	path.reachability = NULL;

	if( file == NULL || areaNum == goalAreaNum )
	{
		path.moveGoal = goalOrigin;
		return true;
	}

	lastAreas[0] = lastAreas[1] = lastAreas[2] = lastAreas[3] = areaNum;
	lastAreaIndex = 0;

	curAreaNum = areaNum;

	for( i = 0; i < maxFlyPathIterations; i++ )
	{

		if( !idAASLocal::RouteToGoalArea( curAreaNum, path.moveGoal, goalAreaNum, travelFlags, travelTime, &reach ) )
		{
			break;
		}

		if( !reach )
		{
			return false;
		}

		// no need to check through the first area
		if( areaNum != curAreaNum )
		{
			if( ( reach->start - origin ).LengthSqr() > Square( maxFlyPathDistance ) )
			{
#if SUBSAMPLE_FLY_PATH
				path.moveGoal = SubSampleFlyPath( areaNum, origin, path.moveGoal, reach->start, travelFlags, path.moveAreaNum );
#endif
				return true;
			}

			if( !idAASLocal::FlyPathValid( areaNum, origin, 0, reach->start, travelFlags, endPos, endAreaNum ) )
			{
#if SUBSAMPLE_FLY_PATH
				path.moveGoal = SubSampleFlyPath( areaNum, origin, path.moveGoal, reach->start, travelFlags, path.moveAreaNum );
#endif
				return true;
			}
		}

		path.moveGoal = reach->start;
		path.moveAreaNum = curAreaNum;

		if( !idAASLocal::FlyPathValid( areaNum, origin, 0, reach->end, travelFlags, endPos, endAreaNum ) )
		{
			return true;
		}

		path.moveGoal = reach->end;
		path.moveAreaNum = reach->toAreaNum;

		if( reach->toAreaNum == goalAreaNum )
		{
			if( !idAASLocal::FlyPathValid( areaNum, origin, 0, goalOrigin, travelFlags, endPos, endAreaNum ) )
			{
#if SUBSAMPLE_FLY_PATH
				path.moveGoal = SubSampleFlyPath( areaNum, origin, path.moveGoal, goalOrigin, travelFlags, path.moveAreaNum );
#endif
				return true;
			}
			path.moveGoal = goalOrigin;
			path.moveAreaNum = goalAreaNum;
			return true;
		}

		lastAreas[lastAreaIndex] = curAreaNum;
		lastAreaIndex = ( lastAreaIndex + 1 ) & 3;

		curAreaNum = reach->toAreaNum;

		if( curAreaNum == lastAreas[0] || curAreaNum == lastAreas[1] ||
				curAreaNum == lastAreas[2] || curAreaNum == lastAreas[3] )
		{
			common->Warning( "idAASLocal::FlyPathToGoal: local routing minimum going from area %d to area %d", areaNum, goalAreaNum );
			break;
		}
	}

	if( reach == NULL )
	{
		return false;
	}

	return true;
}

typedef struct wallEdge_s
{
	int					edgeNum;
	int					verts[2];
	struct wallEdge_s* 	next;
} wallEdge_t;

/*
============
idAASLocal::SortWallEdges
============
*/
void idAASLocal::SortWallEdges( int* edges, int numEdges ) const
{
	int i, j, k, numSequences;
	wallEdge_t** sequenceFirst, **sequenceLast, *wallEdges, *wallEdge;

	wallEdges = ( wallEdge_t* ) _alloca16( numEdges * sizeof( wallEdge_t ) );
	sequenceFirst = ( wallEdge_t** )_alloca16( numEdges * sizeof( wallEdge_t* ) );
	sequenceLast = ( wallEdge_t** )_alloca16( numEdges * sizeof( wallEdge_t* ) );

	for( i = 0; i < numEdges; i++ )
	{
		wallEdges[i].edgeNum = edges[i];
		GetEdgeVertexNumbers( edges[i], wallEdges[i].verts );
		wallEdges[i].next = NULL;
		sequenceFirst[i] = &wallEdges[i];
		sequenceLast[i] = &wallEdges[i];
	}
	numSequences = numEdges;

	for( i = 0; i < numSequences; i++ )
	{
		for( j = i + 1; j < numSequences; j++ )
		{
			if( sequenceFirst[i]->verts[0] == sequenceLast[j]->verts[1] )
			{
				sequenceLast[j]->next = sequenceFirst[i];
				sequenceFirst[i] = sequenceFirst[j];
				break;
			}
			if( sequenceLast[i]->verts[1] == sequenceFirst[j]->verts[0] )
			{
				sequenceLast[i]->next = sequenceFirst[j];
				break;
			}
		}
		if( j < numSequences )
		{
			numSequences--;
			for( k = j; k < numSequences; k++ )
			{
				sequenceFirst[k] = sequenceFirst[k + 1];
				sequenceLast[k] = sequenceLast[k + 1];
			}
			i = -1;
		}
	}

	k = 0;
	for( i = 0; i < numSequences; i++ )
	{
		for( wallEdge = sequenceFirst[i]; wallEdge; wallEdge = wallEdge->next )
		{
			edges[k++] = wallEdge->edgeNum;
		}
	}
}

/*
============
idAASLocal::GetWallEdges
============
*/
int idAASLocal::GetWallEdges( int areaNum, const idBounds& bounds, int travelFlags, int* edges, int maxEdges ) const
{
	int i, j, k, l, face1Num, face2Num, edge1Num, edge2Num, numEdges, absEdge1Num;
	int* areaQueue, curArea, queueStart, queueEnd;
	byte* areasVisited;
	const aasArea_t* area;
	const aasFace_t* face1, *face2;
	idReachability* reach;

	if( !file || areaNum < 0 || areaNum >= file->GetNumAreas() )	//puncho - R2 scale guards: an area number outside the file wrote outside the work arrays (FlowFieldExit has the same test)
	{
		return 0;
	}

	numEdges = 0;
	//puncho - Phase 2C P2.7: with ai_wallSharedEdges or ai_edge0WallProbe the area's wall cache decides ( read once a query )
	const bool cacheDecides = ai_wallSharedEdges.GetBool() || ( ai_edge0Walls.GetInteger() >= 1 && ai_edge0WallProbe.GetBool() );

	//puncho start - R2 scale guards: the object's work arrays (SetupWallCache), not the stack, where 5 bytes an
	// area overflowed near 200,000 areas. The visited marks are all zero between queries: a query clears the
	// marks it set before it returns (both returns below), so its cost follows its box and not the map; the
	// clear of every mark at each query was 11 microseconds at 650,000 areas
	areasVisited = wallAreasVisited;
	areaQueue = wallAreaQueue;
	//puncho end - R2 scale guards

	queueStart = -1;
	queueEnd = 0;
	areaQueue[0] = areaNum;
	areasVisited[areaNum] = true;

	for( curArea = areaNum; queueStart < queueEnd; curArea = areaQueue[++queueStart] )
	{

		area = &file->GetArea( curArea );

		for( i = 0; i < area->numFaces; i++ )
		{
			face1Num = file->GetFaceIndex( area->firstFace + i );
			face1 = &file->GetFace( abs( face1Num ) );

			if( !( face1->flags & FACE_FLOOR ) )
			{
				continue;
			}

			for( j = 0; j < face1->numEdges; j++ )
			{
				edge1Num = file->GetEdgeIndex( face1->firstEdge + j );
				absEdge1Num = abs( edge1Num );

				// test if the edge is shared by another floor face of this area
				for( k = 0; k < area->numFaces; k++ )
				{
					if( k == i )
					{
						continue;
					}
					face2Num = file->GetFaceIndex( area->firstFace + k );
					face2 = &file->GetFace( abs( face2Num ) );

					if( !( face2->flags & FACE_FLOOR ) )
					{
						continue;
					}

					for( l = 0; l < face2->numEdges; l++ )
					{
						edge2Num = abs( file->GetEdgeIndex( face2->firstEdge + l ) );
						if( edge2Num == absEdge1Num )
						{
							break;
						}
					}
					if( l < face2->numEdges )
					{
						break;
					}
				}
				if( k < area->numFaces )
				{
					continue;
				}

				// test if the edge is used by a reachability
				for( reach = area->reach; reach; reach = reach->next )
				{
					if( reach->travelType & travelFlags )
					{
						if( reach->edgeNum == absEdge1Num )
						{
							break;
						}
					}
				}
				if( reach )
				{
					continue;
				}
				//puncho start - Phase 2C P2.7: with ai_wallSharedEdges or ai_edge0WallProbe the area's wall cache decides
				// ( BuildAreaWalls, once an area: the probe does not run per query ), so the two lists agree; an edge the
				// cache cut back from a corner is listed whole ( an edge list has no cut ); one it cut away, or that its
				// SortWallEdges left out, or past MAX_AREA_WALL_EDGES, is not listed
				if( cacheDecides )
				{
					if( areaWalls[curArea].travelFlags != travelFlags )
					{
						BuildAreaWalls( curArea, travelFlags );
					}
					const aasAreaWalls_t& walls = areaWalls[curArea];
					for( k = 0; k < walls.numQuads; k++ )
					{
						if( wallQuads[walls.firstQuad + k].edgeNum == edge1Num )
						{
							break;
						}
					}
					if( k == walls.numQuads )
					{
						continue;
					}
				}
				//puncho end
				//puncho - Phase 2C P2.6 fix round A1: no wall at the corner an edge-0 reachability crosses
				if( !cacheDecides && ai_edge0Walls.GetInteger() >= 1 && EdgeEndsAtEdge0Reach( file, area, edge1Num ) != 0 )	//puncho - P2.7: !cacheDecides
				{
					continue;
				}

				// test if the edge is already in the list
				for( k = 0; k < numEdges; k++ )
				{
					if( edge1Num == edges[k] )
					{
						break;
					}
				}
				if( k < numEdges )
				{
					continue;
				}

				// add the edge to the list
				edges[numEdges++] = edge1Num;
				if( numEdges >= maxEdges )
				{
					//puncho start - R2 scale guards: the marks this query set, as at the function's end
					areasVisited[areaNum] = false;
					for( i = 0; i < queueEnd; i++ )
					{
						areasVisited[areaQueue[i]] = false;
					}
					//puncho end - R2 scale guards
					return numEdges;
				}
			}
		}

		// add new areas to the queue
		for( reach = area->reach; reach; reach = reach->next )
		{
			if( reach->travelType & travelFlags )
			{
				// if the area the reachability leads to hasn't been visited yet and the area bounds touch the search bounds
				if( !areasVisited[reach->toAreaNum] && bounds.IntersectsBounds( file->GetArea( reach->toAreaNum ).bounds ) )
				{
					areaQueue[queueEnd++] = reach->toAreaNum;
					areasVisited[reach->toAreaNum] = true;
				}
			}
		}
	}
	//puncho start - R2 scale guards: every mark this query set goes, so the next query finds them all zero
	areasVisited[areaNum] = false;	// the start area: its queue slot was overwritten by the first push
	for( i = 0; i < queueEnd; i++ )
	{
		areasVisited[areaQueue[i]] = false;
	}
	//puncho end - R2 scale guards
	return numEdges;
}

//puncho start - ai-avoidance (plans/ai-avoidance/00-PLAN.md §3.4, WP-A2): wall quads per area

const int MAX_AREA_WALL_EDGES = 256;	// wall edges one area may have; GetWallEdges listed as many for a whole search box

/*
============
idAASLocal::SetupWallCache
============
*/
void idAASLocal::SetupWallCache()
{
	areaWalls = ( aasAreaWalls_t* ) Mem_ClearedAlloc( file->GetNumAreas() * sizeof( aasAreaWalls_t ), TAG_AAS );
	wallEdgeStamp = ( int* ) Mem_ClearedAlloc( 2 * file->GetNumEdges() * sizeof( int ), TAG_AAS );
	wallQueryStamp = 0;
	//puncho start - R2 scale guards: the wall queries' work arrays, for as long as the file is loaded, the marks
	// all zero. One entry at least, so that a file without areas does not leave them NULL
	const int numWork = Max( 1, file->GetNumAreas() );
	wallAreasVisited = ( byte* ) Mem_ClearedAlloc( numWork * sizeof( byte ), TAG_AAS );
	wallAreaQueue = ( int* ) Mem_ClearedAlloc( numWork * sizeof( int ), TAG_AAS );
	// the pool of quads is copied whenever it grows. It grew by 256 quads: a map of 80,225 areas copied it 346
	// times, 758 MB in all, and one of 650,000 areas would copy it thousands of times; by a quarter of the areas
	// it grows a few times a map
	wallQuads.SetGranularity( Max( 256, file->GetNumAreas() / 4 ) );
	//puncho end - R2 scale guards
}

/*
============
idAASLocal::ShutdownWallCache
============
*/
void idAASLocal::ShutdownWallCache()
{
	Mem_Free( areaWalls );
	areaWalls = NULL;
	Mem_Free( wallEdgeStamp );
	wallEdgeStamp = NULL;
	//puncho start - R2 scale guards
	Mem_Free( wallAreasVisited );
	wallAreasVisited = NULL;
	Mem_Free( wallAreaQueue );
	wallAreaQueue = NULL;
	//puncho end - R2 scale guards
	wallQuads.Clear();
}

/*
============
idAASLocal::FlushWallCache

  Every area is built again on its next query. The quads come from the file's faces, edges and reachabilities, which
  no area state change touches today; EnableArea and DisableArea flush here as they flush the routing cache, so a
  build that one day reads the area flags stays right.
============
*/
void idAASLocal::FlushWallCache()
{
	if( areaWalls == NULL )
	{
		return;
	}
	memset( areaWalls, 0, file->GetNumAreas() * sizeof( aasAreaWalls_t ) );
	wallQuads.SetNum( 0 );
}

/*
============
idAASLocal::BuildAreaWalls

  The wall edges of one area as GetWallEdges finds them (a floor edge no other floor face of the area shares and no
  reachability with the travel flags uses), chained with SortWallEdges, and the quad GetObstacles (AI_pathing.cpp)
  made per edge, kept at zero agent width with the back corners' direction per unit of half width so one cache serves
  every monster size. A chain closes within the area only: an edge whose wall goes on in the next area is extended a
  unit at that end instead of mitred, as the stock build did where its list ended.
  Phase 2C: no wall where a floor face of an area this one walks to has the edge ( ai_wallSharedEdges ), nor at an
  edge-0 corner ( ai_edge0Walls ); there ai_edge0WallProbe frees the edge only where floor lies beyond it all along,
  else cuts it back from the corner. With either switch on, GetWallEdges lists this cache's edges.
============
*/
void idAASLocal::BuildAreaWalls( int areaNum, int travelFlags ) const
{
	int i, j, k, l, face1Num, face2Num, edge1Num, edge2Num, absEdge1Num, numEdges;
	int edges[MAX_AREA_WALL_EDGES], verts[MAX_AREA_WALL_EDGES][2];
	idVec2 dirs[MAX_AREA_WALL_EDGES], edgeNormal;
	const aasArea_t* area;
	const aasFace_t* face1, *face2;
	idReachability* reach;
	idVec3 start, end;
	//puncho start - Phase 2C P2.7: an edge cut back from an edge-0 corner ( ai_edge0WallProbe ), the cut from its signed
	// start and end; kept apart from edges[] by edge number until SortWallEdges has ordered them
	int cutEdges[MAX_AREA_WALL_EDGES], numCutEdges;
	float cutLens[MAX_AREA_WALL_EDGES][2], cuts[MAX_AREA_WALL_EDGES][2], lengths[MAX_AREA_WALL_EDGES];
	const bool sharedEdges = ai_wallSharedEdges.GetBool();
	const bool edge0Probe = ai_edge0Walls.GetInteger() >= 1 && ai_edge0WallProbe.GetBool();
	const float cutLength = edge0Probe ? Edge0WallCutLength( file ) : 0.0f;
	numCutEdges = 0;
	//puncho end

	area = &file->GetArea( areaNum );
	numEdges = 0;

	for( i = 0; i < area->numFaces && numEdges < MAX_AREA_WALL_EDGES; i++ )
	{
		face1Num = file->GetFaceIndex( area->firstFace + i );
		face1 = &file->GetFace( abs( face1Num ) );

		if( !( face1->flags & FACE_FLOOR ) )
		{
			continue;
		}

		for( j = 0; j < face1->numEdges && numEdges < MAX_AREA_WALL_EDGES; j++ )
		{
			edge1Num = file->GetEdgeIndex( face1->firstEdge + j );
			absEdge1Num = abs( edge1Num );

			// test if the edge is shared by another floor face of this area
			for( k = 0; k < area->numFaces; k++ )
			{
				if( k == i )
				{
					continue;
				}
				face2Num = file->GetFaceIndex( area->firstFace + k );
				face2 = &file->GetFace( abs( face2Num ) );

				if( !( face2->flags & FACE_FLOOR ) )
				{
					continue;
				}

				for( l = 0; l < face2->numEdges; l++ )
				{
					edge2Num = abs( file->GetEdgeIndex( face2->firstEdge + l ) );
					if( edge2Num == absEdge1Num )
					{
						break;
					}
				}
				if( l < face2->numEdges )
				{
					break;
				}
			}
			if( k < area->numFaces )
			{
				continue;
			}

			// test if the edge is used by a reachability
			for( reach = area->reach; reach; reach = reach->next )
			{
				if( reach->travelType & travelFlags )
				{
					if( reach->edgeNum == absEdge1Num )
					{
						break;
					}
				}
			}
			if( reach )
			{
				continue;
			}
			//puncho start - Phase 2C P2.7: no wall where the floor of an area this one walks to meets this floor
			if( sharedEdges && EdgeSharedWithWalkArea( file, area, absEdge1Num, travelFlags ) )
			{
				continue;
			}
			//puncho end
			//puncho - Phase 2C P2.6 fix round A1: no wall at the corner an edge-0 reachability crosses
			if( ai_edge0Walls.GetInteger() >= 1 )
			{
				const int ends = EdgeEndsAtEdge0Reach( file, area, edge1Num );
				if( ends != 0 )
				{
					//puncho start - Phase 2C P2.7: only where floor lies beyond it; else the wall stays, cut at the corner
					if( !edge0Probe || !Edge0WallCuts( file, *face1, edge1Num, ends, cutLength, cutLens[numCutEdges] ) )
					{
						continue;
					}
					cutEdges[numCutEdges++] = edge1Num;
					//puncho end
				}
			}

			edges[numEdges++] = edge1Num;
		}
	}
	if( numEdges == MAX_AREA_WALL_EDGES )
	{
		common->DWarning( "idAASLocal::BuildAreaWalls: area %d of %s has more than %d wall edges", areaNum, file->GetName(), MAX_AREA_WALL_EDGES );
	}

	SortWallEdges( edges, numEdges );

	for( i = 0; i < numEdges; i++ )
	{
		GetEdge( edges[i], start, end );
		GetEdgeVertexNumbers( edges[i], verts[i] );
		dirs[i] = end.ToVec2() - start.ToVec2();
		lengths[i] = dirs[i].Normalize();	//puncho - Phase 2C P2.7: the length, for the cuts
		cuts[i][0] = cuts[i][1] = 0.0f;	//puncho - Phase 2C P2.7
	}

	//puncho start - Phase 2C P2.7: each cut edge's cuts back on its place in the sorted list; then the cut goes on from
	// the corner into the edges chained to it, while the edge before is cut away to its end and the chain is still
	// inside the circle of cutLength around the corner ( a corner edge's own cut may be longer where floor lies
	// beyond it; the chain is cut by the circle only ). An edge the cuts leave less than EDGE0_WALL_MIN_LEFT of is no wall
	if( numCutEdges > 0 )
	{
		for( i = 0; i < numEdges; i++ )
		{
			for( k = 0; k < numCutEdges; k++ )
			{
				if( cutEdges[k] == edges[i] )
				{
					cuts[i][0] = cutLens[k][0];
					cuts[i][1] = cutLens[k][1];
					break;
				}
			}
		}
		for( k = 0; k < numCutEdges; k++ )
		{
			for( i = 0; i < numEdges && edges[i] != cutEdges[k]; i++ )
			{
			}
			// SortWallEdges can leave an edge out of its list ( stock: chaining a sequence after sequence i does not move
			// sequenceLast[i] on, so a later link from the old last drops the chain after it ): such an edge has no place to cut
			if( i == numEdges )
			{
				continue;
			}
			GetEdge( edges[i], start, end );
			// a corner at the start: on past the edge's end into the edges chained after it
			if( cutLens[k][0] > 0.0f )
			{
				const idVec2 corner = start.ToVec2();
				for( j = i; j < numEdges - 1 && verts[j][1] == verts[j + 1][0] && cuts[j][0] + cuts[j][1] >= lengths[j] - EDGE0_WALL_MIN_LEFT; j++ )
				{
					idVec3 nextStart, nextEnd;
					GetEdge( edges[j + 1], nextStart, nextEnd );
					if( ( nextStart.ToVec2() - corner ).LengthSqr() >= Square( cutLength ) )
					{
						break;
					}
					cuts[j + 1][0] = Max( cuts[j + 1][0], CircleExit( nextStart.ToVec2(), dirs[j + 1], corner, cutLength ) );
				}
			}
			// a corner at the end: back past the edge's start into the edges chained before it
			if( cutLens[k][1] > 0.0f )
			{
				const idVec2 corner = end.ToVec2();
				for( j = i; j > 0 && verts[j - 1][1] == verts[j][0] && cuts[j][0] + cuts[j][1] >= lengths[j] - EDGE0_WALL_MIN_LEFT; j-- )
				{
					idVec3 prevStart, prevEnd;
					GetEdge( edges[j - 1], prevStart, prevEnd );
					if( ( prevEnd.ToVec2() - corner ).LengthSqr() >= Square( cutLength ) )
					{
						break;
					}
					cuts[j - 1][1] = Max( cuts[j - 1][1], CircleExit( prevEnd.ToVec2(), -dirs[j - 1], corner, cutLength ) );
				}
			}
		}
	}
	auto cutAway = [&]( int n ) -> bool
	{
		return ( cuts[n][0] > 0.0f || cuts[n][1] > 0.0f ) && cuts[n][0] + cuts[n][1] >= lengths[n] - EDGE0_WALL_MIN_LEFT;
	};
	//puncho end

	aasAreaWalls_t& walls = areaWalls[areaNum];
	walls.firstQuad = wallQuads.Num();
	walls.numQuads = 0;	//puncho - Phase 2C P2.7: counted below ( numEdges where no edge is cut away )
	walls.travelFlags = travelFlags;

	for( i = 0; i < numEdges; i++ )
	{
		//puncho - Phase 2C P2.7
		if( cutAway( i ) )
		{
			continue;
		}
		GetEdge( edges[i], start, end );
		edgeNormal.Set( dirs[i].y, -dirs[i].x );

		aasCachedWallQuad_t& quad = wallQuads.Alloc();
		walls.numQuads++;	//puncho - Phase 2C P2.7
		quad.edgeNum = edges[i];
		quad.verts[0] = end.ToVec2();
		quad.verts[1] = start.ToVec2();
		quad.verts[2] = start.ToVec2() - dirs[i];
		quad.verts[3] = end.ToVec2() + dirs[i];
		quad.back[0] = -edgeNormal;
		quad.back[1] = -edgeNormal;
		// a chained corner is mitred with the neighbour's normal, a free end is extended along the edge
		//puncho - Phase 2C P2.7: a cut end stops square at its cut, unextended; no mitre across a cut or with an edge cut away
		if( cuts[i][0] > 0.0f )
		{
			quad.verts[1] = start.ToVec2() + dirs[i] * cuts[i][0];
			quad.verts[2] = quad.verts[1];
		}
		else if( i > 0 && verts[i - 1][1] == verts[i][0] && cuts[i - 1][1] == 0.0f && !cutAway( i - 1 ) )
		{
			quad.back[0] -= idVec2( dirs[i - 1].y, -dirs[i - 1].x );
		}
		else
		{
			quad.verts[1] -= dirs[i];
		}
		if( cuts[i][1] > 0.0f )
		{
			quad.verts[0] = end.ToVec2() - dirs[i] * cuts[i][1];
			quad.verts[3] = quad.verts[0];
		}
		else if( i < numEdges - 1 && verts[i][1] == verts[i + 1][0] && cuts[i + 1][0] == 0.0f && !cutAway( i + 1 ) )
		{
			quad.back[1] -= idVec2( dirs[i + 1].y, -dirs[i + 1].x );
		}
		else
		{
			quad.verts[0] += dirs[i];
		}
	}
}

/*
============
idAASLocal::GetWallQuads

  The same walk over the areas as GetWallEdges, each area's quads from the cache. A query stamp per signed edge
  drops an edge two areas both list, as GetWallEdges' list test did, without a search.
============
*/
int idAASLocal::GetWallQuads( int areaNum, const idBounds& bounds, int travelFlags, float halfWidth, aasWallQuad_t* quads, int maxQuads ) const
{
	int i, numQuads, stampIndex;
	int* areaQueue, curArea, queueStart, queueEnd;
	byte* areasVisited;
	const aasArea_t* area;
	idReachability* reach;

	if( !file || areaNum < 0 || areaNum >= file->GetNumAreas() )	//puncho - R2 scale guards: as in GetWallEdges
	{
		return 0;
	}

	wallQueryStamp++;
	if( wallQueryStamp == INT_MAX )
	{
		memset( wallEdgeStamp, 0, 2 * file->GetNumEdges() * sizeof( int ) );
		wallQueryStamp = 1;
	}

	numQuads = 0;

	//puncho start - R2 scale guards: the object's work arrays, as in GetWallEdges; the marks are all zero here
	areasVisited = wallAreasVisited;
	areaQueue = wallAreaQueue;
	//puncho end - R2 scale guards

	queueStart = -1;
	queueEnd = 0;
	areaQueue[0] = areaNum;
	areasVisited[areaNum] = true;

	for( curArea = areaNum; queueStart < queueEnd; curArea = areaQueue[++queueStart] )
	{
		if( areaWalls[curArea].travelFlags != travelFlags )
		{
			BuildAreaWalls( curArea, travelFlags );
		}
		const aasAreaWalls_t& walls = areaWalls[curArea];
		for( i = 0; i < walls.numQuads; i++ )
		{
			const aasCachedWallQuad_t& quad = wallQuads[walls.firstQuad + i];
			stampIndex = ( abs( quad.edgeNum ) << 1 ) | INT32_SIGNBITSET( quad.edgeNum );
			if( wallEdgeStamp[stampIndex] == wallQueryStamp )
			{
				continue;
			}
			wallEdgeStamp[stampIndex] = wallQueryStamp;
			if( numQuads < maxQuads )
			{
				quads[numQuads].verts[0] = quad.verts[0];
				quads[numQuads].verts[1] = quad.verts[1];
				quads[numQuads].verts[2] = quad.verts[2] + quad.back[0] * halfWidth;
				quads[numQuads].verts[3] = quad.verts[3] + quad.back[1] * halfWidth;
			}
			numQuads++;
		}

		// add new areas to the queue
		area = &file->GetArea( curArea );
		for( reach = area->reach; reach; reach = reach->next )
		{
			if( reach->travelType & travelFlags )
			{
				// if the area the reachability leads to hasn't been visited yet and the area bounds touch the search bounds
				if( !areasVisited[reach->toAreaNum] && bounds.IntersectsBounds( file->GetArea( reach->toAreaNum ).bounds ) )
				{
					areaQueue[queueEnd++] = reach->toAreaNum;
					areasVisited[reach->toAreaNum] = true;
				}
			}
		}
	}
	//puncho start - R2 scale guards: every mark this query set goes, as in GetWallEdges
	areasVisited[areaNum] = false;	// the start area: its queue slot was overwritten by the first push
	for( i = 0; i < queueEnd; i++ )
	{
		areasVisited[areaQueue[i]] = false;
	}
	//puncho end - R2 scale guards
	return numQuads;
}
//puncho end

//puncho start - ai-avoidance (plans/ai-avoidance/00-PLAN.md §3.5, WP-A6): flow fields per goal area

/*
============
idAASLocal::SetupFlowFields

  The cells of every slot at once, with the file (audit M4): a slot takeover inside a think is then a stamp bump, not an
  allocation or a memset of numAreas cells. MAX_AAS_FLOW_FIELDS * numAreas * sizeof( aasFlowCell_t ), 44 B per area per slot
============
*/
void idAASLocal::SetupFlowFields()
{
	const int numAreas = file->GetNumAreas();

	flowCells = ( aasFlowCell_t* ) Mem_ClearedAlloc( MAX_AAS_FLOW_FIELDS * numAreas * sizeof( aasFlowCell_t ), TAG_AAS );
	memset( flowFields, 0, sizeof( flowFields ) );
	for( int i = 0; i < MAX_AAS_FLOW_FIELDS; i++ )
	{
		flowFields[i].cells = flowCells + i * numAreas;
	}
	flowUseCount = 0;
}

/*
============
idAASLocal::ShutdownFlowFields
============
*/
void idAASLocal::ShutdownFlowFields()
{
	Mem_Free( flowCells );
	flowCells = NULL;
	memset( flowFields, 0, sizeof( flowFields ) );
}

/*
============
idAASLocal::FlushFlowFields

  Every slot is free again and its cells are refilled on the next ask. Called where the routing cache is deleted, since
  a cell is one of its answers, when a kept AAS is initialised for the next game (audit M5) and at the use count's wrap;
  the cell memory stays allocated for the map.
============
*/
void idAASLocal::FlushFlowFields() const
{
	for( int i = 0; i < MAX_AAS_FLOW_FIELDS; i++ )
	{
		flowFields[i].goalAreaNum = 0;
		flowFields[i].travelFlags = 0;
		flowFields[i].lastUse = 0;
	}
}

/*
============
idAASLocal::FlowFieldExit

  The field of ( goalAreaNum, travelFlags ) from the 16 slots, the least recently read slot taken over for a new goal;
  the cell of areaNum is filled with one RouteToGoalArea on its first ask, so a horde to one goal pays O( areas touched )
  once and a table read per minion per tic after that. Only a walk reachability is an exit: any other travel type is the
  stock path's job (jumps, ledges), as is the goal area itself and an unreachable goal. The point handed back is the one on
  the crossed floor edge nearest to the asker, a radius in from the edge's ends and FLOW_EXIT_STEP into the next area: a
  horde spreads over the edge, and a body on the edge still has something ahead of it to turn toward.
============
*/
const float FLOW_EXIT_STEP = 8.0f;

bool idAASLocal::FlowFieldExit( int areaNum, const idVec3& origin, float radius, int goalAreaNum, int travelFlags, idVec3& exit, int& exitAreaNum ) const
{
	int i, oldest;
	aasFlowField_t* field;

	if( file == NULL || areaNum <= 0 || areaNum >= file->GetNumAreas() || goalAreaNum <= 0 || goalAreaNum >= file->GetNumAreas() || areaNum == goalAreaNum )
	{
		return false;
	}

	// the LRU clock is this AAS's own count of asks (audit M5): game time restarts at 0 on a load while a kept AAS keeps its
	// slots, and slots stamped with framenum then won every comparison for minutes. The one wrap in a very long session
	// hands every slot back instead of comparing across it
	if( flowUseCount == INT_MAX )
	{
		FlushFlowFields();
		flowUseCount = 0;
	}
	flowUseCount++;

	field = NULL;
	oldest = 0;
	for( i = 0; i < MAX_AAS_FLOW_FIELDS; i++ )
	{
		if( flowFields[i].goalAreaNum == goalAreaNum && flowFields[i].travelFlags == travelFlags )
		{
			field = &flowFields[i];
			break;
		}
		if( flowFields[i].lastUse < flowFields[oldest].lastUse )
		{
			oldest = i;
		}
	}
	if( field == NULL )
	{
		field = &flowFields[oldest];
		field->goalAreaNum = goalAreaNum;
		field->travelFlags = travelFlags;
		// a new stamp unbuilds every cell without touching one (audit M4); the memset is at the wrap, once per INT_MAX takeovers
		if( field->stamp == INT_MAX )
		{
			memset( field->cells, 0, file->GetNumAreas() * sizeof( aasFlowCell_t ) );
			field->stamp = 0;
		}
		field->stamp++;
		gameLocal.aiStats.flowFieldsOpened++;
	}
	field->lastUse = flowUseCount;

	aasFlowCell_t& cell = field->cells[areaNum];
	if( cell.stamp != field->stamp )
	{
		int travelTime;
		idReachability* reach;

		cell.stamp = field->stamp;
		cell.toAreaNum = -1;
		if( RouteToGoalArea( areaNum, origin, goalAreaNum, travelFlags, travelTime, &reach ) && reach != NULL && reach->travelType == TFL_WALK )
		{
			if( reach->edgeNum != 0 )
			{
				GetEdge( reach->edgeNum, cell.edge[0], cell.edge[1] );
			}
			else
			{
				cell.edge[0] = cell.edge[1] = reach->end;
				//puncho - Phase 2C P2.6 fix round A1: ai_edge0Walls 2, the crossing is at the reachability's start
				if( ai_edge0Walls.GetInteger() >= 2 )
				{
					cell.edge[0] = cell.edge[1] = reach->start;
				}
			}
			cell.cross = reach->end - reach->start;
			cell.cross.z = 0.0f;
			if( cell.cross.Normalize() < 0.001f )
			{
				cell.cross.Zero();
			}
			cell.toAreaNum = reach->toAreaNum;
		}
		gameLocal.aiStats.flowCellsBuilt++;
	}
	if( cell.toAreaNum < 0 )
	{
		return false;
	}

	// the nearest point of the edge, kept a radius from its ends when the edge is long enough for that, then a step across
	idVec3 edgeDir = cell.edge[1] - cell.edge[0];
	const float edgeLen = edgeDir.Normalize();
	float along = ( origin - cell.edge[0] ) * edgeDir;
	if( edgeLen > 2.0f * radius )
	{
		along = idMath::ClampFloat( radius, edgeLen - radius, along );
	}
	else
	{
		along = edgeLen * 0.5f;
	}
	exit = cell.edge[0] + edgeDir * along + cell.cross * FLOW_EXIT_STEP;
	//puncho start - Phase 2C P2.6 fix round A1: ai_edge0Walls 2, a body more than 16 units from an edge-0 crossing
	// ( a point, the only exit without length ) is led to the point itself first
	if( ai_edge0Walls.GetInteger() >= 2 && edgeLen == 0.0f && ( origin.ToVec2() - cell.edge[0].ToVec2() ).LengthSqr() > Square( 16.0f ) )
	{
		exit = cell.edge[0];
	}
	//puncho end
	exitAreaNum = cell.toAreaNum;
	return true;
}
//puncho end
