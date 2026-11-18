//puncho - SB1b (R-SB1b-21, R-SB1b-22, R-SB1b-23): the acoustic file builder, H64, the input hash of a bake and the
// noAcoustic delete. Linked in the engine and in rbdmap: only idLib, common and fileSystem are used here.
#include "precompiled.h"
#pragma hdrstop

#include "acoustic.h"

// the first buffer; a .sndmesh is about 0.66 MB per 30,000 triangles, so a few doublings reach it
static const int	SND_FILE_FIRST_CAPACITY	= 64 * 1024;
// SND_FILE_MAX_SIZE, the limit of one file, is in acoustic.h: Stage A tests a .sndmesh's size against it first

/*
==================
SndFile_Grow

Makes the buffer hold newSize bytes: doubles it, the new part zeroed (Mem_ClearedAlloc), the old bytes copied.
Moves data and header.
==================
*/
static void SndFile_Grow( sndFileBuilder_t& b, int64 newSize )
{
	if( newSize <= b.capacity )
	{
		return;
	}
	if( newSize > SND_FILE_MAX_SIZE )
	{
		common->Error( "SndFile_Grow: an acoustic file of %lld bytes is over the format's limit", newSize );
	}

	int64 capacity = ( b.capacity > 0 ) ? b.capacity : SND_FILE_FIRST_CAPACITY;
	while( capacity < newSize )
	{
		capacity *= 2;
	}
	if( capacity > SND_FILE_MAX_SIZE )
	{
		capacity = SND_FILE_MAX_SIZE;
	}

	byte* data = ( byte* )Mem_ClearedAlloc( ( size_t )capacity, TAG_TOOLS );
	if( b.data != NULL )
	{
		memcpy( data, b.data, b.size );
		Mem_Free( b.data );
	}
	b.data = data;
	b.capacity = ( int )capacity;
	b.header = ( sndFileHeader_t* )data;
}

/*
==================
SndFile_Begin

The header and a directory of numSections entries, zeroed; magic and version set.
==================
*/
void SndFile_Begin( sndFileBuilder_t& b, uint32 magic, int numSections )
{
	if( numSections < 1 || numSections > SND_MAX_SECTIONS )
	{
		common->Error( "SndFile_Begin: %i sections, the format holds 1 to %i", numSections, SND_MAX_SECTIONS );
	}

	memset( &b, 0, sizeof( b ) );
	b.numSections = numSections;
	b.nextSection = 0;

	// SND_HEADER_SIZE and SND_SECTION_SIZE are multiples of 16, so the first section starts aligned
	const int start = SND_HEADER_SIZE + numSections * SND_SECTION_SIZE;
	SndFile_Grow( b, start );
	b.size = start;

	b.header->magic = magic;
	b.header->version = SND_FORMAT_VERSION;
	b.header->numSections = ( uint32 )numSections;	//puncho - SB1b-P: so SndFile_FindSection reads the buffer before Finish
}

/*
==================
SndFile_Add

One directory entry and its zeroed space at the current end (always a multiple of 16); the end then moves past
it, rounded up to 16, so the gap after it stays zero.
==================
*/
static void* SndFile_Add( sndFileBuilder_t& b, uint32 id, uint64 bytes, uint32 count )
{
	if( b.data == NULL || b.nextSection >= b.numSections )
	{
		common->Error( "SndFile_Add: section 0x%x is past the %i sections the directory holds", id, b.numSections );
	}
	if( bytes > ( uint64 )SND_FILE_MAX_SIZE )
	{
		common->Error( "SndFile_Add: section 0x%x of %llu bytes is over the format's limit", id, bytes );
	}

	const int offset = b.size;
	const int64 newSize = ( ( int64 )offset + ( int64 )bytes + 15 ) & ~( int64 )15;
	SndFile_Grow( b, newSize );

	sndSection_t& section = ( ( sndSection_t* )( b.data + SND_HEADER_SIZE ) )[b.nextSection];
	section.id = id;
	section.offset = ( uint32 )offset;
	section.size = ( uint32 )bytes;
	section.count = count;
	b.nextSection++;
	b.size = ( int )newSize;

	return b.data + offset;
}

