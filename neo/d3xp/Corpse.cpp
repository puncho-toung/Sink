#include "../idlib/precompiled.h"
#pragma hdrstop
#include "Game_local.h"
#include "Corpse.h"

idCVar g_corpses( "g_corpses", "1", CVAR_GAME | CVAR_BOOL | CVAR_ARCHIVE, "Keep settled corpses in world mode" );
CLASS_DECLARATION( idAFEntity_Base, idCorpse )
END_CLASS

static idLinkList<idCorpse> liveCorpses;

idCorpse::idCorpse()
{
	deathTime = 0;
	headHandle = -1;
	memset( &headRender, 0, sizeof( headRender ) );
	// creation order; the node unlinks itself when the corpse is deleted
	corpseNode.SetOwner( this );
	corpseNode.AddToEnd( liveCorpses );
}

idCorpse::~idCorpse()
{
	FreeHead();
}

void idCorpse::FreeHead()
{
	if( headHandle != -1 )
	{
		gameRenderWorld->FreeEntityDef( headHandle );
	}
	headHandle = -1;
}

void idCorpse::Spawn()
{
	// A snapshot spawn has no map keys. Build runs only after its source and
	// full pose are available, and never creates a networked head entity.
	fl.networkSync = true;
	spawnArgs.Set( "persist_lifetime", "-1" );
	BecomeInactive( TH_ALL );
	//puncho - P2b.1.10c: a corpse joins the corpse class; one a settle makes takes its monster's place there (Capture),
	// and one a record or a save builds evicts nothing: the cap waits for the next death or settle
	if( gameLocal.CorpseBudgetOn() )
	{
		gameLocal.JoinEntityBudget( this, BUDGET_CORPSE );
	}
}

bool idCorpse::Capture( idAI* actor )
{
	idFile_Memory pose;
	if( !WriteActorPose( actor, pose ) )
	{
		return false;
	}
	pose.MakeReadOnly();
	if( !Build( gameLocal.FindEntityDef( actor->GetEntityDefName(), false ), pose ) )
	{
		return false;
	}
	spawnArgs.SetInt( "persist_lifetime", actor->spawnArgs.GetInt( "persist_lifetime", "-1" ) );
	gameLocal.ReplaceInEntityBudget( actor, this );	//puncho - P2b.1.10c: the class keeps the monster's death order
	EnforceLiveCap();
	return true;
}

//puncho - P2.4 (bug review 2026-10-28): the commit only bounds what a record keeps, so a horde fight piled up corpses
// (about twenty clip models and two render entities each, plus a snapshot object) until the map was left. As each new
// corpse settles, the oldest unprotected ones go, under the same cap the record uses; deleting a map row's corpse
// marks its ordinal removed for the clients and the record
void idCorpse::EnforceLiveCap()
{
	//puncho - P2b.1.10c: while the corpse class runs it is the live cap (oldest death first, its monsters counted)
	if( gameLocal.CorpseBudgetOn() )
	{
		gameLocal.EnforceCorpseBudget( this, true );
		return;
	}
	const int cap = g_recordCapCorpses.GetInteger();
	if( cap <= 0 || common->IsClient() )
	{
		return;
	}
	for( int count = liveCorpses.Num(); count > cap; count-- )
	{
		idCorpse* oldest = NULL;
		for( idCorpse* corpse = liveCorpses.Next(); corpse != NULL; corpse = corpse->corpseNode.Next() )
		{
			if( corpse != this && !corpse->spawnArgs.GetBool( "persist_nocap" ) && ( oldest == NULL || corpse->deathTime < oldest->deathTime ) )
			{
				oldest = corpse;
			}
		}
		if( oldest == NULL )
		{
			break;
		}
		worldStore.corpseEvicted++;
		delete oldest;
	}
}

