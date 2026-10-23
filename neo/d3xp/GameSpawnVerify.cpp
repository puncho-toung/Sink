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

/*
================
testSpawnSort

Gate for the float spawn-spot distance. SelectInitialSpawnPoint sorts by distance only
in multiplayer with another non-spectating player present, so this runs the game's
comparator over a fixed synthetic list instead: six spots 60000 units apart on +X, two
near spots and two fake players, with the distance minimum written the way the game
writes it. Every far spot is more than 46341 units from its nearest player, where the
old int distance saturated at 0x7fffffff and tied. Each spot prints its distance as a
float and as its bits, so a script can recompute the order and check that no two tie.
================
*/
CONSOLE_COMMAND( testSpawnSort, "sorts a synthetic spawn spot list with the game's comparator and prints the distances", NULL )
{
	const int numSpots = 8;
	const int numPlayers = 2;
	const idVec3 spotOrigins[ numSpots ] =
	{
		idVec3( 60000.0f, 0.0f, 0.0f ),
		idVec3( 120000.0f, 0.0f, 0.0f ),
		idVec3( 180000.0f, 0.0f, 0.0f ),
		idVec3( 240000.0f, 0.0f, 0.0f ),
		idVec3( 300000.0f, 0.0f, 0.0f ),
		idVec3( 360000.0f, 0.0f, 0.0f ),
		idVec3( 1000.0f, 0.0f, 0.0f ),
		idVec3( 2000.0f, 0.0f, 0.0f )
	};
	// the second player is the nearest one for the three outer spots, so the order
	// interleaves the two players and the per-spot minimum is exercised
	const idVec3 playerOrigins[ numPlayers ] =
	{
		idVec3( 0.0f, 0.0f, 0.0f ),
		idVec3( 410000.0f, 0.0f, 0.0f )
	};

	idList<spawnSpot_t> spots;
	spots.SetNum( numSpots );
	for( int i = 0; i < numSpots; i++ )
	{
		spawnSpot_t& spot = spots[ i ];
		spot.ent = NULL;
		spot.dist = idMath::INFINITUM;
		spot.team = i;	// carries the index into spotOrigins through the sort
		for( int j = 0; j < numPlayers; j++ )
		{
			const float dist = ( spotOrigins[ i ] - playerOrigins[ j ] ).LengthSqr();
			if( dist < spot.dist )
			{
				spot.dist = dist;
			}
		}
	}

	qsort( ( void* )spots.Ptr(), spots.Num(), sizeof( spawnSpot_t ), ( int ( * )( const void*, const void* ) )idGameLocal::sortSpawnPoints );

	for( int j = 0; j < numPlayers; j++ )
	{
		gameLocal.Printf( "player %d origin ( %s )\n", j, playerOrigins[ j ].ToString( 1 ) );
	}

	int ties = 0;
	bool ordered = true;
	for( int i = 0; i < spots.Num(); i++ )
	{
		const spawnSpot_t& spot = spots[ i ];
		unsigned int bits;
		memcpy( &bits, &spot.dist, sizeof( bits ) );
		gameLocal.Printf( "%d: spot %d origin ( %s ) dist %.1f bits 0x%08X\n", i, spot.team, spotOrigins[ spot.team ].ToString( 1 ), spot.dist, bits );
		if( i > 0 && spots[ i - 1 ].dist == spot.dist )
		{
			ties++;
		}
		if( i > 0 && spots[ i - 1 ].dist < spot.dist )
		{
			ordered = false;
		}
	}
	gameLocal.Printf( "testSpawnSort: %d spots, %d ties, %s\n", spots.Num(), ties, ordered ? "farthest first" : "ORDER BROKEN" );
}
