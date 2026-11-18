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


#include "../../aas/AASFile_router.h"
#include "AAS_router.h"

// The runtime is plain C++ with the standard library; four names of it are macros of the Windows headers.
// Its headers are included here and nowhere else (AAS_router.h says why).
#pragma push_macro( "min" )
#pragma push_macro( "max" )
#pragma push_macro( "near" )
#pragma push_macro( "far" )
#undef min
#undef max
#undef near
#undef far

#include <memory>
#include "../../libs/aasrouter/rt_gate.h"
#include "../../libs/aasrouter/rt_goaltable.h"
#include "../../libs/aasrouter/rt_mem.h"

#pragma pop_macro( "far" )
#pragma pop_macro( "near" )
#pragma pop_macro( "max" )
#pragma pop_macro( "min" )

// two macros of the runtime set the layout of its structs and the limits the files were baked with
static_assert( NL_PREFIX_CAP == 48, "the router files are baked with a patched-prefix limit of 48" );
static_assert( nl::MOUTH_LIMIT == 8 && nl::THROUGH_LIMIT == 40, "the router files are baked with these door limits" );

static idCVar aas_routerGoals( "aas_routerGoals", "32", CVAR_GAME | CVAR_INTEGER, "goal tables the baked router keeps per travel-flag set, 12 bytes per AAS area each; read when a map loads", 1, 4096 );

/*
===============================================================================

	idAASRouterProfile

	One set of router files beside an AAS file, for one travel-flag mask: the map, the files, and the
	runtime built from them. The runtime keeps references into the members above it, so the order of the
	members is the order they are made in and the reverse of the order they go in. Made with new in
	LoadProfile, never copied or moved, deleted in idAASRouter::Shutdown.

===============================================================================
*/
class idAASRouterProfile
{
public:
	idAASRouterProfile() : mask( 0 ), label( NULL ), active( false ), warned( false ), numWide( 0 ), numNecks( 0 ), numAsks( 0 ), numFound( 0 )
	{
	}

	idStr							name;			// the AAS file's name, for what is printed
	int								mask;			// the travel flags the set serves
	const char* 					label;			// "walk" or "fly"
	bool							active;			// false: switched off, the stock routing serves the mask
	bool							warned;			// the switch-off was printed
	int								numWide;		// wide-door areas (the door layer's)
	int								numNecks;		// narrow necks (every other door of the door list)
	long long						numAsks;		// asks the set has answered (for aasStats)
	long long						numFound;		// ... of them with a route
	nl::Fixture4					map;			// the map; enabled says "an area of the mask", every door open; mutableArea says "a door of the set"
	std::vector<nl::U>				activeAreas;	// the areas the lists know (the mask's areas but the wide doors), ascending
	std::vector<nl::U>				index;			// per area: its place in activeAreas, NONE when it has none
	std::vector<nl::U>				origin;			// per edge: the time from the centre of its area to its start point
	std::vector<idReachability*> 	edgeReach;		// per edge: the file's reachability
	nl::Blob						listsBlob;		// the baked lists
	nl::Blob						layerBlob;		// the door layer, when the map has wide doors
	std::unique_ptr<nl::Runtime> 	rt;				// keeps references to listsBlob, map, activeAreas, index and origin
	std::unique_ptr<nl::Doors> 		doors;			// with a layer: keeps pointers into layerBlob and a reference to rt
	std::unique_ptr<nl::NeckDoors> 	neckDoors;		// with a layer: the gate in front of the doors
	std::unique_ptr<nl::NeckGate> 	gate;			// without a layer: the gate alone
	std::unique_ptr<nl::GoalTable> 	table;			// without a layer: the per-goal tables in front of the lists; keeps references to rt and gate
	// Without a layer: the necks that are shut; rt->closed points here. It goes before rt does, which is
	// safe because the runtime's destructor does not read through that pointer.
	std::vector<uint8_t>			closedArea;
	nl::GoalSide					goalSide;		// scratch of the asks across wide doors
	std::vector<nl::U>				doorChange;		// scratch of a door change: the doors of the set among the changed areas
};

/*
============
ReadBlob

  a packed file through the file system; false when it is not there or is no packed file
============
*/
static bool ReadBlob( const char* name, nl::Blob& b, ID_TIME_T* time = NULL )	//puncho - R2 router review: time, when given, gets the file's time
{
	byte* data = NULL;
	const int len = fileSystem->ReadFile( name, ( void** )&data, time );
	if( len <= 0 || data == NULL )
	{
		if( data != NULL )
		{
			fileSystem->FreeFile( data );
		}
		return false;
	}
	const bool ok = nl::BlobFromMemory( data, ( size_t )len, b );
	fileSystem->FreeFile( data );
	return ok;
}

