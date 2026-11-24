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
#include "precompiled.h"
#pragma hdrstop

#include "RenderCommon.h"
#include "VolumetricFog.h"

/*
==========================================================================================

THE FROXEL FOG'S FRONTEND ( track V of the renderer upgrade )

What the frontend does for the froxel fog's inject, which reads the view's light records, Z-bins and tile bits. The
light lists' frontend ( tr_frontend_lightlists.cpp ) builds them and calls each function below from a block fenced
TRACK V ( renderer/VolumetricFog.h ).

==========================================================================================
*/

static bool fogCapWarned = false;				// R_VolumetricFogRecordCap: this map has had the warning

// the casters that changed or went this frame, where each stood before its first change of the frame, in its world
// ( R_VolumetricFogCasterChanged )
struct fogChangedCaster_t
{
	idBounds					bounds;
	const idRenderWorldLocal*	world;
	int							index;		// the def's index in its world
};
static const int			FOG_MAX_CHANGED_CASTERS = 512;
static fogChangedCaster_t	fogChangedCasters[FOG_MAX_CHANGED_CASTERS];
static int					fogNumChangedCasters = 0;
static int			fogChangedFrame = -1;		// the tr.frameCount the list is of
static bool			fogChangedOverflow = false;	// more casters changed this frame than the list holds
// ---- TRACK V ( V6 ): the idle view ( bite V6h ) ----
// The frame of the last fog view and of the last fog view that read records. Equal: the fog is live, and the game tic
// gathers the changed casters for the view to come. Unequal: every fog view of that frame was idle; the list stays shut
// until a view reads records again. The first such view does not miss it: its history is not read ( the pass drew
// nothing the frame before, so the frame stamps do not meet, VolumetricFogPass::Render ). A frame with no fog view
// leaves both as they are. -1, -1: live until the first fog view says otherwise
static int			fogViewFrame = -1;
static const idRenderWorldLocal*	fogViewWorld = NULL;	// V7 ( review O1-1 ): the last fog view's world and view, for the light editor's status
static renderView_t	fogViewRenderView;
static int			fogLiveFrame = -1;
static bool			fogChangedStop = false;		// this frame's list takes no more: it overflowed, or the fog is idle
static bool FogViewTakesFogLight( const viewDef_t* viewDef, float reach );
// ---- END TRACK V ----

// V6 ( row 158's rider ): adds the wall time of its scope to a frontend counter ( tr.pc ) while r_volumetricFogZones is on
class idFogScopeTimer
{
public:
	explicit idFogScopeTimer( uint64& counter_ ) : counter( counter_ ), on( R_VolumetricFogZones() ), start( on ? Sys_Microseconds() : 0 ) {}
	~idFogScopeTimer()
	{
		if( on )
		{
			counter += Sys_Microseconds() - start;
		}
	}
private:
	uint64&		counter;
	const bool	on;
	const uint64	start;
};

/*
====================
R_VolumetricFogMapKeys

V4: the froxel fog's media of a map. The worldspawn's keys set the fog's cvars ( renderer/Passes/VolumetricFogPass.cpp )
at every spawn and restore of the worldspawn ( d3xp/WorldSpawn.cpp ), each to the cvar's default where the key is absent,
as the key "gravity" sets g_gravity: a map without the keys has none of these media, and a value typed in the console
holds until the next map is spawned or restored.
====================
*/
void R_VolumetricFogMapKeys( const idDict& worldspawnArgs )
{
	static const char* const keys[][2] =
	{
		{ "fog_height_distance",	"r_volumetricFogHeightDistance" },
		{ "fog_height",			"r_volumetricFogHeight" },
		{ "fog_height_falloff",	"r_volumetricFogHeightFalloff" },
		{ "fog_height_albedo",	"r_volumetricFogHeightAlbedo" },
		{ "fog_dust_distance",	"r_volumetricFogDustDistance" },
		{ "fog_dust_albedo",		"r_volumetricFogDustAlbedo" },
		{ "fog_dust_scale",		"r_volumetricFogDustScale" },
		{ "fog_dust_wind",		"r_volumetricFogDustWind" },
		{ "fog_dust_noise",		"r_volumetricFogDustNoise" },
		{ "fog_dust_octaves",	"r_volumetricFogDustOctaves" },
		{ "fog_dust_erode",		"r_volumetricFogDustErode" },
		{ "fog_dust_warp",		"r_volumetricFogDustWarp" }
	};
	for( int i = 0; i < ( int )( sizeof( keys ) / sizeof( keys[0] ) ); i++ )
	{
		idCVar* cvar = cvarSystem->Find( keys[i][1] );
		if( cvar != NULL )
		{
			cvar->SetString( worldspawnArgs.GetString( keys[i][0], cvar->GetDefaultString() ) );
		}
	}
}

/*
====================
R_VolumetricFogBeginView

The fog's switch ( D40 ), at the start of R_NumberRecordLights, for every view: how far along the view axis the fog's
inject reads this view's light records, 0 where it reads none. The fog runs in the main view without
RDF_IRRADIANCE, RDF_NOAMBIENT and RDF_NOFOG, and reads records while R_VolumetricFogRecordReach says so: the fog on,
air that scatters, no furnace. In the view the fog runs in, the records, the bins and the tile bits are then built
with the lists off as well; they list nothing ( lightListView_t::lists is the lists' own switch ). The first main
view of a map clears the fog's warnings.

V6 ( bite V6h ): the idle view. Fog is placed in volumes, as lights are. Where the cvars leave only the fog lights as a
medium ( R_VolumetricFogMayIdle ) and the view holds no fog light the froxel fog takes ( FogViewTakesFogLight: the verdict
the records would carry ), the pass draws nothing in this view ( FroxelHasMedium ), so the reach is 0 and the view builds
what it builds with the fog off; idle says so, for the light lists' history stamp.
====================
*/
float R_VolumetricFogBeginView( const viewDef_t* viewDef, bool& idle )
{
	idle = false;
	if( viewDef->invalidateHistory )
	{
		fogCapWarned = false;
	}
	if( !R_LightListFogEligible( viewDef ) )
	{
		return 0.0f;
	}
	float reach = R_VolumetricFogRecordReach();
	// ---- TRACK V ( V6 ): the idle view ----
	if( reach > 0.0f && R_VolumetricFogMayIdle() && !FogViewTakesFogLight( viewDef, reach ) )
	{
		idle = true;
		reach = 0.0f;
		tr.pc.c_fogIdleViews++;
	}
	fogViewFrame = tr.frameCount;
	fogViewWorld = viewDef->renderWorld;
	fogViewRenderView = viewDef->renderView;
	if( reach > 0.0f )
	{
		fogLiveFrame = tr.frameCount;
	}
	// ---- END TRACK V ----
	return reach;
}

