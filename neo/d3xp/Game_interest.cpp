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

//puncho - stage R6 (plans/single-map/R6-interest.md); the contracts are in Game_interest.h

/*
================
idGameLocal::NetInterestLatch

InitFromNewMap, before the map's entities spawn: the switches a map is loaded under. Every machine of a game must read
the same values
================
*/
void idGameLocal::NetInterestLatch()
{
	memset( &netInterest, 0, sizeof( netInterest ) );
	// The three map-load switches are the host's. They are server info cvars, so they travel in the match's server
	// info, which every machine is given before its map spawns (idCommonLocal::ExecuteMapChange, SetServerInfo): a
	// client loads under the host's values whatever its own cvars say. A host that does not send them has them off
	netInterest.ordinals = common->IsMultiplayer() && serverInfo.GetBool( "net_mapOrdinals", "0" );
	netInterest.headroom = serverInfo.GetInt( "net_mapOrdinalHeadroom", "1024" );
	netInterest.mapOrdinalEnd = MAX_CLIENTS;
	netInterest.authoredSlot = -1;
	netInterest.statics = netInterest.ordinals && serverInfo.GetBool( "net_interestStatics", "0" );
	// R6.4: the host's instance of this map is number 1; a client takes the number of the first snapshot it reads
	netInterest.restartSerial = common->IsClient() ? 0 : 1;
	netInterest.restartSerialKnown = !common->IsClient();
	for( int c = 0; c < MAX_CLIENTS; c++ )
	{
		netInterest.peerOfClient[ c ] = -1;
	}
	// the managed classes (Game_interest.h); a player is an actor and an articulated figure, and never managed
	if( idClass::GetNumTypes() > NET_INTEREST_MAX_TYPES )
	{
		Error( "idGameLocal::NetInterestLatch: %d class types, the table holds %d", idClass::GetNumTypes(), NET_INTEREST_MAX_TYPES );
	}
	for( int t = 0; t < idClass::GetNumTypes(); t++ )
	{
		const idTypeInfo* type = idClass::GetType( t );
		const bool managed = type != NULL && !type->IsType( idPlayer::Type ) && !type->IsType( idItemTeam::Type )
							 && ( type->IsType( idActor::Type ) || type->IsType( idAFEntity_Base::Type ) || type->IsType( idMoveable::Type )
								  || type->IsType( idItem::Type ) || type->IsType( idProjectile::Type ) || type->IsType( idDebris::Type ) );
		netInterest.classManaged[ t ] = managed ? 1 : 0;
	}
}

/*
================
idGameLocal::NetInterestBeginMapSpawn

SpawnMapEntities, before the world spawns (a map load and every restart): the authored range is the map's, whatever
spawns, and whatever the world or a map entity spawns beside itself starts above it. A map that leaves the run-time
entities less than the headroom is refused before anything spawns
================
*/
void idGameLocal::NetInterestBeginMapSpawn( int numMapEntities )
{
	netInterest.authoredSlot = -1;
	memset( netInterest.authoredGen, 0, sizeof( netInterest.authoredGen ) );
	netInterest.baselineCreates = 0;
	netInterest.plainCreates = 0;
	netInterest.removedCreates = 0;
	netInterest.removedSet = 0;
	netInterest.removedApplied = 0;
	netInterest.revived = 0;
	netInterest.swept = 0;
	netInterest.eventsHeld = 0;
	netInterest.eventsDropped = 0;
	netInterest.mapOrdinalEnd = MAX_CLIENTS;
	if( !netInterest.ordinals )
	{
		return;
	}
	const int64 nonWorld = numMapEntities - 1;
	const int64 headroom = netInterest.headroom;
	const int64 limit = ENTITYNUM_FIRST_NON_REPLICATED;
	if( nonWorld < 0 || headroom < 0 || MAX_CLIENTS + nonWorld + headroom > limit )
	{
		Error( "map %s has %d non-world entities (total %d); a multiplayer map holds %d non-world plus %d headroom (GENTITYNUM_BITS %d)",
			   GetMapName(), ( int )nonWorld, numMapEntities, ( int )Max( ( int64 )0, limit - MAX_CLIENTS - headroom ), ( int )headroom, GENTITYNUM_BITS );
	}
	netInterest.mapOrdinalEnd = MAX_CLIENTS + ( int )nonWorld;
	firstFreeEntityIndex[0] = netInterest.mapOrdinalEnd;
	netInterest.fullSweep = true;	// a client: the map entities about to spawn are not yet anything a snapshot gave
}

/*
================
idGameLocal::NetInterestCheckSlot

RegisterEntity with the ordinal slots on, for a registration that names its slot: an authored slot is the map spawn's
and the snapshot reader's to fill, and no slot is filled over a live entity
================
*/
void idGameLocal::NetInterestCheckSlot( int slot, int authoredSlot ) const
{
	if( slot < 0 || slot >= ENTITYNUM_NONE )
	{
		Error( "entity slot %d is outside the table", slot );
	}
	if( slot >= MAX_CLIENTS && slot < netInterest.mapOrdinalEnd && slot != authoredSlot )
	{
		Error( "entity slot %d belongs to map ordinal %d; a spawn_entnum must lie in [%d, %d)", slot, slot - MAX_CLIENTS + 1, netInterest.mapOrdinalEnd, ENTITYNUM_MAX_NORMAL );
	}
	if( entities[ slot ] != NULL )
	{
		Error( "entity slot %d is taken by '%s'", slot, entities[ slot ]->name.c_str() );
	}
}

/*
================
idGameLocal::NetInterestAuthored

true while the slot holds the entity the map spawn put there
================
*/
bool idGameLocal::NetInterestAuthored( int slot ) const
{
	return netInterest.ordinals && slot >= MAX_CLIENTS && slot < netInterest.mapOrdinalEnd
		   && netInterest.authoredGen[ slot ] != 0 && spawnIds[ slot ] == netInterest.authoredGen[ slot ];
}

/*
================
idGameLocal::NetInterestEntityRemoved

UnregisterEntity, before the slot is cleared. The host alone records that play removed an authored entity; a client's
deletes (its own or a snapshot's) say nothing about the map. MapClear's deletes set the bits too, and the next map
spawn's idWorldStore::BeginSpawn clears them before a snapshot can carry them
================
*/
void idGameLocal::NetInterestEntityRemoved( const idEntity* ent )
{
	if( common->IsClient() || !NetInterestAuthored( ent->entityNumber ) )
	{
		return;
	}
	const int ordinal = ent->entityNumber - MAX_CLIENTS + 1;
	worldStore.removed[ ordinal >> 3 ] |= 1 << ( ordinal & 7 );
	netInterest.removedSet++;
}

