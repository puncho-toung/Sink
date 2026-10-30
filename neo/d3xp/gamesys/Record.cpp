#include "../../idlib/precompiled.h"
#pragma hdrstop
#include "../Game_local.h"
#include "Schema.h"
#include "Record.h"

/*
===============================================================================

	The record entry codec (P2b.1.2). The bytes are little endian, put and taken a byte at a time, so the format is
	the same on any host and no idFile call sits between a field and its bounds.

===============================================================================
*/

const char* idRecordMapArgs::GetString( const char* key, const char* defaultString ) const
{
	const idKeyValue* kv = map != NULL ? map->FindKey( key ) : NULL;
	if( kv == NULL && def != NULL )
	{
		kv = def->FindKey( key );
	}
	return kv != NULL ? kv->GetValue().c_str() : defaultString;
}

int idRecordMapArgs::GetInt( const char* key, const char* defaultString ) const
{
	return atoi( GetString( key, defaultString ) );
}

bool idRecordMapArgs::GetBool( const char* key, const char* defaultString ) const
{
	return atoi( GetString( key, defaultString ) ) != 0;
}

/*
================
idRecordOut
================
*/
idRecordOut::idRecordOut()
{
	static const recordFrame_t none = { CELL_ID_NONE, vec3_origin, mat3_identity, RECORD_FRAME_MAP };
	frame = &none;
	mapArgs.map = NULL;
	mapArgs.def = NULL;
	defName = "";
	bindParent = 0;
	corpseOrigin.Zero();
	created = poseSkipped = corpse = removed = overflow = headerWritten = false;
	numSections = sectionId = sectionVersion = sectionStart = fieldStart = length = 0;
	sectionMask = 0;
}

void idRecordOut::Begin( const recordFrame_t& entryFrame, bool createdEntry, const idRecordMapArgs& baseline, uint64 parent, const char* name )
{
	frame = &entryFrame;
	mapArgs = baseline;
	defName = name;
	bindParent = parent;
	corpseOrigin.Zero();
	created = createdEntry;
	poseSkipped = corpse = removed = overflow = headerWritten = false;
	numSections = sectionId = sectionVersion = sectionStart = fieldStart = length = 0;
	sectionMask = 0;
}

void idRecordOut::SkipPose()
{
	poseSkipped = true;
}

void idRecordOut::SetCorpse( const idVec3& origin )
{
	corpse = true;
	corpseOrigin = origin;
}

void idRecordOut::SetRemoved()
{
	removed = true;
	numSections = 0;
	length = 0;
	headerWritten = false;
	sectionMask = 0;
}

void idRecordOut::SetDefName( const char* name )
{
	defName = name;
}

void idRecordOut::Put( const void* bytes, int count )
{
	if( overflow || removed || length + count > RECORD_MAX_ENTRY_BYTES )
	{
		overflow = !removed;
		return;
	}
	memcpy( data + length, bytes, count );
	length += count;
}

void idRecordOut::BeginSection( int id, int version )
{
	assert( id > 0 && id < RECORD_SECTION_IDS );
	sectionId = id;
	sectionVersion = version;
	sectionMask = 0;
	headerWritten = false;
}

void idRecordOut::Set( int bit )
{
	assert( bit >= 0 && bit < 64 );
	sectionMask |= BIT( bit );
}

bool idRecordOut::Has( int bit ) const
{
	return ( sectionMask & BIT( bit ) ) != 0;
}

// The header goes in front of the first field: the mask is final by then (every DIFF runs before the first WRITE), so
// only the byte count is left to fill in at EndSection, and a section with no set field writes nothing.
void idRecordOut::BeginField( int bytes )
{
	if( !headerWritten && !removed )
	{
		if( numSections >= RECORD_MAX_SECTIONS )
		{
			overflow = true;
			return;
		}
		sectionStart = length;
		byte header[RECORD_SECTION_HEADER_BYTES];
		header[0] = ( byte )sectionId;
		header[1] = ( byte )sectionVersion;
		for( int i = 0; i < 8; i++ )
		{
			header[2 + i] = ( byte )( sectionMask >> ( i * 8 ) );
		}
		header[10] = header[11] = 0;
		Put( header, sizeof( header ) );
		headerWritten = true;
	}
	if( bytes == 0 )
	{
		WriteShort( 0 );	// the length, filled in by EndField
	}
	fieldStart = length;
}

