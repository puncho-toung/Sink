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

#ifndef __SCRATCHSTACK_H__
#define __SCRATCHSTACK_H__

/*
===============================================================================

	idScratchStack (D3-Sink, plans/phase1/00-PLAN.md P1-A4)

	A fixed-size LIFO arena for lists that live for one call. The game's clip and
	radius queries ask for MAX_GENTITIES entries, 128 KB of pointers, and nest through
	damage and physics; they take those lists here instead of from the thread's stack.

	The size is set by Init and never grows. Running out is an error that names the
	size cvar, the high-water mark and the scope stack, as R_FrameAlloc does it, never
	a hidden heap allocation in the middle of a tic.

	Memory goes back in reverse order through Release( mark ), normally from an
	idScratchScope, which an idException unwind releases too. A query's list is the
	top allocation, so Trim cuts it to the result count once the query has filled it:
	a nesting level then costs its results, not MAX_GENTITIES.

	Every scope carries a static name, and the arena keeps them as a stack, which the
	75% warning (once per map) and the overflow error print. It also keeps each tic's
	high-water and the peaks of the map and the session.

	threadScratchStack is the calling thread's arena. The game thread and the main
	thread point at the same one, since they never run game code at the same time.

===============================================================================
*/

const int SCRATCH_ALIGNMENT		= 16;	// Mem_Alloc16's, for idVec4 and SIMD loads
const int MAX_SCRATCH_SCOPES	= 256;	// named levels in the printed scope stack; deeper levels still work

class idScratchStack
{
	friend class idScratchScope;
public:
	idScratchStack();

	void				Init( int bytes, const char* sizeCVarName );
	void				Shutdown();

	int					Mark() const
	{
		return used;
	}
	void				Release( int mark )
	{
		assert( mark >= 0 && mark <= used );
		used = mark;
		if( top >= mark )
		{
			top = -1;
		}
	}

	// 16-byte aligned, not cleared
	template< class type >
	type*				Alloc( int num )
	{
		return ( type* )AllocBytes( num * ( int )sizeof( type ) );
	}

	// shrinks the top allocation to num elements
	template< class type >
	void				Trim( type* list, int num )
	{
		assert( top >= 0 && ( byte* )list == base + top );	// only the top allocation can shrink
		const int end = top + ( ( num * ( int )sizeof( type ) + SCRATCH_ALIGNMENT - 1 ) & ~( SCRATCH_ALIGNMENT - 1 ) );
		assert( num >= 0 && end <= used );
		used = end;
	}

	// K28: asserts that every scope is closed, and in a release build warns once per map with the scope stack
	void				CheckClosed( const char* where );
	// CheckClosed, then keeps the tic's high-water
	void				EndTic();
	// prints the map's peak and deepest scope stack and the session's peak, and re-arms the 75% warning
	void				EndMap();

	// the high-water in bytes of the last tic that EndTic closed
	int					GetTicHighWater() const
	{
		return lastTicHighWater;
	}

private:
	byte* 				base;
	const char* 		sizeCVarName;
	int					size;
	int					warnSize;			// 75% of size
	int					used;
	int					top;				// offset of the top allocation, -1 when released; checks Trim
	int					ticHighWater;
	int					lastTicHighWater;
	int					mapHighWater;
	int					sessionHighWater;
	int					numScopes;			// may pass MAX_SCRATCH_SCOPES; only the outer ones are named
	int					mapDeepest;			// most scopes open at once on this map: a damage chain's depth
	bool				warned;				// the 75% warning printed on this map
	bool				leakWarned;			// CheckClosed's warning printed on this map
	const char* 		scopeNames[MAX_SCRATCH_SCOPES];
	int					scopeMarks[MAX_SCRATCH_SCOPES];

	void* 				AllocBytes( int bytes );
	void				PrintScopes() const;

	//puncho - P1-m8 (plans/phase1/02-FIXES.md): idScratchScope on a thread with no arena; out of line, so the scope's
	// test costs a compare and a branch never taken
	NO_RETURN static void	NoArena( const char* name );
};

// the calling thread's arena; NULL on a thread that never runs game code
extern thread_local idScratchStack* threadScratchStack;

/*
===============================================================================

	idScratchScope

	Opens a named level on the calling thread's arena and releases everything
	allocated in it when it goes out of scope, on an idException unwind as well.
	The name must be a static string: the arena keeps the pointer.

===============================================================================
*/

class idScratchScope
{
public:
	explicit idScratchScope( const char* name )
	{
		stack = threadScratchStack;
		//puncho start - P1-m8: in release builds too, a named fatal error instead of a NULL dereference
		if( stack == NULL )
		{
			idScratchStack::NoArena( name );	// this thread has no arena: game code outside the game and main threads
		}
		//puncho end
		mark = stack->used;
		top = stack->top;
		if( stack->numScopes < MAX_SCRATCH_SCOPES )
		{
			stack->scopeNames[stack->numScopes] = name;
			stack->scopeMarks[stack->numScopes] = mark;
		}
		stack->numScopes++;
		if( stack->numScopes > stack->mapDeepest )
		{
			stack->mapDeepest = stack->numScopes;
		}
	}
	~idScratchScope()
	{
		assert( stack->numScopes > 0 );
		stack->numScopes--;
		stack->Release( mark );
		stack->top = top;			// the caller's list is the top allocation again, for its Trim
	}

	template< class type >
	type*				Alloc( int num )
	{
		return stack->Alloc< type >( num );
	}
	template< class type >
	void				Trim( type* list, int num )
	{
		stack->Trim( list, num );
	}

private:
	idScratchStack* 	stack;
	int					mark;
	int					top;

	idScratchScope( const idScratchScope& );
	void				operator=( const idScratchScope& );
};

#endif /* !__SCRATCHSTACK_H__ */
