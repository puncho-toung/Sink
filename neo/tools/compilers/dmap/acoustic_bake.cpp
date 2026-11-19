//puncho - SB2-I (WP-SB2 R-SB2-1 to R-SB2-6, R-SB2-11, R-SB2-26): Stage B's entry, the bakeAcoustics command, the read
// and full check of .sndmesh, the resolved settings and the skip against an existing .sndbake. Nothing here loads
// Steam Audio or writes a file: a failed check or an up-to-date bake costs only reading. The bake itself is the one
// step AcousticBake_Run, which SB2-B fills.
#include "precompiled.h"
#pragma hdrstop

#include "acoustic.h"

// the engine only: rbdmap (DMAP) compiles this file too and gets nothing from it (R-SB2-2). The engine built without
// USE_STEAMAUDIO_BAKE keeps the command and the stub of BakeAcoustics (C-SB49).
#if !defined( DMAP )

#if defined( USE_STEAMAUDIO_BAKE )

//puncho - SB2-B: the bake step's library table (phonon.h), the estimator and the phase slot
#include "acoustic_phonon.h"
#include "acoustic_estimate.h"
#include "CompilerPhase.h"

/*
===============================================================================

	The checked .sndmesh: the buffer fileSystem->ReadFile returned, its header, and a pointer and count for every
	K-SB5 section. Valid until AcousticBake_FreeMesh. SB2-B reads the mesh only through this view.

===============================================================================
*/

struct acousticBakeMesh_t
{
	byte*						buffer;			// fileSystem->ReadFile's, freed with FreeFile
	int							length;
	const sndFileHeader_t*		header;
	const sndMeshSettings_t*	settings;		// MSH_SETTINGS, one record
	const sndMaterial_t*		materials;
	int							numMaterials;
	const sndMeshVert_t*		verts;
	int							numVerts;
	const sndMeshTri_t*			tris;
	const sndMeshTriMat_t*		triMats;		// one per triangle
	int							numTris;
	const sndMeshArea_t*		areas;
	int							numAreas;
	const int32*				cells;
	int							numCells;
	const sndMeshProbe_t*		probes;
	int							numProbes;
	const sndPortal_t*			portals;
	int							numPortals;
	const sndMeshDoor_t*		doors;
	int							numDoors;
	const sndDevEntity_t*		entities;
	int							numEntities;
	const char*					strings;		// MSH_STRINGS, ends with a 0
	int							stringsSize;
};

// set by the run that owns it and cleared when that run leaves, by return or by an Error unwinding (the scope object
// in BakeAcoustics); a refused re-entry leaves it alone. The library's statics are per process (R-SB2-3)
static bool acousticBakeRunning = false;

// K-SB5, the twelve sections of .sndmesh in id order, so entry s is id s + 1 (asserted where it is read, the reader
// indexes it by MSH_x - 1): record size, 0 for the one section of bytes
static const struct
{
	uint32		id;
	uint32		recordSize;
	const char*	name;
} acousticMeshSections[] =
{
	{ MSH_SETTINGS,		SND_SIZE_MESH_SETTINGS,	"MSH_SETTINGS" },
	{ MSH_MATERIALS,	SND_SIZE_MATERIAL,		"MSH_MATERIALS" },
	{ MSH_VERTS,		SND_SIZE_MESH_VERT,		"MSH_VERTS" },
	{ MSH_TRIS,			SND_SIZE_MESH_TRI,		"MSH_TRIS" },
	{ MSH_TRIMAT,		SND_SIZE_MESH_TRIMAT,	"MSH_TRIMAT" },
	{ MSH_AREAS,		SND_SIZE_MESH_AREA,		"MSH_AREAS" },
	{ MSH_CELLS,		SND_SIZE_CELL,			"MSH_CELLS" },
	{ MSH_PROBES,		SND_SIZE_MESH_PROBE,	"MSH_PROBES" },
	{ MSH_PORTALS,		SND_SIZE_PORTAL,		"MSH_PORTALS" },
	{ MSH_DOORS,		SND_SIZE_MESH_DOOR,		"MSH_DOORS" },
	{ MSH_ENTITIES,		SND_SIZE_DEV_ENTITY,	"MSH_ENTITIES" },
	{ MSH_STRINGS,		0,						"MSH_STRINGS" }
};
static const int ACOUSTIC_MESH_SECTIONS = sizeof( acousticMeshSections ) / sizeof( acousticMeshSections[0] );

/*
==================
AcousticBake_FreeMesh
==================
*/
static void AcousticBake_FreeMesh( acousticBakeMesh_t& mesh )
{
	if( mesh.buffer != NULL )
	{
		fileSystem->FreeFile( mesh.buffer );
	}
	memset( &mesh, 0, sizeof( mesh ) );
}

