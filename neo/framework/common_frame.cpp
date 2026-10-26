/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2012-2016 Robert Beckebans
Copyright (C) 2014-2016 Kot in Action Creative Artel

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#include "precompiled.h"
#pragma hdrstop

#include "Common_local.h"
#include "Prefetch.h"
#include "../renderer/Image.h"

// RB begin
#if defined(USE_DOOMCLASSIC)
	#include "../../doomclassic/doom/doomlib.h"
	#include "../../doomclassic/doom/globaldata.h"
#endif
// RB end

/*

New for tech4x:

Unlike previous SMP work, the actual GPU command drawing is done in the main thread, which avoids the
OpenGL problems with needing windows to be created by the same thread that creates the context, as well
as the issues with passing context ownership back and forth on the 360.

The game tic and the generation of the draw command list is now run in a separate thread, and overlapped
with the interpretation of the previous draw command list.

While the game tic should be nicely contained, the draw command generation winds through the user interface
code, and is potentially hazardous.  For now, the overlap will be restricted to the renderer back end,
which should also be nicely contained.

*/
#define DEFAULT_FIXED_TIC "0"
#define DEFAULT_NO_SLEEP "0"

idCVar com_deltaTimeClamp( "com_deltaTimeClamp", "50", CVAR_INTEGER, "don't process more than this time in a single frame" );

idCVar com_fixedTic( "com_fixedTic", DEFAULT_FIXED_TIC, CVAR_BOOL, "run a single game frame per render frame" );
idCVar com_noSleep( "com_noSleep", DEFAULT_NO_SLEEP, CVAR_BOOL, "don't sleep if the game is running too fast" );
idCVar com_smp( "com_smp", "1", CVAR_BOOL | CVAR_SYSTEM | CVAR_NOCHEAT, "run the game and draw code in a separate thread" );
idCVar com_skipGameDraw( "com_skipGameDraw", "0", CVAR_SYSTEM | CVAR_BOOL, "" );

idCVar com_sleepGame( "com_sleepGame", "0", CVAR_SYSTEM | CVAR_INTEGER, "intentionally add a sleep in the game time" );
idCVar com_sleepDraw( "com_sleepDraw", "0", CVAR_SYSTEM | CVAR_INTEGER, "intentionally add a sleep in the draw time" );
idCVar com_sleepRender( "com_sleepRender", "0", CVAR_SYSTEM | CVAR_INTEGER, "intentionally add a sleep in the render time" );

idCVar net_drawDebugHud( "net_drawDebugHud", "0", CVAR_SYSTEM | CVAR_INTEGER, "0 = None, 1 = Hud 1, 2 = Hud 2, 3 = Snapshots" );

idCVar timescale( "timescale", "1", CVAR_SYSTEM | CVAR_FLOAT, "Number of game frames to run per render frame", 0.001f, 100.0f );


idCVar com_mapRowsLog( "com_mapRowsLog", "", CVAR_SYSTEM, "write per-frame MAP counters to maprows/<plain name>.csv" );
static idFile* mapRowsFile;
static char mapRowsName[128];
static char mapRowsBuffer[65536];
static int mapRowsUsed;
static int mapRowsFlushTime;
static unsigned int mapMainAllocations;

bool ValidDiagnosticName( const char* name )
{
	if( !name[0] || strlen( name ) >= 128 )
	{
		return false;
	}
	for( const char* p = name; *p; p++ )
	{
		if( !( ( *p >= 'a' && *p <= 'z' ) || ( *p >= 'A' && *p <= 'Z' ) ||
			( *p >= '0' && *p <= '9' ) || *p == '_' || *p == '-' ) ) { return false; }
	}
	return true;
}

void FormatMapRows( char rows[6][256], bool high[6] )
{
	renderMapStats_t r = {};
	mapGameStats_t g = {};
	renderSystem->GetMapStats( r );
	if( game != NULL )
	{
		game->GetMapStats( g );
	}
	const int cap = cvarSystem->GetCVarInteger( "net_maxSkipReplicatedStatics" );
	idStr::snPrintf( rows[0], 256, "MAP static vtx %llu/%llu idx %llu/%llu KB frame %llu/65536 KB", ( unsigned long long )( r.staticVertexBytes / 1024 ), ( unsigned long long )( r.staticVertexCapacity / 1024 ), ( unsigned long long )( r.staticIndexBytes / 1024 ), ( unsigned long long )( r.staticIndexCapacity / 1024 ), ( unsigned long long )( r.frameBytes / 1024 ) );
	idStr::snPrintf( rows[1], 256, "MAP handles entities %d/%d lights %d/%d probes %d/%d interactions %llu KB %dx%d", r.entityDefs, r.entityCapacity, r.lightDefs, r.lightCapacity, r.envprobeDefs, r.probeCapacity, ( unsigned long long )( r.interactionBytes / 1024 ), r.interactionWidth, r.interactionHeight );
	idStr::snPrintf( rows[2], 256, "MAP world areas %d portals %d/20800 pvs %llu KB %d ms lightgrid %llu KB", r.areas, r.portals, ( unsigned long long )( g.pvsBytes / 1024 ), g.pvsMilliseconds, ( unsigned long long )( r.lightGridBytes / 1024 ) );
	idStr::snPrintf( rows[3], 256, "MAP game entities %d/16384 active %d nonrep %d/256 cm %d/2048 statics %d/%d", g.entities, g.activeEntities, g.nonReplicated, g.cmModels, g.skipStatics, cap );
	idStr::snPrintf( rows[4], 256, "MAP clip/tic trans %u rot %u motion %u cont %u contacts %u gathers %u cand %u rmt %u us %u", g.clipTrans, g.clipRot, g.clipMotion, g.clipContents, g.clipContacts, g.clipGathers, g.clipCandidates, g.clipRmt, g.clipUs );
	idStr::snPrintf( rows[5], 256, "MAP alloc game %u/tic main %u/frame ent_hw %i refused %lld aasCache %lld KB evictions %lld saturated %lld", g.allocGameTic, mapMainAllocations, g.entityHighWater, ( long long )g.entityRefused, ( long long )( g.aasCacheBytes / 1024 ), ( long long )g.aasCacheEvictions, ( long long )g.aasTravelSaturated );
	if( g.recordMaps > 0 )
	{
		const int used = idStr::Length( rows[3] );
		idStr::snPrintf( rows[3] + used, 256 - used, " rec %d/%d/%d %dKB drop%d commit/apply %.2f/%.2fms corpse %d/%d", g.recordMaps, g.recordEntries, g.recordCreated, g.recordBytes / 1024, g.recordDropped, g.recordCommitUs * 0.001, g.recordApplyUs * 0.001, g.corpseLive, g.corpseEvicted );
	}
	high[0] = r.staticVertexBytes * 4 >= r.staticVertexCapacity * 3 || r.staticIndexBytes * 4 >= r.staticIndexCapacity * 3 || r.frameBytes >= 48 * 1024 * 1024;
	high[1] = ( r.entityCapacity > 0 && r.entityDefs * 4 >= r.entityCapacity * 3 ) || ( r.lightCapacity > 0 && r.lightDefs * 4 >= r.lightCapacity * 3 ) || ( r.probeCapacity > 0 && r.envprobeDefs * 4 >= r.probeCapacity * 3 );
	high[2] = r.portals >= 15600;
	high[3] = g.entities >= 12288 || g.nonReplicated >= 192 || g.cmModels >= 1536 || ( cap > 0 && g.skipStatics * 4 >= cap * 3 );
	high[4] = high[5] = false;
}

