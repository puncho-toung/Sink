//puncho - SB1a (R-SB1a-29): dmap's acoustic Stage A, the mesh it hands to WP-SB1b
#ifndef __ACOUSTIC_H__
#define __ACOUSTIC_H__

#include "dmap.h"
#include "acoustic_shared.h"

/*
===============================================================================

	Stage A runs inside dmap after the .proc is written, while the trees, hulls, primitives and interAreaPortals
	are alive. It builds an acousticMesh_t, hands it to AcousticFinishStageA (WP-SB1b: probes, then .sndmesh) and
	writes the manifest. The mesh lives only for that call. x64 sizes, idStr 40 B.

===============================================================================
*/

struct acousticBrush_t
{
	int32					entity;
	int32					firstPlane;
	int32					numPlanes;
};																		// 12 B

//puncho - SB1b-P (review P4-1, R-SB1b-10 rule 6): one triangle of a static model as Stage A had it before the
// open-leaf cull: world space, the winding acousticTri_t gets (front on ( v[2] - v[0] ) x ( v[1] - v[0] )). In memory only
struct acousticStaticTri_t
{
	idVec3					v[3];
	int32					flags;		// bit1 two-sided, as acousticTri_t; bit2 ACOUSTIC_STATIC_TRI_CULLED
};																		// 40 B

// the cull removed this triangle from mesh.tris (it touches no open leaf). Only staticTris carries it; no file reads
// this list, so no writer masks it
const int32				ACOUSTIC_STATIC_TRI_CULLED = 4;

// one entity in the mesh whose triangles can close a solid rule 5 cannot see: a model static (its model or collision
// surfaces) or the patches and polygon meshes of worldspawn or a `static brushes` entity.
// staticTris[firstTri, firstTri + numTris), emit order
struct acousticStatic_t
{
	int32					entity;
	int32					firstTri;
	int32					numTris;
};																		// 12 B

struct acousticMaterial_t
{
	idStr					name;
	acousticNumbers_t		n;
	int32					source;		// acousticSource_t
	int32					preset;		// acousticPreset_t
};																		// 80 B

struct acousticTri_t
{
	int32					v[3];
	uint16					material;
	uint16					flags;		// bit0 plug, bit1 two-sided
};																		// 16 B

struct acousticPortal_t
{
	int32					area0, area1;
	int32					door;		// first screened door, or -1
	idPlane					plane;
};																		// 28 B

struct acousticDoor_t
{
	idStr					name;
	int32					entity, portal, area0, area1,
							firstPlugTri, numPlugTris, material;
};																		// 72 B

struct acousticEntity_t
{
	idStr					name;
	int32					entity;
	uint32					kind;		// 0 not a candidate, 1 a candidate left out, 2 fed the mesh or got a door record
	uint32					crc;
};																		// 56 B

struct acousticMeshSettings_t
{
	int32					sealDoorways;
	int32					staticTriBudget;
};																		// 8 B

struct acousticMesh_t
{
	acousticMeshSettings_t		settings;
	idList<idVec3>				verts;			// engine units, world space
	idList<acousticTri_t>		tris;			// [0, numWorldTris) world, then plugs grouped by door
	int32						numWorldTris;
	idList<acousticMaterial_t>	materials;		// sorted by name, at most 65,535
	idList<acousticPortal_t>	portals;		// one per interAreaPortals entry, same order
	idList<acousticDoor_t>		doors;			// screened doors, map order
	idList<acousticEntity_t>	entities;		// one per map entity, map order
	idList<idPlane>				brushPlanes;	// world space, R-SB1a-38
	idList<acousticBrush_t>		brushes;		// one per brush of a `static brushes` entity
	idList<acousticStaticTri_t>	staticTris;		// SB1b-P rule 6: every triangle of statics' records, before the cull
	idList<acousticStatic_t>	statics;		// one per such static, map order; never written to a file
	uint32 entityKeysCRC, materialHash;			// header fields (K-SB4)
	uint32 scriptCRC;							// separate value, never folded in
	int32						numAreas;		// uEntities[0].numAreas
	const tree_t*				worldTree;		// pruned world tree, alive until FreeDMapFile
	const idMapFile*			mapFile;		// dmapGlobals.dmapFile
};

static_assert( sizeof( acousticNumbers_t ) == 28, "acousticNumbers_t is 28 B" );
static_assert( sizeof( acousticBrush_t ) == 12, "acousticBrush_t is 12 B" );
static_assert( sizeof( acousticTri_t ) == 16, "acousticTri_t is 16 B" );
static_assert( sizeof( acousticStaticTri_t ) == 40, "acousticStaticTri_t is 40 B" );
static_assert( sizeof( acousticStatic_t ) == 12, "acousticStatic_t is 12 B" );