void idRecordOut::EndField( int bytes )
{
	if( overflow || removed )
	{
		return;
	}
	const int written = length - fieldStart;
	if( bytes != 0 )
	{
		// a fixed row writes exactly its ARG: the reader frames the next field from it
		assert( written == bytes );
		if( written != bytes )
		{
			overflow = true;
		}
		return;
	}
	if( written > RECORD_MAX_FIELD_BYTES )
	{
		overflow = true;
		return;
	}
	data[fieldStart - 2] = ( byte )written;
	data[fieldStart - 1] = ( byte )( written >> 8 );
}

void idRecordOut::EndSection()
{
	if( !headerWritten || removed )
	{
		return;
	}
	const int bytes = length - sectionStart - RECORD_SECTION_HEADER_BYTES;
	if( overflow || bytes > 65535 )
	{
		overflow = true;
		return;
	}
	data[sectionStart + 10] = ( byte )bytes;
	data[sectionStart + 11] = ( byte )( bytes >> 8 );
	ids[numSections] = ( byte )sectionId;
	masks[numSections] = sectionMask;
	numSections++;
	headerWritten = false;
}

bool idRecordOut::HasField( int section, int bit ) const
{
	for( int i = 0; i < numSections; i++ )
	{
		if( ids[i] == section )
		{
			return ( masks[i] & BIT( bit ) ) != 0;
		}
	}
	return false;
}

void idRecordOut::WriteByte( int value )
{
	const byte b = ( byte )value;
	Put( &b, 1 );
}

void idRecordOut::WriteShort( int value )
{
	const byte b[2] = { ( byte )value, ( byte )( value >> 8 ) };
	Put( b, 2 );
}

void idRecordOut::WriteUnsignedInt( uint32 value )
{
	const byte b[4] = { ( byte )value, ( byte )( value >> 8 ), ( byte )( value >> 16 ), ( byte )( value >> 24 ) };
	Put( b, 4 );
}

void idRecordOut::WriteInt( int value )
{
	WriteUnsignedInt( ( uint32 )value );
}

void idRecordOut::WriteFloat( float value )
{
	uint32 bits;
	memcpy( &bits, &value, 4 );
	WriteUnsignedInt( bits );
}

void idRecordOut::Write64( uint64 value )
{
	WriteUnsignedInt( ( uint32 )value );
	WriteUnsignedInt( ( uint32 )( value >> 32 ) );
}

void idRecordOut::WriteData( const void* bytes, int count )
{
	Put( bytes, count );
}

/*
================
idRecordIn
================
*/
idRecordIn::idRecordIn()
{
	static const recordFrame_t none = { CELL_ID_NONE, vec3_origin, mat3_identity, RECORD_FRAME_MAP };
	Begin( none, CELL_ID_NONE, false, "" );
}

void idRecordIn::Begin( const recordFrame_t& entryFrame, uint32 cellId, bool createdEntry, const char* name )
{
	frame = &entryFrame;
	recordCellId = cellId;
	created = createdEntry;
	defName = name;
	base = NULL;
	overfull = false;
	numSections = next = 0;
	mask = 0;
	cursor = fieldEnd = sectionEnd = 0;
	good = true;
	posed = false;
	poseCellId = CELL_ID_NONE;
	poseOrigin.Zero();
	poseAxis.Identity();
}

bool idRecordIn::ReadSections( const byte* data, int length, int count, int& consumed )
{
	base = data;
	numSections = 0;
	next = 0;
	consumed = 0;
	int offset = 0;
	for( int i = 0; i < count; i++ )
	{
		if( length - offset < RECORD_SECTION_HEADER_BYTES )
		{
			return false;
		}
		const byte* h = data + offset;
		uint64 sectionMask = 0;
		for( int b = 0; b < 8; b++ )
		{
			sectionMask |= ( uint64 )h[2 + b] << ( b * 8 );
		}
		const int bytes = h[10] | ( h[11] << 8 );
		offset += RECORD_SECTION_HEADER_BYTES;
		if( bytes > length - offset )
		{
			return false;
		}
		// past the limit the entry is still framed, so the record goes on, and the entry check drops it
		if( numSections < RECORD_MAX_SECTIONS )
		{
			section_t& s = sections[numSections++];
			s.id = h[0];
			s.version = h[1];
			s.mask = sectionMask;
			s.offset = offset;
			s.bytes = bytes;
		}
		else
		{
			overfull = true;
		}
		offset += bytes;
	}
	consumed = offset;
	return true;
}