/*
==================
AcousticBake_ReadMesh

R-SB2-4, then R-SB2-5: reads maps/<map>.sndmesh whole and checks it, the header first, then the content, stopping at
the first breach with a reason that names it. On false the caller frees mesh (the buffer may be set).
==================
*/
static bool AcousticBake_ReadMesh( const char* map, acousticBakeMesh_t& mesh, idStr& reason )
{
	memset( &mesh, 0, sizeof( mesh ) );

	// R-SB2-4: the file and its header
	const idStr path = va( "maps/%s.sndmesh", map );
	void* buffer = NULL;
	const int length = fileSystem->ReadFile( path.c_str(), &buffer );
	if( length < 0 || buffer == NULL )
	{
		reason.Format( "cannot read %s", path.c_str() );
		return false;
	}
	mesh.buffer = ( byte* )buffer;
	mesh.length = length;
	if( length < SND_HEADER_SIZE )
	{
		reason.Format( "%s is %i bytes, shorter than its header", path.c_str(), length );
		return false;
	}

	const byte* data = mesh.buffer;
	const sndFileHeader_t* header = ( const sndFileHeader_t* )data;
	mesh.header = header;
	if( header->magic != SND_MAGIC_MESH )
	{
		reason.Format( "magic 0x%08x, not SNDM", header->magic );
		return false;
	}
	if( header->version != SND_FORMAT_VERSION )
	{
		reason.Format( "version %u, not %i", header->version, SND_FORMAT_VERSION );
		return false;
	}
	// I4-1: a mesh of another Stage A revision would bake into a file the game refuses
	if( header->bakerRevision != SND_BAKER_REVISION_A )
	{
		reason.Format( "the .sndmesh is of Stage A revision %u, this build's is %i: compile the map again", header->bakerRevision, SND_BAKER_REVISION_A );
		return false;
	}
	if( header->compileId == 0 )
	{
		reason = "compileId 0";
		return false;
	}
	if( ( int64 )header->payloadSize != ( int64 )length - SND_HEADER_SIZE )
	{
		reason.Format( "payloadSize %u, the file holds %i bytes after the header", header->payloadSize, length - SND_HEADER_SIZE );
		return false;
	}
	const uint32 crc = SndFile_PayloadCRC( data, length );	// length >= SND_HEADER_SIZE, checked above
	if( header->payloadCRC != crc )
	{
		reason.Format( "payloadCRC 0x%08x, the file's is 0x%08x", header->payloadCRC, crc );
		return false;
	}

	// R-SB2-4: every K-SB5 section present. The writer writes exactly the twelve, ascending (R-SB1b-16, I4-10), so the
	// directory is refused unless entry s is id s + 1; each lies inside the file with its records' size
	if( header->numSections != ACOUSTIC_MESH_SECTIONS )
	{
		reason.Format( "numSections %u, a .sndmesh has %i", header->numSections, ACOUSTIC_MESH_SECTIONS );
		return false;
	}
	if( ( int64 )SND_HEADER_SIZE + ( int64 )header->numSections * SND_SECTION_SIZE > length )
	{
		reason.Format( "a directory of %u sections does not fit the file", header->numSections );
		return false;
	}
	const int64 directoryEnd = ( int64 )SND_HEADER_SIZE + ( int64 )header->numSections * SND_SECTION_SIZE;
	const sndSection_t* directory = ( const sndSection_t* )( data + SND_HEADER_SIZE );
	const void* sectionData[ACOUSTIC_MESH_SECTIONS];
	int sectionCount[ACOUSTIC_MESH_SECTIONS];
	for( int s = 0; s < ACOUSTIC_MESH_SECTIONS; s++ )
	{
		assert( acousticMeshSections[s].id == ( uint32 )( s + 1 ) );
		const char* name = acousticMeshSections[s].name;
		const sndSection_t* section = &directory[s];
		if( section->id != acousticMeshSections[s].id )
		{
			reason.Format( "directory entry %i is id 0x%x, not %s: the ids are not the twelve in ascending order", s, section->id, name );
			return false;
		}
		if( ( section->offset & 15 ) != 0 || ( int64 )section->offset < directoryEnd || ( int64 )section->offset + ( int64 )section->size > length )
		{
			reason.Format( "section %s at offset %u, %u bytes, is not a 16-aligned range of the file after the directory", name, section->offset, section->size );
			return false;
		}
		const uint32 recordSize = acousticMeshSections[s].recordSize;
		if( recordSize == 0 )
		{
			if( section->count != 0 )
			{
				reason.Format( "section %s is bytes but counts %u records", name, section->count );
				return false;
			}
		}
		else if( ( uint64 )section->size != ( uint64 )section->count * recordSize )
		{
			reason.Format( "section %s is %u bytes, not %u records of %u", name, section->size, section->count, recordSize );
			return false;
		}
		sectionData[s] = data + section->offset;
		sectionCount[s] = ( recordSize == 0 ) ? ( int )section->size : ( int )section->count;	// <= length, fits
	}
	if( sectionCount[MSH_SETTINGS - 1] != 1 )
	{
		reason.Format( "section MSH_SETTINGS counts %i records, not 1", sectionCount[MSH_SETTINGS - 1] );
		return false;
	}
	// W4D-1: the bake uses the header's five Stage A words (R-SB2-6), the input hash covers the record's (R-SB1b-23)
	{
		const sndSettings_t& stageA = header->settings;
		const sndMeshSettings_t& record = *( const sndMeshSettings_t* )sectionData[MSH_SETTINGS - 1];
		if( stageA.probeSpacing != record.probeSpacing || stageA.probeSpacingZ != record.probeSpacingZ || stageA.probeClearance != record.probeClearance
				|| stageA.sealDoorways != record.sealDoorways || stageA.staticTriBudget != record.staticTriBudget )
		{
			reason = "the header's Stage A settings differ from MSH_SETTINGS";
			return false;
		}
	}
	if( sectionCount[MSH_TRIMAT - 1] != sectionCount[MSH_TRIS - 1] )
	{
		reason.Format( "section MSH_TRIMAT counts %i records for %i triangles", sectionCount[MSH_TRIMAT - 1], sectionCount[MSH_TRIS - 1] );
		return false;
	}
	if( sectionCount[MSH_STRINGS - 1] < 1 || ( ( const char* )sectionData[MSH_STRINGS - 1] )[0] != '\0' || ( ( const char* )sectionData[MSH_STRINGS - 1] )[sectionCount[MSH_STRINGS - 1] - 1] != '\0' )
	{
		reason = "section MSH_STRINGS does not start and end with a 0";
		return false;
	}

	// the view; the section ids are 1 to 12 in table order
	mesh.settings		= ( const sndMeshSettings_t* )sectionData[MSH_SETTINGS - 1];
	mesh.materials		= ( const sndMaterial_t* )sectionData[MSH_MATERIALS - 1];
	mesh.numMaterials	= sectionCount[MSH_MATERIALS - 1];
	mesh.verts			= ( const sndMeshVert_t* )sectionData[MSH_VERTS - 1];
	mesh.numVerts		= sectionCount[MSH_VERTS - 1];
	mesh.tris			= ( const sndMeshTri_t* )sectionData[MSH_TRIS - 1];
	mesh.triMats		= ( const sndMeshTriMat_t* )sectionData[MSH_TRIMAT - 1];
	mesh.numTris		= sectionCount[MSH_TRIS - 1];
	mesh.areas			= ( const sndMeshArea_t* )sectionData[MSH_AREAS - 1];
	mesh.numAreas		= sectionCount[MSH_AREAS - 1];
	mesh.cells			= ( const int32* )sectionData[MSH_CELLS - 1];
	mesh.numCells		= sectionCount[MSH_CELLS - 1];
	mesh.probes			= ( const sndMeshProbe_t* )sectionData[MSH_PROBES - 1];
	mesh.numProbes		= sectionCount[MSH_PROBES - 1];
	mesh.portals		= ( const sndPortal_t* )sectionData[MSH_PORTALS - 1];
	mesh.numPortals		= sectionCount[MSH_PORTALS - 1];
	mesh.doors			= ( const sndMeshDoor_t* )sectionData[MSH_DOORS - 1];
	mesh.numDoors		= sectionCount[MSH_DOORS - 1];
	mesh.entities		= ( const sndDevEntity_t* )sectionData[MSH_ENTITIES - 1];
	mesh.numEntities	= sectionCount[MSH_ENTITIES - 1];
	mesh.strings		= ( const char* )sectionData[MSH_STRINGS - 1];
	mesh.stringsSize	= sectionCount[MSH_STRINGS - 1];

	// R-SB2-5: the content, in the requirement's order

	// every vertex finite
	for( int i = 0; i < mesh.numVerts; i++ )
	{
		const float* xyz = mesh.verts[i].xyz;
		if( IEEE_FLT_IS_INF_NAN( xyz[0] ) || IEEE_FLT_IS_INF_NAN( xyz[1] ) || IEEE_FLT_IS_INF_NAN( xyz[2] ) )
		{
			reason.Format( "vertex %i is not finite", i );
			return false;
		}
	}

	// every triangle index < vertex count
	for( int i = 0; i < mesh.numTris; i++ )
	{
		for( int k = 0; k < 3; k++ )
		{
			if( mesh.tris[i].v[k] >= ( uint32 )mesh.numVerts )
			{
				reason.Format( "triangle %i: vertex index %u, %i vertices", i, mesh.tris[i].v[k], mesh.numVerts );
				return false;
			}
		}
	}

	// every material index < material count: the triangles', then the doors' (-1 is a door with no plug material,
	// as Stage A writes it)
	for( int i = 0; i < mesh.numTris; i++ )
	{
		if( mesh.triMats[i].material >= mesh.numMaterials )
		{
			reason.Format( "triangle %i: material %i, %i materials", i, ( int )mesh.triMats[i].material, mesh.numMaterials );
			return false;
		}
	}
	for( int d = 0; d < mesh.numDoors; d++ )
	{
		if( mesh.doors[d].material < -1 || mesh.doors[d].material >= mesh.numMaterials )
		{
			reason.Format( "door %i: material %i, %i materials", d, mesh.doors[d].material, mesh.numMaterials );
			return false;
		}
	}

	// absorption, scattering, transmission in [0, 1]; the negated test also refuses a NaN
	for( int i = 0; i < mesh.numMaterials; i++ )
	{
		const sndMaterial_t& m = mesh.materials[i];
		for( int b = 0; b < SND_NUM_BANDS; b++ )
		{
			if( !( m.absorption[b] >= 0.0f && m.absorption[b] <= 1.0f ) )
			{
				reason.Format( "material %i: absorption band %i outside [0, 1]", i, b );
				return false;
			}
		}
		if( !( m.scattering >= 0.0f && m.scattering <= 1.0f ) )
		{
			reason.Format( "material %i: scattering outside [0, 1]", i );
			return false;
		}
		for( int b = 0; b < SND_NUM_BANDS; b++ )
		{
			if( !( m.transmission[b] >= 0.0f && m.transmission[b] <= 1.0f ) )
			{
				reason.Format( "material %i: transmission band %i outside [0, 1]", i, b );
				return false;
			}
		}
	}

	// I4-10: the writer sets no flag beyond bit 0 (plug) and bit 1 (two-sided)
	for( int i = 0; i < mesh.numTris; i++ )
	{
		if( ( mesh.triMats[i].flags & ~( SND_TRI_PLUG | SND_TRI_TWO_SIDED ) ) != 0 )
		{
			reason.Format( "triangle %i: flags 0x%x outside bits 0 and 1", i, ( int )mesh.triMats[i].flags );
			return false;
		}
	}

	// plug triangles (flag bit 0) exactly the ranges of MSH_DOORS. Each range adds +1 at its first triangle and -1
	// past its last; a running sum is then the number of ranges over a triangle, linear in doors plus triangles
	// (I4-6). Left accepted on purpose: ranges of doors on different portals that overlap (the writer never makes
	// them; doors on one portal share one range), and non-zero padding, which the CRC covers.
	for( int d = 0; d < mesh.numDoors; d++ )
	{
		const sndMeshDoor_t& door = mesh.doors[d];
		if( door.firstPlugTri < 0 || door.plugTriCount < 0 || ( int64 )door.firstPlugTri + door.plugTriCount > mesh.numTris )
		{
			reason.Format( "door %i: plug range %i + %i outside %i triangles", d, door.firstPlugTri, door.plugTriCount, mesh.numTris );
			return false;
		}
	}
	if( mesh.numTris > 0 )
	{
		int32* edge = ( int32* )Mem_ClearedAlloc( ( ( size_t )mesh.numTris + 1 ) * sizeof( int32 ), TAG_TOOLS );
		for( int d = 0; d < mesh.numDoors; d++ )
		{
			edge[mesh.doors[d].firstPlugTri]++;
			edge[mesh.doors[d].firstPlugTri + mesh.doors[d].plugTriCount]--;
		}
		int bad = -1;
		int64 cover = 0;	// at most the door count
		for( int i = 0; i < mesh.numTris && bad == -1; i++ )
		{
			cover += edge[i];
			if( ( ( mesh.triMats[i].flags & SND_TRI_PLUG ) != 0 ) != ( cover != 0 ) )
			{
				bad = i;
			}
		}
		Mem_Free( edge );
		if( bad != -1 )
		{
			reason.Format( "triangle %i: plug flag %s, %s a door's plug range", bad, ( mesh.triMats[bad].flags & SND_TRI_PLUG ) ? "set" : "clear", ( mesh.triMats[bad].flags & SND_TRI_PLUG ) ? "outside" : "inside" );
			return false;
		}
	}

	// areas' probe ranges contiguous, in area order, covering MSH_PROBES once (an area with probeCount 0 is valid);
	// each probe in an area's range names that area and is finite. A compiled map has at least one area (I4-10)
	if( mesh.numAreas == 0 )
	{
		reason = "MSH_AREAS holds no area";
		return false;
	}
	int64 nextProbe = 0;
	for( int a = 0; a < mesh.numAreas; a++ )
	{
		const sndMeshArea_t& area = mesh.areas[a];
		if( area.firstProbe != nextProbe || area.probeCount < 0 || nextProbe + area.probeCount > mesh.numProbes )
		{
			reason.Format( "area %i: probes %i + %i, expected to start at %i of %i", a, area.firstProbe, area.probeCount, ( int )nextProbe, mesh.numProbes );
			return false;
		}
		for( int p = area.firstProbe; p < area.firstProbe + area.probeCount; p++ )
		{
			if( mesh.probes[p].area != a )
			{
				reason.Format( "probe %i: area %i, inside area %i's range", p, mesh.probes[p].area, a );
				return false;
			}
			const sndMeshProbe_t& probe = mesh.probes[p];
			if( IEEE_FLT_IS_INF_NAN( probe.position[0] ) || IEEE_FLT_IS_INF_NAN( probe.position[1] ) || IEEE_FLT_IS_INF_NAN( probe.position[2] ) || IEEE_FLT_IS_INF_NAN( probe.radius ) )
			{
				reason.Format( "probe %i is not finite", p );
				return false;
			}
			if( !( probe.radius > 0.0f ) )
			{
				reason.Format( "probe %i: radius %g", p, probe.radius );
				return false;
			}
		}
		nextProbe += area.probeCount;
	}
	if( nextProbe != mesh.numProbes )
	{
		reason.Format( "the areas cover %i of %i probes", ( int )nextProbe, mesh.numProbes );
		return false;
	}

	// each cell value -1 or a probe of its own area; an area's cells are its box, firstCell the running sum
	// (R-SB1b-14), so every cell has one area
	int64 nextCell = 0;
	for( int a = 0; a < mesh.numAreas; a++ )
	{
		const sndMeshArea_t& area = mesh.areas[a];
		int64 cells = -1;
		if( area.dims[0] >= 0 && area.dims[1] >= 0 && area.dims[2] >= 0 && area.firstCell == nextCell )
		{
			// stepwise, so a damaged size cannot overflow: the running product is held to the cell count before each
			// multiply, and the cell count is below 2^29 (the file is under 2 GB)
			cells = area.dims[0];
			cells = ( cells <= mesh.numCells ) ? cells * area.dims[1] : -1;
			cells = ( cells >= 0 && cells <= mesh.numCells ) ? cells * area.dims[2] : -1;
		}
		if( cells < 0 || nextCell + cells > mesh.numCells )
		{
			reason.Format( "area %i: cells from %i, box %i x %i x %i, expected to start at %i of %i", a, area.firstCell, area.dims[0], area.dims[1], area.dims[2], ( int )nextCell, mesh.numCells );
			return false;
		}
		for( int c = area.firstCell; c < area.firstCell + ( int )cells; c++ )
		{
			const int32 probe = mesh.cells[c];
			if( probe != -1 && ( probe < area.firstProbe || probe >= area.firstProbe + area.probeCount ) )
			{
				reason.Format( "cell %i of area %i: probe %i, not -1 or a probe of its area", c, a, probe );
				return false;
			}
		}
		nextCell += cells;
	}
	if( nextCell != mesh.numCells )
	{
		reason.Format( "the areas cover %i of %i cells", ( int )nextCell, mesh.numCells );
		return false;
	}

	// portal and door areas < area count; with them the cross indices the bake follows (a portal's first screened
	// door, a door's portal)
	for( int p = 0; p < mesh.numPortals; p++ )
	{
		const sndPortal_t& portal = mesh.portals[p];
		if( portal.area0 < 0 || portal.area0 >= mesh.numAreas || portal.area1 < 0 || portal.area1 >= mesh.numAreas )
		{
			reason.Format( "portal %i: areas %i and %i, %i areas", p, portal.area0, portal.area1, mesh.numAreas );
			return false;
		}
		if( portal.door < -1 || portal.door >= mesh.numDoors )
		{
			reason.Format( "portal %i: door %i, %i doors", p, portal.door, mesh.numDoors );
			return false;
		}
		// I4R-2: the door a portal names is on that portal
		if( portal.door != -1 && mesh.doors[portal.door].portal != p )
		{
			reason.Format( "portal %i: door %i is on portal %i", p, portal.door, mesh.doors[portal.door].portal );
			return false;
		}
	}
	for( int d = 0; d < mesh.numDoors; d++ )
	{
		const sndMeshDoor_t& door = mesh.doors[d];
		if( door.area0 < 0 || door.area0 >= mesh.numAreas || door.area1 < 0 || door.area1 >= mesh.numAreas )
		{
			reason.Format( "door %i: areas %i and %i, %i areas", d, door.area0, door.area1, mesh.numAreas );
			return false;
		}
		if( door.portal < 0 || door.portal >= mesh.numPortals )
		{
			reason.Format( "door %i: portal %i, %i portals", d, door.portal, mesh.numPortals );
			return false;
		}
		// I4-10: a door record exists only for a screened portal, whose record names its first door
		const int named = mesh.portals[door.portal].door;
		if( named == -1 )
		{
			reason.Format( "door %i: portal %i names no door", d, door.portal );
			return false;
		}
		// I4R-2: the named door is the first door record on the portal (in range and on it, the portal loop above)
		if( named > d )
		{
			reason.Format( "door %i: portal %i names door %i, a later record, as its first", d, door.portal, named );
			return false;
		}
		// I4S-1: doors on one portal share one plug range
		if( mesh.doors[named].firstPlugTri != door.firstPlugTri || mesh.doors[named].plugTriCount != door.plugTriCount )
		{
			reason.Format( "door %i: plug range %i + %i, door %i on the same portal has %i + %i", d, door.firstPlugTri, door.plugTriCount, named, mesh.doors[named].firstPlugTri, mesh.doors[named].plugTriCount );
			return false;
		}
	}

	// probeClearance C in [8, min( S, Sz ) / 4] units (DR-55), the header's Stage A words
	const sndSettings_t& stageA = header->settings;
	const int minSpacing = ( stageA.probeSpacing < stageA.probeSpacingZ ) ? stageA.probeSpacing : stageA.probeSpacingZ;
	if( stageA.probeClearance < 8 || stageA.probeClearance > minSpacing / 4 )
	{
		reason.Format( "probeClearance %i outside [8, min( %i, %i ) / 4]", stageA.probeClearance, stageA.probeSpacing, stageA.probeSpacingZ );
		return false;
	}

	// every name offset inside MSH_STRINGS, which ends with a 0, so each name is terminated inside it
	for( int i = 0; i < mesh.numMaterials; i++ )
	{
		if( mesh.materials[i].nameOffset >= ( uint32 )mesh.stringsSize )
		{
			reason.Format( "material %i: nameOffset %u, MSH_STRINGS is %i bytes", i, mesh.materials[i].nameOffset, mesh.stringsSize );
			return false;
		}
	}
	for( int d = 0; d < mesh.numDoors; d++ )
	{
		if( mesh.doors[d].nameOffset >= ( uint32 )mesh.stringsSize )
		{
			reason.Format( "door %i: nameOffset %u, MSH_STRINGS is %i bytes", d, mesh.doors[d].nameOffset, mesh.stringsSize );
			return false;
		}
	}
	for( int e = 0; e < mesh.numEntities; e++ )
	{
		if( mesh.entities[e].nameOffset >= ( uint32 )mesh.stringsSize )
		{
			reason.Format( "entity %i: nameOffset %u, MSH_STRINGS is %i bytes", e, mesh.entities[e].nameOffset, mesh.stringsSize );
			return false;
		}
		// I4-10: kinds 0, 1 and 2 only (DR-15)
		if( mesh.entities[e].kind > 2 )
		{
			reason.Format( "entity %i: kind %u, above 2", e, mesh.entities[e].kind );
			return false;
		}
	}

	// I4-3: the header's hashes recomputed from the sections, now that every section and name offset is checked. The
	// input hash is one MD5 pass over the file (SndFile_MeshInputHash: the writer's own function; the directory is
	// the twelve, checked above)
	if( SndFile_MeshInputHash( data ) != header->inputHash )
	{
		reason = "the header's inputHash is not the hash of the sections";
		return false;
	}
	if( SndMaterialHash( mesh.materials, mesh.numMaterials, mesh.strings ) != header->materialHash )
	{
		reason = "the header's materialHash is not the hash of the sections";
		return false;
	}
	if( SndEntityKeysCRC( mesh.entities, mesh.numEntities ) != header->entityKeysCRC )
	{
		reason = "the header's entityKeysCRC is not the hash of the sections";
		return false;
	}

	return true;
}

