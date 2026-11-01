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

#ifndef __SCHEMA_H__
#define __SCHEMA_H__

/*
===============================================================================

	Field schemas (P1-A1a, K7 and K8)

	A schema is one #define field list per record, class or struct, beside the members it lists:

		#define MOVEABLE_SCHEMA( X, s ) \
			X( s, nextSoundTime,	INT,	S,	RAW,	0,	SCHEMA_STOCK,	NOKEY,	NODEF,	NOEDIT,	OBJ ) \
			X( s, physicsObj,		STATIC,	SW,	RAW,	0,	SCHEMA_STOCK,	NOKEY,	NODEF,	NOEDIT,	OBJ )

	- s is the instance the expansion works on, passed per direction: idPhysics_Player saves current and saved,
	  writes snapshots from current and reads them into next.
	- The type picks the one idSaveGame / idRestoreGame call the field always had, so the bytes do not change, and a
	  member of another type does not compile.
	- The channels are S (save), W (wire: snapshots) and R (record), in that order: S, W, R, SW, SR, WR, SWR. R is the
	  map record's (MREC v3, P2b.1.2): the SCHEMA_RECORD_ expanders below and gamesys/Record.h. NOCH is a row no
	  channel carries: SCHEMA_INDEX numbers it like any other, SCHEMA_RECORD_INDEX skips it.
	- The encoding is the wire form, RAW, BITS or DELTA; a save is always raw. Its argument, 0 when unused: BITS takes
	  ( exponentBits, mantissaBits ), DELTA the base field, which comes earlier in the list. A LIST takes its element
	  type, a STRUCTLIST the name of the element functions SCHEMA_STRUCT_FUNCTIONS made.
	- The build number is the one that added the field. A restore reads it only from saves of that build or later,
	  and leaves the spawn value otherwise. SCHEMA_STOCK marks a field every save this tree reads has, and folds the
	  test away, so a stock field restores with the code the hand-written call compiled to.
	- The last four columns are the spawn side (C6): the spawnArg key or NOKEY, the spawn value when the key is absent
	  or NODEF (the constructor's), the FGD text or NOEDIT, and where the value lives: OBJ (a member of s), HOT (a
	  column of the hot table, C6), BOTH (the column, mirrored in the member) or KEYONLY (read at spawn, stored
	  nowhere by the schema; hand code may still read it). SCHEMA_SPAWN, SCHEMA_FGD, SCHEMA_KEYCHECK and a value R
	  row's SCHEMA_RECORD_DIFF read them (P2b.2.3b, below).

	The compiler expands a list into statements inside a hand-written function whose stream is savefile or msg, so
	what runs is still read in one place; code that is about no single field goes before or after the expansion:

		void idMoveable::Save( idSaveGame* savefile ) const
		{
			MOVEABLE_SCHEMA( SCHEMA_SAVE, ( *this ) )
		}

	SCHEMA_SAVE, SCHEMA_RESTORE, SCHEMA_WRITE and SCHEMA_READ. No generator, no build step, no registry walked at run
	time: the precedent is CLASS_PROTOTYPE and the event tables (Class.h). A list is a #define in the header of the
	class it lists, inert until expanded, so only the .cpp files that expand one include this file, and Game_local.h for
	the class headers whose bodies expand a record list's SCHEMA_RECORD_INDEX.

	K8: a field the schema cannot express is a HAND entry. It stays hand-written, at its position, in the member
	functions Save<name> and Restore<name>, or Write<name>ToSnapshot and Read<name>FromSnapshot on the wire.

	The record channel (P2b.1.2). A class's R rows are its record section: SCHEMA_RECORD_INDEX numbers them RF_<field>
	in an enum inside the class (Game_local.h includes this file before every class header that holds one), the bit of
	the field in the section's uint64 fieldMask; R rows append only, so a mask never renumbers. Inside WriteRecord,
	SCHEMA_RECORD_DIFF sets the bit of each field that differs from the spawn baseline and SCHEMA_RECORD_WRITE then
	writes the set fields in list order; ApplyRecord expands SCHEMA_RECORD_READ and a static CheckRecordSection
	SCHEMA_RECORD_CHECK, whose s is the class name. A failed read or check clears the sticky rec.good, which the caller
	tests after the list. A HAND row is Record<n>Changed, WriteRecord<n>, ApplyRecord<n> and static CheckRecord<n>, and
	its ARG is its byte count, 0 for a variable field (a uint16 length, at most RECORD_MAX_FIELD_BYTES, before it); the
	field's bytes are bounded by idRecordOut/idRecordIn around the call, so a reader can never cross into the next field.

===============================================================================
*/

#define SCHEMA_STOCK		0

//puncho - P2b.2.3a: the spawn side's absent values (C6). OBJ, HOT, BOTH, KEYONLY, NOCH and the channels are pasted
// into gate names, STORE into SCHEMA_SPAWN_ from P2b.2.3b, so none is ever defined
#define NOKEY				nullptr
#define NODEF				nullptr
#define NOEDIT				nullptr

//puncho start - P2b.2.3b: the spawn read (K1). NOKEY and NODEF are nullptr, so a row's KEY and DEFAULT pick the
// overload: no KEY reads nothing, a KEY without a DEFAULT parses the key only when the dict holds it, a KEY and a
// DEFAULT read as the hand-written getter did, and a DEFAULT without a KEY does not compile. The dict is spawnArgs, or
// a value R row's baseline (idRecordMapArgs), which has the same getters.
typedef decltype( nullptr ) schemaNull_t;

constexpr bool idSchemaIsNull( schemaNull_t )
{
	return true;
}

constexpr bool idSchemaIsNull( const char* )
{
	return false;
}

template< class D >
ID_INLINE void idSchemaGet( const D& dict, const char* key, const char* def, int& member )
{
	member = dict.GetInt( key, def );
}

template< class D >
ID_INLINE void idSchemaGet( const D& dict, const char* key, const char* def, float& member )
{
	member = dict.GetFloat( key, def );
}

template< class D >
ID_INLINE void idSchemaGet( const D& dict, const char* key, const char* def, bool& member )
{
	member = dict.GetBool( key, def );
}

