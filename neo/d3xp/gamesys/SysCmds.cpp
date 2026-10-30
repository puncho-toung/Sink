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


#include "../Game_local.h"
#include "../Corpse.h"

#include "../tools/imgui/afeditor/AfEditor.h"
#include "StreamTrace.h"	//puncho - stream-trace (P1-A1c)
#include "WorldStoreLocal.h"	//puncho - P2b.1.2: recordTest fuzz reads records as the store does

/*
==================
Cmd_GetFloatArg
==================
*/
float Cmd_GetFloatArg( const idCmdArgs& args, int& argNum )
{
	const char* value;

	value = args.Argv( argNum++ );
	return atof( value );
}

/*
===================
Cmd_EntityList_f
===================
*/
void Cmd_EntityList_f( const idCmdArgs& args )
{
	int			e;
	idEntity*	check;
	int			count;
	size_t		size;
	idStr		match;

	if( args.Argc() > 1 )
	{
		match = args.Args();
		match.Replace( " ", "" );
	}
	else
	{
		match = "";
	}

	count = 0;
	size = 0;

	gameLocal.Printf( "%-4s  %-20s %-20s %s\n", " Num", "EntityDef", "Class", "Name" );
	gameLocal.Printf( "--------------------------------------------------------------------\n" );
	for( e = 0; e < MAX_GENTITIES; e++ )
	{
		check = gameLocal.entities[ e ];

		if( !check )
		{
			continue;
		}

		if( !check->name.Filter( match, true ) )
		{
			continue;
		}

		gameLocal.Printf( "%4i: %-20s %-20s %s\n", e,
						  check->GetEntityDefName(), check->GetClassname(), check->name.c_str() );

		count++;
		size += check->spawnArgs.Allocated();
	}

	gameLocal.Printf( "...%d entities\n...%d bytes of spawnargs\n", count, size );
}

//puncho start - identity (P1-A2)
/*
===================
Cmd_ListSpawnIds_f

listSpawnIds [first] [last]: the spawn-id generation of every live entity in [first, last), by default
the map's range [MAX_CLIENTS, mapSpawnCount), then a digest of the (index, generation) pairs, so two
peers compare in one line (P1-A2's loopback gate).
===================
*/
static void Cmd_ListSpawnIds_f( const idCmdArgs& args )
{
	const int mapSpawnCount = gameLocal.GetMapSpawnCount();
	const int first = ( args.Argc() > 1 ) ? idMath::ClampInt( 0, MAX_GENTITIES, atoi( args.Argv( 1 ) ) ) : MAX_CLIENTS;
	const int last = ( args.Argc() > 2 ) ? idMath::ClampInt( first, MAX_GENTITIES, atoi( args.Argv( 2 ) ) ) : Max( first, mapSpawnCount );

	unsigned int digest;
	CRC32_InitChecksum( digest );
	int count = 0;
	for( int i = first; i < last; i++ )
	{
		const idEntity* ent = gameLocal.entities[ i ];
		if( ent == NULL )
		{
			continue;
		}
		const int gen = gameLocal.spawnIds[ i ];
		gameLocal.Printf( "%5d %6d %-24s %s\n", i, gen, ent->GetClassname(), ent->name.c_str() );
		CRC32_UpdateChecksum( digest, &i, sizeof( i ) );
		CRC32_UpdateChecksum( digest, &gen, sizeof( gen ) );
		count++;
	}
	CRC32_FinishChecksum( digest );
	gameLocal.Printf( "listSpawnIds [%d, %d): %d entities, mapSpawnCount %d, digest %08x\n", first, last, count, mapSpawnCount, digest );
}
//puncho end

/*
===================
Cmd_ActiveEntityList_f
===================
*/
void Cmd_ActiveEntityList_f( const idCmdArgs& args )
{
	idEntity*	check;
	int			count;

	count = 0;

	gameLocal.Printf( "%-4s  %-20s %-20s %s\n", " Num", "EntityDef", "Class", "Name" );
	gameLocal.Printf( "--------------------------------------------------------------------\n" );
	for( check = gameLocal.activeEntities.Next(); check != NULL; check = check->activeNode.Next() )
	{
		char	dormant = check->fl.isDormant ? '-' : ' ';
		gameLocal.Printf( "%4i:%c%-20s %-20s %s\n", check->entityNumber, dormant, check->GetEntityDefName(), check->GetClassname(), check->name.c_str() );
		count++;
	}

	gameLocal.Printf( "...%d active entities\n", count );
}

/*
===================
Cmd_ListSpawnArgs_f
===================
*/
void Cmd_ListSpawnArgs_f( const idCmdArgs& args )
{
	int i;
	idEntity* ent;

	ent = gameLocal.FindEntity( args.Argv( 1 ) );
	if( !ent )
	{
		gameLocal.Printf( "entity not found\n" );
		return;
	}

	for( i = 0; i < ent->spawnArgs.GetNumKeyVals(); i++ )
	{
		const idKeyValue* kv = ent->spawnArgs.GetKeyVal( i );
		gameLocal.Printf( "\"%s\"  " S_COLOR_WHITE "\"%s\"\n", kv->GetKey().c_str(), kv->GetValue().c_str() );
	}
}

/*
===================
Cmd_ReloadScript_f
===================
*/
void Cmd_ReloadScript_f( const idCmdArgs& args )
{
	// shutdown the map because entities may point to script objects
	gameLocal.MapShutdown();

	// recompile the scripts
	gameLocal.program.Startup( SCRIPT_DEFAULT );

	if( fileSystem->ReadFile( "doom_main.script", NULL ) > 0 )
	{
		gameLocal.program.CompileFile( "doom_main.script" );
		gameLocal.program.FinishCompilation();
	}

	// error out so that the user can rerun the scripts
	gameLocal.Error( "Exiting map to reload scripts" );
}

CONSOLE_COMMAND( reloadScript2, "Doesn't thow an error...  Use this when switching game modes", 0 )
{
	// shutdown the map because entities may point to script objects
	gameLocal.MapShutdown();

	// recompile the scripts
	gameLocal.program.Startup( SCRIPT_DEFAULT );

	if( fileSystem->ReadFile( "doom_main.script", NULL ) > 0 )
	{
		gameLocal.program.CompileFile( "doom_main.script" );
		gameLocal.program.FinishCompilation();
	}
}

/*
===================
Cmd_Script_f
===================
*/
void Cmd_Script_f( const idCmdArgs& args )
{
	const char* 	script;
	idStr			text;
	idStr			funcname;
	static int		funccount = 0;
	idThread* 		thread;
	const function_t* func;
	idEntity*		ent;

	if( !gameLocal.CheatsOk() )
	{
		return;
	}

	sprintf( funcname, "ConsoleFunction_%d", funccount++ );

	script = args.Args();
	sprintf( text, "void %s() {%s;}\n", funcname.c_str(), script );
	if( gameLocal.program.CompileText( "console", text, true ) )
	{
		func = gameLocal.program.FindFunction( funcname );
		if( func )
		{
			// set all the entity names in case the user named one in the script that wasn't referenced in the default script
			for( ent = gameLocal.spawnedEntities.Next(); ent != NULL; ent = ent->spawnNode.Next() )
			{
				gameLocal.program.SetEntity( ent->name, ent );
			}

			thread = new idThread( func );
			thread->Start();
		}
	}
}

/*
==================
KillEntities

Kills all the entities of the given class in a level.
==================
*/
void KillEntities( const idCmdArgs& args, const idTypeInfo& superClass )
{
	idEntity*	ent;
	idStrList	ignore;
	const char* name;
	int			i;

	if( !gameLocal.GetLocalPlayer() || !gameLocal.CheatsOk( false ) )
	{
		return;
	}

	for( i = 1; i < args.Argc(); i++ )
	{
		name = args.Argv( i );
		ignore.Append( name );
	}

	for( ent = gameLocal.spawnedEntities.Next(); ent != NULL; ent = ent->spawnNode.Next() )
	{
		if( ent->IsType( superClass ) )
		{
			for( i = 0; i < ignore.Num(); i++ )
			{
				if( ignore[ i ] == ent->name )
				{
					break;
				}
			}

			if( i >= ignore.Num() )
			{
				ent->PostEventMS( &EV_Remove, 0 );
			}
		}
	}
}

/*
==================
Cmd_KillMonsters_f

Kills all the monsters in a level.
==================
*/
void Cmd_KillMonsters_f( const idCmdArgs& args )
{
	KillEntities( args, idAI::Type );

	// kill any projectiles as well since they have pointers to the monster that created them
	KillEntities( args, idProjectile::Type );
}

/*
==================
Cmd_KillMovables_f

Kills all the moveables in a level.
==================
*/
void Cmd_KillMovables_f( const idCmdArgs& args )
{
	if( !gameLocal.GetLocalPlayer() || !gameLocal.CheatsOk( false ) )
	{
		return;
	}
	KillEntities( args, idMoveable::Type );
}

/*
==================
Cmd_KillRagdolls_f

Kills all the ragdolls in a level.
==================
*/
void Cmd_KillRagdolls_f( const idCmdArgs& args )
{
	if( !gameLocal.GetLocalPlayer() || !gameLocal.CheatsOk( false ) )
	{
		return;
	}
	KillEntities( args, idAFEntity_Generic::Type );
	KillEntities( args, idAFEntity_WithAttachedHead::Type );
}

/*
==================
Cmd_Give_f

Give items to a client
==================
*/
void Cmd_Give_f( const idCmdArgs& args )
{
	const char* name;
	int			i;
	bool		give_all;
	idPlayer*	player;

	player = gameLocal.GetLocalPlayer();
	if( !player || !gameLocal.CheatsOk() )
	{
		return;
	}

	name = args.Argv( 1 );

	if( idStr::Icmp( name, "all" ) == 0 )
	{
		give_all = true;
	}
	else
	{
		give_all = false;
	}

	if( give_all || ( idStr::Cmpn( name, "weapon", 6 ) == 0 ) )
	{
		if( gameLocal.world->spawnArgs.GetBool( "no_Weapons" ) )
		{
			gameLocal.world->spawnArgs.SetBool( "no_Weapons", false );
			for( i = 0; i < gameLocal.numClients; i++ )
			{
				if( gameLocal.entities[ i ] )
				{
					gameLocal.entities[ i ]->PostEventSec( &EV_Player_SelectWeapon, 0.5f, gameLocal.entities[ i ]->spawnArgs.GetString( "def_weapon1" ) );
				}
			}
		}
	}

	if( ( idStr::Cmpn( name, "weapon_", 7 ) == 0 ) || ( idStr::Cmpn( name, "item_", 5 ) == 0 ) || ( idStr::Cmpn( name, "ammo_", 5 ) == 0 ) )
	{
		player->GiveItem( name );
		return;
	}

	if( give_all || idStr::Icmp( name, "health" ) == 0 )
	{
		player->health = player->inventory.maxHealth;
		if( !give_all )
		{
			return;
		}
	}

	if( give_all || idStr::Icmp( name, "weapons" ) == 0 )
	{
		player->inventory.weapons = ( int )( BIT( MAX_WEAPONS ) - 1 );
		player->CacheWeapons();

		if( !give_all )
		{
			return;
		}
	}

	if( give_all || idStr::Icmp( name, "ammo" ) == 0 )
	{
		for( i = 0 ; i < AMMO_NUMTYPES; i++ )
		{
			player->inventory.SetInventoryAmmoForType( i, player->inventory.MaxAmmoForAmmoClass( player, idWeapon::GetAmmoNameForNum( ( ammo_t )i ) ) );
		}
		if( !give_all )
		{
			return;
		}
	}

	if( give_all || idStr::Icmp( name, "armor" ) == 0 )
	{
		player->inventory.armor = player->inventory.maxarmor;
		if( !give_all )
		{
			return;
		}
	}

	if( idStr::Icmp( name, "berserk" ) == 0 )
	{
		player->GivePowerUp( BERSERK, SEC2MS( 30.0f ), ITEM_GIVE_FEEDBACK | ITEM_GIVE_UPDATE_STATE );
		return;
	}

	if( idStr::Icmp( name, "invis" ) == 0 )
	{
		player->GivePowerUp( INVISIBILITY, SEC2MS( 30.0f ), ITEM_GIVE_FEEDBACK | ITEM_GIVE_UPDATE_STATE );
		return;
	}

	if( idStr::Icmp( name, "invulnerability" ) == 0 )
	{
		if( args.Argc() > 2 )
		{
			player->GivePowerUp( INVULNERABILITY, atoi( args.Argv( 2 ) ), ITEM_GIVE_FEEDBACK | ITEM_GIVE_UPDATE_STATE );
		}
		else
		{
			player->GivePowerUp( INVULNERABILITY, 30000, ITEM_GIVE_FEEDBACK | ITEM_GIVE_UPDATE_STATE );
		}
		return;
	}

	if( idStr::Icmp( name, "helltime" ) == 0 )
	{
		if( args.Argc() > 2 )
		{
			player->GivePowerUp( HELLTIME, atoi( args.Argv( 2 ) ), ITEM_GIVE_FEEDBACK | ITEM_GIVE_UPDATE_STATE );
		}
		else
		{
			player->GivePowerUp( HELLTIME, 30000, ITEM_GIVE_FEEDBACK | ITEM_GIVE_UPDATE_STATE );
		}
		return;
	}

	if( idStr::Icmp( name, "envirosuit" ) == 0 )
	{
		if( args.Argc() > 2 )
		{
			player->GivePowerUp( ENVIROSUIT, atoi( args.Argv( 2 ) ), ITEM_GIVE_FEEDBACK | ITEM_GIVE_UPDATE_STATE );
		}
		else
		{
			player->GivePowerUp( ENVIROSUIT, 30000, ITEM_GIVE_FEEDBACK | ITEM_GIVE_UPDATE_STATE );
		}
		return;
	}
	if( idStr::Icmp( name, "pda" ) == 0 )
	{
		if( args.Argc() == 2 )
		{
			player->GivePDA( NULL, NULL );
		}
		else if( idStr::Icmp( args.Argv( 2 ), "all" ) == 0 )
		{
			// Give the personal PDA first
			player->GivePDA( NULL, NULL );
			for( int i = 0; i < declManager->GetNumDecls( DECL_PDA ); i++ )
			{
				player->GivePDA( static_cast<const idDeclPDA*>( declManager->DeclByIndex( DECL_PDA, i ) ), NULL );
			}
		}
		else
		{
			const idDeclPDA* pda = static_cast<const idDeclPDA*>( declManager->FindType( DECL_PDA, args.Argv( 2 ), false ) );
			if( pda == NULL )
			{
				gameLocal.Printf( "Unknown PDA %s\n", args.Argv( 2 ) );
			}
			else
			{
				player->GivePDA( pda, NULL );
			}
		}
		return;
	}

	if( idStr::Icmp( name, "video" ) == 0 )
	{
		const idDeclVideo* video = static_cast<const idDeclVideo*>( declManager->FindType( DECL_VIDEO, args.Argv( 2 ), false ) );
		if( video == NULL )
		{
			gameLocal.Printf( "Unknown video %s\n", args.Argv( 2 ) );
		}
		else
		{
			player->GiveVideo( video, NULL );
		}
		return;
	}

	if( !give_all && !player->Give( args.Argv( 1 ), args.Argv( 2 ), ITEM_GIVE_FEEDBACK | ITEM_GIVE_UPDATE_STATE ) )
	{
		gameLocal.Printf( "unknown item\n" );
	}
}

/*
==================
Cmd_CenterView_f

Centers the players pitch
==================
*/
void Cmd_CenterView_f( const idCmdArgs& args )
{
	idPlayer*	player;
	idAngles	ang;

	player = gameLocal.GetLocalPlayer();
	if( !player )
	{
		return;
	}

	ang = player->viewAngles;
	ang.pitch = 0.0f;
	player->SetViewAngles( ang );
}

/*
==================
Cmd_God_f

Sets client to godmode

argv(0) god
==================
*/
void Cmd_God_f( const idCmdArgs& args )
{
	const char*		msg;
	idPlayer*	player;

	player = gameLocal.GetLocalPlayer();
	if( !player || !gameLocal.CheatsOk() )
	{
		return;
	}

	if( player->godmode )
	{
		player->godmode = false;
		msg = "godmode OFF\n";
	}
	else
	{
		player->godmode = true;
		msg = "godmode ON\n";
	}

	gameLocal.Printf( "%s", msg );
}

/*
==================
Cmd_Notarget_f

Sets client to notarget

argv(0) notarget
==================
*/
void Cmd_Notarget_f( const idCmdArgs& args )
{
	const char*		msg;
	idPlayer*	player;

	player = gameLocal.GetLocalPlayer();
	if( !player || !gameLocal.CheatsOk() )
	{
		return;
	}

	if( player->fl.notarget )
	{
		player->fl.notarget = false;
		msg = "notarget OFF\n";
	}
	else
	{
		player->fl.notarget = true;
		msg = "notarget ON\n";
	}

	gameLocal.Printf( "%s", msg );
}

/*
==================
Cmd_Noclip_f

argv(0) noclip
==================
*/
void Cmd_Noclip_f( const idCmdArgs& args )
{
	const char*		msg;
	idPlayer*	player;

	player = gameLocal.GetLocalPlayer();
	if( !player || !gameLocal.CheatsOk() )
	{
		return;
	}

	if( player->noclip )
	{
		msg = "noclip OFF\n";
	}
	else
	{
		msg = "noclip ON\n";
	}
	player->noclip = !player->noclip;

	gameLocal.Printf( "%s", msg );
}

/*
=================
Cmd_PlayerModel_f
=================
*/
void Cmd_PlayerModel_f( const idCmdArgs& args )
{
	idPlayer*	player;
	const char* name;
	idVec3		pos;
	idAngles	ang;

	player = gameLocal.GetLocalPlayer();
	if( !player || !gameLocal.CheatsOk() )
	{
		return;
	}

	if( args.Argc() < 2 )
	{
		gameLocal.Printf( "usage: playerModel <modelname>\n" );
		return;
	}

	name = args.Argv( 1 );
	player->spawnArgs.Set( "model", name );

	pos = player->GetPhysics()->GetOrigin();
	ang = player->viewAngles;
	player->SpawnToPoint( pos, ang );
}

/*
==================
Cmd_Say
==================
*/
static void Cmd_Say( bool team, const idCmdArgs& args )
{
	const char* cmd = team ? "sayTeam" : "say" ;

	if( !common->IsMultiplayer() )
	{
		gameLocal.Printf( "%s can only be used in a multiplayer game\n", cmd );
		return;
	}

	if( args.Argc() < 2 )
	{
		gameLocal.Printf( "usage: %s <text>\n", cmd );
		return;
	}

	idStr text = args.Args();
	if( text.Length() == 0 )
	{
		return;
	}

	if( text[ text.Length() - 1 ] == '\n' )
	{
		text[ text.Length() - 1 ] = '\0';
	}
	const char* name = "player";

	// "server" will only appear on a dedicated server
	if( common->IsServer() && gameLocal.GetLocalClientNum() == -1 )
	{
		name = "server";
	}
	else
	{
		name = session->GetActingGameStateLobbyBase().GetLobbyUserName( gameLocal.lobbyUserIDs[ gameLocal.GetLocalClientNum() ] );

		// Append the player's location to team chat messages in CTF
		idPlayer* player = static_cast<idPlayer*>( gameLocal.entities[ gameLocal.GetLocalClientNum() ] );
		if( gameLocal.mpGame.IsGametypeFlagBased() && team && player )
		{
			idLocationEntity* locationEntity = gameLocal.LocationForPoint( player->GetEyePosition() );

			if( locationEntity )
			{
				idStr temp = "[";
				temp += locationEntity->GetLocation();
				temp += "] ";
				temp += text;
				text = temp;
			}

		}
	}

	if( common->IsClient() )
	{
		idBitMsg	outMsg;
		byte		msgBuf[ 256 ];
		outMsg.InitWrite( msgBuf, sizeof( msgBuf ) );
		outMsg.WriteString( name );
		outMsg.WriteString( text, -1, false );
		session->GetActingGameStateLobbyBase().SendReliableToHost( team ? GAME_RELIABLE_MESSAGE_TCHAT : GAME_RELIABLE_MESSAGE_CHAT, outMsg );
	}
	else
	{
		gameLocal.mpGame.ProcessChatMessage( gameLocal.GetLocalClientNum(), team, name, text, NULL );
	}
}

/*
==================
Cmd_Say_f
==================
*/
static void Cmd_Say_f( const idCmdArgs& args )
{
	Cmd_Say( false, args );
}

/*
==================
Cmd_SayTeam_f
==================
*/
static void Cmd_SayTeam_f( const idCmdArgs& args )
{
	Cmd_Say( true, args );
}

/*
==================
Cmd_AddChatLine_f
==================
*/
static void Cmd_AddChatLine_f( const idCmdArgs& args )
{
	gameLocal.mpGame.AddChatLine( args.Argv( 1 ) );
}

/*
==================
Cmd_GetViewpos_f
==================
*/
void Cmd_GetViewpos_f( const idCmdArgs& args )
{
	idPlayer*	player;
	idVec3		origin;
	idMat3		axis;

	player = gameLocal.GetLocalPlayer();
	if( !player )
	{
		return;
	}

	const renderView_t* view = player->GetRenderView();
	if( view )
	{
		gameLocal.Printf( "(%s) %.1f\n", view->vieworg.ToString(), view->viewaxis[0].ToYaw() );
	}
	else
	{
		player->GetViewPos( origin, axis );
		gameLocal.Printf( "(%s) %.1f\n", origin.ToString(), axis[0].ToYaw() );
	}
}

/*
=================
Cmd_SetViewpos_f
=================
*/
static void Cmd_ListBudgets_f( const idCmdArgs& args )
{
	gameLocal.ListBudgets( args.Argc() > 1 && idStr::Icmp( args.Argv( 1 ), "order" ) == 0 );	//puncho - P2b.1.3
}

