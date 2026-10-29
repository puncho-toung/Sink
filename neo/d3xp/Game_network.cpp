/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2014-2016 Robert Beckebans
Copyright (C) 2014-2016 Kot in Action Creative Artel

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
#include "gamesys/MapGraph.h"
#include "../framework/Common_local.h"
#include "gamesys/StreamTrace.h"	//puncho - stream-trace (P1-A1c)

static const int SNAP_GAMESTATE = 0;
static const int SNAP_SHADERPARMS = 1;
static const int SNAP_PORTALS = 2;
static const int SNAP_PLAYERSTATE = SNAP_PORTALS + 1;
static const int SNAP_PLAYERSTATE_END = SNAP_PLAYERSTATE + MAX_PLAYERS;
static const int SNAP_ENTITIES = SNAP_PLAYERSTATE_END;
static const int SNAP_ENTITIES_END = SNAP_ENTITIES + MAX_GENTITIES;
static const int SNAP_LAST_CLIENT_FRAME = SNAP_ENTITIES_END;
static const int SNAP_LAST_CLIENT_FRAME_END = SNAP_LAST_CLIENT_FRAME + MAX_PLAYERS;
static const int SNAP_PORTALS_EXTRA = SNAP_LAST_CLIENT_FRAME_END;
static const int SNAP_MAP_REMOVED = SNAP_LAST_CLIENT_FRAME_END + 64;
static const int MAP_REMOVED_CHUNKS = ( MAX_WORLD_MAP_ORDINALS + 3999 ) / 4000;	//puncho - P2.4 (bug review 2026-10-28): one bit per map ordinal
compile_time_assert( SNAP_MAP_REMOVED + MAP_REMOVED_CHUNKS - MAX_GENTITIES <= MAX_NET_FIXED_OBJECTS );
static const int PORTALS_PER_SNAP_OBJECT = 1300;
static const int MAX_PORTAL_CHUNKS = 16;
compile_time_assert( SNAP_PORTALS_EXTRA + MAX_PORTAL_CHUNKS - MAX_GENTITIES <= MAX_NET_FIXED_OBJECTS );
//puncho - P0-m9: the objects other than entities fit the snapshot layer's budget for them (sys/sys_session.h)
compile_time_assert( SNAP_LAST_CLIENT_FRAME_END - MAX_GENTITIES <= MAX_NET_FIXED_OBJECTS );

/*
===============================================================================

	Client running game code:
	- entity events don't work and should not be issued
	- entities should never be spawned outside idGameLocal::ClientReadSnapshot

===============================================================================
*/

idCVar net_clientSmoothing( "net_clientSmoothing", "0.8", CVAR_GAME | CVAR_FLOAT, "smooth other clients angles and position.", 0.0f, 0.95f );
idCVar net_clientSelfSmoothing( "net_clientSelfSmoothing", "0.6", CVAR_GAME | CVAR_FLOAT, "smooth self position if network causes prediction error.", 0.0f, 0.95f );
extern idCVar net_clientMaxPrediction;

idCVar cg_predictedSpawn_debug( "cg_predictedSpawn_debug", "0", CVAR_BOOL, "Debug predictive spawning of presentables" );
idCVar g_clientFire_checkLineOfSightDebug( "g_clientFire_checkLineOfSightDebug", "0", CVAR_BOOL, "" );
idCVar net_debugDeclRemap( "net_debugDeclRemap", "0", CVAR_INTEGER | CVAR_NOCHEAT, "1 prints each slot of the implicit decl announce table as the host announces it or a client resolves it, 2 also each implicit decl index a client decodes", 0, 2 );	//puncho - content check (P1-A3a, K19)


/*
================
idGameLocal::InitAsyncNetwork
================
*/
void idGameLocal::InitAsyncNetwork()
{
	eventQueue.Init();
	savedEventQueue.Init();

	//puncho start - content check (P1-A3a, K19): one load, one announce table. The width of an entityDef on the wire is
	// the same on every peer: it comes from the explicit entityDefs, which the content digests prove equal, and from the
	// table's cap, since an implicit one crosses as -2 - k. A count with the implicit decls would follow each machine's
	// history. The headroom covers explicit indices above the explicit count (an implicit decl made before its folder
	// registered keeps a low index) and decls a reload adds; GetContentDigests also hashes the width
	numDeclRemap = 0;
	numDeclRemapSent = 0;
	declRemapBytes = 0;
	declRemapDeferred = false;
	declRemapWarned = false;
	declRemapHighWarned = false;
	declRemapHash.Clear();
	for( int k = 0; k < MAX_DECL_REMAP; k++ )
	{
		declRemap[k].index = -1;
		declRemap[k].type = -1;
		declRemap[k].warned = false;
	}

	// the decl manager counts them from its own lists: DeclByIndex would allocate every entityDef's decl object
	const int entityDefBitsHeadroom = 256;
	int numExplicitEntityDefs = 0;
	int explicitEntityDefBound = 0;
	declManager->GetExplicitDeclRange( DECL_ENTITYDEF, numExplicitEntityDefs, explicitEntityDefBound );
	entityDefBits = -( idMath::BitsForInteger( Max( numExplicitEntityDefs, MAX_DECL_REMAP + 1 ) + entityDefBitsHeadroom ) + 1 );
	if( explicitEntityDefBound > ( 1 << ( -entityDefBits - 1 ) ) )
	{
		// WriteBits would stop the host with a fatal error on the first entity of such a def
		Warning( "InitAsyncNetwork: an explicit entityDef sits at index %d, past the %d signed bits the network gives entityDefs (%d explicit)", explicitEntityDefBound - 1, -entityDefBits, numExplicitEntityDefs );
	}
	//puncho end
	realClientTime = 0;
	fast.Set( 0, 0, 0 );
	slow.Set( 0, 0, 0 );
	isNewFrame = true;
	clientSmoothing = net_clientSmoothing.GetFloat();

	lastCmdRunTimeOnClient.Zero();
	lastCmdRunTimeOnServer.Zero();
	usercmdLastClientMilliseconds.Zero();
}

/*
================
idGameLocal::ShutdownAsyncNetwork
================
*/
void idGameLocal::ShutdownAsyncNetwork()
{
	eventQueue.Shutdown();
	savedEventQueue.Shutdown();
}

/*
================
idGameLocal::ServerRemapDecl
================
*/
int idGameLocal::ServerRemapDecl( int clientNum, declType_t type, int index )
{
	//puncho start - content check (P1-A3a, K19): an explicit decl crosses as its index, which MP-A0's digests prove the
	// same on every peer, and -1 stays null. An implicit decl is numbered in the order each machine first asked for it,
	// so it crosses as -2 - k, k its slot in the announce table. The test is IsImplicit, never an index bound: an
	// implicit decl made before its folder registered keeps a low index. clientNum stays unused: every caller passes
	// -1 and the host writes one snapshot for all its peers, so the table is the session's, not a client's
	if( index < 0 )
	{
		return index;
	}
	// one bit per decl: this runs for every entity of every snapshot. A clear bit is final; a set bit is confirmed, since
	// a file may have defined the decl after it was made implicit
	int numDecls;
	const unsigned int* implicitBits = declManager->GetImplicitDeclBits( type, numDecls );
	if( index >= numDecls || ( implicitBits[ index >> 5 ] & ( 1u << ( index & 31 ) ) ) == 0 )
	{
		return index;
	}
	const idDecl* decl = declManager->DeclByIndex( type, index, false );
	if( !decl->IsImplicit() )
	{
		return index;
	}

	for( int k = declRemapHash.First( index ); k != -1; k = declRemapHash.Next( k ) )
	{
		if( declRemap[k].index == index && declRemap[k].type == type )
		{
			return -2 - k;
		}
	}

	// a new slot, unless the table is full: a joiner takes the whole table at once in its initial reliables
	const int nameLength = idStr::Length( decl->GetName() );
	const int slotBytes = 1 + nameLength + 1;
	if( numDeclRemap >= MAX_DECL_REMAP || declRemapBytes + slotBytes > MAX_DECL_REMAP_BYTES || nameLength > MAX_DECL_REMAP_NAME )
	{
		if( !declRemapWarned )
		{
			declRemapWarned = true;
			Warning( "ServerRemapDecl: the implicit decl announce table is full (%d of %d slots, %d of %d bytes) or '%s' is too long: implicit decls not announced yet cross as null", numDeclRemap, MAX_DECL_REMAP, declRemapBytes, MAX_DECL_REMAP_BYTES, decl->GetName() );
		}
		return -1;
	}

	const int k = numDeclRemap++;
	declRemap[k].index = index;
	declRemap[k].type = type;
	declRemap[k].warned = false;
	declRemapBytes += slotBytes;
	declRemapHash.Add( index, k );

	// the caps are sized from content, so the warning says when one nears its end, once per load
	if( !declRemapHighWarned && ( numDeclRemap * 4 > MAX_DECL_REMAP * 3 || declRemapBytes * 4 > MAX_DECL_REMAP_BYTES * 3 ) )
	{
		declRemapHighWarned = true;
		Warning( "ServerRemapDecl: the implicit decl announce table is past 75%% of a cap (%d of %d slots, %d of %d bytes)", numDeclRemap, MAX_DECL_REMAP, declRemapBytes, MAX_DECL_REMAP_BYTES );
	}

	if( net_debugDeclRemap.GetInteger() > 0 )
	{
		Printf( "DECL_REMAP: announce slot %d: %s '%s' (host index %d)\n", k, declManager->GetDeclNameFromType( type ), decl->GetName(), index );
	}

	// The slot must reach every peer before any message decoded with it. A snapshot broadcasts its new slots once it is
	// written, before the session submits it, and a game frame at its end, after the entity events that use them: a
	// client decodes those events only after it has applied the frame's reliables. Outside both (a console command),
	// the slot goes out right away
	if( !declRemapDeferred )
	{
		ServerFlushDeclRemap();
	}
	return -2 - k;
	//puncho end
}

/*
================
idGameLocal::ClientRemapDecl
================
*/
int idGameLocal::ClientRemapDecl( declType_t type, int index )
{
	//puncho start - content check (P1-A3a, K19): explicit indices and -1 cross as they are. -2 - k is slot k of the host's
	// announce table, which ClientReadDeclRemap resolved by name to this machine's decl; a slot not announced yet (or of
	// another type) resolves to NULL with one warning per slot, never to a raw index
	if( index >= -1 )
	{
		return index;
	}
	const int k = -2 - index;
	if( k < MAX_DECL_REMAP && declRemap[k].index >= 0 && declRemap[k].type == type )
	{
		if( net_debugDeclRemap.GetInteger() > 1 )
		{
			Printf( "DECL_REMAP: %s %d is slot %d, '%s' (local index %d)\n", declManager->GetDeclNameFromType( type ), index, k, declManager->DeclByIndex( type, declRemap[k].index, false )->GetName(), declRemap[k].index );
		}
		return declRemap[k].index;
	}
	if( k < MAX_DECL_REMAP )
	{
		if( !declRemap[k].warned )
		{
			declRemap[k].warned = true;
			Warning( "ClientRemapDecl: %s %d is slot %d of the implicit decl table, which the host has not announced as one: null", declManager->GetDeclNameFromType( type ), index, k );
		}
	}
	else if( !declRemapWarned )
	{
		declRemapWarned = true;
		Warning( "ClientRemapDecl: %s %d is past the %d slots of the implicit decl table: null", declManager->GetDeclNameFromType( type ), index, MAX_DECL_REMAP );
	}
	return -1;
	//puncho end
}