/*
==================
SndFile_AddSection

count records of recordSize bytes; the directory's size is their product.
==================
*/
void* SndFile_AddSection( sndFileBuilder_t& b, uint32 id, uint32 recordSize, uint32 count )
{
	return SndFile_Add( b, id, ( uint64 )recordSize * count, count );
}

/*
==================
SndFile_AddBlob

A section of bytes: MSH_STRINGS, BAK_STRINGS, BAK_DEVCHECK. Its directory count is 0.
==================
*/
void* SndFile_AddBlob( sndFileBuilder_t& b, uint32 id, uint32 bytes )
{
	return SndFile_Add( b, id, bytes, 0 );
}

/*
==================
SndFile_EndStep1

step1Bytes = the current end: the end of the last section added, rounded up to 16.
==================
*/
void SndFile_EndStep1( sndFileBuilder_t& b )
{
	b.header->step1Bytes = ( uint32 )b.size;
}

/*
==================
SndFile_Finish

Fills the builder's header fields, then writes <path>.tmp under fs_basepath and renames it over <path>. On any
failure the temporary file is removed and the old file at <path> is left as it was.
==================
*/
bool SndFile_Finish( sndFileBuilder_t& b, const char* relativePath )
{
	bool valid = ( b.data != NULL && b.nextSection == b.numSections );
	const sndSection_t* directory = ( b.data != NULL ) ? ( const sndSection_t* )( b.data + SND_HEADER_SIZE ) : NULL;
	for( int i = 1; valid && i < b.nextSection; i++ )
	{
		valid = ( directory[i].id > directory[i - 1].id );
	}
	if( !valid )
	{
		common->Printf( "Error writing %s: %i of %i sections, ids not strictly ascending or missing\n", relativePath, b.nextSection, b.numSections );
		return false;
	}

	sndFileHeader_t* header = b.header;
	header->version = SND_FORMAT_VERSION;
	header->numSections = ( uint32 )b.numSections;
	header->payloadSize = ( uint32 )( b.size - SND_HEADER_SIZE );
	// computed last, over the whole buffer with its own field 0 (DR-62)
	header->payloadCRC = SndFile_PayloadCRC( b.data, b.size );	//puncho - SB1b-P: the one CRC of a file

	idStr tmpPath = relativePath;
	tmpPath += ".tmp";

	fileSystem->RemoveFile( tmpPath.c_str() );

	idFile* f = fileSystem->OpenFileWrite( tmpPath.c_str(), "fs_basepath" );
	if( f == NULL )
	{
		fileSystem->RemoveFile( tmpPath.c_str() );
		common->Printf( "Error writing %s\n", relativePath );
		return false;
	}
	// FlushChecked drains and checks stdio off Windows; on Windows it only checks the handle (WriteFile has no
	// stdio buffer), so ForceFlush (FlushFileBuffers) still puts the bytes on disk before the rename (DR-33)
	const bool written = ( f->Write( b.data, b.size ) == b.size ) && f->FlushChecked();
	if( written )
	{
		f->ForceFlush();
	}
	fileSystem->CloseFile( f );

	if( !written || !fileSystem->RenameFile( tmpPath.c_str(), relativePath, "fs_basepath" ) )
	{
		fileSystem->RemoveFile( tmpPath.c_str() );
		common->Printf( "Error writing %s\n", relativePath );
		return false;
	}
	return true;
}

/*
==================
SndFile_Free
==================
*/
void SndFile_Free( sndFileBuilder_t& b )
{
	if( b.data != NULL )
	{
		Mem_Free( b.data );
	}
	memset( &b, 0, sizeof( b ) );
}

