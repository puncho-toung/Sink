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

/*
================================================================================
GameVerify.cpp

A pose dump, a smoke count and a code-driven walk for the camera-relative verification sheet
(plans/camera-relative/00-PLAN.md WP-CRV and WP-CR2, 01-VERIFY.md V1.5, V2.2 and V5).
//puncho - P0 adds scenario and gameDigest. Only armed walks and scenario commands change the game.
The commands are CONSOLE_COMMANDs, so they vanish in ID_RETAIL builds; the two tic hooks stay
and return at once when nothing is armed.

The walk is keyed to gameLocal.framenum, never to the console's wait, which the map load and
input events eat (GD8). idGameLocal::RunFrame calls GameVerify_BeginTic before SetupPlayerPVS and
GameVerify_EndTic after the tic's events. Under g_stopTime 1 no tic runs, so neither hook does.

walkTics <n> [forward|right] [at <tic>] [stop] [every] [<capture>]

	Moves the local player 1 unit per tic along its yaw (forward) or to its right, on tics
	<tic> + 1 .. <tic> + n; without "at", from the next tic. The move is absolute from the origin
	at the walk's first tic and velocity is zeroed, so use it with noclip. The end of every walked
	tic prints a pose line. With a capture name it queues "screenshot <capture>.png" after the last
	tic, or "screenshot <capture>_NNNN.png" after every tic with "every". "stop" sets g_stopTime 1
	at the end of the last tic, freezing that tic's state for later commands.

	n = 0 walks nowhere: the end of tic <tic> is the whole walk, so "walkTics 0 at 300 stop name"
	freezes the game at tic 300 and captures it.

	The captures are inserted at the head of the command buffer by the game thread and run at the
	start of the next frame, before the next tic, so each sees its tic. That holds with
	com_fixedTic 1 (one tic per frame) and no "wait" pending in the buffer: a pending wait holds
	the captures with it, which is harmless only after "stop". A map load restarts framenum, so a
	walk armed with "at" right after "map" waits for its tics on the new map, however many waits
	the load eats; keep <tic> past the 101 settle tics a single-player map load runs.
================================================================================
*/

#include "precompiled.h"
#pragma hdrstop

#include "Game_local.h"
#include <utility> //puncho - P0: select idList's defined move assignment

// written by walkTics between frames, while the game thread is idle; read and written by the
// tic hooks on the game thread
struct verifyWalk_t
{
	idVec3		start;					// player origin at the walk's first tic
	idVec3		dir;					// unit step
	int			tics;					// tics to walk
	int			startTic;				// the walk moves on tics startTic + 1 .. startTic + tics; -1 until resolved
	int			lastTic;				// framenum at the last hook, to notice a map load restarting it
	bool		armed;
	bool		started;				// the first tic ran on this map, so start and dir are valid
	bool		right;					// strafe right instead of walking forward
	bool		stop;					// set g_stopTime 1 at the end of the last tic
	bool		every;					// capture every walked tic, not only the last
	char		capture[MAX_OSPATH];	// capture name without extension, empty for none
};

static verifyWalk_t verifyWalk;

//puncho start - P0: the existing end-tic hook queues a guarded console dispatcher. No load hook.
// T is a boundary: commands execute with framenum == T - 1, before tic T starts. T < 102 cannot
// work during the initial/settle tics. A pending console wait or a multi-tic frame aborts, never runs late.
extern idCVar com_fixedTic;

struct verifyScenarioLine_t
{
	int tic;
	int sourceLine; //puncho - P0: runtime admission failures name the original file line
	idStr command;
};

static idList<verifyScenarioLine_t, TAG_GAME> verifyScenarioLines;
static char verifyScenarioName[128];
static int verifyScenarioNext, verifyScenarioGeneration, verifyScenarioLastTic, verifyScenarioQueuedTic;
static bool verifyScenarioArmed, verifyScenarioPending, verifyScenarioWaitMap;
static bool verifyScenarioStarted; //puncho - P0: bound to the target map, even before its first command

static void GameVerify_DisarmScenario()
{
	verifyScenarioArmed = verifyScenarioPending = verifyScenarioWaitMap = verifyScenarioStarted = false;
	verifyScenarioNext = 0;
	verifyScenarioLines.Clear();
}

