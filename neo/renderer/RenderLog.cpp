/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013-2022 Robert Beckebans

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

#include <sys/DeviceManager.h>
extern DeviceManager* deviceManager;

idCVar r_logLevel( "r_logLevel", "0", CVAR_BOOL, "1 = Named render events in RenderDoc but is slower if enabled" );

static const int LOG_LEVEL_BLOCKS_ONLY	= 1;
static const int LOG_LEVEL_EVERYTHING	= 2;

const char* renderLogMainBlockLabels[] =
{
	ASSERT_ENUM_STRING( MRB_GPU_TIME,						0 ),
	ASSERT_ENUM_STRING( MRB_BEGIN_DRAWING_VIEW,				1 ),
	ASSERT_ENUM_STRING( MRB_FILL_DEPTH_BUFFER,				2 ),
	ASSERT_ENUM_STRING( MRB_FILL_HIZ_BUFFER,				3 ),
	ASSERT_ENUM_STRING( MRB_FILL_GEOMETRY_BUFFER,			4 ),
	ASSERT_ENUM_STRING( MRB_SSAO_PASS,						5 ),
	ASSERT_ENUM_STRING( MRB_AMBIENT_PASS,					6 ),
	ASSERT_ENUM_STRING( MRB_SHADOW_ATLAS_PASS,				7 ),
	ASSERT_ENUM_STRING( MRB_DRAW_INTERACTIONS,				8 ),
	ASSERT_ENUM_STRING( MRB_DRAW_SHADER_PASSES,				9 ),
	ASSERT_ENUM_STRING( MRB_FOG_ALL_LIGHTS,					10 ),
	ASSERT_ENUM_STRING( MRB_BLOOM,							11 ),
	ASSERT_ENUM_STRING( MRB_DRAW_SHADER_PASSES_POST,		12 ),
	ASSERT_ENUM_STRING( MRB_DRAW_DEBUG_TOOLS,				13 ),
	ASSERT_ENUM_STRING( MRB_CAPTURE_COLORBUFFER,			14 ),
	ASSERT_ENUM_STRING( MRB_MOTION_VECTORS,					15 ),
	ASSERT_ENUM_STRING( MRB_TAA,							16 ),
	ASSERT_ENUM_STRING( MRB_TONE_MAP_PASS,					17 ),
	ASSERT_ENUM_STRING( MRB_POSTPROCESS,					18 ),
	ASSERT_ENUM_STRING( MRB_DRAW_GUI,                       19 ),
	ASSERT_ENUM_STRING( MRB_CRT_POSTPROCESS,                20 ),
	ASSERT_ENUM_STRING( MRB_DRAW_INTERACTIONS_TEX,          21 ),	//puncho start
	ASSERT_ENUM_STRING( MRB_DRAW_INTERACTIONS_MATH,         22 ),	//puncho end
	// ---- X0: timed blocks (renderer upgrade seams) ----
	ASSERT_ENUM_STRING( MRB_SHADOW_CACHE,					23 ),
	ASSERT_ENUM_STRING( MRB_FORWARD_PASS,					24 ),
	ASSERT_ENUM_STRING( MRB_FORWARD_TRANSLUCENT,			25 ),
	ASSERT_ENUM_STRING( MRB_VOLUMETRICS,					26 ),
	ASSERT_ENUM_STRING( MRB_VOLUMETRICS_APPLY,				27 ),
	ASSERT_ENUM_STRING( MRB_DRAW_INTERACTIONS_TRANSLUCENT,	28 ),
	// ---- END X0 ----
	ASSERT_ENUM_STRING( MRB_TOTAL,							29 )
};

// ---- X0: frame-level main blocks (renderer upgrade seams) ----
// These are opened once per frame or once per view by design ( GPU_TIME wraps the frame, the others
// run per view ), so a second open inside one frame is not a defect and the double-open warning
// below skips them ( D71 )
static const uint32 mainBlockFrameLevel = BIT( MRB_GPU_TIME ) | BIT( MRB_BEGIN_DRAWING_VIEW )
		| BIT( MRB_DRAW_GUI ) | BIT( MRB_POSTPROCESS ) | BIT( MRB_CRT_POSTPROCESS );
// ---- END X0 ----

extern uint64 Sys_Microseconds();

/*
========================
PC_BeginNamedEvent

FIXME: this is not thread safe on the PC
========================
*/
void PC_BeginNamedEvent( const char* szName, const idVec4& color, nvrhi::ICommandList* commandList )
{
	if( r_logLevel.GetInteger() <= 0 )
	{
		return;
	}

	if( commandList )
	{
		commandList->beginMarker( szName );
	}
}

/*
========================
PC_EndNamedEvent
========================
*/
void PC_EndNamedEvent( nvrhi::ICommandList* commandList )
{
	if( r_logLevel.GetInteger() <= 0 )
	{
		return;
	}

	if( commandList )
	{
		commandList->endMarker();
	}
}

