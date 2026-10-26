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

#ifndef __AAS_LOCAL_H__
#define __AAS_LOCAL_H__

#include "AAS.h"
#include "../Pvs.h"


class idRoutingCache
{
	friend class idAASLocal;

public:
	idRoutingCache( int size );
	~idRoutingCache();

	int							Size() const;

private:
	int							type;					// portal or area cache
	int							size;					// size of cache
	int							cluster;				// cluster of the cache
	int							areaNum;				// area of the cache
	int							travelFlags;			// combinations of the travel flags
	idRoutingCache* 			next;					// next in list
	idRoutingCache* 			prev;					// previous in list
	idRoutingCache* 			time_next;				// next in time based list
	idRoutingCache* 			time_prev;				// previous in time based list
	unsigned short				startTravelTime;		// travel time to start with
	unsigned char* 				reachabilities;			// reachabilities used for routing
	unsigned short* 			travelTimes;			// travel time for every area
};


class idRoutingUpdate
{
	friend class idAASLocal;

private:
	int							cluster;				// cluster number of this update
	int							areaNum;				// area number of this update
	unsigned short				tmpTravelTime;			// temporary travel time
	unsigned short* 			areaTravelTimes;		// travel times within the area
	idVec3						start;					// start point into area
	idRoutingUpdate* 			next;					// next in list
	idRoutingUpdate* 			prev;					// prev in list
	bool						isInList;				// true if the update is in the list
};


class idRoutingObstacle
{
	friend class idAASLocal;
	idRoutingObstacle() { }

private:
	idBounds					bounds;					// obstacle bounds
	idList<int, TAG_AAS>					areas;					// areas the bounds are in
};


//puncho start - ai-avoidance (plans/ai-avoidance/00-PLAN.md §3.4, WP-A2): the wall-edge quads of an area, built on the
// first query for the area and its travel flags and kept for the map
typedef struct aasAreaWalls_s
{
	int							firstQuad;				// into wallQuads
	int							numQuads;
	int							travelFlags;			// the flags the quads were built against; 0 = not built
} aasAreaWalls_t;

typedef struct aasCachedWallQuad_s
{
	int							edgeNum;				// the signed AAS edge, the key of the per-query stamp
	idVec2						verts[4];				// the quad at zero agent width: end, start, start - dir, end + dir
	idVec2						back[2];				// added to verts[2] and verts[3] per unit of the agent's half width
} aasCachedWallQuad_t;

// (plans/ai-avoidance/00-PLAN.md §3.5, WP-A6): one direction field per ( goal area, travel flags ), a thin index over the
// routing cache (plan D5): a cell holds what RouteToGoalArea answered for its area, filled on the first ask
typedef struct aasFlowCell_s
{
	idVec3						edge[2];				// the floor edge the walk reachability out of the area crosses (both its end point when it names none)
	idVec3						cross;					// unit direction from the reachability's start to its end: into the next area
	int							toAreaNum;				// the area it leads to; -1 = no walk route from here
	int							stamp;					// the field's stamp when the cell was filled: unbuilt while they differ (audit M4)
} aasFlowCell_t;

typedef struct aasFlowField_s
{
	int							goalAreaNum;			// 0 = free slot
	int							travelFlags;
	int							lastUse;				// flowUseCount at the last read, the LRU key: the AAS's own count, never game time (audit M5)
	int							stamp;					// bumped on every takeover of the slot: no cell is touched until it is asked for
	aasFlowCell_t* 				cells;					// numAreas of them, a slice of flowCells (SetupFlowFields)
} aasFlowField_t;

const int MAX_AAS_FLOW_FIELDS = 16;
//puncho end