bool idCorpse::WriteActorPose( idAI* actor, idFile_Memory& pose )
{
	const idVec3 origin = actor->GetPhysics()->GetOrigin();
	for( int i = 0; i < 3; i++ )
	{
		pose.WriteFloat( origin[i] );
	}
	pose.WriteFloat( actor->GetPhysics()->GetAxis().ToAngles().yaw );
	pose.WriteUnsignedChar( ( actor->spawnArgs.GetBool( "persist_nocap" ) ? 1 : 0 ) | ( actor->spawnArgs.GetBool( "persist_noreset" ) ? 2 : 0 ) );
	const bool useAF = actor->IsActiveAF();
	pose.WriteUnsignedChar( useAF ? 1 : 0 );
	const uint64 time = gameLocal.GetWorldTime();
	pose.WriteUnsignedInt( ( unsigned int )time );
	pose.WriteUnsignedInt( ( unsigned int )( time >> 32 ) );
	if( useAF )
	{
		idPhysics_AF* physics = actor->GetAFPhysics();
		const int count = physics->GetNumBodies();
		if( count > 20 )
		{
			gameLocal.Warning( "corpse %s has %d AF bodies (maximum 20)", actor->name.c_str(), count );
			return false;
		}
		pose.WriteUnsignedChar( count );
		for( int body = 0; body < count; body++ )
		{
			pose.WriteUnsignedChar( body );
			const idVec3 offset = physics->GetOrigin( body ) - origin;
			const idCQuat axis = physics->GetAxis( body ).ToCQuat();
			for( int i = 0; i < 3; i++ )
			{
				if( idMath::Fabs( offset[i] ) >= 2048.0f )
				{
					return false;
				}
				pose.WriteShort( idMath::Ftoi( offset[i] * 16.0f ) );
			}
			for( int i = 0; i < 3; i++ )
			{
				pose.WriteShort( idMath::ClampInt( -32767, 32767, idMath::Ftoi( axis[i] * 32767.0f ) ) );
			}
		}
	}
	else
	{
		idAnimBlend* blend = actor->GetAnimator()->CurrentAnim( ANIMCHANNEL_ALL );
		if( blend->Anim() == NULL )
		{
			blend = actor->GetAnimator()->CurrentAnim( ANIMCHANNEL_TORSO );
		}
		const char* anim = blend->Anim() != NULL ? blend->Anim()->FullName() : "";
		const int length = idStr::Length( anim );
		if( length > 63 )
		{
			return false;
		}
		pose.WriteUnsignedChar( length );
		pose.Write( anim, length );
		pose.WriteUnsignedShort( idMath::ClampInt( 1, 65535, blend->GetFrameNumber( gameLocal.time ) ) );
	}
	return true;
}

