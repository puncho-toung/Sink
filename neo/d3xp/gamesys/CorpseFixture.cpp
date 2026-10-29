#include "../../idlib/precompiled.h"
#pragma hdrstop
#include "../Game_local.h"
#include "../Corpse.h"

// Developer fixture only. All pose construction and allocation precedes the
// measured frames; no idAI is constructed to obtain either pose representation.
void CorpseFixture_f( const idCmdArgs& args )
{
	//puncho - P2.4 (bug review 2026-10-28): a developer fixture with a hard-coded monster and map, not in retail builds;
	// the command stays registered (SysCmds.cpp) and only says so
#if defined( ID_RETAIL )
	gameLocal.Printf( "corpseFixture is a developer fixture and is not in retail builds\n" );
#else
	if( !gameLocal.CheatsOk() || args.Argc() != 3 || idStr::Icmpn( gameLocal.GetMapName(), "maps/_test/mg/", 14 ) != 0 )
	{
		gameLocal.Printf( "corpseFixture <anim|af|verify> <0|16|64> requires an mg test map\n" );
		return;
	}
	const int count = atoi( args.Argv( 2 ) );
	if( count != 0 && count != 16 && count != 64 )
	{
		return;
	}
	if( !idStr::Icmp( args.Argv( 1 ), "verify" ) )
	{
		int ai = 0, corpses = 0, thinking = 0;
		for( idEntity* ent = gameLocal.spawnedEntities.Next(); ent != NULL; ent = ent->spawnNode.Next() )
		{
			ai += ent->IsType( idAI::Type ) ? 1 : 0;
			if( ent->IsType( idCorpse::Type ) )
			{
				corpses++;
				thinking += ent->thinkFlags != 0 ? 1 : 0;
			}
		}
		gameLocal.Printf( "CORPSE_FIXTURE %s count=%d expected=%d ai=%d aiThinks=%d corpseThinking=%d\n", ai == 0 && gameLocal.aiStats.thinks == 0 && corpses == count && thinking == 0 ? "PASS" : "FAIL", corpses, count, ai, gameLocal.aiStats.thinks, thinking );
		return;
	}
	const bool afPose = !idStr::Icmp( args.Argv( 1 ), "af" );
	if( !afPose && idStr::Icmp( args.Argv( 1 ), "anim" ) )
	{
		return;
	}
	for( int i = MAX_CLIENTS; i < MAX_GENTITIES; i++ )
	{
		idEntity* ent = gameLocal.entities[i];
		if( ent != NULL && ( ent->IsType( idAI::Type ) || ent->IsType( idCorpse::Type ) ) )
		{
			delete ent;
		}
	}
	if( count == 0 )
	{
		gameLocal.Printf( "CORPSE_FIXTURE READY mode=%s count=0\n", args.Argv( 1 ) );
		return;
	}
	const idDeclEntityDef* source = gameLocal.FindEntityDef( "monster_zombie_maint", false );
	if( source == NULL )
	{
		gameLocal.Printf( "CORPSE_FIXTURE FAIL missing source\n" );
		return;
	}
	idDict setup;
	setup.Set( "model", source->dict.GetString( "model" ) );
	setup.Set( "articulatedFigure", source->dict.GetString( "ragdoll", source->dict.GetString( "articulatedFigure" ) ) );
	setup.SetBool( "nodrop", true );
	idAFEntity_Generic* seed = static_cast<idAFEntity_Generic*>( gameLocal.SpawnEntityType( idAFEntity_Generic::Type, &setup ) );
	idPhysics_AF* physics = seed->GetAFPhysics();
	const int bodies = physics->GetNumBodies();
	const idAnim* anim = seed->GetAnimator()->GetAnim( seed->GetAnimator()->GetAnim( "idle" ) );
	if( bodies < 1 || bodies > 20 || ( !afPose && ( anim == NULL || idStr::Length( anim->FullName() ) > 63 ) ) )
	{
		delete seed;
		gameLocal.Printf( "CORPSE_FIXTURE FAIL unsupported source pose\n" );
		return;
	}
	int built = 0;
	for( int n = 0; n < count; n++ )
	{
		const idVec3 origin( -280.0f + ( n % 8 ) * 80.0f, 50.0f + ( n / 8 ) * 50.0f, 32.0f );
		idFile_Memory pose;
		for( int axis = 0; axis < 3; axis++ )
		{
			pose.WriteFloat( origin[axis] );
		}
		pose.WriteFloat( 0 );
		pose.WriteUnsignedChar( 0 );
		pose.WriteUnsignedChar( afPose ? 1 : 0 );
		pose.WriteUnsignedInt( 0 );
		pose.WriteUnsignedInt( 0 );
		if( afPose )
		{
			pose.WriteUnsignedChar( bodies );
			for( int body = 0; body < bodies; body++ )
			{
				pose.WriteUnsignedChar( body );
				const idVec3 offset = physics->GetOrigin( body ) - seed->GetPhysics()->GetOrigin();
				const idCQuat orientation = physics->GetAxis( body ).ToCQuat();
				for( int axis = 0; axis < 3; axis++ )
				{
					pose.WriteShort( idMath::Ftoi( offset[axis] * 16.0f ) );
				}
				for( int axis = 0; axis < 3; axis++ )
				{
					pose.WriteShort( idMath::ClampInt( -32767, 32767, idMath::Ftoi( orientation[axis] * 32767.0f ) ) );
				}
			}
		}
		else
		{
			const int length = idStr::Length( anim->FullName() );
			pose.WriteUnsignedChar( length );
			pose.Write( anim->FullName(), length );
			pose.WriteUnsignedShort( 1 );
		}
		idDict args;
		args.Set( "classname", "corpse" );
		idEntity* ent = NULL;
		if( gameLocal.SpawnEntityDef( args, &ent ) && ent != NULL )
		{
			pose.MakeReadOnly();
			if( static_cast<idCorpse*>( ent )->Build( source, pose ) )
			{
				built++;
			}
			else
			{
				delete ent;
			}
		}
	}
	delete seed;
	gameLocal.Printf( "CORPSE_FIXTURE %s mode=%s count=%d\n", built == count ? "READY" : "FAIL", args.Argv( 1 ), built );
#endif
}