//puncho - SB1b-P (review P4R-6): a list about to grow doubles its capacity, at least to minimum, so growth copies
// O(N) elements, not N^2 / granularity (review M2-3). Stage A's mesh and the probes share it
template< class type_t >
ID_INLINE void AcousticReserveOne( idList<type_t>& list, int minimum )
{
	if( list.Num() >= list.NumAllocated() )
	{
		list.Resize( Max( 2 * list.NumAllocated(), minimum ) );
	}
}
static_assert( sizeof( acousticPortal_t ) == 28, "acousticPortal_t is 28 B" );
static_assert( sizeof( acousticMeshSettings_t ) == 8, "acousticMeshSettings_t is 8 B" );

// acousticMaterial_t::source: 0 to 15 is the surface type (surfTypes_t) whose row gave the numbers
enum acousticSource_t
{
	ACOUSTIC_SOURCE_SKY			= 16,
	ACOUSTIC_SOURCE_LIQUID		= 17,
	ACOUSTIC_SOURCE_GLASS		= 18,
	ACOUSTIC_SOURCE_SYNTHETIC	= 19
};

// acousticMaterial_t::preset: one per row of R-SB1a-21's surface-type table, in table order
enum acousticPreset_t
{
	ACOUSTIC_PRESET_GENERIC,
	ACOUSTIC_PRESET_METAL,
	ACOUSTIC_PRESET_CONCRETE,
	ACOUSTIC_PRESET_FLESH,
	ACOUSTIC_PRESET_WOOD,
	ACOUSTIC_PRESET_CARDBOARD,
	ACOUSTIC_PRESET_LIQUID,
	ACOUSTIC_PRESET_GLASS,
	ACOUSTIC_PRESET_PLASTIC,
	ACOUSTIC_PRESET_SKY,
	ACOUSTIC_PRESET_COUNT
};

// SB1a-M, acoustic_mesh.cpp: builds the mesh, calls AcousticFinishStageA, writes the manifest; false fails Stage A
bool			AcousticStageA();
// WP-SB1b: probes and .sndmesh from the mesh; it copies what it keeps; false fails Stage A
bool			AcousticFinishStageA( const acousticMesh_t& mesh );
// WP-SB2 (K-SB9), acoustic_bake.cpp, engine only: Stage B; false fails dmap
#if !defined( DMAP )
bool			BakeAcoustics( const char* mapName, bool fast );
#endif

// dmap.cpp: adds Sys_Milliseconds() - startMs to a DP_ACOUSTIC_* slot under dmap_phaseTimes
void			AcousticPhaseEnd( int slot, int startMs );

extern idCVar	dmap_acousticVerbose;
extern idCVar	dmap_acousticDump;
extern bool		dmapAcousticFast;		// dmap.cpp: the acousticFast word of this run

// dmap.cpp: the worldspawn keys acoustic_sealDoorways and acoustic_staticTriBudget; returns the warnings it printed
int				AcousticReadMeshSettings( const idDict& worldspawn, acousticMeshSettings_t& settings );

// R-SB1a-21, M1 to M10, first match wins; false: the material is out. source and preset are set only when true
bool			AcousticMaterialPasses( const idMaterial* material, int& source, int& preset );
void			AcousticPresetNumbers( int preset, acousticNumbers_t& out );
// the Material.h enum name less SURFTYPE_, upper case: NONE, METAL ... RICOCHET, 10 ... 15 (the manifest's type:<t>)
const char*		AcousticSurfaceTypeName( int surfaceType );

// R-SB1a-32: over every record's kind then crc, each a uint32 low byte first, in map order
uint32			AcousticEntityKeysCRC( const idList<acousticEntity_t>& entities );
// R-SB1a-32: over each material's name, a 0, then its seven floats low byte first
uint32			AcousticMaterialHash( const idList<acousticMaterial_t>& materials );

//puncho - WP-SB1b region begin
// SB1b-H begin
//puncho - SB1b (R-SB1b-16): the format header of .sndmesh and .sndbake
#include "../../../sound/snd_acousticfile.h"

//puncho - SB1b (R-SB1b-21): removes <base>.sndmesh, .sndmanifest, .sndbake, .sndmesh.tmp and .sndbake.tmp;
// mapFileBase is dmapGlobals.mapFileBase, "maps/<map>"
void			AcousticDeleteOutputs( const char* mapFileBase );

//puncho - SB1b (R-SB1b-26): output.cpp's writer of one ( x floats ) group of the .proc, no header holds it
void			Write1DMatrix( idFile* f, int x, float* m );