// R-SB2-11's factor, one constant so the word does not depend on the order of two float multiplies (I4-8); written
// with literals (0.0254f is DOOM_TO_METERS, a const float and so not a constant expression) so it is folded at compile
// time, never run at start (I4R-4). The settings are hashed into the bake: this value belongs to
// SND_BAKER_REVISION_B, and a change to it raises it
static const float ACOUSTIC_MIN_DISTANCE_PER_UNIT = 0.9f * 0.0254f;

/*
==================
AcousticBake_ResolveSettings

R-SB2-6: zeroed, then filled in the requirement's order. The five Stage A words are the .sndmesh header's (checked
equal to MSH_SETTINGS); the Stage B words are SndResolveStageB's; irradianceMinDistance by R-SB2-11. The thread
count is not a setting: it changes no output.
==================
*/
static void AcousticBake_ResolveSettings( const acousticBakeMesh_t& mesh, bool fast, sndSettings_t& settings )
{
	memset( &settings, 0, sizeof( settings ) );

	const sndSettings_t& stageA = mesh.header->settings;
	settings.probeSpacing		= stageA.probeSpacing;
	settings.probeSpacingZ		= stageA.probeSpacingZ;
	settings.probeClearance		= stageA.probeClearance;
	settings.sealDoorways		= stageA.sealDoorways;
	settings.staticTriBudget	= stageA.staticTriBudget;

	// the Stage B words: the preset, the map's raw MSH_SETTINGS values, the roundings and clamps; Stage A's one
	// resolver (acoustic_file.cpp), so the .sndmesh header and the bake cannot drift apart
	const sndMeshSettings_t& raw = *mesh.settings;
	SndResolveStageB( settings, fast, raw.rawRays, raw.rawBounces, raw.rawDuration );

	settings.diffuseSamples = 1024;

	// R-SB2-11: min( 1.0 m, 0.9 x SndProbeMinClearance( C ) x DOOM_TO_METERS ); the least, 0.137 m at C = 8
	const float minDistance = ( float )SndProbeMinClearance( settings.probeClearance ) * ACOUSTIC_MIN_DISTANCE_PER_UNIT;
	settings.irradianceMinDistance = ( minDistance < 1.0f ) ? minDistance : 1.0f;
}

// what the skip found: bake, up to date for the settings asked, or a fast request met by a final bake (I4-12b)
enum acousticSkip_t
{
	ACOUSTIC_SKIP_NONE,
	ACOUSTIC_SKIP_ASKED,
	ACOUSTIC_SKIP_FINAL
};

/*
==================
AcousticBake_SettingsMatch

The two words of an old bake that depend on the settings asked: its inputHash and its settings.
==================
*/
static bool AcousticBake_SettingsMatch( const sndFileHeader_t& bake, const sndFileHeader_t& meshHeader, const sndSettings_t& settings )
{
	return bake.inputHash == SndBakeInputHash( meshHeader.inputHash, settings )
		   && memcmp( &bake.settings, &settings, sizeof( settings ) ) == 0;
}

/*
==================
AcousticBake_SameName

True when the zero-terminated name at offset in an old bake's string table of size bytes equals meshName; reads
nothing past the table, whatever the old file holds.
==================
*/
static bool AcousticBake_SameName( const char* table, uint32 size, uint32 offset, const char* meshName )
{
	for( uint32 k = 0; ; k++ )
	{
		if( offset >= size || k >= size - offset || table[offset + k] != meshName[k] )
		{
			return false;
		}
		if( meshName[k] == '\0' )
		{
			return true;
		}
	}
}

