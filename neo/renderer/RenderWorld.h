/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2015 Robert Beckebans

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

#ifndef __RENDERWORLD_H__
#define __RENDERWORLD_H__

//puncho start - emitter lights
#include "Emitter.h"
//puncho end

/*
===============================================================================

	Render World

===============================================================================
*/

#define PROC_FILE_EXT				"proc"
#define	PROC_FILE_ID				"mapProcFile004"
// 004 ends every surface header with a vertexFormat token: 0 = 8 floats per vertex (xyz st normal),
// 1 = 12 (xyz st normal rgba). 003 has no token and 8 floats per vertex; the readers accept both
#define	PROC_FILE_ID_003			"mapProcFile003"
#define	PROC_VERTEX_FORMAT_BASIC	0
#define	PROC_VERTEX_FORMAT_COLOR	1
#define	PROC_VERTEX_FLOATS_BASIC	8		// floats per vertex of PROC_VERTEX_FORMAT_BASIC
#define	PROC_VERTEX_FLOATS_COLOR	12		// floats per vertex of PROC_VERTEX_FORMAT_COLOR

// the format version a .proc's first token names: 4 (PROC_FILE_ID), 3 (PROC_FILE_ID_003), 0 for
// any other token; every .proc reader checks the id with it
ID_INLINE int ProcFileVersion( const char* token )
{
	if( idStr::Icmp( token, PROC_FILE_ID ) == 0 )
	{
		return 4;
	}
	if( idStr::Icmp( token, PROC_FILE_ID_003 ) == 0 )
	{
		return 3;
	}
	return 0;
}


// shader parms
const int SHADERPARM_RED			= 0;
const int SHADERPARM_GREEN			= 1;
const int SHADERPARM_BLUE			= 2;
const int SHADERPARM_ALPHA			= 3;
const int SHADERPARM_TIMESCALE		= 3;
const int SHADERPARM_TIMEOFFSET		= 4;
const int SHADERPARM_DIVERSITY		= 5;	// random between 0.0 and 1.0 for some effects (muzzle flashes, etc)
const int SHADERPARM_MODE			= 7;	// for selecting which shader passes to enable
const int SHADERPARM_TIME_OF_DEATH	= 7;	// for the monster skin-burn-away effect enable and time offset

// model parms
const int SHADERPARM_MD3_FRAME		= 8;
const int SHADERPARM_MD3_LASTFRAME	= 9;
const int SHADERPARM_MD3_BACKLERP	= 10;

const int SHADERPARM_BEAM_END_X		= 8;	// for _beam models
const int SHADERPARM_BEAM_END_Y		= 9;
const int SHADERPARM_BEAM_END_Z		= 10;
const int SHADERPARM_BEAM_WIDTH		= 11;

const int SHADERPARM_SPRITE_WIDTH		= 8;
const int SHADERPARM_SPRITE_HEIGHT		= 9;

const int SHADERPARM_PARTICLE_STOPTIME = 8;	// don't spawn any more particles after this time

// guis
const int MAX_RENDERENTITY_GUI		= 3;

// the renderEntity_s::joints array needs to point at enough memory to store the number of joints rounded up to two for SIMD
ID_INLINE int SIMD_ROUND_JOINTS( int numJoints )
{
	return ( ( numJoints + 1 ) & ~1 );
}
ID_INLINE void SIMD_INIT_LAST_JOINT( idJointMat* joints, int numJoints )
{
	if( numJoints & 1 )
	{
		joints[numJoints] = joints[numJoints - 1];
	}
}

typedef bool( *deferredEntityCallback_t )( renderEntity_s*, const renderView_s* );

// ---- WP-J27 ( the owner, 2026-11-27: "make lights have their own bloom settings under a tab in the light editor, so that
// bloom per emissive source is configurable, because I sometimes want the regular unpointed glow, and sometimes I want the
// stars with configurable points/widths" ): one source's bloom. idLight fills it from its bloom_* keys and sets it on its
// visual model and on its modelTarget; the stage loop reads it where the model's emitting stages draw, and the bloom gives
// that light its own kernel, strength and boost ( BloomPass, under the gate; the shape under the Fourier bloom ). A cleared
// style ( memset 0, as every render entity starts ) is the view's bloom: shape 0, strength 1, the view's boost ----
enum
{
	BLOOM_STYLE_VIEW	= 0,		// the view's shape and values ( System Options )
	BLOOM_STYLE_STAR	= 1,		// diffraction spikes, the source's own values
	BLOOM_STYLE_GLOW	= 2			// the inverse-square glow, unpointed, the source's own values
};