//puncho start - content check (P1-A3a, K19)
/*
================
idGameLocal::ServerSendDeclRemap

Sends slots [first, last) of the announce table as GAME_RELIABLE_MESSAGE_DECL_REMAP batches of at most 1 KB: to one
lobby user (a joiner's whole table), or to every peer when lobbyUserID is NULL. A batch is the first slot's number, then
a type byte and the decl's name per slot, to the end of the message. Every batch says where its slots go, so a peer that
takes a slot twice (a broadcast during its load and its whole table after) sets it twice to the same decl.
================
*/
void idGameLocal::ServerSendDeclRemap( int first, int last, const lobbyUserID_t* lobbyUserID )
{
	if( first >= last || !common->IsServer() )
	{
		return;		// nothing to send, or single player's dry-run snapshots, which announce to nobody
	}

	idLobbyBase& lobby = session->GetActingGameStateLobbyBase();
	byte msgBuf[ MAX_DECL_REMAP_BATCH ];
	idBitMsg outMsg;

	if( net_debugDeclRemap.GetInteger() > 0 )
	{
		Printf( "DECL_REMAP: sending slots %d to %d %s\n", first, last - 1, ( lobbyUserID != NULL ) ? "to a joiner" : "to every peer" );
	}

	int k = first;
	while( k < last )
	{
		outMsg.InitWrite( msgBuf, sizeof( msgBuf ) );
		outMsg.BeginWriting();
		outMsg.WriteShort( k );
		for( ; k < last; k++ )
		{
			// every slot fits an empty batch: MAX_DECL_REMAP_NAME keeps it under MAX_DECL_REMAP_BATCH - 2
			const char* name = declManager->DeclByIndex( ( declType_t )declRemap[k].type, declRemap[k].index, false )->GetName();
			if( outMsg.GetSize() + 1 + idStr::Length( name ) + 1 > MAX_DECL_REMAP_BATCH )
			{
				break;
			}
			outMsg.WriteByte( declRemap[k].type );
			outMsg.WriteString( name, -1, false );
		}

		if( lobbyUserID != NULL )
		{
			lobby.SendReliableToLobbyUser( *lobbyUserID, GAME_RELIABLE_MESSAGE_DECL_REMAP, outMsg );
		}
		else
		{
			lobby.SendReliable( GAME_RELIABLE_MESSAGE_DECL_REMAP, outMsg, false );
		}
	}
}

/*
================
idGameLocal::ServerFlushDeclRemap

Broadcasts the announce table's slots not sent yet, as few GAME_RELIABLE_MESSAGE_DECL_REMAP batches as they fit in
================
*/
void idGameLocal::ServerFlushDeclRemap()
{
	ServerSendDeclRemap( numDeclRemapSent, numDeclRemap, NULL );
	numDeclRemapSent = numDeclRemap;
}

/*
================
idGameLocal::ClientReadDeclRemap

Takes one GAME_RELIABLE_MESSAGE_DECL_REMAP: each slot's name becomes this machine's decl of that type, made implicit
here too if it is not known yet. Only the five types that cross as indices are taken, since FindType stops the game on
an unregistered type.
================
*/
void idGameLocal::ClientReadDeclRemap( const idBitMsg& msg )
{
	// the idStr read keeps the name byte for byte: the char* one turns '%' into '.' and stops at a 0xFF byte
	idStr name;
	const int first = msg.ReadShort();
	for( int k = first; msg.GetRemainingData() > 0; k++ )
	{
		const int type = msg.ReadByte();
		msg.ReadString( name );
		if( k < 0 || k >= MAX_DECL_REMAP || name.Length() > MAX_DECL_REMAP_NAME || ( type != DECL_ENTITYDEF && type != DECL_MATERIAL && type != DECL_SKIN && type != DECL_SOUND && type != DECL_FX ) )
		{
			Warning( "ClientReadDeclRemap: slot %d of type %d ('%s') is not one this build takes, the rest of the batch dropped", k, type, name.c_str() );
			return;
		}
		const idDecl* decl = declManager->FindType( ( declType_t )type, name, true );
		declRemap[k].index = decl->Index();
		declRemap[k].type = type;
		numDeclRemap = Max( numDeclRemap, k + 1 );
		if( net_debugDeclRemap.GetInteger() > 0 )
		{
			Printf( "DECL_REMAP: slot %d: %s '%s' (local index %d%s)\n", k, declManager->GetDeclNameFromType( ( declType_t )type ), name.c_str(), decl->Index(), decl->IsImplicit() ? ", implicit" : "" );
		}
	}
}
//puncho end

//puncho start - version rule (MP-A0, MD13)
/*
================
DeclLayerDigests

One row per content layer for one decl type: a CRC32 of the explicit decls whose winning file that layer serves, each as
its index, its canonical name and the CRC32 of its text. Implicit decls (FindType with makeDefault and no definition in
any file) are left out: a machine appends them in the order it first asks for them, which follows its own rendering, UI
and map history, so two identical builds would disagree on them, and DeclManager.h promises consistent indices only for
the explicit ones. They cross through the announce table instead. The index is hashed with the name so a pair whose
explicit decls landed at different indices is still refused, and the text so a decl of the same name with other text is
too: an override in a layer, or the same file served from a .resources on one peer and loose on the other. The text CRC
is the one the decl stored when its text was set, '\r' dropped, so no text is decompressed here.
================
*/
static void DeclLayerDigests( declType_t type, layerDigest_t row, contentDigests_t& digests )
{
	unsigned int crcs[ MAX_MOD_LAYERS + 1 ];
	for( int layer = 0; layer < digests.numLayers; layer++ )
	{
		CRC32_InitChecksum( crcs[layer] );
	}
	const int numDecls = declManager->GetNumDecls( type );
	for( int i = 0; i < numDecls; i++ )
	{
		// no parse and no media: every decl object exists from the first BeginLevelLoad
		const idDecl* decl = declManager->DeclByIndex( type, i, false );
		if( decl->IsImplicit() )
		{
			continue;
		}
		// a file no layer holds any more (written by an editor, then removed) counts as base's
		const int declLayer = decl->GetLayer();
		const int layer = ( declLayer >= 0 && declLayer < digests.numLayers ) ? declLayer : 0;
		const int index = LittleLong( i );
		const char* name = decl->GetName();
		const unsigned int textCrc = LittleLong( decl->GetTextCRC() );
		CRC32_UpdateChecksum( crcs[layer], &index, sizeof( index ) );
		CRC32_UpdateChecksum( crcs[layer], name, idStr::Length( name ) + 1 );
		CRC32_UpdateChecksum( crcs[layer], &textCrc, sizeof( textCrc ) );
	}
	for( int layer = 0; layer < digests.numLayers; layer++ )
	{
		CRC32_FinishChecksum( crcs[layer] );
		digests.layers[layer][row] = crcs[layer];
	}
}

/*
================
idGameLocal::GetContentDigests

The content half of the version rule: net_checkVersion refuses a different build at connect, these refuse
different content once the map is loaded. Each category hashes what stands behind a number the network code
sends, from names and ids as stored (no pointers, no locale), so a Linux and a Windows build of one source agree.
The decl types are the ones sent as indices, through ServerRemapDecl / ClientRemapDecl or a raw Index():
entityDefs (snapshot spawns, damage defs), materials (collision surfaces), skins (weapons), sounds and FX.
The decls, the script and the map's entities are split by content layer, so a refusal names the mod; classes, the map's
geometry, the merged entities that spawn and the compiled script image stay global. The host pays for this once per
load, at the first peer's RELIABLE_LOADING_DONE, so its time goes to the developer print.
================
*/
void idGameLocal::GetContentDigests( contentDigests_t& digests ) const
{
	const uint64 startTime = Sys_Microseconds();

	memset( &digests, 0, sizeof( digests ) );
	digests.numLayers = Min( fileSystem->GetNumLayers(), MAX_MOD_LAYERS + 1 );

	unsigned int crc;
	CRC32_InitChecksum( crc );
	for( int i = 0; i < idClass::GetNumTypes(); i++ )
	{
		const char* name = idClass::GetType( i )->classname;
		CRC32_UpdateChecksum( crc, name, idStr::Length( name ) + 1 );
	}
	CRC32_FinishChecksum( crc );
	digests.global[ CONTENT_DIGEST_CLASSES ]		= crc;
	digests.global[ CONTENT_DIGEST_MAP_GEOMETRY ]	= ( mapFile != NULL ) ? mapFile->GetGeometryCRC() : 0;
	digests.global[ CONTENT_DIGEST_MAP_ENTITIES ]	= ( mapFile != NULL ) ? mapFile->GetEntitiesCRC() : 0;
	// the per-layer SCRIPT rows say which files were read; this proves both peers compiled the same image from them
	digests.global[ CONTENT_DIGEST_SCRIPT_IMAGE ]	= program.CalculateChecksum( false );

	digests.global[ CONTENT_DIGEST_WORLD ] = mapGraphDirector.WorldDigest();
	uint32 renderCRC;
	CRC32_InitChecksum( renderCRC );
	int renderValue = LittleLong( gameRenderWorld->NumAreas() );
	CRC32_UpdateChecksum( renderCRC, &renderValue, sizeof( renderValue ) );
	renderValue = LittleLong( gameRenderWorld->NumPortals() );
	CRC32_UpdateChecksum( renderCRC, &renderValue, sizeof( renderValue ) );
	for( int area = 0; area < gameRenderWorld->NumAreas(); area++ )
	{
		for( int portal = 0; portal < gameRenderWorld->NumPortalsInArea( area ); portal++ )
		{
			const exitPortal_t p = gameRenderWorld->GetPortal( area, portal );
			int pair[2] = { LittleLong( p.areas[0] ), LittleLong( p.areas[1] ) };
			CRC32_UpdateChecksum( renderCRC, pair, sizeof( pair ) );
		}
	}
	CRC32_FinishChecksum( renderCRC );
	digests.global[ CONTENT_DIGEST_RENDER_WORLD ] = renderCRC;

	DeclLayerDigests( DECL_ENTITYDEF, LAYER_DIGEST_ENTITYDEFS, digests );
	DeclLayerDigests( DECL_MATERIAL, LAYER_DIGEST_MATERIALS, digests );
	DeclLayerDigests( DECL_SKIN, LAYER_DIGEST_SKINS, digests );
	DeclLayerDigests( DECL_SOUND, LAYER_DIGEST_SOUNDS, digests );
	DeclLayerDigests( DECL_FX, LAYER_DIGEST_FX, digests );
	// entityDefs also cross in entityDefBits, a width InitAsyncNetwork takes from the explicit count and the table cap
	digests.layers[0][ LAYER_DIGEST_ENTITYDEFS ] ^= ( unsigned int )entityDefBits;

	unsigned int crcs[ MAX_MOD_LAYERS + 1 ];
	program.GetLayerChecksums( crcs, digests.numLayers );
	for( int layer = 0; layer < digests.numLayers; layer++ )
	{
		digests.layers[layer][ LAYER_DIGEST_SCRIPT ]		= crcs[layer];
		digests.layers[layer][ LAYER_DIGEST_MAP_ENTITIES ]	= ( mapFile != NULL ) ? mapFile->GetLayerEntitiesCRC( layer ) : 0;
	}

	DPrintf( "content digests: %d layers in %.2f ms\n", digests.numLayers, ( Sys_Microseconds() - startTime ) * 0.001f );
}
//puncho end

