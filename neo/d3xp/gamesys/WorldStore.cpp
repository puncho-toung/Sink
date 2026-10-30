#include "../../idlib/precompiled.h"
#pragma hdrstop
#include "../Game_local.h"
#include "WorldStore.h"
#include "WorldStoreLocal.h"

idWorldStore worldStore;
idCVar g_worldRecords( "g_worldRecords", "0", CVAR_GAME | CVAR_BOOL | CVAR_ARCHIVE, "Persist map deltas across world transitions" );
idCVar g_worldName( "g_worldName", "default", CVAR_GAME | CVAR_ARCHIVE, "World record namespace" );

static idCVar g_showRecords( "g_showRecords", "0", CVAR_GAME | CVAR_BOOL, "Draw persistent entity identities" );
static idCVar g_showLandmarks( "g_showLandmarks", "0", CVAR_GAME | CVAR_BOOL, "Draw landmark frames and transition volumes" );
idCVar g_recordCapCorpses( "g_recordCapCorpses", "64", CVAR_GAME | CVAR_INTEGER | CVAR_ARCHIVE, "Per-map persistent trash corpse cap", 0, 16384 );
idCVar g_recordCapItems( "g_recordCapItems", "256", CVAR_GAME | CVAR_INTEGER | CVAR_ARCHIVE, "Per-map persistent item warning threshold (untaken items are retained)", 0, 16384 );
idCVar g_recordBytesMap( "g_recordBytesMap", "65536", CVAR_GAME | CVAR_INTEGER | CVAR_ARCHIVE, "Per-map record byte budget", 1024, 2097152 );
idCVar g_recordBytesWorld( "g_recordBytesWorld", "2097152", CVAR_GAME | CVAR_INTEGER | CVAR_ARCHIVE, "World record byte budget", 1024, 2097152 );

struct createdCandidate_t
{
	idEntity* ent;
	uint64 id, time;
	bool noCap;
	idList<byte, TAG_GAME> encoded;
};

class idSortCreatedTime : public idSort_Quick<createdCandidate_t, idSortCreatedTime>
{
public:
	int Compare( const createdCandidate_t& a, const createdCandidate_t& b ) const
	{
		if( a.time != b.time )
		{
			return a.time < b.time ? -1 : 1;
		}
		return a.id < b.id ? -1 : a.id > b.id ? 1
			   : 0;
	}
};

class idSortCreatedId : public idSort_Quick<createdCandidate_t, idSortCreatedId>
{
public:
	int Compare( const createdCandidate_t& a, const createdCandidate_t& b ) const
	{
		return a.id < b.id ? -1 : a.id > b.id ? 1
			   : 0;
	}
};

static const unsigned int WSTO_TAG = 0x4f545357, MREC_TAG = 0x4345524d, ORDT_TAG = 0x5444524f, DREC_TAG = 0x43455244;
static const int MAX_WORLD_BYTES = 2097152;

//puncho - P2b.1.2: WSTO 4 holds MREC v3 (Save writes it, Restore reads 4 and drops 3's MREC v2 chunks)
static const int WSTO_VERSION = 4;

static void RecordChunk( idFile& out, unsigned int tag, const void* bytes, int length )
{
	out.WriteUnsignedInt( tag );
	out.WriteUnsignedInt( length );
	out.Write( bytes, length );
}

static idEntity* RecordEntity( const mapSpawnRow_t& row )
{
	if( row.entityNumber < 0 || row.entityNumber >= MAX_GENTITIES )
	{
		return NULL;
	}
	idEntity* ent = gameLocal.entities[row.entityNumber];
	return ent != NULL && gameLocal.GetSpawnId( ent ) == row.spawnId ? ent : NULL;
}

idWorldStore::idWorldStore()
{
	continueNextLoad = false;
	instanceId = 0;
	currentNode = 0;
	nextCreatedId = 1;
	retiringInstance = 0;
	retiringDestination.Clear();
	committed = false;
	applyingReset = false;
	resetApplied = false;
	remains = 0;
	recordMaps = recordEntries = recordCreated = recordBytes = recordDropped = corpseEvicted = 0;
	recordInvalid = 0;
	commitUs = applyUs = 0;
	memset( removed, 0, sizeof( removed ) );
}

void idWorldStore::Clear()
{
	records.Clear();
	dungeonRecords.Clear();
	mapSpawned.Clear();
	worldName.Clear();
	instanceId = 0;
	currentNode = 0;
	nextCreatedId = 1;
	retiringInstance = 0;
	retiringDestination.Clear();
	continueNextLoad = false;
	committed = false;
	applyingReset = false;
	resetApplied = false;
	remains = 0;
	recordMaps = recordEntries = recordCreated = recordBytes = recordDropped = corpseEvicted = 0;
	recordInvalid = 0;
	commitUs = applyUs = 0;
	memset( removed, 0, sizeof( removed ) );
}

void idWorldStore::Shutdown()
{
	Clear();
}

//puncho - P2b.1.1b: StoreRecord's dungeon records: the persistent hub's first while an instance runs, then the director's
void idWorldStore::SetDungeonRecords( const idList<byte, TAG_GAME>& hub, const idList<byte, TAG_GAME>& record )
{
	dungeonRecords.Clear();
	if( hub.Num() )
	{
		dungeonRecords.Append( hub );
	}
	dungeonRecords.Append( record );
}

void idWorldStore::BeginMap( const char* map )
{
	if( !continueNextLoad )
	{
		Clear();
		if( g_worldRecords.GetBool() && !common->IsClient() )
		{
			worldName = g_worldName.GetString()[0] ? g_worldName.GetString() : "default";
		}
	}
	continueNextLoad = false;
	committed = false;
}

void idWorldStore::RetireInstanceAfterActivation( uint64 identity, const char* destination )
{
	retiringInstance = identity;
	retiringDestination = destination;
	retiringDestination.StripFileExtension();
	retiringDestination.ToLower();
	if( retiringDestination.Left( 5 ) != "maps/" )
	{
		retiringDestination.Insert( "maps/", 0 );
	}
}

void idWorldStore::FinishMapActivation()
{
	if( common->IsClient() || retiringInstance == 0 || mapGraphDirector.instanceId == retiringInstance )
	{
		return;
	}
	idStr current = gameLocal.GetMapName();
	current.StripFileExtension();
	current.ToLower();
	if( current != retiringDestination )
	{
		return;
	}
	int retired = 0;
	for( int i = records.Num() - 1; i >= 0; i-- )
	{
		if( records[i].instance == retiringInstance )
		{
			records.RemoveIndex( i );
			retired++;
		}
	}
	gameLocal.Printf( "record retired instance=%llu maps=%d after activation=%s\n", ( unsigned long long )retiringInstance, retired, current.c_str() );
	retiringInstance = 0;
	retiringDestination.Clear();
	recordMaps = records.Num();
	recordBytes = 0;
	for( int i = 0; i < records.Num(); i++ )
	{
		recordBytes += records[i].bytes.Num() + 8;
	}
}

void idWorldStore::BeginSpawn()
{
	remains = 0;
	applyingReset = false;
	resetApplied = false;
	const idMapFile* loaded = gameLocal.GetLoadedMap();
	if( IsActive() && loaded != NULL && loaded->GetNumEntities() > 65535 )
	{
		gameLocal.Error( "World map has more than 65535 authored ordinals" );
	}
	mapSpawned.Clear();
	memset( removed, 0, sizeof( removed ) );
	committed = false;
}

void idWorldStore::TrackSpawn( int ordinal, idEntity* ent )
{
	if( !IsActive() || ent == NULL )
	{
		return;
	}
	mapSpawnRow_t row;
	row.ordinal = ordinal;
	row.entityNumber = ent->entityNumber;
	row.spawnId = gameLocal.GetSpawnId( ent );
	row.departed = false;
	ent->mapSpawnIndex = mapSpawned.Append( row );
}

// The rows are in ascending ordinal order: MapPopulate tracks them in map order and ORDT is validated ascending.
int idWorldStore::FindSpawnRow( int ordinal ) const
{
	int low = 0, high = mapSpawned.Num() - 1;
	while( low <= high )
	{
		const int middle = ( low + high ) >> 1;
		if( mapSpawned[middle].ordinal < ordinal )
		{
			low = middle + 1;
		}
		else if( mapSpawned[middle].ordinal > ordinal )
		{
			high = middle - 1;
		}
		else
		{
			return middle;
		}
	}
	return -1;
}

void idWorldStore::EntityRemoved( idEntity* ent )
{
	const int index = ent->mapSpawnIndex;
	if( IsActive() && index >= 0 && index < mapSpawned.Num() )
	{
		const int ordinal = mapSpawned[index].ordinal;
		removed[ordinal >> 3] |= 1 << ( ordinal & 7 );
	}
}

void idWorldStore::MarkDeparted( idEntity* ent )
{
	if( ent->mapSpawnIndex >= 0 && ent->mapSpawnIndex < mapSpawned.Num() )
	{
		mapSpawned[ent->mapSpawnIndex].departed = true;
	}
	ent->spawnArgs.SetBool( "_worldDeparted", true );
}