/*
================================================================================================

idRenderLog

================================================================================================
*/

idRenderLog	renderLog;


idRenderLog::idRenderLog()
{
	frameCounter = 0;
	frameParity = 0;
	mainBlocksOpen = 0;
	mainBlocksUsed = 0;
	mainBlocksWarned = 0;
}

void idRenderLog::Init()
{
	for( int i = 0; i < MRB_TOTAL * NUM_FRAME_DATA; i++ )
	{
		timerQueries.Append( deviceManager->GetDevice()->createTimerQuery() );
		timerUsed.Append( false );
	}
}

void idRenderLog::Shutdown()
{
	commandList = nullptr;

	for( int i = 0; i < MRB_TOTAL * NUM_FRAME_DATA; i++ )
	{
		timerQueries[i].Reset();
	}
}

void idRenderLog::StartFrame( nvrhi::ICommandList* _commandList )
{
	commandList = _commandList;
}

void idRenderLog::EndFrame()
{
	commandList = nullptr;
}



/*
========================
idRenderLog::OpenMainBlock
========================
*/
void idRenderLog::OpenMainBlock( renderLogMainBlock_t block )
{
	// X0 ( D71 ): the begin marker is emitted for every pair, also for views whose timers are gated off
	if( r_logLevel.GetInteger() > 0 )
	{
		PC_BeginNamedEvent( renderLogMainBlockLabels[block], colorBlack, commandList );
	}

	// SRS - Use glConfig.timerQueryAvailable flag to control timestamp capture for all platforms
	if( glConfig.timerQueryAvailable )
	{
		// ---- X0: markers + open-block mask (renderer upgrade seams) ----
		// All of this runs only while r_logLevel > 0, so with the cvar at 0 the engine behaves
		// exactly as before ( D71 )
		if( r_logLevel.GetInteger() > 0 )
		{
			const uint32 bit = ( uint32 )BIT( block );

			// a block opened twice in one frame only gets its first pair timed ( the timer slot is
			// claimed by the first open ); warn once per block, frame-level blocks excepted. The
			// check is per frame, not per view: stereo and frames that draw several 2D views
			// warn once per block, truthfully.
			if( ( mainBlocksUsed & bit ) && !( mainBlocksWarned & bit ) && !( mainBlockFrameLevel & bit ) )
			{
				mainBlocksWarned |= bit;
				common->Warning( "idRenderLog: %s opened twice in one frame; only its first pair is timed", renderLogMainBlockLabels[block] );
			}

			mainBlocksOpen |= bit;
			mainBlocksUsed |= bit;
		}
		// ---- END X0 ----

		mainBlock = block;

		int timerIndex = mainBlock + frameParity * MRB_TOTAL;

		// SRS - Only issue a new start timer query if timer slot unused
		if( !timerUsed[ timerIndex ] )
		{
			commandList->beginTimerQuery( timerQueries[ timerIndex ] );
		}
	}
}

/*
========================
idRenderLog::CloseMainBlock
========================
*/
void idRenderLog::CloseMainBlock( int _block )
{
	// SRS - Use glConfig.timerQueryAvailable flag to control timestamp capture for all platforms
	if( glConfig.timerQueryAvailable )
	{
		renderLogMainBlock_t block = mainBlock;

		if( _block != -1 )
		{
			block = renderLogMainBlock_t( _block );
		}

		// ---- X0: end marker + open-block mask (renderer upgrade seams) ----
		// Gated on r_logLevel like the open side ( D71 )
		if( r_logLevel.GetInteger() > 0 )
		{
			const uint32 bit = ( uint32 )BIT( block );

			// a close of a block that is not open: warn once per block
			if( !( mainBlocksOpen & bit ) && !( mainBlocksWarned & bit ) )
			{
				mainBlocksWarned |= bit;
				common->Warning( "idRenderLog: %s closed without being opened", renderLogMainBlockLabels[block] );
			}

			mainBlocksOpen &= ~bit;
		}
		// ---- END X0 ----

		int timerIndex = block + frameParity * MRB_TOTAL;

		// SRS - Only issue a new end timer query if timer slot unused
		if( !timerUsed[ timerIndex ] )
		{
			commandList->endTimerQuery( timerQueries[ timerIndex ] );
			timerUsed[ timerIndex ] = true;
		}
	}

	// X0 ( D71 ): the end marker is emitted for every pair, also for views whose timers are gated off
	if( r_logLevel.GetInteger() > 0 )
	{
		PC_EndNamedEvent( commandList );
	}
}

