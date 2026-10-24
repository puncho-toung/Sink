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

#ifndef __STREAMTRACE_H__
#define __STREAMTRACE_H__

/*
===============================================================================

	Stream traces (P1-A1c, MP-A1 tier 1)

	A trace is the shape of a save stream or of a snapshot object: one entry per primitive with its type, its size
	and, for a string, the CRC-32 of its text (never its string table offset). Nothing records until a checker arms
	a trace, and an unarmed stream pays one pointer test per primitive. No byte of a save or a snapshot changes.

	A save trace (idStreamTrace) groups its entries in scopes, kept as a stack:
	- one per class level, around the Save or Restore that CallSave_r / CallRestore_r runs, keyed by the object's
	  index in the save's object list and the class name, so a WriteStaticObject nests inside its owner;
	- the pseudo-scopes <globals> (the stream itself), <program>, <events>, <sound> and <tracemodels>;
	- a struct scope where the byte allowlist needs a key or a position to count from: idClipModel, renderEntity_t,
	  renderLight_t, usercmd_t and idDict.
	A child shows in its parent as one SP_SCOPE entry, so the parent's positions do not move with what the child
	holds. The three paths that bypass the primitives (the sound world, a GUI's state and the trace-model padding)
	record one SP_OPAQUE entry, sized by the bytes that went through the stream's file: Length() on the pipelined
	save file is the compressed size and its Tell() asserts, so the count is taken in front of it.

	A snapshot trace (idSnapshotTrace) is the sink of each object's idBitMsg in a snapshot, keyed by (snapshot time,
	object number). net_snapTrace sends the host's writes or a client's reads to a file; testSnapTraceCompare matches
	every client read with the host write of the same snapshot and object, field for field.

===============================================================================
*/

enum streamPrimitive_t
{
	SP_SCOPE,			// a child scope; hash is the CRC-32 of its name
	SP_OPAQUE,			// bytes that bypass the primitives
	SP_RAW,				// Write / Read
	SP_INT,
	SP_JOINT,
	SP_SHORT,
	SP_BYTE,
	SP_SIGNEDCHAR,
	SP_FLOAT,
	SP_BOOL,
	SP_STRING,			// the string table offset; hash is the CRC-32 of the text
	SP_VEC2,
	SP_VEC3,
	SP_VEC4,
	SP_VEC6,
	SP_WINDING,
	SP_BOUNDS,
	SP_MAT3,
	SP_ANGLES,
	SP_OBJECT,
	SP_NUM_PRIMITIVES
};

typedef struct streamTraceEntry_s
{
	int					scope;		// the scope the entry belongs to
	int					offset;		// its first byte in the stream
	int					size;		// in bytes; 0 for SP_SCOPE
	unsigned int		hash;		// a string's text or a child scope's name; 0 otherwise
	int					child;		// SP_SCOPE: the child's scope index; -1 otherwise
	byte				type;		// streamPrimitive_t
} streamTraceEntry_t;

typedef struct streamTraceScope_s
{
	int					name;		// index in idStreamTrace::names
	int					object;		// index in the save's object list; -1 for <globals> and the pseudo-scopes
	int					parent;		// -1 for <globals>
} streamTraceScope_t;

/*
================================================
idFile_StreamTrace is the file a traced save or restore goes through: it hands every byte to the stream's own file
and counts them, which gives each entry its offset and each bypass its size. With capture set it also keeps a copy,
the uncompressed stream the byte compare reads.
================================================
*/
class idFile_StreamTrace : public idFile
{
public:
							idFile_StreamTrace();

	virtual const char* 	GetName() const;
	virtual const char* 	GetFullPath() const;
	virtual int				Read( void* buffer, int len );
	virtual int				Write( const void* buffer, int len );
	virtual int				Length() const;
	virtual ID_TIME_T		Timestamp() const;
	virtual int				Tell() const;
	virtual void			ForceFlush();
	virtual void			Flush();
	virtual int				Seek( long offset, fsOrigin_t origin );

	idFile* 				file;		// the stream's real file
	idList<byte>* 			capture;	// a copy of every byte, or NULL
	int						count;		// bytes through so far
};

class idStreamTrace
{
public:
	// the next idSaveGame or idRestoreGame to be constructed traces into this; the checker sets it right before the
	// save or the load and clears it right after
	static idStreamTrace* 	armed;

							idStreamTrace();

	void					Clear();
	void					SetCapture( bool keepBytes );

