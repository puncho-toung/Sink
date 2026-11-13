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
#ifndef __LIGHTLISTS_H__
#define __LIGHTLISTS_H__

/*
==========================================================================================

LIGHT LIST SEAMS ( X0 )

The GPU light record ( C1 ) and the shadow rows ( C2 ), with their HLSL mirrors, and
the frontend hooks that will build them. In X0 nothing fills them yet.
Included from RenderCommon.h, so it sees idVec4, uint32 and the assert macros.

==========================================================================================
*/

// flags of recordLight_t::flags
enum
{
	RLF_SUN					= BIT( 0 ),		// the map's primary sun; record 0 from F4 on ( D42, D73 )
	RLF_POINT				= BIT( 1 ),		// point light
	RLF_PARALLEL			= BIT( 2 ),		// parallel ( sun-like ) light
	RLF_MATH				= BIT( 3 ),		// math light; mathLight lanes carry the encode
	RLF_DEFAULT_TEXTURES	= BIT( 4 ),		// light has the default diffuse/specular textures ( D39 )
	RLF_CLUSTERED			= BIT( 5 ),		// the light may be listed ( D40 )
	RLF_AMBIENT				= BIT( 6 ),		// inject-only record ( D26, D40 )
	RLF_FOGLIGHT			= BIT( 7 ),		// inject-only fog record, read the FogPass way ( D26, D40, D81 )
	RLF_NOSPECULAR			= BIT( 8 ),		// no specular contribution
	RLF_MOVED_THIS_FRAME	= BIT( 9 )		// the light moved this frame
};

const int MAX_RECORD_LIGHTS = 1024;

struct lightListView_t;			// defined at the end of this file

/*
====================
recordLight_t

The GPU light record ( C1 ), 192 bytes, the relative frame throughout: everything
minus viewDef_t::renderOrigin. HLSL mirror shaders/lightlists.inc.hlsl.
====================
*/
struct alignas( 16 ) recordLight_t
{
	idVec4		originInvRadiusSq;	// xyz = relative shading origin ( globalLightOrigin, already relative, rev 4.2 ), w = 1 / r^2 of the sphere centred there that encloses the light's bounds ( the binning sphere ); RLF_PARALLEL records: w = 0, no sphere ( rev 4.2, see below )
	idVec4		color;				// rgb = evaluated stage colour x r_lightScale x cullFade, applied ONCE ( linear; D58 as amended, rev 4.2 ), w = spot scale ( 0 = omni ). RLF_FOGLIGHT records ( D81 ): rgb = the RAW stage colour, no light scale, w = the fog distance D ( colour alpha, 500 when alpha <= 1 ), as FogPass reads them
	idVec4		spotDir;			// xyz = cone axis ( a direction: the same in the map frame and the relative frame ), w = spot bias ( 1 = omni )
	int			shadowRow;			// first row of this light's shadow rows; -1 = unshadowed
	uint32		channelMask;		// bit 0 = default; spectrum lights set bit ( 1 + spectrum )
	uint32		flags;				// RLF_SUN, RLF_POINT, RLF_PARALLEL, RLF_MATH, RLF_DEFAULT_TEXTURES ( D39 ), RLF_CLUSTERED ( may be listed, D40 ), RLF_AMBIENT and RLF_FOGLIGHT ( inject only, D26, D40 ), RLF_NOSPECULAR, RLF_MOVED_THIS_FRAME
	int			uniqueId;			// idRenderLightLocal::uniqueId ( world-monotonic, D44 ), copied by the frontend; the volumetrics history key
	idVec4		lightProject[4];	// relative planes S, T, Q, falloff: the viewLight_t planes, unchanged ( already relative ) ( hasMatrix lights have no record, D57 )
	idVec4		mathLight[3];		// rpUser0..2 exactly as R_MathLightToParms() encodes mathLightData_t ( the one encode, factored out of RenderInteractions by X0, S7 ); zero when !RLF_MATH
	idVec4		ids;				// x = number ( importance rank ), y = stage, z = record index in the fog-eligible view's previous frame ( -1; keyed by the frame stamp, D56 ), w = scattering albedo for RLF_FOGLIGHT records ( default 0, D81 ), else 0
};
compile_time_assert( sizeof( recordLight_t ) == 192 );
assert_offsetof( recordLight_t, originInvRadiusSq, 0 );	// every HLSL-mirrored field, not only the size ( S82(i); idlib/sys/sys_assert.h:117 )
assert_offsetof( recordLight_t, color, 16 );
assert_offsetof( recordLight_t, spotDir, 32 );
assert_offsetof( recordLight_t, shadowRow, 48 );
assert_offsetof( recordLight_t, channelMask, 52 );
assert_offsetof( recordLight_t, flags, 56 );
assert_offsetof( recordLight_t, uniqueId, 60 );
assert_offsetof( recordLight_t, lightProject, 64 );
assert_offsetof( recordLight_t, mathLight, 128 );
assert_offsetof( recordLight_t, ids, 176 );