//puncho - P2.4 (bug review 2026-10-28): idItem::Pickup marks the item it gave, and the record removes a marked item on
// revisit. Taken was inferred from IsHidden before, so an authored hide-until-triggered item counted as taken and was
// deleted for good when the player left before revealing it
void idWorldStore::MarkTaken( idEntity* ent )
{
	if( IsActive() )
	{
		ent->spawnArgs.SetBool( "_worldTaken", true );
	}
}

void idWorldStore::CommitCurrentMap()
{
	Commit( true );
}

//puncho start - P2.4 (bug review 2026-10-28): a commit builds this map's record from the live game and never changes
// the live game. Expired entities, corpses over the cap or the byte budget and taken items are left out of the record
// (an authored row becomes REMOVED), not deleted: every save and autosave commits too, and deleting there made corpses
// vanish in front of the player. At a leave the map is torn down right after, so leaving them out is the eviction;
// leaving only decides whether the drops count in the stats, so repeated saves do not count one corpse again.
static void RecordExclude( byte* excluded, const idEntity* ent )
{
	excluded[ent->entityNumber >> 3] |= 1 << ( ent->entityNumber & 7 );
}

// A child of a left-out entity is left out with it, as deleting the master would have removed it.
static bool RecordExcluded( const byte* excluded, const idEntity* ent )
{
	for( int depth = 0; ent != NULL && depth < MAX_GENTITIES; depth++, ent = ent->GetBindMaster() )
	{
		if( excluded[ent->entityNumber >> 3] & ( 1 << ( ent->entityNumber & 7 ) ) )
		{
			return true;
		}
	}
	return false;
}

struct recordRow_t
{
	int offset, length;
	byte flags, numSections;	// RECORD_ENTRY_REMOVED and no section, or the entity's sections
	bool written;				// the row holds an entry; an entity that matches its spawn state writes none
};
//puncho end