/*
================
idGameLocal::NetInterestReadRemoved

a client with the ordinal slots, for one SNAP_MAP_REMOVED chunk of a snapshot (500 bytes of bits from firstByte on, the
last chunk shorter; an object of size 0 is a chunk the host holds no bit in). This machine's bits become the host's.
For a bit that is new, this machine's copy of that map entity goes, if the slot still holds the map's own. A bit
that cleared is a map entity the host has again (it clears bits at a map load only): NetInterestRevive
================
*/
void idGameLocal::NetInterestReadRemoved( int firstByte, const idBitMsg& msg )
{
	const int bytes = Min( 500, ( int )sizeof( worldStore.removed ) - firstByte );
	const bool cleared = msg.GetSize() == 0;
	if( !cleared && msg.GetSize() != bytes )
	{
		Error( "Invalid removed-map snapshot" );
	}
	for( int i = 0; i < bytes; i++ )
	{
		const byte bits = cleared ? 0 : ( byte )msg.ReadByte();
		const byte was = worldStore.removed[ firstByte + i ];
		if( bits == was )
		{
			continue;
		}
		worldStore.removed[ firstByte + i ] = bits;
		for( int bit = 0; bit < 8; bit++ )
		{
			if( ( ( bits ^ was ) & ( 1 << bit ) ) == 0 )
			{
				continue;
			}
			const int ordinal = ( firstByte + i ) * 8 + bit;
			const int slot = MAX_CLIENTS + ordinal - 1;
			if( ( bits & ( 1 << bit ) ) == 0 )
			{
				NetInterestRevive( slot );
				continue;
			}
			if( ordinal < 1 || slot >= netInterest.mapOrdinalEnd )
			{
				Error( "Invalid removed-map snapshot: ordinal %d of a map with %d", ordinal, netInterest.mapOrdinalEnd - MAX_CLIENTS + 1 );
			}
			if( NetInterestAuthored( slot ) )
			{
				NetInterestClientRemove( slot );
				netInterest.removedApplied++;
			}
		}
	}
}

/*
================
idGameLocal::NetInterestBaseline

a client's snapshot reader, for an entity it must build in an authored slot: the map file's entity when the host's is
the map's own (the generation of the map spawn, the map's class, not removed), else NULL and the entity is built from
its class name alone, as outside the authored range
================
*/
const idMapEntity* idGameLocal::NetInterestBaseline( int slot, int generation, const char* classname )
{
	if( !netInterest.ordinals || slot < MAX_CLIENTS || slot >= netInterest.mapOrdinalEnd || mapFile == NULL )
	{
		return NULL;
	}
	const int ordinal = slot - MAX_CLIENTS + 1;
	if( ordinal >= mapFile->GetNumEntities() )
	{
		return NULL;
	}
	if( ( worldStore.removed[ ordinal >> 3 ] & ( 1 << ( ordinal & 7 ) ) ) != 0 )
	{
		netInterest.removedCreates++;
		return NULL;
	}
	const idMapEntity* mapEnt = mapFile->GetEntity( ordinal );
	// the def this machine's map spawn resolves the map's class name to (multiplayer prefers <name>_mp)
	const idDeclEntityDef* mapDef = FindEntityDef( mapEnt->epairs.GetString( "classname" ), false );
	if( netInterest.authoredGen[ slot ] == 0 || generation != netInterest.authoredGen[ slot ] || mapDef == NULL || idStr::Icmp( mapDef->GetName(), classname ) != 0 )
	{
		netInterest.plainCreates++;
		return NULL;
	}
	netInterest.baselineCreates++;
	return mapEnt;
}

/*
================
Cmd_NetDumpPresent_f

netDumpPresent <file>: every live entity of this machine as one line, into <file> under fs_savepath, for a test that
compares the host's entities with a client's. A head line, then per entity:
  slot generation authored type def flags teamMaster bindMaster sentMask x y z "name" targets
authored: 1 while the slot holds its map entity. flags: s skipReplication, n networkSync, d neverDormant, h hidden,
- none; from R6.2 on also m (its group is managed), r (resident: the host does not send it), w (a client: a snapshot
gave or confirmed it). teamMaster, bindMaster: a slot or -1. sentMask: hex, the peers the host's last accepted
snapshot gave the entity to (0 with the filter off and on a client). targets: slot.generation of each, or -
================
*/
void Cmd_NetDumpPresent_f( const idCmdArgs& args )
{
	if( args.Argc() != 2 )
	{
		gameLocal.Printf( "usage: netDumpPresent <file under fs_savepath>\n" );
		return;
	}
	if( gameLocal.GameState() != GAMESTATE_ACTIVE )
	{
		gameLocal.Printf( "netDumpPresent: no map is running\n" );
		return;
	}
	idFile* file = fileSystem->OpenFileWrite( args.Argv( 1 ), "fs_savepath" );
	if( file == NULL )
	{
		gameLocal.Printf( "netDumpPresent: can't write %s\n", args.Argv( 1 ) );
		return;
	}
	const netInterest_t& ni = gameLocal.netInterest;
	const char* role = !common->IsMultiplayer() ? "sp" : common->IsClient() ? "client" : "host";
	int count = 0;
	for( int n = 0; n < MAX_GENTITIES; n++ )
	{
		count += gameLocal.entities[ n ] != NULL;
	}
	file->Printf( "netDump 1 map %s role %s client %d time %d frame %d ordinals %d end %d nonrep %d entities %d creates %d %d %d removed %d %d active %d statics %d swept %d events %d %d\n",
				  gameLocal.GetMapName(), role, gameLocal.GetLocalClientNum(), gameLocal.time, gameLocal.framenum, ni.ordinals ? 1 : 0, ni.mapOrdinalEnd, ENTITYNUM_FIRST_NON_REPLICATED,
				  count, ni.baselineCreates, ni.plainCreates, ni.removedCreates, ni.removedSet, ni.removedApplied, ni.active ? 1 : 0, ni.statics ? 1 : 0, ni.swept, ni.eventsHeld, ni.eventsDropped );
	for( int n = 0; n < MAX_GENTITIES; n++ )
	{
		const idEntity* ent = gameLocal.entities[ n ];
		if( ent == NULL )
		{
			continue;
		}
		char flags[8];
		int f = 0;
		if( ent->fl.skipReplication )
		{
			flags[ f++ ] = 's';
		}
		if( ent->fl.networkSync )
		{
			flags[ f++ ] = 'n';
		}
		if( ent->fl.neverDormant )
		{
			flags[ f++ ] = 'd';
		}
		if( ent->IsHidden() )
		{
			flags[ f++ ] = 'h';
		}
		if( gameLocal.NetInterestGroupManaged( ent->GetTeamMaster() != NULL ? ent->GetTeamMaster() : ent ) )
		{
			flags[ f++ ] = 'm';
		}
		if( !common->IsClient() && gameLocal.NetInterestResident( ent ) )
		{
			flags[ f++ ] = 'r';
		}
		if( common->IsClient() && ( ( ni.wired[ n >> 5 ] >> ( n & 31 ) ) & 1 ) != 0 )
		{
			flags[ f++ ] = 'w';
		}
		if( f == 0 )
		{
			flags[ f++ ] = '-';
		}
		flags[ f ] = '\0';
		const idEntity* master = ent->GetTeamMaster();
		const idEntity* bind = ent->GetBindMaster();
		const idVec3& origin = ent->GetPhysics()->GetOrigin();
		const char* def = ent->GetEntityDefName();
		file->Printf( "%d %d %d %s %s %s %d %d %x %.3f %.3f %.3f \"%s\"", n, gameLocal.spawnIds[ n ], gameLocal.NetInterestAuthored( n ) ? 1 : 0, ent->GetClassname(),
					  ( def != NULL && def[0] != '\0' ) ? def : "-", flags, master != NULL ? master->entityNumber : -1, bind != NULL ? bind->entityNumber : -1,
					  ( ni.active && !common->IsClient() ) ? ni.presenceSent[ n ] : 0,
					  origin.x, origin.y, origin.z, ent->name.c_str() );
		if( ent->targets.Num() == 0 )
		{
			file->Printf( " -" );
		}
		for( int t = 0; t < ent->targets.Num(); t++ )
		{
			const idEntity* target = ent->targets[ t ].GetEntity();
			if( target != NULL )
			{
				file->Printf( " %d.%d", target->entityNumber, gameLocal.spawnIds[ target->entityNumber ] );
			}
			else
			{
				file->Printf( " x" );
			}
		}
		file->Printf( "\n" );
	}
	fileSystem->CloseFile( file );
	gameLocal.Printf( "netDumpPresent: %d entities to %s\n", count, args.Argv( 1 ) );
}

