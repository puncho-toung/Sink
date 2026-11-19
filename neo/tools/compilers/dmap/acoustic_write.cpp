//puncho - SB1b (R-SB1b-19, R-SB1b-20, R-SB1b-23, R-SB1b-27): the end of dmap's Stage A. Validates the mesh SB1a
// hands over, places the probes (acoustic_probes.cpp), writes maps/<map>.sndmesh through the file builder, then
// keeps, restamps or removes an old maps/<map>.sndbake. Runs in dmap, dmap noAcousticBake, testmap and rbdmap alike,
// before any bake and before the manifest. Linked in the engine and in rbdmap: idLib, common, fileSystem and dmap only.
#include "precompiled.h"
#pragma hdrstop

#include "acoustic.h"
#include "CompilerPhase.h"

// acoustic_probes.cpp (SB1b-P): R-SB1b-7 to R-SB1b-15. Fills settings' probeSpacing, probeSpacingZ, probeClearance
// and the MSH_AREAS, MSH_CELLS and MSH_PROBES records; false: Stage A fails, a warning named the cause
bool AcousticProbes( const acousticMesh_t& mesh, sndMeshSettings_t& settings, idList<sndMeshArea_t>& areas, idList<int32>& cells, idList<sndMeshProbe_t>& probes );

static const int	ACOUSTIC_MESH_SECTIONS		= 12;

// R-SB1b-27: the step 1 sections of a .sndbake and BAK_DEVCHECK, first and in this order (R-SB1b-16)
static const int	ACOUSTIC_BAKE_STEP1			= 7;
static const uint32	acousticBakeIds[ACOUSTIC_BAKE_STEP1]	= { BAK_AREAS, BAK_CELLS, BAK_PROBES, BAK_ZONES, BAK_DOORS, BAK_STRINGS, BAK_DEVCHECK };
static const uint32	acousticBakeSizes[ACOUSTIC_BAKE_STEP1]	= { SND_SIZE_BAKE_AREA, SND_SIZE_CELL, SND_SIZE_BAKE_PROBE, SND_SIZE_REVERB, SND_SIZE_BAKE_DOOR, 0, 0 };
static const char*	acousticBakeNames[ACOUSTIC_BAKE_STEP1]	= { "BAK_AREAS", "BAK_CELLS", "BAK_PROBES", "BAK_ZONES", "BAK_DOORS", "BAK_STRINGS", "BAK_DEVCHECK" };
static const int	ACOUSTIC_BAKE_DOORS			= 4;	// index in the lists above
static const int	ACOUSTIC_BAKE_STRINGS		= 5;
static const int	ACOUSTIC_BAKE_DEVCHECK		= 6;

// the records of MSH_MATERIALS, MSH_DOORS, MSH_ENTITIES and their string table, built before the validation that
// checks the two CRCs over them, then written as they are
struct acousticMeshRecords_t
{
	idList<char>			strings;		// MSH_STRINGS
	idList<sndMaterial_t>	materials;
	idList<sndMeshDoor_t>	doors;
	idList<sndDevEntity_t>	entities;
};

/*
==================
AcousticWrite_MapName

<map> of the console lines: the map name as dmap is given it, without "maps/".
==================
*/
static const char* AcousticWrite_MapName()
{
	const char* base = dmapGlobals.mapFileBase;
	return ( idStr::Icmpn( base, "maps/", 5 ) == 0 ) ? base + 5 : base;
}

/*
==================
AcousticWrite_Fail

R-SB1b-19: a failure before the .sndmesh is written removes the .sndmesh and the .sndbake: a bake of a mesh that
could not be written cannot be judged.
==================
*/
static bool AcousticWrite_Fail()
{
	fileSystem->RemoveFile( va( "%s.sndmesh", dmapGlobals.mapFileBase ) );
	fileSystem->RemoveFile( va( "%s.sndbake", dmapGlobals.mapFileBase ) );
	return false;
}

// "" at offset 0, then each name in first reference order, zero-terminated; no merging (R-SB1b-16)
static uint32 AcousticWrite_AddString( idList<char>& strings, const char* name )
{
	const uint32 offset = ( uint32 )strings.Num();
	for( const char* c = name; *c != '\0'; c++ )
	{
		strings.Append( *c );
	}
	strings.Append( '\0' );
	return offset;
}

static void AcousticWrite_BuildRecords( const acousticMesh_t& mesh, acousticMeshRecords_t& records )
{
	// sized first, so the table never grows while it is filled (review F4R-5)
	int stringBytes = 1;
	for( int m = 0; m < mesh.materials.Num(); m++ )
	{
		stringBytes += mesh.materials[m].name.Length() + 1;
	}
	for( int d = 0; d < mesh.doors.Num(); d++ )
	{
		stringBytes += mesh.doors[d].name.Length() + 1;
	}
	for( int e = 0; e < mesh.entities.Num(); e++ )
	{
		stringBytes += mesh.entities[e].name.Length() + 1;
	}
	records.strings.Resize( stringBytes );
	records.strings.Append( '\0' );

	records.materials.SetNum( mesh.materials.Num() );
	for( int m = 0; m < mesh.materials.Num(); m++ )
	{
		const acousticNumbers_t& n = mesh.materials[m].n;
		sndMaterial_t& record = records.materials[m];
		record.nameOffset = AcousticWrite_AddString( records.strings, mesh.materials[m].name.c_str() );
		for( int band = 0; band < SND_NUM_BANDS; band++ )
		{
			record.absorption[band] = n.absorption[band];
			record.transmission[band] = n.transmission[band];
		}
		record.scattering = n.scattering;
	}

	records.doors.SetNum( mesh.doors.Num() );
	for( int d = 0; d < mesh.doors.Num(); d++ )
	{
		const acousticDoor_t& door = mesh.doors[d];
		sndMeshDoor_t& record = records.doors[d];
		record.nameOffset = AcousticWrite_AddString( records.strings, door.name.c_str() );
		record.portal = door.portal;
		record.area0 = door.area0;
		record.area1 = door.area1;
		record.firstPlugTri = door.firstPlugTri;
		record.plugTriCount = door.numPlugTris;
		record.material = door.material;
	}

	records.entities.SetNum( mesh.entities.Num() );
	for( int e = 0; e < mesh.entities.Num(); e++ )
	{
		sndDevEntity_t& record = records.entities[e];
		record.nameOffset = AcousticWrite_AddString( records.strings, mesh.entities[e].name.c_str() );
		record.kind = mesh.entities[e].kind;
		record.crc = mesh.entities[e].crc;
	}
}

