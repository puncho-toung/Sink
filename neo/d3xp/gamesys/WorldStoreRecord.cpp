#include "../../idlib/precompiled.h"
#pragma hdrstop
#include "../Game_local.h"
#include "WorldStore.h"
#include "WorldStoreLocal.h"

//puncho start - P2b.1.2: the MREC v3 codec (WorldStoreLocal.h); the entries' sections are Record.h's. P2b.1.4: the
// header and the entry heads through the same codec, idRecordOut and idRecordIn over the chunk's idFile; a name is a
// STR of bound 63, a 64-bit value low word first
void RecordWriteHeader( idRecordOut& out, const mrecHeader_t& h )
{
	out.WriteUnsigned( h.version, 2 );
	out.WriteUnsigned( h.flags, 2 );
	out.WriteString( h.world, 63 );
	out.Write64( h.instanceId );
	out.WriteUnsigned( h.node, 2 );
	out.WriteUnsigned( h.cellId, 4 );
	out.WriteString( h.unit, 63 );
	out.WriteUnsigned( h.unitRevision, 4 );
	out.WriteUnsigned( h.entitiesCRC, 4 );
	out.WriteUnsigned( h.geometryCRC, 4 );
	out.WriteUnsigned( h.numMapEntities, 2 );
	out.Write64( ( uint64 )h.lastLeft );
	out.Write64( ( uint64 )h.cleared );
	out.WriteUnsigned( h.remains, 2 );
	out.WriteUnsigned( h.writerBuild, 4 );
	out.WriteUnsigned( h.numEntries, 4 );
}

// v2's rules on the fields it had (flags, node, a world and a map, the entity count, the times), and C1's on the cellId:
// a zero top byte is the reserved layer, which holds only CELL_ID_NONE and CELL_ID_PERSISTENT
bool RecordReadHeader( idFile_Memory& in, mrecHeader_t& h, idStr& reason )
{
	idRecordIn r;
	r.BeginFile( &in );
	uint64 left, cleared;
	if( !r.ReadUnsignedShort( h.version ) )
	{
		reason = "truncated header";
		return false;
	}
	if( h.version != MREC_VERSION )
	{
		reason = va( "MREC version %u", h.version );
		return false;
	}
	if( !r.ReadUnsignedShort( h.flags ) || h.flags > 3 || !r.ReadString( h.world, 63 ) || h.world.IsEmpty() || !r.Read64( h.instanceId ) || !r.ReadUnsignedShort( h.node ) || h.node >= 256 || ( h.instanceId == 0 && h.node != 0 ) || !r.ReadUnsignedInt( h.cellId ) || !r.ReadString( h.unit, 63 ) || h.unit.IsEmpty() || !r.ReadUnsignedInt( h.unitRevision ) || !r.ReadUnsignedInt( h.entitiesCRC ) || !r.ReadUnsignedInt( h.geometryCRC ) || !r.ReadUnsignedShort( h.numMapEntities ) || h.numMapEntities == 0 || !r.Read64( left ) || left > 0x7fffffffffffffffULL || !r.Read64( cleared ) || ( cleared > 0x7fffffffffffffffULL && cleared != ( uint64 ) - 1 ) || !r.ReadUnsignedShort( h.remains ) || !r.ReadUnsignedInt( h.writerBuild ) || !r.ReadUnsignedInt( h.numEntries ) || h.numEntries > MAX_GENTITIES )
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
	idRecordIn r;
	r.BeginFile( &in );
	unsigned short version, flags, node;
	uint64 instance;
	uint32 cellId;
	idStr world;
	if( !r.ReadUnsignedShort( version ) || version < 1 || version > MREC_VERSION || !r.ReadUnsignedShort( flags ) || !r.ReadString( world, 63 ) || !r.Read64( instance ) || ( version >= 2 && !r.ReadUnsignedShort( node ) ) || ( version >= 3 && !r.ReadUnsignedInt( cellId ) ) || !r.ReadString( unit, 63 ) )
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
	idRecordIn head;
	head.BeginFile( &in );
	uint64 when = 0;
	const bool framed = created ? ( head.Read64( e.createdId ) && head.ReadString( e.name, 63 ) && head.Read64( when ) ) : ( head.ReadUnsignedShort( e.ordinalHint ) && head.ReadUnsignedInt( e.entityCRC ) && head.ReadString( e.name, 63 ) );
	if( !framed || !head.ReadByte( e.flags ) || !head.ReadByte( e.numSections ) )
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
	idRecordIn framing;	//puncho - P2b.1.4: the created count through the record codec, as the heads around it
	framing.BeginFile( &in );
	int previousOrdinal = -1;
	uint64 previousId = 0;
	uint32 count = h.numEntries;
	for( int kind = 0; kind < 2; kind++ )
	{
		if( kind == 1 && ( !framing.ReadUnsignedInt( count ) || count > MAX_GENTITIES ) )
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
	idRecordOut writer;
	writer.BeginFile( &out );
	RecordWriteHeader( writer, header );
	const recordFrame_t frame = RecordChunkFrame( header.cellId );
	idRecordIn rec;
	for( int i = 0; i < entries.Num(); i++ )
	{
		const mrecEntry_t& e = entries[i];
		if( e.created && ( i == 0 || !entries[i - 1].created ) )
		{
			writer.WriteUnsigned( entries.Num() - i, 4 );
		}
		if( e.created )
		{
			writer.Write64( e.createdId );
			writer.WriteString( e.name, 63 );
			writer.Write64( ( uint64 )e.createdTime );
		}
		else
		{
			writer.WriteUnsigned( e.ordinalHint, 2 );
			writer.WriteUnsigned( e.entityCRC, 4 );
			writer.WriteString( e.name, 63 );
		}
		writer.WriteUnsigned( e.flags, 1 );
		writer.WriteUnsigned( e.numSections, 1 );
		RecordFrameEntry( data, header.cellId, frame, e, e.name, rec );
		for( int s = 0; s < rec.NumSections(); s++ )
		{
			writer.WriteUnsigned( rec.SectionId( s ), 1 );
			writer.WriteUnsigned( rec.SectionVersion( s ), 1 );
			writer.Write64( rec.SectionMask( s ) );
			writer.WriteUnsigned( rec.SectionBytes( s ), 2 );
			writer.WriteData( rec.SectionData( s ), rec.SectionBytes( s ) );
		}
	}
	if( entries.Num() == 0 || !entries[entries.Num() - 1].created )
	{
		writer.WriteUnsigned( 0, 4 );
	}
	return !writer.Overflowed();
}
#endif

bool idWorldStore::WriteCreated( idEntity* ent, const idVec3& landmark, idFile_Memory& out )
{
	const recordFrame_t frame = { CELL_ID_NONE, landmark, mat3_identity, RECORD_FRAME_CARRY };
	return EncodeCreated( ent, frame, out, true );
}

//puncho - P2.4 (bug review 2026-10-28): the transition carry sizes each team member before it accepts the team; that
// pass called WriteCreated, which stamped the created identity (worldCreatedId, worldCreatedTime) for good on the members of a team it
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
	uint64 identity = ent->worldCreatedId;
	uint64 created = ( uint64 )ent->worldCreatedTime;
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
		parentIdentity = parent->worldCreatedId;
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
			ent->worldCreatedId = identity;
			ent->worldCreatedTime = ( int64 )created;
		}
		if( newParentIdentity )
		{
			parent->worldCreatedId = parentIdentity;
			parent->worldCreatedTime = gameLocal.GetWorldTime();
		}
		nextCreatedId = nextIdentity;
	}
	idRecordOut head;
	head.BeginFile( &out );
	head.Write64( identity );
	head.WriteString( recordName, 63 );
	head.Write64( created );
	head.WriteUnsigned( 0, 1 );
	head.WriteUnsigned( rec.NumSections(), 1 );
	head.WriteData( rec.Data(), rec.Length() );
	return !head.Overflowed();
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
			corpse->worldCreatedId = id;
			corpse->worldCreatedTime = when;
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
	idEntity* ent = NULL;
	if( !gameLocal.SpawnEntityDef( args, &ent ) || ent == NULL )
	{
		return NULL;
	}
	ent->worldCreatedId = id;	//puncho - P2b.1.3: members, no longer spawn keys
	ent->worldCreatedTime = when;
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