/*
====================
R_VolumetricFogRecordFlags

The fog's bits of a light's record, OR'd into it by R_LightListFlattenRecord; recordFlags are the record's flags as
R_LightListRecordFlags decided them. An ambient record carries the kind's bit, which says which of the two default
falloff images the inject reads for it ( the stock ambient light material has the point light's ); the record is the
fog's alone and is never listed. The light's own word on the fog is the spawnarg "volumetric"
( renderLight_t::volumetricMode ); any other value reads as LIGHT_VOLUMETRIC_AUTO.
====================
*/
uint32 R_VolumetricFogRecordFlags( const viewLight_t* vLight, uint32 recordFlags )
{
	uint32 flags = 0;
	if( ( recordFlags & RLF_AMBIENT ) != 0 && vLight->pointLight )
	{
		flags |= RLF_POINT;
	}
	const int mode = vLight->lightDef->parms.volumetricMode;
	if( mode == LIGHT_VOLUMETRIC_ALWAYS )
	{
		flags |= RLF_FOG_NOSHADOW;
	}
	else if( mode == LIGHT_VOLUMETRIC_NEVER )
	{
		flags |= RLF_FOG_OFF;
	}
	return flags;
}

// V6 ( row 176 ): the band at the reach. FogLightWithinReach's last verdict per light def index: the light's uniqueId ( a
// reused index is another light; a light whose shape changes in place keeps its id and its verdict, and is measured again
// at the next question ), the eye it was made from, the view it was made in ( tr.frameCount and tr.viewDef: the portal rule
// asks first, once per visit of a fogged portal, then the record's verdict; all get the first answer ) and the epoch it
// belongs to. Only main fog views ask ( the three callers stand behind R_LightListFogEligible: no GUI world, subview or capture
// face ), on the game thread
struct fogReachVerdict_t
{
	idVec3	eye;	// the eye the verdict was made from
	int	uniqueId;	// the light's; a slot never written holds 0 and epoch 0
	int	frame;	// tr.frameCount and
	const viewDef_t*	view;	// the view the verdict was made in
	int	epoch;	// fogReachEpoch when it was made
	bool	taken;	// the light's volume was within the reach: the froxel fog takes it
};
static idList<fogReachVerdict_t, TAG_RENDER>	fogReachVerdicts;	// by light def index, grown as the indices come
static int	fogReachEpoch = 1;	// moves at the first reach question after a map load, or in another world: every verdict before it counts as never made ( reviews QR3-1, O1-6: a load whose first views ask nothing is skipped; the light's uniqueId, which never falls in a world, is the guard )
static int	fogReachFrame = -1;	// the view the epoch was last looked at in ( tr.frameCount and
static const viewDef_t*	fogReachView = NULL;	// the view ) and its world
static const idRenderWorld*	fogReachWorld = NULL;
// a verdict made from an eye this far from the asking eye is not this viewer's last: a teleport ( the backend's camera cut at
// 4096 units among them: the frontend cannot see that cut, it is decided later on the main thread ) or another viewer of the
// same world ( review CN2-1 ); the light is judged by the reach alone. Stereo eyes, a few units apart, share their verdicts
static const float	FOG_REACH_EYE_JUMP = 512.0f;

// V5e ( reviews O1-1, O1-4 ): whether a fog light's whole volume lies within the fog's reach of the eye, by the distance of
// its bounds' farthest corner. A view-depth test changed its answer as the view turned in place, and with it the light's
// look and the view's history; a corner's distance is never less than its view depth, so the volume holds every pixel
// behind the light either way. The record's verdict and the portal rule ask this one test. V6 ( row 176 ): with a band
// of hysteresis ( R_VolumetricFogLightReachBand ): a light not taken last time is taken only nearer than reach x ( 1 -
// band ), one taken is kept until its volume passes the reach, so the outer bound stays the reach and the volume always
// holds the whole extinction of a light it takes. A light never judged since the last map load, or last judged from an eye
// FOG_REACH_EYE_JUMP away, is judged by the reach alone, so a still view gets the verdict it had without the band. The test
// keeps the verdict it returns ( fogReachVerdicts )
static bool FogLightWithinReach( const idRenderLightLocal* ldef, float reach )
{
	const viewDef_t* viewDef = tr.viewDef;
	const idVec3& eye = viewDef->renderView.vieworg;
	// the first question of a view: a map load or another world starts a new epoch
	if( fogReachFrame != tr.frameCount || fogReachView != viewDef )
	{
		if( viewDef->invalidateHistory || fogReachWorld != viewDef->renderWorld )
		{
			if( fogReachEpoch == INT_MAX )
			{
				fogReachVerdicts.Clear();	// review CN2-6: the epochs start again with an empty table
				fogReachEpoch = 0;
			}
			fogReachEpoch++;
		}
		fogReachFrame = tr.frameCount;
		fogReachView = viewDef;
		fogReachWorld = viewDef->renderWorld;
	}
	// ---- TRACK V ( V6, bite V6h ): the answer this view already has, before the corners are measured: the idle test
	// asks first, then the record; the same answer as the test further down, which grows the table first ( a slot
	// past the table's end was never written ) ----
	if( ldef->index >= 0 && ldef->index < fogReachVerdicts.Num() )
	{
		const fogReachVerdict_t& asked = fogReachVerdicts[ ldef->index ];
		if( asked.uniqueId == ldef->uniqueId && asked.epoch == fogReachEpoch && asked.frame == tr.frameCount && asked.view == viewDef )
		{
			return asked.taken;
		}
	}
	// ---- END TRACK V ----
	const idBounds& bounds = ldef->globalLightBounds;
	float farDistanceSq = 0.0f;
	for( int c = 0; c < 8; c++ )
	{
		const idVec3 corner( bounds[c & 1].x, bounds[( c >> 1 ) & 1].y, bounds[( c >> 2 ) & 1].z );
		farDistanceSq = Max( farDistanceSq, ( corner - eye ).LengthSqr() );
	}
	if( ldef->index < 0 )
	{
		return farDistanceSq < reach * reach;
	}
	// review O1-3: room for the index at once; one Alloc() at a time grew the list 16 slots at a time, copying it each time
	if( fogReachVerdicts.NumAllocated() <= ldef->index )
	{
		fogReachVerdicts.Resize( ldef->index + 1 );
	}
	while( fogReachVerdicts.Num() <= ldef->index )
	{
		fogReachVerdicts.Alloc() = fogReachVerdict_t();	// value-initialised: zeros ( uniqueId 0, epoch 0: never made )
	}
	fogReachVerdict_t& verdict = fogReachVerdicts[ ldef->index ];
	const bool same = verdict.uniqueId == ldef->uniqueId && verdict.epoch == fogReachEpoch;
	if( same && verdict.frame == tr.frameCount && verdict.view == viewDef )
	{
		return verdict.taken;	// asked already in this view
	}
	const bool known = same && ( verdict.eye - eye ).LengthSqr() <= FOG_REACH_EYE_JUMP * FOG_REACH_EYE_JUMP;
	const float limit = ( known && !verdict.taken ) ? reach * ( 1.0f - R_VolumetricFogLightReachBand() ) : reach;
	verdict.eye = eye;
	verdict.uniqueId = ldef->uniqueId;
	verdict.frame = tr.frameCount;
	verdict.view = viewDef;
	verdict.epoch = fogReachEpoch;
	verdict.taken = farDistanceSq < limit * limit;
	return verdict.taken;
}