/*
============
ReadDoorList

  The areas that doors shut and open, as the files were baked with them: a text file of area numbers and
  nothing else. The AAS file marks every portal area as one that could be shut, far more than doors ever
  shut, and the lists are baked for the real doors only. Without the file the set has no door.
  A number that names no such area is a refusal.
============
*/
static void ReadDoorList( const char* name, const idAASRouterMap& map, std::vector<uint8_t>& isDoor )
{
	char* text = NULL;
	const int len = fileSystem->ReadFile( name, ( void** )&text, NULL );
	if( len <= 0 || text == NULL )
	{
		if( text != NULL )
		{
			fileSystem->FreeFile( text );
		}
		return;
	}
	bool bad = false;
	for( int i = 0; i < len && !bad; )
	{
		if( text[i] < '0' || text[i] > '9' )
		{
			i++;
			continue;
		}
		long long a = 0;
		while( i < len && text[i] >= '0' && text[i] <= '9' && a < 2147483647 )
		{
			a = a * 10 + ( text[i] - '0' );
			i++;
		}
		if( a >= map.numAreas || !map.mutableArea[( int )a] || !map.enabled[( int )a] )
		{
			bad = true;
		}
		else
		{
			isDoor[( size_t )a] = 1;
		}
	}
	fileSystem->FreeFile( text );
	if( bad )
	{
		throw std::runtime_error( "the door list names an area that the map does not mark as one that can be shut" );
	}
}

/*
============
ChangeProfileAreas

  Areas have been shut (enabled false) or opened, all the same way: ONE door change. The wide doors
  among them go to the door layer in one call, which works its tables out once; the narrow necks go to
  the gate, which is brought up to date once. An area that is no door of the set cannot be followed
  when it shuts (the lists would still route across it): a refusal, which switches the set off.
  Opening such an area changes nothing.
============
*/
static void ChangeProfileAreas( idAASRouterProfile* p, const int* areas, int numAreas, bool enabled, nl::Meter& m )
{
	p->doorChange.clear();
	for( int i = 0; i < numAreas; i++ )
	{
		const int areaNum = areas[i];
		if( areaNum <= 0 || areaNum >= ( int )p->map.g.n )
		{
			continue;
		}
		if( !p->map.g.mutableArea[areaNum] )
		{
			if( !enabled && p->map.g.enabled[areaNum] )
			{
				throw std::runtime_error( "an area was shut that is no door of the router files" );
			}
			continue;
		}
		p->doorChange.push_back( ( nl::U )areaNum );
	}
	if( p->doorChange.empty() )
	{
		return;
	}
	if( p->neckDoors )
	{
		p->neckDoors->Set( p->doorChange, enabled, m );
		return;
	}
	bool necks = false;
	for( const nl::U areaNum : p->doorChange )
	{
		const nl::U k = p->gate->neckOf[areaNum];
		if( k != nl::NONE )
		{
			p->gate->shut[k] = enabled ? 0 : 1;
			p->closedArea[areaNum] = enabled ? 0 : 1;
			necks = true;
		}
	}
	if( necks )
	{
		p->gate->Update( m );
		if( p->table )
		{
			p->table->Clear();		// any door change may change any answer
		}
	}
}

