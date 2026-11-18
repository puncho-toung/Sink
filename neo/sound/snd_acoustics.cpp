//puncho - SB3 (R-SB3-1 to R-SB3-7, R-SB3-10, R-SB3-15, R-SB3-30, R-SB3-34; SB3-W: R-SB3-8, R-SB3-9, R-SB3-33, R-SB3-25 to R-SB3-27): the per-map gate of the baked acoustics
#include "precompiled.h"
#pragma hdrstop

#include "snd_local.h"
#include "snd_acoustics.h"
#include "../tools/compilers/dmap/acoustic_shared.h"	//puncho - SB3-W: the working-tree checks repeat dmap's record CRCs and script walk
#include "snd_occlusion.h"	//puncho - SA3-E (DR-193): the map hooks

/*
===============================================================================

	A map with no bake pays, per load, six NULL tests (ReleaseAcousticsLoad at step 0 and at the exit), the cvar read,
	one missed OpenFileRead and one console line; at map shutdown one virtual call and four NULL tests (three of them
	ReleaseAcousticsLoad's). Everything else here runs only once a .sndbake has been found.

===============================================================================
*/

// R-SB3-10: read once per load (and by ListenerPass on a gated frame); not network-synced, nothing saved
idCVar s_acoustics( "s_acoustics", "1", CVAR_INTEGER | CVAR_ARCHIVE, "baked room acoustics: 0 off, 1 on, 2 on and every off line but 's_acoustics 0' a warning; read at map load", 0, 2, idCmdSystem::ArgCompletion_Integer<0, 2> );

static const int SND_ACOUSTICS_READ_CHUNK = 1024 * 1024;	// step 6 reads the block in 1 MB pieces, the pacifier between

/*
========================
SndAcoustics_SectionStale

The state of a failed directory or section check, R-SB3-7: the section's K-SB6 name, or its id when unknown.
========================
*/
static void SndAcoustics_SectionStale( char* state, int stateSize, uint32 id )
{
	const char* name = NULL;
	switch( id )
	{
		case BAK_AREAS:
			name = "BAK_AREAS";
			break;
		case BAK_CELLS:
			name = "BAK_CELLS";
			break;
		case BAK_PROBES:
			name = "BAK_PROBES";
			break;
		case BAK_ZONES:
			name = "BAK_ZONES";
			break;
		case BAK_DOORS:
			name = "BAK_DOORS";
			break;
		case BAK_STRINGS:
			name = "BAK_STRINGS";
			break;
		case BAK_DEVCHECK:
			name = "BAK_DEVCHECK";
			break;
	}
	if( name != NULL )
	{
		idStr::snPrintf( state, stateSize, "off (stale: section %s)", name );
	}
	else
	{
		idStr::snPrintf( state, stateSize, "off (stale: section 0x%x)", id );
	}
}

//puncho start - SB3-W (R-SB3-8, R-SB3-9, R-SB3-33): step 8 of the load, the working-tree checks

// a material record's seven floats are acousticNumbers_t's, in its order: compared as one run of bytes
static_assert( sizeof( acousticNumbers_t ) == 7 * sizeof( float ) && sizeof( sndMaterial_t ) - offsetof( sndMaterial_t, absorption ) == sizeof( acousticNumbers_t )
			   && offsetof( sndMaterial_t, scattering ) - offsetof( sndMaterial_t, absorption ) == offsetof( acousticNumbers_t, scattering )
			   && offsetof( sndMaterial_t, transmission ) - offsetof( sndMaterial_t, absorption ) == offsetof( acousticNumbers_t, transmission ),
			   "sndMaterial_t's floats are acousticNumbers_t" );

/*
========================
SndAcoustics_StaleRecord

The state of a check that found its first differing record: the field, the record's index and its name, the name
cut to fit so the state keeps its closing parenthesis. SB3-T prints the record from acousticsLine.
========================
*/
static void SndAcoustics_StaleRecord( char* state, int stateSize, const char* field, const char* kind, int index, const char* name )
{
	idStr::snPrintf( state, stateSize, "off (stale: %s, %s %d )", field, kind, index );
	const int room = Max( 0, stateSize - 1 - idStr::Length( state ) );
	idStr::snPrintf( state, stateSize, "off (stale: %s, %s %d %.*s)", field, kind, index, room, name );
}

/*
========================
SndAcoustics_CRCFileRange

R-SB3-9.4: feeds the file's bytes [from, to) to crc through one 4 KB stack chunk. Nothing is read or sought when
the range is empty; the seek is made only when filePos, the file's position, is not already at from. The CRC takes
only what a read returned whole. False after a failed seek or a short read (filePos is then not to be trusted).
========================
*/
static bool SndAcoustics_CRCFileRange( idFile* file, int& filePos, int from, int to, unsigned int& crc )
{
	if( from >= to )
	{
		return true;
	}
	if( filePos != from )
	{
		if( file->Seek( ( long )from, FS_SEEK_SET ) != 0 )
		{
			return false;
		}
		filePos = from;
	}
	byte chunk[4096];
	while( filePos < to )
	{
		const int bytes = Min( to - filePos, ( int )sizeof( chunk ) );
		if( file->Read( chunk, bytes ) != bytes )
		{
			return false;
		}
		CRC32_UpdateChecksum( crc, chunk, bytes );
		filePos += bytes;
	}
	return true;
}