/*
==================
AcousticWrite_Validate

R-SB1b-19 step 1 (R-SB1a-31): the hand-over. A breach is named by its first bad record.
==================
*/
static bool AcousticWrite_Validate( const acousticMesh_t& mesh, const acousticMeshRecords_t& records )
{
	const int numTris = mesh.tris.Num();
	if( mesh.worldTree == NULL || mesh.worldTree->headnode == NULL )
	{
		common->Warning( "acoustics: the mesh has no world tree" );
		return false;
	}
	if( mesh.entities.Num() != mesh.mapFile->GetNumEntities() )
	{
		common->Warning( "acoustics: %i entity records for %i map entities", mesh.entities.Num(), mesh.mapFile->GetNumEntities() );
		return false;
	}
	for( int e = 0; e < mesh.entities.Num(); e++ )
	{
		if( mesh.entities[e].entity != e )
		{
			common->Warning( "acoustics: entity record %i names entity %i", e, mesh.entities[e].entity );
			return false;
		}
	}
	if( mesh.materials.Num() > 65535 )
	{
		common->Warning( "acoustics: %i materials, more than 65,535", mesh.materials.Num() );
		return false;
	}
	if( mesh.numWorldTris < 0 || mesh.numWorldTris > numTris )
	{
		common->Warning( "acoustics: numWorldTris %i outside [0, %i]", mesh.numWorldTris, numTris );
		return false;
	}
	for( int t = 0; t < numTris; t++ )
	{
		const acousticTri_t& tri = mesh.tris[t];
		for( int k = 0; k < 3; k++ )
		{
			if( tri.v[k] < 0 || tri.v[k] >= mesh.verts.Num() )
			{
				common->Warning( "acoustics: triangle %i: vertex index %i out of range (%i vertices)", t, tri.v[k], mesh.verts.Num() );
				return false;
			}
		}
		if( tri.material >= mesh.materials.Num() )
		{
			common->Warning( "acoustics: triangle %i: material %i out of range (%i materials)", t, tri.material, mesh.materials.Num() );
			return false;
		}
		if( ( ( tri.flags & SND_TRI_PLUG ) != 0 ) != ( t >= mesh.numWorldTris ) )
		{
			common->Warning( "acoustics: triangle %i: the plugs are not exactly [%i, %i)", t, mesh.numWorldTris, numTris );
			return false;
		}
	}
	for( int b = 0; b < mesh.brushes.Num(); b++ )
	{
		const acousticBrush_t& brush = mesh.brushes[b];
		if( brush.entity < 0 || brush.entity >= mesh.entities.Num() || brush.firstPlane < 0 || brush.numPlanes < 0 ||
				brush.firstPlane + brush.numPlanes > mesh.brushPlanes.Num() )
		{
			common->Warning( "acoustics: brush record %i: entity %i, planes [%i, +%i) outside the lists", b, brush.entity, brush.firstPlane, brush.numPlanes );
			return false;
		}
	}
	if( mesh.portals.Num() != interAreaPortals.Num() )
	{
		common->Warning( "acoustics: %i portal records for %i interAreaPortals", mesh.portals.Num(), interAreaPortals.Num() );
		return false;
	}
	for( int p = 0; p < mesh.portals.Num(); p++ )
	{
		const acousticPortal_t& portal = mesh.portals[p];
		if( portal.area0 < 0 || portal.area0 >= mesh.numAreas || portal.area1 < 0 || portal.area1 >= mesh.numAreas )
		{
			common->Warning( "acoustics: portal %i: areas %i %i out of range (%i areas)", p, portal.area0, portal.area1, mesh.numAreas );
			return false;
		}
		if( portal.door < -1 || portal.door >= mesh.doors.Num() )
		{
			common->Warning( "acoustics: portal %i: door %i out of range", p, portal.door );
			return false;
		}
	}

	// every plug is inside a door's range, and every plug is covered by one
	idList<bool> covered;
	covered.SetNum( numTris - mesh.numWorldTris );
	for( int i = 0; i < covered.Num(); i++ )
	{
		covered[i] = false;
	}
	for( int d = 0; d < mesh.doors.Num(); d++ )
	{
		const acousticDoor_t& door = mesh.doors[d];
		if( door.portal < 0 || door.portal >= mesh.portals.Num() )
		{
			common->Warning( "acoustics: door %i: portal %i out of range", d, door.portal );
			return false;
		}
		if( door.area0 < 0 || door.area0 >= mesh.numAreas || door.area1 < 0 || door.area1 >= mesh.numAreas )
		{
			common->Warning( "acoustics: door %i: areas %i %i out of range (%i areas)", d, door.area0, door.area1, mesh.numAreas );
			return false;
		}
		if( door.material < -1 || door.material >= mesh.materials.Num() )
		{
			common->Warning( "acoustics: door %i: material %i out of range", d, door.material );
			return false;
		}
		if( door.numPlugTris < 0 || ( door.numPlugTris > 0 && ( door.firstPlugTri < mesh.numWorldTris || door.firstPlugTri + door.numPlugTris > numTris ) ) )
		{
			common->Warning( "acoustics: door %i: plug range [%i, +%i) outside the plugs [%i, %i)", d, door.firstPlugTri, door.numPlugTris, mesh.numWorldTris, numTris );
			return false;
		}
		for( int d2 = 0; d2 < d; d2++ )
		{
			const acousticDoor_t& other = mesh.doors[d2];
			if( other.portal == door.portal && ( other.firstPlugTri != door.firstPlugTri || other.numPlugTris != door.numPlugTris ) )
			{
				common->Warning( "acoustics: doors %i and %i on portal %i do not share a range", d2, d, door.portal );
				return false;
			}
		}
		for( int t = 0; t < door.numPlugTris; t++ )
		{
			covered[door.firstPlugTri - mesh.numWorldTris + t] = true;
		}
	}
	for( int i = 0; i < covered.Num(); i++ )
	{
		if( !covered[i] )
		{
			common->Warning( "acoustics: plug triangle %i is in no door's range", mesh.numWorldTris + i );
			return false;
		}
	}

	const uint32 entityKeysCRC = SndEntityKeysCRC( records.entities.Ptr(), records.entities.Num() );
	if( entityKeysCRC != mesh.entityKeysCRC )
	{
		common->Warning( "acoustics: entityKeysCRC %08x, the records give %08x", ( unsigned int )mesh.entityKeysCRC, ( unsigned int )entityKeysCRC );
		return false;
	}
	const uint32 materialHash = SndMaterialHash( records.materials.Ptr(), records.materials.Num(), records.strings.Ptr() );
	if( materialHash != mesh.materialHash )
	{
		common->Warning( "acoustics: materialHash %08x, the records give %08x", ( unsigned int )mesh.materialHash, ( unsigned int )materialHash );
		return false;
	}
	return true;
}

