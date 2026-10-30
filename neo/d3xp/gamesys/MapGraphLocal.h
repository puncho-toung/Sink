#ifndef __GAME_MAPGRAPHLOCAL_H__
#define __GAME_MAPGRAPHLOCAL_H__

//puncho - P2b.1.1a: what MapGraph.cpp (parser, generator), MapGraphRecord.cpp (the dungeon record), MapGraphDirector.cpp
// (the director) and MapGraphCmds.cpp (its console commands) share

idStr GraphMapName( const char* name );
idStr GraphEffectMapName( const char* name );
bool GraphEffectMatches( const idStr& target, const idStr& graphMap );
const idDeclMapGraph* FindGraph( const char* name );

void MapGraphStart_f( const idCmdArgs& args );
void MapGraphDump_f( const idCmdArgs& args );
void ListDungeon_f( const idCmdArgs& args );
void ListMapGraphs_f( const idCmdArgs& args );
void SetDungeonFlag_f( const idCmdArgs& args );
#if !defined( ID_RETAIL )
void MapGraphTest_f( const idCmdArgs& args );
void MapGraphPortalTest_f( const idCmdArgs& args );
#endif

extern idCVar g_dungeonSeed;
#endif