void idRecordIn::Rewind()
{
	next = 0;
	mask = 0;
	good = true;
}

bool idRecordIn::HasSection( int id ) const
{
	for( int i = 0; i < numSections; i++ )
	{
		if( sections[i].id == id )
		{
			return true;
		}
	}
	return false;
}

int idRecordIn::FindSection( int id ) const
{
	for( int i = 0; i < numSections; i++ )
	{
		if( sections[i].id == id )
		{
			return i;
		}
	}
	return -1;
}

bool idRecordIn::HasField( int section, int bit ) const
{
	for( int i = 0; i < numSections; i++ )
	{
		if( sections[i].id == section )
		{
			return ( sections[i].mask & BIT( bit ) ) != 0;
		}
	}
	return false;
}

bool idRecordIn::BeginSection( int id )
{
	if( next >= numSections || sections[next].id != id )
	{
		return false;
	}
	BeginSectionAt( next );
	return true;
}

void idRecordIn::BeginSectionAt( int index )
{
	assert( index >= 0 && index < numSections );
	const section_t& s = sections[index];
	next = index + 1;
	mask = s.mask;
	cursor = s.offset;
	fieldEnd = s.offset;
	sectionEnd = s.offset + s.bytes;
}

void idRecordIn::EndSection()
{
	if( fieldEnd != sectionEnd )
	{
		good = false;
	}
	mask = 0;
}

// A field starts where the one before it ended, whatever its reader left unread, so one failed field cannot shift the
// fields after it.
bool idRecordIn::BeginField( int bytes )
{
	cursor = fieldEnd;
	if( bytes == 0 )
	{
		if( sectionEnd - cursor < 2 )
		{
			return false;
		}
		bytes = base[cursor] | ( base[cursor + 1] << 8 );
		cursor += 2;
		if( bytes > RECORD_MAX_FIELD_BYTES )
		{
			fieldEnd = sectionEnd;
			return false;
		}
	}
	if( bytes > sectionEnd - cursor )
	{
		fieldEnd = sectionEnd;
		return false;
	}
	fieldEnd = cursor + bytes;
	return true;
}

bool idRecordIn::EndField()
{
	return cursor == fieldEnd;
}

bool idRecordIn::ReadData( void* out, int count )
{
	if( count < 0 || count > fieldEnd - cursor )
	{
		cursor = fieldEnd;
		return false;
	}
	memcpy( out, base + cursor, count );
	cursor += count;
	return true;
}

bool idRecordIn::ReadByte( byte& value )
{
	return ReadData( &value, 1 );
}

bool idRecordIn::ReadShort( short& value )
{
	byte b[2];
	if( !ReadData( b, 2 ) )
	{
		return false;
	}
	value = ( short )( b[0] | ( b[1] << 8 ) );
	return true;
}

bool idRecordIn::ReadUnsignedInt( uint32& value )
{
	byte b[4];
	if( !ReadData( b, 4 ) )
	{
		return false;
	}
	value = ( uint32 )b[0] | ( ( uint32 )b[1] << 8 ) | ( ( uint32 )b[2] << 16 ) | ( ( uint32 )b[3] << 24 );
	return true;
}

bool idRecordIn::ReadInt( int& value )
{
	uint32 bits;
	if( !ReadUnsignedInt( bits ) )
	{
		return false;
	}
	value = ( int )bits;
	return true;
}

bool idRecordIn::ReadFloat( float& value )
{
	uint32 bits;
	if( !ReadUnsignedInt( bits ) )
	{
		return false;
	}
	memcpy( &value, &bits, 4 );
	return true;
}

bool idRecordIn::ReadFiniteFloat( float& value )
{
	uint32 bits;
	if( !ReadUnsignedInt( bits ) || ( bits & 0x7f800000U ) == 0x7f800000U )
	{
		return false;
	}
	memcpy( &value, &bits, 4 );
	return true;
}

bool idRecordIn::Read64( uint64& value )
{
	uint32 low, high;
	if( !ReadUnsignedInt( low ) || !ReadUnsignedInt( high ) )
	{
		return false;
	}
	value = ( uint64 )low | ( ( uint64 )high << 32 );
	return true;
}

