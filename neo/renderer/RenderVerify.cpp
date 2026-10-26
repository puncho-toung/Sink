/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

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

/*
================================================================================
RenderVerify.cpp

Renderer dumps for the camera-relative verification sheet (plans/camera-relative/00-PLAN.md
WP-CRV, 01-VERIFY.md). They add no rendering behaviour. Every command is a CONSOLE_COMMAND, so
the bodies vanish in ID_RETAIL builds; verify builds are non-retail.

The commands that need a finished frame build one themselves, on the main thread, with the
sequence idRenderSystemLocal::TakeScreenshot uses: wait for the game thread, discard the pending
command list, build a frame from the current game state, render it and swap. When that returns
the GPU is idle and no backend command list is open, so a read-back opens its own command list
exactly as R_ReadPixelsRGB8 does. Nothing here runs on the backend mid-frame.
================================================================================
*/

#include "precompiled.h"
#pragma hdrstop

#include "RenderCommon.h"
#include "../framework/Common_local.h"	// commonLocal.WaitGameThread(), commonLocal.Draw()

#include <sys/DeviceManager.h>
extern DeviceManager* deviceManager;

// RenderWorld_envprobes.cpp: the bake's hardware depth to world distance, shared so the dump
// measures the bake's own decode and not a copy of it
float LinearizeProbeDepth( float hwDepth, float zNear, int face, int pixel, int size );

// read by R_SortDrawSurfs (tr_frontend_main.cpp); dumpSortKeys sets it around one frame that it
// builds on this thread, so the prints below never come from a worker thread
bool verifySortKeys = false;
static int verifySortKeysView;

static uint32 R_VerifyFloatBits( const float f )
{
	uint32 bits;
	memcpy( &bits, &f, sizeof( bits ) );
	return bits;
}

/*
==================
R_VerifyBuildFrame

The first half of TakeScreenshot: the frontend has built every view of the frame when this
returns, and tr.pc holds that frame's counters until the next SwapCommandBuffers clears them.
==================
*/
static void R_VerifyBuildFrame()
{
	tr.takingScreenshot = true;

	// make sure the game / draw thread has completed
	commonLocal.WaitGameThread();

	// discard anything currently on the list
	tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );

	// build all the draw commands without running a new game tic
	commonLocal.Draw();
}

/*
==================
R_VerifyDrawFrame

The second half of TakeScreenshot up to its read-back: the GPU has drawn the frame and is idle.
==================
*/
static void R_VerifyDrawFrame()
{
	const emptyCommand_t* cmd = tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );

	tr.RenderCommandBuffers( cmd );

	// this triggers SwapBuffers
	tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );
}

/*
==================
R_VerifyEndFrame
==================
*/
static void R_VerifyEndFrame()
{
	// discard anything currently on the list
	tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );

	tr.takingScreenshot = false;
}

/*
==================
R_VerifyPrintSortKeys

Called from R_SortDrawSurfs after the sort and before the keys are overwritten with the sorted
pointers. keys[] is in draw order; each key is ( sort << 32 ) | ( dist << 16 ) | index, with
index = numDrawSurfs - original position (tr_frontend_main.cpp).
==================
*/
void R_VerifyPrintSortKeys( const viewDef_t* viewDef, drawSurf_t* const* drawSurfs, const uint64* keys, const int numDrawSurfs )
{
	common->Printf( "sortkeys view %i viewID %i subview %i mirror %i xray %i surfs %i\n", verifySortKeysView,
					viewDef->renderView.viewID, viewDef->isSubview ? 1 : 0, viewDef->isMirror ? 1 : 0,
					viewDef->isXraySubview ? 1 : 0, numDrawSurfs );
	verifySortKeysView++;

	for( int i = 0; i < numDrawSurfs; i++ )
	{
		const uint64 key = keys[i];
		const drawSurf_t* surf = drawSurfs[ numDrawSurfs - ( int )( key & 0xFFFF ) ];

		const int entityIndex = ( surf->space != NULL && surf->space->entityDef != NULL ) ? surf->space->entityDef->index : -1;
		const char* materialName = ( surf->material != NULL ) ? surf->material->GetName() : "<none>";

		common->Printf( "%5i sort %g dist %5u key %016llx ent %i mat %s\n", i, surf->sort,
						( unsigned int )( ( key >> 16 ) & 0xFFFF ), ( unsigned long long )key, entityIndex, materialName );
	}
}