static void Cmd_ListMapStatics_f( const idCmdArgs& args )
{
	gameLocal.ListMapStatics();
}

static void Cmd_SaveTestView_f( const idCmdArgs& args )
{
	cvarSystem->SetCVarString( "g_testSpawnView", "" );
	idPlayer* player = gameLocal.GetLocalPlayer();
	if( common->IsMultiplayer() || player == NULL )
	{
		gameLocal.Printf( "saveTestView: no single-player view; testmap uses its start\n" );
		return;
	}
	if( !cvarSystem->GetCVarBool( "developer" ) && !gameLocal.CheatsOk() )
	{
		return;
	}
	const idVec3& origin = player->GetPhysics()->GetOrigin();
	idStr view;
	view.Format( "%.9g %.9g %.9g %.9g %.9g", origin.x, origin.y, origin.z, player->viewAngles.yaw, player->viewAngles.pitch );
	cvarSystem->SetCVarString( "g_testSpawnView", view );
	gameLocal.Printf( "testmap saved view %s\n", view.c_str() );
}

void Cmd_SetViewpos_f( const idCmdArgs& args )
{
	idVec3		origin;
	idAngles	angles;
	int			i;
	idPlayer*	player;

	player = gameLocal.GetLocalPlayer();
	if( !player || !gameLocal.CheatsOk() )
	{
		return;
	}

	if( ( args.Argc() != 4 ) && ( args.Argc() != 5 ) )
	{
		gameLocal.Printf( "usage: setviewpos <x> <y> <z> <yaw>\n" );
		return;
	}

	angles.Zero();
	if( args.Argc() == 5 )
	{
		angles.yaw = atof( args.Argv( 4 ) );
	}

	for( i = 0 ; i < 3 ; i++ )
	{
		origin[i] = atof( args.Argv( i + 1 ) );
	}
	origin.z -= pm_normalviewheight.GetFloat() - 0.25f;

	player->Teleport( origin, angles, NULL );
}

/*
=================
Cmd_Teleport_f
=================
*/
void Cmd_Teleport_f( const idCmdArgs& args )
{
	idVec3		origin;
	idAngles	angles;
	idPlayer*	player;
	idEntity*	ent;

	player = gameLocal.GetLocalPlayer();
	if( !player || !gameLocal.CheatsOk() )
	{
		return;
	}

	if( args.Argc() != 2 )
	{
		gameLocal.Printf( "usage: teleport <name of entity to teleport to>\n" );
		return;
	}

	ent = gameLocal.FindEntity( args.Argv( 1 ) );
	if( !ent )
	{
		gameLocal.Printf( "entity not found\n" );
		return;
	}

	angles.Zero();
	angles.yaw = ent->GetPhysics()->GetAxis()[ 0 ].ToYaw();
	origin = ent->GetPhysics()->GetOrigin();

	player->Teleport( origin, angles, ent );
}

/*
=================
Cmd_Trigger_f
=================
*/
void Cmd_Trigger_f( const idCmdArgs& args )
{
	idVec3		origin;
	idAngles	angles;
	idPlayer*	player;
	idEntity*	ent;

	player = gameLocal.GetLocalPlayer();
	if( !player || !gameLocal.CheatsOk() )
	{
		return;
	}

	if( args.Argc() != 2 )
	{
		gameLocal.Printf( "usage: trigger <name of entity to trigger>\n" );
		return;
	}

	ent = gameLocal.FindEntity( args.Argv( 1 ) );
	if( !ent )
	{
		gameLocal.Printf( "entity not found\n" );
		return;
	}

	ent->Signal( SIG_TRIGGER );
	ent->ProcessEvent( &EV_Activate, player );
	ent->TriggerGuis();
}

/*
===================
Cmd_Spawn_f
===================
*/
void Cmd_Spawn_f( const idCmdArgs& args )
{
	const char* key, *value;
	int			i;
	float		yaw;
	idVec3		org;
	idPlayer*	player;
	idDict		dict;

	player = gameLocal.GetLocalPlayer();
	if( !player || !gameLocal.CheatsOk( false ) )
	{
		return;
	}

	if( args.Argc() & 1 )  	// must always have an even number of arguments
	{
		gameLocal.Printf( "usage: spawn classname [key/value pairs]\n" );
		return;
	}

	yaw = player->viewAngles.yaw;

	value = args.Argv( 1 );
	dict.Set( "classname", value );
	dict.Set( "angle", va( "%f", yaw + 180 ) );

	org = player->GetPhysics()->GetOrigin() + idAngles( 0, yaw, 0 ).ToForward() * 80 + idVec3( 0, 0, 1 );

	// RB: put env_probes a bit higher
	if( idStr::Icmp( value, "env_probe" ) == 0 )
	{
		//org.z = 64;
		org.SnapInt();

		idStr name = gameEdit->GetUniqueEntityName( "env_probe_ingame_placed" );
		dict.Set( "name", name );
	}

	dict.Set( "origin", org.ToString() );

	for( i = 2; i < args.Argc() - 1; i += 2 )
	{

		key = args.Argv( i );
		value = args.Argv( i + 1 );

		dict.Set( key, value );
	}

	gameLocal.SpawnEntityDef( dict );
}

//puncho start - ai-harness (plans/ai-harness/00-PLAN.md §5 WP-H5, measure-plan §2.7): the scale knob for the
// harness. Host only, cheats required; the entity walk is over gameLocal.entities[0 .. num_entities).

/*
===================
Cmd_AISpawnMinions_f

ai_spawnMinions <count> [classname] [spacing] [team]: a square grid of idAI starting 128 units in front
of the player and centred on the view direction, each dropped onto the floor and facing the player,
named minion_<n> after the highest existing one, neverDormant so they think outside the PVS.
===================
*/
void Cmd_AISpawnMinions_f( const idCmdArgs& args )
{
	idPlayer* player = gameLocal.GetLocalPlayer();
	if( !player || !gameLocal.CheatsOk( false ) )
	{
		return;
	}
	if( common->IsClient() )
	{
		gameLocal.Printf( "ai_spawnMinions: host only\n" );
		return;
	}
	if( args.Argc() < 2 )
	{
		gameLocal.Printf( "usage: ai_spawnMinions <count> [classname] [spacing] [team]\n" );
		return;
	}

	int count = atoi( args.Argv( 1 ) );
	const char* classname = ( args.Argc() > 2 ) ? args.Argv( 2 ) : "monster_zombie_maint";
	const float spacing = ( args.Argc() > 3 ) ? atof( args.Argv( 3 ) ) : 72.0f;
	const int team = ( args.Argc() > 4 ) ? atoi( args.Argv( 4 ) ) : 0;
	const int freeSlots = MAX_GENTITIES - gameLocal.num_entities;
	if( count > freeSlots )
	{
		count = freeSlots;
	}
	if( count < 1 )
	{
		gameLocal.Printf( "ai_spawnMinions: count must be at least 1, or no free entities\n" );
		return;
	}

	// a minion off the AAS fails PathToGoal every tic and wanders, polluting pathFailed: only spawn on a
	// reachable walk area of the def's own AAS
	const idDeclEntityDef* def = gameLocal.FindEntityDef( classname, false );
	if( def == NULL )
	{
		gameLocal.Printf( "ai_spawnMinions: unknown entityDef '%s'\n", classname );
		return;
	}
	const idAAS* aas = gameLocal.GetAAS( def->dict.GetString( "use_aas" ) );
	if( aas == NULL )
	{
		gameLocal.Printf( "ai_spawnMinions: no AAS '%s' for '%s' on this map\n", def->dict.GetString( "use_aas" ), classname );
		return;
	}

	// continue the numbering after the highest existing minion_<n>
	int next = 1;
	for( int i = 0; i < gameLocal.num_entities; i++ )
	{
		const idEntity* ent = gameLocal.entities[i];
		if( ent != NULL && ent->name.Cmpn( "minion_", 7 ) == 0 )
		{
			const int n = atoi( ent->name.c_str() + 7 );
			if( n >= next )
			{
				next = n + 1;
			}
		}
	}

	// ceil( sqrt( count ) ) per row; rows go away from the player
	int perRow = ( int )idMath::Sqrt( ( float )count );
	if( perRow * perRow < count )
	{
		perRow++;
	}
	const float yaw = player->viewAngles.yaw;
	const idVec3 forward = idAngles( 0.0f, yaw, 0.0f ).ToForward();
	const idVec3 right = idAngles( 0.0f, yaw - 90.0f, 0.0f ).ToForward();
	const idVec3 playerOrg = player->GetPhysics()->GetOrigin();
	const float halfWidth = 0.5f * ( perRow - 1 ) * spacing;

	int spawned = 0;
	int unplaced = 0;
	for( int i = 0; i < count; i++ )
	{
		const int row = i / perRow;
		const int col = i % perRow;
		idVec3 org = playerOrg + forward * ( 128.0f + row * spacing ) + right * ( col * spacing - halfWidth );

		// one downward trace from 64 above the player's z puts the minion on the floor; a slot over nothing, one
		// that starts in solid, or one outside the AAS is skipped
		trace_t tr;
		const idVec3 top( org.x, org.y, playerOrg.z + 64.0f );
		const idVec3 bottom( org.x, org.y, playerOrg.z - 1024.0f );
		gameLocal.clip.TracePoint( tr, top, bottom, MASK_MONSTERSOLID, player );
		if( tr.fraction >= 1.0f || tr.fraction <= 0.0f )
		{
			unplaced++;
			continue;
		}
		org = tr.endpos;
		if( aas->PointReachableAreaNum( org, aas->GetSettings()->boundingBoxes[0], AREA_REACHABLE_WALK ) == 0 )
		{
			unplaced++;
			continue;
		}

		idDict dict;
		dict.Set( "classname", classname );
		dict.Set( "origin", org.ToString() );
		dict.Set( "angle", va( "%f", ( playerOrg - org ).ToYaw() ) );
		dict.Set( "name", va( "minion_%d", next ) );
		dict.Set( "team", va( "%d", team ) );
		dict.Set( "neverDormant", "1" );
		idEntity* ent = NULL;
		if( !gameLocal.SpawnEntityDef( dict, &ent ) || ent == NULL )
		{
			unplaced++;
			continue;
		}
		// activate it now, as the trigger command does: monster_base::wake_on_enemy() disallows movement until
		// AI_ACTIVATED, and a flag already set when the script reaches it skips that wait, so a team-0 minion
		// with no enemy takes a later ai_moveMinions order (give it a few seconds: state_Idle stops any move on
		// entry). For a team other than the player's, EV_Activate also sets the player as the enemy at once
		// (ReactionTo returns ATTACK_ON_ACTIVATE), which is what a hostile spawn wants
		ent->ProcessEvent( &EV_Activate, player );
		next++;
		spawned++;
	}
	gameLocal.Printf( "ai_spawnMinions: %d spawned, %d could not be placed\n", spawned, unplaced );
}

/*
===================
Cmd_AIMoveMinions_f

ai_moveMinions [aim | here | x y z]: every minion_* gets the moveToPosition event the scripts use, to the
point the player looks at (default), the player's origin, or the given point.
===================
*/
void Cmd_AIMoveMinions_f( const idCmdArgs& args )
{
	idPlayer* player = gameLocal.GetLocalPlayer();
	if( !player || !gameLocal.CheatsOk( false ) )
	{
		return;
	}
	if( common->IsClient() )
	{
		gameLocal.Printf( "ai_moveMinions: host only\n" );
		return;
	}

	idVec3 pos;
	if( args.Argc() >= 4 )
	{
		pos.Set( atof( args.Argv( 1 ) ), atof( args.Argv( 2 ) ), atof( args.Argv( 3 ) ) );
	}
	else if( args.Argc() >= 2 && idStr::Icmp( args.Argv( 1 ), "here" ) == 0 )
	{
		pos = player->GetPhysics()->GetOrigin();
	}
	else
	{
		// the point the player looks at, 8192 units along the view
		trace_t tr;
		const idVec3 start = player->GetEyePosition();
		const idVec3 end = start + player->viewAngles.ToForward() * 8192.0f;
		gameLocal.clip.TracePoint( tr, start, end, MASK_SOLID, player );
		pos = tr.endpos;
	}

	int moved = 0;
	for( int i = 0; i < gameLocal.num_entities; i++ )
	{
		idEntity* ent = gameLocal.entities[i];
		if( ent == NULL || !ent->IsType( idAI::Type ) || ent->name.Cmpn( "minion_", 7 ) != 0 )
		{
			continue;
		}
		// posted, not processed: a console command runs between frames where the census is already closed, so
		// the order is served in the next tic's ServiceEvents and its path calls land in that tic's CSV row
		ent->PostEventMS( &AI_MoveToPosition, 0, pos );
		moved++;
	}
	gameLocal.Printf( "ai_moveMinions: %d ordered to %s\n", moved, pos.ToString() );
}

/*
===================
Cmd_AIKillMinions_f

ai_killMinions: removes every minion_*
===================
*/
void Cmd_AIKillMinions_f( const idCmdArgs& args )
{
	if( !gameLocal.GetLocalPlayer() || !gameLocal.CheatsOk( false ) )
	{
		return;
	}
	if( common->IsClient() )
	{
		gameLocal.Printf( "ai_killMinions: host only\n" );
		return;
	}

	int removed = 0;
	for( int i = 0; i < gameLocal.num_entities; i++ )
	{
		idEntity* ent = gameLocal.entities[i];
		if( ent == NULL || !ent->IsType( idAI::Type ) || ent->name.Cmpn( "minion_", 7 ) != 0 )
		{
			continue;
		}
		// deleted now, as the remove command does, so "ai_killMinions; ai_spawnMinions N" in one frame numbers
		// from minion_1 again (a posted remove would leave a gap the map script's loops stop at)
		delete ent;
		removed++;
	}
	gameLocal.Printf( "ai_killMinions: %d removed\n", removed );
}
//puncho end

/*
==================
Cmd_Damage_f

Damages the specified entity
==================
*/
void Cmd_Damage_f( const idCmdArgs& args )
{
	if( !gameLocal.GetLocalPlayer() || !gameLocal.CheatsOk( false ) )
	{
		return;
	}
	if( args.Argc() != 3 )
	{
		gameLocal.Printf( "usage: damage <name of entity to damage> <damage>\n" );
		return;
	}

	idEntity* ent = gameLocal.FindEntity( args.Argv( 1 ) );
	if( !ent )
	{
		gameLocal.Printf( "entity not found\n" );
		return;
	}

	ent->Damage( gameLocal.world, gameLocal.world, idVec3( 0, 0, 1 ), "damage_moverCrush", atoi( args.Argv( 2 ) ), INVALID_JOINT );
}


/*
==================
Cmd_Remove_f

Removes the specified entity
==================
*/
void Cmd_Remove_f( const idCmdArgs& args )
{
	if( !gameLocal.GetLocalPlayer() || !gameLocal.CheatsOk( false ) )
	{
		return;
	}
	if( args.Argc() != 2 )
	{
		gameLocal.Printf( "usage: remove <name of entity to remove>\n" );
		return;
	}

	idEntity* ent = gameLocal.FindEntity( args.Argv( 1 ) );
	if( !ent )
	{
		gameLocal.Printf( "entity not found\n" );
		return;
	}

	delete ent;
}

/*
===================
Cmd_TestLight_f
===================
*/
void Cmd_TestLight_f( const idCmdArgs& args )
{
	int			i;
	idStr		filename;
	const char* key = NULL, *value = NULL;
	idStr		name;
	idPlayer* 	player = NULL;
	idDict		dict;

	player = gameLocal.GetLocalPlayer();
	if( !player || !gameLocal.CheatsOk( false ) )
	{
		return;
	}

	renderView_t*	rv = player->GetRenderView();

	float fov = tan( idMath::M_DEG2RAD * rv->fov_x / 2 );


	dict.SetMatrix( "rotation", mat3_default );
	dict.SetVector( "origin", rv->vieworg );
	dict.SetVector( "light_target", rv->viewaxis[0] );
	dict.SetVector( "light_right", rv->viewaxis[1] * -fov );
	dict.SetVector( "light_up", rv->viewaxis[2] * fov );
	dict.SetVector( "light_start", rv->viewaxis[0] * 16 );
	dict.SetVector( "light_end", rv->viewaxis[0] * 1000 );

	if( args.Argc() >= 2 )
	{
		value = args.Argv( 1 );
		filename = args.Argv( 1 );
		filename.DefaultFileExtension( ".tga" );
		dict.Set( "texture", filename );
	}

	dict.Set( "classname", "light" );
	for( i = 2; i < args.Argc() - 1; i += 2 )
	{

		key = args.Argv( i );
		value = args.Argv( i + 1 );

		dict.Set( key, value );
	}

	for( i = 0; i < MAX_GENTITIES; i++ )
	{
		name = va( "spawned_light_%d", i );		// not just light_, or it might pick up a prelight shadow
		if( !gameLocal.FindEntity( name ) )
		{
			break;
		}
	}
	dict.Set( "name", name );

	gameLocal.SpawnEntityDef( dict );

	gameLocal.Printf( "Created new light\n" );
}

/*
===================
Cmd_TestPointLight_f
===================
*/
void Cmd_TestPointLight_f( const idCmdArgs& args )
{
	const char* key = NULL, *value = NULL;
	idStr		name;
	int			i;
	idPlayer*	player = NULL;
	idDict		dict;

	player = gameLocal.GetLocalPlayer();
	if( !player || !gameLocal.CheatsOk( false ) )
	{
		return;
	}

	dict.SetVector( "origin", player->GetRenderView()->vieworg );

	if( args.Argc() >= 2 )
	{
		value = args.Argv( 1 );
		dict.Set( "light", value );
	}
	else
	{
		dict.Set( "light", "300" );
	}

	dict.Set( "classname", "light" );
	for( i = 2; i < args.Argc() - 1; i += 2 )
	{

		key = args.Argv( i );
		value = args.Argv( i + 1 );

		dict.Set( key, value );
	}

	for( i = 0; i < MAX_GENTITIES; i++ )
	{
		name = va( "light_%d", i );
		if( !gameLocal.FindEntity( name ) )
		{
			break;
		}
	}
	dict.Set( "name", name );

	gameLocal.SpawnEntityDef( dict );

	gameLocal.Printf( "Created new point light\n" );
}

/*
==================
Cmd_PopLight_f
==================
*/
void Cmd_PopLight_f( const idCmdArgs& args )
{
	idEntity*	ent;
	idMapEntity* mapEnt;
	idMapFile*	mapFile = gameLocal.GetLevelMap();
	idLight*		lastLight;

	if( !gameLocal.CheatsOk() )
	{
		return;
	}

	bool removeFromMap = ( args.Argc() > 1 );

	//puncho start - identity (P1-A2, K12): generations count per slot and say nothing about age; spawnedEntities is in
	// spawn order (RegisterEntity appends, a save keeps the order), so its last light is the newest
	lastLight = NULL;
	for( ent = gameLocal.spawnedEntities.Next(); ent != NULL; ent = ent->spawnNode.Next() )
	{
		if( ent->IsType( idLight::Type ) )
		{
			lastLight = static_cast<idLight*>( ent );
		}
	}
	//puncho end

	if( lastLight )
	{
		// find map file entity
		mapEnt = mapFile->FindEntity( lastLight->name );

		if( removeFromMap && mapEnt )
		{
			mapFile->RemoveEntity( mapEnt );
		}
		gameLocal.Printf( "Removing light %i\n", lastLight->GetLightDefHandle() );
		delete lastLight;
	}
	else
	{
		gameLocal.Printf( "No lights to clear.\n" );
	}

}

/*
====================
Cmd_ClearLights_f
====================
*/
void Cmd_ClearLights_f( const idCmdArgs& args )
{
	idEntity* ent;
	idEntity* next;
	idLight* light;
	idMapEntity* mapEnt;
	idMapFile* mapFile = gameLocal.GetLevelMap();

	bool removeFromMap = ( args.Argc() > 1 );

	gameLocal.Printf( "Clearing all lights.\n" );
	for( ent = gameLocal.spawnedEntities.Next(); ent != NULL; ent = next )
	{
		next = ent->spawnNode.Next();
		if( !ent->IsType( idLight::Type ) )
		{
			continue;
		}

		light = static_cast<idLight*>( ent );
		mapEnt = mapFile->FindEntity( light->name );

		if( removeFromMap && mapEnt )
		{
			mapFile->RemoveEntity( mapEnt );
		}

		delete light;
	}
}

/*
==================
Cmd_TestFx_f
==================
*/
void Cmd_TestFx_f( const idCmdArgs& args )
{
	idVec3		offset;
	const char* name;
	idPlayer* 	player;
	idDict		dict;

	player = gameLocal.GetLocalPlayer();
	if( !player || !gameLocal.CheatsOk() )
	{
		return;
	}

	// delete the testModel if active
	if( gameLocal.testFx )
	{
		delete gameLocal.testFx;
		gameLocal.testFx = NULL;
	}

	if( args.Argc() < 2 )
	{
		return;
	}

	name = args.Argv( 1 );

	offset = player->GetPhysics()->GetOrigin() + player->viewAngles.ToForward() * 100.0f;

	dict.Set( "origin", offset.ToString() );
	dict.Set( "test", "1" );
	dict.Set( "fx", name );
	dict.SetBool( "essential", true );	//puncho - entity budgets (P2 review): testFx is a raw pointer; g_budgetFx must never evict it
	gameLocal.testFx = ( idEntityFx* )gameLocal.SpawnEntityType( idEntityFx::Type, &dict );
}

#define MAX_DEBUGLINES	128

typedef struct
{
	bool used;
	idVec3 start, end;
	int color;
	bool blink;
	bool arrow;
} gameDebugLine_t;

gameDebugLine_t debugLines[MAX_DEBUGLINES];