/*
==================
AcousticBake_SameAsMesh

I4R-3, what Stage A tests after "unreadable" (R-SB1b-27): the old bake's areas, cells, probes and doors counted as
the mesh's; each door's portal, areas and name equal to the mesh door's; BAK_DEVCHECK byte for byte the section
built from this mesh (SB1b's one builder, which also covers scriptCRC). data passed AcousticBakeUnreadable.
==================
*/
static bool AcousticBake_SameAsMesh( const byte* data, const acousticBakeMesh_t& mesh )
{
	const sndSection_t* areas = SndFile_FindSection( data, BAK_AREAS );
	const sndSection_t* cells = SndFile_FindSection( data, BAK_CELLS );
	const sndSection_t* probes = SndFile_FindSection( data, BAK_PROBES );
	const sndSection_t* doors = SndFile_FindSection( data, BAK_DOORS );
	const sndSection_t* strings = SndFile_FindSection( data, BAK_STRINGS );
	const sndSection_t* dev = SndFile_FindSection( data, BAK_DEVCHECK );
	if( areas == NULL || cells == NULL || probes == NULL || doors == NULL || strings == NULL || dev == NULL )
	{
		return false;
	}
	if( areas->count != ( uint32 )mesh.numAreas || cells->count != ( uint32 )mesh.numCells || probes->count != ( uint32 )mesh.numProbes
			|| doors->count != ( uint32 )mesh.numDoors || ( uint64 )doors->size != ( uint64 )doors->count * SND_SIZE_BAKE_DOOR )
	{
		return false;
	}

	const sndBakeDoor_t* bakeDoors = ( const sndBakeDoor_t* )( data + doors->offset );
	const char* bakeStrings = ( const char* )( data + strings->offset );
	for( int d = 0; d < mesh.numDoors; d++ )
	{
		const sndBakeDoor_t& bakeDoor = bakeDoors[d];
		const sndMeshDoor_t& meshDoor = mesh.doors[d];
		if( bakeDoor.portal != meshDoor.portal || bakeDoor.area0 != meshDoor.area0 || bakeDoor.area1 != meshDoor.area1
				|| !AcousticBake_SameName( bakeStrings, strings->size, bakeDoor.nameOffset, mesh.strings + meshDoor.nameOffset ) )
		{
			return false;
		}
	}

	// the scratch is freed on the one path out; nothing between can return or raise
	const int devSize = SndFile_DevCheckSize( mesh.numEntities, mesh.numMaterials, mesh.stringsSize );
	if( devSize <= 0 || dev->size != ( uint32 )devSize )
	{
		return false;
	}
	byte* built = ( byte* )Mem_Alloc( ( size_t )devSize, TAG_TOOLS );
	SndFile_BuildDevCheck( built, mesh.entities, mesh.numEntities, mesh.materials, mesh.numMaterials, mesh.strings, mesh.stringsSize, mesh.settings->scriptCRC );
	const bool same = memcmp( built, data + dev->offset, ( size_t )devSize ) == 0;
	Mem_Free( built );
	return same;
}

/*
==================
AcousticBake_UpToDate

R-SB2-26, before anything is loaded. The old maps/<map>.sndbake, if it exists, must pass R-SB1b-27's step 1 test
(AcousticBakeUnreadable, the one Stage A uses, I4-9) and Stage A's next tests (AcousticBake_SameAsMesh, I4R-3); then
every listed field must equal what this run would write. Returns ACOUSTIC_SKIP_NONE (bake), ACOUSTIC_SKIP_ASKED
(equal for the settings asked) or ACOUSTIC_SKIP_FINAL (a fast request met by a final bake: finalSettings is non-NULL
only for a fast request; a final request is never met by a fast bake). Any difference is a bake, never a failure.
Neither restamps nor deletes: Stage A did that before BakeAcoustics was called (R-SB1b-27).
==================
*/
static acousticSkip_t AcousticBake_UpToDate( const char* map, const acousticBakeMesh_t& mesh, const sndSettings_t& settings, const sndSettings_t* finalSettings )
{
	void* buffer = NULL;
	const int length = fileSystem->ReadFile( va( "maps/%s.sndbake", map ), &buffer );
	if( length < 0 || buffer == NULL )
	{
		return ACOUSTIC_SKIP_NONE;
	}

	const byte* data = ( const byte* )buffer;
	acousticSkip_t skip = ACOUSTIC_SKIP_NONE;
	if( AcousticBakeUnreadable( data, length ) == NULL )
	{
		// readable: the header is whole, the directory inside the file, the step 1 sections and BAK_DEVCHECK in place
		const sndFileHeader_t& bake = *( const sndFileHeader_t* )data;
		const sndFileHeader_t& meshHeader = *mesh.header;

		// the fields that do not depend on the settings asked; scriptCRC is in BAK_DEVCHECK, compared whole there
		const bool stamps = bake.bakerRevision == ( ( ( uint32 )SND_BAKER_REVISION_B << 16 ) | meshHeader.bakerRevision )
							&& bake.toolVersion == SND_TOOL_VERSION
							&& bake.numBands == SND_NUM_BANDS
							&& bake.compileId == meshHeader.compileId
							&& bake.mapGeometryCRC == meshHeader.mapGeometryCRC
							&& bake.entityKeysCRC == meshHeader.entityKeysCRC
							&& bake.materialHash == meshHeader.materialHash
							&& AcousticBake_SameAsMesh( data, mesh );
		if( stamps && AcousticBake_SettingsMatch( bake, meshHeader, settings ) )
		{
			skip = ACOUSTIC_SKIP_ASKED;
		}
		else if( stamps && finalSettings != NULL && AcousticBake_SettingsMatch( bake, meshHeader, *finalSettings ) )
		{
			skip = ACOUSTIC_SKIP_FINAL;
		}
	}

	fileSystem->FreeFile( buffer );
	return skip;
}

//puncho start - SB2-B (R-SB2-7 to R-SB2-14, R-SB2-24, R-SB2-27 to R-SB2-30): the bake step's constants, state and callbacks

static_assert( IPL_NUM_BANDS == 3, "the bake reads three bands (R-SB2-7)" );
static_assert( SND_TOOL_VERSION == STEAMAUDIO_VERSION, "the .sndbake toolVersion is this Steam Audio's (SEAMS D-36)" );

// R-SB2-12 kLeakFail (SEAMS D-10), DR-160: ten times the worst escaped-ray rate per area measured on a sealed mesh,
// which is the compiler-made mesh of M-SB0-6's set (I6), measured with this lane's bakes: 6,026 of 9,076,736 rays at
// final, 6.639e-04 (wave 1's other sealed meshes gave 2.441e-04). The rule does not change: an area fails when
// escaped > max( 8, ACOUSTIC_LEAK_FAIL x cast ), counted in rays. It decides only pass or fail, never a byte of the file, so DR-160 raised no SND_BAKER_REVISION_B
static const float ACOUSTIC_LEAK_FAIL = 6.64e-03f;

// R-SB2-27: changes no output (patch 0001, R-SB0-9), so it is a cvar and never written
idCVar dmap_acousticThreads( "dmap_acousticThreads", "0", CVAR_TOOL | CVAR_INTEGER, "threads of the acoustic bake, 0 = every logical core; changes no output", 0.0f, 64.0f );

static const int ACOUSTIC_BAKE_SECTIONS = 7;	// BAK_AREAS .. BAK_STRINGS, BAK_DEVCHECK (R-SB2-24)

// R-SB2-10 step 1: the library's log, written by its pool threads under the mutex and printed on dmap's thread by the
// progress callback. Lives on AcousticBake_Run's stack; acousticBakeLog points at it only while a context can exist
struct acousticBakeLog_t
{
	idSysMutex			mutex;
	char				text[4096];
	int					length;
	bool				dropped;
};
static acousticBakeLog_t* acousticBakeLog = NULL;

// everything one bake takes, released by AcousticBake_Release on every path, an Error unwinding included (D-42)
struct acousticBakeRun_t
{
	phononApi_t				api;
	IPLContext				context;
	IPLScene				scene;
	IPLStaticMesh			staticMesh;
	IPLProbeBatch			probeBatch;
	IPLEnergyField			energyField;
	IPLVector3*				vertices;		// the scene's inputs, freed once the library has copied them
	IPLTriangle*			triangles;
	IPLint32*				materialIndices;
	IPLMaterial*			materials;
	IPLBakeProbeStats*		stats;			// one per probe, copied by the progress callback (R-SB2-28)
	uint32*					probeFlags;
	sndReverb_t*			probeReverb;
	sndReverb_t*			areaReverb;
	int*					areaZone;
	sndReverb_t*			zoneReverb;
	float*					histogram;		// SND_NUM_BANDS x numBins, one probe's channel 0
	acousticAreaSums_t		sums;
	bool					sumsBegun;
	sndFileBuilder_t		builder;
	int						numProbes;
	int						probesDone;		// progress callback calls
	bool					cancelled;
	int						startMs;		// the bake step, for DP_ACOUSTIC_BAKE
	int						bakeStartMs;	// ReflectionsBakerBake, for the progress line
	int						lineMs;
	int						lineStep;		// 5 % steps printed
};

/*
==================
AcousticBake_Log

Steam Audio's log callback, on any thread. Appends one line; a full buffer drops the rest and says so at the flush.
==================
*/
static void IPLCALL AcousticBake_Log( IPLLogLevel level, const char* message )
{
	acousticBakeLog_t* bakeLog = acousticBakeLog;
	if( bakeLog == NULL || message == NULL )
	{
		return;
	}
	idScopedCriticalSection lock( bakeLog->mutex );
	const int length = idStr::Length( message );
	// the text, a newline and the terminating 0
	if( bakeLog->length + length + 2 > ( int )sizeof( bakeLog->text ) )
	{
		bakeLog->dropped = true;
		return;
	}
	memcpy( bakeLog->text + bakeLog->length, message, length );
	bakeLog->length += length;
	if( length == 0 || message[length - 1] != '\n' )
	{
		bakeLog->text[bakeLog->length++] = '\n';
	}
	bakeLog->text[bakeLog->length] = '\0';
}

