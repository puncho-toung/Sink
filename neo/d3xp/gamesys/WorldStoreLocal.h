#ifndef __GAME_WORLDSTORELOCAL_H__
#define __GAME_WORLDSTORELOCAL_H__

//puncho - P2b.1.1a: the record codec helpers that WorldStore.cpp (commit, apply, save, restore, recordDump) and
// WorldStoreRecord.cpp (codec, created entities, binds) both call, and SysCmds.cpp's recordTest; MapLandmarks.cpp needs
// none of them
void RecordString( idFile& f, const char* value );
bool RecordReadString( idFile& f, idStr& value );
void Record64( idFile& f, uint64 value );
bool RecordRead64( idFile& f, uint64& value );

//puncho start - P2b.1.2: MREC v3 (Record.h has the entries' sections). The header, little endian through idFile:
// version, flags (bit0 visited, bit1 cleared), world, instanceId, node, cellId, unit, unitRevision, entitiesCRC,
// geometryCRC, numMapEntities, lastLeft, cleared (-1 never), remains, writerBuild, numEntries; then the authored entries
// (ascending ordinal), numCreated and the created entries (ascending id)
static const int MREC_VERSION = 3;
static const int MREC_FIXED_HEADER_BYTES = 64;	// the header and the created count without the two names' bytes
static const int MREC_HEADER_CELLID_OFFSET = 15;	// the header's cellId, after the world name's bytes
static const int MREC_HEADER_NUMENTRIES_OFFSET = 56;	// the header's numEntries, after both names' bytes
static const int MREC_AUTHORED_HEAD_BYTES = 9;	// ordinalHint, entityCRC, the name's length, flags, numSections; then the name
static const int MREC_CREATED_HEAD_BYTES = 19;	// createdId, the name's length, createdWorldTime, flags, numSections; then the name

struct mrecHeader_t
{
	idStr			world;
	idStr			unit;
	uint64			instanceId;
	int64			lastLeft;
	int64			cleared;
	uint32			cellId;
	uint32			unitRevision;
	uint32			entitiesCRC;
	uint32			geometryCRC;
	uint32			writerBuild;
	uint32			numEntries;
	unsigned short	version;
	unsigned short	flags;
	unsigned short	node;
	unsigned short	numMapEntities;
	unsigned short	remains;
};

// one entry as the reader frames it: its head, where its sections lie in the chunk, and the check's verdict
struct mrecEntry_t
{
	idStr			name;			// an authored entry's localName, a created entry's defName
	idStr			reason;			// why the check drops it
	uint64			createdId;
	int64			createdTime;
	uint32			entityCRC;
	int				sectionsOffset;
	int				sectionsLength;
	unsigned short	ordinalHint;
	byte			flags;
	byte			numSections;
	bool			created;
	bool			valid;
};

void		RecordWriteHeader( idFile& out, const mrecHeader_t& header );
bool		RecordReadHeader( idFile_Memory& in, mrecHeader_t& header, idStr& reason );
// any MREC's version (1 - 3) and its map or unit name, for the line that drops an older one; 0 when unreadable
int			RecordChunkVersion( const byte* data, int length, idStr& unit );
// a record's frame: a node map's (CELL_ID_NONE), from stage 7 its cell's
recordFrame_t RecordChunkFrame( uint32 cellId );
// The reader every path shares (apply, restore, recordDump, recordTest): the header, then every entry framed and checked
// (RecordCheckEntry, the order, the created identities below nextIdentity). False, reason set, when the chunk is dropped
// whole: another version, a malformed header, an entry that cannot be framed, a byte after the last entry.
bool		RecordScanChunk( const byte* data, int length, uint64 nextIdentity, mrecHeader_t& header, idList<mrecEntry_t, TAG_GAME>& entries, idStr& reason );
// a created entry alone (a carried one), framed into rec
bool		RecordReadCreatedEntry( idFile_Memory& in, mrecEntry_t& entry, idRecordIn& rec );
// an entry's sections framed again into rec, for a walk after the scan
void		RecordFrameEntry( const byte* data, uint32 recordCellId, const recordFrame_t& frame, const mrecEntry_t& entry, const char* defName, idRecordIn& rec );
#if !defined( ID_RETAIL )
	// recordTest's round trip: read, then written again from what the reader took (the same bytes for a chunk it accepts)
	bool		RecordRewriteChunk( const byte* data, int length, uint64 nextIdentity, idFile_Memory& out );
#endif
// The corpse an entry holding Corpse builds, from the source def: idCorpse::ApplyRecord reads its sections
idCorpse*	SpawnCorpseRecord( const char* def, idRecordIn& rec, const recordFrame_t& frame );

//puncho end
#endif