/*
==================
Cmd_AddDebugLine_f
==================
*/
static void Cmd_AddDebugLine_f( const idCmdArgs& args )
{
	int i, argNum;
	const char* value;

	if( !gameLocal.CheatsOk() )
	{
		return;
	}

	if( args.Argc() < 7 )
	{
		gameLocal.Printf( "usage: addline <x y z> <x y z> <color>\n" );
		return;
	}
	for( i = 0; i < MAX_DEBUGLINES; i++ )
	{
		if( !debugLines[i].used )
		{
			break;
		}
	}
	if( i >= MAX_DEBUGLINES )
	{
		gameLocal.Printf( "no free debug lines\n" );
		return;
	}
	value = args.Argv( 0 );
	if( !idStr::Icmp( value, "addarrow" ) )
	{
		debugLines[i].arrow = true;
	}
	else
	{
		debugLines[i].arrow = false;
	}
	debugLines[i].used = true;
	debugLines[i].blink = false;
	argNum = 1;
	debugLines[i].start.x = Cmd_GetFloatArg( args, argNum );
	debugLines[i].start.y = Cmd_GetFloatArg( args, argNum );
	debugLines[i].start.z = Cmd_GetFloatArg( args, argNum );
	debugLines[i].end.x = Cmd_GetFloatArg( args, argNum );
	debugLines[i].end.y = Cmd_GetFloatArg( args, argNum );
	debugLines[i].end.z = Cmd_GetFloatArg( args, argNum );
	debugLines[i].color = Cmd_GetFloatArg( args, argNum );
}

/*
==================
Cmd_RemoveDebugLine_f
==================
*/
static void Cmd_RemoveDebugLine_f( const idCmdArgs& args )
{
	int i, num;
	const char* value;

	if( !gameLocal.CheatsOk() )
	{
		return;
	}

	if( args.Argc() < 2 )
	{
		gameLocal.Printf( "usage: removeline <num>\n" );
		return;
	}
	value = args.Argv( 1 );
	num = atoi( value );
	for( i = 0; i < MAX_DEBUGLINES; i++ )
	{
		if( debugLines[i].used )
		{
			if( --num < 0 )
			{
				break;
			}
		}
	}
	if( i >= MAX_DEBUGLINES )
	{
		gameLocal.Printf( "line not found\n" );
		return;
	}
	debugLines[i].used = false;
}

/*
==================
Cmd_BlinkDebugLine_f
==================
*/
static void Cmd_BlinkDebugLine_f( const idCmdArgs& args )
{
	int i, num;
	const char* value;

	if( !gameLocal.CheatsOk() )
	{
		return;
	}

	if( args.Argc() < 2 )
	{
		gameLocal.Printf( "usage: blinkline <num>\n" );
		return;
	}
	value = args.Argv( 1 );
	num = atoi( value );
	for( i = 0; i < MAX_DEBUGLINES; i++ )
	{
		if( debugLines[i].used )
		{
			if( --num < 0 )
			{
				break;
			}
		}
	}
	if( i >= MAX_DEBUGLINES )
	{
		gameLocal.Printf( "line not found\n" );
		return;
	}
	debugLines[i].blink = !debugLines[i].blink;
}

/*
==================
PrintFloat
==================
*/
static void PrintFloat( float f )
{
	char buf[128];
	int i;

	for( i = idStr::snPrintf( buf, sizeof( buf ), "%3.2f", f ); i < 7; i++ )
	{
		buf[i] = ' ';
	}
	buf[i] = '\0';
	gameLocal.Printf( buf );
}

/*
==================
Cmd_ListDebugLines_f
==================
*/
static void Cmd_ListDebugLines_f( const idCmdArgs& args )
{
	int i, num;

	if( !gameLocal.CheatsOk() )
	{
		return;
	}

	num = 0;
	gameLocal.Printf( "line num: x1     y1     z1     x2     y2     z2     c  b  a\n" );
	for( i = 0; i < MAX_DEBUGLINES; i++ )
	{
		if( debugLines[i].used )
		{
			gameLocal.Printf( "line %3d: ", num );
			PrintFloat( debugLines[i].start.x );
			PrintFloat( debugLines[i].start.y );
			PrintFloat( debugLines[i].start.z );
			PrintFloat( debugLines[i].end.x );
			PrintFloat( debugLines[i].end.y );
			PrintFloat( debugLines[i].end.z );
			gameLocal.Printf( "%d  %d  %d\n", debugLines[i].color, debugLines[i].blink, debugLines[i].arrow );
			num++;
		}
	}
	if( !num )
	{
		gameLocal.Printf( "no debug lines\n" );
	}
}

/*
==================
D_DrawDebugLines
==================
*/
void D_DrawDebugLines()
{
	int i;
	idVec3 forward, right, up, p1, p2;
	idVec4 color;
	float l;

	for( i = 0; i < MAX_DEBUGLINES; i++ )
	{
		if( debugLines[i].used )
		{
			if( !debugLines[i].blink || ( gameLocal.time & ( 1 << 9 ) ) )
			{
				color = idVec4( debugLines[i].color & 1, ( debugLines[i].color >> 1 ) & 1, ( debugLines[i].color >> 2 ) & 1, 1 );
				gameRenderWorld->DebugLine( color, debugLines[i].start, debugLines[i].end );
				//
				if( debugLines[i].arrow )
				{
					// draw a nice arrow
					forward = debugLines[i].end - debugLines[i].start;
					l = forward.Normalize() * 0.2f;
					forward.NormalVectors( right, up );

					if( l > 3.0f )
					{
						l = 3.0f;
					}
					p1 = debugLines[i].end - l * forward + ( l * 0.4f ) * right;
					p2 = debugLines[i].end - l * forward - ( l * 0.4f ) * right;
					gameRenderWorld->DebugLine( color, debugLines[i].end, p1 );
					gameRenderWorld->DebugLine( color, debugLines[i].end, p2 );
					gameRenderWorld->DebugLine( color, p1, p2 );
				}
			}
		}
	}
}

/*
==================
Cmd_ListCollisionModels_f
==================
*/
static void Cmd_ListCollisionModels_f( const idCmdArgs& args )
{
	if( !gameLocal.CheatsOk() )
	{
		return;
	}

	collisionModelManager->ListModels();
}

/*
==================
Cmd_CollisionModelInfo_f
==================
*/
static void Cmd_CollisionModelInfo_f( const idCmdArgs& args )
{
	const char* value;

	if( !gameLocal.CheatsOk() )
	{
		return;
	}

	if( args.Argc() < 2 )
	{
		gameLocal.Printf( "usage: collisionModelInfo <modelNum>\n"
						  "use 'all' instead of the model number for accumulated info\n" );
		return;
	}

	value = args.Argv( 1 );
	if( !idStr::Icmp( value, "all" ) )
	{
		collisionModelManager->ModelInfo( -1 );
	}
	else
	{
		collisionModelManager->ModelInfo( atoi( value ) );
	}
}

/*
==================
Cmd_ReloadAnims_f
==================
*/
static void Cmd_ReloadAnims_f( const idCmdArgs& args )
{
	// don't allow reloading anims when cheats are disabled,
	// but if we're not in the game, it's ok
	if( gameLocal.GetLocalPlayer() && !gameLocal.CheatsOk( false ) )
	{
		return;
	}

	animationLib.ReloadAnims();
}

/*
==================
Cmd_ListAnims_f
==================
*/
static void Cmd_ListAnims_f( const idCmdArgs& args )
{
	idEntity* 		ent;
	int				num;
	size_t			size;
	size_t			alloced;
	idAnimator* 	animator;
	const char* 	classname;
	const idDict* 	dict;
	int				i;

	if( args.Argc() > 1 )
	{
		idAnimator animator;

		classname = args.Argv( 1 );

		dict = gameLocal.FindEntityDefDict( classname, false );
		if( !dict )
		{
			gameLocal.Printf( "Entitydef '%s' not found\n", classname );
			return;
		}
		animator.SetModel( dict->GetString( "model" ) );

		gameLocal.Printf( "----------------\n" );
		num = animator.NumAnims();
		for( i = 0; i < num; i++ )
		{
			gameLocal.Printf( "%s\n", animator.AnimFullName( i ) );
		}
		gameLocal.Printf( "%d anims\n", num );
	}
	else
	{
		animationLib.ListAnims();

		size = 0;
		num = 0;
		for( ent = gameLocal.spawnedEntities.Next(); ent != NULL; ent = ent->spawnNode.Next() )
		{
			animator = ent->GetAnimator();
			if( animator )
			{
				alloced = animator->Allocated();
				size += alloced;
				num++;
			}
		}

		gameLocal.Printf( "%d memory used in %d entity animators\n", size, num );
	}
}

/*
==================
Cmd_AASStats_f
==================
*/
static void Cmd_AASStats_f( const idCmdArgs& args )
{
	int aasNum;

	if( !gameLocal.CheatsOk() )
	{
		return;
	}

	aasNum = aas_test.GetInteger();
	idAAS* aas = gameLocal.GetAAS( aasNum );
	if( !aas )
	{
		gameLocal.Printf( "No aas #%d loaded\n", aasNum );
	}
	else
	{
		aas->Stats();
	}
}

/*
==================
Cmd_TestDamage_f
==================
*/
static void Cmd_TestDamage_f( const idCmdArgs& args )
{
	idPlayer* player;
	const char* damageDefName;

	player = gameLocal.GetLocalPlayer();
	if( !player || !gameLocal.CheatsOk() )
	{
		return;
	}

	if( args.Argc() < 2 || args.Argc() > 3 )
	{
		gameLocal.Printf( "usage: testDamage <damageDefName> [angle]\n" );
		return;
	}

	damageDefName = args.Argv( 1 );

	idVec3	dir;
	if( args.Argc() == 3 )
	{
		float angle = atof( args.Argv( 2 ) );

		idMath::SinCos( DEG2RAD( angle ), dir[1], dir[0] );
		dir[2] = 0;
	}
	else
	{
		dir.Zero();
	}

	// give the player full health before and after
	// running the damage
	player->health = player->inventory.maxHealth;
	player->Damage( NULL, NULL, dir, damageDefName, 1.0f, INVALID_JOINT );
	player->health = player->inventory.maxHealth;
}

/*
==================
Cmd_TestBoneFx_f
==================
*/
static void Cmd_TestBoneFx_f( const idCmdArgs& args )
{
	idPlayer* player;
	const char* bone, *fx;

	player = gameLocal.GetLocalPlayer();
	if( !player || !gameLocal.CheatsOk() )
	{
		return;
	}

	if( args.Argc() < 3 || args.Argc() > 4 )
	{
		gameLocal.Printf( "usage: testBoneFx <fxName> <boneName>\n" );
		return;
	}

	fx = args.Argv( 1 );
	bone = args.Argv( 2 );

	player->StartFxOnBone( fx, bone );
}

/*
==================
Cmd_TestDamage_f
==================
*/
static void Cmd_TestDeath_f( const idCmdArgs& args )
{
	idPlayer* player;

	player = gameLocal.GetLocalPlayer();
	if( !player || !gameLocal.CheatsOk() )
	{
		return;
	}

	idVec3 dir;
	idMath::SinCos( DEG2RAD( 45.0f ), dir[1], dir[0] );
	dir[2] = 0;

	g_testDeath.SetBool( 1 );
	player->Damage( NULL, NULL, dir, "damage_triggerhurt_1000", 1.0f, INVALID_JOINT );
	if( args.Argc() >= 2 )
	{
		player->SpawnGibs( dir, "damage_triggerhurt_1000" );
	}

}

/*
==================
Cmd_WeaponSplat_f
==================
*/
static void Cmd_WeaponSplat_f( const idCmdArgs& args )
{
	idPlayer* player;

	player = gameLocal.GetLocalPlayer();
	if( !player || !gameLocal.CheatsOk() )
	{
		return;
	}

	player->weapon.GetEntity()->BloodSplat( 2.0f );
}

/*
==================
Cmd_SaveSelected_f
==================
*/
static void Cmd_SaveSelected_f( const idCmdArgs& args )
{
	int i;
	idPlayer* player = NULL;
	idEntity* s = NULL;
	idMapEntity* mapEnt;
	idMapFile* mapFile = gameLocal.GetLevelMap();
	idDict dict;
	idStr mapName;
	idStr name;

	player = gameLocal.GetLocalPlayer();
	if( !player || !gameLocal.CheatsOk() )
	{
		return;
	}

	s = player->dragEntity.GetSelected();
	if( !s )
	{
		gameLocal.Printf( "no entity selected, set g_dragShowSelection 1 to show the current selection\n" );
		return;
	}

	if( args.Argc() > 1 )
	{
		mapName = args.Argv( 1 );
		mapName = "maps/" + mapName;
	}
	else
	{
		mapName = mapFile->GetName();
	}

	// find map file entity
	mapEnt = mapFile->FindEntity( s->name );
	// create new map file entity if there isn't one for this articulated figure
	if( !mapEnt )
	{
		mapEnt = new( TAG_SYSTEM ) idMapEntity();
		mapFile->AddEntity( mapEnt );
		for( i = 0; i < 9999; i++ )
		{
			name = va( "%s_%d", s->GetEntityDefName(), i );
			if( !gameLocal.FindEntity( name ) )
			{
				break;
			}
		}
		s->name = name;
		mapEnt->epairs.Set( "classname", s->GetEntityDefName() );
		mapEnt->epairs.Set( "name", s->name );
	}

	if( s->IsType( idMoveable::Type ) )
	{
		// save the moveable state
		mapEnt->epairs.Set( "origin", s->GetPhysics()->GetOrigin().ToString( 8 ) );
		mapEnt->epairs.Set( "rotation", s->GetPhysics()->GetAxis().ToString( 8 ) );
	}
	else if( s->IsType( idAFEntity_Generic::Type ) || s->IsType( idAFEntity_WithAttachedHead::Type ) )
	{
		// save the articulated figure state
		dict.Clear();
		static_cast<idAFEntity_Base*>( s )->SaveState( dict );
		mapEnt->epairs.Copy( dict );
	}

	// write out the map file
	mapFile->WriteLevelEdits( mapName );	//puncho - MP-A5 (K41a): a patch into the write target, or the map whole
}

/*
==================
Cmd_DeleteSelected_f
==================
*/
static void Cmd_DeleteSelected_f( const idCmdArgs& args )
{
	idPlayer* player;

	player = gameLocal.GetLocalPlayer();
	if( !player || !gameLocal.CheatsOk() )
	{
		return;
	}

	if( player )
	{
		player->dragEntity.DeleteSelected();
	}
}

/*
==================
Cmd_SaveMoveables_f
==================
*/
static void Cmd_SaveMoveables_f( const idCmdArgs& args )
{
	int e, i;
	idMoveable* m = NULL;
	idMapEntity* mapEnt = NULL;
	idMapFile* mapFile = gameLocal.GetLevelMap();
	idStr mapName;
	idStr name;

	if( !gameLocal.CheatsOk() )
	{
		return;
	}

	for( e = 0; e < MAX_GENTITIES; e++ )
	{
		m = static_cast<idMoveable*>( gameLocal.entities[ e ] );

		if( !m || !m->IsType( idMoveable::Type ) )
		{
			continue;
		}

		if( m->IsBound() )
		{
			continue;
		}

		if( !m->IsAtRest() )
		{
			break;
		}
	}

	if( e < MAX_GENTITIES )
	{
		gameLocal.Warning( "map not saved because the moveable entity %s is not at rest", gameLocal.entities[ e ]->name.c_str() );
		return;
	}

	if( args.Argc() > 1 )
	{
		mapName = args.Argv( 1 );
		mapName = "maps/" + mapName;
	}
	else
	{
		mapName = mapFile->GetName();
	}

	for( e = 0; e < MAX_GENTITIES; e++ )
	{
		m = static_cast<idMoveable*>( gameLocal.entities[ e ] );

		if( !m || !m->IsType( idMoveable::Type ) )
		{
			continue;
		}

		if( m->IsBound() )
		{
			continue;
		}

		// find map file entity
		mapEnt = mapFile->FindEntity( m->name );
		// create new map file entity if there isn't one for this articulated figure
		if( !mapEnt )
		{
			mapEnt = new( TAG_SYSTEM ) idMapEntity();
			mapFile->AddEntity( mapEnt );
			for( i = 0; i < 9999; i++ )
			{
				name = va( "%s_%d", m->GetEntityDefName(), i );
				if( !gameLocal.FindEntity( name ) )
				{
					break;
				}
			}
			m->name = name;
			mapEnt->epairs.Set( "classname", m->GetEntityDefName() );
			mapEnt->epairs.Set( "name", m->name );
		}
		// save the moveable state
		mapEnt->epairs.Set( "origin", m->GetPhysics()->GetOrigin().ToString( 8 ) );
		mapEnt->epairs.Set( "rotation", m->GetPhysics()->GetAxis().ToString( 8 ) );
	}

	// write out the map file
	mapFile->WriteLevelEdits( mapName );	//puncho - MP-A5 (K41a): a patch into the write target, or the map whole
}

/*
==================
Cmd_SaveRagdolls_f
==================
*/
static void Cmd_SaveRagdolls_f( const idCmdArgs& args )
{
	int e, i;
	idAFEntity_Base* af = NULL;
	idMapEntity* mapEnt = NULL;
	idMapFile* mapFile = gameLocal.GetLevelMap();
	idDict dict;
	idStr mapName;
	idStr name;

	if( !gameLocal.CheatsOk() )
	{
		return;
	}

	if( args.Argc() > 1 )
	{
		mapName = args.Argv( 1 );
		mapName = "maps/" + mapName;
	}
	else
	{
		mapName = mapFile->GetName();
	}

	for( e = 0; e < MAX_GENTITIES; e++ )
	{
		af = static_cast<idAFEntity_Base*>( gameLocal.entities[ e ] );

		if( !af )
		{
			continue;
		}

		if( !af->IsType( idAFEntity_WithAttachedHead::Type ) && !af->IsType( idAFEntity_Generic::Type ) )
		{
			continue;
		}

		if( af->IsBound() )
		{
			continue;
		}

		if( !af->IsAtRest() )
		{
			gameLocal.Warning( "the articulated figure for entity %s is not at rest", gameLocal.entities[ e ]->name.c_str() );
		}

		dict.Clear();
		af->SaveState( dict );

		// find map file entity
		mapEnt = mapFile->FindEntity( af->name );
		// create new map file entity if there isn't one for this articulated figure
		if( !mapEnt )
		{
			mapEnt = new( TAG_SYSTEM ) idMapEntity();
			mapFile->AddEntity( mapEnt );
			for( i = 0; i < 9999; i++ )
			{
				name = va( "%s_%d", af->GetEntityDefName(), i );
				if( !gameLocal.FindEntity( name ) )
				{
					break;
				}
			}
			af->name = name;
			mapEnt->epairs.Set( "classname", af->GetEntityDefName() );
			mapEnt->epairs.Set( "name", af->name );
		}
		// save the articulated figure state
		mapEnt->epairs.Copy( dict );
	}

	// write out the map file
	mapFile->WriteLevelEdits( mapName );	//puncho - MP-A5 (K41a): a patch into the write target, or the map whole
}

/*
==================
Cmd_BindRagdoll_f
==================
*/
static void Cmd_BindRagdoll_f( const idCmdArgs& args )
{
	idPlayer* player;

	player = gameLocal.GetLocalPlayer();
	if( !player || !gameLocal.CheatsOk() )
	{
		return;
	}

	if( player )
	{
		player->dragEntity.BindSelected();
	}
}

/*
==================
Cmd_UnbindRagdoll_f
==================
*/
static void Cmd_UnbindRagdoll_f( const idCmdArgs& args )
{
	idPlayer* player;

	player = gameLocal.GetLocalPlayer();
	if( !player || !gameLocal.CheatsOk() )
	{
		return;
	}

	if( player )
	{
		player->dragEntity.UnbindSelected();
	}
}

/*
==================
Cmd_GameError_f
==================
*/
static void Cmd_GameError_f( const idCmdArgs& args )
{
	gameLocal.Error( "game error" );
}

/*
==================
Cmd_SaveLights_f
==================
*/
static void Cmd_SaveLights_f( const idCmdArgs& args )
{
	int e, i;
	idLight* light = NULL;
	idMapEntity* mapEnt = NULL;
	idMapFile* mapFile = gameLocal.GetLevelMap();
	idDict dict;
	idStr mapName;
	idStr name;

	if( !gameLocal.CheatsOk() )
	{
		return;
	}

	if( args.Argc() > 1 )
	{
		mapName = args.Argv( 1 );
		mapName = "maps/" + mapName;
	}
	else
	{
		mapName = mapFile->GetName();
	}

	for( e = 0; e < MAX_GENTITIES; e++ )
	{
		light = static_cast<idLight*>( gameLocal.entities[ e ] );

		if( !light || !light->IsType( idLight::Type ) )
		{
			continue;
		}

		dict.Clear();
		light->SaveState( &dict );

		// find map file entity
		mapEnt = mapFile->FindEntity( light->name );
		// create new map file entity if there isn't one for this light
		if( !mapEnt )
		{
			mapEnt = new( TAG_SYSTEM ) idMapEntity();
			mapFile->AddEntity( mapEnt );
			for( i = 0; i < 9999; i++ )
			{
				name = va( "%s_%d", light->GetEntityDefName(), i );
				if( !gameLocal.FindEntity( name ) )
				{
					break;
				}
			}
			light->name = name;
			mapEnt->epairs.Set( "classname", light->GetEntityDefName() );
			mapEnt->epairs.Set( "name", light->name );
		}
		// save the light state
		mapEnt->epairs.Copy( dict );
	}

	// write out the map file
	mapFile->WriteLevelEdits( mapName );	//puncho - MP-A5 (K41a): a patch into the write target, or the map whole
}


