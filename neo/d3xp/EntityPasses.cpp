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

#include "Game_local.h"

// P2b.2.1a, the hot table's rows, its passes and the active mirror (EntityHot.h). The mirror's functions and the row
// functions are the frozen core plans/map-graph/math-2b/S2b-2/core/ActiveMirror.cpp as members, logic and constants
// unchanged; the invariants M1-M6 are its check's (check/mirror_check.cpp)

static const int MAX_MISMATCH_LINES = 8;	// g_entityTableVerify prints this many differences a map, then counts

compile_time_assert( TH_UPDATEPARTICLES < 0x100 );	// hotThink_t::thinkFlags holds TH_* in a byte

/*
================
idEntityHot::ThinkRow

the think row an entity's members and class make: what registration, SyncRow and SyncAll write and what
g_entityTableVerify expects
================
*/
hotThink_t idEntityHot::ThinkRow( const idEntity* ent )
{
	hotThink_t row;
	row.thinkFlags = ( byte )ent->GetThinkFlags();
	row.bits = ( byte )( ClassBits( *ent->GetType() ) | ( ent->IsCinematic() ? HOT_CINEMATIC : 0 ) | ( ent->GetTimeGroup() == TIME_GROUP2 ? HOT_TIMEGROUP2 : 0 ) );
	row.tier = 0;
	row.pad = 0;
	return row;
}

/*
================
idEntityHot::Clear

idGameLocal::Clear and LoadMap: every row, the mirror, the counters and the switches. The latch is per map: nothing
maintains the mirror or marks a row through a load (a client never latches at all), and the map's first RunFrame
latches from off, so a switch that is on rebuilds the mirror and refreshes every spatial row on that tic
================
*/
void idEntityHot::Clear()
{
	memset( think, 0, sizeof( think ) );
	memset( spatial, 0, sizeof( spatial ) );
	memset( moved, 0, sizeof( moved ) );
	ActiveClear();

	tableOn = false;
	verifyOn = false;
	activeMaintained = false;
	spatialConsumers = 0;

	droppedWrites = 0;
	activeRebuilds = 0;
	activeTombstones = 0;
	activeCompactions = 0;
	hotMismatch = 0;
	spatialPasses = 0;
	spatialRows = 0;
	spatialTicks = 0.0;
	spatialFullRows = 0;
	spatialFullTicks = 0.0;
}

/*
================
idEntityHot::ClassBits
================
*/
int idEntityHot::ClassBits( const idTypeInfo& type )
{
	int bits = 0;
	if( type.IsType( idAI::Type ) || type.IsType( idFuncSmoke::Type ) || type.IsType( idPhantomObjects::Type ) || type.IsType( idMover_Periodic::Type ) )
	{
		bits |= HOT_DORMANCY;
	}
	if( type.sharedThink )
	{
		bits |= HOT_SHARED;
	}
	if( type.IsType( idAI::Type ) )
	{
		bits |= HOT_PHASED;
	}
	return bits;
}

/*
================
idEntityHot::RegisterRow

RegisterEntity, once ent->entityNumber is set: the row starts from the constructor's members, which no setter could
write before the entity had a number
================
*/
void idEntityHot::RegisterRow( const idEntity* ent )
{
	if( ResetRow( ent->entityNumber ) )
	{
		SetThink( ent->entityNumber, ThinkRow( ent ) );
	}
}

/*
================
idEntityHot::UnregisterRow

UnregisterEntity, while entities[ n ] is still the entity, the last call of ~idEntity. The destructor tombstoned the
entry with activeNode.Remove, but a dying team master can rejoin the list after it: RemoveBinds unbinds a child, whose
rigid body activates (idPhysics_RigidBody::SetMaster), and BecomeActive activates the child's team master. The list
loses that node with the member; the mirror loses it here
================
*/
void idEntityHot::UnregisterRow( int entityNumber )
{
	if( activeMaintained )
	{
		ActiveTombstone( entityNumber );
	}
	ResetRow( entityNumber );
}

/*
================
idEntityHot::SyncRow

after each CallSpawn: the think row again from the spawned members, and the spatial row once physics exists
================
*/
void idEntityHot::SyncRow( const idEntity* ent )
{
	if( SetThink( ent->entityNumber, ThinkRow( ent ) ) && spatialConsumers > 0 )
	{
		RefreshSpatial( ent->entityNumber );
	}
}