struct renderBloomStyle_t
{
	byte		shape;				// BLOOM_STYLE_*
	byte		spikes;				// star: the arms through the lamp, 1 to 7 ( n arms are 2n points ); 0 for any other shape
	byte		hotBoostOwn;		// 1: hotBoost is the source's own; 0: the view's r_bloomHotBoost
	byte		pad;				// always 0
	float		intensityBias;		// the source's light enters the bloom times ( 1 + intensityBias ), 0 to 4 times; stored less 1 so a cleared style is 1
	float		hotBoost;			// stops, 0 to 8: how much brighter the source's hottest pixels enter the transform; read where hotBoostOwn is 1
	// the star's values, each the System Options row of the same name with its range; 0 for any other shape
	float		spikeRotation;		// 0 to 1
	float		spikeRadius;		// 0 to 1: the arms' length
	float		spikeBlur;			// 0 to 1: the arms' width at the lamp
	float		spikeSpread;		// 0 to 1
	float		spikeRatio;			// -1 to 1
	float		spikeRinging;		// 0 to 1
	float		spikeShift;			// 0 to 1
	float		spikeFalloff;		// 0 to 1 ( 0: the plan's fall )
	// the glow's values; 0 for any other shape
	float		glowRadius;			// 0 to 1
	float		glare;				// 0 to 1
};

// the keys ( idLight's spawnargs; the light editor's Bloom tab and the FGD write them ). A key that is absent takes the
// default below, which is the System Options row's own default ( BloomPass.cpp's cvars )
#define BLOOM_KEY_SHAPE				"bloom_shape"			// 0 the view's, 1 star, 2 glow
#define BLOOM_KEY_INTENSITY			"bloom_intensity"		// 1
#define BLOOM_KEY_HOT_BOOST			"bloom_hot_boost"		// absent: the view's
#define BLOOM_KEY_SPIKES			"bloom_spikes"			// 3
#define BLOOM_KEY_SPIKE_ROTATION	"bloom_spike_rotation"	// 0.5
#define BLOOM_KEY_SPIKE_RADIUS		"bloom_spike_radius"	// 0
#define BLOOM_KEY_SPIKE_BLUR		"bloom_spike_blur"		// 0
#define BLOOM_KEY_SPIKE_SPREAD		"bloom_spike_spread"	// 0
#define BLOOM_KEY_SPIKE_RATIO		"bloom_spike_ratio"		// 0
#define BLOOM_KEY_SPIKE_RINGING		"bloom_spike_ringing"	// 0.75
#define BLOOM_KEY_SPIKE_SHIFT		"bloom_spike_shift"		// 0.1
#define BLOOM_KEY_SPIKE_FALLOFF		"bloom_spike_falloff"	// 0
#define BLOOM_KEY_GLOW_RADIUS		"bloom_glow_radius"		// 1
#define BLOOM_KEY_GLARE				"bloom_glare"			// 0

// a style of the given shape at the keys' defaults: the view's for BLOOM_STYLE_VIEW ( the cleared style ), the System Options
// rows' defaults for a star or a glow
ID_INLINE void R_BloomStyleDefaults( renderBloomStyle_t& style, int shape )
{
	memset( &style, 0, sizeof( style ) );
	if( shape == BLOOM_STYLE_STAR )
	{
		style.shape = BLOOM_STYLE_STAR;
		style.spikes = 3;
		style.spikeRotation = 0.5f;
		style.spikeRinging = 0.75f;
		style.spikeShift = 0.1f;
	}
	else if( shape == BLOOM_STYLE_GLOW )
	{
		style.shape = BLOOM_STYLE_GLOW;
		style.glowRadius = 1.0f;
	}
}

// the styles a view can tell apart: slot 0 is the view's, so three sources' own at once ( the glow mask's green carries the
// slot; shaders/builtin/post/bloom_fft_cb.h has the same number )
#define BLOOM_STYLE_SLOTS 4

// true where the style is the view's bloom in every respect ( no slot of its own in the stage loop )
ID_INLINE bool R_BloomStyleIsView( const renderBloomStyle_t& style )
{
	return style.shape == BLOOM_STYLE_VIEW && style.intensityBias == 0.0f && style.hotBoostOwn == 0;
}
// ---- END WP-J27 ----