// RB begin
/*
==================
Cmd_SaveEnvprobes_f
==================
*/
static void Cmd_SaveEnvprobes_f( const idCmdArgs& args )
{
	int e;
	EnvironmentProbe* envProbe = NULL;
	idMapEntity* mapEnt = NULL;
	idMapFile* mapFile = gameLocal.GetLevelMap();
	idDict dict;
	idStr mapName;
	idMapFile mapExportFile;
	const char* name = NULL;

	if( !gameLocal.CheatsOk() )
	{
		return;
	}

	if( args.Argc() > 1 )
	{
		mapName = args.Argv( 1 );
		mapName = "maps/" + mapName;
	}
	else
	{
		mapName = mapFile->GetName();
	}

	//puncho start - MP-A5 (K41a): with layers the probes go into the level map, and WriteLevelEdits writes the edits as
	// the write target's patch, which keeps that patch's other edits and leaves out the probes a lower layer holds. A
	// probe whose name a map entity has updates it; an unnamed map probe at its origin stays as it is, since no patch can
	// address it; any other probe, one the game placed itself, is added. One layer writes every probe into a new patch file.
	if( fileSystem->GetNumLayers() > 1 )
	{
		for( e = 0; e < MAX_GENTITIES; e++ )
		{
			envProbe = static_cast<EnvironmentProbe*>( gameLocal.entities[ e ] );
			if( !envProbe || !envProbe->IsType( EnvironmentProbe::Type ) )
			{
				continue;
			}
			dict.Clear();
			envProbe->SaveState( &dict );

			mapEnt = mapFile->FindEntity( envProbe->name );
			if( mapEnt == NULL )
			{
				const idVec3 origin = dict.GetVector( "origin" );
				int i;
				for( i = 1; i < mapFile->GetNumEntities(); i++ )
				{
					const idDict& epairs = mapFile->GetEntity( i )->epairs;
					if( epairs.GetString( "name" )[0] == '\0' && idStr::Icmp( epairs.GetString( "classname" ), envProbe->GetEntityDefName() ) == 0 && epairs.GetVector( "origin" ).Compare( origin, 0.1f ) )
					{
						break;
					}
				}
				if( i < mapFile->GetNumEntities() )
				{
					continue;
				}
				mapEnt = new( TAG_SYSTEM ) idMapEntity();
				mapFile->AddEntity( mapEnt );
				mapEnt->epairs.Set( "classname", envProbe->GetEntityDefName() );
			}
			mapEnt->epairs.Copy( dict );
		}
		mapFile->WriteLevelEdits( mapName );
		return;
	}
	//puncho end

	mapName += "_extra_ents.map";

	for( e = 0; e < MAX_GENTITIES; e++ )
	{
		envProbe = static_cast<EnvironmentProbe*>( gameLocal.entities[ e ] );

		if( !envProbe || !envProbe->IsType( EnvironmentProbe::Type ) )
		{
			continue;
		}

		dict.Clear();
		envProbe->SaveState( &dict );

		mapEnt = new( TAG_SYSTEM ) idMapEntity();
		mapExportFile.AddEntity( mapEnt );

		envProbe->name = name;
		mapEnt->epairs.Set( "classname", envProbe->GetEntityDefName() );
		mapEnt->epairs.Set( "name", envProbe->name );

		// save the env probes state
		mapEnt->epairs.Copy( dict );
	}

	// write out the map file
	mapExportFile.Write( mapName, ".map" );
}
// RB end

/*
==================
Cmd_SaveParticles_f
==================
*/
static void Cmd_SaveParticles_f( const idCmdArgs& args )
{
	int e;
	idEntity* ent;
	idMapEntity* mapEnt;
	idMapFile* mapFile = gameLocal.GetLevelMap();
	idDict dict;
	idStr mapName, strModel;

	if( !gameLocal.CheatsOk() )
	{
		return;
	}

	if( args.Argc() > 1 )
	{
		mapName = args.Argv( 1 );
		mapName = "maps/" + mapName;
	}
	else
	{
		mapName = mapFile->GetName();
	}

	for( e = 0; e < MAX_GENTITIES; e++ )
	{

		ent = static_cast<idStaticEntity*>( gameLocal.entities[ e ] );

		if( !ent )
		{
			continue;
		}

		strModel = ent->spawnArgs.GetString( "model" );
		if( strModel.Length() && strModel.Find( ".prt" ) > 0 )
		{
			dict.Clear();
			dict.Set( "model", ent->spawnArgs.GetString( "model" ) );
			dict.SetVector( "origin", ent->GetPhysics()->GetOrigin() );

			// find map file entity
			mapEnt = mapFile->FindEntity( ent->name );
			// create new map file entity if there isn't one for this entity
			if( !mapEnt )
			{
				continue;
			}
			// save the particle state
			mapEnt->epairs.Copy( dict );
		}
	}

	// write out the map file
	mapFile->WriteLevelEdits( mapName );	//puncho - MP-A5 (K41a): a patch into the write target, or the map whole
}


/*
==================
Cmd_DisasmScript_f
==================
*/
static void Cmd_DisasmScript_f( const idCmdArgs& args )
{
	gameLocal.program.Disassemble();
}

/*
==================
Cmd_TestSave_f
==================
*/
static void Cmd_TestSave_f( const idCmdArgs& args )
{
	idFile* f, *strings;

	f = fileSystem->OpenFileWrite( "test.sav" );
	strings = NULL;
	gameLocal.SaveGame( f, strings );
	fileSystem->CloseFile( f );
}

//puncho start - stream-trace (P1-A1c, K3-K5): the stream-trace checker's commands (StreamTrace.h)
/*
==================
idStreamTraceArm

Arms the next save or load and disarms it however the call ends: a load that fails calls gameLocal.Error, whose
idException unwinds past the caller, and an armed pointer left behind would trace the next real save into a dead
stack object.
==================
*/
class idStreamTraceArm
{
public:
	idStreamTraceArm( idStreamTrace* trace )
	{
		idStreamTrace::armed = trace;
	}
	~idStreamTraceArm()
	{
		idStreamTrace::armed = NULL;
	}
};

/*
==================
SaveTraced

Saves the game into memory through the compressing pipeline a real save uses (idCommonLocal::SaveGame), traced.
==================
*/
static void SaveTraced( idStreamTrace& trace, idFile_Memory& saveFile, idFile_Memory& stringsFile )
{
	idFile_SaveGamePipelined* pipeline = new( TAG_SAVEGAMES ) idFile_SaveGamePipelined();
	pipeline->OpenForWriting( &saveFile );
	{
		idStreamTraceArm arm( &trace );
		gameLocal.SaveGame( pipeline, &stringsFile );
	}
	pipeline->Finish();
	delete pipeline;
	trace.Finish();
}

/*
==================
CanTraceSave
==================
*/
static bool CanTraceSave( const char* command )
{
	if( common->IsMultiplayer() || gameLocal.GameState() != GAMESTATE_ACTIVE || gameLocal.GetLocalPlayer() == NULL )
	{
		gameLocal.Printf( "%s: needs a single-player map running\n", command );
		return false;
	}
	return true;
}

/*
==================
LoadTraced

The load idCommonLocal::ExecuteMapChange makes, on the render and sound worlds already up: a
load through the session would run frames first, and no frame may run before the next save.
==================
*/
static bool LoadTraced( idStreamTrace& trace, idFile_Memory& saveFile, idFile_Memory& stringsFile, const char* mapName, idRenderWorld* renderWorld, idSoundWorld* soundWorld )
{
	saveFile.MakeReadOnly();
	stringsFile.MakeReadOnly();
	bool loaded;
	{
		idStreamTraceArm arm( &trace );
		loaded = gameLocal.InitFromSaveGame( mapName, renderWorld, soundWorld, &saveFile, &stringsFile, BUILD_NUMBER );
	}
	trace.Finish();
	return loaded;
}

/*
==================
Cmd_TestSaveRoundTrip_f

K3: save to memory, load that and save again, each save taken right after InitFromSaveGame returns, then load the
second save and take a third. The first trip holds each scope's read trace against its write trace. The second and
third saves are compared byte for byte apart from the ints written by idSaveGame::WriteRenderHandle (P1-m10b): a
restore's one-time fix-ups (UpdateVisuals and Present on every entity, a GUI's StateChanged, idClipModel's
touchCount) have run before both of them, so they must agree. The game goes on from the reloaded state.
==================
*/
static void Cmd_TestSaveRoundTrip_f( const idCmdArgs& args )
{
	if( !CanTraceSave( "testSaveRoundTrip" ) )
	{
		return;
	}

	idStreamTrace firstSave, firstLoad, secondSave, secondLoad, thirdSave;
	secondSave.SetCapture( true );
	thirdSave.SetCapture( true );
	idFile_Memory firstFile( "testSaveRoundTrip_1" ), firstStrings( "testSaveRoundTrip_1_strings" );
	idFile_Memory secondFile( "testSaveRoundTrip_2" ), secondStrings( "testSaveRoundTrip_2_strings" );
	idFile_Memory thirdFile( "testSaveRoundTrip_3" ), thirdStrings( "testSaveRoundTrip_3_strings" );

	// the sound world saves its clock and the load shifts every channel by how far it moved, so it holds still
	idRenderWorld* renderWorld = gameRenderWorld;
	idSoundWorld* soundWorld = gameSoundWorld;
	const bool soundWasPaused = soundWorld->IsPaused();
	soundWorld->Pause();

	idStr mapName = gameLocal.GetMapName();
	mapName += ".map";

	// a load that spawns what the save already holds leaves the count higher
	const int entitiesBefore = gameLocal.spawnedEntities.Num();
	int entitiesFirstLoad = -1;
	int entitiesSecondLoad = -1;

	SaveTraced( firstSave, firstFile, firstStrings );
	bool loaded = LoadTraced( firstLoad, firstFile, firstStrings, mapName, renderWorld, soundWorld );
	if( loaded )
	{
		entitiesFirstLoad = gameLocal.spawnedEntities.Num();
		SaveTraced( secondSave, secondFile, secondStrings );
		loaded = LoadTraced( secondLoad, secondFile, secondStrings, mapName, renderWorld, soundWorld );
	}
	if( loaded )
	{
		entitiesSecondLoad = gameLocal.spawnedEntities.Num();
		SaveTraced( thirdSave, thirdFile, thirdStrings );
	}

	if( !soundWasPaused )
	{
		soundWorld->UnPause();
	}
	if( !loaded )
	{
		// P1-m13: a refused load has torn the world down, and the next frame would spawn the player into it.
		// idCommonLocal::ExecuteMapChange quits to the title after a refused load; the drop does the same here
		gameLocal.Error( "testSaveRoundTrip: the load refused the save (the script changed?)" );
		return;
	}

	gameLocal.Printf( "testSaveRoundTrip: %s, %d scopes, %d entries, %d bytes uncompressed (%d, %d and %d compressed)\n",
					  mapName.c_str(), firstSave.NumScopes(), firstSave.NumEntries(), firstSave.NumBytes(), firstFile.Length(), secondFile.Length(), thirdFile.Length() );

	gameLocal.Printf( "testSaveRoundTrip: the first trip, each scope's read trace against its write trace:\n" );
	int traceFailures = firstSave.CheckCoverage() + firstLoad.CheckCoverage() + secondSave.CheckCoverage() + secondLoad.CheckCoverage() + thirdSave.CheckCoverage();
	traceFailures += idStreamTrace::CompareShapes( firstSave, firstLoad );

	gameLocal.Printf( "testSaveRoundTrip: the third save against the second, byte for byte:\n" );
	const int byteFailures = idStreamTrace::CompareBytes( secondSave, thirdSave, secondStrings, thirdStrings );

	gameLocal.Printf( "testSaveRoundTrip: %s: %d trace mismatches, %d byte differences outside the allowlist; entities %d before, %d and %d after the loads\n",
					  ( traceFailures == 0 && byteFailures == 0 ) ? "PASS" : "FAIL", traceFailures, byteFailures, entitiesBefore, entitiesFirstLoad, entitiesSecondLoad );
}

/*
==================
Cmd_TestSaveTraceWrite_f

K5: each class's write trace of the current state, into a file a later build compares against.
==================
*/
static void Cmd_TestSaveTraceWrite_f( const idCmdArgs& args )
{
	if( args.Argc() < 2 )
	{
		gameLocal.Printf( "usage: testSaveTraceWrite <file under fs_savepath>\n" );
		return;
	}
	if( !CanTraceSave( "testSaveTraceWrite" ) )
	{
		return;
	}

	idStreamTrace trace;
	idFile_Memory saveFile( "testSaveTrace" ), stringsFile( "testSaveTrace_strings" );
	SaveTraced( trace, saveFile, stringsFile );
	gameLocal.Printf( "testSaveTraceWrite: %s\n", trace.WriteFile( args.Argv( 1 ) ) ? "done" : "FAIL: can't write the file" );
}

/*
==================
Cmd_TestSaveTraceCompare_f

K5: the current state's write trace against one an earlier build wrote; an order or type change shows per class and
position. Both should come from the same map at the same moment, or state differences show as well.
==================
*/
static void Cmd_TestSaveTraceCompare_f( const idCmdArgs& args )
{
	if( args.Argc() < 2 )
	{
		gameLocal.Printf( "usage: testSaveTraceCompare <file written by testSaveTraceWrite> (under fs_savepath, or an absolute path in quotes)\n" );
		return;
	}
	if( !CanTraceSave( "testSaveTraceCompare" ) )
	{
		return;
	}

	idStreamTrace before;
	if( !before.ReadFile( args.Argv( 1 ) ) )
	{
		gameLocal.Printf( "testSaveTraceCompare: FAIL: no trace to compare against\n" );
		return;
	}
	idStreamTrace now;
	idFile_Memory saveFile( "testSaveTrace" ), stringsFile( "testSaveTrace_strings" );
	SaveTraced( now, saveFile, stringsFile );
	const int classesDiffer = idStreamTrace::CompareClasses( before, now );
	gameLocal.Printf( "testSaveTraceCompare: %s: %d classes differ\n", ( classesDiffer == 0 ) ? "PASS" : "FAIL", classesDiffer );
}

/*
==================
Cmd_TestSnapTraceCompare_f

K4: a host's net_snapTrace file against a client's. This peer's own trace stops first, since it may be one of them.
==================
*/
static void Cmd_TestSnapTraceCompare_f( const idCmdArgs& args )
{
	if( args.Argc() < 3 )
	{
		gameLocal.Printf( "usage: testSnapTraceCompare <host trace> <client trace> (under fs_savepath, or absolute paths in quotes)\n" );
		return;
	}
	cvarSystem->SetCVarString( "net_snapTrace", "" );
	snapshotTrace.Close();
	const int failures = idSnapshotTrace::Compare( args.Argv( 1 ), args.Argv( 2 ) );
	gameLocal.Printf( "testSnapTraceCompare: %s: %d client reads failed\n", ( failures == 0 ) ? "PASS" : "FAIL", failures );
}
//puncho end

//puncho start - identity (P1-A2)
/*
==================
Cmd_TestSpawnChurn_f

testSpawnChurn [spawns] [alive]: P1-A2's churn gate. Spawns and deletes idTarget, `alive` of them
live at once, so the freed slots go round instead of one slot coming straight back. After every spawn
each live idEntityPtr must resolve to its entity, and the pointer to the entity just deleted must
resolve to nothing, the new occupant of its slot included. With g_slotGenFastForward on one of the
slots it uses, the same checks cover generation MAX_SPAWN_GENERATION and the wrap.
==================
*/
static void Cmd_TestSpawnChurn_f( const idCmdArgs& args )
{
	if( !gameLocal.CheatsOk( false ) )
	{
		return;
	}
	if( common->IsMultiplayer() )
	{
		gameLocal.Printf( "testSpawnChurn: single player only\n" );
		return;
	}

	const int spawns = ( args.Argc() > 1 ) ? atoi( args.Argv( 1 ) ) : 300000;
	const int alive = idMath::ClampInt( 1, 4096, ( args.Argc() > 2 ) ? atoi( args.Argv( 2 ) ) : 256 );

	idList< idEntity* > live;
	idList< idEntityPtr< idEntity > > livePtrs;
	live.AssureSize( alive, NULL );
	livePtrs.SetNum( alive );

	idDict dict;
	dict.Set( "classname", "testSpawnChurn" );	// no such entityDef: an idTarget is an idEntity with nothing added

	int liveMisses = 0;
	int staleHits = 0;
	int wraps = 0;
	int maxGen = 0;
	int lowSlot = MAX_GENTITIES;
	int highSlot = -1;
	const int startMs = Sys_Milliseconds();
	for( int i = 0; i < spawns; i++ )
	{
		const int k = i % alive;
		idEntityPtr< idEntity > stale;
		int staleSlot = -1;
		int staleGen = 0;
		if( live[ k ] != NULL )
		{
			stale = livePtrs[ k ];
			staleSlot = live[ k ]->entityNumber;
			staleGen = gameLocal.spawnIds[ staleSlot ];
			delete live[ k ];
		}

		idEntity* ent = gameLocal.SpawnEntityType( idTarget::Type, &dict );
		live[ k ] = ent;
		livePtrs[ k ] = ent;

		const int slot = ent->entityNumber;
		const int gen = gameLocal.spawnIds[ slot ];
		if( slot == staleSlot && gen <= staleGen )
		{
			wraps++;
		}
		maxGen = Max( maxGen, gen );
		lowSlot = Min( lowSlot, slot );
		highSlot = Max( highSlot, slot );

		if( stale.GetEntity() != NULL )
		{
			staleHits++;
		}
		for( int j = 0; j < alive; j++ )
		{
			if( live[ j ] != NULL && livePtrs[ j ].GetEntity() != live[ j ] )
			{
				liveMisses++;
			}
		}
	}

	for( int j = 0; j < alive; j++ )
	{
		delete live[ j ];
	}

	gameLocal.Printf( "testSpawnChurn: %d spawns, %d alive, slots %d-%d, highest generation %d, %d wraps seen, %d live pointers missed, %d stale pointers resolved, %d ms\n",
					  spawns, alive, lowSlot, highSlot, maxGen, wraps, liveMisses, staleHits, Sys_Milliseconds() - startMs );
}
//puncho end

//puncho start - content check (P1-A3a): a test tool for the implicit decl announce table
static idEntityPtr<idProjectile> testDeclRemapRocket;

