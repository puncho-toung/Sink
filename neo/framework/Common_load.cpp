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

#include "Common_local.h"
#include "../sys/sys_lobby_backend.h"


#define LAUNCH_TITLE_DOOM_EXECUTABLE		"doom1.exe"
#define LAUNCH_TITLE_DOOM2_EXECUTABLE		"doom2.exe"


idCVar com_loadTimeline( "com_loadTimeline", "0", CVAR_SYSTEM | CVAR_BOOL, "print the 28 map transition phases" );
idCVar com_loadTimelineLog( "com_loadTimelineLog", "", CVAR_SYSTEM, "append load phases to loadtimeline/<plain name>.csv" );
struct loadTimelineRow_t
{
	uint64 us;
	uint64 readBytes;
	int64 imageBytes, vertexBytes, indexBytes;
};
static loadTimelineRow_t loadRows[LOAD_PHASE_COUNT];
static int loadCurrentPhase = -1;
static int loadIndex;
static uint64 loadPhaseStart;
static unsigned int loadReadStart;
static renderMapStats_t loadResourceStart;
static char loadPreviousMap[MAX_OSPATH];
static bool loadWasSavegame;
static const char* loadPhaseNames[LOAD_PHASE_COUNT] = {
	"MOVE", "BEGIN", "UNLOAD", "PURGE", "LOADGUI", "PRELOAD", "RENDER_WORLD", "MAP_PARSE", "CM", "GAME_INIT", "PVS", "AAS", "MEDIA", "SCRIPT", "SPAWN", "POST_SPAWN", "RESTORE", "SESSION", "FIRST_FRAME", "END_RENDER", "END_SOUND", "END_DECL", "END_UI", "END_FS", "SETTLE", "AUTOSAVE", "INTERACTIONS", "FINISH"
};

void idCommonLocal::LoadPhase( loadPhase_t phase )
{
	if( !com_loadTimeline.GetBool() && !com_loadTimelineLog.GetString()[0] )
	{
		loadCurrentPhase = -1;
		return;
	}
	if( phase == LOAD_MOVE || ( phase == LOAD_BEGIN && loadCurrentPhase != LOAD_MOVE ) )
	{
		memset( loadRows, 0, sizeof( loadRows ) );
		loadCurrentPhase = -1;
	}
	const uint64 now = Sys_Microseconds();
	const unsigned int reads = fileSystem->GetReadBytes();
	renderMapStats_t resources = {};
	renderSystem->GetMapStats( resources );
	if( loadCurrentPhase >= 0 )
	{
		loadTimelineRow_t& row = loadRows[loadCurrentPhase];
		row.us += now - loadPhaseStart;
		row.readBytes += ( unsigned int )( reads - loadReadStart );
		row.imageBytes += ( int64 )resources.imageBytes - ( int64 )loadResourceStart.imageBytes;
		row.vertexBytes += ( int64 )resources.staticVertexBytes - ( int64 )loadResourceStart.staticVertexBytes;
		row.indexBytes += ( int64 )resources.staticIndexBytes - ( int64 )loadResourceStart.staticIndexBytes;
	}
	if( phase == LOAD_BEGIN )
	{
		loadIndex++;
		idStr::Copynz( loadPreviousMap, currentMapName.c_str(), sizeof( loadPreviousMap ) );
		loadWasSavegame = mapSpawnData.savegameFile != NULL;
	}
	loadCurrentPhase = phase == LOAD_PHASE_COUNT ? -1 : phase;
	loadPhaseStart = now;
	loadReadStart = reads;
	loadResourceStart = resources;
}

void idCommonLocal::FinishLoadTimeline()
{
	if( loadCurrentPhase < 0 )
	{
		return;
	}
	LoadPhase( LOAD_PHASE_COUNT );
	idFile* csv = NULL;
	const char* name = com_loadTimelineLog.GetString();
	if( name[0] )
	{
		if( !ValidDiagnosticName( name ) )
		{
			Warning( "com_loadTimelineLog requires a plain name" );
			com_loadTimelineLog.SetString( "" );
		}
		else
		{
			char path[160];
			idStr::snPrintf( path, sizeof( path ), "loadtimeline/%s.csv", name );
			csv = fileSystem->OpenFileAppend( path, true, "fs_savepath" );
			if( csv != NULL && csv->Length() == 0 )
			{
				csv->Printf( "loadIndex,map,previousMap,savegame,phase,name,us,readBytes,imageBytes,vertexBytes,indexBytes\n" );
			}
		}
	}
	uint64 total = 0;
	for( int i = 0; i < LOAD_PHASE_COUNT; i++ )
	{
		const loadTimelineRow_t& row = loadRows[i];
		if( i != LOAD_MOVE )
		{
			total += row.us;
		}
		if( com_loadTimeline.GetBool() )
		{
			Printf( "LOAD %02d %-14s %llu us read %llu image %lld vertex %lld index %lld\n", i, loadPhaseNames[i], ( unsigned long long )row.us, ( unsigned long long )row.readBytes, ( long long )row.imageBytes, ( long long )row.vertexBytes, ( long long )row.indexBytes );
		}
		if( csv != NULL )
		{
			csv->Printf( "%d,%s,%s,%d,%d,%s,%llu,%llu,%lld,%lld,%lld\n", loadIndex, currentMapName.c_str(), loadPreviousMap, loadWasSavegame ? 1 : 0, i, loadPhaseNames[i], ( unsigned long long )row.us, ( unsigned long long )row.readBytes, ( long long )row.imageBytes, ( long long )row.vertexBytes, ( long long )row.indexBytes );
		}
	}
	if( csv != NULL )
	{
		fileSystem->CloseFile( csv );
	}
	if( com_loadTimeline.GetBool() )
	{
		Printf( "LOAD total %llu us (BEGIN..FINISH)\n", ( unsigned long long )total );
	}
}

idCVar com_levelKeep( "com_levelKeep", "0", CVAR_SYSTEM | CVAR_INTEGER, "game-driven asset grace in map loads (0 = legacy purge)", 0, 1000 );
idCVar com_levelKeepAll( "com_levelKeepAll", "0", CVAR_SYSTEM | CVAR_BOOL, "test asset grace on console loads too" );
struct levelCacheStats_t
{
	int loaded, kept, purged, pinned;
	int64 bytes;
};
static levelCacheStats_t levelCacheStats[4];
static const char* levelCacheNames[4] = { "images", "models", "sounds", "guis" };

void idCommonLocal::ReportLevelCache( int manager, int loaded, int kept, int purged, int pinned, int64 bytes )
{
	assert( manager >= 0 && manager < 4 );
	levelCacheStats_t& s = levelCacheStats[manager];
	s.loaded = loaded;
	s.kept = kept;
	s.purged = purged;
	s.pinned = pinned;
	s.bytes = bytes;
	Printf( "LEVELCACHE %s loaded=%d resident=%d purged=%d pinned=%d bytes=%lld\n", levelCacheNames[manager], loaded, kept, purged, pinned, ( long long )bytes );
}

CONSOLE_COMMAND( listLevelCache, "show the last completed load's asset inventory", NULL )
{
	for( int i = 0; i < 4; i++ )
	{
		const levelCacheStats_t& s = levelCacheStats[i];
		common->Printf( "LEVELCACHE %s loaded=%d resident=%d purged=%d pinned=%d bytes=%lld\n", levelCacheNames[i], s.loaded, s.kept, s.purged, s.pinned, ( long long )s.bytes );
	}
}

static const int MAX_LEVEL_PINS = 4;
static idPreloadManifest levelPins[MAX_LEVEL_PINS];
static bool levelPinActive[MAX_LEVEL_PINS];

void idCommon::PreloadManifestName( const char* map, idStr& result )
{
	idStr base;
	idStr( map ).ExtractFileBase( base );
	result = "maps/";
	result += base;
	result += ".preload";
}

int idCommonLocal::LevelKeepGrace() const
{
	return ( IsGameDrivenLoad() || com_levelKeepAll.GetBool() ) ? com_levelKeep.GetInteger() : 0;
}

bool idCommonLocal::IsLevelAssetPinned( int type, const char* name, int filter, int repeat, int usage, int cubeMap ) const
{
	if( LevelKeepGrace() == 0 )
	{
		return false;
	}
	for( int i = 0; i < MAX_LEVEL_PINS; i++ )
	{
		if( !levelPinActive[i] )
		{
			continue;
		}
		for( int j = 0; j < levelPins[i].NumResources(); j++ )
		{
			const preloadEntry_s& p = levelPins[i].GetPreloadByIndex( j );
			if( p.resType == type && p.resourceName.Icmp( name ) == 0 &&
				( type != PRELOAD_IMAGE || ( p.imgData.filter == filter && p.imgData.repeat == repeat && p.imgData.usage == usage && p.imgData.cubeMap == cubeMap ) ) )
			{
				return true;
			}
		}
	}
	return false;
}