/*
====================
shadowRow_t

The shadow rows ( C2 ), 80 bytes, one per spot map, per point face ( 6 ), per
cascade; filled by the backend in R_BuildShadowRows after ShadowAtlasPass.
====================
*/
struct alignas( 16 ) shadowRow_t
{
	idVec4		worldToShadow[4];	// parallel and point units: renderMatrix_clipSpaceToWindowSpace x shadowP[s] x shadowV[s]; spot units: shadowP[0] x shadowV[0] ( its P already carries the window-space and clip-to-UV fix-ups, F6 ). This is the legacy per-draw product of RenderBackend.cpp:2087-2093 ( parallel ), :2103-2109 ( point ), :2119-2124 ( spot ) WITHOUT the model matrix the legacy path folds in at :2087/:2103/:2119: interactionSM's texcoord7 is model space, the forward VS writes the world position ( N9, F40 ). ( rev 4.2: 'world' here is the eye-relative frame; the rows are built from vLight->shadowV and shadowP, which already take relative coordinates )
	idVec4		atlasRect;			// xy = imageAtlasOffset[s] / atlasSize, z = imageSize / atlasSize ( < 0: not placed - the forward loop treats the light as unshadowed, the inject SKIPS it, D80 ), w = cascade far distance ( rpCascadeDistances[s] ) for RLF_SUN rows, shadowFadeOut for every other row ( D47 )
};
compile_time_assert( sizeof( shadowRow_t ) == 80 );
assert_offsetof( shadowRow_t, worldToShadow, 0 );		// S82(i)
assert_offsetof( shadowRow_t, atlasRect, 64 );

/*
==========================================================================================

LIGHT LIST CORE ( F0, the merge )

The per-view light-list builder of the hybrid forward pass, as pure functions over flat
records. This part of the header is the one place the outside world is visible: it needs idVec3,
the idLib integer types and nothing of the renderer, so the same text compiles in the engine
( here; renderer/tr_frontend_lightlists.cpp holds the bodies ) and in an offline test behind a
shim of idLib. The engine glue turns viewLight_t into
lightListLight_t, cvars into lightListParms_t, and the results back into
viewLight_t::recordNumber, numListedSurfs and the per-draw constants; nothing here reads a cvar,
a global or the clock.

What the design rests on:

  - ONE numbering. The record index is decided before the model jobs run ( first hook, after
    R_AddLights ), so the jobs list record indices and nothing translates them later. Record r
    is recordLights[ r ], bit r of a tile word, bit r of a surface's mask and entry r of a run.
  - Both paths shade exactly the pairs the job listed. A surface's run is used two ways: on the
    list path every pixel loops it; on the Z-bin path its bits are ANDed into every tile word.
    So every exclusion the job applied ( a closed door, a static interaction with no triangles,
    spectrum, noDynamicInteractions ) holds on both paths with no test in the loop, and the Z-bin
    path never costs more than the run.
  - Three classes of records, each contiguous and each sorted by its far bin: narrow clustered
    [ 1, firstWide ), wide clustered ( view-depth extent >= wideDepth ) [ firstWide,
    firstFogOnly ), fog-only [ firstFogOnly, numRecords ). A bin holds one range per class, so a
    few very large lights never stretch the index range of every nearer bin, and the forward
    loop never walks the fog's records.
  - Both loops walk records in ascending index: a run is written from its mask, so it is an
    ascending set by construction. A surface whose path flips shades the same lights in the same
    order: the same bits.
  - Integer sort keys ( class, far bin, importance rank ), sorted by radix: one total order.
  - No atomics in the jobs. A job writes its own surfaces' runs and paths; the serial pack
    assigns pool offsets in draw order. The pool, and its hash, are the same every run.

Record 0 is the primary sun's ( C1 ): never binned, no tile bit. A surface the sun's interaction
reaches lists 0 like any other record: entry 0 of its run, bit 0 of its mask.

A broken invariant is fatal ( LL_FATAL: a run that is not an ascending set of listable records, an
arena overrun ). A full table is not: it has a counted outcome that never drops a light ( the
record cap: the legacy path; the pool: the mask, then the shared all-ones mask ).

==========================================================================================
*/