template< class D >
ID_INLINE void idSchemaGet( const D& dict, const char* key, const char* def, idStr& member )
{
	dict.GetString( key, def, member );
}

template< class D >
ID_INLINE void idSchemaGet( const D& dict, const char* key, const char* def, idVec3& member )
{
	dict.GetVector( key, def, member );
}

template< class D, class T >
ID_INLINE void idSchemaGet( const D&, schemaNull_t, schemaNull_t, T& )
{
}

template< class D, class T >
void idSchemaGet( const D&, schemaNull_t, const char*, T& ) = delete;

template< class D, class T >
ID_INLINE void idSchemaGet( const D& dict, const char* key, schemaNull_t, T& member )
{
	if( dict.FindKey( key ) != NULL )
	{
		idSchemaGet( dict, key, "", member );
	}
}

// a value R row's DIFF (rev 4, goals-r1 F3): the authored baseline read into a temporary of the member's type by the
// spawn's own parse, so != is exact. A created entry has no baseline; SCHEMA_RCHANGED_ tests it first.
template< class D, class T >
ID_INLINE bool idSchemaRecordChanged( const D& mapArgs, const char* key, const char* def, const T& member )
{
	T baseline;
	idSchemaGet( mapArgs, key, def, baseline );
	return baseline != member;
}

template< class D, class S, class T >
ID_INLINE bool idSchemaRecordChanged( const D&, schemaNull_t, S, const T& )
{
	static_assert( sizeof( T ) == 0, "a value R row takes a KEY" );
	return true;
}

template< class D, class T >
ID_INLINE bool idSchemaRecordChanged( const D&, const char*, schemaNull_t, const T& )
{
	static_assert( sizeof( T ) == 0, "a value R row takes a literal DEFAULT: a NODEF baseline would be uninitialised" );
	return true;
}
//puncho end