void idCommonLocal::CloseMapRowsLog()
{
	if( mapRowsFile != NULL )
	{
		mapRowsFile->Write( mapRowsBuffer, mapRowsUsed );
		fileSystem->CloseFile( mapRowsFile );
		mapRowsFile = NULL;
		Printf( "MAP CSV closed: %s\n", mapRowsName );
	}
	mapRowsName[0] = 0;
	mapRowsUsed = 0;
}

void idCommonLocal::LogMapRows( int tics )
{
	const char* name = com_mapRowsLog.GetString();
	if( idStr::Cmp( name, mapRowsName ) )
	{
		CloseMapRowsLog();
		if( name[0] )
		{
			if( !ValidDiagnosticName( name ) )
			{
				Warning( "com_mapRowsLog requires a plain name" );
				com_mapRowsLog.SetString( "" );
				return;
			}
			char path[160];
			idStr::snPrintf( path, sizeof( path ), "maprows/%s.csv", name );
			mapRowsFile = fileSystem->OpenFileWrite( path, "fs_savepath" );
			if( mapRowsFile == NULL )
			{
				com_mapRowsLog.SetString( "" ); return;
			}
			idStr::Copynz( mapRowsName, name, sizeof( mapRowsName ) );
			mapRowsFile->Printf( "frame,ms,tics,usFrameBusy,usFrameIdle,usGameRF,usGame,usRF,usRB,usGPU,usMOC,views,draws,tris,shadowViews,shadowDraws,shadowTris,atlasPct,viewEntities,shadowEntities,viewLights,staticVtxKB,staticIdxKB,frameKB,entityDefs,lightDefs,envprobeDefs,interactionKB,areas,portals,pvsKB,lightGridKB,entities,activeEntities,nonReplicated,cmModels,clipTrans,clipRot,clipMotion,clipContents,clipContacts,clipGathers,clipCandidates,clipUs,allocGameTic,allocMainFrame,probesConsidered,drawSurfs,drawSurfsClamped,atlasFailed,lightRefsWalked,contactedCapHits,skipStatics,usFindView,usAddLights,cullDistEntities,cullPixelEntities,cullLights,fadedLights,cullShadowSkips,lightRefsWalkedMax,gatherAreas,gatherMismatch,lodEntities1,lodEntities2,lodEntities3,lodTrisSaved,lodSwitches,lodThrash,lodCacheBuilds,mocOccluderTris,mocSkippedSize,mocSkippedBudget,mocSpotTests,mocSpotCulls,viewEntsUnder4px,viewEntsUnder16px,viewEntsBeyond8k,ent_highWater,ent_refused,spawnStaticsSkipped,aasCacheBytes,aasCacheEvictions,aasTravelSaturated,bud_debris_live,bud_debris_evicted,bud_debris_refused,bud_fx_live,bud_fx_evicted,bud_fx_refused,bud_item_live,bud_item_evicted,bud_item_refused,bud_gib_live,bud_gib_evicted,bud_gib_refused,itab_mode,itab_entries,itab_capacity,itab_maxProbe,itab_mismatches,itab_lateInserts,gai_ms,rec_maps,rec_entries,rec_created,rec_bytes,rec_dropped,rec_commitUs,rec_applyUs,corpse_live,corpse_evicted,dg_transitions,dg_refused,dg_inhibited,dg_prefetches\n" );
			mapRowsFlushTime = Sys_Milliseconds();
			Printf( "MAP CSV opened: %s\n", path );
		}
	}
	if( mapRowsFile == NULL )
	{
		return;
	}
	renderMapStats_t r = {};
	mapGameStats_t g = {};
	renderSystem->GetMapStats( r );
	if( game != NULL )
	{
		game->GetMapStats( g );
	}
	const int64 values[] = {
		idLib::frameNumber, Sys_Milliseconds(), tics,
		( int64 )frameTiming.finishSyncTime - ( int64 )mainFrameTiming.startGameTime,
		( int64 )mainFrameTiming.startGameTime - ( int64 )mainFrameTiming.finishSyncTime,
		( int64 )mainFrameTiming.finishDrawTime - ( int64 )mainFrameTiming.startGameTime,
		( int64 )mainFrameTiming.finishGameTime - ( int64 )mainFrameTiming.startGameTime,
		( int64 )mainFrameTiming.finishDrawTime - ( int64 )mainFrameTiming.finishGameTime,
		( int64 )GetRendererBackEndMicroseconds(), ( int64 )GetRendererGPUMicroseconds(), ( int64 )GetRendererMaskedOcclusionRasterizationMicroseconds(),
		stats_frontend.c_numViews, stats_backend.c_drawElements + stats_backend.c_shadowElements,
		( stats_backend.c_drawIndexes + stats_backend.c_shadowIndexes ) / 3,
		stats_backend.c_shadowViews, stats_backend.c_shadowElements, stats_backend.c_shadowIndexes / 3,
		( int64 )( 100.0 * stats_backend.c_shadowAtlasUsage / Square( r_shadowMapAtlasSize.GetInteger() ) ),
		stats_frontend.c_visibleViewEntities, stats_frontend.c_shadowViewEntities, stats_frontend.c_viewLights,
		( int64 )( r.staticVertexBytes / 1024 ), ( int64 )( r.staticIndexBytes / 1024 ), ( int64 )( r.frameBytes / 1024 ),
		r.entityDefs, r.lightDefs, r.envprobeDefs, ( int64 )( r.interactionBytes / 1024 ), r.areas, r.portals,
		( int64 )( g.pvsBytes / 1024 ), ( int64 )( r.lightGridBytes / 1024 ), g.entities, g.activeEntities, g.nonReplicated, g.cmModels,
		g.clipTrans, g.clipRot, g.clipMotion, g.clipContents, g.clipContacts, g.clipGathers, g.clipCandidates, g.clipUs, g.allocGameTic, mapMainAllocations,
		stats_frontend.c_probesConsidered, stats_frontend.c_drawSurfs, stats_frontend.c_drawSurfsClamped, stats_backend.c_shadowAtlasFailed,
		stats_frontend.c_lightRefsWalked, stats_frontend.c_contactedCapHits, g.skipStatics,
		( int64 )stats_frontend.usFindView, ( int64 )stats_frontend.usAddLights,
		stats_frontend.c_cullDistEntities,
		stats_frontend.c_cullPixelEntities,
		stats_frontend.c_cullLights,
		stats_frontend.c_fadedLights,
		stats_frontend.c_cullShadowSkips,
		stats_frontend.c_lightRefsWalkedMax,
		stats_frontend.c_gatherAreas,
		stats_frontend.c_gatherMismatch,
		stats_frontend.c_lodEntities1,
		stats_frontend.c_lodEntities2,
		stats_frontend.c_lodEntities3,
		stats_frontend.c_lodTrisSaved,
		stats_frontend.c_lodSwitches,
		stats_frontend.c_lodThrash,
		stats_frontend.c_lodCacheBuilds,
		stats_frontend.c_mocOccluderTris,
		stats_frontend.c_mocSkippedSize,
		stats_frontend.c_mocSkippedBudget,
		stats_frontend.c_mocSpotTests,
		stats_frontend.c_mocSpotCulls,
		stats_frontend.c_viewEntsUnder4px,
		stats_frontend.c_viewEntsUnder16px,
		stats_frontend.c_viewEntsBeyond8k,
		g.entityHighWater,
		g.entityRefused,
		g.spawnStaticsSkipped,
		g.aasCacheBytes,
		g.aasCacheEvictions,
		g.aasTravelSaturated,
		g.budgetLive[0],
		g.budgetEvicted[0],
		g.budgetRefused[0],
		g.budgetLive[1],
		g.budgetEvicted[1],
		g.budgetRefused[1],
		g.budgetLive[2],
		g.budgetEvicted[2],
		g.budgetRefused[2],
		g.budgetLive[3],
		g.budgetEvicted[3],
		g.budgetRefused[3],
		r.interactionMode,
		r.interactionEntries,
		r.interactionCapacity,
		r.interactionMaxProbe,
		r.interactionMismatches,
		r.interactionLateInserts,
		r.interactionGenerateMS,
		g.recordMaps, g.recordEntries, g.recordCreated, g.recordBytes, g.recordDropped,
		g.recordCommitUs, g.recordApplyUs, g.corpseLive, g.corpseEvicted,
		g.dungeonTransitions, g.dungeonRefused, g.dungeonInhibited, g.dungeonPrefetches
	};
	char row[2048];
	int length = 0;
	for( int i = 0; i < ( int )( sizeof( values ) / sizeof( values[0] ) ); i++ )
	{
		length += idStr::snPrintf( row + length, sizeof( row ) - length, "%s%lld", i ? "," : "", ( long long )values[i] );
	}
	row[length++] = '\n';
	if( mapRowsUsed + length > ( int )sizeof( mapRowsBuffer ) )
	{
		mapRowsFile->Write( mapRowsBuffer, mapRowsUsed );
		mapRowsUsed = 0;
	}
	memcpy( mapRowsBuffer + mapRowsUsed, row, length );
	mapRowsUsed += length;
	if( Sys_Milliseconds() - mapRowsFlushTime >= 1000 )
	{
		mapRowsFile->Write( mapRowsBuffer, mapRowsUsed );
		mapRowsFile->Flush();
		mapRowsUsed = 0;
		mapRowsFlushTime = Sys_Milliseconds();
	}
}

