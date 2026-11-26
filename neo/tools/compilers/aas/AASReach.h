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

#ifndef __AASREACH_H__
#define __AASREACH_H__

/*
===============================================================================

	Reachabilities

===============================================================================
*/

class idAASReach
{

public:
	bool					Build( const idMapFile* mapFile, idAASFileLocal* file );

private:
	const idMapFile* 		mapFile;
	idAASFileLocal* 		file;
	int						numReachabilities;
	bool					allowSwimReachabilities;
	bool					allowFlyReachabilities;

private:	// reachability
	void					FlagReachableAreas( idAASFileLocal* file );
	bool					ReachabilityExists( int fromAreaNum, int toAreaNum );
	bool					CanSwimInArea( int areaNum );
	bool					AreaHasFloor( int areaNum );
	bool					AreaIsClusterPortal( int areaNum );
	void					AddReachabilityToArea( idReachability* reach, int areaNum );
	void					Reachability_Fly( int areaNum );
	void					Reachability_Swim( int areaNum );
	void					Reachability_EqualFloorHeight( int areaNum );
	bool					Reachability_Step_Barrier_WaterJump_WalkOffLedge( int fromAreaNum, int toAreaNum );
	void					Reachability_WalkOffLedge( int areaNum );
	void					BuildPairsFast();	//puncho - R5 speed

	//puncho start - Phase 2C P2.6: a walk reachability across a single floor vertex
	idList<int>				pointFirstVertex;	// per area ( and one more ): where its floor vertices start in pointVertices
	idList<int>				pointVertices;		// every area's floor vertex numbers, ascending within the area
	idList<int>				pointFirstEdge;		// per area ( and one more ): where its floor edges start in pointEdges
	idList<int>				pointEdges;			// every area's floor edge numbers ( abs ), ascending within the area
	idList<byte>			pointOpen;			// per vertex: 1 = the floor faces at it close the whole angle round it, 2 = a portal's face at it
	idList<int>				pointVertexFirst;	// per vertex ( and one more ): where its floor faces start in pointVertexFaces
	idList<int>				pointVertexFaces;	// every vertex's floor faces
	idList<int>				pointVertexRun;		// beside pointVertexFaces: the face's run between portal faces, -1 = a portal's face
	idList<int>				pointFaceArea;		// per face: the area whose floor it is ( the first that lists it )
	int						numPointRefused;	// point contacts the pair loop refused ( for the log )
	void					PointReachInit();
	void					PointReachFree();
	bool					FloorFaceSector( int faceNum, int vertexNum, double& start, double& span ) const;
	bool					PointSameRun( int area1num, int area2num, int vertexNum ) const;
	int						Reachability_FloorPoint( int area1num, int area2num );
	idList<int>				stepGroup;			// per vertex: its group of near floor corners ( aas_stepNear ), -1 = none
	idList<byte>			stepGroupOpen;		// per group: 1 = open ( StepNearInit )
	idList<int>				stepFirstGroup;		// per area ( and one more ): where its groups start in stepGroups
	idList<int>				stepGroups;			// every area's groups, ascending within the area
	int						numStepRefused;		// near contacts the pair loop refused ( for the log )
	int						numStepMade;		// near-point reachabilities made ( for the log )
	void					StepNearInit( const idList<byte>& facePortal, const idList<byte>& faceAxial );
	bool					GroupFaceSector( int faceNum, int group, double ax, double ay, double& start, double& span ) const;
	int						Reachability_StepNear( int area1num, int area2num );
	//puncho end

};

#endif /* !__AASREACH_H__ */