// Mirror idCmdArgs::TokenizeString's flags, negative-number folding and private limits (CmdArgs.h).
// At admission an expansion contributes only its terminating NUL; its value is checked at dispatch.
static const char* GameVerify_CheckScenarioCommand( const idStr& command, bool expandCvars )
{
	idLexer lexer( LEXFL_NOERRORS | LEXFL_NOWARNINGS | LEXFL_NOSTRINGCONCAT | LEXFL_ALLOWPATHNAMES |
		LEXFL_NOSTRINGESCAPECHARS | LEXFL_ALLOWIPADDRESSES );
	lexer.LoadMemory( command.c_str(), command.Length(), "scenario command" );
	idToken token, number;
	int argc = 0;
	int totalLength = 0;
	while( lexer.ReadToken( &token ) )
	{
		if( argc == 64 )
		{
			return "console command exceeds 64 arguments";
		}
		int length = token.Length();
		if( idStr::Cmp( token.c_str(), "-" ) == 0 && lexer.CheckTokenType( TT_NUMBER, 0, &number ) )
		{
			length = number.Length() + 1;
		}
		if( idStr::Cmp( token.c_str(), "$" ) == 0 )
		{
			if( !lexer.ReadToken( &token ) )
			{
				return "missing or invalid cvar name after $";
			}
			length = expandCvars ? idStr::Length( idLib::cvarSystem != NULL ?
				idLib::cvarSystem->GetCVarString( token.c_str() ) : "<unknown>" ) : 0;
		}
		if( length >= 2 * MAX_STRING_CHARS - totalLength )
		{
			return "console command exceeds token storage (2048 bytes including terminators)";
		}
		totalLength += length + 1;
		argc++;
	}
	return lexer.HadError() ? "console command cannot be tokenized completely" : NULL;
}

static void GameVerify_AbortScenario( int tic )
{
	gameLocal.Printf( "scenario %s aborted: tic %i missed at tic %i\n", verifyScenarioName, tic, gameLocal.framenum );
	GameVerify_DisarmScenario();
}

//puncho - P0: the map teardown boundary also catches reloads/restores without relying on tic numbers.
void GameVerify_MapShutdown()
{
	if( verifyScenarioStarted )
	{
		gameLocal.Printf( "scenario %s aborted: map changed at tic %i\n", verifyScenarioName, gameLocal.framenum );
		GameVerify_DisarmScenario();
	}
}

static void GameVerify_ScenarioEndTic()
{
	if( verifyScenarioWaitMap )
	{
		if( gameLocal.framenum != 1 && gameLocal.framenum >= verifyScenarioLastTic )
		{
			verifyScenarioLastTic = gameLocal.framenum;
			return;
		}
		verifyScenarioWaitMap = false;
		verifyScenarioStarted = true;
	}
	verifyScenarioLastTic = gameLocal.framenum;
	const int tic = verifyScenarioPending ? verifyScenarioQueuedTic : verifyScenarioLines[verifyScenarioNext].tic;
	if( gameLocal.framenum >= tic )
	{
		GameVerify_AbortScenario( tic );
		return;
	}
	if( !verifyScenarioPending && gameLocal.framenum == tic - 1 )
	{
		char command[80];
		idStr::snPrintf( command, sizeof( command ), "_scenarioStep %i %i", verifyScenarioGeneration, verifyScenarioNext );
		verifyScenarioQueuedTic = tic;
		verifyScenarioPending = true;
		cmdSystem->BufferCommandText( CMD_EXEC_INSERT, command );
	}
}

