#include "../../idlib/precompiled.h"
#pragma hdrstop
#include "../Game_local.h"
#include "WorldStore.h"
#include "WorldStoreLocal.h"

void RecordString( idFile& f, const char* value )
{
	const int length = idStr::Length( value );
	if( length > 63 )
	{
		// every caller refuses or skips a longer name first (mapGraphCheck K12 at author time); reaching this is a
		// programmer error, not content
		gameLocal.Error( "world record name exceeds 63 bytes: %s", value );
	}
	f.WriteUnsignedChar( length );
	f.Write( value, length );
}

bool RecordReadString( idFile& f, idStr& value )
{
	byte length;
	char buffer[64];
	if( f.ReadUnsignedChar( length ) != 1 || length > 63 || f.Read( buffer, length ) != length )
	{
		return false;
	}
	if( memchr( buffer, 0, length ) != NULL )
	{
		return false;
	}
	buffer[length] = '\0';
	value = buffer;
	return true;
}

void Record64( idFile& f, uint64 value )
{
	f.WriteUnsignedInt( ( unsigned int )value );
	f.WriteUnsignedInt( ( unsigned int )( value >> 32 ) );
}

bool RecordRead64( idFile& f, uint64& value )
{
	unsigned int low, high;
	if( f.ReadUnsignedInt( low ) != 4 || f.ReadUnsignedInt( high ) != 4 )
	{
		return false;
	}
	value = ( uint64 )low | ( ( uint64 )high << 32 );
	return true;
}

//puncho start - P2b.1.2: the MREC v3 codec (WorldStoreLocal.h); the entries' sections are Record.h's
void RecordWriteHeader( idFile& out, const mrecHeader_t& h )
{
	out.WriteUnsignedShort( h.version );
	out.WriteUnsignedShort( h.flags );
	RecordString( out, h.world );
	Record64( out, h.instanceId );
	out.WriteUnsignedShort( h.node );
	out.WriteUnsignedInt( h.cellId );
	RecordString( out, h.unit );
	out.WriteUnsignedInt( h.unitRevision );
	out.WriteUnsignedInt( h.entitiesCRC );
	out.WriteUnsignedInt( h.geometryCRC );
	out.WriteUnsignedShort( h.numMapEntities );
	Record64( out, ( uint64 )h.lastLeft );
	Record64( out, ( uint64 )h.cleared );
	out.WriteUnsignedShort( h.remains );
	out.WriteUnsignedInt( h.writerBuild );
	out.WriteUnsignedInt( h.numEntries );
}

// v2's rules on the fields it had (flags, node, a world and a map, the entity count, the times), and C1's on the cellId:
// a zero top byte is the reserved layer, which holds only CELL_ID_NONE and CELL_ID_PERSISTENT
bool RecordReadHeader( idFile_Memory& in, mrecHeader_t& h, idStr& reason )
{
	uint64 left, cleared;
	if( in.ReadUnsignedShort( h.version ) != 2 )
	{
		reason = "truncated header";
		return false;
	}
	if( h.version != MREC_VERSION )
	{
		reason = va( "MREC version %u", h.version );
		return false;
	}
	if( in.ReadUnsignedShort( h.flags ) != 2 || h.flags > 3 || !RecordReadString( in, h.world ) || h.world.IsEmpty() || !RecordRead64( in, h.instanceId ) || in.ReadUnsignedShort( h.node ) != 2 || h.node >= 256 || ( h.instanceId == 0 && h.node != 0 ) || in.ReadUnsignedInt( h.cellId ) != 4 || !RecordReadString( in, h.unit ) || h.unit.IsEmpty() || in.ReadUnsignedInt( h.unitRevision ) != 4 || in.ReadUnsignedInt( h.entitiesCRC ) != 4 || in.ReadUnsignedInt( h.geometryCRC ) != 4 || in.ReadUnsignedShort( h.numMapEntities ) != 2 || h.numMapEntities == 0 || !RecordRead64( in, left ) || left > 0x7fffffffffffffffULL || !RecordRead64( in, cleared ) || ( cleared > 0x7fffffffffffffffULL && cleared != ( uint64 ) - 1 ) || in.ReadUnsignedShort( h.remains ) != 2 || in.ReadUnsignedInt( h.writerBuild ) != 4 || in.ReadUnsignedInt( h.numEntries ) != 4 || h.numEntries > MAX_GENTITIES )
	{
		reason = "malformed header";
		return false;
	}
	if( ( h.cellId >> 24 ) == 0 && h.cellId > CELL_ID_PERSISTENT )
	{
		reason = va( "header cellId %08x", h.cellId );
		return false;
	}
	h.lastLeft = ( int64 )left;
	h.cleared = ( int64 )cleared;
	return true;
}

