/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2014 Robert Beckebans

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
#include <precompiled.h>
#pragma hdrstop

#include "renderer/RenderCommon.h"
#include "../../framework/Common_local.h"	// the probe prints the GPU time of the fog's two blocks

#include "VolumetricFogPass.h"
#include "VolumetricFogPass_cb.h"
#include "renderer/VolumetricFog.h"		// the take rule and the reach

idCVar r_volumetricFog( "r_volumetricFog", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "froxel fog in the main view: 0 = off ( no fog pass: the engine's picture without it ), 1 = on" );
static idCVar r_volumetricFogColumnPixels( "r_volumetricFogColumnPixels", "12", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "the side of a froxel column in pixels; the grid follows the render size, and a grid of too many froxels gets wider columns", 4, 64 );
// V5: the quality levels of WP-V5. -1 takes r_volumetricFogColumnPixels and r_volumetricFogSlices as set
static idCVar r_volumetricFogQuality( "r_volumetricFogQuality", "-1", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "the fog grid's quality: 0 low ( 16-pixel columns, 48 slices: at 1920 x 1080 0.39 M froxels, 19 MiB of volumes ), 1 medium ( 12, 64: 0.92 M, 44 MiB ), 2 high ( 8, 96: 3.1 M, 148 MiB ); a level takes at most 4 Mi froxels, about 200 MiB, widening its columns past them; -1 takes r_volumetricFogColumnPixels and r_volumetricFogSlices as set", -1, 2 );
static idCVar r_volumetricFogSlices( "r_volumetricFogSlices", "64", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "the number of depth slices of the fog volume", 8, 256 );
static idCVar r_volumetricFogFar( "r_volumetricFogFar", "3000", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "the far end of the fog volume in units along the view axis; no fog is added beyond it", 64.0f, 65536.0f );
static idCVar r_volumetricFogDistance( "r_volumetricFogDistance", "1500", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "the global fog's distance D in units: the air is 99 percent opaque at D ( sigma_t = 4.632 / D ); 0 = no global fog; a distance above 0 counts as at least 0.1", 0.0f, 1000000.0f );
static idCVar r_volumetricFogAlbedo( "r_volumetricFogAlbedo", "0.9", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "the global fog's scattering albedo: the share of the light the air takes out of a ray that it scatters and does not absorb", 0.0f, 1.0f );
static idCVar r_volumetricFogFurnace( "r_volumetricFogFurnace", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "test: above 0 the fog is the uniform test medium, lit by this radiance from every direction and by nothing else ( cut where the fog is so thick that the half-float volumes cannot hold the in-scatter )", 0.0f, 1000.0f );
static idCVar r_volumetricFogAnisotropy( "r_volumetricFogAnisotropy", "0.4", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "the fog's phase function ( Henyey-Greenstein g ): at 0 a light's fog is as bright from every side, above 0 it is brighter when looking toward the light, below 0 when looking along it", -0.9f, 0.9f );
static idCVar r_volumetricFogCull( "r_volumetricFogCull", "2", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "how the fog's inject finds the lights of a froxel: 2 = the light lists' tile bits and Z-bins, 1 = every light record behind one sphere test per thread group, 0 = every light record ( the reference ); the three give the same fog, except that 2 leaves a light out behind a wall, where the portals clip the light's scissor", 0, 2 );
static idCVar r_volumetricFogHistoryWeight( "r_volumetricFogHistoryWeight", "0.9", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "the weight of last frame's fog in this frame's ( the history, reprojected and with each light's change added ); 0 = no history", 0.0f, 0.98f );
static idCVar r_volumetricFogHistoryClipWeight( "r_volumetricFogHistoryClipWeight", "0.4", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "the history's weight where it is clipped: near a light that moved, where a caster of a light changed, near a light that is new or whose colour changed much; at most the history's weight", 0.0f, 0.98f );
static idCVar r_volumetricFogJitter( "r_volumetricFogJitter", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "the inject's sample of the lights moves inside the froxel from frame to frame, and the history averages the frames ( the media are taken at the centre ); 0 = the froxel's centre" );
static idCVar r_volumetricFogSkip( "r_volumetricFogSkip", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "the inject skips the froxels behind the farthest surface of their column and its neighbours: no pixel reads them; 0 = it lights every froxel ( the furnace always does )" );
static idCVar r_volumetricFogShadows( "r_volumetricFogShadows", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "the fog's inject looks each light's shadow up in the shadow atlas, one comparison per froxel; 0 = no lookup: a light whose row has a place is taken unshadowed, times its fade" );
static idCVar r_volumetricFogDebug( "r_volumetricFogDebug", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "fog debug view in place of the picture: 1 = the transmittance to each pixel as grey, 2 = the slices as bands over the grid of columns", 0, 2 );
// V5 ( D20, D35 ): the translucent stages and the lit glass after the apply fogged by their own shaders
static idCVar r_volumetricFogTranslucent( "r_volumetricFogTranslucent", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "the froxel fog fogs the translucent surfaces drawn after its apply, each at its own depth: 0 draws them unfogged ( V4's picture )" );
// V4: the media ( the base of track M ). The map's worldspawn sets the height fog's cvars from its keys at every spawn and
// restore ( d3xp/WorldSpawn.cpp ), each to its default where the key is absent
static idCVar r_volumetricFogMedia( "r_volumetricFogMedia", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "the fog's media pass and its medium per froxel ( V4 ): the height fog, the dust, the fog lights and the test box; 0 = the global fog alone, as V3 drew it" );
static idCVar r_volumetricFogHeightDistance( "r_volumetricFogHeightDistance", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "the height fog's distance D in units at full density ( sigma_t = 5.54 / D: 1/255 of the light is left after D ); 0 = no height fog. The worldspawn's key fog_height_distance", 0.0f, 1000000.0f );
static idCVar r_volumetricFogHeight( "r_volumetricFogHeight", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "the height fog's base, a height of the map: the fog is full below it and thins above it. The worldspawn's key fog_height" );
static idCVar r_volumetricFogHeightFalloff( "r_volumetricFogHeightFalloff", "256", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "the height in units over which the height fog thins to 1 / e above its base. The worldspawn's key fog_height_falloff", 1.0f, 1000000.0f );
static idCVar r_volumetricFogHeightAlbedo( "r_volumetricFogHeightAlbedo", "0.9", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "the height fog's scattering albedo. The worldspawn's key fog_height_albedo", 0.0f, 1.0f );
static idCVar r_volumetricFogDustDistance( "r_volumetricFogDustDistance", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "the dust's distance D in units where its noise is full ( sigma_t = 5.54 / D ); 0 = no dust. The worldspawn's key fog_dust_distance", 0.0f, 1000000.0f );
static idCVar r_volumetricFogDustAlbedo( "r_volumetricFogDustAlbedo", "0.9", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "the dust's scattering albedo. The worldspawn's key fog_dust_albedo", 0.0f, 1.0f );
static idCVar r_volumetricFogDustScale( "r_volumetricFogDustScale", "256", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "the size in units of the dust's largest lumps: its noise's base cell, rounded so that a whole number of cells spans 1024 units ( the dust is fixed to the map ). The worldspawn's key fog_dust_scale", 8.0f, 1024.0f );
static idCVar r_volumetricFogDustWind( "r_volumetricFogDustWind", "16 0 0", CVAR_RENDERER | CVAR_NEW, "the dust's drift in map units per second of game time, \"x y z\". The worldspawn's key fog_dust_wind" );
// V4e: what the dust costs scales with its noise's kind and octaves, on every froxel the inject fills. Measured at 1920 x 1080
// on an AMD Radeon 8060S ( RUN v4-2026-11-20, timing v4t2 ): fBm 4 octaves about 400 us of GPU, with the erode about 700,
// Worley 4 octaves about 1,300, Perlin-Worley with the erode about 2,000. The defaults are fBm, 3 octaves, no erode
static idCVar r_volumetricFogDustNoise( "r_volumetricFogDustNoise", "1", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "the dust's noise: 1 = fBm ( soft lumps; the cheapest ), 2 = Worley ( round billows; about three times fBm's cost ), 3 = Perlin-Worley ( puffy lumps with soft insides; about four times ). The worldspawn's key fog_dust_noise", 1, 3 );
static idCVar r_volumetricFogDustOctaves( "r_volumetricFogDustOctaves", "3", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "the octaves of the dust's noise, from its size down by halves; each costs as much as the first ( far froxels run fewer ). The worldspawn's key fog_dust_octaves", 1, 8 );
static idCVar r_volumetricFogDustErode( "r_volumetricFogDustErode", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "how much a finer fBm at a quarter of the dust's size eats its thin parts ( noise slot 2's erode, at most 3 octaves ); 0 = none. The worldspawn's key fog_dust_erode", 0.0f, 1.0f );
static idCVar r_volumetricFogDustWarp( "r_volumetricFogDustWarp", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "how far in units a curl field bends the dust's noise ( noise slot 1's warp, the noise in slot 2 and no erode ); 0 = none. The worldspawn's key fog_dust_warp", 0.0f, 1024.0f );
static idCVar r_volumetricFogLights( "r_volumetricFogLights", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "Doom 3 fog lights as media in the froxel fog ( V4, D29 ); V5: the fog takes them in place of the fog pass where the translucents fog themselves ( r_volumetricFogTranslucent ) and the light's volume lies inside the fog's reach; the others keep the fog pass ( the HUD's FOGLIGHTS row )" );
static idCVar r_volumetricFogLightRate( "r_volumetricFogLightRate", "2.3", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "a fog light's sigma_t times its fog distance D in the froxel fog: 2.3 fits the picture the fog pass draws ( the sRGB decode of its table ) over the screen and is its centre column's minimax ( 2.72 the centre's least squares ), 4.632 is its table's rate, 5.54 leaves 1/255 at D ( D21 )", 0.1f, 20.0f );
static idCVar r_volumetricFogTestBox( "r_volumetricFogTestBox", "", CVAR_RENDERER | CVAR_NEW, "test: a box of fog, \"x y z  hx hy hz  D  vx vy vz\" in map units: its centre, its half extents, its distance D ( albedo 1 ) and a velocity per second of game time ( optional ); empty = none" );

// coefficients are stored times this, which keeps thin, dim fog out of the half floats' subnormal range
static const float FROXEL_STORE_SCALE	= 1024.0f;
// S of the biased logarithmic slices, in units: slices are about linear nearer than S and exponential beyond it
static const float FROXEL_SLICE_BIAS	= 16.0f;
// sigma_t * D: the air is 99 percent opaque at the fog distance D, as Doom 3's fog lights are
static const float FROXEL_OPACITY_AT_D	= 4.632f;
// The volumes are half floats, whose largest value is 65504: what the pass stores stays below this bound. A fog
// distance above 0 counts as at least FROXEL_MIN_FOG_DISTANCE units ( sigma_t times the store scale is then
// 47,432 ), and the stored in-scatter is cut at the bound
static const float FROXEL_STORE_MAX		= 60000.0f;
static const float FROXEL_MIN_FOG_DISTANCE	= 0.1f;
// V4: sigma_t * D of the media of the media pass ( the height fog, the dust, the test box; track M's volumes ): 1/255 of the
// light is left after D, the fog lights' rule ( D21, section 6 row 89 ). The global fog keeps V1's FROXEL_OPACITY_AT_D
static const float FOG_MEDIA_OPACITY_AT_D	= 5.54f;
// ( V5: FROXEL_FOGLIGHT_RAMP, the fog lights' enter ramp, is in renderer/VolumetricFog.h, which the frontend shares )
// a bound on the froxels of one volume ( 8 bytes each: 32 MiB a volume; V3 has five such volumes, V4 six and two marks of
// 1 byte a froxel ); a grid past it gets
// wider columns
static const int FROXEL_MAX_CELLS		= 4 << 20;
// the probe's columns: the view's centre, its first column and row ( a corner ), the middle of its left edge
static const int FROXEL_PROBE_COLUMNS	= 3;
// the probe's copy is read this many fog views later: the frame that holds the copy has been handed to the GPU by
// then ( the map itself waits for the GPU )
static const int FROXEL_PROBE_WAIT		= 3;
// the engine framebuffer of the sky mask: the mask as colour, the view's depth image as depth
static const char* const FROXEL_SKYMASK_TARGET = "_fogSkyMask";

// V3: the radical inverse of index in base: the Halton sequence of the jitter ( a base per axis )
static float FroxelHalton( int index, int base )
{
	float result = 0.0f;
	float scale = 1.0f / base;
	for( int i = index; i > 0; i /= base )
	{
		result += ( i % base ) * scale;
		scale /= base;
	}
	return result;
}

// Set by the console command fogProbe, taken by the next fog view. Console commands run on the main thread, where
// the backend runs too, so nothing else touches it
static bool fogProbeRequested = false;

CONSOLE_COMMAND( fogProbe, "prints three columns of the integrated fog volume beside the closed form of the uniform medium, the fogged picture's centre pixel and the GPU time of the fog's two blocks", NULL )
{
	fogProbeRequested = true;
	if( !r_volumetricFog.GetBool() )
	{
		common->Printf( "fogProbe: r_volumetricFog is 0, the probe waits for the first fog view\n" );
	}
}

// V4: the test box ( r_volumetricFogTestBox ): its centre and velocity in map units, its half extents and its distance.
// False where the cvar is empty or does not hold seven numbers, or a size or the distance is not above 0
static bool FroxelTestBox( idVec3& origin, idVec3& halfSize, float& distance, idVec3& velocity )
{
	const char* parms = r_volumetricFogTestBox.GetString();
	if( parms[0] == '\0' )
	{
		return false;
	}
	velocity.Zero();
	const int n = sscanf( parms, "%f %f %f %f %f %f %f %f %f %f", &origin.x, &origin.y, &origin.z, &halfSize.x, &halfSize.y, &halfSize.z, &distance,
						 &velocity.x, &velocity.y, &velocity.z );
	if( n < 10 )
	{
		velocity.Zero();
	}
	return n >= 7 && halfSize.x > 0.0f && halfSize.y > 0.0f && halfSize.z > 0.0f && distance > 0.0f;
}

// V4: whether the media pass has a medium besides the global fog: the height fog, the dust or the test box, on V4's path
// ( r_volumetricFogMedia ); and whether one of them scatters light, so that the fog reads the view's records also where
// the global fog scatters nothing
static bool FroxelHasMediaPass()
{
	idVec3 origin, halfSize, velocity;
	float distance;
	return r_volumetricFogMedia.GetBool() && ( r_volumetricFogHeightDistance.GetFloat() > 0.0f || r_volumetricFogDustDistance.GetFloat() > 0.0f
			|| FroxelTestBox( origin, halfSize, distance, velocity ) );
}

static bool FroxelMediaScatter()
{
	// the test box is asked only whether it is set: this runs on the game thread for each changed entity ( review O2-6 )
	return r_volumetricFogMedia.GetBool() && ( ( r_volumetricFogHeightDistance.GetFloat() > 0.0f && r_volumetricFogHeightAlbedo.GetFloat() > 0.0f )
			|| ( r_volumetricFogDustDistance.GetFloat() > 0.0f && r_volumetricFogDustAlbedo.GetFloat() > 0.0f ) || r_volumetricFogTestBox.GetString()[0] != '\0' );
}

/*
========================
R_VolumetricFogTakesFogLights

V4: whether the fog's inject takes the fog lights as media ( r_volumetricFogLights, on V4's path ). The fog pass still
draws them ( D29 ) until V5 swaps it off for them.
========================
*/
bool R_VolumetricFogTakesFogLights()
{
	return r_volumetricFogMedia.GetBool() && r_volumetricFogLights.GetBool();
}