void idWorldStore::Commit( bool leaving )
{
	const idMapFile* map = gameLocal.GetLoadedMap();
	if( !IsActive() || committed || common->IsClient() || gameLocal.GameState() != GAMESTATE_ACTIVE || map == NULL )
	{
		return;
	}
	const uint64 commitStart = Sys_Microseconds();
	// The dungeon records are stored first so this commit budgets against their current size, at a map leave as
	// much as at a save; Save() writes whatever is in the list.
	mapGraphDirector.StoreRecord();
	byte excluded[( MAX_GENTITIES + 7 ) / 8];
	memset( excluded, 0, sizeof( excluded ) );
	// Expire before sizing any row.
	const int64 now = gameLocal.GetWorldTime();
	for( int i = MAX_CLIENTS; i < MAX_GENTITIES; i++ )
	{
		idEntity* ent = gameLocal.entities[i];
		if( ent == NULL || ent->spawnArgs.GetBool( "_worldDeparted" ) )
		{
			continue;
		}
		const int lifetime = ent->spawnArgs.GetInt( "persist_lifetime", "-1" );
		if( lifetime <= 0 )
		{
			continue;
		}
		int64 born;
		if( ent->IsType( idCorpse::Type ) )
		{
			born = static_cast<idCorpse*>( ent )->DeathTime();
		}
		else if( ent->mapSpawnIndex < 0 && !ent->IsType( idPlayer::Type ) && ent->spawnArgs.GetString( "_worldCreatedId", "0" )[0] != '0' )
		{
			born = strtoll( ent->spawnArgs.GetString( "_worldCreatedTime", "0" ), NULL, 10 );
		}
		else
		{
			continue;
		}
		if( now >= born && now - born >= ( int64 )lifetime * 1000 )
		{
			RecordExclude( excluded, ent );
		}
	}
	bool isCleared = true;
	idList<createdCandidate_t, TAG_GAME> bodies;
	for( idEntity* ent = gameLocal.spawnedEntities.Next(); ent != NULL; ent = ent->spawnNode.Next() )
	{
		if( ent->IsType( idAI::Type ) && ent->health > 0 && static_cast<idAI*>( ent )->team != 0 )
		{
			isCleared = false;
		}
		if( ent->IsType( idCorpse::Type ) && !RecordExcluded( excluded, ent ) )
		{
			createdCandidate_t body;
			body.ent = ent;
			body.id = ent->mapSpawnIndex >= 0 ? ent->mapSpawnIndex : ( uint64 )MAX_GENTITIES + ent->entityNumber;
			body.time = static_cast<idCorpse*>( ent )->DeathTime();
			body.noCap = ent->spawnArgs.GetBool( "persist_nocap" );
			bodies.Append( body );
		}
	}
	bodies.SortWithTemplate( idSortCreatedTime() );
	int evicted = 0;
	int bodyCount = bodies.Num();
	for( int i = 0; i < bodies.Num() && g_recordCapCorpses.GetInteger() > 0 && bodyCount > g_recordCapCorpses.GetInteger(); i++ )
	{
		if( bodies[i].noCap )
		{
			continue;
		}
		evicted++;
		RecordExclude( excluded, bodies[i].ent );
		bodies[i].ent = NULL;
		bodyCount--;
	}
	// Byte pressure may remove ordinary remains, never protected actors or items.
	// Size authored rows before emitting them so an evicted corpse becomes a
	// normal REMOVED tombstone with the same stable ordinal.
	idStr recordMap = gameLocal.GetMapName();
	recordMap.StripFileExtension();
	recordMap.ToLower();
	int otherRecordBytes = StoreFixedBytes();
	for( int i = 0; i < records.Num(); i++ )
	{
		if( records[i].world != worldName || records[i].instance != instanceId || records[i].node != currentNode || records[i].map != recordMap )
		{
			otherRecordBytes += records[i].bytes.Num() + 8;
		}
	}
	//puncho - P2.4 (bug review 2026-10-28): names over 63 bytes are refused or skipped with a warning, not raised as an
	// Error at the leave or the save; mapGraphCheck refuses them at author time (K12)
	if( worldName.Length() > 63 || recordMap.Length() > 63 )
	{
		gameLocal.Warning( "record commit refused: world '%s' or map '%s' name exceeds 63 bytes", worldName.c_str(), recordMap.c_str() );
		return;
	}
	const int authoredLimit = Min( g_recordBytesMap.GetInteger(), g_recordBytesWorld.GetInteger() - otherRecordBytes - 64 );
	int authoredBytes = MREC_FIXED_HEADER_BYTES + worldName.Length() + recordMap.Length();
	//puncho start - P2b.1.2: each row's sections are written once, here, by the entity's WriteRecord chain against its
	// spawn baseline (the map entity's epairs over its def); the record below copies them. An entity that matches its
	// spawn state writes no entry at all
	const recordFrame_t frame = RecordChunkFrame( CELL_ID_NONE );
	idRecordOut rec;
	idList<recordRow_t, TAG_GAME> rows;
	rows.SetNum( mapSpawned.Num() );
	idFile_Memory rowPayloads;
	for( int i = 0; i < mapSpawned.Num(); i++ )
	{
		const mapSpawnRow_t& row = mapSpawned[i];
		idEntity* ent = row.departed ? NULL : RecordEntity( row );
		if( ent != NULL && ( RecordExcluded( excluded, ent ) || ent->spawnArgs.GetBool( "_worldTaken" ) ) )
		{
			ent = NULL;
		}
		recordRow_t& out = rows[i];
		out.offset = rowPayloads.Length();
		out.length = 0;
		out.flags = 0;
		out.numSections = 0;
		out.written = false;
		const idMapEntity* mapEntity = map->GetEntity( row.ordinal );
		const char* rowName = mapEntity->epairs.GetString( "name" );
		if( idStr::Length( rowName ) > 63 )
		{
			gameLocal.Warning( "record: %s entity '%s' not recorded: name exceeds 63 bytes", recordMap.c_str(), rowName );
			continue;
		}
		if( ent != NULL )
		{
			const idDecl* def = ent->entityDefNumber >= 0 ? declManager->DeclByIndex( DECL_ENTITYDEF, ent->entityDefNumber, false ) : NULL;
			idRecordMapArgs baseline;
			baseline.map = &mapEntity->epairs;
			baseline.def = def != NULL ? &static_cast<const idDeclEntityDef*>( def )->dict : NULL;
			rec.Begin( frame, false, baseline, 0, "" );
			ent->WriteRecord( rec, frame );
			if( rec.Overflowed() )
			{
				gameLocal.Warning( "record: %s entity '%s' not recorded: its sections exceed %d bytes", recordMap.c_str(), rowName, RECORD_MAX_ENTRY_BYTES );
				continue;
			}
		}
		if( ent == NULL || rec.IsRemoved() )
		{
			out.flags = RECORD_ENTRY_REMOVED;
			out.written = true;
		}
		else if( rec.NumSections() > 0 )
		{
			out.numSections = ( byte )rec.NumSections();
			out.written = true;
			rowPayloads.Write( rec.Data(), rec.Length() );
		}
		out.length = rowPayloads.Length() - out.offset;
		if( out.written )
		{
			authoredBytes += MREC_AUTHORED_HEAD_BYTES + idStr::Length( rowName ) + out.length;
		}
	}
	//puncho end
	for( int i = 0; i < bodies.Num() && authoredBytes > authoredLimit; i++ )
	{
		if( bodies[i].noCap || bodies[i].ent == NULL )
		{
			continue;
		}
		idEntity* ent = bodies[i].ent;
		if( ent->mapSpawnIndex < 0 )
		{
			continue;
		}
		recordRow_t& out = rows[ent->mapSpawnIndex];
		if( !out.written )
		{
			continue; // not recorded at all (an overlong name), so leaving it out saves nothing
		}
		authoredBytes -= out.length;
		out.flags = RECORD_ENTRY_REMOVED;
		out.numSections = 0;
		out.length = 0;
		evicted++;
		RecordExclude( excluded, ent );
		bodies[i].ent = NULL;
	}
	int rowCount = 0;
	for( int i = 0; i < rows.Num(); i++ )
	{
		rowCount += rows[i].written ? 1 : 0;
	}
	idStr name = gameLocal.GetMapName();
	name.StripFileExtension();
	name.ToLower();
	//puncho start - P2b.1.2: MREC v3's header; a node map's cellId is CELL_ID_NONE, its unit the map name, revision 0
	mrecHeader_t header;
	header.version = MREC_VERSION;
	header.flags = isCleared ? 3 : 1;
	header.world = worldName;
	header.instanceId = instanceId;
	header.node = currentNode;
	header.cellId = CELL_ID_NONE;
	header.unit = name;
	header.unitRevision = 0;
	header.entitiesCRC = map->GetEntitiesCRC();
	header.geometryCRC = map->GetGeometryCRC();
	header.numMapEntities = map->GetNumEntities();
	header.lastLeft = gameLocal.GetWorldTime();
	header.cleared = isCleared ? gameLocal.GetWorldTime() : -1;
	header.remains = remains;
	header.writerBuild = BUILD_NUMBER;
	header.numEntries = rowCount;
	if( isCleared && !resetApplied )
	{
		for( int i = 0; i < records.Num(); i++ )
		{
			const worldMapRecord_t& previous = records[i];
			if( previous.world != worldName || previous.instance != instanceId || previous.node != currentNode || previous.map != name )
			{
				continue;
			}
			idFile_Memory old( "previous MREC", ( const char* )previous.bytes.Ptr(), previous.bytes.Num() );
			mrecHeader_t before;
			idStr reason;
			if( RecordReadHeader( old, before, reason ) && ( before.flags & 2 ) && before.cleared != -1 )
			{
				header.cleared = before.cleared;
			}
			break;
		}
	}
	idFile_Memory record;
	RecordWriteHeader( record, header );
	for( int i = 0; i < mapSpawned.Num(); i++ )
	{
		const mapSpawnRow_t& row = mapSpawned[i];
		const recordRow_t& out = rows[i];
		if( !out.written )
		{
			continue;
		}
		record.WriteUnsignedShort( row.ordinal );
		record.WriteUnsignedInt( map->GetEntityCRC( row.ordinal ) );
		RecordString( record, map->GetEntity( row.ordinal )->epairs.GetString( "name" ) );
		record.WriteUnsignedChar( out.flags );
		record.WriteUnsignedChar( out.numSections );
		record.Write( rowPayloads.GetDataPtr() + out.offset, out.length );
	}
	//puncho end
	idList<createdCandidate_t, TAG_GAME> candidates;
	for( idEntity* ent = gameLocal.spawnedEntities.Next(); ent != NULL; ent = ent->spawnNode.Next() )
	{
		const int lifetime = ent->spawnArgs.GetInt( "persist_lifetime", ent->spawnArgs.GetBool( "dropped" ) ? "-1" : "0" );
		if( ent->mapSpawnIndex >= 0 || ( ent->GetBindMaster() != NULL && ( ent->spawnArgs.GetString( "_worldCreatedId", "0" )[0] == '0' || ent->GetBindMaster()->spawnArgs.GetString( "_worldCreatedId", "0" )[0] == '0' ) ) || ent->IsType( idPlayer::Type ) || ent->spawnArgs.GetBool( "_worldDeparted" ) || lifetime == 0 || RecordExcluded( excluded, ent ) || ent->spawnArgs.GetBool( "_worldTaken" ) )
		{
			continue;
		}
		createdCandidate_t row;
		row.ent = ent;
		row.id = strtoull( ent->spawnArgs.GetString( "_worldCreatedId", "0" ), NULL, 10 );
		if( row.id == 0 )
		{
			row.id = nextCreatedId++;
			ent->spawnArgs.Set( "_worldCreatedId", va( "%llu", ( unsigned long long )row.id ) );
			ent->spawnArgs.Set( "_worldCreatedTime", va( "%lld", ( long long )gameLocal.GetWorldTime() ) );
		}
		row.time = strtoull( ent->spawnArgs.GetString( "_worldCreatedTime", "0" ), NULL, 10 );
		row.noCap = ent->spawnArgs.GetBool( "persist_nocap" );
		if( lifetime > 0 && ( uint64 )gameLocal.GetWorldTime() >= row.time + ( uint64 )lifetime * 1000 )
		{
			continue;
		}
		idFile_Memory encoded;
		if( !EncodeCreated( ent, frame, encoded, true ) )	//puncho - P2b.1.2: in the map's frame
		{
			continue;
		}
		row.encoded.SetNum( encoded.Length() );
		memcpy( row.encoded.Ptr(), encoded.GetDataPtr(), encoded.Length() );
		candidates.Append( row );
	}
	candidates.SortWithTemplate( idSortCreatedTime() );
	int otherBytes = StoreFixedBytes();
	for( int i = 0; i < records.Num(); i++ )
	{
		if( records[i].world != worldName || records[i].instance != instanceId || records[i].node != currentNode || records[i].map != name )
		{
			otherBytes += records[i].bytes.Num() + 8;
		}
	}
	const int byteLimit = Min( g_recordBytesMap.GetInteger(), g_recordBytesWorld.GetInteger() - otherBytes - 64 );
	int itemCount = 0;
	int createdBytes = 0;
	for( int i = 0; i < candidates.Num(); i++ )
	{
		createdBytes += candidates[i].encoded.Num();
	}
	for( int i = 0; i < candidates.Num(); i++ )
	{
		if( candidates[i].ent->IsType( idItem::Type ) || candidates[i].ent->IsType( idMoveableItem::Type ) )
		{
			itemCount++;
		}
	}
	for( int i = 0; i < candidates.Num(); )
	{
		const bool item = candidates[i].ent->IsType( idItem::Type ) || candidates[i].ent->IsType( idMoveableItem::Type );
		if( item && g_recordCapItems.GetInteger() > 0 && itemCount > g_recordCapItems.GetInteger() )
		{
			gameLocal.Warning( "record: item soft cap exceeded; preserving untaken items" );
			itemCount = 0; // one diagnostic per commit
		}
		const bool overBytes = record.Length() + 4 + createdBytes > byteLimit;
		if( !candidates[i].noCap && !item && candidates[i].ent->IsType( idCorpse::Type ) && overBytes )
		{
			createdBytes -= candidates[i].encoded.Num();
			candidates.RemoveIndex( i );
			evicted++;
		}
		else
		{
			i++;
		}
	}
	candidates.SortWithTemplate( idSortCreatedId() );
	idFile_Memory created;
	unsigned int count = 0;
	for( int i = 0; i < candidates.Num(); i++ )
	{
		created.Write( candidates[i].encoded.Ptr(), candidates[i].encoded.Num() );
		count++;
	}
	if( evicted > 0 || record.Length() + created.Length() > byteLimit * 3 / 4 )
	{
		gameLocal.Warning( "record: %s %d bytes, left out %d corpses; authored and nocap entries retained", name.c_str(), record.Length() + created.Length(), evicted );
	}
	record.WriteUnsignedInt( count );
	record.Write( created.GetDataPtr(), created.Length() );
	// The world budget bounds the whole saved store (StoreFixedBytes plus every record), so a record that cannot
	// fit never enters it and Save() cannot overflow MAX_WORLD_BYTES. This map keeps its previous record: the
	// overflow fails before replacing the previous save instead of raising Error inside SaveGame.
	if( record.Length() > g_recordBytesWorld.GetInteger() - otherBytes - 64 )
	{
		gameLocal.Warning( "record commit refused: %s needs %d bytes, %d left in the world budget; previous record kept", name.c_str(), record.Length(), g_recordBytesWorld.GetInteger() - otherBytes - 64 );
		return;
	}
	int index = -1;
	for( int i = 0; i < records.Num(); i++ )
	{
		if( records[i].world == worldName && records[i].instance == instanceId && records[i].node == currentNode && records[i].map == name )
		{
			index = i;
		}
	}
	if( index < 0 )
	{
		index = records.Append( worldMapRecord_t() );
	}
	worldMapRecord_t& target = records[index];
	target.world = worldName;
	target.instance = instanceId;
	target.node = currentNode;
	target.map = name;
	target.bytes.SetNum( record.Length() );
	memcpy( target.bytes.Ptr(), record.GetDataPtr(), record.Length() );
	committed = true;
	resetApplied = false;
	recordMaps = records.Num();
	recordEntries = rowCount;
	recordCreated = count;
	recordBytes = otherBytes + record.Length();
	if( leaving )
	{
		recordDropped += evicted;
		corpseEvicted += evicted;
	}
	commitUs = ( unsigned int )( Sys_Microseconds() - commitStart );
	gameLocal.Printf( "record commit map=%s entries=%d created=%d bytes=%d dropped=%d us=%u\n", name.c_str(), recordEntries, recordCreated, record.Length(), evicted, commitUs );
}

