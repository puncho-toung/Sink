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
#include "AASCluster.h"

//puncho start - P2b.3.8
idCVar aas_fixInvalidPortals( "aas_fixInvalidPortals", "0", CVAR_TOOL | CVAR_BOOL, "remove one-sided cluster portals using the opposite face area" );
//puncho end - P2b.3.8
//puncho start - R5 speed
idCVar aas_clusterFast( "aas_clusterFast", "1", CVAR_TOOL | CVAR_BOOL, "clustering: 1 = a portal area's portal comes from a table, and a cluster's areas are numbered from the flood's own list; 0 = the searches over all portals and all areas as they were; changes no output" );
idCVar aas_clusterCheck( "aas_clusterCheck", "0", CVAR_TOOL | CVAR_BOOL, "clustering with aas_clusterFast 1: also runs the old searches and stops when they give something else" );
// tests only: aas_fault ( AASReach.cpp ) 2 and 3 plant a fault here. A retail build has no such cvar and plants nothing
#ifndef ID_RETAIL
extern idCVar aas_fault;
#endif
static int AAS_ClusterFault()
{
#ifndef ID_RETAIL
	return aas_fault.GetInteger();
#else
	return 0;
#endif
}
//puncho end - R5 speed


/*
================
idAASCluster::UpdatePortal
================
*/
bool idAASCluster::UpdatePortal( int areaNum, int clusterNum )
{
	int portalNum;
	aasPortal_t* portal;

	// find the portal for this area
	//puncho start - R5 speed: CreatePortals wrote every portal area's first portal into areaPortal. The search below
	// stays for aas_clusterFast 0, and aas_clusterCheck runs it beside the table
	if( aas_clusterFast.GetBool() )
	{
		portalNum = areaPortal[areaNum];
		if( portalNum == 0 )
		{
			// no portal: where the search ends
			portalNum = file->portals.Num();
		}
		else if( portalNum < 0 || portalNum >= file->portals.Num() || file->portals[portalNum].areaNum != areaNum )
		{
			common->Error( "areaPortal: area %d is given portal %d of %d, which is not the area's", areaNum, portalNum, file->portals.Num() );
			return true;
		}
		if( aas_clusterCheck.GetBool() )
		{
			int searched;
			for( searched = 1; searched < file->portals.Num(); searched++ )
			{
				if( file->portals[searched].areaNum == areaNum )
				{
					break;
				}
			}
			if( searched != portalNum )
			{
				common->Error( "aas_clusterCheck: area %d: the table gives portal %d, the search %d", areaNum, portalNum, searched );
			}
		}
	}
	else
	{
		//puncho end - R5 speed
	for( portalNum = 1; portalNum < file->portals.Num(); portalNum++ )
	{
		if( file->portals[portalNum].areaNum == areaNum )
		{
			break;
		}
	}
	//puncho - R5 speed: the end of the else above; the search's own lines stay where they were
	}

	if( portalNum >= file->portals.Num() )
	{
		common->Error( "no portal for area %d", areaNum );
		return true;
	}

	portal = &file->portals[portalNum];

	// if the portal is already fully updated
	if( portal->clusters[0] == clusterNum )
	{
		return true;
	}
	if( portal->clusters[1] == clusterNum )
	{
		return true;
	}
	// if the portal has no front cluster yet
	if( !portal->clusters[0] )
	{
		portal->clusters[0] = clusterNum;
	}
	// if the portal has no back cluster yet
	else if( !portal->clusters[1] )
	{
		portal->clusters[1] = clusterNum;
	}
	else
	{
		// remove the cluster portal flag contents
		file->areas[areaNum].contents &= ~AREACONTENTS_CLUSTERPORTAL;
		return false;
	}

	// set the area cluster number to the negative portal number
	file->areas[areaNum].cluster = -portalNum;

	// add the portal to the cluster using the portal index
	file->portalIndex.Append( portalNum );
	file->clusters[clusterNum].numPortals++;
	return true;
}