CONSOLE_COMMAND( _scenarioStep, "internal guarded scenario dispatcher", NULL )
{
	if( args.Argc() != 3 || !verifyScenarioArmed || !verifyScenarioPending ||
		atoi( args.Argv( 1 ) ) != verifyScenarioGeneration || atoi( args.Argv( 2 ) ) != verifyScenarioNext )
	{
		return;
	}
	if( !com_fixedTic.GetBool() || gameLocal.framenum != verifyScenarioQueuedTic - 1 )
	{
		GameVerify_AbortScenario( verifyScenarioQueuedTic );
		return;
	}
	if( verifyScenarioLines[verifyScenarioNext].tic != verifyScenarioQueuedTic )
	{
		verifyScenarioPending = false;
		return;
	}
	// Recheck the live expansion immediately before dispatch: no intervening command runs before it.
	const verifyScenarioLine_t& entry = verifyScenarioLines[verifyScenarioNext];
	const char* error = GameVerify_CheckScenarioCommand( entry.command, true );
	if( error != NULL )
	{
		gameLocal.Printf( "scenario %s aborted at line %i, tic %i: %s\n",
			verifyScenarioName, entry.sourceLine, gameLocal.framenum, error );
		GameVerify_DisarmScenario();
		return;
	}
	// Guard each console command, preserving same-tic order. Copy before disarming frees the lines.
	idStr command = verifyScenarioLines[verifyScenarioNext].command;
	verifyScenarioNext++;
	if( verifyScenarioNext == verifyScenarioLines.Num() )
	{
		//puncho - P0: completion is the final handoff, before quit/map/restart can tear down the map.
		// Execute now on this console boundary; buffered insertion can silently refuse a full buffer.
		GameVerify_DisarmScenario();
		gameLocal.Printf( "scenario %s done at tic %i\n", verifyScenarioName, gameLocal.framenum );
		cmdSystem->BufferCommandText( CMD_EXEC_NOW, command.c_str() );
		return;
	}
	// Earlier commands still need a continuation, cancelled by scenario stop/re-arm or map teardown.
	char continuation[80];
	idStr::snPrintf( continuation, sizeof( continuation ), "\n_scenarioStep %i %i", verifyScenarioGeneration, verifyScenarioNext );
	command.Append( continuation );
	cmdSystem->BufferCommandText( CMD_EXEC_INSERT, command.c_str() );
}