/*
================
idEntityHot::SyncAll

after RestoreObjects, which read the think members directly: HotSyncAll over gameLocal.entities in entity order. The
think, spatial and moved rows only (P2b.2.1b's cell group keeps its registration values); a spatial row is the physics
now when a consumer is registered, else the cleared row. The load's Clear left the switches off, so no consumer is
registered here and the first RunFrame's latch refreshes the rows
================
*/
void idEntityHot::SyncAll()
{
	memset( think, 0, ENTITYNUM_NONE * sizeof( think[0] ) );
	memset( spatial, 0, ENTITYNUM_NONE * sizeof( spatial[0] ) );
	memset( moved, 0, ENTITYNUM_NONE * sizeof( moved[0] ) );
	for( int n = 0; n < MAX_GENTITIES; n++ )
	{
		const idEntity* ent = gameLocal.entities[ n ];
		if( ent == NULL )
		{
			continue;
		}
		if( SetThink( ent->entityNumber, ThinkRow( ent ) ) && spatialConsumers > 0 )
		{
			RefreshSpatial( ent->entityNumber );
		}
	}
}

/*
================
idEntityHot::LatchTic

RunFrame, before SetupPlayerPVS: the switches for the tic. The mirror is rebuilt from the list on the table's 0 -> 1
edge and on the maintenance false -> true edge (verify set while the table is 0), since nothing kept it while it was
off. g_entityTableVerify is the spatial rows' consumer in this stage: it registers on its 0 -> 1 edge,
which refreshes every live row once, and leaves on its 1 -> 0 edge
================
*/
void idEntityHot::LatchTic( bool table, bool verify )
{
	if( ActiveLatchNeedsRebuild( tableOn, activeMaintained, table, verify ) )
	{
		ActiveRebuild();
	}
	if( verify && !verifyOn )
	{
		spatialConsumers++;
		const double start = Sys_GetClockTicks();
		int rows = 0;
		for( int n = 0; n < ENTITYNUM_NONE; n++ )
		{
			if( RefreshSpatial( n ) )
			{
				rows++;
			}
		}
		spatialFullRows = rows;
		spatialFullTicks = Sys_GetClockTicks() - start;
	}
	else if( !verify && verifyOn )
	{
		spatialConsumers--;
	}
	tableOn = table;
	verifyOn = verify;
	activeMaintained = table || verify;
}

/*
================
idEntityHot::RefreshSpatial

a marked slot can be empty: an entity marked, then removed in the same tic, whose row went with its slot
================
*/
bool idEntityHot::RefreshSpatial( int entityNumber )
{
	const idEntity* ent = gameLocal.entities[ entityNumber ];
	if( ent == NULL )
	{
		return false;
	}
	const idPhysics* phys = ent->GetPhysics();
	hotSpatial_t& row = spatial[ entityNumber ];
	row.origin = phys->GetOrigin();
	row.absBounds = phys->GetAbsBounds();
	row.tic = gameLocal.framenum;
	return true;
}

/*
================
idEntityHot::SpatialPass

RunFrame, after the events and the map graph director, before GameVerify_EndTic: the tic-end spatial rows (K5). The
active rows that ran physics join the rows ClearPVSAreas marked, then every marked row is refreshed in entity order.
A consumer keeps the mirror maintained (the verify consumer is a switch of the latch), so the active rows are read
from activeOrder and the think rows, not from the entities. The marks are read over the slots in use: below
numEntities (gameLocal.num_entities), and the top range it never covers, where the world and multiplayer's
non-replicated entities live (RegisterEntity)
================
*/
void idEntityHot::SpatialPass( int numEntities )
{
	if( spatialConsumers <= 0 )
	{
		return;
	}
	assert( activeMaintained );
	const double start = Sys_GetClockTicks();
	for( int i = 0; i < activeNum; i++ )
	{
		const int n = activeOrder[ i ];
		if( n >= 0 && ( think[ n ].thinkFlags & TH_PHYSICS ) != 0 )
		{
			moved[ n ] = 1;
		}
	}
	// eight marks a test: a few dozen rows move in a tic
	const int low = Min( ( numEntities + 7 ) & ~7, MAX_GENTITIES );
	const int ranges[2][2] = { { 0, low }, { Max( low, ENTITYNUM_FIRST_NON_REPLICATED & ~7 ), MAX_GENTITIES } };
	int rows = 0;
	for( int r = 0; r < 2; r++ )
	{
		for( int w = ranges[ r ][ 0 ]; w < ranges[ r ][ 1 ]; w += 8 )
		{
			uint64 marks;
			memcpy( &marks, &moved[ w ], sizeof( marks ) );
			if( marks == 0 )
			{
				continue;
			}
			for( int n = w; n < w + 8; n++ )
			{
				if( moved[ n ] != 0 )
				{
					moved[ n ] = 0;
					if( RefreshSpatial( n ) )
					{
						rows++;
					}
				}
			}
		}
	}
	spatialPasses++;
	spatialRows += rows;
	spatialTicks += Sys_GetClockTicks() - start;
}

