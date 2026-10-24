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

#ifndef __GAME_LOCAL_H__
#define	__GAME_LOCAL_H__

/*
===============================================================================

	Local implementation of the public game interface.

===============================================================================
*/

#ifdef ID_DEBUG_UNINITIALIZED_MEMORY
	// This is real evil but allows the code to inspect arbitrary class variables.
	#define private		public
	#define protected	public
#endif

extern idRenderWorld* 				gameRenderWorld;
extern idSoundWorld* 				gameSoundWorld;

// the "gameversion" client command will print this plus compile date
#define	GAME_VERSION		"baseDOOM-1"

// classes used by idGameLocal
class idEntity;
class idActor;
class idPlayer;
class idCamera;
class idWorldspawn;
class idTestModel;
class idAAS;
class idAI;
class idSmokeParticles;
class idEntityFx;
class idTypeInfo;
class idProgram;
class idThread;
class idEditEntities;
class idLocationEntity;
class idMenuHandler_Shell;
class EnvironmentProbe; // RB

const int MAX_CLIENTS			= MAX_PLAYERS;
const int MAX_CLIENTS_IN_PVS	= MAX_CLIENTS >> 3;
// 14 bits since 2026-10-23: 16384 entity slots. The width is baked into spawn ids (the other
// 18 bits carry the slot's generation, MAX_SPAWN_GENERATION below), idEntityPtr, saves (gated by
// BUILD_NUMBER_SAVE_VERSION_ENTITY_BITS_14), snapshots and bind packing; the snapshot layer's
// MAX_OBJ_PARMS (sys/SnapshotProcessor.h) is sized to match, and the query lists of this many
// entries live in the scratch arena (com_gameScratchKB), not on the stack. Client and server must
// agree on it.
const int GENTITYNUM_BITS		= 14;
const int MAX_GENTITIES			= 1 << GENTITYNUM_BITS;
const int ENTITYNUM_NONE		= MAX_GENTITIES - 1;
const int ENTITYNUM_WORLD		= MAX_GENTITIES - 2;
const int ENTITYNUM_MAX_NORMAL	= MAX_GENTITIES - 2;
const int ENTITYNUM_FIRST_NON_REPLICATED	= ENTITYNUM_MAX_NORMAL - 256;
// P1-A2 (K9): each slot counts its own spawn-id generations, 1 to this and around again; 0 is the null
// spawn id. The wire carries 32 - GENTITYNUM_BITS bits of it, but idEntityPtr packs generation <<
// GENTITYNUM_BITS into a signed int, where 1 << ( 31 - GENTITYNUM_BITS ) would take the sign bit and
// never resolve: 2^17 - 1 at 14 index bits.
const int MAX_SPAWN_GENERATION	= ( 1 << ( 31 - GENTITYNUM_BITS ) ) - 1;

// Admer: brush origin offsets for better TrenchBroom support
constexpr const char* BRUSH_ORIGIN_KEY = "__brushOrigin";

//============================================================================

void gameError( const char* fmt, ... );

#include "gamesys/Event.h"
#include "gamesys/Class.h"
#include "gamesys/SysCvar.h"
#include "gamesys/SysCmds.h"
#include "gamesys/SaveGame.h"

#include "script/Script_Program.h"

#include "anim/Anim.h"

#include "ai/AAS.h"

#include "physics/Clip.h"
#include "physics/Push.h"

#include "Pvs.h"
#include "Leaderboards.h"
#include "MultiplayerGame.h"


class idWeapon;

//============================================================================

const int MAX_GAME_MESSAGE_SIZE		= 8192;
const int MAX_ENTITY_STATE_SIZE		= 512;
const int ENTITY_PVS_SIZE			= ( ( MAX_GENTITIES + 31 ) >> 5 );
const int NUM_RENDER_PORTAL_BITS	= idMath::BitsForInteger( PS_BLOCK_ALL );

const int MAX_EVENT_PARAM_SIZE		= 128;

//puncho start - content check (P1-A3a, K19): an implicit decl crosses the wire as -2 - k, k its slot in the host's
// announce table, one table per load shared by every peer. A joiner gets the whole table among its initial reliables,
// so the table is bounded in bytes as well as in slots: half of the 8000-byte reliable queue
// (idPacketProcessor::MAX_MSG_SIZE), sent in GAME_RELIABLE_MESSAGE_DECL_REMAP batches of at most 1 KB
const int MAX_DECL_REMAP			= 256;		// slots per load; an implicit decl past the cap crosses as -1 (null)
const int MAX_DECL_REMAP_BYTES		= 4000;		// the whole table as sent: a type byte and the name with its terminator per slot
const int MAX_DECL_REMAP_BATCH		= 1024;		// one GAME_RELIABLE_MESSAGE_DECL_REMAP
const int MAX_DECL_REMAP_NAME		= 255;		// a longer name is not announced
//puncho end

typedef struct entityNetEvent_s
{
	int						spawnId;
	int						event;
	int						time;
	int						paramsSize;
	byte					paramsBuf[MAX_EVENT_PARAM_SIZE];
	struct entityNetEvent_s*	next;
	struct entityNetEvent_s* prev;
} entityNetEvent_t;

enum
{
	GAME_RELIABLE_MESSAGE_SYNCEDCVARS,
	GAME_RELIABLE_MESSAGE_SPAWN_PLAYER,
	GAME_RELIABLE_MESSAGE_CHAT,
	GAME_RELIABLE_MESSAGE_TCHAT,
	GAME_RELIABLE_MESSAGE_SOUND_EVENT,
	GAME_RELIABLE_MESSAGE_SOUND_INDEX,
	GAME_RELIABLE_MESSAGE_DB,
	GAME_RELIABLE_MESSAGE_DROPWEAPON,
	GAME_RELIABLE_MESSAGE_RESTART,
	GAME_RELIABLE_MESSAGE_TOURNEYLINE,
	GAME_RELIABLE_MESSAGE_VCHAT,
	GAME_RELIABLE_MESSAGE_STARTSTATE,
	GAME_RELIABLE_MESSAGE_WARMUPTIME,
	GAME_RELIABLE_MESSAGE_SPECTATE,
	GAME_RELIABLE_MESSAGE_EVENT,
	GAME_RELIABLE_MESSAGE_LOBBY_COUNTDOWN,
	GAME_RELIABLE_MESSAGE_RESPAWN_AVAILABLE,			// Used just to show clients the respawn text on the hud.
	GAME_RELIABLE_MESSAGE_MATCH_STARTED_TIME,
	GAME_RELIABLE_MESSAGE_ACHIEVEMENT_UNLOCK,
	GAME_RELIABLE_MESSAGE_CLIENT_HITSCAN_HIT,
	GAME_RELIABLE_MESSAGE_DECL_REMAP					//puncho - content check (P1-A3a, K19): host to peers, slots of the implicit decl announce table
};