// V5 ( D21, D29 ): the cvars under which the froxel fog takes a fog light in place of the fog pass: the fog on, the fog
// lights taken, the translucents fogging themselves ( the swap is atomic with them ), no furnace. The frontend asks it
// for every fog light's record ( R_VolumetricFogMayTakeFogLight ); the backend goes by the records
bool R_VolumetricFogSwapsFogLights()
{
	return r_volumetricFog.GetBool() && R_VolumetricFogTakesFogLights() && r_volumetricFogTranslucent.GetBool() && r_volumetricFogFurnace.GetFloat() <= 0.0f;
}

// V5: the fog light's rate as the inject reads it ( mediaParms2.y ), for the frontend's portal distance
float R_VolumetricFogLightRate()
{
	return r_volumetricFogLightRate.GetFloat();
}

// V4: whether the global fog or a medium of the media pass scatters light ( the frontend's question, R_VolumetricFogMarkLights )
bool R_VolumetricFogMediaScatter()
{
	return ( r_volumetricFogDistance.GetFloat() > 0.0f && r_volumetricFogAlbedo.GetFloat() > 0.0f ) || FroxelMediaScatter();
}

// V4: whether the view holds a fog light the inject takes ( record 0, the sun's slot, is never one: review O1-8 ). V5: the
// records carry the frontend's verdict ( RLF_FOG_OFF ), so this is also the question of V4's path ( row 170 a )
static bool FroxelViewHasFogLight( const viewDef_t* viewDef )
{
	if( !R_VolumetricFogTakesFogLights() || viewDef->recordLights == NULL || viewDef->lightListView == NULL )
	{
		return false;
	}
	const int numRecords = Min( viewDef->numRecordLights, MAX_RECORD_LIGHTS );
	for( int r = 1; r < numRecords; r++ )
	{
		if( R_FogCanTakeFogLight( viewDef->recordLights[r] ) )
		{
			return true;
		}
	}
	return false;
}

// Whether a froxel can hold a medium at all: the global fog has a distance, a medium of the media pass exists or the view
// holds a fog light the inject takes ( V4 ), or a debug view wants the pass. In clear air the pass does nothing in a view:
// no sky mask, no kernels, no apply
static bool FroxelHasMedium( const viewDef_t* viewDef )
{
	return r_volumetricFogDistance.GetFloat() > 0.0f || FroxelHasMediaPass() || FroxelViewHasFogLight( viewDef ) || r_volumetricFogDebug.GetInteger() != 0;
}

/*
========================
R_VolumetricFogRecordReach

How far along the view axis the fog's inject reads a view's light records: the fog's far end; 0 where it reads
none, because the fog is off, the air is clear or scatters nothing ( an albedo of 0; V4: in every medium ), or the furnace is on ( the
uniform test medium, lit by nothing else ). The frontend asks before it builds a view's records for the fog alone and before it keeps a light alive for
the fog ( R_NumberRecordLights ), so both sides go by one rule. They read the cvars a frame apart: the pass copes
with a view that has no records, and records that nobody reads cost one frame's work.
========================
*/
float R_VolumetricFogRecordReach()
{
	const bool globalScatters = r_volumetricFogDistance.GetFloat() > 0.0f && r_volumetricFogAlbedo.GetFloat() > 0.0f;
	if( !r_volumetricFog.GetBool() || !( globalScatters || FroxelMediaScatter() || R_VolumetricFogTakesFogLights() ) || r_volumetricFogFurnace.GetFloat() > 0.0f )
	{
		return 0.0f;
	}
	return idMath::ClampFloat( 64.0f, 65536.0f, r_volumetricFogFar.GetFloat() );
}

VolumetricFogPass::VolumetricFogPass( nvrhi::IDevice* device, CommonRenderPasses* commonPasses )
	: m_Device( device )
	, commonRenderPasses( commonPasses )
	, m_GridWidth( 0 )
	, m_GridHeight( 0 )
	, m_GridSlices( 0 )
	, m_ColumnPixels( 0 )
	, m_ViewGridWidth( 0 )
	, m_ViewGridHeight( 0 )
	, m_SliceStep( 0.0f )
	, m_NumMedia( 0 )
	, m_PrevBoxValid( false )
	, m_ResolvedIndex( 0 )
	, m_PrevNumRecords( 0 )
	, m_HistoryValid( false )
	, m_HistoryFrame( 0 )
	, m_HistorySliceStep( 0.0f )
	, m_HistoryMedium( 0.0f )
	, m_JitterIndex( 0 )
	, m_ApplyColor( NULL )
	, m_RenderedView( NULL )
	, m_RenderedFogLights( false )
	, m_FogLightPath( false )
	, m_SkyMask( NULL )
	, m_SkyMaskView( NULL )
	, m_SkyMaskEmpty( false )
	, m_ProbeWait( 0 )
	, m_ProbeStaged( false )
	, m_ProbeHasPixel( false )
	, m_ProbeLateColor( NULL )
	, m_ProbeLateView( NULL )
	, m_ProbeHasPixelLate( false )
	, m_ProbeSlices( 0 )
	, m_ProbeSliceStep( 0.0f )
	, m_ProbeSigmaT( 0.0f )
	, m_ProbeSource( 0.0f )
	, m_ProbeLit( false )
	, m_ProbeRecords( 0 )
	, m_ProbeTakeable( 0 )
	, m_ProbeCull( 0 )
	, m_ProbeSigmaS( 0.0f )
	, m_ProbeMedia( false )
	, m_ProbeMediaClosed( false )
	, m_ProbeFogLights( false )
	, m_ProbeFurnace( 0.0f )
	, m_ProbeNumMedia( 0 )
	, m_ProbeSkip( false )
	, m_ProbeHistory( false )
{
	memset( m_InjectSetKey, 0, sizeof( m_InjectSetKey ) );
	memset( m_HistoryGrid, 0, sizeof( m_HistoryGrid ) );
	memset( m_PrevFogBits, 0, sizeof( m_PrevFogBits ) );
	m_HistoryMVP = renderMatrix_identity;
	m_HistoryOrigin.Zero();

	// V3: last fog view's records, and the indices of the lights that moved: the pass's own, made once
	nvrhi::BufferDesc recordsDesc;
	recordsDesc.byteSize = MAX_RECORD_LIGHTS * sizeof( recordLight_t );
	recordsDesc.structStride = sizeof( recordLight_t );
	recordsDesc.debugName = "Fog/PrevRecords";
	recordsDesc.initialState = nvrhi::ResourceStates::ShaderResource;
	recordsDesc.keepInitialState = true;
	m_PrevRecords = device->createBuffer( recordsDesc );
	nvrhi::BufferDesc movedDesc;
	movedDesc.byteSize = MAX_RECORD_LIGHTS * sizeof( int );
	movedDesc.structStride = sizeof( int );
	movedDesc.debugName = "Fog/MovedRecords";
	movedDesc.initialState = nvrhi::ResourceStates::ShaderResource;
	movedDesc.keepInitialState = true;
	m_MovedRecords = device->createBuffer( movedDesc );
	nvrhi::BufferDesc casterDesc;
	casterDesc.byteSize = MAX_RECORD_LIGHTS * 2 * FOG_CASTER_BOXES * sizeof( idVec4 );
	casterDesc.structStride = sizeof( idVec4 );
	casterDesc.debugName = "Fog/CasterBounds";
	casterDesc.initialState = nvrhi::ResourceStates::ShaderResource;
	casterDesc.keepInitialState = true;
	m_CasterBounds = device->createBuffer( casterDesc );

	// V4: the view's media and the boxes where a medium moved, the pass's own, written in every fog view that has media
	nvrhi::BufferDesc mediaDesc;
	mediaDesc.byteSize = FOG_MEDIA_MAX * sizeof( fogMedium_t );
	mediaDesc.structStride = sizeof( fogMedium_t );
	mediaDesc.debugName = "Fog/MediaList";
	mediaDesc.initialState = nvrhi::ResourceStates::ShaderResource;
	mediaDesc.keepInitialState = true;
	m_MediaList = device->createBuffer( mediaDesc );
	nvrhi::BufferDesc mediaMovedDesc;
	mediaMovedDesc.byteSize = 4 * FOG_MEDIA_MAX * sizeof( idVec4 );
	mediaMovedDesc.structStride = sizeof( idVec4 );
	mediaMovedDesc.debugName = "Fog/MediaMoved";
	mediaMovedDesc.initialState = nvrhi::ResourceStates::ShaderResource;
	mediaMovedDesc.keepInitialState = true;
	m_MediaMoved = device->createBuffer( mediaMovedDesc );
	memset( m_MediaCopy, 0, sizeof( m_MediaCopy ) );
	memset( m_ProbeMedium, 0, sizeof( m_ProbeMedium ) );
	memset( m_ProbeMediaList, 0, sizeof( m_ProbeMediaList ) );
	memset( m_ProbeSample, 0, sizeof( m_ProbeSample ) );

	nvrhi::BufferDesc constantBufferDesc;
	constantBufferDesc.byteSize = sizeof( froxelConstants_t );
	constantBufferDesc.debugName = "FroxelConstants";
	constantBufferDesc.isConstantBuffer = true;
	constantBufferDesc.isVolatile = true;
	constantBufferDesc.maxVersions = c_MaxRenderPassConstantBufferVersions;
	m_ConstantBuffer = device->createBuffer( constantBufferDesc );

	// the kernels are found here, at the pass's first use ( F17 ): a run with the fog off never loads them
	InitInject();
	InitResolve();
	InitColumns();
	InitMedia();
	InitKernel( m_Integrate, "builtin/volumetrics/froxel_integrate", true );

	const idList<shaderMacro_t> noMacros;
	m_ApplyVS = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/volumetrics/froxel_apply", SHADER_STAGE_VERTEX, "", noMacros, true, LAYOUT_DRAW_VERT ) );
	m_ApplyPS = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/volumetrics/froxel_apply", SHADER_STAGE_FRAGMENT, "", noMacros, true, LAYOUT_DRAW_VERT ) );

	// the fog set's three items at the registers shaders/froxel_fog.inc.hlsl gives them, the scene depth and the
	// sky mask
	nvrhi::BindingLayoutDesc applyLayoutDesc;
	applyLayoutDesc.visibility = nvrhi::ShaderType::All;
	applyLayoutDesc.bindings =
	{
		nvrhi::BindingLayoutItem::VolatileConstantBuffer( 1 ),
		nvrhi::BindingLayoutItem::Texture_SRV( 15 ),
		nvrhi::BindingLayoutItem::Sampler( 5 ),
		nvrhi::BindingLayoutItem::Texture_SRV( 0 ),
		nvrhi::BindingLayoutItem::Texture_SRV( 1 )
	};
	m_ApplyLayout = device->createBindingLayout( applyLayoutDesc );
}

VolumetricFogPass::~VolumetricFogPass()
{
	ReleaseVolume();
}

/*
========================
VolumetricFogPass::ReleaseVolume

Called where the integrated volume is about to die: the grid changed, or the pass goes. Since its last apply the
engine's fog sets name the volume ( backEnd.froxelVolume ), and the engine's binding cache keeps a set, and the
textures the set names, for as long as the cache lives. So the fog set gets the dummy volume back and the cache
starts again. A pass that never applied has handed out nothing and clears nothing.
========================
*/
void VolumetricFogPass::ReleaseVolume()
{
	if( m_Integrated && backEnd.froxelVolume == m_Integrated )
	{
		backEnd.froxelVolume = backEnd.froxelDummyVolume;
		backEnd.bindingCache.Clear();
	}
}

/*
========================
VolumetricFogPass::InitKernel

One of the two kernels behind the inject ( the resolve, the integrate ): its shader, its layout and its pipeline.
Each reads the constants at b1 and writes one volume at u0; a kernel that reads a volume has it at t0.
========================
*/
void VolumetricFogPass::InitKernel( kernel_t& kernel, const char* shaderName, bool readsVolume )
{
	const idList<shaderMacro_t> noMacros;
	kernel.shader = renderProgManager.GetShader( renderProgManager.FindShader( shaderName, SHADER_STAGE_COMPUTE, "", noMacros, true, LAYOUT_DRAW_VERT ) );

	nvrhi::BindingLayoutDesc layoutDesc;
	layoutDesc.visibility = nvrhi::ShaderType::Compute;
	layoutDesc.bindings.push_back( nvrhi::BindingLayoutItem::VolatileConstantBuffer( 1 ) );
	if( readsVolume )
	{
		layoutDesc.bindings.push_back( nvrhi::BindingLayoutItem::Texture_SRV( 0 ) );
	}
	layoutDesc.bindings.push_back( nvrhi::BindingLayoutItem::Texture_UAV( 0 ) );
	kernel.layout = m_Device->createBindingLayout( layoutDesc );

	nvrhi::ComputePipelineDesc pipelineDesc;
	pipelineDesc.bindingLayouts = { kernel.layout };
	pipelineDesc.CS = kernel.shader;
	kernel.pipeline = m_Device->createComputePipeline( pipelineDesc );
}

/*
========================
VolumetricFogPass::InitResolve

The resolve ( V3 ): this frame's injected volume, the state the inject wrote and last frame's resolved volume, read
through a linear sampler, into this frame's resolved volume. Its sets are made with the volumes ( CheckVolumes ), one
for each turn of the two resolved volumes.
========================
*/
void VolumetricFogPass::InitResolve()
{
	const idList<shaderMacro_t> noMacros;
	m_Resolve.shader = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/volumetrics/froxel_resolve", SHADER_STAGE_COMPUTE, "", noMacros, true, LAYOUT_DRAW_VERT ) );

	nvrhi::BindingLayoutDesc layoutDesc;
	layoutDesc.visibility = nvrhi::ShaderType::Compute;
	layoutDesc.bindings =
	{
		nvrhi::BindingLayoutItem::VolatileConstantBuffer( 1 ),
		nvrhi::BindingLayoutItem::Texture_SRV( 0 ),
		nvrhi::BindingLayoutItem::Texture_SRV( 1 ),
		nvrhi::BindingLayoutItem::Texture_SRV( 2 ),
		nvrhi::BindingLayoutItem::Sampler( 0 ),
		nvrhi::BindingLayoutItem::Texture_UAV( 0 ),
		nvrhi::BindingLayoutItem::Texture_SRV( 3 ),		// V4: last frame's marks
		nvrhi::BindingLayoutItem::Texture_UAV( 1 )		// V4: this frame's
	};
	m_Resolve.layout = m_Device->createBindingLayout( layoutDesc );

	nvrhi::ComputePipelineDesc pipelineDesc;
	pipelineDesc.bindingLayouts = { m_Resolve.layout };
	pipelineDesc.CS = m_Resolve.shader;
	m_Resolve.pipeline = m_Device->createComputePipeline( pipelineDesc );
}

