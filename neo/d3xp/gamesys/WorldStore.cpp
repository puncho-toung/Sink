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

static bool ValidateMapRecord( const char* data, int length, uint64 nextIdentity = ( uint64 ) - 1 )
{
	idFile_Memory in( "MREC validation", data, length );
	unsigned short version, flags, entities, remains, node = 0;
	unsigned int crc, count;
	uint64 instance, left, cleared;
	idStr name;
	if( in.ReadUnsignedShort( version ) != 2 || ( version < 1 || version > 2 ) || in.ReadUnsignedShort( flags ) != 2 || flags > 3 || !RecordReadString( in, name ) || name.IsEmpty() || !RecordRead64( in, instance ) || ( version >= 2 && in.ReadUnsignedShort( node ) != 2 ) || node >= 256 || ( instance == 0 && node != 0 ) || !RecordReadString( in, name ) || name.IsEmpty() || in.ReadUnsignedInt( crc ) != 4 || in.ReadUnsignedInt( crc ) != 4 || in.ReadUnsignedShort( entities ) != 2 || entities == 0 || !RecordRead64( in, left ) || left > 0x7fffffffffffffffULL || !RecordRead64( in, cleared ) || in.ReadUnsignedShort( remains ) != 2 || in.ReadUnsignedInt( count ) != 4 || count > MAX_GENTITIES )
	{
		return false;
	}
	int previous = -1;
	for( unsigned int i = 0; i < count; i++ )
	{
		unsigned short ordinal, aspects, size;
		if( in.ReadUnsignedShort( ordinal ) != 2 || ordinal >= entities || ordinal <= previous || in.ReadUnsignedInt( crc ) != 4 || !RecordReadString( in, name ) || in.ReadUnsignedShort( aspects ) != 2 || ( aspects & RECORD_BIND ) || in.ReadUnsignedShort( size ) != 2 || size > in.Length() - in.Tell() || !ValidateAspectBytes( aspects, in.GetDataPtr() + in.Tell(), size ) )
		{
			return false;
		}
		previous = ordinal;
		in.Seek( size, FS_SEEK_CUR );
	}
	if( in.ReadUnsignedInt( count ) != 4 || count > MAX_GENTITIES )
	{
		return false;
	}
	uint64 previousId = 0;
	for( unsigned int i = 0; i < count; i++ )
	{
		uint64 id, when;
		unsigned short aspects, size;
		if( !RecordRead64( in, id ) || id <= previousId || id >= nextIdentity || !RecordReadString( in, name ) || name.IsEmpty() || !RecordRead64( in, when ) || when > 0x7fffffffffffffffULL || in.ReadUnsignedShort( aspects ) != 2 || !( aspects & RECORD_MOVED ) || in.ReadUnsignedShort( size ) != 2 || size > in.Length() - in.Tell() || !ValidateAspectBytes( aspects, in.GetDataPtr() + in.Tell(), size ) )
		{
			return false;
		}
		previousId = id;
		in.Seek( size, FS_SEEK_CUR );
	}
	return in.Tell() == in.Length();
}

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
	unsigned short aspects;
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
	int authoredBytes = 52 + worldName.Length() + recordMap.Length();
	// Each row's aspects are written once, here; the record below copies them. An entity that matches its spawn
	// state writes no row at all.
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
		const char* rowName = map->GetEntity( row.ordinal )->epairs.GetString( "name" );
		if( idStr::Length( rowName ) > 63 )
		{
			gameLocal.Warning( "record: %s entity '%s' not recorded: name exceeds 63 bytes", recordMap.c_str(), rowName );
			out.aspects = 0;
			out.length = 0;
			continue;
		}
		out.aspects = ent == NULL ? RECORD_REMOVED : WriteAspects( ent, rowPayloads, vec3_origin, false, 0 );
		out.length = rowPayloads.Length() - out.offset;
		if( out.aspects != 0 )
		{
			authoredBytes += 11 + idStr::Length( rowName ) + out.length;
		}
	}
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
		if( out.aspects == 0 )
		{
			continue; // not recorded at all (an overlong name), so leaving it out saves nothing
		}
		authoredBytes -= out.length;
		out.aspects = RECORD_REMOVED;
		out.length = 0;
		evicted++;
		RecordExclude( excluded, ent );
		bodies[i].ent = NULL;
	}
	int rowCount = 0;
	for( int i = 0; i < rows.Num(); i++ )
	{
		rowCount += rows[i].aspects != 0 ? 1 : 0;
	}
	idFile_Memory record;
	record.WriteUnsignedShort( 2 );
	record.WriteUnsignedShort( isCleared ? 3 : 1 );
	RecordString( record, worldName );
	Record64( record, instanceId );
	record.WriteUnsignedShort( currentNode );
	idStr name = gameLocal.GetMapName();
	name.StripFileExtension();
	name.ToLower();
	RecordString( record, name );
	record.WriteUnsignedInt( map->GetEntitiesCRC() );
	record.WriteUnsignedInt( map->GetGeometryCRC() );
	record.WriteUnsignedShort( map->GetNumEntities() );
	Record64( record, gameLocal.GetWorldTime() );
	uint64 clearedTime = isCleared ? ( uint64 )gameLocal.GetWorldTime() : ( uint64 ) - 1;
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
			unsigned short version, flags, number;
			unsigned int crc;
			uint64 identity, left, cleared;
			idStr ignored;
			old.ReadUnsignedShort( version );
			old.ReadUnsignedShort( flags );
			RecordReadString( old, ignored );
			RecordRead64( old, identity );
			if( version >= 2 )
			{
				old.ReadUnsignedShort( number );
			}
			RecordReadString( old, ignored );
			old.ReadUnsignedInt( crc );
			old.ReadUnsignedInt( crc );
			old.ReadUnsignedShort( number );
			RecordRead64( old, left );
			if( RecordRead64( old, cleared ) && ( flags & 2 ) && cleared != ( uint64 ) - 1 )
			{
				clearedTime = cleared;
			}
			break;
		}
	}
	Record64( record, clearedTime );
	record.WriteUnsignedShort( remains );
	record.WriteUnsignedInt( rowCount );
	for( int i = 0; i < mapSpawned.Num(); i++ )
	{
		const mapSpawnRow_t& row = mapSpawned[i];
		const recordRow_t& out = rows[i];
		if( out.aspects == 0 )
		{
			continue;
		}
		record.WriteUnsignedShort( row.ordinal );
		record.WriteUnsignedInt( map->GetEntityCRC( row.ordinal ) );
		RecordString( record, map->GetEntity( row.ordinal )->epairs.GetString( "name" ) );
		record.WriteUnsignedShort( out.aspects );
		record.WriteUnsignedShort( out.length );
		record.Write( rowPayloads.GetDataPtr() + out.offset, out.length );
	}
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
		if( !WriteCreated( ent, vec3_origin, encoded ) )
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
		idFile_Memory in( "MREC", ( const char* )record.bytes.Ptr(), record.bytes.Num() );
		unsigned short version, flags, numEntities, savedRemains, node = 0;
		unsigned int entitiesCRC, geometryCRC, count;
		uint64 instance, left, cleared;
		idStr world, name;
		if( in.ReadUnsignedShort( version ) != 2 || ( version < 1 || version > 2 ) || in.ReadUnsignedShort( flags ) != 2 || !RecordReadString( in, world ) || !RecordRead64( in, instance ) || ( version >= 2 && in.ReadUnsignedShort( node ) != 2 ) || node != currentNode || !RecordReadString( in, name ) || in.ReadUnsignedInt( entitiesCRC ) != 4 || in.ReadUnsignedInt( geometryCRC ) != 4 || in.ReadUnsignedShort( numEntities ) != 2 || !RecordRead64( in, left ) || !RecordRead64( in, cleared ) || in.ReadUnsignedShort( savedRemains ) != 2 || in.ReadUnsignedInt( count ) != 4 || count > MAX_GENTITIES )
		{
			gameLocal.Error( "Malformed MREC header for %s", mapName.c_str() );
		}
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
		const uint64 resetAt = resetPolicy == 2 ? cleared : left;
		applyingReset = resetPolicy != 0 && ( resetPolicy != 2 || ( flags & 2 ) ) && now >= resetAt && now - resetAt >= ( uint64 )resetSeconds * 1000;
		resetApplied = applyingReset;
		if( applyingReset )
		{
			gameLocal.Printf( "record: reset %s\n", mapName.c_str() );
		}
		remains = applyingReset ? 0 : savedRemains;
		idList<idEntity*, TAG_GAME> deleted;
		const bool identical = entitiesCRC == map->GetEntitiesCRC() && numEntities == map->GetNumEntities();
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
		for( unsigned int i = 0; i < count; i++ )
		{
			unsigned short ordinal, aspects, length;
			unsigned int crc;
			if( in.ReadUnsignedShort( ordinal ) != 2 || in.ReadUnsignedInt( crc ) != 4 || !RecordReadString( in, name ) || in.ReadUnsignedShort( aspects ) != 2 || in.ReadUnsignedShort( length ) != 2 || length > in.Length() - in.Tell() )
			{
				gameLocal.Error( "Malformed MREC entry for %s", mapName.c_str() );
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
			idFile_Memory payload( "aspects", ( const char* )in.GetDataPtr() + in.Tell(), length );
			in.Seek( length, FS_SEEK_CUR );
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
			if( aspects & RECORD_REMOVED )
			{
				deleted.Append( ent );
			}
			else if( aspects & RECORD_CORPSE )
			{
				idEntity* corpse = SpawnCorpseRecord( ent->GetEntityDefName(), aspects, payload );
				if( corpse != NULL )
				{
					const int lifetime = ent->spawnArgs.GetInt( "persist_lifetime", "-1" );
					corpse->spawnArgs.SetInt( "persist_lifetime", lifetime );
					const int64 death = static_cast<idCorpse*>( corpse )->DeathTime();
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
			}
			else if( !ApplyAspects( ent, aspects, payload, mat3_identity, vec3_origin ) )
			{
				gameLocal.Warning( "record: unsupported aspects for %s", name.c_str() );
			}
		}
		if( in.ReadUnsignedInt( count ) != 4 || count > MAX_GENTITIES )
		{
			gameLocal.Error( "Malformed created count" );
		}
		for( unsigned int i = 0; i < count; i++ )
		{
			SpawnCreated( in, mat3_identity, vec3_origin );
		}
		BindCreatedRecords();
		for( int i = 0; i < deleted.Num(); i++ )
		{
			delete deleted[i];
		}
		applyingReset = false;
		applyUs = ( unsigned int )( Sys_Microseconds() - applyStart );
		gameLocal.Printf( "record apply map=%s created=%u us=%u\n", mapName.c_str(), count, applyUs );
		if( geometryCRC != map->GetGeometryCRC() )
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
	body.WriteUnsignedShort( 3 );
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
	if( in.ReadUnsignedInt( tag ) != 4 || tag != WSTO_TAG || in.ReadUnsignedInt( size ) != 4 || size != ( unsigned int )length - 8 || in.ReadUnsignedShort( version ) != 2 || version > 3 || version < 1 || !RecordRead64( in, nextCreatedId ) || nextCreatedId == 0 || nextCreatedId == ( uint64 ) - 1 || in.ReadUnsignedInt( chunks ) != 4 || chunks > 4096 )
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
			if( !ValidateMapRecord( chunk.GetDataPtr(), size, nextCreatedId ) )
			{
				save->Error( "Malformed MREC payload" );
			}
			worldMapRecord_t record;
			record.node = 0;
			unsigned short recordVersion, flags;
			if( chunk.ReadUnsignedShort( recordVersion ) != 2 || ( recordVersion < 1 || recordVersion > 2 ) || chunk.ReadUnsignedShort( flags ) != 2 || !RecordReadString( chunk, record.world ) || !RecordRead64( chunk, record.instance ) || ( recordVersion >= 2 && chunk.ReadUnsignedShort( record.node ) != 2 ) || !RecordReadString( chunk, record.map ) )
			{
				save->Error( "Invalid MREC identity" );
			}
			record.bytes.SetNum( size );
			memcpy( record.bytes.Ptr(), chunk.GetDataPtr(), size );
			for( int previous = 0; previous < records.Num(); previous++ )
			{
				if( records[previous].world == record.world && records[previous].instance == record.instance && records[previous].node == record.node && records[previous].map == record.map )
				{
					save->Error( "Duplicate MREC identity" );
				}
			}
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
		unsigned short version, flags, number;
		unsigned int crc;
		uint64 value;
		idStr ignored;
		metadata.ReadUnsignedShort( version );
		metadata.ReadUnsignedShort( flags );
		RecordReadString( metadata, ignored );
		RecordRead64( metadata, value );
		if( version >= 2 )
		{
			metadata.ReadUnsignedShort( number );
		}
		RecordReadString( metadata, ignored );
		metadata.ReadUnsignedInt( crc );
		metadata.ReadUnsignedInt( crc );
		metadata.ReadUnsignedShort( number );
		RecordRead64( metadata, value );
		RecordRead64( metadata, value );
		metadata.ReadUnsignedShort( remains );
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
static const int RECORD_DUMP_ASPECTS = 10;
static const char* const recordDumpAspectNames[RECORD_DUMP_ASPECTS] = { "REMOVED", "MOVED", "MOVER", "HEALTH", "BROKEN", "HIDDEN", "CORPSE", "MOTION", "BIND", "SHOWN" };
compile_time_assert( RECORD_SHOWN == BIT( RECORD_DUMP_ASPECTS - 1 ) );

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

static void RecordDumpAspects( idFile* out, unsigned short aspects )
{
	out->Printf( "aspects=%04x", aspects );
	const char* separator = " ";
	for( int i = 0; i < RECORD_DUMP_ASPECTS; i++ )
	{
		if( aspects & BIT( i ) )
		{
			out->Printf( "%s%s", separator, recordDumpAspectNames[i] );
			separator = "|";
		}
	}
}

// per aspect, the entries with it set ( [kind][0] ) and clear ( [kind][1] ); kind 0 the authored rows, 1 the created
struct recordDumpTotals_t
{
	int			counts[2][2][RECORD_DUMP_ASPECTS];
};

// The decoded listing of one MREC v1 or v2 payload: the header, each authored row and created entry (INVALID after one
// whose payload restore would refuse), and per aspect the number of entries with it set and clear (authored and
// created apart), added into the totals. The record layer's commits replace this function and RecordDumpTotals when
// the payload becomes sections and field bits.
static void RecordDumpMap( idFile* out, int index, const byte* data, int length, recordDumpTotals_t& totals )
{
	idFile_Memory in( "recordDump", ( const char* )data, length );
	unsigned short version, flags, entities, remains, node = 0;
	unsigned int entitiesCRC, geometryCRC, count;
	uint64 instance, left, cleared;
	idStr world, map;
	// one MREC v1 or v2 payload, read as ApplyCurrentMap and ValidateMapRecord read it
	if( in.ReadUnsignedShort( version ) != 2 || version < 1 || version > 2 || in.ReadUnsignedShort( flags ) != 2 || !RecordReadString( in, world ) || !RecordRead64( in, instance ) || ( version >= 2 && in.ReadUnsignedShort( node ) != 2 ) || !RecordReadString( in, map ) || in.ReadUnsignedInt( entitiesCRC ) != 4 || in.ReadUnsignedInt( geometryCRC ) != 4 || in.ReadUnsignedShort( entities ) != 2 || !RecordRead64( in, left ) || !RecordRead64( in, cleared ) || in.ReadUnsignedShort( remains ) != 2 || in.ReadUnsignedInt( count ) != 4 )
	{
		out->Printf( "map %d malformed header\n", index );
		return;
	}
	out->Printf( "map %d header version=%u flags=%u world=%s map=%s instance=%llu node=%u entitiesCRC=%08x geometryCRC=%08x entities=%u left=%lld cleared=%lld remains=%u rows=%u\n", index, version, flags, world.c_str(), map.c_str(), ( unsigned long long )instance, node, entitiesCRC, geometryCRC, entities, ( long long )left, ( long long )cleared, remains, count );
	int counts[2][2][RECORD_DUMP_ASPECTS];
	memset( counts, 0, sizeof( counts ) );
	for( int kind = 0; kind < 2; kind++ )
	{
		if( kind == 1 && in.ReadUnsignedInt( count ) != 4 )
		{
			out->Printf( "map %d malformed created count\n", index );
			return;
		}
		for( unsigned int i = 0; i < count; i++ )
		{
			unsigned short ordinal = 0, aspects, size;
			unsigned int crc = 0;
			uint64 id = 0, when = 0;
			idStr name;
			const bool good = kind == 0 ?
							  ( in.ReadUnsignedShort( ordinal ) == 2 && in.ReadUnsignedInt( crc ) == 4 && RecordReadString( in, name ) ) :
							  ( RecordRead64( in, id ) && RecordReadString( in, name ) && RecordRead64( in, when ) );
			if( !good || in.ReadUnsignedShort( aspects ) != 2 || in.ReadUnsignedShort( size ) != 2 || size > in.Length() - in.Tell() )
			{
				out->Printf( "map %d malformed %s %u\n", index, kind == 0 ? "row" : "created", i );
				return;
			}
			if( kind == 0 )
			{
				out->Printf( "map %d row ordinal=%u crc=%08x name=%s ", index, ordinal, crc, name.c_str() );
			}
			else
			{
				out->Printf( "map %d created id=%llu def=%s time=%llu ", index, ( unsigned long long )id, name.c_str(), ( unsigned long long )when );
			}
			// as Restore validates: no BIND on an authored row, MOVED in every created entry, the payload's own bytes
			const bool valid = ( kind == 0 ? !( aspects & RECORD_BIND ) : ( aspects & RECORD_MOVED ) != 0 ) && ValidateAspectBytes( aspects, in.GetDataPtr() + in.Tell(), size );
			RecordDumpAspects( out, aspects );
			out->Printf( " size=%u%s\n", size, valid ? "" : " INVALID" );
			for( int a = 0; a < RECORD_DUMP_ASPECTS; a++ )
			{
				counts[kind][( aspects & BIT( a ) ) ? 0 : 1][a]++;
			}
			in.Seek( size, FS_SEEK_CUR );
		}
	}
	for( int a = 0; a < RECORD_DUMP_ASPECTS; a++ )
	{
		out->Printf( "map %d aspect %s rows set=%d clear=%d created set=%d clear=%d\n", index, recordDumpAspectNames[a], counts[0][0][a], counts[0][1][a], counts[1][0][a], counts[1][1][a] );
		for( int kind = 0; kind < 2; kind++ )
		{
			totals.counts[kind][0][a] += counts[kind][0][a];
			totals.counts[kind][1][a] += counts[kind][1][a];
		}
	}
	if( in.Tell() != in.Length() )
	{
		out->Printf( "map %d trailing %d bytes\n", index, in.Length() - in.Tell() );
	}
}

static void RecordDumpTotals( idFile* out, const recordDumpTotals_t& totals )
{
	for( int a = 0; a < RECORD_DUMP_ASPECTS; a++ )
	{
		out->Printf( "total aspect %s rows set=%d clear=%d created set=%d clear=%d\n", recordDumpAspectNames[a], totals.counts[0][0][a], totals.counts[0][1][a], totals.counts[1][0][a], totals.counts[1][1][a] );
	}
}

void idWorldStore::DumpRecords( idFile* out ) const
{
	out->Printf( "recordDump 1\n" );
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
		RecordDumpMap( out, i, record.bytes.Ptr(), record.bytes.Num(), totals );
	}
	RecordDumpTotals( out, totals );
}
//puncho end
#endif

#if !defined( ID_RETAIL )	//puncho - dev guard (P2b.1.1a): recordTest's codec self-test is test scaffolding
void idWorldStore::TestCodec()
{
	PrepareSave();
	int failures = 0, cases = 0;
	for( int i = 0; i < records.Num(); i++ )
	{
		const idList<byte, TAG_GAME>& bytes = records[i].bytes;
		failures += ValidateMapRecord( ( const char* )bytes.Ptr(), bytes.Num(), nextCreatedId ) ? 0 : 1;
		cases++;
		// Exhaust all small records and sample bounded cuts of large ones.
		const int stride = Max( 1, bytes.Num() / 512 );
		for( int length = 0; length < bytes.Num(); length += stride )
		{
			failures += ValidateMapRecord( ( const char* )bytes.Ptr(), length, nextCreatedId ) ? 1 : 0;
			cases++;
		}
		idList<byte, TAG_GAME> corrupt = bytes;
		corrupt[0] = 255;
		failures += ValidateMapRecord( ( const char* )corrupt.Ptr(), corrupt.Num(), nextCreatedId ) ? 1 : 0;
		cases++;
	}
	const char badFloat[24] = { 0, 0, ( char )0x80, 0x7f };
	failures += ValidateAspectBytes( RECORD_MOTION, badFloat, sizeof( badFloat ) ) ? 1 : 0;
	failures += ValidateAspectBytes( 0x8000, "", 0 ) ? 1 : 0;
	cases += 2;
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
