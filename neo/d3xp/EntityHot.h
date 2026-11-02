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

#ifndef __ENTITYHOT_H__
#define __ENTITYHOT_H__

#include "../idlib/CellId.h"	// C1's cell ids for the cell group (Lib.h includes it first)

/*
===============================================================================

	The hot table (P2b.2.1a, P2b.2.1b; plans/map-graph/stages/S2b-2-entity-tables.md K3, K4, K5, K6)

	gameLocal.entityHot holds a row per entity number, as gameLocal.entities[] does, in fixed arrays: nothing is
	allocated, saved or replicated, and every byte is derived. RegisterEntity fills a row when it gives an entity its
	number and UnregisterEntity resets it; a write for ENTITYNUM_NONE (before RegisterEntity or after
	UnregisterEntity) is dropped and counted (droppedWrites).

	think	thinkFlags, timeGroup and cinematic, and the class bits. idEntity's three members are private, and its setters
			(SetThinkFlags, SetTimeGroup, SetCinematic) write the member and the column together, so the two agree
			(BOTH): idEntity's readers read the member, the table's passes read the column. The constructor and Restore
			write the members alone; registration copies the constructor's values into the row, SyncAll the restored
			ones.
			With g_entityTable and g_entityPassDormancy latched (dormancyOn), RunEntityThink runs a HOT_DORMANCY row's
			CheckDormant itself, at the moment its Think would run it first: a sleeper's Think is not called (sleepersSkipped),
			and an awake row's Think skips its own test, since dormancyCheckedFor names it (P2b.2.2b-i, K7).
	spatial	the physics origin and absolute bounds as the spatial pass left them at the end of a tic, for passes outside
			the think walk; valid until the next think walk, so a reader inside a walk reads physics. Kept only while a
			consumer is registered: the registration refreshes every live row once, then each pass refreshes the rows
			ClearPVSAreas marked (MarkMoved) and the active rows with TH_PHYSICS.
	mirror	the active list as entity numbers in the list's order (activeOrder, -1 a tombstone) and its inverse
			(activeIndex), maintained only while g_entityTable is latched 1 or g_entityTableVerify is set: an append
			tombstones the entity's old entry first, a full array compacts in place, and a sort, a prune or the latch's
			edges rebuild it from the list. The rule is plans/map-graph/math-2b/S2b-2/core/ActiveMirror.cpp, frozen.
			With g_entityTable and g_entityPassWalks latched (walksOn), RunFrame's think walks, RunTimeGroup2 and
			RunSharedThink go over it instead of the list (P2b.2.2a): the same entities in the same order, each row's
			bits read before its entity.
	cell	the cell the entity is in and the cell it spawned from (C1's ids), its spawn ordinal there, and cellFlags.
			Registration, unregistration and Clear write CELL_ID_NONE, CELL_ID_NONE, CELL_ORDINAL_NONE, 0, and no stage
			2 code writes a row after: stage 4 calls SetHome at a map spawn and SetCell for the rows that move, stage
			7's store owns CELL_FLAG_ELSEWHERE. Machine-local and never saved; SyncAll leaves it alone, so after a load
			a row holds its registration values until stage 4's load path sets them.

	The switches are latched per map: Clear turns them off, and the first RunFrame latches them from off, so a mirror
	that is on is rebuilt and a consumer refreshes every row on that tic. g_entityTable 0 walks the active list as
	stock does, bit for bit (C9). Game thread only: inside RunFrame, and the snapshot writer between frames.

===============================================================================
*/

class idEntity;

// hotThink_t::bits. The class bits come from idEntityHot::ClassBits; the last two mirror cinematic and timeGroup
enum hotBits_t
{
	HOT_DORMANCY	= 0x01,		// a Think that opens with CheckDormant: idAI, idFuncSmoke, idPhantomObjects, idMover_Periodic
	HOT_SHARED		= 0x02,		// the class overrides SharedThink (idTypeInfo::sharedThink)
	HOT_PHASED		= 0x04,		// idAI: P2b.2.6's phase block
	HOT_SKIP		= 0x08,		// MP-E1's tiers; never set here
	HOT_CINEMATIC	= 0x10,		// idEntity::cinematic
	HOT_TIMEGROUP2	= 0x20		// idEntity::timeGroup is TIME_GROUP2
};
const int HOT_BIT_COUNT = 6;