typedef enum
{
	GAMESTATE_UNINITIALIZED,		// prior to Init being called
	GAMESTATE_NOMAP,				// no map loaded
	GAMESTATE_STARTUP,				// inside InitFromNewMap().  spawning map entities.
	GAMESTATE_ACTIVE,				// normal gameplay
	GAMESTATE_SHUTDOWN				// inside MapShutdown().  clearing memory.
} gameState_t;

typedef struct
{
	idEntity*	ent;
	int			dist;
	int			team;
} spawnSpot_t;

//============================================================================

class idEventQueue
{
public:
	typedef enum
	{
		OUTOFORDER_IGNORE,
		OUTOFORDER_DROP,
		OUTOFORDER_SORT
	} outOfOrderBehaviour_t;

	idEventQueue() : start( NULL ), end( NULL ) {}

	entityNetEvent_t* 		Alloc();
	void					Free( entityNetEvent_t* event );
	void					Shutdown();

	void					Init();
	void					Enqueue( entityNetEvent_t* event, outOfOrderBehaviour_t oooBehaviour );
	entityNetEvent_t* 		Dequeue();
	entityNetEvent_t* 		RemoveLast();

	entityNetEvent_t* 		Start()
	{
		return start;
	}

private:
	entityNetEvent_t* 					start;
	entityNetEvent_t* 					end;
	idBlockAlloc<entityNetEvent_t, 32>	eventAllocator;
};

//============================================================================

template< class type >
class idEntityPtr
{
public:
	idEntityPtr();

	// save games
	void					Save( idSaveGame* savefile ) const;					// archives object for save game file
	void					Restore( idRestoreGame* savefile );					// unarchives object from save game file

	idEntityPtr& 			operator=( const type* ent );
	idEntityPtr& 			operator=( const idEntityPtr& ep );

	bool					operator==( const idEntityPtr& ep )
	{
		return spawnId == ep.spawnId;
	}

	type* 					operator->() const
	{
		return GetEntity();
	}
	operator type* () const
	{
		return GetEntity();
	}

	// synchronize entity pointers over the network
	int						GetSpawnId() const
	{
		return spawnId;
	}
	bool					SetSpawnId( int id );
	bool					UpdateSpawnId();

	bool					IsValid() const;
	type* 					GetEntity() const;
	int						GetEntityNum() const;

private:
	int						spawnId;
};

struct timeState_t
{
	int					time;
	int					previousTime;
	int					realClientTime;

	void				Set( int t, int pt, int rct )
	{
		time = t;
		previousTime = pt;
		realClientTime = rct;
	};
	void				Get( int& t, int& pt, int& rct )
	{
		t = time;
		pt = previousTime;
		rct = realClientTime;
	};
	void				Save( idSaveGame* savefile ) const
	{
		savefile->WriteInt( time );
		savefile->WriteInt( previousTime );
		savefile->WriteInt( realClientTime );
	}
	void				Restore( idRestoreGame* savefile )
	{
		savefile->ReadInt( time );
		savefile->ReadInt( previousTime );
		savefile->ReadInt( realClientTime );
	}
};

enum slowmoState_t
{
	SLOWMO_STATE_OFF,
	SLOWMO_STATE_RAMPUP,
	SLOWMO_STATE_ON,
	SLOWMO_STATE_RAMPDOWN
};

//============================================================================

//puncho start - ai-harness (plans/ai-harness/00-PLAN.md §2.3). Adds the elapsed microseconds to accum
// and opens an Optick event on desc, both only while ai_profile is set, so a site costs one cvar read
// with the harness off. Mirrors idOptionalGpuProfileScope (framework/Common.h): Event::Start returns
// NULL outside a capture. The timer is read last on entry and first on exit so the Optick calls stay
// outside the accumulated figure. Accepted RAII exception, like idScopedProfileEvent.
class idAIProfileScope
{
public:
	idAIProfileScope( int& accumulator, const profileDesc_t* desc );
	~idAIProfileScope();

	idAIProfileScope( const idAIProfileScope& ) = delete;
	idAIProfileScope& operator=( const idAIProfileScope& ) = delete;

private:
	int*			accum;		// NULL when ai_profile was clear at construction
	uint64			start;
#if USE_OPTICK
	::Optick::EventData*	event;
#endif
};

ID_INLINE idAIProfileScope::idAIProfileScope( int& accumulator, const profileDesc_t* desc )
{
	if( ai_profile.GetInteger() == 0 )
	{
		accum = NULL;
		start = 0;
#if USE_OPTICK
		event = NULL;
#endif
		return;
	}
	accum = &accumulator;
#if USE_OPTICK
	event = ( desc != NULL ) ? ::Optick::Event::Start( *desc ) : NULL;
#endif
	start = Sys_Microseconds();
}

ID_INLINE idAIProfileScope::~idAIProfileScope()
{
	if( accum == NULL )
	{
		return;
	}
	*accum += ( int )( Sys_Microseconds() - start );
#if USE_OPTICK
	if( event != NULL )
	{
		::Optick::Event::Stop( *event );
	}
#endif
}
//puncho end

//puncho start - ai-avoidance (plans/ai-avoidance/00-PLAN.md §3.3, WP-A5): one live actor in the neighbour grid, as physics
// left it at the end of the tic the grid was built in. POD, copied by value; the chain through nextInCell is per bucket
const int	MAX_AI_NEIGHBORS		= 1024;
const int	AI_NEIGHBOR_CELLS		= 1024;			// hash buckets, a power of two
const float	AI_NEIGHBOR_CELL_SIZE	= 64.0f;