/*
================
idEntityHot::MismatchLine
================
*/
void idEntityHot::MismatchLine( int entityNumber, const char* what, int table, int entity )
{
	if( hotMismatch++ < MAX_MISMATCH_LINES )
	{
		const idEntity* ent = ( entityNumber >= 0 && entityNumber < MAX_GENTITIES ) ? gameLocal.entities[ entityNumber ] : NULL;
		gameLocal.Warning( "entity table: tic %d entity %d (%s) %s: table %d, entity %d", gameLocal.framenum, entityNumber, ent != NULL ? ent->GetName() : "none", what, table, entity );
	}
}

/*
================
idEntityHot::Verify

g_entityTableVerify, after the spatial pass: every think row against its entity's members and class (an empty slot's
row zero), every spatial row against physics, and the mirror against the list (M1, M2): its live entries are the
list's entities in the list's order, each at its activeIndex. Counts each difference into hotMismatch
================
*/
void idEntityHot::Verify()
{
	for( int n = 0; n < ENTITYNUM_NONE; n++ )
	{
		const idEntity* ent = gameLocal.entities[ n ];
		const hotThink_t& row = think[ n ];
		if( activeIndex[ n ] >= 0 && ( activeIndex[ n ] >= activeNum || activeOrder[ activeIndex[ n ] ] != n ) )
		{
			MismatchLine( n, "activeIndex", activeIndex[ n ], -1 );
		}
		if( ent == NULL )
		{
			if( row.thinkFlags != 0 || row.bits != 0 || row.tier != 0 )
			{
				MismatchLine( n, "empty slot's think row", row.thinkFlags | ( row.bits << 8 ) | ( row.tier << 16 ), 0 );
			}
			continue;
		}
		const hotThink_t want = ThinkRow( ent );
		if( row.thinkFlags != ent->GetThinkFlags() )
		{
			MismatchLine( n, "thinkFlags", row.thinkFlags, ent->GetThinkFlags() );
		}
		if( row.bits != want.bits )
		{
			MismatchLine( n, "bits", row.bits, want.bits );
		}
		if( row.tier != 0 )
		{
			MismatchLine( n, "tier", row.tier, 0 );
		}
		// the column holds TIME_GROUP2 as a bit: any third value would read as TIME_GROUP1
		if( ent->GetTimeGroup() != TIME_GROUP1 && ent->GetTimeGroup() != TIME_GROUP2 )
		{
			MismatchLine( n, "timeGroup", ( row.bits & HOT_TIMEGROUP2 ) != 0 ? TIME_GROUP2 : TIME_GROUP1, ent->GetTimeGroup() );
		}
		const idPhysics* phys = ent->GetPhysics();
		if( !spatial[ n ].origin.Compare( phys->GetOrigin() ) || !spatial[ n ].absBounds.Compare( phys->GetAbsBounds() ) )
		{
			MismatchLine( n, "spatial row (its tic, this tic)", spatial[ n ].tic, gameLocal.framenum );
		}
	}

	int slot = 0;
	int position = 0;
	for( const idEntity* ent = gameLocal.activeEntities.Next(); ent != NULL; ent = ent->activeNode.Next() )
	{
		while( slot < activeNum && activeOrder[ slot ] < 0 )
		{
			slot++;
		}
		const int n = ent->entityNumber;
		if( slot >= activeNum || activeOrder[ slot ] != n || activeIndex[ n ] != slot )
		{
			// past the first difference every position differs: one line
			MismatchLine( n, "active list position (mirror entry, list position)", slot < activeNum ? activeOrder[ slot ] : -1, position );
			return;
		}
		slot++;
		position++;
	}
	while( slot < activeNum && activeOrder[ slot ] < 0 )
	{
		slot++;
	}
	if( slot < activeNum )
	{
		MismatchLine( activeOrder[ slot ], "mirror entry past the list's end (slot, list length)", slot, position );
	}
}