extern idCVar in_useJoystick;
extern idCVar in_joystickRumble;

/*
===============
idGameThread::Run

Run in a background thread for performance, but can also
be called directly in the foreground thread for comparison.
===============
*/
extern void RunPrintRelayTest();

int idGameThread::Run()
{
	RunPrintRelayTest();
	commonLocal.frameTiming.startGameTime = Sys_Microseconds();

	threadScratchStack = &commonLocal.gameScratchStack;	//puncho - MP-A4 (K27): the main thread's arena too; the two never run game code at once

	// debugging tool to test frame dropping behavior
	if( com_sleepGame.GetInteger() )
	{
		Sys_Sleep( com_sleepGame.GetInteger() );
	}

	if( numGameFrames == 0 )
	{
		// Ensure there's no stale gameReturn data from a paused game
		ret = gameReturn_t();
	}

	if( isClient )
	{
		// run the game logic
		for( int i = 0; i < numGameFrames; i++ )
		{
			SCOPED_PROFILE_EVENT( "Client Prediction" );
			if( userCmdMgr )
			{
				game->ClientRunFrame( *userCmdMgr, ( i == numGameFrames - 1 ), ret );
				game->FinishMapTicStats();
				commonLocal.gameScratchStack.EndTic();	//puncho - MP-A4 (K28)
			}
			if( ret.syncNextGameFrame || ret.sessionCommand[0] != 0 )
			{
				break;
			}
		}
	}
	else
	{
		// run the game logic
		for( int i = 0; i < numGameFrames; i++ )
		{
			SCOPED_PROFILE_EVENT( "GameTic" );
			if( userCmdMgr )
			{
				game->RunFrame( *userCmdMgr, ret );
				game->FinishMapTicStats();
				commonLocal.gameScratchStack.EndTic();	//puncho - MP-A4 (K28)
			}
			if( ret.syncNextGameFrame || ret.sessionCommand[0] != 0 )
			{
				break;
			}
		}
	}

	// we should have consumed all of our usercmds
	if( userCmdMgr )
	{
		// RB begin
#if defined(USE_DOOMCLASSIC)
		if( userCmdMgr->HasUserCmdForPlayer( game->GetLocalClientNum() ) && common->GetCurrentGame() == DOOM3_BFG )
#else
		if( userCmdMgr->HasUserCmdForPlayer( game->GetLocalClientNum() ) )
#endif
			// RB end
		{
			idLib::Printf( "idGameThread::Run: didn't consume all usercmds\n" );
		}
	}

	commonLocal.frameTiming.finishGameTime = Sys_Microseconds();

	SetThreadGameTime( ( commonLocal.frameTiming.finishGameTime - commonLocal.frameTiming.startGameTime ) / 1000 );

	// build render commands and geometry
	{
		SCOPED_PROFILE_EVENT( "Draw" );
		commonLocal.Draw();
	}

	commonLocal.frameTiming.finishDrawTime = Sys_Microseconds();

	SetThreadRenderTime( ( commonLocal.frameTiming.finishDrawTime - commonLocal.frameTiming.finishGameTime ) / 1000 );

	SetThreadTotalTime( ( commonLocal.frameTiming.finishDrawTime - commonLocal.frameTiming.startGameTime ) / 1000 );

	return 0;
}

/*
===============
idGameThread::RunGameAndDraw

===============
*/
gameReturn_t idGameThread::RunGameAndDraw( int numGameFrames_, idUserCmdMgr& userCmdMgr_, bool isClient_, int startGameFrame )
{
	// this should always immediately return
	this->WaitForThread();

	// save the usercmds for the background thread to pick up
	userCmdMgr = &userCmdMgr_;

	isClient = isClient_;

	// grab the return value created by the last thread execution
	gameReturn_t latchedRet = ret;

	numGameFrames = numGameFrames_;

	// start the thread going
	// foresthale 2014-05-12: also check com_editors as many of them are not particularly thread-safe (editLights for example)
	if( !com_smp.GetBool() || com_editors != 0 )
	{
		// run it in the main thread so PIX profiling catches everything
		Run();
	}
	else
	{
		this->SignalWork();
	}

	// return the latched result while the thread runs in the background
	return latchedRet;
}