enum
{
	AIN_HOLDING	= BIT( 0 ),		// a monster without a move order: infinite mass to the separation (R03.24)
	AIN_PLAYER	= BIT( 1 )		// a player: infinite mass too, never pushed
};

struct aiNeighbor_t
{
	idVec3		origin;
	idVec3		velocity;		// for the obstacle gather's same-direction filter
	float		radius;			// flat half extent of the clip bounds: separation keeps 1.2 ( r + r_j )
	float		zMin;			// absolute bounds along z
	float		zMax;
	float		mass;
	int			entityNumber;
	int			nextInCell;		// the next body in the same bucket, -1 end
	short		cellX;			// the cell the body is in, so a bucket collision is not a neighbour
	short		cellY;
	short		group;			// the clip model's collision group (WP-A3), 0 none
	byte		flags;			// AIN_*
	byte		pad;
};

// (plan §3.6, WP-A7): an ai_squad marker's role keys, read from the .map at load like the band layer's loader will read
// the rest of the marker (no entityDef, no entity at run time); monsters join with the key 'squad'
const int	MAX_AI_SQUADS			= 64;
const byte	AI_SQUAD_ROLE_UNSET		= 255;			// the marker gave no role: the monster's def decides

// (plans/ai-combat/07-DESIGN-c8-levers.md §C): the bits of aiSquadDef_t::overrides, one per combat lever the marker set
enum
{
	AI_SQUAD_SET_ACCURACY			= BIT( 0 ),
	AI_SQUAD_SET_ATTACK_RATE		= BIT( 1 ),
	AI_SQUAD_SET_DODGE_RATE			= BIT( 2 ),
	AI_SQUAD_SET_DODGE_CHANCE		= BIT( 3 ),
	AI_SQUAD_SET_RANGE_MIN			= BIT( 4 ),
	AI_SQUAD_SET_RANGE_MAX			= BIT( 5 ),
	AI_SQUAD_SET_KITE_CHANCE		= BIT( 6 ),
	AI_SQUAD_SET_FIRE_NEAR_FRIENDS	= BIT( 7 ),
	AI_SQUAD_SET_SNIPER				= BIT( 8 )	//puncho - ai-combat (plans/ai-combat/08-PLAN-shooter-tokens.md §3.1)
};

struct aiSquadDef_t
{
	char		name[32];
	byte		role;			// minionRole_t, or AI_SQUAD_ROLE_UNSET
	byte		avoid;			// avoidClass_t forced on the members, AVOID_CLASS_BY_ROLE when not given
	byte		formation;		// 0 none, 1 line, 2 column: read and kept, the slot layout is not built (plan Q3: none)
	byte		pinned;			// sys.aiSetAvoid set avoid: it wins over the marker's key and the member's own class
	// (plans/ai-combat/07-DESIGN-c8-levers.md §C): the marker's combat levers, each valid only while its bit is set in
	// overrides; idAI::GetCombatKey returns it before the member's def key
	float		accuracy;		// accuracy: scales attack_accuracy and projectile_spread at launch, above 0
	float		attackRate;		// attack_rate, seconds, above 0
	float		dodgeRate;		// dodge_rate, seconds, above 0
	float		dodgeChance;	// dodge_chance, 0..1
	float		rangeMin;		// range_min
	float		rangeMax;		// range_max
	float		kiteChance;		// kite_chance, 0..1
	float		fireNearFriends;	// fire_near_friends, 0 or 1
	float		sniper;			// sniper, 0 or 1: members test without a shooter token //puncho - ai-combat (plans/ai-combat/08-PLAN-shooter-tokens.md)
	short		overrides;		// AI_SQUAD_SET_*: the levers the marker gave
};
//puncho end

//============================================================================

class idGameLocal : public idGame
{
public:

	int						previousServerTime;		// time in msec of last frame on the server
	int						serverTime;				// in msec. ( on the client ) the server time. ( on the server ) the actual game time.
	idDict					serverInfo;				// all the tunable parameters, like numclients, etc
	int						numClients;				// pulled from serverInfo and verified
	idArray< lobbyUserID_t, MAX_CLIENTS >	lobbyUserIDs;	// Maps from a client (player) number to a lobby user
	idDict					persistentPlayerInfo[MAX_CLIENTS];
	idEntity* 				entities[MAX_GENTITIES];// index to entities
	int						spawnIds[MAX_GENTITIES];// for use in idEntityPtr
	idArray< int, 2 >		firstFreeEntityIndex;	// first free index in the entities array. [0] for replicated entities, [1] for non-replicated
	int						num_entities;			// current number <= MAX_GENTITIES
	idHashIndex				entityHash;				// hash table to quickly find entities by name
	idWorldspawn* 			world;					// world entity
	idLinkList<idEntity>	spawnedEntities;		// all spawned entities
	idLinkList<idEntity>	activeEntities;			// all thinking entities (idEntity::thinkFlags != 0)
	idLinkList<idEntity>	aimAssistEntities;		// all aim Assist entities
	int						numEntitiesToDeactivate;// number of entities that became inactive in current frame
	bool					sortPushers;			// true if active lists needs to be reordered to place pushers at the front
	bool					sortTeamMasters;		// true if active lists needs to be reordered to place physics team masters before their slaves
	idDict					persistentLevelInfo;	// contains args that are kept around between levels

	// can be used to automatically effect every material in the world that references globalParms
	float					globalShaderParms[ MAX_GLOBAL_SHADER_PARMS ];

	idRandom				random;					// random number generator used throughout the game

	idProgram				program;				// currently loaded script and data space
	idThread* 				frameCommandThread;