// v1: version, flags, world, instance, map; v2 adds the node after the instance; v3 the cellId after the node
int RecordChunkVersion( const byte* data, int length, idStr& unit )
{
	idFile_Memory in( "MREC version", ( const char* )data, length );
	unsigned short version, flags, node;
	uint64 instance;
	uint32 cellId;
	idStr world;
	if( in.ReadUnsignedShort( version ) != 2 || version < 1 || version > MREC_VERSION || in.ReadUnsignedShort( flags ) != 2 || !RecordReadString( in, world ) || !RecordRead64( in, instance ) || ( version >= 2 && in.ReadUnsignedShort( node ) != 2 ) || ( version >= 3 && in.ReadUnsignedInt( cellId ) != 4 ) || !RecordReadString( in, unit ) )
	{
		unit.Clear();
		return 0;
	}
	return version;
}

recordFrame_t RecordChunkFrame( uint32 cellId )
{
	recordFrame_t frame;
	frame.cellId = cellId;
	frame.origin.Zero();
	frame.axis.Identity();
	frame.kind = RECORD_FRAME_MAP;
	if( cellId != CELL_ID_NONE && CellPlaceable( cellId ) )
	{
		frame.origin.Set( ( float )( CellX( cellId ) * CELL_SIZE ), ( float )( CellY( cellId ) * CELL_SIZE ), ( float )( CellZ( cellId ) * CELL_SIZE ) );
		frame.kind = RECORD_FRAME_CELL;
	}
	return frame;
}

// an entry's head, then its sections framed into rec; false when it cannot be delimited
static bool RecordReadEntry( idFile_Memory& in, bool created, mrecEntry_t& e, idRecordIn& rec )
{
	e.created = created;
	e.createdId = 0;
	e.createdTime = 0;
	e.entityCRC = 0;
	e.ordinalHint = 0;
	e.valid = false;
	uint64 when = 0;
	const bool head = created ? ( RecordRead64( in, e.createdId ) && RecordReadString( in, e.name ) && RecordRead64( in, when ) ) : ( in.ReadUnsignedShort( e.ordinalHint ) == 2 && in.ReadUnsignedInt( e.entityCRC ) == 4 && RecordReadString( in, e.name ) );
	if( !head || in.ReadUnsignedChar( e.flags ) != 1 || in.ReadUnsignedChar( e.numSections ) != 1 )
	{
		return false;
	}
	e.createdTime = ( int64 )when;
	e.sectionsOffset = in.Tell();
	if( !rec.ReadSections( ( const byte* )in.GetDataPtr() + e.sectionsOffset, in.Length() - e.sectionsOffset, e.numSections, e.sectionsLength ) )
	{
		return false;
	}
	in.Seek( e.sectionsLength, FS_SEEK_CUR );
	return true;
}

bool RecordReadCreatedEntry( idFile_Memory& in, mrecEntry_t& entry, idRecordIn& rec )
{
	return RecordReadEntry( in, true, entry, rec );
}

void RecordFrameEntry( const byte* data, uint32 recordCellId, const recordFrame_t& frame, const mrecEntry_t& entry, const char* defName, idRecordIn& rec )
{
	int consumed;
	rec.Begin( frame, recordCellId, entry.created, defName );
	// the scan framed these bytes already
	verify( rec.ReadSections( data + entry.sectionsOffset, entry.sectionsLength, entry.numSections, consumed ) );
}

