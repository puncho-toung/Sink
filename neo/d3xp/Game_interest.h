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
#ifndef __GAME_INTEREST_H__
#define __GAME_INTEREST_H__

/*
===============================================================================

	puncho - stage R6 (plans/single-map/R6-interest.md): multiplayer interest.

	R6.1, the map's ordinal slots (net_mapOrdinals, read at a map load, multiplayer only): the map entity of ordinal i
	(its index in the map file, the world is 0) takes the slot MAX_CLIENTS + i - 1 on the host and on every client,
	whatever either side spawns beside it. An inhibited ordinal leaves its slot empty for the life of the map. Every
	other entity of the replicated range is given a slot at or above mapOrdinalEnd. So a slot below mapOrdinalEnd
	names one map entity on every machine, and a client can build that entity again from its own map file.

	The removed bits (idWorldStore::removed, one per ordinal) are the host's record of the authored entities play has
	removed; the snapshot's SNAP_MAP_REMOVED chunks carry them, and a client deletes its copy and does not build it
	again while the bit stands. The host clears bits at a map load only; a client that sees one clear builds the
	entity again where it had deleted it (it read a snapshot from before the host's restart after its own).

	R6.2, interest (net_interest, live, needs the ordinal slots): the host gives each peer the entities near that
	peer's player. Entities bound into one physics team are one group. A group is MANAGED when its team master is of
	a managed class (monsters, moveables, items, projectiles, ragdolls, corpses, debris) and no member is a player or
	neverDormant; every other group is MANDATORY and always wanted. A managed group is wanted while a member is within
	net_interestNearRadius of the viewer, or within net_interestRadius and in the viewer's PVS; one the peer holds
	stays for net_interestLinger accepted snapshots while inside the radius plus net_interestHysteresis. A wanted group
	brings the groups its members target and the owners of its projectiles. Of the groups a peer does not hold yet,
	at most net_interestCreateBudget enter with one accepted snapshot, mandatory ones first, in slot order.
	  The session reports which peers took a snapshot (idGame::ServerSnapshotAccepted); only then does the build
	become what the host believes the peer holds (presenceSent). An entity outside a peer's mask is left out of that
	peer's delta, and one its base holds goes as a delete.
	  A client deletes, after every snapshot, the managed map entities the snapshot did not hold, and builds one that
	comes (back) from its map file. An event waits net_eventHoldMs for an entity that is not there yet.

	R6.3, residents (net_interestStatics, read at a map load, needs the ordinal slots): a map entity of no managed
	class that is not networkSync and bound to nothing is never sent; every machine has its own copy, and the removed
	bits tell a client when play removed one. Once such an entity is sent (it became networkSync, or was bound) it
	stays sent. An entity event goes to the peers that hold the entity, or to all for a resident.

===============================================================================
*/

const int NET_INTEREST_PEERS		= 8;		// the peers a host can serve: MAX_PLAYERS, a snapshot mask's bits 1 to 8
const int NET_INTEREST_MAX_TYPES	= 1024;		// idClass types the class table holds
const int NET_RESTART_SERIAL_RUN	= 30;		// snapshots of one other instance in a row before a client takes the host's number (3 s)

struct netInterestPeer_t
{
	int				accepted;			// snapshots this peer's session took since the peer was first seen
	int				present;			// entities of its last accepted snapshot
	int				creates;			// entities that entered, summed over its accepted snapshots
	int				deletes;			// entities that left
	int				eventsSkipped;		// events not sent: the peer did not hold the entity
	// the last build
	int				wanted;				// groups wanted
	int				deferred;			// wanted groups the budget held back
	int				forcedNew;			// new groups admitted beside the budget: what a held group depends on
	int				pacedNew;			// new groups admitted under the budget
	int				usSelect;			// microseconds of this peer's selection
};

struct netInterest_t
{
	// read at a map load by idGameLocal::NetInterestLatch; a map restart keeps them
	bool			ordinals;			// net_mapOrdinals in a multiplayer game
	int				headroom;			// net_mapOrdinalHeadroom: slots kept for run-time entities above the map's
	int				mapOrdinalEnd;		// one past the last authored slot; MAX_CLIENTS while ordinals is off

	// a one-shot of the two engine paths that fill an authored slot (the map spawn, a client's snapshot reader): the
	// next registration may take this slot; -1 for none
	int				authoredSlot;