// TH_* in a byte: EntityPasses.cpp asserts it, since Entity.h comes after this file
struct hotThink_t
{
	byte					thinkFlags;		// TH_*
	byte					bits;			// HOT_*
	byte					tier;			// MP-E1's; 0
	byte					pad;
};
compile_time_assert( sizeof( hotThink_t ) == 4 );

struct hotSpatial_t
{
	idVec3					origin;			// GetPhysics()->GetOrigin()
	idBounds				absBounds;		// GetPhysics()->GetAbsBounds()
	int						tic;			// gameLocal.framenum of the refresh
};
compile_time_assert( sizeof( hotSpatial_t ) == 40 );

// hotCell_t::cellFlags
enum hotCellFlags_t
{
	CELL_FLAG_ELSEWHERE		= 0x01		// stage 7's store, its only writer (S2b-7)
};
const int CELL_ORDINAL_NONE = 0xFFFF;	// cellOrdinal until SetHome

struct hotCell_t
{
	uint32					cellId;			// the cell the entity is in; after registration SetCell is its only writer
	uint32					homeCellId;		// the cell it spawned from (SetHome)
	uint16					cellOrdinal;	// its spawn ordinal in homeCellId (SetHome)
	byte					cellFlags;		// CELL_FLAG_*
	byte					pad;
};
compile_time_assert( sizeof( hotCell_t ) == 12 );

class idEntityHot
{
public:
	hotThink_t				think[MAX_GENTITIES];
	hotSpatial_t			spatial[MAX_GENTITIES];
	byte					moved[MAX_GENTITIES];				// 1: marked for the next spatial pass
	int						activeOrder[2 * MAX_GENTITIES];		// entity numbers in the active list's order, -1 a tombstone
	int						activeIndex[MAX_GENTITIES];			// the slot of n in activeOrder, -1 none

	// listEntityTable's clocks and counters, reset by Clear
	double					spatialTicks;						// the spatial passes' clock ticks (Sys_GetClockTicks)
	double					spatialFullTicks;					// the last registration's full refresh
	int						activeNum;							// slots in use, tombstones included
	int						activeCursor;						// a walk's slot (P2b.2.2a), -1 outside a walk
	int						dormancyCheckedFor;					// the row whose CheckDormant RunEntityThink has just run, for its Think's guard (P2b.2.2b-i), -1 otherwise
	int						spatialConsumers;
	int						droppedWrites;
	int						activeRebuilds;
	int						activeTombstones;
	int						activeCompactions;
	int						hotMismatch;						// g_entityTableVerify's differences; the first 8 are printed
	int						spatialPasses;						// passes that ran (a consumer was registered)
	int						spatialRows;						// rows those passes refreshed
	int						spatialFullRows;					// rows of the last registration's full refresh
	int						walkTG2Thinks;						// RunTimeGroup2's thinks over the mirror (P2b.2.2a)
	int						walkSharedCalls;					// RunSharedThink's calls over the mirror: HOT_SHARED rows only
	int						sleepersSkipped;					// RunEntityThink's lifted tests that found the row dormant: its Think was not called (P2b.2.2b-i)
	int						dormancyUnlifted;					// CheckDormant calls, lift latched, from a row without HOT_DORMANCY: a class ClassBits misses

	// the switches as LatchTic read them at the start of the tic; per map (Clear), never saved
	bool					tableOn;
	bool					verifyOn;
	bool					activeMaintained;					// tableOn || verifyOn: the mirror follows the list
	bool					walksOn;							// tableOn && g_entityPassWalks: the walks go over the mirror (P2b.2.2a)
	bool					dormancyOn;							// tableOn && g_entityPassDormancy: RunEntityThink tests a HOT_DORMANCY row's dormancy (P2b.2.2b-i)
	// pads sizeof to a multiple of 64 (asserted below), not the address: gameLocal is 32-byte aligned, so entityHot, the
	// last member of idGameLocal, and every array in it start 32 bytes into a cache line
	byte					pad[43];