bool idCorpse::Build( const idDeclEntityDef* source, idFile_Memory& pose )
{
	if( source == NULL || pose.Length() < 29 || pose.Length() > 300 )
	{
		return false;
	}
	idVec3 origin;
	float yaw;
	byte flags, kind;
	unsigned int low, high;
	for( int i = 0; i < 3; i++ )
	{
		if( pose.ReadFloat( origin[i] ) != 4 )
		{
			return false;
		}
	}
	if( pose.ReadFloat( yaw ) != 4 || pose.ReadUnsignedChar( flags ) != 1 || pose.ReadUnsignedChar( kind ) != 1 || pose.ReadUnsignedInt( low ) != 4 || pose.ReadUnsignedInt( high ) != 4 || kind > 1 || flags > 7 )
	{
		return false;
	}
	deathTime = ( int64 )( ( uint64 )low | ( ( uint64 )high << 32 ) );
	sourceName = source->GetName();
	spawnArgs.Set( "model", source->dict.GetString( "model" ) );
	spawnArgs.Set( "articulatedFigure", source->dict.GetString( "ragdoll", source->dict.GetString( "articulatedFigure" ) ) );
	spawnArgs.Set( "skin", source->dict.GetString( "skin" ) );
	spawnArgs.SetBool( "persist_nocap", ( flags & 1 ) != 0 );
	spawnArgs.SetBool( "persist_noreset", ( flags & 2 ) != 0 );
	spawnArgs.SetInt( "persist_lifetime", source->dict.GetInt( "persist_lifetime", "-1" ) );
	SetModel( spawnArgs.GetString( "model" ) );
	if( spawnArgs.GetString( "skin" )[0] )
	{
		SetSkin( declManager->FindSkin( spawnArgs.GetString( "skin" ) ) );
	}
	SetOrigin( origin );
	SetAxis( idAngles( 0, yaw, 0 ).ToMat3() );
	if( kind == 1 )
	{
		byte count;
		if( pose.ReadUnsignedChar( count ) != 1 || count == 0 || count > 20 || pose.Length() - pose.Tell() != count * 13 || !LoadAF() )
		{
			return false;
		}
		af.StartFromCurrentPose( 0 );
		for( int i = 0; i < count; i++ )
		{
			byte body;
			short value;
			idVec3 offset;
			idCQuat axis;
			pose.ReadUnsignedChar( body );
			if( body != i || body >= af.GetPhysics()->GetNumBodies() )
			{
				return false;
			}
			for( int j = 0; j < 3; j++ )
			{
				pose.ReadShort( value );
				offset[j] = value * ( 1.0f / 16.0f );
			}
			for( int j = 0; j < 3; j++ )
			{
				pose.ReadShort( value );
				axis[j] = value * ( 1.0f / 32767.0f );
			}
			af.GetPhysics()->SetOrigin( origin + offset, body );
			af.GetPhysics()->SetAxis( axis.ToMat3(), body );
		}
		af.GetPhysics()->PutToRest();
		//puncho - P2.4 (bug review 2026-10-28): the pose is static. An impulse (RadiusPush, a projectile) woke the AF and
		// moved the body away from the static head render entity, made the corpse a thinker and left poseBytes, the
		// saves, the record and the clients behind; impact off also makes it unpushable by movers
		af.GetPhysics()->DisableImpact();
		af.UpdateAnimation();
	}
	else
	{
		byte length;
		char anim[64];
		unsigned short frame;
		if( pose.ReadUnsignedChar( length ) != 1 || length > 63 || pose.Length() - pose.Tell() != length + 2 || pose.Read( anim, length ) != length || pose.ReadUnsignedShort( frame ) != 2 )
		{
			return false;
		}
		anim[length] = '\0';
		const int animNumber = animator.GetAnim( anim );
		if( animNumber != 0 )
		{
			animator.SetFrame( ANIMCHANNEL_ALL, animNumber, frame, gameLocal.time, 0 );
		}
	}
	animator.CreateFrame( gameLocal.time, true );
	animator.GetJoints( &renderEntity.numJoints, &renderEntity.joints );
	animator.GetBounds( gameLocal.time, renderEntity.bounds );
	renderEntity.callback = NULL;
	UpdateVisuals();
	Present();
	BecomeInactive( TH_ALL );
	poseBytes.SetNum( pose.Length() );
	memcpy( poseBytes.Ptr(), pose.GetDataPtr(), pose.Length() );

	FreeHead();
	const char* headDef = source->dict.GetString( "def_head" );
	const idDeclEntityDef* head = headDef[0] ? gameLocal.FindEntityDef( headDef, false ) : NULL;
	if( head != NULL )
	{
		memset( &headRender, 0, sizeof( headRender ) );
		headRender.hModel = headAnimator.SetModel( head->dict.GetString( "model" ) );
		const jointHandle_t joint = animator.GetJointHandle( source->dict.GetString( "head_joint" ) );
		idVec3 jointOrigin;
		idMat3 jointAxis;
		if( headRender.hModel != NULL && animator.GetJointTransform( joint, gameLocal.time, jointOrigin, jointAxis ) )
		{
			headAnimator.CreateFrame( gameLocal.time, true );
			headAnimator.GetJoints( &headRender.numJoints, &headRender.joints );
			headAnimator.GetBounds( gameLocal.time, headRender.bounds );
			headRender.origin = renderEntity.origin + jointOrigin * renderEntity.axis;
			headRender.axis = jointAxis * renderEntity.axis;
			for( int i = 0; i < 4; i++ )
			{
				headRender.shaderParms[i] = 1.0f;
			}
			headHandle = gameRenderWorld->AddEntityDef( &headRender );
		}
	}
	return true;
}

void idCorpse::WritePose( idFile* out ) const
{
	out->Write( poseBytes.Ptr(), poseBytes.Num() );
}

//puncho start - P2b.1.2: the map record of a corpse (idEntity's section and AI_RECORD_SCHEMA's)

