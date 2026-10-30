#include "../../idlib/precompiled.h"
#pragma hdrstop

#include "../Game_local.h"
#include "MapGraph.h"
#include "WorldStore.h"
#include "MapGraphLocal.h"

void MapGraphStart_f( const idCmdArgs& args )
{
	if( args.Argc() < 2 )
	{
		common->Printf( "mapGraphStart <graph> [seed]\n" );
		return;
	}
	mapGraphDirector.Start( args.Argv( 1 ), args.Argc() > 2 ? strtoull( args.Argv( 2 ), NULL, 0 ) : 0 );
}
void MapGraphDump_f( const idCmdArgs& args )
{
	if( args.Argc() != 4 )
	{
		common->Printf( "mapGraphDump <graph> <seed> <file>\n" );
		return;
	}
	const idDeclMapGraph* decl = FindGraph( args.Argv( 1 ) );
	idMapGraphDirector generated;
	const uint64 start = Sys_Microseconds();
	if( !decl || !generated.Generate( decl, strtoull( args.Argv( 2 ), NULL, 0 ) ) )
	{
		common->Warning( "graph generation failed" );
		return;
	}
	idFile* file = fileSystem->OpenFileWrite( args.Argv( 3 ) );
	generated.Dump( file );
	if( file )
	{
		fileSystem->CloseFile( file );
	}
	common->Printf( "generated %d nodes %d links %d sealed required exits in %.3f ms\n", generated.nodes.Num(), generated.links.Num(), generated.sealedRequired, ( Sys_Microseconds() - start ) * 0.001 );
}
void ListDungeon_f( const idCmdArgs& args )
{
	common->Printf( "dungeon %s node %d visits %d nodes %d links %d pending %d\n", mapGraphDirector.graphName.c_str(), mapGraphDirector.partyNode, mapGraphDirector.GetNodeVisits(), mapGraphDirector.nodes.Num(), mapGraphDirector.links.Num(), mapGraphDirector.pending.Num() );
#if !defined( ID_RETAIL )
	//puncho - P2b.1.10e: the hub flag cache's dev counters, since the director's Init: a decode a write at most
	common->Printf( "dungeon hub flags: writes %u decodes %u\n", idMapGraphDirector::hubWrites, idMapGraphDirector::hubDecodes );
#endif
}
void ListMapGraphs_f( const idCmdArgs& args )
{
	declManager->ListType( args, DECL_MAPGRAPH );
}
void SetDungeonFlag_f( const idCmdArgs& args )
{
	if( args.Argc() == 3 )
	{
		mapGraphDirector.SetFlag( args.Argv( 1 ), atoi( args.Argv( 2 ) ) );
	}
}
//puncho - P2.4 (bug review 2026-10-28), P2b.1.1a: the replay and recipe self-test and the portal-caster test are developer scaffolding, not in retail builds
#if !defined( ID_RETAIL )
void MapGraphTest_f( const idCmdArgs& args )
{
	if( args.Argc() < 2 )
	{
		common->Printf( "mapGraphTest <graph> [seeds=1000]\n" );
		return;
	}
	const idDeclMapGraph* decl = FindGraph( args.Argv( 1 ) );
	if( !decl )
	{
		common->Warning( "mapGraphTest: graph missing" );
		return;
	}
	const int seeds = args.Argc() > 2 ? idMath::ClampInt( 1, 10000, atoi( args.Argv( 2 ) ) ) : 1000;
	int failures = 0, truncations = 0;
	const uint64 start = Sys_Microseconds();
	for( int seed = 0; seed < seeds; seed++ )
	{
		idMapGraphDirector a, b, restored;
		idList<byte, TAG_GAME> first, second, roundTrip;
		if( !a.Generate( decl, seed ) || !b.Generate( decl, seed ) || !a.WriteRecord( first ) || !b.WriteRecord( second ) )
		{
			failures++;
			continue;
		}
		if( first.Num() != second.Num() || memcmp( first.Ptr(), second.Ptr(), first.Num() ) )
		{
			failures++;
		}
		if( !restored.ReadRecord( first.Ptr(), first.Num() ) || !restored.WriteRecord( roundTrip ) || first.Num() != roundTrip.Num() || memcmp( first.Ptr(), roundTrip.Ptr(), first.Num() ) )
		{
			failures++;
		}
		if( seed == 0 )
		{
			for( int length = 0; length < first.Num(); length++ )
			{
				if( restored.ReadRecord( first.Ptr(), length ) )
				{
					failures++;
				}
				truncations++;
			}
			second = first;
			second[0] = 255;
			if( restored.ReadRecord( second.Ptr(), second.Num() ) )
			{
				failures++;
			}
		}
		bool reachable[256] = { false };
		reachable[0] = a.nodes.Num() != 0;
		for( int pass = 0; pass < a.nodes.Num(); pass++ )
		{
			for( int link = 0; link < a.links.Num(); link++ )
			{
				const dungeonLink_t& l = a.links[link];
				if( l.to < a.nodes.Num() && reachable[l.from] )
				{
					reachable[l.to] = true;
				}
			}
		}
		for( int node = 0; node < a.nodes.Num(); node++ )
		{
			if( !decl->persistent && ( !reachable[node] || a.nodes[node].depth > decl->maxDepth ) )
			{
				failures++;
			}
		}
		for( int kind = 0; kind < decl->kinds.Num(); kind++ )
		{
			int count = 0;
			for( int node = 0; node < a.nodes.Num(); node++ )
			{
				count += a.nodes[node].kindIndex == kind;
			}
			if( !decl->persistent && count > decl->kinds[kind].cap )
			{
				failures++;
			}
		}
		// Flags affect resolution; the serialized topology dump must remain identical.
		idFile_Memory before, after;
		a.Dump( &before );
		a.flagValues[0] = 65535;
		a.Dump( &after );
		if( before.Length() != after.Length() || memcmp( before.GetDataPtr(), after.GetDataPtr(), before.Length() ) )
		{
			failures++;
		}
		if( seed == 0 && a.links.Num() )
		{
			a.links[0].conditionMode = 2;
			a.links[0].condFlag = 255;
			a.links[0].condValue = 65535;
			a.flagValues[255] = 65535;
			if( !a.WriteRecord( first ) || !restored.ReadRecord( first.Ptr(), first.Num() ) || restored.links[0].condFlag != 255 || restored.links[0].condValue != 65535 || restored.links[0].conditionMode != 2 )
			{
				failures++;
			}
		}
	}
	//puncho - P2b.1.4: the recipe's bytes written out (version 1, flags 1, instance 17, seed 12345, CRC 0x12345678, node 2,
	// visits 3, variant flags 0x8000000000000001, world "mg_test"), so the codec is checked against bytes it did not make
	byte recipeBytes[] =
	{
		1, 1,
		17, 0, 0, 0, 0, 0, 0, 0,
		0x39, 0x30, 0, 0, 0, 0, 0, 0,
		0x78, 0x56, 0x34, 0x12,
		2, 0,
		3, 0,
		1, 0, 0, 0, 0, 0, 0, 0x80,
		7, 'm', 'g', '_', 't', 'e', 's', 't'
	};
	const int recipeLength = sizeof( recipeBytes );
	idMapGraphDirector decoded;
	byte encoded[96];
	if( !decoded.SetMatchGameData( recipeBytes, recipeLength ) || decoded.WriteMatchGameData( "unused", encoded, sizeof( encoded ) ) != recipeLength || memcmp( encoded, recipeBytes, recipeLength ) )
	{
		failures++;
	}
	for( int length = 1; length < recipeLength; length++ )
	{
		if( decoded.SetMatchGameData( recipeBytes, length ) )
		{
			failures++;
		}
		truncations++;
	}
	recipeBytes[1] = 4;
	if( decoded.SetMatchGameData( recipeBytes, recipeLength ) )
	{
		failures++;
	}
	common->Printf( "mapGraphTest: %s seeds=%d truncations=%d failures=%d time=%.3fms\n", failures ? "FAIL" : "PASS", seeds, truncations, failures, ( Sys_Microseconds() - start ) * 0.001 );
}
void MapGraphPortalTest_f( const idCmdArgs& args )
{
	const int player = args.Argc() > 1 ? atoi( args.Argv( 1 ) ) : -1;
	if( player < 0 || player >= MAX_CLIENTS || !gameLocal.entities[player] )
	{
		common->Warning( "mapGraphPortalTest <live player slot>" );
		return;
	}
	const idVec3 origin = gameLocal.entities[player]->GetPhysics()->GetOrigin();
	mapGraphDirector.OpenTownPortalFor( gameLocal.entities[player] );
	const landmark_t* portal = gameLocal.mapLandmarks.FindLandmark( "_townportal" );
	const bool pass = portal && portal->origin == origin && gameLocal.FindEntity( "_dungeon_townportal" );
	common->Printf( "mapGraphPortalTest: %s caster=%d origin=%s\n", pass ? "PASS" : "FAIL", player, origin.ToString() );
}
#endif