/*
==================
SndHash64

H64 (DR-20): the first 8 bytes of the MD5, read little-endian.
==================
*/
void SndHash64Begin( MD5_CTX& ctx )
{
	MD5_Init( &ctx );
}

void SndHash64Update( MD5_CTX& ctx, const void* data, int size )
{
	MD5_Update( &ctx, ( const unsigned char* )data, ( size_t )size );
}

uint64 SndHash64End( MD5_CTX& ctx )
{
	unsigned char digest[16];
	MD5_Final( &ctx, digest );

	uint64 hash = 0;
	for( int i = 7; i >= 0; i-- )
	{
		hash = ( hash << 8 ) | digest[i];
	}
	return hash;
}

uint64 SndHash64( const void* data, int size )
{
	MD5_CTX ctx;
	SndHash64Begin( ctx );
	SndHash64Update( ctx, data, size );
	return SndHash64End( ctx );
}

/*
==================
SndBakeInputHash

The .sndbake inputHash (R-SB1b-23): H64 over meshHash as 8 bytes little-endian, then the 48 settings bytes. One
definition for the bake (SB2), the restamp (R-SB1b-27) and the game's check.
==================
*/
uint64 SndBakeInputHash( uint64 meshHash, const sndSettings_t& settings )
{
	byte meshBytes[8];
	for( int i = 0; i < 8; i++ )
	{
		meshBytes[i] = ( byte )( meshHash >> ( i * 8 ) );
	}

	MD5_CTX ctx;
	SndHash64Begin( ctx );
	SndHash64Update( ctx, meshBytes, ( int )sizeof( meshBytes ) );
	SndHash64Update( ctx, &settings, ( int )sizeof( settings ) );
	return SndHash64End( ctx );
}

//puncho start - SB1b-P (fix round 1): one resolver of the Stage B words, one file CRC, one section lookup
/*
==================
SndResolveStageB

R-SB2-6 (K-SB8): the Stage B words of the .sndmesh header and of the bake. Writes preset, rays, bounces, duration and
ambisonicOrder only. The preset first, then each non-zero raw value overrides its field (a duration that is not
finite is absent), then the clamps: rays to [32, 1048576] and up to a multiple of 32, the ceiling to [1, 256], the
duration to [0.1, 4.0] s.
==================
*/
void SndResolveStageB( sndSettings_t& settings, bool fast, int32 rawRays, int32 rawBounces, float rawDuration )
{
	settings.preset			= fast ? 1 : 0;
	settings.rays			= fast ? 2048 : 32768;
	settings.bounces		= 256;
	settings.duration		= 1.0f;
	settings.ambisonicOrder	= 1;

	if( rawRays != 0 )
	{
		settings.rays = rawRays;
	}
	if( rawBounces != 0 )
	{
		settings.bounces = rawBounces;
	}
	if( rawDuration != 0.0f && !IEEE_FLT_IS_NAN( rawDuration ) && !IEEE_FLT_IS_INF( rawDuration ) )
	{
		settings.duration = rawDuration;
	}

	// 1048576 is a multiple of 32, so clamping before the rounding gives the same words as after it
	int64 rays = settings.rays;
	rays = ( rays < 32 ) ? 32 : ( ( rays > 1048576 ) ? 1048576 : rays );
	settings.rays = ( int32 )( ( ( rays + 31 ) / 32 ) * 32 );

	settings.bounces = ( settings.bounces < 1 ) ? 1 : ( ( settings.bounces > 256 ) ? 256 : settings.bounces );
	if( !( settings.duration >= 0.1f ) )
	{
		settings.duration = 0.1f;
	}
	else if( settings.duration > 4.0f )
	{
		settings.duration = 4.0f;
	}
}