// V7: the light's and its material's part of R_VolumetricFogMayTakeFogLight's verdict ( below ), which the light editor's
// status shares
// ( R_VolumetricFogLightStatus ), so that the Fog tab's words cannot drift from the verdict: the first test that fails,
// FOGLIGHT_STATUS_FROXEL when none does
static fogLightStatus_t FogLightStaticStatus( const idRenderLightLocal* ldef )
{
	const idMaterial* shader = ldef->lightShader;
	if( shader == NULL || !shader->IsFogLight() )
	{
		return FOGLIGHT_STATUS_NOT_FOGLIGHT;
	}
	if( shader->GetNumStages() == 0 )
	{
		return FOGLIGHT_STATUS_NO_STAGE;
	}
	if( shader->Spectrum() != 0 )
	{
		return FOGLIGHT_STATUS_SPECTRUM;
	}
	for( int i = 0; i < shader->GetNumStages(); i++ )
	{
		if( shader->GetStage( i )->texture.hasMatrix )
		{
			return FOGLIGHT_STATUS_MATRIX;
		}
	}
	if( ldef->parms.parallel )
	{
		return FOGLIGHT_STATUS_PARALLEL;
	}
	if( ldef->parms.volumetricMode == LIGHT_VOLUMETRIC_NEVER )
	{
		return FOGLIGHT_STATUS_NEVER;
	}
	return FOGLIGHT_STATUS_FROXEL;
}

/*
====================
R_VolumetricFogMayTakeFogLight

V5 ( D21, D29, D57; rows 166, 170 c ): whether the froxel fog takes a fog light in place of the fog pass in the view being
built ( tr.viewDef ), all but the reach, which R_VolumetricFogMarkLights adds once the lights' view depths are known. The
fog view ( the backend's fogEligible ), the cvars ( R_VolumetricFogSwapsFogLights ), and the light: a fog light with a
stage, no texture matrix ( D57: no record ), not parallel ( a parallel light may hold the sun's slot, record 0, row 166 ),
not "volumetric" 2, of the default spectrum ( the record's channel bit 0 ), with an alpha share above 0. alpha is the first
stage's colour alpha as the caller evaluated it. PortalIsFoggedOut asks it too, so its portals are culled by the medium's
rule wherever the record may be taken: never by the fog pass's rule where the medium draws the fog ( S96 ).
====================
*/
bool R_VolumetricFogMayTakeFogLight( const idRenderLightLocal* ldef, float alpha )
{
	const viewDef_t* viewDef = tr.viewDef;
	if( viewDef == NULL || !R_LightListFogEligible( viewDef ) || !R_VolumetricFogSwapsFogLights() )
	{
		return false;
	}
	return FogLightStaticStatus( ldef ) == FOGLIGHT_STATUS_FROXEL && alpha > 0.0f;
}

/*
====================
R_VolumetricFogLightStatus

V7 ( plan section 3.5 ): which fog draws a fog light, and why, for the light editor's Fog tab. The light def is the last
fog view's world's, by its handle ( not tr.primaryWorld: a GUI's render window may draw after the player's view, review
O1-1 ). The light's and its material's tests are the verdict's own ( FogLightStaticStatus ); the strength is the first
stage's alpha evaluated as R_AddSingleLight evaluates it, from the last fog view's parms and time; the reach is
FogLightWithinReach's remembered verdict, if it was made in the last fog view ( a light the view did
not ask about is not in it ). Reads the frontend's tables and writes nothing: the light editor draws in idCommonLocal::Draw,
right after game->Draw, on the thread that has just run the frontend ( idGameThread::Run ), SMP or not ( review O2-4 ).
====================
*/
fogLightStatus_t R_VolumetricFogLightStatus( qhandle_t lightDefHandle )
{
	const idRenderWorldLocal* world = ( fogViewWorld != NULL && tr.worlds.FindIndex( const_cast<idRenderWorldLocal*>( fogViewWorld ) ) >= 0 ) ? fogViewWorld : NULL;
	if( world == NULL || lightDefHandle < 0 || lightDefHandle >= world->lightDefs.Num() || world->lightDefs[ lightDefHandle ] == NULL )
	{
		return FOGLIGHT_STATUS_UNKNOWN;
	}
	const idRenderLightLocal* ldef = world->lightDefs[ lightDefHandle ];
	const fogLightStatus_t lightStatus = FogLightStaticStatus( ldef );
	if( lightStatus == FOGLIGHT_STATUS_NOT_FOGLIGHT )
	{
		return lightStatus;
	}
	if( !cvarSystem->GetCVarBool( "r_volumetricFog" ) )
	{
		return FOGLIGHT_STATUS_FOG_OFF;
	}
	if( !R_VolumetricFogSwapsFogLights() )
	{
		return FOGLIGHT_STATUS_LIGHTS_OFF;
	}
	if( lightStatus != FOGLIGHT_STATUS_FROXEL )
	{
		return lightStatus;
	}
	const idMaterial* shader = ldef->lightShader;
	float* regs = ( float* )_alloca( Max( shader->GetNumRegisters(), 1 ) * sizeof( float ) );	// review O1-5: no heap per frame
	shader->EvaluateRegisters( regs, ldef->parms.shaderParms, fogViewRenderView.shaderParms, fogViewRenderView.time[0] * 0.001f,
							  ldef->parms.referenceSound );
	if( regs[ shader->GetStage( 0 )->color.registers[3] ] <= 0.0f )
	{
		return FOGLIGHT_STATUS_STRENGTH;
	}
	// the verdict FogLightWithinReach kept: the light's uniqueId restarts in every world, so the epoch ( moved at a map load
	// or another world ) guards it; the frame, not the view: the views of one frame ( stereo eyes ) share their verdicts
	// ( reviews QR2 F-2, QR4-6 )
	if( ldef->index >= 0 && ldef->index < fogReachVerdicts.Num() )
	{
		const fogReachVerdict_t& verdict = fogReachVerdicts[ ldef->index ];
		if( verdict.uniqueId == ldef->uniqueId && verdict.epoch == fogReachEpoch && verdict.frame == fogViewFrame )
		{
			return verdict.taken ? FOGLIGHT_STATUS_FROXEL : FOGLIGHT_STATUS_REACH;
		}
	}
	return FOGLIGHT_STATUS_NOT_IN_VIEW;
}