//puncho start - P2b.1.2: the children that bind at spawn to an entity the record moved

// an entity the record moved (its Pose, a mover's state, an elevator's floor), from its spawn pose to the applied one;
// posed when its entry held Pose, the one pose a spawn bind must not move again
struct recordPosed_t
{
	idEntity*		ent;
	idVec3			fromOrigin;
	idMat3			fromAxis;
	idVec3			toOrigin;
	idMat3			toAxis;
	bool			posed;
};

// A map entity still to bind at spawn (the "bind" key, bound after the record applies) moves with an entity the record
// moved, so the spawn bind finds it at its spawn offset from it: a bound entity records no Pose of its own. One whose
// own MoverState moved it is still in the spawn frame and moves too; a binary mover's pos1 and pos2 go along, or its
// PostBind would make them local off by the master's move. A pass per bind depth: a grandchild moves with the child.
static void RecordCarryBindChildren( const idList<recordPosed_t, TAG_GAME>& moved )
{
	if( moved.Num() == 0 )
	{
		return;
	}
	idList<recordPosed_t, TAG_GAME> carried = moved;
	idList<idEntity*, TAG_GAME> pending;
	for( idEntity* ent = gameLocal.spawnedEntities.Next(); ent != NULL; ent = ent->spawnNode.Next() )
	{
		if( ent->mapSpawnIndex < 0 || ent->GetBindMaster() != NULL || ent->spawnArgs.FindKey( "bind" ) == NULL )
		{
			continue;
		}
		bool posed = false;
		for( int i = 0; i < moved.Num() && !posed; i++ )
		{
			posed = moved[i].ent == ent && moved[i].posed;
		}
		if( !posed )
		{
			pending.Append( ent );
		}
	}
	for( bool progress = true; progress; )
	{
		progress = false;
		for( int i = 0; i < pending.Num(); i++ )
		{
			idEntity* child = pending[i];
			if( child == NULL )
			{
				continue;
			}
			const idEntity* master = gameLocal.FindEntity( child->spawnArgs.GetString( "bind" ) );
			int found = -1;
			for( int j = 0; j < carried.Num() && found < 0; j++ )
			{
				found = carried[j].ent == master ? j : -1;
			}
			if( found < 0 )
			{
				continue;
			}
			const recordPosed_t m = carried[found];
			// the offset the spawn bind takes: a translation for a master that kept its axis or a bind that ignores it
			const bool rotate = m.fromAxis != m.toAxis && child->spawnArgs.GetBool( "bindOrientated", "1" );
			const idMat3 rotation = rotate ? m.fromAxis.Transpose() * m.toAxis : mat3_identity;
			const idVec3 origin = child->GetPhysics()->GetOrigin();
			const idMat3 axis = child->GetPhysics()->GetAxis();
			const idVec3 toOrigin = rotate ? m.toOrigin + ( origin - m.fromOrigin ) * rotation : origin + ( m.toOrigin - m.fromOrigin );
			const idMat3 toAxis = rotate ? axis * rotation : axis;
			child->SetOrigin( toOrigin );
			child->SetAxis( toAxis );
			child->GetPhysics()->PutToRest();
			if( child->IsType( idMover_Binary::Type ) )
			{
				static_cast<idMover_Binary*>( child )->CarryRecordPositions( m.fromOrigin, rotation, m.toOrigin, rotate );
			}
			// a child the record moved already keeps its spawn pose as the from of its entry: its own children move by
			// the whole of it
			int self = -1;
			for( int j = 0; j < carried.Num() && self < 0; j++ )
			{
				self = carried[j].ent == child ? j : -1;
			}
			if( self < 0 )
			{
				recordPosed_t& c = carried.Alloc();
				c.ent = child;
				c.fromOrigin = origin;
				c.fromAxis = axis;
				c.posed = false;
				self = carried.Num() - 1;
			}
			carried[self].toOrigin = toOrigin;
			carried[self].toAxis = toAxis;
			pending[i] = NULL;
			progress = true;
		}
	}
}
//puncho end