/*
===============
idCommonLocal::DrawWipeModel

Draw the fade material over everything that has been drawn
===============
*/
void idCommonLocal::DrawWipeModel()
{

	if( wipeStartTime >= wipeStopTime )
	{
		return;
	}

	int currentTime = Sys_Milliseconds();

	if( !wipeHold && currentTime > wipeStopTime )
	{
		return;
	}

	float fade = ( float )( currentTime - wipeStartTime ) / ( wipeStopTime - wipeStartTime );
	renderSystem->SetColor4( 1, 1, 1, fade );
	renderSystem->DrawStretchPic( 0, 0, renderSystem->GetVirtualWidth(), renderSystem->GetVirtualHeight(), 0, 0, 1, 1, wipeMaterial );
}

// RB begin
void idCommonLocal::DrawLoadPacifierProgressbar()
{
	if( loadPacifierExpectedCount <= 0 )
	{
		return;
	}

	float loadPacifierProgress = float( loadPacifierCount ) / loadPacifierExpectedCount;

	// draw our basic overlay
	renderSystem->SetColor( idVec4( 0.55f, 0.0f, 0.0f, 1.0f ) );
	renderSystem->DrawStretchPic( 0, renderSystem->GetVirtualHeight() - 64, renderSystem->GetVirtualWidth(), 16, 0, 0, 1, 1, whiteMaterial );
	//renderSystem->SetColor( idVec4( 0.0f, 0.5f, 0.8f, 1.0f ) );
	renderSystem->SetColor( colorGold );
	renderSystem->DrawStretchPic( 0, renderSystem->GetVirtualHeight() - 64, loadPacifierProgress * renderSystem->GetVirtualWidth(), 16, 0, 0, 1, 1, whiteMaterial );

	renderSystem->DrawSmallStringExt( 0, renderSystem->GetVirtualHeight() - 64, loadPacifierStatus, idVec4( 1.0f, 1.0f, 1.0f, 1.0f ), true );
}
// RB end

/*
===============
idCommonLocal::Draw
===============
*/
void idCommonLocal::Draw()
{
	// debugging tool to test frame dropping behavior
	if( com_sleepDraw.GetInteger() )
	{
		Sys_Sleep( com_sleepDraw.GetInteger() );
	}

	extern idCVar com_transitionOverlayMs;
	if( insideExecuteMapChange && IsGameDrivenLoad() && transitionLoadStartTime != 0 &&
		com_transitionOverlayMs.GetInteger() >= 0 && Sys_Milliseconds() - transitionLoadStartTime < com_transitionOverlayMs.GetInteger() )
	{
		renderSystem->SetColor( colorBlack );
		renderSystem->DrawStretchPic( 0, 0, renderSystem->GetVirtualWidth(), renderSystem->GetVirtualHeight(), 0, 0, 1, 1, whiteMaterial );
	}
	else if( loadPacifierBinarizeActive || LoadPacifierRunning() )
	{
		// foresthale 2014-05-30: when binarizing an asset we show a special
		// overlay indicating progress
		renderSystem->SetColor( colorBlack );
		renderSystem->DrawStretchPic( 0, 0, renderSystem->GetVirtualWidth(), renderSystem->GetVirtualHeight(), 0, 0, 1, 1, whiteMaterial );

		// render the loading gui (idSWF actually) if it is loaded
		// (we want to see progress of the loading gui binarize too)
		if( loadGUI != NULL )
		{
			loadGUI->Render( renderSystem, Sys_Milliseconds() );
		transitionGuiDraws++;
		}

		// draw general progress bar
		DrawLoadPacifierProgressbar();

		if( loadPacifierBinarizeActive )
		{
			// update our progress estimates
			int time = Sys_Milliseconds();
			if( loadPacifierBinarizeProgress > 0.0f )
			{
				loadPacifierBinarizeTimeLeft = ( 1.0 - loadPacifierBinarizeProgress ) * ( time - loadPacifierBinarizeStartTime ) * 0.001f / loadPacifierBinarizeProgress;
			}
			else
			{
				loadPacifierBinarizeTimeLeft = -1.0f;
			}

			// prepare our strings
			idStr text;
			if( loadPacifierBinarizeTimeLeft >= 99.5f )
			{
				text = va( "Binarizing %3.0f%% ETA %2.0f minutes", loadPacifierBinarizeProgress * 100.0f, loadPacifierBinarizeTimeLeft / 60.0f );
			}
			else if( loadPacifierBinarizeTimeLeft )
			{
				text = va( "Binarizing %3.0f%% ETA %2.0f seconds", loadPacifierBinarizeProgress * 100.0f, loadPacifierBinarizeTimeLeft );
			}
			else
			{
				text = va( "Binarizing %3.0f%%", loadPacifierBinarizeProgress * 100.0f );
			}

			// draw our basic overlay
			renderSystem->SetColor( idVec4( 0.0f, 0.0f, 0.0f, 0.75f ) );
			renderSystem->DrawStretchPic( 0, renderSystem->GetVirtualHeight() - 48, renderSystem->GetVirtualWidth(), 48, 0, 0, 1, 1, whiteMaterial );
			//renderSystem->SetColor( idVec4( 0.0f, 0.5f, 0.8f, 1.0f ) );
			renderSystem->SetColor( colorBrown );
			renderSystem->DrawStretchPic( 0, renderSystem->GetVirtualHeight() - 48, loadPacifierBinarizeProgress * renderSystem->GetVirtualWidth(), 16, 0, 0, 1, 1, whiteMaterial );
			renderSystem->DrawSmallStringExt( 0, renderSystem->GetVirtualHeight() - 48, loadPacifierBinarizeFilename.c_str(), idVec4( 1.0f, 1.0f, 1.0f, 1.0f ), true );
			renderSystem->DrawSmallStringExt( 0, renderSystem->GetVirtualHeight() - 32, va( "%s %d/%d lvls", loadPacifierBinarizeInfo.c_str(), loadPacifierBinarizeMiplevel, loadPacifierBinarizeMiplevelTotal ), idVec4( 1.0f, 1.0f, 1.0f, 1.0f ), true );
			renderSystem->DrawSmallStringExt( 0, renderSystem->GetVirtualHeight() - 16, text, idVec4( 1.0f, 1.0f, 1.0f, 1.0f ), true );
		}
	}
	else if( loadGUI != NULL )
	{
		// foresthale 2014-05-30: showing a black background looks better than flickering in widescreen
		renderSystem->SetColor( colorBlack );
		renderSystem->DrawStretchPic( 0, 0, renderSystem->GetVirtualWidth(), renderSystem->GetVirtualHeight(), 0, 0, 1, 1, whiteMaterial );

		loadGUI->Render( renderSystem, Sys_Milliseconds() );
		transitionGuiDraws++;
	}
	// RB begin
#if defined(USE_DOOMCLASSIC)
	else if( currentGame == DOOM_CLASSIC || currentGame == DOOM2_CLASSIC )
	{
		const float sysWidth = renderSystem->GetWidth() * renderSystem->GetPixelAspect();
		const float sysHeight = renderSystem->GetHeight();
		const float sysAspect = sysWidth / sysHeight;
		const float doomAspect = 4.0f / 3.0f;
		const float adjustment = sysAspect / doomAspect;
		const float barHeight = ( adjustment >= 1.0f ) ? 0.0f : ( 1.0f - adjustment ) * ( float )renderSystem->GetVirtualHeight() * 0.25f;
		const float barWidth = ( adjustment <= 1.0f ) ? 0.0f : ( adjustment - 1.0f ) * ( float )renderSystem->GetVirtualWidth() * 0.25f;
		if( barHeight > 0.0f )
		{
			renderSystem->SetColor( colorBlack );
			renderSystem->DrawStretchPic( 0, 0, renderSystem->GetVirtualWidth(), barHeight, 0, 0, 1, 1, whiteMaterial );
			renderSystem->DrawStretchPic( 0, renderSystem->GetVirtualHeight() - barHeight, renderSystem->GetVirtualWidth(), barHeight, 0, 0, 1, 1, whiteMaterial );
		}
		if( barWidth > 0.0f )
		{
			renderSystem->SetColor( colorBlack );
			renderSystem->DrawStretchPic( 0, 0, barWidth, renderSystem->GetVirtualHeight(), 0, 0, 1, 1, whiteMaterial );
			renderSystem->DrawStretchPic( renderSystem->GetVirtualWidth() - barWidth, 0, barWidth, renderSystem->GetVirtualHeight(), 0, 0, 1, 1, whiteMaterial );
		}
		renderSystem->SetColor4( 1, 1, 1, 1 );
		renderSystem->DrawStretchPic( barWidth, barHeight, renderSystem->GetVirtualWidth() - barWidth * 2.0f, renderSystem->GetVirtualHeight() - barHeight * 2.0f, 0, 0, 1, 1, doomClassicMaterial );
	}
#endif
	// RB end
	else if( game && game->Shell_IsActive() )
	{
		bool gameDraw = game->Draw( game->GetLocalClientNum() );
		if( gameDraw )
		{
			RecordTransitionDraw();
		}
		if( !gameDraw )
		{
			renderSystem->SetColor( colorBlack );
			renderSystem->DrawStretchPic( 0, 0, renderSystem->GetVirtualWidth(), renderSystem->GetVirtualHeight(), 0, 0, 1, 1, whiteMaterial );
		}
		game->Shell_Render();
	}
	else if( mapSpawned )
	{
		bool gameDraw = false;

		// D20 / plan 3.9: the light editor's per-frame tool logic, after the player has moved and before
		// the scene is drawn; latched inside on idLib::frameNumber, as Draw() runs more than once a
		// frame, and never from an out-of-sequence UpdateScreen redraw that comes before the tics (EA-10)
		if( ( com_editors & EDITOR_LIGHT ) && !insideUpdateScreen )
		{
			ImGuiTools::RunToolFrame();
		}

		// normal drawing for both single and multi player
		if( !com_skipGameDraw.GetBool() && Game()->GetLocalClientNum() >= 0 )
		{
			// draw the game view
			int	start = Sys_Milliseconds();
			if( game )
			{
				gameDraw = game->Draw( Game()->GetLocalClientNum() );
				if( gameDraw )
				{
					RecordTransitionDraw();
				}
			}
			int end = Sys_Milliseconds();
			time_gameDraw += ( end - start );	// note time used for com_speeds
		}
		if( !gameDraw )
		{
			renderSystem->SetColor( colorBlack );
			renderSystem->DrawStretchPic( 0, 0, renderSystem->GetVirtualWidth(), renderSystem->GetVirtualHeight(), 0, 0, 1, 1, whiteMaterial );
		}
	}
	else
	{
		renderSystem->SetColor4( 0, 0, 0, 1 );
		renderSystem->DrawStretchPic( 0, 0, renderSystem->GetVirtualWidth(), renderSystem->GetVirtualHeight(), 0, 0, 1, 1, whiteMaterial );
	}

	{
		SCOPED_PROFILE_EVENT( "Post-Draw" );

		// draw Imgui before the console
		ImGuiHook::Render();

		// draw the wipe material on top of this if it hasn't completed yet
		DrawWipeModel();

		Dialog().Render( loadGUI != NULL );

		// draw the half console / notify console on top of everything
		console->Draw( false );

		// old CRT TV simulation has to be last or it breaks the immersion
		renderSystem->DrawCRTPostFX();
	}
}