CONSOLE_COMMAND( pinMap, "pin resident assets in a map preload manifest (four slots)", idCmdSystem::ArgCompletion_MapName )
{
	if( args.Argc() != 2 )
	{
		common->Printf( "usage: pinMap <map>\n" );
		return;
	}
	idStr name;
	idCommon::PreloadManifestName( args.Argv( 1 ), name );
	int slot = -1;
	for( int i = 0; i < MAX_LEVEL_PINS; i++ )
	{
		if( levelPinActive[i] && name.Icmp( levelPins[i].GetManifestName() ) == 0 )
		{
			return;
		}
		if( !levelPinActive[i] && slot == -1 )
		{
			slot = i;
		}
	}
	if( slot == -1 )
	{
		common->Warning( "pinMap: all %d slots occupied", MAX_LEVEL_PINS );
		return;
	}
	levelPinActive[slot] = levelPins[slot].LoadManifest( name );
	common->Printf( "PIN %s %s\n", name.c_str(), levelPinActive[slot] ? "active (resident assets only)" : "missing; nothing pinned" );
}

CONSOLE_COMMAND( unpinMap, "remove a map's asset pin", idCmdSystem::ArgCompletion_MapName )
{
	idStr name;
	idCommon::PreloadManifestName( args.Argv( 1 ), name );
	for( int i = 0; i < MAX_LEVEL_PINS; i++ )
	{
		if( levelPinActive[i] && name.Icmp( levelPins[i].GetManifestName() ) == 0 )
		{
			levelPinActive[i] = false;
			common->Printf( "PIN %s removed\n", name.c_str() );
		}
	}
}

CONSOLE_COMMAND( listPins, "list resident-only map asset pins", NULL )
{
	for( int i = 0; i < MAX_LEVEL_PINS; i++ )
	{
		if( levelPinActive[i] )
		{
			common->Printf( "PIN %d %s %d entries\n", i, levelPins[i].GetManifestName(), levelPins[i].NumResources() );
		}
	}
}

idCVar com_transitionWipeMs( "com_transitionWipeMs", "1000", CVAR_SYSTEM | CVAR_INTEGER, "game-driven total presentation budget; -1 uses legacy held wipes", -1, 10000 );
idCVar com_transitionOverlayMs( "com_transitionOverlayMs", "50", CVAR_SYSTEM | CVAR_INTEGER, "game-driven loading overlay delay after fade; -1 draws immediately", -1, 10000 );
idCVar com_transitionKeepInput( "com_transitionKeepInput", "0", CVAR_SYSTEM | CVAR_BOOL, "preserve held buttons and toggles across game-driven changes" );
idCVar com_transitionStrip( "com_transitionStrip", "0", CVAR_SYSTEM | CVAR_INTEGER, "game-driven load bits: 1 skip settle, 4 skip synchronous autosave (checkpoint requires MP-D0)", 0, 7 );

idCVar com_wipeSeconds( "com_wipeSeconds", "1", CVAR_SYSTEM, "" );
idCVar com_disableAutoSaves( "com_disableAutoSaves", "0", CVAR_SYSTEM | CVAR_BOOL, "" );
idCVar com_disableAllSaves( "com_disableAllSaves", "0", CVAR_SYSTEM | CVAR_BOOL, "" );


extern idCVar sys_lang;

extern idCVar g_demoMode;

// This is for the dirty hack to get a dialog to show up before we capture the screen for autorender.
const int NumScreenUpdatesToShowDialog = 25;



/*
================
idCommonLocal::StartWipe

Draws and captures the current state, then starts a wipe with that image
================
*/
void idCommonLocal::StartWipe( const char* _wipeMaterial, bool hold )
{
	console->Close();

	Draw();

	renderSystem->CaptureRenderToImage( "_currentRender" );

	wipeMaterial = declManager->FindMaterial( _wipeMaterial, false );

	wipeStartTime = Sys_Milliseconds();
	int duration = SEC2MS( com_wipeSeconds.GetFloat() );
	if( IsGameDrivenLoad() && com_transitionWipeMs.GetInteger() >= 0 )
	{
		// Loading consumes the presentation budget; never add a held second before IO.
		duration = hold ? 0 : Max( 0, com_transitionWipeMs.GetInteger() - ( wipeStartTime - transitionLoadStartTime ) );
	}
	wipeStopTime = wipeStartTime + duration;
	wipeHold = hold;
}

/*
================
idCommonLocal::CompleteWipe
================
*/
void idCommonLocal::CompleteWipe()
{
	while( Sys_Milliseconds() < wipeStopTime )
	{
		BusyWait();
		Sys_Sleep( 10 );
	}

	// ensure it is completely faded out
	wipeStopTime = Sys_Milliseconds();
	BusyWait();
}

/*
================
idCommonLocal::ClearWipe
================
*/
void idCommonLocal::ClearWipe()
{
	wipeHold = false;
	wipeStopTime = 0;
	wipeStartTime = 0;
	wipeForced = false;
}

/*
===============
idCommonLocal::StartNewGame
===============
*/
void idCommonLocal::StartNewGame( const char* mapName, bool devmap, int gameMode )
{
	mapSpawnData.gameDriven = false;
	if( session->GetSignInManager().GetMasterLocalUser() == NULL )
	{
		// For development make sure a controller is registered
		// Can't just register the local user because it will be removed because of it's persistent state
		session->GetSignInManager().SetDesiredLocalUsers( 1, 1 );
		session->GetSignInManager().Pump();
	}

	idStr mapNameClean = mapName;
	mapNameClean.StripFileExtension();
	mapNameClean.BackSlashesToSlashes();

	idMatchParameters matchParameters;
	matchParameters.mapName = mapNameClean;
	if( gameMode == GAME_MODE_SINGLEPLAYER )
	{
		matchParameters.numSlots = 1;
		matchParameters.gameMode = GAME_MODE_SINGLEPLAYER;
		matchParameters.gameMap = GAME_MAP_SINGLEPLAYER;
	}
	else
	{
		matchParameters.gameMap = mpGameMaps.Num();	// If this map isn't found in mpGameMaps, then set it to some undefined value (this happens when, for example, we load a box map with netmap)
		matchParameters.gameMode = gameMode;
		matchParameters.matchFlags = DefaultPartyFlags;
		for( int i = 0; i < mpGameMaps.Num(); i++ )
		{
			if( idStr::Icmp( mpGameMaps[i].mapFile, mapNameClean ) == 0 )
			{
				matchParameters.gameMap = i;
				break;
			}
		}
		matchParameters.numSlots = session->GetTitleStorageInt( "MAX_PLAYERS_ALLOWED", 4 );
	}

	cvarSystem->MoveCVarsToDict( CVAR_SERVERINFO, matchParameters.serverInfo );
	if( devmap )
	{
		matchParameters.serverInfo.Set( "devmap", "1" );
	}
	else
	{
		matchParameters.serverInfo.Delete( "devmap" );
	}

	session->QuitMatchToTitle();
	if( WaitForSessionState( idSession::IDLE ) )
	{
		session->CreatePartyLobby( matchParameters );
		if( WaitForSessionState( idSession::PARTY_LOBBY ) )
		{
			session->CreateMatch( matchParameters );
			if( WaitForSessionState( idSession::GAME_LOBBY ) )
			{
				cvarSystem->SetCVarBool( "developer", devmap );
				session->StartMatch();
			}
		}
	}
}