void idWorldStore::ApplyCurrentMap()
{
	const idMapFile* map = gameLocal.GetLoadedMap();
	if( !IsActive() || common->IsClient() || map == NULL )
	{
		return;
	}
	const uint64 applyStart = Sys_Microseconds();
	idStr mapName = gameLocal.GetMapName();
	mapName.StripFileExtension();
	mapName.ToLower();
	for( int index = 0; index < records.Num(); index++ )
	{
		const worldMapRecord_t& record = records[index];
		if( record.world != worldName || record.instance != instanceId || record.node != currentNode || record.map != mapName )
		{
			continue;
		}
		//puncho start - P2b.1.2: every entry is framed and checked before any applies; a chunk that cannot be read, or
		// that is not this map's, is dropped whole and logged, an entry that fails its check alone
		const byte* data = record.bytes.Ptr();
		mrecHeader_t header;
		idList<mrecEntry_t, TAG_GAME> entries;
		idStr reason;
		bool readable = RecordScanChunk( data, record.bytes.Num(), nextCreatedId, header, entries, reason );
		if( readable && ( header.node != currentNode || header.cellId != CELL_ID_NONE ) )
		{
			reason = va( "node %u cell %08x is not this map's", header.node, header.cellId );
			readable = false;
		}
		if( !readable )
		{
			gameLocal.Warning( "record: %s dropped: %s", mapName.c_str(), reason.c_str() );
			recordInvalid++;
			records.RemoveIndex( index );
			recordMaps = records.Num();
			return;
		}
		const recordFrame_t frame = RecordChunkFrame( header.cellId );
		//puncho end
		int resetPolicy = 0;
		uint32 resetSeconds = 0;
		if( instanceId == 0 || !mapGraphDirector.GetResetPolicy( mapName, resetPolicy, resetSeconds ) )
		{
			const idDict& world = map->GetEntity( 0 )->epairs;
			const char* policy = world.GetString( "world_reset", "never" );
			resetPolicy = idStr::Icmp( policy, "timed" ) == 0 ? 1 : idStr::Icmp( policy, "cleared" ) == 0 ? 2
						  : 0;
			resetSeconds = Max( 0, world.GetInt( "world_reset_time" ) );
		}
		const uint64 now = gameLocal.GetWorldTime();
		const uint64 resetAt = resetPolicy == 2 ? ( uint64 )header.cleared : ( uint64 )header.lastLeft;
		applyingReset = resetPolicy != 0 && ( resetPolicy != 2 || ( header.flags & 2 ) ) && now >= resetAt && now - resetAt >= ( uint64 )resetSeconds * 1000;
		resetApplied = applyingReset;
		if( applyingReset )
		{
			gameLocal.Printf( "record: reset %s\n", mapName.c_str() );
		}
		remains = applyingReset ? 0 : header.remains;
		idList<idEntity*, TAG_GAME> deleted;
		idList<recordPosed_t, TAG_GAME> moved;
		const bool identical = header.entitiesCRC == map->GetEntitiesCRC() && header.numMapEntities == map->GetNumEntities();
		//puncho start - P2.4 (bug review 2026-10-28): one lookup per record row, not a scan of the map and the rows. A
		// claimed bit per map ordinal, the spawned row by binary search (rows are in ordinal order), and on an edited map
		// each entity's CRC computed once (implementer E's request: GetEntityCRC walks every pair on each call) with the
		// names and CRCs hashed once, added in reverse so a chain yields the lowest ordinal first as the scans did
		const int numMapEntities = map->GetNumEntities();
		idList<byte, TAG_GAME> claimed;
		claimed.SetNum( ( numMapEntities + 7 ) / 8 );
		memset( claimed.Ptr(), 0, claimed.Num() );
		idList<unsigned int, TAG_GAME> entityCRCs;
		idHashIndex entityNames( 4096, numMapEntities ), entityHashes( 4096, numMapEntities );
		if( !identical )
		{
			entityCRCs.SetNum( numMapEntities );
			for( int j = numMapEntities - 1; j >= 1; j-- )
			{
				entityCRCs[j] = map->GetEntityCRC( j );
				const char* entityName = map->GetEntity( j )->epairs.GetString( "name" );
				if( entityName[0] != '\0' )
				{
					entityNames.Add( entityNames.GenerateKey( entityName ), j );
				}
				entityHashes.Add( entityHashes.GenerateKey( ( int )entityCRCs[j] ), j );
			}
			entityCRCs[0] = map->GetEntityCRC( 0 );
		}
		//puncho end
		idRecordIn rec;
		unsigned int count = 0;
		for( int i = 0; i < entries.Num(); i++ )
		{
			const mrecEntry_t& entry = entries[i];
			if( entry.created )
			{
				continue;
			}
			const unsigned short ordinal = entry.ordinalHint;
			const unsigned int crc = entry.entityCRC;
			const idStr& name = entry.name;
			if( !entry.valid )
			{
				gameLocal.Warning( "record: %s '%s'#%d dropped: %s", mapName.c_str(), name.c_str(), ordinal, entry.reason.c_str() );
				recordInvalid++;
				continue;
			}
			int candidate = -1;
			if( identical )
			{
				candidate = ordinal < numMapEntities ? ordinal : -1;
			}
			else if( !name.IsEmpty() )
			{
				for( int j = entityNames.First( entityNames.GenerateKey( name ) ); j != -1; j = entityNames.Next( j ) )
				{
					if( name == map->GetEntity( j )->epairs.GetString( "name" ) )
					{
						candidate = j;
						break;
					}
				}
			}
			else if( ordinal < numMapEntities && entityCRCs[ordinal] == crc )
			{
				candidate = ordinal;
			}
			else
			{
				for( int j = entityHashes.First( entityHashes.GenerateKey( ( int )crc ) ); j != -1; j = entityHashes.Next( j ) )
				{
					if( !( claimed[j >> 3] & ( 1 << ( j & 7 ) ) ) && entityCRCs[j] == crc )
					{
						candidate = j;
						break;
					}
				}
			}
			if( candidate < 0 || ( claimed[candidate >> 3] & ( 1 << ( candidate & 7 ) ) ) || ( identical ? map->GetEntityCRC( candidate ) : entityCRCs[candidate] ) != crc )
			{
				gameLocal.Warning( "record: %s '%s'#%d dropped: entity changed", mapName.c_str(), name.c_str(), ordinal );
				continue;
			}
			claimed[candidate >> 3] |= 1 << ( candidate & 7 );
			const int row = FindSpawnRow( candidate );
			idEntity* ent = row >= 0 ? RecordEntity( mapSpawned[row] ) : NULL;
			if( ent == NULL || ( applyingReset && !ent->spawnArgs.GetBool( "persist_noreset" ) ) )
			{
				continue;
			}
			if( entry.flags & RECORD_ENTRY_REMOVED )
			{
				deleted.Append( ent );
				continue;
			}
			//puncho start - P2b.1.2: the entity's own chain applies its sections; an AI that died in play is replaced by
			// the corpse built from the same entry, and freed after the record has applied
			RecordFrameEntry( data, header.cellId, frame, entry, ent->GetEntityDefName(), rec );
			if( !RecordSectionsFit( ent, rec ) )
			{
				gameLocal.Warning( "record: %s '%s'#%d dropped: its sections are not those of %s", mapName.c_str(), name.c_str(), ordinal, ent->GetClassname() );
				recordInvalid++;
				continue;
			}
			recordPosed_t pose;
			pose.ent = ent;
			pose.fromOrigin = ent->GetPhysics()->GetOrigin();
			pose.fromAxis = ent->GetPhysics()->GetAxis();
			pose.posed = rec.HasField( RECORD_SECTION_ENTITY, idEntity::RF_Pose );
			const recordApply_t result = ent->ApplyRecord( rec, frame );
			if( result == RECORD_REPLACED )
			{
				rec.Rewind();
				idCorpse* corpse = SpawnCorpseRecord( ent->GetEntityDefName(), rec, frame );
				if( corpse == NULL )
				{
					gameLocal.Warning( "record: %s '%s'#%d dropped: no corpse could be built", mapName.c_str(), name.c_str(), ordinal );
					recordInvalid++;
				}
				else
				{
					const int lifetime = ent->spawnArgs.GetInt( "persist_lifetime", "-1" );
					corpse->spawnArgs.SetInt( "persist_lifetime", lifetime );
					const int64 death = corpse->DeathTime();
					if( lifetime == 0 || ( lifetime > 0 && gameLocal.GetWorldTime() - death >= ( int64 )lifetime * 1000 ) )
					{
						delete corpse;
					}
					else
					{
						ReplaceMapEntity( ent, corpse );
					}
				}
				deleted.Append( ent );
				continue;
			}
			if( result != RECORD_APPLIED || !rec.Done() )
			{
				// a chain that refused the entry before any field applied dropped it; one that failed a later field left
				// the earlier ones applied
				gameLocal.Warning( "record: %s '%s'#%d %s", mapName.c_str(), name.c_str(), ordinal, rec.SectionsBegun() == 0 ? "dropped: its fields do not apply" : "partly applied: a field did not apply" );
				recordInvalid++;
			}
			// what the record moved carries the entities that bind to it at spawn
			pose.toOrigin = ent->GetPhysics()->GetOrigin();
			pose.toAxis = ent->GetPhysics()->GetAxis();
			if( pose.toOrigin != pose.fromOrigin || pose.toAxis != pose.fromAxis )
			{
				moved.Append( pose );
			}
			//puncho end
		}
		for( int i = 0; i < entries.Num(); i++ )
		{
			const mrecEntry_t& entry = entries[i];
			if( !entry.created )
			{
				continue;
			}
			count++;
			if( !entry.valid )
			{
				gameLocal.Warning( "record: %s '%s'#%llu dropped: %s", mapName.c_str(), entry.name.c_str(), ( unsigned long long )entry.createdId, entry.reason.c_str() );
				recordInvalid++;
				continue;
			}
			RecordFrameEntry( data, header.cellId, frame, entry, entry.name, rec );
			SpawnCreatedEntry( mapName, entry.name, entry.createdId, entry.createdTime, rec, frame );
		}
		BindCreatedRecords();
		RecordCarryBindChildren( moved );
		for( int i = 0; i < deleted.Num(); i++ )
		{
			delete deleted[i];
		}
		applyingReset = false;
		applyUs = ( unsigned int )( Sys_Microseconds() - applyStart );
		gameLocal.Printf( "record apply map=%s created=%u us=%u\n", mapName.c_str(), count, applyUs );
		if( header.geometryCRC != map->GetGeometryCRC() )
		{
			gameLocal.Warning( "record: %s geometry changed; retained entity poses", mapName.c_str() );
		}
		return;
	}
}

// Bytes of the saved store that belong to no map record: the WSTO header, the dungeon records and the
// ordinal table with its chunk header. The commit budget adds this to the other records so the whole
// store, not just the records, stays under the world budget.
int idWorldStore::StoreFixedBytes() const
{
	int bytes = 22 + 8 + 2 + 10 * mapSpawned.Num();
	for( int i = 0; i < dungeonRecords.Num(); i++ )
	{
		bytes += dungeonRecords[i].Num() + 8;
	}
	return bytes;
}

void idWorldStore::PrepareSave()
{
	// A save can precede the first departure. Capture namespace and current map
	// without suppressing the later leave commit after gameplay resumes.
	committed = false;
	Commit( false );
	committed = false;
}

void idWorldStore::Save( idSaveGame* save )
{
	// PrepareSave committed the current map under the world budget, which bounds the whole store; the store can
	// still exceed MAX_WORLD_BYTES when the dungeon records grew after other maps' records were committed, or when
	// g_recordBytesWorld was lowered. Shed records, largest first and the current map's only when nothing else is
	// left, rather than fail the save: a shed map repopulates from its baseline on the next visit, and every drop
	// is logged.
	idStr currentMap = gameLocal.GetMapName();
	currentMap.StripFileExtension();
	currentMap.ToLower();
	int total = StoreFixedBytes();
	for( int i = 0; i < records.Num(); i++ )
	{
		total += records[i].bytes.Num() + 8;
	}
	while( total > MAX_WORLD_BYTES && records.Num() > 0 )
	{
		int largest = -1;
		for( int pass = 0; pass < 2 && largest < 0; pass++ )
		{
			for( int i = 0; i < records.Num(); i++ )
			{
				const bool current = records[i].world == worldName && records[i].instance == instanceId && records[i].node == currentNode && records[i].map == currentMap;
				if( ( pass == 1 || !current ) && ( largest < 0 || records[i].bytes.Num() > records[largest].bytes.Num() ) )
				{
					largest = i;
				}
			}
		}
		gameLocal.Warning( "world records exceed %d bytes; dropping the %d-byte record of %s", MAX_WORLD_BYTES, records[largest].bytes.Num(), records[largest].map.c_str() );
		total -= records[largest].bytes.Num() + 8;
		records.RemoveIndex( largest );
	}
	recordMaps = records.Num();
	idFile_Memory body;
	body.WriteUnsignedShort( WSTO_VERSION );	//puncho - P2b.1.2: 4, MREC v3
	Record64( body, nextCreatedId );
	body.WriteUnsignedInt( records.Num() + 1 + dungeonRecords.Num() );
	for( int i = 0; i < records.Num(); i++ )
	{
		RecordChunk( body, MREC_TAG, records[i].bytes.Ptr(), records[i].bytes.Num() );
	}
	for( int i = 0; i < dungeonRecords.Num(); i++ )
	{
		RecordChunk( body, DREC_TAG, dungeonRecords[i].Ptr(), dungeonRecords[i].Num() );
	}
	idFile_Memory ordinals;
	ordinals.WriteUnsignedShort( mapSpawned.Num() );
	for( int i = 0; i < mapSpawned.Num(); i++ )
	{
		ordinals.WriteUnsignedShort( mapSpawned[i].ordinal );
		ordinals.WriteInt( mapSpawned[i].entityNumber );
		ordinals.WriteInt( mapSpawned[i].spawnId );
	}
	RecordChunk( body, ORDT_TAG, ordinals.GetDataPtr(), ordinals.Length() );
	idFile_Memory out;
	RecordChunk( out, WSTO_TAG, body.GetDataPtr(), body.Length() );
	if( out.Length() > MAX_WORLD_BYTES )
	{
		// every map record was shed and the two dungeon records are 512 KiB each at most, so this is a
		// programmer error in the byte accounting above, not a content size
		gameLocal.Error( "World records exceed %d bytes", MAX_WORLD_BYTES );
	}
	save->WriteInt( out.Length() );
	save->Write( out.GetDataPtr(), out.Length() );
}