/*
================
idAASCluster::FloodClusterAreas

//puncho - R2 widening: this was FloodClusterAreas_r, which called itself once per area. One cluster of an
outdoor map holds nearly every area, and at 10,102 areas in one cluster the calls did not fit the 1 MB stack
of a Windows build. The same walk with its frames in an array: the areas are entered, and the portals
updated, in the order the calls did it, so the file written is the same. An area gets its cluster number
once, so there are never more frames than areas.
================
*/
bool idAASCluster::FloodClusterAreas( int areaNum, int clusterNum )
{
	aasArea_t* area;
	aasFace_t* face;
	int faceNum, nextAreaNum, numFrames, maxFrames;
	bool enter, result;
	aasFloodFrame_t* frames, *frame;

	//puncho - R5 speed: frameList is a member, sized once in Build ( it was made new for every cluster ); the
	// frames are written before they are read, and the member is freed with this object, also when common->Error throws
	maxFrames = frameList.Num();
	frames = frameList.Ptr();
	numFloodAreas = 0;
	bool faultSkip = ( AAS_ClusterFault() == 2 );	// tests only, see where an area gets its cluster number
	numFrames = 0;
	result = true;
	nextAreaNum = areaNum;
	enter = true;

	while( true )
	{
		if( enter )
		{
			// what one call did before it went on to the neighbours
			enter = false;
			area = &file->areas[nextAreaNum];

			// if the area is already part of a cluster
			if( area->cluster > 0 )
			{
				if( area->cluster != clusterNum )
				{
					// there's a reachability going from one cluster to another only in one direction
					common->Error( "cluster %d touched cluster %d at area %d\r\n", clusterNum, area->cluster, nextAreaNum );
					result = false;
					break;
				}
			}
			// if this area is a cluster portal
			else if( area->contents & AREACONTENTS_CLUSTERPORTAL )
			{
				if( !UpdatePortal( nextAreaNum, clusterNum ) )
				{
					result = false;
					break;
				}
			}
			else
			{
				// set the area cluster number
				area->cluster = clusterNum;
				//puncho - R5 speed: NumberClusterAreas numbers from this list. Area 0 is the dummy: the scans over all areas
				// start at area 1, so it is never listed ( no reachability leads to it either )
				if( faultSkip )
				{
					// aas_fault 2, tests only: the first area of this flood is left out; CheckClusterAreaNumbers must refuse
					faultSkip = false;
					numFaults++;
				}
				else if( nextAreaNum > 0 )
				{
					floodAreas[numFloodAreas++] = nextAreaNum;
				}

				if( numFrames >= maxFrames )
				{
					common->Error( "idAASCluster::FloodClusterAreas: more frames than areas at area %d", nextAreaNum );
					result = false;
					break;
				}
				frame = &frames[numFrames++];
				frame->areaNum = nextAreaNum;
				frame->nextFace = 0;
				frame->reach = area->reach;
				frame->revReach = area->rev_reach;
			}
		}

		if( numFrames == 0 )
		{
			break;
		}

		// the next neighbour of the newest frame, in the order of the calls: faces, reachabilities, reversed reachabilities
		frame = &frames[numFrames - 1];
		area = &file->areas[frame->areaNum];

		if( !noFaceFlood && frame->nextFace < area->numFaces )
		{
			// use area faces to flood into adjacent areas
			faceNum = abs( file->faceIndex[area->firstFace + frame->nextFace] );
			frame->nextFace++;
			face = &file->faces[faceNum];
			if( face->areas[0] == frame->areaNum )
			{
				nextAreaNum = face->areas[1];
			}
			else
			{
				nextAreaNum = face->areas[0];
			}
			if( nextAreaNum )
			{
				enter = true;
			}
		}
		else if( frame->reach )
		{
			// use the reachabilities to flood into other areas
			nextAreaNum = frame->reach->toAreaNum;
			frame->reach = frame->reach->next;
			enter = true;
		}
		else if( frame->revReach )
		{
			// use the reversed reachabilities to flood into other areas
			nextAreaNum = frame->revReach->fromAreaNum;
			frame->revReach = frame->revReach->rev_next;
			enter = true;
		}
		else
		{
			// the call for this area would return true here
			numFrames--;
		}
	}

	return result;
}

/*
================
idAASCluster::RemoveAreaClusterNumbers
================
*/
void idAASCluster::RemoveAreaClusterNumbers()
{
	int i;

	for( i = 1; i < file->areas.Num(); i++ )
	{
		file->areas[i].cluster = 0;
	}
}