/*
========================
VolumetricFogPass::InitColumns

The columns ( V3 ): the scene's depth as the apply reads it and the sky mask into the farthest surface of each column
of the grid. Its set names the engine's depth and the sky mask, whose objects change from frame to frame, and comes
from the engine's binding cache.
========================
*/
void VolumetricFogPass::InitColumns()
{
	const idList<shaderMacro_t> noMacros;
	m_ColumnsKernel.shader = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/volumetrics/froxel_columns", SHADER_STAGE_COMPUTE, "", noMacros, true, LAYOUT_DRAW_VERT ) );

	nvrhi::BindingLayoutDesc layoutDesc;
	layoutDesc.visibility = nvrhi::ShaderType::Compute;
	layoutDesc.bindings =
	{
		nvrhi::BindingLayoutItem::VolatileConstantBuffer( 1 ),
		nvrhi::BindingLayoutItem::Texture_SRV( 0 ),
		nvrhi::BindingLayoutItem::Texture_SRV( 1 ),
		nvrhi::BindingLayoutItem::Texture_UAV( 0 )
	};
	m_ColumnsKernel.layout = m_Device->createBindingLayout( layoutDesc );

	nvrhi::ComputePipelineDesc pipelineDesc;
	pipelineDesc.bindingLayouts = { m_ColumnsKernel.layout };
	pipelineDesc.CS = m_ColumnsKernel.shader;
	m_ColumnsKernel.pipeline = m_Device->createComputePipeline( pipelineDesc );
}

/*
========================
VolumetricFogPass::InitMedia

The media pass ( V4 ): the media list at t0 and the columns' farthest surfaces at t1 into the media volume at u0, with
the constants at b1. Its set names the pass's own objects alone and is made with the volumes ( CheckVolumes ).
========================
*/
void VolumetricFogPass::InitMedia()
{
	const idList<shaderMacro_t> noMacros;
	m_MediaKernel.shader = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/volumetrics/froxel_media", SHADER_STAGE_COMPUTE, "", noMacros, true, LAYOUT_DRAW_VERT ) );
	nvrhi::BindingLayoutDesc layoutDesc;
	layoutDesc.visibility = nvrhi::ShaderType::Compute;
	layoutDesc.bindings =
	{
		nvrhi::BindingLayoutItem::VolatileConstantBuffer( 1 ),
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 0 ),
		nvrhi::BindingLayoutItem::Texture_SRV( 1 ),
		nvrhi::BindingLayoutItem::Texture_UAV( 0 )
	};
	m_MediaKernel.layout = m_Device->createBindingLayout( layoutDesc );
	nvrhi::ComputePipelineDesc pipelineDesc;
	pipelineDesc.bindingLayouts = { m_MediaKernel.layout };
	pipelineDesc.CS = m_MediaKernel.shader;
	m_MediaKernel.pipeline = m_Device->createComputePipeline( pipelineDesc );
}

/*
========================
VolumetricFogPass::InitInject

The inject kernel ( V2 ): its shader, its layout and its pipeline. Besides the constants at b1 and the injected
volume at u0 it reads the light lists' tables as the frontend built them and the backend uploaded them for the
view ( R_BuildShadowRows ): the light records at t0, the shadow rows at t1, the Z-bins at t2 and the tile bits at
t3; and the three light images of the forward layouts at t4 to t6, through the black-bordered sampler at s0.
========================
*/
void VolumetricFogPass::InitInject()
{
	const idList<shaderMacro_t> noMacros;
	m_Inject.shader = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/volumetrics/froxel_inject", SHADER_STAGE_COMPUTE, "", noMacros, true, LAYOUT_DRAW_VERT ) );

	nvrhi::BindingLayoutDesc layoutDesc;
	layoutDesc.visibility = nvrhi::ShaderType::Compute;
	layoutDesc.bindings =
	{
		nvrhi::BindingLayoutItem::VolatileConstantBuffer( 1 ),
		nvrhi::BindingLayoutItem::Texture_UAV( 0 ),
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 0 ),
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 1 ),
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 2 ),
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 3 ),
		nvrhi::BindingLayoutItem::Texture_SRV( 4 ),
		nvrhi::BindingLayoutItem::Texture_SRV( 5 ),
		nvrhi::BindingLayoutItem::Texture_SRV( 6 ),
		nvrhi::BindingLayoutItem::Texture_SRV( 7 ),
		nvrhi::BindingLayoutItem::Sampler( 0 ),
		nvrhi::BindingLayoutItem::Sampler( 1 ),
		nvrhi::BindingLayoutItem::Texture_UAV( 1 ),
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 8 ),
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 9 ),
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 10 ),
		nvrhi::BindingLayoutItem::Texture_SRV( 11 ),
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 12 ),
		nvrhi::BindingLayoutItem::Texture_SRV( 13 ),			// V4: the media volume
		nvrhi::BindingLayoutItem::StructuredBuffer_SRV( 14 )	// V4: the boxes where a medium moved
	};
	m_Inject.layout = m_Device->createBindingLayout( layoutDesc );

	nvrhi::ComputePipelineDesc pipelineDesc;
	pipelineDesc.bindingLayouts = { m_Inject.layout };
	pipelineDesc.CS = m_Inject.shader;
	m_Inject.pipeline = m_Device->createComputePipeline( pipelineDesc );
}

/*
========================
VolumetricFogPass::CheckInjectSet

The inject's binding set. Besides the pass's own constants, volumes and buffers it names nine objects of the engine:
the light lists' four tables ( the backend makes the tile bits again when a view needs more words ), the three light
images ( for an image that has not loaded yet the white image stands in, and once it has loaded it is another
texture ), the shadow atlas ( made again when r_shadowMapAtlasSize changes ) and the list of vanished lights
( V3 ). So the set is made again when one of the nine is another object than the set was made for; CheckVolumes drops
it with the old volumes. A map load, a resize and vid_restart delete the pass with the backend's caches ( ClearCaches ),
and the next pass makes its own set; reloadImages keeps the pass, and the key catches the images it makes again. While
the set lives it holds its nine objects, so none of their addresses can be another object's. Returns false while one of
them does not exist: the view then gets no fog.
========================
*/
bool VolumetricFogPass::CheckInjectSet()
{
	nvrhi::ITexture* shadowAtlas = ( nvrhi::ITexture* )globalImages->shadowAtlasImage->GetTextureID();
	nvrhi::IResource* key[9] =
	{
		backEnd.recordLightsBuffer.Get(), backEnd.shadowRowsBuffer.Get(), backEnd.zBinsBuffer.Get(), backEnd.tileBitsBuffer.Get(),
		R_ForwardDefaultLightImage( 0 ), R_ForwardDefaultLightImage( 1 ), R_ForwardDefaultLightImage( 2 ), shadowAtlas,
		backEnd.vanishedPrevBuffer.Get()
	};
	for( int i = 0; i < 9; i++ )
	{
		if( key[i] == NULL )
		{
			return false;
		}
	}
	if( m_Inject.bindingSet && memcmp( key, m_InjectSetKey, sizeof( key ) ) == 0 )
	{
		return true;
	}

	nvrhi::BindingSetDesc injectDesc;
	injectDesc.bindings =
	{
		nvrhi::BindingSetItem::ConstantBuffer( 1, m_ConstantBuffer ),
		nvrhi::BindingSetItem::Texture_UAV( 0, m_Injected ),
		nvrhi::BindingSetItem::StructuredBuffer_SRV( 0, backEnd.recordLightsBuffer ),
		nvrhi::BindingSetItem::StructuredBuffer_SRV( 1, backEnd.shadowRowsBuffer ),
		nvrhi::BindingSetItem::StructuredBuffer_SRV( 2, backEnd.zBinsBuffer ),
		nvrhi::BindingSetItem::StructuredBuffer_SRV( 3, backEnd.tileBitsBuffer ),
		nvrhi::BindingSetItem::Texture_SRV( 4, R_ForwardDefaultLightImage( 0 ) ),
		nvrhi::BindingSetItem::Texture_SRV( 5, R_ForwardDefaultLightImage( 1 ) ),
		nvrhi::BindingSetItem::Texture_SRV( 6, R_ForwardDefaultLightImage( 2 ) ),
		nvrhi::BindingSetItem::Texture_SRV( 7, shadowAtlas ),
		nvrhi::BindingSetItem::Sampler( 0, commonRenderPasses->m_LinearBorderSampler ),
		nvrhi::BindingSetItem::Sampler( 1, commonRenderPasses->m_LinearClampCompareSampler ),
		nvrhi::BindingSetItem::Texture_UAV( 1, m_State ),
		nvrhi::BindingSetItem::StructuredBuffer_SRV( 8, m_PrevRecords ),
		nvrhi::BindingSetItem::StructuredBuffer_SRV( 9, backEnd.vanishedPrevBuffer ),
		nvrhi::BindingSetItem::StructuredBuffer_SRV( 10, m_MovedRecords ),
		nvrhi::BindingSetItem::Texture_SRV( 11, m_Columns ),
		nvrhi::BindingSetItem::StructuredBuffer_SRV( 12, m_CasterBounds ),
		nvrhi::BindingSetItem::Texture_SRV( 13, m_Media ),
		nvrhi::BindingSetItem::StructuredBuffer_SRV( 14, m_MediaMoved )
	};
	m_Inject.bindingSet = m_Device->createBindingSet( injectDesc, m_Inject.layout );
	memcpy( m_InjectSetKey, key, sizeof( key ) );
	return m_Inject.bindingSet != nullptr;
}

/*
========================
VolumetricFogPass::CheckVolumes

Makes the three volumes and the binding sets of the resolve and the integrate for a grid ( the inject's set is
CheckInjectSet's, which finds none after this and makes it ), when the pass has none or the grid changed
( the render size's grid: a grid cvar changed ). In every frame the kernels write every froxel the lookup's
filter can fetch ( Render: the view's grid and one column and row more ), so a new volume is not cleared.
Returns false when there is no grid to fill or the volumes could not be made: the view then stays unfogged, and
the same grid is not tried again.
========================
*/
bool VolumetricFogPass::CheckVolumes( int gridWidth, int gridHeight, int gridSlices )
{
	if( gridWidth < 1 || gridHeight < 1 || gridSlices < 1 )
	{
		return false;
	}
	if( gridWidth == m_GridWidth && gridHeight == m_GridHeight && gridSlices == m_GridSlices )
	{
		return m_Integrated != nullptr;		// this grid's volumes exist, or the device could not make them
	}
	// the grid changed: the old volumes go, and the fog set binds the dummy until this view's apply
	ReleaseVolume();

	nvrhi::TextureDesc desc;
	desc.dimension = nvrhi::TextureDimension::Texture3D;
	desc.width = gridWidth;
	desc.height = gridHeight;
	desc.depth = gridSlices;
	desc.mipLevels = 1;
	desc.format = nvrhi::Format::RGBA16_FLOAT;
	desc.isUAV = true;
	desc.initialState = nvrhi::ResourceStates::ShaderResource;
	desc.keepInitialState = true;

	desc.debugName = "Fog/Injected";
	m_Injected = m_Device->createTexture( desc );
	desc.debugName = "Fog/Resolved0";
	m_Resolved[0] = m_Device->createTexture( desc );
	desc.debugName = "Fog/Resolved1";
	m_Resolved[1] = m_Device->createTexture( desc );
	desc.debugName = "Fog/State";
	m_State = m_Device->createTexture( desc );
	desc.debugName = "Fog/Integrated";
	m_Integrated = m_Device->createTexture( desc );
	// V4: the media pass's sum ( 8 bytes a froxel, like the others ), and the skip's two marks, one byte a froxel each ( row 93 ):
	// R8_UNORM's linear filtering is required on every device, R32_FLOAT's is not ( review O2-9 ), and the validation layer
	// accepts the storage ( RUN v4-2026-11-20, the break x_v4r8; consultant CN1-1 had read the feature table wrong )
	desc.debugName = "Fog/Media";
	m_Media = m_Device->createTexture( desc );
	desc.format = nvrhi::Format::R8_UNORM;
	desc.debugName = "Fog/Mark0";
	m_Mark[0] = m_Device->createTexture( desc );
	desc.debugName = "Fog/Mark1";
	m_Mark[1] = m_Device->createTexture( desc );
	m_HistoryValid = false;					// the new volumes hold no fog yet
	// V3: the columns' farthest surfaces, a texel per column
	nvrhi::TextureDesc columnsDesc;
	columnsDesc.dimension = nvrhi::TextureDimension::Texture2D;
	columnsDesc.width = gridWidth;
	columnsDesc.height = gridHeight;
	columnsDesc.format = nvrhi::Format::R32_FLOAT;
	columnsDesc.isUAV = true;
	columnsDesc.initialState = nvrhi::ResourceStates::ShaderResource;
	columnsDesc.keepInitialState = true;
	columnsDesc.debugName = "Fog/Columns";
	m_Columns = m_Device->createTexture( columnsDesc );
	if( !m_Injected || !m_Resolved[0] || !m_Resolved[1] || !m_State || !m_Integrated || !m_Columns || !m_Media || !m_Mark[0] || !m_Mark[1] )
	{
		// The device could not make a volume ( out of memory ): no fog. The kernels' sets let the old volumes go, and
		// the grid is kept, so that it is not tried again in every view
		m_Injected = nullptr;
		m_Resolved[0] = nullptr;
		m_Resolved[1] = nullptr;
		m_State = nullptr;
		m_Integrated = nullptr;
		m_Columns = nullptr;
		m_Media = nullptr;
		m_Mark[0] = nullptr;
		m_Mark[1] = nullptr;
		m_MediaKernel.bindingSet = nullptr;
		m_Inject.bindingSet = nullptr;
		for( int i = 0; i < 2; i++ )
		{
			m_ResolveSets[i] = nullptr;
			m_IntegrateSets[i] = nullptr;
		}
		m_GridWidth = gridWidth;
		m_GridHeight = gridHeight;
		m_GridSlices = gridSlices;
		idLib::Warning( "VolumetricFogPass: the device could not make the fog's volumes of %d x %d x %d froxels, no fog until the grid changes", gridWidth, gridHeight, gridSlices );
		return false;
	}

	m_Inject.bindingSet = nullptr;			// it named the old injected volume

	// V3: a set of each kernel for each turn of the two resolved volumes: in turn i the resolve writes m_Resolved[ i ]
	// from the other one, and the integrate reads m_Resolved[ i ]
	for( int i = 0; i < 2; i++ )
	{
		nvrhi::BindingSetDesc resolveDesc;
		resolveDesc.bindings =
		{
			nvrhi::BindingSetItem::ConstantBuffer( 1, m_ConstantBuffer ),
			nvrhi::BindingSetItem::Texture_SRV( 0, m_Injected ),
			nvrhi::BindingSetItem::Texture_SRV( 1, m_State ),
			nvrhi::BindingSetItem::Texture_SRV( 2, m_Resolved[ 1 - i ] ),
			nvrhi::BindingSetItem::Sampler( 0, commonRenderPasses->m_LinearClampSampler ),
			nvrhi::BindingSetItem::Texture_UAV( 0, m_Resolved[ i ] ),
			nvrhi::BindingSetItem::Texture_SRV( 3, m_Mark[ 1 - i ] ),
			nvrhi::BindingSetItem::Texture_UAV( 1, m_Mark[ i ] )
		};
		m_ResolveSets[i] = m_Device->createBindingSet( resolveDesc, m_Resolve.layout );

		nvrhi::BindingSetDesc integrateDesc;
		integrateDesc.bindings =
		{
			nvrhi::BindingSetItem::ConstantBuffer( 1, m_ConstantBuffer ),
			nvrhi::BindingSetItem::Texture_SRV( 0, m_Resolved[ i ] ),
			nvrhi::BindingSetItem::Texture_UAV( 0, m_Integrated )
		};
		m_IntegrateSets[i] = m_Device->createBindingSet( integrateDesc, m_Integrate.layout );
	}

	// V4: the media pass's set: the media list, the columns and the media volume, all the pass's own
	nvrhi::BindingSetDesc mediaSetDesc;
	mediaSetDesc.bindings =
	{
		nvrhi::BindingSetItem::ConstantBuffer( 1, m_ConstantBuffer ),
		nvrhi::BindingSetItem::StructuredBuffer_SRV( 0, m_MediaList ),
		nvrhi::BindingSetItem::Texture_SRV( 1, m_Columns ),
		nvrhi::BindingSetItem::Texture_UAV( 0, m_Media )
	};
	m_MediaKernel.bindingSet = m_Device->createBindingSet( mediaSetDesc, m_MediaKernel.layout );
	m_ProbeWait = 0;						// a probe under way was of the old volume

	m_GridWidth = gridWidth;
	m_GridHeight = gridHeight;
	m_GridSlices = gridSlices;
	return true;
}