typedef struct renderEntity_s
{
	idRenderModel* 			hModel;				// this can only be null if callback is set

	int						entityNum;
	int						bodyId;

	// Entities that are expensive to generate, like skeletal models, can be
	// deferred until their bounds are found to be in view, in the frustum
	// of a shadowing light that is in view, or contacted by a trace / overlay test.
	// This is also used to do visual cueing on items in the view
	// The renderView may be NULL if the callback is being issued for a non-view related
	// source.
	// The callback function should clear renderEntity->callback if it doesn't
	// want to be called again next time the entity is referenced (ie, if the
	// callback has now made the entity valid until the next updateEntity)
	idBounds				bounds;					// only needs to be set for deferred models and md5s
	deferredEntityCallback_t	callback;

	void* 					callbackData;			// used for whatever the callback wants

	// player bodies and possibly player shadows should be suppressed in views from
	// that player's eyes, but will show up in mirrors and other subviews
	// security cameras could suppress their model in their subviews if we add a way
	// of specifying a view number for a remoteRenderMap view
	int						suppressSurfaceInViewID;
	int						suppressShadowInViewID;

	// world models for the player and weapons will not cast shadows from view weapon
	// muzzle flashes
	int						suppressShadowInLightID;

	// if non-zero, the surface and shadow (if it casts one)
	// will only show up in the specific view, ie: player weapons
	int						allowSurfaceInViewID;

	// positioning
	// axis rotation vectors must be unit length for many
	// R_LocalToGlobal functions to work, so don't scale models!
	// axis vectors are [0] = forward, [1] = left, [2] = up
	idVec3					origin;
	idMat3					axis;

	// texturing
	const idMaterial* 		customShader;			// if non-0, all surfaces will use this
	const idMaterial* 		referenceShader;		// used so flares can reference the proper light shader
	const idDeclSkin* 		customSkin;				// 0 for no remappings
	class idSoundEmitter* 	referenceSound;			// for shader sound tables, allowing effects to vary with sounds
	float					shaderParms[ MAX_ENTITY_SHADER_PARMS ];	// can be used in any way by shader or model generation

	// networking: see WriteGUIToSnapshot / ReadGUIFromSnapshot
	class idUserInterface* gui[ MAX_RENDERENTITY_GUI ];

	struct renderView_s*		remoteRenderView;		// any remote camera surfaces will use this

	int						numJoints;
	idJointMat* 			joints;					// array of joints that will modify vertices.
	// NULL if non-deformable model.  NOT freed by renderer

	float					modelDepthHack;			// squash depth range so particle effects don't clip into walls

	// options to override surface shader flags (replace with material parameters?)
	bool					noSelfShadow;			// cast shadows onto other objects,but not self
	bool					noShadow;				// no shadow at all

	bool					noDynamicInteractions;	// don't create any light / shadow interactions after
	// the level load is completed.  This is a performance hack
	// for the gigantic outdoor meshes in the monorail map, so
	// all the lights in the moving monorail don't touch the meshes

	bool					weaponDepthHack;		// squash depth range so view weapons don't poke into walls
	// this automatically implies noShadow
	bool					noOverlays;				// force no overlays on this model
	bool					skipMotionBlur;			// Mask out this object during motion blur
	bool					noGlow;					// WP-J21: the model writes no glow mask, so none of its stages reaches the bloom; set by idLight on its visual model from the light's spawnarg "emit_glow" 0 ( a cleared entity glows ); it takes one of the two bytes before forceUpdate, so the struct keeps its size
	int						forceUpdate;			// force an update (NOTE: not a bool to keep this struct a multiple of 4 bytes)
	int						timeGroup;
	int						xrayIndex;
	float					maxVisDist;
	float					minVisPixels;
	int						cullShadow;
	float					lodScale;
	renderBloomStyle_t		bloomStyle;				// WP-J27: the source's own bloom ( a light's bloom_* keys, on its visual model and its modelTarget ); a cleared one is the view's
} renderEntity_t;