void idWorldStore::Restore( idRestoreGame* save )
{
	Clear();
	int length;
	save->ReadInt( length );
	if( length < 22 || length > MAX_WORLD_BYTES )
	{
		save->Error( "Invalid WSTO size %d", length );
	}
	idList<byte, TAG_GAME> bytes;
	bytes.SetNum( length );
	save->Read( bytes.Ptr(), length );
	idFile_Memory in( "WSTO", ( const char* )bytes.Ptr(), length );
	unsigned int tag, size, chunks;
	unsigned short version;
	if( in.ReadUnsignedInt( tag ) != 4 || tag != WSTO_TAG || in.ReadUnsignedInt( size ) != 4 || size != ( unsigned int )length - 8 || in.ReadUnsignedShort( version ) != 2 || version < 1 )
	{
		save->Error( "Unsupported or malformed WSTO" );
	}
	//puncho start - P2b.1.2: WSTO 4 holds MREC v3; 3 holds MREC v2, each dropped with a line while its DREC and ORDT
	// stay; 1 and 2 are dropped whole; a later version is refused by name
	if( version > WSTO_VERSION )
	{
		save->Error( "WSTO version %u is newer than this build reads (%d)", version, WSTO_VERSION );
	}
	if( version < 3 )
	{
		gameLocal.Warning( "record: the world store of WSTO version %u dropped (this build reads 3 and 4)", version );
		recordInvalid++;
		return;
	}
	//puncho end
	if( !RecordRead64( in, nextCreatedId ) || nextCreatedId == 0 || nextCreatedId == ( uint64 ) - 1 || in.ReadUnsignedInt( chunks ) != 4 || chunks > 4096 )
	{
		save->Error( "Unsupported or malformed WSTO" );
	}
	bool gotOrdinals = false;
	for( unsigned int i = 0; i < chunks; i++ )
	{
		if( in.ReadUnsignedInt( tag ) != 4 || in.ReadUnsignedInt( size ) != 4 || size > ( unsigned int )( in.Length() - in.Tell() ) )
		{
			save->Error( "Truncated WSTO chunk" );
		}
		idFile_Memory chunk( "record", ( const char* )in.GetDataPtr() + in.Tell(), size );
		in.Seek( size, FS_SEEK_CUR );
		if( tag == MREC_TAG )
		{
			//puncho start - P2b.1.2: a chunk the reader cannot frame, of another version or a duplicate identity is
			// dropped and logged; an entry that fails its check is dropped when the map applies
			worldMapRecord_t record;
			mrecHeader_t header;
			idList<mrecEntry_t, TAG_GAME> entries;
			idStr reason;
			if( version == 3 )
			{
				reason = va( "MREC version %u (WSTO 3)", RecordChunkVersion( ( const byte* )chunk.GetDataPtr(), size, record.map ) );
			}
			else if( RecordScanChunk( ( const byte* )chunk.GetDataPtr(), size, nextCreatedId, header, entries, reason ) )
			{
				record.world = header.world;
				record.instance = header.instanceId;
				record.node = header.node;
				record.map = header.unit;
				for( int previous = 0; previous < records.Num() && reason.IsEmpty(); previous++ )
				{
					if( records[previous].world == record.world && records[previous].instance == record.instance && records[previous].node == record.node && records[previous].map == record.map )
					{
						reason = "a second record of the map";
					}
				}
			}
			if( !reason.IsEmpty() )
			{
				gameLocal.Warning( "record: %s dropped: %s", record.map.IsEmpty() ? "(unreadable)" : record.map.c_str(), reason.c_str() );
				recordInvalid++;
				continue;
			}
			//puncho end
			record.bytes.SetNum( size );
			memcpy( record.bytes.Ptr(), chunk.GetDataPtr(), size );
			records.Append( record );
			idStr current = gameLocal.GetMapName();
			current.StripFileExtension();
			current.ToLower();
			if( current == record.map )
			{
				worldName = record.world;
				instanceId = record.instance;
			}
		}
		else if( tag == ORDT_TAG )
		{
			unsigned short count;
			if( gotOrdinals || chunk.ReadUnsignedShort( count ) != 2 || count > MAX_GENTITIES || size != 2U + 10U * count )
			{
				save->Error( "Invalid ORDT" );
			}
			gotOrdinals = true;
			mapSpawned.SetNum( count );
			byte seen[( MAX_GENTITIES + 7 ) / 8] = { 0 };
			const idMapFile* loaded = gameLocal.GetLoadedMap();
			for( int j = 0; j < count; j++ )
			{
				unsigned short ordinal;
				chunk.ReadUnsignedShort( ordinal );
				mapSpawned[j].ordinal = ordinal;
				chunk.ReadInt( mapSpawned[j].entityNumber );
				chunk.ReadInt( mapSpawned[j].spawnId );
				mapSpawned[j].departed = false;
				if( loaded == NULL || ordinal >= loaded->GetNumEntities() || mapSpawned[j].entityNumber < 0 || mapSpawned[j].entityNumber >= MAX_GENTITIES || ( j > 0 && ordinal <= mapSpawned[j - 1].ordinal ) )
				{
					save->Error( "Invalid ORDT identity" );
				}
				const int number = mapSpawned[j].entityNumber;
				if( ( seen[number >> 3] & ( 1 << ( number & 7 ) ) ) || ( mapSpawned[j].spawnId & ( MAX_GENTITIES - 1 ) ) != number || ( ( unsigned int )mapSpawned[j].spawnId >> GENTITYNUM_BITS ) == 0 )
				{
					save->Error( "Duplicate or mismatched ORDT generation" );
				}
				seen[number >> 3] |= 1 << ( number & 7 );
			}
		}
		else if( tag == DREC_TAG )
		{
			idList<byte, TAG_GAME>& record = dungeonRecords.Alloc();
			record.SetNum( size );
			memcpy( record.Ptr(), chunk.GetDataPtr(), size );
		}
		else
		{
			gameLocal.Warning( "Skipping unknown WSTO chunk %08x", tag );
		}
	}
	if( in.Tell() != in.Length() || !gotOrdinals )
	{
		save->Error( "Invalid WSTO chunk count" );
	}
	if( !mapGraphDirector.RestoreRecord() )
	{
		save->Error( "Invalid DREC" );
	}
	if( dungeonRecords.Num() == 0 )
	{
		// A non-world save carries an empty store. Do not turn it into a new
		// world from current cvars while reconstructing the saved game.
		worldName.Clear();
		instanceId = 0;
		currentNode = 0;
		return;
	}
	const dungeonRecipe_t& recipe = mapGraphDirector.RecipeForLoad( gameLocal.GetMapName() );
	worldName = recipe.worldName;
	instanceId = recipe.instanceId;
	currentNode = instanceId != 0 ? recipe.currentNode : 0;
	idStr activeMap = gameLocal.GetMapName();
	activeMap.StripFileExtension();
	activeMap.ToLower();
	for( int i = 0; i < records.Num(); i++ )
	{
		const worldMapRecord_t& record = records[i];
		if( record.world != worldName || record.instance != instanceId || record.node != currentNode || record.map != activeMap )
		{
			continue;
		}
		idFile_Memory metadata( "current MREC metadata", ( const char* )record.bytes.Ptr(), record.bytes.Num() );
		mrecHeader_t header;
		idStr reason;
		if( RecordReadHeader( metadata, header, reason ) )	//puncho - P2b.1.2: framed when restored
		{
			remains = header.remains;
		}
		break;
	}
}