	idClip					clip;					// collision detection
	float					maxObstacleRadius;		// ai-avoidance (WP-A4): the largest flat clip radius of any idActor or idMoveable spawned on this map; sizes the obstacle gather box with ai_avoidHorizon. Not saved: rebuilt on restore
	//puncho start - ai-avoidance (plans/ai-avoidance/00-PLAN.md §3.3, WP-A5): the neighbour grid. Every live actor is hashed
	// into 64-unit cells once per tic after the think loop (BuildNeighborGrid, from the positions physics just produced) and
	// read only by the next tic's thinks: separation, the k nearest actors of the obstacle gather, later the perception
	// candidates. Host state rebuilt every tic, never saved or replicated; valid only for the tic after it was built
	aiNeighbor_t			aiNeighbors[MAX_AI_NEIGHBORS];
	int						aiNeighborCells[AI_NEIGHBOR_CELLS];	// index of the first body in the bucket, -1 none
	int						numAINeighbors;
	int						aiNeighborTic;			// framenum the grid was built in, -1 none
	float					aiNeighborMaxRadius;	// the largest flat radius in the grid, bounds every gather range
	int						sepCorrectionsLeft;		// overlap corrections this tic may still apply (ai_sepBudget, reset with the grid)
	// (WP-A7) the ai_squad markers of this map, read from the map file in LoadMap and LocalMapRestart before the entities
	// spawn; the table is not saved, only the pins sys.aiSetAvoid set are, by squad name, and re-applied on restore (audit M2)
	aiSquadDef_t			aiSquads[MAX_AI_SQUADS];
	int						numAISquads;
	//puncho end
	//puncho start - ai-combat (plans/ai-combat/08-PLAN-shooter-tokens.md): the shooter tokens. idAI::AssignShooterTokens
	// bumps the generation once per tic after the think loop; a monster holds a token while its shooterTokenGen is the
	// current one and the pass ran in the previous tic. Host state, never saved or replicated
	int						shooterTokenGen;		// generation of the last pass, 0 before the first
	int						shooterTokenTic;		// framenum of the last pass, -1 none
	//puncho end
	//puncho start - ai-harness (plans/ai-harness/00-PLAN.md §2.2, §2.5): host-side debug state, never saved or replicated
	aiTicStats_t			aiStats;				// the live tic census, zeroed after SortActiveEntityList
	snapStats_t				snapStats;				// written by ServerWriteSnapshot on the main thread between game frames
	aiHudStats_t			aiHudStats;				// the last tic's record for the HUD, filled by RunAIStats
	int						aiThinkRing[ 128 ];		// per-tic usThink by framenum, for the HUD peak
	aiTicStats_t			aiSecSum;				// per-second aggregates for the aistats: line
	int						aiSecTics;
	int						aiSecMaxThink;
	int						aiSecNextTime;			// game time of the next aistats: line and CSV flush
	idFile*					aiProfileFile;			// the ai_profileLog CSV, NULL while the cvar is empty
	int						aiProfileLen;			// bytes waiting in aiProfileBuf
	char					aiProfileBuf[ 64 * 1024 ];	// CSV rows between the per-second flushes: one Write per second, not per tic (a row is about 250 B since ai-combat: 120 of them need 30 KB)
	//puncho end
	idPush					push;					// geometric pushing
	idPVS					pvs;					// potential visible set

	idTestModel* 			testmodel;				// for development testing of models
	idEntityFx* 			testFx;					// for development testing of fx

	idStr					sessionCommand;			// a target_sessionCommand can set this to return something to the session

	idMultiplayerGame		mpGame;					// handles rules for standard dm

	idSmokeParticles* 		smokeParticles;			// global smoke trails
	idEditEntities* 		editEntities;			// in game editing

	int						cinematicSkipTime;		// don't allow skipping cinemetics until this time has passed so player doesn't skip out accidently from a firefight
	int						cinematicStopTime;		// cinematics have several camera changes, so keep track of when we stop them so that we don't reset cinematicSkipTime unnecessarily
	int						cinematicMaxSkipTime;	// time to end cinematic when skipping.  there's a possibility of an infinite loop if the map isn't set up right.
	bool					inCinematic;			// game is playing cinematic (player controls frozen)
	bool					skipCinematic;

	int						framenum;
	int						time;					// in msec
	int						previousTime;			// time in msec of last frame

	int						vacuumAreaNum;			// -1 if level doesn't have any outside areas

	gameType_t				gameType;
	idLinkList<idEntity>	snapshotEntities;		// entities from the last snapshot
	int						realClientTime;			// real client time
	bool					isNewFrame;				// true if this is a new game frame, not a rerun due to prediction
	float					clientSmoothing;		// smoothing of other clients in the view
	int						entityDefBits;			// bits required to store an entity def number (signed: K19's implicit decls cross negative)

	static const char* 		sufaceTypeNames[ MAX_SURFACE_TYPES ];	// text names for surface types

	idEntityPtr<idEntity>	lastGUIEnt;				// last entity with a GUI, used by Cmd_NextGUI_f
	int						lastGUI;				// last GUI on the lastGUIEnt

	idEntityPtr<idPlayer>	playerActivateFragChamber;	// The player that activated the frag chamber

	idEntityPtr<idEntity>	portalSkyEnt;
	bool					portalSkyActive;

	void					SetPortalSkyEnt( idEntity* ent );
	bool					IsPortalSkyAcive();

	timeState_t				fast;
	timeState_t				slow;
	int						selectedGroup;

	slowmoState_t			slowmoState;
	float					slowmoScale;

	bool					quickSlowmoReset;

	virtual void			SelectTimeGroup( int timeGroup );
	virtual int				GetTimeGroupTime( int timeGroup );

	void					ComputeSlowScale();
	void					RunTimeGroup2( idUserCmdMgr& userCmdMgr );

	void					ResetSlowTimeVars();
	void					QuickSlowmoReset();


	void					Tokenize( idStrList& out, const char* in );

	// ---------------------- Public idGame Interface -------------------

	idGameLocal();

	virtual void			Init();
	virtual void			Shutdown();
	virtual void			SetServerInfo( const idDict& serverInfo );
	virtual const idDict& 	GetServerInfo();

