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

bool ValidateAspectBytes( unsigned short aspects, const char* data, int length )
{
	if( ( aspects & ~( RECORD_REMOVED | RECORD_MOVED | RECORD_MOVER | RECORD_HEALTH | RECORD_BROKEN | RECORD_HIDDEN | RECORD_CORPSE | RECORD_MOTION | RECORD_BIND | RECORD_SHOWN ) ) || ( aspects & ( RECORD_HIDDEN | RECORD_SHOWN ) ) == ( RECORD_HIDDEN | RECORD_SHOWN ) )
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

// The pose a map entity spawns with, from the keys idEntity::Spawn reads (ParseSpawnArgsToRenderEntity's origin and
// rotation, plus an origin brush's offset for an entity with a model). A model scale or a key this misses makes the
// live pose differ, which only writes a MOVED the entity did not need: the safe direction.
static void RecordSpawnPose( const idEntity* ent, idVec3& origin, idMat3& axis )
{
	const idDict& args = ent->spawnArgs;
	origin = args.GetVector( "origin" );
	if( args.GetString( "model" )[0] != '\0' )
	{
		origin += ent->GetOriginBrushOffset();
	}
	idAngles angles;
	if( !args.GetMatrix( "rotation", "1 0 0 0 1 0 0 0 1", axis ) )
	{
		if( args.GetAngles( "angles", "0 0 0", angles ) )
		{
			axis = angles.ToMat3();
		}
		else
		{
			axis = idAngles( 0.0f, args.GetFloat( "angle" ), 0.0f ).ToMat3();
		}
	}
}

// bindParent is the created identity of the bind master a created record binds to, 0 for none.
unsigned short WriteAspects( idEntity* ent, idFile_Memory& payload, const idVec3& landmark, bool motion, uint64 bindParent )
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
	//puncho start - P2.4 (bug review 2026-10-28): an authored row is a delta over the .map. MOVED only for an entity at
	// rest off its spawn pose, and never for a parametric mover: SetOrigin resets its extrapolation base, so a
	// func_bobbing or func_pendulum recentred its swing on the commit phase and ratcheted on every revisit (binary
	// movers keep their MOVER aspect). A bound entity follows its master, and SetOrigin would take the world position
	// as its offset. A created entity has no spawn pose, so it always carries one. HEALTH only when it changed
	unsigned short aspects = 0;
	bool moved = motion;
	if( !motion && ent->GetBindMaster() == NULL && !ent->GetPhysics()->IsType( idPhysics_Parametric::Type ) && ent->GetPhysics()->IsAtRest() )
	{
		idVec3 spawnOrigin;
		idMat3 spawnAxis;
		RecordSpawnPose( ent, spawnOrigin, spawnAxis );
		moved = ( ent->GetPhysics()->GetOrigin() - spawnOrigin ).LengthSqr() > 1.0f || !ent->GetPhysics()->GetAxis().Compare( spawnAxis, 0.001f );
	}
	// visibility against the "hide" key the entity spawns with: a hide-until-triggered entity revealed in play comes
	// back shown, and one never revealed comes back as authored
	if( ent->IsHidden() != ent->spawnArgs.GetBool( "hide" ) )
	{
		aspects |= ent->IsHidden() ? RECORD_HIDDEN : RECORD_SHOWN;
	}
	if( moved )
	{
		aspects |= RECORD_MOVED;
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
	}
	if( ent->IsType( idMover_Binary::Type ) )
	{
		aspects |= RECORD_MOVER;
		payload.WriteUnsignedChar( static_cast<idMover_Binary*>( ent )->RecordAtPosition2() );
		payload.WriteUnsignedChar( ent->spawnArgs.GetInt( "locked" ) != 0 );
	}
	if( ent->health != ent->spawnArgs.GetInt( "health" ) )
	{
		aspects |= RECORD_HEALTH;
		payload.WriteInt( ent->health );
	}
	//puncho end
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
	if( motion && ent->GetBindMaster() != NULL && bindParent != 0 )
	{
		aspects |= RECORD_BIND;
		Record64( payload, bindParent );
		payload.WriteShort( ent->GetBindJoint() );
		payload.WriteShort( ent->GetBindBody() );
		payload.WriteUnsignedChar( ent->fl.bindOrientated ? 1 : 0 );
	}
	return aspects;
}