//puncho - SB1b (R-SB1b-22): the file builder, acoustic_file.cpp. One buffer, header and directory first; every
// section starts at a 16-byte offset and is zeroed. A later add may move data and header: callers keep offsets,
// not pointers, across adds. The caller fills every header field through b.header except magic, version,
// numSections, payloadSize, payloadCRC (the builder's) and step1Bytes (SndFile_EndStep1).
struct sndFileBuilder_t
{
	byte*				data;
	int					size, capacity;
	int					numSections, nextSection;
	sndFileHeader_t*	header;
};
void	SndFile_Begin( sndFileBuilder_t& b, uint32 magic, int numSections );					// header + directory, zeroed
void*	SndFile_AddSection( sndFileBuilder_t& b, uint32 id, uint32 recordSize, uint32 count );	// zeroed space
void*	SndFile_AddBlob( sndFileBuilder_t& b, uint32 id, uint32 bytes );						// count 0: the byte sections
void	SndFile_EndStep1( sndFileBuilder_t& b );												// step1Bytes = current end
bool	SndFile_Finish( sndFileBuilder_t& b, const char* relativePath );
void	SndFile_Free( sndFileBuilder_t& b );

//puncho - SB1b (R-SB1b-23): H64, the first 8 bytes of the MD5 read little-endian (DR-20)
uint64	SndHash64( const void* data, int size );
void	SndHash64Begin( MD5_CTX& ctx );
void	SndHash64Update( MD5_CTX& ctx, const void* data, int size );
uint64	SndHash64End( MD5_CTX& ctx );

//puncho - SB1b-P (R-SB2-6, K-SB8): the one resolver of the Stage B words, for the .sndmesh header and the bake. Writes
// preset, rays, bounces, duration, ambisonicOrder and no other field: the preset (final 32,768 rays, fast 2,048;
// ceiling 256; 1.0 s; order 1), then each non-zero raw value overrides (a duration that is not finite is absent),
// then rays clamped to [32, 1048576] and rounded up to a multiple of 32, the ceiling to [1, 256], the duration to
// [0.1, 4.0] s
void	SndResolveStageB( sndSettings_t& settings, bool fast, int32 rawRays, int32 rawBounces, float rawDuration );

//puncho - SB1b-P: shared readers of a file in memory. SndFile_PayloadCRC: CRC32 over the whole buffer with the four
// bytes of payloadCRC taken as 0; length >= SND_HEADER_SIZE. SndFile_FindSection: the directory entry with that id
// among the header's numSections, NULL if none; the caller has checked that the directory lies inside the buffer
uint32				SndFile_PayloadCRC( const byte* data, int length );
const sndSection_t*	SndFile_FindSection( const byte* data, uint32 id );

//puncho - SB1b-P (review I4-3): the .sndmesh inputHash (R-SB1b-23). data holds the header and directory, and every
// one of the twelve sections inside the buffer at its directory entry's offset and size (checked by the caller);
// found through the directory; 0 when a hashed section is missing. acoustic_file.cpp
uint64				SndFile_MeshInputHash( const byte* data );

//puncho - SB1b-P: BAK_DEVCHECK, one builder for the restamp and the bake (R-SB2-24), fed the .sndmesh's MSH_ENTITIES,
// MSH_MATERIALS and MSH_STRINGS bytes and MSH_SETTINGS.scriptCRC; out holds SndFile_DevCheckSize bytes, all written
int					SndFile_DevCheckSize( int numEntities, int numMaterials, int stringsSize );
void				SndFile_BuildDevCheck( byte* out, const sndDevEntity_t* entities, int numEntities, const sndMaterial_t* materials, int numMaterials,
										   const char* strings, int stringsSize, uint32 scriptCRC );

//puncho - SB1b-P (review I4-9): R-SB1b-27 step 1 on an old .sndbake read whole, NULL when it is readable, else the
// field or section that is not; the header, the whole-file CRC, the directory, the step 1 sections and BAK_DEVCHECK
// in their places, step1Bytes, and the game loader's SndBakeStep1. acoustic_write.cpp
const char*			AcousticBakeUnreadable( const byte* data, int length );

//puncho - SB1b-P (review F4-3): the largest acoustic file, by the format's uint32 offsets and the builder's int sizes
const int64			SND_FILE_MAX_SIZE = 0x7fff0000;
// SB1b-H end
// SB1b-S begin
//puncho - SB1b (R-SB1b-30): the structure id of this compile, acoustic_structure.cpp. Call after WriteOutputFile
// (its WriteOutputNodes pruned and numbered the world tree) and before FreeDMapFile, with mesh.numAreas > 0;
// equals idRenderWorld::GetMapCompileId() on the game's load of the .proc this compile wrote
uint64			AcousticStructureId( const acousticMesh_t& mesh );
// SB1b-S end
//puncho - WP-SB1b region end

#endif /* !__ACOUSTIC_H__ */
