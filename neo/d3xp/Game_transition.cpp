#include "precompiled.h"
#pragma hdrstop
#include "Game_local.h"
#include <float.h>

int idGameLocal::GetPlayerOrigins( idVec3* origins, int maxOrigins ) const
{
	int count = 0;
	for( int i = 0; i < MAX_CLIENTS && count < maxOrigins; i++ )
	{
		if( entities[i] != NULL && entities[i]->IsType( idPlayer::Type ) )
		{
			origins[count++] = entities[i]->GetPhysics()->GetOrigin();
		}
	}
	return count;
}

static idCVar g_transitionLandmarks( "g_transitionLandmarks", "0", CVAR_NOCHEAT | CVAR_GAME | CVAR_BOOL, "preserve landmark-relative party placement" );
static idCVar g_transitionCarry( "g_transitionCarry", "0", CVAR_NOCHEAT | CVAR_GAME | CVAR_BOOL, "carry eligible entities in transition volumes" );
static idCVar g_showTransition( "g_showTransition", "0", CVAR_GAME | CVAR_BOOL, "draw transition landmarks" );
static const int MAX_TRANSITION_CARRY = 64;
static const int MAX_TRANSITION_RECORD_BYTES = 160;

struct transitionPlayer_t
{
	lobbyUserID_t user;
	idVec3 offset, velocity, expected, actual;
	idAngles angles;
	idBounds bounds;
	idMat3 axis;
	idDict info;
	bool valid, placed, selected, crouched, spectating, wantSpectate;
};
struct transitionCarry_t
{
	byte bytes[MAX_TRANSITION_RECORD_BYTES];
	int length, entity, master, joint, body;
	bool orientated, applied;
	idEntityPtr<idEntity> spawned;
};
struct transitionState_t
{
	mapTransition_t request;
	transitionPlayer_t players[MAX_CLIENTS];
	transitionCarry_t carry[MAX_TRANSITION_CARRY];
	int count, fallback, rotationIgnored;
	bool pending, active, relative, carryApplied;
};
static transitionState_t transitionState;
static bool transitionTestPlacementPending;
static bool transitionTestFarPeer;

bool QueueTransitionTestParty( const mapTransition_t& request, bool farPeer )
{
	if( !gameLocal.BeginMapTransition( request ) )
	{
		return false;
	}
	transitionTestPlacementPending = true;
	transitionTestFarPeer = farPeer;
	return true;
}


static int TransitionPlayerIndex( int client )
{
	if( client < 0 || client >= MAX_CLIENTS )
	{
		return -1;
	}
	for( int i = 0; i < MAX_CLIENTS; i++ )
	{
		if( transitionState.players[i].valid &&
			( ( !common->IsMultiplayer() && i == client ) ||
			( gameLocal.lobbyUserIDs[client].IsValid() && transitionState.players[i].user.CompareIgnoreLobbyType( gameLocal.lobbyUserIDs[client] ) ) ) )
		{
			return i;
		}
	}
	return -1;
}

void idGameLocal::ClearMapTransition()
{
	sessionCommand.Clear();
	worldStore.continueNextLoad = false;
	transitionState.pending = transitionState.active = false;
	transitionTestPlacementPending = false;
	for( int i = 0; i < transitionState.count; i++ )
	{
		transitionState.carry[i].spawned = NULL;
	}
	transitionState.count = 0;
	transitionState.carryApplied = false;
	for( int i = 0; i < MAX_CLIENTS; i++ )
	{
		transitionState.players[i].valid = false;
		transitionState.players[i].info.Clear();
	}
}

bool idGameLocal::BeginMapTransition( const mapTransition_t& request )
{
	if( common->IsClient() || transitionState.pending || transitionState.active )
	{
		Warning( "transition refused: client or transition already pending" );
		return false;
	}
	idStr normalizedMap = request.nextMap;
	normalizedMap.BackSlashesToSlashes();
	normalizedMap.StripLeading( "maps/" );
	normalizedMap.StripFileExtension();
	if( normalizedMap.IsEmpty() || normalizedMap.Length() > 63 || normalizedMap.Find( '"' ) >= 0 || normalizedMap.Find( '\n' ) >= 0 || normalizedMap.Find( ';' ) >= 0 || normalizedMap.Find( ".." ) >= 0 || normalizedMap.Find( ':' ) >= 0 || normalizedMap[0] == '/' )
	{
		Warning( "transition refused: invalid map name" );
		return false;
	}
	if( common->IsMultiplayer() && session->GetActivePlatformLobbyBase().HasActivePeers() && ( !cvarSystem->GetCVarBool( "net_coopTransitions" ) || !( session->GetGameLobbyBase().GetMatchParms().matchFlags & MATCH_REQUIRE_PARTY_LOBBY ) ) )
	{
		Warning( "transition refused: net_coopTransitions is disabled" );
		return false;
	}
	transitionState.request = request;
	transitionState.request.nextMap = normalizedMap;
	transitionState.pending = true;
	return true;
}

