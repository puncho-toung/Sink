#include "../../idlib/precompiled.h"
#pragma hdrstop
#include "../Game_local.h"
#include "WorldStore.h"

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

static void RecordString( idFile& f, const char* value )
{
	const int length = idStr::Length( value );
	if( length > 63 )
	{
		gameLocal.Error( "world record name exceeds 63 bytes: %s", value );
	}
	f.WriteUnsignedChar( length );
	f.Write( value, length );
}

static bool RecordReadString( idFile& f, idStr& value )
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

static void Record64( idFile& f, uint64 value )
{
	f.WriteUnsignedInt( ( unsigned int )value );
	f.WriteUnsignedInt( ( unsigned int )( value >> 32 ) );
}

static bool RecordRead64( idFile& f, uint64& value )
{
	unsigned int low, high;
	if( f.ReadUnsignedInt( low ) != 4 || f.ReadUnsignedInt( high ) != 4 )
	{
		return false;
	}
	value = ( uint64 )low | ( ( uint64 )high << 32 );
	return true;
}

static bool ValidateAspectBytes( unsigned short aspects, const char* data, int length )
{
	if( aspects & ~( RECORD_REMOVED | RECORD_MOVED | RECORD_MOVER | RECORD_HEALTH | RECORD_BROKEN | RECORD_HIDDEN | RECORD_CORPSE | RECORD_MOTION | RECORD_BIND ) )
	{
		return false;
	}
	if( aspects & RECORD_REMOVED )
	{
		return aspects == RECORD_REMOVED && length == 0;
	}
	idFile_Memory in( "record validation", data, length );
	if( aspects & RECORD_MOVED )
	{
		for( int i = 0; i < 6; i++ )
		{
			unsigned int bits;
			if( in.ReadUnsignedInt( bits ) != 4 || ( bits & 0x7f800000U ) == 0x7f800000U )
			{
				return false;
			}
		}
	}
	if( aspects & RECORD_MOVER )
	{
		byte position, flags;
		if( in.ReadUnsignedChar( position ) != 1 || position > 1 || in.ReadUnsignedChar( flags ) != 1 || flags > 1 )
		{
			return false;
		}
	}
	if( aspects & RECORD_HEALTH )
	{
		int health;
		if( in.ReadInt( health ) != 4 )
		{
			return false;
		}
	}
	if( aspects & RECORD_CORPSE )
	{
		if( aspects != ( RECORD_MOVED | RECORD_CORPSE ) )
		{
			return false;
		}
		for( int i = 0; i < 4; i++ )
		{
			unsigned int bits;
			if( in.ReadUnsignedInt( bits ) != 4 || ( bits & 0x7f800000U ) == 0x7f800000U )
			{
				return false;
			}
		}
		byte flags, kind;
		uint64 death;
		if( in.ReadUnsignedChar( flags ) != 1 || flags > 7 || in.ReadUnsignedChar( kind ) != 1 || kind > 1 || !RecordRead64( in, death ) || death > 0x7fffffffffffffffULL )
		{
			return false;
		}
		if( kind == 0 )
		{
			idStr anim;
			unsigned short frame;
			if( !RecordReadString( in, anim ) || in.ReadUnsignedShort( frame ) != 2 || frame == 0 )
			{
				return false;
			}
		}
		else
		{
			byte count;
			if( in.ReadUnsignedChar( count ) != 1 || count == 0 || count > 20 )
			{
				return false;
			}
			for( int i = 0; i < count; i++ )
			{
				byte index;
				short value;
				if( in.ReadUnsignedChar( index ) != 1 || index != i )
				{
					return false;
				}
				for( int j = 0; j < 6; j++ )
				{
					if( in.ReadShort( value ) != 2 )
					{
						return false;
					}
				}
			}
		}
	}
	if( aspects & RECORD_MOTION )
	{
		for( int i = 0; i < 6; i++ )
		{
			unsigned int bits;
			if( in.ReadUnsignedInt( bits ) != 4 || ( bits & 0x7f800000U ) == 0x7f800000U )
			{
				return false;
			}
		}
	}
	if( aspects & RECORD_BIND )
	{
		uint64 parent;
		short joint, body;
		byte orientated;
		if( !RecordRead64( in, parent ) || parent == 0 || in.ReadShort( joint ) != 2 || joint < -1 || in.ReadShort( body ) != 2 || body < -1 || ( joint >= 0 && body >= 0 ) || in.ReadUnsignedChar( orientated ) != 1 || orientated > 1 )
		{
			return false;
		}
	}
	return in.Tell() == in.Length();
}

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
	if( IsActive() && gameLocal.mapFile != NULL && gameLocal.mapFile->GetNumEntities() > 65535 )
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