/*
==================
AcousticBake_FlushLog

dmap's thread: prints what the library logged since the last flush.
==================
*/
static void AcousticBake_FlushLog()
{
	acousticBakeLog_t* bakeLog = acousticBakeLog;
	if( bakeLog == NULL )
	{
		return;
	}
	idScopedCriticalSection lock( bakeLog->mutex );
	if( bakeLog->length > 0 )
	{
		common->Printf( "%s", bakeLog->text );
	}
	if( bakeLog->dropped )
	{
		common->Printf( "bakeAcoustics: lines of the library's log were dropped\n" );
	}
	bakeLog->length = 0;
	bakeLog->text[0] = '\0';
	bakeLog->dropped = false;
}

/*
==================
AcousticBake_EscapeHeld

R-SB2-29: Escape held while a window of this process (the engine's window or its console) is in front. Windows only;
elsewhere there is no cancel.
==================
*/
static bool AcousticBake_EscapeHeld()
{
#if defined( _WIN32 )
	if( ( GetAsyncKeyState( VK_ESCAPE ) & 0x8000 ) == 0 )
	{
		return false;
	}
	HWND window = GetForegroundWindow();
	if( window == NULL )
	{
		return false;
	}
	DWORD process = 0;
	GetWindowThreadProcessId( window, &process );
	return process == GetCurrentProcessId();
#else
	return false;
#endif
}

/*
==================
AcousticBake_Progress

R-SB2-28, dmap's thread, once per probe in index order. Counts its own calls: call n is probe n - 1 (DR-39); the
fraction is never read. Copies the probe's statistics (valid only here, SEAMS D-25), flushes the log, polls cancel
(only here is the bake cancelled, V10) and prints at most one line per 5 % step and per second.
==================
*/
static void IPLCALL AcousticBake_Progress( IPLfloat32 progress, void* userData )
{
	acousticBakeRun_t& run = *( acousticBakeRun_t* )userData;
	const int probe = run.probesDone;
	run.probesDone++;
	if( probe < run.numProbes )
	{
		run.api.ReflectionsBakerGetLastProbeStats( &run.stats[probe] );
	}

	AcousticBake_FlushLog();

#if defined( _WIN32 )
	// DR-162: Windows marks a window hung, and puts its ghost in front, when its thread has not looked at its queue for
	// five seconds; with the ghost in front this process reads Escape's key state as 0. A look with PM_NOREMOVE, every
	// callback, keeps the engine's window in front; it takes no message out and dispatches no queued message (only
	// messages another thread or program sends to the window are delivered inside it). The result is not used
	MSG message;
	PeekMessage( &message, NULL, 0, 0, PM_NOREMOVE );
#endif

	if( !run.cancelled && AcousticBake_EscapeHeld() )
	{
		run.api.ReflectionsBakerCancelBake( run.context );
		run.cancelled = true;
	}

	const int done = run.probesDone;
	const int total = ( run.numProbes > 0 ) ? run.numProbes : 1;
	const int step = ( int )( ( int64 )done * 20 / total );
	const int now = Sys_Milliseconds();
	if( step > run.lineStep && now - run.lineMs >= 1000 )
	{
		const int64 elapsed = now - run.bakeStartMs;
		const int64 left = ( done > 0 ) ? elapsed * ( total - done ) / done : 0;
		common->Printf( "bakeAcoustics: %i%% %i/%i probes, %i s, about %i s left\n", ( int )( ( int64 )done * 100 / total ), done, run.numProbes,
						( int )( elapsed / 1000 ), ( int )( left / 1000 ) );
		run.lineStep = step;
		run.lineMs = now;
	}
}

/*
==================
AcousticBake_Release

R-SB2-10 step 9, the one teardown of a bake: the library's objects in reverse order of creation, the log, the
unload, then this lane's memory. Safe on a partly built run (every member starts zeroed).
==================
*/
static void AcousticBake_Release( acousticBakeRun_t& run )
{
	if( run.sumsBegun )
	{
		AcousticAreaSums_Free( run.sums );
		run.sumsBegun = false;
	}
	SndFile_Free( run.builder );

	if( run.energyField != NULL )
	{
		run.api.EnergyFieldRelease( &run.energyField );
	}
	if( run.probeBatch != NULL )
	{
		run.api.ProbeBatchRelease( &run.probeBatch );
	}
	if( run.staticMesh != NULL )
	{
		run.api.StaticMeshRelease( &run.staticMesh );
	}
	if( run.scene != NULL )
	{
		run.api.SceneRelease( &run.scene );
	}
	if( run.context != NULL )
	{
		run.api.ContextRelease( &run.context );
	}
	AcousticBake_FlushLog();
	PhononApi_Unload( run.api );

	Mem_Free( run.vertices );
	Mem_Free( run.triangles );
	Mem_Free( run.materialIndices );
	Mem_Free( run.materials );
	Mem_Free( run.stats );
	Mem_Free( run.probeFlags );
	Mem_Free( run.probeReverb );
	Mem_Free( run.areaReverb );
	Mem_Free( run.areaZone );
	Mem_Free( run.zoneReverb );
	Mem_Free( run.histogram );
}

// owns one bake's run and the log pointer: released when AcousticBake_Run leaves, by return or by an Error unwinding
// (SndFile_* and the estimator can raise one), as idScopedAcousticBake does for the mesh (D-42)
class idScopedAcousticBakeRun
{
public:
	idScopedAcousticBakeRun( acousticBakeRun_t& run_, acousticBakeLog_t& bakeLog ) : run( run_ )
	{
		memset( &run, 0, sizeof( run ) );
		run.startMs = Sys_Milliseconds();
		bakeLog.length = 0;
		bakeLog.text[0] = '\0';
		bakeLog.dropped = false;
		acousticBakeLog = &bakeLog;
	}
	~idScopedAcousticBakeRun()
	{
		AcousticBake_Release( run );
		acousticBakeLog = NULL;		// after the unload: no library thread is left to log
		AcousticPhaseEnd( DP_ACOUSTIC_BAKE, run.startMs );
	}
private:
	acousticBakeRun_t&	run;
};

// R-SB2-8 (K-SB3): engine x forward, y left, z up to Steam Audio x right, y up, z back; float, negate then scale
static IPLVector3 AcousticBake_ToSteamAudio( const float* p )
{
	IPLVector3 v;
	memset( &v, 0, sizeof( v ) );
	v.x = ( -p[1] ) * DOOM_TO_METERS;
	v.y = ( p[2] ) * DOOM_TO_METERS;
	v.z = ( -p[0] ) * DOOM_TO_METERS;
	return v;
}

/*
==================
AcousticBake_Gone

B5-3, the test of Stage A's AcousticWrite_Gone (acoustic_write.cpp, file-static there): true when relativePath exists
in neither write folder (a stat, so a file held open still counts as there) and opens nowhere else on the search
path. RemoveFile reports nothing, so the leak delete is checked with this.
==================
*/
static bool AcousticBake_Gone( const char* relativePath )
{
	// an empty fs_savepath would make IsFolder stat a path at the root of the current drive
	const bool hasSavePath = ( cvarSystem->GetCVarString( "fs_savepath" )[0] != '\0' );
	if( fileSystem->IsFolder( relativePath, "fs_basepath" ) != FOLDER_ERROR || ( hasSavePath && fileSystem->IsFolder( relativePath, "fs_savepath" ) != FOLDER_ERROR ) )
	{
		return false;	// FOLDER_ERROR is the stat's "no such path"; anything else exists
	}
	idFile* file = fileSystem->OpenFileRead( relativePath, false );
	if( file == NULL )
	{
		return true;
	}
	fileSystem->CloseFile( file );
	return false;
}

static bool AcousticBake_Fail( const char* map, const char* reason );

/*
==================
AcousticBake_LibraryFail

A Steam Audio call that returned an error status: the FAILED line names the call.
==================
*/
static bool AcousticBake_LibraryFail( const char* map, const char* call, IPLerror status )
{
	idStr reason;
	reason.Format( "%s returned IPLerror %i", call, ( int )status );
	return AcousticBake_Fail( map, reason.c_str() );
}

//puncho end