/*
===============================================================================

	R6.2 and R6.3

===============================================================================
*/

struct niGroup_t
{
	int		key;		// the team master's slot: the group's row of the out counters
	int		first;		// its members in niMembers
	int		num;
	byte	managed;	// 0: mandatory
	byte	want;		// the viewer in hand wants it for itself
	byte	sent;		// what that viewer's peer holds of it: 0 nothing, 1 some members, 2 all
};

struct niViewer_t
{
	int			peer;
	uint32		bit;
	idVec3		origin;
	pvsHandle_t	pvs;
};

// one build's scratch, main thread only (ServerWriteSnapshot and ServerSnapshotAccepted run between game frames)
static niGroup_t	niGroups[ MAX_GENTITIES ];
static int			niNumGroups;
static int			niGroupOf[ MAX_GENTITIES ];		// by a team master's slot: its group, -1
static byte			niWire[ MAX_GENTITIES ];		// by slot, below num_entities: 1 when the entity is in a group of this build
static int			niMembers[ MAX_GENTITIES ];		// slots, group after group
// by member index, for the members of managed groups: what every viewer's pass reads of a member, gathered once a
// build so the passes run over flat memory and not through each entity and its physics object
static idVec3		niOrigin[ MAX_GENTITIES ];
static int			niAreas[ MAX_GENTITIES ][ idEntity::MAX_PVS_AREAS ];
static byte			niNumAreas[ MAX_GENTITIES ];
// by member index, for every member: 1 = of a managed class, 2 = has targets or is a projectile. A bundle walks only
// the entities that can bring another group
static byte			niBrings[ MAX_GENTITIES ];
static uint32		niAdmitted[ MAX_GENTITIES ];	// by group: the stamp of the viewer pass that admitted it
static uint32		niTried[ MAX_GENTITIES ];		// by group: the stamp of the bundle that reached it
static int			niQueue[ MAX_GENTITIES ];
static uint32		niStamp;
// a client's snapshot read: the slots it built an entity in
static int			niCreated[ MAX_GENTITIES ];
static int			niNumCreated;
static int			niDoomed[ MAX_GENTITIES ];

/*
================
idGameLocal::NetInterestSlotReset

RegisterEntity and UnregisterEntity: the slot's new occupant has been sent to nobody, and nothing a snapshot said of
the old one holds for it
================
*/
void idGameLocal::NetInterestSlotReset( int slot )
{
	netInterest.presenceSent[ slot ] = 0;
	netInterest.presenceBuilt[ slot ] = 0;
	memset( netInterest.outSent[ slot ], 0, sizeof( netInterest.outSent[ slot ] ) );
	memset( netInterest.outBuilt[ slot ], 0, sizeof( netInterest.outBuilt[ slot ] ) );
	netInterest.wiredEver[ slot >> 3 ] &= ~( 1 << ( slot & 7 ) );
	netInterest.wired[ slot >> 5 ] &= ~( 1u << ( slot & 31 ) );
	// no build has seen this occupant yet (NetInterestEventUsers)
	netInterest.fresh[ slot >> 5 ] |= 1u << ( slot & 31 );
	// a player entity registers or leaves: whoever plays this client next is a peer the host has sent nothing, also
	// when it comes with the peer index the last one had (after the last peer left no build ran to see the change)
	if( slot < MAX_CLIENTS )
	{
		netInterest.peerOfClient[ slot ] = -1;
	}
}

/*
================
idGameLocal::NetInterestGroupManaged

for a team master (or an entity in no team): its group comes and goes with a viewer's interest. The host's selection
and a client's sweep both ask this, each of its own entities
================
*/
bool idGameLocal::NetInterestGroupManaged( const idEntity* master ) const
{
	const int type = master->GetType()->typeNum;
	if( type < 0 || type >= NET_INTEREST_MAX_TYPES || netInterest.classManaged[ type ] == 0 )
	{
		return false;
	}
	for( idEntity* part = const_cast< idEntity* >( master ); part != NULL; part = part->GetTeamChain() )
	{
		if( part->fl.neverDormant || part->IsType( idPlayer::Type ) )
		{
			return false;
		}
		// a map entity that is not replicated is each machine's own: were its master taken from a client, the
		// destructor would remove it there, and nothing on the wire could bring it back (Opus A, MAJOR-1)
		if( part->fl.skipReplication && NetInterestAuthored( part->entityNumber ) )
		{
			return false;
		}
	}
	return true;
}

/*
================
idGameLocal::NetInterestResident

the host: a map entity that is never sent, because every machine has its own copy and nothing about it changes
================
*/
bool idGameLocal::NetInterestResident( const idEntity* ent ) const
{
	const int slot = ent->entityNumber;
	if( !netInterest.statics || !NetInterestAuthored( slot ) || ( netInterest.wiredEver[ slot >> 3 ] & ( 1 << ( slot & 7 ) ) ) != 0 )
	{
		return false;
	}
	const int type = ent->GetType()->typeNum;
	return type >= 0 && type < NET_INTEREST_MAX_TYPES && netInterest.classManaged[ type ] == 0
		   && !ent->fl.networkSync && ent->GetBindMaster() == NULL;
}

// the group of the team an entity is in, -1 when the entity is not on the wire
static ID_INLINE int NI_GroupOf( const idEntity* ent )
{
	const idEntity* master = ent->GetTeamMaster();
	return niGroupOf[ ( master != NULL ? master : ent )->entityNumber ];
}

static ID_INLINE void NI_Reach( const idEntity* ent, uint32 pass, uint32 stamp, int& tail )
{
	if( ent == NULL )
	{
		return;
	}
	const int g = NI_GroupOf( ent );
	if( g >= 0 && niAdmitted[ g ] != pass && niTried[ g ] != stamp )
	{
		niTried[ g ] = stamp;
		niQueue[ tail++ ] = g;
	}
}

/*
================
NI_Bundle

the group `root` and every group it brings with it that the viewer pass has not admitted yet, into niQueue; returns
their number. A managed entity brings the entities it targets, and a projectile its owner; a group reached that way
brings what its own members target. newGroups counts the groups of which the peer holds nothing
================
*/
static int NI_Bundle( int root, uint32 pass, int& newGroups )
{
	const uint32 stamp = ++niStamp;
	int head = 0, tail = 0;
	niTried[ root ] = stamp;
	niQueue[ tail++ ] = root;
	newGroups = 0;
	while( head < tail )
	{
		const int g = niQueue[ head++ ];
		const niGroup_t& grp = niGroups[ g ];
		if( grp.sent == 0 )
		{
			newGroups++;
		}
		for( int i = 0; i < grp.num; i++ )
		{
			const byte brings = niBrings[ grp.first + i ];
			if( ( brings & 2 ) == 0 || ( g == root && ( brings & 1 ) == 0 ) )
			{
				continue;
			}
			const idEntity* member = gameLocal.entities[ niMembers[ grp.first + i ] ];
			for( int t = 0; t < member->targets.Num(); t++ )
			{
				NI_Reach( member->targets[ t ].GetEntity(), pass, stamp, tail );
			}
			if( member->IsType( idProjectile::Type ) )
			{
				NI_Reach( static_cast< const idProjectile* >( member )->GetOwner(), pass, stamp, tail );
			}
		}
	}
	return tail;
}