/*
========================
VolumetricFogPass::DrawSkyMask

Marks the pixels of the fog view that show a sky surface ( a material of sort SS_PORTAL_SKY: it writes depth, so by
its depth the sky would stand as near as the brush that carries it ). The apply fogs a marked pixel as infinitely
far ( D52 ). Called right behind the depth prepass: the sky surfaces are drawn once more with the engine's colour
program, depth test EQUAL and no depth write, into a mask that shares the view's depth image, so a mark stands
exactly where the prepass left a sky surface visible. It opens no main block; the caller resets the graphics
state cache behind it.
========================
*/
void VolumetricFogPass::DrawSkyMask( nvrhi::ICommandList* commandList, const viewDef_t* viewDef )
{
	m_SkyMask = NULL;
	m_SkyMaskView = NULL;

	if( !FroxelHasMedium( viewDef ) )
	{
		return;					// clear air: the pass does nothing in this view
	}

	nvrhi::ITexture* depth = globalImages->currentDepthImage->GetTextureHandle().Get();
	if( depth == NULL )
	{
		return;
	}

	// The target is an engine framebuffer, because the engine's draw path keys its pipelines by one. The engine
	// deletes every framebuffer on a resize, behind ClearCaches, so no pointer to it is kept past this call; a pass
	// made later ( a map load, the cvar ) finds and takes over the one an earlier pass made
	Framebuffer* target = Framebuffer::Find( FROXEL_SKYMASK_TARGET );
	if( target == NULL )
	{
		const nvrhi::TextureDesc& depthDesc = depth->getDesc();

		nvrhi::TextureDesc desc;
		desc.width = depthDesc.width;
		desc.height = depthDesc.height;
		desc.format = nvrhi::Format::R8_UNORM;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "Fog/SkyMask";
		nvrhi::TextureHandle mask = m_Device->createTexture( desc );

		target = new Framebuffer( FROXEL_SKYMASK_TARGET, nvrhi::FramebufferDesc().addColorAttachment( mask ).setDepthAttachment( depth ) );
		m_SkyMaskEmpty = false;				// a new texture holds anything
	}
	const nvrhi::FramebufferDesc& targetDesc = target->GetApiObject()->getDesc();
	if( targetDesc.depthAttachment.texture != depth )
	{
		// Made for another depth image: no mask, and the apply then leaves the view unfogged. The engine re-makes
		// the depth image only where it deletes every framebuffer too ( Framebuffer::ResizeFramebuffers ), so this
		// is the fault of a later change: say so, once
		static bool warned = false;
		if( !warned )
		{
			warned = true;
			idLib::Warning( "VolumetricFogPass: the framebuffer %s belongs to another depth image, the view gets no fog", FROXEL_SKYMASK_TARGET );
		}
		return;
	}
	nvrhi::ITexture* mask = targetDesc.colorAttachments[0].texture;

	// cleared only when it holds marks, or when this pass does not know what it holds ( a mask taken over )
	if( !m_SkyMaskEmpty )
	{
		commandList->clearTextureFloat( mask, nvrhi::AllSubresources, nvrhi::Color( 0.0f ) );
		m_SkyMaskEmpty = true;
	}

	Framebuffer* previous = backEnd.currentFrameBuffer;
	bool bound = false;
	for( int i = 0; i < viewDef->numDrawSurfs; i++ )
	{
		const drawSurf_t* surf = viewDef->drawSurfs[i];
		const idMaterial* material = surf->material;
		if( material == NULL || material->GetSort() != SS_PORTAL_SKY || material->Coverage() == MC_TRANSLUCENT )
		{
			continue;			// not a sky surface, or one that left no depth
		}
		if( !bound )
		{
			target->Bind();
			backEnd.GL_State( GLS_DEPTHMASK | GLS_DEPTHFUNC_EQUAL );
			backEnd.GL_Color( 1.0f, 1.0f, 1.0f, 1.0f );
			backEnd.currentSpace = NULL;
			bound = true;
			m_SkyMaskEmpty = false;
		}
		if( surf->space != backEnd.currentSpace )
		{
			RB_SetMVP( surf->space->mvp );
			backEnd.currentSpace = surf->space;
		}
		if( surf->jointCache )
		{
			renderProgManager.BindShader_ColorSkinned();
		}
		else
		{
			renderProgManager.BindShader_Color();
		}
		backEnd.DrawElementsWithCounters( surf );
	}
	if( bound && previous != NULL )
	{
		previous->Bind();
	}

	m_SkyMask = mask;
	m_SkyMaskView = viewDef;
}

