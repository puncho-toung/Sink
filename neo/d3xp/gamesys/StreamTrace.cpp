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
#include "StreamTrace.h"

// P1-A1c, the stream-trace checker (MP-A1 tier 1); the contracts are plans/phase1/00-PLAN.md K1-K6

static const int	MAX_REPORTED = 16;		// findings printed in full per check; the rest are counted

static const int	STREAMTRACE_FILE_ID			= ( 'S' << 24 ) | ( 'T' << 16 ) | ( 'R' << 8 ) | 'C';
static const int	STREAMTRACE_FILE_VERSION	= 1;
static const int	SNAPTRACE_FILE_ID			= ( 'S' << 24 ) | ( 'N' << 16 ) | ( 'T' << 8 ) | 'R';
static const int	SNAPTRACE_FILE_VERSION		= 1;

static const char* streamPrimitiveNames[ SP_NUM_PRIMITIVES ] =
{
	"SCOPE", "OPAQUE", "RAW", "INT", "JOINT", "SHORT", "BYTE", "SIGNEDCHAR", "FLOAT", "BOOL", "STRING",
	"VEC2", "VEC3", "VEC4", "VEC6", "WINDING", "BOUNDS", "MAT3", "ANGLES", "OBJECT"
};

static const char* bitMsgPrimitiveNames[ BITMSG_NUM_PRIMITIVES ] =
{
	"BITS", "BOOL", "CHAR", "BYTE", "SHORT", "USHORT", "LONG", "LONGLONG", "FLOAT", "FLOATBITS", "ANGLE8", "ANGLE16",
	"DIR", "STRING", "DATA"
};

/*
The byte allowlist of the save round trip (K3). A Restore re-acquires render handles through Add*Def, which hands
them out with FindNull in restore order, so every int it overwrites that way may differ between the two saves; so
may six named fields and numEntitiesToDeactivate. A rule names the scope it applies to and the positions in it,
counted from the scope's start or from the child scopes it anchors on (each one, or only the n-th from the last):
delta entries after the anchor (negative: before it), count entries. A delta of 0 on an anchor allows the child.
*/
typedef struct streamTraceAllow_s
{
	const char* 		scope;
	const char* 		anchor;		// NULL: count from the scope's start
	int					fromEnd;	// 0: every anchor; n: the n-th anchor from the last
	int					delta;
	int					count;
	const char* 		what;
} streamTraceAllow_t;

static const streamTraceAllow_t allowRules[] =
{
	{ "idEntity",			"renderEntity_t",	0,	1,	1,	"idEntity xrayEntityHandle, modelDefHandle (Entity.cpp:846/850)" },
	{ "idExplodingBarrel",	"renderEntity_t",	0,	-2,	2,	"idExplodingBarrel particleModelDefHandle, lightDefHandle (Moveable.cpp:956-957)" },
	{ "idProjectile",		"renderLight_t",	0,	1,	1,	"idProjectile lightDefHandle (Projectile.cpp:141)" },
	{ "idBFGProjectile",	"renderEntity_t",	0,	1,	1,	"idBFGProjectile beam and second model handles (Projectile.cpp:2373/2377)" },
	{ "idWeapon",			"renderLight_t",	0,	-1,	1,	"idWeapon light and flash handles (Weapon.cpp:371-477)" },
	{ "idClipModel",		NULL,				0,	12,	2,	"idClipModel renderModelHandle and link flag (Clip.cpp:542-543)" },
	{ "<globals>",			"usercmd_t",		0,	0,	0,	"the dummy usercmd" },
	{ "<globals>",			"idDict",			1,	1,	4,	"playerPVS, playerConnectedAreas" },
	{ "<globals>",			"idDict",			2,	MAX_GLOBAL_SHADER_PARMS + 15,	2,	"GetLocalClientNum, realClientTime" },
	{ "<globals>",			"idDict",			2,	-3,	1,	"numEntitiesToDeactivate (Game_local.cpp:586/1567)" },
};
static const int NUM_ALLOW_RULES = sizeof( allowRules ) / sizeof( allowRules[ 0 ] );

/*
===============================================================================

	idFile_StreamTrace

===============================================================================
*/

/*
================
idFile_StreamTrace::idFile_StreamTrace
================
*/
idFile_StreamTrace::idFile_StreamTrace()
{
	file = NULL;
	capture = NULL;
	count = 0;
}

/*
================
idFile_StreamTrace::GetName
================
*/
const char* idFile_StreamTrace::GetName() const
{
	return ( file != NULL ) ? file->GetName() : "";
}

/*
================
idFile_StreamTrace::GetFullPath
================
*/
const char* idFile_StreamTrace::GetFullPath() const
{
	return ( file != NULL ) ? file->GetFullPath() : "";
}

/*
================
idFile_StreamTrace::Read

The count moves by what was asked, as the stream's position does in the entries' arithmetic.
================
*/
int idFile_StreamTrace::Read( void* buffer, int len )
{
	count += len;
	return file->Read( buffer, len );
}

/*
================
idFile_StreamTrace::Write
================
*/
int idFile_StreamTrace::Write( const void* buffer, int len )
{
	if( capture != NULL && len > 0 )
	{
		const int num = capture->Num();
		if( num + len > capture->NumAllocated() )
		{
			capture->Resize( ( num + len ) * 2 );
		}
		capture->SetNum( num + len );
		memcpy( capture->Ptr() + num, buffer, len );
	}
	count += len;
	return file->Write( buffer, len );
}

/*
================
idFile_StreamTrace::Length
================
*/
int idFile_StreamTrace::Length() const
{
	return file->Length();
}

/*
================
idFile_StreamTrace::Timestamp
================
*/
ID_TIME_T idFile_StreamTrace::Timestamp() const
{
	return file->Timestamp();
}

/*
================
idFile_StreamTrace::Tell
================
*/
int idFile_StreamTrace::Tell() const
{
	return count;
}

/*
================
idFile_StreamTrace::ForceFlush
================
*/
void idFile_StreamTrace::ForceFlush()
{
	file->ForceFlush();
}

/*
================
idFile_StreamTrace::Flush
================
*/
void idFile_StreamTrace::Flush()
{
	file->Flush();
}

/*
================
idFile_StreamTrace::Seek
================
*/
int idFile_StreamTrace::Seek( long offset, fsOrigin_t origin )
{
	return file->Seek( offset, origin );
}

/*
===============================================================================

	idStreamTrace

===============================================================================
*/

idStreamTrace* idStreamTrace::armed = NULL;