	virtual const idDict& 	GetPersistentPlayerInfo( int clientNum );
	virtual void			SetPersistentPlayerInfo( int clientNum, const idDict& playerInfo );
	virtual void			InitFromNewMap( const char* mapName, idRenderWorld* renderWorld, idSoundWorld* soundWorld, int gameType, int randSeed );
	virtual bool			InitFromSaveGame( const char* mapName, idRenderWorld* renderWorld, idSoundWorld* soundWorld, idFile* saveGameFile, idFile* stringTableFile, int saveGameVersion );
	virtual void			SaveGame( idFile* saveGameFile, idFile* stringTableFile );
	virtual void			GetSaveGameDetails( idSaveGameDetails& gameDetails );
	virtual void			MapShutdown();
	virtual void			CacheDictionaryMedia( const idDict* dict );
	virtual void			Preload( const idPreloadManifest& manifest );
	virtual void			RunFrame( idUserCmdMgr& cmdMgr, gameReturn_t& gameReturn );
	void					RunAllUserCmdsForPlayer( idUserCmdMgr& cmdMgr, const int playerNumber );
	void					RunSingleUserCmd( usercmd_t& cmd, idPlayer& player );
	void					RunEntityThink( idEntity& ent, idUserCmdMgr& userCmdMgr );
	virtual bool			Draw( int clientNum );
	//puncho start - ai-harness
	virtual const aiHudStats_t*	GetAIStats() const;
	//puncho end
	virtual bool			HandlePlayerGuiEvent( const sysEvent_t* ev );
	virtual void			ServerWriteSnapshot( idSnapShot& ss );
	virtual void			ProcessReliableMessage( int clientNum, int type, const idBitMsg& msg );
	virtual void			ClientReadSnapshot( const idSnapShot& ss );
	virtual void			ClientRunFrame( idUserCmdMgr& cmdMgr, bool lastPredictFrame, gameReturn_t& ret );
	virtual void			GetContentDigests( unsigned int digests[ CONTENT_DIGEST_COUNT ] ) const;	//puncho - version rule (MP-A0), Game_network.cpp
	void					BuildReturnValue( gameReturn_t& ret );

	virtual int				GetMPGameModes( const char** * gameModes, const char** * gameModesDisplay );

	virtual void			GetClientStats( int clientNum, char* data, const int len );

	virtual bool			IsInGame() const
	{
		return GameState() == GAMESTATE_ACTIVE;
	}

	virtual int				MapPeerToClient( int peer ) const;
	virtual int				GetLocalClientNum() const;

	virtual void			GetAimAssistAngles( idAngles& angles );
	virtual float			GetAimAssistSensitivity();

	// ---------------------- Public idGameLocal Interface -------------------

	void					Printf( VERIFY_FORMAT_STRING const char* fmt, ... ) const;
	void					DPrintf( VERIFY_FORMAT_STRING const char* fmt, ... ) const;
	void					Warning( VERIFY_FORMAT_STRING const char* fmt, ... ) const;
	void					DWarning( VERIFY_FORMAT_STRING const char* fmt, ... ) const;
	void					Error( VERIFY_FORMAT_STRING const char* fmt, ... ) const;

	// Initializes all map variables common to both save games and spawned games
	void					LoadMap( const char* mapName, int randseed );

	void					LocalMapRestart();
	void					MapRestart();
	static void				MapRestart_f( const idCmdArgs& args );

	idMapFile* 				GetLevelMap();
	const char* 			GetMapName() const;

	int						NumAAS() const;
	idAAS* 					GetAAS( int num ) const;
	idAAS* 					GetAAS( const char* name ) const;
	void					SetAASAreaState( const idBounds& bounds, const int areaContents, bool closed );
	aasHandle_t				AddAASObstacle( const idBounds& bounds );
	void					RemoveAASObstacle( const aasHandle_t handle );
	void					RemoveAllAASObstacles();

	bool					CheatsOk( bool requirePlayer = true );
	gameState_t				GameState() const;
	idEntity* 				SpawnEntityType( const idTypeInfo& classdef, const idDict* args = NULL, bool bIsClientReadSnapshot = false );
	bool					SpawnEntityDef( const idDict& args, idEntity** ent = NULL, bool setDefaults = true );
	int						GetSpawnId( const idEntity* ent ) const;
	int						GetMapSpawnCount() const
	{
		return mapSpawnCount;
	}

	const idDeclEntityDef* 	FindEntityDef( const char* name, bool makeDefault = true ) const;
	const idDict* 			FindEntityDefDict( const char* name, bool makeDefault = true ) const;

	void					RegisterEntity( idEntity* ent, int forceSpawnId, const idDict& spawnArgsToCopy );
	void					UnregisterEntity( idEntity* ent );
	const idDict& 			GetSpawnArgs() const
	{
		return spawnArgs;
	}

	bool					RequirementMet( idEntity* activator, const idStr& requires, int removeItem );

	void					AlertAI( idEntity* ent );
	idActor* 				GetAlertEntity();

	bool					InPlayerPVS( idEntity* ent ) const;
	//puncho start - ai-avoidance (WP-A5): the grid is for the tic after the one it was built in
	bool					NeighborGridValid() const
	{
		return aiNeighborTic == framenum - 1;
	}
	//puncho start - ai-combat (plans/ai-combat/08-PLAN-shooter-tokens.md §3.3)
	// the tokens are for the tic after the pass, like the grid
	bool					ShooterTokensValid() const
	{
		return shooterTokenTic == framenum - 1;
	}
	//puncho end
	// the bodies within range (flat, centre to centre) of origin: indices into aiNeighbors, the caller filters
	int						GatherNeighbors( const idVec3& origin, float range, int* indices, int maxIndices ) const;
	// (WP-A7) the squad table: index by name, -1 none; sys.aiSetAvoid pins a class on a squad, false when either is unknown
	int						FindAISquad( const char* name ) const;
	bool					SetAISquadAvoid( const char* name, const char* avoid );
	//puncho end
	bool					InPlayerConnectedArea( idEntity* ent ) const;
	pvsHandle_t				GetPlayerPVS()
	{
		return playerPVS;
	};

	void					SetCamera( idCamera* cam );
	idCamera* 				GetCamera() const;
	bool			        SkipCinematic();
	void					CalcFov( float base_fov, float& fov_x, float& fov_y ) const;

	void					AddEntityToHash( const char* name, idEntity* ent );
	bool					RemoveEntityFromHash( const char* name, idEntity* ent );
	int						GetTargets( const idDict& args, idList< idEntityPtr<idEntity> >& list, const char* ref ) const;

	// returns the master entity of a trace.  for example, if the trace entity is the player's head, it will return the player.
	idEntity* 				GetTraceEntity( const trace_t& trace ) const;

	static void				ArgCompletion_EntityName( const idCmdArgs& args, void( *callback )( const char* s ) );
	idEntity* 				FindTraceEntity( idVec3 start, idVec3 end, const idTypeInfo& c, const idEntity* skip ) const;
	idEntity* 				FindEntity( const char* name ) const;
	idEntity* 				FindEntityUsingDef( idEntity* from, const char* match ) const;
	int						EntitiesWithinRadius( const idVec3 org, float radius, idEntity** entityList, int maxCount ) const;