/*
========================
VolumetricFogPass::Render

The three kernels of the fog view, in the backend's MRB_VOLUMETRICS block. The medium is the same everywhere
( the global fog ); the inject lights it with the view's ambient light and its light records ( V2 ), or with the
furnace radiance alone; the resolve copies.
========================
*/
void VolumetricFogPass::Render( nvrhi::ICommandList* commandList, const viewDef_t* viewDef )
{
	m_RenderedView = NULL;

	// a probe's copy is read some fog views after it was made
	if( m_ProbeWait > 0 && --m_ProbeWait == 0 )
	{
		ProbeRead();
	}

	// clear air: no medium in any froxel, so the kernels and the apply have nothing to do and the view stays unfogged
	if( !FroxelHasMedium( viewDef ) )
	{
		if( fogProbeRequested )
		{
			fogProbeRequested = false;
			common->Printf( "fogprobe: the air is clear ( no global fog, no medium, no fog light taken ), there is nothing to read\n" );
		}
		return;
	}
	// no sky mask for this view ( DrawSkyMask could not make one ): the apply will not run, so nothing is filled
	if( m_SkyMaskView != viewDef || m_SkyMask == NULL )
	{
		return;
	}

	// The global fog: sigma_t from its distance, sigma_s from the albedo, the in-scatter per unit from the furnace.
	// What goes into the half-float volumes stays finite ( FROXEL_STORE_MAX ): a distance above 0 counts as at least
	// FROXEL_MIN_FOG_DISTANCE, and the in-scatter of a furnace too bright for the store is cut
	const float fogDistance = r_volumetricFogDistance.GetFloat();
	const float sigmaT = ( fogDistance > 0.0f ) ? FROXEL_OPACITY_AT_D / Max( fogDistance, FROXEL_MIN_FOG_DISTANCE ) : 0.0f;
	const float sigmaS = sigmaT * idMath::ClampFloat( 0.0f, 1.0f, r_volumetricFogAlbedo.GetFloat() );
	const float source = Min( sigmaS * Max( 0.0f, r_volumetricFogFurnace.GetFloat() ), FROXEL_STORE_MAX / FROXEL_STORE_SCALE );

	// V2: the fog is lit by the view's ambient light and its light records, unless the furnace is on: a furnace
	// radiance above 0 means the uniform test medium lit by nothing else, whose closed form the probe prints. The
	// records are the frontend's, built for this view while r_volumetricFog is on ( R_NumberRecordLights ) and
	// uploaded by the backend right before this pass ( R_BuildShadowRows ). A view that was built before the cvar
	// went up has none, and its fog is lit by the ambient alone; air that scatters nothing ( an albedo of 0, or a
	// debug view in clear air ) reads none
	const bool furnace = r_volumetricFogFurnace.GetFloat() > 0.0f;
	const lightListView_t* llv = viewDef->lightListView;
	// V4: the fog lights are taken as media on V4's path ( r_volumetricFogLights ), never under the furnace. V5: only where the
	// view holds a fog light the frontend's verdict left on ( row 170 a: a view without one runs V3's path and pays nothing )
	const bool takeFogLights = !furnace && FroxelViewHasFogLight( viewDef );
	// V4: the lights only where something scatters them ( review O1-2 ): the global fog, a medium of the media pass, a fog light
	// with an albedo above 0
	const bool takeLights = sigmaS > 0.0f || FroxelMediaScatter() || ( takeFogLights && viewDef->recordLights != NULL
							  && R_FogLightsScatter( viewDef->recordLights, Min( viewDef->numRecordLights, MAX_RECORD_LIGHTS ) ) );
	const int numRecords = ( !furnace && ( sigmaS > 0.0f || FroxelMediaScatter() || takeFogLights ) && llv != NULL && viewDef->recordLights != NULL ) ? viewDef->numRecordLights : 0;
	const int cullMode = idMath::ClampInt( 0, 2, r_volumetricFogCull.GetInteger() );
	const float anisotropy = idMath::ClampFloat( -0.9f, 0.9f, r_volumetricFogAnisotropy.GetFloat() );

	// The volumes are sized by the render size, which is the sky mask's, not by the view: a view that changes its
	// size ( the engine's dynamic resolution ) fills a part of them and nothing is made again. A grid of more than
	// FROXEL_MAX_CELLS froxels gets wider columns
	const nvrhi::TextureDesc& renderDesc = m_SkyMask->getDesc();
	// V5: a quality level sets both; -1 the two cvars
	static const int qualityColumnPixels[3] = { 16, 12, 8 };
	static const int qualitySlices[3] = { 48, 64, 96 };
	const int quality = idMath::ClampInt( -1, 2, r_volumetricFogQuality.GetInteger() );
	const int gridSlices = ( quality >= 0 ) ? qualitySlices[ quality ] : idMath::ClampInt( 8, 256, r_volumetricFogSlices.GetInteger() );
	const int wantedColumnPixels = ( quality >= 0 ) ? qualityColumnPixels[ quality ] : idMath::ClampInt( 4, 64, r_volumetricFogColumnPixels.GetInteger() );
	int columnPixels = wantedColumnPixels;
	while( columnPixels < 64 && ( ( ( int )renderDesc.width + columnPixels - 1 ) / columnPixels ) * ( ( ( int )renderDesc.height + columnPixels - 1 ) / columnPixels ) * gridSlices > FROXEL_MAX_CELLS )
	{
		columnPixels += 1;		// V5e ( review O2-3 ): a pixel at a time: a finer level never has fewer froxels than a lower one ( F1-4 )
	}
	if( columnPixels != wantedColumnPixels )
	{
		static bool warned = false;
		if( !warned )
		{
			warned = true;
			idLib::Warning( "VolumetricFogPass: %s %d with %d slices is more than %d froxels at this render size, the columns are %d pixels", ( quality >= 0 ) ? "r_volumetricFogQuality's columns" : "r_volumetricFogColumnPixels", wantedColumnPixels, gridSlices, FROXEL_MAX_CELLS, columnPixels );
		}
	}
	if( !CheckVolumes( ( ( int )renderDesc.width + columnPixels - 1 ) / columnPixels, ( ( int )renderDesc.height + columnPixels - 1 ) / columnPixels, gridSlices ) || !CheckInjectSet() )
	{
		return;
	}
	m_ColumnPixels = columnPixels;
	const float farEnd = idMath::ClampFloat( 64.0f, 65536.0f, r_volumetricFogFar.GetFloat() );
	m_SliceStep = idMath::Log( 1.0f + farEnd / FROXEL_SLICE_BIAS ) / gridSlices;

	// the view's grid: the part of the volumes this view's kernels fill and its lookup reads
	const int viewWidth = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	const int viewHeight = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;
	const int gridWidth = Min( ( viewWidth + columnPixels - 1 ) / columnPixels, m_GridWidth );
	const int gridHeight = Min( ( viewHeight + columnPixels - 1 ) / columnPixels, m_GridHeight );
	m_ViewGridWidth = gridWidth;
	m_ViewGridHeight = gridHeight;
	// The kernels fill one column and one row more, where the volumes have them: at the view's last column and row
	// the lookup's filter also fetches the next froxel, at weight 0, and a froxel that nothing wrote may hold anything
	const int fillWidth = Min( gridWidth + 1, m_GridWidth );
	const int fillHeight = Min( gridHeight + 1, m_GridHeight );

	// the tangents of the view's half angles; these two elements of the projection carry no jitter
	const float tanHalfX = 1.0f / viewDef->projectionMatrix[0 * 4 + 0];
	const float tanHalfY = 1.0f / idMath::Fabs( viewDef->projectionMatrix[1 * 4 + 1] );

	froxelConstants_t constants;
	memset( &constants, 0, sizeof( constants ) );
	constants.gridSize.Set( ( float )fillWidth, ( float )fillHeight, ( float )gridSlices, ( float )numRecords );
	constants.sliceParms.Set( FROXEL_SLICE_BIAS, m_SliceStep, 1.0f / FROXEL_STORE_SCALE, 0.0f );
	// a column's centre in the view's -1..1, times the tangent: how far the ray moves sideways per unit forward
	constants.rayParms.Set( 2.0f * columnPixels / viewWidth * tanHalfX, 2.0f * columnPixels / viewHeight * tanHalfY, tanHalfX, tanHalfY );
	constants.medium.Set( source * FROXEL_STORE_SCALE, source * FROXEL_STORE_SCALE, source * FROXEL_STORE_SCALE, sigmaT * FROXEL_STORE_SCALE );

	// The inject's lanes ( V2 ). The view frame, in the frame of the light records ( relative to the view's render
	// origin ): a froxel's sample is the eye + depth * ( forward + x * right - y * up ). id's view axis is forward,
	// left, up. The integers travel as float values, each far below 2^24
	const idVec3 eye = R_RelativeEye( viewDef );
	const idMat3& axis = viewDef->renderView.viewaxis;
	constants.viewOrigin.Set( eye.x, eye.y, eye.z, sigmaS * FROXEL_STORE_SCALE );
	constants.viewForward.Set( axis[0].x, axis[0].y, axis[0].z, anisotropy );
	constants.viewRight.Set( -axis[1].x, -axis[1].y, -axis[1].z, ( float )cullMode );
	constants.viewUp.Set( axis[2].x, axis[2].y, axis[2].z, FROXEL_STORE_MAX );
	// The shadows ( V3 ): the legacy pass's two depth bias scales ( a parallel light's, every other light's ), and a
	// parallel light's cascades: the frontend gives every parallel light the same count of rows, the view's splits
	float parallelRows = 0.0f;
	for( int r = 0; r < numRecords; r++ )
	{
		if( ( viewDef->recordLights[r].flags & RLF_PARALLEL ) != 0 && llv->rowsOfRecord[r] > 0 )
		{
			parallelRows = ( float )llv->rowsOfRecord[r];
			break;
		}
	}
	constants.shadowParms.Set( r_shadowMapSunDepthBiasScale.GetFloat(), r_shadowMapRegularDepthBiasScale.GetFloat(), parallelRows, r_volumetricFogShadows.GetBool() ? 1.0f : 0.0f );
	int numTakeable = 0;
	if( numRecords > 0 )
	{
		const lightListTables_t& t = llv->tables;
		constants.tileParms.Set( ( float )t.parms.tileSize, ( float )t.tilesX, ( float )t.tilesY, ( float )t.wordsPerTile );
		constants.binParms.Set( t.binScale, ( float )t.parms.numBins, ( float )columnPixels, t.parms.zNear );
		// The take mask: a bit per record whose light the fog can take at all ( R_FogCanTakeRecord,
		// renderer/VolumetricFog.h: not a fog light ( V4: a fog light where the inject takes them, R_FogCanTakeFogLight ),
		// not one switched off for the fog or of another spectrum, and
		// ambient, flagged, or with shadow rows ). The kernel reads no record without a bit. Whether a light WITH
		// rows is taken is the row's to say, per sample ( FroxelRecordLight )
		compile_time_assert( sizeof( constants.takeMask ) * 8 >= MAX_RECORD_LIGHTS );
		const int maskRecords = Min( numRecords, MAX_RECORD_LIGHTS );
		for( int r = 0; r < maskRecords; r++ )
		{
			const recordLight_t& rec = viewDef->recordLights[r];
			// V4: a fog light in the sun's slot ( record 0: the world's sun is its largest parallel light, a fog light too ) gets
			// no bit: the inject's sun path would add it as a light ( review QR3-1 )
			if( ( takeLights && R_FogCanTakeRecord( rec, rec.shadowRow >= 0 ) ) || ( takeFogLights && r > 0 && R_FogCanTakeFogLight( rec ) ) )
			{
				constants.takeMask[r >> 5] |= 1u << ( r & 31 );
				numTakeable++;
			}
		}
	}
	// The ambient light: the view's blended SH holds the radiance around the view ( viewDef_t::shIrradiance; the
	// surfaces' ambient is its cosine convolution over pi, times r_lightScale and r_forceAmbient: the ambient pass's
	// diffuse modifier and its rpAmbientColor ). The air scatters toward the camera the radiance from every direction
	// weighted by the phase function, which is a convolution too: band l of the SH times g^l. Scaled as the
	// surfaces' ambient is, so that fog in shadow matches a wall in shadow. An SH of two bands rings under a sharp
	// kernel: past a g of about 0.55 a probe with one bright side would give a dark ring and a bright lobe behind it
	// that no wall shows, so the ambient's bands take a g of at most 0.5 either way
	if( !furnace )
	{
		const float ambientScale = r_lightScale.GetFloat() * r_forceAmbient.GetFloat();
		const float ambientG = idMath::ClampFloat( -0.5f, 0.5f, anisotropy );
		for( int i = 0; i < 9; i++ )
		{
			const float band = ( i == 0 ) ? 1.0f : ( ( i < 4 ) ? ambientG : ambientG * ambientG );
			const idVec4& sh = viewDef->shIrradiance[i];
			constants.ambient[i].Set( sh.x * ambientScale * band, sh.y * ambientScale * band, sh.z * ambientScale * band, 0.0f );
		}
	}
	// V3: the history. It holds last frame's fog where it was made for this view's grid by the last fog view, in the
	// frame right before ( the light lists' frame stamps: lightListView_t::prevFrame names the fog-eligible view before
	// this one ), with no camera cut since ( H10 ). Its view state is the engine's ( H10: the main view's unjittered MVP
	// and render origin of last frame, which the backend replaces after this pass ), not the pass's own, and it must be
	// the history's view's: a main view drawn without the fog's records ( clear air, r_volumetricFogDistance 0, with the
	// forward lists off ) replaces it and leaves the frame stamps as they were
	const float historyWeight = idMath::ClampFloat( 0.0f, 0.98f, r_volumetricFogHistoryWeight.GetFloat() );
	// V4: a flip of r_volumetricFogMedia cuts the history too: the two paths mark the skipped froxels in two ways
	// ( review O2-2: only where the view has a medium besides the global fog, or takes fog lights: a map with the global fog
	// alone runs V3's path and pays nothing for the marks; V5: the fog lights' term follows the view's records, held from the
	// first view that takes a fog light to the next camera cut, below )
	// V5f ( review F1-2 ): the fog lights hold V4's path from the first view that takes one to the next camera cut, so a fog light
	// coming into view or leaving it does not cut the view's history; V4 ran this path on every view with r_volumetricFogLights 1
	if( backEnd.viewHistoryCut || !R_VolumetricFogTakesFogLights() )
	{
		m_FogLightPath = false;
	}
	m_FogLightPath = m_FogLightPath || takeFogLights;
	const bool mediaPath = r_volumetricFogMedia.GetBool() && ( FroxelHasMediaPass() || m_FogLightPath );
	const int historyGrid[8] = { gridWidth, gridHeight, columnPixels, gridSlices, furnace ? 1 : 0, viewWidth, viewHeight, mediaPath ? 1 : 0 };
	const int historyEye = ( viewDef->renderView.viewEyeBuffer == 1 ) ? 1 : 0;
	const bool historyValid = m_HistoryValid && historyWeight > 0.0f && !backEnd.viewHistoryCut && llv != NULL && llv->prevFrame != 0
							  && llv->prevFrame == m_HistoryFrame && memcmp( historyGrid, m_HistoryGrid, sizeof( historyGrid ) ) == 0 && m_SliceStep == m_HistorySliceStep
							  && memcmp( &backEnd.prevMVP[ historyEye ], &m_HistoryMVP, sizeof( m_HistoryMVP ) ) == 0
							  && memcmp( &backEnd.prevRenderOrigin[ historyEye ], &m_HistoryOrigin, sizeof( m_HistoryOrigin ) ) == 0;
	// the jitter: one offset in the froxel a frame, for every froxel, Halton 2, 3 and 5 over 8 fog views, less each
	// axis's mean over the 8, so the history's sample lies at the froxel's centre on average. 8: no longer than the
	// history's window at its weight of 0.9 ( tools/harness/temporal_harness.py runs this sequence )
	idVec3 jitter( 0.0f, 0.0f, 0.0f );
	if( r_volumetricFogJitter.GetBool() && historyValid )			// without a history nothing averages it
	{
		idVec3 mean( 0.0f, 0.0f, 0.0f );
		for( int k = 1; k <= 8; k++ )
		{
			mean += idVec3( FroxelHalton( k, 2 ), FroxelHalton( k, 3 ), FroxelHalton( k, 5 ) );
		}
		mean *= 1.0f / 8.0f;
		const int n = m_JitterIndex % 8 + 1;
		jitter.Set( FroxelHalton( n, 2 ) - mean.x, FroxelHalton( n, 3 ) - mean.y, FroxelHalton( n, 5 ) - mean.z );
	}
	m_JitterIndex++;
	// what the fog takes of each record, and which of its rows have a place in the atlas ( the rows track F made for this
	// view ): a record whose share of its light jumps with no flag of the light's ( the fog takes it or leaves it, a row
	// gains or loses its place ) is clipped in its last volume, as a light that moved. For every record of the view,
	// those the inject reads this frame or not
	static byte fogBits[ MAX_RECORD_LIGHTS ];
	int numRows = 0;
	const shadowRow_t* rows = R_LightListShadowRows( viewDef, numRows );
	const int bitRecords = ( llv != NULL && viewDef->recordLights != NULL ) ? Min( viewDef->numRecordLights, MAX_RECORD_LIGHTS ) : 0;
	for( int r = 0; r < bitRecords; r++ )
	{
		byte bits = ( ( constants.takeMask[r >> 5] & ( 1u << ( r & 31 ) ) ) != 0 ) ? 0x80 : 0;
		const int first = viewDef->recordLights[r].shadowRow;
		const int count = Min( ( int )llv->rowsOfRecord[r], 6 );
		for( int s = 0; s < count; s++ )
		{
			if( first >= 0 && first + s < numRows && rows[first + s].atlasRect.z >= 0.0f )
			{
				bits |= ( byte )( 1 << s );
			}
		}
		fogBits[r] = bits;
	}
	// the lights that moved, and those whose share jumped, by their index in last frame's records: the inject clips the
	// history in their last volume
	int numMoved = 0;
	if( historyValid )
	{
		static int moved[ MAX_RECORD_LIGHTS ];
		for( int r = 0; r < numRecords; r++ )
		{
			const recordLight_t& rec = viewDef->recordLights[r];
			const int prev = ( int )rec.ids.z;
			const bool jumped = r < bitRecords && prev >= 0 && prev < MAX_RECORD_LIGHTS && fogBits[r] != m_PrevFogBits[prev];
			if( ( ( rec.flags & RLF_MOVED_THIS_FRAME ) != 0 || jumped ) && prev >= 0 && prev < m_PrevNumRecords )
			{
				moved[ numMoved++ ] = prev;
			}
		}
		if( numMoved > 0 )
		{
			commandList->writeBuffer( m_MovedRecords, moved, numMoved * sizeof( int ) );
		}
	}
	// where a caster that changed shadows, by record ( RLF_FOG_CASTERMOVED ), in the relative frame
	// ( froxelView_t::casterBoxes ): the inject clips the history there
	if( historyValid && numRecords > 0 && viewDef->froxelView != NULL && viewDef->froxelView->casterBoxes != NULL )
	{
		commandList->writeBuffer( m_CasterBounds, viewDef->froxelView->casterBoxes, Min( numRecords, MAX_RECORD_LIGHTS ) * 2 * FOG_CASTER_BOXES * sizeof( idVec4 ) );
	}
	constants.sliceParms.w = historyValid ? m_HistoryMedium : 0.0f;
	// one texel of the shadow atlas ( it is square ): the share keeps a lookup one texel inside its row's tile
	nvrhi::ITexture* atlas = ( nvrhi::ITexture* )globalImages->shadowAtlasImage->GetTextureID();
	constants.ambient[0].w = ( atlas != NULL ) ? 1.0f / ( float )atlas->getDesc().width : 0.0f;
	const idVec3d originDelta = viewDef->renderOrigin - backEnd.prevRenderOrigin[ historyEye ];
	constants.jitter.Set( jitter.x, jitter.y, jitter.z, historyValid ? 1.0f : 0.0f );
	constants.historyParms.Set( historyWeight, Min( historyWeight, idMath::ClampFloat( 0.0f, 0.98f, r_volumetricFogHistoryClipWeight.GetFloat() ) ), 0.25f,
								( historyValid && llv->vanishedPrev != NULL ) ? ( float )Min( llv->numVanishedPrev, MAX_RECORD_LIGHTS ) : 0.0f );
	// a screen UV to the volume UV of this view's grid ( the grid is the history's when it is valid ): the view's width
	// over the column side over the volume's columns, and so for the rows
	constants.historyGrid.Set( ( float )viewWidth / ( float )( columnPixels * m_GridWidth ), ( float )viewHeight / ( float )( columnPixels * m_GridHeight ),
							   1.0f / ( float )gridSlices, ( float )m_PrevNumRecords );
	constants.prevOrigin.Set( ( float )originDelta.x, ( float )originDelta.y, ( float )originDelta.z, ( float )numMoved );
	for( int i = 0; i < 4; i++ )
	{
		constants.prevMVP[i].Set( backEnd.prevMVP[ historyEye ][i][0], backEnd.prevMVP[ historyEye ][i][1], backEnd.prevMVP[ historyEye ][i][2], backEnd.prevMVP[ historyEye ][i][3] );
	}
	// V3: the skip of the froxels no pixel's lookup reaches. The columns read the depth the apply reads; under the
	// furnace every slice is filled ( its closed form is printed for every slice )
	// the depth the apply reads ( R_UseHiZ: the pyramid's mip 0, else the depth image ); no skip without it
	nvrhi::ITexture* columnsDepth = R_UseHiZ() ? globalImages->hierarchicalZbufferImage->GetTextureHandle().Get() : globalImages->currentDepthImage->GetTextureHandle().Get();
	const bool skip = r_volumetricFogSkip.GetBool() && !furnace && columnsDepth != NULL;
	constants.skipParms.Set( skip ? 1.0f : 0.0f, ( float )columnPixels, ( float )viewWidth, ( float )viewHeight );
	constants.pixelParms.Set( viewDef->projectionMatrix[3 * 4 + 2], viewDef->projectionMatrix[2 * 4 + 2], ( float )viewDef->viewport.x1, ( float )viewDef->viewport.y1 );
	// V4: the media. On V4's path the view's media besides the global fog go into the media list, and the media pass sums
	// them with the global fog into the media volume, which the inject reads; where the view has none the pass is not
	// issued and the inject reads the constant as V3 did. The resolve takes the history's medium per froxel on V4's path
	// whatever the view holds
	int numMovedBoxes = 0;
	m_NumMedia = mediaPath ? BuildMedia( commandList, viewDef, numMovedBoxes ) : 0;
	constants.mediaParms.Set( ( m_NumMedia > 0 ) ? 1.0f : 0.0f, furnace ? r_volumetricFogFurnace.GetFloat() : 0.0f, mediaPath ? 1.0f : 0.0f, ( float )m_NumMedia );
	constants.mediaParms2.Set( ( float )numMovedBoxes, r_volumetricFogLightRate.GetFloat() * FROXEL_STORE_SCALE, 1.0f / FROXEL_FOGLIGHT_RAMP, takeFogLights ? 1.0f : 0.0f );
	commandList->writeBuffer( m_ConstantBuffer, &constants, sizeof( constants ) );

	const int groupsX = ( fillWidth + 7 ) / 8;
	const int groupsY = ( fillHeight + 7 ) / 8;
	nvrhi::ComputeState state;
	if( skip )
	{
		// V3: one thread group per column, the view's grid and the one more column and row the kernels fill
		OPTICK_GPU_EVENT( "Fog_Columns" );
		nvrhi::BindingSetDesc columnsDesc;
		columnsDesc.bindings =
		{
			nvrhi::BindingSetItem::ConstantBuffer( 1, m_ConstantBuffer ),
			nvrhi::BindingSetItem::Texture_SRV( 0, columnsDepth ),
			nvrhi::BindingSetItem::Texture_SRV( 1, m_SkyMask ),
			nvrhi::BindingSetItem::Texture_UAV( 0, m_Columns )
		};
		state.pipeline = m_ColumnsKernel.pipeline;
		state.bindings = { backEnd.bindingCache.GetOrCreateBindingSet( columnsDesc, m_ColumnsKernel.layout ) };
		commandList->setComputeState( state );
		commandList->dispatch( fillWidth, fillHeight, 1 );
	}
	if( m_NumMedia > 0 )
	{
		// V4: the media pass, before the inject that reads its volume; the same froxels as the inject
		OPTICK_GPU_EVENT( "Fog_Media" );
		state.pipeline = m_MediaKernel.pipeline;
		state.bindings = { m_MediaKernel.bindingSet };
		commandList->setComputeState( state );
		commandList->dispatch( groupsX, groupsY, gridSlices );
	}
	{
		OPTICK_GPU_EVENT( "Fog_Inject" );
		state.pipeline = m_Inject.pipeline;
		state.bindings = { m_Inject.bindingSet };
		commandList->setComputeState( state );
		commandList->dispatch( groupsX, groupsY, gridSlices );
	}
	// V3: this view's records are last frame's for the next fog view ( after the inject, which read the old ones )
	const int allRecords = ( llv != NULL && viewDef->recordLights != NULL ) ? Min( viewDef->numRecordLights, MAX_RECORD_LIGHTS ) : 0;
	if( allRecords > 0 )
	{
		commandList->copyBuffer( m_PrevRecords, 0, backEnd.recordLightsBuffer, 0, allRecords * sizeof( recordLight_t ) );
	}
	// the resolved volumes take turns: this frame's is the one last frame's was not
	m_ResolvedIndex = 1 - m_ResolvedIndex;
	{
		OPTICK_GPU_EVENT( "Fog_Resolve" );
		state.pipeline = m_Resolve.pipeline;
		state.bindings = { m_ResolveSets[ m_ResolvedIndex ] };
		commandList->setComputeState( state );
		commandList->dispatch( groupsX, groupsY, gridSlices );
	}
	{
		// one thread per column: it walks the slices from the camera outward
		OPTICK_GPU_EVENT( "Fog_Integrate" );
		state.pipeline = m_Integrate.pipeline;
		state.bindings = { m_IntegrateSets[ m_ResolvedIndex ] };
		commandList->setComputeState( state );
		commandList->dispatch( groupsX, groupsY, 1 );
	}
	// what the next fog view's history is made of
	m_HistoryValid = true;
	m_HistoryFrame = ( llv != NULL ) ? llv->viewFrame : 0;
	memcpy( m_HistoryGrid, historyGrid, sizeof( m_HistoryGrid ) );
	m_HistorySliceStep = m_SliceStep;
	m_HistoryMVP = viewDef->worldSpace.unjitteredMVP;
	m_HistoryOrigin = viewDef->renderOrigin;
	m_HistoryMedium = constants.medium.w;
	memcpy( m_PrevFogBits, fogBits, bitRecords );
	m_PrevNumRecords = allRecords;

	// the probe: the console command asks for one, and it starts when none is under way
	if( fogProbeRequested && m_ProbeWait == 0 )
	{
		fogProbeRequested = false;
		m_ProbeSlices = gridSlices;
		m_ProbeColumn[0][0] = gridWidth / 2;
		m_ProbeColumn[0][1] = gridHeight / 2;
		m_ProbeColumn[1][0] = 0;
		m_ProbeColumn[1][1] = 0;
		m_ProbeColumn[2][0] = 0;
		m_ProbeColumn[2][1] = gridHeight / 2;
		m_ProbeRayParms = constants.rayParms;
		m_ProbeSliceStep = m_SliceStep;
		m_ProbeSigmaT = sigmaT;
		m_ProbeSource = source;
		m_ProbeLit = !furnace;
		m_ProbeRecords = numRecords;
		m_ProbeTakeable = numTakeable;
		m_ProbeCull = cullMode;
		m_ProbeSigmaS = sigmaS;
		m_ProbeRenderOrigin = viewDef->renderOrigin;
		m_ProbeHistory = historyValid;
		ProbeMedia( constants );
		ProbeCopy( commandList );
		m_ProbeWait = FROXEL_PROBE_WAIT;
	}

	m_RenderedView = viewDef;
	m_RenderedFogLights = takeFogLights;		// V5: the apply tells the backend, whose FogAllLights then skips the fog pass for them
}