/*
================
CorpsePayloadAxes

A pose's bytes after its leading origin, from world axes into a frame's (toFrame) or back: the yaw, and each AF body's
offset and axis. A frame with the identity axis copies the bytes as they are, so a map record holds the pose a corpse
was built with, bit for bit. False on a body list Build would refuse.
================
*/
static bool CorpsePayloadAxes( const byte* src, int length, byte* dst, const idMat3& axis, bool toFrame )
{
	memcpy( dst, src, length );
	if( axis == mat3_identity )
	{
		return true;
	}
	if( length < 17 )
	{
		return false;
	}
	float yaw;
	memcpy( &yaw, src, 4 );
	const float frameYaw = axis.ToAngles().yaw;
	yaw = LittleFloat( idMath::AngleNormalize180( toFrame ? LittleFloat( yaw ) - frameYaw : LittleFloat( yaw ) + frameYaw ) );
	memcpy( dst, &yaw, 4 );
	if( src[5] != 1 )
	{
		return true;
	}
	const int count = src[14];
	if( length != 15 + count * 13 )
	{
		return false;
	}
	const idMat3 rotation = toFrame ? axis.Transpose() : axis;
	for( int i = 0; i < count; i++ )
	{
		byte* body = dst + 15 + i * 13 + 1;
		short v[6];
		memcpy( v, body, sizeof( v ) );
		idVec3 offset;
		idCQuat q;
		for( int j = 0; j < 3; j++ )
		{
			offset[j] = LittleShort( v[j] ) * ( 1.0f / 16.0f );
			q[j] = LittleShort( v[3 + j] ) * ( 1.0f / 32767.0f );
		}
		offset = offset * rotation;
		q = ( q.ToMat3() * rotation ).ToCQuat();
		for( int j = 0; j < 3; j++ )
		{
			v[j] = LittleShort( ( short )idMath::ClampInt( -32767, 32767, idMath::Ftoi( offset[j] * 16.0f ) ) );
			v[3 + j] = LittleShort( ( short )idMath::ClampInt( -32767, 32767, idMath::Ftoi( q[j] * 32767.0f ) ) );
		}
		memcpy( body, v, sizeof( v ) );
	}
	return true;
}

void idCorpse::WriteRecordPayload( idRecordOut& rec, const byte* pose, int length )
{
	byte payload[300];
	if( length < 29 || length > 300 || !CorpsePayloadAxes( pose + 12, length - 12, payload, rec.Frame().axis, true ) )
	{
		rec.SetRemoved();
		return;
	}
	rec.WriteData( payload, length - 12 );
}

/*
================
idCorpse::WriteRecord

A corpse is recorded against its source def, so the record builds the same corpse again, and its Pose at the origin
its pose was built at (the pose's leading origin), not at the AF body its physics reports.
================
*/
void idCorpse::WriteRecord( idRecordOut& rec, const recordFrame_t& frame ) const
{
	// a corpse never built has no pose to record: REMOVED, the parent still called first (it keeps no section)
	if( poseBytes.Num() < 29 )
	{
		rec.SetRemoved();
	}
	else
	{
		idVec3 origin;
		memcpy( origin.ToFloatPtr(), poseBytes.Ptr(), sizeof( origin ) );
		for( int i = 0; i < 3; i++ )
		{
			origin[i] = LittleFloat( origin[i] );
		}
		rec.SetDefName( sourceName.c_str() );
		rec.SetCorpse( origin );
	}
	idAFEntity_Base::WriteRecord( rec, frame );
	rec.BeginSection( RECORD_SECTION_AI, RECORD_VERSION_AI );
	AI_RECORD_SCHEMA( SCHEMA_RECORD_DIFF, ( *this ) )
	AI_RECORD_SCHEMA( SCHEMA_RECORD_WRITE, ( *this ) )
	rec.EndSection();
}

recordApply_t idCorpse::ApplyRecord( idRecordIn& rec, const recordFrame_t& frame )
{
	const recordApply_t result = idAFEntity_Base::ApplyRecord( rec, frame );
	if( result != RECORD_APPLIED || !rec.BeginSection( RECORD_SECTION_AI ) )
	{
		return result;
	}
	AI_RECORD_SCHEMA( SCHEMA_RECORD_READ, ( *this ) )
	rec.EndSection();
	return rec.good ? RECORD_APPLIED : RECORD_INVALID;
}

bool idCorpse::CheckRecordSection( idRecordIn& rec )
{
	AI_RECORD_SCHEMA( SCHEMA_RECORD_CHECK, idCorpse )
	return rec.good;
}

void idCorpse::WriteRecordCorpse( idRecordOut& rec ) const
{
	if( !rec.IsRemoved() )
	{
		WriteRecordPayload( rec, poseBytes.Ptr(), poseBytes.Num() );
	}
}