/*
================
idStreamTrace::idStreamTrace
================
*/
idStreamTrace::idStreamTrace()
{
	scopes.SetGranularity( 4096 );
	entries.SetGranularity( 65536 );
	object = -1;
	structEntered = false;
}

/*
================
idStreamTrace::Clear
================
*/
void idStreamTrace::Clear()
{
	names.Clear();
	nameHash.Clear();
	scopes.Clear();
	entries.Clear();
	stack.Clear();
	bytes.Clear();
	counter.file = NULL;
	counter.count = 0;
	object = -1;
	structEntered = false;
}

/*
================
idStreamTrace::SetCapture
================
*/
void idStreamTrace::SetCapture( bool keepBytes )
{
	counter.capture = keepBytes ? &bytes : NULL;
}

/*
================
idStreamTrace::Attach

The stream's start: everything until the first pseudo-scope or object belongs to <globals>.
================
*/
idFile* idStreamTrace::Attach( idFile* streamFile )
{
	Clear();
	counter.file = streamFile;
	BeginScope( "<globals>" );
	return &counter;
}

/*
================
idStreamTrace::Finish
================
*/
void idStreamTrace::Finish()
{
	stack.Clear();
	counter.file = NULL;
	object = -1;
	structEntered = false;
}

/*
================
idStreamTrace::SetObject
================
*/
void idStreamTrace::SetObject( int objectIndex )
{
	object = objectIndex;
}

/*
================
idStreamTrace::NameIndex
================
*/
int idStreamTrace::NameIndex( const char* name )
{
	const int key = nameHash.GenerateKey( name );
	for( int i = nameHash.First( key ); i != -1; i = nameHash.Next( i ) )
	{
		if( names[ i ].Cmp( name ) == 0 )
		{
			return i;
		}
	}
	const int index = names.Append( name );
	nameHash.Add( key, index );
	return index;
}

/*
================
idStreamTrace::BeginScope

A child scope shows in its parent as one SP_SCOPE entry.
================
*/
void idStreamTrace::BeginScope( const char* name )
{
	const int scope = scopes.Num();
	const int parent = ( stack.Num() > 0 ) ? stack[ stack.Num() - 1 ] : -1;

	streamTraceScope_t& s = scopes.Alloc();
	s.name = NameIndex( name );
	s.object = object;
	s.parent = parent;

	if( parent >= 0 )
	{
		streamTraceEntry_t& e = entries.Alloc();
		e.scope = parent;
		e.offset = counter.count;
		e.size = 0;
		e.hash = StringHash( name );
		e.child = scope;
		e.type = SP_SCOPE;
	}

	stack.Append( scope );
}

/*
================
idStreamTrace::EndScope
================
*/
void idStreamTrace::EndScope()
{
	if( stack.Num() > 0 )
	{
		stack.SetNum( stack.Num() - 1 );
	}
}

/*
================
idStreamTrace::EnterStruct

A struct writer opens its scope and calls itself, which runs its body with the scope open; the flag tells the inner
call it is the inner one. An untraced stream never gets here, so the writer still costs one pointer test.
================
*/
bool idStreamTrace::EnterStruct( const char* name )
{
	if( structEntered )
	{
		structEntered = false;
		return false;
	}
	BeginScope( name );
	structEntered = true;
	return true;
}

/*
================
idStreamTrace::Primitive
================
*/
void idStreamTrace::Primitive( streamPrimitive_t type, int size, unsigned int hash )
{
	streamTraceEntry_t& e = entries.Alloc();
	e.scope = ( stack.Num() > 0 ) ? stack[ stack.Num() - 1 ] : -1;
	e.offset = counter.count - size;
	e.size = size;
	e.hash = hash;
	e.child = -1;
	e.type = type;
}

/*
================
idStreamTrace::Opaque
================
*/
void idStreamTrace::Opaque( int start )
{
	Primitive( SP_OPAQUE, counter.count - start, 0 );
}

/*
================
idStreamTrace::StringHash
================
*/
unsigned int idStreamTrace::StringHash( const char* text )
{
	if( text == NULL || text[ 0 ] == '\0' )
	{
		return 0;
	}
	return CRC32_BlockChecksum( text, idStr::Length( text ) );
}

/*
================
idStreamTrace::IndexScopes

Groups the entries by scope, keeping stream order: scope s owns order[ first[ s ] ] .. order[ first[ s + 1 ] - 1 ].
================
*/
void idStreamTrace::IndexScopes( idList<int>& first, idList<int>& order ) const
{
	first.SetNum( scopes.Num() + 1 );
	memset( first.Ptr(), 0, first.Num() * sizeof( int ) );
	for( int i = 0; i < entries.Num(); i++ )
	{
		if( entries[ i ].scope >= 0 )
		{
			first[ entries[ i ].scope + 1 ]++;
		}
	}
	for( int s = 1; s < first.Num(); s++ )
	{
		first[ s ] += first[ s - 1 ];
	}

	idList<int> next;
	next.SetNum( scopes.Num() );
	for( int s = 0; s < scopes.Num(); s++ )
	{
		next[ s ] = first[ s ];
	}
	order.SetNum( first[ scopes.Num() ] );
	for( int i = 0; i < entries.Num(); i++ )
	{
		const int s = entries[ i ].scope;
		if( s >= 0 )
		{
			order[ next[ s ]++ ] = i;
		}
	}
}

/*
================
idStreamTrace::SameShape

Compares one scope of this trace with one of another: name, object, and each entry's type and size, plus a string's
hash when compareHashes is set (a child scope's name always). position is the first entry that differs.
================
*/
bool idStreamTrace::SameShape( const idStreamTrace& other, int scope, int otherScope, const idList<int>& first, const idList<int>& order, const idList<int>& otherFirst, const idList<int>& otherOrder, bool compareHashes, int& position ) const
{
	position = 0;
	if( idStr::Cmp( ScopeName( scope ), other.ScopeName( otherScope ) ) != 0 || scopes[ scope ].object != other.scopes[ otherScope ].object )
	{
		return false;
	}
	const int num = first[ scope + 1 ] - first[ scope ];
	const int otherNum = otherFirst[ otherScope + 1 ] - otherFirst[ otherScope ];
	for( position = 0; position < num && position < otherNum; position++ )
	{
		const streamTraceEntry_t& a = entries[ order[ first[ scope ] + position ] ];
		const streamTraceEntry_t& b = other.entries[ otherOrder[ otherFirst[ otherScope ] + position ] ];
		if( a.type != b.type || a.size != b.size )
		{
			return false;
		}
		if( ( compareHashes || a.type == SP_SCOPE ) && a.hash != b.hash )
		{
			return false;
		}
	}
	return ( num == otherNum );
}