/*
========================
VolumetricFogPass::BuildMedia

V4: the view's media besides the global fog, into the media list ( fogMedium_t, at most FOG_MEDIA_MAX ): the height fog,
the dust and the test box, each with sigma_s and sigma_t at full density times the store scale. A place of the map goes into the
view's relative frame on the CPU in double, so a medium fixed to the map keeps its place far from the origin. Also the
boxes where a medium moved, came or went since the last fog view ( m_MediaMoved: its last and its present bounds, in this
view's relative frame; an empty box where it had or has none ), where the inject clips the history in the froxels inside
one of the two and not the other. Returns the media in the list.
========================
*/
static void FroxelMediumClear( fogMedium_t& m, float sigmaT, float albedo )
{
	memset( &m, 0, sizeof( m ) );
	const float t = sigmaT * FROXEL_STORE_SCALE;
	const float s = t * idMath::ClampFloat( 0.0f, 1.0f, albedo );
	m.sigma.Set( s, s, s, t );
	// the identity frame: the medium's coordinates are the relative frame's
	m.frame[0].Set( 1.0f, 0.0f, 0.0f, 0.0f );
	m.frame[1].Set( 0.0f, 1.0f, 0.0f, 0.0f );
	m.frame[2].Set( 0.0f, 0.0f, 1.0f, 0.0f );
}

int VolumetricFogPass::BuildMedia( nvrhi::ICommandList* commandList, const viewDef_t* viewDef, int& numMovedBoxes )
{
	numMovedBoxes = 0;
	int count = 0;

	// the height fog: full below its base, thinning to 1 / e every falloff above it
	const float heightDistance = r_volumetricFogHeightDistance.GetFloat();
	if( heightDistance > 0.0f )
	{
		fogMedium_t& m = m_MediaCopy[ count++ ];
		FroxelMediumClear( m, FOG_MEDIA_OPACITY_AT_D / Max( heightDistance, FROXEL_MIN_FOG_DISTANCE ), r_volumetricFogHeightAlbedo.GetFloat() );
		const double base = ( double )r_volumetricFogHeight.GetFloat() - viewDef->renderOrigin.z;
		m.shape.Set( ( float )FOG_SHAPE_HEIGHT, ( float )base, 1.0f / Max( r_volumetricFogHeightFalloff.GetFloat(), 1.0f ), 0.0f );
	}

	// V4c: the dust, an unbounded medium of noise ( section 6 row 85 ) fixed to the map and drifting with its wind. Its noise reads
	// the relative frame's point plus an offset: the render origin's place in the map less the wind's drift, wrapped to
	// 1024 units in double. Every octave's cell divides 1024 and the lattice repeats every 1024 units, so the field is the
	// map's own wherever the view is
	const float dustDistance = r_volumetricFogDustDistance.GetFloat();
	if( dustDistance > 0.0f )
	{
		fogMedium_t& m = m_MediaCopy[ count++ ];
		FroxelMediumClear( m, FOG_MEDIA_OPACITY_AT_D / Max( dustDistance, FROXEL_MIN_FOG_DISTANCE ), r_volumetricFogDustAlbedo.GetFloat() );
		// the base cell: 1024 over a whole number of cells, at least 8 units
		const int cells = idMath::ClampInt( 1, 128, idMath::Ftoi( 1024.0f / Max( r_volumetricFogDustScale.GetFloat(), 8.0f ) + 0.5f ) );
		const float frequency = ( float )cells / 1024.0f;
		idVec3 wind( 0.0f, 0.0f, 0.0f );
		sscanf( r_volumetricFogDustWind.GetString(), "%f %f %f", &wind.x, &wind.y, &wind.z );
		const double seconds = viewDef->renderView.time[0] * 0.001;
		const double origin[3] = { viewDef->renderOrigin.x, viewDef->renderOrigin.y, viewDef->renderOrigin.z };
		float offset[3];
		for( int i = 0; i < 3; i++ )
		{
			double wrapped = fmod( origin[i] - ( double )wind[i] * seconds, 1024.0 );
			if( wrapped < 0.0 )
			{
				wrapped += 1024.0;
			}
			offset[i] = ( float )wrapped;
		}
		// the slots, three lanes each: type, op, octaves, metric; frequency, gain, strength, bias; jitter, tile. With a warp,
		// slot 1 bends the points slot 2's noise reads; else slot 1 is the noise and slot 2 erodes it with a finer fBm
		const float warp = r_volumetricFogDustWarp.GetFloat();
		const float erode = r_volumetricFogDustErode.GetFloat();
		const float noise = ( float )idMath::ClampInt( FOG_NOISE_FBM, FOG_NOISE_PERLINWORLEY, r_volumetricFogDustNoise.GetInteger() );
		int slots = 0;
		if( warp > 0.0f )
		{
			m.slot[0].Set( ( float )FOG_NOISE_CURL, ( float )FOG_OP_WARP, 2.0f, 0.0f );
			m.slot[1].Set( frequency, 0.5f, warp, 0.0f );
			m.slot[2].Set( 1.0f, ( float )cells, 0.0f, 0.0f );
			slots = 1;
		}
		const int octaves = idMath::ClampInt( 1, 8, r_volumetricFogDustOctaves.GetInteger() );
		m.slot[ 3 * slots ].Set( noise, ( float )FOG_OP_MULTIPLY, ( float )octaves, 0.0f );
		m.slot[ 3 * slots + 1 ].Set( frequency, 0.5f, 1.0f, 0.0f );
		m.slot[ 3 * slots + 2 ].Set( 1.0f, ( float )cells, 0.0f, 0.0f );
		slots++;
		if( slots < 2 && erode > 0.0f )
		{
			m.slot[3].Set( ( float )FOG_NOISE_FBM, ( float )FOG_OP_ERODE, ( float )Min( octaves, 3 ), 0.0f );
			m.slot[4].Set( frequency * 4.0f, 0.5f, erode, 0.0f );
			m.slot[5].Set( 1.0f, ( float )( cells * 4 ), 0.0f, 0.0f );
			slots++;
		}
		m.noiseOffset.Set( offset[0], offset[1], offset[2], ( float )slots );
	}

	// the test box: world-aligned, its centre moving with the view's game time
	idVec3 boxOrigin, boxHalf, boxVelocity;
	float boxDistance = 0.0f;
	const bool boxNow = FroxelTestBox( boxOrigin, boxHalf, boxDistance, boxVelocity );
	idVec3d boxMins, boxMaxs;
	if( boxNow )
	{
		const double seconds = viewDef->renderView.time[0] * 0.001;
		const idVec3d centre( boxOrigin.x + boxVelocity.x * seconds, boxOrigin.y + boxVelocity.y * seconds, boxOrigin.z + boxVelocity.z * seconds );
		boxMins = centre - idVec3d( boxHalf );
		boxMaxs = centre + idVec3d( boxHalf );
		const idVec3 relative = ( centre - viewDef->renderOrigin ).ToVec3();
		fogMedium_t& m = m_MediaCopy[ count++ ];
		FroxelMediumClear( m, FOG_MEDIA_OPACITY_AT_D / Max( boxDistance, FROXEL_MIN_FOG_DISTANCE ), 1.0f );
		m.sphere.Set( relative.x, relative.y, relative.z, 1.0f / boxHalf.LengthSqr() );
		m.shape.Set( ( float )FOG_SHAPE_BOX, boxHalf.x, boxHalf.y, boxHalf.z );
		for( int i = 0; i < 3; i++ )
		{
			m.frame[i].w = -relative[i];
		}
	}

	// the box moved, came or went: its last and its present bounds in this view's relative frame, an empty box ( min above
	// max ) where it had or has none
	const bool boxMoved = ( boxNow != m_PrevBoxValid ) || ( boxNow && ( !( boxMins - m_PrevBoxMins ).IsZero() || !( boxMaxs - m_PrevBoxMaxs ).IsZero() ) );
	if( boxMoved )
	{
		idVec4 boxes[4];
		for( int i = 0; i < 4; i++ )
		{
			const float side = ( i & 1 ) ? -idMath::INFINITUM : idMath::INFINITUM;
			boxes[i].Set( side, side, side, 0.0f );
		}
		if( m_PrevBoxValid )
		{
			const idVec3 mins = ( m_PrevBoxMins - viewDef->renderOrigin ).ToVec3();
			const idVec3 maxs = ( m_PrevBoxMaxs - viewDef->renderOrigin ).ToVec3();
			boxes[0].Set( mins.x, mins.y, mins.z, 0.0f );
			boxes[1].Set( maxs.x, maxs.y, maxs.z, 0.0f );
		}
		if( boxNow )
		{
			const idVec3 mins = ( boxMins - viewDef->renderOrigin ).ToVec3();
			const idVec3 maxs = ( boxMaxs - viewDef->renderOrigin ).ToVec3();
			boxes[2].Set( mins.x, mins.y, mins.z, 0.0f );
			boxes[3].Set( maxs.x, maxs.y, maxs.z, 0.0f );
		}
		commandList->writeBuffer( m_MediaMoved, boxes, sizeof( boxes ) );
		numMovedBoxes = 1;
	}
	m_PrevBoxValid = boxNow;
	m_PrevBoxMins = boxMins;
	m_PrevBoxMaxs = boxMaxs;

	if( count > 0 )
	{
		commandList->writeBuffer( m_MediaList, m_MediaCopy, count * sizeof( fogMedium_t ) );
	}
	return count;
}

/*
========================
VolumetricFogPass::ProbeMedia

V4: at the start of a probe whose view ran the media pass, the medium of each slice of the probe's three columns as the
media pass finds it at the froxel's centre ( V4g, review F1-1 ), worked out on the CPU from
the media list: the global fog's constant plus each medium's density times its sigma, without the group test, which only
leaves out media of density 0, and without the skip ( a froxel the inject skips holds the global fog alone: the closed form
is not checked then ). A medium with noise slots has no closed form here, nor has a view that takes fog lights ( the inject
adds them; V4g, review F1-11 ): the probe then prints the media volume alone.
The list and its count are kept for the read, which comes some fog views later.
========================
*/
void VolumetricFogPass::ProbeMedia( const froxelConstants_t& constants )
{
	m_ProbeMedia = m_NumMedia > 0;
	m_ProbeFurnace = constants.mediaParms.y;
	m_ProbeMediaClosed = true;
	m_ProbeFogLights = constants.mediaParms2.w > 0.0f;		// V5: also where the view has no other medium ( V4g's note )
	m_ProbeSkip = constants.skipParms.x > 0.0f;
	m_ProbeNumMedia = m_NumMedia;
	memcpy( m_ProbeMediaList, m_MediaCopy, m_NumMedia * sizeof( fogMedium_t ) );
	if( !m_ProbeMedia )
	{
		return;
	}
	for( int i = 0; i < m_NumMedia; i++ )
	{
		if( m_ProbeMediaList[i].noiseOffset.w > 0.5f )
		{
			m_ProbeMediaClosed = false;
		}
	}
	if( constants.mediaParms2.w > 0.0f )
	{
		m_ProbeMediaClosed = false;		// V4g: the fog lights the inject takes are not in the media list ( review F1-11 )
	}
	const idVec3 eye( constants.viewOrigin.x, constants.viewOrigin.y, constants.viewOrigin.z );
	const idVec3 forward( constants.viewForward.x, constants.viewForward.y, constants.viewForward.z );
	const idVec3 right( constants.viewRight.x, constants.viewRight.y, constants.viewRight.z );
	const idVec3 up( constants.viewUp.x, constants.viewUp.y, constants.viewUp.z );
	const int slices = Min( m_ProbeSlices, 256 );
	for( int c = 0; c < FROXEL_PROBE_COLUMNS; c++ )
	{
		const float lateralX = ( m_ProbeColumn[c][0] + 0.5f ) * constants.rayParms.x - constants.rayParms.z;
		const float lateralY = ( m_ProbeColumn[c][1] + 0.5f ) * constants.rayParms.y - constants.rayParms.w;
		const idVec3 ray = forward + lateralX * right - lateralY * up;
		for( int k = 0; k < slices; k++ )
		{
			const float viewDepth = FROXEL_SLICE_BIAS * ( idMath::Exp( ( k + 0.5f ) * m_SliceStep ) - 1.0f );
			const idVec3 p = eye + viewDepth * ray;
			idVec4 medium( constants.viewOrigin.w, constants.viewOrigin.w, constants.viewOrigin.w, constants.medium.w );
			for( int i = 0; i < m_ProbeNumMedia; i++ )
			{
				const fogMedium_t& m = m_ProbeMediaList[i];
				const idVec3 q( m.frame[0].ToVec3() * p + m.frame[0].w, m.frame[1].ToVec3() * p + m.frame[1].w, m.frame[2].ToVec3() * p + m.frame[2].w );
				float density = 1.0f;
				if( ( int )m.shape.x == FOG_SHAPE_HEIGHT )
				{
					density = idMath::Exp( -Max( q.z - m.shape.y, 0.0f ) * m.shape.z );
				}
				else if( ( int )m.shape.x == FOG_SHAPE_BOX )
				{
					density = ( idMath::Fabs( q.x ) <= m.shape.y && idMath::Fabs( q.y ) <= m.shape.z && idMath::Fabs( q.z ) <= m.shape.w ) ? 1.0f : 0.0f;
				}
				medium += m.sigma * density;
			}
			// the media pass's store bound: a sum past it is scaled down whole ( review QR2-2 )
			const float largest = Max( medium.w, Max( medium.x, Max( medium.y, medium.z ) ) );
			medium *= Min( 1.0f, constants.viewUp.w / Max( largest, 1e-20f ) );
			// per unit, as the integrate reads it
			m_ProbeMedium[c][k] = medium * ( 1.0f / FROXEL_STORE_SCALE );
			if( c == 0 )
			{
				m_ProbeSample[k] = p;
			}
		}
	}
}