/*
==================
Cmd_TestDeclRemap_f

Each use drives a real path of the implicit decl announce table:
  make <prefix> <count> [reverse]	makes <count> implicit materials <prefix>/mtr_<i> and sounds <prefix>/snd_<i>, first
									to last or last to first, so two peers number them differently
  decode <type> <index>				a client decodes an index as a snapshot would (ClientRemapDecl), so an unannounced
									slot shows its one warning; type is a decl type name
  sound <name>						the host plays a sound to everyone by name: GAME_RELIABLE_MESSAGE_SOUND_INDEX
									(idMultiplayerGame::PlayGlobalSound) and EVENT_STARTSOUNDSHADER from its player
  rocket							the host spawns a still rocket over its player, launched so it detonates on impact
  impact <material>					the host's rocket hits the world on a surface of that material: EVENT_DAMAGE_EFFECT
									(idProjectile::AddDefaultDamageEffect)
  fx <prefix> <count>				the host spawns <count> func_fx whose effect <prefix>/fx_<i> is an implicit decl, so
									every snapshot carries them (idEntityFx::WriteToSnapshot), and announces each effect at
									once, as an event would: a host with no peers writes no snapshots, and a joiner must find
									the slots already taken
  speakers <prefix> <count>			the host spawns <count> speakers whose timers all fire in one later game frame, each
									playing its implicit sound <prefix>/snd_<i> as an entity event, so that frame's first
									uses of the table are announced together
net_debugDeclRemap prints the slots as the host announces them and as a client resolves them.
==================
*/
static void Cmd_TestDeclRemap_f( const idCmdArgs& args )
{
	const char* what = args.Argv( 1 );
	if( idStr::Icmp( what, "make" ) == 0 && args.Argc() >= 4 )
	{
		const int count = atoi( args.Argv( 3 ) );
		const bool reverse = ( idStr::Icmp( args.Argv( 4 ), "reverse" ) == 0 );
		for( int n = 0; n < count; n++ )
		{
			const int i = reverse ? count - 1 - n : n;
			const idMaterial* material = declManager->FindMaterial( va( "%s/mtr_%03d", args.Argv( 2 ), i ) );
			const idSoundShader* sound = declManager->FindSound( va( "%s/snd_%03d", args.Argv( 2 ), i ) );
			gameLocal.Printf( "testDeclRemap: material '%s' index %d%s, sound '%s' index %d%s\n", material->GetName(), material->Index(), material->IsImplicit() ? " implicit" : "", sound->GetName(), sound->Index(), sound->IsImplicit() ? " implicit" : "" );
		}
		return;
	}
	if( idStr::Icmp( what, "decode" ) == 0 && args.Argc() >= 4 && common->IsClient() )
	{
		const declType_t type = declManager->GetDeclTypeFromName( args.Argv( 2 ) );
		if( type == DECL_MAX_TYPES )
		{
			gameLocal.Printf( "testDeclRemap: no decl type '%s'\n", args.Argv( 2 ) );
			return;
		}
		const int index = atoi( args.Argv( 3 ) );
		gameLocal.Printf( "testDeclRemap: %s %d decodes to %d\n", args.Argv( 2 ), index, gameLocal.ClientRemapDecl( type, index ) );
		return;
	}

	if( !common->IsServer() )
	{
		gameLocal.Printf( "testDeclRemap: %s runs on a multiplayer host\n", what );
		return;
	}
	idPlayer* player = gameLocal.GetLocalPlayer();
	if( player == NULL )
	{
		gameLocal.Printf( "testDeclRemap: no local player\n" );
		return;
	}

	if( idStr::Icmp( what, "sound" ) == 0 && args.Argc() >= 3 )
	{
		const idSoundShader* sound = declManager->FindSound( args.Argv( 2 ) );
		gameLocal.Printf( "testDeclRemap: sound '%s' index %d%s\n", sound->GetName(), sound->Index(), sound->IsImplicit() ? " implicit" : "" );
		gameLocal.mpGame.PlayGlobalSound( -1, SND_COUNT, sound->GetName() );
		player->StartSoundShader( sound, SND_CHANNEL_ANY, 0, true, NULL );
	}
	else if( idStr::Icmp( what, "rocket" ) == 0 )
	{
		// no speed and no gravity: it hangs where it is, replicated, until impact or its fuse removes it
		const idVec3 origin = player->GetPhysics()->GetOrigin() + idVec3( 0.0f, 0.0f, 120.0f );
		idDict spawnArgs;
		spawnArgs.Set( "classname", "projectile_rocket" );
		spawnArgs.SetVector( "origin", origin );
		spawnArgs.Set( "velocity", "0 0 0" );
		spawnArgs.Set( "angular_velocity", "0 0 0" );
		spawnArgs.Set( "gravity", "0" );
		spawnArgs.Set( "thrust", "0" );
		spawnArgs.Set( "fuse", "60" );
		idEntity* ent = NULL;
		if( !gameLocal.SpawnEntityDef( spawnArgs, &ent ) || ent == NULL || !ent->IsType( idProjectile::Type ) )
		{
			gameLocal.Printf( "testDeclRemap: no rocket\n" );
			return;
		}
		idProjectile* rocket = static_cast<idProjectile*>( ent );
		rocket->Launch( origin, idVec3( 0.0f, 0.0f, -1.0f ), vec3_origin );
		testDeclRemapRocket = rocket;
		gameLocal.Printf( "testDeclRemap: rocket %d at %s\n", rocket->entityNumber, origin.ToString() );
	}
	else if( idStr::Icmp( what, "impact" ) == 0 && args.Argc() >= 3 )
	{
		idProjectile* rocket = testDeclRemapRocket.GetEntity();
		if( rocket == NULL )
		{
			gameLocal.Printf( "testDeclRemap: no rocket, run testDeclRemap rocket first\n" );
			return;
		}
		trace_t collision;
		memset( &collision, 0, sizeof( collision ) );
		collision.endpos = rocket->GetPhysics()->GetOrigin();
		collision.endAxis = rocket->GetPhysics()->GetAxis();
		collision.c.point = collision.endpos;
		collision.c.normal.Set( 0.0f, 0.0f, 1.0f );
		collision.c.material = declManager->FindMaterial( args.Argv( 2 ) );
		collision.c.entityNum = ENTITYNUM_WORLD;
		gameLocal.Printf( "testDeclRemap: rocket %d hits '%s' index %d%s\n", rocket->entityNumber, collision.c.material->GetName(), collision.c.material->Index(), collision.c.material->IsImplicit() ? " implicit" : "" );
		rocket->Collide( collision, idVec3( 0.0f, 0.0f, -300.0f ) );
		testDeclRemapRocket = ( idProjectile* )NULL;
	}
	else if( idStr::Icmp( what, "fx" ) == 0 && args.Argc() >= 4 )
	{
		const int count = atoi( args.Argv( 3 ) );
		const idVec3 origin = player->GetPhysics()->GetOrigin();
		for( int i = 0; i < count; i++ )
		{
			idDict spawnArgs;
			spawnArgs.Set( "classname", "func_fx" );
			spawnArgs.Set( "fx", va( "%s/fx_%03d", args.Argv( 2 ), i ) );
			spawnArgs.SetVector( "origin", origin + idVec3( 0.0f, 0.0f, 8.0f + i ) );
			gameLocal.SpawnEntityDef( spawnArgs );
			const idDecl* fx = declManager->FindType( DECL_FX, spawnArgs.GetString( "fx" ) );
			gameLocal.ServerRemapDecl( -1, DECL_FX, fx->Index() );
		}
		gameLocal.Printf( "testDeclRemap: %d func_fx spawned\n", count );
	}
	else if( idStr::Icmp( what, "speakers" ) == 0 && args.Argc() >= 4 )
	{
		// networkSync, so each speaker exists on the clients too and they decode its sound event
		const int count = atoi( args.Argv( 3 ) );
		const idVec3 origin = player->GetPhysics()->GetOrigin();
		for( int i = 0; i < count; i++ )
		{
			idDict spawnArgs;
			spawnArgs.Set( "classname", "speaker" );
			spawnArgs.Set( "s_shader", va( "%s/snd_%03d", args.Argv( 2 ), i ) );
			spawnArgs.Set( "wait", "1" );
			spawnArgs.Set( "networkSync", "1" );
			spawnArgs.SetVector( "origin", origin + idVec3( 0.0f, 0.0f, 8.0f + i ) );
			gameLocal.SpawnEntityDef( spawnArgs );
		}
		gameLocal.Printf( "testDeclRemap: %d speakers spawned\n", count );
	}
	else
	{
		gameLocal.Printf( "usage: testDeclRemap make <prefix> <count> [reverse] | decode <type> <index> | sound <name> | rocket | impact <material> | fx <prefix> <count> | speakers <prefix> <count>\n" );
	}
}
//puncho end

//puncho start - gate tools for plans/phase1/02-FIXES.md track F3
/*
==================
Cmd_TestLaunch_f

testLaunch <projectileDef> <target> [dmgPower]: launches a projectile from the local player's eye at the target's centre,
spawned as idWeapon::Event_LaunchProjectiles spawns it on this peer: replicated on the host, net_skip_replication on a
client. So a client's BFG, rocket or soul cube runs its Launch, Think, Collide and Explode on the client, with no attack
button and no aim.
==================
*/
static void Cmd_TestLaunch_f( const idCmdArgs& args )
{
	idPlayer* player = gameLocal.GetLocalPlayer();
	if( player == NULL || args.Argc() < 3 )
	{
		gameLocal.Printf( "usage: testLaunch <projectileDef> <target> [dmgPower]\n" );
		return;
	}
	const idDict* def = gameLocal.FindEntityDefDict( args.Argv( 1 ), false );
	idEntity* target = gameLocal.FindEntity( args.Argv( 2 ) );
	if( def == NULL || target == NULL )
	{
		gameLocal.Printf( "testLaunch: no entityDef '%s' or no entity '%s'\n", args.Argv( 1 ), args.Argv( 2 ) );
		return;
	}
	const float dmgPower = ( args.Argc() > 3 ) ? atof( args.Argv( 3 ) ) : 1.0f;

	idDict projectileDict = *def;
	projectileDict.SetBool( "net_skip_replication", common->IsClient() );	// before the spawn: it picks the slot range
	idEntity* ent = NULL;
	gameLocal.SpawnEntityDef( projectileDict, &ent, false );
	if( ent == NULL || !ent->IsType( idProjectile::Type ) )
	{
		gameLocal.Printf( "testLaunch: '%s' is not an idProjectile\n", args.Argv( 1 ) );
		delete ent;
		return;
	}

	idProjectile* proj = static_cast< idProjectile* >( ent );
	const idVec3 eye = player->GetEyePosition();
	idVec3 dir = target->GetPhysics()->GetAbsBounds().GetCenter() - eye;
	dir.Normalize();
	const idVec3 start = eye + dir * 40.0f;	// outside the owner's bounds
	proj->Create( player, start, dir );
	proj->Launch( start, dir, vec3_origin, 0.0f, 1.0f, dmgPower );
	gameLocal.Printf( "testLaunch: %s launched %s, entity %d, skipReplication %d, at '%s' (entity %d)\n", common->IsClient() ? "client" : "host",
					  args.Argv( 1 ), ent->entityNumber, ent->fl.skipReplication ? 1 : 0, target->name.c_str(), target->entityNumber );
}

/*
==================
Cmd_TestUnspectate_f

testUnspectate: on the host, every player wants to play, as pressing attack while spectating asks, so warmup can end
==================
*/
static void Cmd_TestUnspectate_f( const idCmdArgs& args )
{
	if( !common->IsServer() )
	{
		gameLocal.Printf( "testUnspectate: runs on a multiplayer host\n" );
		return;
	}
	int players = 0;
	for( int i = 0; i < gameLocal.numClients; i++ )
	{
		idEntity* ent = gameLocal.entities[ i ];
		if( ent == NULL || !ent->IsType( idPlayer::Type ) )
		{
			continue;
		}
		idPlayer* player = static_cast< idPlayer* >( ent );
		if( player->wantSpectate )
		{
			player->forceRespawn = true;
		}
		player->wantSpectate = false;
		players++;
	}
	gameLocal.Printf( "testUnspectate: %d players want to play\n", players );
}

/*
==================
Cmd_TestEntState_f

testEntState <entity>...: this peer's view of each entity, so a host's and a client's lines can be compared
==================
*/
static void Cmd_TestEntState_f( const idCmdArgs& args )
{
	const char* peer = common->IsClient() ? "client" : "host";
	for( int i = 1; i < args.Argc(); i++ )
	{
		idEntity* ent = gameLocal.FindEntity( args.Argv( i ) );
		if( ent == NULL )
		{
			gameLocal.Printf( "testEntState %s %s: gone\n", peer, args.Argv( i ) );
			continue;
		}
		const renderEntity_t* re = ent->GetRenderEntity();
		gameLocal.Printf( "testEntState %s %s: entity %d %s, health %d, takedamage %d, hidden %d, contents %d, networkSync %d, model %s, mode %g, timeOffset %g\n",
						  peer, ent->name.c_str(), ent->entityNumber, ent->GetClassname(), ent->health, ent->fl.takedamage ? 1 : 0, ent->IsHidden() ? 1 : 0,
						  ent->GetPhysics()->GetContents(), ent->fl.networkSync ? 1 : 0, ( re->hModel != NULL ) ? re->hModel->Name() : "none",
						  re->shaderParms[ SHADERPARM_MODE ], re->shaderParms[ SHADERPARM_TIMEOFFSET ] );
	}
}

/*
==================
Cmd_TestHitClaim_f

testHitClaim <count> <victim> <joint> [damageDef]: a client sends <count> hit claims (32 at most, inside the reliable
queue) against <victim> at <joint>, a joint name or number, with the local player as the attacker, in the layout
idPlayer::Damage sends. A joint out of range is a bad claim; a real joint in the attacker's view is a legitimate one.
==================
*/
static void Cmd_TestHitClaim_f( const idCmdArgs& args )
{
	idPlayer* player = gameLocal.GetLocalPlayer();
	if( !common->IsClient() || player == NULL || args.Argc() < 4 )
	{
		gameLocal.Printf( "usage: testHitClaim <count> <victim> <joint> [damageDef], on a client\n" );
		return;
	}
	const int count = idMath::ClampInt( 1, 32, atoi( args.Argv( 1 ) ) );
	idEntity* victim = gameLocal.FindEntity( args.Argv( 2 ) );
	const idDeclEntityDef* damageDef = gameLocal.FindEntityDef( ( args.Argc() > 4 ) ? args.Argv( 4 ) : "damage_bullet_machinegun_mp", false );
	if( victim == NULL || damageDef == NULL )
	{
		gameLocal.Printf( "testHitClaim: no entity '%s' or no damage def\n", args.Argv( 2 ) );
		return;
	}
	const jointHandle_t joint = ( victim->GetAnimator() != NULL ) ? victim->GetAnimator()->GetJointHandle( args.Argv( 3 ) ) : INVALID_JOINT;
	const int location = ( joint != INVALID_JOINT ) ? joint : atoi( args.Argv( 3 ) );
	idVec3 dir = victim->GetPhysics()->GetAbsBounds().GetCenter() - player->GetEyePosition();
	dir.Normalize();

	idLobbyBase& lobby = session->GetActingGameStateLobbyBase();
	for( int i = 0; i < count; i++ )
	{
		byte		msgBuffer[ MAX_GAME_MESSAGE_SIZE ];
		idBitMsg	msg;
		msg.InitWrite( msgBuffer, sizeof( msgBuffer ) );
		msg.BeginWriting();
		msg.WriteShort( player->entityNumber );
		msg.WriteShort( victim->entityNumber );
		msg.WriteVectorFloat( dir );
		msg.WriteLong( damageDef->Index() );
		msg.WriteFloat( 1.0f );
		msg.WriteLong( location );
		lobby.SendReliableToHost( GAME_RELIABLE_MESSAGE_CLIENT_HITSCAN_HIT, msg );
	}
	gameLocal.Printf( "testHitClaim: %d claims sent against '%s' (entity %d), joint %d, damage def %s\n", count, victim->name.c_str(), victim->entityNumber, location, damageDef->GetName() );
}

/*
==================
Cmd_TestSnapSubmit_f

testSnapSubmit <objects> <bytes>: one synthetic snapshot of <objects> objects of <bytes> random bytes each, through the
host's submit path (idSnapshotProcessor::SubmitPendingSnap with idLobby's 128 KB of object job memory) against an empty
base, so every object is new. A full table is MAX_GENTITIES entity objects and the game's fixed ones: 16403 today.
A cap reached ends in idLib::Error; reaching the PASS line means the jobs took it.
==================
*/
static void Cmd_TestSnapSubmit_f( const idCmdArgs& args )
{
	if( args.Argc() < 3 )
	{
		gameLocal.Printf( "usage: testSnapSubmit <objects> <bytes>\n" );
		return;
	}
	const int numObjects = idMath::ClampInt( 1, MAX_NET_ENTITIES + MAX_NET_FIXED_OBJECTS, atoi( args.Argv( 1 ) ) );
	const int objectBytes = idMath::ClampInt( 1, 4096, atoi( args.Argv( 2 ) ) );
	const int objMemorySize = 128 * 1024;	// idLobby::SNAP_OBJ_JOB_MEMORY

	idSnapShot ss;
	idTempArray< byte > data( objectBytes );
	idRandom random( 1 );
	for( int i = 0; i < numObjects; i++ )
	{
		for( int b = 0; b < objectBytes; b++ )
		{
			data[ b ] = ( byte )( 1 + random.RandomInt( 254 ) );
		}
		ss.S_AddObject( i, ~0U, data.Ptr(), objectBytes );
	}

	idSnapshotProcessor* proc = new( TAG_NETWORKING ) idSnapshotProcessor();
	uint8* objMemory = ( uint8* )Mem_Alloc16( objMemorySize, TAG_NETWORKING );
	lzwCompressionData_t* lzwData = ( lzwCompressionData_t* )Mem_Alloc( sizeof( lzwCompressionData_t ), TAG_NETWORKING );
	gameLocal.Printf( "testSnapSubmit: submitting %d objects of %d bytes\n", numObjects, objectBytes );
	proc->TrySetPendingSnapshot( ss );
	proc->SubmitPendingSnap( 1, objMemory, objMemorySize, lzwData );
	gameLocal.Printf( "testSnapSubmit: PASS: %d objects of %d bytes submitted, delta ready %d\n", numObjects, objectBytes, proc->PendingSnapReadyToSend() ? 1 : 0 );
	Mem_Free( lzwData );
	Mem_Free16( objMemory );
	delete proc;
}
//puncho end

/*
==================
Cmd_RecordViewNotes_f
==================
*/
static void Cmd_RecordViewNotes_f( const idCmdArgs& args )
{
	idPlayer* player;
	idVec3 origin;
	idMat3 axis;

	if( args.Argc() <= 3 )
	{
		return;
	}

	player = gameLocal.GetLocalPlayer();
	if( !player )
	{
		return;
	}

	player->GetViewPos( origin, axis );

	// Argv(1) = filename for map (viewnotes/mapname/person)
	// Argv(2) = note number (person0001)
	// Argv(3) = comments

	idStr str = args.Argv( 1 );
	str.SetFileExtension( ".txt" );

	idFile* file = fileSystem->OpenFileAppend( str );

	if( file )
	{
		file->WriteFloatString( "\"view\"\t( %s )\t( %s )\r\n", origin.ToString(), axis.ToString() );
		file->WriteFloatString( "\"comments\"\t\"%s: %s\"\r\n\r\n", args.Argv( 2 ), args.Argv( 3 ) );
		fileSystem->CloseFile( file );
	}

	idStr viewComments = args.Argv( 1 );
	viewComments.StripLeading( "viewnotes/" );
	viewComments += " -- Loc: ";
	viewComments += origin.ToString();
	viewComments += "\n";
	viewComments += args.Argv( 3 );

	// TODO_SPARTY: removed old hud need to find a way of doing this with the new hud
	//player->hud->SetStateString( "viewcomments", viewComments );
	//player->hud->HandleNamedEvent( "showViewComments" );
}

/*
==================
Cmd_CloseViewNotes_f
==================
*/
static void Cmd_CloseViewNotes_f( const idCmdArgs& args )
{
	idPlayer* player = gameLocal.GetLocalPlayer();

	if( !player )
	{
		return;
	}

	// TODO_SPARTY: removed old hud need to find a way of doing this with the new hud
	//player->hud->SetStateString( "viewcomments", "" );
	//player->hud->HandleNamedEvent( "hideViewComments" );
}

/*
==================
Cmd_ShowViewNotes_f
==================
*/
static void Cmd_ShowViewNotes_f( const idCmdArgs& args )
{
	static idLexer parser( LEXFL_ALLOWPATHNAMES | LEXFL_NOSTRINGESCAPECHARS | LEXFL_NOSTRINGCONCAT | LEXFL_NOFATALERRORS );
	idToken	token;
	idPlayer* player;
	idVec3 origin;
	idMat3 axis;

	player = gameLocal.GetLocalPlayer();

	if( !player )
	{
		return;
	}

	if( !parser.IsLoaded() )
	{
		idStr str = "viewnotes/";
		str += gameLocal.GetMapName();
		str.StripFileExtension();
		str += "/";
		if( args.Argc() > 1 )
		{
			str += args.Argv( 1 );
		}
		else
		{
			str += "comments";
		}
		str.SetFileExtension( ".txt" );
		if( !parser.LoadFile( str ) )
		{
			gameLocal.Printf( "No view notes for %s\n", gameLocal.GetMapName() );
			return;
		}
	}

	if( parser.ExpectTokenString( "view" ) && parser.Parse1DMatrix( 3, origin.ToFloatPtr() ) &&
			parser.Parse1DMatrix( 9, axis.ToFloatPtr() ) && parser.ExpectTokenString( "comments" ) && parser.ReadToken( &token ) )
	{

		// TODO_SPARTY: removed old hud need to find a way of doing this with the new hud
		//player->hud->SetStateString( "viewcomments", token );
		//player->hud->HandleNamedEvent( "showViewComments" );
		player->Teleport( origin, axis.ToAngles(), NULL );
	}
	else
	{
		parser.FreeSource();
		// TODO_SPARTY: removed old hud need to find a way of doing this with the new hud
		//player->hud->HandleNamedEvent( "hideViewComments" );
		return;
	}
}

/*
=================
FindEntityGUIs

helper function for Cmd_NextGUI_f.  Checks the passed entity to determine if it
has any valid gui surfaces.
=================
*/
bool FindEntityGUIs( idEntity* ent, const modelSurface_t** surfaces,  int maxSurfs, int& guiSurfaces )
{
	renderEntity_t*			renderEnt;
	idRenderModel*			renderModel;
	const modelSurface_t*	surf;
	const idMaterial*		shader;
	int						i;

	assert( surfaces != NULL );
	assert( ent != NULL );

	memset( surfaces, 0x00, sizeof( modelSurface_t* ) * maxSurfs );
	guiSurfaces = 0;

	renderEnt  = ent->GetRenderEntity();
	renderModel = renderEnt->hModel;
	if( renderModel == NULL )
	{
		return false;
	}

	for( i = 0; i < renderModel->NumSurfaces(); i++ )
	{
		surf = renderModel->Surface( i );
		if( surf == NULL )
		{
			continue;
		}
		shader = surf->shader;
		if( shader == NULL )
		{
			continue;
		}
		if( shader->GetEntityGui() > 0 )
		{
			surfaces[ guiSurfaces++ ] = surf;
		}
	}

	return ( guiSurfaces != 0 );
}

/*
=================
Cmd_NextGUI_f
=================
*/
void Cmd_NextGUI_f( const idCmdArgs& args )
{
	idVec3					origin;
	idAngles				angles;
	idPlayer*				player;
	idEntity*				ent;
	int						guiSurfaces;
	bool					newEnt;
	renderEntity_t*			renderEnt;
	int						surfIndex;
	srfTriangles_t*			geom;
	idVec3					normal;
	idVec3					center;
	const modelSurface_t*	surfaces[ MAX_RENDERENTITY_GUI ];

	player = gameLocal.GetLocalPlayer();
	if( !player || !gameLocal.CheatsOk() )
	{
		return;
	}

	if( args.Argc() != 1 )
	{
		gameLocal.Printf( "usage: nextgui\n" );
		return;
	}

	// start at the last entity
	ent = gameLocal.lastGUIEnt.GetEntity();

	// see if we have any gui surfaces left to go to on the current entity.
	guiSurfaces = 0;
	newEnt = false;
	if( ent == NULL )
	{
		newEnt = true;
	}
	else if( FindEntityGUIs( ent, surfaces, MAX_RENDERENTITY_GUI, guiSurfaces ) == true )
	{
		if( gameLocal.lastGUI >= guiSurfaces )
		{
			newEnt = true;
		}
	}
	else
	{
		// no actual gui surfaces on this ent, so skip it
		newEnt = true;
	}

	if( newEnt == true )
	{
		// go ahead and skip to the next entity with a gui...
		if( ent == NULL )
		{
			ent = gameLocal.spawnedEntities.Next();
		}
		else
		{
			ent = ent->spawnNode.Next();
		}

		for( ; ent != NULL; ent = ent->spawnNode.Next() )
		{
			if( ent->spawnArgs.GetString( "gui", NULL ) != NULL )
			{
				break;
			}

			if( ent->spawnArgs.GetString( "gui2", NULL ) != NULL )
			{
				break;
			}

			if( ent->spawnArgs.GetString( "gui3", NULL ) != NULL )
			{
				break;
			}

			// try the next entity
			gameLocal.lastGUIEnt = ent;
		}

		gameLocal.lastGUIEnt = ent;
		gameLocal.lastGUI = 0;

		if( !ent )
		{
			gameLocal.Printf( "No more gui entities. Starting over...\n" );
			return;
		}
	}

	if( FindEntityGUIs( ent, surfaces, MAX_RENDERENTITY_GUI, guiSurfaces ) == false )
	{
		gameLocal.Printf( "Entity \"%s\" has gui properties but no gui surfaces.\n", ent->name.c_str() );
	}

	if( guiSurfaces == 0 )
	{
		gameLocal.Printf( "Entity \"%s\" has gui properties but no gui surfaces!\n", ent->name.c_str() );
		return;
	}

	gameLocal.Printf( "Teleporting to gui entity \"%s\", gui #%d.\n" , ent->name.c_str(), gameLocal.lastGUI );

	renderEnt = ent->GetRenderEntity();
	surfIndex = gameLocal.lastGUI++;
	geom = surfaces[ surfIndex ]->geometry;
	if( geom == NULL )
	{
		gameLocal.Printf( "Entity \"%s\" has gui surface %d without geometry!\n", ent->name.c_str(), surfIndex );
		return;
	}

	const idVec3& v0 = geom->verts[geom->indexes[0]].xyz;
	const idVec3& v1 = geom->verts[geom->indexes[1]].xyz;
	const idVec3& v2 = geom->verts[geom->indexes[2]].xyz;

	const idPlane plane( v0, v1, v2 );

	normal = plane.Normal() * renderEnt->axis;
	center = geom->bounds.GetCenter() * renderEnt->axis + renderEnt->origin;

	origin = center + ( normal * 32.0f );
	origin.z -= player->EyeHeight();
	normal *= -1.0f;
	angles = normal.ToAngles();

	//	make sure the player is in noclip
	player->noclip = true;
	player->Teleport( origin, angles, NULL );
}