/*
====================
FogViewTakesFogLight

V6 ( bite V6h ): whether the view being built holds a fog light the froxel fog takes, asked of the view's lights
before any record exists. The lights are the view's own: the portals, the frustum and the scissor have culled them,
as they cull every light's volume. A light counts where its record would be RLF_FOGLIGHT without RLF_FOG_OFF:
a fog light, no blend light, with a scissor ( R_NumberRecordLights makes no record without one ), which
R_VolumetricFogMayTakeFogLight takes at its stage's alpha ( R_VolumetricFogLightRecord's question; it holds the texture
matrix, the mode and the spectrum ) and whose volume lies within the reach ( R_VolumetricFogMarkLights' question ). The
same functions, on the same lights, in the same view: FogLightWithinReach keeps one answer per light and view, so the
band's memory is written as the records' verdicts wrote it. It asks more lights than get a record only past the record
cap, which keeps today's path. False: FroxelViewHasFogLight would find no record in this view.
====================
*/
static bool FogViewTakesFogLight( const viewDef_t* viewDef, float reach )
{
	idFogScopeTimer timer( tr.pc.usFogFE );
	for( const viewLight_t* vLight = viewDef->viewLights; vLight != NULL; vLight = vLight->next )
	{
		const idMaterial* shader = vLight->lightShader;
		if( shader == NULL || !shader->IsFogLight() || shader->IsBlendLight() || vLight->scissorRect.IsEmpty() )
		{
			continue;
		}
		float alpha = 0.0f;
		if( shader->GetNumStages() > 0 )
		{
			alpha = vLight->shaderRegisters[ shader->GetStage( 0 )->color.registers[3] ];
		}
		if( R_VolumetricFogMayTakeFogLight( vLight->lightDef, alpha ) && FogLightWithinReach( vLight->lightDef, reach ) )
		{
			return true;
		}
	}
	return false;
}

/*
====================
R_VolumetricFogPortalDistance

V5 ( D21, S96 ): the distance along the view axis beyond which PortalIsFoggedOut may cull a fog light's portal, given
the fog pass's own D ( alpha, or DEFAULT_FOG_DISTANCE at an alpha of at most 1 ). Where the froxel fog may take the light
( R_VolumetricFogMayTakeFogLight ) the fog pass does not draw it, and an area beyond the portal shows only through the
medium: cull it only where every ray to the portal runs through the medium's full density for the opaque distance
D 5.54 / ( rate share ) ( transmittance 1/255; the share as the inject takes it ). The light's box is convex, so a segment between two points of it stays in
it, and the density is full deeper than the enter ramp below the fog plane on the whole segment when it is at both ends:
so the eye inside the box and both more than the ramp below the plane, every point of the portal too ( it lies in the
box: R_CreateLightDefFogPortals ), and a ray is never shorter than its end's view depth. The opaque distance must also
lie inside the fog's reach, past which the volume holds no more. 0: never cull ( the caller's test then keeps it ).
Elsewhere the fog pass draws the light: D.
====================
*/
float R_VolumetricFogPortalDistance( const idRenderLightLocal* ldef, const idWinding* w, float alpha )
{
	const float distance = ( alpha <= 1.0f ) ? DEFAULT_FOG_DISTANCE : alpha;
	if( !R_VolumetricFogMayTakeFogLight( ldef, alpha ) )
	{
		return distance;
	}
	// the medium's rate as the inject has it ( froxel_lights.inc.hlsl, FroxelAddFogLight ): an alpha of at most 1 is not a
	// distance but the fog pass's opacity at its most, Linear( alpha ), and scales the rate to the share that reaches it at
	// D ( consultant CN3-1: the raw alpha overstated the density and culled where the medium is still thin )
	const float lightRate = R_VolumetricFogLightRate();
	float share = 1.0f;
	if( alpha <= 1.0f && lightRate > 0.0f )
	{
		const float c = idMath::ClampFloat( 0.0f, 1.0f, alpha );
		const float opacity = ( c <= 0.04045f ) ? c / 12.92f : idMath::Pow( ( c + 0.055f ) / 1.055f, 2.4f );		// Linear1
		share = Min( 1.0f, -idMath::Log( Max( 1.0f - opacity, 1e-6f ) ) / lightRate );
	}
	else if( lightRate > 0.0f )
	{
		share = Min( 1.0f, 13.8155f / lightRate );		// review O1-4 b: an alpha above 1 is the inject's Linear( 1 ), -ln( 1e-6 )
	}
	const float rate = lightRate * share;
	const float reach = R_VolumetricFogRecordReach();
	if( reach <= 0.0f || !FogLightWithinReach( ldef, reach ) )
	{
		return distance;		// review O1-4 c: past the reach the record's verdict hands the light to the fog pass, and its rule holds
	}
	tr.pc.c_fogPortalsMedium++;		// V5f ( review F1-3; after the reach, V5g R-1 )
	if( ldef->parms.fogEdge > 0.0f )
	{
		return 0.0f;		// V7 ( row 241 ): soft sides thin the medium near the volume's faces, where the rule below takes it as full
	}
	if( rate <= 0.0f )
	{
		return 0.0f;
	}
	const float opaque = distance * 5.54f / rate;		// -ln( 1 / 255 )
	if( opaque >= reach )
	{
		return 0.0f;
	}
	// the fog plane in world space, as the frontend derives the view light's ( R_AddSingleLight ): the far plane of the
	// absolute projection, negative inside
	const idRenderMatrix& project = ldef->baseLightProject;
	idPlane fogPlane( project[2][0] - project[3][0], project[2][1] - project[3][1], project[2][2] - project[3][2], project[2][3] - project[3][3] );
	const float lengthSq = fogPlane.Normal().LengthSqr();
	if( lengthSq <= 0.0f )
	{
		return 0.0f;
	}
	const float planeScale = idMath::InvSqrt( lengthSq );
	for( int i = 0; i < 4; i++ )
	{
		fogPlane[i] *= planeScale;
	}
	const idVec3& eye = tr.viewDef->renderView.vieworg;
	if( fogPlane.Distance( eye ) > -FROXEL_FOGLIGHT_RAMP || idRenderMatrix::CullPointToMVP( project, eye, true ) )
	{
		return 0.0f;
	}
	for( int i = 0; i < w->GetNumPoints(); i++ )
	{
		if( fogPlane.Distance( ( *w )[i].ToVec3() ) > -FROXEL_FOGLIGHT_RAMP )
		{
			return 0.0f;
		}
	}
	return Max( opaque, distance );		// QR3-1: never nearer than the fog pass's own D, should the reach hand the light back to it
}

