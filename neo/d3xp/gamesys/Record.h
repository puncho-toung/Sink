#ifndef __GAME_RECORD_H__
#define __GAME_RECORD_H__

/*
===============================================================================

	Map record entries on the schema's R channel (P2b.1.2, MREC v3)

	An entry holds one section per recorded class level of its entity, root first, a section only where its mask is
	not zero:

		uint8 id, uint8 version, uint64 fieldMask, uint16 bytes (the fields only), the set fields in list order

	The bits are the level's RF_<field> enumerators (Schema.h's SCHEMA_RECORD_INDEX over its R rows). Every value is
	little endian, written byte by byte here rather than through idFile. A field's bytes are its row's ARG, or a uint16
	length first when ARG is 0; idRecordOut and idRecordIn bound each field around its HAND function, so a reader never
	crosses into the next field and a short or long field fails its entry instead of the ones after it.

	An entity's WriteRecord chain sets the bit of each field that differs from the spawn baseline (spawnArgs and the
	map dict, MapArgs(), derived at commit and never stored) and writes the set fields; its ApplyRecord chain reads
	them back. Every entry is checked (each section's CheckRecordSection, through the table in Record.cpp) before any
	entry of its record applies.

	The one record codec (P2b.1.4): the same writer and reader run over an idFile (BeginFile) for the bytes no entity
	owns, the dungeon record and the recipe (Schema.h's struct lists, MapGraph.h) and the MREC and WSTO framing. The
	bytes are made here as in an entry and handed to idFile::Write, or taken from idFile::Read; a value that does not
	fit its width or bound fails the write (Overflowed), a short file fails the read.

===============================================================================
*/

class idEntity;

// section ids are never reused; 6 - 63 are reserved for MP-D1's conversions
enum recordSectionId_t
{
	RECORD_SECTION_ENTITY			= 1,	// ENTITY_RECORD_SCHEMA (Entity.h)
	RECORD_SECTION_MOVER_BINARY		= 2,	// MOVER_BINARY_RECORD_SCHEMA (Mover.h)
	RECORD_SECTION_DAMAGABLE		= 3,	// DAMAGABLE_RECORD_SCHEMA (Misc.h)
	RECORD_SECTION_AI				= 4,	// AI_RECORD_SCHEMA (ai/AI.h); an idCorpse writes it too
	RECORD_SECTION_ELEVATOR			= 5,	// ELEVATOR_RECORD_SCHEMA (Mover.h)
	RECORD_SECTION_LAST				= RECORD_SECTION_ELEVATOR,	// the highest id in use; the ids up to it are all in use
	RECORD_SECTION_IDS				= 64
};

// each list's version, written in its section header; a reader drops a section of another version
static const int RECORD_VERSION_ENTITY			= 1;
static const int RECORD_VERSION_MOVER_BINARY	= 1;
static const int RECORD_VERSION_DAMAGABLE		= 1;
static const int RECORD_VERSION_AI				= 1;
static const int RECORD_VERSION_ELEVATOR		= 1;

// an authored entry's flags; a created entry's are 0
static const int RECORD_ENTRY_REMOVED			= BIT( 0 );
static const int RECORD_ENTRY_MIGRATED			= BIT( 1 );

static const int RECORD_SECTION_HEADER_BYTES	= 12;
static const int RECORD_MAX_FIELD_BYTES			= 4096;		// a variable field's uint16 length
static const int RECORD_MAX_SECTIONS			= 16;		// per entry; a deeper class chain is a new limit
static const int RECORD_MAX_ENTRY_BYTES			= 16384;	// one entry's sections, the writer's fixed buffer

// what a recorded position is relative to: a local position is ( origin - frame.origin ) in frame axes, and applies as
// frame.origin + local * frame.axis
enum recordFrameKind_t
{
	RECORD_FRAME_MAP,		// a node map: CELL_ID_NONE, origin 0, identity axis
	RECORD_FRAME_CELL,		// stage 7: the cell's id, its origin ( cx, cy, cz ) * CELL_SIZE, identity axis
	// a carried created entry: CELL_ID_NONE and the departure landmark, at arrival the arrival landmark and the
	// rotation between the two
	RECORD_FRAME_CARRY
};

struct recordFrame_t
{
	uint32				cellId;
	idVec3				origin;
	idMat3				axis;
	recordFrameKind_t	kind;
};

enum recordApply_t
{
	RECORD_APPLIED,		// every section of the entry applied
	RECORD_REPLACED,	// the entity is replaced (an AI that died in play comes back as its corpse); the caller frees it last
	RECORD_INVALID		// a field could not apply; the caller drops the entry and logs it
};

// The baseline dict of an authored entry: the map entity's epairs, the def's value where the map has none (what
// spawnArgs held at spawn, before play changed them). A created entry has no baseline: IsValid() is false.
class idRecordMapArgs
{
public:
	const idDict* 		map;
	const idDict* 		def;

	bool				IsValid() const
	{
		return map != NULL;
	}
	const char* 		GetString( const char* key, const char* defaultString = "" ) const;
	int					GetInt( const char* key, const char* defaultString = "0" ) const;
	bool				GetBool( const char* key, const char* defaultString = "0" ) const;
};