	// after the tail, so that adding it moved no member above
	hotCell_t				cell[MAX_GENTITIES];

	void					Clear();
	void					RegisterRow( const idEntity* ent );
	void					UnregisterRow( int entityNumber );
	void					SyncRow( const idEntity* ent );
	void					SyncAll();

	// the column half of idEntity's setters: called only from them, never alone
	void					SetThinkFlags( int entityNumber, int flags );
	void					SetTimeGroup( int entityNumber, int timeGroup );
	void					SetCinematic( int entityNumber, bool cinematic );
	void					MarkMoved( int entityNumber );

	// the cell group's writers, for stages 4 and 7; nothing in stage 2 calls them
	void					SetCell( int entityNumber, uint32 cellId );
	void					SetHome( int entityNumber, uint32 homeCellId, int cellOrdinal );

	void					LatchTic( bool table, bool verify, bool walks, bool dormancy );
	void					SpatialPass( int numEntities );
	void					Verify();

	// the frozen core (math-2b/S2b-2/core/ActiveMirror.cpp) as members: maxEntities is MAX_GENTITIES, the listOrder of
	// ActiveRebuild is gameLocal.activeEntities and the restored rows of SyncAll are gameLocal.entities
	void					ActiveTombstone( int entityNumber );
	bool					ActiveAppend( int entityNumber );
	void					ActiveRebuild();

private:
	bool					ResetRow( int entityNumber );
	bool					SetThink( int entityNumber, const hotThink_t& value );
	void					ActiveClear();
	int						ActiveCompact();
	static bool				ActiveLatchNeedsRebuild( bool tableWasOn, bool maintenanceWasOn, bool tableOn, bool verifyOn );
	static int				ClassBits( const idTypeInfo& type );
	static hotThink_t		ThinkRow( const idEntity* ent );
	bool					RowWritable( int entityNumber );
	bool					RefreshSpatial( int entityNumber );
	void					MismatchLine( int entityNumber, const char* what, int table, int entity );
};
compile_time_assert( ( sizeof( idEntityHot ) & 63 ) == 0 );
compile_time_assert( MAX_GENTITIES % 8 == 0 );	// SpatialPass tests the marks eight at a time

// every column write's rule: ENTITYNUM_NONE and anything outside the table is counted, never written
ID_INLINE bool idEntityHot::RowWritable( int entityNumber )
{
	if( entityNumber < 0 || entityNumber >= ENTITYNUM_NONE )
	{
		droppedWrites++;
		return false;
	}
	return true;
}

ID_INLINE void idEntityHot::SetThinkFlags( int entityNumber, int flags )
{
	if( RowWritable( entityNumber ) )
	{
		think[entityNumber].thinkFlags = ( byte )flags;
	}
}

ID_INLINE void idEntityHot::SetTimeGroup( int entityNumber, int timeGroup )
{
	if( RowWritable( entityNumber ) )
	{
		think[entityNumber].bits = ( think[entityNumber].bits & ~HOT_TIMEGROUP2 ) | ( timeGroup == TIME_GROUP2 ? HOT_TIMEGROUP2 : 0 );
	}
}

ID_INLINE void idEntityHot::SetCinematic( int entityNumber, bool cinematic )
{
	if( RowWritable( entityNumber ) )
	{
		think[entityNumber].bits = ( think[entityNumber].bits & ~HOT_CINEMATIC ) | ( cinematic ? HOT_CINEMATIC : 0 );
	}
}

// ClearPVSAreas' mark: with no consumer the spatial rows are not kept, and this is one test
ID_INLINE void idEntityHot::MarkMoved( int entityNumber )
{
	if( spatialConsumers <= 0 )
	{
		return;
	}
	if( RowWritable( entityNumber ) )
	{
		moved[entityNumber] = 1;
	}
}

#endif /* !__ENTITYHOT_H__ */