void Cmd_SetActorState_f( const idCmdArgs& args )
{

	if( args.Argc() != 3 )
	{
		common->Printf( "usage: setActorState <entity name> <state>\n" );
		return;
	}

	idEntity* ent;
	ent = gameLocal.FindEntity( args.Argv( 1 ) );
	if( !ent )
	{
		gameLocal.Printf( "entity not found\n" );
		return;
	}


	if( !ent->IsType( idActor::Type ) )
	{
		gameLocal.Printf( "entity not an actor\n" );
		return;
	}

	idActor* actor = ( idActor* )ent;
	actor->PostEventMS( &AI_SetState, 0, args.Argv( 2 ) );
}

#if 0
// not used
static void ArgCompletion_DefFile( const idCmdArgs& args, void( *callback )( const char* s ) )
{
	cmdSystem->ArgCompletion_FolderExtension( args, callback, "def/", true, ".def", NULL );
}
#endif

/*
===============
Cmd_TestId_f
outputs a string from the string table for the specified id
===============
*/
void Cmd_TestId_f( const idCmdArgs& args )
{
	idStr	id;
	int		i;
	if( args.Argc() == 1 )
	{
		common->Printf( "usage: testid <string id>\n" );
		return;
	}

	for( i = 1; i < args.Argc(); i++ )
	{
		id += args.Argv( i );
	}
	if( idStr::Cmpn( id, STRTABLE_ID, STRTABLE_ID_LENGTH ) != 0 )
	{
		id = STRTABLE_ID + id;
	}
	gameLocal.mpGame.AddChatLine( idLocalization::GetString( id ), "<nothing>", "<nothing>", "<nothing>" );
}


// RB begin
void Cmd_EditLights_f( const idCmdArgs& args )
{
	// EA-4: the editor's own state decides the direction, not g_editEntityMode, which editAFs or a
	// hand-set value can change while the light editor is open (the toggle must always close it)
	if( !ImGuiTools::LightEditorIsOpen() )
	{
		g_editEntityMode.SetInteger( 1 );

		// turn off com_smp multithreading so we can load and check light textures on main thread
		com_editors |= EDITOR_LIGHT;

		// no light selected, mouse kept by the game; le_startEasy 1 opens the easy overlay, else a pick opens the panel
		ImGuiTools::LightEditorOpen();
	}
	else
	{
		// S4 / D30 / D48: revert and deselect, then close; leaving never writes the .map. The latch
		// reset must come before Close, which may run when ImGui is already gone
		ImGuiTools::LightEditorDeselect();
		ImGuiHook::ResetInputLatches();
		ImGuiTools::LightEditorClose();

		g_editEntityMode.SetInteger( 0 );

		com_editors &= ~EDITOR_LIGHT;

		// turn off light debug drawing in the render backend
		r_singleLight.SetInteger( -1 );
		r_showLights.SetInteger( 0 );
	}

	// put player into fly mode
	Cmd_Noclip_f( args );
}
// RB end

// SP begin
void Cmd_ShowAfEditor_f( const idCmdArgs& args )
{
	if( g_editEntityMode.GetInteger() != 3 )
	{
		g_editEntityMode.SetInteger( 3 );
		com_editors |= EDITOR_AF;
		ImGuiTools::AfEditor::Instance().Init();
		ImGuiTools::AfEditor::Instance().ShowIt( true );
	}
	else
	{
		g_editEntityMode.SetInteger( 0 );
		com_editors &= ~EDITOR_AF;
		ImGuiTools::AfEditor::Instance().ShowIt( false );
	}
}
// SP end

/*
=================
idGameLocal::InitConsoleCommands

Let the system know about all of our commands
so it can perform tab completion
=================
*/
static void Cmd_ListMap_f( const idCmdArgs& args )
{
	if( gameRenderWorld == NULL )
	{
		gameLocal.Printf( "listMap: no map loaded\n" );
		return;
	}
	char rows[6][256];
	bool high[6];
	FormatMapRows( rows, high );
	for( int i = 0; i < 6; i++ )
	{
		gameLocal.Printf( "%s\n", rows[i] );
	}
	unsigned int digest;
	CRC32_InitChecksum( digest );
	int open = 0;
	for( int i = 1; i <= gameRenderWorld->NumPortals(); i++ )
	{
		const int state = gameRenderWorld->GetPortalState( i );
		CRC32_UpdateChecksum( digest, &state, sizeof( state ) );
		if( state == 0 )
		{
			open++;
		}
	}
	CRC32_FinishChecksum( digest );
	gameLocal.Printf( "portals %d open %d digest 0x%08x\n", gameRenderWorld->NumPortals(), open, digest );
	int slots = 0;
	for( int i = 0; i < idProjectile::MAX_SIMULATED_PROJECTILES; i++ )
	{
		if( idProjectile::projectilesToSimulate[i].projectile != NULL )
		{
			slots++;
		}
	}
	gameLocal.Printf( "catchUp slots %d/64 dropped %d\n", slots, idProjectile::simulationDropped );
	// The command may allocate; the per-tic and per-frame samplers never do.
	struct classCount_t
	{
		const char* name;
		int count;
	};
	classCount_t* classes = ( classCount_t* )Mem_ClearedAlloc( MAX_GENTITIES * sizeof( classCount_t ), TAG_GAME );
	int count = 0;
	for( int i = 0; i < MAX_GENTITIES; i++ )
	{
		idEntity* ent = gameLocal.entities[i];
		if( ent == NULL )
		{
			continue;
		}
		const char* name = ent->GetType()->classname;
		int j;
		for( j = 0; j < count && idStr::Cmp( classes[j].name, name ); j++ )
		{
		}
		if( j == count )
		{
			classes[count++].name = name;
		}
		classes[j].count++;
	}
	for( int i = 0; i < Min( 40, count ); i++ )
	{
		int best = i;
		for( int j = i + 1; j < count; j++ )
		{
			if( classes[j].count > classes[best].count || ( classes[j].count == classes[best].count && idStr::Cmp( classes[j].name, classes[best].name ) < 0 ) )
			{
				best = j;
			}
		}
		const classCount_t temp = classes[i];
		classes[i] = classes[best];
		classes[best] = temp;
		gameLocal.Printf( "MAP class %d %s\n", classes[i].count, classes[i].name );
	}
	Mem_Free( classes );
	renderSystem->ListMapLightGrids();
}

static void Cmd_WorldTime_f( const idCmdArgs& args )
{
	gameLocal.Printf( "worldTime %lld base %lld map %s time %d\n", ( long long )gameLocal.GetWorldTime(), ( long long )gameLocal.worldTimeBase, gameLocal.GetMapName(), gameLocal.time );
}

#if !defined( ID_RETAIL )	//puncho - dev guard (P2 review): the collision and clip self-tests are test scaffolding
static void Cmd_CMSelfTest_f( const idCmdArgs& args )
{
	if( !gameLocal.CheatsOk() )
	{
		return;
	}
	collisionModelManager->SelfTest( args.Argc() > 1 ? atoi( args.Argv( 1 ) ) : 10000, args.Argc() > 2 ? atoi( args.Argv( 2 ) ) : 1 );
}

static void Cmd_ClipSelfTest_f( const idCmdArgs& args )
{
	if( !gameLocal.CheatsOk() )
	{
		return;
	}
	gameLocal.clip.SelfTest( args.Argc() > 1 ? atoi( args.Argv( 1 ) ) : 10000, args.Argc() > 2 ? atoi( args.Argv( 2 ) ) : 1 );
}
#endif

static void Cmd_CheckPVSAreas_f( const idCmdArgs& args )
{
	int count = 0;
	for( int i = 0; i < MAX_GENTITIES; i++ )
	{
		const idEntity* entity = gameLocal.entities[i];
		if( entity == NULL || entity->GetPhysics() == NULL )
		{
			continue;
		}
		int areas[5];
		const int touched = gameRenderWorld->BoundsInAreas( entity->GetPhysics()->GetAbsBounds(), areas, 5 );
		if( touched > 4 )
		{
			gameLocal.Printf( "PVS entity %d %s touches more than four areas\n", i, entity->name.c_str() );
			count++;
		}
	}
	gameLocal.Printf( "checkPVSAreas: %d entities\n", count );
}

static void Cmd_ListDormancy_f( const idCmdArgs& args )
{
	for( int i = 0; i < MAX_GENTITIES; i++ )
	{
		const idEntity* entity = gameLocal.entities[i];
		if( entity != NULL && entity->dormantRadius > 0.0f )
		{
			gameLocal.Printf( "dormant entity %d %s radius %.0f band %.0f asleep %d requested %d deferred %d\n", i, entity->name.c_str(),
				entity->dormantRadius, entity->dormantHysteresis, entity->fl.isDormant, entity->dormantWakeRequested, entity->dormantWakeDeferredAt );
		}
	}
}

static void Cmd_RunScriptFunction_f( const idCmdArgs& args )
{
	if( args.Argc() != 2 || !gameLocal.CheatsOk() || common->IsClient() || gameLocal.GameState() != GAMESTATE_ACTIVE || gameLocal.IsMapTransitionActive() )
	{
		gameLocal.Printf( "runScriptFunction requires an idle server map and one function name\n" );
		return;
	}
	const function_t* function = gameLocal.program.FindFunction( args.Argv( 1 ) );
	if( function == NULL || function->eventdef != NULL || function->type == NULL || function->type->Type() != ev_function || function->type->ReturnType() == NULL || function->type->NumParameters() != 0 || function->type->ReturnType()->Type() != ev_void || function->def == NULL || function->def->scope == NULL || function->def->scope->Type() != ev_namespace )
	{
		gameLocal.Printf( "Unknown function or unsupported signature: %s (requires global void function with no arguments)\n", args.Argv( 1 ) );
		return;
	}
	// Invoke precompiled diagnostics without modifying the program image or
	// adding the synthetic console filename that cannot be reopened on restore.
	idThread* thread = new idThread( function );
	thread->Start(); // Ordinary script-thread ownership, including waits and completion.
}

#if !defined( ID_RETAIL )	//puncho - dev guard (P2b.1.1a): the record fixture is test scaffolding
struct recordTestRow_t
{
	int ordinal, health;
	unsigned int poseCRC;
	bool present, hidden;
	idVec3 origin;
};
static idList<recordTestRow_t, TAG_GAME> recordTestBaseline;
static idStr recordTestMap;
static int recordTestFailures;

//puncho - P2b.1.2: recordTest fuzz's draws from idRandom( seed ). Its RandomInt is the low bits of a power-of-two LCG,
// whose lowest bits alternate, which would correlate the record, the kind and the offset of a mutant: the state steps
// as idRandom's, in unsigned arithmetic, and is mixed before it is reduced to the range
static int RecordFuzzRandom( idRandom& rng, int limit )
{
	uint32 value = ( uint32 )rng.GetSeed() * 69069U + 1U;
	rng.SetSeed( ( int )value );
	value ^= value >> 16;
	value *= 0x7feb352dU;
	value ^= value >> 15;
	value *= 0x846ca68bU;
	value ^= value >> 16;
	return limit > 0 ? ( int )( value % ( uint32 )limit ) : 0;
}