CONSOLE_COMMAND( scenario, "scenario <name> reads scenarios/<name>.scn; scenario stop disarms; no argument prints state", NULL )
{
	if( args.Argc() == 1 )
	{
		gameLocal.Printf( "scenario %s %s at tic %i, next command %i of %i%s\n", verifyScenarioName,
			verifyScenarioArmed ? "armed" : "disarmed", gameLocal.framenum, verifyScenarioNext + 1,
			verifyScenarioLines.Num(), verifyScenarioWaitMap ? " (waiting for map)" : "" );
		return;
	}
	if( args.Argc() == 2 && idStr::Icmp( args.Argv( 1 ), "stop" ) == 0 )
	{
		gameLocal.Printf( "scenario %s stopped at tic %i\n", verifyScenarioName, gameLocal.framenum );
		GameVerify_DisarmScenario();
		return;
	}
	if( args.Argc() != 2 )
	{
		gameLocal.Printf( "usage: scenario <name> | stop\n" );
		return;
	}
	if( !com_fixedTic.GetBool() )
	{
		gameLocal.Printf( "scenario refused: com_fixedTic 1 is required for one tic per console-command boundary\n" );
		return;
	}
	const char* name = args.Argv( 1 );
	bool validName = name[0] != '\0' && idStr::Length( name ) < ( int )sizeof( verifyScenarioName );
	for( const char* c = name; *c; c++ )
	{
		if( !( ( *c >= 'a' && *c <= 'z' ) || ( *c >= 'A' && *c <= 'Z' ) ||
			( *c >= '0' && *c <= '9' ) || *c == '_' || *c == '-' ) )
		{
			validName = false;
		}
	}
	if( !validName )
	{
		gameLocal.Printf( "scenario requires a plain name of fewer than 128 characters\n" );
		return;
	}
	char path[160];
	idStr::snPrintf( path, sizeof( path ), "scenarios/%s.scn", name );
	void* buffer = NULL;
	const int length = fileSystem->ReadFile( path, &buffer );
	if( length < 0 || buffer == NULL )
	{
		gameLocal.Printf( "scenario: cannot read %s\n", path );
		return;
	}
	idList<verifyScenarioLine_t, TAG_GAME> lines;
	const char* source = ( const char* )buffer;
	const char* error = NULL;
	int lineNumber = 0;
	for( int offset = 0; offset < length; )
	{
		lineNumber++;
		const int start = offset;
		while( offset < length && source[offset] != '\n' )
		{
			if( source[offset] == '\0' )
			{
				error = "embedded NUL in text";
			}
			offset++;
		}
		idStr line;
		line.Append( source + start, offset - start );
		if( offset < length )
		{
			offset++;
		}
		if( error != NULL )
		{
			break;
		}
		line.StripTrailingWhitespace();
		const char* first = line.c_str();
		while( *first == ' ' || *first == '\t' )
		{
			first++;
		}
		if( *first == '\0' || ( first[0] == '/' && first[1] == '/' ) )
		{
			continue;
		}
		idLexer lexer( LEXFL_NOERRORS | LEXFL_NOWARNINGS );
		lexer.LoadMemory( line.c_str(), line.Length(), path, lineNumber );
		idToken token;
		if( *first < '0' || *first > '9' || !lexer.ReadToken( &token ) || token.type != TT_NUMBER )
		{
			error = "expected a whole decimal tic and a console command";
			break;
		}
		int tic = 0;
		for( int i = 0; i < token.Length(); i++ )
		{
			const int digit = token[i] - '0';
			if( digit < 0 || digit > 9 || tic > ( 2147483647 - digit ) / 10 )
			{
				error = "tic must be a whole decimal number within the int range";
				break;
			}
			tic = tic * 10 + digit;
		}
		if( error != NULL )
		{
			break;
		}
		if( tic < 102 )
		{
			error = "tic must be at least 102: map load runs one initial tic and 100 settle tics with no console commands between them";
			break;
		}
		if( lines.Num() > 0 && tic < lines[lines.Num() - 1].tic )
		{
			error = "tics must never decrease";
			break;
		}
		const char* command = line.c_str() + lexer.GetFileOffset();
		if( *command != ' ' && *command != '\t' )
		{
			error = "expected whitespace and a console command after the tic";
			break;
		}
		while( *command == ' ' || *command == '\t' )
		{
			command++;
		}
		if( *command == '\0' )
		{
			error = "missing console command";
			break;
		}
		// Match command-buffer separators, including quoted semicolons. Guard each console command
		// separately so "wait; command" cannot leave the second command queued without a boundary check.
		const char* part = command;
		bool quoted = false;
		for( const char* c = command; ; c++ )
		{
			if( *c == '"' )
			{
				quoted = !quoted;
			}
			if( *c == '\0' || ( *c == ';' && !quoted ) || *c == '\r' )
			{
				verifyScenarioLine_t& entry = lines.Alloc();
				entry.tic = tic;
				entry.sourceLine = lineNumber;
				entry.command.Append( part, ( int )( c - part ) );
				error = GameVerify_CheckScenarioCommand( entry.command, false );
				if( error != NULL )
				{
					break;
				}
				if( *c == '\0' )
				{
					break;
				}
				part = c + 1;
			}
		}
		if( error != NULL )
		{
			break;
		}
	}
	fileSystem->FreeFile( buffer );
	if( error != NULL )
	{
		gameLocal.Printf( "scenario %s refused at line %i: %s\n", name, lineNumber, error );
		return;
	}
	//puncho - P0: commit only after parsing succeeds; transfer the array and its owned strings.
	verifyScenarioLines = std::move( lines );
	idStr::Copynz( verifyScenarioName, name, sizeof( verifyScenarioName ) );
	verifyScenarioGeneration = verifyScenarioGeneration == 2147483647 ? 1 : verifyScenarioGeneration + 1;
	verifyScenarioNext = 0;
	verifyScenarioPending = verifyScenarioStarted = false;
	verifyScenarioLastTic = gameLocal.framenum;
	verifyScenarioArmed = verifyScenarioLines.Num() != 0;
	verifyScenarioWaitMap = verifyScenarioArmed && ( gameLocal.GameState() != GAMESTATE_ACTIVE ||
		verifyScenarioLines[0].tic <= gameLocal.framenum );
	verifyScenarioStarted = verifyScenarioArmed && !verifyScenarioWaitMap;
	if( verifyScenarioArmed )
	{
		gameLocal.Printf( "scenario %s armed at tic %i, %i commands%s\n", name, gameLocal.framenum,
			verifyScenarioLines.Num(), verifyScenarioWaitMap ? " (waiting for map)" : "" );
		if( !verifyScenarioWaitMap )
		{
			// Arming at the boundary itself must not wait for another game tic.
			GameVerify_ScenarioEndTic();
		}
	}
	else
	{
		gameLocal.Printf( "scenario %s done at tic %i\n", name, gameLocal.framenum );
		GameVerify_DisarmScenario();
	}
}

