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

/*
===============================================================================

	The hot table (P2b.2.1a; plans/map-graph/stages/S2b-2-entity-tables.md K3, K5, K6)

	gameLocal.entityHot holds a row per entity number, as gameLocal.entities[] does, in fixed arrays: nothing is
	allocated, saved or replicated, and every byte is derived. RegisterEntity fills a row when it gives an entity its
	number and UnregisterEntity resets it; a write for ENTITYNUM_NONE, an entity before the one or after the other, is
	dropped and counted (droppedWrites).

	think	thinkFlags, timeGroup and cinematic, and the class bits. idEntity's three members are private and only their
			setters (SetThinkFlags, SetTimeGroup, SetCinematic) write them, the column with the member, so the column is
			authoritative and the member a copy (BOTH). The constructor and Restore set the members alone: registration
			copies the constructor's values into the row, SyncAll the restored ones.
	spatial	the physics origin and absolute bounds as the spatial pass left them at the end of a tic, for passes outside
			the think walk; valid until the next think walk, so a reader inside a walk reads physics. Kept only while a
			consumer is registered: the registration refreshes every live row once, then each pass refreshes the rows
			ClearPVSAreas marked (MarkMoved) and the active rows with TH_PHYSICS.
	mirror	the active list as entity numbers in the list's order (activeOrder, -1 a tombstone) and its inverse
			(activeIndex), maintained only while g_entityTable is latched 1 or g_entityTableVerify is set: an append
			tombstones the entity's old entry first, a full array compacts in place, and a sort, a prune or a restore
			rebuilds it from the list. The rule is plans/map-graph/math-2b/S2b-2/core/ActiveMirror.cpp, frozen.

	g_entityTable 0 runs today's walks over the same storage, bit for bit (C9). Game thread only: inside RunFrame, and
	the snapshot writer between frames.

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

class idEntityHot
{
public:
	hotThink_t				think[MAX_GENTITIES];
	hotSpatial_t			spatial[MAX_GENTITIES];
	byte					moved[MAX_GENTITIES];				// 1: marked for the next spatial pass
	int						activeOrder[2 * MAX_GENTITIES];		// entity numbers in the active list's order, -1 a tombstone
	int						activeIndex[MAX_GENTITIES];			// the slot of n in activeOrder, -1 none
	int						activeNum;							// slots in use, tombstones included
	int						activeCursor;						// a walk's slot (P2b.2.2a), -1 outside a walk

	// the switches as LatchTic read them at the start of the tic; derived, never saved
	bool					tableOn;
	bool					verifyOn;
	bool					activeMaintained;					// tableOn || verifyOn: the mirror follows the list
	int						spatialConsumers;

	// listEntityTable's counters, reset by Clear
	int						droppedWrites;
	int						activeRebuilds;
	int						activeTombstones;
	int						activeCompactions;
	int						hotMismatch;						// g_entityTableVerify's differences; the first 8 are printed
	int						spatialPasses;						// passes that ran (a consumer was registered)
	int						spatialRows;						// rows those passes refreshed
	double					spatialTicks;						// their clock ticks (Sys_GetClockTicks)
	int						spatialFullRows;					// rows of the last registration's full refresh
	double					spatialFullTicks;

	void					Clear();
	void					RegisterRow( const idEntity* ent );
	void					UnregisterRow( int entityNumber );
	void					SyncRow( const idEntity* ent );
	void					SyncAll();
	static int				ClassBits( const idTypeInfo& type );

	// the BOTH setters' column writes (idEntity::SetThinkFlags, SetTimeGroup, SetCinematic)
	void					SetThinkFlags( int entityNumber, int flags );
	void					SetTimeGroup( int entityNumber, int timeGroup );
	void					SetCinematic( int entityNumber, bool cinematic );
	void					MarkMoved( int entityNumber );

	void					LatchTic( bool table, bool verify );
	void					SpatialPass();
	void					Verify();

	// the frozen core (math-2b/S2b-2/core/ActiveMirror.cpp) as members: maxEntities is MAX_GENTITIES, the listOrder of
	// ActiveRebuild is gameLocal.activeEntities and the restored rows of SyncAll are gameLocal.entities
	bool					ResetRow( int entityNumber );
	bool					SetThink( int entityNumber, const hotThink_t& value );
	void					ActiveClear();
	void					ActiveTombstone( int entityNumber );
	int						ActiveCompact();
	bool					ActiveAppend( int entityNumber );
	void					ActiveRebuild();
	static bool				ActiveLatchNeedsRebuild( bool tableWasOn, bool maintenanceWasOn, bool tableOn, bool verifyOn );

private:
	bool					RowWritable( int entityNumber );
	bool					RefreshSpatial( int entityNumber );
	void					MismatchLine( int entityNumber, const char* what, int table, int entity );
};

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