/*
========================
SndAcoustics_WorkingTree

Step 8 of R-SB3-4: refuses a bake whose entities, materials, scripts or bytes changed under it in a working tree or
a mod. True when every check that runs passes; false with the off state written. The section's buffer is held in
scratch, the world's, so the release after an Error finds it; it is freed and scratch NULL before this returns, on
every path. The three name sets are locals, which an Error's unwinding destroys. FindMaterial can parse a material and
so raise an Error (an image); the world's ReleaseAcousticsLoad frees what the load held (W4-3).
DR-148: the entity check reads no decl and raises nothing; its two hash indexes are locals as the name sets are, and
its arrays are in the buffer.
========================
*/
static bool SndAcoustics_WorkingTree( idFile* file, int fileLength, const byte* block, int step1Bytes, const sndSection_t* section,
									  const idMapFile* mapFile, byte*& scratch, char* state, int stateSize )
{
	// R-SB3-8: never on developer. A packaged game with no mod runs none and does not even seek (C-SB3)
	const bool full = !fileSystem->InProductionMode();
	const bool layers = ( fileSystem->GetNumLayers() > 1 );
	if( !full && !layers )
	{
		return true;
	}

	const sndFileHeader_t& header = *( const sndFileHeader_t* )block;
	// section: step 7's BAK_DEVCHECK entry or NULL; bounded inside the file there, so its size fits an int
	if( section == NULL || section->size < SND_SIZE_DEV_HEAD )
	{
		SndAcoustics_SectionStale( state, stateSize, BAK_DEVCHECK );
		return false;
	}

	// R-SB3-33: one buffer holds the section as read and, after it, per map entity the kind of the record that claimed
	// it (AcousticScriptCRC's kinds) and its GetEntityCRC, then a claimed byte per record, as many as the section can
	// hold (DR-148)
	const int sectionBytes = ( int )section->size;
	const int numEntities = mapFile->GetNumEntities();
	const int64 kindsOffset = ( ( int64 )sectionBytes + 3 ) & ~( int64 )3;
	const int64 crcsOffset = kindsOffset + ( int64 )numEntities * ( int64 )sizeof( uint32 );		//puncho - DR-148
	const int64 claimedOffset = crcsOffset + ( int64 )numEntities * ( int64 )sizeof( uint32 );	//puncho - DR-148
	const int64 bufferBytes = claimedOffset + ( int64 )( sectionBytes / SND_SIZE_DEV_ENTITY );	//puncho - DR-148
	if( bufferBytes > INT_MAX )
	{
		SndAcoustics_SectionStale( state, stateSize, BAK_DEVCHECK );
		return false;
	}
	assert( scratch == NULL );
	scratch = ( byte* )Mem_Alloc16( ( size_t )bufferBytes, TAG_AUDIO );
	byte* buffer = scratch;
	if( buffer == NULL )
	{
		idStr::Copynz( state, "off (no memory)", stateSize );
		return false;
	}
	uint32* kinds = ( uint32* )( buffer + kindsOffset );
	uint32* entityCRCs = ( uint32* )( buffer + crcsOffset );	//puncho - DR-148
	byte* claimed = buffer + claimedOffset;						//puncho - DR-148

	acousticNameSet_t	staticNames;		// DR-148 (Y1): the names of the claimed func_static and misc_model candidates
	acousticNameSet_t	candidateClasses;	// DR-148 (Y1): the classnames of every claimed candidate
	acousticNameSet_t	tokens;				// the script walk's tokens
	bool				passed = false;
	// about 6,000 unique tokens from 21,000 adds in the base scripts: grown in 4,096 steps, not 16 (W4-2). On an empty
	// list this only sets the step; the first Append allocates
	tokens.names.SetGranularity( 4096 );
	do
	{
		// R-SB3-33: one seek, one read, then every bound before a single record is used
		if( file->Seek( ( long )section->offset, FS_SEEK_SET ) != 0 || file->Read( buffer, sectionBytes ) != sectionBytes )
		{
			SndAcoustics_SectionStale( state, stateSize, BAK_DEVCHECK );
			break;
		}
		const sndDevHead_t& head = *( const sndDevHead_t* )buffer;
		bool bad = ( head.entitiesOffset % 16 ) != 0 || ( head.materialsOffset % 16 ) != 0 || ( head.stringsOffset % 16 ) != 0
				   || head.entitiesOffset < SND_SIZE_DEV_HEAD || head.materialsOffset < SND_SIZE_DEV_HEAD || head.stringsOffset < SND_SIZE_DEV_HEAD
				   || ( uint64 )head.entitiesOffset + ( uint64 )SND_SIZE_DEV_ENTITY * head.numEntities > ( uint64 )head.materialsOffset
				   || ( uint64 )head.materialsOffset + ( uint64 )SND_SIZE_MATERIAL * head.numMaterials > ( uint64 )head.stringsOffset
				   || ( uint64 )head.stringsOffset + head.stringsSize > ( uint64 )sectionBytes
				   || head.stringsSize == 0;
		if( bad )
		{
			SndAcoustics_SectionStale( state, stateSize, BAK_DEVCHECK );
			break;
		}
		// every table lies inside the section, so every count fits an int
		const sndDevEntity_t* entities = ( const sndDevEntity_t* )( buffer + head.entitiesOffset );
		const sndMaterial_t* materials = ( const sndMaterial_t* )( buffer + head.materialsOffset );
		const char* strings = ( const char* )( buffer + head.stringsOffset );
		const int numRecords = ( int )head.numEntities;
		const int numMaterials = ( int )head.numMaterials;
		bad = ( strings[head.stringsSize - 1] != '\0' );
		for( int i = 0; i < numRecords && !bad; i++ )
		{
			bad = entities[i].nameOffset >= head.stringsSize || entities[i].kind > 2;	//puncho - DR-148 (X4.7): a kind above 2 is the section's fault
		}
		for( int m = 0; m < numMaterials && !bad; m++ )
		{
			bad = materials[m].nameOffset >= head.stringsSize;
		}
		if( bad )
		{
			SndAcoustics_SectionStale( state, stateSize, BAK_DEVCHECK );
			break;
		}

		//puncho start - DR-148 (X4): R-SB3-9.1 entityKeysCRC, records and map entities matched by content, so an entity
		// that cannot change a bake (a light above all) may come, go, change and move. Pass or fail does not depend on
		// the map's order; only which entity a line names does. Each map entity's CRC once (GetEntityCRC walks every
		// pair), hashed in reverse so a chain yields the lowest index first, as the world store's P2.4 matching does.
		// A claimed entity or record leaves its chain (E4-5), so a chain holds only unclaimed ones; kinds[j] != 0 is map
		// entity j claimed by a candidate record (the script CRC's kinds), claimed[i] kind 0 record i claimed (pass 4)
		memset( kinds, 0, ( size_t )numEntities * sizeof( uint32 ) );
		memset( claimed, 0, ( size_t )numRecords );
		idHashIndex entityHash( 4096, numEntities );
		for( int j = numEntities - 1; j >= 0; j-- )
		{
			entityCRCs[j] = mapFile->GetEntityCRC( j );
			entityHash.Add( entityHash.GenerateKey( ( int )entityCRCs[j] ), j );
		}
		// 2: each candidate record, in record order, claims the map entity at its own index when that is unclaimed and
		// equal (E4-4), else the lowest unclaimed one with its CRC. The two sets AcousticKind0CRC reads are built from
		// the claimed entities' keys, which equal the baked ones where the CRCs do (Y1, Y3)
		int differs = -1;	// a record nothing matches
		for( int i = 0; i < numRecords && differs == -1; i++ )
		{
			if( entities[i].kind == 0 )
			{
				continue;
			}
			const uint32 crc = entities[i].crc;
			int match = -1;
			if( i < numEntities && kinds[i] == 0 && entityCRCs[i] == crc )
			{
				match = i;
			}
			for( int j = entityHash.First( entityHash.GenerateKey( ( int )crc ) ); j != -1 && match == -1; j = entityHash.Next( j ) )
			{
				if( entityCRCs[j] == crc )
				{
					match = j;
				}
			}
			if( match == -1 )
			{
				differs = i;
				continue;
			}
			kinds[match] = entities[i].kind;
			// the removal walks the chain from its head, where unclaimed entities stay: each kind 0 entity with a
			// candidate's exact key/values ahead of this one costs a step (E4R-4, ruled)
			entityHash.Remove( entityHash.GenerateKey( ( int )crc ), match );
			const idDict& epairs = mapFile->GetEntity( match )->epairs;
			const char* classname = epairs.GetString( "classname" );
			if( classname[0] != '\0' )
			{
				AcousticNameSet_Add( candidateClasses, classname, idStr::Length( classname ) );
			}
			const char* name = epairs.GetString( "name" );
			if( name[0] != '\0' && ( !idStr::Icmp( classname, "func_static" ) || !idStr::Icmp( classname, "misc_model" ) ) )
			{
				AcousticNameSet_Add( staticNames, name, idStr::Length( name ) );
			}
		}
		// 3: each unclaimed map entity claims a kind 0 record of its AcousticKind0CRC when that is not 0. No entity def
		// is read (DR-148 Z2). Refused here, by pass 2 or by the geometry CRC of step 5: a candidate changed or removed;
		// any entity added with a model key; any func_static or misc_model with no record; any entity of a baked
		// candidate's class with no record; an entity that starts or stops naming a static candidate; a brush added,
		// removed or changed. Not seen, named limits: a door-class entity of a class the bake has no candidate of,
		// added with no model key (with a model in its entityDef it gets a door record; with none its default cube can
		// take a baked door's portal); a brush entity with no model key whose class becomes such a door class; a brush
		// func_static with no model key that takes the record of an inert static leaving in the same edit; which entity
		// owns which brush; the map order of two doors on one portal; an entity made a door by a spawnclass key in the
		// map (E4S-1). Refused though no bake can change, the price of
		// reading no entity def: an entity with a model key that is no candidate added or removed; an inert func_static
		// or misc_model added or removed; an entity with no model key of a baked candidate's class added or removed.
		// Kind 0 records hashed in reverse, as above
		int added = -1;		// a map entity nothing matches
		if( differs == -1 )
		{
			idHashIndex recordHash( 1024, numRecords );
			for( int i = numRecords - 1; i >= 0; i-- )
			{
				if( entities[i].kind == 0 && entities[i].crc != 0 )
				{
					recordHash.Add( recordHash.GenerateKey( ( int )entities[i].crc ), i );
				}
			}
			for( int j = 0; j < numEntities && added == -1; j++ )
			{
				if( kinds[j] != 0 )
				{
					continue;
				}
				const uint32 crc = AcousticKind0CRC( mapFile->GetEntity( j )->epairs, staticNames, candidateClasses );
				if( crc == 0 )
				{
					continue;
				}
				int match = -1;
				for( int i = recordHash.First( recordHash.GenerateKey( ( int )crc ) ); i != -1 && match == -1; i = recordHash.Next( i ) )
				{
					if( entities[i].crc == crc )
					{
						match = i;
					}
				}
				if( match == -1 )
				{
					added = j;
				}
				else
				{
					claimed[match] = 1;
					recordHash.Remove( recordHash.GenerateKey( ( int )crc ), match );
				}
			}
			// 4: a kind 0 record of nonzero CRC (Y1) that found no entity
			for( int i = 0; i < numRecords && added == -1 && differs == -1; i++ )
			{
				if( entities[i].kind == 0 && entities[i].crc != 0 && claimed[i] == 0 )
				{
					differs = i;
				}
			}
		}
		if( differs != -1 )
		{
			SndAcoustics_StaleRecord( state, stateSize, "entityKeysCRC", "baked entity", differs, strings + entities[differs].nameOffset );	// E4-7
			break;
		}
		if( added != -1 )
		{
			SndAcoustics_StaleRecord( state, stateSize, "entityKeysCRC", "map entity", added, mapFile->GetEntity( added )->epairs.GetString( "name" ) );	// E4-7
			break;
		}
		// 5: the records themselves, as written
		//puncho end
		if( SndEntityKeysCRC( entities, numRecords ) != header.entityKeysCRC )
		{
			idStr::Copynz( state, "off (stale: entityKeysCRC)", stateSize );
			break;
		}

		// R-SB3-9.2 materialHash: each record's numbers resolved again, never creating a default material. A
		// _acoustic_ name is synthetic (R-SB1a-24): its preset's numbers, fixed by bakerRevision, checked at step 5
		int materialDiffers = -1;
		for( int m = 0; m < numMaterials && materialDiffers == -1; m++ )
		{
			const char* name = strings + materials[m].nameOffset;
			if( idStr::Cmpn( name, "_acoustic_", 10 ) == 0 )
			{
				continue;
			}
			// a name with no declaration: Stage A recorded its implicit material's numbers, which a game that has not
			// loaded the model yet cannot ask for without creating that material (W4-4)
			acousticNumbers_t numbers;
			const idMaterial* material = declManager->FindMaterial( name, false );
			if( material != NULL )
			{
				AcousticResolveNumbers( material, numbers );
			}
			else
			{
				AcousticUndeclaredNumbers( numbers );
			}
			if( memcmp( &numbers, materials[m].absorption, sizeof( numbers ) ) != 0 )
			{
				materialDiffers = m;
			}
		}
		if( materialDiffers != -1 )
		{
			SndAcoustics_StaleRecord( state, stateSize, "materialHash", "material", materialDiffers, strings + materials[materialDiffers].nameOffset );
			break;
		}
		// every record equals its resolved numbers, so the hash over the records is the hash over the numbers
		if( SndMaterialHash( materials, numMaterials, strings ) != header.materialHash )
		{
			idStr::Copynz( state, "off (stale: materialHash)", stateSize );
			break;
		}

		// R-SB3-9.3 scriptCRC: SB1a's walk in the game's compile order, then the sum over the script-named static
		// candidates (func_static and misc_model, DR-148 Y2). No
		// pacifier between files (W4-1): each call would be one more place an Error can be raised inside the load
		acousticScriptSink_t sink;
		sink.file = AcousticTokenSink;
		sink.context = &tokens;
		AcousticScriptWalk( mapFile->GetName(), sink );
		if( AcousticScriptCRC( tokens, *mapFile, kinds, numEntities ) != head.scriptCRC )	//puncho - DR-148: kinds is per map entity
		{
			idStr::Copynz( state, "off (stale: scriptCRC)", stateSize );
			break;
		}

		// R-SB3-9.4 payloadCRC, working tree only: the whole file with its own 4 bytes as 0, in file order. The block
		// and this section come from memory; only the bytes outside both are read: a gap before the section (none in
		// a file the builder wrote) and what lies after it, its padding (W4-7)
		if( full )
		{
			static const byte zero[4] = { 0, 0, 0, 0 };
			const int crcOffset = ( int )offsetof( sndFileHeader_t, payloadCRC );
			unsigned int crc;
			CRC32_InitChecksum( crc );
			CRC32_UpdateChecksum( crc, block, crcOffset );
			CRC32_UpdateChecksum( crc, zero, ( int )sizeof( zero ) );
			CRC32_UpdateChecksum( crc, block + crcOffset + sizeof( zero ), step1Bytes - crcOffset - ( int )sizeof( zero ) );
			const int sectionStart = ( int )section->offset;
			const int sectionEnd = sectionStart + sectionBytes;		// inside the file, step 7
			int filePos = sectionEnd;								// where the section's read left the file
			// the gap between the block and the section
			bool readOk = SndAcoustics_CRCFileRange( file, filePos, step1Bytes, sectionStart, crc );
			// the part of the section past the block (all of it unless the section overlaps step 1)
			const int fromBuffer = Max( step1Bytes, sectionStart );
			if( readOk && sectionEnd > fromBuffer )
			{
				CRC32_UpdateChecksum( crc, buffer + ( fromBuffer - sectionStart ), sectionEnd - fromBuffer );
			}
			// what lies after the section: today its padding
			readOk = readOk && SndAcoustics_CRCFileRange( file, filePos, Max( step1Bytes, sectionEnd ), fileLength, crc );
			CRC32_FinishChecksum( crc );
			if( !readOk || crc != header.payloadCRC )
			{
				idStr::Copynz( state, "off (stale: payloadCRC)", stateSize );
				break;
			}
		}
		passed = true;
	}
	while( false );

	Mem_Free16( buffer );
	scratch = NULL;
	return passed;
}
//puncho end