/*
================
idGameLocal::NetInterestBuild

the host, inside ServerWriteSnapshot after the players' PVS handles are set up and before the entities are written:
presenceBuilt[slot] becomes the mask of the peers this snapshot gives the entity to. With the filter off nothing is
selected and ServerWriteSnapshot gives every entity to all
================
*/
void idGameLocal::NetInterestBuild( const pvsHandle_t* pvsHandles )
{
	netInterest_t& ni = netInterest;

	// the peers that view the game, and what the host has counted for a peer it sees for the first time: nothing
	niViewer_t viewers[ MAX_CLIENTS ];
	int numViewers = 0;
	ni.builtViewers = 0;
	idLobbyBase& lobby = session->GetActingGameStateLobbyBase();
	assert( MAX_CLIENTS <= NET_INTEREST_PEERS );

	// The filter comes on: every peer the host already serves holds whatever it has sent so far, which is everything.
	// This stands ahead of the loop below, which empties the column of a peer it sees for the first time: such a peer
	// holds nothing, also when it arrives in the very build the filter comes on in (consultant 10)
	// every entity alive now is seen by this build, or by none while the filter is off
	memset( ni.fresh, 0, sizeof( ni.fresh ) );
	ni.viewPointSet = 0;
	const bool on = ni.ordinals && net_interest.GetBool();
	if( on && !ni.active )
	{
		for( int n = 0; n < MAX_GENTITIES; n++ )
		{
			const idEntity* ent = entities[ n ];
			ni.presenceSent[ n ] = ( ent != NULL && !ent->fl.skipReplication && !NetInterestResident( ent ) ) ? ~0u : 0;
		}
		memset( ni.outSent, 0, sizeof( ni.outSent ) );
	}
	ni.active = on;
	uint32 unfiltered = 0;
	for( int c = 0; c < MAX_CLIENTS; c++ )
	{
		int peer = -1;
		idPlayer* player = ( entities[ c ] != NULL && entities[ c ]->IsType( idPlayer::Type ) ) ? static_cast< idPlayer* >( entities[ c ] ) : NULL;
		if( player != NULL )
		{
			peer = lobby.PeerIndexFromLobbyUser( lobbyUserIDs[ c ] );
			if( peer >= NET_INTEREST_PEERS )
			{
				// a peer beyond the eight this table keeps counters for is not filtered: it is given everything,
				// as with the filter off (a snapshot mask has bits for peers 0 to 30)
				if( peer < 31 )
				{
					unfiltered |= 1u << ( peer + 1 );
				}
				peer = -1;
			}
		}
		if( peer != ni.peerOfClient[ c ] )
		{
			const int both[ 2 ] = { peer, ni.peerOfClient[ c ] };
			for( int k = 0; k < 2; k++ )
			{
				if( both[ k ] < 0 )
				{
					continue;
				}
				const uint32 clear = ~( 1u << ( both[ k ] + 1 ) );
				for( int n = 0; n < MAX_GENTITIES; n++ )
				{
					ni.presenceSent[ n ] &= clear;
					ni.outSent[ n ][ both[ k ] ] = 0;
				}
				memset( &ni.peers[ both[ k ] ], 0, sizeof( ni.peers[ 0 ] ) );
			}
			ni.peerOfClient[ c ] = peer;
		}
		if( peer < 0 || ( ni.builtViewers & ( 1u << ( peer + 1 ) ) ) != 0 )
		{
			continue;
		}
		const idPlayer* spectated = player;
		if( player->spectating && player->spectator != c && player->spectator >= 0 && player->spectator < MAX_CLIENTS
				&& entities[ player->spectator ] != NULL && entities[ player->spectator ]->IsType( idPlayer::Type ) )
		{
			spectated = static_cast< const idPlayer* >( entities[ player->spectator ] );
		}
		niViewer_t& viewer = viewers[ numViewers++ ];
		viewer.peer = peer;
		viewer.bit = 1u << ( peer + 1 );
		viewer.origin = ni.testViewerSet[ c ] ? ni.testViewer[ c ] : spectated->GetPhysics()->GetOrigin();
		viewer.pvs = pvsHandles[ c ];
		ni.builtViewers |= viewer.bit;
		ni.viewPoint[ peer ] = viewer.origin;
		ni.viewPointSet |= viewer.bit;
	}

	ni.builtViewers |= unfiltered;
	if( !on )
	{
		return;
	}
	memset( ni.presenceBuilt, 0, sizeof( ni.presenceBuilt ) );
	// the world is no member of any group and every viewer holds it
	if( entities[ ENTITYNUM_WORLD ] != NULL && !entities[ ENTITYNUM_WORLD ]->fl.skipReplication )
	{
		ni.presenceBuilt[ ENTITYNUM_WORLD ] = ni.builtViewers;
	}

	const uint64 buildStart = Sys_Microseconds();
	const bool usePVS = net_interestPVS.GetBool();

	// the groups, in the order of their lowest member slot
	if( niStamp > 0xF0000000u )
	{
		memset( niAdmitted, 0, sizeof( niAdmitted ) );
		memset( niTried, 0, sizeof( niTried ) );
		niStamp = 0;
	}
	memset( niGroupOf, 0xff, sizeof( niGroupOf ) );
	niNumGroups = 0;
	for( int pass = 0; pass < 2; pass++ )
	{
		if( pass == 1 )
		{
			int first = 0;
			for( int g = 0; g < niNumGroups; g++ )
			{
				niGroups[ g ].first = first;
				first += niGroups[ g ].num;
				niGroups[ g ].num = 0;
			}
		}
		for( int n = 0; n < num_entities; n++ )
		{
			const idEntity* ent = entities[ n ];
			if( pass == 0 )
			{
				niWire[ n ] = ( ent != NULL && !ent->fl.skipReplication && !NetInterestResident( ent ) ) ? 1 : 0;
			}
			if( niWire[ n ] == 0 )
			{
				continue;
			}
			const idEntity* master = ent->GetTeamMaster();
			if( master == NULL )
			{
				master = ent;
			}
			int g = niGroupOf[ master->entityNumber ];
			if( g < 0 )
			{
				g = niNumGroups++;
				niGroupOf[ master->entityNumber ] = g;
				niGroups[ g ].key = master->entityNumber;
				niGroups[ g ].num = 0;
				niGroups[ g ].managed = NetInterestGroupManaged( master ) ? 1 : 0;
			}
			if( pass == 1 )
			{
				const int k = niGroups[ g ].first + niGroups[ g ].num;
				niMembers[ k ] = n;
				niBrings[ k ] = ( byte )( ( ni.classManaged[ ent->GetType()->typeNum ] != 0 ? 1 : 0 )
										  | ( ( ent->targets.Num() > 0 || ent->IsType( idProjectile::Type ) ) ? 2 : 0 ) );
				if( niGroups[ g ].managed )
				{
					idEntity* member = entities[ n ];
					niOrigin[ k ] = member->GetPhysics()->GetOrigin();
					niNumAreas[ k ] = 0;
					if( usePVS )
					{
						const int numAreas = Min( member->GetNumPVSAreas(), ( int )idEntity::MAX_PVS_AREAS );
						memcpy( niAreas[ k ], member->GetPVSAreas(), numAreas * sizeof( niAreas[ k ][ 0 ] ) );
						niNumAreas[ k ] = ( byte )numAreas;
					}
				}
			}
			niGroups[ g ].num++;
		}
	}
	ni.usGather = ( int )( Sys_Microseconds() - buildStart );

	const double radius = Max( 0.0f, net_interestRadius.GetFloat() );
	const double nearRadius = Min( radius, ( double )Max( 0.0f, net_interestNearRadius.GetFloat() ) );
	const double holdRadius = radius + Max( 0.0f, net_interestHysteresis.GetFloat() );
	const double r2 = radius * radius, near2 = nearRadius * nearRadius, hold2 = holdRadius * holdRadius;
	const int linger = idMath::ClampInt( 0, 255, net_interestLinger.GetInteger() );
	const int budget = idMath::ClampInt( 1, MAX_GENTITIES, net_interestCreateBudget.GetInteger() );

	for( int v = 0; v < numViewers; v++ )
	{
		const niViewer_t& viewer = viewers[ v ];
		netInterestPeer_t& stats = ni.peers[ viewer.peer ];
		const uint64 start = Sys_Microseconds();

		// what this viewer wants for itself
		int wanted = 0;
		for( int g = 0; g < niNumGroups; g++ )
		{
			niGroup_t& grp = niGroups[ g ];
			int held = 0;
			for( int i = 0; i < grp.num; i++ )
			{
				held += ( ni.presenceSent[ niMembers[ grp.first + i ] ] & viewer.bit ) != 0;
			}
			grp.sent = ( held == 0 ) ? 0 : ( held == grp.num ) ? 2 : 1;
			if( !grp.managed )
			{
				grp.want = 1;
				ni.outBuilt[ grp.key ][ viewer.peer ] = 0;
				wanted++;
				continue;
			}
			double d2 = 1.0e300;	// farther than any member
			for( int i = 0; i < grp.num; i++ )
			{
				const idVec3& origin = niOrigin[ grp.first + i ];
				const double dx = ( double )origin.x - viewer.origin.x, dy = ( double )origin.y - viewer.origin.y, dz = ( double )origin.z - viewer.origin.z;
				const double d = dx * dx + dy * dy + dz * dz;
				if( d < d2 )
				{
					d2 = d;
				}
			}
			bool in = d2 <= near2;
			if( !in && d2 <= r2 )
			{
				in = !usePVS;
				for( int i = 0; !in && viewer.pvs.i >= 0 && i < grp.num; i++ )
				{
					in = pvs.InCurrentPVS( viewer.pvs, niAreas[ grp.first + i ], niNumAreas[ grp.first + i ] );
				}
			}
			const int out = ni.outSent[ grp.key ][ viewer.peer ];
			grp.want = ( in || ( grp.sent != 0 && d2 <= hold2 && out < linger ) ) ? 1 : 0;
			ni.outBuilt[ grp.key ][ viewer.peer ] = ( byte )( in ? 0 : Min( 255, out + 1 ) );
			wanted += grp.want;
		}

		// what the peer holds and is still wanted stays, with everything it brings: nothing of that is held back
		const uint32 pass = ++niStamp;
		int forcedNew = 0, pacedNew = 0, deferred = 0, fresh;
		for( int g = 0; g < niNumGroups; g++ )
		{
			if( niGroups[ g ].want && niGroups[ g ].sent != 0 && niAdmitted[ g ] != pass )
			{
				const int n = NI_Bundle( g, pass, fresh );
				for( int i = 0; i < n; i++ )
				{
					niAdmitted[ niQueue[ i ] ] = pass;
				}
				forcedNew += fresh;
			}
		}
		// what the peer does not hold enters under the budget, whole bundles: the mandatory groups first, then the
		// managed ones, each in slot order. The first bundle enters whatever its size; the first that does not fit
		// ends the admission of this build
		bool open = true;
		for( int managed = 0; managed <= 1; managed++ )
		{
			for( int g = 0; g < niNumGroups; g++ )
			{
				if( !niGroups[ g ].want || niGroups[ g ].managed != managed || niAdmitted[ g ] == pass )
				{
					continue;
				}
				if( !open )
				{
					deferred++;
					continue;
				}
				const int n = NI_Bundle( g, pass, fresh );
				if( pacedNew == 0 || pacedNew + fresh <= budget )
				{
					for( int i = 0; i < n; i++ )
					{
						niAdmitted[ niQueue[ i ] ] = pass;
					}
					pacedNew += fresh;
					open = pacedNew < budget;
				}
				else
				{
					open = false;
					deferred++;
				}
			}
		}
		for( int g = 0; g < niNumGroups; g++ )
		{
			if( niAdmitted[ g ] == pass )
			{
				const niGroup_t& grp = niGroups[ g ];
				for( int i = 0; i < grp.num; i++ )
				{
					ni.presenceBuilt[ niMembers[ grp.first + i ] ] |= viewer.bit;
				}
			}
		}
		stats.wanted = wanted;
		stats.deferred = deferred;
		stats.forcedNew = forcedNew;
		stats.pacedNew = pacedNew;
		stats.usSelect = ( int )( Sys_Microseconds() - start );
	}
	if( unfiltered != 0 )
	{
		const int numMembers = ( niNumGroups > 0 ) ? niGroups[ niNumGroups - 1 ].first + niGroups[ niNumGroups - 1 ].num : 0;
		for( int k = 0; k < numMembers; k++ )
		{
			ni.presenceBuilt[ niMembers[ k ] ] |= unfiltered;
		}
	}
	ni.usBuild = ( int )( Sys_Microseconds() - buildStart );
}

