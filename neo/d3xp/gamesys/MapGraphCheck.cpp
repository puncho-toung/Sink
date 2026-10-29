#include "../../idlib/precompiled.h"
#pragma hdrstop
#include "../Game_local.h"

static void KitFailure( idFile* csv, const char* map, int rule, const char* text, int& failures )
{
	gameLocal.Printf( "mapGraphCheck: %s: K%d %s\n", map, rule, text );
	if( csv != NULL )
	{
		csv->Printf( "%s,K%d,%s\n", map, rule, text );
	}
	failures++;
}

static bool KitDoorway( const idMapEntity* ent, idWinding& winding, idVec3& normal )
{
	int faces = 0;
	for( int p = 0; p < ent->GetNumPrimitives(); p++ )
	{
		if( ent->GetPrimitive( p )->GetType() != idMapPrimitive::TYPE_BRUSH )
		{
			continue;
		}
		const idMapBrush* brush = static_cast<const idMapBrush*>( ent->GetPrimitive( p ) );
		for( int side = 0; side < brush->GetNumSides(); side++ )
		{
			if( idStr::Icmp( brush->GetSide( side )->GetMaterial(), "textures/editor/landmark" ) != 0 )
			{
				continue;
			}
			faces++;
			winding.BaseForPlane( brush->GetSide( side )->GetPlane() );
			for( int clip = 0; clip < brush->GetNumSides(); clip++ )
			{
				if( clip != side && !winding.ClipInPlace( -brush->GetSide( clip )->GetPlane(), 0.001f ) )
				{
					break;
				}
			}
			normal = brush->GetSide( side )->GetPlane().Normal();
		}
	}
	return faces == 1 && winding.GetNumPoints() >= 3 && idMath::Fabs( normal.z ) < 0.001f && winding.GetArea() >= 1024.0f && winding.Check( false );
}

static idVec3 KitCentroid( const idWinding& winding )
{
	double sum[3] = { 0, 0, 0 }, total = 0;
	for( int i = 1; i + 1 < winding.GetNumPoints(); i++ )
	{
		const idVec3 a = winding[0].ToVec3(), b = winding[i].ToVec3(), c = winding[i + 1].ToVec3();
		double u[3], v[3], cross[3];
		for( int j = 0; j < 3; j++ )
		{
			u[j] = ( double )b[j] - a[j];
			v[j] = ( double )c[j] - a[j];
		}
		cross[0] = u[1] * v[2] - u[2] * v[1];
		cross[1] = u[2] * v[0] - u[0] * v[2];
		cross[2] = u[0] * v[1] - u[1] * v[0];
		const double area = sqrt( cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2] );
		total += area;
		for( int j = 0; j < 3; j++ )
		{
			sum[j] += area * ( ( double )a[j] + b[j] + c[j] ) / 3.0;
		}
	}
	return total > 0 ? idVec3( sum[0] / total, sum[1] / total, sum[2] / total ) : vec3_origin;
}

static bool KitSameOutline( const idWinding& a, const idVec3& an, const idWinding& b, const idVec3& bn, bool mirror )
{
	if( a.GetNumPoints() != b.GetNumPoints() )
	{
		return false;
	}
	const idVec3 ac = KitCentroid( a ), bc = KitCentroid( b );
	const idVec3 ar = an.Cross( idVec3( 0, 0, 1 ) ), br = bn.Cross( idVec3( 0, 0, 1 ) );
	for( int i = 0; i < a.GetNumPoints(); i++ )
	{
		const idVec3 point = a[i].ToVec3() - ac;
		bool found = false;
		for( int j = 0; j < b.GetNumPoints(); j++ )
		{
			const idVec3 other = b[j].ToVec3() - bc;
			if( idMath::Fabs( point * ar - ( mirror ? -1.0f : 1.0f ) * ( other * br ) ) < 0.01f && idMath::Fabs( point.z - other.z ) < 0.01f )
			{
				found = true;
				break;
			}
		}
		if( !found )
		{
			return false;
		}
	}
	return true;
}