/*
================
idGameLocal::SyncPlayersWithLobbyUsers
================
*/
void idGameLocal::SyncPlayersWithLobbyUsers( bool initial )
{
	idLobbyBase& lobby = session->GetActingGameStateLobbyBase();
	if( !lobby.IsHost() )
	{
		return;
	}

	idStaticList< lobbyUserID_t, MAX_CLIENTS > newLobbyUsers;

	// First, loop over lobby users, and see if we find a lobby user that we haven't registered
	for( int i = 0; i < lobby.GetNumLobbyUsers(); i++ )
	{
		lobbyUserID_t lobbyUserID1 = lobby.GetLobbyUserIdByOrdinal( i );

		if( !lobbyUserID1.IsValid() )
		{
			continue;
		}

		if( !initial && !lobby.IsLobbyUserLoaded( lobbyUserID1 ) )
		{
			continue;
		}

		// Now, see if we find this lobby user in our list
		bool found = false;

		for( int j = 0; j < MAX_PLAYERS; j++ )
		{
			idPlayer* player = static_cast<idPlayer*>( entities[ j ] );
			if( player == NULL )
			{
				continue;
			}

			lobbyUserID_t lobbyUserID2 = lobbyUserIDs[j];

			if( lobbyUserID1 == lobbyUserID2 )
			{
				found = true;
				break;
			}
		}

		if( !found )
		{
			// If we didn't find it, we need to create a player and assign it to this new lobby user
			newLobbyUsers.Append( lobbyUserID1 );
		}
	}

	// Validate connected players
	for( int i = 0; i < MAX_PLAYERS; i++ )
	{
		idPlayer* player = static_cast<idPlayer*>( entities[ i ] );
		if( player == NULL )
		{
			continue;
		}

		lobbyUserID_t lobbyUserID = lobbyUserIDs[i];

		if( !lobby.IsLobbyUserValid( lobbyUserID ) )
		{
			//puncho start - P0-m8: the count behind the one warning, then a clean slate for the next client in this slot
			if( hitClaimRefusals[ i ] > 1 )
			{
				Printf( "CLIENT_HITSCAN_HIT: %d claims from client %d refused on this map, the first one printed\n", hitClaimRefusals[ i ], i );
			}
			hitClaimRefusals[ i ] = 0;
			//puncho end
			delete entities[ i ];
			mpGame.DisconnectClient( i );
			lobbyUserIDs[i] = lobbyUserID_t();
			continue;
		}

		lobby.EnableSnapshotsForLobbyUser( lobbyUserID );
	}

	while( newLobbyUsers.Num() > 0 )
	{
		// Find a free player data slot to use for this new player
		int freePlayerDataIndex = -1;

		for( int i = 0; i < MAX_PLAYERS; ++i )
		{
			idPlayer* player = static_cast<idPlayer*>( entities[ i ] );
			if( player == NULL )
			{
				freePlayerDataIndex = i;
				break;
			}
		}
		if( freePlayerDataIndex == -1 )
		{
			break;			// No player data slots (this shouldn't happen)
		}
		lobbyUserID_t lobbyUserID = newLobbyUsers[0];
		newLobbyUsers.RemoveIndex( 0 );

		mpGame.ServerClientConnect( freePlayerDataIndex );
		Printf( "client %d connected.\n", freePlayerDataIndex );
		hitClaimRefusals[ freePlayerDataIndex ] = 0;	//puncho - P0-m8: counted per connect

		lobbyUserIDs[ freePlayerDataIndex ] = lobbyUserID;

		// Clear this player's old usercmds.
		common->ResetPlayerInput( freePlayerDataIndex );

		common->UpdateLevelLoadPacifier();


		// spawn the player
		SpawnPlayer( freePlayerDataIndex );

		common->UpdateLevelLoadPacifier();

		ServerWriteInitialReliableMessages( freePlayerDataIndex, lobbyUserID );
	}
}

/*
================
idGameLocal::ServerSendNetworkSyncCvars
================
*/
void idGameLocal::ServerSendNetworkSyncCvars()
{
	if( ( cvarSystem->GetModifiedFlags() & CVAR_NETWORKSYNC ) == 0 )
	{
		return;
	}
	cvarSystem->ClearModifiedFlags( CVAR_NETWORKSYNC );

	idBitMsg	outMsg;
	byte		msgBuf[MAX_GAME_MESSAGE_SIZE];

	idLobbyBase& lobby = session->GetActingGameStateLobbyBase();

	outMsg.InitWrite( msgBuf, sizeof( msgBuf ) );
	outMsg.BeginWriting();
	idDict syncedCvars;
	cvarSystem->MoveCVarsToDict( CVAR_NETWORKSYNC, syncedCvars, true );
	outMsg.WriteDeltaDict( syncedCvars, NULL );
	lobby.SendReliable( GAME_RELIABLE_MESSAGE_SYNCEDCVARS, outMsg, false );

	idLib::Printf( "Sending networkSync cvars:\n" );
	syncedCvars.Print();
}

/*
================
idGameLocal::ServerWriteInitialReliableMessages

  Send reliable messages to initialize the client game up to a certain initial state.
================
*/
void idGameLocal::ServerWriteInitialReliableMessages( int clientNum, lobbyUserID_t lobbyUserID )
{
	if( clientNum == GetLocalClientNum() )
	{
		// We don't need to send messages to ourself
		return;
	}

	idBitMsg	outMsg;
	byte		msgBuf[MAX_GAME_MESSAGE_SIZE];

	idLobbyBase& lobby = session->GetActingGameStateLobbyBase();

	outMsg.InitWrite( msgBuf, sizeof( msgBuf ) );
	outMsg.BeginWriting();
	idDict syncedCvars;
	cvarSystem->MoveCVarsToDict( CVAR_NETWORKSYNC, syncedCvars, true );
	outMsg.WriteDeltaDict( syncedCvars, NULL );
	lobby.SendReliableToLobbyUser( lobbyUserID, GAME_RELIABLE_MESSAGE_SYNCEDCVARS, outMsg );

	idLib::Printf( "Sending initial networkSync cvars:\n" );
	syncedCvars.Print();

	//puncho start - content check (P1-A3a, K19): the whole implicit decl announce table, before the saved events, which
	// were stored with their indices already encoded. MAX_DECL_REMAP_BYTES keeps it inside the reliable queue
	ServerSendDeclRemap( 0, numDeclRemap, &lobbyUserID );
	//puncho end

	// send all saved events
	for( entityNetEvent_t* event = savedEventQueue.Start(); event; event = event->next )
	{
		outMsg.InitWrite( msgBuf, sizeof( msgBuf ) );
		outMsg.BeginWriting();
		outMsg.WriteBits( event->spawnId, 32 );
		outMsg.WriteByte( event->event );
		outMsg.WriteLong( event->time );
		outMsg.WriteBits( event->paramsSize, idMath::BitsForInteger( MAX_EVENT_PARAM_SIZE ) );
		if( event->paramsSize )
		{
			outMsg.WriteData( event->paramsBuf, event->paramsSize );
		}
		lobby.SendReliableToLobbyUser( lobbyUserID, GAME_RELIABLE_MESSAGE_EVENT, outMsg );
	}

	mpGame.ServerWriteInitialReliableMessages( clientNum, lobbyUserID );
}

/*
================
idGameLocal::SaveEntityNetworkEvent
================
*/
void idGameLocal::SaveEntityNetworkEvent( const idEntity* ent, int eventId, const idBitMsg* msg )
{
	entityNetEvent_t* event = savedEventQueue.Alloc();
	event->spawnId = GetSpawnId( ent );
	event->event = eventId;
	event->time = time;
	if( msg )
	{
		event->paramsSize = msg->GetSize();
		memcpy( event->paramsBuf, msg->GetReadData(), msg->GetSize() );
	}
	else
	{
		event->paramsSize = 0;
	}

	savedEventQueue.Enqueue( event, idEventQueue::OUTOFORDER_IGNORE );
}