void idWorldStore::RestoreOrdinals()
{
	for( int i = 0; i < mapSpawned.Num(); i++ )
	{
		idEntity* ent = RecordEntity( mapSpawned[i] );
		if( ent != NULL )
		{
			ent->mapSpawnIndex = i;
		}
		else
		{
			const int ordinal = mapSpawned[i].ordinal;
			removed[ordinal >> 3] |= 1 << ( ordinal & 7 );
		}
	}
}

void idWorldStore::ListRecords() const
{
	for( int i = 0; i < records.Num(); i++ )
	{
		gameLocal.Printf( "record %s %s node=%d %d bytes\n", records[i].world.c_str(), records[i].map.c_str(), records[i].node, records[i].bytes.Num() );
	}
}

#if !defined( ID_RETAIL )
//puncho start - P2b.1.0: recordDump, the reference the record-layer commits are held to. Text only, in store, ordinal and
// created-id order, with no pointer, address or wall-clock time, so two runs of one tic sequence write the same bytes
// 64 bytes a line, offset first, so a byte changed in place shows as one changed line
static const int RECORD_DUMP_HEX_BYTES = 64;

static void RecordDumpHex( idFile* out, const char* label, const byte* bytes, int length )
{
	static const char digits[] = "0123456789abcdef";
	char line[RECORD_DUMP_HEX_BYTES * 2 + 1];
	for( int offset = 0; offset < length; offset += RECORD_DUMP_HEX_BYTES )
	{
		const int count = Min( RECORD_DUMP_HEX_BYTES, length - offset );
		for( int i = 0; i < count; i++ )
		{
			line[i * 2] = digits[bytes[offset + i] >> 4];
			line[i * 2 + 1] = digits[bytes[offset + i] & 15];
		}
		line[count * 2] = '\0';
		out->Printf( "%s %06x %s\n", label, offset, line );
	}
}

//puncho start - P2b.1.2: the decoder on MREC v3's sections and field bits. Per known section, and per field of it, the
// entries holding it ( [kind][0] ) and not ( [kind][1] ); kind 0 the authored entries, 1 the created; column
// RECORD_DUMP_FIELDS is the section itself. An authored entry's REMOVED flag is counted as section 0's column
static const int RECORD_DUMP_SECTIONS = RECORD_SECTION_LAST + 1;	// every id in use, with column 0 for the REMOVED flag
static const int RECORD_DUMP_FIELDS = 64;

struct recordDumpTotals_t
{
	int			counts[2][2][RECORD_DUMP_SECTIONS][RECORD_DUMP_FIELDS + 1];
};

static void RecordDumpCounts( idFile* out, const char* prefix, const int counts[2][2][RECORD_DUMP_SECTIONS][RECORD_DUMP_FIELDS + 1] )
{
	out->Printf( "%s flag REMOVED rows set=%d clear=%d\n", prefix, counts[0][0][0][RECORD_DUMP_FIELDS], counts[0][1][0][RECORD_DUMP_FIELDS] );
	for( int id = 1; id < RECORD_DUMP_SECTIONS; id++ )
	{
		const char* section = RecordSectionName( id );
		if( section == NULL )
		{
			continue;
		}
		out->Printf( "%s section %s rows set=%d clear=%d created set=%d clear=%d\n", prefix, section, counts[0][0][id][RECORD_DUMP_FIELDS], counts[0][1][id][RECORD_DUMP_FIELDS], counts[1][0][id][RECORD_DUMP_FIELDS], counts[1][1][id][RECORD_DUMP_FIELDS] );
		for( int bit = 0; bit < RecordSectionFields( id ); bit++ )
		{
			out->Printf( "%s field %s.%s rows set=%d clear=%d created set=%d clear=%d\n", prefix, section, RecordFieldName( id, bit ), counts[0][0][id][bit], counts[0][1][id][bit], counts[1][0][id][bit], counts[1][1][id][bit] );
		}
	}
}

static void RecordDumpAdd( recordDumpTotals_t& totals, const recordDumpTotals_t& counts )
{
	for( int a = 0; a < 2 * 2 * RECORD_DUMP_SECTIONS * ( RECORD_DUMP_FIELDS + 1 ); a++ )
	{
		( &totals.counts[0][0][0][0] )[a] += ( &counts.counts[0][0][0][0] )[a];
	}
}

// one entry: its head, flags and sections with the fields each holds by name, INVALID and the reason when the check
// drops it; counted into counts
static void RecordDumpEntry( idFile* out, const char* prefix, const byte* data, uint32 cellId, const recordFrame_t& frame, const mrecEntry_t& e, recordDumpTotals_t& counts )
{
	const int kind = e.created ? 1 : 0;
	if( kind == 0 )
	{
		out->Printf( "%s entry ordinal=%u crc=%08x name=%s flags=%02x%s%s sections=%u", prefix, e.ordinalHint, e.entityCRC, e.name.c_str(), e.flags, ( e.flags & RECORD_ENTRY_REMOVED ) ? " REMOVED" : "", ( e.flags & RECORD_ENTRY_MIGRATED ) ? " MIGRATED" : "", e.numSections );
		counts.counts[0][( e.flags & RECORD_ENTRY_REMOVED ) ? 0 : 1][0][RECORD_DUMP_FIELDS]++;
	}
	else
	{
		out->Printf( "%s created id=%llu def=%s time=%lld flags=%02x sections=%u", prefix, ( unsigned long long )e.createdId, e.name.c_str(), ( long long )e.createdTime, e.flags, e.numSections );
	}
	out->Printf( "%s%s\n", e.valid ? "" : " INVALID ", e.valid ? "" : e.reason.c_str() );
	idRecordIn rec;
	RecordFrameEntry( data, cellId, frame, e, e.name, rec );
	for( int s = 0; s < rec.NumSections(); s++ )
	{
		const int id = rec.SectionId( s );
		const char* section = RecordSectionName( id );
		out->Printf( "%s  section id=%d %s version=%d mask=%016llx bytes=%d fields=", prefix, id, section != NULL ? section : "unknown", rec.SectionVersion( s ), ( unsigned long long )rec.SectionMask( s ), rec.SectionBytes( s ) );
		const char* separator = "";
		for( int bit = 0; bit < 64; bit++ )
		{
			if( rec.SectionMask( s ) & BIT( bit ) )
			{
				const char* field = RecordFieldName( id, bit );
				out->Printf( "%s%s", separator, field != NULL ? field : va( "bit%d", bit ) );
				separator = "|";
			}
		}
		out->Printf( "\n" );
	}
	for( int id = 1; id < RECORD_DUMP_SECTIONS; id++ )
	{
		counts.counts[kind][rec.HasSection( id ) ? 0 : 1][id][RECORD_DUMP_FIELDS]++;
		for( int bit = 0; bit < RecordSectionFields( id ); bit++ )
		{
			counts.counts[kind][rec.HasField( id, bit ) ? 0 : 1][id][bit]++;
		}
	}
}

// The decoded listing of one MREC v3 payload: the header, each entry with its flags and sections (the fields each
// holds by name, INVALID and the reason when the check would drop it), and the counts, added into the totals. A chunk
// the reader drops whole prints why.
static void RecordDumpMap( idFile* out, int index, const byte* data, int length, uint64 nextIdentity, recordDumpTotals_t& totals )
{
	mrecHeader_t h;
	idList<mrecEntry_t, TAG_GAME> entries;
	idStr reason;
	if( !RecordScanChunk( data, length, nextIdentity, h, entries, reason ) )
	{
		out->Printf( "map %d dropped: %s\n", index, reason.c_str() );
		return;
	}
	out->Printf( "map %d header version=%u flags=%u world=%s unit=%s instance=%llu node=%u cellId=%08x unitRevision=%u entitiesCRC=%08x geometryCRC=%08x entities=%u left=%lld cleared=%lld remains=%u writerBuild=%u entries=%u\n", index, h.version, h.flags, h.world.c_str(), h.unit.c_str(), ( unsigned long long )h.instanceId, h.node, h.cellId, h.unitRevision, h.entitiesCRC, h.geometryCRC, h.numMapEntities, ( long long )h.lastLeft, ( long long )h.cleared, h.remains, h.writerBuild, h.numEntries );
	const recordFrame_t frame = RecordChunkFrame( h.cellId );
	recordDumpTotals_t counts;
	memset( &counts, 0, sizeof( counts ) );
	char prefix[32];
	idStr::snPrintf( prefix, sizeof( prefix ), "map %d", index );
	for( int i = 0; i < entries.Num(); i++ )
	{
		RecordDumpEntry( out, prefix, data, h.cellId, frame, entries[i], counts );
	}
	RecordDumpCounts( out, prefix, counts.counts );
	RecordDumpAdd( totals, counts );
}