/*
========================
idSoundWorldLocal::LoadMapAcoustics

R-SB3-4. Main thread, inside LOAD_ACOUSTICS. Steps in order; the first failure leaves through the one exit
(R-SB3-34) with acoustics NULL and nothing allocated. acoustics is set last, so a pacifier render during the load
runs the ungated arm. An Error raised from step 4 through step 8 leaks nothing: the file, the block and step 8's
buffer are the world's (ReleaseAcousticsLoad, W4-3). The cover ends at step 9's graph allocation: from there to
step 13 the graph and the object are held by locals only, so nothing there may raise, SB4's and SA3's hooks at
steps 11 and 12 included (D-40).
========================
*/
void idSoundWorldLocal::LoadMapAcoustics( const char* mapName, unsigned int mapGeometryCRC, const idMapFile* mapFile )
{
	// D-8: the log follows the world this was last called on
	soundSystemLocal.acousticsGameWorld = this;

	// step 0: a block left by a load that never reached its shutdown; the menu world has no render world
	if( acoustics != NULL )
	{
		FreeMapAcoustics();
	}
	ReleaseAcousticsLoad();		//puncho SB3-W (W4-3): what a load an Error unwound still held
	if( renderWorld == NULL )
	{
		idStr::snPrintf( acousticsLine, sizeof( acousticsLine ), "acoustics: %s off (no render world)", mapName );
		common->Warning( "%s", acousticsLine );
		return;
	}

	char				state[96];	//puncho - SB3-W: room for the named record of a working-tree mismatch
	const int			mode = s_acoustics.GetInteger();
	idFile*				file = NULL;
	int					fileLength = 0;
	sndFileHeader_t		header;
	byte*				block = NULL;
	idSoundAcoustics*	it = NULL;

	// section pointers into the block and their counts, from step 7
	const sndBakeArea_t*	areas = NULL;
	const int32*			cells = NULL;
	const sndBakeProbe_t*	probes = NULL;
	const sndReverb_t*		zones = NULL;
	const sndBakeDoor_t*	doors = NULL;
	const char*				doorStrings = NULL;
	int						numAreas = 0, numCells = 0, numProbes = 0, numZones = 0, numDoors = 0, doorStringBytes = 0;

	state[0] = '\0';
	do
	{
		// step 1
		if( mode == 0 )
		{
			idStr::Copynz( state, "off (s_acoustics 0)", sizeof( state ) );
			break;
		}

		// step 2 is retired (DR-84); step 3
		if( !soundSystemLocal.hardware.SupportsReverbCarrier() )	//puncho - SB4-C (D-47): two pointer tests on every load
		{
			idStr::Copynz( state, "off (no carrier)", sizeof( state ) );
			break;
		}

		// step 4: on every load that passed steps 1 and 3; a miss is the whole cost of a map with no bake (C-SB2b)
		idStrStatic< MAX_OSPATH > path = mapName;
		path += ".sndbake";
		file = fileSystem->OpenFileRead( path );
		if( file == NULL )
		{
			idStr::Copynz( state, "off (no bake)", sizeof( state ) );
			break;
		}
		acousticsLoadFile = file;		//puncho SB3-W (W4-3): reachable from the world until the load releases it

		// step 5: the fixed checks of R-SB3-6, cheapest first; a file under 112 B fails at magic
		fileLength = file->Length();
		memset( &header, 0, sizeof( header ) );
		const int headerRead = file->Read( &header, SND_HEADER_SIZE );
		const char* staleField = NULL;
		if( headerRead != SND_HEADER_SIZE || header.magic != SND_MAGIC_BAKE )
		{
			staleField = "magic";
		}
		else if( header.version != SND_FORMAT_VERSION )
		{
			staleField = "version";
		}
		else if( header.bakerRevision != SND_BAKER_REVISION_BAKE )
		{
			staleField = "bakerRevision";
		}
		else if( header.toolVersion != SND_TOOL_VERSION )
		{
			staleField = "toolVersion";
		}
		else if( header.numBands != SND_NUM_BANDS )
		{
			staleField = "numBands";
		}
		else if( ( uint64 )SND_HEADER_SIZE + header.payloadSize != ( uint64 )fileLength )
		{
			staleField = "payloadSize";
		}
		// numSections before step1Bytes: step1Bytes' lower bound is computed from it (L3-4)
		else if( header.numSections == 0 || header.numSections > SND_MAX_SECTIONS )
		{
			staleField = "numSections";
		}
		else if( ( uint64 )header.step1Bytes < ( uint64 )SND_HEADER_SIZE + ( uint64 )SND_SECTION_SIZE * header.numSections
				 || ( uint64 )header.step1Bytes > ( uint64 )fileLength || ( header.step1Bytes % 16 ) != 0 )
		{
			staleField = "step1Bytes";
		}
		else if( !SndSettingsValid( header.settings )
				 || !SndFloatsFinite( &header.settings.duration, ( int )sizeof( header.settings.duration ) )
				 || !SndFloatsFinite( &header.settings.irradianceMinDistance, ( int )sizeof( header.settings.irradianceMinDistance ) ) )
		{
			staleField = "settings";
		}
		else if( header.mapGeometryCRC != mapGeometryCRC )
		{
			staleField = "mapGeometryCRC";
		}
		if( staleField == NULL )
		{
			// step 5a: only now, every cheaper check passed, the render world computes the structure id (C-SB5g);
			// a real id is never 0, so 0 (an empty or failed world) never matches
			const uint64 structureId = renderWorld->GetMapCompileId();
			if( structureId == 0 || structureId != header.compileId )
			{
				staleField = "structure";
			}
		}
		if( staleField != NULL )
		{
			idStr::snPrintf( state, sizeof( state ), "off (stale: %s)", staleField );
			break;
		}

		// step 6: the step 1 block, in place (R-SB3-5)
		const int step1Bytes = ( int )header.step1Bytes;
		block = ( byte* )Mem_Alloc16( step1Bytes, TAG_AUDIO );
		if( block == NULL )
		{
			idStr::Copynz( state, "off (no memory)", sizeof( state ) );
			break;
		}
		acousticsLoadBlock = block;		//puncho SB3-W (W4-3): reachable from the world until step 10 hands it on
		memcpy( block, &header, SND_HEADER_SIZE );
		int done = SND_HEADER_SIZE;
		while( done < step1Bytes )
		{
			const int chunk = Min( step1Bytes - done, SND_ACOUSTICS_READ_CHUNK );
			if( file->Read( block + done, chunk ) != chunk )
			{
				break;
			}
			done += chunk;
			common->UpdateLevelLoadPacifier();
		}
		if( done != step1Bytes )
		{
			idStr::Copynz( state, "off (stale: payloadSize)", sizeof( state ) );
			break;
		}

		// step 7: the directory and the step 1 sections, R-SB3-7. The file's own test is SndBakeStep1, the one the
		// compiler keeps or skips a bake by; step 5 established what it relies on (magic, version, numSections at
		// most SND_MAX_SECTIONS, the directory inside step1Bytes, step1Bytes inside the file). Then the two tests
		// against this render world
		sndBakeStep1_t step1;
		const uint32 badSection = SndBakeStep1( block, step1Bytes, fileLength, step1 );
		if( badSection != 0 )
		{
			SndAcoustics_SectionStale( state, sizeof( state ), badSection );
			break;
		}
		areas = step1.areas;
		numAreas = step1.numAreas;
		cells = step1.cells;
		numCells = step1.numCells;
		probes = step1.probes;
		numProbes = step1.numProbes;
		zones = step1.zones;
		numZones = step1.numZones;
		doors = step1.doors;
		numDoors = step1.numDoors;
		doorStrings = step1.doorStrings;
		doorStringBytes = step1.doorStringBytes;
		if( numAreas != renderWorld->NumAreas() )
		{
			SndAcoustics_SectionStale( state, sizeof( state ), BAK_AREAS );
			break;
		}
		const int numRenderPortals = renderWorld->NumPortals();
		bool doorsBad = false;
		for( int d = 0; d < numDoors && !doorsBad; d++ )
		{
			doorsBad = ( doors[d].portal >= numRenderPortals );
		}
		if( doorsBad )
		{
			SndAcoustics_SectionStale( state, sizeof( state ), BAK_DOORS );
			break;
		}

		//puncho SB3-W: step 8
		// the working-tree checks, R-SB3-8, -33, -9 (C-SB3); a packaged game with no mod runs none. The state gets
		// only what acousticsLine still holds after "acoustics: <map> ", so a named record is cut there, keeping its
		// closing parenthesis (W4-5). Limit: a map name up to 67 characters leaves room for the fixed part of every named
		// state; past that (out of scope, W4R-6) idStr::snPrintf prints its overflow warning and the line loses its
		// end. No clamp avoids it: the whole line is what does not fit
		const int lineRoom = ( int )sizeof( acousticsLine ) - idStr::Length( "acoustics:  " ) - idStr::Length( mapName );
		const int stateRoom = Max( 1, Min( ( int )sizeof( state ), lineRoom ) );
		if( !SndAcoustics_WorkingTree( file, fileLength, block, step1Bytes, step1.devcheck, mapFile, acousticsLoadScratch, state, stateRoom ) )
		{
			break;
		}

		//puncho SB3-W: step 9
		// the portal graph, R-SB3-25 to -27 (C-SB5a, C-SB5b). Edges in area order, then GetPortal order: one per
		// directed portal, so 2 x NumPortals(). One block: edges, area records, states, door uses, in that order
		int numGraphEdges = 0;
		for( int a = 0; a < numAreas; a++ )
		{
			numGraphEdges += renderWorld->NumPortalsInArea( a );
		}
		const int graphEdgeBytes = numGraphEdges * ( int )sizeof( sndPortalEdge_t );
		const int graphAreaBytes = numAreas * ( int )sizeof( sndPortalArea_t );
		byte* graph = ( byte* )Mem_Alloc16( graphEdgeBytes + graphAreaBytes + numRenderPortals + numDoors, TAG_AUDIO );
		if( graph == NULL )
		{
			idStr::Copynz( state, "off (no memory)", sizeof( state ) );
			break;
		}
		sndPortalEdge_t* graphEdges = ( sndPortalEdge_t* )graph;
		sndPortalArea_t* graphAreas = ( sndPortalArea_t* )( graph + graphEdgeBytes );
		uint8* graphStates = graph + graphEdgeBytes + graphAreaBytes;
		uint8* graphDoorUse = graphStates + numRenderPortals;
		int maxAreaEdges = 0;
		int maxAreaEdgesArea = 0;
		int edge = 0;
		for( int a = 0; a < numAreas; a++ )
		{
			const int numAreaEdges = renderWorld->NumPortalsInArea( a );
			graphAreas[a].firstEdge = edge;
			graphAreas[a].numEdges = numAreaEdges;
			if( numAreaEdges > maxAreaEdges )
			{
				maxAreaEdges = numAreaEdges;
				maxAreaEdgesArea = a;
			}
			for( int i = 0; i < numAreaEdges; i++ )
			{
				// GetPortal walks the area's list: k^2 / 2 steps per area (C-SB5a)
				const exitPortal_t portal = renderWorld->GetPortal( a, i );
				sndPortalEdge_t& e = graphEdges[edge++];
				// the winding as the render world keeps it: its plane's side is not promised (R-SB3-30)
				e.w = portal.w;
				portal.w->GetPlane( e.plane );
				e.center = portal.w->GetCenter();
				portal.w->GetBounds( e.bounds );
				e.otherArea = portal.areas[1];
				e.portal = portal.portalHandle - 1;
				e.door = -1;
				e.flags = 0;
			}
		}
		for( int p = 0; p < numRenderPortals; p++ )
		{
			graphStates[p] = ( uint8 )renderWorld->GetPortalState( p + 1 );
		}
		// R-SB3-27: doors in file order; the first record on a portal keeps it. No game entity is read, the gate stays on.
		// The first 8 mismatches warn one by one, the rest in one line: a file's count is not bounded (W4-6)
		static const int SND_DOOR_WARNINGS = 8;
		int doorMismatches = 0;
		assert( numDoors <= 0x8000 );		// step 7: every index fits an edge's int16
		for( int d = 0; d < numDoors; d++ )
		{
			const sndBakeDoor_t& door = doors[d];
			// portal p joins area0 and area1, in either order, exactly when area0's list holds p's edge into area1
			int forward = -1;
			int reverse = -1;
			for( int j = graphAreas[door.area0].firstEdge, end = j + graphAreas[door.area0].numEdges; j < end && forward == -1; j++ )
			{
				forward = ( graphEdges[j].portal == door.portal ) ? j : -1;
			}
			for( int j = graphAreas[door.area1].firstEdge, end = j + graphAreas[door.area1].numEdges; j < end && reverse == -1; j++ )
			{
				reverse = ( graphEdges[j].portal == door.portal ) ? j : -1;
			}
			if( forward == -1 || reverse == -1 || graphEdges[forward].otherArea != door.area1 )
			{
				graphDoorUse[d] = SND_DOOR_MISMATCH;
				if( doorMismatches < SND_DOOR_WARNINGS )
				{
					common->Warning( "acoustics: %s door %d '%s' names portal %d between areas %d and %d, which it does not join; the door is not used",
									 mapName, d, doorStrings + door.nameOffset, door.portal, door.area0, door.area1 );
				}
				doorMismatches++;
			}
			else if( graphEdges[forward].door != -1 )
			{
				graphDoorUse[d] = SND_DOOR_SHADOWED;
			}
			else
			{
				graphEdges[forward].door = ( int16 )d;
				graphEdges[reverse].door = ( int16 )d;
				graphDoorUse[d] = SND_DOOR_USED;
			}
		}
		if( doorMismatches > SND_DOOR_WARNINGS )
		{
			common->Warning( "acoustics: %s %d more doors name a portal between areas it does not join; they are not used",
							 mapName, doorMismatches - SND_DOOR_WARNINGS );
		}

		// step 10: the object holds the block and step 9's graph from here
		it = new( TAG_AUDIO ) idSoundAcoustics;
		it->block = block;
		it->blockBytes = step1Bytes;
		block = NULL;
		acousticsLoadBlock = NULL;	//puncho SB3-W (W4-3): the object holds it from here
		it->areas = areas;
		it->cells = cells;
		it->probes = probes;
		it->zones = zones;
		it->doors = doors;
		it->doorStrings = doorStrings;
		it->numAreas = numAreas;
		it->numCells = numCells;
		it->numProbes = numProbes;
		it->numZones = numZones;
		it->numDoors = numDoors;
		it->doorStringBytes = doorStringBytes;
		it->graph = graph;			//puncho SB3-W: step 9's graph, freed with the object by DeleteAcoustics
		it->edges = graphEdges;
		it->areaEdges = graphAreas;
		it->states = graphStates;
		it->doorUse = graphDoorUse;
		it->numEdges = numGraphEdges;
		it->numPortals = numRenderPortals;
		it->maxAreaEdges = maxAreaEdges;
		it->maxAreaEdgesArea = maxAreaEdgesArea;
		it->statesChanged = false;
		it->armed = false;			// DR-52: armed at the load cut, R-SB3-13
		it->mesh = NULL;
		it->mapName = mapName;

		// step 11: the carrier's graph made or an idle one reused, then SB4's begin hook
		const bool carrier = soundSystemLocal.hardware.AcquireReverbCarrier();	//puncho - SB4-C (D-47): raises no Error
		if( !carrier )
		{
			DeleteAcoustics( it );
			it = NULL;
			idStr::Copynz( state, "off (no carrier)", sizeof( state ) );
			break;
		}
		reverb.OnMapAcousticsLoaded( *it );	//puncho - SB4-L (D-48, TW-20): the director's line; raises no Error (D-43)
	}
	while( false );

	//puncho SB3-W (W4-3): closes the file and, on an off, frees the block the object never took
	ReleaseAcousticsLoad();

	// R-SB3-34: the one exit of every off of steps 1 to 9 and 11
	if( it == NULL )
	{
		idStr::snPrintf( acousticsLine, sizeof( acousticsLine ), "acoustics: %s %s", mapName, state );
		if( mode == 2 )
		{
			common->Warning( "%s", acousticsLine );
		}
		else
		{
			common->Printf( "%s\n", acousticsLine );
		}
		soundSystemLocal.hardware.ReleaseIdleReverbGraph();	//puncho - SB4-C (R-SB4-25): one compare while no voice lists the bus or is pending
		return;
	}

	// step 12: SA3's begin hook creates the world's occlusion object
	idSoundOcclusion::OnMapAcousticsLoaded( *this, *it );	//puncho - SA3-E (R-SA3-23)

	// step 13
	acoustics = it;
	idStr::snPrintf( acousticsLine, sizeof( acousticsLine ), "acoustics: %s on (%d probes, %d zones)", mapName, it->numProbes, it->numZones );
	common->Printf( "%s\n", acousticsLine );
	common->DPrintf( "acoustics: %s at most %d portal edges in one area (area %d)\n", mapName, it->maxAreaEdges, it->maxAreaEdgesArea );
}