/*
============
LoadProfile

  Loads one set from the files beside the AAS file: builds the map, reads the door list, the lists and the
  optional door layer, tells wide doors from narrow necks, and brings the doors to the file's present state.
  NULL when the map has no lists file for the mask (nothing is said), or when anything fails, a refusal of
  the runtime included (said once).
============
*/
static idAASRouterProfile* LoadProfile( idAASFile* file, const char* aasFileName, int mask, const char* label )
{
	const idStr listsName = va( "%s.%s.nlw4", aasFileName, label );
	if( fileSystem->GetFileLength( listsName.c_str() ) < 0 )
	{
		return NULL;
	}
	idAASRouterProfile* p = new( TAG_AAS ) idAASRouterProfile();
	p->name = aasFileName;
	p->mask = mask;
	p->label = label;
	try
	{
		idAASRouterMap built;
		idStr error;
		if( !built.Build( file, mask, error ) )
		{
			throw std::runtime_error( error.c_str() );
		}
		nl::FlatGraph& g = p->map.g;
		const nl::U numAreas = ( nl::U )built.numAreas;
		g.n = numAreas;
		g.edges.resize( ( size_t )built.edges.Num() );
		for( int e = 0; e < built.edges.Num(); e++ )
		{
			g.edges[e].from = ( nl::U )built.edges[e].from;
			g.edges[e].to = ( nl::U )built.edges[e].to;
			g.edges[e].w = built.edges[e].time;
			g.edges[e].ordinal = ( nl::U )built.edges[e].ordinal;
		}
		g.out.assign( built.out.Ptr(), built.out.Ptr() + built.out.Num() );
		g.in.assign( built.in.Ptr(), built.in.Ptr() + built.in.Num() );
		g.rev.assign( built.rev.Ptr(), built.rev.Ptr() + built.rev.Num() );
		g.enabled.assign( built.enabled.Ptr(), built.enabled.Ptr() + built.enabled.Num() );
		p->origin.assign( built.origin.Ptr(), built.origin.Ptr() + built.origin.Num() );
		p->edgeReach.assign( built.reach.Ptr(), built.reach.Ptr() + built.reach.Num() );

		// the doors of the set: the list the files were baked with
		g.mutableArea.assign( ( size_t )numAreas, 0 );
		ReadDoorList( va( "%s.%s.rtd", aasFileName, label ), built, g.mutableArea );

		ID_TIME_T listsTime = FILE_NOT_FOUND_TIMESTAMP;
		if( !ReadBlob( listsName.c_str(), p->listsBlob, &listsTime ) )
		{
			throw std::runtime_error( "cannot read the lists file" );
		}
		//puncho start - R2 router review: lists are baked from an AAS file, so they are never older than it.
		// The checks of the load prove that the lists agree with the map, not that they were made from it:
		// lists of an earlier AAS file can pass them and then deny routes that exist. Older lists are refused.
		// The test does not catch an AAS file that is the older of the two (put back or copied with its old
		// time, or found in another layer) beside lists of a later AAS file.
		// (A time of 0 or less is a file from a .resources container or an unknown time; then nothing is
		// compared. An entry of a .pk4 has its own time and is compared like a loose file.)
		const ID_TIME_T aasTime = file->GetFileTime();
		if( aasTime > 0 && listsTime > 0 && listsTime < aasTime )
		{
			throw std::runtime_error( "the lists are older than the AAS file; bake them again (rbdmap routerMap)" );
		}
		//puncho end - R2 router review
		const idStr layerName = va( "%s.%s.nld4", aasFileName, label );
		const bool hasLayer = fileSystem->GetFileLength( layerName.c_str() ) >= 0;
		if( hasLayer && !ReadBlob( layerName.c_str(), p->layerBlob ) )
		{
			throw std::runtime_error( "cannot read the door layer" );
		}

		// wide doors (the door layer names them) and narrow necks (every other door)
		std::vector<nl::U> wide;
		s3c::DoorClassification kinds;
		kinds.loop.assign( ( size_t )numAreas, 0 );
		kinds.forced.assign( ( size_t )numAreas, 0 );
		if( hasLayer )
		{
			const std::vector<uint8_t>& doorAreas = p->layerBlob.Get( "door.area" );
			if( doorAreas.size() % 10 )
			{
				throw std::runtime_error( "the door layer is cut off" );
			}
			wide.resize( doorAreas.size() / 10 );
			for( size_t i = 0; i < wide.size(); i++ )
			{
				const nl::U area = ( nl::U )nl::Rd( &doorAreas[i * 10], 4 );
				if( area >= numAreas || !g.mutableArea[area] )
				{
					throw std::runtime_error( "the door layer names an area that the door list does not" );
				}
				wide[i] = area;
				kinds.loop[area] = 1;
			}
		}
		for( nl::U a = 0; a < numAreas; a++ )
		{
			kinds.loopCount += kinds.loop[a];
			if( g.mutableArea[a] && !kinds.loop[a] )
			{
				kinds.forced[a] = 1;
				kinds.forcedCount++;
			}
		}
		p->numWide = ( int )wide.size();
		p->numNecks = ( int )kinds.forcedCount;

		// the areas the lists know: the mask's areas but the wide doors
		p->index.assign( ( size_t )numAreas, nl::NONE );
		for( nl::U a = 0; a < numAreas; a++ )
		{
			if( g.enabled[a] && !kinds.loop[a] )
			{
				p->index[a] = ( nl::U )p->activeAreas.size();
				p->activeAreas.push_back( a );
			}
		}

		p->rt.reset( new( TAG_AAS ) nl::Runtime( p->listsBlob, p->map, p->activeAreas, p->index, p->origin ) );

		nl::Meter m;
		if( hasLayer )
		{
			p->doors.reset( new( TAG_AAS ) nl::Doors( p->layerBlob, p->listsBlob, *p->rt ) );
			p->neckDoors.reset( new( TAG_AAS ) nl::NeckDoors( *p->rt, *p->doors, p->map, kinds ) );
			p->goalSide = nl::GoalSide( p->doors->D );
			p->neckDoors->Set( wide, true, m );		// the layer starts with every wide door shut
		}
		else
		{
			p->gate.reset( new( TAG_AAS ) nl::NeckGate( p->map, kinds ) );
			p->closedArea.assign( ( size_t )numAreas, 0 );
			p->rt->closed = p->closedArea.data();
			p->table.reset( new( TAG_AAS ) nl::GoalTable( *p->rt, *p->gate, ( nl::U )aas_routerGoals.GetInteger() ) );
		}

		// the file's present state: every area that is shut now, as one door change (an area that is no
		// door of the set is a refusal)
		idList<int, TAG_AAS> shut;
		for( int a = 1; a < built.numAreas; a++ )
		{
			if( file->GetArea( a ).travelFlags & TFL_INVALID )
			{
				shut.Append( a );
			}
		}
		ChangeProfileAreas( p, shut.Ptr(), shut.Num(), false, m );

		p->active = true;
		common->Printf( "AAS router: %s (%s): %d areas, %d reaches, lists %d bytes, door layer %d bytes, %d wide door areas, %d narrow necks\n",
						aasFileName, label, ( int )numAreas, ( int )g.edges.size(), ( int )p->listsBlob.Bytes(), ( int )p->layerBlob.Bytes(), p->numWide, p->numNecks );
		if( p->table )
		{
			common->Printf( "AAS router: %s (%s): %d goal tables, %d KB\n", aasFileName, label, ( int )p->table->slots.size(), ( int )( p->table->Bytes() >> 10 ) );
		}
	}
	catch( const std::exception& e )
	{
		common->Warning( "AAS router: %s (%s): %s", aasFileName, label, e.what() );
		delete p;
		return NULL;
	}
	return p;
}