#ifndef LL_FATAL
	#define LL_FATAL( msg )		idLib::FatalError( "%s", msg )
#endif

const int		LL_MAX_RECORDS		= 1024;				// MAX_RECORD_LIGHTS: record 0 ( the sun ) included
const int		LL_MAX_WORDS		= LL_MAX_RECORDS / 32;	// words per tile and per surface mask at the record cap
const int		LL_MAX_BINS			= 8192;				// r_zbinCount's ceiling
const int		LL_BIN_STRIDE		= 4;				// uint32 per bin: narrow, wide, fog-only range, 0 ( a uint4 on the GPU, 16 bytes )
const int		LL_MAX_TILES		= 512;				// per axis: 8192 pixels at 16-pixel tiles
const int		LL_MAX_POOL			= 1 << 16;			// pool words per view: 256 KiB of uint32
const int		LL_POOL_SUN			= 0;				// pool word 0 holds 0, the sun's record index: the list loop reads it on the Z-bin path
const int		LL_POOL_ALL_ONES	= 1;				// pool words [ 1, 33 ): the shared all-ones mask ( pool overflow ); bit 0 = the sun is listed
const int		LL_POOL_FIRST		= LL_POOL_ALL_ONES + LL_MAX_WORDS;	// 33: the first word a surface's run or mask may take
const int		LL_MAX_SAMPLES		= 64;				// 4 x 4 over a planar chunk, 4 x 4 x 4 over a box
const int		LL_RANK_BITS		= 11;				// importance rank in the sort key: LL_MAX_RECORDS <= 2048
const uint32	LL_RANGE_EMPTY		= 0x0000FFFFu;		// lo = 0xFFFF, hi = 0: lo > hi

// lightListSurface_t::path, rpSurfLights.z ( the shader uses the same values ); r_surfLightForcePath takes the first two
enum lightListPath_t
{
	LLP_LIST				= 0,		// every pixel loops the surface's own run
	LLP_ZBIN				= 1,		// every pixel loops its Z-bin candidates ANDed with the surface's mask
	LLP_ZBIN_ALL			= 2			// the pool was full: the Z-bin loop under the shared all-ones mask ( loud: counted, warned, magenta )
};

// the material's forward tag ( keyword "forward [ n ]" ): a designer routes a surface by hand. A value the switch
// does not know is counted and read as LLT_AUTO.
enum lightListTag_t
{
	LLT_AUTO				= 0,		// the rule decides
	LLT_LIST				= 1,		// always the run
	LLT_ZBIN				= 2,		// always the Z-bins
	LLT_NUM
};

// how a surface's path was decided, for the log line ( lightListSurface_t::how )
enum lightListHow_t
{
	LLH_FORCED				= 0,		// r_surfLightForcePath
	LLH_TAG					= 1,		// the material's tag
	LLH_SHORT				= 2,		// n <= F: no sample needed, the estimate cannot be negative
	LLH_SAMPLED				= 3,		// n against F + the sampled estimate
	LLH_UNSAMPLED			= 4,		// no sample on screen: n against unsampledMax
	LLH_NUM,
	LLH_UNKNOWN_TAG			= 0x80		// flag: the tag was not one the switch knows
};

// per-record features; a draw ORs those of its run into rpSurfLights.w
enum
{
	LLF_SHADOWED			= BIT( 0 ),	// may have a shadow row: the per-pixel Vogel set-up is needed
	LLF_MATH				= BIT( 1 ),	// RLF_MATH
	LLF_PROJECTED			= BIT( 2 ),	// spot / projected default texture
	LLF_POINT				= BIT( 3 ),	// RLF_POINT
	LLF_ALL					= 0xF
};

// a frame's knobs, read from the cvars ONCE by the glue; jobs read this, never a cvar
struct lightListParms_t
{
	int			width;					// render size in pixels
	int			height;
	float		tanLeft;				// the projection's side planes as view x / z and y / z ( the rule's samples )
	float		tanRight;
	float		tanBottom;
	float		tanTop;
	float		zNear;
	int			tileSize;				// r_zbinTileSize ( 16 )
	int			numBins;				// r_zbinCount ( 8192 )
	float		binFar;					// r_zbinFar ( 16384 ): beyond it, the last bin
	float		wideDepth;				// r_zbinWideDepth ( 2048 ): clustered lights whose view-depth extent is at least this take the wide range
	int			forcePath;				// r_surfLightForcePath: -1 = tags, then the rule; else LLP_LIST or LLP_ZBIN
	float		fetchCost;				// r_surfLightFetchCost ( F, 1 )
	int			unsampledMax;			// r_surfLightUnsampledMax ( 8 )
	int			poolCap;				// <= LL_MAX_POOL
};