/*
====================
R_VolumetricFogLightRecord

V4: what a fog light's record carries for the froxel fog beyond the fog pass's colour and distance ( D81 ), set by
R_LightListFlattenRecord after it filled the record. ids.w: the scattering albedo, the light's spawnarg "fog_albedo"
( renderLight_t::fogAlbedo ), 0 by default, so that the fog only darkens and shines with its colour as the fog pass draws
it. The emitter lanes, which a fog light never uses ( a fog light is never RLF_EMITTER, and the record was cleared ):
emitter[0] the fog plane the fog pass fogs from, vLight->fogPlane ( unit length, relative to the view's render origin,
negative inside; a projected light's planes do not give it, consultant CN4-1 ), and emitter[1].x the share of the fog
the stage colour's alpha leaves where it is not a distance: the fog pass draws with the alpha clamped to 0 .. 1 as its
colour's alpha ( GL_Color ), so an alpha of at most 1 scales the fog's opacity and an alpha above 1 ( the distance ) does
not ( CN4-2 ). V7 ( row 156 ): emitter[1].y the glow, the spawnarg "fog_emission" ( 1 + renderLight_t::fogEmissionBias,
so 1 by default ), the factor of the fog's emission in place of 1 - albedo. V7 ( row 241 ): soft sides, for a light whose
spawnarg "fog_edge" is above 0: emitter[1].z the band's width in units, and the inverse lengths of the planes of the
volume's faces but the fog plane over that width, so that the inject has each face's distance in widths ( review O1-8;
the planes are world-unit planes in the relative frame, scout SV ): emitter[2] those of S = 0, S = Q, T = 0, T = Q
( lightProject[0], [0] - [2], [1], [1] - [2] ), emitter[1].w that of the falloff's near face ( lightProject[3] at 0; its
far face at 1 is the fog plane, which keeps its ramp ); a degenerate plane leaves the sides hard. Nothing for any other
record: its ids.w stays 0.
====================
*/
void R_VolumetricFogLightRecord( const viewLight_t* vLight, uint32 recordFlags, recordLight_t& rec )
{
	if( ( recordFlags & RLF_FOGLIGHT ) == 0 )
	{
		return;
	}
	rec.ids.w = idMath::ClampFloat( 0.0f, 1.0f, vLight->lightDef->parms.fogAlbedo );
	rec.emitter[0] = vLight->fogPlane.ToVec4();
	float share = 1.0f;
	float alpha = 0.0f;			// V5: the verdict below reads it too
	const idMaterial* shader = vLight->lightShader;
	if( shader->GetNumStages() > 0 )
	{
		alpha = vLight->shaderRegisters[ shader->GetStage( 0 )->color.registers[3] ];
		share = ( alpha <= 1.0f ) ? idMath::ClampFloat( 0.0f, 1.0f, alpha ) : 1.0f;
	}
	rec.emitter[1].Set( share, 1.0f + vLight->lightDef->parms.fogEmissionBias, 0.0f, 0.0f );
	const float edge = vLight->lightDef->parms.fogEdge;
	if( edge > 0.0f )
	{
		const idPlane* planes = vLight->lightProject;
		// in widths ( review O1-8: the inject needs no divide ), divided before the guard below checks them ( review CN4-2 )
		const float inverse[5] = { 1.0f / ( planes[0].Normal().Length() * edge ), 1.0f / ( ( planes[0].Normal() - planes[2].Normal() ).Length() * edge ),
									1.0f / ( planes[1].Normal().Length() * edge ), 1.0f / ( ( planes[1].Normal() - planes[2].Normal() ).Length() * edge ),
									1.0f / ( planes[3].Normal().Length() * edge ) };
		bool finite = true;
		for( int i = 0; i < 5; i++ )
		{
			finite &= inverse[i] > 0.0f && inverse[i] < idMath::INFINITUM;		// review CN2-4: a plane of no length, or of one past the float range
		}
		if( finite )
		{
			rec.emitter[1].z = edge;
			rec.emitter[1].w = inverse[4];
			rec.emitter[2].Set( inverse[0], inverse[1], inverse[2], inverse[3] );
		}
	}

	// V5 ( D21, D29 ): the verdict but for the reach ( R_VolumetricFogMarkLights adds it ): a fog light the froxel fog does not
	// take keeps the fog pass and stays out of the inject
	if( !R_VolumetricFogMayTakeFogLight( vLight->lightDef, alpha ) )
	{
		rec.flags |= RLF_FOG_OFF;
	}
	// V5 ( row 170 b, review F1-4 ): "moved" is any update of the def; a fog light faded by a script changed only its colour
	// or alpha: the inject's change term carries a colour's change exactly, a density's by the resolve's renormalisation
	// ( row 159; review F1-6 ): it does not clip its history unless its shape changed
	if( vLight->lightDef->lastShapeFrameNum != tr.frameCount )
	{
		rec.flags &= ~RLF_MOVED_THIS_FRAME;
	}
}