/*
============
SwitchOff

  a refusal: the set serves no more asks and follows no more doors; said once
============
*/
static void SwitchOff( idAASRouterProfile* p, const char* why )
{
	p->active = false;
	if( !p->warned )
	{
		p->warned = true;
		common->Warning( "AAS router: %s (%s) switched off: %s", p->name.c_str(), p->label, why );
	}
}

/*
============
idAASRouter::idAASRouter
============
*/
idAASRouter::idAASRouter()
{
	profiles[0] = NULL;
	profiles[1] = NULL;
	changedEnabled = false;
	keeping = 0;
}

/*
============
idAASRouter::~idAASRouter
============
*/
idAASRouter::~idAASRouter()
{
	Shutdown();
}

/*
============
idAASRouter::Init
============
*/
int idAASRouter::Init( idAASFile* file, const char* aasFileName )
{
	Shutdown();
	profiles[0] = LoadProfile( file, aasFileName, TFL_WALK | TFL_AIR, "walk" );
	profiles[1] = LoadProfile( file, aasFileName, TFL_WALK | TFL_AIR | TFL_FLY, "fly" );
	return ( profiles[0] != NULL ? 1 : 0 ) + ( profiles[1] != NULL ? 1 : 0 );
}

/*
============
idAASRouter::Shutdown
============
*/
void idAASRouter::Shutdown()
{
	delete profiles[0];
	profiles[0] = NULL;
	delete profiles[1];
	profiles[1] = NULL;
	changed.Clear();
	keeping = 0;
}