// one per view light that gets a record ( C1: not blend, not hasMatrix ), in any order
struct lightListLight_t
{
	float		zMin;					// linear view distance of the light volume: zNear / zmax .. zNear / zmin ( C1, reversed-Z )
	float		zMax;
	int16		x0, y0, x1, y1;			// inclusive pixel rectangle, top-left origin, inside the view: the light's scissor, padded by the caller; x0 > x1 = none
	int			importance;				// scissor area in pixels: larger survives the cap
	uint16		features;				// LLF_*
	byte		clustered;				// RLF_CLUSTERED: may be listed
	byte		primarySun;				// viewLight_t::isPrimarySun ( D73 ): takes record 0
};

struct lightListRect_t					// a record's inclusive tile rectangle; x0 > x1 = none
{
	int16		x0, x1, y0, y1;
};

// the view's tables: built in the first hook, read only by the jobs and the pack
struct lightListTables_t
{
	lightListParms_t	parms;
	int					tilesX;
	int					tilesY;
	int					wordsPerTile;	// ( numRecords + 31 ) / 32: bit i is record i; also the words of a surface's mask
	float				binScale;		// bins per unit of view distance
	int					numRecords;		// record 0 included
	int					firstWide;		// records [ 1, firstWide ) are narrow clustered, [ firstWide, firstFogOnly ) wide
	int					firstFogOnly;	// records [ 1, firstFogOnly ) are clustered, the rest only for the fog
	int					sunListed;		// 1: record 0 holds a clustered primary sun, so a run may hold 0
	int					numCapped;		// lights past LL_MAX_RECORDS: legacy path, counted
	int					numLights;		// the input count
	int*				recordOfLight;	// [ numLights ]: the record, -1 = none ( capped, or wholly behind the eye )
	int*				lightOfRecord;	// [ numRecords ]: the input light, -1 = the empty sun slot
	uint16*				features;		// [ numRecords ]
	uint16*				importanceRank;	// [ numRecords ]: 0 = the largest scissor among the survivors; C1's ids.x, for reading only
	lightListRect_t*	rect;			// [ numRecords ]
	uint32*				bins;			// [ numBins * 4 ]: narrow, wide, fog-only range, 0; each lo | hi << 16
};

// a surface's extent in view space ( +z forward ): a planar chunk ( 2 edges ) or a box ( 3 edges )
struct lightListShape_t
{
	idVec3		origin;
	idVec3		edges[3];
	int			numEdges;
};

// one draw surface ( drawSurf_t's TRACK F block ): the job fills run, count, path and how; the pack first and featureMask
struct lightListSurface_t
{
	uint16*			run;				// ascending record indices, frame memory owned by the entity's job; NULL = nothing listed
	int16			count;
	byte			path;				// lightListPath_t
	byte			how;				// lightListHow_t, LLH_UNKNOWN_TAG ORed in
	uint32			first;				// the pool word the run ( list ) or the mask ( Z-bin ) starts at; LL_POOL_ALL_ONES on overflow
	uint32			featureMask;		// LLF_* of the run ( LLF_ALL under the all-ones mask )
};

struct lightListStats_t					// one log line per view ( r_lightListLog )
{
	int			records;
	int			narrow;
	int			wide;
	int			fogOnly;
	int			capped;
	int			listSurfs;
	int			zbinSurfs;
	int			zbinAllSurfs;			// pool overflow, second step: unmasked ( loud )
	int			listedPairs;
	int			poolUsed;
	int			poolOverflow;			// pool overflow, first step: a run that did not fit became a mask
	int			how[LLH_NUM];
	int			unknownTags;
	uint32		hash;					// FNV-1a over the order, bins, tile bits, pool and paths
};

struct lightListArena_t					// one block per view from R_FrameAlloc, carved by a bump
{
	byte*		base;
	int			used;
	int			size;
};

void*			R_LightListAlloc( lightListArena_t& arena, int bytes );
int				R_LightListArenaBytes( const lightListParms_t& parms, int numLights );

// first hook ( R_NumberRecordLights ), serial
void			R_LightListSetup( const lightListParms_t& parms, int numLights, lightListTables_t& t, lightListArena_t& arena );
void			R_LightListNumber( const lightListLight_t* lights, int numLights, lightListTables_t& t, lightListArena_t& arena );
// first hook, one job per tile row band ( D79 ); tileBits zeroed by the caller, each call writes only rows [ row0, row1 )
void			R_LightListTileRows( const lightListTables_t& t, int row0, int row1, uint32* tileBits );