typedef struct renderLight_s
{
	idMat3					axis;				// rotation vectors, must be unit length
	idVec3					origin;

	// if non-zero, the light will not show up in the specific view,
	// which may be used if we want to have slightly different muzzle
	// flash lights for the player and other views
	int						suppressLightInViewID;

	// if non-zero, the light will only show up in the specific view
	// which can allow player gun gui lights and such to not effect everyone
	int						allowLightInViewID;

	// I am sticking the bools and the byte field together so there are no unused
	// gaps in the padded structure, which could confuse the memcmp that checks for
	// redundant updates
	bool					forceShadows;		// Used to override the material parameters
	bool					noShadows;			// (should we replace this with material parameters on the shader?)
	bool					noSpecular;			// (should we replace this with material parameters on the shader?)

	bool					pointLight;			// otherwise a projection light (should probably invert the sense of this, because points are way more common)
	bool					parallel;			// lightCenter gives the direction to the light at infinity
	bool					primarySun;			// spawnarg "primarySun": this parallel light is the map's sun ( D42, D73 )
	byte					forwardMode;		// spawnarg "forward": 0 = default, 1 = force the light list, 2 = force legacy
	byte					volumetricMode;		// spawnarg "volumetric": 0 = default ( the froxel fog takes the light where its shadow is known ), 1 = also where it has no shadow map, 2 = never
	// ---- WP-J21: the emitter's switch, a full group of its own so the redundant-update memcmp sees no gap ----
	bool					noGlow;				// spawnarg "emit_glow" 0: the light's fog never glows in the bloom ( RLF_FOG_NOGLOW ) and its visual model writes no glow mask ( renderEntity_t::noGlow ); stored inverted so that a cleared render light glows, the default 1
	byte					glowPad[3];			// always 0, as a cleared render light has it
	// ---- END WP-J21 ----
	float					fogAlbedo;			// spawnarg "fog_albedo" ( V4 ): a fog light's scattering albedo in the froxel fog, 0 to 1: the share of its fog that scatters the lights and the ambient; 0 = it scatters nothing, as the fog pass draws it
	float					fogEmissionBias;	// spawnarg "fog_emission" ( V7 ) minus 1: how strongly a fog light's froxel fog glows with its colour, 0 to 8, 1 by default ( as the fog pass draws it ); stored less 1 so that a cleared render light keeps the default
	float					fogEdge;			// spawnarg "fog_edge" ( V7 ): soft sides: within this many units of a fog light's faces, all but its fog plane's ( an unrotated box's top, a projected light's far face ), its froxel fog thins to nothing; 0 = hard sides, up to 4,096
	idVec3					lightRadius;		// xyz radius for point lights
	idVec3					lightCenter;		// offset the lighting direction for shading and
	// shadows, relative to origin

	// frustum definition for projected lights, all reletive to origin
	// FIXME: we should probably have real plane equations here, and offer
	// a helper function for conversion from this format
	idVec3					target;
	idVec3					right;
	idVec3					up;
	idVec3					start;
	idVec3					end;

	// muzzle flash lights will not cast shadows from player and weapon world models
	int						lightId;


	const idMaterial* 		shader;				// NULL = either lights/defaultPointLight or lights/defaultProjectedLight
	float					shaderParms[MAX_ENTITY_SHADER_PARMS];		// can be used in any way by shader
	idSoundEmitter* 		referenceSound;		// for shader sound tables, allowing effects to vary with sounds
	//puncho start - emitter block
	emitterData_t			emitter;
	//puncho end
	float					maxVisDist;
	float					minVisPixels;
} renderLight_t;

// parallel (sun) lights are faked as a point this far along their direction.
// 1e9 keeps the direction error under 0.03 degrees anywhere in the world:
// the MIN_WORLD_COORD..MAX_WORLD_COORD diagonal is ~454k units, atan( 454k / 1e9 ) ~= 0.026.
// Never larger (and never INFINITUM): the shaders take the vector to this point
// ( interactionSM.vs.hlsl:144 and :213 ) and normalize it, which forms dot( L, L ) in fp32,
// so |L| must stay below sqrt( FLT_MAX ) ~= 1.8e19.
const float PARALLEL_LIGHT_FAKE_DISTANCE = 1.0e9f;

// RB begin
typedef struct
{
	idVec3					origin;
	float					shaderParms[MAX_ENTITY_SHADER_PARMS];

	// if non-zero, the environment probe will not show up in the specific view,
	// which may be used if we want to have slightly different muzzle
	// flash lights for the player and other views
	int						suppressEnvprobeInViewID;

	// if non-zero, the environment probe will only show up in the specific view
	// which can allow player gun gui lights and such to not effect everyone
	int						allowEnvprobeInViewID;

} renderEnvironmentProbe_t;
// RB end