bool RecordScanChunk( const byte* data, int length, uint64 nextIdentity, mrecHeader_t& h, idList<mrecEntry_t, TAG_GAME>& entries, idStr& reason )
{
	entries.SetNum( 0 );
	idFile_Memory in( "MREC", ( const char* )data, length );
	if( !RecordReadHeader( in, h, reason ) )
	{
		return false;
	}
	const recordFrame_t frame = RecordChunkFrame( h.cellId );
	idRecordIn rec;
	int previousOrdinal = -1;
	uint64 previousId = 0;
	uint32 count = h.numEntries;
	for( int kind = 0; kind < 2; kind++ )
	{
		if( kind == 1 && ( in.ReadUnsignedInt( count ) != 4 || count > MAX_GENTITIES ) )
		{
			reason = "malformed created count";
			return false;
		}
		for( uint32 i = 0; i < count; i++ )
		{
			mrecEntry_t& e = entries.Alloc();
			rec.Begin( frame, h.cellId, kind == 1, "" );
			if( !RecordReadEntry( in, kind == 1, e, rec ) )
			{
				reason = va( "%s entry %u cannot be framed", kind == 0 ? "authored" : "created", i );
				return false;
			}
			e.valid = RecordCheckEntry( rec, e.flags, e.reason );
			// the order and the identities: an ordinal of the map once, ascending; a created id once, ascending, below the
			// store's next identity, with a def name and a time
			if( e.valid && kind == 0 && ( e.ordinalHint >= h.numMapEntities || ( int )e.ordinalHint <= previousOrdinal ) )
			{
				e.valid = false;
				e.reason = "ordinal out of order";
			}
			if( e.valid && kind == 1 && ( e.createdId <= previousId || e.createdId >= nextIdentity || e.name.IsEmpty() || e.createdTime < 0 ) )
			{
				e.valid = false;
				e.reason = "created identity";
			}
			// the order runs over the entries kept, so one corrupt ordinal or id drops itself, not the entries after it
			if( e.valid && kind == 0 )
			{
				previousOrdinal = e.ordinalHint;
			}
			else if( e.valid )
			{
				previousId = e.createdId;
			}
		}
	}
	if( in.Tell() != in.Length() )
	{
		reason = va( "%d bytes after the last entry", in.Length() - in.Tell() );
		return false;
	}
	return true;
}

#if !defined( ID_RETAIL )
// recordTest's round trip: the chunk read and written again from what the reader took from it, the header's fields,
// each entry's head and each section's header, then its field bytes
bool RecordRewriteChunk( const byte* data, int length, uint64 nextIdentity, idFile_Memory& out )
{
	mrecHeader_t header;
	idList<mrecEntry_t, TAG_GAME> entries;
	idStr reason;
	if( !RecordScanChunk( data, length, nextIdentity, header, entries, reason ) )
	{
		return false;
	}
	RecordWriteHeader( out, header );
	const recordFrame_t frame = RecordChunkFrame( header.cellId );
	idRecordIn rec;
	for( int i = 0; i < entries.Num(); i++ )
	{
		const mrecEntry_t& e = entries[i];
		if( e.created && ( i == 0 || !entries[i - 1].created ) )
		{
			out.WriteUnsignedInt( entries.Num() - i );
		}
		if( e.created )
		{
			Record64( out, e.createdId );
			RecordString( out, e.name );
			Record64( out, ( uint64 )e.createdTime );
		}
		else
		{
			out.WriteUnsignedShort( e.ordinalHint );
			out.WriteUnsignedInt( e.entityCRC );
			RecordString( out, e.name );
		}
		out.WriteUnsignedChar( e.flags );
		out.WriteUnsignedChar( e.numSections );
		RecordFrameEntry( data, header.cellId, frame, e, e.name, rec );
		for( int s = 0; s < rec.NumSections(); s++ )
		{
			out.WriteUnsignedChar( rec.SectionId( s ) );
			out.WriteUnsignedChar( rec.SectionVersion( s ) );
			Record64( out, rec.SectionMask( s ) );
			out.WriteUnsignedShort( rec.SectionBytes( s ) );
			out.Write( rec.SectionData( s ), rec.SectionBytes( s ) );
		}
	}
	if( entries.Num() == 0 || !entries[entries.Num() - 1].created )
	{
		out.WriteUnsignedInt( 0 );
	}
	return true;
}
#endif

bool idWorldStore::WriteCreated( idEntity* ent, const idVec3& landmark, idFile_Memory& out )
{
	const recordFrame_t frame = { CELL_ID_NONE, landmark, mat3_identity, RECORD_FRAME_CARRY };
	return EncodeCreated( ent, frame, out, true );
}