/*
================
idGameLocal::ServerWriteSnapshot

  Write a snapshot of the current game state
================
*/
void idGameLocal::ServerWriteSnapshot( idSnapShot& ss )
{

	ss.SetTime( fast.time );

	//puncho start - stream-trace (P1-A1c, K4): NULL unless net_snapTrace is set; the marks below scope each object's
	// writes
	idSnapshotTrace* const snapTrace = snapshotTrace.Start( true, ss.GetTime() );
	//puncho end

	//puncho start - content check (P1-A3a, K19): the slots this snapshot announces go out at its end
	const bool declRemapWasDeferred = declRemapDeferred;
	declRemapDeferred = true;
	//puncho end

	byte buffer[ MAX_ENTITY_STATE_SIZE ];
	idBitMsg msg;

	// First write the generic game state to the snapshot
	msg.InitWrite( buffer, sizeof( buffer ) );
	//puncho start - stream-trace
	if( snapTrace != NULL )
	{
		snapTrace->Object( SNAP_GAMESTATE, "<gamestate>", msg, false );
	}
	//puncho end
	mpGame.WriteToSnapshot( msg );
	ss.S_AddObject( SNAP_GAMESTATE, ~0U, msg, "Game State" );

	// Update global shader parameters
	msg.InitWrite( buffer, sizeof( buffer ) );
	//puncho start - stream-trace
	if( snapTrace != NULL )
	{
		snapTrace->Object( SNAP_SHADERPARMS, "<shaderparms>", msg, false );
	}
	//puncho end
	for( int i = 0; i < MAX_GLOBAL_SHADER_PARMS; i++ )
	{
		msg.WriteFloat( globalShaderParms[i] );
	}
	ss.S_AddObject( SNAP_SHADERPARMS, ~0U, msg, "Shader Parms" );

	// update portals for opened doors
	msg.InitWrite( buffer, sizeof( buffer ) );
	//puncho start - stream-trace
	if( snapTrace != NULL )
	{
		snapTrace->Object( SNAP_PORTALS, "<portals>", msg, false );
	}
	//puncho end
	int numPortals = gameRenderWorld->NumPortals();
	msg.WriteLong( numPortals );
	for( int i = 0; i < Min( numPortals, PORTALS_PER_SNAP_OBJECT ); i++ )
	{
		msg.WriteBits( gameRenderWorld->GetPortalState( ( qhandle_t )( i + 1 ) ) , NUM_RENDER_PORTAL_BITS );
	}
	ss.S_AddObject( SNAP_PORTALS, ~0U, msg, "Portal State" );
	for( int chunk = 1; chunk * PORTALS_PER_SNAP_OBJECT < numPortals; chunk++ )
	{
		msg.InitWrite( buffer, sizeof( buffer ) );
		if( snapTrace != NULL )
		{
			snapTrace->Object( SNAP_PORTALS_EXTRA + chunk - 1, "<portals-extra>", msg, false );
		}
		for( int i = chunk * PORTALS_PER_SNAP_OBJECT; i < Min( numPortals, ( chunk + 1 ) * PORTALS_PER_SNAP_OBJECT ); i++ )
		{
			msg.WriteBits( gameRenderWorld->GetPortalState( i + 1 ), NUM_RENDER_PORTAL_BITS );
		}
		ss.S_AddObject( SNAP_PORTALS_EXTRA + chunk - 1, ~0U, msg, "Portal State Extra" );
	}


	idEntity* skyEnt = portalSkyEnt.GetEntity();
	pvsHandle_t	portalSkyPVS;
	portalSkyPVS.i = -1;
	if( skyEnt != NULL )
	{
		portalSkyPVS = pvs.SetupCurrentPVS( skyEnt->GetPVSAreas(), skyEnt->GetNumPVSAreas() );
	}

	// Build PVS data for each player and write their player state to the snapshot as well
	pvsHandle_t pvsHandles[ MAX_PLAYERS ];
	for( int i = 0; i < MAX_PLAYERS; i++ )
	{
		idPlayer* player = static_cast<idPlayer*>( entities[ i ] );
		if( player == NULL )
		{
			pvsHandles[i].i = -1;
			continue;
		}
		idPlayer* spectated = player;
		if( player->spectating && player->spectator != i && entities[ player->spectator ] )
		{
			spectated = static_cast< idPlayer* >( entities[ player->spectator ] );
		}

		msg.InitWrite( buffer, sizeof( buffer ) );
		//puncho start - stream-trace
		if( snapTrace != NULL )
		{
			snapTrace->Object( SNAP_PLAYERSTATE + i, "<playerstate>", msg, false );
		}
		//puncho end
		spectated->WritePlayerStateToSnapshot( msg );
		ss.S_AddObject( SNAP_PLAYERSTATE + i, ~0U, msg, "Player State" );

		int sourceAreas[ idEntity::MAX_PVS_AREAS ];
		int numSourceAreas = gameRenderWorld->BoundsInAreas( spectated->GetPlayerPhysics()->GetAbsBounds(), sourceAreas, idEntity::MAX_PVS_AREAS );
		pvsHandles[i] = pvs.SetupCurrentPVS( sourceAreas, numSourceAreas, PVS_NORMAL );
		if( portalSkyPVS.i >= 0 )
		{
			pvsHandle_t	tempPVS = pvs.MergeCurrentPVS( pvsHandles[i], portalSkyPVS );
			pvs.FreeCurrentPVS( pvsHandles[i] );
			pvsHandles[i] = tempPVS;
		}

		// Write the last usercmd processed by the server so that clients know
		// when to stop predicting.
		msg.BeginWriting();
		//puncho start - stream-trace
		if( snapTrace != NULL )
		{
			snapTrace->Object( SNAP_LAST_CLIENT_FRAME + i, "<lastclientframe>", msg, false );
		}
		//puncho end
		msg.WriteLong( usercmdLastClientMilliseconds[i] );
		ss.S_AddObject( SNAP_LAST_CLIENT_FRAME + i, ~0U, msg, "Last client frame" );
	}

	if( portalSkyPVS.i >= 0 )
	{
		pvs.FreeCurrentPVS( portalSkyPVS );
	}

	//puncho start - ai-harness (plans/ai-harness/00-PLAN.md §2.6): the idAI share of the snapshot. The Optick scope
	// lives to the end of the function (only the PVS frees follow the loop) so the loop keeps its shape; usWrite
	// is the explicit pair around the loop alone. usMinion accumulates clock ticks in an idTimer because one
	// header-only entity is below the microsecond clock's resolution (the timer_singlethink pattern).
	// Counts always on, the two clocks only under ai_profile (D10).
	PROFILE_STATIC_DESC( descSnap, "Net::MinionSnapshot", ::Optick::Category::Network );
	SCOPED_PROFILE_EVENT_DESC( descSnap );
	const bool		profileSnap = ai_profile.GetInteger() != 0;
	idTimer			timerMinion;
	int				snapObjects = 0;
	int				snapMinionObjects = 0;
	int				snapMinionBytes = 0;
	const uint64	usWriteStart = profileSnap ? Sys_Microseconds() : 0;
	//puncho end

	// Add all entities to the snapshot
	for( idEntity* ent = spawnedEntities.Next(); ent != NULL; ent = ent->spawnNode.Next() )
	{
		if( ent->GetSkipReplication() )
		{
			continue;
		}

		//puncho start - ai-harness
		const bool isMinion = ent->IsType( idAI::Type );
		if( isMinion && profileSnap )
		{
			timerMinion.Start();
		}
		//puncho end

		msg.InitWrite( buffer, sizeof( buffer ) );
		//puncho start - stream-trace
		if( snapTrace != NULL )
		{
			snapTrace->Object( SNAP_ENTITIES + ent->entityNumber, ent->GetClassname(), msg, false );
		}
		//puncho end
		msg.WriteBits( spawnIds[ ent->entityNumber ], 32 - GENTITYNUM_BITS );
		msg.WriteBits( ent->GetType()->typeNum, idClass::GetTypeNumBits() );
		msg.WriteBits( ServerRemapDecl( -1, DECL_ENTITYDEF, ent->entityDefNumber ), entityDefBits );

		msg.WriteBits( ent->GetPredictedKey(), 32 );

		//puncho - F4-m1: say whether the class data follows. A client cannot tell from the size, since the header
		// rounds up to whole bytes, and its own copy's networkSync can differ (a net_instanthit projectile)
		msg.WriteBool( ent->fl.networkSync );
		if( ent->fl.networkSync )
		{
			// write the class specific data to the snapshot
			ent->WriteToSnapshot( msg );
		}

		ss.S_AddObject( SNAP_ENTITIES + ent->entityNumber, ~0U, msg, ent->GetName() );

		//puncho start - ai-harness
		snapObjects++;
		if( isMinion )
		{
			if( profileSnap )
			{
				timerMinion.Stop();
			}
			snapMinionObjects++;
			snapMinionBytes += msg.GetSize();
		}
		//puncho end
	}

	if( worldStore.IsActive() && session->GetActingGameStateLobbyBase().GetNumConnectedPeers() > 0 )
	{
		for( int chunk = 0; chunk < MAP_REMOVED_CHUNKS; chunk++ )
		{
			const int first = chunk * 500;
			const int bytes = Min( 500, ( int )sizeof( worldStore.removed ) - first );
			bool any = false;
			for( int i = 0; i < bytes; i++ ) any |= worldStore.removed[first+i] != 0;
			if( !any )
			{
				continue;
			}
			msg.InitWrite( buffer, sizeof( buffer ) );
			msg.BeginWriting();
			msg.WriteData( worldStore.removed + first, bytes );
			ss.S_AddObject( SNAP_MAP_REMOVED + chunk, ~0U, msg, "Removed map entities" );
		}
	}

	//puncho start - ai-harness: written on the main thread between game frames (SendSnapshots is the only caller);
	// RunFrame copies the block at the top of the next tic. The two tags go on the Net::MinionSnapshot event.
	snapStats.objects = snapObjects;
	snapStats.minionObjects = snapMinionObjects;
	snapStats.minionBytes = snapMinionBytes;
	snapStats.usWrite = profileSnap ? ( int )( Sys_Microseconds() - usWriteStart ) : 0;
	snapStats.usMinion = profileSnap ? ( int )( timerMinion.Milliseconds() * 1000.0f ) : 0;
	snapStats.sequence++;
	{
		PROFILE_EVENT_TAG( "minionObjects", snapStats.minionObjects );
	}
	{
		PROFILE_EVENT_TAG( "minionBytes", snapStats.minionBytes );
	}
	//puncho end

	// Free PVS handles for all the players
	for( int i = 0; i < MAX_PLAYERS; i++ )
	{
		if( pvsHandles[i].i < 0 )
		{
			continue;
		}
		pvs.FreeCurrentPVS( pvsHandles[i] );
	}

	//puncho start - stream-trace
	if( snapTrace != NULL )
	{
		snapTrace->End();
	}
	//puncho end

	//puncho start - content check (P1-A3a, K19): broadcast the slots this snapshot announced. The reliables are queued
	// before the session submits the snapshot, so every packet that carries it carries them too, ahead of it, until acked
	ServerFlushDeclRemap();
	declRemapDeferred = declRemapWasDeferred;
	//puncho end
}

/*
================
idGameLocal::NetworkEventWarning
================
*/
void idGameLocal::NetworkEventWarning( const entityNetEvent_t* event, const char* fmt, ... )
{
	char buf[1024];
	int length = 0;
	va_list argptr;

	int entityNum	= event->spawnId & ( ( 1 << GENTITYNUM_BITS ) - 1 );
	int id			= event->spawnId >> GENTITYNUM_BITS;

	length += idStr::snPrintf( buf + length, sizeof( buf ) - 1 - length, "event %d for entity %d %d: ", event->event, entityNum, id );
	va_start( argptr, fmt );
	length = idStr::vsnPrintf( buf + length, sizeof( buf ) - 1 - length, fmt, argptr );
	va_end( argptr );
	idStr::Append( buf, sizeof( buf ), "\n" );

	common->DWarning( buf );
}