// RB: added back refdef flags from Quake 3
const int RDF_NOSHADOWS		= BIT( 0 ); // force renderer to use faster lighting only path
const int RDF_NOAMBIENT		= BIT( 1 ); // don't render indirect lighting
const int RDF_IRRADIANCE	= BIT( 2 ); // render into 256^2 HDR render target for irradiance/radiance GGX calculation
const int RDF_UNDERWATER	= BIT( 3 ); // TODO enable automatic underwater caustics and fog
const int RDF_MAINVIEW		= BIT( 4 ); // the player's main 3D view: the only view that stores previous-frame state (motion vectors, TAA history)
const int RDF_NOFOG			= BIT( 5 ); // no froxel fog in this view ( the portal sky view ); set by the game

typedef struct renderView_s
{
	// player views will set this to a non-zero integer for model suppress / allow
	// subviews (mirrors, cameras, etc) will always clear it to zero
	int						viewID;

	float					fov_x, fov_y;		// in degrees
	idVec3					vieworg;			// has already been adjusted for stereo world seperation
	idVec3					vieworg_weapon;		// has already been adjusted for stereo world seperation
	idMat3					viewaxis;			// transformation matrix, view looks down the positive X axis

	bool					cramZNear;			// for cinematics, we want to set ZNear much lower
	bool					flipProjection;
	bool					forceUpdate;		// for an update

	// time in milliseconds for shader effects and other time dependent rendering issues
	int						time[2];
	float					shaderParms[MAX_GLOBAL_SHADER_PARMS];		// can be used in any way by shader
	const idMaterial*		globalMaterial;							// used to override everything draw

	// the viewEyeBuffer may be of a different polarity than stereoScreenSeparation if the eyes have been swapped
	int						viewEyeBuffer;				// -1 = left eye, 1 = right eye, 0 = monoscopic view or GUI
	float					stereoScreenSeparation;		// projection matrix horizontal offset, positive or negative based on camera eye

	int						rdflags;			// RB: RDF_NOSHADOWS, etc
} renderView_t;


// exitPortal_t is returned by idRenderWorld::GetPortal()
typedef struct
{
	int					areas[2];		// areas connected by this portal
	const idWinding*		w;				// winding points have counter clockwise ordering seen from areas[0]
	int					blockingBits;	// PS_BLOCK_VIEW, PS_BLOCK_AIR, etc
	qhandle_t			portalHandle;
} exitPortal_t;


// guiPoint_t is returned by idRenderWorld::GuiTrace()
typedef struct
{
	float				x, y;			// 0.0 to 1.0 range if trace hit a gui, otherwise -1
	int					guiId;			// id of gui ( 0, 1, or 2 ) that the trace happened against
} guiPoint_t;


// modelTrace_t is for tracing vs. visual geometry
typedef struct modelTrace_s
{
	float					fraction;			// fraction of trace completed
	idVec3					point;				// end point of trace in global space
	idVec3					normal;				// hit triangle normal vector in global space
	const idMaterial* 		material;			// material of hit surface
	const renderEntity_t* 	entity;				// render entity that was hit
	int						jointNumber;		// md5 joint nearest to the hit triangle
} modelTrace_t;


static const int NUM_PORTAL_ATTRIBUTES = 3;

typedef enum
{
	PS_BLOCK_NONE = 0,

	PS_BLOCK_VIEW = 1,
	PS_BLOCK_LOCATION = 2,		// game map location strings often stop in hallways
	PS_BLOCK_AIR = 4,			// windows between pressurized and unpresurized areas

	PS_BLOCK_ALL = ( 1 << NUM_PORTAL_ATTRIBUTES ) - 1
} portalConnection_t;

// Admer: Suppress Windows API DrawText, so IntelliSense can properly highlight idRenderWorld::DrawText
#ifdef _WIN32
	#ifdef DrawText
		#undef DrawText
	#endif
#endif

class idRenderWorld
{
public:
	virtual					~idRenderWorld() {};

	// The same render world can be reinitialized as often as desired
	// a NULL or empty mapName will create an empty, single area world
	virtual bool			InitFromMap( const char* mapName ) = 0;

	// This fixes a crash when switching between expansion packs in the same game session
	// the modelManager gets reset, which deletes all render models without resetting the localModels list inside renderWorldLocal.
	// Now we'll have a hook to reset the list from here.
	virtual void			ResetLocalRenderModels() = 0;

	//-------------- Entity and Light Defs -----------------