//puncho - P2.4 (bug review 2026-10-28): the transition carry sizes each team member before it accepts the team; that
// pass called WriteCreated, which stamped _worldCreatedId and _worldCreatedTime for good on the members of a team it
// then rejected and on their bind parents. This writes the same bytes, with placeholder identities of the same size,
// and stamps nothing
bool idWorldStore::MeasureCreated( idEntity* ent, const idVec3& landmark, idFile_Memory& out )
{
	const recordFrame_t frame = { CELL_ID_NONE, landmark, mat3_identity, RECORD_FRAME_CARRY };
	return EncodeCreated( ent, frame, out, false );
}

// A created entry: uint64 createdId, str defName, int64 createdWorldTime, uint8 flags 0, uint8 numSections, the
// entity's sections. assign takes the entity's (and its crossing bind master's) created identity from nextCreatedId
// and stamps it once the entry is written; without it the identities a write would take are only written.
bool idWorldStore::EncodeCreated( idEntity* ent, const recordFrame_t& frame, idFile_Memory& out, bool assign )
{
	if( ent == NULL || ent->GetEntityDefName()[0] == '\0' )
	{
		return false;
	}
	uint64 nextIdentity = nextCreatedId;
	uint64 identity = strtoull( ent->spawnArgs.GetString( "_worldCreatedId", "0" ), NULL, 10 );
	uint64 created = strtoull( ent->spawnArgs.GetString( "_worldCreatedTime", "0" ), NULL, 10 );
	const bool newIdentity = identity == 0;
	if( newIdentity )
	{
		identity = nextIdentity++;
		created = ( uint64 )gameLocal.GetWorldTime();
	}
	uint64 parentIdentity = 0;
	bool newParentIdentity = false;
	idEntity* parent = ent->GetBindMaster();
	if( parent != NULL )
	{
		parentIdentity = strtoull( parent->spawnArgs.GetString( "_worldCreatedId", "0" ), NULL, 10 );
		if( parentIdentity == 0 && parent->CanCrossTransition() )
		{
			parentIdentity = nextIdentity++;
			newParentIdentity = true;
		}
	}
	// a corpse records its source def (idCorpse::WriteRecord); a name over 63 bytes is refused here, never raised at
	// the write
	static const idRecordMapArgs noBaseline = { NULL, NULL };
	idRecordOut rec;
	rec.Begin( frame, true, noBaseline, parentIdentity, ent->GetEntityDefName() );
	ent->WriteRecord( rec, frame );
	const char* recordName = rec.DefName();
	if( rec.IsRemoved() || rec.Overflowed() || !rec.HasField( RECORD_SECTION_ENTITY, idEntity::RF_Pose ) || recordName[0] == '\0' || idStr::Length( recordName ) > 63 )
	{
		return false;
	}
	compile_time_assert( MAX_WORLD_CREATED_CARRY_BYTES == 168 );
	if( ent->CanCrossTransition() && MREC_CREATED_HEAD_BYTES + idStr::Length( recordName ) + rec.Length() > MAX_WORLD_CREATED_CARRY_BYTES )
	{
		return false;
	}
	if( assign )
	{
		if( newIdentity )
		{
			ent->spawnArgs.Set( "_worldCreatedId", va( "%llu", ( unsigned long long )identity ) );
			ent->spawnArgs.Set( "_worldCreatedTime", va( "%lld", ( long long )created ) );
		}
		if( newParentIdentity )
		{
			parent->spawnArgs.Set( "_worldCreatedId", va( "%llu", ( unsigned long long )parentIdentity ) );
			parent->spawnArgs.Set( "_worldCreatedTime", va( "%lld", ( long long )gameLocal.GetWorldTime() ) );
		}
		nextCreatedId = nextIdentity;
	}
	Record64( out, identity );
	RecordString( out, recordName );
	Record64( out, created );
	out.WriteUnsignedChar( 0 );
	out.WriteUnsignedChar( rec.NumSections() );
	out.Write( rec.Data(), rec.Length() );
	return true;
}