	void					KillBox( idEntity* ent, bool catch_teleport = false );
	void					RadiusDamage( const idVec3& origin, idEntity* inflictor, idEntity* attacker, idEntity* ignoreDamage, idEntity* ignorePush, const char* damageDefName, float dmgPower = 1.0f );
	void					RadiusPush( const idVec3& origin, const float radius, const float push, const idEntity* inflictor, const idEntity* ignore, float inflictorScale, const bool quake );
	void					RadiusPushClipModel( const idVec3& origin, const float push, const idClipModel* clipModel );

	void					ProjectDecal( const idVec3& origin, const idVec3& dir, float depth, bool parallel, float size, const char* material, float angle = 0 );
	void					BloodSplat( const idVec3& origin, const idVec3& dir, float size, const char* material );

	void					CallFrameCommand( idEntity* ent, const function_t* frameCommand );
	void					CallObjectFrameCommand( idEntity* ent, const char* frameCommand );

	const idVec3& 			GetGravity() const;

	// added the following to assist licensees with merge issues
	int						GetFrameNum() const
	{
		return framenum;
	};
	int						GetTime() const
	{
		return time;
	};

	int						GetNextClientNum( int current ) const;
	idPlayer* 				GetClientByNum( int current ) const;

	idPlayer* 				GetLocalPlayer() const;

	void					SpreadLocations();
	idLocationEntity* 		LocationForPoint( const idVec3& point );	// May return NULL
	idEntity* 				SelectInitialSpawnPoint( idPlayer* player );

	void					SetPortalState( qhandle_t portal, int blockingBits );
	void					SaveEntityNetworkEvent( const idEntity* ent, int event, const idBitMsg* msg );
	int						ServerRemapDecl( int clientNum, declType_t type, int index );
	int						ClientRemapDecl( declType_t type, int index );
	//puncho start - content check (P1-A3a, K19): the implicit decl announce table, Game_network.cpp
	void					ServerSendDeclRemap( int first, int last, const lobbyUserID_t* lobbyUserID );
	void					ClientReadDeclRemap( const idBitMsg& msg );
	//puncho end
	void					SyncPlayersWithLobbyUsers( bool initial );
	void					ServerWriteInitialReliableMessages( int clientNum, lobbyUserID_t lobbyUserID );
	void					ServerSendNetworkSyncCvars();

	virtual void			SetInterpolation( const float fraction, const int serverGameMS, const int ssStartTime, const int ssEndTime );

	void					ServerProcessReliableMessage( int clientNum, int type, const idBitMsg& msg );
	void					ClientProcessReliableMessage( int type, const idBitMsg& msg );

	// Snapshot times - track exactly what times we are interpolating from and to
	int						GetSSEndTime() const
	{
		return netInterpolationInfo.ssEndTime;
	}
	int						GetSSStartTime() const
	{
		return netInterpolationInfo.ssStartTime;
	}

	virtual void			SetServerGameTimeMs( const int time );
	virtual int				GetServerGameTimeMs() const;

	idEntity* 				FindPredictedEntity( uint32 predictedKey, idTypeInfo* type );
	uint32					GeneratePredictionKey( idWeapon* weapon, idPlayer* playerAttacker, int overrideKey );

	int						GetLastClientUsercmdMilliseconds( int playerIndex ) const
	{
		return usercmdLastClientMilliseconds[ playerIndex ];
	}

	void					SetGlobalMaterial( const idMaterial* mat );
	const idMaterial* 		GetGlobalMaterial();

	void					SetGibTime( int _time )
	{
		nextGibTime = _time;
	};
	int						GetGibTime()
	{
		return nextGibTime;
	};

	virtual bool				InhibitControls();
	virtual bool				IsPDAOpen() const;
	virtual bool				IsPlayerChatting() const;

	// Creates leaderboards for each map/mode defined.
	virtual void				Leaderboards_Init();
	virtual void				Leaderboards_Shutdown();

	// MAIN MENU FUNCTIONS
	virtual void					Shell_Init( const char* filename, idSoundWorld* sw );
	virtual void					Shell_Cleanup();
	virtual void					Shell_Show( bool show );
	virtual void					Shell_ClosePause();
	virtual void					Shell_CreateMenu( bool inGame );
	virtual bool					Shell_IsActive() const;
	virtual bool					Shell_HandleGuiEvent( const sysEvent_t* sev );
	virtual void					Shell_Render();
	virtual void					Shell_ResetMenu();
	virtual void					Shell_SyncWithSession() ;
	virtual void					Shell_SetCanContinue( bool valid );
	virtual void					Shell_UpdateSavedGames();
	virtual void					Shell_UpdateClientCountdown( int countdown );
	virtual void					Shell_UpdateLeaderboard( const idLeaderboardCallback* callback );
	virtual void					Shell_SetGameComplete();
	virtual bool			        SkipCinematicScene();
	virtual bool                    CheckInCinematic();

	enum gameDemoCommand_t
	{
		GCMD_UNKNOWN,
		GCMD_GAMETIME,
	};

	void					Shell_ClearRepeater();

	const char* 			GetMapFileName()
	{
		return mapFileName.c_str();
	}

	const char* 			GetMPPlayerDefName() const;

private:
	idStr					mapFileName;			// name of the map, empty string if no map loaded
	idMapFile* 				mapFile;				// will be NULL during the game unless in-game editing is used
	bool					mapCycleLoaded;

	int						slotGen[MAX_GENTITIES];	// K9: the last generation each slot gave out; spawnIds[n] is the live entity's, or -1
	int						forcedGenEntityNum;		// K10: a client's one-shot, the next registration of this slot takes forcedGen; -1 for none
	int						forcedGen;
	int						mapSpawnCount;			// K11: one past the highest slot the map's entities took below ENTITYNUM_FIRST_NON_REPLICATED
	idList<int>				clientSpawnCensus;		// K14a: a client's own spawns in play, three index ranges per type number (g_clientSpawnCensus)

