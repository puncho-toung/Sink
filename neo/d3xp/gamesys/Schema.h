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
			X( s, nextSoundTime,	INT,	S,	RAW,	0,	SCHEMA_STOCK ) \
			X( s, physicsObj,		STATIC,	SW,	RAW,	0,	SCHEMA_STOCK )

	- s is the instance the expansion works on, passed per direction: idPhysics_Player saves current and saved,
	  writes snapshots from current and reads them into next.
	- The type picks the one idSaveGame / idRestoreGame call the field always had, so the bytes do not change, and a
	  member of another type does not compile.
	- The channels are S (save), W (wire: snapshots) and R (record), in that order: S, W, R, SW, SR, WR, SWR. R is the
	  map record's (MREC v3, P2b.1.2): the SCHEMA_RECORD_ expanders below and gamesys/Record.h.
	- The encoding is the wire form, RAW, BITS or DELTA; a save is always raw. Its argument, 0 when unused: BITS takes
	  ( exponentBits, mantissaBits ), DELTA the base field, which comes earlier in the list. A LIST takes its element
	  type, a STRUCTLIST the name of the element functions SCHEMA_STRUCT_FUNCTIONS made.
	- The build number is the one that added the field. A restore reads it only from saves of that build or later,
	  and leaves the spawn value otherwise. SCHEMA_STOCK marks a field every save this tree reads has, and folds the
	  test away, so a stock field restores with the code the hand-written call compiled to.

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

// one entry, per direction
#define SCHEMA_SAVE( s, n, TYPE, CH, ENC, ARG, BUILD )		SCHEMA_S_##CH( SCHEMA_SAVE_##TYPE( savefile, s, n, ARG ) )
#define SCHEMA_RESTORE( s, n, TYPE, CH, ENC, ARG, BUILD )	SCHEMA_S_##CH( if( ( BUILD ) == SCHEMA_STOCK || savefile->GetBuildNumber() >= ( BUILD ) ) { SCHEMA_RESTORE_##TYPE( savefile, s, n, ARG ) } )
#define SCHEMA_WRITE( s, n, TYPE, CH, ENC, ARG, BUILD )		SCHEMA_W_##CH( SCHEMA_WRITE_##TYPE##_##ENC( msg, s, n, ARG ) )
#define SCHEMA_READ( s, n, TYPE, CH, ENC, ARG, BUILD )		SCHEMA_W_##CH( SCHEMA_READ_##TYPE##_##ENC( msg, s, n, ARG ) )
// numbers the fields SF_<field> in an enum inside the class; nothing expands it yet, MD7's dirty mask will
#define SCHEMA_INDEX( s, n, TYPE, CH, ENC, ARG, BUILD )		SF_##n,

// the channel gates
#define SCHEMA_S_S( x )		x
#define SCHEMA_S_SW( x )	x
#define SCHEMA_S_SR( x )	x
#define SCHEMA_S_SWR( x )	x
#define SCHEMA_S_W( x )
#define SCHEMA_S_R( x )
#define SCHEMA_S_WR( x )
#define SCHEMA_W_W( x )		x
#define SCHEMA_W_SW( x )	x
#define SCHEMA_W_WR( x )	x
#define SCHEMA_W_SWR( x )	x
#define SCHEMA_W_S( x )
#define SCHEMA_W_R( x )
#define SCHEMA_W_SR( x )
//puncho start - P2b.1.2: the record channel's gates, variadic so an enumerator's comma passes through
#define SCHEMA_R_R( ... )	__VA_ARGS__
#define SCHEMA_R_SR( ... )	__VA_ARGS__
#define SCHEMA_R_WR( ... )	__VA_ARGS__
#define SCHEMA_R_SWR( ... )	__VA_ARGS__
#define SCHEMA_R_S( ... )
#define SCHEMA_R_W( ... )
#define SCHEMA_R_SW( ... )

// the record channel, per R row: rec is the idRecordOut or idRecordIn, s the instance (the class name for CHECK)
#define SCHEMA_RECORD_INDEX( s, n, TYPE, CH, ENC, ARG, BUILD )	SCHEMA_R_##CH( RF_##n, )
#define SCHEMA_RECORD_NAME( s, n, TYPE, CH, ENC, ARG, BUILD )	SCHEMA_R_##CH( #n, )
#define SCHEMA_RECORD_DIFF( s, n, TYPE, CH, ENC, ARG, BUILD )	SCHEMA_R_##CH( if( SCHEMA_RCHANGED_##TYPE( rec, s, n, ARG ) ) rec.Set( RF_##n ); )
#define SCHEMA_RECORD_WRITE( s, n, TYPE, CH, ENC, ARG, BUILD )	SCHEMA_R_##CH( if( rec.Has( RF_##n ) ) { SCHEMA_RPUT_##TYPE( rec, s, n, ARG ) } )
#define SCHEMA_RECORD_READ( s, n, TYPE, CH, ENC, ARG, BUILD )	SCHEMA_R_##CH( if( rec.Has( RF_##n ) && !( SCHEMA_RGET_##TYPE( rec, s, n, ARG ) ) ) { rec.good = false; } )
#define SCHEMA_RECORD_CHECK( s, n, TYPE, CH, ENC, ARG, BUILD )	SCHEMA_R_##CH( if( rec.Has( RF_##n ) && !( SCHEMA_RCHECK_##TYPE( rec, s, n, ARG ) ) ) { rec.good = false; } )

// record HAND rows: the field functions, inside the byte bounds of the field
#define SCHEMA_RCHANGED_HAND( r, s, n, a )		( s ).Record##n##Changed( r )
#define SCHEMA_RPUT_HAND( r, s, n, a )			( r ).BeginField( a ); ( s ).WriteRecord##n( r ); ( r ).EndField( a );
#define SCHEMA_RGET_HAND( r, s, n, a )			( ( r ).BeginField( a ) && ( s ).ApplyRecord##n( r ) && ( r ).EndField() )
#define SCHEMA_RCHECK_HAND( r, s, n, a )		( ( r ).BeginField( a ) && s::CheckRecord##n( r ) && ( r ).EndField() )
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

#endif /* !__SCHEMA_H__ */