/*
===============
idCommonLocal::UpdateScreen

This is an out-of-sequence screen update, not the normal game rendering
===============
*/
// DG: added possibility to *not* release mouse in UpdateScreen(), it fucks up the view angle for screenshots
void idCommonLocal::UpdateScreen( bool captureToImage, bool releaseMouse )
{
	if( insideUpdateScreen || com_shuttingDown )
	{
		return;
	}
	insideUpdateScreen = true;

	// make sure the game / draw thread has completed
	gameThread.WaitForThread();
	DrainPrintRelay();

	// release the mouse capture back to the desktop
	if( releaseMouse )
	{
		Sys_GrabMouseCursor( false );
	}
	// DG end

	// build all the draw commands without running a new game tic
	Draw();
	frameTiming.finishDrawTime = Sys_Microseconds();    // SRS - Added frame timing for out-of-sequence updates (e.g. used in timedemo "twice" mode)

	// foresthale 2014-03-01: note: the only place that has captureToImage=true is idAutoRender::StartBackgroundAutoSwaps
	if( captureToImage )
	{
		renderSystem->CaptureRenderToImage( "_currentRender", false );
	}

	// this should exit right after vsync, with the GPU idle and ready to draw
	frameTiming.startRenderTime = Sys_Microseconds();   // SRS - Added frame timing for out-of-sequence updates (e.g. used in timedemo "twice" mode)
	const emptyCommand_t* cmd = renderSystem->SwapCommandBuffers( &time_frontend, &time_backend, &time_moc, &time_gpu, &stats_backend, &stats_frontend );

	// get the GPU busy with new commands
	renderSystem->RenderCommandBuffers( cmd );
	frameTiming.finishRenderTime = Sys_Microseconds();  // SRS - Added frame timing for out-of-sequence updates (e.g. used in timedemo "twice" mode)

	insideUpdateScreen = false;
}
/*
================
idCommonLocal::ProcessGameReturn
================
*/
void idCommonLocal::ProcessGameReturn( const gameReturn_t& ret )
{
	// set joystick rumble
	if( in_useJoystick.GetBool() && in_joystickRumble.GetBool() && !game->Shell_IsActive() && session->GetSignInManager().GetMasterInputDevice() >= 0 )
	{
		Sys_SetRumble( session->GetSignInManager().GetMasterInputDevice(), ret.vibrationLow, ret.vibrationHigh );		// Only set the rumble on the active controller
	}
	else
	{
		for( int i = 0; i < MAX_INPUT_DEVICES; i++ )
		{
			Sys_SetRumble( i, 0, 0 );
		}
	}

	syncNextGameFrame = ret.syncNextGameFrame;

	if( ret.sessionCommand[0] )
	{
		idCmdArgs args;

		args.TokenizeString( ret.sessionCommand, false );
		const bool transitionCommand = !idStr::Icmp( args.Argv( 0 ), "map" ) || !idStr::Icmp( args.Argv( 0 ), "transition" );
		if( transitionCommand && IsMultiplayer() && session->GetActingGameStateLobbyBase().HasActivePeers() && cvarSystem->GetCVarBool( "net_coopTransitions" ) )
		{
			if( !BeginCoopTransition( args.Argv( 1 ) ) )
			{
				Warning( "co-op transition request refused" );
			}
			return;
		}

		if( cvarSystem->GetCVarBool( "net_refuseMapWithPeers" ) && IsMultiplayer() &&
			session->GetActivePlatformLobbyBase().HasActivePeers() &&
			( !idStr::Icmp( args.Argv( 0 ), "map" ) || !idStr::Icmp( args.Argv( 0 ), "transition" ) || !idStr::Icmp( args.Argv( 0 ), "devmap" ) ) )
		{
			Warning( "map change to '%s' refused: peers in the session (co-op transitions are P2.3.5)", args.Argv( 1 ) );
			return;
		}


		if( !idStr::Icmp( args.Argv( 0 ), "map" ) || !idStr::Icmp( args.Argv( 0 ), "transition" ) )
		{
			BeginTransitionTimeline( args.Argv( 0 ) );
			MoveToNewMap( args.Argv( 1 ), false );
		}
		else if( !idStr::Icmp( args.Argv( 0 ), "devmap" ) )
		{
			MoveToNewMap( args.Argv( 1 ), true );
		}
		else if( !idStr::Icmp( args.Argv( 0 ), "died" ) )
		{
			if( !IsMultiplayer() )
			{
				game->Shell_Show( true );
			}
		}
		else if( !idStr::Icmp( args.Argv( 0 ), "disconnect" ) )
		{
			cmdSystem->BufferCommandText( CMD_EXEC_INSERT, "stoprecording ; disconnect" );
		}
		else if( !idStr::Icmp( args.Argv( 0 ), "endOfDemo" ) )
		{
			cmdSystem->BufferCommandText( CMD_EXEC_NOW, "endOfDemo" );
		}
	}
}