/*
====================
R_VolumetricFogBoundsList

V8 ( B0, CF1 section 1 ): the view's bounded media, froxelView_t::fogBounds. One entry per record the froxel fog takes as a
fog light after the reach's verdict ( R_FogCanTakeFogLight ), from its light's globalLightBounds in the frame relative to
the view's renderOrigin, with the record's index and whether it scatters ( its albedo, ids.w ). The pass takes the fog
lights by the same test ( its take mask ) and checks that each one it takes has its entry: a list that misses one bounds
nothing. Made only with r_volumetricFogBounds on and the fog lights taken. Each entry is a unit wider each way than the
light's bounds ( review O1-6: the inject takes the medium inside the record's planes, which round apart from this bounds;
near the eye under a zoom the box's margin is a small part of a unit ). Frame memory, as the caster boxes. Track M's placed
volumes join this list
====================
*/
static void R_VolumetricFogBoundsList( viewDef_t* viewDef, const lightListView_t* llv, const recordLight_t* records )
{
	if( !R_VolumetricFogBounds() || !R_VolumetricFogTakesFogLights() )
	{
		return;					// the pass bounds nothing with the switch off ( review CN2-1: nor is the list made ), nor where the
								// inject takes no fog light ( review O2-11 )
	}
	const lightListTables_t& t = llv->tables;
	int numBounds = 0;
	for( int r = 1; r < t.numRecords; r++ )
	{
		if( t.lightOfRecord[r] >= 0 && R_FogCanTakeFogLight( records[r] ) )
		{
			numBounds++;
		}
	}
	if( viewDef == NULL || numBounds == 0 )
	{
		return;
	}
	if( viewDef->froxelView == NULL )
	{
		viewDef->froxelView = ( froxelView_t* )R_ClearedFrameAlloc( sizeof( froxelView_t ), FRAME_ALLOC_UNKNOWN );
	}
	idVec4* out = ( idVec4* )R_FrameAlloc( 2 * numBounds * sizeof( idVec4 ), FRAME_ALLOC_UNKNOWN );
	int n = 0;
	for( int r = 1; r < t.numRecords; r++ )
	{
		const int input = t.lightOfRecord[r];
		if( input < 0 || !R_FogCanTakeFogLight( records[r] ) )
		{
			continue;
		}
		const idBounds& bounds = llv->lightOfInput[input]->lightDef->globalLightBounds;
		out[2 * n].Set( ( float )( bounds[0].x - viewDef->renderOrigin.x ) - 1.0f, ( float )( bounds[0].y - viewDef->renderOrigin.y ) - 1.0f,
						( float )( bounds[0].z - viewDef->renderOrigin.z ) - 1.0f, ( float )r );
		out[2 * n + 1].Set( ( float )( bounds[1].x - viewDef->renderOrigin.x ) + 1.0f, ( float )( bounds[1].y - viewDef->renderOrigin.y ) + 1.0f,
							( float )( bounds[1].z - viewDef->renderOrigin.z ) + 1.0f, ( records[r].ids.w > 0.0f ) ? 1.0f : 0.0f );
		n++;
	}
	viewDef->froxelView->fogBounds = out;
	viewDef->froxelView->numFogBounds = n;
}

/*
====================
R_VolumetricFogMarkLights

A light the inject can take counts as lit ( R_ViewLightIsLit ), so one whose surfaces are all out of sight keeps its
place in the view and its shadow maps: its fog is in sight. Marked: a light the fog can take ( R_FogCanTakeRecord,
with "can have a shadow map in this view" from its shadow LOD ) that begins nearer than the fog's far end. Called
while the fog reads the view's records; a view light the fog does not mark keeps the false it was made with
( R_ClearedFrameAlloc ).
====================
*/
void R_VolumetricFogMarkLights( lightListView_t* llv, recordLight_t* records, float fogReach )
{
	idFogScopeTimer timer( tr.pc.usFogFE );		// V6
	const lightListTables_t& t = llv->tables;
	// V6 ( row 158's rider; r_volumetricFogZones 1 ): the records the lists could not use, built because the fog reads this
	// view's records ( a record without a light, the empty sun slot, is not one ), and the changed casters' list as the views
	// read it
	if( R_VolumetricFogZones() )
	{
		for( int r = 0; r < t.numRecords; r++ )
		{
			if( t.lightOfRecord[r] >= 0 && ( !llv->lists || ( records[r].flags & RLF_CLUSTERED ) == 0 ) )		// review CN1-4: with the lists off every record is the fog's
			{
				tr.pc.c_fogRecords++;
			}
		}
		if( fogChangedFrame == tr.frameCount )
		{
			tr.pc.c_fogChangedCasters = Max( tr.pc.c_fogChangedCasters, fogChangedOverflow ? FOG_MAX_CHANGED_CASTERS + 1 : fogNumChangedCasters );
		}
	}
	// V5 ( D21 ): the reach of the verdict. A fog light the froxel fog may take is taken only where its whole volume lies
	// nearer than the fog's reach along the view axis, so that the volume holds its whole extinction for every pixel behind
	// it; one that reaches past it keeps the fog pass and stays out of the inject ( RLF_FOG_OFF )
	// The far end is the light's bounds', not its scissor's depth range, which is clipped to the view and capped at twice the
	// bins' far end, and is cleared with r_useLightScissors 0 ( consultant CN3-2 to CN3-4 ); by distance, not view depth,
	// so the verdict does not change as the view turns ( review O1-1 )
	for( int r = 1; r < t.numRecords; r++ )
	{
		const int input = t.lightOfRecord[r];
		if( input >= 0 && R_FogCanTakeFogLight( records[r] ) && !FogLightWithinReach( llv->lightOfInput[input]->lightDef, fogReach ) )
		{
			records[r].flags |= RLF_FOG_OFF;
		}
	}
	// V8 ( B0 ): the view's bounded media, after the verdict: the fog lights the froxel fog takes
	R_VolumetricFogBoundsList( tr.viewDef, llv, records );
	const bool fogLights = R_VolumetricFogTakesFogLights();		// V4: a fog light the inject takes is in sight as a medium
	// V4: a light only where something scatters it ( review O1-2; the pass's take mask goes by the same rule )
	const bool lights = R_VolumetricFogMediaScatter() || ( fogLights && R_FogLightsScatter( records, t.numRecords ) );
	for( int r = 0; r < t.numRecords; r++ )
	{
		const int input = t.lightOfRecord[r];
		if( input < 0 )
		{
			continue;														// the empty sun slot
		}
		viewLight_t* vLight = llv->lightOfInput[input];
		vLight->fogRelevant = llv->lights[input].zMin < fogReach && ( ( lights && R_FogCanTakeRecord( records[r], vLight->shadowLOD != -1 ) )
							  || ( fogLights && r > 0 && R_FogCanTakeFogLight( records[r] ) ) );
	}
}