/*
==================
AcousticBake_Run

The bake: library, scene, probes, leak gate, estimator, the write (R-SB2-7 onward). SB2-B fills this body and
changes nothing else of the call. It is the first place a library may be loaded or a file written.
==================
*/
static bool AcousticBake_Run( const char* mapName, const acousticBakeMesh_t& mesh, const sndSettings_t& settings )
{
	//puncho start - SB2-B
	// declared in this order so that the run is released (scope) before the log it may still flush is destroyed
	acousticBakeLog_t bakeLog;
	acousticBakeRun_t run;
	idScopedAcousticBakeRun scope( run, bakeLog );
	idStr reason;

	// R-SB2-23 reads each door's plug material; Stage A writes -1 for none only on a mesh it then fails, and the
	// requirement gives no loss for one, so such a mesh is refused before anything is loaded (reported to the director)
	for( int d = 0; d < mesh.numDoors; d++ )
	{
		if( mesh.doors[d].material < 0 )
		{
			reason.Format( "door %i has no plug material", d );
			return AcousticBake_Fail( mapName, reason.c_str() );
		}
	}

	// R-SB2-27, SEAMS D-25: at least one thread (with 0 the library bakes zeros and reports nothing)
	int numThreads = dmap_acousticThreads.GetInteger();
	if( numThreads <= 0 )
	{
		numThreads = parallelJobManager->GetLogicalCpuCores();
	}
	if( numThreads < 1 )
	{
		numThreads = 1;
	}
	// SEAMS D-25: SndResolveStageB holds the ceiling to [1, 256] and the rays to [32, 1048576]
	assert( settings.bounces >= 1 && settings.bounces <= 256 && settings.rays >= 32 );

	// R-SB2-30, the start line
	common->Printf( "bakeAcoustics: %s %s %i rays ceiling %i %.2f s, %i probes, %i tris, %i threads\n", mapName, ( settings.preset == 1 ) ? "fast" : "final",
					settings.rays, settings.bounces, settings.duration, mesh.numProbes, mesh.numTris, numThreads );

	// R-SB2-7: the loader checks the patch level; its error is the reason
	if( !PhononApi_Load( run.api, reason ) )
	{
		return AcousticBake_Fail( mapName, reason.c_str() );
	}
	phononApi_t& api = run.api;
	IPLerror status;

	// R-SB2-10 step 1: the context
	IPLContextSettings contextSettings;
	memset( &contextSettings, 0, sizeof( contextSettings ) );
	contextSettings.version = STEAMAUDIO_VERSION;
	contextSettings.logCallback = AcousticBake_Log;
	contextSettings.allocateCallback = NULL;
	contextSettings.freeCallback = NULL;
	contextSettings.simdLevel = IPL_SIMDLEVEL_SSE2;		// R-SB2-10, section 5.8: one SIMD path on every machine
	contextSettings.flags = ( IPLContextFlags )0;
	status = api.ContextCreate( &contextSettings, &run.context );
	if( status != IPL_STATUS_SUCCESS )
	{
		return AcousticBake_LibraryFail( mapName, "iplContextCreate", status );
	}

	// step 2: the scene, the built-in tracer (R-SB0-11)
	IPLSceneSettings sceneSettings;
	memset( &sceneSettings, 0, sizeof( sceneSettings ) );
	sceneSettings.type = IPL_SCENETYPE_DEFAULT;
	status = api.SceneCreate( run.context, &sceneSettings, &run.scene );
	if( status != IPL_STATUS_SUCCESS )
	{
		return AcousticBake_LibraryFail( mapName, "iplSceneCreate", status );
	}

	// step 3: one static mesh, world and plug triangles, from the checked view (indices and materials in range,
	// R-SB2-5); the library refuses a mesh with no triangle or no material (api_scene.cpp:136)
	run.vertices = ( IPLVector3* )Mem_Alloc( ( size_t )Max( mesh.numVerts, 1 ) * sizeof( IPLVector3 ), TAG_TOOLS );
	run.triangles = ( IPLTriangle* )Mem_Alloc( ( size_t )Max( mesh.numTris, 1 ) * sizeof( IPLTriangle ), TAG_TOOLS );
	run.materialIndices = ( IPLint32* )Mem_Alloc( ( size_t )Max( mesh.numTris, 1 ) * sizeof( IPLint32 ), TAG_TOOLS );
	run.materials = ( IPLMaterial* )Mem_Alloc( ( size_t )Max( mesh.numMaterials, 1 ) * sizeof( IPLMaterial ), TAG_TOOLS );
	for( int i = 0; i < mesh.numVerts; i++ )
	{
		run.vertices[i] = AcousticBake_ToSteamAudio( mesh.verts[i].xyz );
	}
	for( int i = 0; i < mesh.numTris; i++ )
	{
		IPLTriangle& tri = run.triangles[i];
		memset( &tri, 0, sizeof( tri ) );
		tri.indices[0] = ( IPLint32 )mesh.tris[i].v[0];
		tri.indices[1] = ( IPLint32 )mesh.tris[i].v[1];
		tri.indices[2] = ( IPLint32 )mesh.tris[i].v[2];
		run.materialIndices[i] = ( IPLint32 )mesh.triMats[i].material;
	}
	for( int i = 0; i < mesh.numMaterials; i++ )
	{
		IPLMaterial& material = run.materials[i];
		memset( &material, 0, sizeof( material ) );
		for( int b = 0; b < SND_NUM_BANDS; b++ )
		{
			material.absorption[b] = mesh.materials[i].absorption[b];
			material.transmission[b] = mesh.materials[i].transmission[b];
		}
		material.scattering = mesh.materials[i].scattering;
	}
	IPLStaticMeshSettings meshSettings;
	memset( &meshSettings, 0, sizeof( meshSettings ) );
	meshSettings.numVertices = mesh.numVerts;
	meshSettings.numTriangles = mesh.numTris;
	meshSettings.numMaterials = mesh.numMaterials;
	meshSettings.vertices = run.vertices;
	meshSettings.triangles = run.triangles;
	meshSettings.materialIndices = run.materialIndices;
	meshSettings.materials = run.materials;
	status = api.StaticMeshCreate( run.scene, &meshSettings, &run.staticMesh );
	if( status != IPL_STATUS_SUCCESS )
	{
		return AcousticBake_LibraryFail( mapName, "iplStaticMeshCreate", status );
	}
	// the library has copied them (static_mesh.cpp:32-40)
	Mem_Free( run.vertices );
	Mem_Free( run.triangles );
	Mem_Free( run.materialIndices );
	Mem_Free( run.materials );
	run.vertices = NULL;
	run.triangles = NULL;
	run.materialIndices = NULL;
	run.materials = NULL;
	api.StaticMeshAdd( run.staticMesh, run.scene );
	api.SceneCommit( run.scene );

	// step 4: the probes in MSH_PROBES order, batch index = file index
	status = api.ProbeBatchCreate( run.context, &run.probeBatch );
	if( status != IPL_STATUS_SUCCESS )
	{
		return AcousticBake_LibraryFail( mapName, "iplProbeBatchCreate", status );
	}
	for( int p = 0; p < mesh.numProbes; p++ )
	{
		IPLSphere sphere;
		memset( &sphere, 0, sizeof( sphere ) );
		sphere.center = AcousticBake_ToSteamAudio( mesh.probes[p].position );
		sphere.radius = mesh.probes[p].radius * DOOM_TO_METERS;
		api.ProbeBatchAddProbe( run.probeBatch, sphere );
	}
	api.ProbeBatchCommit( run.probeBatch );

	run.numProbes = mesh.numProbes;
	run.stats = ( IPLBakeProbeStats* )Mem_ClearedAlloc( ( size_t )Max( mesh.numProbes, 1 ) * sizeof( IPLBakeProbeStats ), TAG_TOOLS );
	run.probeFlags = ( uint32* )Mem_ClearedAlloc( ( size_t )Max( mesh.numProbes, 1 ) * sizeof( uint32 ), TAG_TOOLS );

	// step 5: the bake. The identifier is reused by step 7
	IPLBakedDataIdentifier identifier;
	memset( &identifier, 0, sizeof( identifier ) );
	identifier.type = IPL_BAKEDDATATYPE_REFLECTIONS;
	identifier.variation = IPL_BAKEDDATAVARIATION_REVERB;

	IPLReflectionsBakeParams bakeParams;
	memset( &bakeParams, 0, sizeof( bakeParams ) );
	bakeParams.scene = run.scene;
	bakeParams.probeBatch = run.probeBatch;
	bakeParams.sceneType = IPL_SCENETYPE_DEFAULT;
	bakeParams.identifier = identifier;
	bakeParams.bakeFlags = IPL_REFLECTIONSBAKEFLAGS_BAKECONVOLUTION;	// not PARAMETRIC (O-SB2-2)
	bakeParams.numRays = settings.rays;
	bakeParams.numDiffuseSamples = settings.diffuseSamples;
	bakeParams.numBounces = settings.bounces;
	bakeParams.simulatedDuration = settings.duration;
	bakeParams.savedDuration = settings.duration;
	bakeParams.order = 1;
	bakeParams.numThreads = numThreads;
	bakeParams.rayBatchSize = 1;
	bakeParams.irradianceMinDistance = settings.irradianceMinDistance;		// R-SB2-11, resolved by SB2-I
	bakeParams.bakeBatchSize = 1;
	bakeParams.openCLDevice = NULL;
	bakeParams.radeonRaysDevice = NULL;

	run.bakeStartMs = Sys_Milliseconds();
	run.lineMs = run.bakeStartMs;
	api.ReflectionsBakerBake( run.context, &bakeParams, AcousticBake_Progress, &run );
	AcousticBake_FlushLog();

	// step 6: a cancel first; nothing is written and the old file stays
	if( run.cancelled )
	{
		common->Printf( "bakeAcoustics: %s cancelled at %i/%i; old .sndbake kept\n", mapName, run.probesDone, mesh.numProbes );
		return false;
	}
	// every probe in the file is baked (R-SB2-19): the library calls back once per probe it finished
	if( run.probesDone != mesh.numProbes )
	{
		reason.Format( "the library baked %i of %i probes", run.probesDone, mesh.numProbes );
		return AcousticBake_Fail( mapName, reason.c_str() );
	}

	// R-SB2-12 to R-SB2-14, the leak gate: per area in area order (its probes are a contiguous range, R-SB2-5)
	bool leak = false;
	for( int a = 0; a < mesh.numAreas; a++ )
	{
		const sndMeshArea_t& area = mesh.areas[a];
		uint64 escaped = 0;
		int worst = -1;		// the probe with the most escaped rays, the lowest index on a tie
		for( int p = area.firstProbe; p < area.firstProbe + area.probeCount; p++ )
		{
			const IPLBakeProbeStats& stats = run.stats[p];
			escaped += stats.escapedRays;
			if( stats.escapedRays > 0 )
			{
				run.probeFlags[p] |= ACOUSTIC_PROBE_ESCAPED;
				if( worst == -1 || stats.escapedRays > run.stats[worst].escapedRays )
				{
					worst = p;
				}
			}
		}
		if( escaped == 0 )
		{
			continue;
		}
		const uint64 cast = ( uint64 )settings.rays * ( uint64 )area.probeCount;
		const double limit = Max( 8.0, ( double )ACOUSTIC_LEAK_FAIL * ( double )cast );
		if( ( double )escaped > limit )
		{
			leak = true;
		}
		// R-SB2-13: the worst probe's lowest-key escape, back in engine units (the inverse of R-SB2-8)
		const IPLBakeProbeStats& stats = run.stats[worst];
		common->Printf( "bakeAcoustics: WARNING area %i: %llu of %llu rays escaped (%.1e); first from ( %.0f %.0f %.0f ) toward ( %.2f %.2f %.2f )\n", a,
						( unsigned long long )escaped, ( unsigned long long )cast, ( double )escaped / ( double )cast,
						-stats.escapeOrigin.z * METERS_TO_DOOM, -stats.escapeOrigin.x * METERS_TO_DOOM, stats.escapeOrigin.y * METERS_TO_DOOM,
						-stats.escapeDirection.z, -stats.escapeDirection.x, stats.escapeDirection.y );
	}
	if( leak )
	{
		// R-SB2-14: no earlier bake of a cracked mesh stays usable
		// B5-3: RemoveFile reports nothing; a read-only, locked or shadowed old file is named in the FAILED line
		const idStr oldBake = va( "maps/%s.sndbake", mapName );
		fileSystem->RemoveFile( oldBake.c_str() );
		return AcousticBake_Fail( mapName, AcousticBake_Gone( oldBake.c_str() ) ? "leak" : "leak; the old .sndbake could not be removed" );
	}

	// step 7: one energy field; each probe's field in index order, channel 0 of the three bands, the bin count the
	// library's. Then the estimator's calls in SEAMS D-38's order
	IPLEnergyFieldSettings fieldSettings;
	memset( &fieldSettings, 0, sizeof( fieldSettings ) );
	fieldSettings.duration = settings.duration;
	fieldSettings.order = 1;
	status = api.EnergyFieldCreate( run.context, &fieldSettings, &run.energyField );
	if( status != IPL_STATUS_SUCCESS )
	{
		return AcousticBake_LibraryFail( mapName, "iplEnergyFieldCreate", status );
	}
	const int numBins = api.EnergyFieldGetNumBins( run.energyField );

	run.histogram = ( float* )Mem_ClearedAlloc( ( size_t )SND_NUM_BANDS * Max( numBins, 1 ) * sizeof( float ), TAG_TOOLS );
	run.probeReverb = ( sndReverb_t* )Mem_ClearedAlloc( ( size_t )Max( mesh.numProbes, 1 ) * sizeof( sndReverb_t ), TAG_TOOLS );
	AcousticAreaSums_Begin( run.sums, mesh.numAreas, numBins );
	run.sumsBegun = true;
	for( int p = 0; p < mesh.numProbes; p++ )
	{
		api.ProbeBatchGetEnergyField( run.probeBatch, &identifier, p, run.energyField );
		for( int b = 0; b < SND_NUM_BANDS && numBins > 0; b++ )
		{
			const IPLfloat32* band = api.EnergyFieldGetBand( run.energyField, 0, b );
			if( band == NULL )
			{
				reason.Format( "probe %i: no energy field band %i", p, b );
				return AcousticBake_Fail( mapName, reason.c_str() );
			}
			memcpy( run.histogram + ( size_t )b * numBins, band, ( size_t )numBins * sizeof( float ) );
		}
		const IPLBakeProbeStats& stats = run.stats[p];
		run.probeFlags[p] |= AcousticEstimateProbe( run.histogram, numBins, stats.segments, stats.segmentMm, stats.ceilingPathMm, run.probeReverb[p] );
		// the probe's area is checked in range (R-SB2-5, SEAMS D-38)
		AcousticAreaSums_AddProbe( run.sums, mesh.probes[p].area, run.histogram, stats.segments, stats.segmentMm, stats.ceilingPathMm );
	}

	// step 8: area defaults, zones, zone records (R-SB2-20 to R-SB2-22)
	run.areaReverb = ( sndReverb_t* )Mem_ClearedAlloc( ( size_t )mesh.numAreas * sizeof( sndReverb_t ), TAG_TOOLS );
	run.areaZone = ( int* )Mem_ClearedAlloc( ( size_t )mesh.numAreas * sizeof( int ), TAG_TOOLS );
	AcousticAreaDefaults( run.sums, run.areaReverb );
	const int numZones = AcousticBuildZones( run.sums, run.areaReverb, mesh.portals, mesh.numPortals, run.areaZone );
	// SEAMS D-38: the estimator only asserts; a zone outside the count fails here, before the records are read
	if( numZones < 1 )
	{
		reason.Format( "%i zones for %i areas", numZones, mesh.numAreas );
		return AcousticBake_Fail( mapName, reason.c_str() );
	}
	for( int a = 0; a < mesh.numAreas; a++ )
	{
		if( run.areaZone[a] < 0 || run.areaZone[a] >= numZones )
		{
			reason.Format( "area %i: zone %i, %i zones", a, run.areaZone[a], numZones );
			return AcousticBake_Fail( mapName, reason.c_str() );
		}
	}
	run.zoneReverb = ( sndReverb_t* )Mem_ClearedAlloc( ( size_t )numZones * sizeof( sndReverb_t ), TAG_TOOLS );
	idList<int> isolatedAreas;
	AcousticZoneRecords( run.sums, run.areaZone, numZones, run.zoneReverb, run.areaReverb, isolatedAreas );
	if( isolatedAreas.Num() > 0 )
	{
		idStr list;
		for( int i = 0; i < isolatedAreas.Num(); i++ )
		{
			if( i > 0 )
			{
				list += " ";
			}
			list += va( "%i", isolatedAreas[i] );
		}
		common->Warning( "bakeAcoustics: %s: %i areas with no probe and no zone partner take the empty record: %s", mapName, isolatedAreas.Num(), list.c_str() );
	}
	AcousticAreaSums_Free( run.sums );
	run.sumsBegun = false;

	// the end line's counts (R-SB2-19, R-SB2-30)
	int numEmpty = 0, numBit3 = 0, numBit4 = 0, numBit5 = 0;
	// DR-152: the ceiling rays summed in 64 bits (at most 2^20 rays x 2^32 probes)
	uint64 ceilingRays = 0;
	for( int p = 0; p < mesh.numProbes; p++ )
	{
		ceilingRays += run.stats[p].ceilingRays;
		numEmpty += ( run.probeFlags[p] & ACOUSTIC_PROBE_EMPTY ) ? 1 : 0;
		numBit3 += ( run.probeFlags[p] & ACOUSTIC_PROBE_CEILING_CUT ) ? 1 : 0;
		numBit4 += ( run.probeFlags[p] & ACOUSTIC_PROBE_TWO_POINT ) ? 1 : 0;
		numBit5 += ( run.probeFlags[p] & ACOUSTIC_PROBE_SPARSE ) ? 1 : 0;
	}

	// R-SB2-24: BAK_STRINGS is "" then each door's name, zero-terminated, in door order, no merging (as the restamp
	// builds it, R-SB1b-27); the names are in MSH_STRINGS, every offset checked inside it and terminated (R-SB2-5)
	int64 stringBytes = 1;
	for( int d = 0; d < mesh.numDoors; d++ )
	{
		stringBytes += idStr::Length( mesh.strings + mesh.doors[d].nameOffset ) + 1;
	}
	const int devSize = SndFile_DevCheckSize( mesh.numEntities, mesh.numMaterials, mesh.stringsSize );

	// the file's size before the builder starts: past the format's limit the builder raises an Error (as the restamp)
	int64 fileSize = SND_HEADER_SIZE + ( int64 )ACOUSTIC_BAKE_SECTIONS * SND_SECTION_SIZE;
	const int64 sectionBytes[ACOUSTIC_BAKE_SECTIONS] =
	{
		( int64 )mesh.numAreas * SND_SIZE_BAKE_AREA, ( int64 )mesh.numCells * SND_SIZE_CELL, ( int64 )mesh.numProbes * SND_SIZE_BAKE_PROBE,
		( int64 )numZones * SND_SIZE_REVERB, ( int64 )mesh.numDoors * SND_SIZE_BAKE_DOOR, stringBytes, ( int64 )devSize
	};
	for( int s = 0; s < ACOUSTIC_BAKE_SECTIONS; s++ )
	{
		fileSize += ( sectionBytes[s] + 15 ) & ~( int64 )15;
	}
	if( devSize <= 0 || fileSize > SND_FILE_MAX_SIZE )
	{
		common->Printf( "bakeAcoustics: %s the .sndbake would be %lld bytes, over the format's limit of %lld\n", mapName, fileSize, SND_FILE_MAX_SIZE );
		return AcousticBake_Fail( mapName, "write" );
	}

	// the sections in id order through the builder, which zeroes every section before it is filled (R-SB1b-22); a
	// pointer it returns is used before the next add, which may move the buffer
	sndFileBuilder_t& b = run.builder;
	SndFile_Begin( b, SND_MAGIC_BAKE, ACOUSTIC_BAKE_SECTIONS );

	sndBakeArea_t* bakeAreas = ( sndBakeArea_t* )SndFile_AddSection( b, BAK_AREAS, SND_SIZE_BAKE_AREA, ( uint32 )mesh.numAreas );
	for( int a = 0; a < mesh.numAreas; a++ )
	{
		const sndMeshArea_t& area = mesh.areas[a];
		sndBakeArea_t& record = bakeAreas[a];
		record.zone = run.areaZone[a];
		for( int k = 0; k < 3; k++ )
		{
			record.cellMin[k] = area.cellMin[k];
			record.dims[k] = area.dims[k];
		}
		record.firstCell = area.firstCell;
		record.firstProbe = area.firstProbe;
		record.probeCount = area.probeCount;
		record.defaultReverb = run.areaReverb[a];
	}

	if( mesh.numCells > 0 )
	{
		memcpy( SndFile_AddSection( b, BAK_CELLS, SND_SIZE_CELL, ( uint32 )mesh.numCells ), mesh.cells, ( size_t )mesh.numCells * SND_SIZE_CELL );
	}
	else
	{
		SndFile_AddSection( b, BAK_CELLS, SND_SIZE_CELL, 0 );
	}

	sndBakeProbe_t* bakeProbes = ( sndBakeProbe_t* )SndFile_AddSection( b, BAK_PROBES, SND_SIZE_BAKE_PROBE, ( uint32 )mesh.numProbes );
	for( int p = 0; p < mesh.numProbes; p++ )
	{
		const sndMeshProbe_t& probe = mesh.probes[p];
		sndBakeProbe_t& record = bakeProbes[p];
		record.position[0] = probe.position[0];
		record.position[1] = probe.position[1];
		record.position[2] = probe.position[2];
		record.radius = probe.radius;
		record.area = probe.area;
		record.flags = run.probeFlags[p];
		record.reverb = run.probeReverb[p];
	}

	memcpy( SndFile_AddSection( b, BAK_ZONES, SND_SIZE_REVERB, ( uint32 )numZones ), run.zoneReverb, ( size_t )numZones * SND_SIZE_REVERB );

	// R-SB2-23: portal, both areas and the name's place copied beside the loss of the door's plug material
	sndBakeDoor_t* bakeDoors = ( sndBakeDoor_t* )SndFile_AddSection( b, BAK_DOORS, SND_SIZE_BAKE_DOOR, ( uint32 )mesh.numDoors );
	uint32 nameOffset = 1;
	for( int d = 0; d < mesh.numDoors; d++ )
	{
		const sndMeshDoor_t& door = mesh.doors[d];
		sndBakeDoor_t& record = bakeDoors[d];
		record.portal = door.portal;
		record.area0 = door.area0;
		record.area1 = door.area1;
		AcousticDoorLoss( mesh.materials[door.material].transmission, record.lossDb );
		record.nameOffset = nameOffset;
		nameOffset += ( uint32 )idStr::Length( mesh.strings + door.nameOffset ) + 1;
	}

	char* bakeStrings = ( char* )SndFile_AddBlob( b, BAK_STRINGS, ( uint32 )stringBytes );
	int at = 1;		// byte 0 is "", zeroed by the builder
	for( int d = 0; d < mesh.numDoors; d++ )
	{
		const char* name = mesh.strings + mesh.doors[d].nameOffset;
		const int length = idStr::Length( name ) + 1;
		memcpy( bakeStrings + at, name, length );
		at += length;
	}
	SndFile_EndStep1( b );

	// SEAMS D-42, D-44: the mesh's entity and material records and strings and its scriptCRC, through the one builder
	SndFile_BuildDevCheck( ( byte* )SndFile_AddBlob( b, BAK_DEVCHECK, ( uint32 )devSize ), mesh.entities, mesh.numEntities, mesh.materials, mesh.numMaterials,
						   mesh.strings, mesh.stringsSize, mesh.settings->scriptCRC );

	// the header: magic, version, numSections, step1Bytes, payloadSize, payloadCRC are the builder's
	sndFileHeader_t& header = *b.header;
	const sndFileHeader_t& meshHeader = *mesh.header;
	header.compileId = meshHeader.compileId;
	header.mapGeometryCRC = meshHeader.mapGeometryCRC;
	header.bakerRevision = ( ( uint32 )SND_BAKER_REVISION_B << 16 ) | meshHeader.bakerRevision;
	header.toolVersion = SND_TOOL_VERSION;
	header.numBands = SND_NUM_BANDS;
	header.entityKeysCRC = meshHeader.entityKeysCRC;
	header.materialHash = meshHeader.materialHash;
	header.inputHash = SndBakeInputHash( meshHeader.inputHash, settings );
	header.settings = settings;

	// B5-4 (SEAMS D-42): the game loader's own test of the step 1 sections, on the bytes about to be written, so a bake
	// the game would call stale is never written (more than 32,768 doors; an area's dims the loader refuses). Its
	// conditions hold here: SndFile_Begin set numSections (7), SndFile_EndStep1 set step1Bytes past the directory and
	// inside b.size; it reads neither payloadSize nor payloadCRC, which SndFile_Finish fills
	sndBakeStep1_t step1;
	memset( &step1, 0, sizeof( step1 ) );
	const uint32 unsound = SndBakeStep1( b.data, ( int )b.header->step1Bytes, b.size, step1 );
	if( unsound != 0 )
	{
		reason.Format( "the bake's own section 0x%x fails the game loader's step 1 test (SndBakeStep1); nothing written", unsound );
		return AcousticBake_Fail( mapName, reason.c_str() );
	}

	// the builder writes <path>.tmp, flushes, renames; on failure it removes the .tmp and the old file stays
	const idStr bakePath = va( "maps/%s.sndbake", mapName );
	if( !SndFile_Finish( b, bakePath.c_str() ) )
	{
		return AcousticBake_Fail( mapName, "write" );
	}

	common->Printf( "bakeAcoustics: %s wrote %s (%i bytes, %i zones, %i empty, bits 3/4/5 %i/%i/%i, %llu ceiling rays, %.1f s)\n", mapName, bakePath.c_str(), b.size, numZones,
					numEmpty, numBit3, numBit4, numBit5, ( unsigned long long )ceilingRays, ( Sys_Milliseconds() - run.startMs ) * 0.001f );
	return true;
	//puncho end
}

