//puncho - SB1b (R-SB1b-16, R-SB1b-17, R-SB1b-18): the one format header of .sndmesh and .sndbake
#ifndef __SND_ACOUSTICFILE_H__
#define __SND_ACOUSTICFILE_H__

#include "../idlib/hashing/CRC32.h"

/*
===============================================================================

	Baked room acoustics: the file layout the map compiler writes (dmap Stage A writes .sndmesh, the bake writes
	.sndbake) and the game reads. Included by the engine (game, sound, dmap) and by the standalone compiler, so
	it uses idLib types only.

	A file is the 112-byte header, a directory of numSections sndSection_t in ascending id, then the sections in
	the same order. Every section starts at a 16-byte file offset and the file ends at one; every gap is zero.
	A section of records has size == count * SND_SIZE_*; a section of bytes (MSH_STRINGS, BAK_STRINGS,
	BAK_DEVCHECK) has count 0 and its size in bytes. Every word is little-endian; there is no pointer and no
	padding inside a record, so a record is written and hashed as its bytes.

===============================================================================
*/

// the layout is written and read as raw bytes
#if defined( __BYTE_ORDER__ )
	static_assert( __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the acoustic files are little-endian" );
#elif defined( _MSC_VER ) && ( defined( _M_X64 ) || defined( _M_IX86 ) || defined( _M_ARM64 ) )
	// MSVC defines no byte-order macro; x64, x86 and ARM64 Windows are all little-endian
#else
	#error "the acoustic files are little-endian: check this target's byte order"
#endif

#define SND_HEADER_SIZE			112
#define SND_SECTION_SIZE		16
#define SND_MAX_SECTIONS		32			// the loader refuses more (S1C-16)
#define SND_MAGIC_MESH			0x4D444E53	// "SNDM"
#define SND_MAGIC_BAKE			0x42444E53	// "SNDB"
#define SND_FORMAT_VERSION		1
#define SND_NUM_BANDS			3			// 400 Hz, 2.5 kHz, 15 kHz

// R-SB1b-17 (DR-17): A is Stage A's, raised by SB1a (rules, tables) or SB1b (probes, writer)
#define SND_BAKER_REVISION_A	1
// Stage B's revision: this one line is SB2's and SB0's from now on (SB2-I changes it; nothing else does)
#define SND_BAKER_REVISION_B	1
// .sndmesh.bakerRevision = A; .sndbake.bakerRevision = this
#define SND_BAKER_REVISION_BAKE	( ( SND_BAKER_REVISION_B << 16 ) | SND_BAKER_REVISION_A )
#define SND_TOOL_VERSION	0x00040801	// Steam Audio 4.8.1, as STEAMAUDIO_VERSION packs it; the bake asserts the two equal when it lands (SEAMS D-36)

// section ids; the directory lists them ascending, the sections lie in the file in the same order
enum sndSectionId_t
{
	MSH_SETTINGS	= 1,		// sndMeshSettings_t, one
	MSH_MATERIALS	= 2,		// sndMaterial_t
	MSH_VERTS		= 3,		// sndMeshVert_t
	MSH_TRIS		= 4,		// sndMeshTri_t
	MSH_TRIMAT		= 5,		// sndMeshTriMat_t
	MSH_AREAS		= 6,		// sndMeshArea_t
	MSH_CELLS		= 7,		// int32 probe index or -1
	MSH_PROBES		= 8,		// sndMeshProbe_t
	MSH_PORTALS		= 9,		// sndPortal_t, one per interAreaPortals entry (= the game's portal handle - 1)
	MSH_DOORS		= 10,		// sndMeshDoor_t
	MSH_ENTITIES	= 11,		// sndDevEntity_t
	MSH_STRINGS		= 12,		// bytes: offset 0 is ""; names in first reference order (materials, doors, entities)

	BAK_AREAS		= 0x101,	// sndBakeArea_t
	BAK_CELLS		= 0x102,	// int32 probe index or -1
	BAK_PROBES		= 0x103,	// sndBakeProbe_t
	BAK_ZONES		= 0x104,	// sndReverb_t, index = zone
	BAK_DOORS		= 0x105,	// sndBakeDoor_t
	BAK_STRINGS		= 0x106,	// bytes: offset 0 is ""; the door names in door order
	BAK_DEVCHECK	= 0x180,	// bytes: sndDevHead_t, entities, materials, a copy of MSH_STRINGS
	BAK_SA_BATCH	= 0x301		// reserved for step 3; not written in step 1 (DR-19)
};

// MSH_TRIMAT flags
#define SND_TRI_PLUG			0x0001		// bit 0: a door plug
#define SND_TRI_TWO_SIDED		0x0002		// bit 1: two-sided (DR-37)

struct sndSettings_t
{
	int32		probeSpacing;			// S, units
	int32		probeSpacingZ;			// Sz, units
	int32		probeClearance;			// C, units
	int32		sealDoorways;
	int32		staticTriBudget;
	int32		preset;					// 0 final, 1 fast
	int32		rays;
	int32		bounces;
	float		duration;				// s
	int32		ambisonicOrder;
	int32		diffuseSamples;
	float		irradianceMinDistance;	// m
};

struct sndFileHeader_t
{
	uint32			magic;				// SND_MAGIC_MESH or SND_MAGIC_BAKE
	uint32			version;			// SND_FORMAT_VERSION
	uint64			compileId;			// the structure id (R-SB1b-29)
	uint32			mapGeometryCRC;
	uint32			bakerRevision;
	uint32			toolVersion;
	uint32			numBands;
	uint32			entityKeysCRC;
	uint32			materialHash;
	uint64			inputHash;
	sndSettings_t	settings;
	uint32			step1Bytes;			// .sndbake: file offset of the end of BAK_STRINGS, rounded up to 16
	uint32			payloadSize;		// bytes after the header
	uint32			payloadCRC;			// CRC32 over the whole file with these four bytes taken as 0
	uint32			numSections;
};

struct sndSection_t
{
	uint32		id;
	uint32		offset;					// file offset, a multiple of 16
	uint32		size;					// bytes, no pad
	uint32		count;					// records; 0 for a section of bytes
};

struct sndMeshSettings_t
{
	int32		probeSpacing;			// S
	int32		probeSpacingZ;			// Sz
	int32		probeClearance;			// C
	int32		sealDoorways;
	int32		staticTriBudget;
	uint32		scriptCRC;				// outside the input hash
	int32		rawRays;				// worldspawn acoustic_rays, 0 absent
	int32		rawBounces;				// worldspawn acoustic_bounces, 0 absent
	float		rawDuration;			// worldspawn acoustic_duration, 0 absent
	uint32		reserved;
};

struct sndMaterial_t
{
	uint32		nameOffset;				// into the string table
	float		absorption[SND_NUM_BANDS];
	float		scattering;
	float		transmission[SND_NUM_BANDS];
};

struct sndMeshVert_t
{
	float		xyz[3];					// engine units, world space
};

struct sndMeshTri_t
{
	uint32		v[3];
};

struct sndMeshTriMat_t
{
	uint16		material;
	uint16		flags;					// SND_TRI_PLUG, SND_TRI_TWO_SIDED
};

struct sndMeshArea_t
{
	int32		cellMin[3];
	int32		dims[3];
	int32		firstCell;
	int32		firstProbe;
	int32		probeCount;
};

struct sndMeshProbe_t
{
	float		position[3];
	float		radius;
	int32		area;
};

struct sndPortal_t
{
	int32		area0;
	int32		area1;
	int32		door;					// first screened door, -1 none
};

struct sndMeshDoor_t
{
	uint32		nameOffset;
	int32		portal;
	int32		area0;
	int32		area1;
	int32		firstPlugTri;
	int32		plugTriCount;
	int32		material;
};

struct sndDevEntity_t
{
	uint32		nameOffset;
	uint32		kind;					// 0 not a candidate, 1 a candidate left out, 2 fed the mesh or a door (DR-15)
	uint32		crc;
};

struct sndReverb_t
{
	float		decayTime[SND_NUM_BANDS];	// s
	float		earlyDb;
	float		lateDb;
	float		preDelay;				// s
	float		lateDelay;				// s
	float		roomSize;				// m
};

struct sndBakeArea_t
{
	int32		zone;
	int32		cellMin[3];
	int32		dims[3];
	int32		firstCell;
	int32		firstProbe;
	int32		probeCount;
	sndReverb_t	defaultReverb;
};

struct sndBakeProbe_t
{
	float		position[3];
	float		radius;
	int32		area;
	uint32		flags;					// SB2's bits (R-SB2-19)
	sndReverb_t	reverb;
};

struct sndBakeDoor_t
{
	int32		portal;
	int32		area0;
	int32		area1;
	float		lossDb[SND_NUM_BANDS];
	uint32		nameOffset;
};

// the head of BAK_DEVCHECK; its four offsets are from the section start and 16-aligned
struct sndDevHead_t
{
	uint32		numEntities;
	uint32		numMaterials;
	uint32		scriptCRC;
	uint32		entitiesOffset;
	uint32		materialsOffset;
	uint32		stringsOffset;
	uint32		stringsSize;
	uint32		reserved;
};

// record sizes, the directory's size == count * SND_SIZE_*
#define SND_SIZE_SETTINGS		48
#define SND_SIZE_MESH_SETTINGS	40
#define SND_SIZE_MATERIAL		32
#define SND_SIZE_MESH_VERT		12
#define SND_SIZE_MESH_TRI		12
#define SND_SIZE_MESH_TRIMAT	4
#define SND_SIZE_MESH_AREA		36
#define SND_SIZE_CELL			4
#define SND_SIZE_MESH_PROBE		20
#define SND_SIZE_PORTAL			12
#define SND_SIZE_MESH_DOOR		28
#define SND_SIZE_DEV_ENTITY		12
#define SND_SIZE_REVERB			32
#define SND_SIZE_BAKE_AREA		72
#define SND_SIZE_BAKE_PROBE		56
#define SND_SIZE_BAKE_DOOR		28
#define SND_SIZE_DEV_HEAD		32

static_assert( sizeof( sndFileHeader_t ) == SND_HEADER_SIZE, "sndFileHeader_t is 112 B" );
static_assert( offsetof( sndFileHeader_t, magic ) == 0, "sndFileHeader_t layout" );
static_assert( offsetof( sndFileHeader_t, version ) == 4, "sndFileHeader_t layout" );
static_assert( offsetof( sndFileHeader_t, compileId ) == 8, "sndFileHeader_t layout" );
static_assert( offsetof( sndFileHeader_t, mapGeometryCRC ) == 16, "sndFileHeader_t layout" );
static_assert( offsetof( sndFileHeader_t, bakerRevision ) == 20, "sndFileHeader_t layout" );
static_assert( offsetof( sndFileHeader_t, toolVersion ) == 24, "sndFileHeader_t layout" );
static_assert( offsetof( sndFileHeader_t, numBands ) == 28, "sndFileHeader_t layout" );
static_assert( offsetof( sndFileHeader_t, entityKeysCRC ) == 32, "sndFileHeader_t layout" );
static_assert( offsetof( sndFileHeader_t, materialHash ) == 36, "sndFileHeader_t layout" );
static_assert( offsetof( sndFileHeader_t, inputHash ) == 40, "sndFileHeader_t layout" );
static_assert( offsetof( sndFileHeader_t, settings ) == 48, "sndFileHeader_t layout" );
static_assert( offsetof( sndFileHeader_t, step1Bytes ) == 96, "sndFileHeader_t layout" );
static_assert( offsetof( sndFileHeader_t, payloadSize ) == 100, "sndFileHeader_t layout" );
static_assert( offsetof( sndFileHeader_t, payloadCRC ) == 104, "sndFileHeader_t layout" );
static_assert( offsetof( sndFileHeader_t, numSections ) == 108, "sndFileHeader_t layout" );

static_assert( sizeof( sndSettings_t ) == SND_SIZE_SETTINGS, "sndSettings_t is 48 B" );
static_assert( offsetof( sndSettings_t, probeSpacing ) == 0, "sndSettings_t layout" );
static_assert( offsetof( sndSettings_t, probeSpacingZ ) == 4, "sndSettings_t layout" );
static_assert( offsetof( sndSettings_t, probeClearance ) == 8, "sndSettings_t layout" );
static_assert( offsetof( sndSettings_t, sealDoorways ) == 12, "sndSettings_t layout" );
static_assert( offsetof( sndSettings_t, staticTriBudget ) == 16, "sndSettings_t layout" );
static_assert( offsetof( sndSettings_t, preset ) == 20, "sndSettings_t layout" );
static_assert( offsetof( sndSettings_t, rays ) == 24, "sndSettings_t layout" );
static_assert( offsetof( sndSettings_t, bounces ) == 28, "sndSettings_t layout" );
static_assert( offsetof( sndSettings_t, duration ) == 32, "sndSettings_t layout" );
static_assert( offsetof( sndSettings_t, ambisonicOrder ) == 36, "sndSettings_t layout" );
static_assert( offsetof( sndSettings_t, diffuseSamples ) == 40, "sndSettings_t layout" );
static_assert( offsetof( sndSettings_t, irradianceMinDistance ) == 44, "sndSettings_t layout" );

static_assert( sizeof( sndSection_t ) == SND_SECTION_SIZE, "sndSection_t is 16 B" );
static_assert( offsetof( sndSection_t, id ) == 0, "sndSection_t layout" );
static_assert( offsetof( sndSection_t, offset ) == 4, "sndSection_t layout" );
static_assert( offsetof( sndSection_t, size ) == 8, "sndSection_t layout" );
static_assert( offsetof( sndSection_t, count ) == 12, "sndSection_t layout" );
static_assert( ( SND_HEADER_SIZE % 16 ) == 0 && ( SND_SECTION_SIZE % 16 ) == 0, "the first section starts 16-aligned" );

static_assert( sizeof( sndMeshSettings_t ) == SND_SIZE_MESH_SETTINGS, "sndMeshSettings_t is 40 B" );
static_assert( offsetof( sndMeshSettings_t, probeSpacing ) == 0, "sndMeshSettings_t layout" );
static_assert( offsetof( sndMeshSettings_t, probeSpacingZ ) == 4, "sndMeshSettings_t layout" );
static_assert( offsetof( sndMeshSettings_t, probeClearance ) == 8, "sndMeshSettings_t layout" );
static_assert( offsetof( sndMeshSettings_t, sealDoorways ) == 12, "sndMeshSettings_t layout" );
static_assert( offsetof( sndMeshSettings_t, staticTriBudget ) == 16, "sndMeshSettings_t layout" );
static_assert( offsetof( sndMeshSettings_t, scriptCRC ) == 20, "sndMeshSettings_t layout" );
static_assert( offsetof( sndMeshSettings_t, rawRays ) == 24, "sndMeshSettings_t layout" );
static_assert( offsetof( sndMeshSettings_t, rawBounces ) == 28, "sndMeshSettings_t layout" );
static_assert( offsetof( sndMeshSettings_t, rawDuration ) == 32, "sndMeshSettings_t layout" );
static_assert( offsetof( sndMeshSettings_t, reserved ) == 36, "sndMeshSettings_t layout" );

static_assert( sizeof( sndMaterial_t ) == SND_SIZE_MATERIAL, "sndMaterial_t is 32 B" );
static_assert( offsetof( sndMaterial_t, nameOffset ) == 0, "sndMaterial_t layout" );
static_assert( offsetof( sndMaterial_t, absorption ) == 4, "sndMaterial_t layout" );
static_assert( offsetof( sndMaterial_t, scattering ) == 16, "sndMaterial_t layout" );
static_assert( offsetof( sndMaterial_t, transmission ) == 20, "sndMaterial_t layout" );

static_assert( sizeof( sndMeshVert_t ) == SND_SIZE_MESH_VERT, "sndMeshVert_t is 12 B" );
static_assert( sizeof( sndMeshTri_t ) == SND_SIZE_MESH_TRI, "sndMeshTri_t is 12 B" );
static_assert( sizeof( sndMeshTriMat_t ) == SND_SIZE_MESH_TRIMAT, "sndMeshTriMat_t is 4 B" );
static_assert( offsetof( sndMeshTriMat_t, material ) == 0, "sndMeshTriMat_t layout" );
static_assert( offsetof( sndMeshTriMat_t, flags ) == 2, "sndMeshTriMat_t layout" );

static_assert( sizeof( sndMeshArea_t ) == SND_SIZE_MESH_AREA, "sndMeshArea_t is 36 B" );
static_assert( offsetof( sndMeshArea_t, cellMin ) == 0, "sndMeshArea_t layout" );
static_assert( offsetof( sndMeshArea_t, dims ) == 12, "sndMeshArea_t layout" );
static_assert( offsetof( sndMeshArea_t, firstCell ) == 24, "sndMeshArea_t layout" );
static_assert( offsetof( sndMeshArea_t, firstProbe ) == 28, "sndMeshArea_t layout" );
static_assert( offsetof( sndMeshArea_t, probeCount ) == 32, "sndMeshArea_t layout" );

static_assert( sizeof( int32 ) == SND_SIZE_CELL, "a cell is 4 B" );

static_assert( sizeof( sndMeshProbe_t ) == SND_SIZE_MESH_PROBE, "sndMeshProbe_t is 20 B" );
static_assert( offsetof( sndMeshProbe_t, position ) == 0, "sndMeshProbe_t layout" );
static_assert( offsetof( sndMeshProbe_t, radius ) == 12, "sndMeshProbe_t layout" );
static_assert( offsetof( sndMeshProbe_t, area ) == 16, "sndMeshProbe_t layout" );

static_assert( sizeof( sndPortal_t ) == SND_SIZE_PORTAL, "sndPortal_t is 12 B" );
static_assert( offsetof( sndPortal_t, area0 ) == 0, "sndPortal_t layout" );
static_assert( offsetof( sndPortal_t, area1 ) == 4, "sndPortal_t layout" );
static_assert( offsetof( sndPortal_t, door ) == 8, "sndPortal_t layout" );

static_assert( sizeof( sndMeshDoor_t ) == SND_SIZE_MESH_DOOR, "sndMeshDoor_t is 28 B" );
static_assert( offsetof( sndMeshDoor_t, nameOffset ) == 0, "sndMeshDoor_t layout" );
static_assert( offsetof( sndMeshDoor_t, portal ) == 4, "sndMeshDoor_t layout" );
static_assert( offsetof( sndMeshDoor_t, area0 ) == 8, "sndMeshDoor_t layout" );
static_assert( offsetof( sndMeshDoor_t, area1 ) == 12, "sndMeshDoor_t layout" );
static_assert( offsetof( sndMeshDoor_t, firstPlugTri ) == 16, "sndMeshDoor_t layout" );
static_assert( offsetof( sndMeshDoor_t, plugTriCount ) == 20, "sndMeshDoor_t layout" );
static_assert( offsetof( sndMeshDoor_t, material ) == 24, "sndMeshDoor_t layout" );

static_assert( sizeof( sndDevEntity_t ) == SND_SIZE_DEV_ENTITY, "sndDevEntity_t is 12 B" );
static_assert( offsetof( sndDevEntity_t, nameOffset ) == 0, "sndDevEntity_t layout" );
static_assert( offsetof( sndDevEntity_t, kind ) == 4, "sndDevEntity_t layout" );
static_assert( offsetof( sndDevEntity_t, crc ) == 8, "sndDevEntity_t layout" );

static_assert( sizeof( sndReverb_t ) == SND_SIZE_REVERB, "sndReverb_t is 32 B" );
static_assert( offsetof( sndReverb_t, decayTime ) == 0, "sndReverb_t layout" );
static_assert( offsetof( sndReverb_t, earlyDb ) == 12, "sndReverb_t layout" );
static_assert( offsetof( sndReverb_t, lateDb ) == 16, "sndReverb_t layout" );
static_assert( offsetof( sndReverb_t, preDelay ) == 20, "sndReverb_t layout" );
static_assert( offsetof( sndReverb_t, lateDelay ) == 24, "sndReverb_t layout" );
static_assert( offsetof( sndReverb_t, roomSize ) == 28, "sndReverb_t layout" );

static_assert( sizeof( sndBakeArea_t ) == SND_SIZE_BAKE_AREA, "sndBakeArea_t is 72 B" );
static_assert( offsetof( sndBakeArea_t, zone ) == 0, "sndBakeArea_t layout" );
static_assert( offsetof( sndBakeArea_t, cellMin ) == 4, "sndBakeArea_t layout" );
static_assert( offsetof( sndBakeArea_t, dims ) == 16, "sndBakeArea_t layout" );
static_assert( offsetof( sndBakeArea_t, firstCell ) == 28, "sndBakeArea_t layout" );
static_assert( offsetof( sndBakeArea_t, firstProbe ) == 32, "sndBakeArea_t layout" );
static_assert( offsetof( sndBakeArea_t, probeCount ) == 36, "sndBakeArea_t layout" );
static_assert( offsetof( sndBakeArea_t, defaultReverb ) == 40, "sndBakeArea_t layout" );

static_assert( sizeof( sndBakeProbe_t ) == SND_SIZE_BAKE_PROBE, "sndBakeProbe_t is 56 B" );
static_assert( offsetof( sndBakeProbe_t, position ) == 0, "sndBakeProbe_t layout" );
static_assert( offsetof( sndBakeProbe_t, radius ) == 12, "sndBakeProbe_t layout" );
static_assert( offsetof( sndBakeProbe_t, area ) == 16, "sndBakeProbe_t layout" );
static_assert( offsetof( sndBakeProbe_t, flags ) == 20, "sndBakeProbe_t layout" );
static_assert( offsetof( sndBakeProbe_t, reverb ) == 24, "sndBakeProbe_t layout" );

static_assert( sizeof( sndBakeDoor_t ) == SND_SIZE_BAKE_DOOR, "sndBakeDoor_t is 28 B" );
static_assert( offsetof( sndBakeDoor_t, portal ) == 0, "sndBakeDoor_t layout" );
static_assert( offsetof( sndBakeDoor_t, area0 ) == 4, "sndBakeDoor_t layout" );
static_assert( offsetof( sndBakeDoor_t, area1 ) == 8, "sndBakeDoor_t layout" );
static_assert( offsetof( sndBakeDoor_t, lossDb ) == 12, "sndBakeDoor_t layout" );
static_assert( offsetof( sndBakeDoor_t, nameOffset ) == 24, "sndBakeDoor_t layout" );

static_assert( sizeof( sndDevHead_t ) == SND_SIZE_DEV_HEAD, "sndDevHead_t is 32 B" );
static_assert( offsetof( sndDevHead_t, numEntities ) == 0, "sndDevHead_t layout" );
static_assert( offsetof( sndDevHead_t, numMaterials ) == 4, "sndDevHead_t layout" );
static_assert( offsetof( sndDevHead_t, scriptCRC ) == 8, "sndDevHead_t layout" );
static_assert( offsetof( sndDevHead_t, entitiesOffset ) == 12, "sndDevHead_t layout" );
static_assert( offsetof( sndDevHead_t, materialsOffset ) == 16, "sndDevHead_t layout" );
static_assert( offsetof( sndDevHead_t, stringsOffset ) == 20, "sndDevHead_t layout" );
static_assert( offsetof( sndDevHead_t, stringsSize ) == 24, "sndDevHead_t layout" );
static_assert( offsetof( sndDevHead_t, reserved ) == 28, "sndDevHead_t layout" );

/*
===============================================================================

	Hash and CRC helpers (R-SB1b-18), one copy for every package. Each word is hashed as its little-endian bytes
	(the target is little-endian, asserted above), never a struct's padding.

===============================================================================
*/

// CRC32 over each record's kind then crc, in map order (DR-15)
ID_INLINE uint32 SndEntityKeysCRC( const sndDevEntity_t* records, int numRecords )
{
	unsigned int crc;
	CRC32_InitChecksum( crc );
	for( int i = 0; i < numRecords; i++ )
	{
		CRC32_UpdateChecksum( crc, &records[i].kind, ( int )sizeof( records[i].kind ) );
		CRC32_UpdateChecksum( crc, &records[i].crc, ( int )sizeof( records[i].crc ) );
	}
	CRC32_FinishChecksum( crc );
	return crc;
}

// CRC32 over, per material in table order, its name's bytes, one 0, its seven floats as stored; the caller has
// checked that every nameOffset points at a zero-terminated name inside strings
ID_INLINE uint32 SndMaterialHash( const sndMaterial_t* records, int numRecords, const char* strings )
{
	unsigned int crc;
	CRC32_InitChecksum( crc );
	for( int i = 0; i < numRecords; i++ )
	{
		const char* name = strings + records[i].nameOffset;
		CRC32_UpdateChecksum( crc, name, ( int )strlen( name ) + 1 );
		CRC32_UpdateChecksum( crc, records[i].absorption, ( int )( sizeof( records[i] ) - offsetof( sndMaterial_t, absorption ) ) );
	}
	CRC32_FinishChecksum( crc );
	return crc;
}

// R-SB1b-7: the loader's range check of the lattice spacing
ID_INLINE bool SndSettingsValid( const sndSettings_t& settings )
{
	return settings.probeSpacing >= 32 && settings.probeSpacing <= 1024 && settings.probeSpacingZ >= 32 && settings.probeSpacingZ <= 1024;
}

// R-SB1b-24: every probe is at least this far from every triangle, max( C / 2, 6 ) units
ID_INLINE int SndProbeMinClearance( int clearance )
{
	const int half = clearance / 2;
	return ( half > 6 ) ? half : 6;
}

//puncho start - SB3-W (R-SB3-7): the step 1 sections' own test, one copy for the loader and the compiler

// the runs SndBakeStep1 tests are all floats: a field added later must fail here, not go untested (L3R-5)
static_assert( sizeof( sndReverb_t ) == 8 * sizeof( float ), "sndReverb_t is eight floats, tested as one run" );
static_assert( offsetof( sndBakeProbe_t, position ) == 0 && offsetof( sndBakeProbe_t, radius ) == 3 * sizeof( float )
			   && offsetof( sndBakeProbe_t, area ) == 4 * sizeof( float ), "sndBakeProbe_t's tested run is position[3] and radius" );
static_assert( offsetof( sndBakeProbe_t, reverb ) + sizeof( sndReverb_t ) == sizeof( sndBakeProbe_t ), "sndBakeProbe_t ends with its reverb" );
static_assert( offsetof( sndBakeArea_t, defaultReverb ) + sizeof( sndReverb_t ) == sizeof( sndBakeArea_t ), "sndBakeArea_t ends with its reverb" );
static_assert( sizeof( sndBakeDoor_t::lossDb ) == SND_NUM_BANDS * sizeof( float ) && offsetof( sndBakeDoor_t, nameOffset ) == offsetof( sndBakeDoor_t, lossDb ) + SND_NUM_BANDS * sizeof( float ),
			   "sndBakeDoor_t's tested run is lossDb[SND_NUM_BANDS], floats" );
static_assert( sizeof( sndSettings_t::duration ) == sizeof( float ) && sizeof( sndSettings_t::irradianceMinDistance ) == sizeof( float ), "the settings' two floats" );

// true when every float of a run of bytes is finite. Tests the exponent bits, not the value, so a NaN or an infinity
// read from a file never reaches a float compare. The run is 4-aligned
ID_INLINE bool SndFloatsFinite( const void* floats, int bytes )
{
	const uint32* words = ( const uint32* )floats;
	const int numWords = bytes / 4;
	for( int i = 0; i < numWords; i++ )
	{
		if( ( words[i] & 0x7f800000 ) == 0x7f800000 )
		{
			return false;
		}
	}
	return true;
}

// the step 1 sections of a .sndbake, pointers into the caller's data
struct sndBakeStep1_t
{
	const sndBakeArea_t*	areas;
	int						numAreas;
	const int32*			cells;
	int						numCells;
	const sndBakeProbe_t*	probes;
	int						numProbes;
	const sndReverb_t*		zones;
	int						numZones;
	const sndBakeDoor_t*	doors;
	int						numDoors;
	const char*				doorStrings;
	int						doorStringBytes;
	const sndSection_t*		devcheck;			// the BAK_DEVCHECK directory entry, NULL when absent
};

/*
SndBakeStep1, R-SB3-7 without the render world: the directory and the six step 1 sections, one pass each, in this
order: each directory entry (ascending ids, offset 16-aligned and past the directory; a step 1 section inside
step1Bytes with size == count * record size, BAK_STRINGS count 0 and size > 0; BAK_DEVCHECK inside fileLength
with count 0; other ids skipped), the six present, then the records: areas (zone, dims, firstCell, probe range, the
stepwise lattice product, finite reverb), cells (-1 or a probe), probes (area < the file's area count, finite),
zones (finite), doors (at most 32,768 for the edges' int16; portal >= 0, areas in range, name inside the strings,
finite), the last string byte 0. Returns 0 with out filled, or the id of the first section that fails; a failing
directory entry whose id is 0 returns 0xffffffff, since 0 is the pass.

Relies on, and does not test, what the caller has established: data holds at least step1Bytes bytes from the
file's byte 0, 4-aligned; data's header has numSections <= SND_MAX_SECTIONS, so the directory is
112 + 16 * numSections bytes, and step1Bytes >= that, so the directory lies inside data; step1Bytes <= fileLength
(so every size inside it fits an int). The magic and version are not read here: the caller has checked them so
that the layout is this one. Not tested here, the caller's against its render world: the file's area count and each
door's portal < NumPortals().
*/
ID_INLINE uint32 SndBakeStep1( const byte* data, int step1Bytes, int fileLength, sndBakeStep1_t& out )
{
	const sndFileHeader_t* header = ( const sndFileHeader_t* )data;
	const sndSection_t* dir = ( const sndSection_t* )( data + SND_HEADER_SIZE );
	const int numSections = ( int )header->numSections;
	const sndSection_t* step1[BAK_STRINGS - BAK_AREAS + 1] = { NULL, NULL, NULL, NULL, NULL, NULL };
	static const uint32 recordSizes[BAK_DOORS - BAK_AREAS + 1] = { SND_SIZE_BAKE_AREA, SND_SIZE_CELL, SND_SIZE_BAKE_PROBE, SND_SIZE_REVERB, SND_SIZE_BAKE_DOOR };
	const uint32 firstSectionOffset = SND_HEADER_SIZE + SND_SECTION_SIZE * header->numSections;	// none lies over the header or the directory
	const sndSection_t* devcheck = NULL;
	for( int i = 0; i < numSections; i++ )
	{
		const sndSection_t& s = dir[i];
		bool bad = ( i > 0 && s.id <= dir[i - 1].id ) || ( s.offset % 16 ) != 0 || s.offset < firstSectionOffset;
		if( !bad && s.id >= BAK_AREAS && s.id <= BAK_STRINGS )
		{
			bad = ( uint64 )s.offset + s.size > ( uint64 )step1Bytes;
			if( s.id == BAK_STRINGS )
			{
				bad = bad || s.count != 0 || s.size == 0;		// bytes; offset 0 is ""
			}
			else
			{
				bad = bad || ( uint64 )s.size != ( uint64 )s.count * recordSizes[s.id - BAK_AREAS];
			}
			step1[s.id - BAK_AREAS] = &s;
		}
		else if( !bad && s.id == BAK_DEVCHECK )
		{
			bad = ( uint64 )s.offset + s.size > ( uint64 )fileLength || s.count != 0;
			devcheck = &s;
		}
		// any other id is skipped
		if( bad )
		{
			return ( s.id != 0 ) ? s.id : 0xffffffffu;
		}
	}
	for( int i = 0; i <= BAK_STRINGS - BAK_AREAS; i++ )
	{
		if( step1[i] == NULL )
		{
			return BAK_AREAS + i;
		}
	}

	// every size is inside step1Bytes, so every count fits an int
	const sndBakeArea_t* areas = ( const sndBakeArea_t* )( data + step1[BAK_AREAS - BAK_AREAS]->offset );
	const int numAreas = ( int )step1[BAK_AREAS - BAK_AREAS]->count;
	const int32* cells = ( const int32* )( data + step1[BAK_CELLS - BAK_AREAS]->offset );
	const int numCells = ( int )step1[BAK_CELLS - BAK_AREAS]->count;
	const sndBakeProbe_t* probes = ( const sndBakeProbe_t* )( data + step1[BAK_PROBES - BAK_AREAS]->offset );
	const int numProbes = ( int )step1[BAK_PROBES - BAK_AREAS]->count;
	const sndReverb_t* zones = ( const sndReverb_t* )( data + step1[BAK_ZONES - BAK_AREAS]->offset );
	const int numZones = ( int )step1[BAK_ZONES - BAK_AREAS]->count;
	const sndBakeDoor_t* doors = ( const sndBakeDoor_t* )( data + step1[BAK_DOORS - BAK_AREAS]->offset );
	const int numDoors = ( int )step1[BAK_DOORS - BAK_AREAS]->count;
	const char* doorStrings = ( const char* )( data + step1[BAK_STRINGS - BAK_AREAS]->offset );
	const int doorStringBytes = ( int )step1[BAK_STRINGS - BAK_AREAS]->size;

	// every index in range
	for( int a = 0; a < numAreas; a++ )
	{
		const sndBakeArea_t& area = areas[a];
		bool areasBad = area.zone < 0 || area.zone >= numZones
						|| area.dims[0] < 0 || area.dims[0] > numCells
						|| area.dims[1] < 0 || area.dims[1] > numCells
						|| area.dims[2] < 0 || area.dims[2] > numCells
						|| area.firstCell < 0
						|| area.firstProbe < 0 || area.probeCount < 0 || ( int64 )area.firstProbe + area.probeCount > numProbes
						|| !SndFloatsFinite( &area.defaultReverb, SND_SIZE_REVERB );
		if( !areasBad )
		{
			// in steps, each partial product at most numCells, so none can wrap (L3-1)
			int64 areaCells = ( int64 )area.dims[0] * area.dims[1];
			if( areaCells > numCells )
			{
				areasBad = true;
			}
			else
			{
				areaCells *= area.dims[2];
				areasBad = areaCells > numCells || ( int64 )area.firstCell + areaCells > numCells;
			}
		}
		if( areasBad )
		{
			return BAK_AREAS;
		}
	}
	for( int c = 0; c < numCells; c++ )
	{
		if( cells[c] < -1 || cells[c] >= numProbes )
		{
			return BAK_CELLS;
		}
	}
	for( int p = 0; p < numProbes; p++ )
	{
		if( probes[p].area < 0 || probes[p].area >= numAreas
				|| !SndFloatsFinite( probes[p].position, ( int )( sizeof( probes[p].position ) + sizeof( probes[p].radius ) ) )
				|| !SndFloatsFinite( &probes[p].reverb, SND_SIZE_REVERB ) )
		{
			return BAK_PROBES;
		}
	}
	for( int z = 0; z < numZones; z++ )
	{
		if( !SndFloatsFinite( &zones[z], SND_SIZE_REVERB ) )
		{
			return BAK_ZONES;
		}
	}
	// an edge holds a door's index in an int16 (R-SB3-30): more records than that cannot be used (W4-6)
	if( numDoors > 0x8000 )
	{
		return BAK_DOORS;
	}
	for( int d = 0; d < numDoors; d++ )
	{
		const sndBakeDoor_t& door = doors[d];
		if( door.portal < 0
				|| door.area0 < 0 || door.area0 >= numAreas || door.area1 < 0 || door.area1 >= numAreas
				|| door.nameOffset >= ( uint32 )doorStringBytes
				|| !SndFloatsFinite( door.lossDb, ( int )sizeof( door.lossDb ) ) )
		{
			return BAK_DOORS;
		}
	}
	if( doorStrings[doorStringBytes - 1] != '\0' )
	{
		return BAK_STRINGS;
	}

	out.areas = areas;
	out.numAreas = numAreas;
	out.cells = cells;
	out.numCells = numCells;
	out.probes = probes;
	out.numProbes = numProbes;
	out.zones = zones;
	out.numZones = numZones;
	out.doors = doors;
	out.numDoors = numDoors;
	out.doorStrings = doorStrings;
	out.doorStringBytes = doorStringBytes;
	out.devcheck = devcheck;
	return 0;
}
//puncho end

// R-SB1b-23, defined in tools/compilers/dmap/acoustic_file.cpp (linked in the engine and rbdmap): the .sndbake
// inputHash, H64 over meshHash (8 bytes little-endian) then the 48 settings bytes
uint64 SndBakeInputHash( uint64 meshHash, const sndSettings_t& settings );

/*
===============================================================================

	The structure id (R-SB1b-29), fed the same way by dmap (AcousticStructureId) and the game
	(idRenderWorldLocal::GetMapCompileId):

		sndStructureHash_t s;
		SndStructureBegin( s, numAreas, numPortals );
		per portal:			SndStructurePortal( s, numPoints, area0, area1 );
		per node, preorder:	SndStructureNode( s, child0, child1 );
		uint64 id = SndStructureEnd( s );

	CRC32 over those int32 words, little-endian; the node count is not hashed. id = crc << 32 | the low 16 bits
	of numAreas << 16 | the low 16 bits of numPortals; 0 becomes 1.

===============================================================================
*/

struct sndStructureHash_t
{
	uint32		crc;
	int32		numAreas;
	int32		numPortals;
};

ID_INLINE void SndStructureBegin( sndStructureHash_t& state, int numAreas, int numPortals )
{
	const int32 words[2] = { numAreas, numPortals };
	state.numAreas = numAreas;
	state.numPortals = numPortals;
	CRC32_InitChecksum( state.crc );
	CRC32_UpdateChecksum( state.crc, words, ( int )sizeof( words ) );
}

ID_INLINE void SndStructurePortal( sndStructureHash_t& state, int numPoints, int area0, int area1 )
{
	const int32 words[3] = { numPoints, area0, area1 };
	CRC32_UpdateChecksum( state.crc, words, ( int )sizeof( words ) );
}

ID_INLINE void SndStructureNode( sndStructureHash_t& state, int child0, int child1 )
{
	const int32 words[2] = { child0, child1 };
	CRC32_UpdateChecksum( state.crc, words, ( int )sizeof( words ) );
}

ID_INLINE uint64 SndStructureEnd( sndStructureHash_t& state )
{
	CRC32_FinishChecksum( state.crc );
	const uint64 id = ( ( uint64 )state.crc << 32 ) | ( ( uint64 )( state.numAreas & 0xffff ) << 16 ) | ( uint64 )( state.numPortals & 0xffff );
	return ( id != 0 ) ? id : 1;
}

#endif /* !__SND_ACOUSTICFILE_H__ */