void idWorldStore::DumpRecords( idFile* out ) const
{
	out->Printf( "recordDump 2\n" );
	out->Printf( "store world=%s instance=%llu node=%u nextCreatedId=%llu maps=%d dungeon=%d spawned=%d map=%s worldTime=%lld\n", worldName.c_str(), ( unsigned long long )instanceId, currentNode, ( unsigned long long )nextCreatedId, records.Num(), dungeonRecords.Num(), mapSpawned.Num(), gameLocal.GetMapName(), ( long long )gameLocal.GetWorldTime() );
	byte recipe[MAX_MATCH_GAME_DATA];
	const int recipeLength = mapGraphDirector.PeekMatchGameData( gameLocal.GetMapName(), recipe, sizeof( recipe ) );
	out->Printf( "recipe bytes=%d\n", recipeLength );
	RecordDumpHex( out, "recipe", recipe, Max( 0, recipeLength ) );
	char label[32];
	for( int i = 0; i < dungeonRecords.Num(); i++ )
	{
		out->Printf( "drec %d bytes=%d\n", i, dungeonRecords[i].Num() );
		idStr::snPrintf( label, sizeof( label ), "drec %d", i );
		RecordDumpHex( out, label, dungeonRecords[i].Ptr(), dungeonRecords[i].Num() );
	}
	recordDumpTotals_t totals;
	memset( &totals, 0, sizeof( totals ) );
	for( int i = 0; i < records.Num(); i++ )
	{
		const worldMapRecord_t& record = records[i];
		out->Printf( "map %d world=%s map=%s instance=%llu node=%u bytes=%d\n", i, record.world.c_str(), record.map.c_str(), ( unsigned long long )record.instance, record.node, record.bytes.Num() );
		idStr::snPrintf( label, sizeof( label ), "mrec %d", i );
		RecordDumpHex( out, label, record.bytes.Ptr(), record.bytes.Num() );
		RecordDumpMap( out, i, record.bytes.Ptr(), record.bytes.Num(), nextCreatedId, totals );
	}
	// the last transition's carried entries, in the carry's frame, when they are still held (Motion lives only here)
	const recordFrame_t carryFrame = { CELL_ID_NONE, vec3_origin, mat3_identity, RECORD_FRAME_CARRY };
	recordDumpTotals_t carried;
	memset( &carried, 0, sizeof( carried ) );
	const byte* carry[MAX_TRANSITION_CARRY_RECORDS];
	int carryLengths[MAX_TRANSITION_CARRY_RECORDS];
	const int numCarried = TransitionCarryRecords( carry, carryLengths, MAX_TRANSITION_CARRY_RECORDS );
	for( int i = 0; i < numCarried; i++ )
	{
		out->Printf( "carry %d bytes=%d\n", i, carryLengths[i] );
		idStr::snPrintf( label, sizeof( label ), "carry %d", i );
		RecordDumpHex( out, label, carry[i], carryLengths[i] );
		idFile_Memory in( "carry", ( const char* )carry[i], carryLengths[i] );
		mrecEntry_t entry;
		idRecordIn rec;
		rec.Begin( carryFrame, CELL_ID_NONE, true, "" );
		if( !RecordReadCreatedEntry( in, entry, rec ) )
		{
			out->Printf( "%s cannot be framed\n", label );
			continue;
		}
		entry.valid = RecordCheckEntry( rec, entry.flags, entry.reason );
		RecordDumpEntry( out, label, carry[i], CELL_ID_NONE, carryFrame, entry, carried );
	}
	if( numCarried > 0 )
	{
		RecordDumpCounts( out, "carry", carried.counts );
		RecordDumpAdd( totals, carried );
	}
	RecordDumpCounts( out, "total", totals.counts );
	out->Printf( "total dropped=%d\n", recordInvalid );
}
//puncho end
#endif

#if !defined( ID_RETAIL )	//puncho - dev guard (P2b.1.1a): recordTest's codec self-test is test scaffolding
void idWorldStore::TestCodec()
{
	PrepareSave();
	int failures = 0, cases = 0;
	mrecHeader_t header;
	idList<mrecEntry_t, TAG_GAME> entries;
	idStr reason;
	for( int i = 0; i < records.Num(); i++ )
	{
		const idList<byte, TAG_GAME>& bytes = records[i].bytes;
		//puncho - P2b.1.2: a committed record frames whole and every entry passes its check
		bool good = RecordScanChunk( bytes.Ptr(), bytes.Num(), nextCreatedId, header, entries, reason );
		for( int j = 0; j < entries.Num(); j++ )
		{
			good &= entries[j].valid;
		}
		failures += good ? 0 : 1;
		cases++;
		// Exhaust all small records and sample bounded cuts of large ones.
		const int stride = Max( 1, bytes.Num() / 512 );
		for( int length = 0; length < bytes.Num(); length += stride )
		{
			failures += RecordScanChunk( bytes.Ptr(), length, nextCreatedId, header, entries, reason ) ? 1 : 0;
			cases++;
		}
		idList<byte, TAG_GAME> corrupt = bytes;
		corrupt[0] = 255;
		failures += RecordScanChunk( corrupt.Ptr(), corrupt.Num(), nextCreatedId, header, entries, reason ) ? 1 : 0;
		cases++;
	}
	//puncho start - P2b.1.2: the entry check on two hand-made created entries, a Pose holding an infinity and an
	// unknown section id; both are dropped
	static const recordFrame_t frame = { CELL_ID_NONE, vec3_origin, mat3_identity, RECORD_FRAME_MAP };
	byte badPose[RECORD_SECTION_HEADER_BYTES + 28] = { RECORD_SECTION_ENTITY, RECORD_VERSION_ENTITY, 1 << idEntity::RF_Pose, 0, 0, 0, 0, 0, 0, 0, 28, 0 };
	badPose[RECORD_SECTION_HEADER_BYTES + 6] = 0x80;
	badPose[RECORD_SECTION_HEADER_BYTES + 7] = 0x7f;
	const byte unknown[RECORD_SECTION_HEADER_BYTES] = { 63, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	const byte* const cases2[2] = { badPose, unknown };
	const int lengths[2] = { sizeof( badPose ), sizeof( unknown ) };
	for( int i = 0; i < 2; i++ )
	{
		idRecordIn rec;
		int consumed;
		rec.Begin( frame, CELL_ID_NONE, true, "" );
		failures += rec.ReadSections( cases2[i], lengths[i], 1, consumed ) && RecordCheckEntry( rec, 0, reason ) ? 1 : 0;
		cases++;
	}
	//puncho end
	gameLocal.Printf( "RECORD_CODEC %s records=%d cases=%d failures=%d\n", failures == 0 && records.Num() > 0 ? "PASS" : "FAIL", records.Num(), cases, failures );
}
#endif

void idWorldStore::DrawDebug() const
{
	if( !g_showRecords.GetBool() && !g_showLandmarks.GetBool() )
	{
		return;
	}
	idPlayer* player = gameLocal.GetLocalPlayer();
	if( player == NULL )
	{
		return;
	}
	const idMat3 view = player->viewAngles.ToMat3();
	if( g_showRecords.GetBool() )
	{
		for( idEntity* ent = gameLocal.spawnedEntities.Next(); ent != NULL; ent = ent->spawnNode.Next() )
		{
			if( ent->mapSpawnIndex < 0 && ent->spawnArgs.GetString( "_worldCreatedId", "0" )[0] == '0' )
			{
				continue;
			}
			gameRenderWorld->DebugBounds( ent->IsType( idCorpse::Type ) ? colorRed : colorGreen, ent->GetPhysics()->GetAbsBounds() );
			gameRenderWorld->DrawText( va( "ord %d created %s", ent->mapSpawnIndex, ent->spawnArgs.GetString( "_worldCreatedId", "0" ) ), ent->GetPhysics()->GetOrigin(), 0.15f, colorWhite, view );
		}
	}
	if( g_showLandmarks.GetBool() )
	{
		const idMapLandmarks& tables = gameLocal.mapLandmarks;
		for( int i = 0; i < tables.NumLandmarks(); i++ )
		{
			const landmark_t& lm = tables.GetLandmark( i );
			gameRenderWorld->DebugArrow( colorCyan, lm.origin, lm.origin + lm.axis[0] * 64.0f, 8 );
			gameRenderWorld->DrawText( lm.id, lm.origin, 0.2f, colorCyan, view );
		}
		for( int i = 0; i < tables.NumTransitionVolumes(); i++ )
		{
			gameRenderWorld->DebugBounds( colorYellow, tables.GetTransitionVolume( i ).bounds );
		}
	}
}

void idWorldStore::ReplaceMapEntity( idEntity* oldEntity, idEntity* replacement )
{
	const int index = oldEntity->mapSpawnIndex;
	if( index >= 0 && index < mapSpawned.Num() )
	{
		EntityRemoved( oldEntity );
		mapSpawned[index].entityNumber = replacement->entityNumber;
		mapSpawned[index].spawnId = gameLocal.GetSpawnId( replacement );
		replacement->mapSpawnIndex = index;
		oldEntity->mapSpawnIndex = -1;
	}
}

uint64 idWorldStore::AllocateInstanceId( uint64 seed )
{
	if( nextCreatedId > 0xffffffffULL )
	{
		gameLocal.Error( "World instance identity counter exhausted" );
	}
	return ( nextCreatedId++ << 32 ) | ( seed & 0xffffffffULL );
}