	// the stream's side, called by idSaveGame and idRestoreGame
	idFile* 				Attach( idFile* streamFile );		// returns the file the stream goes through from now on
	void					Finish();							// closes the open scopes and lets go of the file
	void					SetObject( int objectIndex );		// the top-level object whose Save or Restore runs next
	void					BeginScope( const char* name );		// static storage: a class name or a literal
	void					EndScope();
	bool					EnterStruct( const char* name );	// see idSaveGame::WriteRenderEntity
	void					Primitive( streamPrimitive_t type, int size, unsigned int hash );	// after the bytes moved
	void					Opaque( int start );				// the bytes since Tell() returned start
	int						Tell() const
	{
		return counter.count;
	}

	static unsigned int		StringHash( const char* text );

	// the checks; each prints what it finds and returns the number of failures
	static int				CompareShapes( const idStreamTrace& write, const idStreamTrace& read );
	static int				CompareBytes( const idStreamTrace& a, const idStreamTrace& b, const idFile_Memory& aStrings, const idFile_Memory& bStrings );
	static int				CompareClasses( const idStreamTrace& before, const idStreamTrace& now );

	int						CheckCoverage() const;			// every byte of the stream in exactly one entry

	bool					WriteFile( const char* fileName ) const;
	bool					ReadFile( const char* fileName );

	int						NumScopes() const
	{
		return scopes.Num();
	}
	int						NumEntries() const
	{
		return entries.Num();
	}
	int						NumBytes() const
	{
		return counter.count;
	}

private:
	idStrList				names;			// scope names, each once
	idHashIndex				nameHash;
	idList<streamTraceScope_t>	scopes;		// in the order they opened
	idList<streamTraceEntry_t>	entries;	// in stream order
	idList<int>				stack;			// the open scopes
	idList<byte>			bytes;			// the captured stream, with capture set
	idFile_StreamTrace		counter;
	int						object;			// the current top-level object, -1 outside the object loop
	bool					structEntered;

	int						NameIndex( const char* name );
	const char* 			ScopeName( int scope ) const
	{
		return names[ scopes[ scope ].name ].c_str();
	}
	void					IndexScopes( idList<int>& first, idList<int>& order ) const;
	bool					SameShape( const idStreamTrace& other, int scope, int otherScope, const idList<int>& first, const idList<int>& order, const idList<int>& otherFirst, const idList<int>& otherOrder, bool compareHashes, int& position ) const;
	bool					IsAllowed( int scope, int position, const idList<int>& first, const idList<int>& order, int& rule ) const;
	bool					RuleCovers( int rule, int scope, int position, const idList<int>& first, const idList<int>& order ) const;
	void					DescribeEntry( const streamTraceEntry_t* entry, idStr& out ) const;
};

/*
================================================
idSnapshotTrace records the snapshot objects one peer writes (the host) or reads (a client) into the file named by
net_snapTrace. Each object's field list is stored once per distinct list and referenced by every object that has
it, so a long run stays a few megabytes.
================================================
*/
typedef struct snapTraceField_s
{
	int					type;		// bitMsgPrimitive_t
	int					numBits;	// as the call passed it
	unsigned int		hash;		// a string's text, else 0
} snapTraceField_t;

typedef struct snapTraceList_s
{
	int					first;		// index in fields
	int					num;
	int					className;	// index in classNames
} snapTraceList_t;

class idSnapshotTrace : public idBitMsgTrace
{
public:
							idSnapshotTrace();

	// NULL unless net_snapTrace names a file; opens or reopens it as the cvar asks
	idSnapshotTrace* 		Start( bool isWriter, int snapshotTime );
	// ends the open object and opens object number on msg; name is its class, NULL where the reader cannot know it yet
	void					Object( int number, const char* name, const idBitMsg& msg, bool isStale );
	void					End();								// the end of the snapshot: flushes
	void					Close();

	virtual void			BitMsgPrimitive( bitMsgPrimitive_t type, int numBits, unsigned int hash );

	static int				Compare( const char* hostFileName, const char* clientFileName );

private:
	idFile* 				file;
	idStr					fileName;
	bool					writes;
	int						time;
	int						objectNum;
	int						className;
	bool					stale;
	bool					objectOpen;
	idList<snapTraceField_t>	current;	// the open object's fields
	idList<snapTraceField_t>	fields;		// every distinct list, back to back
	idList<snapTraceList_t>	lists;
	idHashIndex				listHash;
	idStrList				classNames;
	idHashIndex				classHash;

	void					EndObject();
};

extern idSnapshotTrace		snapshotTrace;

#endif /* !__STREAMTRACE_H__ */