/*
===============
idCommonLocal::MoveToNewMap
Single player transition from one map to another
===============
*/
void idCommonLocal::MoveToNewMap( const char* mapName, bool devmap )
{
	mapSpawnData.gameDriven = !devmap;
	LoadPhase( LOAD_MOVE );
	mapSpawnData.worldTimeBase = devmap ? 0 : game->GetWorldTime();
	idMatchParameters matchParameters;
	matchParameters.numSlots = 1;
	matchParameters.gameMode = GAME_MODE_SINGLEPLAYER;
	matchParameters.gameMap = GAME_MAP_SINGLEPLAYER;
	matchParameters.mapName = mapName;
	cvarSystem->MoveCVarsToDict( CVAR_SERVERINFO, matchParameters.serverInfo );
	if( devmap )
	{
		matchParameters.serverInfo.Set( "devmap", "1" );
		mapSpawnData.persistentPlayerInfo.Clear();
	}
	else
	{
		matchParameters.serverInfo.Delete( "devmap" );
		mapSpawnData.persistentPlayerInfo = game->GetPersistentPlayerInfo( 0 );
	}
	session->QuitMatchToTitle();
	if( WaitForSessionState( idSession::IDLE ) )
	{
		session->CreatePartyLobby( matchParameters );
		if( WaitForSessionState( idSession::PARTY_LOBBY ) )
		{
			session->CreateMatch( matchParameters );
			if( WaitForSessionState( idSession::GAME_LOBBY ) )
			{
				session->StartMatch();
			}
		}
	}
}

/*
===============
idCommonLocal::UnloadMap

Performs cleanup that needs to happen between maps, or when a
game is exited.
Exits with mapSpawned = false
===============
*/
void idCommonLocal::UnloadMap()
{
	// EA-14: MapShutdown's close makes no ImGui call, so a button held across the unload must not
	// leave ImGui's mouse latches set (returns at once when ImGui is not initialised). D48: an
	// unload never writes the light editor's lights
	ImGuiHook::ResetInputLatches();

	// end the current map in the game
	if( game )
	{
		game->MapShutdown();
	}

	//puncho start - MP-A4 (plans/phase1/00-PLAN.md K26, K28): an error calls Stop, and so this, before its
	// exception unwinds the open scratch scopes, so the arena is only checked when no error is in progress
	if( mapSpawned )
	{
		if( com_errorEntered == ERP_NONE )
		{
			gameScratchStack.CheckClosed( "map unload" );
		}
		gameScratchStack.EndMap();
	}
	//puncho end

	mapSpawned = false;
}

/*
===============
idCommonLocal::LoadLoadingGui
===============
*/
void idCommonLocal::LoadLoadingGui( const char* mapName, bool& hellMap )
{
	defaultLoadscreen = false;
	loadGUI = new idSWF( "loading/default", NULL );

	if( g_demoMode.GetBool() )
	{
		hellMap = false;
		if( loadGUI != NULL )
		{
			const idMaterial* defaultMat = declManager->FindMaterial( "guis/assets/loadscreens/default" );
			renderSystem->LoadLevelImages();

			loadGUI->Activate( true );
			idSWFSpriteInstance* bgImg = loadGUI->GetRootObject().GetSprite( "bgImage" );
			if( bgImg != NULL )
			{
				bgImg->SetMaterial( defaultMat );
			}
		}
		defaultLoadscreen = true;
		return;
	}

	// load / program a gui to stay up on the screen while loading
	idStrStatic< MAX_OSPATH > stripped = mapName;
	stripped.StripFileExtension();
	stripped.StripPath();

	// use default load screen for demo
	idStrStatic< MAX_OSPATH > matName = "guis/assets/loadscreens/";
	matName.Append( stripped );
	const idMaterial* mat = declManager->FindMaterial( matName );

	renderSystem->LoadLevelImages();

	if( mat->GetImageWidth() < 32 )
	{
		mat = declManager->FindMaterial( "guis/assets/loadscreens/default" );
		renderSystem->LoadLevelImages();
	}

	loadTipList.SetNum( loadTipList.Max() );
	for( int i = 0; i < loadTipList.Max(); ++i )
	{
		loadTipList[i] = i;
	}

	if( loadGUI != NULL )
	{
		loadGUI->Activate( true );
		nextLoadTip = Sys_Milliseconds() + LOAD_TIP_CHANGE_INTERVAL;

		idSWFSpriteInstance* bgImg = loadGUI->GetRootObject().GetSprite( "bgImage" );
		if( bgImg != NULL )
		{
			bgImg->SetMaterial( mat );
		}

		idSWFSpriteInstance* overlay = loadGUI->GetRootObject().GetSprite( "overlay" );

		const idDeclEntityDef* mapDef = static_cast<const idDeclEntityDef*>( declManager->FindType( DECL_MAPDEF, mapName, false ) );
		if( mapDef != NULL )
		{
			isHellMap = mapDef->dict.GetBool( "hellMap", false );

			if( isHellMap && overlay != NULL )
			{
				overlay->SetVisible( false );
			}

			idStr desc;
			idStr subTitle;
			idStr displayName;
			idSWFTextInstance* txtVal = NULL;

			txtVal = loadGUI->GetRootObject().GetNestedText( "txtRegLoad" );
			displayName = idLocalization::GetString( mapDef->dict.GetString( "name", mapName ) );

			if( txtVal != NULL )
			{
				txtVal->SetText( "#str_00408" );
				txtVal->SetStrokeInfo( true, 2.0f, 1.0f );
			}

			const idMatchParameters& matchParameters = session->GetActingGameStateLobbyBase().GetMatchParms();
			if( matchParameters.gameMode == GAME_MODE_SINGLEPLAYER )
			{
				desc = idLocalization::GetString( mapDef->dict.GetString( "desc", "" ) );
				subTitle = idLocalization::GetString( mapDef->dict.GetString( "subTitle", "" ) );
			}
			else
			{
				const idStrList& modes = common->GetModeDisplayList();
				subTitle = modes[ idMath::ClampInt( 0, modes.Num() - 1, matchParameters.gameMode ) ];

				const char* modeDescs[] = { "#str_swf_deathmatch_desc", "#str_swf_tourney_desc", "#str_swf_team_deathmatch_desc", "#str_swf_lastman_desc", "#str_swf_ctf_desc" };
				desc = idLocalization::GetString( modeDescs[matchParameters.gameMode] );
			}

			if( !isHellMap )
			{
				txtVal = loadGUI->GetRootObject().GetNestedText( "txtName" );
			}
			else
			{
				txtVal = loadGUI->GetRootObject().GetNestedText( "txtHellName" );
			}
			if( txtVal != NULL )
			{
				txtVal->SetText( displayName );
				txtVal->SetStrokeInfo( true, 2.0f, 1.0f );
			}

			txtVal = loadGUI->GetRootObject().GetNestedText( "txtSub" );
			if( txtVal != NULL && !isHellMap )
			{
				txtVal->SetText( subTitle );
				txtVal->SetStrokeInfo( true, 1.75f, 0.75f );
			}

			txtVal = loadGUI->GetRootObject().GetNestedText( "txtDesc" );
			if( txtVal != NULL )
			{
				if( isHellMap )
				{
					txtVal->SetText( va( "\n%s", desc.c_str() ) );
				}
				else
				{
					txtVal->SetText( desc );
				}
				txtVal->SetStrokeInfo( true, 1.75f, 0.75f );
			}
		}
	}
}