void MapGraphCheck_f( const idCmdArgs& args )
{
	if( args.Argc() < 2 )
	{
		return;
	}
	const idDeclMapGraph* decl = static_cast<const idDeclMapGraph*>( declManager->FindType( DECL_MAPGRAPH, args.Argv( 1 ), false ) );
	if( decl == NULL )
	{
		gameLocal.Printf( "mapGraphCheck: FAIL missing graph\n" );
		return;
	}
	idFile* csv = fileSystem->OpenFileWrite( "mapgraphcheck.csv" );
	if( csv != NULL )
	{
		csv->Printf( "map,rule,message\n" );
	}
	int failures = 0;
	for( int k = 0; k < decl->kinds.Num(); k++ )
	{
		for( int m = 0; m < decl->kinds[k].maps.Num(); m++ )
		{
			const graphMap_t& authored = decl->kinds[k].maps[m];
			idMapFile map;
			if( !map.Parse( authored.name ) )
			{
				KitFailure( csv, authored.name, 1, "map cannot be parsed", failures );
				continue;
			}
			idWorldStore tables;
			tables.ReadMapData( &map );
			for( int i = 0; i < tables.landmarks.Num(); i++ )
			{
				const landmark_t& landmark = tables.landmarks[i];
				const idMapEntity* ent = map.GetEntity( landmark.ordinal );
				if( landmark.id.IsEmpty() )
				{
					KitFailure( csv, authored.name, 2, "empty landmark id", failures );
				}
				if( ent->epairs.GetString( "landmark" )[0] == '\0' )
				{
					gameLocal.Warning( "mapGraphCheck: %s: K2 name-only landmark %s", authored.name.c_str(), landmark.id.c_str() );
				}
				for( int j = 0; j < i; j++ )
				{
					if( !tables.landmarks[j].id.Icmp( landmark.id ) )
					{
						KitFailure( csv, authored.name, 2, "duplicate landmark id", failures );
					}
				}
				idWinding door;
				idVec3 normal;
				if( !decl->persistent && !KitDoorway( ent, door, normal ) )
				{
					KitFailure( csv, authored.name, 3, "doorway must be one convex vertical face of at least 1024 square units", failures );
				}
				else if( door.GetNumPoints() > 0 )
				{
					idMapFile reference;
					bool matched = false;
					if( reference.Parse( va( "prefabs/landmarks/%s.map", landmark.kind.c_str() ) ) )
					{
						for( int j = 1; j < reference.GetNumEntities(); j++ )
						{
							idWinding outline;
							idVec3 referenceNormal;
							if( KitDoorway( reference.GetEntity( j ), outline, referenceNormal ) && KitSameOutline( door, normal, outline, referenceNormal, false ) )
							{
								matched = true;
							}
						}
					}
					if( !matched || !KitSameOutline( door, normal, door, normal, true ) )
					{
						KitFailure( csv, authored.name, 4, "outline differs from kind template or is not mirror-symmetric", failures );
					}
				}
				bool declared = false;
				for( int j = 0; j < authored.landmarks.Num(); j++ )
				{
					const graphLandmark_t& expected = authored.landmarks[j];
					if( expected.id == landmark.id && expected.kind == landmark.kind && expected.flags == landmark.flags )
					{
						declared = true;
					}
				}
				if( !declared )
				{
					KitFailure( csv, authored.name, 5, "map landmark missing or different in decl", failures );
				}
				if( landmark.flags & 4 )
				{
					bool covered = false;
					for( int r = 0; r < decl->rules.Num(); r++ )
					{
						if( decl->rules[r].landmark == landmark.kind && ( decl->rules[r].endcap || decl->rules[r].from == decl->kinds[k].name ) )
						{
							covered = true;
						}
					}
					if( !covered )
					{
						KitFailure( csv, authored.name, 7, "required exit has no rule or endcap", failures );
					}
				}
				if( !landmark.link.IsEmpty() && landmark.link[0] != '@' )
				{
					const int colon = landmark.link.Find( ':' );
					idMapFile destination;
					idWorldStore target;
					const landmark_t* arrival = NULL;
					if( colon > 0 && destination.Parse( landmark.link.Left( colon ) ) )
					{
						target.ReadMapData( &destination );
						arrival = target.FindLandmark( landmark.link.c_str() + colon + 1 );
					}
					if( arrival == NULL || !( arrival->flags & 1 ) || arrival->kind != landmark.kind )
					{
						KitFailure( csv, authored.name, 10, "link does not resolve to a matching entry", failures );
					}
				}
			}
			for( int i = 0; i < authored.landmarks.Num(); i++ )
			{
				if( tables.FindLandmark( authored.landmarks[i].id ) == NULL )
				{
					KitFailure( csv, authored.name, 5, "decl landmark absent from map", failures );
				}
			}
			for( int i = 0; i < tables.transitionVolumes.Num(); i++ )
			{
				const transitionVolume_t& volume = tables.transitionVolumes[i];
				const landmark_t* landmark = tables.FindLandmark( volume.landmark );
				if( landmark == NULL || !volume.bounds.ContainsPoint( landmark->origin ) )
				{
					KitFailure( csv, authored.name, 6, "carry volume misses its landmark", failures );
				}
			}
			for( int i = 1; i < map.GetNumEntities(); i++ )
			{
				const idDict& keys = map.GetEntity( i )->epairs;
				//puncho - P2.4 (bug review 2026-10-28): a record holds names of 63 bytes at most. The runtime skips a longer
				// entity name or landmark id with a warning and never records a longer def name; refuse them here
				const char* lengthKeys[] = { "name", "landmark", "classname", "levelName", "triggerName" };
				for( int j = 0; j < 5; j++ )
				{
					if( idStr::Length( keys.GetString( lengthKeys[j] ) ) > 63 )
					{
						KitFailure( csv, authored.name, 12, va( "entity %d %s exceeds 63 bytes", i, lengthKeys[j] ), failures );
					}
				}
				if( !idStr::Icmp( keys.GetString( "classname" ), "trigger_changelevel" ) )
				{
					const landmark_t* landmark = tables.FindLandmark( keys.GetString( "landmark" ) );
					if( landmark == NULL || !( landmark->flags & 2 ) )
					{
						KitFailure( csv, authored.name, 6, "changelevel trigger does not name an exit", failures );
					}
				}
				const char* keysToCheck[] = { "dungeon_if", "dungeon_unless" };
				for( int j = 0; j < 2; j++ )
				{
					const char* flag = keys.GetString( keysToCheck[j] );
					if( flag[0] && ( decl->FindFlag( flag ) < 0 || decl->FindFlag( flag ) >= 64 ) )
					{
						KitFailure( csv, authored.name, 8, "spawn flag is unknown or outside recipe bits", failures );
					}
				}
				const idDict* def = gameLocal.FindEntityDefDict( keys.GetString( "classname" ), false );
				if( def != NULL )
				{
					const char* spawnClass = def->GetString( "spawnclass" );
					const idTypeInfo* type = idClass::GetClass( spawnClass );
					const bool recorded = type && ( type->IsType( idAI::Type ) || type->IsType( idItem::Type ) || type->IsType( idMoveable::Type ) || type->IsType( idMover_Binary::Type ) || type->IsType( idDamagable::Type ) );
					if( recorded && !keys.GetString( "name" )[0] )
					{
						gameLocal.Warning( "mapGraphCheck: %s: K9 unnamed record entity ordinal %d", authored.name.c_str(), i );
					}
					if( recorded && keys.GetString( "name" )[0] )
					{
						for( int j = 1; j < i; j++ )
						{
							if( !idStr::Icmp( keys.GetString( "name" ), map.GetEntity( j )->epairs.GetString( "name" ) ) )
							{
								gameLocal.Warning( "mapGraphCheck: %s: K9 duplicate record name %s", authored.name.c_str(), keys.GetString( "name" ) );
							}
						}
					}
					if( !idStr::Icmp( keys.GetString( "classname" ), "target_levelTrigger" ) )
					{
						//puncho - P2.4 (bug review 2026-10-28): resolve levelName as the runtime matches it (MapGraph.cpp
						// GraphEffectMatches): a bare stock name is the graph map with that base name, a path is maps/<path>
						idStr level = keys.GetString( "levelName" );
						level.ToLower();
						level.BackSlashesToSlashes();
						level.StripFileExtension();
						if( level.Find( '/' ) < 0 )
						{
							idStr resolved;
							for( int kk = 0; kk < decl->kinds.Num() && resolved.IsEmpty(); kk++ )
							{
								for( int mm = 0; mm < decl->kinds[kk].maps.Num() && resolved.IsEmpty(); mm++ )
								{
									const idStr& node = decl->kinds[kk].maps[mm].name;
									if( idStr::Cmp( node.c_str() + node.Last( '/' ) + 1, level ) == 0 )
									{
										resolved = node;
									}
								}
							}
							if( !resolved.IsEmpty() )
							{
								level = resolved;
							}
						}
						else if( level.Icmpn( "maps/", 5 ) != 0 )
						{
							level = "maps/" + level;
						}
						idMapFile target;
						int matches = 0;
						if( target.Parse( level ) )
						{
							for( int j = 1; j < target.GetNumEntities(); j++ )
							{
								matches += keys.GetString( "triggerName" )[0] && !idStr::Icmp( keys.GetString( "triggerName" ), target.GetEntity( j )->epairs.GetString( "name" ) ) ? 1 : 0;
							}
						}
						if( matches != 1 )
						{
							gameLocal.Warning( "mapGraphCheck: %s: K9 level-trigger target must have one unique name", authored.name.c_str() );
						}
					}
					if( def->GetFloat( "burnaway" ) != 0 && def->GetInt( "persist_lifetime", "-1" ) != 0 )
					{
						gameLocal.Warning( "mapGraphCheck: %s: K9 persistent burning def %s", authored.name.c_str(), keys.GetString( "classname" ) );
					}
				}
			}
		}
	}
	const uint64 generationStart = Sys_Microseconds();
	for( int seed = 0; seed < idMath::ClampInt( 1, 10000, args.Argc() > 2 ? atoi( args.Argv( 2 ) ) : 100 ); seed++ )
	{
		idMapGraphDirector generated;
		if( !generated.Generate( decl, seed ) || generated.nodes.Num() > decl->maxNodes )
		{
			KitFailure( csv, args.Argv( 1 ), 11, "generator failed or exceeded node cap", failures );
		}
		const int nodeCount = Min( 256, generated.nodes.Num() );
		for( int kind = 0; kind < decl->kinds.Num(); kind++ )
		{
			int count = 0;
			for( int i = 0; i < nodeCount; i++ )
			{
				count += generated.nodes[i].kindIndex == kind ? 1 : 0;
			}
			if( !decl->persistent && count > decl->kinds[kind].cap )
			{
				KitFailure( csv, args.Argv( 1 ), 11, "generated kind cap exceeded", failures );
			}
		}
		bool reached[256] = { false };
		if( generated.nodes.Num() > 0 )
		{
			reached[0] = true;
		}
		for( int pass = 0; pass < nodeCount; pass++ )
		{
			for( int i = 0; i < generated.links.Num(); i++ )
			{
				const dungeonLink_t& link = generated.links[i];
				if( link.from < nodeCount && link.to < nodeCount && reached[link.from] )
				{
					reached[link.to] = true;
				}
			}
		}
		for( int i = 0; i < nodeCount; i++ )
		{
			if( !decl->persistent && !reached[i] )
			{
				KitFailure( csv, args.Argv( 1 ), 11, "generated node is unreachable", failures );
			}
		}
		if( generated.sealedRequired > 0 )
		{
			gameLocal.Printf( "mapGraphCheck: seed %d sealed required exits %d\n", seed, generated.sealedRequired );
		}
	}
	gameLocal.Printf( "mapGraphCheck: %s failures=%d generationCheckMs=%.3f\n", failures == 0 ? "PASS" : "FAIL", failures, ( Sys_Microseconds() - generationStart ) * 0.001 );
	if( csv != NULL )
	{
		fileSystem->CloseFile( csv );
	}
}