bool idGameLocal::TransitionPartyReady( const char* landmark ) const
{
	const transitionVolume_t* volume = worldStore.FindTransitionVolume( landmark );
	if( volume == NULL )
	{
		Warning( "wait_party: no volume for landmark '%s'", landmark );
		return false;
	}
	for( int i = 0; i < MAX_CLIENTS; i++ )
	{
		if( entities[i] != NULL && entities[i]->IsType( idPlayer::Type ) && entities[i]->health > 0 && !volume->bounds.ContainsPoint( entities[i]->GetPhysics()->GetOrigin() ) )
		{
			static int nextNotice = 0;
			if( Sys_Milliseconds() >= nextNotice )
			{
				nextNotice = Sys_Milliseconds() + 1000;
				char notice[160];
				idStr::snPrintf( notice, sizeof( notice ), "%s is outside %s", entities[i]->name.c_str(), landmark );
				Printf( "wait_party: %s\n", notice );
				if( common->IsMultiplayer() )
				{
					gameLocal.mpGame.ProcessChatMessage( -1, false, "Waiting for party", notice, NULL );
				}
				else if( gameLocal.GetLocalPlayer() != NULL )
				{
					gameLocal.GetLocalPlayer()->ShowTip( "Waiting for party", notice, true );
				}
			}
			return false;
		}
	}
	return true;
}

static idEntity* TransitionBindRoot( idEntity* ent )
{
	for( int depth = 0; ent != NULL && depth < MAX_GENTITIES; depth++ )
	{
		if( ent->GetBindMaster() == NULL )
		{
			return ent;
		}
		ent = ent->GetBindMaster();
	}
	return NULL;
}