void GameVerify_Shutdown()
{
	GameVerify_DisarmScenario();
}
//puncho end


static uint32 GameVerify_FloatBits( const float f )
{
	uint32 bits;
	memcpy( &bits, &f, sizeof( bits ) );
	return bits;
}

/*
==================
GameVerify_PrintPose

One line for the current framenum: the local player's physics origin and body 0 of the first
entity, by entity number, whose physics is an articulated figure right now (an AF entity or a
ragdolled actor). Entity number order is fixed by the map's spawn order, so two runs pick the same
body; its name and rest state are printed in case the pick changes during a run. The hex is the
comparison; the decimals are for reading.
==================
*/
static void GameVerify_PrintPose()
{
	idPlayer* player = gameLocal.GetLocalPlayer();
	if( player == NULL )
	{
		gameLocal.Printf( "pose %i noplayer\n", gameLocal.framenum );
		return;
	}

	const idVec3 p = player->GetPhysics()->GetOrigin();

	const idEntity* afEntity = NULL;
	for( int i = 0; i < gameLocal.num_entities; i++ )
	{
		const idEntity* ent = gameLocal.entities[i];
		if( ent != NULL && ent->GetPhysics()->IsType( idPhysics_AF::Type ) )
		{
			afEntity = ent;
			break;
		}
	}

	if( afEntity == NULL )
	{
		gameLocal.Printf( "pose %i player %08x %08x %08x ( %.3f %.3f %.3f ) af none\n", gameLocal.framenum,
						  GameVerify_FloatBits( p.x ), GameVerify_FloatBits( p.y ), GameVerify_FloatBits( p.z ), p.x, p.y, p.z );
		return;
	}

	const idPhysics_AF* af = static_cast<const idPhysics_AF*>( afEntity->GetPhysics() );
	const idVec3 b = af->GetOrigin( 0 );

	gameLocal.Printf( "pose %i player %08x %08x %08x ( %.3f %.3f %.3f ) af %s rest %i %08x %08x %08x ( %.3f %.3f %.3f )\n",
					  gameLocal.framenum,
					  GameVerify_FloatBits( p.x ), GameVerify_FloatBits( p.y ), GameVerify_FloatBits( p.z ), p.x, p.y, p.z,
					  afEntity->GetName(), af->IsAtRest() ? 1 : 0,
					  GameVerify_FloatBits( b.x ), GameVerify_FloatBits( b.y ), GameVerify_FloatBits( b.z ), b.x, b.y, b.z );
}

/*
==================
GameVerify_BeginTic

Called by idGameLocal::RunFrame at the start of every tic, after framenum is advanced and before
SetupPlayerPVS and the think loop, so the player's view and PVS of this tic already see the step.
==================
*/
void GameVerify_BeginTic()
{
	verifyWalk_t& w = verifyWalk;
	if( !w.armed )
	{
		return;
	}

	// a map load restarts framenum at 0: the walk starts over at its tics on the new map
	if( gameLocal.framenum < w.lastTic )
	{
		w.started = false;
	}
	w.lastTic = gameLocal.framenum;

	if( w.startTic < 0 )
	{
		// no "at": the walk's first step is this tic, or for n = 0 its end is
		w.startTic = gameLocal.framenum - ( w.tics > 0 ? 1 : 0 );
	}

	const int step = gameLocal.framenum - w.startTic;
	if( step < 1 || step > w.tics )
	{
		return;
	}

	idPlayer* player = gameLocal.GetLocalPlayer();
	if( player == NULL )
	{
		return;
	}

	if( step == 1 )
	{
		idVec3 forward;
		idVec3 right;
		idAngles( 0.0f, player->viewAngles.yaw, 0.0f ).ToVectors( &forward, &right );

		w.start = player->GetPhysics()->GetOrigin();
		w.dir = w.right ? right : forward;
		w.started = true;
	}

	if( !w.started )
	{
		// armed after the walk's first tic had passed on this map
		return;
	}

	// absolute from the start, not accumulated, so the steps cannot drift
	player->SetOrigin( w.start + w.dir * ( float )step );
	player->GetPhysics()->SetLinearVelocity( vec3_origin );
}

