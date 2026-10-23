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
(plans/camera-relative/00-PLAN.md WP-CRV and WP-CR2, 01-VERIFY.md V1.5, V2.2 and V5). Nothing here
changes the game unless walkTics arms it. The commands are CONSOLE_COMMANDs, so they vanish in
ID_RETAIL builds; the two tic hooks stay and return at once.

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
GameVerify_EndTic

Called by idGameLocal::RunFrame after the tic's think loop and events, when the tic's state is
final.
==================
*/
void GameVerify_EndTic()
{
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

/*
==================
printSmokeCount

The live recipe's positive check (GD1, V1.5): smoke was emitted when the count is above 0. The
anchor is the smoke model's render origin (WP-CR2): the eye of the first player view rendered in a
frame, snapped to 4096 units; it stays 0 0 0 without a local player.
==================
*/
CONSOLE_COMMAND( printSmokeCount, "prints the number of live smoke particles and the smoke model's anchor", NULL )
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