/*
================
idGameLocal::ServerProcessEntityNetworkEventQueue
================
*/
void idGameLocal::ServerProcessEntityNetworkEventQueue()
{
	while( eventQueue.Start() )
	{
		entityNetEvent_t* event = eventQueue.Start();

		if( event->time > time )
		{
			break;
		}

		idEntityPtr< idEntity > entPtr;

		if( !entPtr.SetSpawnId( event->spawnId ) )
		{
			NetworkEventWarning( event, "Entity does not exist any longer, or has not been spawned yet." );
		}
		else
		{
			idEntity* ent = entPtr.GetEntity();
			assert( ent );

			idBitMsg eventMsg;
			eventMsg.InitRead( event->paramsBuf, sizeof( event->paramsBuf ) );
			eventMsg.SetSize( event->paramsSize );
			eventMsg.BeginReading();
			if( !ent->ServerReceiveEvent( event->event, event->time, eventMsg ) )
			{
				NetworkEventWarning( event, "unknown event" );
			}
		}

		entityNetEvent_t* freedEvent = eventQueue.Dequeue();
		verify( freedEvent == event );
		eventQueue.Free( event );
	}
}

/*
================
idGameLocal::ProcessReliableMessage
================
*/
void idGameLocal::ProcessReliableMessage( int clientNum, int type, const idBitMsg& msg )
{
	if( session->GetActingGameStateLobbyBase().IsPeer() )
	{
		ClientProcessReliableMessage( type, msg );
	}
	else
	{
		ServerProcessReliableMessage( clientNum, type, msg );
	}
}

/*
================
idGameLocal::ServerProcessReliableMessage
================
*/
void idGameLocal::ServerProcessReliableMessage( int clientNum, int type, const idBitMsg& msg )
{
	if( clientNum < 0 )
	{
		return;
	}
	switch( type )
	{
		case GAME_RELIABLE_MESSAGE_TRANSITION:
		case GAME_RELIABLE_MESSAGE_PREFETCH:
			return; // Host alone controls map intent.

		case GAME_RELIABLE_MESSAGE_CHAT:
		case GAME_RELIABLE_MESSAGE_TCHAT:
		{
			char name[128];
			char text[128];

			msg.ReadString( name, sizeof( name ) );
			msg.ReadString( text, sizeof( text ) );

			mpGame.ProcessChatMessage( clientNum, type == GAME_RELIABLE_MESSAGE_TCHAT, name, text, NULL );
			break;
		}
		case GAME_RELIABLE_MESSAGE_VCHAT:
		{
			int index = msg.ReadLong();
			bool team = msg.ReadBits( 1 ) != 0;
			mpGame.ProcessVoiceChat( clientNum, team, index );
			break;
		}
		case GAME_RELIABLE_MESSAGE_DROPWEAPON:
		{
			mpGame.DropWeapon( clientNum );
			break;
		}
		case GAME_RELIABLE_MESSAGE_EVENT:
		{
			// allocate new event
			entityNetEvent_t* event = eventQueue.Alloc();
			eventQueue.Enqueue( event, idEventQueue::OUTOFORDER_DROP );

			event->spawnId = msg.ReadBits( 32 );
			event->event = msg.ReadByte();
			event->time = msg.ReadLong();

			event->paramsSize = msg.ReadBits( idMath::BitsForInteger( MAX_EVENT_PARAM_SIZE ) );
			if( event->paramsSize )
			{
				if( event->paramsSize > MAX_EVENT_PARAM_SIZE )
				{
					NetworkEventWarning( event, "invalid param size" );
					return;
				}
				msg.ReadByteAlign();
				msg.ReadData( event->paramsBuf, event->paramsSize );
			}
			break;
		}
		case GAME_RELIABLE_MESSAGE_SPECTATE:
		{
			bool spec = msg.ReadBool();
			idPlayer* player = GetClientByNum( clientNum );
			if( serverInfo.GetBool( "si_spectators" ) )
			{
				// never let spectators go back to game while sudden death is on
				if( mpGame.GetGameState() == idMultiplayerGame::SUDDENDEATH && !spec && player->wantSpectate )
				{
					// Don't allow the change
				}
				else
				{
					if( player->wantSpectate && !spec )
					{
						player->forceRespawn = true;
					}
					player->wantSpectate = spec;
				}
			}
			else
			{
				// If the server turned off si_spectators while a player is spectating, then any spectate message forces the player out of spectate mode
				if( player->wantSpectate )
				{
					player->forceRespawn = true;
				}
				player->wantSpectate = false;
			}
			break;
		}
		case GAME_RELIABLE_MESSAGE_CLIENT_HITSCAN_HIT:
		{
			const int attackerNum = msg.ReadShort();
			const int victimNum = msg.ReadShort();
			idVec3 dir;
			msg.ReadVectorFloat( dir );
			const int damageDefIndex = msg.ReadLong();
			const float damageScale = msg.ReadFloat();
			const int location = msg.ReadLong();

			//puncho start - MP-C7 (plans/00-MASTER-PLAN.md stream C): the claim is client data. Only the sender
			// may be the attacker, both ends must be players before the casts below, and every index is checked
			// before it is used: a bad damageDefIndex reaches DeclByIndex's common->Error and drops the host.
			// INVALID_JOINT stays a silent refusal further down, since splash on a player sends it legitimately
			//
			// A claim that can be applied (any joint but INVALID_JOINT) also has its scale and direction bounded. Such a
			// claim comes from idProjectile::Collide or its beam (melee runs on the server, splash sends INVALID_JOINT):
			// damagePower, 1.0 from every stock weapon script but the BFG, whose power ammo caps it at its clip
			// (idWeapon::Event_LaunchProjectiles, weapon_bfg "clipSize" 4), times berserk's PROJECTILE_DAMAGE 2.0
			// (idPlayer::PowerUpModifier): 8 at most in stock data. The bound sits well above that on purpose: it only
			// stops garbage, since any scale in range can still kill in one shot (tying the scale to the weapon is
			// MP-C7'), and a weapon def edited past stock values must not have its hits refused. The direction is the
			// projectile's normalized velocity
			const float maxDamageScale = 64.0f;
			const float dirLengthSqr = dir.LengthSqr();
			const char* refused = NULL;
			if( attackerNum != clientNum )
			{
				refused = "attacker is not the sender";
			}
			else if( victimNum < 0 || victimNum >= MAX_CLIENTS || gameLocal.entities[victimNum] == NULL || !gameLocal.entities[victimNum]->IsType( idPlayer::Type ) )
			{
				refused = "victim is not a player";
			}
			else if( gameLocal.entities[attackerNum] == NULL || !gameLocal.entities[attackerNum]->IsType( idPlayer::Type ) )
			{
				refused = "attacker is not a player";
			}
			else if( location != INVALID_JOINT && ( location < 0 || location >= gameLocal.entities[victimNum]->GetRenderEntity()->numJoints ) )
			{
				refused = "joint out of range";
			}
			else if( damageDefIndex < 0 || damageDefIndex >= declManager->GetNumDecls( DECL_ENTITYDEF ) )
			{
				refused = "damage def out of range";
			}
			//puncho start - content check (P1-A3a, K20): the claim names its damage def by explicit index, the one
			// numbering MP-A0's digests prove shared. An implicit index on this host is no decl the client meant
			// (idPlayer::Damage sends -1 for an implicit damage def)
			else if( declManager->DeclByIndex( DECL_ENTITYDEF, damageDefIndex, false )->IsImplicit() )
			{
				refused = "damage def is implicit";
			}
			//puncho end
			else if( location != INVALID_JOINT && ( IEEE_FLT_IS_INF_NAN( damageScale ) || damageScale < 0.0f || damageScale > maxDamageScale ) )
			{
				refused = "damage scale out of range";
			}
			else if( location != INVALID_JOINT && ( IEEE_FLT_IS_INF_NAN( dirLengthSqr ) || dirLengthSqr < 1e-6f ) )
			{
				refused = "direction not finite or zero";
			}
			if( refused != NULL )
			{
				//puncho start - P0-m8: a peer can send these in a loop, so its first refusal warns and the rest are counted
				// (printed when it disconnects or the map ends)
				const int refusals = ( clientNum < MAX_CLIENTS ) ? ++hitClaimRefusals[ clientNum ] : 1;
				if( refusals == 1 )
				{
					common->Warning( "CLIENT_HITSCAN_HIT from client %d refused, %s (attacker %d, victim %d, joint %d, damage def %d, scale %g, dir %g %g %g); further refusals are counted", clientNum, refused, attackerNum, victimNum, location, damageDefIndex, damageScale, dir.x, dir.y, dir.z );
				}
				//puncho end
				break;
			}
			//puncho end

			idPlayer& victim = static_cast< idPlayer& >( *gameLocal.entities[victimNum] );
			idPlayer& attacker = static_cast< idPlayer& >( *gameLocal.entities[attackerNum] );

			if( victim.GetPhysics() == NULL )
			{
				break;
			}

			if( attacker.weapon.GetEntity() == NULL )
			{
				break;
			}

			if( location == INVALID_JOINT )
			{
				break;
			}

			// Line of sight check. As a basic precaution against cheating,
			// the server performs a ray intersection from the client's position
			// to the joint he hit on the target.
			idVec3 muzzleOrigin;
			idMat3 muzzleAxis;

			attacker.weapon.GetEntity()->GetProjectileLaunchOriginAndAxis( muzzleOrigin, muzzleAxis );

			idVec3 targetLocation = victim.GetRenderEntity()->origin + victim.GetRenderEntity()->joints[location].ToVec3() * victim.GetRenderEntity()->axis;

			trace_t tr;
			gameLocal.clip.Translation( tr, muzzleOrigin, targetLocation, NULL, mat3_identity, MASK_SHOT_RENDERMODEL, &attacker );

			idEntity* hitEnt = gameLocal.entities[ tr.c.entityNum ];
			if( hitEnt != &victim )
			{
				break;
			}
			const idDeclEntityDef* damageDef = static_cast<const idDeclEntityDef*>( declManager->DeclByIndex( DECL_ENTITYDEF, damageDefIndex, false ) );

			if( damageDef != NULL )
			{
				dir *= idMath::InvSqrt( dirLengthSqr );	//puncho - MP-C7: knockback normalizes its copy, but Killed hands dir to Gib as it is
				victim.Damage( NULL, gameLocal.entities[attackerNum], dir, damageDef->GetName(), damageScale, location );
			}
			break;
		}
		default:
		{
			Warning( "Unknown reliable message (%d) from client %d", type, clientNum );
			break;
		}
	}
}