/*
================
idStreamTrace::DescribeEntry
================
*/
void idStreamTrace::DescribeEntry( const streamTraceEntry_t* entry, idStr& out ) const
{
	if( entry == NULL )
	{
		out = "nothing";
	}
	else if( entry->type == SP_SCOPE )
	{
		out = va( "SCOPE %s", ScopeName( entry->child ) );
	}
	else if( entry->type == SP_STRING )
	{
		out = va( "STRING %d #%08x", entry->size, entry->hash );
	}
	else
	{
		out = va( "%s %d", ( entry->type < SP_NUM_PRIMITIVES ) ? streamPrimitiveNames[ entry->type ] : "?", entry->size );
	}
}

/*
================
idStreamTrace::CheckCoverage

Every byte of the stream must belong to exactly one entry; a gap is a path that wrote around the primitives.
================
*/
int idStreamTrace::CheckCoverage() const
{
	int failures = 0;
	int expected = 0;
	for( int i = 0; i < entries.Num(); i++ )
	{
		const streamTraceEntry_t& e = entries[ i ];
		if( e.offset != expected )
		{
			if( ++failures <= MAX_REPORTED )
			{
				gameLocal.Printf( "  untraced bytes: %d at offset %d, before an entry of %s\n", e.offset - expected, expected, ( e.scope >= 0 ) ? ScopeName( e.scope ) : "no scope" );
			}
		}
		expected = e.offset + e.size;
	}
	if( expected != counter.count )
	{
		failures++;
		gameLocal.Printf( "  untraced bytes: %d at the end of the stream (offset %d)\n", counter.count - expected, expected );
	}
	return failures;
}

/*
================
idStreamTrace::CompareShapes

K3, first half: each scope's read trace against its write trace, in the order the scopes opened.
================
*/
int idStreamTrace::CompareShapes( const idStreamTrace& write, const idStreamTrace& read )
{
	idList<int> writeFirst, writeOrder, readFirst, readOrder;
	write.IndexScopes( writeFirst, writeOrder );
	read.IndexScopes( readFirst, readOrder );

	int failures = 0;
	if( write.scopes.Num() != read.scopes.Num() )
	{
		failures++;
		gameLocal.Printf( "  scope count differs: %d written, %d read\n", write.scopes.Num(), read.scopes.Num() );
	}

	const int num = Min( write.scopes.Num(), read.scopes.Num() );
	for( int s = 0; s < num; s++ )
	{
		int position;
		if( write.SameShape( read, s, s, writeFirst, writeOrder, readFirst, readOrder, true, position ) )
		{
			continue;
		}
		if( ++failures > MAX_REPORTED )
		{
			continue;
		}

		const int writeNum = writeFirst[ s + 1 ] - writeFirst[ s ];
		const int readNum = readFirst[ s + 1 ] - readFirst[ s ];
		const streamTraceEntry_t* w = ( position < writeNum ) ? &write.entries[ writeOrder[ writeFirst[ s ] + position ] ] : NULL;
		const streamTraceEntry_t* r = ( position < readNum ) ? &read.entries[ readOrder[ readFirst[ s ] + position ] ] : NULL;
		idStr wrote, readBack;
		write.DescribeEntry( w, wrote );
		read.DescribeEntry( r, readBack );
		const int parent = write.scopes[ s ].parent;
		gameLocal.Printf( "  MISMATCH scope %d, object %d %s (in %s) / read as object %d %s: position %d (offset %d): wrote %s, read %s; %d entries written, %d read\n",
						  s, write.scopes[ s ].object, write.ScopeName( s ), ( parent >= 0 ) ? write.ScopeName( parent ) : "-",
						  read.scopes[ s ].object, read.ScopeName( s ), position, ( w != NULL ) ? w->offset : -1, wrote.c_str(), readBack.c_str(), writeNum, readNum );
	}
	return failures;
}

/*
================
idStreamTrace::RuleCovers
================
*/
bool idStreamTrace::RuleCovers( int rule, int scope, int position, const idList<int>& first, const idList<int>& order ) const
{
	const streamTraceAllow_t& r = allowRules[ rule ];
	if( idStr::Cmp( ScopeName( scope ), r.scope ) != 0 )
	{
		return false;
	}
	if( r.anchor == NULL )
	{
		return ( position >= r.delta && position < r.delta + r.count );
	}

	const int start = first[ scope ];
	const int num = first[ scope + 1 ] - start;

	int numAnchors = 0;
	for( int p = 0; p < num; p++ )
	{
		const streamTraceEntry_t& e = entries[ order[ start + p ] ];
		if( e.type == SP_SCOPE && idStr::Cmp( ScopeName( e.child ), r.anchor ) == 0 )
		{
			numAnchors++;
		}
	}

	int anchor = 0;
	for( int p = 0; p < num; p++ )
	{
		const streamTraceEntry_t& e = entries[ order[ start + p ] ];
		if( e.type != SP_SCOPE || idStr::Cmp( ScopeName( e.child ), r.anchor ) != 0 )
		{
			continue;
		}
		anchor++;
		if( r.fromEnd != 0 && anchor != numAnchors - r.fromEnd + 1 )
		{
			continue;
		}
		if( r.delta == 0 )
		{
			if( position == p )
			{
				return true;
			}
		}
		else if( position >= p + r.delta && position < p + r.delta + r.count )
		{
			return true;
		}
	}
	return false;
}

/*
================
idStreamTrace::IsAllowed

An entry is allowed by a rule on its own scope, or because a rule on an enclosing scope allows a whole child.
================
*/
bool idStreamTrace::IsAllowed( int scope, int position, const idList<int>& first, const idList<int>& order, int& rule ) const
{
	for( rule = 0; rule < NUM_ALLOW_RULES; rule++ )
	{
		if( allowRules[ rule ].delta != 0 || allowRules[ rule ].anchor == NULL )
		{
			if( RuleCovers( rule, scope, position, first, order ) )
			{
				return true;
			}
		}
	}

	for( int child = scope; scopes[ child ].parent >= 0; child = scopes[ child ].parent )
	{
		const int parent = scopes[ child ].parent;
		const int start = first[ parent ];
		const int num = first[ parent + 1 ] - start;
		for( int p = 0; p < num; p++ )
		{
			const streamTraceEntry_t& e = entries[ order[ start + p ] ];
			if( e.type != SP_SCOPE || e.child != child )
			{
				continue;
			}
			for( rule = 0; rule < NUM_ALLOW_RULES; rule++ )
			{
				if( allowRules[ rule ].delta == 0 && allowRules[ rule ].anchor != NULL && RuleCovers( rule, parent, p, first, order ) )
				{
					return true;
				}
			}
			break;
		}
	}
	rule = -1;
	return false;
}