/*
===============
idCommonLocal::ExecuteMapChange

Performs the initialization of a game based on session match parameters, used for both single
player and multiplayer, but not for renderDemos, which don't create a game at all.
Exits with mapSpawned = true
===============
*/
void idCommonLocal::ExecuteMapChange()
{
	if( session->GetState() != idSession::LOADING )
	{
		idLib::Warning( "Session state is not LOADING in ExecuteMapChange" );
		return;
	}

	// Clear all dialogs before beginning the load
	common->Dialog().ClearDialogs( true );

	// Remember the current load ID.
	// This is so we can tell if we had a new loadmap request from within an existing loadmap call
	const int cachedLoadingID = session->GetLoadingID();

	const idMatchParameters& matchParameters = session->GetActingGameStateLobbyBase().GetMatchParms();

	if( matchParameters.numSlots <= 0 )
	{
		idLib::Warning( "numSlots <= 0 in ExecuteMapChange" );
		return;
	}

	insideExecuteMapChange = true;

	common->Printf( "--------- Execute Map Change ---------\n" );
	common->Printf( "Map: %s\n", matchParameters.mapName.c_str() );

	// ensure that r_znear is reset to the default value
	// this fixes issues with the projection matrix getting messed up when switching maps or loading a saved game
	// while an in-game cinematic is playing.
	cvarSystem->SetCVarFloat( "r_znear", 3.0f );

	// reset all cheat cvars for a multiplayer game
	if( IsMultiplayer() )
	{
		cvarSystem->ResetFlaggedVariables( CVAR_CHEAT );
	}

	int start = Sys_Milliseconds();
	LoadPhase( LOAD_BEGIN );

	for( int i = 0; i < MAX_INPUT_DEVICES; i++ )
	{
		Sys_SetRumble( i, 0, 0 );
	}

	// close console and remove any prints from the notify lines
	console->Close();

	// clear all menu sounds
	soundWorld->Pause();
	menuSoundWorld->ClearAllSoundEmitters();
	soundSystem->SetPlayingSoundWorld( menuSoundWorld );
	soundSystem->Render();

	// extract the map name from serverinfo
	currentMapName = matchParameters.mapName;
	currentMapName.StripFileExtension();

	idStrStatic< MAX_OSPATH > fullMapName = "maps/";
	fullMapName += currentMapName;
	fullMapName.SetFileExtension( "map" );

	if( mapSpawnData.savegameFile )
	{
		fileSystem->BeginLevelLoad( currentMapName, NULL, 0 );
	}
	else
	{
		fileSystem->BeginLevelLoad( currentMapName, saveFile.GetDataPtr(), saveFile.GetAllocated() );
	}

	// capture the current screen and start a wipe
	// immediately complete the wipe to fade out the level transition
	// run the wipe to completion
	StartWipe( "wipeMaterial", true );
	CompleteWipe();
	transitionLoadStartTime = Sys_Milliseconds();
	transitionGuiDraws = 0;
	if( IsGameDrivenLoad() )
	{
		Printf( "TRANSITION wipe=%d overlay=%d keepInput=%d strip=%d\n", com_transitionWipeMs.GetInteger(), com_transitionOverlayMs.GetInteger(), com_transitionKeepInput.GetBool(), com_transitionStrip.GetInteger() );
	}

	int sm = Sys_Milliseconds();
	// shut down the existing game if it is running
	LoadPhase( LOAD_UNLOAD );
	UnloadMap();
	int ms = Sys_Milliseconds() - sm;
	common->Printf( "%6d msec to unload map\n", ms );

	// Free media from previous level and
	// note which media we are going to need to load
	sm = Sys_Milliseconds();
	LoadPhase( LOAD_PURGE );
	renderSystem->BeginLevelLoad();
	soundSystem->BeginLevelLoad();
	declManager->BeginLevelLoad();
	uiManager->BeginLevelLoad();
	ms = Sys_Milliseconds() - sm;
	common->Printf( "%6d msec to free assets\n", ms );

	//Sys_DumpMemory( true );

	// load / program a gui to stay up on the screen while loading
	// set the loading gui that we will wipe to
	LoadPhase( LOAD_LOADGUI );
	bool hellMap = false;
	LoadLoadingGui( currentMapName, hellMap );

	// Stop rendering the wipe
	ClearWipe();


	LoadPhase( LOAD_PRELOAD );
	if( fileSystem->UsingResourceFiles() )
	{
		idStr manifestName;
		PreloadManifestName( currentMapName, manifestName );
		idPreloadManifest manifest;
		manifest.LoadManifest( manifestName );
		renderSystem->Preload( manifest, currentMapName );
		soundSystem->Preload( manifest );
		game->Preload( manifest );
	}

	if( common->IsMultiplayer() )
	{
		// In multiplayer, make sure the player is either 60Hz or 120Hz
		// to avoid potential issues.
		const float mpEngineHz = ( com_engineHz.GetFloat() < 90.0f ) ? 60.0f : 120.0f;
		com_engineHz_denominator = 100LL * mpEngineHz;
		com_engineHz_latched = mpEngineHz;
	}
	else
	{
		// allow com_engineHz to be changed between map loads
		com_engineHz_denominator = 100LL * com_engineHz.GetFloat();
		com_engineHz_latched = com_engineHz.GetFloat();
	}

	// note any warning prints that happen during the load process
	common->ClearWarnings( currentMapName );

	// release the mouse cursor
	// before we do this potentially long operation
	Sys_GrabMouseCursor( false );

	// let the renderSystem load all the geometry
	LoadPhase( LOAD_RENDER_WORLD );
	if( !renderWorld->InitFromMap( fullMapName ) )
	{
		common->Error( "couldn't load %s", fullMapName.c_str() );
	}

	// for the synchronous networking we needed to roll the angles over from
	// level to level, but now we can just clear everything
	usercmdGen->InitForNewMap( IsGameDrivenLoad() && com_transitionKeepInput.GetBool() );

	// load and spawn all other entities ( from a savegame possibly )
	if( mapSpawnData.savegameFile )
	{
		if( !game->InitFromSaveGame( fullMapName, renderWorld, soundWorld, mapSpawnData.savegameFile, mapSpawnData.stringTableFile, mapSpawnData.savegameVersion ) )
		{
			// If the loadgame failed, end the session, which will force us to go back to the main menu
			session->QuitMatchToTitle();
		}
	}
	else
	{
		if( !IsMultiplayer() )
		{
			assert( game->GetLocalClientNum() == 0 );
			assert( matchParameters.gameMode == GAME_MODE_SINGLEPLAYER );
			assert( matchParameters.gameMap == GAME_MAP_SINGLEPLAYER );
			game->SetPersistentPlayerInfo( 0, mapSpawnData.persistentPlayerInfo );
		}
		game->SetServerInfo( matchParameters.serverInfo );
		game->SetWorldTimeBase( mapSpawnData.worldTimeBase );
		game->InitFromNewMap( fullMapName, renderWorld, soundWorld, matchParameters.gameMode, Sys_Milliseconds() );
	}

	LoadPhase( LOAD_SESSION );
	game->Shell_CreateMenu( true );

	// Reset some values important to multiplayer
	ResetNetworkingState();

	// If the session state is not loading here, something went wrong.
	if( session->GetState() == idSession::LOADING && session->GetLoadingID() == cachedLoadingID )
	{
		// Notify session we are done loading
		session->LoadingFinished();

		while( session->GetState() == idSession::LOADING )
		{
			Sys_GenerateEvents();
			session->UpdateSignInManager();
			session->Pump();
			Sys_Sleep( 10 );
		}
	}

	if( !mapSpawnData.savegameFile )
	{
		LoadPhase( LOAD_FIRST_FRAME );
		// run a single frame to catch any resources that are referenced by events posted in spawn
		idUserCmdMgr emptyCommandManager;
		gameReturn_t emptyGameReturn;
		for( int playerIndex = 0; playerIndex < MAX_PLAYERS; ++playerIndex )
		{
			emptyCommandManager.PutUserCmdForPlayer( playerIndex, usercmd_t() );
		}
		if( IsClient() )
		{
			game->ClientRunFrame( emptyCommandManager, false, emptyGameReturn );
		}
		else
		{
			game->RunFrame( emptyCommandManager, emptyGameReturn );
		}
	}

	LoadPhase( LOAD_END_RENDER );
	renderSystem->EndLevelLoad();
	LoadPhase( LOAD_END_SOUND );
	soundSystem->EndLevelLoad();
	LoadPhase( LOAD_END_DECL );
	declManager->EndLevelLoad();
	LoadPhase( LOAD_END_UI );
	uiManager->EndLevelLoad( currentMapName );
	LoadPhase( LOAD_END_FS );
	fileSystem->EndLevelLoad();

	if( !mapSpawnData.savegameFile && !IsMultiplayer() )
	{
		LoadPhase( LOAD_SETTLE );
		common->Printf( "----- Running initial game frames -----\n" );

		// In single player, run a bunch of frames to make sure ragdolls are settled
		idUserCmdMgr emptyCommandManager;
		gameReturn_t emptyGameReturn;
		for( int i = 0; i < ( IsGameDrivenLoad() && ( com_transitionStrip.GetInteger() & 1 ) ? 0 : 100 ); i++ )
		{
			for( int playerIndex = 0; playerIndex < MAX_PLAYERS; ++playerIndex )
			{
				emptyCommandManager.PutUserCmdForPlayer( playerIndex, usercmd_t() );
			}
			game->RunFrame( emptyCommandManager, emptyGameReturn );
		}

		// kick off an auto-save of the game (so we can always continue in this map if we die before hitting an autosave)
		LoadPhase( LOAD_AUTOSAVE );
		common->Printf( "----- Saving Game -----\n" );
		if( IsGameDrivenLoad() && ( com_transitionStrip.GetInteger() & 4 ) )
		{
			static bool warnedCheckpoint = false;
			if( !warnedCheckpoint )
			{
				Warning( "transition autosave skipped: MP-D0 RequestCheckpoint is unavailable" );
				warnedCheckpoint = true;
			}
		}
		else
		{
			SaveGame( "autosave" );
		}
	}

	LoadPhase( LOAD_INTERACTIONS );
	common->Printf( "----- Generating Interactions -----\n" );

	// let the renderSystem generate interactions now that everything is spawned
	renderWorld->GenerateAllInteractions();
	LoadPhase( LOAD_FINISH );

	{
		int vertexMemUsedKB = vertexCache.staticData.vertexMemUsed.GetValue() / 1024;
		int indexMemUsedKB = vertexCache.staticData.indexMemUsed.GetValue() / 1024;
		idLib::Printf( "Used %dkb of static vertex memory (%d%%)\n", vertexMemUsedKB, vertexMemUsedKB * 100 / ( STATIC_VERTEX_MEMORY / 1024 ) );
		idLib::Printf( "Used %dkb of static index memory (%d%%)\n", indexMemUsedKB, indexMemUsedKB * 100 / ( STATIC_INDEX_MEMORY / 1024 ) );
	}

	if( common->JapaneseCensorship() )
	{
		if( currentMapName.Icmp( "game/mp/d3xpdm3" ) == 0 )
		{
			const idMaterial* gizpool2 = declManager->FindMaterial( "textures/hell/gizpool2" );
			idMaterial* chiglass1bluex = const_cast<idMaterial*>( declManager->FindMaterial( "textures/sfx/chiglass1bluex" ) );
			idTempArray<char> text( gizpool2->GetTextLength() );
			gizpool2->GetText( text.Ptr() );
			chiglass1bluex->Parse( text.Ptr(), text.Num(), false );
		}
	}

	common->PrintWarnings();

	session->Pump();

	if( session->GetState() != idSession::INGAME )
	{
		// Something went wrong, don't process stale reliables that have been queued up.
		reliableQueue.Clear();
	}

	if( !IsGameDrivenLoad() || !com_transitionKeepInput.GetBool() )
	{
		usercmdGen->Clear();
	}

	// remove any prints from the notify lines
	console->ClearNotifyLines();

	Sys_SetPhysicalWorkMemory( -1, -1 );

	// at this point we should be done with the loading gui so we kill it
	delete loadGUI;
	loadGUI = NULL;


	// capture the current screen and start a wipe
	StartWipe( "wipe2Material" );

	// we are valid for game draws now
	insideExecuteMapChange = false;
	mapSpawned = true;
	if( !IsGameDrivenLoad() || !com_transitionKeepInput.GetBool() )
	{
		Sys_ClearEvents();
	}
	if( IsGameDrivenLoad() )
	{
		Printf( "TRANSITION guiDraws=%d\n", transitionGuiDraws );
	}
	transitionLoadStartTime = 0;

	int	msec = Sys_Milliseconds() - start;
	FinishLoadTimeline();
	common->Printf( "%6d msec to load %s\n", msec, currentMapName.c_str() );
	//Sys_DumpMemory( false );

	// Issue a render at the very end of the load process to update soundTime before the first frame
	soundSystem->Render();
}