/*
================
idGameLocal::ServerSnapshotAccepted

the host, right after the session was handed the snapshot ServerWriteSnapshot built: acceptedMask holds bit p + 1 for
every peer whose session took it as its next snapshot to send. For those peers the build is now what the host
believes they hold; a peer that was still sending an older snapshot keeps its old state and its old counters, and
the same groups are selected for it again
================
*/
void idGameLocal::ServerSnapshotAccepted( uint32 acceptedMask )
{
	netInterest_t& ni = netInterest;
	acceptedMask &= ni.builtViewers;
	if( !ni.active || acceptedMask == 0 )
	{
		return;
	}
	int present[ NET_INTEREST_PEERS ], creates[ NET_INTEREST_PEERS ], deletes[ NET_INTEREST_PEERS ];
	memset( present, 0, sizeof( present ) );
	memset( creates, 0, sizeof( creates ) );
	memset( deletes, 0, sizeof( deletes ) );
	// the slots a build can give a peer: the replicated range in use, and the world
	const int slots[ 2 ][ 2 ] = { { 0, Min( num_entities, ( int )ENTITYNUM_WORLD ) }, { ENTITYNUM_WORLD, ENTITYNUM_WORLD + 1 } };
	for( int r = 0; r < 2; r++ )
	{
		for( int n = slots[ r ][ 0 ]; n < slots[ r ][ 1 ]; n++ )
		{
			const uint32 was = ni.presenceSent[ n ] & acceptedMask;
			const uint32 now = ni.presenceBuilt[ n ] & acceptedMask;
			if( ( was | now ) == 0 )
			{
				continue;
			}
			for( int p = 0; p < NET_INTEREST_PEERS; p++ )
			{
				const uint32 bit = 1u << ( p + 1 );
				present[ p ] += ( now & bit ) != 0;
				creates[ p ] += ( now & ~was & bit ) != 0;
				deletes[ p ] += ( was & ~now & bit ) != 0;
			}
			ni.presenceSent[ n ] = ( ni.presenceSent[ n ] & ~acceptedMask ) | now;
		}
	}
	for( int p = 0; p < NET_INTEREST_PEERS; p++ )
	{
		if( ( acceptedMask & ( 1u << ( p + 1 ) ) ) == 0 )
		{
			continue;
		}
		for( int g = 0; g < niNumGroups; g++ )
		{
			ni.outSent[ niGroups[ g ].key ][ p ] = ni.outBuilt[ niGroups[ g ].key ][ p ];
		}
		ni.peers[ p ].accepted++;
		ni.peers[ p ].present = present[ p ];
		ni.peers[ p ].creates += creates[ p ];
		ni.peers[ p ].deletes += deletes[ p ];
	}
}