/*
========================
idRenderLog::FetchGPUTimers
========================
*/
void idRenderLog::FetchGPUTimers( backEndCounters_t& pc )
{
	// ---- X0: frame-end check of the open-block mask (renderer upgrade seams) ----
	// This is where a new frame starts ( the parity flips here ); a block still marked open was
	// left open across the frame end; warn once per block, then restart the masks ( D71 )
	if( r_logLevel.GetInteger() > 0 )
	{
		for( int i = 0; i < MRB_TOTAL; i++ )
		{
			const uint32 bit = ( uint32 )BIT( i );

			if( ( mainBlocksOpen & bit ) && !( mainBlocksWarned & bit ) )
			{
				mainBlocksWarned |= bit;
				common->Warning( "idRenderLog: %s left open at the end of the frame", renderLogMainBlockLabels[i] );
			}
		}

		mainBlocksOpen = 0;
		mainBlocksUsed = 0;
	}
	// ---- END X0 ----

	frameCounter++;
	frameParity = ( frameParity + 1 ) % NUM_FRAME_DATA;

	for( int i = 0; i < MRB_TOTAL; i++ )
	{
		int timerIndex = i + frameParity * MRB_TOTAL;

		if( timerUsed[timerIndex] )
		{
			double time = deviceManager->GetDevice()->getTimerQueryTime( timerQueries[ timerIndex ] );
			time *= 1000000.0; // seconds -> microseconds

			switch( i )
			{
				case MRB_GPU_TIME:
					pc.gpuMicroSec = time;
					break;

				case MRB_BEGIN_DRAWING_VIEW:
					pc.gpuBeginDrawingMicroSec = time;
					break;

				case MRB_FILL_DEPTH_BUFFER:
					pc.gpuDepthMicroSec = time;
					break;

				case MRB_FILL_HIZ_BUFFER:
					pc.gpuHiZMicroSec = time;
					break;

				case MRB_FILL_GEOMETRY_BUFFER:
					pc.gpuGeometryMicroSec = time;
					break;

				case MRB_SSAO_PASS:
					pc.gpuScreenSpaceAmbientOcclusionMicroSec = time;
					break;

				case MRB_AMBIENT_PASS:
					pc.gpuAmbientPassMicroSec = time;
					break;

				case MRB_SHADOW_ATLAS_PASS:
					pc.gpuShadowAtlasPassMicroSec = time;
					break;

				case MRB_DRAW_INTERACTIONS:
					pc.gpuInteractionsMicroSec = time;
					break;
				//puncho start - without these cases the new blocks fall through to "default: break;" and
				// their timestamps are read from the GPU and discarded, like MRB_DRAW_DEBUG_TOOLS today.
				case MRB_DRAW_INTERACTIONS_TEX:
					pc.gpuInteractionsTexMicroSec = time;
					break;

				case MRB_DRAW_INTERACTIONS_MATH:
					pc.gpuInteractionsMathMicroSec = time;
					break;
				//puncho end

				case MRB_DRAW_SHADER_PASSES:
					pc.gpuShaderPassMicroSec = time;
					break;

				case MRB_FOG_ALL_LIGHTS:
					pc.gpuFogAllLightsMicroSec = time;
					break;

				case MRB_BLOOM:
					pc.gpuBloomMicroSec = time;
					break;

				case MRB_DRAW_SHADER_PASSES_POST:
					pc.gpuShaderPassPostMicroSec = time;
					break;

				case MRB_MOTION_VECTORS:
					pc.gpuMotionVectorsMicroSec = time;
					break;

				case MRB_TAA:
					pc.gpuTemporalAntiAliasingMicroSec = time;
					break;

				case MRB_TONE_MAP_PASS:
					pc.gpuToneMapPassMicroSec = time;
					break;

				case MRB_POSTPROCESS:
					pc.gpuPostProcessingMicroSec = time;
					break;

				case MRB_DRAW_GUI:
					pc.gpuDrawGuiMicroSec = time;
					break;

				case MRB_CRT_POSTPROCESS:
					pc.gpuCrtPostProcessingMicroSec = time;
					break;

				// ---- X0: the new timed blocks (renderer upgrade seams) ----
				case MRB_SHADOW_CACHE:
					pc.gpuShadowCacheMicroSec = time;
					break;

				case MRB_FORWARD_PASS:
					pc.gpuForwardPassMicroSec = time;
					break;

				case MRB_FORWARD_TRANSLUCENT:
					pc.gpuForwardTranslucentMicroSec = time;
					break;

				case MRB_VOLUMETRICS:
					pc.gpuVolumetricsMicroSec = time;
					break;

				case MRB_VOLUMETRICS_APPLY:
					pc.gpuVolumetricsApplyMicroSec = time;
					break;

				case MRB_DRAW_INTERACTIONS_TRANSLUCENT:
					pc.gpuInteractionsTranslucentMicroSec = time;
					break;
				// ---- END X0 ----

				default:
					break;
			}
		}

		// reset timer
		timerUsed[timerIndex] = false;
	}
}


/*
========================
idRenderLog::OpenBlock
========================
*/
void idRenderLog::OpenBlock( const char* label, const idVec4& color )
{
	PC_BeginNamedEvent( label, color, commandList );
}

/*
========================
idRenderLog::CloseBlock
========================
*/
void idRenderLog::CloseBlock()
{
	PC_EndNamedEvent( commandList );
}