extern idCVar com_forceGenericSIMD;

extern idCVar com_pause;
extern idCVar com_activeApp;

/*
=================
idCommonLocal::Frame
=================
*/
void idCommonLocal::Frame()
{
	const unsigned int frameAllocStart = Mem_GetAllocationCount();
	try
	{
		SCOPED_PROFILE_EVENT( "Common::Frame" );

		// This is the only place this is incremented
		idLib::frameNumber++;

		//OPTICK_TAG( "N", idLib::frameNumber );

		// allow changing SIMD usage on the fly
		if( com_forceGenericSIMD.IsModified() )
		{
			idSIMD::InitProcessor( "doom", com_forceGenericSIMD.GetBool() );
			com_forceGenericSIMD.ClearModified();
		}

		// RB begin
#if defined(USE_DOOMCLASSIC)
		// Do the actual switch between Doom 3 and the classics here so
		// that things don't get confused in the middle of the frame.
		PerformGameSwitch();
#endif
		// RB end

		// pump all the events
		Sys_GenerateEvents();

		// write config file if anything changed
		WriteConfiguration();

		eventLoop->RunEventLoop();

		renderSystem->OnFrame();

		// DG: prepare new ImGui frame - I guess this is a good place, as all new events should be available?
		ImGuiHook::NewFrame();

		// Activate the shell if it's been requested
		if( showShellRequested && game )
		{
			game->Shell_Show( true );
			showShellRequested = false;
		}

		// if the console or another gui is down, we don't need to hold the mouse cursor
		bool chatting = false;

		// DG: Add pause from com_pause cvar
		// RB begin
#if defined(USE_DOOMCLASSIC)
		if( com_pause.GetInteger() || console->Active() || Dialog().IsDialogActive() || session->IsSystemUIShowing()
				|| ( game && game->InhibitControls() && !IsPlayingDoomClassic() ) || ImGuiTools::ReleaseMouseForTools() )
#else
		if( com_pause.GetInteger() || console->Active() || Dialog().IsDialogActive() || session->IsSystemUIShowing()
				|| ( game && game->InhibitControls() ) ||  ImGuiTools::ReleaseMouseForTools() )
#endif
			// RB end, DG end
		{
			// RB: don't release the mouse when opening a PDA or menu
			// SRS - but always release at main menu after exiting game or demo
			if( console->Active() || !mapSpawned || ImGuiTools::ReleaseMouseForTools() )
			{
				Sys_GrabMouseCursor( false );
			}
			usercmdGen->InhibitUsercmd( INHIBIT_SESSION, true );
			chatting = true;
		}
		else
		{
			Sys_GrabMouseCursor( true );
			usercmdGen->InhibitUsercmd( INHIBIT_SESSION, false );
		}

		// RB begin
#if defined(USE_DOOMCLASSIC)
		const bool pauseGame = ( !mapSpawned
								 || ( !IsMultiplayer()
									  && ( Dialog().IsDialogPausing() || session->IsSystemUIShowing()
										   || ( game && game->Shell_IsActive() ) || com_pause.GetInteger() ) ) )
							   && !IsPlayingDoomClassic();
#else
		const bool pauseGame = ( !mapSpawned
								 || ( !IsMultiplayer()
									  && ( Dialog().IsDialogPausing() || session->IsSystemUIShowing()
										   || ( game && game->Shell_IsActive() ) || com_pause.GetInteger() ) ) );
#endif
		// RB end

		//--------------------------------------------
		// wait for the GPU to finish drawing
		//
		// It is imporant to minimize the time spent between this
		// section and the call to renderSystem->RenderCommandBuffers(),
		// because the GPU is completely idle.
		//--------------------------------------------
		// this should exit right after vsync, with the GPU idle and ready to draw
		// This may block if the GPU isn't finished renderng the previous frame.
		frameTiming.startSyncTime = Sys_Microseconds();
		const emptyCommand_t* renderCommands = NULL;

		// foresthale 2014-05-12: also check com_editors as many of them are not particularly thread-safe (editLights for example)
		if( com_smp.GetBool() && com_editors == 0 )
		{
			renderCommands = renderSystem->SwapCommandBuffers( &time_frontend, &time_backend, &time_moc, &time_gpu, &stats_backend, &stats_frontend );
		}
		else
		{
			// the GPU will stay idle through command generation for minimal
			// input latency
			renderSystem->SwapCommandBuffers_FinishRendering( &time_frontend, &time_backend, &time_moc, &time_gpu, &stats_backend, &stats_frontend );
		}
		frameTiming.finishSyncTime = Sys_Microseconds();

		// RB: slow down engine in background so it does not eat up so many resources along other 3D tools
		if( !com_activeApp.GetBool() && !IsServer() /* and not VR */ )
		{
			const float backgroundEngineHz = 15.0f;
			com_engineHz_denominator = 100LL * backgroundEngineHz;
			com_engineHz_latched = backgroundEngineHz;
		}
#if defined(USE_DOOMCLASSIC)
		else if( IsPlayingDoomClassic() )
		{
			com_engineHz_denominator = 100LL * DOOM_CLASSIC_HZ;
			com_engineHz_latched = DOOM_CLASSIC_HZ;
		}
#endif
		else
		{
			// allow com_engineHz to be changed between map loads
			com_engineHz_denominator = 100LL * com_engineHz.GetFloat();
			com_engineHz_latched = com_engineHz.GetFloat();
		}
		// RB end

		//--------------------------------------------
		// Determine how many game tics we are going to run,
		// now that the previous frame is completely finished.
		//
		// It is important that any waiting on the GPU be done
		// before this, or there will be a bad stuttering when
		// dropping frames for performance management.
		//--------------------------------------------

		// input:
		// thisFrameTime
		// com_noSleep
		// com_engineHz
		// com_fixedTic
		// com_deltaTimeClamp
		// IsMultiplayer
		//
		// in/out state:
		// gameFrame
		// gameTimeResidual
		// lastFrameTime
		// syncNextFrame
		//
		// Output:
		// numGameFrames

		// How many game frames to run
		int numGameFrames = 0;

		{

			OPTICK_CATEGORY( "Wait for Frame", Optick::Category::Wait );

			for( ;; )
			{
				const int thisFrameTime = Sys_Milliseconds();
				static int lastFrameTime = thisFrameTime;	// initialized only the first time
				const int deltaMilliseconds = thisFrameTime - lastFrameTime;
				lastFrameTime = thisFrameTime;

				// if there was a large gap in time since the last frame, or the frame
				// rate is very very low, limit the number of frames we will run
				const int clampedDeltaMilliseconds = Min( deltaMilliseconds, com_deltaTimeClamp.GetInteger() );

				gameTimeResidual += clampedDeltaMilliseconds * timescale.GetFloat();

				// don't run any frames when paused
				/*
				RB moved down

				if( pauseGame )
				{
					gameFrame++;
					gameTimeResidual = 0;
					break;
				}
				*/

				// debug cvar to force multiple game tics
				if( com_fixedTic.GetInteger() > 0 )
				{
					numGameFrames = com_fixedTic.GetInteger();
					gameFrame += numGameFrames;
					gameTimeResidual = 0;
					break;
				}

				if( syncNextGameFrame )
				{
					// don't sleep at all
					syncNextGameFrame = false;
					gameFrame++;
					numGameFrames++;
					gameTimeResidual = 0;
					break;
				}

				for( ;; )
				{
					// How much time to wait before running the next frame,
					// based on com_engineHz
					const int frameDelay = FRAME_TO_MSEC( gameFrame + 1 ) - FRAME_TO_MSEC( gameFrame );
					if( gameTimeResidual < frameDelay )
					{
						break;
					}
					gameTimeResidual -= frameDelay;
					gameFrame++;
					numGameFrames++;
					// if there is enough residual left, we may run additional frames
				}

				if( numGameFrames > 0 )
				{
					// ready to actually run them
					break;
				}

				// if we are vsyncing, we always want to run at least one game
				// frame and never sleep, which might happen due to scheduling issues
				// if we were just looking at real time.
				if( com_noSleep.GetBool() )
				{
					numGameFrames = 1;
					gameFrame += numGameFrames;
					gameTimeResidual = 0;
					break;
				}

				// not enough time has passed to run a frame, as might happen if
				// we don't have vsync on, or the monitor is running at 120hz while
				// com_engineHz is 60, so sleep a bit and check again
				Sys_Sleep( 0 );
			}
		}

		// don't run any frames when paused
		// RB: reset numGameFrames here so we use the sleep above
		// and don't run as many frames as possible on the GPU
		if( pauseGame )
		{
			numGameFrames = 0;
		}

		//--------------------------------------------
		// It would be better to push as much of this as possible
		// either before or after the renderSystem->SwapCommandBuffers(),
		// because the GPU is completely idle.
		//--------------------------------------------

		// Update session and syncronize to the new session state after sleeping
		session->UpdateSignInManager();
		session->Pump();
		session->ProcessSnapAckQueue();
		const bool hadMapBeforeTransitionPump = mapSpawned;
		PumpMapTransition();
		if( hadMapBeforeTransitionPump && !mapSpawned )
		{
			// A timeout can LeaveGame here. Its UpdateScreen advances frame buffers,
			// invalidating renderCommands and the tic count captured earlier this frame.
			return;
		}

		if( session->GetState() == idSession::LOADING )
		{
			// If the session reports we should be loading a map, load it!
			ExecuteMapChange();
			mapSpawnData.savegameFile = NULL;
			mapSpawnData.persistentPlayerInfo.Clear();
			mapSpawnData.worldTimeBase = 0;
			if( transitionState == 0 )
			{
				mapSpawnData.gameDriven = false;
			}
			return;
		}
		else if( session->GetState() != idSession::INGAME && mapSpawned && transitionState == 0 )
		{
			// If the game is running, but the session reports we are not in a game, disconnect
			// This happens when a server disconnects us or we sign out
			LeaveGame();
			return;
		}

		if( mapSpawned && !pauseGame )
		{
			if( IsClient() )
			{
				RunNetworkSnapshotFrame();
			}
		}

		ExecuteReliableMessages();
		if( transitionState != 0 )
		{
			numGameFrames = 0;
			gameTimeResidual = 0;
		}

		// send frame and mouse events to active guis
		GuiFrameEvents();

		//--------------------------------------------
		// Prepare usercmds and kick off the game processing
		// in a background thread
		//--------------------------------------------

		// get the previous usercmd for bypassed head tracking transform
		const usercmd_t	previousCmd = usercmdGen->GetCurrentUsercmd();

		// build a new usercmd
		int deviceNum = session->GetSignInManager().GetMasterInputDevice();
		usercmdGen->BuildCurrentUsercmd( deviceNum );
		if( deviceNum == -1 )
		{
			for( int i = 0; i < MAX_INPUT_DEVICES; i++ )
			{
				Sys_PollJoystickInputEvents( i );
				Sys_EndJoystickInputEvents();
			}
		}
		if( pauseGame )
		{
			usercmdGen->Clear();
		}

		usercmd_t newCmd = usercmdGen->GetCurrentUsercmd();

		// Store server game time - don't let time go past last SS time in case we are extrapolating
		if( IsClient() )
		{
			newCmd.serverGameMilliseconds = Min( Game()->GetServerGameTimeMs(), Game()->GetSSEndTime() );
		}
		else
		{
			newCmd.serverGameMilliseconds = Game()->GetServerGameTimeMs();
		}

		userCmdMgr.MakeReadPtrCurrentForPlayer( Game()->GetLocalClientNum() );

		// Stuff a copy of this userCmd for each game frame we are going to run.
		// Ideally, the usercmds would be built in another thread so you could
		// still get 60hz control accuracy when the game is running slower.
		for( int i = 0 ; i < numGameFrames ; i++ )
		{
			newCmd.clientGameMilliseconds = FRAME_TO_MSEC( gameFrame - numGameFrames + i + 1 );
			userCmdMgr.PutUserCmdForPlayer( game->GetLocalClientNum(), newCmd );
		}

		// RB begin
#if defined(USE_DOOMCLASSIC)
		// If we're in Doom or Doom 2, run tics and upload the new texture.
		// SRS - Add check for com_pause cvar to make sure window is in focus - if not classic game should be paused
		if( ( GetCurrentGame() == DOOM_CLASSIC || GetCurrentGame() == DOOM2_CLASSIC ) && !( Dialog().IsDialogPausing() || session->IsSystemUIShowing() || com_pause.GetInteger() ) )
		{
			RunDoomClassicFrame();
		}
#endif
		// RB end

		// start the game / draw command generation thread going in the background
		gameReturn_t ret = gameThread.RunGameAndDraw( numGameFrames, userCmdMgr, IsClient(), gameFrame - numGameFrames );

		frameTiming.startRenderTime = Sys_Microseconds();

		// foresthale 2014-05-12: also check com_editors as many of them are not particularly thread-safe (editLights for example)
		if( !com_smp.GetBool() || com_editors != 0 )
		{
			// in non-smp mode, run the commands we just generated, instead of
			// frame-delayed ones from a background thread
			renderCommands = renderSystem->SwapCommandBuffers_FinishCommandBuffers();
		}

		//----------------------------------------
		// Run the render back end, getting the GPU busy with new commands
		// ASAP to minimize the pipeline bubble.
		//----------------------------------------
		renderSystem->RenderCommandBuffers( renderCommands );
		if( com_sleepRender.GetInteger() > 0 )
		{
			// debug tool to test frame adaption
			Sys_Sleep( com_sleepRender.GetInteger() );
		}
		frameTiming.finishRenderTime = Sys_Microseconds();

		// make sure the game / draw thread has completed
		// This may block if the game is taking longer than the render back end
		gameThread.WaitForThread();
		DrainPrintRelay();
		FlushTransitionTimeline();
		Prefetch_Pump();
		if( mapSpawned )
		{
			renderWorld->FinishInteractionSlice();
		}

		// SRS - Use finishSyncTime_EndFrame to record timing just after gameThread.WaitForThread()
		frameTiming.finishSyncTime_EndFrame = Sys_Microseconds();

		// Send local usermds to the server.
		// This happens after the game frame has run so that prediction data is up to date.
		SendUsercmds( Game()->GetLocalClientNum() );

		// Now that we have an updated game frame, we can send out new snapshots to our clients
		session->Pump(); // Pump to get updated usercmds to relay
		if( transitionState != 1 && transitionState != 3 )
		{
			SendSnapshots();
		}

		// Render the sound system using the latest commands from the game thread
		if( pauseGame )
		{
			soundWorld->Pause();
			soundSystem->SetPlayingSoundWorld( menuSoundWorld );
			soundSystem->SetMute( false );
		}
		else
		{
			soundWorld->UnPause();
			soundSystem->SetPlayingSoundWorld( soundWorld );
			soundSystem->SetMute( false );
		}
		// SRS - Mute all sound output when dialog waiting or window not in focus (mutes Doom3, Classic, Cinematic Audio)
		if( Dialog().IsDialogPausing() || session->IsSystemUIShowing() || com_pause.GetInteger() )
		{
			soundSystem->SetMute( true );
		}

		soundSystem->Render();

		// process the game return for map changes, etc
		ProcessGameReturn( ret );

		idLobbyBase& lobby = session->GetActivePlatformLobbyBase();
		if( lobby.HasActivePeers() )
		{
			if( net_drawDebugHud.GetInteger() == 1 )
			{
				lobby.DrawDebugNetworkHUD();
			}
			if( net_drawDebugHud.GetInteger() == 2 )
			{
				lobby.DrawDebugNetworkHUD2();
			}
			lobby.DrawDebugNetworkHUD_ServerSnapshotMetrics( net_drawDebugHud.GetInteger() == 3 );
		}

		// report timing information
		if( com_speeds.GetBool() )
		{
			static int lastTime = Sys_Milliseconds();
			int	nowTime = Sys_Milliseconds();
			int	com_frameMsec = nowTime - lastTime;
			lastTime = nowTime;
			//puncho - MP-A4 (K26): scratch: the last tic's high-water in the game's scratch arena, read after WaitForThread
			Printf( "frame:%d all:%3d gfr:%3d rf:%3lld bk:%3lld scratch:%4dKB\n", idLib::frameNumber, com_frameMsec, time_gameFrame, time_frontend / 1000, time_backend / 1000, gameScratchStack.GetTicHighWater() >> 10 );
			time_gameFrame = 0;
			time_gameDraw = 0;
		}

		// the FPU stack better be empty at this point or some bad code or compiler bug left values on the stack
		if( !Sys_FPU_StackIsEmpty() )
		{
			Printf( "%s", Sys_FPU_GetState() );
			FatalError( "idCommon::Frame: the FPU stack is not empty at the end of the frame\n" );
		}

		mapMainAllocations = Mem_GetAllocationCount() - frameAllocStart;
		LogMapRows( numGameFrames );
		mainFrameTiming = frameTiming;

		session->GetSaveGameManager().Pump();
	}
	catch( idException& )
	{
		DrainPrintRelay();
		// an ERP_DROP was thrown
#if defined(USE_DOOMCLASSIC)
		if( currentGame == DOOM_CLASSIC || currentGame == DOOM2_CLASSIC )
		{
			return;
		}
#endif

		// kill loading gui
		delete loadGUI;
		loadGUI = NULL;

		// drop back to main menu
		LeaveGame();

		// force the console open to show error messages
		console->Open();
		return;
	}
}