/*
========================
idSoundWorldLocal::FreeMapAcoustics

R-SB3-15, at map shutdown inside LOAD_UNLOAD. The pointer goes first, so nothing reads the object while the hooks
run. Stores the line; prints nothing.
========================
*/
void idSoundWorldLocal::FreeMapAcoustics()
{
	// W4-3: an Error raised inside the load reaches here through Stop, UnloadMap and the game's MapShutdown before
	// its exception unwinds the load: the file closes and the block and step 8's buffer are freed now. Right only
	// because every path that reaches here while LoadMapAcoustics is on the stack ends in a throw (today one re-entry
	// point, the pacifier at step 6); a caller that returned instead would leave the load reading a freed block (W4R-5)
	ReleaseAcousticsLoad();
	if( acoustics == NULL )
	{
		return;
	}
	idSoundAcoustics* it = acoustics;
	acoustics = NULL;
	idSoundOcclusion::OnMapAcousticsFreed( *this );	//puncho - SA3-E (R-SA3-23)
	reverb.OnMapAcousticsFreed();	//puncho - SB4-L (D-48, TW-20): the director's line; before the object is deleted
	idStr::snPrintf( acousticsLine, sizeof( acousticsLine ), "acoustics: %s off (unloaded)", it->mapName.c_str() );
	DeleteAcoustics( it );
}