/*
==================
R_VerifyWriteDepthPFM

Writes a depth texture that sits in DepthWrite between frames (the shadow atlas) as a grey PFM,
rows bottom-up as the format wants. Depth formats cannot be copied to a staging texture on every
API, so it is blitted to R32F first, as R_ReadProbeColorAndDepthRGB16F does, but with a point
sampler and 32 bits so the values are the stored ones, bit for bit.
==================
*/
static bool R_VerifyWriteDepthPFM( nvrhi::ITexture* depthTexture, const char* fileName, int& texelsBelowClear )
{
	nvrhi::IDevice* device = deviceManager->GetDevice();

	nvrhi::TextureDesc desc = depthTexture->getDesc();
	const int width = desc.width;
	const int height = desc.height;

	desc.format = nvrhi::Format::R32_FLOAT;
	desc.isRenderTarget = true;
	desc.initialState = nvrhi::ResourceStates::RenderTarget;
	desc.keepInitialState = true;

	nvrhi::TextureHandle tempTexture = device->createTexture( desc );
	nvrhi::FramebufferHandle tempFramebuffer = device->createFramebuffer( nvrhi::FramebufferDesc().addColorAttachment( tempTexture ) );

	nvrhi::CommandListHandle commandList = device->createCommandList();
	commandList->open();

	commandList->beginTrackingTextureState( depthTexture, nvrhi::TextureSubresourceSet( 0, 1, 0, 1 ), nvrhi::ResourceStates::DepthWrite );

	BlitParameters blit;
	blit.targetFramebuffer = tempFramebuffer;
	blit.sourceTexture = depthTexture;
	blit.sampler = BlitSampler::Point;
	backEnd.GetCommonPasses().BlitTexture( commandList, blit );

	nvrhi::StagingTextureHandle stagingTexture = device->createStagingTexture( desc, nvrhi::CpuAccessMode::Read );
	commandList->copyTexture( stagingTexture, nvrhi::TextureSlice(), tempTexture, nvrhi::TextureSlice() );

	commandList->setTextureState( depthTexture, nvrhi::TextureSubresourceSet( 0, 1, 0, 1 ), nvrhi::ResourceStates::DepthWrite );
	commandList->commitBarriers();

	commandList->close();
	device->executeCommandList( commandList );

	size_t rowPitch = 0;
	const byte* data = ( const byte* )device->mapStagingTexture( stagingTexture, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch );
	if( data == NULL )
	{
		return false;
	}

	idFile* f = fileSystem->OpenFileWrite( fileName, "fs_basepath" );
	if( f == NULL )
	{
		device->unmapStagingTexture( stagingTexture );
		return false;
	}

	// a negative scale marks little-endian floats
	f->Printf( "Pf\n%i %i\n-1.0\n", width, height );

	texelsBelowClear = 0;
	for( int y = height - 1; y >= 0; y-- )
	{
		const float* row = ( const float* )( data + y * rowPitch );
		for( int x = 0; x < width; x++ )
		{
			if( row[x] < 1.0f )
			{
				texelsBelowClear++;
			}
		}
		f->Write( row, width * ( int )sizeof( float ) );
	}

	fileSystem->CloseFile( f );
	device->unmapStagingTexture( stagingTexture );

	return true;
}

/*
==================
dumpAreaBounds

V1.3: the hex of every area->globalBounds, so two processes can be compared bit for bit.
==================
*/
CONSOLE_COMMAND( dumpAreaBounds, "prints every portal area's globalBounds as six hex floats", NULL )
{
	const idRenderWorldLocal* world = tr.primaryWorld;
	if( world == NULL )
	{
		common->Printf( "dumpAreaBounds: no map loaded\n" );
		return;
	}

	// the frontend reads the areas on the game thread; make sure it is idle
	commonLocal.WaitGameThread();

	const int numAreas = world->NumAreas();
	common->Printf( "areaBounds %s areas %i\n", world->mapName.c_str(), numAreas );
	for( int i = 0; i < numAreas; i++ )
	{
		const idBounds& b = world->portalAreas[i].globalBounds;
		common->Printf( "area %i %08x %08x %08x %08x %08x %08x\n", i,
						R_VerifyFloatBits( b[0].x ), R_VerifyFloatBits( b[0].y ), R_VerifyFloatBits( b[0].z ),
						R_VerifyFloatBits( b[1].x ), R_VerifyFloatBits( b[1].y ), R_VerifyFloatBits( b[1].z ) );
	}
}

/*
==================
dumpSortKeys

V3.3c / GA3: the ( sort, dist ) key order of every view, exactly as R_SortDrawSurfs sorted it.
The frame is built here on the main thread, so the keys are printed by the thread that sorts.
==================
*/
CONSOLE_COMMAND( dumpSortKeys, "builds one frame and prints every view's draw-surface sort keys in draw order", NULL )
{
	if( tr.primaryWorld == NULL )
	{
		common->Printf( "dumpSortKeys: no map loaded\n" );
		return;
	}

	common->Printf( "dumpSortKeys: frame %i\n", tr.frameCount );

	tr.takingScreenshot = true;
	commonLocal.WaitGameThread();
	tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );

	verifySortKeysView = 0;
	verifySortKeys = true;
	commonLocal.Draw();
	verifySortKeys = false;

	R_VerifyDrawFrame();
	R_VerifyEndFrame();

	common->Printf( "dumpSortKeys: %i views\n", verifySortKeysView );
}