/*
================
CountGroup
================
*/
static void CountGroup( idStrList& groups, idList<int>& counts, idHashIndex& hash, const char* key )
{
	const int k = hash.GenerateKey( key );
	for( int g = hash.First( k ); g != -1; g = hash.Next( g ) )
	{
		if( groups[ g ].Cmp( key ) == 0 )
		{
			counts[ g ]++;
			return;
		}
	}
	const int g = groups.Append( key );
	counts.Append( 1 );
	hash.Add( k, g );
}

/*
================
idStreamTrace::CompareBytes

K3, second half: two saves of the same state, byte for byte through their traces, apart from the allowlist. A
string compares by its text: its bytes are a table offset, which moves for every string the other save added first,
and the tables follow from the texts. Where the two saves stop holding the same objects in the same order, the
scopes no longer pair: that point is one failure, and nothing after it is compared. Both traces must have been
captured.
================
*/
int idStreamTrace::CompareBytes( const idStreamTrace& a, const idStreamTrace& b, const idFile_Memory& aStrings, const idFile_Memory& bStrings )
{
	idList<int> aFirst, aOrder, bFirst, bOrder;
	a.IndexScopes( aFirst, aOrder );
	b.IndexScopes( bFirst, bOrder );

	int allowed[ NUM_ALLOW_RULES ];
	memset( allowed, 0, sizeof( allowed ) );

	// every failure also counts under its (scope, position, entry), so the whole tally prints however many there are
	idStrList groups;
	idList<int> groupCounts;
	idHashIndex groupHash;

	int failures = 0;
	int num = Min( a.scopes.Num(), b.scopes.Num() );
	for( int s = 0; s < num; s++ )
	{
		if( a.scopes[ s ].object != b.scopes[ s ].object || idStr::Cmp( a.ScopeName( s ), b.ScopeName( s ) ) != 0 )
		{
			gameLocal.Printf( "  PARTED: from scope %d on the saves hold different objects: object %d %s, then object %d %s; %d and %d scopes in all\n",
							  s, a.scopes[ s ].object, a.ScopeName( s ), b.scopes[ s ].object, b.ScopeName( s ), a.scopes.Num(), b.scopes.Num() );
			failures++;
			num = s;
			break;
		}
	}
	if( num == Min( a.scopes.Num(), b.scopes.Num() ) && a.scopes.Num() != b.scopes.Num() )
	{
		const idStreamTrace& longer = ( a.scopes.Num() > b.scopes.Num() ) ? a : b;
		gameLocal.Printf( "  PARTED: only the %s save goes on after scope %d, object %d %s\n", ( &longer == &a ) ? "first" : "second", num, longer.scopes[ num ].object, longer.ScopeName( num ) );
		failures++;
	}

	for( int s = 0; s < num; s++ )
	{
		int position;
		if( !a.SameShape( b, s, s, aFirst, aOrder, bFirst, bOrder, false, position ) )
		{
			const int aNum = aFirst[ s + 1 ] - aFirst[ s ];
			const int bNum = bFirst[ s + 1 ] - bFirst[ s ];
			idStr first, second;
			a.DescribeEntry( ( position < aNum ) ? &a.entries[ aOrder[ aFirst[ s ] + position ] ] : NULL, first );
			b.DescribeEntry( ( position < bNum ) ? &b.entries[ bOrder[ bFirst[ s ] + position ] ] : NULL, second );
			if( ++failures <= MAX_REPORTED )
			{
				gameLocal.Printf( "  DIFFERENT SHAPE scope %d, object %d %s / object %d %s: position %d: %s, then %s; %d entries, then %d\n", s, a.scopes[ s ].object, a.ScopeName( s ),
								  b.scopes[ s ].object, b.ScopeName( s ), position, first.c_str(), second.c_str(), aNum, bNum );
			}
			CountGroup( groups, groupCounts, groupHash, va( "shape of %s at position %d", a.ScopeName( s ), position ) );
			continue;
		}

		const int numEntries = aFirst[ s + 1 ] - aFirst[ s ];
		for( int p = 0; p < numEntries; p++ )
		{
			const streamTraceEntry_t& ea = a.entries[ aOrder[ aFirst[ s ] + p ] ];
			const streamTraceEntry_t& eb = b.entries[ bOrder[ bFirst[ s ] + p ] ];
			if( ea.type == SP_SCOPE )
			{
				continue;
			}
			if( ea.offset + ea.size > a.bytes.Num() || eb.offset + eb.size > b.bytes.Num() )
			{
				if( ++failures <= MAX_REPORTED )
				{
					gameLocal.Printf( "  scope %d, object %d %s: position %d lies outside the captured stream\n", s, a.scopes[ s ].object, a.ScopeName( s ), p );
				}
				continue;
			}
			if( ea.hash == eb.hash && ( ea.type == SP_STRING || memcmp( a.bytes.Ptr() + ea.offset, b.bytes.Ptr() + eb.offset, ea.size ) == 0 ) )
			{
				continue;
			}

			int rule;
			if( a.IsAllowed( s, p, aFirst, aOrder, rule ) )
			{
				allowed[ rule ]++;
				continue;
			}

			idStr what;
			a.DescribeEntry( &ea, what );
			const int parent = a.scopes[ s ].parent;
			CountGroup( groups, groupCounts, groupHash, va( "%s (in %s) position %d, %s", a.ScopeName( s ), ( parent >= 0 ) ? a.ScopeName( parent ) : "-", p, streamPrimitiveNames[ ea.type ] ) );
			if( ++failures <= MAX_REPORTED )
			{
				idStr values;
				if( ea.size == 4 )
				{
					int first, second;
					memcpy( &first, a.bytes.Ptr() + ea.offset, 4 );
					memcpy( &second, b.bytes.Ptr() + eb.offset, 4 );
					values = va( ": %d, then %d", BigLong( first ), BigLong( second ) );
				}
				else if( ea.size == 1 )
				{
					values = va( ": %d, then %d", a.bytes[ ea.offset ], b.bytes[ eb.offset ] );
				}
				gameLocal.Printf( "  BYTES DIFFER scope %d, object %d %s (in %s): position %d (offset %d), %s%s\n", s, a.scopes[ s ].object, a.ScopeName( s ),
								  ( parent >= 0 ) ? a.ScopeName( parent ) : "-", p, ea.offset, what.c_str(), values.c_str() );
			}
		}
	}

	if( aStrings.Length() != bStrings.Length() || memcmp( aStrings.GetDataPtr(), bStrings.GetDataPtr(), aStrings.Length() ) != 0 )
	{
		int at = 0;
		const int shorter = Min( aStrings.Length(), bStrings.Length() );
		while( at < shorter && aStrings.GetDataPtr()[ at ] == bStrings.GetDataPtr()[ at ] )
		{
			at++;
		}
		gameLocal.Printf( "  (the string tables differ from byte %d on, %d and %d bytes; the strings above say why)\n", at, aStrings.Length(), bStrings.Length() );
	}

	for( int g = 0; g < groups.Num() && g < 4 * MAX_REPORTED; g++ )
	{
		gameLocal.Printf( "  failed: %5d x %s\n", groupCounts[ g ], groups[ g ].c_str() );
	}
	if( groups.Num() > 4 * MAX_REPORTED )
	{
		gameLocal.Printf( "  failed: and %d more kinds\n", groups.Num() - 4 * MAX_REPORTED );
	}
	for( int r = 0; r < NUM_ALLOW_RULES; r++ )
	{
		if( allowed[ r ] > 0 )
		{
			gameLocal.Printf( "  allowed: %5d x %s\n", allowed[ r ], allowRules[ r ].what );
		}
	}
	return failures;
}