/*
================
idGameLocal::NetInterestEventUsers

idEntity::ServerSendEvent: the lobby users an entity event goes to, as SendReliable's mask (bit j = the user of
ordinal j). A user on this machine gets none (the host runs the event itself); the excluded user gets none; with the
filter on, a user whose peer does not hold the entity gets none, since its copy is gone or not built yet. False when
nobody is left: the event is not sent at all, because SendReliable reads a mask of 0 as everybody
================
*/
bool idGameLocal::NetInterestEventUsers( const idEntity* ent, lobbyUserID_t excluding, peerMask_t& userMask )
{
	idLobbyBase& lobby = session->GetActingGameStateLobbyBase();
	// an entity that is never sent (a resident, a skipReplication entity) is on every machine
	const bool filtered = netInterest.active && !ent->fl.skipReplication && !NetInterestResident( ent );
	// a peer holds the entity, or the build in flight gives it to the peer: the event waits for it there
	// (net_eventHoldMs). An entity that registered after the last build is in neither; its first events (the sound
	// of its spawn) go to the peers whose view point is within the near radius, which the next build gives the
	// entity whatever the PVS says (Opus A, MINOR-1)
	const int slot = ent->entityNumber;
	uint32 sent = netInterest.presenceSent[ slot ] | netInterest.presenceBuilt[ slot ];
	if( filtered && ( ( netInterest.fresh[ slot >> 5 ] >> ( slot & 31 ) ) & 1 ) != 0 )
	{
		const idVec3& origin = ent->GetPhysics()->GetOrigin();
		const float nearRadius = Max( 0.0f, net_interestNearRadius.GetFloat() );
		for( int p = 0; p < NET_INTEREST_PEERS; p++ )
		{
			if( ( netInterest.viewPointSet & ( 1u << ( p + 1 ) ) ) != 0 && ( netInterest.viewPoint[ p ] - origin ).LengthSqr() <= nearRadius * nearRadius )
			{
				sent |= 1u << ( p + 1 );
			}
		}
	}
	userMask = 0;
	const int numUsers = Min( lobby.GetNumLobbyUsers(), ( int )( 8 * sizeof( peerMask_t ) ) );
	for( int j = 0; j < numUsers; j++ )
	{
		const lobbyUserID_t user = lobby.GetLobbyUserIdByOrdinal( j );
		if( !user.IsValid() || ( excluding.IsValid() && user == excluding ) )
		{
			continue;
		}
		const int peer = lobby.PeerIndexFromLobbyUser( user );
		if( peer < 0 )
		{
			continue;
		}
		if( filtered && peer < 31 && ( sent & ( 1u << ( peer + 1 ) ) ) == 0 )
		{
			if( peer < NET_INTEREST_PEERS )
			{
				netInterest.peers[ peer ].eventsSkipped++;
			}
			continue;
		}
		userMask |= ( peerMask_t )( 1 << j );
	}
	return userMask != 0;
}

/*
================
idGameLocal::NetRestartBegin

the host, once for every restart of the loaded map, before it restarts and before it tells the peers: the number
of the instance of the map its snapshots are of from now on. The restart message carries it, and so does every
snapshot (ahead of the game state)
================
*/
uint32 idGameLocal::NetRestartBegin()
{
	if( common->IsClient() )
	{
		return netInterest.restartSerial;
	}
	if( netInterest.restartSerial == 0xFFFFFFFFu )
	{
		Error( "idGameLocal::NetRestartBegin: the map was restarted %u times", netInterest.restartSerial - 1 );
	}
	return ++netInterest.restartSerial;
}

/*
================
idGameLocal::NetRestartSerialSet

a client reads the host's restart message: the instance this machine restarts into
================
*/
void idGameLocal::NetRestartSerialSet( uint32 serial )
{
	netInterest.restartSerial = serial;
	netInterest.restartSerialKnown = true;
}

/*
================
idGameLocal::NetRestartSerialAccept

a client, for the serial of a snapshot it is about to read. A snapshot of another instance of the map is applied to
nothing: the host built it before a restart this machine has made already, or after one this machine has not made
yet (the restart message and the snapshots travel apart). The layers below have read and acknowledged it. After a
map load a client takes the first snapshot's number: a snapshot of an earlier map never reaches a loaded client
================
*/
bool idGameLocal::NetRestartSerialAccept( uint32 serial )
{
	netInterest_t& ni = netInterest;
	if( !ni.restartSerialKnown )
	{
		ni.restartSerial = serial;
		ni.restartSerialKnown = true;
		ni.otherSerialRun = 0;
		return true;
	}
	if( serial == ni.restartSerial )
	{
		ni.otherSerialRun = 0;
		return true;
	}
	ni.staleSnapshots++;
	// The snapshots of another instance around a restart are a few. An unbroken run of NET_RESTART_SERIAL_RUN of one
	// other number means the host is in that instance and this machine was told a wrong one (a restart message of an
	// earlier map delivered into this one; Opus A, step A). A machine that applies nothing never recovers, so it takes
	// the host's number and says so
	if( serial != ni.otherSerial )
	{
		ni.otherSerial = serial;
		ni.otherSerialRun = 0;
	}
	if( ++ni.otherSerialRun < NET_RESTART_SERIAL_RUN )
	{
		return false;
	}
	Warning( "this machine took itself to be in instance %u of the map; the host's last %d snapshots are of instance %u: taking the host's", ni.restartSerial, ni.otherSerialRun, serial );
	ni.restartSerial = serial;
	ni.otherSerialRun = 0;
	return true;
}

/*
================
idGameLocal::NetInterestRevive

a client: the host's removed bit of an authored slot cleared, so the map entity lives on the host again. Where this
machine deleted its copy for that bit it builds the entity again as its map spawn did, with the generation that
spawn gave it. The snapshot in hand then confirms it, or the sweep takes it; a resident is never sent, and nothing
else would bring it back. Before the restart serial (R6.4) a client read snapshots the host had built before a map
restart after its own restart, and their bits deleted what the restart had just spawned; since it, this is for a
machine that was in a wrong instance for a while (NetRestartSerialAccept)
================
*/
void idGameLocal::NetInterestRevive( int slot )
{
	const int ordinal = slot - MAX_CLIENTS + 1;
	if( slot < MAX_CLIENTS || slot >= netInterest.mapOrdinalEnd || entities[ slot ] != NULL || netInterest.authoredGen[ slot ] == 0
			|| mapFile == NULL || ordinal >= mapFile->GetNumEntities() )
	{
		return;
	}
	const idMapEntity* mapEnt = mapFile->GetEntity( ordinal );
	idDict args = mapEnt->epairs;
	args.SetVector( BRUSH_ORIGIN_KEY, mapEnt->originOffset );
	// the one-shots of this slot's registration: the map spawn's slot, and the generation it gave
	forcedGenEntityNum = slot;
	forcedGen = netInterest.authoredGen[ slot ];
	netInterest.authoredSlot = slot;
	idEntity* ent = NULL;
	SpawnEntityDef( args, &ent );
	forcedGenEntityNum = -1;
	netInterest.authoredSlot = -1;
	if( !NetInterestAuthored( slot ) )
	{
		Warning( "NetInterestRevive: map ordinal %d did not spawn again", ordinal );
		return;
	}
	netInterest.revived++;
	if( niNumCreated < MAX_GENTITIES )
	{
		niCreated[ niNumCreated++ ] = slot;	// its targets and its bind, with what this snapshot read builds
	}
}