/*
===============
idCommonLocal::UpdateLevelLoadPacifier

Pumps the session and if multiplayer, displays dialogs during the loading process.
===============
*/
void idCommonLocal::UpdateLevelLoadPacifier()
{
	autoRenderIconType_t icon = AUTORENDER_DEFAULTICON;
	bool autoswapsRunning = renderSystem->AreAutomaticBackgroundSwapsRunning( &icon );
	if( !insideExecuteMapChange && !autoswapsRunning )
	{
		return;
	}

	const int sessionUpdateTime = common->IsMultiplayer() ? 16 : 100;

	const int time = Sys_Milliseconds();

	// Throttle session pumps.
	if( time - lastPacifierSessionTime >= sessionUpdateTime )
	{
		lastPacifierSessionTime = time;

		Sys_GenerateEvents();

		session->UpdateSignInManager();
		session->Pump();
		session->ProcessSnapAckQueue();
	}

	if( autoswapsRunning )
	{
		// If autoswaps are running, only update if a Dialog is shown/dismissed
		bool dialogState = Dialog().HasAnyActiveDialog();
		if( lastPacifierDialogState != dialogState )
		{
			lastPacifierDialogState = dialogState;
			renderSystem->EndAutomaticBackgroundSwaps();
			if( dialogState )
			{
				icon = AUTORENDER_DIALOGICON; // Done this way to handle the rare case of a tip changing at the same time a dialog comes up
				for( int i = 0; i < NumScreenUpdatesToShowDialog; ++i )
				{
					UpdateScreen( false );
				}
			}
			renderSystem->BeginAutomaticBackgroundSwaps( icon );
		}
	}
	else
	{
		// On the PC just update at a constant rate for the Steam overlay
		if( time - lastPacifierGuiTime >= 50 )
		{
			lastPacifierGuiTime = time;
			UpdateScreen( false );
		}
	}

	if( time >= nextLoadTip && loadGUI != NULL && loadTipList.Num() > 0 && !defaultLoadscreen )
	{
		if( autoswapsRunning )
		{
			renderSystem->EndAutomaticBackgroundSwaps();
		}
		nextLoadTip = time + LOAD_TIP_CHANGE_INTERVAL;
		const int rnd = time % loadTipList.Num();
		idStrStatic<20> tipId;
		tipId.Format( "#str_loadtip_%d", loadTipList[ rnd ] );
		loadTipList.RemoveIndex( rnd );

		idSWFTextInstance* txtVal = loadGUI->GetRootObject().GetNestedText( "txtDesc" );
		if( txtVal != NULL )
		{
			if( isHellMap )
			{
				txtVal->SetText( va( "\n%s", idLocalization::GetString( tipId ) ) );
			}
			else
			{
				txtVal->SetText( idLocalization::GetString( tipId ) );
			}
			txtVal->SetStrokeInfo( true, 1.75f, 0.75f );
		}

		UpdateScreen( false );

		if( autoswapsRunning )
		{
			renderSystem->BeginAutomaticBackgroundSwaps( icon );
		}
	}
}

// RB begin
void idCommonLocal::LoadPacifierInfo( VERIFY_FORMAT_STRING const char* fmt, ... )
{
	char msg[256];

	va_list argptr;
	va_start( argptr, fmt );
	idStr::vsnPrintf( msg, 256 - 1, fmt, argptr );
	msg[ sizeof( msg ) - 1 ] = '\0';
	va_end( argptr );

	loadPacifierStatus = msg;

	//if( com_refreshOnPrint )
	//{
	//	UpdateScreen( false );
	//}
}

void idCommonLocal::LoadPacifierProgressTotal( int total )
{
	loadPacifierCount = 0;
	loadPacifierExpectedCount = total;
	loadPacifierTics = 0;
	loadPacifierNextTicCount = 0;
}

void idCommonLocal::LoadPacifierProgressIncrement( int step )
{
	loadPacifierCount += step;

	// don't refresh the UI with every step if there are e.g. 1300 steps
	if( ( ( loadPacifierCount + 1 ) >= loadPacifierNextTicCount ) && loadPacifierExpectedCount > 0 )
	{
		size_t ticsNeeded = ( size_t )( ( ( double )( loadPacifierCount + 1 ) / loadPacifierExpectedCount ) * 50.0 );

		//do
		//{
		//common->Printf( "*" );
		//}
		//while( ++loadPacifierTics < ticsNeeded );
		loadPacifierTics = ticsNeeded;

		loadPacifierNextTicCount = ( size_t )( ( loadPacifierTics / 50.0 ) * loadPacifierExpectedCount );
		if( loadPacifierCount == ( loadPacifierExpectedCount - 1 ) )
		{
			// reset

			//if( tics < 51 )
			//{
			//	common->Printf( "*" );
			//}
			//common->Printf( "\n" );

			//stateUI.progress = 1;

			//loadPacifierCount = 0;
			//loadPacifierExpectedCount = 0;
			//loadPacifierTics = 0;
			//loadPacifierNextTicCount = 0;
		}

		UpdateLevelLoadPacifier();
	}

	if( loadPacifierCount >= loadPacifierExpectedCount )
	{
		loadPacifierExpectedCount = 0;
	}
}

bool idCommonLocal::LoadPacifierRunning()
{
	return loadPacifierExpectedCount > 0;
}
// RB end

// foresthale 2014-05-30: loading progress pacifier for binarize operations only
void idCommonLocal::LoadPacifierBinarizeFilename( const char* filename, const char* reason )
{
	idLib::Printf( "Binarize File: '%s' - reason '%s'\n", filename, reason );

	// we won't actually show updates on very quick files (<16ms), so keep this false until the first progress
	loadPacifierBinarizeActive = false;
	loadPacifierBinarizeFilename = filename;
	loadPacifierBinarizeInfo = "";
	loadPacifierBinarizeProgress = 0.0f;
	loadPacifierBinarizeStartTime = Sys_Milliseconds();
	loadPacifierBinarizeMiplevel = 0;
	loadPacifierBinarizeMiplevelTotal = 0;
}