	// entityDefs and lightDefs are added to a given world to determine
	// what will be drawn for a rendered scene.  Most update work is defered
	// until it is determined that it is actually needed for a given view.
	virtual	qhandle_t		AddEntityDef( const renderEntity_t* re ) = 0;
	virtual	void			UpdateEntityDef( qhandle_t entityHandle, const renderEntity_t* re ) = 0;
	virtual	void			FreeEntityDef( qhandle_t entityHandle ) = 0;
	virtual const renderEntity_t* GetRenderEntity( qhandle_t entityHandle ) const = 0;

	virtual	qhandle_t		AddLightDef( const renderLight_t* rlight ) = 0;
	virtual	void			UpdateLightDef( qhandle_t lightHandle, const renderLight_t* rlight ) = 0;
	virtual	void			FreeLightDef( qhandle_t lightHandle ) = 0;
	virtual const renderLight_t* GetRenderLight( qhandle_t lightHandle ) const = 0;

	// RB: environment probes for IBL
	virtual	qhandle_t		AddEnvprobeDef( const renderEnvironmentProbe_t* ep ) = 0;
	virtual	void			UpdateEnvprobeDef( qhandle_t envprobeHandle, const renderEnvironmentProbe_t* ep ) = 0;
	virtual	void			FreeEnvprobeDef( qhandle_t envprobeHandle ) = 0;
	virtual const renderEnvironmentProbe_t* GetRenderEnvprobe( qhandle_t envprobeHandle ) const = 0;
	// RB end

	// Force the generation of all light / surface interactions at the start of a level
	// If this isn't called, they will all be dynamically generated
	virtual	void			GenerateAllInteractions() = 0;
	virtual void GenerateTransitionInteractions( const idVec3* origins, int count ) { GenerateAllInteractions(); }
	virtual void FinishInteractionSlice() {}

	// returns true if this area model needs portal sky to draw
	virtual bool			CheckAreaForPortalSky( int areaNum ) = 0;

	//-------------- Decals and Overlays  -----------------

	// Creates decals on all world surfaces that the winding projects onto.
	// The projection origin should be infront of the winding plane.
	// The decals are projected onto world geometry between the winding plane and the projection origin.
	// The decals are depth faded from the winding plane to a certain distance infront of the
	// winding plane and the same distance from the projection origin towards the winding.
	virtual void			ProjectDecalOntoWorld( const idFixedWinding& winding, const idVec3& projectionOrigin, const bool parallel, const float fadeDepth, const idMaterial* material, const int startTime ) = 0;

	// Creates decals on static models.
	virtual void			ProjectDecal( qhandle_t entityHandle, const idFixedWinding& winding, const idVec3& projectionOrigin, const bool parallel, const float fadeDepth, const idMaterial* material, const int startTime ) = 0;

	// Creates overlays on dynamic models.
	virtual void			ProjectOverlay( qhandle_t entityHandle, const idPlane localTextureAxis[2], const idMaterial* material, const int startTime ) = 0;

	// Removes all decals and overlays from the given entity def.
	virtual void			RemoveDecals( qhandle_t entityHandle ) = 0;

	//-------------- Scene Rendering -----------------

	// some calls to material functions use the current renderview time when servicing cinematics.  this function
	// ensures that any parms accessed (such as time) are properly set.
	virtual void			SetRenderView( const renderView_t* renderView ) = 0;

	// rendering a scene may actually render multiple subviews for mirrors and portals, and
	// may render composite textures for gui console screens and light projections
	// It would also be acceptable to render a scene multiple times, for "rear view mirrors", etc
	virtual void			RenderScene( const renderView_t* renderView ) = 0;

	//-------------- Portal Area Information -----------------

	// returns the number of portals
	virtual int				NumPortals() const = 0;

	// returns 0 if no portal contacts the bounds
	// This is used by the game to identify portals that are contained
	// inside doors, so the connection between areas can be topologically
	// terminated when the door shuts.
	virtual	qhandle_t		FindPortal( const idBounds& b ) const = 0;

	// doors explicitly close off portals when shut
	// multiple bits can be set to block multiple things, ie: ( PS_VIEW | PS_LOCATION | PS_AIR )
	virtual	void			SetPortalState( qhandle_t portal, int blockingBits ) = 0;
	virtual int				GetPortalState( qhandle_t portal ) = 0;

	// returns true only if a chain of portals without the given connection bits set
	// exists between the two areas (a door doesn't separate them, etc)
	virtual	bool			AreasAreConnected( int areaNum1, int areaNum2, portalConnection_t connection ) const = 0;