/*
================
idGameLocal::NetInterestClientRemove

a client deletes the entity of a slot because the wire, a removed bit or the sweep takes it away, and with it the
entities this machine bound to it for itself: a monster's def_attach props are spawned by its own spawn here, nothing
on the wire names them, and a client drops the EV_Remove the destructors post for a replicated entity, so each
would stay behind, unbound, once for every time its monster left (Opus A, MAJOR-2). Not this function's: an entity
a snapshot gave (the wire deletes it), a map entity, a player, and an entity that is not replicated (its EV_Remove
does run on a client)
================
*/
void idGameLocal::NetInterestClientRemove( int slot )
{
	idEntity* ent = entities[ slot ];
	if( ent == NULL )
	{
		return;
	}
	int own[ 64 ];
	int numOwn = 0;
	for( idEntity* part = ent->GetTeamChain(); part != NULL && numOwn < 64; part = part->GetTeamChain() )
	{
		const int n = part->entityNumber;
		if( n >= MAX_CLIENTS && !part->fl.skipReplication && ( ( netInterest.wired[ n >> 5 ] >> ( n & 31 ) ) & 1 ) == 0
				&& !NetInterestAuthored( n ) && part->IsBoundTo( ent ) )
		{
			own[ numOwn++ ] = n;
		}
	}
	for( int i = 0; i < numOwn; i++ )
	{
		delete entities[ own[ i ] ];
	}
	delete entities[ slot ];
}

/*
================
idGameLocal::NetInterestClientBegin

a client's ClientReadSnapshot, before the objects
================
*/
void idGameLocal::NetInterestClientBegin()
{
	memset( netInterest.seen, 0, sizeof( netInterest.seen ) );
	niNumCreated = 0;
}

/*
================
idGameLocal::NetInterestClientDelete

a client's ClientReadSnapshot, for an object of size 0: true when the slot's entity is the wire's to delete. A deleted
object stays in every later snapshot with size 0 (idSnapShot keeps the state), so the delete is read again and
again; it must not take an entity this machine put into the slot afterwards for itself
================
*/
bool idGameLocal::NetInterestClientDelete( int slot )
{
	return ( ( netInterest.wired[ slot >> 5 ] >> ( slot & 31 ) ) & 1 ) != 0;
}

/*
================
idGameLocal::NetInterestClientSeen

a client's ClientReadSnapshot, for an entity object with a state, once the slot holds the host's entity
================
*/
void idGameLocal::NetInterestClientSeen( int slot, bool created )
{
	netInterest.seen[ slot >> 5 ] |= 1u << ( slot & 31 );
	netInterest.wired[ slot >> 5 ] |= 1u << ( slot & 31 );
	if( created && niNumCreated < MAX_GENTITIES )
	{
		niCreated[ niNumCreated++ ] = slot;
	}
}

/*
================
idGameLocal::NetInterestClientFinish

a client's ClientReadSnapshot with the ordinal slots, after the objects and before the events.
  The sweep: a managed map entity the snapshot does not hold is not this client's to see. The host holds it back (out
of the viewer's interest, or not admitted yet) or has removed it; either way the copy goes, and comes back from the map
file when the host sends it. Mandatory and resident entities stay: the host sends them late or never.
  The entities this read built outside a map load missed two things of a map spawn: the targets of one built before
its target (FindTargets ran at its spawn), and the bind of the "bind" key (posted as an event, which a client drops
outside a map load). Both are done here, when every entity of the snapshot exists. A class that sends its bind in
its own state keeps what the host said
================
*/
void idGameLocal::NetInterestClientFinish()
{
	if( !netInterest.ordinals )
	{
		return;
	}
	// The candidates. The first read after a map spawn looks at every authored slot: this machine's own map spawn
	// made entities no snapshot has spoken of. After that sweep a managed map entity is here only because a snapshot
	// gave or confirmed it (wired) or this read built it (niCreated), so only those can be missing from the snapshot
	// in hand; a resident is never wired, and with residents on most of a map would be opened for nothing on every
	// read (Opus B, step A)
	int numCandidates = 0;
	if( netInterest.fullSweep )
	{
		netInterest.fullSweep = false;
		for( int n = MAX_CLIENTS; n < netInterest.mapOrdinalEnd; n++ )
		{
			if( entities[ n ] != NULL && ( ( netInterest.seen[ n >> 5 ] >> ( n & 31 ) ) & 1 ) == 0 )
			{
				niDoomed[ numCandidates++ ] = n;
			}
		}
	}
	else
	{
		const int words = ( netInterest.mapOrdinalEnd + 31 ) >> 5;
		for( int w = 0; w < words; w++ )
		{
			const uint32 bits = netInterest.wired[ w ] & ~netInterest.seen[ w ];
			for( int b = 0; bits != 0 && b < 32; b++ )
			{
				const int n = ( w << 5 ) + b;
				if( ( bits & ( 1u << b ) ) != 0 && n >= MAX_CLIENTS && n < netInterest.mapOrdinalEnd && entities[ n ] != NULL )
				{
					niDoomed[ numCandidates++ ] = n;
				}
			}
		}
		for( int i = 0; i < niNumCreated; i++ )
		{
			const int n = niCreated[ i ];
			if( n >= MAX_CLIENTS && n < netInterest.mapOrdinalEnd && entities[ n ] != NULL
					&& ( ( ( netInterest.seen[ n >> 5 ] | netInterest.wired[ n >> 5 ] ) >> ( n & 31 ) ) & 1 ) == 0 )
			{
				niDoomed[ numCandidates++ ] = n;
			}
		}
	}
	int numDoomed = 0;
	for( int c = 0; c < numCandidates; c++ )
	{
		const int n = niDoomed[ c ];
		const idEntity* ent = entities[ n ];
		if( ent->fl.skipReplication || !NetInterestAuthored( n ) )
		{
			continue;
		}
		const idEntity* master = ent->GetTeamMaster();
		if( NetInterestGroupManaged( master != NULL ? master : ent ) )
		{
			niDoomed[ numDoomed++ ] = n;
		}
	}
	for( int i = 0; i < numDoomed; i++ )
	{
		// deleting one member can take a bound one with it
		if( entities[ niDoomed[ i ] ] != NULL && NetInterestAuthored( niDoomed[ i ] ) )
		{
			NetInterestClientRemove( niDoomed[ i ] );
			netInterest.swept++;
		}
	}
	for( int i = 0; i < niNumCreated; i++ )
	{
		idEntity* ent = entities[ niCreated[ i ] ];
		if( ent == NULL )
		{
			continue;
		}
		if( ent->spawnArgs.MatchPrefix( "target" ) != NULL )
		{
			ent->FindTargets();
		}
		const int type = ent->GetType()->typeNum;
		const bool bindOnWire = ent->fl.networkSync && type >= 0 && type < NET_INTEREST_MAX_TYPES && netInterest.classManaged[ type ] == 0;
		if( !bindOnWire && ent->GetBindMaster() == NULL && ent->spawnArgs.GetString( "bind" )[ 0 ] != '\0' )
		{
			ent->ProcessEvent( &EV_SpawnBind );
		}
	}
	niNumCreated = 0;
}