/*
================
idStreamTrace::CompareClasses

K5: a trace saved by an earlier build against this one. Scopes pair by (object, name, n-th such scope of the
object); each pair compares type and size per position, never values, and the findings add up per class.
================
*/
typedef struct streamTraceClassStats_s
{
	int					paired;
	int					differ;
	int					onlyBefore;
	int					onlyNow;
	int					object;			// the first difference
	int					position;
	idStr				before;
	idStr				now;
} streamTraceClassStats_t;

static int ScopeKey( const char* name, int object )
{
	return idStr::Hash( name ) + object * 31;
}

int idStreamTrace::CompareClasses( const idStreamTrace& before, const idStreamTrace& now )
{
	idList<int> beforeFirst, beforeOrder, nowFirst, nowOrder;
	before.IndexScopes( beforeFirst, beforeOrder );
	now.IndexScopes( nowFirst, nowOrder );

	// the ordinal of each earlier scope among those of its object with its name
	idHashIndex beforeHash( 65536, Max( before.scopes.Num(), 1 ) );
	idList<int> beforeOrdinal;
	beforeOrdinal.SetNum( before.scopes.Num() );
	for( int s = 0; s < before.scopes.Num(); s++ )
	{
		const int key = ScopeKey( before.ScopeName( s ), before.scopes[ s ].object );
		int ordinal = 0;
		for( int o = beforeHash.First( key ); o != -1; o = beforeHash.Next( o ) )
		{
			if( before.scopes[ o ].object == before.scopes[ s ].object && idStr::Cmp( before.ScopeName( o ), before.ScopeName( s ) ) == 0 )
			{
				ordinal++;
			}
		}
		beforeOrdinal[ s ] = ordinal;
		beforeHash.Add( key, s );
	}

	idStrList classNames;
	idHashIndex classHash;
	idList<streamTraceClassStats_t> stats;
	idList<bool> beforePaired;
	beforePaired.SetNum( before.scopes.Num() );
	for( int s = 0; s < beforePaired.Num(); s++ )
	{
		beforePaired[ s ] = false;
	}

	idHashIndex nowHash( 65536, Max( now.scopes.Num(), 1 ) );
	for( int s = 0; s < now.scopes.Num(); s++ )
	{
		const char* name = now.ScopeName( s );
		const int object = now.scopes[ s ].object;
		const int key = ScopeKey( name, object );

		int ordinal = 0;
		for( int o = nowHash.First( key ); o != -1; o = nowHash.Next( o ) )
		{
			if( now.scopes[ o ].object == object && idStr::Cmp( now.ScopeName( o ), name ) == 0 )
			{
				ordinal++;
			}
		}
		nowHash.Add( key, s );

		int match = -1;
		for( int o = beforeHash.First( key ); o != -1; o = beforeHash.Next( o ) )
		{
			if( before.scopes[ o ].object == object && beforeOrdinal[ o ] == ordinal && idStr::Cmp( before.ScopeName( o ), name ) == 0 )
			{
				match = o;
				break;
			}
		}

		const int classKey = classHash.GenerateKey( name );
		int c = -1;
		for( int i = classHash.First( classKey ); i != -1; i = classHash.Next( i ) )
		{
			if( classNames[ i ].Cmp( name ) == 0 )
			{
				c = i;
				break;
			}
		}
		if( c < 0 )
		{
			c = classNames.Append( name );
			classHash.Add( classKey, c );
			streamTraceClassStats_t& n = stats.Alloc();
			n.paired = n.differ = n.onlyBefore = n.onlyNow = 0;
			n.object = n.position = -1;
		}
		streamTraceClassStats_t& st = stats[ c ];

		if( match < 0 )
		{
			st.onlyNow++;
			continue;
		}
		beforePaired[ match ] = true;
		st.paired++;

		int position;
		if( now.SameShape( before, s, match, nowFirst, nowOrder, beforeFirst, beforeOrder, false, position ) )
		{
			continue;
		}
		if( st.differ++ == 0 )
		{
			const int nowNum = nowFirst[ s + 1 ] - nowFirst[ s ];
			const int beforeNum = beforeFirst[ match + 1 ] - beforeFirst[ match ];
			st.object = object;
			st.position = position;
			before.DescribeEntry( ( position < beforeNum ) ? &before.entries[ beforeOrder[ beforeFirst[ match ] + position ] ] : NULL, st.before );
			now.DescribeEntry( ( position < nowNum ) ? &now.entries[ nowOrder[ nowFirst[ s ] + position ] ] : NULL, st.now );
		}
	}

	int onlyBefore = 0;
	for( int s = 0; s < before.scopes.Num(); s++ )
	{
		if( !beforePaired[ s ] )
		{
			onlyBefore++;
		}
	}

	int classesDiffer = 0;
	for( int c = 0; c < classNames.Num(); c++ )
	{
		const streamTraceClassStats_t& st = stats[ c ];
		if( st.differ == 0 && st.onlyNow == 0 )
		{
			continue;
		}
		classesDiffer++;
		if( st.differ > 0 )
		{
			gameLocal.Printf( "  %s: %d of %d paired scopes differ; first at object %d, position %d: was %s, now %s%s\n", classNames[ c ].c_str(), st.differ, st.paired,
							  st.object, st.position, st.before.c_str(), st.now.c_str(), ( st.onlyNow > 0 ) ? va( "; %d only in this build", st.onlyNow ) : "" );
		}
		else
		{
			gameLocal.Printf( "  %s: %d scopes only in this build (%d paired, none differ)\n", classNames[ c ].c_str(), st.onlyNow, st.paired );
		}
	}
	if( onlyBefore > 0 )
	{
		gameLocal.Printf( "  %d scopes of the old trace have no partner here\n", onlyBefore );
	}
	gameLocal.Printf( "  %d classes, %d scopes in the old trace, %d here\n", classNames.Num(), before.scopes.Num(), now.scopes.Num() );
	return classesDiffer;
}