/*
========================
VolumetricFogPass::ProbeReadMedia

V4: ProbeRead's columns where the probe's view ran the media pass. The closed form is worked out slab by slab from the
medium ProbeMedia found at each slice's sample, as the integrate does it ( a slab of constant medium: transmittance
exp( -tau ), in-scatter sigma_s * L / sigma_t * ( 1 - exp( -tau ) ) times the transmittance in front of it ), in double;
under the furnace L is its radiance, and a lit fog has no closed form for its in-scatter. Beside it the media volume's
sigma_t against the CPU's, the media pass's own check.
========================
*/
void VolumetricFogPass::ProbeReadMedia( const byte* data, size_t rowPitch )
{
	const double step = m_ProbeSliceStep;
	size_t mediaPitch = 0;
	const byte* media = ( const byte* )m_Device->mapStagingTexture( m_ProbeMediaStaging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &mediaPitch );
	common->Printf( "fogprobe: %d slices, far end %.1f, the media pass ran with %d media%s%s%s\n", m_ProbeSlices, FROXEL_SLICE_BIAS * ( exp( m_ProbeSlices * step ) - 1.0 ), m_ProbeNumMedia,
					m_ProbeMediaClosed ? "" : " ( noise or fog lights: no closed form, the media volume is printed )", m_ProbeLit ? " ( lit: the in-scatter has no closed form )" : "",
					m_ProbeSkip ? " ( the skip was on: the slices past a column's reach are not checked )" : "" );
	const bool closed = m_ProbeMediaClosed && !m_ProbeSkip;
	const int slices = Min( m_ProbeSlices, 256 );
	// V4c: a medium with noise has no closed form here: its lanes and the centre column's samples are printed, so that a
	// reference of the noise can be checked against the media volume's centre column offline. The samples and the render
	// origin are printed for every media probe, so that the media can be checked from their places in the map, apart from
	// the media list this pass built ( review QR4 )
	common->Printf( "fogprobe: render origin %.4f %.4f %.4f\n", m_ProbeRenderOrigin.x, m_ProbeRenderOrigin.y, m_ProbeRenderOrigin.z );
	{
		for( int i = 0; i < m_ProbeNumMedia; i++ )
		{
			const fogMedium_t& m = m_ProbeMediaList[i];
			common->Printf( "fogprobe: medium %d sigma %.6g %.6g shape %g %.6g %.6g %.6g noise offset %.6f %.6f %.6f slots %g\n", i, m.sigma.x, m.sigma.w,
							m.shape.x, m.shape.y, m.shape.z, m.shape.w, m.noiseOffset.x, m.noiseOffset.y, m.noiseOffset.z, m.noiseOffset.w );
			for( int s = 0; s < 6; s++ )
			{
				common->Printf( "fogprobe: medium %d slot lane %d %.9g %.9g %.9g %.9g\n", i, s, m.slot[s].x, m.slot[s].y, m.slot[s].z, m.slot[s].w );
			}
		}
		for( int k = 0; k < slices; k++ )
		{
			common->Printf( "fogprobe: sample %d %.5f %.5f %.5f width %.6f\n", k, m_ProbeSample[k].x, m_ProbeSample[k].y, m_ProbeSample[k].z,
							( float )( FROXEL_SLICE_BIAS * ( exp( ( k + 0.5 ) * step ) - 1.0 ) * Max( m_ProbeRayParms.x, m_ProbeRayParms.y ) ) );
		}
	}
	for( int c = 0; c < FROXEL_PROBE_COLUMNS; c++ )
	{
		const halfFloat_t* row = ( const halfFloat_t* )( data + c * rowPitch );
		const halfFloat_t* mediaRow = ( media != NULL ) ? ( const halfFloat_t* )( media + c * mediaPitch ) : NULL;
		const double lateralX = ( m_ProbeColumn[c][0] + 0.5 ) * m_ProbeRayParms.x - m_ProbeRayParms.z;
		const double lateralY = ( m_ProbeColumn[c][1] + 0.5 ) * m_ProbeRayParms.y - m_ProbeRayParms.w;
		const double rayLength = sqrt( 1.0 + lateralX * lateralX + lateralY * lateralY );
		double transmittance = 1.0;
		double inScatter = 0.0;
		double nearFace = 0.0;
		double maxErrorT = 0.0;
		double maxErrorS = 0.0;
		double maxErrorMedium = 0.0;
		for( int k = 0; k < slices; k++ )
		{
			const double farFace = FROXEL_SLICE_BIAS * ( exp( ( k + 1 ) * step ) - 1.0 );
			const double slab = ( farFace - nearFace ) * rayLength;
			nearFace = farFace;
			const double sigmaT = Max( m_ProbeMedium[c][k].w, 0.0f );
			const double sigmaS = m_ProbeMedium[c][k].x;
			const double tau = sigmaT * slab;
			const double slabT = exp( -tau );
			const double weight = ( tau > 1e-6 ) ? ( 1.0 - slabT ) / sigmaT : slab;
			inScatter += transmittance * sigmaS * m_ProbeFurnace * weight;
			transmittance *= slabT;
			const double gotS = F16toF32( row[k * 4 + 0] );
			const double gotT = F16toF32( row[k * 4 + 3] );
			const double gotMedium = ( mediaRow != NULL ) ? F16toF32( mediaRow[k * 4 + 3] ) / FROXEL_STORE_SCALE : 0.0;
			if( closed )
			{
				maxErrorT = Max( maxErrorT, fabs( gotT - transmittance ) );
				maxErrorS = Max( maxErrorS, fabs( gotS - inScatter ) );
				maxErrorMedium = Max( maxErrorMedium, fabs( gotMedium - sigmaT ) / Max( sigmaT, 1e-6 ) );
			}
			// every slice of the centre column, so that tools/media_ref.py sees every sample ( a box between the printed slices
			// was not checked at all )
			if( k < 2 || ( ( k + 1 ) & k ) == 0 || k == slices - 1 || c == 0 )
			{
				common->Printf( "fogprobe:   column %d,%d slice %2d far %8.2f  sigma_t %.6f ( CPU %.6f )  T %.5f ( closed form %.5f )  in-scatter %.5f ( %.5f )\n",
								m_ProbeColumn[c][0], m_ProbeColumn[c][1], k, farFace, gotMedium, sigmaT, gotT, transmittance, gotS, m_ProbeLit ? 0.0 : inScatter );
			}
		}
		if( closed )
		{
			common->Printf( "fogprobe: column %d,%d ray length %.5f: largest error of T %.6f, of the in-scatter %.6f%s, of sigma_t %.5f of itself\n",
							m_ProbeColumn[c][0], m_ProbeColumn[c][1], rayLength, maxErrorT, maxErrorS, m_ProbeLit ? " ( lit: not a check )" : "", maxErrorMedium );
		}
	}
	if( media != NULL )
	{
		m_Device->unmapStagingTexture( m_ProbeMediaStaging );
	}
}

/*
========================
VolumetricFogPass::ProbeCopy

Copies the probe's columns out of the integrated volume into a strip: a row per column, a texel per slice. The
strip is a 3D texture of depth 1, so that every copy is between textures of one kind ( 3D to 3D ) and the staging
texture, of depth 1 too, is mapped as one block by both APIs. ProbeStage, behind the apply of the same view,
sends the strip to its staging texture.
========================
*/
void VolumetricFogPass::ProbeCopy( nvrhi::ICommandList* commandList )
{
	if( !m_ProbeStrip || ( int )m_ProbeStrip->getDesc().width != m_ProbeSlices )
	{
		nvrhi::TextureDesc desc;
		desc.dimension = nvrhi::TextureDimension::Texture3D;
		desc.width = m_ProbeSlices;
		desc.height = FROXEL_PROBE_COLUMNS;
		desc.depth = 1;
		desc.format = nvrhi::Format::RGBA16_FLOAT;
		desc.initialState = nvrhi::ResourceStates::CopyDest;
		desc.keepInitialState = true;
		desc.debugName = "Fog/ProbeStrip";
		m_ProbeStrip = m_Device->createTexture( desc );

		nvrhi::TextureDesc stagingDesc;
		stagingDesc.dimension = nvrhi::TextureDimension::Texture3D;
		stagingDesc.width = m_ProbeSlices;
		stagingDesc.height = FROXEL_PROBE_COLUMNS;
		stagingDesc.depth = 1;
		stagingDesc.format = nvrhi::Format::RGBA16_FLOAT;
		stagingDesc.debugName = "Fog/ProbeStaging";
		m_ProbeStaging = m_Device->createStagingTexture( stagingDesc, nvrhi::CpuAccessMode::Read );
	}
	if( !m_ProbePixel )
	{
		nvrhi::TextureDesc desc;
		desc.width = 1;
		desc.height = 1;
		desc.format = nvrhi::Format::RGBA16_FLOAT;
		desc.initialState = nvrhi::ResourceStates::CopyDest;
		desc.keepInitialState = true;
		desc.debugName = "Fog/ProbePixel";
		m_ProbePixel = m_Device->createTexture( desc );

		nvrhi::TextureDesc stagingDesc;
		stagingDesc.width = 1;
		stagingDesc.height = 1;
		stagingDesc.format = nvrhi::Format::RGBA16_FLOAT;
		stagingDesc.debugName = "Fog/ProbePixelStaging";
		m_ProbePixelStaging = m_Device->createStagingTexture( stagingDesc, nvrhi::CpuAccessMode::Read );

		// V5: the pixel behind the translucents
		desc.debugName = "Fog/ProbePixelLate";
		m_ProbePixelLate = m_Device->createTexture( desc );
		stagingDesc.debugName = "Fog/ProbePixelLateStaging";
		m_ProbePixelLateStaging = m_Device->createStagingTexture( stagingDesc, nvrhi::CpuAccessMode::Read );
	}

	for( int c = 0; c < FROXEL_PROBE_COLUMNS; c++ )
	{
		for( int k = 0; k < m_ProbeSlices; k++ )
		{
			commandList->copyTexture( m_ProbeStrip, nvrhi::TextureSlice().setOrigin( k, c, 0 ).setSize( 1, 1, 1 ),
									  m_Integrated, nvrhi::TextureSlice().setOrigin( m_ProbeColumn[c][0], m_ProbeColumn[c][1], k ).setSize( 1, 1, 1 ) );
		}
	}
	// V4: the media volume's same three columns, where the media pass ran
	if( m_ProbeMedia )
	{
		if( !m_ProbeMediaStrip || ( int )m_ProbeMediaStrip->getDesc().width != m_ProbeSlices )
		{
			nvrhi::TextureDesc desc = m_ProbeStrip->getDesc();
			desc.debugName = "Fog/ProbeMediaStrip";
			m_ProbeMediaStrip = m_Device->createTexture( desc );
			nvrhi::TextureDesc stagingDesc;
			stagingDesc.dimension = nvrhi::TextureDimension::Texture3D;
			stagingDesc.width = m_ProbeSlices;
			stagingDesc.height = FROXEL_PROBE_COLUMNS;
			stagingDesc.depth = 1;
			stagingDesc.format = nvrhi::Format::RGBA16_FLOAT;
			stagingDesc.debugName = "Fog/ProbeMediaStaging";
			m_ProbeMediaStaging = m_Device->createStagingTexture( stagingDesc, nvrhi::CpuAccessMode::Read );
		}
		for( int c = 0; c < FROXEL_PROBE_COLUMNS; c++ )
		{
			for( int k = 0; k < m_ProbeSlices; k++ )
			{
				commandList->copyTexture( m_ProbeMediaStrip, nvrhi::TextureSlice().setOrigin( k, c, 0 ).setSize( 1, 1, 1 ),
										  m_Media, nvrhi::TextureSlice().setOrigin( m_ProbeColumn[c][0], m_ProbeColumn[c][1], k ).setSize( 1, 1, 1 ) );
			}
		}
	}
	m_ProbeStaged = false;
	m_ProbeHasPixel = false;
	m_ProbeHasPixelLate = false;
	m_ProbeLateColor = NULL;
	m_ProbeLateView = NULL;
}

/*
========================
VolumetricFogPass::ProbeStage

The probe's second half, behind the apply of the view the probe started in: the centre pixel of the view's
colour target as the apply left it ( under r_volumetricFogDebug 1 that is the transmittance to the pixel ) goes
through a texture of one pixel into a staging texture of its own, and the strip into its staging texture.
========================
*/
void VolumetricFogPass::ProbeStage( nvrhi::ICommandList* commandList, nvrhi::ITexture* color, const viewDef_t* viewDef )
{
	if( color->getDesc().format == nvrhi::Format::RGBA16_FLOAT )
	{
		const int centreX = ( viewDef->viewport.x1 + viewDef->viewport.x2 + 1 ) / 2;
		const int centreY = ( viewDef->viewport.y1 + viewDef->viewport.y2 + 1 ) / 2;
		commandList->copyTexture( m_ProbePixel, nvrhi::TextureSlice(),
								  color, nvrhi::TextureSlice().setOrigin( centreX, centreY, 0 ).setSize( 1, 1, 1 ) );
		commandList->copyTexture( m_ProbePixelStaging, nvrhi::TextureSlice(), m_ProbePixel, nvrhi::TextureSlice() );
		m_ProbeHasPixel = true;
		m_ProbeLateColor = color;			// V5: ProbeStageTranslucent reads it again behind the translucents
		m_ProbeLateView = viewDef;
	}
	commandList->copyTexture( m_ProbeStaging, nvrhi::TextureSlice(), m_ProbeStrip, nvrhi::TextureSlice() );
	if( m_ProbeMedia )
	{
		commandList->copyTexture( m_ProbeMediaStaging, nvrhi::TextureSlice(), m_ProbeMediaStrip, nvrhi::TextureSlice() );
	}
	m_ProbeStaged = true;
}