void idCommonLocal::LoadPacifierBinarizeInfo( const char* info )
{
	loadPacifierBinarizeInfo = info;
}

void idCommonLocal::LoadPacifierBinarizeMiplevel( int level, int maxLevel )
{
	loadPacifierBinarizeMiplevel = level;
	loadPacifierBinarizeMiplevelTotal = maxLevel;
}

// foresthale 2014-05-30: loading progress pacifier for binarize operations only
void idCommonLocal::LoadPacifierBinarizeProgress( float progress )
{
	static int lastUpdateTime = 0;
	int time = Sys_Milliseconds();
	if( progress == 0.0f )
	{
		// restart the progress, so that if multiple images have to be
		// binarized for one filename, we don't give bogus estimates...
		loadPacifierBinarizeStartTime = Sys_Milliseconds();
	}

	loadPacifierBinarizeProgress = progress;

	// RB: update every 2 milliseconds have passed
	if( ( time - lastUpdateTime ) >= 2 )
	{
		lastUpdateTime = time;
		loadPacifierBinarizeActive = true;

		UpdateLevelLoadPacifier();
	}
}

// foresthale 2014-05-30: loading progress pacifier for binarize operations only
void idCommonLocal::LoadPacifierBinarizeEnd()
{
	loadPacifierBinarizeActive = false;
	loadPacifierBinarizeStartTime = 0;
	loadPacifierBinarizeProgress = 0.0f;
	loadPacifierBinarizeTimeLeft = 0.0f;
	loadPacifierBinarizeFilename = "";
	loadPacifierBinarizeProgressTotal = 0;
	loadPacifierBinarizeProgressCurrent = 0;
	loadPacifierBinarizeMiplevel = 0;
	loadPacifierBinarizeMiplevelTotal = 0;
}

// foresthale 2014-05-30: loading progress pacifier for binarize operations only
void idCommonLocal::LoadPacifierBinarizeProgressTotal( int total )
{
	loadPacifierBinarizeProgressTotal = total;
	loadPacifierBinarizeProgressCurrent = 0;
}

// foresthale 2014-05-30: loading progress pacifier for binarize operations only
void idCommonLocal::LoadPacifierBinarizeProgressIncrement( int step )
{
	loadPacifierBinarizeProgressCurrent += step;

	if( loadPacifierBinarizeProgressTotal > 0 )
	{
		LoadPacifierBinarizeProgress( ( float )loadPacifierBinarizeProgressCurrent / loadPacifierBinarizeProgressTotal );
	}
}

/*
===============
idCommonLocal::ScrubSaveGameFileName

Turns a bad file name into a good one or your money back
===============
*/
void idCommonLocal::ScrubSaveGameFileName( idStr& saveFileName ) const
{
	int i;
	idStr inFileName;

	inFileName = saveFileName;
	inFileName.RemoveColors();
	inFileName.StripFileExtension();

	saveFileName.Clear();

	int len = inFileName.Length();
	for( i = 0; i < len; i++ )
	{
		if( strchr( "',.~!@#$%^&*()[]{}<>\\|/=?+;:-\'\"", inFileName[i] ) )
		{
			// random junk
			saveFileName += '_';
		}
		else if( ( const unsigned char )inFileName[i] >= 128 )
		{
			// high ascii chars
			saveFileName += '_';
		}
		else if( inFileName[i] == ' ' )
		{
			saveFileName += '_';
		}
		else
		{
			saveFileName += inFileName[i];
		}
	}
}

/*
===============
idCommonLocal::SaveGame
===============
*/
bool idCommonLocal::SaveGame( const char* saveName )
{
	if( pipelineFile != NULL )
	{
		// We're already in the middle of a save. Leave us alone.
		return false;
	}

	if( com_disableAllSaves.GetBool() || ( com_disableAutoSaves.GetBool() && ( idStr::Icmp( saveName, "autosave" ) == 0 ) ) )
	{
		return false;
	}

	if( IsMultiplayer() )
	{
		common->Printf( "Can't save during net play.\n" );
		return false;
	}

	if( mapSpawnData.savegameFile != NULL )
	{
		return false;
	}

	const idDict& persistentPlayerInfo = game->GetPersistentPlayerInfo( 0 );
	if( persistentPlayerInfo.GetInt( "health" ) <= 0 )
	{
		common->Printf( "You must be alive to save the game\n" );
		return false;
	}

	soundWorld->Pause();
	soundSystem->SetPlayingSoundWorld( menuSoundWorld );
	soundSystem->Render();

	Dialog().ShowSaveIndicator( true );
	if( insideExecuteMapChange )
	{
		UpdateLevelLoadPacifier();
	}
	else
	{
		// Heremake sure we pump the gui enough times to show the 'saving' dialog
		const bool captureToImage = false;
		for( int i = 0; i < NumScreenUpdatesToShowDialog; ++i )
		{
			UpdateScreen( captureToImage );
		}
		renderSystem->BeginAutomaticBackgroundSwaps( AUTORENDER_DIALOGICON );
	}

	// Make sure the file is writable and the contents are cleared out (Set to write from the start of file)
	saveFile.MakeWritable();
	saveFile.Clear( false );
	stringsFile.MakeWritable();
	stringsFile.Clear( false );

	// Setup the save pipeline
	pipelineFile = new( TAG_SAVEGAMES ) idFile_SaveGamePipelined();
	pipelineFile->OpenForWriting( &saveFile );

	// Write SaveGame Header:
	// Game Name / Version / Map Name / Persistant Player Info

	// game
	const char* gamename = GAME_NAME;
	saveFile.WriteString( gamename );

	// map
	saveFile.WriteString( currentMapName );

	saveFile.WriteBool( consoleUsed );

	game->GetServerInfo().WriteToFileHandle( &saveFile );

	// let the game save its state
	game->SaveGame( pipelineFile, &stringsFile );

	pipelineFile->Finish();

	idSaveGameDetails gameDetails;
	game->GetSaveGameDetails( gameDetails );

	gameDetails.descriptors.Set( SAVEGAME_DETAIL_FIELD_LANGUAGE, sys_lang.GetString() );
	gameDetails.descriptors.SetInt( SAVEGAME_DETAIL_FIELD_CHECKSUM, ( int )gameDetails.descriptors.Checksum() );

	gameDetails.slotName = saveName;
	ScrubSaveGameFileName( gameDetails.slotName );

	saveFileEntryList_t files;
	files.Append( &stringsFile );
	files.Append( &saveFile );

	session->SaveGameSync( gameDetails.slotName, files, gameDetails );

	if( !insideExecuteMapChange )
	{
		renderSystem->EndAutomaticBackgroundSwaps();
	}

	syncNextGameFrame = true;

	return true;
}