/*
==================
GameVerify_PrintSmokeCount

One line: the live smoke count and the smoke model's anchor (see printSmokeCount).
==================
*/
static void GameVerify_PrintSmokeCount()
{
	if( gameLocal.smokeParticles == NULL )
	{
		gameLocal.Printf( "smoke %i none\n", gameLocal.framenum );
		return;
	}

	const idVec3& anchor = gameLocal.smokeParticles->GetAnchor();
	gameLocal.Printf( "smoke %i count %i anchor ( %.1f %.1f %.1f )\n", gameLocal.framenum,
					  gameLocal.smokeParticles->NumActiveSmokes(), anchor.x, anchor.y, anchor.z );
}

/*
==================
GameVerify_EndTic

Called by idGameLocal::RunFrame after the tic's think loop and events, when the tic's state is
final.
==================
*/
void GameVerify_EndTic()
{
	//puncho - P0: one extra inactive test at the existing end-tic hook
	if( verifyScenarioArmed )
	{
		GameVerify_ScenarioEndTic();
	}

	verifyWalk_t& w = verifyWalk;
	if( !w.armed || w.startTic < 0 )
	{
		return;
	}

	const int step = gameLocal.framenum - w.startTic;
	if( w.tics > 0 )
	{
		if( !w.started || step < 1 || step > w.tics )
		{
			return;
		}
	}
	else if( step != 0 )
	{
		return;
	}

	GameVerify_PrintPose();

	if( w.capture[0] != '\0' && ( w.every || step == w.tics ) )
	{
		// inserted, not appended, so it runs ahead of any cfg text still waiting in the buffer; the
		// main thread does not touch the buffer while this tic runs (it executes it before the tic)
		char cmd[MAX_OSPATH + 32];
		if( w.every )
		{
			idStr::snPrintf( cmd, sizeof( cmd ), "screenshot %s_%04i.png", w.capture, step );
		}
		else
		{
			idStr::snPrintf( cmd, sizeof( cmd ), "screenshot %s.png", w.capture );
		}
		cmdSystem->BufferCommandText( CMD_EXEC_INSERT, cmd );
	}

	if( step == w.tics )
	{
		if( w.stop )
		{
			// read at the start of the next RunFrame, so the state of this tic is the frozen one
			g_stopTime.SetBool( true );
		}
		gameLocal.Printf( "walkTics: done at tic %i\n", gameLocal.framenum );
		// the live recipe's smoke check (GD1, V1.5) rides on the done line: a cfg cannot run a
		// command after the walk without a wait that would hold the capture back
		GameVerify_PrintSmokeCount();
		w.armed = false;
	}
}

/*
==================
dumpPose

V5: the pose of the current tic, in the format the walk prints every tic.
==================
*/
CONSOLE_COMMAND( dumpPose, "prints the player origin and the first articulated figure's body 0 origin as hex floats", NULL )
{
	GameVerify_PrintPose();
}

//puncho start - P0: FNV-1a 64, scalar bytes in little-endian order. idLib's hashes are CRC32/MD4/MD5,
// not a 64-bit incremental hash. Never hash object storage, pointers, padding or profiler clocks.
static const uint64 VERIFY_HASH_OFFSET = 14695981039346656037ULL;

static void GameVerify_HashWord( uint64& hash, uint32 value )
{
	for( int i = 0; i < 4; i++ )
	{
		hash = ( hash ^ ( value & 255 ) ) * 1099511628211ULL;
		value >>= 8;
	}
}

static void GameVerify_HashFloat( uint64& hash, float value )
{
	GameVerify_HashWord( hash, GameVerify_FloatBits( value ) );
}

static void GameVerify_HashVector( uint64& hash, const idVec3& value )
{
	GameVerify_HashFloat( hash, value.x );
	GameVerify_HashFloat( hash, value.y );
	GameVerify_HashFloat( hash, value.z );
}

static void GameVerify_HashEntity( uint64& hash, const idEntity* ent )
{
	GameVerify_HashWord( hash, ent != NULL ? ent->entityNumber : -1 );
	GameVerify_HashWord( hash, ent != NULL ? gameLocal.GetSpawnId( ent ) : 0 );
}

// Keep the stored generation even for an expired reference: it is movement state too.
static void GameVerify_HashReference( uint64& hash, int spawnId )
{
	GameVerify_HashWord( hash, spawnId != 0 ? spawnId & ( ( 1 << GENTITYNUM_BITS ) - 1 ) : -1 );
	GameVerify_HashWord( hash, spawnId );
}