// count records of recordSize bytes, copied as they are; the builder zeroes the space and pads after it
static void AcousticWrite_AddRecords( sndFileBuilder_t& b, uint32 id, uint32 recordSize, const void* records, int count )
{
	void* out = SndFile_AddSection( b, id, recordSize, ( uint32 )count );
	if( count > 0 )
	{
		memcpy( out, records, ( size_t )recordSize * count );
	}
}

// a worldspawn integer as atoi reads it whole: an optional sign, then one or more digits and nothing else
static bool AcousticWrite_IsPlainInteger( const char* text )
{
	const char* c = text + ( ( text[0] == '-' || text[0] == '+' ) ? 1 : 0 );
	if( *c == '\0' )
	{
		return false;
	}
	for( ; *c != '\0'; c++ )
	{
		if( *c < '0' || *c > '9' )
		{
			return false;
		}
	}
	return true;
}

// the name a console line gives a step 1 section id
static const char* AcousticWrite_SectionName( uint32 id )
{
	for( int k = 0; k < ACOUSTIC_BAKE_STEP1; k++ )
	{
		if( acousticBakeIds[k] == id )
		{
			return acousticBakeNames[k];
		}
	}
	return "section";
}

/*
==================
AcousticBakeUnreadable

R-SB1b-27 step 1, for Stage A and any other reader of an old .sndbake (the bake's skip, review I4-9): the field or
section that makes the file unreadable, named in the "removed (unreadable: ...)" line, or NULL when it is sound. The
header, the whole-file CRC, the directory, the six step 1 sections and BAK_DEVCHECK first and in order (anything after
them has a higher id), the string table's leading "", the dev head, step1Bytes; then the game loader's own test of
the step 1 sections (SndBakeStep1: sizes, indices, finite floats, the strings), so a file that passes here passes the
game's test of the step 1 sections (review F4-1). The loader's header tests (toolVersion, numBands, the settings'
ranges) are not run here.
==================
*/
const char* AcousticBakeUnreadable( const byte* data, int length )
{
	if( length < SND_HEADER_SIZE )
	{
		return "header";
	}
	const sndFileHeader_t* header = ( const sndFileHeader_t* )data;
	if( header->magic != SND_MAGIC_BAKE )
	{
		return "magic";
	}
	if( header->version != SND_FORMAT_VERSION )
	{
		return "version";
	}
	if( header->numSections > SND_MAX_SECTIONS )
	{
		return "numSections";
	}
	if( ( int64 )length != SND_HEADER_SIZE + ( int64 )header->payloadSize )
	{
		return "payloadSize";
	}

	// over the whole file with its own four bytes taken as 0 (DR-62)
	if( SndFile_PayloadCRC( data, length ) != header->payloadCRC )
	{
		return "payloadCRC";
	}

	const int numSections = ( int )header->numSections;
	const int64 directoryEnd = SND_HEADER_SIZE + ( int64 )numSections * SND_SECTION_SIZE;
	if( directoryEnd > length )
	{
		return "directory";
	}
	const sndSection_t* directory = ( const sndSection_t* )( data + SND_HEADER_SIZE );
	int64 previousEnd = directoryEnd;
	for( int i = 0; i < numSections; i++ )
	{
		const sndSection_t& section = directory[i];
		if( ( i > 0 && section.id <= directory[i - 1].id ) || ( section.offset % 16 ) != 0 || section.offset < previousEnd ||
				( int64 )section.offset + section.size > length )
		{
			return "directory";
		}
		previousEnd = ( int64 )section.offset + section.size;
	}

	// the step 1 sections and BAK_DEVCHECK in their places; their sizes are SndBakeStep1's test below
	for( int k = 0; k < ACOUSTIC_BAKE_STEP1; k++ )
	{
		if( k >= numSections || directory[k].id != acousticBakeIds[k] )
		{
			return acousticBakeNames[k];
		}
	}
	// the string table starts with ""; the dev section holds at least its head
	const sndSection_t& strings = directory[ACOUSTIC_BAKE_STRINGS];
	if( strings.size < 1 || data[strings.offset] != 0 )
	{
		return acousticBakeNames[ACOUSTIC_BAKE_STRINGS];
	}
	if( directory[ACOUSTIC_BAKE_DEVCHECK].size < SND_SIZE_DEV_HEAD )
	{
		return acousticBakeNames[ACOUSTIC_BAKE_DEVCHECK];
	}
	if( header->step1Bytes != ( ( strings.offset + strings.size + 15 ) & ~15u ) || header->step1Bytes > ( uint32 )length )
	{
		return "step1Bytes";
	}

	// the game loader's test; its conditions (magic, version, numSections, the directory inside step1Bytes,
	// step1Bytes inside the file) hold here
	sndBakeStep1_t step1;
	const uint32 unsound = SndBakeStep1( data, ( int )header->step1Bytes, length, step1 );
	if( unsound != 0 )
	{
		return AcousticWrite_SectionName( unsound );
	}
	return NULL;
}