/*
====================
R_FogShadowRegion

A box that holds the shadow a caster's box casts inside a light's volume, cleared where it casts none there.
R_ShadowBounds, the cull's box, runs to the volume's far bound on every axis where the light is not beyond the caster,
and for a parallel light, whose origin stands PARALLEL_LIGHT_FAKE_DISTANCE away, on nearly every axis. Here a parallel
light's shadow is the caster's box swept along the light's direction until it has left the volume through some face
( the least sweep over the three axes: a low sun's shadow leaves through the floor ), and any other light's the box
scaled from the light's origin until it has left the volume ( the least scale over the axes on which the box lies wholly
to one side of the origin ); a box that holds the origin shadows the whole volume.
====================
*/
static idBounds R_FogShadowRegion( const idBounds& caster, const idRenderLightLocal* light )
{
	const idBounds& volume = light->globalLightBounds;
	idBounds region = caster;
	if( light->parms.parallel )
	{
		idVec3 away = light->parms.origin - light->globalLightOrigin;		// the way the light's shadows go
		away.Normalize();
		float sweep = idMath::INFINITUM;
		for( int i = 0; i < 3; i++ )
		{
			if( away[i] > 1e-6f )
			{
				sweep = Min( sweep, ( volume[1][i] - caster[0][i] ) / away[i] );
			}
			else if( away[i] < -1e-6f )
			{
				sweep = Min( sweep, ( caster[1][i] - volume[0][i] ) / -away[i] );
			}
		}
		if( sweep > 0.0f && sweep < idMath::INFINITUM )
		{
			region.AddBounds( idBounds( caster[0] + away * sweep, caster[1] + away * sweep ) );
		}
	}
	else
	{
		const idVec3& origin = light->globalLightOrigin;
		float scale = idMath::INFINITUM;
		for( int i = 0; i < 3; i++ )
		{
			if( caster[0][i] > origin[i] )
			{
				scale = Min( scale, ( volume[1][i] - origin[i] ) / ( caster[0][i] - origin[i] ) );
			}
			else if( caster[1][i] < origin[i] )
			{
				scale = Min( scale, ( origin[i] - volume[0][i] ) / ( origin[i] - caster[1][i] ) );
			}
		}
		if( scale == idMath::INFINITUM )
		{
			return volume;
		}
		if( scale > 1.0f )
		{
			region.AddBounds( idBounds( origin + ( caster[0] - origin ) * scale, origin + ( caster[1] - origin ) * scale ) );
		}
	}
	if( !region.IntersectsBounds( volume ) )
	{
		region.Clear();
		return region;
	}
	region.IntersectSelf( volume );
	return region;
}

/*
====================
R_FogAddCasterBox

One more caster's region among a record's boxes: none where the view does not see it, a box of its own while there are
fewer than FOG_CASTER_BOXES, else merged into the box it grows least
====================
*/
static void R_FogAddCasterBox( const viewDef_t* viewDef, idBounds* boxes, int& numBoxes, const idBounds& region )
{
	if( region.IsCleared() || idRenderMatrix::CullBoundsToMVP( viewDef->worldCullMVP, region ) )
	{
		return;			// no shadow in the volume, or none the view sees
	}
	if( numBoxes < FOG_CASTER_BOXES )
	{
		boxes[numBoxes++] = region;
		return;
	}
	int best = 0;
	float bestGrowth = idMath::INFINITUM;
	for( int i = 0; i < FOG_CASTER_BOXES; i++ )
	{
		idBounds merged = boxes[i];
		merged.AddBounds( region );
		const float growth = merged.GetVolume() - boxes[i].GetVolume();
		if( growth < bestGrowth )
		{
			bestGrowth = growth;
			best = i;
		}
	}
	boxes[best].AddBounds( region );
}