CONSOLE_COMMAND( gameDigest, "prints a read-only digest of entity, AI, stored animation and RNG state", NULL )
{
	uint64 entHash = VERIFY_HASH_OFFSET;
	uint64 aiHash = VERIFY_HASH_OFFSET;
	uint64 animHash = VERIFY_HASH_OFFSET;
	uint64 rngHash = VERIFY_HASH_OFFSET;
	int count = 0;
	//puncho - P0: num_entities excludes the world and multiplayer non-replicated slots.
	for( int i = 0; i < MAX_GENTITIES; i++ )
	{
		idEntity* ent = gameLocal.entities[i];
		if( ent == NULL )
		{
			continue;
		}
		count++;
		GameVerify_HashEntity( entHash, ent );
		const char* name = ent->GetClassname();
		do
		{
			entHash = ( entHash ^ ( byte )*name ) * 1099511628211ULL;
		}
		while( *name++ != '\0' );
		const idPhysics* physics = ent->GetPhysics();
		GameVerify_HashVector( entHash, physics->GetOrigin() );
		for( int axis = 0; axis < 3; axis++ )
		{
			GameVerify_HashVector( entHash, physics->GetAxis()[axis] );
		}
		GameVerify_HashVector( entHash, physics->GetLinearVelocity() );
		GameVerify_HashVector( entHash, physics->GetAngularVelocity() );
		GameVerify_HashWord( entHash, ent->health );

		if( ent->IsType( idAI::Type ) )
		{
			const idAI* ai = static_cast<const idAI*>( ent );
			const idMoveState& move = ai->GetMoveStateForDigest();
			GameVerify_HashEntity( aiHash, ent );
			GameVerify_HashWord( aiHash, move.moveType );
			GameVerify_HashWord( aiHash, move.moveCommand );
			GameVerify_HashWord( aiHash, move.moveStatus );
			GameVerify_HashVector( aiHash, move.moveDest );
			GameVerify_HashVector( aiHash, move.moveDir );
			GameVerify_HashReference( aiHash, move.goalEntity.GetSpawnId() );
			GameVerify_HashVector( aiHash, move.goalEntityOrigin );
			GameVerify_HashWord( aiHash, move.toAreaNum );
			GameVerify_HashWord( aiHash, move.startTime );
			GameVerify_HashWord( aiHash, move.duration );
			GameVerify_HashFloat( aiHash, move.speed );
			GameVerify_HashFloat( aiHash, move.range );
			GameVerify_HashFloat( aiHash, move.wanderYaw );
			GameVerify_HashWord( aiHash, move.nextWanderTime );
			GameVerify_HashWord( aiHash, move.blockTime );
			GameVerify_HashReference( aiHash, move.obstacle.GetSpawnId() );
			GameVerify_HashVector( aiHash, move.lastMoveOrigin );
			GameVerify_HashWord( aiHash, move.lastMoveTime );
			GameVerify_HashWord( aiHash, move.anim );
			GameVerify_HashReference( aiHash, ai->GetEnemySpawnIdForDigest() );
		}

		// Both GetAnimator overrides only return the stored pointer. These accessors never build a pose.
		const idAnimator* animator = ent->GetAnimator();
		if( animator != NULL )
		{
			GameVerify_HashEntity( animHash, ent );
			GameVerify_HashWord( animHash, animator->GetLastTransformTime() );
			GameVerify_HashWord( animHash, animator->GetStoppedAnimatingUpdate() ? 1 : 0 );
			const idJointMat* joints = animator->GetBuiltJoints();
			GameVerify_HashWord( animHash, animator->NumJoints() );
			GameVerify_HashWord( animHash, joints != NULL ? 1 : 0 );
			uint64 jointsHash = VERIFY_HASH_OFFSET;
			if( joints != NULL )
			{
				for( int joint = 0; joint < animator->NumJoints(); joint++ )
				{
					// idJointMat stores twelve floats; encode each value rather than its object bytes.
					for( int component = 0; component < 12; component++ )
					{
						GameVerify_HashFloat( jointsHash, joints[joint].ToFloatPtr()[component] );
					}
				}
			}
			GameVerify_HashWord( animHash, ( uint32 )jointsHash );
			GameVerify_HashWord( animHash, ( uint32 )( jointsHash >> 32 ) );
		}
	}
	GameVerify_HashWord( rngHash, gameLocal.random.GetSeed() );
	uint64 allHash = VERIFY_HASH_OFFSET;
	const uint64 hashes[] = { entHash, aiHash, animHash, rngHash };
	for( int i = 0; i < 4; i++ )
	{
		GameVerify_HashWord( allHash, ( uint32 )hashes[i] );
		GameVerify_HashWord( allHash, ( uint32 )( hashes[i] >> 32 ) );
	}
	gameLocal.Printf( "digest tic %i all=%016llx ent=%016llx ai=%016llx anim=%016llx rng=%016llx entities=%i\n",
		gameLocal.framenum, ( unsigned long long )allHash, ( unsigned long long )entHash,
		( unsigned long long )aiHash, ( unsigned long long )animHash, ( unsigned long long )rngHash, count );
}
//puncho end