	//puncho start - content check (P1-A3a, K19): the implicit decl announce table, cleared by every load (InitAsyncNetwork).
	// The host fills it from ServerRemapDecl and broadcasts each new slot; a client fills it from the broadcasts, resolving
	// each name to its own decl
	struct declRemap_t
	{
		int					index;					// this machine's decl index; -1 for a client slot not announced yet
		short				type;					// declType_t
		bool				warned;					// a client: an index of this slot already resolved to NULL with a warning
	};
	declRemap_t				declRemap[ MAX_DECL_REMAP ];
	int						numDeclRemap;			// the host: slots in use; a client: one past the highest slot announced
	int						numDeclRemapSent;		// the host: slots already broadcast
	int						declRemapBytes;			// the host: the table's size as ServerSendDeclRemap writes it
	bool					declRemapDeferred;		// the host, inside ServerWriteSnapshot: new slots are broadcast at its end
	bool					declRemapWarned;		// the host: the table is full; a client: an index past the cap arrived
	idHashIndex				declRemapHash;			// the host: decl index -> slot, confirmed against the slot's type
	//puncho end

	idLocationEntity** 		locationEntities;		// for location names, etc

	idCamera* 				camera;
	const idMaterial* 		globalMaterial;			// for overriding everything

	idList<idAAS*>			aasList;				// area system

	idMenuHandler_Shell* 	shellHandler;

	idStrList				aasNames;

	idEntityPtr<idActor>	lastAIAlertEntity;
	int						lastAIAlertTime;

	idDict					spawnArgs;				// spawn args used during entity spawning  FIXME: shouldn't be necessary anymore

	pvsHandle_t				playerPVS;				// merged pvs of all players
	pvsHandle_t				playerConnectedAreas;	// all areas connected to any player area

	idVec3					gravity;				// global gravity vector
	gameState_t				gamestate;				// keeps track of whether we're spawning, shutting down, or normal gameplay
	bool					influenceActive;		// true when a phantasm is happening
	int						nextGibTime;

	idEventQueue			eventQueue;
	idEventQueue			savedEventQueue;

	idStaticList<spawnSpot_t, MAX_GENTITIES> spawnSpots;
	idStaticList<idEntity*, MAX_GENTITIES> initialSpots;
	int						currentInitialSpot;

	idStaticList<spawnSpot_t, MAX_GENTITIES> teamSpawnSpots[2];
	idStaticList<idEntity*, MAX_GENTITIES> teamInitialSpots[2];
	int						teamCurrentInitialSpot[2];

	struct netInterpolationInfo_t  		// Was in GameTimeManager.h in id5, needed common place to put this.
	{
		netInterpolationInfo_t()
			: pct( 0.0f )
			, serverGameMs( 0 )
			, previousServerGameMs( 0 )
			, ssStartTime( 0 )
			, ssEndTime( 0 )
		{}
		float	pct;					// % of current interpolation
		int		serverGameMs;			// Interpolated server game time
		int		previousServerGameMs;	// last frame's interpolated server game time
		int		ssStartTime;			// Server time of old snapshot
		int		ssEndTime;				// Server time of next snapshot
	};

	netInterpolationInfo_t	netInterpolationInfo;

	idDict					newInfo;

	idArray< int, MAX_PLAYERS >	usercmdLastClientMilliseconds;	// The latest client time the server has run.
	idArray< int, MAX_PLAYERS >	lastCmdRunTimeOnClient;
	idArray< int, MAX_PLAYERS >	lastCmdRunTimeOnServer;

	void					Clear();
	// returns true if the entity shouldn't be spawned at all in this game type or difficulty level
	bool					InhibitEntitySpawn( idDict& spawnArgs );
	// spawn entities from the map file
	void					SpawnMapEntities();
	// commons used by init, shutdown, and restart; a restart passes the generations its slots had (K11)
	void					MapPopulate( const int* slotGenBeforeRestart );
	void					MapClear( bool clearClients );

	// RB: spawn environment probes if there aren't any by default
	void					PopulateEnvironmentProbes();

	pvsHandle_t				GetClientPVS( idPlayer* player, pvsType_t type );
	void					SetupPlayerPVS();
	void					FreePlayerPVS();
	void					UpdateGravity();
	void					SortActiveEntityList();
	void					ShowTargets();
	void					RunDebugInfo();
	//puncho start - ai-harness: once per tic after ServiceEvents; the flush also runs from MapShutdown
	void					RunAIStats( int numEnts, int usTic );
	void					FlushAIProfileLog();
	void					ClearAIStats();			// the census, ring and aggregates; not the CSV file
	void					GrowMaxObstacleRadius( const idEntity* ent );	// ai-avoidance (WP-A4): an actor or moveable spawned or restored
	void					BuildNeighborGrid();	// ai-avoidance (WP-A5): after the think loop (AI_crowd.cpp)
	void					ReadAISquads();			// ai-avoidance (WP-A7): the ai_squad markers of mapFile (AI_crowd.cpp)
	//puncho end

	void					RunSharedThink();

	void					InitScriptForMap();
	void					SetScriptFPS( const float com_engineHz );
	void					SpawnPlayer( int clientNum );

	void					InitConsoleCommands();
	void					ShutdownConsoleCommands();

	void					InitAsyncNetwork();
	void					ShutdownAsyncNetwork();
	void					NetworkEventWarning( const entityNetEvent_t* event, VERIFY_FORMAT_STRING const char* fmt, ... );
	void					ServerProcessEntityNetworkEventQueue();
	void					ClientProcessEntityNetworkEventQueue();
	// call after any change to serverInfo. Will update various quick-access flags
	void					UpdateServerInfoFlags();
	void					RandomizeInitialSpawns();
	static int				sortSpawnPoints( const void* ptr1, const void* ptr2 );

	bool					SimulateProjectiles();
};

//============================================================================

extern idGameLocal			gameLocal;
extern idAnimManager		animationLib;

//============================================================================

class idGameError : public idException
{
public:
	idGameError( const char* text ) : idException( text ) {}
};

//============================================================================

template< class type >
ID_INLINE idEntityPtr<type>::idEntityPtr()
{
	spawnId = 0;
}

template< class type >
ID_INLINE void idEntityPtr<type>::Save( idSaveGame* savefile ) const
{
	savefile->WriteInt( spawnId );
}