/*
================
The section table: per id the list's version, its row count, its check, the class that owns it and the names
recordDump prints. A list's names come from its own rows (SCHEMA_RECORD_NAME), so the table cannot drift from the list.
================
*/
static const char* const entityRecordNames[] = { ENTITY_RECORD_SCHEMA( SCHEMA_RECORD_NAME, _ ) };
static const char* const moverBinaryRecordNames[] = { MOVER_BINARY_RECORD_SCHEMA( SCHEMA_RECORD_NAME, _ ) };
static const char* const damagableRecordNames[] = { DAMAGABLE_RECORD_SCHEMA( SCHEMA_RECORD_NAME, _ ) };
static const char* const aiRecordNames[] = { AI_RECORD_SCHEMA( SCHEMA_RECORD_NAME, _ ) };
static const char* const elevatorRecordNames[] = { ELEVATOR_RECORD_SCHEMA( SCHEMA_RECORD_NAME, _ ) };
verify_array_size( entityRecordNames, idEntity::ENTITY_RECORD_FIELDS );
verify_array_size( moverBinaryRecordNames, idMover_Binary::MOVER_BINARY_RECORD_FIELDS );
verify_array_size( damagableRecordNames, idDamagable::DAMAGABLE_RECORD_FIELDS );
verify_array_size( aiRecordNames, idAI::AI_RECORD_FIELDS );
verify_array_size( elevatorRecordNames, idElevator::ELEVATOR_RECORD_FIELDS );

struct recordSectionInfo_t
{
	int					id;
	int					version;
	int					fields;
	bool ( *check )( idRecordIn& rec );
	const idTypeInfo* 	owner;
	const idTypeInfo* 	builtOwner;		// the class the store builds in the owner's place, or NULL
	const char* 		name;
	const char* const* 	fieldNames;
};

static const recordSectionInfo_t recordSections[] =
{
	{ RECORD_SECTION_ENTITY, RECORD_VERSION_ENTITY, idEntity::ENTITY_RECORD_FIELDS, idEntity::CheckRecordSection, &idEntity::Type, NULL, "entity", entityRecordNames },
	{ RECORD_SECTION_MOVER_BINARY, RECORD_VERSION_MOVER_BINARY, idMover_Binary::MOVER_BINARY_RECORD_FIELDS, idMover_Binary::CheckRecordSection, &idMover_Binary::Type, NULL, "moverBinary", moverBinaryRecordNames },
	{ RECORD_SECTION_DAMAGABLE, RECORD_VERSION_DAMAGABLE, idDamagable::DAMAGABLE_RECORD_FIELDS, idDamagable::CheckRecordSection, &idDamagable::Type, NULL, "damagable", damagableRecordNames },
	{ RECORD_SECTION_AI, RECORD_VERSION_AI, idAI::AI_RECORD_FIELDS, idCorpse::CheckRecordSection, &idAI::Type, &idCorpse::Type, "ai", aiRecordNames },
	{ RECORD_SECTION_ELEVATOR, RECORD_VERSION_ELEVATOR, idElevator::ELEVATOR_RECORD_FIELDS, idElevator::CheckRecordSection, &idElevator::Type, NULL, "elevator", elevatorRecordNames },
};
static const int NUM_RECORD_SECTIONS = ARRAY_COUNT( recordSections );
compile_time_assert( NUM_RECORD_SECTIONS == RECORD_SECTION_LAST );

static const recordSectionInfo_t* RecordSectionInfo( int id )
{
	for( int i = 0; i < NUM_RECORD_SECTIONS; i++ )
	{
		if( recordSections[i].id == id )
		{
			return &recordSections[i];
		}
	}
	return NULL;
}

const char* RecordSectionName( int id )
{
	const recordSectionInfo_t* info = RecordSectionInfo( id );
	return info != NULL ? info->name : NULL;
}

int RecordSectionFields( int id )
{
	const recordSectionInfo_t* info = RecordSectionInfo( id );
	return info != NULL ? info->fields : 0;
}

const char* RecordFieldName( int id, int bit )
{
	const recordSectionInfo_t* info = RecordSectionInfo( id );
	return info != NULL && bit >= 0 && bit < info->fields ? info->fieldNames[bit] : NULL;
}