// Build takes the pose with its origin first: the entry's Pose gives it (idEntity::ApplyRecordPose ran before this
// section), the source def is the entry's (rec.DefName()).
bool idCorpse::ApplyRecordCorpse( idRecordIn& rec )
{
	const int length = rec.FieldRemaining();
	if( !rec.posed || length < 17 || length > 288 )
	{
		return false;
	}
	byte payload[288];
	byte pose[300];
	for( int i = 0; i < 3; i++ )
	{
		const float value = LittleFloat( rec.poseOrigin[i] );
		memcpy( pose + i * 4, &value, 4 );
	}
	if( !rec.ReadData( payload, length ) || !CorpsePayloadAxes( payload, length, pose + 12, rec.Frame().axis, false ) )
	{
		return false;
	}
	idFile_Memory in( "corpse record", ( const char* )pose, 12 + length );
	return Build( gameLocal.FindEntityDef( rec.DefName(), false ), in );
}

// v2's rules on the pose after its origin: a finite yaw, flags and kind, a death time; an anim name and a frame, or a
// body list in order
bool idCorpse::CheckRecordCorpse( idRecordIn& rec )
{
	float yaw;
	byte flags, kind;
	uint64 death;
	if( !rec.ReadFiniteFloat( yaw ) || !rec.ReadByte( flags ) || flags > 7 || !rec.ReadByte( kind ) || kind > 1 || !rec.Read64( death ) || death > 0x7fffffffffffffffULL )
	{
		return false;
	}
	if( kind == 0 )
	{
		byte length;
		char anim[64];
		short frame;
		return rec.ReadByte( length ) && length <= 63 && rec.ReadData( anim, length ) && memchr( anim, 0, length ) == NULL && rec.ReadShort( frame ) && frame != 0;
	}
	byte count;
	if( !rec.ReadByte( count ) || count == 0 || count > 20 )
	{
		return false;
	}
	for( int i = 0; i < count; i++ )
	{
		byte index;
		if( !rec.ReadByte( index ) || index != i )
		{
			return false;
		}
		for( int j = 0; j < 6; j++ )
		{
			short value;
			if( !rec.ReadShort( value ) )
			{
				return false;
			}
		}
	}
	return true;
}
//puncho end

void idCorpse::Save( idSaveGame* save ) const
{
	save->WriteString( sourceName );
	save->WriteInt( poseBytes.Num() );
	save->Write( poseBytes.Ptr(), poseBytes.Num() );
}

void idCorpse::Restore( idRestoreGame* save )
{
	const int lifetime = spawnArgs.GetInt( "persist_lifetime", "-1" );
	int length;
	save->ReadString( sourceName );
	save->ReadInt( length );
	if( length < 29 || length > 300 )
	{
		save->Error( "Invalid corpse pose size" );
	}
	idList<byte, TAG_GAME> bytes;
	bytes.SetNum( length );
	save->Read( bytes.Ptr(), length );
	idFile_Memory pose( "corpse", ( const char* )bytes.Ptr(), length );
	if( !Build( gameLocal.FindEntityDef( sourceName, false ), pose ) )
	{
		save->Error( "Invalid corpse pose" );
	}
	spawnArgs.SetInt( "persist_lifetime", lifetime );
}

void idCorpse::WriteToSnapshot( idBitMsg& msg ) const
{
	const idDeclEntityDef* source = gameLocal.FindEntityDef( sourceName, false );
	msg.WriteBits( source != NULL ? source->Index() : -1, gameLocal.entityDefBits );
	msg.WriteBits( poseBytes.Num(), 9 );
	msg.WriteData( poseBytes.Ptr(), poseBytes.Num() );
}

void idCorpse::ReadFromSnapshot( const idBitMsg& msg )
{
	const int source = msg.ReadBits( gameLocal.entityDefBits );
	const int length = msg.ReadBits( 9 );
	byte bytes[300];
	if( source < 0 || source >= declManager->GetNumDecls( DECL_ENTITYDEF ) || length < 29 || length > 300 || msg.ReadData( bytes, length ) != length )
	{
		gameLocal.Error( "Invalid corpse snapshot" );
	}
	if( poseBytes.Num() == length && memcmp( poseBytes.Ptr(), bytes, length ) == 0 )
	{
		return;
	}
	idFile_Memory pose( "corpse", ( const char* )bytes, length );
	const idDeclEntityDef* def = static_cast<const idDeclEntityDef*>( declManager->DeclByIndex( DECL_ENTITYDEF, source, false ) );
	if( !Build( def, pose ) )
	{
		gameLocal.Error( "Invalid corpse snapshot pose" );
	}
}