/*
================
idRecordOut

One entry's sections as an entity's WriteRecord chain writes them. The store sets the entry's context with Begin, the
chain writes, the store copies Data()/Length() behind the entry's head. Nothing allocates: the sections go into the
fixed buffer, and an entry past it is refused (Overflowed).

Or, after BeginFile, the codec over an idFile: every Write goes to the file, no section or field framing, and
Overflowed() is a value that did not fit, after which nothing more is written.
================
*/
class idRecordOut
{
public:
	idRecordOut();

	// per entry: the frame, created (no baseline), the baseline dict of an authored entry, the created identity of a
	// created entry's bind master (0 for none) and the def name a created entry records
	void				Begin( const recordFrame_t& frame, bool created, const idRecordMapArgs& mapArgs, uint64 bindParent, const char* defName );
	// the codec over file, until the next Begin
	void				BeginFile( idFile* file );

	// what a subclass tells the root levels before it calls its parent's WriteRecord, so the root-first sections see it
	void				SkipPose();								// the level's own row carries the pose, or writing one ratchets it
	void				SetCorpse( const idVec3& origin );		// the entry holds Corpse: Pose always, at the corpse's build origin
	void				SetRemoved();							// the entity records as REMOVED: no section, whatever was written
	void				SetDefName( const char* name );			// a created entry's def (a corpse records its source def)

	// a class level's section; a section whose mask stays 0 writes nothing
	void				BeginSection( int id, int version );
	void				Set( int bit );
	bool				Has( int bit ) const;
	void				EndSection();

	// a field, around its HAND function: bytes is the row's ARG, 0 for a variable field
	void				BeginField( int bytes );
	void				EndField( int bytes );
	void				WriteByte( int value );
	void				WriteShort( int value );
	void				WriteInt( int value );
	void				WriteUnsignedInt( uint32 value );
	void				WriteFloat( float value );
	void				Write64( uint64 value );
	void				WriteData( const void* data, int length );
	void				WriteUnsigned( uint64 value, int bytes );				// 1, 2, 4 or 8 bytes; fails when value needs more
	void				WriteCount( int count, int bytes, int maxCount );		// a list's count; fails past maxCount
	void				WriteString( const char* value, int bound );			// a uint8 length, the bytes; fails past bound
	void				WriteChunk( uint32 tag, const void* data, int length );	// a WSTO chunk: tag, length, the bytes

	const recordFrame_t& Frame() const
	{
		return *frame;
	}
	bool				IsCreated() const
	{
		return created;
	}
	const idRecordMapArgs& MapArgs() const
	{
		return mapArgs;
	}
	uint64				BindParent() const
	{
		return bindParent;
	}
	bool				PoseSkipped() const
	{
		return poseSkipped;
	}
	bool				IsCorpse() const
	{
		return corpse;
	}
	const idVec3& 		CorpseOrigin() const
	{
		return corpseOrigin;
	}

	// the result, for the store
	bool				IsRemoved() const
	{
		return removed;
	}
	bool				Overflowed() const
	{
		return overflow;
	}
	int					NumSections() const
	{
		return numSections;
	}
	const byte* 		Data() const
	{
		return data;
	}
	int					Length() const
	{
		return length;
	}
	const char* 		DefName() const
	{
		return defName;
	}
	bool				HasField( int section, int bit ) const;

private:
	void				Put( const void* bytes, int count );

	idFile* 			file;			// the codec's file, NULL for an entry
	const recordFrame_t* frame;
	idRecordMapArgs		mapArgs;
	const char* 		defName;
	uint64				bindParent;
	idVec3				corpseOrigin;
	bool				created;
	bool				poseSkipped;
	bool				corpse;
	bool				removed;
	bool				overflow;
	bool				headerWritten;	// the open section's header, written by its first field
	int					numSections;
	int					sectionId;
	int					sectionVersion;
	uint64				sectionMask;
	int					sectionStart;
	int					fieldStart;
	int					length;
	uint64				masks[RECORD_MAX_SECTIONS];
	byte				ids[RECORD_MAX_SECTIONS];
	byte				data[RECORD_MAX_ENTRY_BYTES];
};

/*
================
idRecordIn

One entry's sections as a reader frames them (ReadSections: each header read, its bytes bounded), then walked by the
check table or by an entity's ApplyRecord chain. good is sticky: a failed field or a leftover byte clears it, and the
caller tests it after the list.

Or, after BeginFile, the codec over an idFile: every Read takes from the file and fails on a short file; a struct
list's failed row clears good, and its caller tests that the file ends where the list does.
================
*/
class idRecordIn
{
public:
	idRecordIn();

	// per entry: the frame, the record header's cellId, created, the def name (a created entry's, or an authored
	// entry's map entity's: a corpse built for it takes its source def from here)
	void				Begin( const recordFrame_t& frame, uint32 recordCellId, bool created, const char* defName );
	// the codec over file, good set, until the next Begin
	void				BeginFile( idFile* file );
	// frames numSections sections from data; false when one runs past length. consumed is the bytes they take.
	bool				ReadSections( const byte* data, int length, int numSections, int& consumed );
	// back to the first section, for another walk of the same entry (the corpse route re-reads it)
	void				Rewind();