	// returns the number of portal areas in a map, so game code can build information
	// tables for the different areas
	virtual	int				NumAreas() const = 0;

	// Will return -1 if the point is not in an area, otherwise
	// it will return 0 <= value < NumAreas()
	virtual int				PointInArea( const idVec3& point ) const = 0;

	// fills the *areas array with the numbers of the areas the bounds cover
	// returns the total number of areas the bounds cover
	virtual int				BoundsInAreas( const idBounds& bounds, int* areas, int maxAreas ) const = 0;

	// Used by the sound system to do area flowing
	virtual	int				NumPortalsInArea( int areaNum ) = 0;

	// returns one portal from an area
	virtual exitPortal_t	GetPortal( int areaNum, int portalNum ) = 0;

	// RB: returns the AABB of a BSP area
	virtual	idBounds		AreaBounds( int areaNum ) const = 0;

	//-------------- Tracing  -----------------

	// Checks a ray trace against any gui surfaces in an entity, returning the
	// fraction location of the trace on the gui surface, or -1,-1 if no hit.
	// This doesn't do any occlusion testing, simply ignoring non-gui surfaces.
	// start / end are in global world coordinates.
	virtual guiPoint_t		GuiTrace( qhandle_t entityHandle, const idVec3 start, const idVec3 end ) const = 0;

	// Traces vs the render model, possibly instantiating a dynamic version, and returns true if something was hit
	virtual bool			ModelTrace( modelTrace_t& trace, qhandle_t entityHandle, const idVec3& start, const idVec3& end, const float radius ) const = 0;

	// Traces vs the whole rendered world. FIXME: we need some kind of material flags.
	virtual bool			Trace( modelTrace_t& trace, const idVec3& start, const idVec3& end, const float radius, bool skipDynamic = true, bool skipPlayer = false ) const = 0;

	// Traces vs the world model bsp tree.
	virtual bool			FastWorldTrace( modelTrace_t& trace, const idVec3& start, const idVec3& end ) const = 0;

	//-------------- Demo Control  -----------------

	// this is used to regenerate all interactions ( which is currently only done during influences ), there may be a less
	// expensive way to do it
	virtual void			RegenerateWorld() = 0;

	//-------------- Debug Visualization  -----------------

	// Line drawing for debug visualization
	virtual void			DebugClearLines( int time ) = 0;		// a time of 0 will clear all lines and text
	virtual void			DebugLine( const idVec4& color, const idVec3& start, const idVec3& end, const int lifetime = 0, const bool depthTest = false ) = 0;
	virtual void			DebugArrow( const idVec4& color, const idVec3& start, const idVec3& end, int size, const int lifetime = 0 ) = 0;
	virtual void			DebugWinding( const idVec4& color, const idWinding& w, const idVec3& origin, const idMat3& axis, const int lifetime = 0, const bool depthTest = false ) = 0;
	virtual void			DebugCircle( const idVec4& color, const idVec3& origin, const idVec3& dir, const float radius, const int numSteps, const int lifetime = 0, const bool depthTest = false ) = 0;
	virtual void			DebugSphere( const idVec4& color, const idSphere& sphere, const int lifetime = 0, bool depthTest = false ) = 0;
	virtual void			DebugBounds( const idVec4& color, const idBounds& bounds, const idVec3& org = vec3_origin, const int lifetime = 0 ) = 0;
	virtual void			DebugBox( const idVec4& color, const idBox& box, const int lifetime = 0 ) = 0;
	virtual void			DebugCone( const idVec4& color, const idVec3& apex, const idVec3& dir, float radius1, float radius2, const int lifetime = 0 ) = 0;
	virtual void			DebugAxis( const idVec3& origin, const idMat3& axis ) = 0;

	// Polygon drawing for debug visualization.
	virtual void			DebugClearPolygons( int time ) = 0;		// a time of 0 will clear all polygons
	virtual void			DebugPolygon( const idVec4& color, const idWinding& winding, const int lifeTime = 0, const bool depthTest = false ) = 0;

	// Text drawing for debug visualization.
	virtual void			DrawText( const char* text, const idVec3& origin, float scale, const idVec4& color, const idMat3& viewAxis, const int align = 1, const int lifetime = 0, bool depthTest = false ) = 0;
};

#endif /* !__RENDERWORLD_H__ */