//puncho start - MP-A5 (K44)
/*
===============
CheckSaveLayers

The save's mod list and per-layer entityDef names against this session's (idDeclManager::GetLayerSaveHeader). Saves
store entityDef indices raw, so a new or removed entityDef in any layer, base content included, would restore them
wrong: the save is refused, naming the lowest layer that differs. Overrides and text edits define no new name and
pass. A save older than the header is refused by its version.
===============
*/
static bool CheckSaveLayers( const idDict& saved, idStr& refusal )
{
	//puncho - P0-m5: the oldest save this build loads, the first whose details hold the mod list and the layer rows
	const int OLDEST_LOADABLE_SAVE_VERSION = BUILD_NUMBER_SAVE_VERSION_MOD_LAYERS;
	const int saveVersion = saved.GetInt( SAVEGAME_DETAIL_FIELD_SAVE_VERSION, 0 );
	if( saveVersion < OLDEST_LOADABLE_SAVE_VERSION )
	{
		refusal.Format( "save version %d predates the mod list in the save header (version %d)", saveVersion, OLDEST_LOADABLE_SAVE_VERSION );
		return false;
	}

	idDict current;
	declManager->GetLayerSaveHeader( current );
	const char* savedMods = saved.GetString( "modList" );
	const char* currentMods = current.GetString( "modList" );
	if( idStr::Icmp( savedMods, currentMods ) != 0 )
	{
		idStrList savedList;
		idStrList currentList;
		idFileSystem::SplitModList( savedMods, savedList );
		fileSystem->GetModList( currentList );
		idStr missing;
		idStr extra;
		idFileSystem::DiffModLists( savedList, currentList, missing, extra );
		refusal.Format( "the save was made with mods '%s', these are '%s'", savedMods, currentMods );
		if( !missing.IsEmpty() )
		{
			refusal += va( "; missing %s", missing.c_str() );
		}
		if( !extra.IsEmpty() )
		{
			refusal += va( "; extra %s", extra.c_str() );
		}
		if( missing.IsEmpty() && extra.IsEmpty() )
		{
			refusal += "; reordered";
		}
		return false;
	}

	// every key either dict holds; the lowest layer that differs is named. Only the entityDef rows count (P1-M1): the
	// other types' rows that 1411-1413 saves carry are skipped, since no other type's index reaches a save
	const char* prefix = "layerDecls_";
	const char* entityDefName = declManager->GetDeclNameFromType( DECL_ENTITYDEF );
	int worstLayer = MAX_MOD_LAYERS + 1;
	idStr worstKey;
	for( int pass = 0; pass < 2; pass++ )
	{
		const idDict& from = ( pass == 0 ) ? saved : current;
		const idDict& other = ( pass == 0 ) ? current : saved;
		for( const idKeyValue* kv = from.MatchPrefix( prefix ); kv != NULL; kv = from.MatchPrefix( prefix, kv ) )
		{
			const char* row = kv->GetKey().c_str() + idStr::Length( prefix );
			const char* rowType = strchr( row, '_' );
			if( rowType == NULL || idStr::Icmp( rowType + 1, entityDefName ) != 0 )	//puncho - P1-M1
			{
				continue;
			}
			const int layer = atoi( row );
			if( layer < worstLayer && kv->GetValue().Cmp( other.GetString( kv->GetKey(), "0 0" ) ) != 0 )
			{
				worstLayer = layer;
				worstKey = kv->GetKey();
			}
		}
	}
	if( !worstKey.IsEmpty() )
	{
		const char* typeName = strchr( worstKey.c_str() + idStr::Length( prefix ), '_' );
		const char* layerName = ( worstLayer < fileSystem->GetNumLayers() ) ? fileSystem->GetLayerName( worstLayer ) : "?";
		refusal.Format( "layer %s (%s): the %s names it defines differ, save '%s' against now '%s' (count, digest); a new or removed decl shifts the indices saves store",
						layerName, ( worstLayer == 0 ) ? "base" : "mod", ( typeName != NULL ) ? typeName + 1 : worstKey.c_str(),
						saved.GetString( worstKey, "0 0" ), current.GetString( worstKey, "0 0" ) );
		return false;
	}
	return true;
}
//puncho end

/*
===============
idCommonLocal::LoadGame
===============
*/
bool idCommonLocal::LoadGame( const char* saveName )
{
	if( IsMultiplayer() )
	{
		common->Printf( "Can't load during net play.\n" );
		if( wipeForced )
		{
			ClearWipe();
		}
		return false;
	}

	// RB begin
#if defined(USE_DOOMCLASSIC)
	if( GetCurrentGame() != DOOM3_BFG )
	{
		return false;
	}
#endif
	// RB end

	if( session->GetSignInManager().GetMasterLocalUser() == NULL )
	{
		return false;
	}
	if( mapSpawnData.savegameFile != NULL )
	{
		return false;
	}

	bool found = false;
	const saveGameDetailsList_t& sgdl = session->GetSaveGameManager().GetEnumeratedSavegames();
	for( int i = 0; i < sgdl.Num(); i++ )
	{
		if( sgdl[i].slotName == saveName )
		{
			if( sgdl[i].GetLanguage() != sys_lang.GetString() )
			{
				idStaticList< idSWFScriptFunction*, 4 > callbacks;
				idStaticList< idStrId, 4 > optionText;
				optionText.Append( idStrId( "#str_swf_continue" ) );
				idStrStatic<256> langName = "#str_lang_" + sgdl[i].GetLanguage();
				idStrStatic<256> msg;
				msg.Format( idLocalization::GetString( "#str_dlg_wrong_language" ), idLocalization::GetString( langName ) );
				Dialog().AddDynamicDialog( GDM_SAVEGAME_WRONG_LANGUAGE, callbacks, optionText, true, msg, false, true );
				if( wipeForced )
				{
					ClearWipe();
				}
				return false;
			}
			//puncho start - MP-A5 (K44): the mods and the names each layer defines, before any map loads
			idStr refusal;
			if( !CheckSaveLayers( sgdl[i].descriptors, refusal ) )
			{
				common->Warning( "Save '%s' is refused: %s", saveName, refusal.c_str() );
				if( wipeForced )
				{
					ClearWipe();
				}
				return false;
			}
			//puncho end
			found = true;
			break;
		}
	}
	if( !found )
	{
		common->Printf( "Could not find save '%s'\n", saveName );
		if( wipeForced )
		{
			ClearWipe();
		}
		return false;
	}

	mapSpawnData.savegameFile = &saveFile;
	mapSpawnData.stringTableFile = &stringsFile;

	saveFileEntryList_t files;
	files.Append( mapSpawnData.stringTableFile );
	files.Append( mapSpawnData.savegameFile );

	idStr slotName = saveName;
	ScrubSaveGameFileName( slotName );
	saveFile.Clear( false );
	stringsFile.Clear( false );

	saveGameHandle_t loadGameHandle = session->LoadGameSync( slotName, files );
	if( loadGameHandle != 0 )
	{
		return true;
	}
	mapSpawnData.savegameFile = NULL;
	if( wipeForced )
	{
		ClearWipe();
	}
	return false;
}

/*
========================
HandleInsufficientStorage
========================
*/
void HandleInsufficientStorage( const idSaveLoadParms& parms )
{
	session->GetSaveGameManager().ShowRetySaveDialog( parms.directory, parms.requiredSpaceInBytes );
}

/*
========================
HandleCommonErrors
========================
*/
bool HandleCommonErrors( const idSaveLoadParms& parms )
{
	common->Dialog().ShowSaveIndicator( false );

	if( parms.GetError() == SAVEGAME_E_NONE )
	{
		return true;
	}

	if( parms.GetError() & SAVEGAME_E_CORRUPTED )
	{
		// This one might need to be handled by the game
		common->Dialog().AddDialog( GDM_CORRUPT_CONTINUE, DIALOG_CONTINUE, NULL, NULL, false );

		// Find the game in the enumerated details, mark as corrupt so the menus can show as corrupt
		saveGameDetailsList_t& list = session->GetSaveGameManager().GetEnumeratedSavegamesNonConst();
		for( int i = 0; i < list.Num(); i++ )
		{
			if( idStr::Icmp( list[i].slotName, parms.description.slotName ) == 0 )
			{
				list[i].damaged = true;
			}
		}
		return true;
	}
	else if( parms.GetError() & SAVEGAME_E_INSUFFICIENT_ROOM )
	{
		HandleInsufficientStorage( parms );
		return true;
	}
	else if( parms.GetError() & SAVEGAME_E_UNABLE_TO_SELECT_STORAGE_DEVICE && saveGame_enable.GetBool() )
	{
		common->Dialog().AddDialog( GDM_UNABLE_TO_USE_SELECTED_STORAGE_DEVICE, DIALOG_CONTINUE, NULL, NULL, false );
		return true;
	}
	else if( parms.GetError() & SAVEGAME_E_INVALID_FILENAME )
	{
		idLib::Warning( "Invalid savegame filename [%s]!", parms.directory.c_str() );
		return true;
	}
	else if( parms.GetError() & SAVEGAME_E_DLC_NOT_FOUND )
	{
		common->Dialog().AddDialog( GDM_DLC_ERROR_MISSING_GENERIC, DIALOG_CONTINUE, NULL, NULL, false );
		return true;
	}
	else if( parms.GetError() & SAVEGAME_E_DISC_SWAP )
	{
		common->Dialog().AddDialog( GDM_DISC_SWAP, DIALOG_CONTINUE, NULL, NULL, false );
		return true;
	}
	else if( parms.GetError() & SAVEGAME_E_INCOMPATIBLE_NEWER_VERSION )
	{
		common->Dialog().AddDialog( GDM_INCOMPATIBLE_NEWER_SAVE, DIALOG_CONTINUE, NULL, NULL, false );
		return true;
	}

	return false;
}

