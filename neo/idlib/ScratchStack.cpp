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

thread_local idScratchStack* threadScratchStack = NULL;

/*
========================
idScratchStack::idScratchStack
========================
*/
idScratchStack::idScratchStack()
{
	base = NULL;
	sizeCVarName = "";
	size = 0;
	warnSize = 0;
	used = 0;
	top = -1;
	ticHighWater = 0;
	lastTicHighWater = 0;
	mapHighWater = 0;
	sessionHighWater = 0;
	numScopes = 0;
	mapDeepest = 0;
	warned = false;
	leakWarned = false;
}

/*
========================
idScratchStack::Init
========================
*/
void idScratchStack::Init( int bytes, const char* sizeCVarName_ )
{
	assert( base == NULL );
	size = bytes & ~( SCRATCH_ALIGNMENT - 1 );
	warnSize = size / 4 * 3;
	sizeCVarName = sizeCVarName_;
	base = ( byte* )Mem_Alloc16( size, TAG_GAME );
	if( base == NULL )
	{
		idLib::FatalError( "idScratchStack::Init: could not allocate %d bytes (%s)", size, sizeCVarName );
	}
}

/*
========================
idScratchStack::Shutdown
========================
*/
void idScratchStack::Shutdown()
{
	assert( used == 0 );
	Mem_Free16( base );
	base = NULL;
	size = 0;
}

/*
========================
idScratchStack::AllocBytes

The high-water counts a list before its Trim: a query's MAX_GENTITIES list is
really there until the query has filled it.
========================
*/
void* idScratchStack::AllocBytes( int bytes )
{
	assert( base != NULL && bytes >= 0 );

	const int aligned = ( bytes + SCRATCH_ALIGNMENT - 1 ) & ~( SCRATCH_ALIGNMENT - 1 );
	if( aligned > size - used )
	{
		// fixed on purpose: a list that does not fit is a limit to raise, not memory to find mid-tic
		PrintScopes();
		idLib::Error( "idScratchStack: out of memory: %d bytes asked with %d of %d in use, session high-water %d; %s sets the size in KB",
					  bytes, used, size, Max( sessionHighWater, Max( mapHighWater, ticHighWater ) ), sizeCVarName );
	}

	top = used;
	used += aligned;

	if( used > ticHighWater )
	{
		ticHighWater = used;
		if( used > warnSize && !warned )
		{
			warned = true;
			idLib::Warning( "idScratchStack: %d of %d KB in use, past 75%% of %s", used >> 10, size >> 10, sizeCVarName );
			PrintScopes();
		}
	}

	return base + top;
}

/*
========================
idScratchStack::CheckClosed

K28: every scope is closed. A leak is a bug to find, so it is never reset: the
assert stops a debug build, and a release build, which compiles the assert out,
pays one compare and warns once per map with the scope stack (phase 12's rule
that a stopgap lands with its log).
========================
*/
void idScratchStack::CheckClosed( const char* where )
{
	assert( used == 0 && numScopes == 0 );

	if( ( used | numScopes ) != 0 && !leakWarned )
	{
		leakWarned = true;
		idLib::Warning( "idScratchStack: %d bytes and %d scopes still open at %s: a scope was not closed", used, numScopes, where );
		PrintScopes();
	}
}

/*
========================
idScratchStack::EndTic
========================
*/
void idScratchStack::EndTic()
{
	CheckClosed( "tic end" );

	lastTicHighWater = ticHighWater;
	mapHighWater = Max( mapHighWater, ticHighWater );
	ticHighWater = 0;
}

/*
========================
idScratchStack::EndMap

Allocations since the last EndTic are folded in first: the map load and its settle
frames run game code without one.
========================
*/
void idScratchStack::EndMap()
{
	mapHighWater = Max( mapHighWater, ticHighWater );
	ticHighWater = 0;
	sessionHighWater = Max( sessionHighWater, mapHighWater );

	idLib::Printf( "idScratchStack: map peak %d KB (%d%%), deepest %d scopes, session peak %d KB, of %d KB (%s)\n",
				   mapHighWater >> 10, ( int )( ( int64 )mapHighWater * 100 / Max( size, 1 ) ), mapDeepest,
				   sessionHighWater >> 10, size >> 10, sizeCVarName );

	mapHighWater = 0;
	mapDeepest = 0;
	warned = false;
	leakWarned = false;
}

/*
========================
idScratchStack::PrintScopes
========================
*/
void idScratchStack::PrintScopes() const
{
	const int numNamed = Min( numScopes, MAX_SCRATCH_SCOPES );

	idLib::Printf( "idScratchStack: %d scopes, outermost first (mark in bytes):\n", numScopes );
	for( int i = 0; i < numNamed; i++ )
	{
		idLib::Printf( "  %3d %8d %s\n", i, scopeMarks[i], scopeNames[i] );
	}
	if( numScopes > numNamed )
	{
		idLib::Printf( "  ... %d deeper scopes not named\n", numScopes - numNamed );
	}
	idLib::Printf( "  %d of %d bytes in use\n", used, size );
}