bool RecordCheckEntry( idRecordIn& rec, int entryFlags, idStr& reason )
{
	rec.Rewind();
	if( rec.Overfull() )
	{
		reason = "too many sections";
		return false;
	}
	// a created entry's flags are 0 and it has a section; an authored entry has only REMOVED and MIGRATED, and no
	// section iff REMOVED
	bool flagsGood;
	if( rec.IsCreated() )
	{
		flagsGood = entryFlags == 0 && rec.NumSections() > 0;
	}
	else
	{
		const bool reserved = ( entryFlags & ~( RECORD_ENTRY_REMOVED | RECORD_ENTRY_MIGRATED ) ) != 0;
		const bool removed = ( entryFlags & RECORD_ENTRY_REMOVED ) != 0;
		flagsGood = !reserved && removed == ( rec.NumSections() == 0 );
	}
	if( !flagsGood )
	{
		reason = va( "flags %02x with %d sections", entryFlags, rec.NumSections() );
		return false;
	}
	uint64 seen = 0;
	for( int i = 0; i < rec.NumSections(); i++ )
	{
		const int id = rec.SectionId( i );
		const recordSectionInfo_t* info = RecordSectionInfo( id );
		if( info == NULL )
		{
			reason = va( "unknown section %d", id );
			return false;
		}
		if( rec.SectionVersion( i ) != info->version )
		{
			reason = va( "section %d version %d", id, rec.SectionVersion( i ) );
			return false;
		}
		const uint64 fieldBits = info->fields >= 64 ? ~( uint64 )0 : BIT( info->fields ) - 1;
		if( rec.SectionMask( i ) == 0 || ( rec.SectionMask( i ) & ~fieldBits ) != 0 )
		{
			reason = va( "section %d mask %016llx", id, ( unsigned long long )rec.SectionMask( i ) );
			return false;
		}
		if( seen & BIT( id ) )
		{
			reason = va( "section %d twice", id );
			return false;
		}
		seen |= BIT( id );
		rec.BeginSectionAt( i );
		if( !info->check( rec ) || !rec.good )
		{
			reason = va( "section %d field", id );
			return false;
		}
		rec.EndSection();
		if( !rec.good )
		{
			reason = va( "section %d leftover bytes", id );
			return false;
		}
	}
	const bool corpse = rec.HasField( RECORD_SECTION_AI, idAI::RF_Corpse );
	if( ( rec.IsCreated() || corpse ) && !rec.HasField( RECORD_SECTION_ENTITY, idEntity::RF_Pose ) )
	{
		reason = "no Pose";
		return false;
	}
	const bool migrated = rec.posed && rec.poseCellId != rec.RecordCellId();
	if( migrated != ( ( entryFlags & RECORD_ENTRY_MIGRATED ) != 0 ) )
	{
		reason = "MIGRATED";
		return false;
	}
	rec.Rewind();
	return true;
}

bool RecordSectionsFit( const idEntity* ent, const idRecordIn& rec )
{
	const idTypeInfo* previous = NULL;
	for( int i = 0; i < rec.NumSections(); i++ )
	{
		const recordSectionInfo_t* info = RecordSectionInfo( rec.SectionId( i ) );
		if( info == NULL )
		{
			return false;
		}
		// the owner, or the class the store builds in its place (an idCorpse reads the AI's section)
		const idTypeInfo* owner = info->builtOwner != NULL && ent->IsType( *info->builtOwner ) ? info->builtOwner : info->owner;
		// root first: each owner is the class of the one before it or derives from it
		if( !ent->IsType( *owner ) || ( previous != NULL && ( owner == previous || !owner->IsType( *previous ) ) ) )
		{
			return false;
		}
		previous = owner;
	}
	return true;
}

// the identity frames (a map, a cell, a carry's departure) leave every value as it is: no rotation runs, so a -0 or
// the last bit of a coordinate never changes on the way into the record
idVec3 RecordLocalPoint( const recordFrame_t& frame, const idVec3& world )
{
	const idVec3 offset = world - frame.origin;
	return frame.axis == mat3_identity ? offset : offset * frame.axis.Transpose();
}

idVec3 RecordLocalVector( const recordFrame_t& frame, const idVec3& world )
{
	return frame.axis == mat3_identity ? world : world * frame.axis.Transpose();
}

idMat3 RecordLocalAxis( const recordFrame_t& frame, const idMat3& world )
{
	return frame.axis == mat3_identity ? world : world * frame.axis.Transpose();
}