/*
================
idStreamTrace::WriteFile
================
*/
bool idStreamTrace::WriteFile( const char* fileName ) const
{
	idFile* f = fileSystem->OpenFileWrite( fileName );
	if( f == NULL )
	{
		return false;
	}

	f->WriteBig( STREAMTRACE_FILE_ID );
	f->WriteBig( STREAMTRACE_FILE_VERSION );

	f->WriteBig( names.Num() );
	for( int i = 0; i < names.Num(); i++ )
	{
		f->WriteString( names[ i ] );
	}

	f->WriteBig( scopes.Num() );
	for( int i = 0; i < scopes.Num(); i++ )
	{
		f->WriteBig( scopes[ i ].name );
		f->WriteBig( scopes[ i ].object );
		f->WriteBig( scopes[ i ].parent );
	}

	f->WriteBig( entries.Num() );
	for( int i = 0; i < entries.Num(); i++ )
	{
		const streamTraceEntry_t& e = entries[ i ];
		f->WriteBig( e.scope );
		f->WriteBig( e.offset );
		f->WriteBig( e.size );
		f->WriteBig( e.hash );
		f->WriteBig( e.child );
		f->Write( &e.type, 1 );
	}

	gameLocal.Printf( "  wrote %d scopes and %d entries to %s\n", scopes.Num(), entries.Num(), f->GetFullPath() );
	fileSystem->CloseFile( f );
	return true;
}

/*
================
OpenTraceFile

An absolute path is read as it is; anything else under the search paths, fs_savepath among them.
================
*/
static idFile* OpenTraceFile( const char* fileName )
{
	if( fileName[ 0 ] == '/' || fileName[ 0 ] == '\\' || ( fileName[ 0 ] != '\0' && fileName[ 1 ] == ':' ) )
	{
		return fileSystem->OpenExplicitFileRead( fileName );
	}
	return fileSystem->OpenFileRead( fileName );
}

/*
================
idStreamTrace::ReadFile
================
*/
bool idStreamTrace::ReadFile( const char* fileName )
{
	Clear();

	idFile* f = OpenTraceFile( fileName );
	if( f == NULL )
	{
		gameLocal.Printf( "  can't open %s\n", fileName );
		return false;
	}

	bool ok = false;
	int id = 0, version = 0, num = 0;
	f->ReadBig( id );
	f->ReadBig( version );
	if( id != STREAMTRACE_FILE_ID || version != STREAMTRACE_FILE_VERSION )
	{
		gameLocal.Printf( "  %s is not a stream trace of version %d\n", fileName, STREAMTRACE_FILE_VERSION );
		fileSystem->CloseFile( f );
		return false;
	}

	f->ReadBig( num );
	if( num >= 0 && num < 1000000 )
	{
		names.SetNum( num );
		for( int i = 0; i < num; i++ )
		{
			f->ReadString( names[ i ] );
			nameHash.Add( nameHash.GenerateKey( names[ i ].c_str() ), i );
		}

		f->ReadBig( num );
		if( num >= 0 && num < 10000000 )
		{
			ok = true;
			scopes.SetNum( num );
			for( int i = 0; i < num && ok; i++ )
			{
				f->ReadBig( scopes[ i ].name );
				f->ReadBig( scopes[ i ].object );
				f->ReadBig( scopes[ i ].parent );
				ok = ( scopes[ i ].name >= 0 && scopes[ i ].name < names.Num() && scopes[ i ].parent < i );
			}

			num = -1;
			f->ReadBig( num );
			ok = ok && ( num >= 0 && num < 100000000 );
			if( ok )
			{
				entries.SetNum( num );
				for( int i = 0; i < num && ok; i++ )
				{
					streamTraceEntry_t& e = entries[ i ];
					f->ReadBig( e.scope );
					f->ReadBig( e.offset );
					f->ReadBig( e.size );
					f->ReadBig( e.hash );
					f->ReadBig( e.child );
					ok = ( f->Read( &e.type, 1 ) == 1 && e.scope < scopes.Num() && e.child < scopes.Num() && ( e.type != SP_SCOPE || e.child >= 0 ) );
				}
			}
		}
	}
	fileSystem->CloseFile( f );

	if( !ok )
	{
		gameLocal.Printf( "  %s is truncated or damaged\n", fileName );
		Clear();
	}
	return ok;
}

/*
===============================================================================

	idSnapshotTrace

===============================================================================
*/

static idCVar net_snapTrace( "net_snapTrace", "", CVAR_GAME | CVAR_NOCHEAT, "P1-A1c: a file under fs_savepath that records each snapshot object this peer writes (the host) or reads (a client), for testSnapTraceCompare; empty is off" );

idSnapshotTrace snapshotTrace;

/*
================
idSnapshotTrace::idSnapshotTrace
================
*/
idSnapshotTrace::idSnapshotTrace()
{
	file = NULL;
	writes = false;
	time = 0;
	objectNum = -1;
	className = -1;
	stale = false;
	objectOpen = false;
}

/*
================
idSnapshotTrace::Start
================
*/
idSnapshotTrace* idSnapshotTrace::Start( bool isWriter, int snapshotTime )
{
	const char* name = net_snapTrace.GetString();
	if( name[ 0 ] == '\0' )
	{
		if( file != NULL )
		{
			Close();
		}
		return NULL;
	}

	if( file == NULL || fileName.Cmp( name ) != 0 || writes != isWriter )
	{
		Close();
		file = fileSystem->OpenFileWrite( name );
		if( file == NULL )
		{
			gameLocal.Warning( "net_snapTrace: can't write %s; tracing stays off", name );
			net_snapTrace.SetString( "" );
			return NULL;
		}
		fileName = name;
		writes = isWriter;
		fields.Clear();
		lists.Clear();
		listHash.Clear();
		classNames.Clear();
		classHash.Clear();

		file->WriteBig( SNAPTRACE_FILE_ID );
		file->WriteBig( SNAPTRACE_FILE_VERSION );
		file->WriteBool( writes );
		gameLocal.Printf( "net_snapTrace: recording the snapshot objects this peer %s to %s\n", writes ? "writes" : "reads", file->GetFullPath() );
	}

	time = snapshotTime;
	objectOpen = false;
	return this;
}