/*
==================
AcousticBake_Fail

The FAILED line (R-SB2-30), also as a warning.
==================
*/
static bool AcousticBake_Fail( const char* map, const char* reason )
{
	common->Printf( "bakeAcoustics: %s FAILED: %s\n", map, reason );
	common->Warning( "bakeAcoustics: %s FAILED: %s", map, reason );
	return false;
}

// owns the re-entry flag and the mesh view for one run: released when the run leaves, by return or by an Error
// unwinding through it (common->Error throws; the pattern of dmap.cpp's idScopedDmapEditorFlag, I4-2). Only those
// two: whatever the bake step allocates needs its own release on an Error
class idScopedAcousticBake
{
public:
	idScopedAcousticBake( acousticBakeMesh_t& mesh_ ) : mesh( mesh_ )
	{
		memset( &mesh, 0, sizeof( mesh ) );
		acousticBakeRunning = true;
	}
	~idScopedAcousticBake()
	{
		AcousticBake_FreeMesh( mesh );
		acousticBakeRunning = false;
	}
private:
	acousticBakeMesh_t&	mesh;
};

/*
==================
BakeAcoustics

Stage B (K-SB9). Called by dmap after RunAAS with what dmap passes, "maps/<map>" with no extension (testmap <map>
bake goes through dmap), and by the bakeAcoustics command with the same form. Only a leading "maps/" is stripped
here: the rest, dots included, is the name dmap wrote its files under (I4-7).
Order: re-entry, read and check, settings, skip, then the bake step. No library and no write before the last.
==================
*/
bool BakeAcoustics( const char* mapName, bool fast )
{
	idStr map = mapName;
	map.BackSlashesToSlashes();
	if( map.Icmpn( "maps/", 5 ) == 0 )
	{
		map = map.Right( map.Length() - 5 );
	}

	fileSystem->RemoveFile( va( "maps/%s.sndbake.tmp", map.c_str() ) );	//puncho - SB2-B (R-SB2-25): a kill can leave one

	// refused before the scope object exists, so the owning run's flag stays set
	if( acousticBakeRunning )
	{
		return AcousticBake_Fail( map.c_str(), "a bake is already running" );
	}

	acousticBakeMesh_t mesh;
	idScopedAcousticBake scope( mesh );

	idStr reason;
	if( !AcousticBake_ReadMesh( map.c_str(), mesh, reason ) )
	{
		return AcousticBake_Fail( map.c_str(), reason.c_str() );
	}

	sndSettings_t settings;
	AcousticBake_ResolveSettings( mesh, fast, settings );
	// I4-4: the game refuses a bake whose S or Sz is outside its range
	if( !SndSettingsValid( settings ) )
	{
		reason.Format( "settings the game would refuse: probeSpacing %i, probeSpacingZ %i, each must be in [32, 1024]", settings.probeSpacing, settings.probeSpacingZ );
		return AcousticBake_Fail( map.c_str(), reason.c_str() );
	}

	// I4-12b: a fast request is also met by a bake made for final, never the other way round
	sndSettings_t finalSettings;
	if( fast )
	{
		AcousticBake_ResolveSettings( mesh, false, finalSettings );
	}
	const acousticSkip_t skip = AcousticBake_UpToDate( map.c_str(), mesh, settings, fast ? &finalSettings : NULL );
	if( skip == ACOUSTIC_SKIP_ASKED )
	{
		common->Printf( "bakeAcoustics: %s up to date\n", map.c_str() );
		return true;
	}
	if( skip == ACOUSTIC_SKIP_FINAL )
	{
		common->Printf( "bakeAcoustics: %s up to date (a final bake)\n", map.c_str() );
		return true;
	}

	return AcousticBake_Run( map.c_str(), mesh, settings );
}