/*
==================
SndFile_PayloadCRC

CRC32 over the whole file with the four bytes of payloadCRC taken as 0 (DR-62). length >= SND_HEADER_SIZE.
==================
*/
uint32 SndFile_PayloadCRC( const byte* data, int length )
{
	assert( length >= SND_HEADER_SIZE );
	const int crcOffset = ( int )offsetof( sndFileHeader_t, payloadCRC );
	const uint32 zero = 0;
	unsigned int crc;
	CRC32_InitChecksum( crc );
	CRC32_UpdateChecksum( crc, data, crcOffset );
	CRC32_UpdateChecksum( crc, &zero, ( int )sizeof( zero ) );
	CRC32_UpdateChecksum( crc, data + crcOffset + ( int )sizeof( zero ), length - crcOffset - ( int )sizeof( zero ) );
	CRC32_FinishChecksum( crc );
	return crc;
}

/*
==================
SndFile_FindSection

The directory entry with that id among the header's numSections, or NULL. The caller has checked that the
directory lies inside the buffer.
==================
*/
const sndSection_t* SndFile_FindSection( const byte* data, uint32 id )
{
	const sndFileHeader_t* header = ( const sndFileHeader_t* )data;
	const sndSection_t* directory = ( const sndSection_t* )( data + SND_HEADER_SIZE );
	for( uint32 i = 0; i < header->numSections; i++ )
	{
		if( directory[i].id == id )
		{
			return &directory[i];
		}
	}
	return NULL;
}

/*
==================
SndFile_MeshInputHash

R-SB1b-23 (DR-29): the .sndmesh inputHash, H64 over MSH_SETTINGS bytes 0-19, each MSH_MATERIALS record's bytes 4-31,
MSH_VERTS, MSH_TRIS, MSH_TRIMAT, MSH_AREAS, MSH_CELLS, MSH_PROBES, MSH_PORTALS whole, each MSH_DOORS record's bytes
4-27. Names, the script CRC and the raw Stage B words stay out, so a rename or a script edit keeps the bake.
data holds a .sndmesh's header and directory, and every one of the twelve sections lies inside the buffer at the
offset and size its directory entry gives (the builder's buffer before Finish, or a file as read whose directory
entries the caller has checked against its length); the sections are found through the directory. Returns 0 when
one of the ten sections hashed is missing, or MSH_SETTINGS is shorter than its record.
==================
*/
uint64 SndFile_MeshInputHash( const byte* data )
{
	const sndSection_t* settings = SndFile_FindSection( data, MSH_SETTINGS );
	const sndSection_t* materials = SndFile_FindSection( data, MSH_MATERIALS );
	const sndSection_t* doors = SndFile_FindSection( data, MSH_DOORS );
	if( settings == NULL || materials == NULL || doors == NULL || settings->size < SND_SIZE_MESH_SETTINGS )
	{
		return 0;
	}
	for( int id = MSH_VERTS; id <= MSH_PORTALS; id++ )
	{
		if( SndFile_FindSection( data, ( uint32 )id ) == NULL )
		{
			return 0;
		}
	}

	MD5_CTX ctx;
	SndHash64Begin( ctx );

	SndHash64Update( ctx, data + settings->offset, ( int )offsetof( sndMeshSettings_t, scriptCRC ) );

	const int materialSkip = ( int )offsetof( sndMaterial_t, absorption );
	for( uint32 offset = 0; offset + SND_SIZE_MATERIAL <= materials->size; offset += SND_SIZE_MATERIAL )
	{
		SndHash64Update( ctx, data + materials->offset + offset + materialSkip, SND_SIZE_MATERIAL - materialSkip );
	}

	for( int id = MSH_VERTS; id <= MSH_PORTALS; id++ )
	{
		const sndSection_t* section = SndFile_FindSection( data, ( uint32 )id );
		SndHash64Update( ctx, data + section->offset, ( int )section->size );
	}

	const int doorSkip = ( int )offsetof( sndMeshDoor_t, portal );
	for( uint32 offset = 0; offset + SND_SIZE_MESH_DOOR <= doors->size; offset += SND_SIZE_MESH_DOOR )
	{
		SndHash64Update( ctx, data + doors->offset + offset + doorSkip, SND_SIZE_MESH_DOOR - doorSkip );
	}

	return SndHash64End( ctx );
}