/*
==================
printMocCounters

V2.3b / GD6: the masked occlusion culling counters of one whole frame, built here so they are
read after the frontend finished and before SwapCommandBuffers clears them, never through the
mid-frame PrintPerformanceCounters. The counters are bumped without atomics from the parallel
add-model and add-light jobs, so they can undercount by lost increments.
==================
*/
CONSOLE_COMMAND( printMocCounters, "builds one frame and prints its masked occlusion culling counters", NULL )
{
	if( tr.primaryWorld == NULL )
	{
		common->Printf( "printMocCounters: no map loaded\n" );
		return;
	}

	R_VerifyBuildFrame();

	const int frame = tr.frameCount;
	const int tests = tr.pc.c_mocTests;
	const int culledSurfaces = tr.pc.c_mocCulledSurfaces;
	const int culledLights = tr.pc.c_mocCulledLights;

	R_VerifyDrawFrame();
	R_VerifyEndFrame();

	common->Printf( "moc frame %i tests %i culledSurfaces %i culledLights %i\n", frame, tests, culledSurfaces, culledLights );
}

/*
==================
dumpShadowMaps

V3.7: builds and draws one frame, then writes the shadow atlas as a PFM. ShadowAtlasPass clears
the atlas for every view that has lights, so the file holds the last such view of the frame,
the player view. The count of texels below the clear depth is the positive check that shadows
were drawn at all; an empty atlas would compare equal to any other empty atlas.
==================
*/
CONSOLE_COMMAND( dumpShadowMaps, "builds one frame and writes its shadow atlas depth as a PFM: dumpShadowMaps <path>", NULL )
{
	if( args.Argc() != 2 )
	{
		common->Printf( "usage: dumpShadowMaps <path relative to fs_basepath/<game>>\n" );
		return;
	}

	if( tr.primaryWorld == NULL )
	{
		common->Printf( "dumpShadowMaps: no map loaded\n" );
		return;
	}

	if( !r_useShadowAtlas.GetBool() )
	{
		common->Printf( "dumpShadowMaps: only the shadow atlas is dumped; set r_useShadowAtlas 1\n" );
		return;
	}

	R_VerifyBuildFrame();
	R_VerifyDrawFrame();

	int texelsBelowClear = 0;
	const bool written = R_VerifyWriteDepthPFM( globalImages->shadowAtlasImage->GetTextureHandle(), args.Argv( 1 ), texelsBelowClear );

	R_VerifyEndFrame();

	if( !written )
	{
		common->Printf( "dumpShadowMaps: read-back or write of %s failed\n", args.Argv( 1 ) );
		return;
	}

	common->Printf( "dumpShadowMaps: wrote %s (%i x %i, %i texels below the clear depth)\n", args.Argv( 1 ),
					globalImages->shadowAtlasImage->GetUploadWidth(), globalImages->shadowAtlasImage->GetUploadHeight(), texelsBelowClear );
}