// one entry, per direction
#define SCHEMA_SAVE( s, n, TYPE, CH, ENC, ARG, BUILD, KEY, DEFAULT, EDITOR, STORE )		SCHEMA_S_##CH( SCHEMA_SAVE_##TYPE( savefile, s, n, ARG ) )
#define SCHEMA_RESTORE( s, n, TYPE, CH, ENC, ARG, BUILD, KEY, DEFAULT, EDITOR, STORE )	SCHEMA_S_##CH( if( ( BUILD ) == SCHEMA_STOCK || savefile->GetBuildNumber() >= ( BUILD ) ) { SCHEMA_RESTORE_##TYPE( savefile, s, n, ARG ) } )
#define SCHEMA_WRITE( s, n, TYPE, CH, ENC, ARG, BUILD, KEY, DEFAULT, EDITOR, STORE )	SCHEMA_W_##CH( SCHEMA_WRITE_##TYPE##_##ENC( msg, s, n, ARG ) )
#define SCHEMA_READ( s, n, TYPE, CH, ENC, ARG, BUILD, KEY, DEFAULT, EDITOR, STORE )		SCHEMA_W_##CH( SCHEMA_READ_##TYPE##_##ENC( msg, s, n, ARG ) )
// numbers the fields SF_<field> in an enum inside the class, NOCH rows included; only recordTest mixedlist expands it
// yet, MD7's dirty mask will
#define SCHEMA_INDEX( s, n, TYPE, CH, ENC, ARG, BUILD, KEY, DEFAULT, EDITOR, STORE )	SF_##n,
//puncho start - P2b.2.3b: the spawn side (K1). SCHEMA_SPAWN is the one read per field, inside a Spawn, where the dict
// is the bare name spawnArgs; SCHEMA_FGD a schemaKey_t row (Game.h) and SCHEMA_KEYCHECK the row's checks, both expanded
// by CLASS_SCHEMA_KEYS (below)
#define SCHEMA_SPAWN( s, n, TYPE, CH, ENC, ARG, BUILD, KEY, DEFAULT, EDITOR, STORE )	SCHEMA_SPAWN_##STORE( s, n, TYPE, KEY, DEFAULT )
#define SCHEMA_FGD( s, n, TYPE, CH, ENC, ARG, BUILD, KEY, DEFAULT, EDITOR, STORE )		{ #n, KEY, DEFAULT, EDITOR, SCHEMA_FGDTYPE_##TYPE },
// a non-keyable type has NOKEY; a DEFAULT needs a KEY; a KEYONLY row has a KEY and NOCH; a HOT row has a DEFAULT and,
// in this phase, NOCH; a value R row has a literal DEFAULT, its baseline's parse
#define SCHEMA_KEYCHECK( s, n, TYPE, CH, ENC, ARG, BUILD, KEY, DEFAULT, EDITOR, STORE ) \
	static_assert( SCHEMA_FGDTYPE_##TYPE != SCHEMA_KEY_NONE || idSchemaIsNull( KEY ), #n ": this type takes no KEY" ); \
	static_assert( idSchemaIsNull( DEFAULT ) || !idSchemaIsNull( KEY ), #n ": a DEFAULT needs a KEY" ); \
	static_assert( SCHEMA_STORE_##STORE != SCHEMA_STORE_KEYONLY || ( !idSchemaIsNull( KEY ) && SCHEMA_IS_NOCH( CH ) ), #n ": a KEYONLY row needs a KEY and NOCH" ); \
	static_assert( SCHEMA_STORE_##STORE != SCHEMA_STORE_HOT || ( !idSchemaIsNull( DEFAULT ) && SCHEMA_IS_NOCH( CH ) ), #n ": a HOT row needs a DEFAULT and NOCH" ); \
	static_assert( !( SCHEMA_HAS_R( CH ) && SCHEMA_FGDTYPE_##TYPE != SCHEMA_KEY_NONE ) || !idSchemaIsNull( DEFAULT ), #n ": a value R row needs a literal DEFAULT" );
// KEYCHECK's tests of the STORE and CH tokens, which are never defined themselves: the channel gates pass a 1 for each
// channel the row has
#define SCHEMA_STORE_OBJ		0
#define SCHEMA_STORE_HOT		1
#define SCHEMA_STORE_BOTH		2
#define SCHEMA_STORE_KEYONLY	3
#define SCHEMA_HAS_R( CH )		( SCHEMA_R_##CH( 1 + ) 0 )
#define SCHEMA_IS_NOCH( CH )	( ( SCHEMA_S_##CH( 1 + ) SCHEMA_W_##CH( 1 + ) SCHEMA_R_##CH( 1 + ) 0 ) == 0 )
//puncho end

// the channel gates
#define SCHEMA_S_S( x )		x
#define SCHEMA_S_SW( x )	x
#define SCHEMA_S_SR( x )	x
#define SCHEMA_S_SWR( x )	x
#define SCHEMA_S_W( x )
#define SCHEMA_S_R( x )
#define SCHEMA_S_WR( x )
#define SCHEMA_S_NOCH( x )	//puncho - P2b.2.3a
#define SCHEMA_W_W( x )		x
#define SCHEMA_W_SW( x )	x
#define SCHEMA_W_WR( x )	x
#define SCHEMA_W_SWR( x )	x
#define SCHEMA_W_S( x )
#define SCHEMA_W_R( x )
#define SCHEMA_W_SR( x )
#define SCHEMA_W_NOCH( x )	//puncho - P2b.2.3a
//puncho start - P2b.1.2: the record channel's gates, variadic so an enumerator's comma passes through
#define SCHEMA_R_R( ... )	__VA_ARGS__
#define SCHEMA_R_SR( ... )	__VA_ARGS__
#define SCHEMA_R_WR( ... )	__VA_ARGS__
#define SCHEMA_R_SWR( ... )	__VA_ARGS__
#define SCHEMA_R_S( ... )
#define SCHEMA_R_W( ... )
#define SCHEMA_R_SW( ... )
#define SCHEMA_R_NOCH( ... )	//puncho - P2b.2.3a

// the record channel, per R row: rec is the idRecordOut or idRecordIn, s the instance (the class name for CHECK). Each
// body is inside its row's gate, so a list that also holds S, W and NOCH rows names an RF_ only for its R rows
#define SCHEMA_RECORD_INDEX( s, n, TYPE, CH, ENC, ARG, BUILD, KEY, DEFAULT, EDITOR, STORE )	SCHEMA_R_##CH( RF_##n, )
#define SCHEMA_RECORD_NAME( s, n, TYPE, CH, ENC, ARG, BUILD, KEY, DEFAULT, EDITOR, STORE )	SCHEMA_R_##CH( #n, )
#define SCHEMA_RECORD_DIFF( s, n, TYPE, CH, ENC, ARG, BUILD, KEY, DEFAULT, EDITOR, STORE )	SCHEMA_R_##CH( if( SCHEMA_RCHANGED_##TYPE( rec, s, n, ARG, KEY, DEFAULT ) ) rec.Set( RF_##n ); )
#define SCHEMA_RECORD_WRITE( s, n, TYPE, CH, ENC, ARG, BUILD, KEY, DEFAULT, EDITOR, STORE )	SCHEMA_R_##CH( if( rec.Has( RF_##n ) ) { SCHEMA_RPUT_##TYPE( rec, s, n, ARG ) } )
#define SCHEMA_RECORD_READ( s, n, TYPE, CH, ENC, ARG, BUILD, KEY, DEFAULT, EDITOR, STORE )	SCHEMA_R_##CH( if( rec.Has( RF_##n ) && !( SCHEMA_RGET_##TYPE( rec, s, n, ARG ) ) ) { rec.good = false; } )
#define SCHEMA_RECORD_CHECK( s, n, TYPE, CH, ENC, ARG, BUILD, KEY, DEFAULT, EDITOR, STORE )	SCHEMA_R_##CH( if( rec.Has( RF_##n ) && !( SCHEMA_RCHECK_##TYPE( rec, s, n, ARG ) ) ) { rec.good = false; } )

// record HAND rows: the field functions, inside the byte bounds of the field
#define SCHEMA_RCHANGED_HAND( r, s, n, a, K, D )		( s ).Record##n##Changed( r )
#define SCHEMA_RPUT_HAND( r, s, n, a )			( r ).BeginField( a ); ( s ).WriteRecord##n( r ); ( r ).EndField( a );
#define SCHEMA_RGET_HAND( r, s, n, a )			( ( r ).BeginField( a ) && ( s ).ApplyRecord##n( r ) && ( r ).EndField() )
#define SCHEMA_RCHECK_HAND( r, s, n, a )		( ( r ).BeginField( a ) && s::CheckRecord##n( r ) && ( r ).EndField() )
//puncho end

//puncho start - P2b.2.3b: the value R rows (K1). Changed when the entry is created, which has no baseline (a HAND row
// tests the same, Entity.cpp RecordHealthChanged), or when the member differs from its authored baseline. Each is a
// field framed at its type's size, ARG unused: INT and ENUM int32, FLOAT float32, BOOL a byte, VEC3 three float32,
// STRING a variable field holding a str of at most SCHEMA_RECORD_STR_BOUND bytes. A get reads into the member and a
// check into a temporary (INT and ENUM need none: any four bytes are one); both refuse a non-finite float, a bool byte
// past 1, and a string past its bound or holding a NUL. The codec types (U8 to I64, STR, LIST) take no KEY and have no
// RCHANGED.
#define SCHEMA_RECORD_STR_BOUND						63
#define SCHEMA_RCHANGED_VALUE( r, K, D, member )	( ( r ).IsCreated() || idSchemaRecordChanged( ( r ).MapArgs(), K, D, member ) )
#define SCHEMA_RCHANGED_INT( r, s, n, a, K, D )		SCHEMA_RCHANGED_VALUE( r, K, D, ( s ).n )
#define SCHEMA_RCHANGED_FLOAT( r, s, n, a, K, D )	SCHEMA_RCHANGED_VALUE( r, K, D, ( s ).n )
#define SCHEMA_RCHANGED_BOOL( r, s, n, a, K, D )	SCHEMA_RCHANGED_VALUE( r, K, D, ( s ).n )
#define SCHEMA_RCHANGED_VEC3( r, s, n, a, K, D )	SCHEMA_RCHANGED_VALUE( r, K, D, ( s ).n )
#define SCHEMA_RCHANGED_STRING( r, s, n, a, K, D )	SCHEMA_RCHANGED_VALUE( r, K, D, ( s ).n )
#define SCHEMA_RCHANGED_ENUM( r, s, n, a, K, D )	SCHEMA_RCHANGED_VALUE( r, K, D, ( int )( ( s ).n ) )
#define SCHEMA_RPUT_INT( r, s, n, a )				( r ).BeginField( 4 ); ( r ).WriteInt( ( s ).n ); ( r ).EndField( 4 );
#define SCHEMA_RGET_INT( r, s, n, a )				( ( r ).BeginField( 4 ) && ( r ).ReadInt( ( s ).n ) && ( r ).EndField() )
#define SCHEMA_RCHECK_INT( r, s, n, a )				( ( r ).BeginField( 4 ) )
#define SCHEMA_RPUT_ENUM( r, s, n, a )				( r ).BeginField( 4 ); ( r ).WriteInt( ( int )( ( s ).n ) ); ( r ).EndField( 4 );
#define SCHEMA_RGET_ENUM( r, s, n, a )				( ( r ).BeginField( 4 ) && ( r ).ReadInt( ( int& )( ( s ).n ) ) && ( r ).EndField() )
#define SCHEMA_RCHECK_ENUM( r, s, n, a )			( ( r ).BeginField( 4 ) )
#define SCHEMA_RPUT_FLOAT( r, s, n, a )				( r ).BeginField( 4 ); ( r ).WriteFloat( ( s ).n ); ( r ).EndField( 4 );
#define SCHEMA_RGET_FLOAT( r, s, n, a )				( ( r ).BeginField( 4 ) && ( r ).ReadFiniteFloat( ( s ).n ) && ( r ).EndField() )
#define SCHEMA_RCHECK_FLOAT( r, s, n, a )			( ( r ).BeginField( 4 ) && idSchemaCheckFloats( r, 1 ) && ( r ).EndField() )
#define SCHEMA_RPUT_BOOL( r, s, n, a )				( r ).BeginField( 1 ); ( r ).WriteByte( ( s ).n ? 1 : 0 ); ( r ).EndField( 1 );
#define SCHEMA_RGET_BOOL( r, s, n, a )				( ( r ).BeginField( 1 ) && idSchemaReadBool( r, ( s ).n ) && ( r ).EndField() )
#define SCHEMA_RCHECK_BOOL( r, s, n, a )			( ( r ).BeginField( 1 ) && idSchemaCheckBool( r ) && ( r ).EndField() )
#define SCHEMA_RPUT_VEC3( r, s, n, a )				( r ).BeginField( 12 ); ( r ).WriteFloat( ( s ).n[0] ); ( r ).WriteFloat( ( s ).n[1] ); ( r ).WriteFloat( ( s ).n[2] ); ( r ).EndField( 12 );
#define SCHEMA_RGET_VEC3( r, s, n, a )				( ( r ).BeginField( 12 ) && ( r ).ReadFiniteFloat( ( s ).n[0] ) && ( r ).ReadFiniteFloat( ( s ).n[1] ) && ( r ).ReadFiniteFloat( ( s ).n[2] ) && ( r ).EndField() )
#define SCHEMA_RCHECK_VEC3( r, s, n, a )			( ( r ).BeginField( 12 ) && idSchemaCheckFloats( r, 3 ) && ( r ).EndField() )
#define SCHEMA_RPUT_STRING( r, s, n, a )			( r ).BeginField( 0 ); ( r ).WriteString( ( s ).n, SCHEMA_RECORD_STR_BOUND ); ( r ).EndField( 0 );
#define SCHEMA_RGET_STRING( r, s, n, a )			( ( r ).BeginField( 0 ) && ( r ).ReadString( ( s ).n, SCHEMA_RECORD_STR_BOUND ) && ( r ).EndField() )
#define SCHEMA_RCHECK_STRING( r, s, n, a )			( ( r ).BeginField( 0 ) && idSchemaCheckString( r ) && ( r ).EndField() )

// the reads a get and a check share, over idRecordIn (Record.h comes after this file in Game_local.h)
template< class R >
ID_INLINE bool idSchemaReadBool( R& rec, bool& value )
{
	byte b;
	if( !rec.ReadByte( b ) || b > 1 )
	{
		return false;
	}
	value = ( b != 0 );
	return true;
}

template< class R >
ID_INLINE bool idSchemaCheckBool( R& rec )
{
	bool value;
	return idSchemaReadBool( rec, value );
}

template< class R >
ID_INLINE bool idSchemaCheckFloats( R& rec, int count )
{
	float value;
	for( int i = 0; i < count; i++ )
	{
		if( !rec.ReadFiniteFloat( value ) )
		{
			return false;
		}
	}
	return true;
}

template< class R >
ID_INLINE bool idSchemaCheckString( R& rec )
{
	idStr value;
	return rec.ReadString( value, SCHEMA_RECORD_STR_BOUND );
}
//puncho end

//puncho start - P2b.1.4: the record struct lists, the byte form of a record no entity owns (the dungeon record, the
// recipe and their elements, MapGraph.h). SCHEMA_RECORD_STRUCT_FUNCTIONS( name, type, LIST ) makes static
// WriteRecord<name>( idRecordOut&, const type& ) and ReadRecord<name>( idRecordIn&, type& ), which run over an idFile
// (Record.h's BeginFile), and the typedef recordStruct<name>_t a LIST of the struct declares its elements with. Every row
// is written, in list order: no mask, no field framing. Every row is CH R, ENC RAW and BUILD SCHEMA_STOCK (a row of
// another channel, encoding or build does not compile). The types:
//	U8 U16 U32 U64 I64	little endian, the width from the type; a value that does not fit fails the write; the read
//						assigns the member from an unsigned of the width (I64: the signed value)
//	STR					ARG the bound: a uint8 length, then the bytes; a longer string fails the write, the read refuses a
//						longer length or a NUL byte
//	VEC3				three float32; the read refuses a non-finite component
//	LIST				ARG ( element, countWidth, maxCount ): the count in countWidth bytes, then each element through the
//						element's struct functions; a count past maxCount fails the write and is refused before any
//						element; each element read goes to the hand bool Check<element>( recordStruct<element>_t&,
//						const type& ) and is appended only when it passes
//	HAND				static void WriteRecord<n>( idRecordOut&, const type& ) and bool ReadRecord<n>( idRecordIn&, type& )
//						beside the list; ARG its byte count, 0 for a variable field (not read by the expanders)
// A failed write sets Overflowed() and writes nothing more; a failed read or check clears the sticky rec.good, and the
// rows after it are skipped. The caller tests either after the expansion, and a reader that the file ends there.
#define SCHEMA_RECORD_STRUCT_FUNCTIONS( name, type, LIST ) \
	typedef type recordStruct##name##_t; \
	static void WriteRecord##name( idRecordOut& rec, const type& s ) { LIST( SCHEMA_RECORD_STRUCT_PUT, s ) } \
	static void ReadRecord##name( idRecordIn& rec, type& s ) { LIST( SCHEMA_RECORD_STRUCT_GET, s ) }
#define SCHEMA_RECORD_STRUCT_PUT( s, n, TYPE, CH, ENC, ARG, BUILD, KEY, DEFAULT, EDITOR, STORE )	compile_time_assert( ( BUILD ) == SCHEMA_STOCK ); SCHEMA_SROW_##CH##_##ENC( SCHEMA_RSPUT_##TYPE( rec, s, n, ARG ) )
#define SCHEMA_RECORD_STRUCT_GET( s, n, TYPE, CH, ENC, ARG, BUILD, KEY, DEFAULT, EDITOR, STORE )	compile_time_assert( ( BUILD ) == SCHEMA_STOCK ); SCHEMA_SROW_##CH##_##ENC( if( rec.good ) { SCHEMA_RSGET_##TYPE( rec, s, n, ARG ) } )
// the one gate a struct row passes: SCHEMA_SROW_<CH>_<ENC> exists only for R and RAW
#define SCHEMA_SROW_R_RAW( ... )			__VA_ARGS__

#define SCHEMA_RSGET_UNSIGNED( r, s, n, T, READ )	{ T value; if( ( r ).READ( value ) ) { ( s ).n = value; } else { ( r ).good = false; } }
#define SCHEMA_RSPUT_U8( r, s, n, a )		( r ).WriteUnsigned( ( s ).n, 1 );
#define SCHEMA_RSGET_U8( r, s, n, a )		SCHEMA_RSGET_UNSIGNED( r, s, n, byte, ReadByte )
#define SCHEMA_RSPUT_U16( r, s, n, a )		( r ).WriteUnsigned( ( s ).n, 2 );
#define SCHEMA_RSGET_U16( r, s, n, a )		SCHEMA_RSGET_UNSIGNED( r, s, n, uint16, ReadUnsignedShort )
#define SCHEMA_RSPUT_U32( r, s, n, a )		( r ).WriteUnsigned( ( s ).n, 4 );
#define SCHEMA_RSGET_U32( r, s, n, a )		SCHEMA_RSGET_UNSIGNED( r, s, n, uint32, ReadUnsignedInt )
#define SCHEMA_RSPUT_U64( r, s, n, a )		( r ).WriteUnsigned( ( s ).n, 8 );
#define SCHEMA_RSGET_U64( r, s, n, a )		SCHEMA_RSGET_UNSIGNED( r, s, n, uint64, Read64 )
#define SCHEMA_RSPUT_I64( r, s, n, a )		( r ).WriteUnsigned( ( uint64 )( s ).n, 8 );
#define SCHEMA_RSGET_I64( r, s, n, a )		{ uint64 value; if( ( r ).Read64( value ) ) { ( s ).n = ( int64 )value; } else { ( r ).good = false; } }
#define SCHEMA_RSPUT_STR( r, s, n, a )		( r ).WriteString( ( s ).n, a );
#define SCHEMA_RSGET_STR( r, s, n, a )		if( !( r ).ReadString( ( s ).n, a ) ) { ( r ).good = false; }
#define SCHEMA_RSPUT_VEC3( r, s, n, a )		( r ).WriteFloat( ( s ).n[0] ); ( r ).WriteFloat( ( s ).n[1] ); ( r ).WriteFloat( ( s ).n[2] );
#define SCHEMA_RSGET_VEC3( r, s, n, a )		if( !( r ).ReadFiniteFloat( ( s ).n[0] ) || !( r ).ReadFiniteFloat( ( s ).n[1] ) || !( r ).ReadFiniteFloat( ( s ).n[2] ) ) { ( r ).good = false; }
#define SCHEMA_RSPUT_HAND( r, s, n, a )		WriteRecord##n( r, s );
#define SCHEMA_RSGET_HAND( r, s, n, a )		if( !ReadRecord##n( r, s ) ) { ( r ).good = false; }

// a LIST's ARG, taken apart; the element's names are pasted through SCHEMA_CAT so the argument expands first
#define SCHEMA_LIST_ELEMENT( e, w, m )		e
#define SCHEMA_LIST_WIDTH( e, w, m )		w
#define SCHEMA_LIST_MAX( e, w, m )			m
#define SCHEMA_CAT( x, y )					SCHEMA_CAT_( x, y )
#define SCHEMA_CAT_( x, y )					x##y
#define SCHEMA_CAT3( x, y, z )				SCHEMA_CAT3_( x, y, z )
#define SCHEMA_CAT3_( x, y, z )				x##y##z
#define SCHEMA_RSPUT_LIST( r, s, n, a ) \
	compile_time_assert( SCHEMA_LIST_WIDTH a <= 4 ); \
	( r ).WriteCount( ( s ).n.Num(), SCHEMA_LIST_WIDTH a, SCHEMA_LIST_MAX a ); \
	for( int i = 0; i < ( s ).n.Num(); i++ ) \
	{ \
		SCHEMA_CAT( WriteRecord, SCHEMA_LIST_ELEMENT a )( r, ( s ).n[i] ); \
	}
#define SCHEMA_RSGET_LIST( r, s, n, a ) \
	{ \
		compile_time_assert( SCHEMA_LIST_WIDTH a <= 4 ); \
		int count; \
		if( !( r ).ReadCount( count, SCHEMA_LIST_WIDTH a, SCHEMA_LIST_MAX a ) ) \
		{ \
			( r ).good = false; \
		} \
		for( int i = 0; i < count && ( r ).good; i++ ) \
		{ \
			SCHEMA_CAT3( recordStruct, SCHEMA_LIST_ELEMENT a, _t ) element; \
			SCHEMA_CAT( ReadRecord, SCHEMA_LIST_ELEMENT a )( r, element ); \
			if( ( r ).good && SCHEMA_CAT( Check, SCHEMA_LIST_ELEMENT a )( element, s ) ) \
			{ \
				( s ).n.Append( element ); \
			} \
			else \
			{ \
				( r ).good = false; \
			} \
		} \
	}
//puncho end

// save types: f is the stream, s the instance, n the field, a the argument
#define SCHEMA_SAVE_INT( f, s, n, a )			( f )->WriteInt( ( s ).n );
#define SCHEMA_RESTORE_INT( f, s, n, a )		( f )->ReadInt( ( s ).n );
#define SCHEMA_SAVE_FLOAT( f, s, n, a )			( f )->WriteFloat( ( s ).n );
#define SCHEMA_RESTORE_FLOAT( f, s, n, a )		( f )->ReadFloat( ( s ).n );
#define SCHEMA_SAVE_BOOL( f, s, n, a )			( f )->WriteBool( ( s ).n );
#define SCHEMA_RESTORE_BOOL( f, s, n, a )		( f )->ReadBool( ( s ).n );
#define SCHEMA_SAVE_ENUM( f, s, n, a )			( f )->WriteInt( ( int )( ( s ).n ) );
#define SCHEMA_RESTORE_ENUM( f, s, n, a )		( f )->ReadInt( ( int& )( ( s ).n ) );
#define SCHEMA_SAVE_JOINT( f, s, n, a )			( f )->WriteJoint( ( s ).n );
#define SCHEMA_RESTORE_JOINT( f, s, n, a )		( f )->ReadJoint( ( s ).n );
#define SCHEMA_SAVE_VEC3( f, s, n, a )			( f )->WriteVec3( ( s ).n );
#define SCHEMA_RESTORE_VEC3( f, s, n, a )		( f )->ReadVec3( ( s ).n );
#define SCHEMA_SAVE_MAT3( f, s, n, a )			( f )->WriteMat3( ( s ).n );
#define SCHEMA_RESTORE_MAT3( f, s, n, a )		( f )->ReadMat3( ( s ).n );
#define SCHEMA_SAVE_BOUNDS( f, s, n, a )		( f )->WriteBounds( ( s ).n );
#define SCHEMA_RESTORE_BOUNDS( f, s, n, a )		( f )->ReadBounds( ( s ).n );
#define SCHEMA_SAVE_QUAT( f, s, n, a )			( f )->WriteFloat( ( s ).n.x ); ( f )->WriteFloat( ( s ).n.y ); ( f )->WriteFloat( ( s ).n.z ); ( f )->WriteFloat( ( s ).n.w );
#define SCHEMA_RESTORE_QUAT( f, s, n, a )		( f )->ReadFloat( ( s ).n.x ); ( f )->ReadFloat( ( s ).n.y ); ( f )->ReadFloat( ( s ).n.z ); ( f )->ReadFloat( ( s ).n.w );
#define SCHEMA_SAVE_STRING( f, s, n, a )		( f )->WriteString( ( s ).n );
#define SCHEMA_RESTORE_STRING( f, s, n, a )		( f )->ReadString( ( s ).n );
#define SCHEMA_SAVE_DICT( f, s, n, a )			( f )->WriteDict( &( s ).n );
#define SCHEMA_RESTORE_DICT( f, s, n, a )		( f )->ReadDict( &( s ).n );
#define SCHEMA_SAVE_OBJECT( f, s, n, a )		( f )->WriteObject( ( s ).n );
#define SCHEMA_RESTORE_OBJECT( f, s, n, a )		( f )->ReadObject( reinterpret_cast<idClass*&>( ( s ).n ) );
#define SCHEMA_SAVE_STATIC( f, s, n, a )		( f )->WriteStaticObject( ( s ).n );
#define SCHEMA_RESTORE_STATIC( f, s, n, a )		( f )->ReadStaticObject( ( s ).n );
#define SCHEMA_SAVE_STRUCT( f, s, n, a )		( s ).n.Save( f );
#define SCHEMA_RESTORE_STRUCT( f, s, n, a )		( s ).n.Restore( f );
#define SCHEMA_SAVE_MATERIAL( f, s, n, a )		( f )->WriteMaterial( ( s ).n );
#define SCHEMA_RESTORE_MATERIAL( f, s, n, a )	( f )->ReadMaterial( ( s ).n );
#define SCHEMA_SAVE_MODELDEF( f, s, n, a )		( f )->WriteModelDef( ( s ).n );
#define SCHEMA_RESTORE_MODELDEF( f, s, n, a )	( f )->ReadModelDef( ( s ).n );
#define SCHEMA_SAVE_USERCMD( f, s, n, a )		( f )->WriteUsercmd( ( s ).n );
#define SCHEMA_RESTORE_USERCMD( f, s, n, a )	( f )->ReadUsercmd( ( s ).n );
#define SCHEMA_SAVE_TRACE( f, s, n, a )			( f )->WriteTrace( ( s ).n );
#define SCHEMA_RESTORE_TRACE( f, s, n, a )		( f )->ReadTrace( ( s ).n );
#define SCHEMA_SAVE_HAND( f, s, n, a )			( s ).Save##n( f );
#define SCHEMA_RESTORE_HAND( f, s, n, a )		( s ).Restore##n( f );

// an idList: the count, then each element as its type, or through the element functions of a struct
#define SCHEMA_SAVE_LIST( f, s, n, a )			( f )->WriteInt( ( s ).n.Num() ); for( int i = 0; i < ( s ).n.Num(); i++ ) { SCHEMA_SAVE_##a( f, s, n[ i ], 0 ) }
#define SCHEMA_RESTORE_LIST( f, s, n, a )		{ int num; ( f )->ReadInt( num ); ( s ).n.SetNum( num ); for( int i = 0; i < num; i++ ) { SCHEMA_RESTORE_##a( f, s, n[ i ], 0 ) } }
#define SCHEMA_SAVE_STRUCTLIST( f, s, n, a )	( f )->WriteInt( ( s ).n.Num() ); for( int i = 0; i < ( s ).n.Num(); i++ ) { Save##a( f, ( s ).n[ i ] ); }
#define SCHEMA_RESTORE_STRUCTLIST( f, s, n, a )	{ int num; ( f )->ReadInt( num ); ( s ).n.SetNum( num ); for( int i = 0; i < num; i++ ) { Restore##a( f, ( s ).n[ i ] ); } }

// the element functions of a struct a STRUCTLIST holds, expanded from the struct's own list (a list cannot expand
// itself inside another's expansion, so the nesting goes through a function)
#define SCHEMA_STRUCT_FUNCTIONS( name, type, LIST ) \
	static void Save##name( idSaveGame* savefile, const type& s ) { LIST( SCHEMA_SAVE, s ) } \
	static void Restore##name( idRestoreGame* savefile, type& s ) { LIST( SCHEMA_RESTORE, s ) }

// wire types: m is the message
#define SCHEMA_PAIR( x, y )						x, y
#define SCHEMA_WRITE_VEC3_RAW( m, s, n, a )		( m ).WriteFloat( ( s ).n[0] ); ( m ).WriteFloat( ( s ).n[1] ); ( m ).WriteFloat( ( s ).n[2] );
#define SCHEMA_READ_VEC3_RAW( m, s, n, a )		( s ).n[0] = ( m ).ReadFloat(); ( s ).n[1] = ( m ).ReadFloat(); ( s ).n[2] = ( m ).ReadFloat();
#define SCHEMA_WRITE_VEC3_BITS( m, s, n, a )	( m ).WriteFloat( ( s ).n[0], SCHEMA_PAIR a ); ( m ).WriteFloat( ( s ).n[1], SCHEMA_PAIR a ); ( m ).WriteFloat( ( s ).n[2], SCHEMA_PAIR a );
#define SCHEMA_READ_VEC3_BITS( m, s, n, a )		( s ).n[0] = ( m ).ReadFloat a; ( s ).n[1] = ( m ).ReadFloat a; ( s ).n[2] = ( m ).ReadFloat a;
#define SCHEMA_WRITE_VEC3_DELTA( m, s, n, a )	( m ).WriteDeltaFloat( ( s ).a[0], ( s ).n[0] ); ( m ).WriteDeltaFloat( ( s ).a[1], ( s ).n[1] ); ( m ).WriteDeltaFloat( ( s ).a[2], ( s ).n[2] );
#define SCHEMA_READ_VEC3_DELTA( m, s, n, a )	( s ).n[0] = ( m ).ReadDeltaFloat( ( s ).a[0] ); ( s ).n[1] = ( m ).ReadDeltaFloat( ( s ).a[1] ); ( s ).n[2] = ( m ).ReadDeltaFloat( ( s ).a[2] );
#define SCHEMA_WRITE_STATIC_RAW( m, s, n, a )	( s ).n.WriteToSnapshot( m );
#define SCHEMA_READ_STATIC_RAW( m, s, n, a )	( s ).n.ReadFromSnapshot( m );
#define SCHEMA_WRITE_HAND_RAW( m, s, n, a )		( s ).Write##n##ToSnapshot( m );
#define SCHEMA_READ_HAND_RAW( m, s, n, a )		( s ).Read##n##FromSnapshot( m );

//puncho start - P2b.2.3b: spawn types (K1). A stored row reads into its member, a HOT row into its column through the
// setter (the hot table lands with P2b.2.1a), a KEYONLY row not at all; the keyable types are INT FLOAT BOOL STRING
// VEC3 ENUM (an enum through int&), and every other type takes no KEY
#define SCHEMA_SPAWN_OBJ( s, n, TYPE, K, D )		SCHEMA_GET_##TYPE( s, n, K, D )
#define SCHEMA_SPAWN_BOTH( s, n, TYPE, K, D )		SCHEMA_GET_##TYPE( s, n, K, D )
#define SCHEMA_SPAWN_KEYONLY( s, n, TYPE, K, D )
#define SCHEMA_SPAWN_HOT( s, n, TYPE, K, D )		{ SCHEMA_CTYPE_##TYPE v; SCHEMA_GET_V_##TYPE( v, K, D ) gameLocal.entityHot.Set_##n( ( s ).entityNumber, v ); }

#define SCHEMA_GET_INT( s, n, K, D )			idSchemaGet( spawnArgs, K, D, ( s ).n );
#define SCHEMA_GET_FLOAT( s, n, K, D )			idSchemaGet( spawnArgs, K, D, ( s ).n );
#define SCHEMA_GET_BOOL( s, n, K, D )			idSchemaGet( spawnArgs, K, D, ( s ).n );
#define SCHEMA_GET_STRING( s, n, K, D )			idSchemaGet( spawnArgs, K, D, ( s ).n );
#define SCHEMA_GET_VEC3( s, n, K, D )			idSchemaGet( spawnArgs, K, D, ( s ).n );
#define SCHEMA_GET_ENUM( s, n, K, D )			idSchemaGet( spawnArgs, K, D, ( int& )( ( s ).n ) );
#define SCHEMA_GET_V_INT( v, K, D )				idSchemaGet( spawnArgs, K, D, v );
#define SCHEMA_GET_V_FLOAT( v, K, D )			idSchemaGet( spawnArgs, K, D, v );
#define SCHEMA_GET_V_BOOL( v, K, D )			idSchemaGet( spawnArgs, K, D, v );
#define SCHEMA_GET_V_STRING( v, K, D )			idSchemaGet( spawnArgs, K, D, v );
#define SCHEMA_GET_V_VEC3( v, K, D )			idSchemaGet( spawnArgs, K, D, v );
#define SCHEMA_GET_V_ENUM( v, K, D )			idSchemaGet( spawnArgs, K, D, v );
#define SCHEMA_CTYPE_INT						int
#define SCHEMA_CTYPE_FLOAT						float
#define SCHEMA_CTYPE_BOOL						bool
#define SCHEMA_CTYPE_STRING						idStr
#define SCHEMA_CTYPE_VEC3						idVec3
#define SCHEMA_CTYPE_ENUM						int

#define SCHEMA_GET_NOKEY( n, K )				static_assert( idSchemaIsNull( K ), #n ": this type takes no KEY" );
#define SCHEMA_GET_JOINT( s, n, K, D )			SCHEMA_GET_NOKEY( n, K )
#define SCHEMA_GET_MAT3( s, n, K, D )			SCHEMA_GET_NOKEY( n, K )
#define SCHEMA_GET_BOUNDS( s, n, K, D )			SCHEMA_GET_NOKEY( n, K )
#define SCHEMA_GET_QUAT( s, n, K, D )			SCHEMA_GET_NOKEY( n, K )
#define SCHEMA_GET_DICT( s, n, K, D )			SCHEMA_GET_NOKEY( n, K )
#define SCHEMA_GET_OBJECT( s, n, K, D )			SCHEMA_GET_NOKEY( n, K )
#define SCHEMA_GET_STATIC( s, n, K, D )			SCHEMA_GET_NOKEY( n, K )
#define SCHEMA_GET_STRUCT( s, n, K, D )			SCHEMA_GET_NOKEY( n, K )
#define SCHEMA_GET_MATERIAL( s, n, K, D )		SCHEMA_GET_NOKEY( n, K )
#define SCHEMA_GET_MODELDEF( s, n, K, D )		SCHEMA_GET_NOKEY( n, K )
#define SCHEMA_GET_USERCMD( s, n, K, D )		SCHEMA_GET_NOKEY( n, K )
#define SCHEMA_GET_TRACE( s, n, K, D )			SCHEMA_GET_NOKEY( n, K )
#define SCHEMA_GET_HAND( s, n, K, D )			SCHEMA_GET_NOKEY( n, K )
#define SCHEMA_GET_LIST( s, n, K, D )			SCHEMA_GET_NOKEY( n, K )
#define SCHEMA_GET_STRUCTLIST( s, n, K, D )		SCHEMA_GET_NOKEY( n, K )
#define SCHEMA_GET_U8( s, n, K, D )				SCHEMA_GET_NOKEY( n, K )
#define SCHEMA_GET_U16( s, n, K, D )			SCHEMA_GET_NOKEY( n, K )
#define SCHEMA_GET_U32( s, n, K, D )			SCHEMA_GET_NOKEY( n, K )
#define SCHEMA_GET_U64( s, n, K, D )			SCHEMA_GET_NOKEY( n, K )
#define SCHEMA_GET_I64( s, n, K, D )			SCHEMA_GET_NOKEY( n, K )
#define SCHEMA_GET_STR( s, n, K, D )			SCHEMA_GET_NOKEY( n, K )

// the FGD's value type of each type (schemaKeyType_t, Game.h): a type an FGD line cannot hold is SCHEMA_KEY_NONE, which
// is also what makes a type non-keyable to SCHEMA_KEYCHECK
#define SCHEMA_FGDTYPE_INT						SCHEMA_KEY_INTEGER
#define SCHEMA_FGDTYPE_ENUM						SCHEMA_KEY_INTEGER
#define SCHEMA_FGDTYPE_FLOAT					SCHEMA_KEY_FLOAT
#define SCHEMA_FGDTYPE_BOOL						SCHEMA_KEY_BOOL
#define SCHEMA_FGDTYPE_STRING					SCHEMA_KEY_STRING
#define SCHEMA_FGDTYPE_VEC3						SCHEMA_KEY_STRING
#define SCHEMA_FGDTYPE_JOINT					SCHEMA_KEY_NONE
#define SCHEMA_FGDTYPE_MAT3						SCHEMA_KEY_NONE
#define SCHEMA_FGDTYPE_BOUNDS					SCHEMA_KEY_NONE
#define SCHEMA_FGDTYPE_QUAT						SCHEMA_KEY_NONE
#define SCHEMA_FGDTYPE_DICT						SCHEMA_KEY_NONE
#define SCHEMA_FGDTYPE_OBJECT					SCHEMA_KEY_NONE
#define SCHEMA_FGDTYPE_STATIC					SCHEMA_KEY_NONE
#define SCHEMA_FGDTYPE_STRUCT					SCHEMA_KEY_NONE
#define SCHEMA_FGDTYPE_MATERIAL					SCHEMA_KEY_NONE
#define SCHEMA_FGDTYPE_MODELDEF					SCHEMA_KEY_NONE
#define SCHEMA_FGDTYPE_USERCMD					SCHEMA_KEY_NONE
#define SCHEMA_FGDTYPE_TRACE					SCHEMA_KEY_NONE
#define SCHEMA_FGDTYPE_HAND						SCHEMA_KEY_NONE
#define SCHEMA_FGDTYPE_LIST						SCHEMA_KEY_NONE
#define SCHEMA_FGDTYPE_STRUCTLIST				SCHEMA_KEY_NONE
#define SCHEMA_FGDTYPE_U8						SCHEMA_KEY_NONE
#define SCHEMA_FGDTYPE_U16						SCHEMA_KEY_NONE
#define SCHEMA_FGDTYPE_U32						SCHEMA_KEY_NONE
#define SCHEMA_FGDTYPE_U64						SCHEMA_KEY_NONE
#define SCHEMA_FGDTYPE_I64						SCHEMA_KEY_NONE
#define SCHEMA_FGDTYPE_STR						SCHEMA_KEY_NONE

// A class's keys for the FGD (idGame::GetSchemaKeys) and its rows' checks, in the class's .cpp after its
// CLASS_DECLARATION block: the registrar is defined after C::Type in the same file, and dynamic initialization runs
// in definition order within a translation unit, so it sets the two members after Type's constructor has run
#define CLASS_SCHEMA_KEYS( C, LIST ) \
	static const schemaKey_t C##_schemaKeys[] = { LIST( SCHEMA_FGD, 0 ) }; \
	LIST( SCHEMA_KEYCHECK, 0 ) \
	static struct C##_schemaKeysRegistrar_t \
	{ \
		C##_schemaKeysRegistrar_t() \
		{ \
			C::Type.schemaKeys = C##_schemaKeys; \
			C::Type.numSchemaKeys = ARRAY_COUNT( C##_schemaKeys ); \
		} \
	} C##_schemaKeysRegistrar;
//puncho end

#endif /* !__SCHEMA_H__ */