/*
================
idAASCluster::NumberClusterAreas
================
*/
void idAASCluster::NumberClusterAreas( int clusterNum )
{
	int i, k, portalNum;
	aasCluster_t* cluster;
	aasPortal_t* portal;

	cluster = &file->clusters[clusterNum];
	cluster->numAreas = 0;
	cluster->numReachableAreas = 0;

	//puncho start - R5 speed: with aas_clusterFast the two scans over the areas run over floodAreas, the areas the
	// flood gave this cluster's number, sorted ascending here: the same areas in the same order as the scans over
	// every area find, because the flood is the only code that gives an area a cluster number above 0
	const bool fast = aas_clusterFast.GetBool();
	const int numScan = fast ? numFloodAreas : file->areas.Num() - 1;
	if( fast )
	{
		std::sort( floodAreas.Ptr(), floodAreas.Ptr() + numFloodAreas );
		if( aas_clusterCheck.GetBool() )
		{
			// the check: the list against a scan over every area
			for( i = 1, k = 0; i < file->areas.Num(); i++ )
			{
				if( file->areas[i].cluster != clusterNum )
				{
					continue;
				}
				if( k >= numFloodAreas || floodAreas[k] != i )
				{
					common->Error( "aas_clusterCheck: cluster %d: area %d is not in the flood's list", clusterNum, i );
				}
				k++;
			}
			if( k != numFloodAreas )
			{
				common->Error( "aas_clusterCheck: cluster %d: the flood's list holds %d areas, the scan finds %d", clusterNum, numFloodAreas, k );
			}
		}
	}
	//puncho end - R5 speed

	// number all areas in this cluster WITH reachabilities
	for( k = 0; k < numScan; k++ )
	{
		i = fast ? floodAreas[k] : k + 1;

		if( file->areas[i].cluster != clusterNum )
		{
			continue;
		}

		if( !( file->areas[i].flags & ( AREA_REACHABLE_WALK | AREA_REACHABLE_FLY ) ) )
		{
			continue;
		}

		file->areas[i].clusterAreaNum = cluster->numAreas++;
		cluster->numReachableAreas++;
	}

	// number all portals in this cluster WITH reachabilities
	for( i = 0; i < cluster->numPortals; i++ )
	{
		portalNum = file->portalIndex[cluster->firstPortal + i];
		portal = &file->portals[portalNum];

		if( !( file->areas[portal->areaNum].flags & ( AREA_REACHABLE_WALK | AREA_REACHABLE_FLY ) ) )
		{
			continue;
		}

		if( portal->clusters[0] == clusterNum )
		{
			portal->clusterAreaNum[0] = cluster->numAreas++;
		}
		else
		{
			portal->clusterAreaNum[1] = cluster->numAreas++;
		}
		cluster->numReachableAreas++;
	}

	// number all areas in this cluster WITHOUT reachabilities
	for( k = 0; k < numScan; k++ )
	{
		i = fast ? floodAreas[k] : k + 1;

		if( file->areas[i].cluster != clusterNum )
		{
			continue;
		}

		if( file->areas[i].flags & ( AREA_REACHABLE_WALK | AREA_REACHABLE_FLY ) )
		{
			continue;
		}

		file->areas[i].clusterAreaNum = cluster->numAreas++;
	}

	// number all portals in this cluster WITHOUT reachabilities
	for( i = 0; i < cluster->numPortals; i++ )
	{
		portalNum = file->portalIndex[cluster->firstPortal + i];
		portal = &file->portals[portalNum];

		if( file->areas[portal->areaNum].flags & ( AREA_REACHABLE_WALK | AREA_REACHABLE_FLY ) )
		{
			continue;
		}

		if( portal->clusters[0] == clusterNum )
		{
			portal->clusterAreaNum[0] = cluster->numAreas++;
		}
		else
		{
			portal->clusterAreaNum[1] = cluster->numAreas++;
		}
	}
}