/*
================
idSnapshotTrace::Object
================
*/
void idSnapshotTrace::Object( int number, const char* name, const idBitMsg& msg, bool isStale )
{
	EndObject();

	if( name == NULL )
	{
		name = "";
	}
	const int key = classHash.GenerateKey( name );
	className = -1;
	for( int i = classHash.First( key ); i != -1; i = classHash.Next( i ) )
	{
		if( classNames[ i ].Cmp( name ) == 0 )
		{
			className = i;
			break;
		}
	}
	if( className < 0 )
	{
		className = classNames.Append( name );
		classHash.Add( key, className );
	}

	objectNum = number;
	stale = isStale;
	objectOpen = true;
	current.SetNum( 0 );
	msg.SetTrace( this );
}

/*
================
idSnapshotTrace::BitMsgPrimitive
================
*/
void idSnapshotTrace::BitMsgPrimitive( bitMsgPrimitive_t type, int numBits, unsigned int hash )
{
	if( !objectOpen )
	{
		return;
	}
	snapTraceField_t& f = current.Alloc();
	f.type = type;
	f.numBits = numBits;
	f.hash = hash;
}

/*
================
idSnapshotTrace::EndObject

Writes the object's record, and its field list the first time that list is seen.
================
*/
void idSnapshotTrace::EndObject()
{
	if( !objectOpen )
	{
		return;
	}
	objectOpen = false;

	const int num = current.Num();
	const int bytes = num * sizeof( snapTraceField_t );
	const int key = ( int )( CRC32_BlockChecksum( current.Ptr(), bytes ) + className );
	int list = -1;
	for( int i = listHash.First( key ); i != -1; i = listHash.Next( i ) )
	{
		if( lists[ i ].className == className && lists[ i ].num == num && ( bytes == 0 || memcmp( fields.Ptr() + lists[ i ].first, current.Ptr(), bytes ) == 0 ) )
		{
			list = i;
			break;
		}
	}

	if( list < 0 )
	{
		list = lists.Num();
		snapTraceList_t& l = lists.Alloc();
		l.first = fields.Num();
		l.num = num;
		l.className = className;
		fields.Append( current );
		listHash.Add( key, list );

		file->WriteUnsignedChar( 'L' );
		file->WriteInt( list );
		file->WriteString( classNames[ className ] );
		file->WriteInt( num );
		for( int i = 0; i < num; i++ )
		{
			file->WriteInt( current[ i ].type );
			file->WriteInt( current[ i ].numBits );
			file->WriteUnsignedInt( current[ i ].hash );
		}
	}

	file->WriteUnsignedChar( 'O' );
	file->WriteInt( time );
	file->WriteInt( objectNum );
	file->WriteInt( list );
	file->WriteBool( stale );
}

/*
================
idSnapshotTrace::End
================
*/
void idSnapshotTrace::End()
{
	EndObject();
	if( file != NULL )
	{
		file->Flush();
	}
}

/*
================
idSnapshotTrace::Close
================
*/
void idSnapshotTrace::Close()
{
	EndObject();
	if( file != NULL )
	{
		fileSystem->CloseFile( file );
		file = NULL;
	}
	fileName.Clear();
}

/*
================
snapTraceFile_t

A snapshot trace read back for the compare.
================
*/
typedef struct snapTraceObject_s
{
	int					time;
	int					objectNum;
	int					list;
	bool				stale;
} snapTraceObject_t;

typedef struct snapTraceFile_s
{
	bool						writes;
	idList<snapTraceField_t>	fields;
	idList<snapTraceList_t>		lists;
	idStrList					classNames;
	idList<snapTraceObject_t>	objects;
} snapTraceFile_t;

static bool ReadSnapTrace( const char* fileName, snapTraceFile_t& out )
{
	idFile* f = OpenTraceFile( fileName );
	if( f == NULL )
	{
		gameLocal.Printf( "  can't open %s\n", fileName );
		return false;
	}

	int id = 0, version = 0;
	f->ReadBig( id );
	f->ReadBig( version );
	f->ReadBool( out.writes );
	if( id != SNAPTRACE_FILE_ID || version != SNAPTRACE_FILE_VERSION )
	{
		gameLocal.Printf( "  %s is not a snapshot trace of version %d\n", fileName, SNAPTRACE_FILE_VERSION );
		fileSystem->CloseFile( f );
		return false;
	}

	bool ok = true;
	unsigned char tag;
	while( ok && f->ReadUnsignedChar( tag ) == 1 )
	{
		if( tag == 'L' )
		{
			int list = -1, num = -1;
			idStr name;
			f->ReadInt( list );
			f->ReadString( name );
			f->ReadInt( num );
			ok = ( list == out.lists.Num() && num >= 0 && num < 65536 );
			if( !ok )
			{
				break;
			}
			snapTraceList_t& l = out.lists.Alloc();
			l.first = out.fields.Num();
			l.num = num;
			l.className = out.classNames.Append( name );
			for( int i = 0; i < num; i++ )
			{
				snapTraceField_t& field = out.fields.Alloc();
				f->ReadInt( field.type );
				f->ReadInt( field.numBits );
				f->ReadUnsignedInt( field.hash );
			}
		}
		else if( tag == 'O' )
		{
			snapTraceObject_t& o = out.objects.Alloc();
			f->ReadInt( o.time );
			f->ReadInt( o.objectNum );
			f->ReadInt( o.list );
			ok = ( f->ReadBool( o.stale ) == 1 && o.list >= 0 && o.list < out.lists.Num() );
			if( !ok )
			{
				out.objects.SetNum( out.objects.Num() - 1 );	// a record cut short by the end of a killed run
			}
		}
		else
		{
			ok = false;
		}
	}
	fileSystem->CloseFile( f );

	if( !ok )
	{
		gameLocal.Printf( "  %s ends in a damaged record; the %d objects before it are used\n", fileName, out.objects.Num() );
	}
	return true;
}

static int CountSnapshots( const snapTraceFile_t& t )
{
	int count = 0;
	for( int i = 0; i < t.objects.Num(); i++ )
	{
		if( i == 0 || t.objects[ i ].time != t.objects[ i - 1 ].time )
		{
			count++;
		}
	}
	return count;
}