class idAASLocal : public idAAS
{
public:
	idAASLocal();
	virtual						~idAASLocal();
	virtual bool				Init( const idStr& mapName, unsigned int mapFileCRC );
	virtual void				Shutdown();
	virtual void				Stats() const;
	virtual void				GetRoutingStats( int& bytes, int& evictions, int& saturated ) const;
	virtual void				Test( const idVec3& origin );
	virtual const idAASSettings* GetSettings() const;
	virtual int					PointAreaNum( const idVec3& origin ) const;
	virtual int					PointReachableAreaNum( const idVec3& origin, const idBounds& searchBounds, const int areaFlags ) const;
	virtual int					BoundsReachableAreaNum( const idBounds& bounds, const int areaFlags ) const;
	virtual void				PushPointIntoAreaNum( int areaNum, idVec3& origin ) const;
	virtual idVec3				AreaCenter( int areaNum ) const;
	virtual int					AreaFlags( int areaNum ) const;
	virtual int					AreaTravelFlags( int areaNum ) const;
	virtual bool				Trace( aasTrace_t& trace, const idVec3& start, const idVec3& end ) const;
	virtual const idPlane& 		GetPlane( int planeNum ) const;
	virtual int					GetWallEdges( int areaNum, const idBounds& bounds, int travelFlags, int* edges, int maxEdges ) const;
	virtual void				SortWallEdges( int* edges, int numEdges ) const;
	virtual int					GetWallQuads( int areaNum, const idBounds& bounds, int travelFlags, float halfWidth, aasWallQuad_t* quads, int maxQuads ) const;	//puncho - ai-avoidance
	virtual bool				FlowFieldExit( int areaNum, const idVec3& origin, float radius, int goalAreaNum, int travelFlags, idVec3& exit, int& exitAreaNum ) const;	//puncho - ai-avoidance (WP-A6)
	virtual void				GetEdgeVertexNumbers( int edgeNum, int verts[2] ) const;
	virtual void				GetEdge( int edgeNum, idVec3& start, idVec3& end ) const;
	virtual bool				SetAreaState( const idBounds& bounds, const int areaContents, bool disabled );
	virtual aasHandle_t			AddObstacle( const idBounds& bounds );
	virtual void				RemoveObstacle( const aasHandle_t handle );
	virtual void				RemoveAllObstacles();
	virtual int					TravelTimeToGoalArea( int areaNum, const idVec3& origin, int goalAreaNum, int travelFlags ) const;
	virtual bool				RouteToGoalArea( int areaNum, const idVec3 origin, int goalAreaNum, int travelFlags, int& travelTime, idReachability** reach ) const;
	virtual bool				WalkPathToGoal( aasPath_t& path, int areaNum, const idVec3& origin, int goalAreaNum, const idVec3& goalOrigin, int travelFlags ) const;
	virtual bool				WalkPathValid( int areaNum, const idVec3& origin, int goalAreaNum, const idVec3& goalOrigin, int travelFlags, idVec3& endPos, int& endAreaNum ) const;
	virtual bool				FlyPathToGoal( aasPath_t& path, int areaNum, const idVec3& origin, int goalAreaNum, const idVec3& goalOrigin, int travelFlags ) const;
	virtual bool				FlyPathValid( int areaNum, const idVec3& origin, int goalAreaNum, const idVec3& goalOrigin, int travelFlags, idVec3& endPos, int& endAreaNum ) const;
	virtual void				ShowWalkPath( const idVec3& origin, int goalAreaNum, const idVec3& goalOrigin ) const;
	virtual void				ShowFlyPath( const idVec3& origin, int goalAreaNum, const idVec3& goalOrigin ) const;
	virtual bool				FindNearestGoal( aasGoal_t& goal, int areaNum, const idVec3 origin, const idVec3& target, int travelFlags, aasObstacle_t* obstacles, int numObstacles, idAASCallback& callback ) const;

private:
	idAASFile* 					file;
	idStr						name;

private:	// routing data
	idRoutingCache***			areaCacheIndex;			// for each area in each cluster the travel times to all other areas in the cluster
	int							areaCacheIndexSize;		// number of area cache entries
	idRoutingCache** 			portalCacheIndex;		// for each area in the world the travel times from each portal
	int							portalCacheIndexSize;	// number of portal cache entries
	idRoutingUpdate* 			areaUpdate;				// memory used to update the area routing cache
	idRoutingUpdate* 			portalUpdate;			// memory used to update the portal routing cache
	unsigned short* 			goalAreaTravelTimes;	// travel times to goal areas
	unsigned short* 			areaTravelTimes;		// travel times through the areas
	int							numAreaTravelTimes;		// number of area travel times
	mutable idRoutingCache* 	cacheListStart;			// start of list with cache sorted from oldest to newest
	mutable idRoutingCache* 	cacheListEnd;			// end of list with cache sorted from oldest to newest
	mutable int					cacheEvictions;
	mutable int					travelSaturated;
	unsigned short				ClampTravelTime( int value, bool saturate ) const;
	mutable int					totalCacheMemory;		// total cache memory used
	idList<idRoutingObstacle*, TAG_AAS>	obstacleList;			// list with obstacles

private:	// wall quads per area (ai-avoidance WP-A2), filled from const queries like the routing cache
	mutable aasAreaWalls_t* 	areaWalls;				// one per area, cleared memory until the first query
	mutable idList<aasCachedWallQuad_t, TAG_AAS>	wallQuads;	// the pool the areas index into
	mutable int* 				wallEdgeStamp;			// per signed edge (2 * numEdges), the query that last listed it
	mutable int					wallQueryStamp;			// the current query

private:	// flow fields per goal area (ai-avoidance WP-A6), filled from const queries like the routing cache they index
	mutable aasFlowField_t		flowFields[MAX_AAS_FLOW_FIELDS];
	mutable aasFlowCell_t* 		flowCells;				// MAX_AAS_FLOW_FIELDS * numAreas cells, allocated with the file (audit M4)
	mutable int					flowUseCount;			// one per FlowFieldExit, the LRU clock: a kept AAS or a restored framenum cannot skew it (audit M5)

private:	// routing
	bool						SetupRouting();
	void						ShutdownRouting();
	unsigned short				AreaTravelTime( int areaNum, const idVec3& start, const idVec3& end ) const;
	void						CalculateAreaTravelTimes();
	void						DeleteAreaTravelTimes();
	void						SetupRoutingCache();
	void						DeleteClusterCache( int clusterNum );
	void						DeletePortalCache();
	void						ShutdownRoutingCache();
	void						RoutingStats() const;
	void						LinkCache( idRoutingCache* cache ) const;
	void						UnlinkCache( idRoutingCache* cache ) const;
	void						DeleteOldestCache() const;
	idReachability* 			GetAreaReachability( int areaNum, int reachabilityNum ) const;
	int							ClusterAreaNum( int clusterNum, int areaNum ) const;
	void						UpdateAreaRoutingCache( idRoutingCache* areaCache ) const;
	idRoutingCache* 			GetAreaRoutingCache( int clusterNum, int areaNum, int travelFlags ) const;
	void						UpdatePortalRoutingCache( idRoutingCache* portalCache ) const;
	idRoutingCache* 			GetPortalRoutingCache( int clusterNum, int areaNum, int travelFlags ) const;
	void						RemoveRoutingCacheUsingArea( int areaNum );
	void						DisableArea( int areaNum );
	void						EnableArea( int areaNum );
	bool						SetAreaState_r( int nodeNum, const idBounds& bounds, const int areaContents, bool disabled );
	void						GetBoundsAreas_r( int nodeNum, const idBounds& bounds, idList<int>& areas ) const;
	void						SetObstacleState( const idRoutingObstacle* obstacle, bool enable );

private:	// pathing
	//puncho start - ai-avoidance (WP-A2)
	void						SetupWallCache();
	void						ShutdownWallCache();
	void						FlushWallCache();
	void						BuildAreaWalls( int areaNum, int travelFlags ) const;
	void						SetupFlowFields();		// WP-A6
	void						ShutdownFlowFields();	// WP-A6
	void						FlushFlowFields() const;	// WP-A6: with the routing cache, on a kept AAS, at the use count's wrap
	//puncho end
	bool						EdgeSplitPoint( idVec3& split, int edgeNum, const idPlane& plane ) const;
	bool						FloorEdgeSplitPoint( idVec3& split, int areaNum, const idPlane& splitPlane, const idPlane& frontPlane, bool closest ) const;
	idVec3						SubSampleWalkPath( int areaNum, const idVec3& origin, const idVec3& start, const idVec3& end, int travelFlags, int& endAreaNum ) const;
	idVec3						SubSampleFlyPath( int areaNum, const idVec3& origin, const idVec3& start, const idVec3& end, int travelFlags, int& endAreaNum ) const;

private:	// debug
	const idBounds& 			DefaultSearchBounds() const;
	void						DrawCone( const idVec3& origin, const idVec3& dir, float radius, const idVec4& color ) const;
	void						DrawArea( int areaNum ) const;
	void						DrawFace( int faceNum, bool side ) const;
	void						DrawEdge( int edgeNum, bool arrow ) const;
	void						DrawReachability( const idReachability* reach ) const;
	void						ShowArea( const idVec3& origin ) const;
	void						ShowWallEdges( const idVec3& origin ) const;
	void						ShowHideArea( const idVec3& origin, int targerAreaNum ) const;
	bool						PullPlayer( const idVec3& origin, int toAreaNum ) const;
	void						RandomPullPlayer( const idVec3& origin ) const;
	void						ShowPushIntoArea( const idVec3& origin ) const;
};

#endif /* !__AAS_LOCAL_H__ */