/*
================
idAASCluster::FindClusters
================
*/
bool idAASCluster::FindClusters()
{
	int i, clusterNum;
	aasCluster_t cluster;

	RemoveAreaClusterNumbers();

	for( i = 1; i < file->areas.Num(); i++ )
	{
		// if the area is already part of a cluster
		if( file->areas[i].cluster )
		{
			continue;
		}

		// if not flooding through faces only use areas that have reachabilities
		if( noFaceFlood )
		{
			if( !( file->areas[i].flags & ( AREA_REACHABLE_WALK | AREA_REACHABLE_FLY ) ) )
			{
				continue;
			}
		}

		// if the area is a cluster portal
		if( file->areas[i].contents & AREACONTENTS_CLUSTERPORTAL )
		{
			continue;
		}

		cluster.numAreas = 0;
		cluster.numReachableAreas = 0;
		cluster.firstPortal = file->portalIndex.Num();
		cluster.numPortals = 0;
		clusterNum = file->clusters.Num();
		file->clusters.Append( cluster );

		// flood the areas in this cluster
		if( !FloodClusterAreas( i, clusterNum ) )	//puncho - R2 widening
		{
			return false;
		}

		// number the cluster areas
		NumberClusterAreas( clusterNum );
	}
	return true;
}

/*
================
idAASCluster::CreatePortals
================
*/
void idAASCluster::CreatePortals()
{
	int i;
	aasPortal_t portal;

	//puncho start - R5 speed: areaPortal[area] = the area's portal, 0 = none; UpdatePortal reads it
	areaPortal.SetNum( file->areas.Num() );
	memset( areaPortal.Ptr(), 0, areaPortal.Num() * sizeof( int ) );
	//puncho end - R5 speed
	for( i = 1; i < file->areas.Num(); i++ )
	{
		// if the area is a cluster portal
		if( file->areas[i].contents & AREACONTENTS_CLUSTERPORTAL )
		{
			portal.areaNum = i;
			portal.clusters[0] = portal.clusters[1] = 0;
			portal.maxAreaTravelTime = 0;
			//puncho - R5 speed: an area's first portal, the one the search from portal 1 finds
			if( areaPortal[i] == 0 )
			{
				areaPortal[i] = file->portals.Num();
				if( AAS_ClusterFault() == 3 )
				{
					// tests only: one too high; UpdatePortal must refuse the entry
					areaPortal[i]++;
					numFaults++;
				}
			}
			file->portals.Append( portal );
		}
	}
}

/*
================
idAASCluster::TestPortals
================
*/
bool idAASCluster::TestPortals()
{
	int i;
	aasPortal_t* portal, *portal2;
	aasArea_t* area, *area2;
	idReachability* reach;
	bool ok;

	ok = true;
	for( i = 1; i < file->portals.Num(); i++ )
	{
		portal = &file->portals[i];
		area = &file->areas[portal->areaNum];

		// if this portal was already removed
		if( !( area->contents & AREACONTENTS_CLUSTERPORTAL ) )
		{
			continue;
		}

		// may not removed this portal if it has a reachability to a removed portal
		for( reach = area->reach; reach; reach = reach->next )
		{
			area2 = &file->areas[ reach->toAreaNum ];
			if( area2->contents & AREACONTENTS_CLUSTERPORTAL )
			{
				continue;
			}
			if( area2->cluster < 0 )
			{
				break;
			}
		}
		if( reach )
		{
			continue;
		}

		// may not removed this portal if it has a reversed reachability to a removed portal
		for( reach = area->rev_reach; reach; reach = reach->rev_next )
		{
			area2 = &file->areas[ reach->toAreaNum ];
			if( area2->contents & AREACONTENTS_CLUSTERPORTAL )
			{
				continue;
			}
			if( area2->cluster < 0 )
			{
				break;
			}
		}
		if( reach )
		{
			continue;
		}

		// portal should have two clusters set
		if( !portal->clusters[0] )
		{
			area->contents &= ~AREACONTENTS_CLUSTERPORTAL;
			ok = false;
			continue;
		}
		if( !portal->clusters[1] )
		{
			area->contents &= ~AREACONTENTS_CLUSTERPORTAL;
			ok = false;
			continue;
		}

		// this portal may not have reachabilities to a portal that doesn't seperate the same clusters
		for( reach = area->reach; reach; reach = reach->next )
		{
			area2 = &file->areas[ reach->toAreaNum ];

			if( !( area2->contents & AREACONTENTS_CLUSTERPORTAL ) )
			{
				continue;
			}

			if( area2->cluster > 0 )
			{
				area2->contents &= ~AREACONTENTS_CLUSTERPORTAL;
				ok = false;
				continue;
			}

			portal2 = &file->portals[ -file->areas[ reach->toAreaNum ].cluster ];

			if( ( portal2->clusters[0] != portal->clusters[0] && portal2->clusters[0] != portal->clusters[1] ) ||
					( portal2->clusters[1] != portal->clusters[0] && portal2->clusters[1] != portal->clusters[1] ) )
			{
				area2->contents &= ~AREACONTENTS_CLUSTERPORTAL;
				ok = false;
				continue;
			}
		}
	}

	return ok;
}