// RB begin
#if defined(USE_DOOMCLASSIC)

/*
=================
idCommonLocal::RunDoomClassicFrame
=================
*/
void idCommonLocal::RunDoomClassicFrame()
{
	static int doomTics = 0;

	if( DoomLib::expansionDirty )
	{

		// re-Initialize the Doom Engine.
		DoomLib::Interface.Shutdown();
		DoomLib::Interface.Startup( 1, false );
		DoomLib::expansionDirty = false;
	}


	if( DoomLib::Interface.Frame( doomTics, &userCmdMgr ) )
	{
		Globals* data = ( Globals* )DoomLib::GetGlobalData( 0 );

		idArray< unsigned int, 256 > palette;
		std::copy( data->XColorMap, data->XColorMap + palette.Num(), palette.Ptr() );

		// Do the palette lookup.
		for( int row = 0; row < DOOMCLASSIC_RENDERHEIGHT; ++row )
		{
			for( int column = 0; column < DOOMCLASSIC_RENDERWIDTH; ++column )
			{
				const int doomScreenPixelIndex = row * DOOMCLASSIC_RENDERWIDTH + column;
				const byte paletteIndex = data->screens[0][doomScreenPixelIndex];
				const unsigned int paletteColor = palette[paletteIndex];
				const byte red = ( paletteColor & 0xFF000000 ) >> 24;
				const byte green = ( paletteColor & 0x00FF0000 ) >> 16;
				const byte blue = ( paletteColor & 0x0000FF00 ) >> 8;

				const int imageDataPixelIndex = row * DOOMCLASSIC_RENDERWIDTH * DOOMCLASSIC_BYTES_PER_PIXEL + column * DOOMCLASSIC_BYTES_PER_PIXEL;
				doomClassicImageData[imageDataPixelIndex]		= red;
				doomClassicImageData[imageDataPixelIndex + 1]	= green;
				doomClassicImageData[imageDataPixelIndex + 2]	= blue;
				doomClassicImageData[imageDataPixelIndex + 3]	= 255;
			}
		}
	}

	renderSystem->UploadImage( "_doomClassic", doomClassicImageData.Ptr(), DOOMCLASSIC_RENDERWIDTH, DOOMCLASSIC_RENDERHEIGHT );
	doomTics++;
}

#endif
// RB end