idCorpse* SpawnCorpseRecord( const char* def, idRecordIn& rec, const recordFrame_t& frame )
{
	if( gameLocal.FindEntityDef( def, false ) == NULL )
	{
		return NULL;
	}
	idDict args;
	args.Set( "classname", "corpse" );
	idEntity* ent = NULL;
	if( !gameLocal.SpawnEntityDef( args, &ent ) || ent == NULL )
	{
		return NULL;
	}
	// the class spawned is the corpse: RecordSectionsFit below is the type check, the AI section fits only an idCorpse
	idCorpse* corpse = static_cast<idCorpse*>( ent );
	rec.Rewind();
	if( !RecordSectionsFit( corpse, rec ) || corpse->ApplyRecord( rec, frame ) != RECORD_APPLIED || !rec.Done() )
	{
		delete corpse;
		return NULL;
	}
	return corpse;
}

// A carried created entry, framed and checked in the carry's frame (the arrival landmark and the rotation between the
// two), then spawned
idEntity* idWorldStore::SpawnCreated( idFile_Memory& in, const idMat3& rotation, const idVec3& landmark )
{
	const recordFrame_t frame = { CELL_ID_NONE, landmark, rotation, RECORD_FRAME_CARRY };
	mrecEntry_t entry;
	idRecordIn rec;
	rec.Begin( frame, CELL_ID_NONE, true, "" );
	if( !RecordReadCreatedEntry( in, entry, rec ) )
	{
		gameLocal.Warning( "record: carried entry dropped: cannot be framed" );
		recordInvalid++;
		return NULL;
	}
	if( !RecordCheckEntry( rec, entry.flags, entry.reason ) || entry.name.IsEmpty() )
	{
		gameLocal.Warning( "record: carried '%s'#%llu dropped: %s", entry.name.c_str(), ( unsigned long long )entry.createdId, entry.reason.c_str() );
		recordInvalid++;
		return NULL;
	}
	RecordFrameEntry( ( const byte* )in.GetDataPtr(), CELL_ID_NONE, frame, entry, entry.name, rec );
	return SpawnCreatedEntry( "carried", entry.name, entry.createdId, entry.createdTime, rec, frame );
}

// rec holds the entry's checked sections; nothing spawns for an unknown def, an expired lifetime or a reset map, and an
// entry that cannot be built is dropped, logged against unit
idEntity* idWorldStore::SpawnCreatedEntry( const char* unit, const char* def, uint64 id, int64 when, idRecordIn& rec, const recordFrame_t& frame )
{
	const idDeclEntityDef* declaration = gameLocal.FindEntityDef( def, false );
	if( declaration == NULL )
	{
		return NULL;
	}
	if( rec.HasSection( RECORD_SECTION_AI ) )
	{
		idCorpse* corpse = SpawnCorpseRecord( def, rec, frame );
		if( corpse == NULL )
		{
			gameLocal.Warning( "record: %s '%s'#%llu dropped: no corpse could be built", unit, def, ( unsigned long long )id );
			recordInvalid++;
		}
		else
		{
			const int lifetime = corpse->spawnArgs.GetInt( "persist_lifetime", "-1" );
			const int64 death = corpse->DeathTime();
			if( ( applyingReset && !corpse->spawnArgs.GetBool( "persist_noreset" ) ) || lifetime == 0 || ( lifetime > 0 && gameLocal.GetWorldTime() - death >= ( int64 )lifetime * 1000 ) )
			{
				delete corpse;
				return NULL;
			}
			corpse->spawnArgs.Set( "_worldCreatedId", va( "%llu", ( unsigned long long )id ) );
			corpse->spawnArgs.Set( "_worldCreatedTime", va( "%llu", ( unsigned long long )when ) );
		}
		return corpse;
	}
	if( applyingReset && !declaration->dict.GetBool( "persist_noreset" ) )
	{
		return NULL;
	}
	const int lifetime = declaration->dict.GetInt( "persist_lifetime", "-1" );
	if( lifetime > 0 && ( uint64 )gameLocal.GetWorldTime() >= ( uint64 )when + ( uint64 )lifetime * 1000 )
	{
		return NULL;
	}
	idDict args;
	args.Set( "classname", def );
	args.SetBool( "dropped", true );
	args.SetInt( "persist_lifetime", lifetime );
	args.Set( "_worldCreatedId", va( "%llu", ( unsigned long long )id ) );
	args.Set( "_worldCreatedTime", va( "%llu", ( unsigned long long )when ) );
	idEntity* ent = NULL;
	if( !gameLocal.SpawnEntityDef( args, &ent ) || ent == NULL )
	{
		return NULL;
	}
	rec.Rewind();
	if( !RecordSectionsFit( ent, rec ) || ent->ApplyRecord( rec, frame ) != RECORD_APPLIED || !rec.Done() )
	{
		gameLocal.Warning( "record: %s '%s'#%llu dropped: its sections do not apply to %s", unit, def, ( unsigned long long )id, ent->GetClassname() );
		recordInvalid++;
		delete ent;
		return NULL;
	}
	return ent;
}