/*
========================
idSoundWorldLocal::DeleteAcoustics

L3-3: the one teardown of the object, for FreeMapAcoustics, a load that fails after step 10 and the world's
destructor. The callers run their hooks first.
========================
*/
void idSoundWorldLocal::DeleteAcoustics( idSoundAcoustics* it )
{
	// nothing fills mesh yet; the lane that reads the .sndmesh into it frees it here
	assert( it->mesh == NULL );
	Mem_Free16( it->graph );
	Mem_Free16( it->block );
	delete it;
}

/*
========================
idSoundWorldLocal::ReleaseAcousticsLoad

SB3-W (W4-3): the one release of what a load in progress holds: the open .sndbake, the step 1 block before step 10
takes it, step 8's buffer. The load's own exit calls it; so do step 0 of the next load, FreeMapAcoustics (which an
Error inside the load reaches through Stop before its exception unwinds) and the world's destructor. Three NULL
tests when nothing is held.
========================
*/
void idSoundWorldLocal::ReleaseAcousticsLoad()
{
	if( acousticsLoadFile != NULL )
	{
		fileSystem->CloseFile( acousticsLoadFile );
		acousticsLoadFile = NULL;
	}
	if( acousticsLoadBlock != NULL )
	{
		Mem_Free16( acousticsLoadBlock );
		acousticsLoadBlock = NULL;
	}
	if( acousticsLoadScratch != NULL )
	{
		Mem_Free16( acousticsLoadScratch );
		acousticsLoadScratch = NULL;
	}
}