static void Cmd_RecordTest_f( const idCmdArgs& args )
{
	if( !gameLocal.CheatsOk() || args.Argc() < 2 )
	{
		return;
	}
	if( idStr::Icmp( args.Argv( 1 ), "lifetime" ) == 0 && args.Argc() == 4 && idStr::Icmpn( gameLocal.GetMapName(), "maps/_test/mg/", 14 ) == 0 )
	{
		int changed = 0;
		const bool corpses = idStr::Icmp( args.Argv( 2 ), "corpses" ) == 0;
		for( int i = MAX_CLIENTS; i < MAX_GENTITIES; i++ )
		{
			idEntity* ent = gameLocal.entities[i];
			if( ent != NULL && ( corpses ? ent->IsType( idCorpse::Type ) : ent->name == args.Argv( 2 ) ) && !ent->spawnArgs.GetBool( "persist_nocap" ) )
			{
				ent->spawnArgs.SetInt( "persist_lifetime", atoi( args.Argv( 3 ) ) );
				changed++;
			}
		}
		gameLocal.Printf( "RECORD_TEST %s lifetime targets=%d\n", changed > 0 ? "PASS" : "FAIL", changed );
		return;
	}
	if( idStr::Icmp( args.Argv( 1 ), "codec" ) == 0 )
	{
		worldStore.TestCodec();
		return;
	}
	//puncho - P2b.1.4: recordTest recipe, the recipe codec against SetMatchGameData (idMapGraphDirector::TestRecipe)
	if( idStr::Icmp( args.Argv( 1 ), "recipe" ) == 0 )
	{
		mapGraphDirector.TestRecipe( gameLocal.GetMapName() );
		return;
	}
	//puncho start - P2b.1.2: recordTest fuzz <n> <seed>. The current map committed (PrepareSave), then n mutants of the
	// store's records from idRandom( seed ), each through the reader and the check ApplyCurrentMap runs before any apply
	// (RecordScanChunk, the record's identity, each entry's check, an authored entry's sections against its entity).
	// Kinds: a bit flipped, a byte overwritten, a truncation, the version, numEntries, a mask bit past the list, a
	// reserved flag bit, a header cellId on the reserved layer that is neither 0 nor 1, a section byte count past the
	// chunk, a MIGRATED flag that disagrees with Pose.cellId, a section id twice in an entry. VERSION/CELL/BYTES must
	// fail scanning; MASK/FLAGS/MIGRATED/SECTION_ID must fail scanning or their targeted entry's check.
	// ENTRIES and the untargeted kinds may survive as long as nothing crashes. Each unmutated record must
	// round-trip byte for byte (read, then written again).
	if( idStr::Icmp( args.Argv( 1 ), "fuzz" ) == 0 && args.Argc() == 4 )
	{
		const int count = Max( 0, atoi( args.Argv( 2 ) ) );
		const int seed = atoi( args.Argv( 3 ) );
		worldStore.PrepareSave();
		const uint64 nextIdentity = worldStore.GetNextCreatedId();
		bool corpusValid = true;
		bool roundtrip = worldStore.NumMapRecords() > 0;
		for( int i = 0; i < worldStore.NumMapRecords(); i++ )
		{
			const idList<byte, TAG_GAME>& bytes = worldStore.MapRecordBytes( i );
			mrecHeader_t original;
			idList<mrecEntry_t, TAG_GAME> originalEntries;
			idStr originalReason;
			if( !RecordScanChunk( bytes.Ptr(), bytes.Num(), nextIdentity, original, originalEntries, originalReason ) )
			{
				corpusValid = false;
				gameLocal.Warning( "recordTest fuzz: source %d: %s", i, originalReason.c_str() );
			}
			for( int e = 0; e < originalEntries.Num(); e++ )
			{
				if( !originalEntries[e].valid )
				{
					corpusValid = false;
					gameLocal.Warning( "recordTest fuzz: source %d entry %d: %s", i, e, originalEntries[e].reason.c_str() );
				}
			}
			idFile_Memory rewritten;
			roundtrip &= RecordRewriteChunk( bytes.Ptr(), bytes.Num(), nextIdentity, rewritten ) && rewritten.Length() == bytes.Num() && memcmp( rewritten.GetDataPtr(), bytes.Ptr(), bytes.Num() ) == 0;
		}
		enum { FUZZ_FLIP, FUZZ_BYTE, FUZZ_TRUNCATE, FUZZ_VERSION, FUZZ_ENTRIES, FUZZ_MASK, FUZZ_FLAGS, FUZZ_CELL, FUZZ_BYTES, FUZZ_MIGRATED, FUZZ_SECTION_ID, FUZZ_KINDS };
		idRandom rng( seed );
		int dropped = 0, accepted = 0, missed = 0, skipped = 0;
		int kinds[FUZZ_KINDS], changed[FUZZ_KINDS];
		memset( changed, 0, sizeof( changed ) );
		memset( kinds, 0, sizeof( kinds ) );
		mrecHeader_t header;
		idList<mrecEntry_t, TAG_GAME> entries;
		idStr reason;
		idRecordIn rec;
		for( int m = 0; m < count && corpusValid && worldStore.NumMapRecords() > 0; m++ )
		{
			const idList<byte, TAG_GAME>& source = worldStore.MapRecordBytes( RecordFuzzRandom( rng, worldStore.NumMapRecords() ) );
			idList<byte, TAG_GAME> bytes = source;
			const int length = bytes.Num();
			const int kind = RecordFuzzRandom( rng, FUZZ_KINDS );
			int offset = RecordFuzzRandom( rng, length );
			// Locate targets only in a successfully scanned source; a malformed corpus must not become test UB.
			if( !RecordScanChunk( source.Ptr(), length, nextIdentity, header, entries, reason ) )
			{
				corpusValid = false;
				break;
			}
			const int worldBytes = header.world.Length(), unitBytes = header.unit.Length();
			int target = entries.Num() > 0 ? RecordFuzzRandom( rng, entries.Num() ) : -1;
			int newLength = length;
			bool mustDrop = kind > FUZZ_TRUNCATE;
			switch( kind )
			{
				case FUZZ_FLIP:
					bytes[offset] ^= ( byte )( 1 << RecordFuzzRandom( rng, 8 ) );
					break;
				case FUZZ_BYTE:
					bytes[offset] ^= ( byte )( 1 + RecordFuzzRandom( rng, 255 ) );
					break;
				case FUZZ_TRUNCATE:
					newLength = offset;
					break;
				case FUZZ_VERSION:
					offset = 0;
					bytes[0] = ( byte )( RecordFuzzRandom( rng, 2 ) ? 2 : 4 + RecordFuzzRandom( rng, 200 ) );
					bytes[1] = 0;
					break;
				case FUZZ_ENTRIES:
				{
					// numEntries, the header's last field: one more or one fewer may still frame; the count is not a check
					const int at = MREC_HEADER_NUMENTRIES_OFFSET + worldBytes + unitBytes;
					offset = at;
					const uint32 value = ( uint32 )( header.numEntries + ( RecordFuzzRandom( rng, 2 ) ? 1 : 0xffffffffu ) );
					for( int b = 0; b < 4; b++ )
					{
						bytes[at + b] = ( byte )( value >> ( b * 8 ) );
					}
					mustDrop = false;
					break;
				}
				case FUZZ_MASK:
				case FUZZ_BYTES:
				{
					// Choose among actual sections, including later class levels and created entries; REMOVED has none.
					idList<int, TAG_GAME> sectionEntries, sectionOffsets;
					for( int e = 0; e < entries.Num(); e++ )
					{
						int at = entries[e].sectionsOffset;
						for( int s = 0; s < entries[e].numSections; s++ )
						{
							const int fields = RecordSectionFields( bytes[at] );
							if( kind == FUZZ_MASK ? ( fields > 0 && fields < 64 ) : length - at - RECORD_SECTION_HEADER_BYTES < 65535 )
							{
								sectionEntries.Append( e );
								sectionOffsets.Append( at );
							}
							at += RECORD_SECTION_HEADER_BYTES + bytes[at + 10] + ( bytes[at + 11] << 8 );
						}
					}
					if( sectionOffsets.Num() == 0 )
					{
						mustDrop = false;
						skipped++;
						break;
					}
					const int section = RecordFuzzRandom( rng, sectionOffsets.Num() );
					target = sectionEntries[section];
					const int at = sectionOffsets[section];
					if( kind == FUZZ_MASK )
					{
						const int fields = RecordSectionFields( bytes[at] );
						const int bit = fields + RecordFuzzRandom( rng, 64 - fields );
						offset = at + 2 + bit / 8;
						bytes[offset] |= ( byte )( 1 << ( bit & 7 ) );
					}
					else
					{
						// Smallest representable size that certainly exceeds the remaining chunk, without clamping.
						const int value = length - at - RECORD_SECTION_HEADER_BYTES + 1;
						offset = at + 10;
						bytes[at + 10] = ( byte )value;
						bytes[at + 11] = ( byte )( value >> 8 );
					}
					break;
				}
				case FUZZ_FLAGS:
				{
					if( target < 0 )
					{
						mustDrop = false;
						skipped++;
						break;
					}
					offset = entries[target].sectionsOffset - 2;
					bytes[offset] |= ( byte )( 1 << ( 2 + RecordFuzzRandom( rng, 6 ) ) );
					break;
				}
				case FUZZ_MIGRATED:
				{
					// MIGRATED on an entry whose Pose.cellId is the record's (or that has no Pose, or is created)
					if( target < 0 )
					{
						mustDrop = false;
						skipped++;
						break;
					}
					offset = entries[target].sectionsOffset - 2;
					bytes[offset] ^= ( byte )RECORD_ENTRY_MIGRATED;
					break;
				}
				case FUZZ_SECTION_ID:
				{
					// an earlier section's id copied into a later one of the same entry: the "section twice" rule
					idList<int, TAG_GAME> multi;
					for( int e = 0; e < entries.Num(); e++ )
					{
						if( entries[e].numSections >= 2 )
						{
							multi.Append( e );
						}
					}
					if( multi.Num() == 0 )
					{
						mustDrop = false;
						skipped++;
						break;
					}
					target = multi[RecordFuzzRandom( rng, multi.Num() )];
					idList<int, TAG_GAME> headers;
					int at = entries[target].sectionsOffset;
					for( int s = 0; s < entries[target].numSections; s++ )
					{
						headers.Append( at );
						at += RECORD_SECTION_HEADER_BYTES + bytes[at + 10] + ( bytes[at + 11] << 8 );
					}
					const int later = 1 + RecordFuzzRandom( rng, headers.Num() - 1 );
					offset = headers[later];
					bytes[offset] = bytes[headers[RecordFuzzRandom( rng, later )]];
					break;
				}
				case FUZZ_CELL:
				{
					const int at = MREC_HEADER_CELLID_OFFSET + worldBytes;
					offset = at;
					const uint32 value = 2 + ( uint32 )RecordFuzzRandom( rng, 0xfffffe );
					for( int b = 0; b < 4; b++ )
					{
						bytes[at + b] = ( byte )( value >> ( b * 8 ) );
					}
					break;
				}
			}
			kinds[kind]++;
			changed[kind] += newLength != length || memcmp( bytes.Ptr(), source.Ptr(), length ) != 0 ? 1 : 0;
			// The codec oracle is separate: a foreign node must not hide a missed validator in the hub record.
			reason.Clear();
			const bool scanned = RecordScanChunk( bytes.Ptr(), newLength, nextIdentity, header, entries, reason );
			const bool framed = scanned && header.node == worldStore.GetCurrentNode() && header.cellId == CELL_ID_NONE;
			int bad = framed ? 0 : 1;
			const int targetValid = scanned && target >= 0 && target < entries.Num() ? entries[target].valid : -1;
			for( int e = 0; framed && e < entries.Num(); e++ )
			{
				bool valid = entries[e].valid;
				if( valid && !entries[e].created && !( entries[e].flags & RECORD_ENTRY_REMOVED ) )
				{
					const int row = worldStore.FindSpawnRow( entries[e].ordinalHint );
					const mapSpawnRow_t* spawn = row >= 0 ? &worldStore.SpawnRow( row ) : NULL;
					idEntity* ent = spawn != NULL ? gameLocal.entities[spawn->entityNumber] : NULL;
					if( ent != NULL && gameLocal.GetSpawnId( ent ) == spawn->spawnId )
					{
						const recordFrame_t frame = RecordChunkFrame( header.cellId );
						RecordFrameEntry( bytes.Ptr(), header.cellId, frame, entries[e], ent->GetEntityDefName(), rec );
						valid = RecordSectionsFit( ent, rec );
					}
				}
				bad += valid ? 0 : 1;
				accepted += valid ? 1 : 0;
			}
			dropped += bad;
			// VERSION/CELL/BYTES must fail scanning; unrelated invalid entries or node mismatch never satisfy them.
			const bool hit = !scanned || ( ( kind == FUZZ_MASK || kind == FUZZ_FLAGS || kind == FUZZ_MIGRATED || kind == FUZZ_SECTION_ID ) && targetValid == 0 );
			if( mustDrop && !hit )
			{
				missed++;
				gameLocal.Printf( "RECORD_FUZZ missed mutant %d kind %d offset %d\n", m, kind, offset );
			}
		}
		bool covered = true;
		for( int k = 0; k < FUZZ_KINDS; k++ )
		{
			if( kinds[k] > 0 && changed[k] == 0 )
			{
				covered = false;
				gameLocal.Warning( "recordTest fuzz: kind %d attempted %d times without changing bytes", k, kinds[k] );
			}
		}
		const bool pass = corpusValid && covered && roundtrip && missed == 0 && count > 0;
		gameLocal.Printf( "RECORD_FUZZ kinds flip=%d byte=%d truncate=%d version=%d entries=%d mask=%d flags=%d cell=%d bytes=%d migrated=%d section=%d skipped=%d missed=%d\n", kinds[FUZZ_FLIP], kinds[FUZZ_BYTE], kinds[FUZZ_TRUNCATE], kinds[FUZZ_VERSION], kinds[FUZZ_ENTRIES], kinds[FUZZ_MASK], kinds[FUZZ_FLAGS], kinds[FUZZ_CELL], kinds[FUZZ_BYTES], kinds[FUZZ_MIGRATED], kinds[FUZZ_SECTION_ID], skipped, missed );
		gameLocal.Printf( "RECORD_FUZZ %s n=%d seed=%d dropped=%d accepted=%d roundtrip=%s\n", pass ? "PASS" : "FAIL", count, seed, dropped, accepted, roundtrip ? "equal" : "DIFF" );
		return;
	}
	//puncho end
	if( idStr::Icmp( args.Argv( 1 ), "pose" ) == 0 && args.Argc() == 6 )
	{
		idEntity* ent = gameLocal.FindEntity( args.Argv( 2 ) );
		if( ent == NULL )
		{
			recordTestFailures++;
			gameLocal.Printf( "RECORD_TEST FAIL missing pose target\n" );
			return;
		}
		ent->SetOrigin( idVec3( atof( args.Argv( 3 ) ), atof( args.Argv( 4 ) ), atof( args.Argv( 5 ) ) ) );
		ent->GetPhysics()->PutToRest();
		return;
	}
	if( idStr::Icmp( args.Argv( 1 ), "exit" ) == 0 && args.Argc() == 3 )
	{
		if( !mapGraphDirector.RequestTransition( args.Argv( 2 ) ) )
		{
			recordTestFailures++;
			gameLocal.Printf( "RECORD_TEST FAIL refused graph exit\n" );
		}
		return;
	}
	if( idStr::Icmp( args.Argv( 1 ), "corpses" ) == 0 && args.Argc() == 3 )
	{
		int count = 0;
		for( idEntity* ent = gameLocal.spawnedEntities.Next(); ent != NULL; ent = ent->spawnNode.Next() )
		{
			count += ent->IsType( idCorpse::Type ) ? 1 : 0;
		}
		gameLocal.Printf( "RECORD_TEST %s corpses=%d expected=%s\n", count == atoi( args.Argv( 2 ) ) ? "PASS" : "WAIT", count, args.Argv( 2 ) );
		return;
	}
	if( idStr::Icmp( args.Argv( 1 ), "flag" ) == 0 && args.Argc() == 4 )
	{
		const int value = mapGraphDirector.GetFlag( args.Argv( 2 ) );
		const bool equal = value == atoi( args.Argv( 3 ) );
		if( !equal )
		{
			recordTestFailures++;
		}
		gameLocal.Printf( "RECORD_TEST %s flag=%s value=%d expected=%s\n", equal ? "PASS" : "FAIL", args.Argv( 2 ), value, args.Argv( 3 ) );
		return;
	}
	if( idStr::Icmp( args.Argv( 1 ), "node" ) == 0 && args.Argc() == 3 )
	{
		const bool equal = worldStore.GetCurrentNode() == atoi( args.Argv( 2 ) );
		if( !equal )
		{
			recordTestFailures++;
		}
		gameLocal.Printf( "RECORD_TEST %s node=%d expected=%s\n", equal ? "PASS" : "FAIL", worldStore.GetCurrentNode(), args.Argv( 2 ) );
		return;
	}
	if( idStr::Icmp( args.Argv( 1 ), "exists" ) == 0 && args.Argc() == 4 )
	{
		const bool exists = gameLocal.FindEntity( args.Argv( 2 ) ) != NULL;
		const bool equal = exists == ( atoi( args.Argv( 3 ) ) != 0 );
		if( !equal )
		{
			recordTestFailures++;
		}
		gameLocal.Printf( "RECORD_TEST %s entity=%s exists=%d\n", equal ? "PASS" : "FAIL", args.Argv( 2 ), exists );
		return;
	}
	if( idStr::Icmp( args.Argv( 1 ), "kill" ) == 0 && args.Argc() == 3 )
	{
		idEntity* ent = gameLocal.FindEntity( args.Argv( 2 ) );
		if( ent != NULL && ent->IsType( idAI::Type ) )
		{
			ent->Show();
			ent->health = 1;
			ent->Damage( gameLocal.GetLocalPlayer(), gameLocal.GetLocalPlayer(), vec3_origin, "damage_bullet_pistol", 1.0f, INVALID_JOINT );
		}
		else
		{
			recordTestFailures++;
			gameLocal.Printf( "RECORD_TEST FAIL missing kill target\n" );
		}
		return;
	}
	if( idStr::Icmp( args.Argv( 1 ), "bind_count" ) == 0 && args.Argc() == 3 )
	{
		int count = 0;
		for( idEntity* ent = gameLocal.spawnedEntities.Next(); ent != NULL; ent = ent->spawnNode.Next() )
		{
			if( ent->mapSpawnIndex < 0 && ent->GetBindMaster() != NULL && ent->worldCreatedId != 0 )
			{
				count++;
			}
		}
		const bool equal = count == atoi( args.Argv( 2 ) );
		if( !equal )
		{
			recordTestFailures++;
		}
		gameLocal.Printf( "RECORD_TEST %s created_binds=%d expected=%s\n", equal ? "PASS" : "FAIL", count, args.Argv( 2 ) );
		return;
	}
	if( idStr::Icmp( args.Argv( 1 ), "capture" ) == 0 )
	{
		recordTestBaseline.Clear();
		recordTestMap = gameLocal.GetMapName();
		recordTestFailures = 0;
		for( int i = 0; i < worldStore.NumSpawnRows(); i++ )
		{
			const mapSpawnRow_t& row = worldStore.SpawnRow( i );
			idEntity* ent = gameLocal.entities[row.entityNumber];
			recordTestRow_t expected;
			expected.ordinal = row.ordinal;
			expected.present = ent != NULL && gameLocal.GetSpawnId( ent ) == row.spawnId;
			expected.health = expected.present ? ent->health : 0;
			expected.hidden = expected.present && ent->IsHidden();
			expected.origin = expected.present ? ent->GetPhysics()->GetOrigin() : vec3_origin;
			expected.poseCRC = 0;
			if( expected.present && ent->IsType( idCorpse::Type ) )
			{
				idFile_Memory pose;
				static_cast<idCorpse*>( ent )->WritePose( &pose );
				expected.poseCRC = CRC32_BlockChecksum( pose.GetDataPtr(), pose.Length() );
			}
			recordTestBaseline.Append( expected );
		}
		gameLocal.Printf( "RECORD_TEST CAPTURE %s %d rows\n", recordTestMap.c_str(), recordTestBaseline.Num() );
		return;
	}
	if( idStr::Icmp( args.Argv( 1 ), "compare" ) == 0 )
	{
		int failures = 0;
		if( recordTestBaseline.Num() == 0 || recordTestMap != gameLocal.GetMapName() || recordTestBaseline.Num() != worldStore.NumSpawnRows() || gameLocal.IsMapTransitionActive() )
		{
			failures++;
			gameLocal.Printf( "RECORD_TEST identity mismatch expected=%s actual=%s rows=%d/%d pending=%d\n", recordTestMap.c_str(), gameLocal.GetMapName(), recordTestBaseline.Num(), worldStore.NumSpawnRows(), gameLocal.IsMapTransitionActive() );
		}
		for( int i = 0; i < recordTestBaseline.Num() && i < worldStore.NumSpawnRows(); i++ )
		{
			const recordTestRow_t& expected = recordTestBaseline[i];
			const mapSpawnRow_t& row = worldStore.SpawnRow( i );
			idEntity* ent = gameLocal.entities[row.entityNumber];
			const bool present = ent != NULL && gameLocal.GetSpawnId( ent ) == row.spawnId;
			unsigned int poseCRC = 0;
			if( present && ent->IsType( idCorpse::Type ) )
			{
				idFile_Memory pose;
				static_cast<idCorpse*>( ent )->WritePose( &pose );
				poseCRC = CRC32_BlockChecksum( pose.GetDataPtr(), pose.Length() );
			}
			if( poseCRC != expected.poseCRC || row.ordinal != expected.ordinal || present != expected.present || ( present && ( ent->health != expected.health || ent->IsHidden() != expected.hidden || !ent->GetPhysics()->GetOrigin().Compare( expected.origin, 0.0001f ) ) ) )
			{
				failures++;
				gameLocal.Printf( "RECORD_TEST mismatch ordinal %d present=%d/%d health=%d/%d hidden=%d/%d origin=%s/%s\n", expected.ordinal, present, expected.present, present ? ent->health : 0, expected.health, present && ent->IsHidden(), expected.hidden, present ? ent->GetPhysics()->GetOrigin().ToString() : "absent", expected.origin.ToString() );
			}
		}
		recordTestFailures += failures;
		gameLocal.Printf( "RECORD_TEST %s %s changed_rows=%d total_failures=%d\n", failures == 0 ? "PASS" : "FAIL", args.Argc() > 2 ? args.Argv( 2 ) : "compare", failures, recordTestFailures );
		return;
	}
	if( idStr::Icmp( args.Argv( 1 ), "finish" ) == 0 )
	{
		gameLocal.Printf( "RECORD_TEST COMPLETE %s total_failures=%d\n", recordTestFailures == 0 && recordTestBaseline.Num() > 0 ? "PASS" : "FAIL", recordTestFailures );
	}
}
#endif

static void Cmd_ListMapRecords_f( const idCmdArgs& args )
{
	worldStore.ListRecords();
	//puncho start - P2b.1.3: the spawnArgs keys builds before RECORD_IDENTITY kept the world identity in
	// (idEntity::NumLegacyWorldKeys); a load deletes them and nothing writes them, so 0
	int legacyKeys = 0;
	for( idEntity* ent = gameLocal.spawnedEntities.Next(); ent != NULL; ent = ent->spawnNode.Next() )
	{
		legacyKeys += ent->NumLegacyWorldKeys();
	}
	gameLocal.Printf( "record legacy keys=%d\n", legacyKeys );
	//puncho end
}

static void Cmd_SetWorldTime_f( const idCmdArgs& args )
{
	if( args.Argc() != 2 || !gameLocal.CheatsOk() )
	{
		return;
	}
	const int64 value = strtoll( args.Argv( 1 ), NULL, 10 );
	if( value < 0 )
	{
		return;
	}
	gameLocal.SetWorldTimeBase( value - gameLocal.fast.time );
}

//puncho start - P2b.1.0: the reference instruments. A row after its ordinal: Phase 2's columns with their text kept, then
// the physics axis as a compressed quaternion and the record states, each empty where the entity's class has none
static void DumpMapStateRow( idFile* out, idEntity* ent )
{
	const idVec3& origin = ent->GetPhysics()->GetOrigin();
	const idCQuat axis = ent->GetPhysics()->GetAxis().ToCQuat();
	out->Printf( "%s,%s,%d,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,", ent->name.c_str(), ent->GetEntityDefName(), ent->health, ent->IsHidden(), origin.x, origin.y, origin.z, axis.x, axis.y, axis.z );
	if( ent->IsType( idMover_Binary::Type ) )
	{
		out->Printf( "%d,%d,", static_cast<idMover_Binary*>( ent )->RecordMoverState(), ent->spawnArgs.GetInt( "locked" ) );
	}
	else
	{
		out->Printf( ",," );
	}
	if( ent->IsType( idDamagable::Type ) )
	{
		out->Printf( "%d,", static_cast<idDamagable*>( ent )->RecordBroken() );
	}
	else
	{
		out->Printf( "," );
	}
	if( ent->IsType( idElevator::Type ) )
	{
		out->Printf( "%d", static_cast<idElevator*>( ent )->RecordFloor() );
	}
	out->Printf( "\n" );
}

struct dumpCreatedRow_t
{
	uint64		id;
	idEntity*	ent;
};

class idSortDumpCreated : public idSort_Quick<dumpCreatedRow_t, idSortDumpCreated>
{
public:
	int Compare( const dumpCreatedRow_t& a, const dumpCreatedRow_t& b ) const
	{
		if( a.id != b.id )
		{
			return a.id < b.id ? -1 : 1;
		}
		return a.ent->entityNumber - b.ent->entityNumber;
	}
};
//puncho end

static void Cmd_DumpMapState_f( const idCmdArgs& args )
{
	if( args.Argc() != 2 )
	{
		return;
	}
	idFile* out = fileSystem->OpenFileWrite( args.Argv( 1 ) );
	if( out == NULL )
	{
		return;
	}
	out->Printf( "ordinal,name,def,health,hidden,x,y,z,qx,qy,qz,moverPos,locked,broken,floor\n" );	//puncho - P2b.1.0: seven state columns
	for( int i = 0; i < worldStore.NumSpawnRows(); i++ )
	{
		const mapSpawnRow_t& row = worldStore.SpawnRow( i );
		idEntity* ent = gameLocal.entities[row.entityNumber];
		if( ent == NULL || gameLocal.GetSpawnId( ent ) != row.spawnId )
		{
			out->Printf( "%d,REMOVED,,,,,,,,,,,,,\n", row.ordinal );
			continue;
		}
		out->Printf( "%d,", row.ordinal );
		DumpMapStateRow( out, ent );
	}
	//puncho start - P2b.1.0: a row per created entity, by created id, after the map rows; stamped with a created id by a
	// carry, an authored entity keeps only its map row
	idList<dumpCreatedRow_t, TAG_GAME> created;
	for( idEntity* ent = gameLocal.spawnedEntities.Next(); ent != NULL; ent = ent->spawnNode.Next() )
	{
		const uint64 id = ent->worldCreatedId;
		if( ent->mapSpawnIndex < 0 && id != 0 )
		{
			dumpCreatedRow_t& row = created.Alloc();
			row.id = id;
			row.ent = ent;
		}
	}
	created.SortWithTemplate( idSortDumpCreated() );
	for( int i = 0; i < created.Num(); i++ )
	{
		out->Printf( "C%llu,", ( unsigned long long )created[i].id );
		DumpMapStateRow( out, created[i].ent );
	}
	//puncho end
	fileSystem->CloseFile( out );
}

#if !defined( ID_RETAIL )
//puncho - P2b.1.0: the world store's chunks raw and decoded, with the recipe bytes; idWorldStore::DumpRecords has the format
static void Cmd_RecordDump_f( const idCmdArgs& args )
{
	if( args.Argc() != 2 )
	{
		gameLocal.Printf( "usage: recordDump <file under fs_savepath>\n" );
		return;
	}
	idFile* out = fileSystem->OpenFileWrite( args.Argv( 1 ) );
	if( out == NULL )
	{
		gameLocal.Printf( "recordDump: can't write %s\n", args.Argv( 1 ) );
		return;
	}
	worldStore.DumpRecords( out );
	fileSystem->CloseFile( out );
}
#endif

static void Cmd_ListTransition_f( const idCmdArgs& args )
{
	gameLocal.ListTransition();
}

#if !defined( ID_RETAIL )	//puncho - dev guard (P2b.1.1a): the transition fixtures are test scaffolding
bool QueueTransitionTestParty( const mapTransition_t& request, bool farPeer );

static void Cmd_TestPartyPlacement_f( const idCmdArgs& args )
{
	if( common->IsClient() || !gameLocal.CheatsOk() || args.Argc() != 5 )
	{
		gameLocal.Printf( "usage: testPartyPlacement <map> <departure> <arrival> <farPeer 0/1> (server only)\n" );
		return;
	}
	if( gameLocal.mapLandmarks.FindLandmark( args.Argv( 2 ) ) == NULL )
	{
		gameLocal.Printf( "testPartyPlacement: missing landmark\n" );
		return;
	}
	int count = 0;
	for( int i = 0; i < MAX_CLIENTS; i++ )
	{
		count += gameLocal.entities[i] != NULL && gameLocal.entities[i]->IsType( idPlayer::Type );
	}
	if( count < 1 || count > 2 )
	{
		gameLocal.Printf( "testPartyPlacement: fixture requires one or two players\n" );
		return;
	}
	mapTransition_t request;
	request.nextMap = args.Argv( 1 );
	request.departLandmark = args.Argv( 2 );
	request.arrivalLandmark = args.Argv( 3 );
	QueueTransitionTestParty( request, atoi( args.Argv( 4 ) ) != 0 );
}

static void Cmd_TransitionTest_f( const idCmdArgs& args )
{
	if( args.Argc() < 3 || common->IsClient() )
	{
		gameLocal.Printf( "usage: transitionTest <map> <departure> [arrival] (server only)\n" );
		return;
	}
	mapTransition_t request;
	request.nextMap = args.Argv( 1 );
	request.departLandmark = args.Argv( 2 );
	request.arrivalLandmark = args.Argc() > 3 ? args.Argv( 3 ) : args.Argv( 2 );
	gameLocal.BeginMapTransition( request );
}
#endif

void MapGraphCheck_f( const idCmdArgs& args );
void CorpseFixture_f( const idCmdArgs& args );