/*
================
idSnapshotTrace::Compare

K4: every client read must match the host's write of the same (snapshot time, object), field for field. A read of
nothing (a delete, a disconnected player's state) is skipped; a stale object reads only its header, a prefix. Host
writes the client never read are counted, not failed. The time is the snapshot's own, ss.SetTime( fast.time ) on the
host and ss.GetTime() on the client: the one identity of a snapshot both peers hold without a new byte on the wire.
================
*/
int idSnapshotTrace::Compare( const char* hostFileName, const char* clientFileName )
{
	snapTraceFile_t host, client;
	if( !ReadSnapTrace( hostFileName, host ) || !ReadSnapTrace( clientFileName, client ) )
	{
		return 1;
	}
	if( !host.writes || client.writes )
	{
		gameLocal.Printf( "  the first file must be a host's writes and the second a client's reads\n" );
		return 1;
	}

	idHashIndex hostHash( 65536, Max( host.objects.Num(), 1 ) );
	for( int h = 0; h < host.objects.Num(); h++ )
	{
		hostHash.Add( hostHash.GenerateKey( host.objects[ h ].time, host.objects[ h ].objectNum ), h );
	}
	idList<bool> hostRead;
	hostRead.SetNum( host.objects.Num() );
	for( int h = 0; h < hostRead.Num(); h++ )
	{
		hostRead[ h ] = false;
	}

	int matched = 0, skipped = 0, stalePrefix = 0, mismatched = 0, unmatched = 0, ambiguous = 0;
	idStrList groups;		// every mismatch also counts under its (class, field, write, read)
	idList<int> groupCounts;
	idHashIndex groupHash;
	for( int c = 0; c < client.objects.Num(); c++ )
	{
		const snapTraceObject_t& co = client.objects[ c ];
		const snapTraceList_t& cl = client.lists[ co.list ];
		if( cl.num == 0 )
		{
			skipped++;
			continue;
		}

		const int key = hostHash.GenerateKey( co.time, co.objectNum );
		int firstHost = -1;
		int candidates = 0;
		bool match = false;
		bool prefix = false;
		for( int h = hostHash.First( key ); h != -1; h = hostHash.Next( h ) )
		{
			const snapTraceObject_t& ho = host.objects[ h ];
			if( ho.time != co.time || ho.objectNum != co.objectNum )
			{
				continue;
			}
			hostRead[ h ] = true;
			if( firstHost < 0 )
			{
				firstHost = h;
			}
			else if( ho.list != host.objects[ firstHost ].list )
			{
				candidates++;
			}
			const snapTraceList_t& hl = host.lists[ ho.list ];
			const int common = Min( hl.num, cl.num );
			const bool samePrefix = ( common == 0 || memcmp( host.fields.Ptr() + hl.first, client.fields.Ptr() + cl.first, common * sizeof( snapTraceField_t ) ) == 0 );
			if( samePrefix && hl.num == cl.num )
			{
				match = true;
			}
			else if( samePrefix && co.stale && cl.num < hl.num )
			{
				prefix = true;
			}
		}
		if( candidates > 0 )
		{
			ambiguous++;
		}

		if( firstHost < 0 )
		{
			if( ++unmatched <= MAX_REPORTED )
			{
				gameLocal.Printf( "  NO HOST WRITE for the client's read of object %d at snapshot time %d (%d fields)\n", co.objectNum, co.time, cl.num );
			}
			continue;
		}
		if( match )
		{
			matched++;
			continue;
		}
		if( prefix )
		{
			stalePrefix++;
			continue;
		}

		const snapTraceList_t& hl = host.lists[ host.objects[ firstHost ].list ];
		int p = 0;
		while( p < hl.num && p < cl.num && memcmp( &host.fields[ hl.first + p ], &client.fields[ cl.first + p ], sizeof( snapTraceField_t ) ) == 0 )
		{
			p++;
		}
		idStr wrote = "nothing", readBack = "nothing";
		if( p < hl.num )
		{
			const snapTraceField_t& hf = host.fields[ hl.first + p ];
			wrote = va( "%s %d", ( hf.type >= 0 && hf.type < BITMSG_NUM_PRIMITIVES ) ? bitMsgPrimitiveNames[ hf.type ] : "?", hf.numBits );
		}
		if( p < cl.num )
		{
			const snapTraceField_t& cf = client.fields[ cl.first + p ];
			readBack = va( "%s %d", ( cf.type >= 0 && cf.type < BITMSG_NUM_PRIMITIVES ) ? bitMsgPrimitiveNames[ cf.type ] : "?", cf.numBits );
		}
		if( p < hl.num && p < cl.num && wrote == readBack )
		{
			wrote += va( " #%08x", host.fields[ hl.first + p ].hash );	// the same call, a different string
			readBack += va( " #%08x", client.fields[ cl.first + p ].hash );
		}
		CountGroup( groups, groupCounts, groupHash, va( "%s field %d: host wrote %s, client read %s", host.classNames[ hl.className ].c_str(), p, wrote.c_str(), readBack.c_str() ) );
		if( ++mismatched <= MAX_REPORTED )
		{
			gameLocal.Printf( "  MISMATCH snapshot time %d, object %d %s: field %d: host wrote %s, client read %s; %d fields written, %d read\n",
							  co.time, co.objectNum, host.classNames[ hl.className ].c_str(), p, wrote.c_str(), readBack.c_str(), hl.num, cl.num );
		}
	}
	for( int g = 0; g < groups.Num() && g < 4 * MAX_REPORTED; g++ )
	{
		gameLocal.Printf( "  failed: %5d x %s\n", groupCounts[ g ], groups[ g ].c_str() );
	}

	int neverRead = 0;
	for( int h = 0; h < host.objects.Num(); h++ )
	{
		if( !hostRead[ h ] )
		{
			neverRead++;
		}
	}

	gameLocal.Printf( "  client: %d objects read in %d snapshots; %d match the host write field for field, %d read nothing (skipped), %d stale (header only), %d mismatched, %d with no host write\n",
					  client.objects.Num(), CountSnapshots( client ), matched, skipped, stalePrefix, mismatched, unmatched );
	gameLocal.Printf( "  host: %d objects written in %d snapshots, %d of them never read by the client (counted, not failed); %d reads had more than one host candidate\n",
					  host.objects.Num(), CountSnapshots( host ), neverRead, ambiguous );
	gameLocal.Printf( "  %d distinct field lists written, %d read\n", host.lists.Num(), client.lists.Num() );
	return mismatched + unmatched;
}