/*
========================
VolumetricFogPass::ProbeRead

Prints the probe: per column the transmittance and the in-scatter of some slices, and how far every slice is
from the closed form of a uniform medium along the column's ray, worked out here in double precision:
T = exp( -sigma_t * far * | dir | ) and in-scatter = sigma_s * L / sigma_t * ( 1 - T ). With the furnace radiance
L and albedo 1 the in-scatter must be L * ( 1 - T ): the fog furnace. A fog that is lit ( V2: no furnace ) has no
closed form for its in-scatter: its three channels are printed as they are, for every slice of the centre column.
Then the picture's centre pixel as the apply left it, and the GPU time of the fog's two blocks as the engine last
measured them.
========================
*/
void VolumetricFogPass::ProbeRead()
{
	if( !m_ProbeStaged )
	{
		common->Printf( "fogprobe: the probe's view had no apply, nothing was read\n" );
		return;
	}

	size_t rowPitch = 0;
	const byte* data = ( const byte* )m_Device->mapStagingTexture( m_ProbeStaging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch );
	if( data == NULL )
	{
		common->Printf( "fogprobe: the read-back failed\n" );
		return;
	}
	// whether the probe's view read the history or cut it ( a camera cut, another grid, a flip of r_volumetricFogMedia: the
	// gate's check of the cut, which the probe's values cannot show in a still view )
	common->Printf( "fogprobe: the history was %s\n", m_ProbeHistory ? "read" : "cut" );

	const double step = m_ProbeSliceStep;
	if( m_ProbeMedia )
	{
		// V4: the media pass ran: the closed form is worked out slab by slab from the medium the CPU finds at each slice's
		// sample ( ProbeMedia ), and the media volume is printed beside it
		ProbeReadMedia( data, rowPitch );
	}
	else
	{
		common->Printf( "fogprobe: %d slices, far end %.1f, sigma_t %.6g, sigma_s * L %.6g\n", m_ProbeSlices,
						FROXEL_SLICE_BIAS * ( exp( m_ProbeSlices * step ) - 1.0 ), m_ProbeSigmaT, m_ProbeSource );
		if( m_ProbeFogLights )
		{
			common->Printf( "fogprobe: the view took fog lights: the closed forms of the slices below are the global fog's alone ( V5, consultant CN3-5 )\n" );
		}
		if( m_ProbeLit )
		{
			common->Printf( "fogprobe: lit by the ambient light and %d light records, %d of which the fog can take ( r_volumetricFogCull %d ), sigma_s %.6g; the in-scatter has no closed form\n", m_ProbeRecords, m_ProbeTakeable, m_ProbeCull, m_ProbeSigmaS );
		}
		for( int c = 0; c < FROXEL_PROBE_COLUMNS; c++ )
		{
			const halfFloat_t* row = ( const halfFloat_t* )( data + c * rowPitch );
			const double lateralX = ( m_ProbeColumn[c][0] + 0.5 ) * m_ProbeRayParms.x - m_ProbeRayParms.z;
			const double lateralY = ( m_ProbeColumn[c][1] + 0.5 ) * m_ProbeRayParms.y - m_ProbeRayParms.w;
			const double rayLength = sqrt( 1.0 + lateralX * lateralX + lateralY * lateralY );

			double maxErrorT = 0.0;
			double maxErrorS = 0.0;
			for( int k = 0; k < m_ProbeSlices; k++ )
			{
				const double farFace = FROXEL_SLICE_BIAS * ( exp( ( k + 1 ) * step ) - 1.0 );
				const double length = farFace * rayLength;
				const double wantT = exp( -m_ProbeSigmaT * length );
				const double wantS = ( m_ProbeSigmaT > 0.0f ) ? m_ProbeSource / m_ProbeSigmaT * ( 1.0 - wantT ) : m_ProbeSource * length;
				const double gotS = F16toF32( row[k * 4 + 0] );
				const double gotT = F16toF32( row[k * 4 + 3] );
				maxErrorT = Max( maxErrorT, fabs( gotT - wantT ) );
				maxErrorS = Max( maxErrorS, fabs( gotS - wantS ) );
				if( m_ProbeLit )
				{
					if( c == 0 || k < 2 || ( ( k + 1 ) & k ) == 0 || k == m_ProbeSlices - 1 )
					{
						common->Printf( "fogprobe:   column %d,%d slice %2d far %8.2f  T %.5f ( closed form %.5f )  lit in-scatter %.6f %.6f %.6f\n",
										m_ProbeColumn[c][0], m_ProbeColumn[c][1], k, farFace, gotT, wantT, gotS, F16toF32( row[k * 4 + 1] ), F16toF32( row[k * 4 + 2] ) );
					}
				}
				else if( k < 2 || ( ( k + 1 ) & k ) == 0 || k == m_ProbeSlices - 1 )
				{
					common->Printf( "fogprobe:   column %d,%d slice %2d far %8.2f  T %.5f ( closed form %.5f )  in-scatter %.5f ( %.5f )\n",
									m_ProbeColumn[c][0], m_ProbeColumn[c][1], k, farFace, gotT, wantT, gotS, wantS );
				}
			}
			if( m_ProbeFogLights )
			{
				// V5: the closed forms above are the global fog's alone; the fog lights the inject took are not in them
				common->Printf( "fogprobe: column %d,%d ray length %.5f: the view took fog lights, which the closed form leaves out: no error is judged\n",
								m_ProbeColumn[c][0], m_ProbeColumn[c][1], rayLength );
			}
			else if( m_ProbeLit )
			{
				common->Printf( "fogprobe: column %d,%d ray length %.5f: largest error of T %.6f\n",
								m_ProbeColumn[c][0], m_ProbeColumn[c][1], rayLength, maxErrorT );
			}
			else
			{
				common->Printf( "fogprobe: column %d,%d ray length %.5f: largest error of T %.6f, of the in-scatter %.6f\n",
								m_ProbeColumn[c][0], m_ProbeColumn[c][1], rayLength, maxErrorT, maxErrorS );
			}
		}
	}
	m_Device->unmapStagingTexture( m_ProbeStaging );

	if( m_ProbeHasPixel )
	{
		const halfFloat_t* pixel = ( const halfFloat_t* )m_Device->mapStagingTexture( m_ProbePixelStaging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch );
		if( pixel != NULL )
		{
			common->Printf( "fogprobe: the picture's centre pixel behind the apply: %.5f %.5f %.5f\n", F16toF32( pixel[0] ), F16toF32( pixel[1] ), F16toF32( pixel[2] ) );
			m_Device->unmapStagingTexture( m_ProbePixelStaging );
		}
	}
	if( m_ProbeHasPixelLate )
	{
		const halfFloat_t* pixel = ( const halfFloat_t* )m_Device->mapStagingTexture( m_ProbePixelLateStaging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch );
		if( pixel != NULL )
		{
			common->Printf( "fogprobe: the picture's centre pixel behind the translucents: %.5f %.5f %.5f ( r_volumetricFogTranslucent %d )\n", F16toF32( pixel[0] ), F16toF32( pixel[1] ), F16toF32( pixel[2] ), r_volumetricFogTranslucent.GetInteger() );
			m_Device->unmapStagingTexture( m_ProbePixelLateStaging );
		}
	}

	// the GPU time of the fog's two blocks as the engine last measured them ( the HUD's "Volumetrics" and "Fog apply" )
	common->Printf( "fogprobe: GPU microseconds: Volumetrics %d, Fog apply %d\n", ( int )commonLocal.GetRendererGpuVolumetricsMicroseconds(), ( int )commonLocal.GetRendererGpuVolumetricsApplyMicroseconds() );
}

/*
========================
VolumetricFogPass::Apply

Fogs the opaque picture of the fog view, in the backend's MRB_VOLUMETRICS_APPLY block: one triangle over the
view, picture = in-scatter + picture * transmittance by blending, so the picture is not read. It writes the fog
set's real constants ( D66 ) and hands the integrated volume to the backend's fog set ( D27 ); the caller resets
the graphics state cache behind it.
========================
*/
void VolumetricFogPass::Apply( nvrhi::ICommandList* commandList, const viewDef_t* viewDef )
{
	if( m_RenderedView != viewDef || m_SkyMaskView != viewDef || m_SkyMask == NULL )
	{
		return;					// Render() did not fill the volumes for this view, or the view has no sky mask
	}
	m_RenderedView = NULL;
	m_SkyMaskView = NULL;

	Framebuffer* active = Framebuffer::GetActiveFramebuffer();
	if( active == NULL )
	{
		return;
	}
	const nvrhi::FramebufferDesc& activeDesc = active->GetApiObject()->getDesc();
	if( activeDesc.colorAttachments.empty() || !activeDesc.colorAttachments[0].valid() )
	{
		return;
	}
	nvrhi::ITexture* color = activeDesc.colorAttachments[0].texture;

	// The scene depth comes from mip 0 of the Hi-Z pyramid, a copy the backend makes right behind the depth prepass:
	// the depth attachment itself is neither sampled nor rebound here ( D83 ). Without the pyramid the depth image
	// is sampled, which costs its two transitions.
	nvrhi::ITexture* depth = R_UseHiZ() ? globalImages->hierarchicalZbufferImage->GetTextureHandle().Get() : globalImages->currentDepthImage->GetTextureHandle().Get();
	if( depth == NULL )
	{
		return;
	}

	if( !m_ApplyTarget || m_ApplyColor != color )
	{
		m_ApplyTarget = m_Device->createFramebuffer( nvrhi::FramebufferDesc().addColorAttachment( color ) );
		m_ApplyColor = color;
		m_ApplyPipeline = nullptr;			// a pipeline is made for a framebuffer's formats
	}
	if( !m_ApplyPipeline )
	{
		nvrhi::GraphicsPipelineDesc psoDesc;
		psoDesc.bindingLayouts = { m_ApplyLayout };
		psoDesc.VS = m_ApplyVS;
		psoDesc.PS = m_ApplyPS;
		psoDesc.primType = nvrhi::PrimitiveType::TriangleList;
		psoDesc.renderState.rasterState.setCullNone();
		psoDesc.renderState.depthStencilState.depthTestEnable = false;
		psoDesc.renderState.depthStencilState.depthWriteEnable = false;
		psoDesc.renderState.depthStencilState.stencilEnable = false;
		// source * ONE + destination * SRC_ALPHA: the shader returns the in-scatter and the transmittance. The
		// target's own alpha stays
		psoDesc.renderState.blendState.targets[0].blendEnable = true;
		psoDesc.renderState.blendState.targets[0].srcBlend = nvrhi::BlendFactor::One;
		psoDesc.renderState.blendState.targets[0].destBlend = nvrhi::BlendFactor::SrcAlpha;
		psoDesc.renderState.blendState.targets[0].srcBlendAlpha = nvrhi::BlendFactor::Zero;
		psoDesc.renderState.blendState.targets[0].destBlendAlpha = nvrhi::BlendFactor::One;
		m_ApplyPipeline = m_Device->createGraphicsPipeline( psoDesc, m_ApplyTarget );
	}

	const int viewWidth = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	const int viewHeight = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;

	froxelFogParms_t parms;
	parms.volumeParms.Set( ( float )viewWidth / ( m_GridWidth * m_ColumnPixels ), ( float )viewHeight / ( m_GridHeight * m_ColumnPixels ), ( float )m_GridSlices, 1.0f / m_GridSlices );
	parms.depthParms.Set( FROXEL_SLICE_BIAS, m_SliceStep, viewDef->projectionMatrix[3 * 4 + 2], viewDef->projectionMatrix[2 * 4 + 2] );
	parms.fogParms.Set( 1.0f, ( float )r_volumetricFogDebug.GetInteger(), ( float )viewDef->viewport.x1, ( float )viewDef->viewport.y1 );
	parms.reserved.Set( 1.0f / viewWidth, 1.0f / viewHeight, ( m_ViewGridWidth - 0.5f ) / m_GridWidth, ( m_ViewGridHeight - 0.5f ) / m_GridHeight );
	commandList->writeBuffer( backEnd.froxelFogParms, &parms, sizeof( parms ) );

	// from here on the engine's fog set binds this view's volume in place of the dummy
	backEnd.froxelVolume = m_Integrated;
	// V5: and the translucent draws after the apply fog themselves at their own depth ( D20, D83 ), until the end of
	// the view's translucent shader passes
	// ( review O1-3: also where the inject took fog lights, which the frontend did only with the cvar on: the fog pass skips
	// them, so the swap stays whole when the cvar changes between the frontend's frame and this one )
	backEnd.froxelFogTranslucent = r_volumetricFogTranslucent.GetBool() || m_RenderedFogLights;
	// V5 ( D21 ): the fog lights its inject took are drawn: FogAllLights leaves them to it
	backEnd.froxelFogLightsTaken = m_RenderedFogLights;

	// the set comes from the engine's cache, which ReleaseVolume() clears where the volume dies
	nvrhi::BindingSetDesc bindingSetDesc;
	bindingSetDesc.bindings =
	{
		nvrhi::BindingSetItem::ConstantBuffer( 1, backEnd.froxelFogParms ),
		nvrhi::BindingSetItem::Texture_SRV( 15, m_Integrated ),
		nvrhi::BindingSetItem::Sampler( 5, commonRenderPasses->m_LinearClampSampler ),
		nvrhi::BindingSetItem::Texture_SRV( 0, depth ),
		nvrhi::BindingSetItem::Texture_SRV( 1, m_SkyMask )
	};
	nvrhi::BindingSetHandle bindingSet = backEnd.bindingCache.GetOrCreateBindingSet( bindingSetDesc, m_ApplyLayout );

	const nvrhi::Viewport viewport( ( float )viewDef->viewport.x1, ( float )( viewDef->viewport.x2 + 1 ), ( float )viewDef->viewport.y1, ( float )( viewDef->viewport.y2 + 1 ), 0.0f, 1.0f );
	nvrhi::GraphicsState state;
	state.indexBuffer = {};					// the fork leaves this one member uninitialized
	state.pipeline = m_ApplyPipeline;
	state.framebuffer = m_ApplyTarget;
	state.bindings = { bindingSet };
	state.viewport.addViewport( viewport );
	state.viewport.addScissorRect( nvrhi::Rect( viewport ) );
	commandList->setGraphicsState( state );

	nvrhi::DrawArguments args;
	args.instanceCount = 1;
	args.vertexCount = 3;
	commandList->draw( args );
	// a direct NVRHI draw goes past DrawElementsWithCounters
	if( backEnd.concurrencyStats )
	{
		backEnd.pc.passDraws[backEnd.currentPass]++;
	}

	// a probe that this view's Render() started reads the picture behind the apply
	if( m_ProbeWait == FROXEL_PROBE_WAIT )
	{
		ProbeStage( commandList, color, viewDef );
	}
}

/*
========================
VolumetricFogPass::ProbeStageTranslucent

V5: the probe's centre pixel once more, behind the view's translucent shader passes, which fog themselves at their
own depth; the backend calls it at the end of those passes in every view, and it copies only in the view whose
apply staged the probe. ProbeRead prints both pixels.
========================
*/
bool VolumetricFogPass::ProbeStageTranslucent( nvrhi::ICommandList* commandList, const viewDef_t* viewDef )
{
	if( m_ProbeLateView != viewDef || m_ProbeLateColor == NULL )
	{
		return false;
	}
	const int centreX = ( viewDef->viewport.x1 + viewDef->viewport.x2 + 1 ) / 2;
	const int centreY = ( viewDef->viewport.y1 + viewDef->viewport.y2 + 1 ) / 2;
	commandList->copyTexture( m_ProbePixelLate, nvrhi::TextureSlice(),
							  m_ProbeLateColor, nvrhi::TextureSlice().setOrigin( centreX, centreY, 0 ).setSize( 1, 1, 1 ) );
	commandList->copyTexture( m_ProbePixelLateStaging, nvrhi::TextureSlice(), m_ProbePixelLate, nvrhi::TextureSlice() );
	m_ProbeHasPixelLate = true;
	m_ProbeLateColor = NULL;
	m_ProbeLateView = NULL;
	return true;
}

// The volumetric fog pass's debug-tools overlay; X0 lands it as a no-op and track V fills the body.
void VolumetricFogPass::DebugTools( nvrhi::ICommandList* commandList, const viewDef_t* viewDef )
{
}