void idGameLocal::InitConsoleCommands()
{
	cmdSystem->AddCommand( "corpseFixture", CorpseFixture_f, CMD_FL_GAME | CMD_FL_CHEAT, "bounded static corpse render fixture (mg test maps only)" );
	cmdSystem->AddCommand( "mapGraphCheck", MapGraphCheck_f, CMD_FL_GAME, "check map graph authoring kit and seeded caps" );
	cmdSystem->AddCommand( "runScriptFunction", Cmd_RunScriptFunction_f, CMD_FL_GAME | CMD_FL_CHEAT, "invoke a precompiled global noarg void function without changing the script image" );
#if !defined( ID_RETAIL )
	cmdSystem->AddCommand( "recordTest", Cmd_RecordTest_f, CMD_FL_GAME | CMD_FL_CHEAT, "capture/compare record functional fixture; pose moves one test prop" );
#endif
	cmdSystem->AddCommand( "listTransition", Cmd_ListTransition_f, CMD_FL_GAME, "list bounded transition state" );
#if !defined( ID_RETAIL )
	cmdSystem->AddCommand( "testPartyPlacement", Cmd_TestPartyPlacement_f, CMD_FL_GAME | CMD_FL_CHEAT, "place and capture a one/two-player fixture party at the transition boundary" );
	cmdSystem->AddCommand( "transitionTest", Cmd_TransitionTest_f, CMD_FL_GAME | CMD_FL_CHEAT, "request a landmark transition" );
#endif
	cmdSystem->AddCommand( "listMapRecords", Cmd_ListMapRecords_f, CMD_FL_GAME, "list persistent map records" );
	cmdSystem->AddCommand( "dumpMapState", Cmd_DumpMapState_f, CMD_FL_GAME, "write current authored state CSV" );
	cmdSystem->AddCommand( "setWorldTime", Cmd_SetWorldTime_f, CMD_FL_GAME | CMD_FL_CHEAT, "set unscaled world clock in milliseconds" );
	cmdSystem->AddCommand( "listMap", Cmd_ListMap_f, CMD_FL_GAME, "list MAP resource rows, portal digest and entity classes" );
	cmdSystem->AddCommand( "worldTime", Cmd_WorldTime_f, CMD_FL_GAME, "print the unscaled world clock" );
#if !defined( ID_RETAIL )
	cmdSystem->AddCommand( "cm_selfTest", Cmd_CMSelfTest_f, CMD_FL_GAME | CMD_FL_CHEAT, "seeded collision bounds, contents, rotation and wrap tests" );
	cmdSystem->AddCommand( "clipSelfTest", Cmd_ClipSelfTest_f, CMD_FL_GAME | CMD_FL_CHEAT, "seeded clip gather wrap test" );
	cmdSystem->AddCommand( "recordDump", Cmd_RecordDump_f, CMD_FL_GAME, "write the world store's records, raw and decoded, and the bytes of the recipe this map loaded with" );	//puncho - P2b.1.0
#endif
	cmdSystem->AddCommand( "game_memory",			idClass::DisplayInfo_f,		CMD_FL_GAME,				"displays game class info" );
	cmdSystem->AddCommand( "listClasses",			idClass::ListClasses_f,		CMD_FL_GAME,				"lists game classes" );
	cmdSystem->AddCommand( "listThreads",			idThread::ListThreads_f,	CMD_FL_GAME | CMD_FL_CHEAT,	"lists script threads" );
	cmdSystem->AddCommand( "listEntities",			Cmd_EntityList_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"lists game entities" );
	cmdSystem->AddCommand( "listActiveEntities",	Cmd_ActiveEntityList_f,		CMD_FL_GAME | CMD_FL_CHEAT,	"lists active game entities" );
	cmdSystem->AddCommand( "listMonsters",			idAI::List_f,				CMD_FL_GAME | CMD_FL_CHEAT,	"lists monsters" );
	cmdSystem->AddCommand( "listSpawnArgs",			Cmd_ListSpawnArgs_f,		CMD_FL_GAME | CMD_FL_CHEAT,	"list the spawn args of an entity", idGameLocal::ArgCompletion_EntityName );
	cmdSystem->AddCommand( "listSpawnIds",			Cmd_ListSpawnIds_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"lists the spawn-id generations of the map's entities, with a digest" );	//puncho - identity (P1-A2)
	cmdSystem->AddCommand( "say",					Cmd_Say_f,					CMD_FL_GAME,				"text chat" );
	cmdSystem->AddCommand( "sayTeam",				Cmd_SayTeam_f,				CMD_FL_GAME,				"team text chat" );
	cmdSystem->AddCommand( "addChatLine",			Cmd_AddChatLine_f,			CMD_FL_GAME,				"internal use - core to game chat lines" );
	cmdSystem->AddCommand( "give",					Cmd_Give_f,					CMD_FL_GAME | CMD_FL_CHEAT,	"gives one or more items" );
	cmdSystem->AddCommand( "centerview",			Cmd_CenterView_f,			CMD_FL_GAME,				"centers the view" );
	cmdSystem->AddCommand( "god",					Cmd_God_f,					CMD_FL_GAME | CMD_FL_CHEAT,	"enables god mode" );
	cmdSystem->AddCommand( "notarget",				Cmd_Notarget_f,				CMD_FL_GAME | CMD_FL_CHEAT,	"disables the player as a target" );
	cmdSystem->AddCommand( "noclip",				Cmd_Noclip_f,				CMD_FL_GAME | CMD_FL_CHEAT,	"disables collision detection for the player" );
	cmdSystem->AddCommand( "where",					Cmd_GetViewpos_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"prints the current view position" );
	cmdSystem->AddCommand( "listBudgets", Cmd_ListBudgets_f, CMD_FL_GAME, "list transient budget counters and entity high water; listBudgets order adds each class's join order" );
	cmdSystem->AddCommand( "checkPVSAreas", Cmd_CheckPVSAreas_f, CMD_FL_GAME, "list entities touching more than four PVS areas" );
	cmdSystem->AddCommand( "listDormancy", Cmd_ListDormancy_f, CMD_FL_GAME, "list authored dormancy entities and wake state" );
	cmdSystem->AddCommand( "listMapStatics", Cmd_ListMapStatics_f, CMD_FL_GAME, "list static spawn-skip eligibility and retained entity numbers" );
	cmdSystem->AddCommand( "saveTestView", Cmd_SaveTestView_f, CMD_FL_GAME, "save a single-player view for the next testmap" );
	cmdSystem->AddCommand( "getviewpos",			Cmd_GetViewpos_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"prints the current view position" );
	cmdSystem->AddCommand( "setviewpos",			Cmd_SetViewpos_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"sets the current view position" );
	cmdSystem->AddCommand( "teleport",				Cmd_Teleport_f,				CMD_FL_GAME | CMD_FL_CHEAT,	"teleports the player to an entity location", idGameLocal::ArgCompletion_EntityName );
	cmdSystem->AddCommand( "trigger",				Cmd_Trigger_f,				CMD_FL_GAME | CMD_FL_CHEAT,	"triggers an entity", idGameLocal::ArgCompletion_EntityName );
	cmdSystem->AddCommand( "spawn",					Cmd_Spawn_f,				CMD_FL_GAME | CMD_FL_CHEAT,	"spawns a game entity", idCmdSystem::ArgCompletion_Decl<DECL_ENTITYDEF> );
	//puncho start - ai-harness
	cmdSystem->AddCommand( "ai_spawnMinions",		Cmd_AISpawnMinions_f,		CMD_FL_GAME | CMD_FL_CHEAT,	"ai_spawnMinions <count> [classname] [spacing] [team]: spawns idAI on a grid in front of the player, named minion_<n>" );
	cmdSystem->AddCommand( "ai_moveMinions",		Cmd_AIMoveMinions_f,		CMD_FL_GAME | CMD_FL_CHEAT,	"ai_moveMinions [aim | here | x y z]: orders every minion_* to the point" );
	cmdSystem->AddCommand( "ai_killMinions",		Cmd_AIKillMinions_f,		CMD_FL_GAME | CMD_FL_CHEAT,	"removes every minion_*" );
	//puncho end
	cmdSystem->AddCommand( "damage",				Cmd_Damage_f,				CMD_FL_GAME | CMD_FL_CHEAT,	"apply damage to an entity", idGameLocal::ArgCompletion_EntityName );
	cmdSystem->AddCommand( "remove",				Cmd_Remove_f,				CMD_FL_GAME | CMD_FL_CHEAT,	"removes an entity", idGameLocal::ArgCompletion_EntityName );
	cmdSystem->AddCommand( "killMonsters",			Cmd_KillMonsters_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"removes all monsters" );
	cmdSystem->AddCommand( "killMoveables",			Cmd_KillMovables_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"removes all moveables" );
	cmdSystem->AddCommand( "killRagdolls",			Cmd_KillRagdolls_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"removes all ragdolls" );
	cmdSystem->AddCommand( "addline",				Cmd_AddDebugLine_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"adds a debug line" );
	cmdSystem->AddCommand( "addarrow",				Cmd_AddDebugLine_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"adds a debug arrow" );
	cmdSystem->AddCommand( "removeline",			Cmd_RemoveDebugLine_f,		CMD_FL_GAME | CMD_FL_CHEAT,	"removes a debug line" );
	cmdSystem->AddCommand( "blinkline",				Cmd_BlinkDebugLine_f,		CMD_FL_GAME | CMD_FL_CHEAT,	"blinks a debug line" );
	cmdSystem->AddCommand( "listLines",				Cmd_ListDebugLines_f,		CMD_FL_GAME | CMD_FL_CHEAT,	"lists all debug lines" );
	cmdSystem->AddCommand( "playerModel",			Cmd_PlayerModel_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"sets the given model on the player", idCmdSystem::ArgCompletion_Decl<DECL_MODELDEF> );
	cmdSystem->AddCommand( "testFx",				Cmd_TestFx_f,				CMD_FL_GAME | CMD_FL_CHEAT,	"tests an FX system", idCmdSystem::ArgCompletion_Decl<DECL_FX> );
	cmdSystem->AddCommand( "testBoneFx",			Cmd_TestBoneFx_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"tests an FX system bound to a joint", idCmdSystem::ArgCompletion_Decl<DECL_FX> );
	cmdSystem->AddCommand( "testLight",				Cmd_TestLight_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"tests a light" );
	cmdSystem->AddCommand( "testPointLight",		Cmd_TestPointLight_f,		CMD_FL_GAME | CMD_FL_CHEAT,	"tests a point light" );
	cmdSystem->AddCommand( "popLight",				Cmd_PopLight_f,				CMD_FL_GAME | CMD_FL_CHEAT,	"removes the last created light" );
	cmdSystem->AddCommand( "testDeath",				Cmd_TestDeath_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"tests death" );
	cmdSystem->AddCommand( "testSave",				Cmd_TestSave_f,				CMD_FL_GAME | CMD_FL_CHEAT,	"writes out a test savegame" );
	//puncho start - stream-trace (P1-A1c)
	cmdSystem->AddCommand( "testSaveRoundTrip",		Cmd_TestSaveRoundTrip_f,	CMD_FL_GAME | CMD_FL_CHEAT,	"saves to memory and reloads twice; checks the first trip by trace and the last two saves byte for byte" );
	cmdSystem->AddCommand( "testSaveTraceWrite",	Cmd_TestSaveTraceWrite_f,	CMD_FL_GAME | CMD_FL_CHEAT,	"testSaveTraceWrite <file>: writes each class's save trace of the current state" );
	cmdSystem->AddCommand( "testSaveTraceCompare",	Cmd_TestSaveTraceCompare_f,	CMD_FL_GAME | CMD_FL_CHEAT,	"testSaveTraceCompare <file>: compares the current save trace with one an earlier build wrote" );
	cmdSystem->AddCommand( "testSnapTraceCompare",	Cmd_TestSnapTraceCompare_f,	CMD_FL_GAME,				"testSnapTraceCompare <host file> <client file>: matches a client's net_snapTrace reads with the host's writes" );
	//puncho end
	cmdSystem->AddCommand( "testSpawnChurn",		Cmd_TestSpawnChurn_f,		CMD_FL_GAME | CMD_FL_CHEAT,	"spawns and deletes entities to test the per-slot spawn-id generation" );	//puncho - identity (P1-A2)
	//puncho start - gate tools for plans/phase1/02-FIXES.md track F3
	cmdSystem->AddCommand( "testLaunch",			Cmd_TestLaunch_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"testLaunch <projectileDef> <target> [dmgPower]: launches a projectile at an entity as this peer's weapon would" );
	cmdSystem->AddCommand( "testUnspectate",		Cmd_TestUnspectate_f,		CMD_FL_GAME | CMD_FL_CHEAT,	"on a multiplayer host, makes every player want to play, so warmup can end" );
	cmdSystem->AddCommand( "testEntState",			Cmd_TestEntState_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"testEntState <entity>...: prints this peer's view of each entity" );
	cmdSystem->AddCommand( "testHitClaim",			Cmd_TestHitClaim_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"testHitClaim <count> <victim> <joint> [damageDef]: a client sends hit claims to the host" );
	cmdSystem->AddCommand( "testSnapSubmit",		Cmd_TestSnapSubmit_f,		CMD_FL_GAME | CMD_FL_CHEAT,	"testSnapSubmit <objects> <bytes>: submits one synthetic snapshot through the snapshot jobs" );
	//puncho end
	cmdSystem->AddCommand( "testDeclRemap",			Cmd_TestDeclRemap_f,		CMD_FL_GAME | CMD_FL_CHEAT,	"drives the implicit decl announce table: make, decode, sound, rocket, impact, fx, speakers" );	//puncho - content check (P1-A3a)
	cmdSystem->AddCommand( "testModel",				idTestModel::TestModel_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"tests a model", idTestModel::ArgCompletion_TestModel );
	cmdSystem->AddCommand( "testSkin",				idTestModel::TestSkin_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"tests a skin on an existing testModel", idCmdSystem::ArgCompletion_Decl<DECL_SKIN> );
	cmdSystem->AddCommand( "testShaderParm",		idTestModel::TestShaderParm_f,		CMD_FL_GAME | CMD_FL_CHEAT,	"sets a shaderParm on an existing testModel" );
	cmdSystem->AddCommand( "keepTestModel",			idTestModel::KeepTestModel_f,		CMD_FL_GAME | CMD_FL_CHEAT,	"keeps the last test model in the game" );
	cmdSystem->AddCommand( "testAnim",				idTestModel::TestAnim_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"tests an animation", idTestModel::ArgCompletion_TestAnim );
	cmdSystem->AddCommand( "testParticleStopTime",	idTestModel::TestParticleStopTime_f, CMD_FL_GAME | CMD_FL_CHEAT,	"tests particle stop time on a test model" );
	cmdSystem->AddCommand( "nextAnim",				idTestModel::TestModelNextAnim_f,	CMD_FL_GAME | CMD_FL_CHEAT,	"shows next animation on test model" );
	cmdSystem->AddCommand( "prevAnim",				idTestModel::TestModelPrevAnim_f,	CMD_FL_GAME | CMD_FL_CHEAT,	"shows previous animation on test model" );
	cmdSystem->AddCommand( "nextFrame",				idTestModel::TestModelNextFrame_f,	CMD_FL_GAME | CMD_FL_CHEAT,	"shows next animation frame on test model" );
	cmdSystem->AddCommand( "prevFrame",				idTestModel::TestModelPrevFrame_f,	CMD_FL_GAME | CMD_FL_CHEAT,	"shows previous animation frame on test model" );
	cmdSystem->AddCommand( "testBlend",				idTestModel::TestBlend_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"tests animation blending" );
	cmdSystem->AddCommand( "reloadScript",			Cmd_ReloadScript_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"reloads scripts" );
	cmdSystem->AddCommand( "script",				Cmd_Script_f,				CMD_FL_GAME | CMD_FL_CHEAT,	"executes a line of script" );
	cmdSystem->AddCommand( "listCollisionModels",	Cmd_ListCollisionModels_f,	CMD_FL_GAME,				"lists collision models" );
	cmdSystem->AddCommand( "collisionModelInfo",	Cmd_CollisionModelInfo_f,	CMD_FL_GAME,				"shows collision model info" );
	cmdSystem->AddCommand( "reloadanims",			Cmd_ReloadAnims_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"reloads animations" );
	cmdSystem->AddCommand( "listAnims",				Cmd_ListAnims_f,			CMD_FL_GAME,				"lists all animations" );
	cmdSystem->AddCommand( "aasStats",				Cmd_AASStats_f,				CMD_FL_GAME,				"shows AAS stats" );
	cmdSystem->AddCommand( "testDamage",			Cmd_TestDamage_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"tests a damage def", idCmdSystem::ArgCompletion_Decl<DECL_ENTITYDEF> );
	cmdSystem->AddCommand( "weaponSplat",			Cmd_WeaponSplat_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"projects a blood splat on the player weapon" );
	cmdSystem->AddCommand( "saveSelected",			Cmd_SaveSelected_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"saves the selected entity to the .map file" );
	cmdSystem->AddCommand( "deleteSelected",		Cmd_DeleteSelected_f,		CMD_FL_GAME | CMD_FL_CHEAT,	"deletes selected entity" );
	cmdSystem->AddCommand( "saveMoveables",			Cmd_SaveMoveables_f,		CMD_FL_GAME | CMD_FL_CHEAT,	"save all moveables to the .map file" );
	cmdSystem->AddCommand( "saveRagdolls",			Cmd_SaveRagdolls_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"save all ragdoll poses to the .map file" );
	cmdSystem->AddCommand( "bindRagdoll",			Cmd_BindRagdoll_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"binds ragdoll at the current drag position" );
	cmdSystem->AddCommand( "unbindRagdoll",			Cmd_UnbindRagdoll_f,		CMD_FL_GAME | CMD_FL_CHEAT,	"unbinds the selected ragdoll" );
	cmdSystem->AddCommand( "saveLights",			Cmd_SaveLights_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"saves all lights to the .map file" );
	cmdSystem->AddCommand( "saveParticles",			Cmd_SaveParticles_f,		CMD_FL_GAME | CMD_FL_CHEAT,	"saves all lights to the .map file" );
	cmdSystem->AddCommand( "clearLights",			Cmd_ClearLights_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"clears all lights" );
	cmdSystem->AddCommand( "gameError",				Cmd_GameError_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"causes a game error" );

	cmdSystem->AddCommand( "disasmScript",			Cmd_DisasmScript_f,			CMD_FL_GAME | CMD_FL_CHEAT,	"disassembles script" );
	cmdSystem->AddCommand( "recordViewNotes",		Cmd_RecordViewNotes_f,		CMD_FL_GAME | CMD_FL_CHEAT,	"record the current view position with notes" );
	cmdSystem->AddCommand( "showViewNotes",			Cmd_ShowViewNotes_f,		CMD_FL_GAME | CMD_FL_CHEAT,	"show any view notes for the current map, successive calls will cycle to the next note" );
	cmdSystem->AddCommand( "closeViewNotes",		Cmd_CloseViewNotes_f,		CMD_FL_GAME | CMD_FL_CHEAT,	"close the view showing any notes for this map" );

	// RB begin
	cmdSystem->AddCommand( "exportScriptEvents",	idClass::ExportScriptEvents_f,	CMD_FL_GAME | CMD_FL_TOOL,	"update script/doom_events.script" );
	cmdSystem->AddCommand( "editLights",			Cmd_EditLights_f,			CMD_FL_GAME | CMD_FL_TOOL,	"launches the in-game Light Editor" );
	cmdSystem->AddCommand( "saveEnvprobes",			Cmd_SaveEnvprobes_f,		CMD_FL_GAME | CMD_FL_CHEAT,	"saves all autogenerated env_probes to a .extras_env_probes.map file" );
	// RB end

	// SP Begin
	cmdSystem->AddCommand( "editAFs",				Cmd_ShowAfEditor_f,			CMD_FL_GAME | CMD_FL_TOOL, "launches the in-game Articulated Figure Editor" );
	// SP end

	// multiplayer client commands ( replaces old impulses stuff )
	//cmdSystem->AddCommand( "clientDropWeapon",		idMultiplayerGame::DropWeapon_f, CMD_FL_GAME,			"drop current weapon" );
	cmdSystem->AddCommand( "clientMessageMode",		idMultiplayerGame::MessageMode_f, CMD_FL_GAME,			"ingame gui message mode" );
	// FIXME: implement
//	cmdSystem->AddCommand( "clientVote",			idMultiplayerGame::Vote_f,	CMD_FL_GAME,				"cast your vote: clientVote yes | no" );
//	cmdSystem->AddCommand( "clientCallVote",		idMultiplayerGame::CallVote_f,	CMD_FL_GAME,			"call a vote: clientCallVote si_.. proposed_value" );
	cmdSystem->AddCommand( "clientVoiceChat",		idMultiplayerGame::VoiceChat_f,	CMD_FL_GAME,			"voice chats: clientVoiceChat <sound shader>" );
	cmdSystem->AddCommand( "clientVoiceChatTeam",	idMultiplayerGame::VoiceChatTeam_f,	CMD_FL_GAME,		"team voice chats: clientVoiceChat <sound shader>" );

	// multiplayer server commands
	cmdSystem->AddCommand( "serverMapRestart",		idGameLocal::MapRestart_f,	CMD_FL_GAME,				"restart the current game" );

	// localization help commands
	cmdSystem->AddCommand( "nextGUI",				Cmd_NextGUI_f,				CMD_FL_GAME | CMD_FL_CHEAT,	"teleport the player to the next func_static with a gui" );
	cmdSystem->AddCommand( "testid",				Cmd_TestId_f,				CMD_FL_GAME | CMD_FL_CHEAT,	"output the string for the specified id." );

	cmdSystem->AddCommand( "setActorState",			Cmd_SetActorState_f,		CMD_FL_GAME | CMD_FL_CHEAT,	"Manually sets an actors script state", idGameLocal::ArgCompletion_EntityName );
}

/*
=================
idGameLocal::ShutdownConsoleCommands
=================
*/
void idGameLocal::ShutdownConsoleCommands()
{
	cmdSystem->RemoveFlaggedCommands( CMD_FL_GAME );
}