void idGameLocal::CommitMapTransition()
{
	if( g_showTransition.GetBool() )
	{
		for( int i = 0; i < worldStore.landmarks.Num(); i++ )
		{
			gameRenderWorld->DebugBounds( colorCyan, idBounds( idVec3( -8, -8, -8 ), idVec3( 8, 8, 8 ) ), worldStore.landmarks[i].origin );
			gameRenderWorld->DebugSphere( colorCyan, idSphere( worldStore.landmarks[i].origin, worldStore.landmarks[i].radius ) );
		}
	}
	if( g_showTransition.GetBool() && mapFile != NULL )
	{
		for( int i = 0; i < mapFile->GetNumEntities(); i++ )
		{
			const idDict& args = mapFile->GetEntity( i )->epairs;
			if( !idStr::Icmp( args.GetString( "classname" ), "info_landing" ) )
			{
				gameRenderWorld->DebugBounds( colorGreen, idBounds( idVec3( -8, -8, 0 ), idVec3( 8, 8, 16 ) ), args.GetVector( "origin" ) );
			}
		}
	}
	if( !transitionState.pending || common->IsClient() )
	{
		return;
	}
	const uint64 carryStart = Sys_Microseconds();
	transitionState.pending = false;
	transitionState.active = true;
	transitionState.count = transitionState.fallback = transitionState.rotationIgnored = 0;
	transitionState.carryApplied = false;
	const landmark_t* depart = worldStore.FindLandmark( transitionState.request.departLandmark );
	if( transitionTestPlacementPending )
	{
		// Developer fixture setup shares the post-think capture boundary. Otherwise
		// remote client-authoritative physics can undo a console teleport before capture.
		transitionTestPlacementPending = false;
		int count = 0;
		for( int i = 0; depart != NULL && i < MAX_CLIENTS; i++ )
		{
			if( entities[i] == NULL || !entities[i]->IsType( idPlayer::Type ) )
			{
				continue;
			}
			idPlayer* player = static_cast<idPlayer*>( entities[i] );
			idVec3 origin = depart->origin + idVec3( -40.0f, count == 0 ? -48.0f : 48.0f, -35.5f );
			if( transitionTestFarPeer && count > 0 )
			{
				origin.x = 0.0f;
			}
			player->Teleport( origin, ang_zero, NULL );
			player->GetPhysics()->SetLinearVelocity( idVec3( 8.0f, 0.0f, 0.0f ) );
			count++;
		}
		Printf( "testPartyPlacement players=%d farPeer=%d\n", count, transitionTestFarPeer );
	}

	transitionState.relative = g_transitionLandmarks.GetBool() && depart != NULL && !transitionState.request.arrivalLandmark.IsEmpty();
	if( g_transitionLandmarks.GetBool() && !transitionState.relative )
	{
		Warning( "transition: missing departure/arrival landmark; using ordinary spawn" );
	}
	if( depart != NULL && !depart->axis.Compare( mat3_identity ) )
	{
		transitionState.rotationIgnored++;
		Warning( "transition: landmark rotation ignored (translation only)" );
	}
	for( int i = 0; i < MAX_CLIENTS; i++ )
	{
		transitionPlayer_t& record = transitionState.players[i];
		record.valid = entities[i] != NULL && entities[i]->IsType( idPlayer::Type );
		record.placed = record.selected = false;
		if( !record.valid )
		{
			record.info.Clear();
			continue;
		}
		idPlayer* player = static_cast<idPlayer*>( entities[i] );
		record.user = lobbyUserIDs[i];
		record.spectating = player->spectating;
		record.wantSpectate = player->wantSpectate;
		record.offset = player->GetPhysics()->GetOrigin() - ( depart != NULL ? depart->origin : vec3_origin );
		record.velocity = player->GetPhysics()->GetLinearVelocity();
		record.angles = player->viewAngles;
		record.bounds = player->GetPhysics()->GetBounds();
		record.axis = player->GetPhysics()->GetAxis();
		record.crouched = static_cast<idPhysics_Player*>( player->GetPlayerPhysics() )->IsCrouching();
		player->SavePersistantInfo();
		record.info = persistentPlayerInfo[i];
		// A spectator can have no equipped weapon (-1). The snapshot field is
		// unsigned; preserve the desired slot, or the empty slot, before holding tics.
		if( record.info.GetInt( "current_weapon", "0" ) < 0 )
		{
			record.info.SetInt( "current_weapon", idMath::ClampInt( 0, MAX_WEAPONS - 1, player->GetIdealWeapon() ) );
		}
		Printf( "transition_player_depart slot=%d offset=%.9g,%.9g,%.9g health=%d infoHash=%08x\n", i, record.offset.x, record.offset.y, record.offset.z, player->health, record.info.Checksum() );
	}
	const transitionVolume_t* volume = worldStore.FindTransitionVolume( transitionState.request.departLandmark );
	if( g_transitionCarry.GetBool() && transitionState.relative && worldStore.IsActive() && volume != NULL && volume->carryAll )
	{
		// Transition-only fixed scratch. Bound teams are ranked as a unit; no frame allocation.
		idEntity* touching[MAX_GENTITIES];
		bool roots[MAX_GENTITIES] = {};
		bool accepted[MAX_GENTITIES] = {};
		const int touchingCount = clip.EntitiesTouchingBounds( volume->bounds, -1, touching, MAX_GENTITIES );
		for( int i = 0; i < touchingCount; i++ )
		{
			idEntity* root = TransitionBindRoot( touching[i] );
			if( root != NULL && root->entityNumber >= MAX_CLIENTS && root->entityNumber < ENTITYNUM_WORLD && root->CanCrossTransition() && root->spawnArgs.GetBool( "transition", "1" ) && volume->bounds.ContainsPoint( root->GetPhysics()->GetOrigin() ) )
			{
				roots[root->entityNumber] = true;
			}
		}
		const int cap = idMath::ClampInt( 0, MAX_TRANSITION_CARRY, volume->cap );
		int acceptedCount = 0;
		for( ;; )
		{
			int best = -1;
			float distance = FLT_MAX;
			for( int i = MAX_CLIENTS; i < ENTITYNUM_WORLD; i++ )
			{
				if( roots[i] )
				{
					const float d = ( entities[i]->GetPhysics()->GetOrigin() - depart->origin ).LengthSqr();
					if( d < distance )
					{
						best = i; distance = d;
					}
				}
			}
			if( best < 0 )
			{
				break;
			}
			roots[best] = false;
			int group[MAX_TRANSITION_CARRY];
			int groupCount = 0;
			bool valid = true;
			for( int i = MAX_CLIENTS; i < ENTITYNUM_WORLD; i++ )
			{
				idEntity* ent = entities[i];
				if( ent == NULL || TransitionBindRoot( ent ) != entities[best] )
				{
					continue;
				}
				if( !ent->CanCrossTransition() || !ent->spawnArgs.GetBool( "transition", "1" ) || groupCount >= MAX_TRANSITION_CARRY )
				{
					valid = false; break;
				}
				group[groupCount++] = i;
			}
			if( !valid )
			{
				continue;
			}
			if( acceptedCount + groupCount > cap )
			{
				Warning( "transition carry cap %d: whole team %d remains", cap, best );
				continue;
			}
			for( int i = 0; i < groupCount; i++ )
			{
				idFile_Memory bytes;
				if( !worldStore.WriteCreated( entities[group[i]], depart->origin, bytes ) || bytes.Length() > MAX_TRANSITION_RECORD_BYTES )
				{
					valid = false; break;
				}
			}
			if( !valid )
			{
				Warning( "transition carry: team %d has an unsupported or oversized record", best );
				continue;
			}
			for( int i = 0; i < groupCount; i++ )
			{
				accepted[group[i]] = true;
			}
			acceptedCount += groupCount;
		}
		for( int i = MAX_CLIENTS; i < ENTITYNUM_WORLD; i++ )
		{
			if( !accepted[i] )
			{
				continue;
			}
			idEntity* ent = entities[i];
			idFile_Memory bytes;
			if( !worldStore.WriteCreated( ent, depart->origin, bytes ) || bytes.Length() > MAX_TRANSITION_RECORD_BYTES )
			{
				continue;
			}
			transitionCarry_t& record = transitionState.carry[transitionState.count++];
			record.length = bytes.Length();
			memcpy( record.bytes, bytes.GetDataPtr(), record.length );
			record.entity = i;
			record.master = ent->GetBindMaster() != NULL ? ent->GetBindMaster()->entityNumber : -1;
			record.joint = ent->GetBindJoint();
			record.body = ent->GetBindBody();
			record.orientated = ent->fl.bindOrientated;
			record.applied = false;
			record.spawned = NULL;
			worldStore.MarkDeparted( ent );
		}
	}
	if( cvarSystem->GetCVarBool( "s_transitionCarry" ) )
	{
		if( cvarSystem->GetCVarInteger( "com_levelKeep" ) < 1 )
		{
			Warning( "transition sound disabled: requires com_levelKeep >= 1" );
		}
		else
		{
			for( int i = 0; i < num_entities; i++ )
			{
				if( entities[i] != NULL && entities[i]->IsType( idSound::Type ) )
				{
					static_cast<idSound*>( entities[i] )->CarryTransitionSound();
				}
			}
		}
	}
	worldStore.continueNextLoad = worldStore.IsActive();
	worldStore.CommitCurrentMap();
	common->ReportTransitionMetric( TRANSITION_CARRY_US, Sys_Microseconds() - carryStart );
	sessionCommand = transitionState.relative ? "transition " : "map ";
	sessionCommand += transitionState.request.nextMap;
	int playerCount = 0;
	for( int i = 0; i < MAX_CLIENTS; i++ ) { playerCount += transitionState.players[i].valid ? 1 : 0; }
	Printf( "transition_commit players=%d carry=%d map=%s\n", playerCount, transitionState.count, transitionState.request.nextMap.c_str() );
}