/*
================
idAASCluster::RemoveInvalidPortals
================
*/
void idAASCluster::RemoveInvalidPortals()
{
	int i, j, k, face1Num, face2Num, otherAreaNum, numOpenAreas, numInvalidPortals;
	aasFace_t* face1, *face2;

	numInvalidPortals = 0;
	for( i = 0; i < file->areas.Num(); i++ )
	{
		if( !( file->areas[i].contents & AREACONTENTS_CLUSTERPORTAL ) )
		{
			continue;
		}

		numOpenAreas = 0;
		for( j = 0; j < file->areas[i].numFaces; j++ )
		{
			face1Num = file->faceIndex[ file->areas[i].firstFace + j ];
			face1 = &file->faces[ abs( face1Num ) ];
//puncho start - P2b.3.8
			// StoreFile puts this area on side 0 for a positive face index.
			otherAreaNum = face1->areas[ aas_fixInvalidPortals.GetBool() ? face1Num > 0 : face1Num < 0 ];
//puncho end - P2b.3.8

			if( !otherAreaNum )
			{
				continue;
			}

			for( k = 0; k < j; k++ )
			{
				face2Num = file->faceIndex[ file->areas[i].firstFace + k ];
				face2 = &file->faces[ abs( face2Num ) ];
				if( otherAreaNum == face2->areas[ aas_fixInvalidPortals.GetBool() ? face2Num > 0 : face2Num < 0 ] )
				{
					break;
				}
			}
			if( k < j )
			{
				continue;
			}

			if( !( file->areas[otherAreaNum].contents & AREACONTENTS_CLUSTERPORTAL ) )
			{
				numOpenAreas++;
			}
		}

		if( numOpenAreas <= 1 )
		{
//puncho start - P2b.3.8
			file->areas[i].contents &= aas_fixInvalidPortals.GetBool() ? ~AREACONTENTS_CLUSTERPORTAL : AREACONTENTS_CLUSTERPORTAL;
//puncho end - P2b.3.8
			numInvalidPortals++;
		}
	}

	common->Printf( "\r%6d invalid portals removed\n", numInvalidPortals );
}

/*
================
idAASCluster::CheckClusterAreaNumbers

//puncho - R5 speed: run once, on the clusters that are written. NumberClusterAreas numbers the areas of a
cluster in ascending area number: the areas with reachabilities from 0; then, after the cluster's portals
with reachabilities, the areas without from numReachableAreas. This sweep over all areas gives every area
that number again and compares, and it counts each cluster's areas and portals against numReachableAreas
and numAreas. It does not read the flood's list, so it holds for either way of numbering.
================
*/
bool idAASCluster::CheckClusterAreaNumbers()
{
	int i, c, expect, portalNum;
	const int numClusters = file->clusters.Num();
	const int reachable = AREA_REACHABLE_WALK | AREA_REACHABLE_FLY;
	idList<int> with, without, portalsWith, portalsWithout;

	with.SetNum( numClusters );
	without.SetNum( numClusters );
	portalsWith.SetNum( numClusters );
	portalsWithout.SetNum( numClusters );
	for( c = 0; c < numClusters; c++ )
	{
		with[c] = 0;
		without[c] = file->clusters[c].numReachableAreas;
		portalsWith[c] = 0;
		portalsWithout[c] = 0;
	}

	for( i = 1; i < file->areas.Num(); i++ )
	{
		c = file->areas[i].cluster;
		if( c <= 0 )
		{
			continue;
		}
		if( c >= numClusters )
		{
			return false;
		}
		if( file->areas[i].flags & reachable )
		{
			expect = with[c]++;
		}
		else
		{
			expect = without[c]++;
		}
		if( file->areas[i].clusterAreaNum != expect )
		{
			return false;
		}
	}

	for( c = 1; c < numClusters; c++ )
	{
		const aasCluster_t* cluster = &file->clusters[c];
		for( i = 0; i < cluster->numPortals; i++ )
		{
			portalNum = file->portalIndex[cluster->firstPortal + i];
			if( file->areas[file->portals[portalNum].areaNum].flags & reachable )
			{
				portalsWith[c]++;
			}
			else
			{
				portalsWithout[c]++;
			}
		}
		if( with[c] + portalsWith[c] != cluster->numReachableAreas || without[c] + portalsWithout[c] != cluster->numAreas )
		{
			return false;
		}
	}
	return true;
}