/*
================
idGameLocal::ClientReadSnapshot
================
*/
void idGameLocal::ClientReadSnapshot( const idSnapShot& ss )
{
	if( GetLocalClientNum() < 0 )
	{
		return;
	}

	//puncho start - content check (P1-A3a, K19): the announce table's slots this snapshot may use came in as reliables,
	// ahead of it in the same or an earlier packet, but game reliables otherwise run after the snapshots of the frame
	commonLocal.ExecuteReliableMessagesOfType( GAME_RELIABLE_MESSAGE_DECL_REMAP );
	//puncho end

	// if prediction is off, enable local client smoothing
	//localPlayer->SetSelfSmooth( dupeUsercmds > 2 );

	// clear any debug lines from a previous frame
	gameRenderWorld->DebugClearLines( time );

	// clear any debug polygons from a previous frame
	gameRenderWorld->DebugClearPolygons( time );

	SelectTimeGroup( false );

	// so that StartSound/StopSound doesn't risk skipping
	isNewFrame = true;

	// clear the snapshot entity list
	snapshotEntities.Clear();

	//puncho start - stream-trace (P1-A1c, K4): NULL unless net_snapTrace is set; each object's reads are scoped
	// below
	idSnapshotTrace* const snapTrace = snapshotTrace.Start( false, ss.GetTime() );
	//puncho end

	// read all entities from the snapshot
	for( int o = 0; o < ss.NumObjects(); o++ )
	{
		idBitMsg msg;
		int snapObjectNum = ss.GetObjectMsgByIndex( o, msg );
		if( snapObjectNum < 0 )
		{
			assert( false );
			continue;
		}
		//puncho start - stream-trace: the class is known only once the header is read
		if( snapTrace != NULL )
		{
			snapTrace->Object( snapObjectNum, NULL, msg, ss.ObjectIsStaleByIndex( o ) );
		}
		//puncho end
		if( snapObjectNum == SNAP_GAMESTATE )
		{
			mpGame.ReadFromSnapshot( msg );
			continue;
		}
		if( snapObjectNum == SNAP_SHADERPARMS )
		{
			for( int i = 0; i < MAX_GLOBAL_SHADER_PARMS; i++ )
			{
				globalShaderParms[i] = msg.ReadFloat();
			}
			continue;
		}
		if( snapObjectNum == SNAP_PORTALS )
		{
			// update portals for opened doors
			int numPortals = msg.ReadLong();
			if( numPortals != gameRenderWorld->NumPortals() || numPortals < 0 || numPortals > PORTALS_PER_SNAP_OBJECT * MAX_PORTAL_CHUNKS )
			{
				Error( "snapshot portal count %d, this map has %d", numPortals, gameRenderWorld->NumPortals() );
			}
			for( int i = 0; i < Min( numPortals, PORTALS_PER_SNAP_OBJECT ); i++ )
			{
				gameRenderWorld->SetPortalState( ( qhandle_t )( i + 1 ), msg.ReadBits( NUM_RENDER_PORTAL_BITS ) );
			}
			continue;
		}
		if( snapObjectNum >= SNAP_PLAYERSTATE && snapObjectNum < SNAP_PLAYERSTATE_END )
		{
			int playerNumber = snapObjectNum - SNAP_PLAYERSTATE;
			idPlayer* otherPlayer = static_cast< idPlayer* >( entities[ playerNumber ] );

			// Don't process Player Snapshots that are disconnected.
			const int lobbyIndex = session->GetActingGameStateLobbyBase().GetLobbyUserIndexFromLobbyUserID( lobbyUserIDs[ playerNumber ] );
			if( lobbyIndex < 0 || session->GetActingGameStateLobbyBase().IsLobbyUserConnected( lobbyIndex ) == false )
			{
				continue;
			}

			if( otherPlayer != NULL )
			{
				otherPlayer->ReadPlayerStateFromSnapshot( msg );
				if( otherPlayer != entities[ GetLocalClientNum() ] )    // This happens when we spectate another player
				{
					idWeapon* weap = otherPlayer->weapon.GetEntity();
					if( weap && ( weap->GetRenderEntity()->bounds[0] == weap->GetRenderEntity()->bounds[1] ) )
					{
						// update the weapon's viewmodel bounds so that the model doesn't flicker in the spectator's view
						weap->GetAnimator()->GetBounds( gameLocal.time, weap->GetRenderEntity()->bounds );
						weap->UpdateVisuals();
					}
				}
			}
			continue;
		}
		if( snapObjectNum >= SNAP_LAST_CLIENT_FRAME && snapObjectNum < SNAP_LAST_CLIENT_FRAME_END )
		{
			int playerNumber = snapObjectNum - SNAP_LAST_CLIENT_FRAME;

			// Don't process Player Snapshots that are disconnected.
			const int lobbyIndex = session->GetActingGameStateLobbyBase().GetLobbyUserIndexFromLobbyUserID( lobbyUserIDs[ playerNumber ] );
			if( lobbyIndex < 0 || session->GetActingGameStateLobbyBase().IsLobbyUserConnected( lobbyIndex ) == false )
			{
				continue;
			}

			usercmdLastClientMilliseconds[playerNumber] = msg.ReadLong();
			continue;
		}
		if( snapObjectNum >= SNAP_PORTALS_EXTRA && snapObjectNum < SNAP_PORTALS_EXTRA + MAX_PORTAL_CHUNKS - 1 )
		{
			const int first = ( snapObjectNum - SNAP_PORTALS_EXTRA + 1 ) * PORTALS_PER_SNAP_OBJECT;
			const int count = gameRenderWorld->NumPortals();
			if( first >= count )
			{
				Error( "snapshot portal chunk starts at %d, this map has %d", first, count );
			}
			for( int i = first; i < Min( count, first + PORTALS_PER_SNAP_OBJECT ); i++ )
			{
				gameRenderWorld->SetPortalState( i + 1, msg.ReadBits( NUM_RENDER_PORTAL_BITS ) );
			}
			continue;
		}
		if( snapObjectNum >= SNAP_MAP_REMOVED && snapObjectNum < SNAP_MAP_REMOVED + MAP_REMOVED_CHUNKS )
		{
			const int first = ( snapObjectNum - SNAP_MAP_REMOVED ) * 500;
			const int bytes = Min( 500, ( int )sizeof( worldStore.removed ) - first );
			//puncho start - P2.4 (bug review 2026-10-28): a deleted object arrives with size 0, as for entities. The host
			// deletes a chunk when its bits clear, which BeginSpawn does on a world-mode LocalMapRestart; that is a clear,
			// not a malformed message, and the Error dropped the client
			if( msg.GetSize() == 0 )
			{
				memset( worldStore.removed + first, 0, bytes );
				continue;
			}
			//puncho end
			if( !worldStore.IsActive() || msg.GetSize() != bytes )
			{
				Error( "Invalid removed-map snapshot" );
			}
			for( int i = 0; i < bytes; i++ )
			{
				const byte bits = msg.ReadByte();
				const byte fresh = bits & ~worldStore.removed[first+i];
				worldStore.removed[first+i] |= bits;
				for( int bit = 0; bit < 8; bit++ )
				{
					//puncho - P2.4 (bug review 2026-10-28): the bit is a map ordinal; the row is this client's spawn of it, if any
					const int index = ( fresh & ( 1 << bit ) ) ? worldStore.FindSpawnRow( ( first + i ) * 8 + bit ) : -1;
					if( index < 0 )
					{
						continue;
					}
					const mapSpawnRow_t& row = worldStore.mapSpawned[index];
					idEntity* ent = entities[row.entityNumber];
					if( ent != NULL && GetSpawnId( ent ) == row.spawnId )
					{
						delete ent;
					}
				}
			}
			continue;
		}
		if( snapObjectNum < SNAP_ENTITIES || snapObjectNum >= SNAP_ENTITIES_END )
		{
			continue;
		}

		int entityNumber = snapObjectNum - SNAP_ENTITIES;

		if( msg.GetSize() == 0 )
		{
			delete entities[entityNumber];
			continue;
		}

		bool debug = false;

		int spawnId = msg.ReadBits( 32 - GENTITYNUM_BITS );
		int typeNum = msg.ReadBits( idClass::GetTypeNumBits() );
		int entityDefNumber = ClientRemapDecl( DECL_ENTITYDEF, msg.ReadBits( entityDefBits ) );
		const int predictedKey = msg.ReadBits( 32 );
		const bool hasClassData = msg.ReadBool();	//puncho - F4-m1: the host's networkSync for this entity

		idTypeInfo* typeInfo = idClass::GetType( typeNum );
		if( !typeInfo )
		{
			idLib::Error( "Unknown type number %d for entity %d with class number %d", typeNum, entityNumber, entityDefNumber );
		}

		// If there is no entity on this client, but the server's entity matches a predictionKey, move the client's
		// predicted entity to the normal, replicated area in the entity list.
		if( entities[entityNumber] == NULL )
		{
			if( predictedKey != idEntity::INVALID_PREDICTION_KEY )
			{
				idLib::PrintfIf( debug, "Looking for predicted key %d.\n", predictedKey );
				idEntity* predictedEntity = FindPredictedEntity( predictedKey, typeInfo );

				if( predictedEntity != NULL )
				{
					// This presentable better be in the proper place in the list or bad things will happen if we move this presentable around
					assert( predictedEntity->GetEntityNumber() >= ENTITYNUM_FIRST_NON_REPLICATED );
					continue;
#if 0
					idProjectile* predictedProjectile = idProjectile::CastTo( predictedEntity );
					if( predictedProjectile != NULL )
					{
						for( int i = 0; i < MAX_PLAYERS; i++ )
						{
							if( entities[i] == NULL )
							{
								continue;
							}
							idPlayer* player = idPlayer::CastTo( entities[i] );
							if( player != NULL )
							{
								if( player->GetUniqueProjectile() == predictedProjectile )
								{
									// Set new spawn id
									player->TrackUniqueProjectile( predictedProjectile );
								}
							}
						}
					}

					idLib::PrintfIf( debug, "Found predicted EntNum old:%i new:%i spawnID:%i\n", predictedEntity->GetEntityNumber(), entityNumber, spawnId >> GENTITYNUM_BITS );

					// move the entity
					RemoveEntityFromHash( predictedEntity->name.c_str(), predictedEntity );
					UnregisterEntity( predictedEntity );
					assert( entities[predictedEntity->GetEntityNumber()] == NULL );
					predictedEntity->spawnArgs.SetInt( "spawn_entnum", entityNumber );
					RegisterEntity( predictedEntity, spawnId, predictedEntity->spawnArgs );
					predictedEntity->SetName( "" );

					// now mark us as no longer predicted
					predictedEntity->BecomeReplicated();
#endif
				}
				//TODO make this work with non-client preditced entities
				/* else {
					idLib::Warning( "Could not find predicted entity - key: %d. EntityIndex: %d", predictedKey, entityNum );
				} */
			}
		}

		idEntity* ent = entities[entityNumber];

		// if there is no entity or an entity of the wrong type
		if( !ent || ent->GetType()->typeNum != typeNum || ent->entityDefNumber != entityDefNumber || spawnId != spawnIds[ entityNumber ] )
		{
			//puncho start - identity (P1-A2): the content check proves the map text equal, not the slot layout: a map-range
			// slot the host fills with something else is logged, so a drift between the peers' map spawns shows. A
			// networkSync entity's slot is the host's to free and refill (a broken light, a removed item), so its
			// replacement is legitimate (P1-m7)
			if( ent != NULL && entityNumber >= MAX_CLIENTS && entityNumber < mapSpawnCount && !ent->fl.networkSync )
			{
				Warning( "ClientReadSnapshot: the host's %s of generation %d replaces map-range entity %d '%s' (%s, generation %d)", typeInfo->classname, spawnId, entityNumber, ent->name.c_str(), ent->GetClassname(), spawnIds[ entityNumber ] );
			}
			//puncho end

			delete ent;

			//puncho start - identity (P1-A2): a one-shot that only the registration of this slot takes; the children the
			// new entity spawns advance their own slots
			forcedGenEntityNum = entityNumber;
			forcedGen = spawnId;
			//puncho end

			if( entityNumber < MAX_CLIENTS )
			{
				commonLocal.GetUCmdMgr().ResetPlayer( entityNumber );
				SpawnPlayer( entityNumber );
				ent = entities[ entityNumber ];
				ent->FreeModelDef();
			}
			else
			{
				idDict args;
				args.SetInt( "spawn_entnum", entityNumber );
				args.Set( "name", va( "entity%d", entityNumber ) );

				if( entityDefNumber >= 0 )
				{
					if( entityDefNumber >= declManager->GetNumDecls( DECL_ENTITYDEF ) )
					{
						Error( "server has %d entityDefs instead of %d", entityDefNumber, declManager->GetNumDecls( DECL_ENTITYDEF ) );
					}
					const char* classname = declManager->DeclByIndex( DECL_ENTITYDEF, entityDefNumber, false )->GetName();
					args.Set( "classname", classname );
					//puncho - identity (P1-A2, P1-m6): the spawnId test catches a registration that did not take forcedGen
					if( !SpawnEntityDef( args, &ent ) || !entities[entityNumber] || entities[entityNumber]->GetType()->typeNum != typeNum || spawnIds[ entityNumber ] != spawnId )
					{
						Error( "Failed to spawn entity with classname '%s' of type '%s'", classname, typeInfo->classname );
					}
				}
				else
				{
					ent = SpawnEntityType( *typeInfo, &args, true );
					if( !entities[entityNumber] || entities[entityNumber]->GetType()->typeNum != typeNum || spawnIds[ entityNumber ] != spawnId )	//puncho - P1-m6
					{
						Error( "Failed to spawn entity of type '%s'", typeInfo->classname );
					}
				}
				if( ent != NULL )
				{
					// Fixme: for now, force all think flags on. We'll need to figure out how we want dormancy to work on clients
					// (but for now since clientThink is so light weight, this is ok)
					ent->BecomeActive( TH_ANIMATE );
					ent->BecomeActive( TH_THINK );
					ent->BecomeActive( TH_PHYSICS );
				}
				if( entityNumber < MAX_CLIENTS && entityNumber >= numClients )
				{
					numClients = entityNumber + 1;
				}
			}
			//puncho start - identity (P1-A2, P1-m6): the one-shot is for this slot's registration only; a spawn that did
			// not take it must not leave it armed for a later one
			forcedGenEntityNum = -1;
			//puncho end
		}

		if( ss.ObjectIsStaleByIndex( o ) )
		{
			if( ent->entityNumber >= MAX_CLIENTS && ent->entityNumber < mapSpawnCount && !ent->spawnArgs.GetBool( "net_dynamic", "0" ) ) //_D3XP
			{
				// server says it's not in PVS
				// if that happens on map entities, most likely something is wrong
				// I can see that moving pieces along several PVS could be a legit situation though
				// this is a band aid, which means something is not done right elsewhere
				common->DWarning( "map entity 0x%x (%s) is stale", ent->entityNumber, ent->name.c_str() );
			}
			else
			{
				ent->snapshotStale = true;

				ent->FreeModelDef();
				// possible fix for left over lights on CTF flag
				ent->FreeLightDef();
				ent->UpdateVisuals();
				ent->GetPhysics()->UnlinkClip();
			}
		}
		else
		{
			// add the entity to the snapshot list
			ent->snapshotNode.AddToEnd( snapshotEntities );
			int snapshotChanged = ss.ObjectChangedCountByIndex( o );
			msg.SetHasChanged( ent->snapshotChanged != snapshotChanged );
			ent->snapshotChanged = snapshotChanged;

			ent->FlagNewSnapshot();

			// read the class specific data from the snapshot; SRS - only if network-synced
			//puncho - F4-m1: only when the host wrote it. The remaining bits were the test, and the byte padding after
			// the header passed it: a client spawned a net_instanthit bullet with networkSync on (the host turns it off,
			// Weapon.cpp), read past the end and spun in idProjectile::ReadFromSnapshot
			if( hasClassData && ent->fl.networkSync )
			{
				ent->ReadFromSnapshot_Ex( msg );
				ent->snapshotBits = msg.GetSize();
			}

			// Set after ReadFromSnapshot so we can detect coming unstale
			ent->snapshotStale = false;
		}
	}

	//puncho start - stream-trace
	if( snapTrace != NULL )
	{
		snapTrace->End();
	}
	//puncho end

	// process entity events
	ClientProcessEntityNetworkEventQueue();
}