/*
==================
printSmokeCount

The live recipe's positive check (GD1, V1.5): smoke was emitted when the count is above 0. The
anchor is the smoke model's render origin (WP-CR2): the eye of the first player view rendered in a
frame, snapped to 4096 units; it stays 0 0 0 without a local player. The walk's done line prints
the same line, because nothing in a cfg can follow the walk without holding its capture back.
==================
*/
CONSOLE_COMMAND( printSmokeCount, "prints the number of live smoke particles and the smoke model's anchor", NULL )
{
	GameVerify_PrintSmokeCount();
}

/*
==================
walkTics

V2.2 and V5 (GD8): see the comment at the top of this file.
==================
*/
CONSOLE_COMMAND( walkTics, "walks the player 1 unit per game tic: walkTics <n> [forward|right] [at <tic>] [stop] [every] [<capture>]", NULL )
{
	if( args.Argc() < 2 )
	{
		gameLocal.Printf( "usage: walkTics <n> [forward|right] [at <tic>] [stop] [every] [<capture>]\n" );
		return;
	}

	// no player is needed to arm it: a cfg arms the walk right after "map", before the load
	if( !gameLocal.CheatsOk( false ) )
	{
		return;
	}

	verifyWalk_t w;
	memset( &w, 0, sizeof( w ) );
	w.tics = atoi( args.Argv( 1 ) );
	w.startTic = -1;

	if( w.tics < 0 )
	{
		gameLocal.Printf( "walkTics: n must be 0 or more\n" );
		return;
	}

	for( int i = 2; i < args.Argc(); i++ )
	{
		const char* arg = args.Argv( i );
		if( idStr::Icmp( arg, "forward" ) == 0 )
		{
			w.right = false;
		}
		else if( idStr::Icmp( arg, "right" ) == 0 )
		{
			w.right = true;
		}
		else if( idStr::Icmp( arg, "stop" ) == 0 )
		{
			w.stop = true;
		}
		else if( idStr::Icmp( arg, "every" ) == 0 )
		{
			w.every = true;
		}
		else if( idStr::Icmp( arg, "at" ) == 0 && i + 1 < args.Argc() )
		{
			i++;
			w.startTic = atoi( args.Argv( i ) );
		}
		else if( w.capture[0] == '\0' )
		{
			idStr::Copynz( w.capture, arg, sizeof( w.capture ) );
		}
		else
		{
			gameLocal.Printf( "walkTics: unexpected '%s'\n", arg );
			return;
		}
	}

	if( w.every && w.capture[0] == '\0' )
	{
		gameLocal.Printf( "walkTics: 'every' needs a capture name\n" );
		return;
	}

	w.lastTic = gameLocal.framenum;
	w.armed = true;
	verifyWalk = w;

	if( w.startTic >= 0 && gameLocal.framenum >= w.startTic + ( w.tics > 0 ? 1 : 0 ) )
	{
		gameLocal.Printf( "walkTics: tic %i has passed on this map (now %i); the walk runs after the next map load\n", w.startTic, gameLocal.framenum );
	}

	gameLocal.Printf( "walkTics: %i tics %s, start tic %i, stop %i, capture '%s'%s, armed at tic %i\n", w.tics,
					  w.right ? "right" : "forward", w.startTic, w.stop ? 1 : 0, w.capture, w.every ? " every tic" : "", gameLocal.framenum );
}