/*
============
idAASRouter::Route

  One ask. The narrow necks (the gate) and the wide doors (the layer) say whether the pair is connected
  under the current doors; the lists give the route. A refusal switches the set off and the stock routing
  answers this ask and every later one.
============
*/
bool idAASRouter::Route( int areaNum, int goalAreaNum, int travelFlags, bool& found, int& travelTime, idReachability** reach )
{
	idAASRouterProfile* p = NULL;
	for( int i = 0; i < 2; i++ )
	{
		if( profiles[i] != NULL && profiles[i]->mask == travelFlags && profiles[i]->active )
		{
			p = profiles[i];
			break;
		}
	}
	if( p == NULL )
	{
		return false;
	}
	found = false;
	travelTime = 0;
	*reach = NULL;
	if( areaNum <= 0 || areaNum >= ( int )p->map.g.n || goalAreaNum <= 0 || goalAreaNum >= ( int )p->map.g.n || areaNum == goalAreaNum )
	{
		found = ( areaNum == goalAreaNum );
		p->numAsks++;
		p->numFound += found ? 1 : 0;
		return true;
	}
	try
	{
		nl::Meter m;
		bool ok = false;
		nl::U first = nl::NONE;
		nl::W time = 0;
		if( p->neckDoors )
		{
			const nl::DoorRoute a = p->neckDoors->Ask( ( nl::U )areaNum, ( nl::U )goalAreaNum, p->goalSide, m );
			ok = a.found;
			first = a.first;
			time = a.time;
		}
		else
		{
			// The game asks again at every area and every tic. Asked afresh each time the lists may loop: only the
			// route that one ask fixes is proven. The goal's table keeps, per area, the reach of a route that was
			// written out whole, so a walk that asks at every area follows written routes to the goal.
			ok = p->table->Ask( ( nl::U )areaNum, ( nl::U )goalAreaNum, first, time, m );
		}
		if( ok )
		{
			if( first == nl::NONE || first >= p->edgeReach.size() )
			{
				throw std::runtime_error( "a route without its first reach" );
			}
			*reach = p->edgeReach[first];
			travelTime = time > 2147483647 ? 2147483647 : ( int )time;
			found = true;
			p->numFound++;
		}
		p->numAsks++;
		return true;
	}
	catch( const std::exception& e )
	{
		SwitchOff( p, e.what() );
		found = false;
		travelTime = 0;
		*reach = NULL;
		return false;
	}
}

/*
============
idAASRouter::PrintStats

  per loaded set: whether it is on, and the asks it has answered since it was loaded
============
*/
void idAASRouter::PrintStats() const
{
	for( int i = 0; i < 2; i++ )
	{
		const idAASRouterProfile* p = profiles[i];
		if( p == NULL )
		{
			continue;
		}
		common->Printf( "AAS router: %s (%s): %s, %lld asks answered, %lld of them with a route\n",
						p->name.c_str(), p->label, p->active ? "on" : "switched off", p->numAsks, p->numFound );
		if( p->table )
		{
			common->Printf( "AAS router: %s (%s): goal tables: %lld new pairs, %lld goals replaced, %lld times voided\n",
							p->name.c_str(), p->label, ( long long )p->table->misses, ( long long )p->table->evictions, ( long long )p->table->clears );
		}
	}
}

/*
============
idAASRouter::BeginAreaChanges
============
*/
void idAASRouter::BeginAreaChanges()
{
	keeping++;
}

/*
============
idAASRouter::SetAreaEnabled
============
*/
void idAASRouter::SetAreaEnabled( int areaNum, bool enabled )
{
	if( changed.Num() != 0 && changedEnabled != enabled )
	{
		MakeAreaChanges();		// one door change shuts or opens, not both
	}
	changedEnabled = enabled;
	changed.Append( areaNum );
	if( keeping == 0 )
	{
		MakeAreaChanges();
	}
}

/*
============
idAASRouter::EndAreaChanges
============
*/
void idAASRouter::EndAreaChanges()
{
	if( keeping > 0 )
	{
		keeping--;
	}
	if( keeping == 0 )
	{
		MakeAreaChanges();
	}
}

/*
============
idAASRouter::MakeAreaChanges

  the kept areas as one door change, in every set that is loaded and not switched off
============
*/
void idAASRouter::MakeAreaChanges()
{
	if( changed.Num() == 0 )
	{
		return;
	}
	for( int i = 0; i < 2; i++ )
	{
		idAASRouterProfile* p = profiles[i];
		if( p == NULL || !p->active )
		{
			continue;
		}
		try
		{
			nl::Meter m;
			ChangeProfileAreas( p, changed.Ptr(), changed.Num(), changedEnabled, m );
		}
		catch( const std::exception& e )
		{
			SwitchOff( p, e.what() );
		}
	}
	changed.SetNum( 0 );
}