	int					NumSections() const
	{
		return numSections;
	}
	// framed with more sections than RECORD_MAX_SECTIONS: the entry check drops it (Rewind keeps this)
	bool				Overfull() const
	{
		return overfull;
	}
	int					SectionId( int index ) const
	{
		assert( index >= 0 && index < numSections );
		return sections[index].id;
	}
	int					SectionVersion( int index ) const
	{
		assert( index >= 0 && index < numSections );
		return sections[index].version;
	}
	uint64				SectionMask( int index ) const
	{
		assert( index >= 0 && index < numSections );
		return sections[index].mask;
	}
	int					SectionBytes( int index ) const
	{
		assert( index >= 0 && index < numSections );
		return sections[index].bytes;
	}
	const byte* 		SectionData( int index ) const
	{
		assert( index >= 0 && index < numSections );
		return base + sections[index].offset;
	}
	bool				HasSection( int id ) const;
	int					FindSection( int id ) const;	// its index, -1 when the entry has none
	bool				HasField( int section, int bit ) const;

	// the next unread section, when it is this level's (ApplyRecord, root first)
	bool				BeginSection( int id );
	// any section by position (the check table)
	void				BeginSectionAt( int index );
	bool				Has( int bit ) const
	{
		return ( mask & BIT( bit ) ) != 0;
	}
	// clears good when the section's fields left bytes unread
	void				EndSection();
	// every section walked: an entity's chain that left one unread does not own it
	bool				Done() const
	{
		return next == numSections;
	}
	// the sections an ApplyRecord chain has begun: none when it refused the entry before any field applied
	int					SectionsBegun() const
	{
		return next;
	}

	// a field: bytes is the row's ARG, 0 for a variable field (a uint16 length first, at most RECORD_MAX_FIELD_BYTES)
	bool				BeginField( int bytes );
	bool				EndField();
	int					FieldRemaining() const
	{
		return fieldEnd - cursor;
	}
	bool				ReadByte( byte& value );
	bool				ReadShort( short& value );
	bool				ReadInt( int& value );
	bool				ReadUnsignedInt( uint32& value );
	bool				ReadFloat( float& value );
	bool				ReadFiniteFloat( float& value );	// false on an infinity or a NaN as well
	bool				Read64( uint64& value );
	bool				ReadData( void* out, int count );
	bool				ReadUnsignedShort( uint16& value );
	bool				ReadCount( int& count, int bytes, int maxCount );	// count is 0 unless it is read and within maxCount
	bool				ReadString( idStr& value, int bound );				// false past bound or on a NUL byte

	const recordFrame_t& Frame() const
	{
		return *frame;
	}
	uint32				RecordCellId() const
	{
		return recordCellId;
	}
	bool				IsCreated() const
	{
		return created;
	}
	const char* 		DefName() const
	{
		return defName;
	}

	bool				good;
	// left by the Pose read for the entry check and a later level: idCorpse::Build takes its origin from here
	bool				posed;
	uint32				poseCellId;
	idVec3				poseOrigin;
	idMat3				poseAxis;

private:
	struct section_t
	{
		uint64			mask;
		int				offset;
		int				bytes;
		byte			id;
		byte			version;
	};

	idFile* 			file;			// the codec's file, NULL for an entry
	const recordFrame_t* frame;
	const char* 		defName;
	const byte* 		base;
	uint32				recordCellId;
	bool				created;
	bool				overfull;
	int					numSections;
	int					next;
	uint64				mask;
	int					cursor;
	int					fieldEnd;
	int					sectionEnd;
	section_t			sections[RECORD_MAX_SECTIONS];
};

// The check every entry passes before any entry of its record applies: each section's id known, its version the
// list's, no mask bit past the list, no empty mask, no id twice, every set field's check and no leftover byte; then
// the entry rules (a created entry and an entry holding Corpse carry Pose; MIGRATED iff Pose.cellId differs from the
// record's), the entry flags (a created entry's 0; an authored entry's REMOVED and MIGRATED only, no section iff
// REMOVED). False with reason set when the entry is to be dropped.
bool		RecordCheckEntry( idRecordIn& rec, int entryFlags, idStr& reason );
// The sections of an entry fit ent's class chain: each section's owning class is one ent is, root first; a section the
// store builds another class for (the AI's, read by an idCorpse) fits that class too.
bool		RecordSectionsFit( const idEntity* ent, const idRecordIn& rec );
// recordDump's names: a section's list name and a field's row name, NULL past the list
const char* RecordSectionName( int id );
const char* RecordFieldName( int id, int bit );
int			RecordSectionFields( int id );

// local and world positions in a frame
idVec3		RecordLocalPoint( const recordFrame_t& frame, const idVec3& world );
idMat3		RecordLocalAxis( const recordFrame_t& frame, const idMat3& world );
idVec3		RecordLocalVector( const recordFrame_t& frame, const idVec3& world );

#endif /* !__GAME_RECORD_H__ */