/*
================
idGameLocal::ClientProcessEntityNetworkEventQueue
================
*/
void idGameLocal::ClientProcessEntityNetworkEventQueue()
{
	while( eventQueue.Start() )
	{
		entityNetEvent_t* event = eventQueue.Start();

		// only process forward, in order
		if( event->time >  this->serverTime )
		{
			break;
		}

		idEntityPtr< idEntity > entPtr;

		if( !entPtr.SetSpawnId( event->spawnId ) )
		{
			if( !gameLocal.entities[ event->spawnId & ( ( 1 << GENTITYNUM_BITS ) - 1 ) ] )
			{
				// if new entity exists in this position, silently ignore
				NetworkEventWarning( event, "Entity does not exist any longer, or has not been spawned yet." );
			}
		}
		else
		{
			idEntity* ent = entPtr.GetEntity();
			assert( ent );

			idBitMsg eventMsg;
			eventMsg.InitRead( event->paramsBuf, sizeof( event->paramsBuf ) );
			eventMsg.SetSize( event->paramsSize );
			eventMsg.BeginReading();
			if( !ent->ClientReceiveEvent( event->event, event->time, eventMsg ) )
			{
				NetworkEventWarning( event, "unknown event" );
			}
		}

		verify( eventQueue.Dequeue() == event );
		eventQueue.Free( event );
	}
}

/*
================
idGameLocal::ClientProcessReliableMessage
================
*/
void idGameLocal::ClientProcessReliableMessage( int type, const idBitMsg& msg )
{
	switch( type )
	{
		case GAME_RELIABLE_MESSAGE_TRANSITION:
		{
			const unsigned short sequence = msg.ReadUShort();
			char map[64];
			msg.ReadString( map, sizeof( map ) );
			common->NotifyMapTransition( sequence, map );
			break;
		}

		case GAME_RELIABLE_MESSAGE_SYNCEDCVARS:
		{
			idDict syncedCvars;
			msg.ReadDeltaDict( syncedCvars, NULL );

			idLib::Printf( "Got networkSync cvars:\n" );
			syncedCvars.Print();

			cvarSystem->ResetFlaggedVariables( CVAR_NETWORKSYNC );
			cvarSystem->SetCVarsFromDict( syncedCvars );
			break;
		}
		case GAME_RELIABLE_MESSAGE_CHAT:
		case GAME_RELIABLE_MESSAGE_TCHAT:   // (client should never get a TCHAT though)
		{
			char name[128];
			char text[128];
			msg.ReadString( name, sizeof( name ) );
			msg.ReadString( text, sizeof( text ) );
			mpGame.AddChatLine( "%s^0: %s\n", name, text );
			break;
		}
		case GAME_RELIABLE_MESSAGE_SOUND_EVENT:
		{
			snd_evt_t snd_evt = ( snd_evt_t )msg.ReadByte();
			mpGame.PlayGlobalSound( -1, snd_evt );
			break;
		}
		case GAME_RELIABLE_MESSAGE_SOUND_INDEX:
		{
			int index = gameLocal.ClientRemapDecl( DECL_SOUND, msg.ReadLong() );
			if( index >= 0 && index < declManager->GetNumDecls( DECL_SOUND ) )
			{
				const idSoundShader* shader = declManager->SoundByIndex( index );
				mpGame.PlayGlobalSound( -1, SND_COUNT, shader->GetName() );
			}
			break;
		}
		case GAME_RELIABLE_MESSAGE_DB:
		{
			idMultiplayerGame::msg_evt_t msg_evt = ( idMultiplayerGame::msg_evt_t )msg.ReadByte();
			int parm1, parm2;
			parm1 = msg.ReadByte();
			parm2 = msg.ReadByte();
			mpGame.PrintMessageEvent( msg_evt, parm1, parm2 );
			break;
		}
		case GAME_RELIABLE_MESSAGE_EVENT:
		{
			// allocate new event
			entityNetEvent_t* event = eventQueue.Alloc();
			eventQueue.Enqueue( event, idEventQueue::OUTOFORDER_IGNORE );

			event->spawnId = msg.ReadBits( 32 );
			event->event = msg.ReadByte();
			event->time = msg.ReadLong();

			event->paramsSize = msg.ReadBits( idMath::BitsForInteger( MAX_EVENT_PARAM_SIZE ) );
			if( event->paramsSize )
			{
				if( event->paramsSize > MAX_EVENT_PARAM_SIZE )
				{
					NetworkEventWarning( event, "invalid param size" );
					return;
				}
				msg.ReadByteAlign();
				msg.ReadData( event->paramsBuf, event->paramsSize );
			}
			break;
		}
		case GAME_RELIABLE_MESSAGE_RESTART:
		{
			MapRestart();
			break;
		}
		case GAME_RELIABLE_MESSAGE_TOURNEYLINE:
		{
			int line = msg.ReadByte();
			idPlayer* p = static_cast< idPlayer* >( entities[ GetLocalClientNum() ] );
			if( !p )
			{
				break;
			}
			p->tourneyLine = line;
			break;
		}
		case GAME_RELIABLE_MESSAGE_STARTSTATE:
		{
			mpGame.ClientReadStartState( msg );
			break;
		}
		case GAME_RELIABLE_MESSAGE_WARMUPTIME:
		{
			mpGame.ClientReadWarmupTime( msg );
			break;
		}
		case GAME_RELIABLE_MESSAGE_LOBBY_COUNTDOWN:
		{
			int timeRemaining = msg.ReadLong();
			Shell_UpdateClientCountdown( timeRemaining );
			break;
		}
		case GAME_RELIABLE_MESSAGE_RESPAWN_AVAILABLE:
		{
			idPlayer* p = static_cast< idPlayer* >( entities[ GetLocalClientNum() ] );
			if( p )
			{
				p->ShowRespawnHudMessage();
			}
			break;
		}
		case GAME_RELIABLE_MESSAGE_MATCH_STARTED_TIME:
		{
			mpGame.ClientReadMatchStartedTime( msg );
			break;
		}
		case GAME_RELIABLE_MESSAGE_ACHIEVEMENT_UNLOCK:
		{
			mpGame.ClientReadAchievementUnlock( msg );
			break;
		}
		//puncho start - content check (P1-A3a, K19)
		case GAME_RELIABLE_MESSAGE_PREFETCH:
		{
			char predictedMap[64];
			msg.ReadString( predictedMap, sizeof( predictedMap ) );
			if( predictedMap[0] && !strstr( predictedMap, ".." ) )
			{
				PrefetchMap( predictedMap );
			}
			break;
		}
		case GAME_RELIABLE_MESSAGE_DECL_REMAP:
		{
			ClientReadDeclRemap( msg );
			break;
		}
		//puncho end
		default:
		{
			Error( "Unknown reliable message (%d) from host", type );
			break;
		}
	}
}