//puncho - P2b.1.3: the bind a created entry's Bind field applied, held until BindCreatedRecords
void idWorldStore::AddPendingBind( const idEntity* ent, uint64 parent, short joint, short body, bool orientated )
{
	pendingBind_t& bind = pendingBinds.Alloc();
	bind.parent = parent;
	bind.entityNumber = ent->entityNumber;
	bind.spawnId = gameLocal.GetSpawnId( ent );
	bind.joint = joint;
	bind.body = body;
	bind.orientated = orientated;
}

void idWorldStore::BindCreatedRecords()
{
	struct bindRow_t
	{
		idEntity* entity;
		uint64 id, parent;
		int parentIndex, depth;
		int joint, body;
		bool orientated;
	};
	idList<bindRow_t, TAG_GAME> rows;
	idHashIndex identities;
	for( idEntity* ent = gameLocal.spawnedEntities.Next(); ent != NULL; ent = ent->spawnNode.Next() )
	{
		const uint64 id = ent->worldCreatedId;
		if( id == 0 )
		{
			continue;
		}
		bindRow_t row;
		row.entity = ent;
		row.id = id;
		row.parent = 0;
		row.parentIndex = -1;
		row.depth = -1;
		row.joint = -1;
		row.body = -1;
		row.orientated = false;
		identities.Add( identities.GenerateKey( ( int )id, ( int )( id >> 32 ) ), rows.Append( row ) );
	}
	//puncho start - P2b.1.3: each pending bind onto its entity's row, found by the entity's created identity; one whose
	// entity is gone since its Bind field applied, or that never took an identity, binds nothing
	for( int i = 0; i < pendingBinds.Num(); i++ )
	{
		const pendingBind_t& bind = pendingBinds[i];
		idEntity* ent = gameLocal.entities[bind.entityNumber];
		if( ent == NULL || gameLocal.GetSpawnId( ent ) != bind.spawnId || ent->worldCreatedId == 0 )
		{
			continue;
		}
		const uint64 id = ent->worldCreatedId;
		for( int j = identities.First( identities.GenerateKey( ( int )id, ( int )( id >> 32 ) ) ); j != -1; j = identities.Next( j ) )
		{
			if( rows[j].entity == ent )
			{
				rows[j].parent = bind.parent;
				rows[j].joint = bind.joint;
				rows[j].body = bind.body;
				rows[j].orientated = bind.orientated;
				break;
			}
		}
	}
	pendingBinds.Clear();
	//puncho end
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
		const int joint = rows[i].joint;
		const int body = rows[i].body;
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
			const int joint = rows[i].joint;
			const int body = rows[i].body;
			const bool orientated = rows[i].orientated;
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