	// the generation the map spawn gave each authored slot, 0 for an empty one; a slot holds its map entity while
	// spawnIds[slot] is this value
	unsigned short	authoredGen[ MAX_GENTITIES ];

	// counted since the map spawn
	int				baselineCreates;	// a client built a map entity again from the map file's keys
	int				plainCreates;		// a client built an entity of the authored range from its class name alone
	int				removedCreates;		// a client was sent a map entity whose removed bit it holds
	int				removedSet;			// the host set a removed bit
	int				removedApplied;		// a client deleted its copy for a removed bit
	int				staleSnapshots;		// a client did not apply a snapshot of another instance of the map (R6.4; since the map load)
	int				revived;			// a client built a map entity again when its removed bit cleared
	int				swept;				// a client deleted a managed map entity its snapshot did not hold
	int				eventsHeld;			// a client's event waited for its entity
	int				eventsDropped;		// a client's event was dropped: no entity within the hold, or another generation

	// R6.4, multiplayer, whatever the switches say: the instance of the loaded map. The host counts its restarts of
	// the map from 1; the restart message and every snapshot carry the number, and a client applies only the
	// snapshots of the instance it is in (NetRestartSerialAccept). Set at a map load, kept over a restart
	uint32			restartSerial;
	bool			restartSerialKnown;	// a client: a restart message or a first snapshot gave the number
	uint32			otherSerial;		// a client: the number of the last snapshot it did not apply
	int				otherSerialRun;		// and how many of that number came in a row

	// R6.3
	bool			statics;			// net_interestStatics with the ordinal slots, read at a map load
	byte			wiredEver[ MAX_GENTITIES / 8 ];	// the host: this occupant of the slot was put on the wire and stays there

	// R6.2, a client: the slot's entity was given or confirmed by a snapshot (a size-0 object deletes only such an
	// entity), and the slots the snapshot being read holds
	uint32			wired[ MAX_GENTITIES / 32 ];
	uint32			seen[ MAX_GENTITIES / 32 ];
	bool			fullSweep;			// a client: the next sweep looks at every authored slot (the first after a map spawn)

	// R6.2, the host. Written between game frames, on the main thread (ServerWriteSnapshot, ServerSnapshotAccepted);
	// the game thread reads presenceSent for events and clears a slot's rows when an entity registers or leaves
	bool			active;				// the last build was filtered
	byte			classManaged[ NET_INTEREST_MAX_TYPES ];	// by idTypeInfo::typeNum
	int				peerOfClient[ MAX_CLIENTS ];			// the peer each client number was last seen on, -1
	uint32			fresh[ MAX_GENTITIES / 32 ];			// the slot's occupant registered after the last build
	idVec3			viewPoint[ NET_INTEREST_PEERS ];		// where the last build had each peer view from
	uint32			viewPointSet;							// the mask bits of the peers viewPoint holds
	uint32			presenceSent[ MAX_GENTITIES ];			// bit p + 1: peer p's last accepted snapshot held the entity
	uint32			presenceBuilt[ MAX_GENTITIES ];			// the same for the build in flight
	byte			outSent[ MAX_GENTITIES ][ NET_INTEREST_PEERS ];	// by the group's master slot: accepted snapshots in a row the group was out of range
	byte			outBuilt[ MAX_GENTITIES ][ NET_INTEREST_PEERS ];
	uint32			builtViewers;		// the mask bits of the peers the build in flight selected for
	int				usBuild;			// microseconds of the last build's selection, all viewers
	int				usGather;			// of those, the groups and what was gathered of their members
	bool			testViewerSet[ MAX_CLIENTS ];	// netTestViewer: a test names the point a client's peer views from
	idVec3			testViewer[ MAX_CLIENTS ];
	netInterestPeer_t	peers[ NET_INTEREST_PEERS ];
};

void	Cmd_NetDumpPresent_f( const idCmdArgs& args );
void	Cmd_ListInterest_f( const idCmdArgs& args );
void	Cmd_NetTestViewer_f( const idCmdArgs& args );
void	Cmd_NetTestEvent_f( const idCmdArgs& args );
void	Cmd_NetTestStale_f( const idCmdArgs& args );

#endif /* !__GAME_INTEREST_H__ */