/*
========================
idCommonLocal::OnSaveCompleted
========================
*/
void idCommonLocal::OnSaveCompleted( idSaveLoadParms& parms )
{
	assert( pipelineFile != NULL );
	delete pipelineFile;
	pipelineFile = NULL;

	if( parms.GetError() == SAVEGAME_E_NONE )
	{
		game->Shell_UpdateSavedGames();
	}

	if( !HandleCommonErrors( parms ) )
	{
		common->Dialog().AddDialog( GDM_ERROR_SAVING_SAVEGAME, DIALOG_CONTINUE, NULL, NULL, false );
	}
}

/*
========================
idCommonLocal::OnLoadCompleted
========================
*/
void idCommonLocal::OnLoadCompleted( idSaveLoadParms& parms )
{
	if( !HandleCommonErrors( parms ) )
	{
		common->Dialog().AddDialog( GDM_ERROR_LOADING_SAVEGAME, DIALOG_CONTINUE, NULL, NULL, false );
	}
}

/*
========================
idCommonLocal::OnLoadFilesCompleted
========================
*/
void idCommonLocal::OnLoadFilesCompleted( idSaveLoadParms& parms )
{
	if( ( mapSpawnData.savegameFile != NULL ) && ( parms.GetError() == SAVEGAME_E_NONE ) )
	{
		// just need to make the file readable
		( ( idFile_Memory* )mapSpawnData.savegameFile )->MakeReadOnly();
		( ( idFile_Memory* )mapSpawnData.stringTableFile )->MakeReadOnly();

		idStr gamename;
		idStr mapname;

		mapSpawnData.savegameVersion = parms.description.GetSaveVersion();
		mapSpawnData.savegameFile->ReadString( gamename );
		mapSpawnData.savegameFile->ReadString( mapname );

		if( ( gamename != GAME_NAME ) || ( mapname.IsEmpty() ) || ( parms.description.GetSaveVersion() > BUILD_NUMBER ) )
		{
			// if this isn't a savegame for the correct game, abort loadgame
			common->Warning( "Attempted to load an invalid savegame" );
		}
		else
		{
			common->DPrintf( "loading savegame\n" );

			mapSpawnData.savegameFile->ReadBool( consoleUsed );
			consoleUsed = consoleUsed || com_allowConsole.GetBool();

			idMatchParameters matchParameters;
			matchParameters.numSlots = 1;
			matchParameters.gameMode = GAME_MODE_SINGLEPLAYER;
			matchParameters.gameMap = GAME_MAP_SINGLEPLAYER;
			matchParameters.mapName = mapname;
			matchParameters.serverInfo.ReadFromFileHandle( mapSpawnData.savegameFile );

			session->QuitMatchToTitle();
			if( WaitForSessionState( idSession::IDLE ) )
			{
				session->CreatePartyLobby( matchParameters );
				if( WaitForSessionState( idSession::PARTY_LOBBY ) )
				{
					session->CreateMatch( matchParameters );
					if( WaitForSessionState( idSession::GAME_LOBBY ) )
					{
						session->StartMatch();
						return;
					}
				}
			}
		}
	}
	// If we got here then we didn't actually load the save game for some reason
	mapSpawnData.savegameFile = NULL;
}

/*
========================
idCommonLocal::TriggerScreenWipe
========================
*/
void idCommonLocal::TriggerScreenWipe( const char* _wipeMaterial, bool hold )
{
	StartWipe( _wipeMaterial, hold );
	CompleteWipe();
	wipeForced = true;
	renderSystem->BeginAutomaticBackgroundSwaps( AUTORENDER_DEFAULTICON );
}

/*
========================
idCommonLocal::OnEnumerationCompleted
========================
*/
void idCommonLocal::OnEnumerationCompleted( idSaveLoadParms& parms )
{
	if( parms.GetError() == SAVEGAME_E_NONE )
	{
		game->Shell_UpdateSavedGames();
	}
}

/*
========================
idCommonLocal::OnDeleteCompleted
========================
*/
void idCommonLocal::OnDeleteCompleted( idSaveLoadParms& parms )
{
	if( parms.GetError() == SAVEGAME_E_NONE )
	{
		game->Shell_UpdateSavedGames();
	}
}

/*
===============
LoadGame_f
===============
*/
CONSOLE_COMMAND_SHIP( loadGame, "loads a game", idCmdSystem::ArgCompletion_SaveGame )
{
	console->Close();
	commonLocal.LoadGame( ( args.Argc() > 1 ) ? args.Argv( 1 ) : "quick" );
}

/*
===============
SaveGame_f
===============
*/
CONSOLE_COMMAND_SHIP( saveGame, "saves a game", NULL )
{
	const char* savename = ( args.Argc() > 1 ) ? args.Argv( 1 ) : "quick";
	if( commonLocal.SaveGame( savename ) )
	{
		common->Printf( "Saved: %s\n", savename );
	}
}

/*
==================
Common_Map_f

Restart the server on a different map
==================
*/
CONSOLE_COMMAND_SHIP( map, "loads a map", idCmdSystem::ArgCompletion_MapName )
{
	commonLocal.StartNewGame( args.Argv( 1 ), false, GAME_MODE_SINGLEPLAYER );
}

/*
==================
Common_RestartMap_f
==================
*/
CONSOLE_COMMAND_SHIP( restartMap, "restarts the current map", NULL )
{
	if( g_demoMode.GetBool() )
	{
		cmdSystem->AppendCommandText( va( "devmap %s %d\n", commonLocal.GetCurrentMapName(), 0 ) );
	}
}

/*
==================
Common_DevMap_f

Restart the server on a different map in developer mode
==================
*/
CONSOLE_COMMAND_SHIP( devmap, "loads a map in developer mode", idCmdSystem::ArgCompletion_MapName )
{
	commonLocal.StartNewGame( args.Argv( 1 ), true, GAME_MODE_SINGLEPLAYER );
}

/*
==================
Common_NetMap_f

Restart the server on a different map in multiplayer mode
==================
*/
CONSOLE_COMMAND_SHIP( netmap, "loads a map in multiplayer mode", idCmdSystem::ArgCompletion_MapName )
{
	int gameMode = 0; // Default to deathmatch
	if( args.Argc() > 2 )
	{
		gameMode = atoi( args.Argv( 2 ) );
	}
	commonLocal.StartNewGame( args.Argv( 1 ), true, gameMode );
}

/*
==================
Common_TestMap_f
==================
*/
CONSOLE_COMMAND( testmap, "tests a map", idCmdSystem::ArgCompletion_MapName )
{
	idStr map, string;
	bool noAAS = false;
	bool keepView = false;
	for( int i = 2; i < args.Argc(); i++ )
	{
		if( idStr::Icmp( args.Argv( i ), "noAAS" ) == 0 )
		{
			noAAS = true;
		}
		else if( idStr::Icmp( args.Argv( i ), "keepView" ) == 0 )
		{
			keepView = true;
		}
		else
		{
			common->Printf( "usage: testmap <map> [noAAS] [keepView]\n" );
			return;
		}
	}

	map = args.Argv( 1 );
	if( !map.Length() )
	{
		return;
	}
	map.StripFileExtension();

	if( common->IsMultiplayer() )
	{
		common->Warning( "testmap is single-player only" );
		return;
	}
	cvarSystem->SetCVarString( "g_testSpawnView", "" );
	if( keepView )
	{
		cmdSystem->BufferCommandText( CMD_EXEC_NOW, "saveTestView" );
	}

	cmdSystem->BufferCommandText( CMD_EXEC_NOW, "disconnect" );

	sprintf( string, "dmap %smaps/%s.map", noAAS ? "noAAS " : "", map.c_str() );
	cmdSystem->BufferCommandText( CMD_EXEC_NOW, string );

	sprintf( string, "devmap %s", map.c_str() );
	cmdSystem->BufferCommandText( CMD_EXEC_NOW, string );
}


/*
==================
Common_TestMap_f

// TODO finish this
==================
*/
#if 0
CONSOLE_COMMAND( bakemap, "loads a map and bakes environment probes", idCmdSystem::ArgCompletion_MapName )
{
	idStr map, string;

	map = args.Argv( 1 );
	if( !map.Length() )
	{
		return;
	}
	map.StripFileExtension();

	cmdSystem->BufferCommandText( CMD_EXEC_NOW, "disconnect" );

	sprintf( string, "devmap %s.map", map.c_str() );
	cmdSystem->BufferCommandText( CMD_EXEC_NOW, string );

	sprintf( string, "bakeEnvironmentProbes" );
	cmdSystem->BufferCommandText( CMD_EXEC_NOW, string );
}
#endif