bool ApplyAspects( idEntity* ent, unsigned short aspects, idFile_Memory& payload, const idMat3& rotation, const idVec3& landmark )
{
	const int expected = ( ( aspects & RECORD_MOVED ) ? 24 : 0 ) + ( ( aspects & RECORD_MOVER ) ? 2 : 0 ) + ( ( aspects & RECORD_HEALTH ) ? 4 : 0 ) + ( ( aspects & RECORD_MOTION ) ? 24 : 0 ) + ( ( aspects & RECORD_BIND ) ? 13 : 0 );
	if( ( aspects & ~( RECORD_REMOVED | RECORD_MOVED | RECORD_MOVER | RECORD_HEALTH | RECORD_BROKEN | RECORD_HIDDEN | RECORD_MOTION | RECORD_BIND | RECORD_SHOWN ) ) != 0 || payload.Length() != expected )
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
	else if( aspects & RECORD_SHOWN )
	{
		ent->Show();
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
	return EncodeCreated( ent, landmark, out, true );
}

//puncho - P2.4 (bug review 2026-10-28): the transition carry sizes each team member before it accepts the team; that
// pass called WriteCreated, which stamped _worldCreatedId and _worldCreatedTime for good on the members of a team it
// then rejected and on their bind parents. This writes the same bytes, with placeholder identities of the same size,
// and stamps nothing
bool idWorldStore::MeasureCreated( idEntity* ent, const idVec3& landmark, idFile_Memory& out )
{
	return EncodeCreated( ent, landmark, out, false );
}

// assign takes the entity's (and its crossing bind master's) created identity from nextCreatedId and stamps it;
// without it the identities a write would take are only written.
bool idWorldStore::EncodeCreated( idEntity* ent, const idVec3& landmark, idFile_Memory& out, bool assign )
{
	if( ent == NULL )
	{
		return false;
	}
	// a corpse records its source def; a name over 63 bytes is refused here, never raised at the write
	const char* recordName = ent->IsType( idCorpse::Type ) ? static_cast<idCorpse*>( ent )->SourceName() : ent->GetEntityDefName();
	if( ent->GetEntityDefName()[0] == '\0' || recordName[0] == '\0' || idStr::Length( recordName ) > 63 )
	{
		return false;
	}
	uint64 nextIdentity = nextCreatedId;
	uint64 identity = strtoull( ent->spawnArgs.GetString( "_worldCreatedId", "0" ), NULL, 10 );
	uint64 created = strtoull( ent->spawnArgs.GetString( "_worldCreatedTime", "0" ), NULL, 10 );
	if( identity == 0 )
	{
		identity = nextIdentity++;
		created = ( uint64 )gameLocal.GetWorldTime();
		if( assign )
		{
			ent->spawnArgs.Set( "_worldCreatedId", va( "%llu", ( unsigned long long )identity ) );
			ent->spawnArgs.Set( "_worldCreatedTime", va( "%lld", ( long long )gameLocal.GetWorldTime() ) );
		}
	}
	uint64 parentIdentity = 0;
	idEntity* parent = ent->GetBindMaster();
	if( parent != NULL )
	{
		parentIdentity = strtoull( parent->spawnArgs.GetString( "_worldCreatedId", "0" ), NULL, 10 );
		if( parentIdentity == 0 && parent->CanCrossTransition() )
		{
			parentIdentity = nextIdentity++;
			if( assign )
			{
				parent->spawnArgs.Set( "_worldCreatedId", va( "%llu", ( unsigned long long )parentIdentity ) );
				parent->spawnArgs.Set( "_worldCreatedTime", va( "%lld", ( long long )gameLocal.GetWorldTime() ) );
			}
		}
	}
	if( assign )
	{
		nextCreatedId = nextIdentity;
	}
	idFile_Memory payload;
	const unsigned short aspects = WriteAspects( ent, payload, landmark, true, parentIdentity );
	if( !( aspects & RECORD_MOVED ) )
	{
		return false;
	}
	compile_time_assert( MAX_WORLD_CREATED_CARRY_BYTES <= 160 );
	if( ent->CanCrossTransition() && 8 + 1 + idStr::Length( recordName ) + 8 + 4 + payload.Length() > MAX_WORLD_CREATED_CARRY_BYTES )
	{
		return false;
	}
	Record64( out, identity );
	RecordString( out, recordName );
	Record64( out, created );
	out.WriteUnsignedShort( aspects );
	out.WriteUnsignedShort( payload.Length() );
	out.Write( payload.GetDataPtr(), payload.Length() );
	return true;
}

idEntity* SpawnCorpseRecord( const char* def, unsigned short aspects, idFile_Memory& payload )
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