/*
================
Cmd_ListInterest_f

listInterest: the switches a map was loaded under, and on the host a line for every peer
================
*/
void Cmd_ListInterest_f( const idCmdArgs& args )
{
	const netInterest_t& ni = gameLocal.netInterest;
	gameLocal.Printf( "listInterest: ordinals %d statics %d end %d headroom %d active %d radius %g hysteresis %g near %g pvs %d linger %d budget %d buildUs %d gatherUs %d\n",
					  ni.ordinals ? 1 : 0, ni.statics ? 1 : 0, ni.mapOrdinalEnd, ni.headroom, ni.active ? 1 : 0, net_interestRadius.GetFloat(), net_interestHysteresis.GetFloat(),
					  net_interestNearRadius.GetFloat(), net_interestPVS.GetBool() ? 1 : 0, net_interestLinger.GetInteger(), net_interestCreateBudget.GetInteger(), ni.usBuild, ni.usGather );
	gameLocal.Printf( "listInterest: client creates baseline %d plain %d removed %d, swept %d, removed bits set %d applied %d, events held %d dropped %d, revived %d\n",
					  ni.baselineCreates, ni.plainCreates, ni.removedCreates, ni.swept, ni.removedSet, ni.removedApplied, ni.eventsHeld, ni.eventsDropped, ni.revived );
	gameLocal.Printf( "listInterest: restart serial %u known %d, snapshots of another instance not applied %d\n", ni.restartSerial, ni.restartSerialKnown ? 1 : 0, ni.staleSnapshots );
	for( int p = 0; p < NET_INTEREST_PEERS; p++ )
	{
		const netInterestPeer_t& peer = ni.peers[ p ];
		if( ( ni.builtViewers & ( 1u << ( p + 1 ) ) ) == 0 && peer.accepted == 0 )
		{
			continue;
		}
		gameLocal.Printf( "listInterest: peer %d accepted %d present %d creates %d deletes %d wanted %d deferred %d forcedNew %d pacedNew %d eventsSkipped %d selectUs %d\n",
						  p, peer.accepted, peer.present, peer.creates, peer.deletes, peer.wanted, peer.deferred, peer.forcedNew, peer.pacedNew, peer.eventsSkipped, peer.usSelect );
	}
}

/*
================
Cmd_NetTestViewer_f

netTestViewer <client> [<x> <y> <z>]: on the host, a test's way to say where a client's peer views the game from: the
selection takes this point for that client's player (which, as a spectator, stays wherever the game holds it) until
the command is given again without a point
================
*/
void Cmd_NetTestViewer_f( const idCmdArgs& args )
{
	const int client = ( args.Argc() >= 2 ) ? atoi( args.Argv( 1 ) ) : -1;
	if( ( args.Argc() != 2 && args.Argc() != 5 ) || common->IsClient() || client < 0 || client >= MAX_CLIENTS )
	{
		gameLocal.Printf( "usage, on the host: netTestViewer <client number> [<x> <y> <z>]\n" );
		return;
	}
	netInterest_t& ni = gameLocal.netInterest;
	ni.testViewerSet[ client ] = ( args.Argc() == 5 );
	if( ni.testViewerSet[ client ] )
	{
		ni.testViewer[ client ].Set( ( float )atof( args.Argv( 2 ) ), ( float )atof( args.Argv( 3 ) ), ( float )atof( args.Argv( 4 ) ) );
		gameLocal.Printf( "netTestViewer: client %d views from %s\n", client, ni.testViewer[ client ].ToString( 0 ) );
	}
	else
	{
		gameLocal.Printf( "netTestViewer: client %d views from its player\n", client );
	}
}

/*
================
Cmd_NetTestEvent_f

netTestEvent <entity name>: on the host, a test's entity event: a stop-sound of a channel nothing plays on, sent as
any entity event is (to the peers that hold the entity)
================
*/
void Cmd_NetTestEvent_f( const idCmdArgs& args )
{
	idEntity* ent = ( args.Argc() == 2 && !common->IsClient() ) ? gameLocal.FindEntity( args.Argv( 1 ) ) : NULL;
	if( ent == NULL )
	{
		gameLocal.Printf( "usage, on the host: netTestEvent <entity name>\n" );
		return;
	}
	idBitMsg msg;
	byte msgBuf[ MAX_EVENT_PARAM_SIZE ];
	msg.InitWrite( msgBuf, sizeof( msgBuf ) );
	msg.BeginWriting();
	msg.WriteByte( SND_CHANNEL_DEMONIC );
	ent->ServerSendEvent( idEntity::EVENT_STOPSOUNDSHADER, &msg, false );
	gameLocal.Printf( "netTestEvent: sent for %s (slot %d)\n", ent->name.c_str(), ent->entityNumber );
}

/*
================
Cmd_NetTestStale_f

netTestStale <map ordinal>: on a client, a test's stand-in for a snapshot the host built before a map restart and
the client read after its own: the ordinal's removed bit is set and the map entity deleted, through the reader
itself. The host's next snapshot does not hold the bit.
netTestStale serial <n>: on a client, the instance of the map this machine takes itself to be in (R6.4): with a
number that is not the host's every snapshot is one of another instance and is applied to nothing
================
*/
void Cmd_NetTestStale_f( const idCmdArgs& args )
{
	if( args.Argc() == 3 && idStr::Icmp( args.Argv( 1 ), "serial" ) == 0 && common->IsClient() )
	{
		gameLocal.NetRestartSerialSet( ( uint32 )atoi( args.Argv( 2 ) ) );
		gameLocal.Printf( "netTestStale: this machine takes itself to be in instance %u of the map\n", gameLocal.netInterest.restartSerial );
		return;
	}
	const int ordinal = ( args.Argc() == 2 ) ? atoi( args.Argv( 1 ) ) : 0;
	if( !common->IsClient() || !gameLocal.netInterest.ordinals || ordinal < 1 || ordinal > gameLocal.netInterest.mapOrdinalEnd - MAX_CLIENTS )
	{
		gameLocal.Printf( "usage, on a client of a map with net_mapOrdinals: netTestStale <map ordinal>; on any client: netTestStale serial <n>\n" );
		return;
	}
	const int firstByte = ( ordinal / 8 / 500 ) * 500;
	const int bytes = Min( 500, ( int )sizeof( worldStore.removed ) - firstByte );
	byte buffer[ 500 ];
	memcpy( buffer, worldStore.removed + firstByte, bytes );
	buffer[ ordinal / 8 - firstByte ] |= ( byte )( 1 << ( ordinal & 7 ) );
	idBitMsg msg;
	msg.InitRead( buffer, bytes );
	gameLocal.NetInterestReadRemoved( firstByte, msg );
	gameLocal.Printf( "netTestStale: ordinal %d (slot %d) removed as a snapshot from before a restart would\n", ordinal, MAX_CLIENTS + ordinal - 1 );
}