#else // !USE_STEAMAUDIO_BAKE

/*
==================
BakeAcoustics

Built without the acoustic bake (R-SB2-2, R-SB0-19).
==================
*/
bool BakeAcoustics( const char*, bool )
{
	common->Printf( "bakeAcoustics: FAILED: built without the acoustic bake\n" );
	return false;
}

#endif // USE_STEAMAUDIO_BAKE

/*
==================
BakeAcoustics_f

bakeAcoustics [fast] <map> (R-SB2-1): beside Dmap_f's pattern, refresh on print for the run. It needs no loaded map
and does not disconnect. The name is put in the form dmap passes to BakeAcoustics, "maps/<map>": one trailing
".map", ".json", ".gltf" or ".glb" is stripped (any other dot is part of the name, I4-7, I4R-1) and "maps/" is added
when absent; BakeAcoustics then strips that "maps/" once. "bakeAcoustics fast" alone is a map named fast.
==================
*/
void BakeAcoustics_f( const idCmdArgs& args )
{
	bool fast = false;
	const char* mapName = NULL;
	if( args.Argc() == 2 )
	{
		mapName = args.Argv( 1 );
	}
	else if( args.Argc() == 3 && idStr::Icmp( args.Argv( 1 ), "fast" ) == 0 )
	{
		fast = true;
		mapName = args.Argv( 2 );
	}
	idStr map = ( mapName != NULL ) ? mapName : "";
	map.BackSlashesToSlashes();
	// one trailing extension of a map the completion offers, compared without case (I4R-1); any other dot stays
	static const char* const mapExtensions[] = { ".map", ".json", ".gltf", ".glb" };
	for( int e = 0; e < ( int )( sizeof( mapExtensions ) / sizeof( mapExtensions[0] ) ); e++ )
	{
		const int extLength = idStr::Length( mapExtensions[e] );
		// >=: a name that is nothing but the extension strips to empty and prints the usage line (I4S-3)
		if( map.Length() >= extLength && map.Right( extLength ).Icmp( mapExtensions[e] ) == 0 )
		{
			map = map.Left( map.Length() - extLength );
			break;
		}
	}
	if( map.Icmpn( "maps/", 5 ) != 0 )
	{
		map = "maps/" + map;
	}
	// nothing left after "maps/" (an empty argument, or "maps/" alone): the usage line (I4R-5)
	if( map.Length() <= 5 )
	{
		common->Printf( "usage: bakeAcoustics [fast] <map>\n" );
		return;
	}

	common->ClearWarnings( "running bakeAcoustics" );
	common->SetRefreshOnPrint( true );
	BakeAcoustics( map.c_str(), fast );
	common->SetRefreshOnPrint( false );
	common->PrintWarnings();
}

#endif // !DMAP