void idGameLocal::ApplyTransitionCarry()
{
	if( !transitionState.active || transitionState.carryApplied || common->IsClient() )
	{
		return;
	}
	const landmark_t* arrival = worldStore.FindLandmark( transitionState.request.arrivalLandmark );
	idVec3 origin;
	if( arrival != NULL )
	{
		origin = arrival->origin;
		if( !arrival->axis.Compare( mat3_identity ) )
		{
			transitionState.rotationIgnored++;
			Warning( "transition: arrival rotation ignored" );
		}
	}
	else
	{
		// Player spawn selection supplies the fallback after the map's records apply.
		int player = -1;
		for( int i = 0; i < MAX_CLIENTS; i++ )
		{
			if( transitionState.players[i].placed )
			{
				player = i; break;
			}
		}
		if( player < 0 )
		{
			return;
		}
		origin = transitionState.players[player].actual;
		Warning( "transition: no arrival landmark; carry uses stock player spawn" );
	}
	bool allApplied = true;
	for( int i = 0; i < transitionState.count; i++ )
	{
		transitionCarry_t& record = transitionState.carry[i];
		if( record.applied )
		{
			continue;
		}
		idFile_Memory bytes( "transition-created", ( const char* )record.bytes, record.length );
		record.spawned = worldStore.SpawnCreated( bytes, mat3_identity, origin );
		record.applied = record.spawned.GetEntity() != NULL;
		allApplied &= record.applied;
		if( !record.applied )
		{
			Warning( "transition: carry %d could not spawn; record retained", i );
		}
	}
	for( int i = 0; i < transitionState.count; i++ )
	{
		idEntity* ent = transitionState.carry[i].spawned.GetEntity();
		if( ent == NULL || transitionState.carry[i].master < 0 )
		{
			continue;
		}
		for( int j = 0; j < transitionState.count; j++ )
		{
			idEntity* master = transitionState.carry[j].spawned.GetEntity();
			if( master == NULL || transitionState.carry[j].entity != transitionState.carry[i].master )
			{
				continue;
			}
			if( transitionState.carry[i].joint != INVALID_JOINT )
			{
				ent->BindToJoint( master, static_cast<jointHandle_t>( transitionState.carry[i].joint ), transitionState.carry[i].orientated );
			}
			else if( transitionState.carry[i].body >= 0 )
			{
				ent->BindToBody( master, transitionState.carry[i].body, transitionState.carry[i].orientated );
			}
			else
			{
				ent->Bind( master, transitionState.carry[i].orientated );
			}
			break;
		}
	}
	transitionState.carryApplied = allApplied;
}

