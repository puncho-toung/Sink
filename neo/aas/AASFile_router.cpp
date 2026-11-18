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

//puncho - R2 router: new file
#include "precompiled.h"
#pragma hdrstop

#include "AASFile.h"
#include "AASFile_router.h"

/*
============
AreaTime

  The time of a straight move inside an area: the arithmetic of idAASLocal::AreaTravelTime
  (d3xp/ai/AAS_routing.cpp), in the same order, without the clamp to 16 bits. Keep the two in step.
  tf: the area's travel flags. Returns false when the time is no number the router can keep.
============
*/
static bool AreaTime( const int tf, const idVec3& start, const idVec3& end, unsigned int& time )
{
	float dist;

	dist = ( end - start ).Length();

	if( tf & TFL_CROUCH )
	{
		dist *= 100.0f / 100.0f;
	}
	else if( tf & TFL_WATER )
	{
		dist *= 100.0f / 150.0f;
	}
	else
	{
		dist *= 100.0f / 300.0f;
	}
	if( IEEE_FLT_IS_NAN( dist ) || IEEE_FLT_IS_INF( dist ) || dist >= 2147483000.0f )
	{
		return false;
	}
	if( dist < 1.0f )
	{
		time = 1;
		return true;
	}
	time = ( unsigned int )idMath::Ftoi( dist );
	return true;
}

/*
============
idAASRouterMap::idAASRouterMap
============
*/
idAASRouterMap::idAASRouterMap()
{
	numAreas = 0;
	travelFlags = 0;
}

/*
============
idAASRouterMap::Clear
============
*/
void idAASRouterMap::Clear()
{
	numAreas = 0;
	travelFlags = 0;
	edges.Clear();
	out.Clear();
	in.Clear();
	rev.Clear();
	enabled.Clear();
	mutableArea.Clear();
	origin.Clear();
	reach.Clear();
}

/*
============
idAASRouterMap::Build
============
*/
bool idAASRouterMap::Build( idAASFile* file, int mask, idStr& error )
{
	int a;
	int e;
	idList<int> pos;

	Clear();

	numAreas = file->GetNumAreas();
	travelFlags = mask;

	enabled.SetNum( numAreas );
	mutableArea.SetNum( numAreas );
	for( a = 0; a < numAreas; a++ )
	{
		enabled[a] = 0;
		mutableArea[a] = 0;
	}
	for( a = 1; a < numAreas; a++ )
	{
		const aasArea_t& area = file->GetArea( a );
		const int areaFlag = ( area.contents & AREACONTENTS_WATER ) ? TFL_WATER : TFL_AIR;

		enabled[a] = ( ( area.flags & ( AREA_REACHABLE_WALK | AREA_REACHABLE_FLY ) ) != 0 && ( areaFlag & ~mask ) == 0 ) ? 1 : 0;
		mutableArea[a] = ( ( area.contents & ( AREACONTENTS_CLUSTERPORTAL | AREACONTENTS_OBSTACLE ) ) != 0 ) ? 1 : 0;
	}

	// the eligible reachabilities are counted first and the two lists made in one piece: a list that grows
	// by Append is copied whole every 16 entries, which costs with the square of a large map's reachabilities
	int numEdges = 0;
	for( a = 1; a < numAreas; a++ )
	{
		if( enabled[a] )
		{
			for( idReachability* r = file->GetArea( a ).reach; r != NULL; r = r->next )
			{
				if( r->toAreaNum > 0 && r->toAreaNum < numAreas && enabled[r->toAreaNum] && ( r->travelType & ~TFL_INVALID & ~mask ) == 0 )
				{
					numEdges++;
				}
			}
		}
	}
	edges.SetNum( numEdges );
	reach.SetNum( numEdges );

	out.SetNum( numAreas + 1 );
	e = 0;
	for( a = 0; a < numAreas; a++ )
	{
		out[a] = e;
		if( enabled[a] )
		{
			int ordinal = 0;
			idReachability* r;

			for( r = file->GetArea( a ).reach; r != NULL; r = r->next, ordinal++ )
			{
				if( r->fromAreaNum != a )
				{
					error = "a reachability in the list of another area";
					Clear();
					return false;
				}
				if( r->toAreaNum > 0 && r->toAreaNum < numAreas && enabled[r->toAreaNum] && ( r->travelType & ~TFL_INVALID & ~mask ) == 0 )
				{
					edges[e].from = a;
					edges[e].to = r->toAreaNum;
					edges[e].time = 0;
					edges[e].ordinal = ordinal;
					reach[e] = r;
					e++;
				}
			}
		}
	}
	out[numAreas] = e;

	in.SetNum( numAreas + 1 );
	for( a = 0; a <= numAreas; a++ )
	{
		in[a] = 0;
	}
	for( e = 0; e < edges.Num(); e++ )
	{
		in[ edges[e].to + 1 ]++;
	}
	for( a = 1; a <= numAreas; a++ )
	{
		in[a] += in[a - 1];
	}
	rev.SetNum( edges.Num() );
	pos.SetNum( numAreas + 1 );
	for( a = 0; a <= numAreas; a++ )
	{
		pos[a] = in[a];
	}
	for( e = 0; e < edges.Num(); e++ )
	{
		const int to = edges[e].to;

		rev[ pos[to] ] = e;
		pos[to]++;
	}

	for( e = 0; e < edges.Num(); e++ )
	{
		const int u = edges[e].from;
		idReachability* r = reach[e];
		const aasArea_t& area = file->GetArea( u );
		const int tf = area.travelFlags;
		unsigned int m = 0;

		for( int k = in[u]; k < in[u + 1]; k++ )
		{
			const int j = rev[k];
			unsigned int t;

			if( !AreaTime( tf, reach[j]->end, r->start, t ) )
			{
				error = "an area travel time out of range";
				Clear();
				return false;
			}
			if( t > m )
			{
				m = t;
			}
		}
		if( m < 1 )
		{
			m = 1;
		}
		const unsigned int ledge = ( ( area.flags & AREA_LEDGE ) != 0 && ( mask & TFL_FLY ) == 0 ) ? LEDGE_TRAVELTIME_PANALTY : 0;
		const unsigned long long w = ( unsigned long long )r->travelTime + m + ledge;
		if( w < 1 || w > 2147483647ULL )
		{
			error = "a reach time out of range";
			Clear();
			return false;
		}
		edges[e].time = ( unsigned int )w;
	}

	origin.SetNum( edges.Num() );
	for( e = 0; e < edges.Num(); e++ )
	{
		const int u = edges[e].from;
		const aasArea_t& area = file->GetArea( u );
		idReachability* r = reach[e];
		unsigned int t;

		if( !AreaTime( area.travelFlags, area.center, r->start, t ) )
		{
			error = "an area travel time out of range";
			Clear();
			return false;
		}
		origin[e] = t;
	}

	return true;
}