// the model jobs ( H3 ). The job appends record indices to its run in any order, then:
int				R_LightListFinishRun( const lightListTables_t& t, uint16* run, int count, uint32* mask );
int				R_LightListChoosePath( const lightListTables_t& t, const uint32* tileBits, const lightListShape_t& shape,
									   const uint16* run, int count, const uint32* mask, int tag, int* how );

// second hook ( R_BuildLightLists ), serial: pool offsets in draw order, listed counts per record
void			R_LightListPack( const lightListTables_t& t, lightListSurface_t** surfs, int numSurfs, uint32* pool,
								 int* listedOfRecord, lightListStats_t& stats );
uint32			R_LightListHash( const lightListTables_t& t, const uint32* tileBits, const uint32* pool, int poolUsed,
								 lightListSurface_t* const* surfs, int numSurfs );

// shared with the shader: must match lightloop.hlsli exactly, clamps included
ID_INLINE int R_LightListBinIndex( const lightListTables_t& t, float viewZ )
{
	return ( int )idMath::ClampFloat( 0.0f, ( float )t.parms.numBins - 1.0f, viewZ * t.binScale );
}

ID_INLINE uint32 R_LightListWordMask( int w, int lo, int hi )
{
	const int first = Max( lo - w * 32, 0 );
	const int last = Min( hi - w * 32, 31 );
	return ( first > last ) ? 0u : ( 0xFFFFFFFFu >> ( 31 - ( last - first ) ) ) << first;
}

// what the Z-bin loop evaluates at a pixel and view depth under a surface's mask ( NULL: no mask ), ascending, the sun
// ( bit 0 of the mask ) first. Returns the count; writes at most maxOut indices when out is not NULL.
int				R_LightListCandidates( const lightListTables_t& t, const uint32* tileBits, const uint32* mask, int px, int py,
									   float viewZ, int* out, int maxOut );

compile_time_assert( LL_MAX_RECORDS == MAX_RECORD_LIGHTS );

/*
==========================================================================================

THE ENGINE SIDE ( F1 )

What the frontend keeps per view around the core's tables, and the calls the rest of the
renderer makes. Bodies in tr_frontend_lightlists.cpp.

==========================================================================================
*/

struct viewLight_t;

// renderLight_t::forwardMode ( the spawnarg "forward" ): a designer routes a light by hand
enum lightForwardMode_t
{
	LIGHT_FORWARD_AUTO		= 0,			// the kind rule and r_forwardLightKinds decide
	LIGHT_FORWARD_LIST		= 1,			// listed whenever the loop can express the light
	LIGHT_FORWARD_LEGACY	= 2				// never listed
};

/*
====================
lightListView_t

viewDef_t::lightListView: frame memory, one per view that has records. The frontend writes it
in the two hooks; the backend only reads it ( the uploads, the debug view ).
====================
*/
struct lightListView_t
{
	lightListTables_t		tables;
	lightListLight_t*		lights;				// the builder's input, kept for the capture
	int						numLights;
	viewLight_t**			lightOfInput;		// input index -> viewLight_t ( frontend only: the backend must not follow it to the light def )
	uint32*					flagsOfInput;		// input index -> RLF_* as R_LightListRecordFlags decided
	uint32*					tileBits;			// tilesX * tilesY * wordsPerTile words: the tile-bit upload
	uint32*					pool;				// stats.poolUsed words: the pool upload
	byte*					rowsOfRecord;		// [ numRecords ]: the shadow rows a record owns, from recordLight_t::shadowRow on; 0 = none
	int*					vanishedPrev;		// records of the previous fog-eligible view whose lights have none now ( D82 ); NULL in any other view
	int						numVanishedPrev;
	int						prevFrame;			// tr.frameCount of that previous view ( ids.z and vanishedPrev refer to it ); 0 = there was none
	lightListStats_t		stats;
	int						unknownLightModes;	// forwardMode values the switch does not know ( read as 0 )
	int						showMode;			// r_showLightLists, read once per view: rpZBinScale.w
	bool					lists;				// surfaces may be listed in this view; false: records and bins only
};

void	R_NumberRecordLights();		// frontend, called from R_RenderView after R_AddLights
void	R_BuildLightLists();		// frontend, after R_AddModels and before R_OptimizeViewLightsList
void	R_LightListViewParms( const lightListView_t* llv, float zbinParms[4], float zbinScale[4] );	// the view's two lanes ( C18 ), for the backend
void	R_LightListInitCommands();	// called once from R_InitCommands

#endif // __LIGHTLISTS_H__