/*
================
idAASCluster::Build
================
*/
bool idAASCluster::Build( idAASFileLocal* file )
{

	common->Printf( "[Clustering]\n" );

	this->file = file;
	this->noFaceFlood = true;
	//puncho start - R5 speed: the flood's frames and its list of numbered areas, sized once
	frameList.SetNum( file->areas.Num() );
	floodAreas.SetNum( file->areas.Num() );
	numFloodAreas = 0;
	numFaults = 0;
	//puncho end - R5 speed

	RemoveInvalidPortals();

	while( 1 )
	{

		// delete all existing clusters
		file->DeleteClusters();

		// create the portals from the portal areas
		CreatePortals();
		if( AAS_ClusterFault() == 3 )
		{
			//puncho - R5 speed, tests only: said here because the flood ends the compile when it meets the fault
			common->Printf( "\raas fault: 3 planted %d\n", numFaults );
		}

		common->Printf( "\r%6d", file->portals.Num() );

		// find the clusters
		if( !FindClusters() )
		{
			continue;
		}

		// test the portals
		if( !TestPortals() )
		{
			continue;
		}

		break;
	}

	common->Printf( "\r%6d portals\n", file->portals.Num() );
	//puncho start - R5 speed: which path ran ( the tests ask for this line; it comes after the portals line, so it starts
	// a line of its own ), then, always, the area numbers as NumberClusterAreas' scans over all areas give them
	common->Printf( "aas cluster: fast %d, check %d, fault %d planted %d\n", aas_clusterFast.GetBool() ? 1 : 0, aas_clusterCheck.GetBool() ? 1 : 0, AAS_ClusterFault(), numFaults );
	if( !CheckClusterAreaNumbers() )
	{
		common->Error( "the cluster area numbers are not the ones the scan over all areas gives" );
	}
	//puncho end - R5 speed

	common->Printf( "%6d clusters\n", file->clusters.Num() );

	for( int i = 0; i < file->clusters.Num(); i++ )
	{
		common->Printf( "%6d reachable areas in cluster %d\n", file->clusters[i].numReachableAreas, i );
	}

	file->ReportRoutingEfficiency();

	return true;
}

/*
================
idAASCluster::BuildSingleCluster
================
*/
bool idAASCluster::BuildSingleCluster( idAASFileLocal* file )
{
	int i, numAreas;
	aasCluster_t cluster;

	common->Printf( "[Clustering]\n" );

	this->file = file;

	// delete all existing clusters
	file->DeleteClusters();

	cluster.firstPortal = 0;
	cluster.numPortals = 0;
	cluster.numAreas = file->areas.Num();
	cluster.numReachableAreas = 0;
	// give all reachable areas in the cluster a number
	for( i = 0; i < file->areas.Num(); i++ )
	{
		file->areas[i].cluster = file->clusters.Num();
		if( file->areas[i].flags & ( AREA_REACHABLE_WALK | AREA_REACHABLE_FLY ) )
		{
			file->areas[i].clusterAreaNum = cluster.numReachableAreas++;
		}
	}
	// give the remaining areas a number within the cluster
	numAreas = cluster.numReachableAreas;
	for( i = 0; i < file->areas.Num(); i++ )
	{
		if( file->areas[i].flags & ( AREA_REACHABLE_WALK | AREA_REACHABLE_FLY ) )
		{
			continue;
		}
		file->areas[i].clusterAreaNum = numAreas++;
	}
	file->clusters.Append( cluster );

	common->Printf( "%6d portals\n", file->portals.Num() );
	common->Printf( "%6d clusters\n", file->clusters.Num() );

	for( i = 0; i < file->clusters.Num(); i++ )
	{
		common->Printf( "%6d reachable areas in cluster %d\n", file->clusters[i].numReachableAreas, i );
	}

	file->ReportRoutingEfficiency();

	return true;
}