void idWorldStore::BindCreatedRecords()
{
	struct bindRow_t
	{
		idEntity* entity;
		uint64 id, parent;
		int parentIndex, depth;
	};
	idList<bindRow_t, TAG_GAME> rows;
	idHashIndex identities;
	for( idEntity* ent = gameLocal.spawnedEntities.Next(); ent != NULL; ent = ent->spawnNode.Next() )
	{
		const uint64 id = strtoull( ent->spawnArgs.GetString( "_worldCreatedId", "0" ), NULL, 10 );
		if( id == 0 )
		{
			continue;
		}
		bindRow_t row;
		row.entity = ent;
		row.id = id;
		row.parent = strtoull( ent->spawnArgs.GetString( "_worldBindParent", "0" ), NULL, 10 );
		row.parentIndex = -1;
		row.depth = -1;
		identities.Add( identities.GenerateKey( ( int )id, ( int )( id >> 32 ) ), rows.Append( row ) );
	}
	for( int i = 0; i < rows.Num(); i++ )
	{
		if( rows[i].parent == 0 )
		{
			rows[i].depth = 0;
			continue;
		}
		const uint64 parent = rows[i].parent;
		for( int j = identities.First( identities.GenerateKey( ( int )parent, ( int )( parent >> 32 ) ) ); j != -1; j = identities.Next( j ) )
		{
			if( rows[j].id == parent )
			{
				rows[i].parentIndex = j;
				break;
			}
		}
	}
	for( int i = 0; i < rows.Num(); i++ )
	{
		if( rows[i].parent == 0 )
		{
			continue;
		}
		int depth = 0, cursor = i;
		while( cursor >= 0 && rows[cursor].parent != 0 && depth <= rows.Num() )
		{
			cursor = rows[cursor].parentIndex;
			depth++;
		}
		idEntity* ent = rows[i].entity;
		idEntity* parent = rows[i].parentIndex >= 0 ? rows[rows[i].parentIndex].entity : NULL;
		const int joint = ent->spawnArgs.GetInt( "_worldBindJoint", "-1" );
		const int body = ent->spawnArgs.GetInt( "_worldBindBody", "-1" );
		if( cursor < 0 || depth > rows.Num() || parent == ent || parent == NULL || joint < -1 || body < -1 || ( joint >= 0 && ( body >= 0 || parent->GetAnimator() == NULL || joint >= parent->GetAnimator()->NumJoints() ) ) || ( body >= 0 && body >= parent->GetPhysics()->GetNumClipModels() ) )
		{
			gameLocal.Warning( "record: invalid or missing created bind parent for %s", ent->name.c_str() );
			continue;
		}
		rows[i].depth = depth;
	}
	// Parents first. Binding preserves each recorded world pose while deriving
	// its local transform; no map-generation/entity-slot identity enters the record.
	for( int depth = 1; depth <= rows.Num(); depth++ )
	{
		for( int i = 0; i < rows.Num(); i++ )
		{
			if( rows[i].depth != depth )
			{
				continue;
			}
			if( rows[rows[i].parentIndex].depth < 0 )
			{
				rows[i].depth = -1;
				continue;
			}
			idEntity* ent = rows[i].entity;
			idEntity* parent = rows[rows[i].parentIndex].entity;
			const int joint = ent->spawnArgs.GetInt( "_worldBindJoint", "-1" );
			const int body = ent->spawnArgs.GetInt( "_worldBindBody", "-1" );
			const bool orientated = ent->spawnArgs.GetBool( "_worldBindOrientated" );
			if( joint >= 0 )
			{
				ent->BindToJoint( parent, ( jointHandle_t )joint, orientated );
			}
			else if( body >= 0 )
			{
				ent->BindToBody( parent, body, orientated );
			}
			else
			{
				ent->Bind( parent, orientated );
			}
		}
	}
}