template< class type >
ID_INLINE void idEntityPtr<type>::Restore( idRestoreGame* savefile )
{
	savefile->ReadInt( spawnId );
}

template< class type >
ID_INLINE idEntityPtr<type>& idEntityPtr<type>::operator=( const type* ent )
{
	if( ent == NULL )
	{
		spawnId = 0;
	}
	else
	{
		spawnId = ( gameLocal.spawnIds[ent->entityNumber] << GENTITYNUM_BITS ) | ent->entityNumber;
	}
	return *this;
}

template< class type >
ID_INLINE idEntityPtr< type >& idEntityPtr<type>::operator=( const idEntityPtr& ep )
{
	spawnId = ep.spawnId;
	return *this;
}


template< class type >
ID_INLINE bool idEntityPtr<type>::SetSpawnId( int id )
{
	// the reason for this first check is unclear:
	// the function returning false may mean the spawnId is already set right, or the entity is missing
	if( id == spawnId )
	{
		return false;
	}
	if( ( id >> GENTITYNUM_BITS ) == gameLocal.spawnIds[ id & ( ( 1 << GENTITYNUM_BITS ) - 1 ) ] )
	{
		spawnId = id;
		return true;
	}
	return false;
}

template< class type >
ID_INLINE bool idEntityPtr<type>::IsValid() const
{
	return ( gameLocal.spawnIds[ spawnId & ( ( 1 << GENTITYNUM_BITS ) - 1 ) ] == ( spawnId >> GENTITYNUM_BITS ) );
}

template< class type >
ID_INLINE type* idEntityPtr<type>::GetEntity() const
{
	int entityNum = spawnId & ( ( 1 << GENTITYNUM_BITS ) - 1 );
	// DG: removed extraneous parenthesis to shut up clang
	if( gameLocal.spawnIds[ entityNum ] == ( spawnId >> GENTITYNUM_BITS ) )
	{
		return static_cast<type*>( gameLocal.entities[ entityNum ] );
	}
	return NULL;
}

template< class type >
ID_INLINE int idEntityPtr<type>::GetEntityNum() const
{
	return ( spawnId & ( ( 1 << GENTITYNUM_BITS ) - 1 ) );
}

//  ===========================================================================

//
// these defines work for all startsounds from all entity types
// make sure to change script/doom_defs.script if you add any channels, or change their order
//
typedef enum
{
	SND_CHANNEL_ANY = SCHANNEL_ANY,
	SND_CHANNEL_VOICE = SCHANNEL_ONE,
	SND_CHANNEL_VOICE2,
	SND_CHANNEL_BODY,
	SND_CHANNEL_BODY2,
	SND_CHANNEL_BODY3,
	SND_CHANNEL_WEAPON,
	SND_CHANNEL_ITEM,
	SND_CHANNEL_HEART,
	SND_CHANNEL_PDA_AUDIO,
	SND_CHANNEL_PDA_VIDEO,
	SND_CHANNEL_DEMONIC,
	SND_CHANNEL_RADIO,

	// internal use only.  not exposed to script or framecommands.
	SND_CHANNEL_AMBIENT,
	SND_CHANNEL_DAMAGE,
	SND_CHANNEL_MUSIC	// RB
} gameSoundChannel_t;

// content masks
#define	MASK_ALL					(-1)
#define	MASK_SOLID					(CONTENTS_SOLID)
#define	MASK_MONSTERSOLID			(CONTENTS_SOLID|CONTENTS_MONSTERCLIP|CONTENTS_BODY)
#define	MASK_PLAYERSOLID			(CONTENTS_SOLID|CONTENTS_PLAYERCLIP|CONTENTS_BODY)
#define	MASK_DEADSOLID				(CONTENTS_SOLID|CONTENTS_PLAYERCLIP)
#define	MASK_WATER					(CONTENTS_WATER)
#define	MASK_OPAQUE					(CONTENTS_OPAQUE)
#define	MASK_SHOT_RENDERMODEL		(CONTENTS_SOLID|CONTENTS_RENDERMODEL)
#define	MASK_SHOT_BOUNDINGBOX		(CONTENTS_SOLID|CONTENTS_BODY)

const float DEFAULT_GRAVITY			= 1066.0f;
#define DEFAULT_GRAVITY_STRING		"1066"
const idVec3 DEFAULT_GRAVITY_VEC3( 0, 0, -DEFAULT_GRAVITY );

const int	CINEMATIC_SKIP_DELAY	= SEC2MS( 2.0f );

//============================================================================

#include "physics/Force.h"
#include "physics/Force_Constant.h"
#include "physics/Force_Drag.h"
#include "physics/Force_Grab.h"
#include "physics/Force_Field.h"
#include "physics/Force_Spring.h"
#include "physics/Physics.h"
#include "physics/Physics_Static.h"
#include "physics/Physics_StaticMulti.h"
#include "physics/Physics_Base.h"
#include "physics/Physics_Actor.h"
#include "physics/Physics_Monster.h"
#include "physics/Physics_Player.h"
#include "physics/Physics_Parametric.h"
#include "physics/Physics_RigidBody.h"
#include "physics/Physics_AF.h"

#include "SmokeParticles.h"

#include "Entity.h"
#include "GameEdit.h"
#include "Grabber.h"
#include "AF.h"
#include "IK.h"
#include "AFEntity.h"
#include "Misc.h"
#include "Actor.h"
#include "Projectile.h"
#include "Weapon.h"
#include "Light.h"
#include "EnvironmentProbe.h"
#include "WorldSpawn.h"
#include "Item.h"
#include "PlayerView.h"
#include "PlayerIcon.h"
#include "Achievements.h"
#include "AimAssist.h"
#include "Player.h"
#include "Mover.h"
#include "Camera.h"
#include "Moveable.h"
#include "Target.h"
#include "Trigger.h"
#include "Sound.h"
#include "Fx.h"
#include "SecurityCamera.h"
#include "BrittleFracture.h"

#include "ai/AI.h"
#include "anim/Anim_Testmodel.h"

// menus
#include "menus/MenuWidget.h"
#include "menus/MenuScreen.h"
#include "menus/MenuHandler.h"

#include "script/Script_Compiler.h"
#include "script/Script_Interpreter.h"
#include "script/Script_Thread.h"

#endif	/* !__GAME_LOCAL_H__ */