/*
====================
R_VolumetricFogShadowFlags

The fog's shadow bits of a light's record, OR'd into it by R_BuildLightLists once the view's chains are final. A
light that casts shadows and has no caster in this view's chain, which holds every caster whose shadow can reach the
view ( R_AddSingleLight leaves out one whose shadow bounds miss the view's frustum ), gets no shadow map
( R_ViewLightHasShadowMap ): its rows read "not placed". Nothing shadows it inside the view, so the inject takes it
unshadowed; and like every other light that casts shadows it takes it times 1 - fade, the fade to unshadowed on its
last shadow LOD, and not at all at a shadow LOD of -1, where it has no rows ( the owner's row 50: a light with no
caster fades like its neighbours, so a lamp's fog does not change when a caster enters its box ). R_BuildShadowRows
writes the fade into its rows. Only at r_shadowCache 2 does such a light have a place in the atlas: the cache's
static map, whose casters' shadows all miss the view; the inject does not read it. The chain is gathered for every
light that casts shadows, whatever its shadow LOD ( R_AddSingleModel ), and it is final when this is called: the model
jobs have run, and the optimize pass, which empties the chain of a light that is not lit, runs behind it.
RLF_FOG_NOCASTER is a fact about the record, set whether or not the fog is on; RLF_FOG_CASTERMOVED and the record's
caster boxes ( froxelView_t ) are made only while the fog reads records.
====================
*/
uint32 R_VolumetricFogShadowFlags( viewDef_t* viewDef, int record, int numRecords, const viewLight_t* vLight )
{
	if( vLight->shadowLOD == -1 || !vLight->lightDef->LightCastsShadows() )
	{
		return 0;			// no shadow in the fog: what a caster does changes nothing of it
	}
	if( vLight->globalShadows == NULL )
	{
		return RLF_FOG_NOCASTER;
	}
	// V4g: nor for a light the fog does not take ( review F1-7: fogRelevant is R_VolumetricFogMarkLights' answer, the take
	// rule of review O1-2: a light only where something scatters it )
	// V6 ( bite V6h ): the light's own answer first; the three tests are as they were, and a light the fog does not take
	// no longer pays the reach's cvars
	if( !vLight->fogRelevant || !R_LightListFogEligible( viewDef ) || R_VolumetricFogRecordReach() <= 0.0f )
	{
		return 0;			// the fog does not run in this view, reads no records, or does not take this light: no history to clip
	}
	idFogScopeTimer timer( tr.pc.usFogFE );		// V6: the work past the early returns ( those cost a call and a few tests a record )
	// V3: a caster that changed this frame ( an entity that moved or animated, or went ) changes the light's shadow,
	// which the history cannot follow by the light's colour. The inject clips the history where that caster shadows:
	// the region of its shadow where it stands now ( the chain's casters whose def changed this frame; a light's chain
	// holds an entity's surfaces in a run, R_AddModels ) and where it stood ( R_VolumetricFogCasterChanged )
	const idRenderLightLocal* light = vLight->lightDef;
	idBounds boxes[FOG_CASTER_BOXES];
	int numBoxes = 0;
	const idRenderEntityLocal* last = NULL;
	for( const drawSurf_t* surf = vLight->globalShadows; surf != NULL; surf = surf->nextOnLight )
	{
		const idRenderEntityLocal* def = ( surf->space != NULL ) ? surf->space->entityDef : NULL;
		if( def == NULL || def == last )
		{
			continue;
		}
		last = def;
		if( def->lastModifiedFrameNum == tr.frameCount )
		{
			R_FogAddCasterBox( viewDef, boxes, numBoxes, R_FogShadowRegion( def->globalReferenceBounds, light ) );
		}
	}
	if( fogChangedFrame == tr.frameCount )
	{
		if( fogChangedOverflow )
		{
			numBoxes = 1;
			boxes[0] = light->globalLightBounds;			// more casters changed than the list holds: the whole volume
		}
		else
		{
			for( int i = 0; i < fogNumChangedCasters; i++ )
			{
				if( fogChangedCasters[i].world == viewDef->renderWorld && fogChangedCasters[i].bounds.IntersectsBounds( light->globalLightBounds ) )
				{
					R_FogAddCasterBox( viewDef, boxes, numBoxes, R_FogShadowRegion( fogChangedCasters[i].bounds, light ) );
				}
			}
		}
	}
	if( numBoxes == 0 )
	{
		return 0;
	}
	// the record's boxes, in the relative frame, made for the view when its first record has any ( frame memory,
	// cleared )
	if( viewDef->froxelView == NULL )
	{
		viewDef->froxelView = ( froxelView_t* )R_ClearedFrameAlloc( sizeof( froxelView_t ), FRAME_ALLOC_UNKNOWN );
	}
	if( viewDef->froxelView->casterBoxes == NULL )
	{
		viewDef->froxelView->casterBoxes = ( idVec4* )R_ClearedFrameAlloc( numRecords * 2 * FOG_CASTER_BOXES * sizeof( idVec4 ), FRAME_ALLOC_UNKNOWN );
	}
	idVec4* out = viewDef->froxelView->casterBoxes + record * 2 * FOG_CASTER_BOXES;
	for( int i = 0; i < numBoxes; i++ )
	{
		for( int j = 0; j < 2; j++ )
		{
			out[2 * i + j].Set( ( float )( boxes[i][j].x - viewDef->renderOrigin.x ), ( float )( boxes[i][j].y - viewDef->renderOrigin.y ),
								( float )( boxes[i][j].z - viewDef->renderOrigin.z ), ( i == 0 && j == 0 ) ? ( float )numBoxes : 0.0f );
		}
	}
	return RLF_FOG_CASTERMOVED;
}

/*
====================
R_VolumetricFogCasterChanged

idRenderWorldLocal::UpdateEntityDef and FreeEntityDef call it, beside the shadow cache's notify, before an entity's
def changes or goes: on the game thread, in the frame whose frontend then builds the records ( both see the same
tr.frameCount ). It keeps where a caster stood, whose shadow the fog's history holds: a caster that went, or moved out
of a light's volume, has no surface left in the light's chain. UpdateEntityDef calls it as well where an entity changes
in place ( an animation whose bounds, origin and axis stay: the shortcut that does not mark the def ). A def is kept
once a frame, where it stood first, and with its world, so that a GUI's world adds nothing to a view's. A def whose
shadow the main view does not draw is left out ( noShadow; the player's own body, suppressShadowInViewID ).
====================
*/
void R_VolumetricFogCasterChanged( const idRenderEntityLocal* def )
{
	idFogScopeTimer timer( tr.pc.usFogCasters );		// V6: every call, its tests included
	if( fogChangedFrame != tr.frameCount )
	{
		fogChangedFrame = tr.frameCount;
		fogNumChangedCasters = 0;
		fogChangedOverflow = false;
		// ---- TRACK V ( V6, bite V6h ): once a frame: shut while the fog is idle ( the last frame's fog views read no
		// records and the cvars leave only the fog lights ); a global fog or a medium set since opens it at once ----
		fogChangedStop = fogLiveFrame != fogViewFrame && R_VolumetricFogMayIdle();
		// ---- END TRACK V ----
	}
	if( fogChangedStop || def->parms.noShadow || def->parms.suppressShadowInViewID != 0 || def->globalReferenceBounds.IsCleared()
			|| R_VolumetricFogRecordReach() <= 0.0f )
	{
		return;
	}
	for( int i = 0; i < fogNumChangedCasters; i++ )
	{
		if( fogChangedCasters[i].index == def->index && fogChangedCasters[i].world == def->world )
		{
			return;
		}
	}
	if( fogNumChangedCasters == FOG_MAX_CHANGED_CASTERS )
	{
		fogChangedOverflow = true;
		fogChangedStop = true;			// V6 ( V6h ): the one flag the calls test
		return;
	}
	fogChangedCaster_t& caster = fogChangedCasters[fogNumChangedCasters++];
	caster.bounds = def->globalReferenceBounds;
	caster.world = def->world;
	caster.index = def->index;
}

/*
====================
R_VolumetricFogRecordCap

The light lists' warnings speak of the lists ( R_LightListLoud ). A view that builds its tables for the fog alone
lists nothing; of the record cap it says its own word, once a map: the lights past the cap have no record, so they
add nothing to the fog.
====================
*/
void R_VolumetricFogRecordCap( const lightListView_t* llv )
{
	if( !llv->lists && llv->stats.capped > 0 && !fogCapWarned )
	{
		fogCapWarned = true;
		common->Warning( "lightlists: %d lights past the %d records of a view add nothing to the froxel fog", llv->stats.capped, LL_MAX_RECORDS );
	}
}