/*
================
idGameLocal::ClientRunFrame
================
*/
void idGameLocal::ClientRunFrame( idUserCmdMgr& cmdMgr, bool lastPredictFrame, gameReturn_t& ret )
{
	clip.GetMapStatistics( mapClipStart );
	mapAllocStart = Mem_GetAllocationCount();
	idEntity* ent;

	// update the game time
	previousTime = FRAME_TO_MSEC( framenum );
	framenum++;
	time = FRAME_TO_MSEC( framenum );

	idPlayer* player = static_cast<idPlayer*>( entities[GetLocalClientNum()] );
	if( !player )
	{

		// service any pending events
		idEvent::ServiceEvents();

		return;
	}

	// check for local client lag
	idLobbyBase& lobby = session->GetActingGameStateLobbyBase();
	if( lobby.GetPeerTimeSinceLastPacket( lobby.PeerIndexForHost() ) >= net_clientMaxPrediction.GetInteger() )
	{
		player->isLagged = true;
	}
	else
	{
		player->isLagged = false;
	}

	// update the real client time and the new frame flag
	if( time > realClientTime )
	{
		realClientTime = time;
		isNewFrame = true;
	}
	else
	{
		isNewFrame = false;
	}

	slow.Set( time, previousTime, realClientTime );
	fast.Set( time, previousTime, realClientTime );

	// run prediction on all active entities
	for( ent = activeEntities.Next(); ent != NULL; ent = ent->activeNode.Next() )
	{
		ent->thinkFlags |= TH_PHYSICS;

		if( ent->entityNumber != GetLocalClientNum() )
		{
			ent->ClientThink( netInterpolationInfo.serverGameMs, netInterpolationInfo.pct, true );
		}
		else
		{
			RunAllUserCmdsForPlayer( cmdMgr, ent->entityNumber );
		}
	}

	// service any pending events
	idEvent::ServiceEvents();

	// show any debug info for this frame
	if( isNewFrame )
	{
		RunDebugInfo();
		D_DrawDebugLines();
	}

	BuildReturnValue( ret );
}

/*
===============
idGameLocal::Tokenize
===============
*/
void idGameLocal::Tokenize( idStrList& out, const char* in )
{
	char buf[ MAX_STRING_CHARS ];
	char* token, *next;

	idStr::Copynz( buf, in, MAX_STRING_CHARS );
	token = buf;
	next = strchr( token, ';' );
	while( token )
	{
		if( next )
		{
			*next = '\0';
		}
		idStr::ToLower( token );
		out.Append( token );
		if( next )
		{
			token = next + 1;
			next = strchr( token, ';' );
		}
		else
		{
			token = NULL;
		}
	}
}

/*
========================
idGameLocal::FindPredictedEntity
========================
*/
idEntity*   idGameLocal::FindPredictedEntity( uint32 predictedKey, idTypeInfo* type )
{
	for( idEntity* predictedEntity = activeEntities.Next(); predictedEntity != NULL; predictedEntity = predictedEntity->activeNode.Next() )
	{
		if( !verify( predictedEntity != NULL ) )
		{
			continue;
		}
		if( !predictedEntity->IsReplicated() && predictedEntity->GetPredictedKey() == predictedKey )
		{
			if( predictedEntity->GetType() != type )
			{
				idLib::Warning( "Mismatched presentable type. Predicted: %s Actual: %s", predictedEntity->GetType()->classname, type->classname );
			}
			return predictedEntity;
		}
	}
	return NULL;
}

/*
========================
idGameLocal::GeneratePredictionKey
========================
*/
uint32  idGameLocal::GeneratePredictionKey( idWeapon* weapon, idPlayer* playerAttacker, int overrideKey )
{
	if( overrideKey != -1 )
	{
		uint32 predictedKey = overrideKey;
		int peerIndex		= -1;

		if( common->IsServer() )
		{
			peerIndex = session->GetActingGameStateLobbyBase().PeerIndexFromLobbyUser( lobbyUserIDs[ playerAttacker->entityNumber ] );
		}
		else
		{
			peerIndex = session->GetActingGameStateLobbyBase().PeerIndexOnHost();
		}

		predictedKey |= ( peerIndex << 28 );

		return predictedKey;
	}

	uint32 predictedKey = idEntity::INVALID_PREDICTION_KEY;
	int peerIndex		= -1;

	// Get key - fireCount or throwCount
	//if ( weapon != NULL ) {
	if( common->IsClient() )
	{
		predictedKey = playerAttacker->GetClientFireCount();
	}
	else
	{
		predictedKey = playerAttacker->usercmd.fireCount;
	}
	//} else {
	//	predictedKey = ( playerAttacker->GetThrowCount() );
	//}

	// Get peer index
	if( common->IsServer() )
	{
		peerIndex = session->GetActingGameStateLobbyBase().PeerIndexFromLobbyUser( lobbyUserIDs[ playerAttacker->entityNumber ] );
	}
	else
	{
		peerIndex = session->GetActingGameStateLobbyBase().PeerIndexOnHost();
	}

	if( cg_predictedSpawn_debug.GetBool() )
	{
		idLib::Printf( "GeneratePredictionKey. predictedKey: %d peedIndex: %d\n", predictedKey, peerIndex );
	}

	predictedKey |= ( peerIndex << 28 );
	return predictedKey;
}

/*
===============
idEventQueue::Alloc
===============
*/
entityNetEvent_t* idEventQueue::Alloc()
{
	entityNetEvent_t* event = eventAllocator.Alloc();
	event->prev = NULL;
	event->next = NULL;
	return event;
}

/*
===============
idEventQueue::Free
===============
*/
void idEventQueue::Free( entityNetEvent_t* event )
{
	// should only be called on an unlinked event!
	assert( !event->next && !event->prev );
	eventAllocator.Free( event );
}

/*
===============
idEventQueue::Shutdown
===============
*/
void idEventQueue::Shutdown()
{
	eventAllocator.Shutdown();
	this->Init();
}

/*
===============
idEventQueue::Init
===============
*/
void idEventQueue::Init()
{
	start = NULL;
	end = NULL;
}

/*
===============
idEventQueue::Dequeue
===============
*/
entityNetEvent_t* idEventQueue::Dequeue()
{
	entityNetEvent_t* event = start;
	if( !event )
	{
		return NULL;
	}

	start = start->next;

	if( !start )
	{
		end = NULL;
	}
	else
	{
		start->prev = NULL;
	}

	event->next = NULL;
	event->prev = NULL;

	return event;
}

/*
===============
idEventQueue::RemoveLast
===============
*/
entityNetEvent_t* idEventQueue::RemoveLast()
{
	entityNetEvent_t* event = end;
	if( !event )
	{
		return NULL;
	}

	end = event->prev;

	if( !end )
	{
		start = NULL;
	}
	else
	{
		end->next = NULL;
	}

	event->next = NULL;
	event->prev = NULL;

	return event;
}

/*
===============
idEventQueue::Enqueue
===============
*/
void idEventQueue::Enqueue( entityNetEvent_t* event, outOfOrderBehaviour_t behaviour )
{
	if( behaviour == OUTOFORDER_DROP )
	{
		// go backwards through the queue and determine if there are
		// any out-of-order events
		while( end && end->time > event->time )
		{
			entityNetEvent_t* outOfOrder = RemoveLast();
			common->DPrintf( "WARNING: new event with id %d ( time %d ) caused removal of event with id %d ( time %d ), game time = %d.\n", event->event, event->time, outOfOrder->event, outOfOrder->time, gameLocal.time );
			Free( outOfOrder );
		}
	}
	else if( behaviour == OUTOFORDER_SORT && end )
	{
		// NOT TESTED -- sorting out of order packets hasn't been
		//				 tested yet... wasn't strictly necessary for
		//				 the patch fix.
		entityNetEvent_t* cur = end;
		// iterate until we find a time < the new event's
		while( cur && cur->time > event->time )
		{
			cur = cur->prev;
		}
		if( !cur )
		{
			// add to start
			event->next = start;
			event->prev = NULL;
			start = event;
		}
		else
		{
			// insert
			event->prev = cur;
			event->next = cur->next;
			cur->next = event;
		}
		return;
	}

	// add the new event
	event->next = NULL;
	event->prev = NULL;

	if( end )
	{
		end->next = event;
		event->prev = end;
	}
	else
	{
		start = event;
	}
	end = event;
}


void idGameLocal::BroadcastMapTransition( unsigned short sequence, const char* map )
{
	byte bytes[68];
	idBitMsg message( bytes, sizeof( bytes ) );
	message.WriteUShort( sequence );
	message.WriteString( map, 64 );
	session->GetActingGameStateLobbyBase().SendReliable( GAME_RELIABLE_MESSAGE_TRANSITION, message, false );
}

bool idGameLocal::ReceiveMapTransition( int type, const idBitMsg& msg )
{
	// The captured epoch is frozen through drain and arrival. Lobby/content ACKs
	// never enter this game-message path; chat remains valid across the crossing.
	if( common->IsServer() && common->IsGameDrivenLoad() && type != GAME_RELIABLE_MESSAGE_CHAT && type != GAME_RELIABLE_MESSAGE_TCHAT && type != GAME_RELIABLE_MESSAGE_VCHAT )
	{
		static unsigned int rejected = 0;
		Printf( "transition_stale_game_reliable count=%u type=%d\n", ++rejected, type );
		return true;
	}
	if( type != GAME_RELIABLE_MESSAGE_TRANSITION )
	{
		return false;
	}
	// Intent must be consumed before the next lobby reliable can change session state.
	if( common->IsClient() && msg.GetRemainingData() >= 3 && msg.GetRemainingData() <= 66 )
	{
		const unsigned short sequence = msg.ReadUShort();
		char map[64];
		msg.ReadString( map, sizeof( map ) );
		common->NotifyMapTransition( sequence, map );
	}
	return true;
}