/*
==================
dumpProbeDepth

V3.9 / V4.3 / GC13: renders one cube face of an environment probe the way bakeEnvironmentProbes
does and prints the linear distance from the probe before any R11F encoding: at the face's
centre pixel, and the 4 x 4 minimum the bake packs into the centre texel of its 64 x 64 face.
==================
*/
CONSOLE_COMMAND( dumpProbeDepth, "renders one probe cube face and prints its linear depth before R11F encoding: dumpProbeDepth <probe> <face 0-5 | px nx py ny pz nz>", NULL )
{
	if( args.Argc() != 3 )
	{
		common->Printf( "usage: dumpProbeDepth <probe index> <face 0-5 | px nx py ny pz nz>\n" );
		return;
	}

	if( tr.primaryWorld == NULL || tr.primaryView == NULL )
	{
		common->Printf( "dumpProbeDepth: no map loaded or no primary view yet\n" );
		return;
	}

	const int probeIndex = atoi( args.Argv( 1 ) );
	if( probeIndex < 0 || probeIndex >= tr.primaryWorld->envprobeDefs.Num() || tr.primaryWorld->envprobeDefs[probeIndex] == NULL )
	{
		common->Printf( "dumpProbeDepth: no probe %i (%i probe slots)\n", probeIndex, tr.primaryWorld->envprobeDefs.Num() );
		return;
	}

	// faces in tr.cubeAxis order: forward +x, -x, +y, -y, +z, -z
	static const char* faceNames[6] = { "px", "nx", "py", "ny", "pz", "nz" };
	int face = -1;
	for( int i = 0; i < 6; i++ )
	{
		if( idStr::Icmp( args.Argv( 2 ), faceNames[i] ) == 0 )
		{
			face = i;
		}
	}
	if( face == -1 )
	{
		const char* s = args.Argv( 2 );
		if( s[0] >= '0' && s[0] <= '5' && s[1] == '\0' )
		{
			face = s[0] - '0';
		}
	}
	if( face == -1 )
	{
		common->Printf( "dumpProbeDepth: bad face '%s'\n", args.Argv( 2 ) );
		return;
	}

	const RenderEnvprobeLocal* def = tr.primaryWorld->envprobeDefs[probeIndex];
	const int captureSize = ENVPROBE_CAPTURE_SIZE;

	// the view and the state of one face of bakeEnvironmentProbes (RenderWorld_envprobes.cpp)
	renderView_t ref = tr.primaryView->renderView;
	ref.rdflags = RDF_NOAMBIENT | RDF_IRRADIANCE;
	ref.fov_x = ref.fov_y = 90;
	ref.vieworg = def->parms.origin;
	ref.viewaxis = tr.cubeAxis[face];

	// make sure the game / draw thread has completed
	commonLocal.WaitGameThread();

	// the bake turns these off ("prevents a crash"); restore what was set, not true as the bake does
	extern idCVar r_useParallelAddModels;
	extern idCVar r_useParallelAddShadows;
	extern idCVar r_useParallelAddLights;
	const bool oldScissor = r_useScissor.GetBool();
	const bool oldAddModels = r_useParallelAddModels.GetBool();
	const bool oldAddShadows = r_useParallelAddShadows.GetBool();
	const bool oldAddLights = r_useParallelAddLights.GetBool();
	r_useScissor.SetBool( false );
	r_useParallelAddModels.SetBool( false );
	r_useParallelAddShadows.SetBool( false );
	r_useParallelAddLights.SetBool( false );

	// discard anything currently on the list (this triggers SwapBuffers)
	tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );

	tr.takingEnvprobe = true;

	tr.primaryWorld->RenderScene( &ref );
	const emptyCommand_t* cmd = tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );
	tr.RenderCommandBuffers( cmd );
	tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );

	byte* colorPic = NULL;
	byte* depthPic = NULL;
	const bool depthOK = R_ReadProbeColorAndDepthRGB16F( deviceManager->GetDevice(), &backEnd.GetCommonPasses(),
						 globalImages->envprobeHDRImage->GetTextureHandle(), globalImages->envprobeDepthImage->GetTextureHandle(),
						 &colorPic, &depthPic, captureSize, captureSize );

	tr.takingEnvprobe = false;

	r_useScissor.SetBool( oldScissor );
	r_useParallelAddModels.SetBool( oldAddModels );
	r_useParallelAddShadows.SetBool( oldAddShadows );
	r_useParallelAddLights.SetBool( oldAddLights );

	if( colorPic != NULL )
	{
		R_StaticFree( colorPic );
	}

	if( !depthOK || depthPic == NULL )
	{
		common->Printf( "dumpProbeDepth: depth read-back failed for probe %i face %s\n", probeIndex, faceNames[face] );
		if( depthPic != NULL )
		{
			R_StaticFree( depthPic );
		}
		return;
	}

	// the read-back is RGB16F; the bake decodes channel 0 of each pixel with the capture's near plane
	const halfFloat_t* depth = ( const halfFloat_t* )depthPic;
	const float captureZNear = ( ref.cramZNear ) ? ( r_znear.GetFloat() * 0.25f ) : r_znear.GetFloat();

	const int centre = ( captureSize / 2 ) * captureSize + ( captureSize / 2 );
	const float centreDist = LinearizeProbeDepth( F16toF32( depth[ centre * 3 + 0 ] ), captureZNear, face, centre, captureSize );

	// the bake's MIN reduction for its 64 x 64 texel ( 32, 32 ), which covers the centre pixel
	const int outSize = 64;
	const int block = captureSize / outSize;
	const int ox = outSize / 2;
	const int oy = outSize / 2;
	float minDist = 1e30f;
	for( int sy = 0; sy < block; sy++ )
	{
		for( int sx = 0; sx < block; sx++ )
		{
			const int p = ( oy * block + sy ) * captureSize + ( ox * block + sx );
			const float d = LinearizeProbeDepth( F16toF32( depth[ p * 3 + 0 ] ), captureZNear, face, p, captureSize );
			minDist = ( d < minDist ) ? d : minDist;
		}
	}

	R_StaticFree( depthPic );

	common->Printf( "probeDepth probe %i face %s origin ( %s ) centre %.3f 0x%08x min4x4 %.3f 0x%08x\n", probeIndex, faceNames[face],
					def->parms.origin.ToString( 3 ), centreDist, R_VerifyFloatBits( centreDist ), minDist, R_VerifyFloatBits( minDist ) );
}