// BAK_DEVCHECK from the new .sndmesh's records, by the one builder SB2 uses too: the keep test compares it with the
// old section, the restamp writes it
static void AcousticWrite_DevCheck( const acousticMesh_t& mesh, const acousticMeshRecords_t& records, idList<byte>& dev )
{
	dev.SetNum( SndFile_DevCheckSize( records.entities.Num(), records.materials.Num(), records.strings.Num() ) );
	SndFile_BuildDevCheck( dev.Ptr(), records.entities.Ptr(), records.entities.Num(), records.materials.Ptr(), records.materials.Num(),
						   records.strings.Ptr(), records.strings.Num(), mesh.scriptCRC );
}

/*
==================
AcousticWrite_Gone

True when relativePath exists in neither write folder (fs_basepath, fs_savepath: a stat, no open, so a file another
process holds open for writing still counts as there) and opens nowhere else on the search path, packs and layers
included, the place the game loads it from (reviews F4-2, F4R-1). The open does not copy the file (fs_copyfiles).
==================
*/
static bool AcousticWrite_Gone( const char* relativePath )
{
	// an empty fs_savepath (rbdmap's default) would make IsFolder stat a path at the root of the current drive
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

/*
==================
AcousticWrite_Remove

RemoveFile, then the test that the path is gone (RemoveFile reports nothing). False, with a warning naming the
file, when it still opens: the stage fails, as for a failed restamp.
==================
*/
static bool AcousticWrite_Remove( const char* relativePath )
{
	fileSystem->RemoveFile( relativePath );
	if( AcousticWrite_Gone( relativePath ) )
	{
		return true;
	}
	common->Warning( "acoustics: %s could not be removed (read-only, locked or open, or in a pack or a layer above the write folders fs_basepath and fs_savepath)", relativePath );
	return false;
}

enum acousticRestamp_t
{
	ACOUSTIC_RESTAMP_WRITTEN,		// written, and the path opens as the file written
	ACOUSTIC_RESTAMP_FAILED,		// over the size limit, or the builder printed Error writing <path>; the old file is as it was
	ACOUSTIC_RESTAMP_SHADOWED		// written, but the path opens as another file
};

/*
==================
AcousticWrite_Restamp

R-SB1b-27 step 3: a new .sndbake through the builder. The old header with the four stamps of the new .sndmesh; the
areas, cells, probes and zones byte for byte; the doors with their name offsets rewritten into BAK_STRINGS, rebuilt
from the new door names in door order; BAK_DEVCHECK as dev (built from the new .sndmesh); any section above 0x180
copied unchanged (SEAMS D-26). inputHash, settings, toolVersion, numBands and bakerRevision stay. After the rename
the path is read back and must be the bytes written (review F4-2).
==================
*/
static acousticRestamp_t AcousticWrite_Restamp( const acousticMesh_t& mesh, const sndFileHeader_t& meshHeader, const idList<byte>& dev,
		const byte* data, const char* bakePath )
{
	const sndFileHeader_t* old = ( const sndFileHeader_t* )data;
	const sndSection_t* directory = ( const sndSection_t* )( data + SND_HEADER_SIZE );
	const int numSections = ( int )old->numSections;

	// BAK_STRINGS: "" then each door name zero-terminated, in door order, no merging; sized first (review F4-8)
	int stringBytes = 1;
	for( int d = 0; d < mesh.doors.Num(); d++ )
	{
		stringBytes += mesh.doors[d].name.Length() + 1;
	}
	idList<char> strings;
	idList<uint32> nameOffsets;
	strings.Resize( stringBytes );
	strings.Append( '\0' );
	nameOffsets.SetNum( mesh.doors.Num() );
	for( int d = 0; d < mesh.doors.Num(); d++ )
	{
		nameOffsets[d] = AcousticWrite_AddString( strings, mesh.doors[d].name.c_str() );
	}

	// the file's size before the builder starts: past its limit the builder raises an Error (review F4R-5)
	int64 fileSize = SND_HEADER_SIZE + ( int64 )numSections * SND_SECTION_SIZE;
	for( int k = 0; k < numSections; k++ )
	{
		const int64 size = ( k == ACOUSTIC_BAKE_STRINGS ) ? strings.Num() : ( ( k == ACOUSTIC_BAKE_DEVCHECK ) ? dev.Num() : ( int64 )directory[k].size );
		fileSize += ( size + 15 ) & ~( int64 )15;
	}
	if( fileSize > SND_FILE_MAX_SIZE )
	{
		common->Warning( "acoustics: the restamped %s would be %lld bytes, over the format's limit of %lld", bakePath, fileSize, SND_FILE_MAX_SIZE );
		return ACOUSTIC_RESTAMP_FAILED;
	}

	sndFileBuilder_t b;
	SndFile_Begin( b, SND_MAGIC_BAKE, numSections );
	// the old header; the builder rewrites its own fields at Finish, SndFile_EndStep1 sets step1Bytes
	memcpy( b.header, old, SND_HEADER_SIZE );

	for( int k = 0; k < ACOUSTIC_BAKE_STRINGS; k++ )
	{
		const sndSection_t& section = directory[k];
		byte* out = ( byte* )SndFile_AddSection( b, section.id, acousticBakeSizes[k], section.count );
		memcpy( out, data + section.offset, section.size );
		if( k == ACOUSTIC_BAKE_DOORS )
		{
			// the count equals mesh.doors.Num(): step 2 removed the file otherwise
			sndBakeDoor_t* doors = ( sndBakeDoor_t* )out;
			for( int d = 0; d < mesh.doors.Num(); d++ )
			{
				doors[d].nameOffset = nameOffsets[d];
			}
		}
	}
	memcpy( SndFile_AddBlob( b, BAK_STRINGS, ( uint32 )strings.Num() ), strings.Ptr(), strings.Num() );
	SndFile_EndStep1( b );

	memcpy( SndFile_AddBlob( b, BAK_DEVCHECK, ( uint32 )dev.Num() ), dev.Ptr(), dev.Num() );

	// SEAMS D-26: a section the restamp does not know, as it was; a size that is no multiple of its count goes in as
	// bytes and gets its old count written into the directory
	for( int k = ACOUSTIC_BAKE_STEP1; k < numSections; k++ )
	{
		const sndSection_t& section = directory[k];
		byte* out;
		if( section.count != 0 && ( section.size % section.count ) == 0 )
		{
			out = ( byte* )SndFile_AddSection( b, section.id, section.size / section.count, section.count );
		}
		else
		{
			out = ( byte* )SndFile_AddBlob( b, section.id, section.size );
			( ( sndSection_t* )( b.data + SND_HEADER_SIZE ) )[b.nextSection - 1].count = section.count;
		}
		memcpy( out, data + section.offset, section.size );
	}

	b.header->compileId = meshHeader.compileId;
	b.header->mapGeometryCRC = meshHeader.mapGeometryCRC;
	b.header->entityKeysCRC = meshHeader.entityKeysCRC;
	b.header->materialHash = meshHeader.materialHash;

	acousticRestamp_t result = ACOUSTIC_RESTAMP_FAILED;
	if( SndFile_Finish( b, bakePath ) )
	{
		// the path must now open as these bytes, not as a copy under fs_savepath, in a pack or in a layer above
		void* readBack = NULL;
		const int length = fileSystem->ReadFile( bakePath, &readBack );
		const bool same = ( readBack != NULL && length == b.size && memcmp( readBack, b.data, b.size ) == 0 );
		if( readBack != NULL )
		{
			fileSystem->FreeFile( readBack );
		}
		result = same ? ACOUSTIC_RESTAMP_WRITTEN : ACOUSTIC_RESTAMP_SHADOWED;
	}
	SndFile_Free( b );
	return result;
}

/*
==================
AcousticWrite_OldBake

R-SB1b-27: after the .sndmesh is written, before any bake and before the manifest (SEAMS D-38). No .sndbake:
nothing. Else exactly one of: removed (unreadable), removed (stale), restamped, or kept (a line under
dmap_acousticVerbose only). Kept means the four header stamps are equal, every door's old name is its new one
(review F4R-2), and the old BAK_DEVCHECK is byte for byte the one a restamp would write now (review F4-1).
False fails the stage: a restamp that failed (the old file is removed), a file that still opens after its removal,
or a restamp that does not open as the file written (review F4-2). The caller then removes every acoustic output,
the .sndmesh of this compile included (ruled, F4-4).
==================
*/
static bool AcousticWrite_OldBake( const acousticMesh_t& mesh, const sndFileHeader_t& meshHeader, const acousticMeshRecords_t& records,
								   int numCells, int numProbes )
{
	idStr bakePath;
	bakePath.Format( "%s.sndbake", dmapGlobals.mapFileBase );
	const char* mapName = AcousticWrite_MapName();

	void* buffer = NULL;
	const int length = fileSystem->ReadFile( bakePath.c_str(), &buffer );
	if( length < 0 || buffer == NULL )
	{
		if( buffer != NULL )
		{
			fileSystem->FreeFile( buffer );
		}
		// no old bake, or one that exists and does not read (held open by another process, unreadable): removed all
		// the same, and it must be gone, by a stat of both write folders and an open of the search path (F4R-1)
		return AcousticWrite_Remove( bakePath.c_str() );
	}
	const byte* data = ( const byte* )buffer;

	// 1. unreadable
	const char* field = AcousticBakeUnreadable( data, length );
	if( field != NULL )
	{
		fileSystem->FreeFile( buffer );
		if( !AcousticWrite_Remove( bakePath.c_str() ) )
		{
			return false;
		}
		common->Printf( "acoustics: %s.sndbake removed (unreadable: %s)\n", mapName, field );
		return true;
	}

	// step 1 proved every section's place, so the directory is indexed
	const sndFileHeader_t* old = ( const sndFileHeader_t* )data;
	const sndSection_t* directory = ( const sndSection_t* )( data + SND_HEADER_SIZE );
	const sndSection_t& oldDoors = directory[ACOUSTIC_BAKE_DOORS];
	const sndSection_t& oldStrings = directory[ACOUSTIC_BAKE_STRINGS];
	const sndSection_t& oldDev = directory[ACOUSTIC_BAKE_DEVCHECK];

	// 2. needs a new bake: another baker, another input, or a layout that is not the new mesh's
	if( old->bakerRevision != SND_BAKER_REVISION_BAKE )
	{
		field = "bakerRevision";
	}
	else if( SndBakeInputHash( meshHeader.inputHash, old->settings ) != old->inputHash )
	{
		field = "inputHash";
	}
	else if( oldDoors.count != ( uint32 )mesh.doors.Num() )
	{
		field = acousticBakeNames[ACOUSTIC_BAKE_DOORS];
	}
	else if( directory[0].count != ( uint32 )mesh.numAreas )
	{
		field = acousticBakeNames[0];
	}
	else if( directory[1].count != ( uint32 )numCells )
	{
		field = acousticBakeNames[1];
	}
	else if( directory[2].count != ( uint32 )numProbes )
	{
		field = acousticBakeNames[2];
	}
	else
	{
		const sndBakeDoor_t* doors = ( const sndBakeDoor_t* )( data + oldDoors.offset );
		for( int d = 0; d < mesh.doors.Num(); d++ )
		{
			const sndMeshDoor_t& door = records.doors[d];
			if( doors[d].portal != door.portal || doors[d].area0 != door.area0 || doors[d].area1 != door.area1 )
			{
				field = acousticBakeNames[ACOUSTIC_BAKE_DOORS];
				break;
			}
		}
	}
	if( field != NULL )
	{
		fileSystem->FreeFile( buffer );
		if( !AcousticWrite_Remove( bakePath.c_str() ) )
		{
			return false;
		}
		common->Printf( "acoustics: %s.sndbake removed (stale: %s)\n", mapName, field );
		return true;
	}

	// 3. stamps differ: the four header stamps, the dev head's script CRC, the door names, then the rest of BAK_DEVCHECK

	// each door's old name: step 1 (SndBakeStep1) proved every nameOffset a terminated name inside BAK_STRINGS
	bool namesDiffer = false;
	{
		const sndBakeDoor_t* doors = ( const sndBakeDoor_t* )( data + oldDoors.offset );
		const char* oldNames = ( const char* )( data + oldStrings.offset );
		for( int d = 0; d < mesh.doors.Num() && !namesDiffer; d++ )
		{
			namesDiffer = ( idStr::Cmp( oldNames + doors[d].nameOffset, mesh.doors[d].name.c_str() ) != 0 );
		}
	}

	idList<byte> dev;
	AcousticWrite_DevCheck( mesh, records, dev );
	const byte* oldDevData = data + oldDev.offset;
	const int scriptOffset = ( int )offsetof( sndDevHead_t, scriptCRC );
	uint32 oldScriptCRC;
	memcpy( &oldScriptCRC, oldDevData + scriptOffset, sizeof( oldScriptCRC ) );
	const bool devDiffers = ( oldDev.size != ( uint32 )dev.Num() || memcmp( oldDevData, dev.Ptr(), scriptOffset ) != 0 ||
							  memcmp( oldDevData + scriptOffset + 4, dev.Ptr() + scriptOffset + 4, dev.Num() - scriptOffset - 4 ) != 0 );

	idStr fields;
	if( old->compileId != meshHeader.compileId )
	{
		fields += "compileId";
	}
	if( old->mapGeometryCRC != meshHeader.mapGeometryCRC )
	{
		fields += fields.IsEmpty() ? "mapGeometryCRC" : ", mapGeometryCRC";
	}
	if( old->entityKeysCRC != meshHeader.entityKeysCRC )
	{
		fields += fields.IsEmpty() ? "entityKeysCRC" : ", entityKeysCRC";
	}
	if( old->materialHash != meshHeader.materialHash )
	{
		fields += fields.IsEmpty() ? "materialHash" : ", materialHash";
	}
	if( oldScriptCRC != mesh.scriptCRC )
	{
		fields += fields.IsEmpty() ? "scriptCRC" : ", scriptCRC";
	}
	if( namesDiffer )
	{
		fields += fields.IsEmpty() ? "BAK_STRINGS" : ", BAK_STRINGS";
	}
	if( devDiffers )
	{
		fields += fields.IsEmpty() ? "BAK_DEVCHECK" : ", BAK_DEVCHECK";
	}

	// 4. all equal: keep, nothing written
	if( fields.IsEmpty() )
	{
		fileSystem->FreeFile( buffer );
		if( dmap_acousticVerbose.GetBool() )
		{
			common->Printf( "acoustics: %s.sndbake up to date\n", mapName );
		}
		return true;
	}

	const acousticRestamp_t restamped = AcousticWrite_Restamp( mesh, meshHeader, dev, data, bakePath.c_str() );
	fileSystem->FreeFile( buffer );
	if( restamped == ACOUSTIC_RESTAMP_FAILED )
	{
		// it would carry wrong stamps; a warning or the builder's Error writing <path> named the cause, the old file is left
		if( AcousticWrite_Remove( bakePath.c_str() ) )
		{
			common->Printf( "acoustics: %s.sndbake removed (restamp failed)\n", mapName );
		}
		return false;
	}
	if( restamped == ACOUSTIC_RESTAMP_SHADOWED )
	{
		common->Warning( "acoustics: %s was restamped in the write folder but opens as another file (a copy under fs_savepath, or one in a pack or a layer above the write folder)", bakePath.c_str() );
		return false;
	}
	common->Printf( "acoustics: %s.sndbake restamped (%s)\n", mapName, fields.c_str() );
	return true;
}

/*
==================
AcousticFinishStageA

R-SB1b-19, in this order: (1) validate the hand-over; (2) settings, the game's planes, bins, placement, self-check,
timed as acousticProbes; (3) the twelve sections in id order; (4) the header; (5) the .sndmesh, then the old bake
(R-SB1b-27); (3) to (5) timed as acousticWrite. A failure before the .sndmesh is written removes the .sndmesh and the
.sndbake. Called by acoustic_mesh.cpp's AcousticStageA, which writes the manifest after this returns true; on false it
removes every acoustic output, the .sndmesh this call wrote included (a failed old-bake step, ruled F4-4).
==================
*/
bool AcousticFinishStageA( const acousticMesh_t& mesh )
{
	// R-SB1b-30 (SEAMS D-35): with no areas the tree is never pruned or numbered, so the structure id has nothing to read
	if( mesh.numAreas <= 0 )
	{
		common->Printf( "acoustics: %s has no areas\n", AcousticWrite_MapName() );
		return AcousticWrite_Fail();
	}

	// (1) the records the two CRCs are checked over, then the hand-over
	acousticMeshRecords_t records;
	AcousticWrite_BuildRecords( mesh, records );
	if( !AcousticWrite_Validate( mesh, records ) )
	{
		return AcousticWrite_Fail();
	}

	// (2) the probes
	const int probesStart = Sys_Milliseconds();
	sndMeshSettings_t meshSettings;
	memset( &meshSettings, 0, sizeof( meshSettings ) );
	idList<sndMeshArea_t> areas;
	idList<int32> cells;
	idList<sndMeshProbe_t> probes;
	const bool placed = AcousticProbes( mesh, meshSettings, areas, cells, probes );
	AcousticPhaseEnd( DP_ACOUSTIC_PROBES, probesStart );
	if( !placed )
	{
		return AcousticWrite_Fail();
	}

	// (3) the twelve sections in id order
	const int writeStart = Sys_Milliseconds();

	// the raw worldspawn Stage B words, 0 absent; outside the input hash (S1A-21)
	const idDict& worldspawn = mesh.mapFile->GetEntity( 0 )->epairs;
	meshSettings.sealDoorways = mesh.settings.sealDoorways;
	meshSettings.staticTriBudget = mesh.settings.staticTriBudget;
	meshSettings.scriptCRC = mesh.scriptCRC;
	meshSettings.rawRays = worldspawn.GetInt( "acoustic_rays", 0 );
	meshSettings.rawBounces = worldspawn.GetInt( "acoustic_bounces", 0 );
	meshSettings.rawDuration = worldspawn.GetFloat( "acoustic_duration", 0.0f );
	const bool durationNotFinite = IEEE_FLT_IS_NAN( meshSettings.rawDuration ) || IEEE_FLT_IS_INF( meshSettings.rawDuration );
	if( durationNotFinite )
	{
		common->Warning( "acoustics: acoustic_duration '%s' is not a finite number, ignored", worldspawn.GetString( "acoustic_duration" ) );
		meshSettings.rawDuration = 0.0f;
	}
	meshSettings.reserved = 0;

	// the file's size before the builder starts: past its limit the builder raises an Error, which would end rbdmap
	// with the new .proc beside the old acoustic files (review F4-3)
	{
		const int64 sizes[ACOUSTIC_MESH_SECTIONS] =
		{
			SND_SIZE_MESH_SETTINGS,
			( int64 )SND_SIZE_MATERIAL * records.materials.Num(),
			( int64 )SND_SIZE_MESH_VERT * mesh.verts.Num(),
			( int64 )SND_SIZE_MESH_TRI * mesh.tris.Num(),
			( int64 )SND_SIZE_MESH_TRIMAT * mesh.tris.Num(),
			( int64 )SND_SIZE_MESH_AREA * areas.Num(),
			( int64 )SND_SIZE_CELL * cells.Num(),
			( int64 )SND_SIZE_MESH_PROBE * probes.Num(),
			( int64 )SND_SIZE_PORTAL * mesh.portals.Num(),
			( int64 )SND_SIZE_MESH_DOOR * records.doors.Num(),
			( int64 )SND_SIZE_DEV_ENTITY * records.entities.Num(),
			( int64 )records.strings.Num()
		};
		int64 fileSize = SND_HEADER_SIZE + ( int64 )ACOUSTIC_MESH_SECTIONS * SND_SECTION_SIZE;
		for( int i = 0; i < ACOUSTIC_MESH_SECTIONS; i++ )
		{
			fileSize += ( sizes[i] + 15 ) & ~( int64 )15;
		}
		if( fileSize > SND_FILE_MAX_SIZE )
		{
			common->Warning( "acoustics: the .sndmesh would be %lld bytes, over the format's limit of %lld", fileSize, SND_FILE_MAX_SIZE );
			AcousticPhaseEnd( DP_ACOUSTIC_WRITE, writeStart );
			return AcousticWrite_Fail();
		}
	}

	sndFileBuilder_t b;
	SndFile_Begin( b, SND_MAGIC_MESH, ACOUSTIC_MESH_SECTIONS );
	AcousticWrite_AddRecords( b, MSH_SETTINGS, SND_SIZE_MESH_SETTINGS, &meshSettings, 1 );
	AcousticWrite_AddRecords( b, MSH_MATERIALS, SND_SIZE_MATERIAL, records.materials.Ptr(), records.materials.Num() );
	{
		sndMeshVert_t* verts = ( sndMeshVert_t* )SndFile_AddSection( b, MSH_VERTS, SND_SIZE_MESH_VERT, ( uint32 )mesh.verts.Num() );
		for( int v = 0; v < mesh.verts.Num(); v++ )
		{
			verts[v].xyz[0] = mesh.verts[v].x;
			verts[v].xyz[1] = mesh.verts[v].y;
			verts[v].xyz[2] = mesh.verts[v].z;
		}
	}
	{
		sndMeshTri_t* tris = ( sndMeshTri_t* )SndFile_AddSection( b, MSH_TRIS, SND_SIZE_MESH_TRI, ( uint32 )mesh.tris.Num() );
		for( int t = 0; t < mesh.tris.Num(); t++ )
		{
			for( int k = 0; k < 3; k++ )
			{
				tris[t].v[k] = ( uint32 )mesh.tris[t].v[k];
			}
		}
	}
	{
		sndMeshTriMat_t* trimat = ( sndMeshTriMat_t* )SndFile_AddSection( b, MSH_TRIMAT, SND_SIZE_MESH_TRIMAT, ( uint32 )mesh.tris.Num() );
		for( int t = 0; t < mesh.tris.Num(); t++ )
		{
			trimat[t].material = mesh.tris[t].material;
			trimat[t].flags = mesh.tris[t].flags;
		}
	}
	AcousticWrite_AddRecords( b, MSH_AREAS, SND_SIZE_MESH_AREA, areas.Ptr(), areas.Num() );
	AcousticWrite_AddRecords( b, MSH_CELLS, SND_SIZE_CELL, cells.Ptr(), cells.Num() );
	AcousticWrite_AddRecords( b, MSH_PROBES, SND_SIZE_MESH_PROBE, probes.Ptr(), probes.Num() );
	{
		sndPortal_t* portals = ( sndPortal_t* )SndFile_AddSection( b, MSH_PORTALS, SND_SIZE_PORTAL, ( uint32 )mesh.portals.Num() );
		for( int p = 0; p < mesh.portals.Num(); p++ )
		{
			portals[p].area0 = mesh.portals[p].area0;
			portals[p].area1 = mesh.portals[p].area1;
			portals[p].door = mesh.portals[p].door;
		}
	}
	AcousticWrite_AddRecords( b, MSH_DOORS, SND_SIZE_MESH_DOOR, records.doors.Ptr(), records.doors.Num() );
	AcousticWrite_AddRecords( b, MSH_ENTITIES, SND_SIZE_DEV_ENTITY, records.entities.Ptr(), records.entities.Num() );
	memcpy( SndFile_AddBlob( b, MSH_STRINGS, ( uint32 )records.strings.Num() ), records.strings.Ptr(), records.strings.Num() );

	// (4) the header; step1Bytes, toolVersion and numBands stay 0 in a .sndmesh
	sndFileHeader_t& header = *b.header;
	header.compileId = AcousticStructureId( mesh );
	header.mapGeometryCRC = mesh.mapFile->GetGeometryCRC();
	header.bakerRevision = SND_BAKER_REVISION_A;
	header.entityKeysCRC = mesh.entityKeysCRC;
	header.materialHash = mesh.materialHash;
	header.inputHash = SndFile_MeshInputHash( b.data );	// the builder's buffer: SndFile_Begin set numSections
	header.settings.probeSpacing = meshSettings.probeSpacing;
	header.settings.probeSpacingZ = meshSettings.probeSpacingZ;
	header.settings.probeClearance = meshSettings.probeClearance;
	header.settings.sealDoorways = meshSettings.sealDoorways;
	header.settings.staticTriBudget = meshSettings.staticTriBudget;
	// the Stage B words are the resolved request, by the bake's own resolver (R-SB2-6): the preset of dmap's
	// acousticFast word, then the map's raw values
	SndResolveStageB( header.settings, dmapAcousticFast, meshSettings.rawRays, meshSettings.rawBounces, meshSettings.rawDuration );
	header.settings.diffuseSamples = 0;
	header.settings.irradianceMinDistance = 0.0f;
	const sndFileHeader_t meshHeader = header;

	// one warning per key whose text is present and not used as written: an integer key whose text is not an optional
	// sign and digits, or a duration idStr::IsNumeric refuses (atoi reads "1e3" as 1, "64k" as 64); text that reads
	// as 0, which counts as absent (F4R-5); a value the resolver clamps or rounds (F4-5, F4S-1). A duration that is not
	// finite has its own warning above and none here
	const char* raysText = worldspawn.GetString( "acoustic_rays" );
	const char* bouncesText = worldspawn.GetString( "acoustic_bounces" );
	const char* durationText = worldspawn.GetString( "acoustic_duration" );
	if( raysText[0] != '\0' && ( !AcousticWrite_IsPlainInteger( raysText ) || meshSettings.rawRays == 0 || meshSettings.rawRays != meshHeader.settings.rays ) )
	{
		common->Warning( "acoustics: acoustic_rays '%s' read as %i, using %i", raysText, meshSettings.rawRays, meshHeader.settings.rays );
	}
	if( bouncesText[0] != '\0' && ( !AcousticWrite_IsPlainInteger( bouncesText ) || meshSettings.rawBounces == 0 || meshSettings.rawBounces != meshHeader.settings.bounces ) )
	{
		common->Warning( "acoustics: acoustic_bounces '%s' read as %i, using %i", bouncesText, meshSettings.rawBounces, meshHeader.settings.bounces );
	}
	if( durationText[0] != '\0' && !durationNotFinite &&
			( !idStr::IsNumeric( durationText ) || meshSettings.rawDuration == 0.0f || meshSettings.rawDuration != meshHeader.settings.duration ) )
	{
		common->Warning( "acoustics: acoustic_duration '%s' read as %g, using %g", durationText, meshSettings.rawDuration, meshHeader.settings.duration );
	}

	// (5) the .sndmesh, atomically; then the old bake
	idStr meshPath;
	meshPath.Format( "%s.sndmesh", dmapGlobals.mapFileBase );
	const bool written = SndFile_Finish( b, meshPath.c_str() );
	SndFile_Free( b );
	if( !written )
	{
		AcousticPhaseEnd( DP_ACOUSTIC_WRITE, writeStart );
		return AcousticWrite_Fail();
	}

	const bool kept = AcousticWrite_OldBake( mesh, meshHeader, records, cells.Num(), probes.Num() );
	AcousticPhaseEnd( DP_ACOUSTIC_WRITE, writeStart );
	return kept;
}