void idWorldStore::EntityRemoved( idEntity* ent )
{
	const int index = ent->mapSpawnIndex;
	if( IsActive() && index >= 0 && index < mapSpawned.Num() )
	{
		removed[index >> 3] |= 1 << ( index & 7 );
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

static unsigned short WriteAspects( idEntity* ent, idFile_Memory& payload, const idVec3& landmark, bool motion )
{
	if( ent->IsType( idAI::Type ) && ent->health <= 0 )
	{
		idAI* actor = static_cast<idAI*>( ent );
		if( !g_corpses.GetBool() || actor->IsGibbed() || actor->spawnArgs.GetInt( "persist_lifetime", "-1" ) == 0 )
		{
			return RECORD_REMOVED;
		}
		// A departure can precede the script's settled-remove event. Capture
		// the current pose directly; never materialize an AI with health <= 0.
		idFile_Memory pose;
		if( !idCorpse::WriteActorPose( actor, pose ) )
		{
			return RECORD_REMOVED;
		}
		const idVec3 origin = actor->GetPhysics()->GetOrigin() - landmark;
		const idCQuat axis = actor->GetPhysics()->GetAxis().ToCQuat();
		for( int i = 0; i < 3; i++ )
		{
			payload.WriteFloat( origin[i] );
		}
		for( int i = 0; i < 3; i++ )
		{
			payload.WriteFloat( axis[i] );
		}
		payload.Write( pose.GetDataPtr(), pose.Length() );
		return RECORD_MOVED | RECORD_CORPSE;
	}
	if( ent->IsType( idCorpse::Type ) )
	{
		const idVec3 origin = ent->GetPhysics()->GetOrigin() - landmark;
		const idCQuat axis = ent->GetPhysics()->GetAxis().ToCQuat();
		for( int i = 0; i < 3; i++ )
		{
			payload.WriteFloat( origin[i] );
		}
		for( int i = 0; i < 3; i++ )
		{
			payload.WriteFloat( axis[i] );
		}
		static_cast<idCorpse*>( ent )->WritePose( &payload );
		return RECORD_MOVED | RECORD_CORPSE;
	}
	unsigned short aspects = RECORD_MOVED | RECORD_HEALTH;
	if( ent->IsHidden() )
	{
		aspects |= RECORD_HIDDEN;
	}
	const idVec3 origin = ent->GetPhysics()->GetOrigin() - landmark;
	const idCQuat rotation = ent->GetPhysics()->GetAxis().ToCQuat();
	for( int i = 0; i < 3; i++ )
	{
		payload.WriteFloat( origin[i] );
	}
	for( int i = 0; i < 3; i++ )
	{
		payload.WriteFloat( rotation[i] );
	}
	if( ent->IsType( idMover_Binary::Type ) )
	{
		aspects |= RECORD_MOVER;
		payload.WriteUnsignedChar( static_cast<idMover_Binary*>( ent )->RecordAtPosition2() );
		payload.WriteUnsignedChar( ent->spawnArgs.GetInt( "locked" ) != 0 );
	}
	payload.WriteInt( ent->health );
	if( ent->IsType( idDamagable::Type ) && static_cast<idDamagable*>( ent )->RecordBroken() )
	{
		aspects |= RECORD_BROKEN;
	}
	if( motion )
	{
		aspects |= RECORD_MOTION;
		for( int i = 0; i < 3; i++ )
		{
			payload.WriteFloat( ent->GetPhysics()->GetLinearVelocity()[i] );
		}
		for( int i = 0; i < 3; i++ )
		{
			payload.WriteFloat( ent->GetPhysics()->GetAngularVelocity()[i] );
		}
	}
	if( motion && ent->GetBindMaster() != NULL )
	{
		const uint64 parent = strtoull( ent->GetBindMaster()->spawnArgs.GetString( "_worldCreatedId", "0" ), NULL, 10 );
		if( parent != 0 )
		{
			aspects |= RECORD_BIND;
			Record64( payload, parent );
			payload.WriteShort( ent->GetBindJoint() );
			payload.WriteShort( ent->GetBindBody() );
			payload.WriteUnsignedChar( ent->fl.bindOrientated ? 1 : 0 );
		}
	}
	return aspects;
}

static bool ApplyAspects( idEntity* ent, unsigned short aspects, idFile_Memory& payload, const idMat3& rotation, const idVec3& landmark )
{
	const int expected = ( ( aspects & RECORD_MOVED ) ? 24 : 0 ) + ( ( aspects & RECORD_MOVER ) ? 2 : 0 ) + ( ( aspects & RECORD_HEALTH ) ? 4 : 0 ) + ( ( aspects & RECORD_MOTION ) ? 24 : 0 ) + ( ( aspects & RECORD_BIND ) ? 13 : 0 );
	if( ( aspects & ~( RECORD_REMOVED | RECORD_MOVED | RECORD_MOVER | RECORD_HEALTH | RECORD_BROKEN | RECORD_HIDDEN | RECORD_MOTION | RECORD_BIND ) ) != 0 || payload.Length() != expected )
	{
		return false;
	}
	if( aspects & RECORD_MOVED )
	{
		idVec3 origin;
		idCQuat axis;
		for( int i = 0; i < 3; i++ )
		{
			payload.ReadFloat( origin[i] );
		}
		for( int i = 0; i < 3; i++ )
		{
			payload.ReadFloat( axis[i] );
		}
		ent->SetOrigin( landmark + origin * rotation );
		ent->SetAxis( axis.ToMat3() * rotation );
		ent->GetPhysics()->PutToRest();
	}
	if( aspects & RECORD_MOVER )
	{
		byte position, flags;
		payload.ReadUnsignedChar( position );
		payload.ReadUnsignedChar( flags );
		if( position > 1 || flags > 1 || !ent->IsType( idMover_Binary::Type ) )
		{
			return false;
		}
		static_cast<idMover_Binary*>( ent )->ApplyRecordState( position != 0, flags != 0 );
	}
	if( aspects & RECORD_HEALTH )
	{
		payload.ReadInt( ent->health );
	}
	if( ( aspects & RECORD_BROKEN ) && ent->IsType( idDamagable::Type ) )
	{
		static_cast<idDamagable*>( ent )->ApplyRecordBroken();
	}
	if( aspects & RECORD_HIDDEN )
	{
		ent->Hide();
	}
	if( aspects & RECORD_MOTION )
	{
		idVec3 linear, angular;
		for( int i = 0; i < 3; i++ )
		{
			payload.ReadFloat( linear[i] );
		}
		for( int i = 0; i < 3; i++ )
		{
			payload.ReadFloat( angular[i] );
		}
		ent->GetPhysics()->SetLinearVelocity( linear * rotation );
		ent->GetPhysics()->SetAngularVelocity( angular * rotation );
	}
	if( aspects & RECORD_BIND )
	{
		uint64 parent;
		short joint, body;
		byte orientated;
		RecordRead64( payload, parent );
		payload.ReadShort( joint );
		payload.ReadShort( body );
		payload.ReadUnsignedChar( orientated );
		ent->spawnArgs.Set( "_worldBindParent", va( "%llu", ( unsigned long long )parent ) );
		ent->spawnArgs.SetInt( "_worldBindJoint", joint );
		ent->spawnArgs.SetInt( "_worldBindBody", body );
		ent->spawnArgs.SetBool( "_worldBindOrientated", orientated != 0 );
	}
	return true;
}

bool idWorldStore::WriteCreated( idEntity* ent, const idVec3& landmark, idFile_Memory& out )
{
	if( ent == NULL || ent->GetEntityDefName()[0] == '\0' || idStr::Length( ent->GetEntityDefName() ) > 63 )
	{
		return false;
	}
	uint64 identity = strtoull( ent->spawnArgs.GetString( "_worldCreatedId", "0" ), NULL, 10 );
	if( identity == 0 )
	{
		identity = nextCreatedId++;
		ent->spawnArgs.Set( "_worldCreatedId", va( "%llu", ( unsigned long long )identity ) );
		ent->spawnArgs.Set( "_worldCreatedTime", va( "%lld", ( long long )gameLocal.GetWorldTime() ) );
	}
	idEntity* parent = ent->GetBindMaster();
	if( parent != NULL && parent->CanCrossTransition() && parent->spawnArgs.GetString( "_worldCreatedId", "0" )[0] == '0' )
	{
		parent->spawnArgs.Set( "_worldCreatedId", va( "%llu", ( unsigned long long )nextCreatedId++ ) );
		parent->spawnArgs.Set( "_worldCreatedTime", va( "%lld", ( long long )gameLocal.GetWorldTime() ) );
	}
	idFile_Memory payload;
	const unsigned short aspects = WriteAspects( ent, payload, landmark, true );
	if( !( aspects & RECORD_MOVED ) )
	{
		return false;
	}
	compile_time_assert( MAX_WORLD_CREATED_CARRY_BYTES <= 160 );
	if( ent->CanCrossTransition() && 8 + 1 + idStr::Length( ent->GetEntityDefName() ) + 8 + 4 + payload.Length() > MAX_WORLD_CREATED_CARRY_BYTES )
	{
		return false;
	}
	Record64( out, identity );
	RecordString( out, ent->IsType( idCorpse::Type ) ? static_cast<idCorpse*>( ent )->SourceName() : ent->GetEntityDefName() );
	Record64( out, strtoull( ent->spawnArgs.GetString( "_worldCreatedTime", "0" ), NULL, 10 ) );
	out.WriteUnsignedShort( aspects );
	out.WriteUnsignedShort( payload.Length() );
	out.Write( payload.GetDataPtr(), payload.Length() );
	return true;
}

static idEntity* SpawnCorpseRecord( const char* def, unsigned short aspects, idFile_Memory& payload )
{
	if( aspects != ( RECORD_MOVED | RECORD_CORPSE ) || payload.Length() < 53 || payload.Length() > 324 )
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
	idFile_Memory pose( "corpse pose", ( const char* )payload.GetDataPtr() + 24, payload.Length() - 24 );
	if( !static_cast<idCorpse*>( ent )->Build( gameLocal.FindEntityDef( def, false ), pose ) )
	{
		delete ent;
		return NULL;
	}
	return ent;
}

idEntity* idWorldStore::SpawnCreated( idFile_Memory& in, const idMat3& rotation, const idVec3& landmark )
{
	uint64 id, when;
	idStr def;
	unsigned short aspects, length;
	if( !RecordRead64( in, id ) || !RecordReadString( in, def ) || !RecordRead64( in, when ) || in.ReadUnsignedShort( aspects ) != 2 || in.ReadUnsignedShort( length ) != 2 || length > in.Length() - in.Tell() )
	{
		return NULL;
	}
	idFile_Memory payload( "created", ( const char* )in.GetDataPtr() + in.Tell(), length );
	in.Seek( length, FS_SEEK_CUR );
	if( !( aspects & RECORD_MOVED ) || !ValidateAspectBytes( aspects, payload.GetDataPtr(), length ) || gameLocal.FindEntityDef( def, false ) == NULL )
	{
		return NULL;
	}
	if( !( aspects & RECORD_CORPSE ) && applyingReset && !gameLocal.FindEntityDef( def, false )->dict.GetBool( "persist_noreset" ) )
	{
		return NULL;
	}
	if( aspects & RECORD_CORPSE )
	{
		idEntity* corpse = SpawnCorpseRecord( def, aspects, payload );
		if( corpse != NULL )
		{
			const int lifetime = corpse->spawnArgs.GetInt( "persist_lifetime", "-1" );
			const int64 death = static_cast<idCorpse*>( corpse )->DeathTime();
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
	const idDeclEntityDef* declaration = gameLocal.FindEntityDef( def, false );
	const int lifetime = declaration->dict.GetInt( "persist_lifetime", "-1" );
	if( lifetime > 0 && ( uint64 )gameLocal.GetWorldTime() >= when + ( uint64 )lifetime * 1000 )
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
	if( !ApplyAspects( ent, aspects, payload, rotation, landmark ) )
	{
		delete ent;
		return NULL;
	}
	return ent;
}

void idWorldStore::CommitCurrentMap()
{
	if( !IsActive() || committed || common->IsClient() || gameLocal.gamestate != GAMESTATE_ACTIVE || gameLocal.mapFile == NULL )
	{
		return;
	}
	const uint64 commitStart = Sys_Microseconds();
	// Expire before collecting pointers or enumerating canonical save objects.
	// Entity-slot traversal remains valid when deletion also removes attachments.
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
			delete ent;
		}
	}
	idMapFile* map = gameLocal.mapFile;
	bool isCleared = true;
	idList<createdCandidate_t, TAG_GAME> bodies;
	for( idEntity* ent = gameLocal.spawnedEntities.Next(); ent != NULL; ent = ent->spawnNode.Next() )
	{
		if( ent->IsType( idAI::Type ) && ent->health > 0 && static_cast<idAI*>( ent )->team != 0 )
		{
			isCleared = false;
		}
		if( ent->IsType( idCorpse::Type ) )
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
	int bodyCount = bodies.Num();
	for( int i = 0; i < bodies.Num() && g_recordCapCorpses.GetInteger() > 0 && bodyCount > g_recordCapCorpses.GetInteger(); i++ )
	{
		if( bodies[i].noCap )
		{
			continue;
		}
		corpseEvicted++;
		delete bodies[i].ent;
		bodies[i].ent = NULL;
		bodyCount--;
	}
	// Byte pressure may remove ordinary remains, never protected actors or items.
	// Size authored rows before emitting them so an evicted corpse becomes a
	// normal REMOVED tombstone with the same stable ordinal.
	idStr recordMap = gameLocal.GetMapName();
	recordMap.StripFileExtension();
	recordMap.ToLower();
	int otherRecordBytes = 0;
	for( int i = 0; i < records.Num(); i++ )
	{
		if( records[i].world != worldName || records[i].instance != instanceId || records[i].node != currentNode || records[i].map != recordMap )
		{
			otherRecordBytes += records[i].bytes.Num() + 8;
		}
	}
	const int authoredLimit = Min( g_recordBytesMap.GetInteger(), g_recordBytesWorld.GetInteger() - otherRecordBytes - 64 );
	int authoredBytes = 52 + worldName.Length() + recordMap.Length();
	for( int i = 0; i < mapSpawned.Num(); i++ )
	{
		const mapSpawnRow_t& row = mapSpawned[i];
		idEntity* ent = row.departed ? NULL : RecordEntity( row );
		idFile_Memory payload;
		if( ent != NULL )
		{
			WriteAspects( ent, payload, vec3_origin, false );
		}
		authoredBytes += 11 + idStr::Length( map->GetEntity( row.ordinal )->epairs.GetString( "name" ) ) + payload.Length();
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
		idFile_Memory payload;
		WriteAspects( ent, payload, vec3_origin, false );
		authoredBytes -= payload.Length();
		corpseEvicted++;
		delete ent;
		bodies[i].ent = NULL;
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
	record.WriteUnsignedInt( mapSpawned.Num() );
	for( int i = 0; i < mapSpawned.Num(); i++ )
	{
		const mapSpawnRow_t& row = mapSpawned[i];
		idEntity* ent = row.departed ? NULL : RecordEntity( row );
		if( ent != NULL && ( ent->IsType( idItem::Type ) || ent->IsType( idMoveableItem::Type ) ) && ent->IsHidden() && !ent->spawnArgs.GetBool( "inv_carry" ) && !ent->spawnArgs.GetBool( "inv_objective" ) )
		{
			ent = NULL;
		}
		record.WriteUnsignedShort( row.ordinal );
		record.WriteUnsignedInt( map->GetEntityCRC( row.ordinal ) );
		RecordString( record, map->GetEntity( row.ordinal )->epairs.GetString( "name" ) );
		idFile_Memory payload;
		const unsigned short aspects = ent == NULL ? RECORD_REMOVED : WriteAspects( ent, payload, vec3_origin, false );
		record.WriteUnsignedShort( aspects );
		record.WriteUnsignedShort( payload.Length() );
		record.Write( payload.GetDataPtr(), payload.Length() );
	}
	idList<createdCandidate_t, TAG_GAME> candidates;
	for( idEntity* ent = gameLocal.spawnedEntities.Next(); ent != NULL; ent = ent->spawnNode.Next() )
	{
		const int lifetime = ent->spawnArgs.GetInt( "persist_lifetime", ent->spawnArgs.GetBool( "dropped" ) ? "-1" : "0" );
		if( ent->mapSpawnIndex >= 0 || ( ent->GetBindMaster() != NULL && ( ent->spawnArgs.GetString( "_worldCreatedId", "0" )[0] == '0' || ent->GetBindMaster()->spawnArgs.GetString( "_worldCreatedId", "0" )[0] == '0' ) ) || ent->IsType( idPlayer::Type ) || ent->spawnArgs.GetBool( "_worldDeparted" ) || lifetime == 0 )
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
	int evicted = 0;
	int otherBytes = 0;
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
			delete candidates[i].ent;
			corpseEvicted++;
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
		gameLocal.Warning( "record: %s %d bytes, evicted %d; authored and nocap entries retained", name.c_str(), record.Length() + created.Length(), evicted );
	}
	record.WriteUnsignedInt( count );
	record.Write( created.GetDataPtr(), created.Length() );
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
	recordEntries = mapSpawned.Num();
	recordCreated = count;
	recordBytes = otherBytes + record.Length();
	recordDropped += evicted;
	commitUs = ( unsigned int )( Sys_Microseconds() - commitStart );
	gameLocal.Printf( "record commit map=%s entries=%d created=%d bytes=%d dropped=%d us=%u\n", name.c_str(), recordEntries, recordCreated, record.Length(), evicted, commitUs );
}

void idWorldStore::ApplyCurrentMap()
{
	if( !IsActive() || common->IsClient() || gameLocal.mapFile == NULL )
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
			const idDict& world = gameLocal.mapFile->GetEntity( 0 )->epairs;
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
		idList<int, TAG_GAME> claimed;
		idMapFile* map = gameLocal.mapFile;
		const bool identical = entitiesCRC == map->GetEntitiesCRC() && numEntities == map->GetNumEntities();
		for( unsigned int i = 0; i < count; i++ )
		{
			unsigned short ordinal, aspects, length;
			unsigned int crc;
			if( in.ReadUnsignedShort( ordinal ) != 2 || in.ReadUnsignedInt( crc ) != 4 || !RecordReadString( in, name ) || in.ReadUnsignedShort( aspects ) != 2 || in.ReadUnsignedShort( length ) != 2 || length > in.Length() - in.Tell() )
			{
				gameLocal.Error( "Malformed MREC entry for %s", mapName.c_str() );
			}
			int candidate = -1;
			if( identical && ordinal < map->GetNumEntities() )
			{
				candidate = ordinal;
			}
			else if( !name.IsEmpty() )
			{
				for( int j = 1; j < map->GetNumEntities(); j++ )
				{
					if( name == map->GetEntity( j )->epairs.GetString( "name" ) )
					{
						candidate = j;
						break;
					}
				}
			}
			else if( ordinal < map->GetNumEntities() && map->GetEntityCRC( ordinal ) == crc )
			{
				candidate = ordinal;
			}
			else
			{
				for( int j = 1; j < map->GetNumEntities(); j++ )
				{
					if( claimed.FindIndex( j ) < 0 && map->GetEntityCRC( j ) == crc )
					{
						candidate = j;
						break;
					}
				}
			}
			idFile_Memory payload( "aspects", ( const char* )in.GetDataPtr() + in.Tell(), length );
			in.Seek( length, FS_SEEK_CUR );
			if( candidate < 0 || claimed.FindIndex( candidate ) >= 0 || map->GetEntityCRC( candidate ) != crc )
			{
				gameLocal.Warning( "record: %s '%s'#%d dropped: entity changed", mapName.c_str(), name.c_str(), ordinal );
				continue;
			}
			claimed.Append( candidate );
			for( int row = 0; row < mapSpawned.Num(); row++ )
			{
				if( mapSpawned[row].ordinal != candidate )
				{
					continue;
				}
				idEntity* ent = RecordEntity( mapSpawned[row] );
				if( ent == NULL )
				{
					break;
				}
				if( applyingReset && !ent->spawnArgs.GetBool( "persist_noreset" ) )
				{
					break;
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
				break;
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

void idWorldStore::PrepareSave()
{
	// A save can precede the first departure. Capture namespace and current map
	// without suppressing the later leave commit after gameplay resumes.
	committed = false;
	CommitCurrentMap();
	committed = false;
}

void idWorldStore::Save( idSaveGame* save )
{
	mapGraphDirector.StoreRecord();
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
			for( int j = 0; j < count; j++ )
			{
				unsigned short ordinal;
				chunk.ReadUnsignedShort( ordinal );
				mapSpawned[j].ordinal = ordinal;
				chunk.ReadInt( mapSpawned[j].entityNumber );
				chunk.ReadInt( mapSpawned[j].spawnId );
				mapSpawned[j].departed = false;
				if( gameLocal.mapFile == NULL || ordinal >= gameLocal.mapFile->GetNumEntities() || mapSpawned[j].entityNumber < 0 || mapSpawned[j].entityNumber >= MAX_GENTITIES || ( j > 0 && ordinal <= mapSpawned[j - 1].ordinal ) )
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
			removed[i >> 3] |= 1 << ( i & 7 );
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
		for( int i = 0; i < landmarks.Num(); i++ )
		{
			const landmark_t& lm = landmarks[i];
			gameRenderWorld->DebugArrow( colorCyan, lm.origin, lm.origin + lm.axis[0] * 64.0f, 8 );
			gameRenderWorld->DrawText( lm.id, lm.origin, 0.2f, colorCyan, view );
		}
		for( int i = 0; i < transitionVolumes.Num(); i++ )
		{
			gameRenderWorld->DebugBounds( colorYellow, transitionVolumes[i].bounds );
		}
	}
}

const landmark_t* idWorldStore::FindLandmark( const char* name ) const
{
	for( int i = 0; i < landmarks.Num(); i++ )
	{
		if( landmarks[i].id.Icmp( name ) == 0 )
		{
			return &landmarks[i];
		}
	}
	return NULL;
}

const transitionVolume_t* idWorldStore::FindTransitionVolume( const char* name ) const
{
	for( int i = 0; i < transitionVolumes.Num(); i++ )
	{
		if( transitionVolumes[i].landmark.Icmp( name ) == 0 )
		{
			return &transitionVolumes[i];
		}
	}
	return NULL;
}

void idWorldStore::ReadMapData( idMapFile* map )
{
	landmarks.Clear();
	transitionVolumes.Clear();
	for( int i = 1; i < map->GetNumEntities(); i++ )
	{
		const idMapEntity* ent = map->GetEntity( i );
		const idDict& args = ent->epairs;
		const bool isLandmark = idStr::Icmp( args.GetString( "classname" ), "info_landmark" ) == 0;
		const bool isVolume = idStr::Icmp( args.GetString( "classname" ), "trigger_transition" ) == 0;
		if( !isLandmark && !isVolume )
		{
			continue;
		}
		idBounds bounds;
		bounds.Clear();
		idVec3 center = args.GetVector( "origin" );
		idVec3 forward = idAngles( 0, args.GetFloat( "angle" ), 0 ).ToForward();
		for( int p = 0; p < ent->GetNumPrimitives(); p++ )
		{
			if( ent->GetPrimitive( p )->GetType() != idMapPrimitive::TYPE_BRUSH )
			{
				continue;
			}
			const idMapBrush* brush = static_cast<const idMapBrush*>( ent->GetPrimitive( p ) );
			for( int side = 0; side < brush->GetNumSides(); side++ )
			{
				idWinding winding( brush->GetSide( side )->GetPlane() );
				for( int clip = 0; clip < brush->GetNumSides(); clip++ )
				{
					if( clip != side && !winding.ClipInPlace( -brush->GetSide( clip )->GetPlane(), 0.01f ) )
					{
						break;
					}
				}
				for( int v = 0; v < winding.GetNumPoints(); v++ )
				{
					bounds.AddPoint( winding[v].ToVec3() + ent->originOffset );
				}
				if( isLandmark && idStr::Icmp( brush->GetSide( side )->GetMaterial(), "textures/editor/landmark" ) == 0 && winding.GetNumPoints() >= 3 )
				{
					double areaSum = 0, sum[3] = { 0, 0, 0 };
					const idVec3 a = winding[0].ToVec3();
					for( int v = 1; v + 1 < winding.GetNumPoints(); v++ )
					{
						const idVec3 b = winding[v].ToVec3(), c = winding[v + 1].ToVec3();
						double u[3], edgeV[3], cross[3];
						for( int axis = 0; axis < 3; axis++ )
						{
							u[axis] = ( double )b[axis] - a[axis];
							edgeV[axis] = ( double )c[axis] - a[axis];
						}
						cross[0] = u[1] * edgeV[2] - u[2] * edgeV[1];
						cross[1] = u[2] * edgeV[0] - u[0] * edgeV[2];
						cross[2] = u[0] * edgeV[1] - u[1] * edgeV[0];
						const double area = sqrt( cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2] );
						areaSum += area;
						for( int axis = 0; axis < 3; axis++ )
						{
							sum[axis] += area * ( ( double )a[axis] + b[axis] + c[axis] ) / 3.0;
						}
					}
					if( areaSum > 0 )
					{
						for( int axis = 0; axis < 3; axis++ )
						{
							center[axis] = sum[axis] / areaSum + ent->originOffset[axis];
						}
					}
					forward = brush->GetSide( side )->GetPlane().Normal();
				}
			}
		}
		if( isLandmark )
		{
			landmark_t row;
			row.id = args.GetString( "landmark", args.GetString( "name" ) );
			row.kind = args.GetString( "kind" );
			row.origin = center;
			row.axis[0] = forward;
			row.axis[2].Set( 0, 0, 1 );
			row.axis[1] = forward.Cross( row.axis[2] );
			row.flags = ( args.GetBool( "entry", "1" ) ? 1 : 0 ) | ( args.GetBool( "exit", "1" ) ? 2 : 0 ) | ( args.GetBool( "required" ) ? 4 : 0 );
			row.radius = args.GetFloat( "radius", "256" );
			row.link = args.GetString( "link" );
			row.sealedBy = args.GetString( "sealed_by" );
			row.ordinal = i;
			landmarks.Append( row );
		}
		else if( !bounds.IsCleared() )
		{
			transitionVolume_t row;
			row.bounds = bounds;
			row.landmark = args.GetString( "landmark" );
			row.cap = idMath::ClampInt( 0, 64, args.GetInt( "cap", "64" ) );
			row.carryAll = idStr::Icmp( args.GetString( "carry", "all" ), "all" ) == 0;
			row.ordinal = i;
			transitionVolumes.Append( row );
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