/*
==================
SndFile_DevCheckSize, SndFile_BuildDevCheck

BAK_DEVCHECK, one builder for the restamp, its keep test and the bake (R-SB2-24): the head, then the .sndmesh's
MSH_ENTITIES records, its MSH_MATERIALS records and a copy of its MSH_STRINGS, each part at a 16-aligned offset from
the section start, zero between; every nameOffset stays relative to the copied table. out holds
SndFile_DevCheckSize bytes and every one is written.
==================
*/
static int SndFile_DevMaterialsOffset( int numEntities )
{
	return ( SND_SIZE_DEV_HEAD + numEntities * SND_SIZE_DEV_ENTITY + 15 ) & ~15;
}

static int SndFile_DevStringsOffset( int numEntities, int numMaterials )
{
	return ( SndFile_DevMaterialsOffset( numEntities ) + numMaterials * SND_SIZE_MATERIAL + 15 ) & ~15;
}

int SndFile_DevCheckSize( int numEntities, int numMaterials, int stringsSize )
{
	return SndFile_DevStringsOffset( numEntities, numMaterials ) + stringsSize;
}

void SndFile_BuildDevCheck( byte* out, const sndDevEntity_t* entities, int numEntities, const sndMaterial_t* materials, int numMaterials,
							const char* strings, int stringsSize, uint32 scriptCRC )
{
	sndDevHead_t head;
	memset( &head, 0, sizeof( head ) );
	head.numEntities = ( uint32 )numEntities;
	head.numMaterials = ( uint32 )numMaterials;
	head.scriptCRC = scriptCRC;
	head.entitiesOffset = SND_SIZE_DEV_HEAD;
	head.materialsOffset = ( uint32 )SndFile_DevMaterialsOffset( numEntities );
	head.stringsOffset = ( uint32 )SndFile_DevStringsOffset( numEntities, numMaterials );
	head.stringsSize = ( uint32 )stringsSize;

	memset( out, 0, SndFile_DevCheckSize( numEntities, numMaterials, stringsSize ) );
	memcpy( out, &head, sizeof( head ) );
	if( numEntities > 0 )
	{
		memcpy( out + head.entitiesOffset, entities, ( size_t )numEntities * SND_SIZE_DEV_ENTITY );
	}
	if( numMaterials > 0 )
	{
		memcpy( out + head.materialsOffset, materials, ( size_t )numMaterials * SND_SIZE_MATERIAL );
	}
	if( stringsSize > 0 )
	{
		memcpy( out + head.stringsOffset, strings, stringsSize );
	}
}
//puncho end

/*
==================
AcousticDeleteOutputs

noAcoustic and the skips of R-SB1a-4 (R-SB1b-21): the map's acoustic files and any temporary file a failed write
left, from both roots (RemoveFile tries fs_basepath and fs_savepath), as dmap removes its other outputs.
==================
*/
void AcousticDeleteOutputs( const char* mapFileBase )
{
	fileSystem->RemoveFile( va( "%s.sndmesh", mapFileBase ) );
	fileSystem->RemoveFile( va( "%s.sndmanifest", mapFileBase ) );
	fileSystem->RemoveFile( va( "%s.sndbake", mapFileBase ) );
	fileSystem->RemoveFile( va( "%s.sndmesh.tmp", mapFileBase ) );
	fileSystem->RemoveFile( va( "%s.sndbake.tmp", mapFileBase ) );
	fileSystem->RemoveFile( va( "%s_acoustic.obj", mapFileBase ) );	//puncho - SB1a-D (review M3-10): Stage A's dump
}