static bool TransitionPositionClear( const transitionPlayer_t& record, const idVec3& origin )
{
	if( gameRenderWorld->PointInArea( origin ) < 0 )
	{
		return false;
	}
	const idTraceModel traceModel( record.bounds );
	idClipModel model( traceModel );
	if( gameLocal.clip.ContentsModel( origin, &model, record.axis, MASK_PLAYERSOLID, 0, vec3_origin, mat3_identity ) )
	{
		return false;
	}
	idBounds bounds;
	bounds.FromTransformedBounds( record.bounds, origin, record.axis );
	for( int i = 0; i < MAX_CLIENTS; i++ )
	{
		const transitionPlayer_t& other = transitionState.players[i];
		if( !other.valid || !other.placed )
		{
			continue;
		}
		idBounds placed;
		placed.FromTransformedBounds( other.bounds, other.actual, other.axis );
		if( bounds.IntersectsBounds( placed ) )
		{
			return false;
		}
	}
	return true;
}

bool idGameLocal::SelectTransitionSpawn( idPlayer* player, idVec3& origin, idAngles& angles )
{
	const int index = TransitionPlayerIndex( player->entityNumber );
	if( !transitionState.active || !transitionState.relative || index < 0 || transitionState.players[index].placed )
	{
		return false;
	}
	transitionPlayer_t& record = transitionState.players[index];
	const landmark_t* arrival = worldStore.FindLandmark( transitionState.request.arrivalLandmark );
	if( arrival == NULL )
	{
		Warning( "transition: arrival '%s' missing; ordinary spawn", transitionState.request.arrivalLandmark.c_str() );
		return false;
	}
	origin = arrival->origin + record.offset;
	angles = record.angles;
	bool clear = record.offset.LengthSqr() <= arrival->radius * arrival->radius && TransitionPositionClear( record, origin );
	if( !clear && mapFile != NULL )
	{
		// Select by authored index, then ordinal without allocating or reordering map data.
		int lastIndex = -1, lastOrdinal = -1;
		for( int pass = 0; pass < mapFile->GetNumEntities(); pass++ )
		{
			int best = -1, bestIndex = INT_MAX;
			for( int i = 0; i < mapFile->GetNumEntities(); i++ )
			{
				const idDict& args = mapFile->GetEntity( i )->epairs;
				if( idStr::Icmp( args.GetString( "classname" ), "info_landing" ) || idStr::Icmp( args.GetString( "landmark" ), transitionState.request.arrivalLandmark ) )
				{
					continue;
				}
				const int number = args.GetInt( "index", "0" );
				if( number < lastIndex || ( number == lastIndex && i <= lastOrdinal ) )
				{
					continue;
				}
				if( number < bestIndex )
				{
					best = i; bestIndex = number;
				}
			}
			if( best < 0 )
			{
				break;
			}
			lastIndex = bestIndex; lastOrdinal = best;
			origin = mapFile->GetEntity( best )->epairs.GetVector( "origin" );
			if( TransitionPositionClear( record, origin ) )
			{
				Printf( "transition_landing slot=%d index=%d origin=%.9g,%.9g,%.9g\n", player->entityNumber, bestIndex, origin.x, origin.y, origin.z );
				clear = true; break;
			}
		}
		transitionState.fallback++;
	}
	if( !clear )
	{
		return false;
	}
	record.expected = origin;
	record.selected = true;
	return true;
}