/*
================
idEntityHot::ResetRow

a row's think, spatial and moved bytes to zero; refuses and counts ENTITYNUM_NONE (the core's HotResetRow; the cell
group is P2b.2.1b's)
================
*/
bool idEntityHot::ResetRow( int entityNumber )
{
	if( !RowWritable( entityNumber ) )
	{
		return false;
	}
	memset( &think[ entityNumber ], 0, sizeof( think[ entityNumber ] ) );
	memset( &spatial[ entityNumber ], 0, sizeof( spatial[ entityNumber ] ) );
	moved[ entityNumber ] = 0;
	return true;
}

/*
================
idEntityHot::SetThink

a row's think group, written whole; refuses and counts ENTITYNUM_NONE (the core's HotSetThink)
================
*/
bool idEntityHot::SetThink( int entityNumber, const hotThink_t& value )
{
	if( !RowWritable( entityNumber ) )
	{
		return false;
	}
	think[ entityNumber ] = value;
	return true;
}

/*
================
idEntityHot::ActiveClear
================
*/
void idEntityHot::ActiveClear()
{
	activeNum = 0;
	activeCursor = -1;
	memset( activeIndex, -1, sizeof( activeIndex ) );
}

/*
================
idEntityHot::ActiveTombstone

~idEntity, after activeNode.Remove, while the mirror is maintained
================
*/
void idEntityHot::ActiveTombstone( int entityNumber )
{
	if( entityNumber < 0 || entityNumber >= ENTITYNUM_NONE )
	{
		return;
	}

	const int position = activeIndex[ entityNumber ];
	if( position >= 0 )
	{
		activeOrder[ position ] = -1;
		activeIndex[ entityNumber ] = -1;
		activeTombstones++;
	}
}

/*
================
idEntityHot::ActiveCompact

stable; maps activeCursor to ( live entries in [0, cursor] ) - 1, so a walk's post-Think increment lands on the first
live entry after the old cursor whether or not the current one is still live. Returns the cursor
================
*/
int idEntityHot::ActiveCompact()
{
	int write = 0;
	int mappedCursor = -1;
	for( int read = 0; read < activeNum; read++ )
	{
		const int n = activeOrder[ read ];
		if( n < 0 )
		{
			continue;
		}
		if( read <= activeCursor )
		{
			mappedCursor++;
		}
		activeOrder[ write ] = n;
		activeIndex[ n ] = write++;
	}
	activeNum = write;
	activeCursor = mappedCursor;
	activeCompactions++;
	return mappedCursor;
}

/*
================
idEntityHot::ActiveAppend

BecomeActive, after AddToEnd, while the mirror is maintained. A full array compacts: it frees 2N - L slots for L live
entries, L < N, so the append never fails (M4); the compacting append can run deep inside a think, which is why the
cursor is a member
================
*/
bool idEntityHot::ActiveAppend( int entityNumber )
{
	if( entityNumber < 0 || entityNumber >= ENTITYNUM_NONE )
	{
		return false;
	}

	// AddToEnd moves a linked node; leaving its old entry live would duplicate it
	ActiveTombstone( entityNumber );
	if( activeNum == 2 * MAX_GENTITIES )
	{
		ActiveCompact();
	}
	activeOrder[ activeNum ] = entityNumber;
	activeIndex[ entityNumber ] = activeNum++;
	return true;
}

/*
================
idEntityHot::ActiveRebuild

the mirror from the list, in its order: on the latch's edges, inside each sort body that ran and inside the prune;
never after SortActiveEntityList, which runs every tic. A restore relinks the list past the mirror, but its load
cleared the latch, so the map's first latch rebuilds
================
*/
void idEntityHot::ActiveRebuild()
{
	ActiveClear();
	int i = 0;
	for( const idEntity* ent = gameLocal.activeEntities.Next(); ent != NULL; ent = ent->activeNode.Next() )
	{
		const int n = ent->entityNumber;
		activeOrder[ i ] = n;
		activeIndex[ n ] = i;
		i++;
	}
	activeNum = i;
	activeRebuilds++;
}

/*
================
idEntityHot::ActiveLatchNeedsRebuild

once per tic; a 0 -> 1 table edge rebuilds even when verify was already maintaining
================
*/
bool idEntityHot::ActiveLatchNeedsRebuild( bool tableWasOn, bool maintenanceWasOn, bool tableOn, bool verifyOn )
{
	return ( tableOn && !tableWasOn ) || ( ( tableOn || verifyOn ) && !maintenanceWasOn );
}