/*
============
idAASRouterMap::Write

  The file for the bake tool, every number little-endian:
	"RTG1"
	int numAreas, int numEdges
	numEdges times: int from, int to, unsigned int time, int ordinal
	numAreas + 1 ints: out;  numAreas + 1 ints: in;  numEdges ints: rev
	numAreas times: byte enabled, byte mutableArea, byte 255
	numEdges unsigned ints: origin
	int 0
  The 255 and the last 0 say that the kinds of the doors and the doors' groups are for the bake tool to
  work out. The file is put together in memory and written with one call (a write per number would be a
  system call each); false when not every byte was written.
============
*/
bool idAASRouterMap::Write( idFile* f ) const
{
	int a;
	int e;
	idFile_Memory mem( "routermap" );
	const int expected = 4 + 4 * 2 + 16 * edges.Num() + 4 * ( out.Num() + in.Num() + rev.Num() ) + 3 * numAreas + 4 * origin.Num() + 4;

	mem.PreAllocate( expected + 1 );		// in one piece: the memory file grows by copying itself
	mem.Write( "RTG1", 4 );
	mem.WriteInt( numAreas );
	mem.WriteInt( edges.Num() );
	for( e = 0; e < edges.Num(); e++ )
	{
		mem.WriteInt( edges[e].from );
		mem.WriteInt( edges[e].to );
		mem.WriteUnsignedInt( edges[e].time );
		mem.WriteInt( edges[e].ordinal );
	}
	for( a = 0; a < out.Num(); a++ )
	{
		mem.WriteInt( out[a] );
	}
	for( a = 0; a < in.Num(); a++ )
	{
		mem.WriteInt( in[a] );
	}
	for( e = 0; e < rev.Num(); e++ )
	{
		mem.WriteInt( rev[e] );
	}
	for( a = 0; a < numAreas; a++ )
	{
		mem.WriteUnsignedChar( enabled[a] );
		mem.WriteUnsignedChar( mutableArea[a] );
		mem.WriteUnsignedChar( 255 );
	}
	for( e = 0; e < origin.Num(); e++ )
	{
		mem.WriteUnsignedInt( origin[e] );
	}
	mem.WriteInt( 0 );

	return mem.Length() == expected && f->Write( mem.GetDataPtr(), mem.Length() ) == mem.Length();
}