bool idGameLocal::RestoreTransitionPlayerInfo( int clientNum )
{
	const int index = TransitionPlayerIndex( clientNum );
	if( !transitionState.active || index < 0 )
	{
		return false;
	}
	persistentPlayerInfo[clientNum] = transitionState.players[index].info;
	return true;
}

void idGameLocal::FinishTransitionSpawn( idPlayer* player )
{
	const int index = TransitionPlayerIndex( player->entityNumber );
	if( !transitionState.active || index < 0 || transitionState.players[index].placed )
	{
		return;
	}
	transitionPlayer_t& record = transitionState.players[index];
	record.actual = player->GetPhysics()->GetOrigin();
	record.placed = true;
	if( record.selected )
	{
		player->GetPhysics()->SetLinearVelocity( record.velocity );
		static_cast<idPhysics_Player*>( player->GetPlayerPhysics() )->RestoreTransitionCrouch( record.crouched );
	}
	Printf( "transition_player_arrive slot=%d selected=%d positionExact=%d velocityExact=%d healthMatch=%d expected=%.9g,%.9g,%.9g actual=%.9g,%.9g,%.9g\n", player->entityNumber, record.selected, record.selected && record.expected.Compare( record.actual ), record.selected && record.velocity.Compare( player->GetPhysics()->GetLinearVelocity() ), player->health == record.info.GetInt( "health", "100" ), record.expected.x, record.expected.y, record.expected.z, record.actual.x, record.actual.y, record.actual.z );
	ApplyTransitionCarry();
	bool complete = transitionState.carryApplied || transitionState.count == 0;
	for( int i = 0; i < MAX_CLIENTS; i++ )
	{
		complete &= !transitionState.players[i].valid || transitionState.players[i].placed;
	}
	if( complete )
	{
		transitionState.active = false;
		Printf( "transition_arrived carry=%d fallback=%d rotation_ignored=%d\n", transitionState.count, transitionState.fallback, transitionState.rotationIgnored );
	}
}

void idGameLocal::ListTransition() const
{
	common->ListTransitionNetwork();
	Printf( "transition pending=%d active=%d map=%s carry=%d bytes=%d fallback=%d rotation_ignored=%d\n", transitionState.pending, transitionState.active, transitionState.request.nextMap.c_str(), transitionState.count, ( int )sizeof( transitionState ), transitionState.fallback, transitionState.rotationIgnored );
	for( int i = 0; i < MAX_CLIENTS; i++ )
	{
		const transitionPlayer_t& p = transitionState.players[i];
		if( p.valid )
		{
			Printf( " player %d placed=%d expected=%s actual=%s\n", i, p.placed, p.expected.ToString(), p.actual.ToString() );
		}
	}
}

bool idGameLocal::PrepareTransitionPlayerSpawn( idPlayer* player )
{
	const int index = TransitionPlayerIndex( player->entityNumber );
	if( !transitionState.active || index < 0 )
	{
		return false;
	}
	player->spectator = player->entityNumber;
	player->spectating = transitionState.players[index].spectating;
	player->wantSpectate = transitionState.players[index].wantSpectate;
	player->forceRespawn = false;
	return true;
}

bool idGameLocal::IsMapTransitionActive() const
{
	return transitionState.pending || transitionState.active;
}
