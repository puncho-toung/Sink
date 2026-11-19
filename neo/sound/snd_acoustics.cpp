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

//puncho - SB3-T: neither tool is archived or network-synced (R-SB3-16, R-SB3-18)
idCVar s_acousticsShow( "s_acousticsShow", "0", CVAR_INTEGER, "draw baked acoustics: 1 probes, 2 cells, 4 zones, 8 doors, 16 plugs" );
idCVar s_acousticsLog( "s_acousticsLog", "", 0, "write acousticslog/<plain name>.csv under fs_savepath; empty closes the log" );

static const int SND_ACOUSTICS_READ_CHUNK = 1024 * 1024;	// step 6 reads the block in 1 MB pieces, the pacifier between

//puncho start - SB3-T (R-SB3-18, R-SB3-31, DR-209, DR-210, DR-211): main-thread diagnostic log
static const int ACOUSTICS_LOG_BUFFER_SIZE = 64 * 1024;
static char* acousticsLogBuffer = NULL;
static int acousticsLogUsed = 0;
static uint64 acousticsLogFrame = 0;	// DR-211: next F row's number, used only while the file is open
static uint64 acousticsLogLastFlushUs = 0;
static char acousticsLogName[128];

// No pointer to a channel, voice or bake survives L1. The world pointer is compared only, never dereferenced.
struct acousticsLogWorld_t
{
	const idSoundWorldLocal*	world;
	float					dtMs;
	float					pos[3];
	int						ms;
	int						gate;
	int						cutReasons;
	int						held;
	int						area;
	bool					updated;
	bool					portalStatesChanged;	//puncho - SB3-T (TL8-1): this L1's flag, consumed with its F row
};
static acousticsLogWorld_t acousticsLogSnapshot = {};

static const char acousticsLogHeaders[] =
	"F,frame,ms,worldUpdated,dtMs,gate,cutReasons,held,lx,ly,lz,area,r_tickMs,r_dtMs,r_cut,r_held,r_area,r_areasVisited,r_cellsRead,r_probesWeighted,r_weightSum,r_floorWeight,r_target0,r_target1,r_target2,r_target3,r_target4,r_smoothed0,r_smoothed1,r_smoothed2,r_smoothed3,r_smoothed4,r_zone,r_candidate,r_dwellMs,r_pendingZone,r_fadeProgress,r_draining,r_voicesWithBus,portalStatesChanged\n"
	"S,frame,submix,state,bus,return,enabled,WetDryMix,ReflectionsDelay,ReverbDelay,RearDelay,SideDelay,PositionLeft,PositionRight,PositionMatrixLeft,PositionMatrixRight,EarlyDiffusion,LateDiffusion,LowEQGain,LowEQCutoff,HighEQGain,HighEQCutoff,RoomFilterFreq,RoomFilterMain,RoomFilterHF,ReflectionsGain,ReverbGain,DecayTime,Density,RoomSize,DisableLateField\n"
	"V,frame,emitter,channel,shader,attenTgt,attenDb,occTgt,occ,held,volumeDb,gain,offTx,offTy,offTz,offX,offY,offZ,portalLossDb,portalOcclusion,routes,occFlags,loss0,loss1,loss2,reverbSend\n";

// A full buffer costs one Write, without a per-row file flush. Clearing used before the Write also prevents a
// short write from being retried by Close and duplicating the prefix the file may already have accepted.
static bool AcousticsLog_WriteBuffer()
{
	if( acousticsLogUsed == 0 )
	{
		return true;
	}
	const int bytes = acousticsLogUsed;
	acousticsLogUsed = 0;
	if( soundSystemLocal.acousticsLog->Write( acousticsLogBuffer, bytes ) != bytes )
	{
		idLib::Warning( "s_acousticsLog: writing acousticslog/%s.csv failed; the log is off", acousticsLogName );
		s_acousticsLog.SetString( "" );
		return false;
	}
	return true;
}

void AcousticsLog_Close()
{
	if( soundSystemLocal.acousticsLog == NULL && acousticsLogBuffer == NULL )	//puncho - SB3-T (OP8-2 (a))
	{
		return;
	}
	if( soundSystemLocal.acousticsLog != NULL )
	{
		AcousticsLog_WriteBuffer();
		soundSystemLocal.acousticsLog->Flush();
		fileSystem->CloseFile( soundSystemLocal.acousticsLog );
		soundSystemLocal.acousticsLog = NULL;
	}
	Mem_Free( acousticsLogBuffer );
	acousticsLogBuffer = NULL;
	acousticsLogUsed = 0;
	acousticsLogFrame = 0;
	acousticsLogLastFlushUs = 0;
	acousticsLogName[0] = '\0';
	memset( &acousticsLogSnapshot, 0, sizeof( acousticsLogSnapshot ) );
	soundSystemLocal.acousticsLogWorld = NULL;
}

static bool AcousticsLog_AppendRow( const char* row, int bytes )
{
	assert( bytes >= 0 && bytes <= ACOUSTICS_LOG_BUFFER_SIZE );
	if( bytes > ACOUSTICS_LOG_BUFFER_SIZE - acousticsLogUsed )
	{
		if( !AcousticsLog_WriteBuffer() )
		{
			AcousticsLog_Close();
			return false;
		}
	}
	memcpy( acousticsLogBuffer + acousticsLogUsed, row, bytes );
	acousticsLogUsed += bytes;
	return true;
}

//puncho - SB3-T (OP8-1): keep the list's count out of Update's hardware-loop register allocation
void idSoundWorldLocal::AcousticsLog_World( const idStaticList< idActiveChannel, MAX_HARDWARE_VOICES >& channels, int currentTime ) const
{
	// Render owns opening, switching and closing. A cvar edit before this update must not append orphan V rows to
	// the old file; the first Render with the new name will publish the world for the following update (D-8).
	if( soundSystemLocal.acousticsLog == NULL || soundSystemLocal.acousticsGameWorld != this
			|| idStr::Cmp( s_acousticsLog.GetString(), acousticsLogName ) != 0 )
	{
		return;
	}
	acousticsLogSnapshot.world = this;
	acousticsLogSnapshot.ms = currentTime;
	acousticsLogSnapshot.dtMs = updateDtMs;
	acousticsLogSnapshot.cutReasons = listenerCutReasons;
	acousticsLogSnapshot.gate = acoustics == NULL ? 0 : ( acoustics->IsArmed() ? 2 : 1 );
	acousticsLogSnapshot.portalStatesChanged = acousticsLogSnapshot.gate == 2 && acoustics->PortalStatesChanged();	//puncho - SB3-T (TL8-1)
	for( int k = 0; k < 3; k++ )
	{
		acousticsLogSnapshot.pos[k] = listener.pos[k];
	}
	if( acousticsLogSnapshot.gate == 2 )
	{
		acousticsLogSnapshot.held = listenerAreaHeld ? 1 : 0;
		acousticsLogSnapshot.area = listener.area;
	}
	acousticsLogSnapshot.updated = true;

	// A canonical decl name is shorter than MAX_STRING_CHARS (DeclManager's canonicalName). Even doubling every
	// quote leaves room for all 25 separators and the round-trip float spellings in this fixed 4 KB row.
	char row[4096];
	for( int i = 0; i < channels.Num(); i++ )
	{
		const idSoundChannel* chan = channels[i].channel;
		const idSoundEmitterLocal* emitter = chan->emitter;
		const idSoundWorldLocal* world = emitter->soundWorld;
		const char* shader = chan->soundShader->GetName();
		if( idStr::Length( shader ) >= MAX_STRING_CHARS )
		{
			idLib::Warning( "s_acousticsLog: shader name exceeds the decl name bound; the log is off" );
			s_acousticsLog.SetString( "" );
			AcousticsLog_Close();
			return;
		}
		// DR-211: Render appends the F row with this same number, then advances it.
		int bytes = idStr::snPrintf( row, sizeof( row ), "V,%llu,%d,%d,\"", ( unsigned long long )acousticsLogFrame, emitter->index, chan->logicalChannel );
		for( const char* p = shader; *p != '\0'; p++ )
		{
			if( *p == '"' )
			{
				row[bytes++] = '"';
			}
			row[bytes++] = *p;
		}
		row[bytes++] = '"';
		const char* held = acousticsLogSnapshot.gate == 2 ? ( world->listenerAreaHeld ? "1" : "0" ) : "";
		bytes += idStr::snPrintf( row + bytes, ( int )sizeof( row ) - bytes, ",%.9g,%.9g,%.9g,%.9g,%s,%.9g,",
								 chan->excessTargetDB, chan->excessDB.value, chan->occTarget, chan->occlusion.value, held, chan->volumeDB );
		if( chan->hardwareVoice != NULL )
		{
			bytes += idStr::snPrintf( row + bytes, ( int )sizeof( row ) - bytes, "%.9g", chan->hardwareVoice->GetGain() );
		}
		// DR-211: the same target and tests as E6's UpdateHardware, including its zero for omni channels.
		const bool global = ( chan->parms.soundShaderFlags & SSF_GLOBAL ) != 0;
		const bool omni = ( chan->parms.soundShaderFlags & SSF_OMNIDIRECTIONAL ) != 0;
		const bool emitterIsListener = emitter->emitterId == world->listener.id;
		const bool occluded = !global && !emitterIsListener && ( chan->parms.soundShaderFlags & SSF_NO_OCCLUSION ) == 0;
		const idVec3 target = ( occluded && !omni ) ? ( emitter->spatializedOrigin - emitter->origin ) : vec3_zero;
		bytes += idStr::snPrintf( row + bytes, ( int )sizeof( row ) - bytes, ",%.9g,%.9g,%.9g", target.x, target.y, target.z );
		bytes += idStr::snPrintf( row + bytes, ( int )sizeof( row ) - bytes, ",%.9g,%.9g,%.9g,%.9g,%.9g",
								 chan->panOffset[0].value, chan->panOffset[1].value, chan->panOffset[2].value,
								 emitter->portalLossDb, emitter->portalOcclusion );
		sndOccLogVoice_t record;
		// D-51 / DR-209: this emitter record repeats on every channel, including global and SSF_NO_OCCLUSION.
		if( world->occlusion != NULL && world->occlusion->GetLogVoice( emitter, record ) )
		{
			bytes += idStr::snPrintf( row + bytes, ( int )sizeof( row ) - bytes, ",%u,%u,%.9g,%.9g,%.9g",
									 ( unsigned int )record.numRoutes, ( unsigned int )record.flags, record.lossDb[0], record.lossDb[1], record.lossDb[2] );
		}
		else
		{
			memcpy( row + bytes, ",,,,,", 5 );
			bytes += 5;
		}
		row[bytes++] = ',';
		if( chan->hardwareVoice != NULL )
		{
			bytes += idStr::snPrintf( row + bytes, ( int )sizeof( row ) - bytes, "%.9g", chan->hardwareVoice->GetReverbSend() );
		}
		row[bytes++] = '\n';
		if( !AcousticsLog_AppendRow( row, bytes ) )
		{
			return;
		}
	}
}

void AcousticsLog_Frame()
{
	const char* name = s_acousticsLog.GetString();
	if( name[0] == '\0' )
	{
		AcousticsLog_Close();
		return;
	}
	if( soundSystemLocal.acousticsLog != NULL && idStr::Cmp( name, acousticsLogName ) != 0 )
	{
		AcousticsLog_Close();
		// A failed final write clears the cvar and may invalidate the previous string pointer.
		name = s_acousticsLog.GetString();
		if( name[0] == '\0' )
		{
			return;
		}
	}
	const uint64 nowUs = Sys_Microseconds();
	if( soundSystemLocal.acousticsLog == NULL )
	{
		if( !ValidDiagnosticName( name ) )
		{
			idLib::Warning( "s_acousticsLog requires a plain name" );
			s_acousticsLog.SetString( "" );
			AcousticsLog_Close();
			return;
		}
		char path[160];
		idStr::snPrintf( path, sizeof( path ), "acousticslog/%s.csv", name );
		//puncho - SB3-T (DR-212 item 1): Windows OpenFileAppend opens with OPEN_ALWAYS, but does not seek to EOF.
		soundSystemLocal.acousticsLog = fileSystem->OpenFileAppend( path, false, "fs_savepath" );
		if( soundSystemLocal.acousticsLog == NULL )
		{
			idLib::Warning( "s_acousticsLog: could not open %s", path );
			s_acousticsLog.SetString( "" );
			AcousticsLog_Close();
			return;
		}
		if( soundSystemLocal.acousticsLog->Seek( 0, FS_SEEK_END ) != 0 )
		{
			idLib::Warning( "s_acousticsLog: could not seek to the end of %s", path );
			s_acousticsLog.SetString( "" );
			AcousticsLog_Close();
			return;
		}
		acousticsLogBuffer = ( char* )Mem_Alloc( ACOUSTICS_LOG_BUFFER_SIZE, TAG_AUDIO );
		if( acousticsLogBuffer == NULL )
		{
			idLib::Warning( "s_acousticsLog: could not allocate the 64 KB buffer" );
			s_acousticsLog.SetString( "" );
			AcousticsLog_Close();
			return;
		}
		idStr::Copynz( acousticsLogName, name, sizeof( acousticsLogName ) );
		acousticsLogFrame = 0;
		acousticsLogLastFlushUs = nowUs;
		//puncho - SB3-T (TL8-4): every open starts a new header block before frame numbering restarts at 0.
		AcousticsLog_AppendRow( acousticsLogHeaders, ( int )sizeof( acousticsLogHeaders ) - 1 );
	}

	// D-8: only Render publishes the world. The destructor's NULL also invalidates a snapshot if an address is
	// reused before the next Render. No stale world pointer is dereferenced here.
	if( soundSystemLocal.acousticsLogWorld != soundSystemLocal.acousticsGameWorld )
	{
		memset( &acousticsLogSnapshot, 0, sizeof( acousticsLogSnapshot ) );
	}
	soundSystemLocal.acousticsLogWorld = soundSystemLocal.acousticsGameWorld;
	const idSoundWorldLocal* world = soundSystemLocal.acousticsLogWorld;
	const idSoundAcoustics* acoustics = world != NULL ? world->acoustics : NULL;
	const int gate = acoustics == NULL ? 0 : ( acoustics->IsArmed() ? 2 : 1 );
	char row[512];
	int bytes;
	// DR-211: ms is the last L1's emitter time, retained with the other stashed values when no update ran.
	// Recheck the live gate to mask area/held after an unload; paused rows keep the snapshot with worldUpdated 0.
	if( world != NULL && acousticsLogSnapshot.world == world )
	{
		char area[16] = "";
		const char* held = "";
		if( gate == 2 && acousticsLogSnapshot.gate == 2 )
		{
			idStr::snPrintf( area, sizeof( area ), "%d", acousticsLogSnapshot.area );
			held = acousticsLogSnapshot.held ? "1" : "0";
		}
		bytes = idStr::snPrintf( row, sizeof( row ), "F,%llu,%d,%d,%.9g,%d,%d,%s,%.9g,%.9g,%.9g,%s",
							   ( unsigned long long )acousticsLogFrame, acousticsLogSnapshot.ms,
							   acousticsLogSnapshot.updated ? 1 : 0, acousticsLogSnapshot.dtMs, gate, acousticsLogSnapshot.cutReasons, held,
							   acousticsLogSnapshot.pos[0], acousticsLogSnapshot.pos[1], acousticsLogSnapshot.pos[2], area );
	}
	else
	{
		// The opening Render (or a newly selected world) has not seen L1 yet; there is no world snapshot to invent.
		bytes = idStr::snPrintf( row, sizeof( row ), "F,%llu,,0,,%d,,,,,,", ( unsigned long long )acousticsLogFrame, gate );
	}
	// STAND-IN SB4-Z / DR-209: exactly 27 empty reverbLogFrame_t columns, tickMs through voicesWithBus. submix[]
	// belongs to the S rows, whose condition and values are also unavailable until SB4-Z. No S row is emitted yet.
	for( int c = 0; c < 27; c++ )
	{
		row[bytes++] = ',';
	}
	//puncho - SB3-T (TL8-1): no new portal-change event on opening, paused or otherwise unupdated rows.
	bytes += idStr::snPrintf( row + bytes, ( int )sizeof( row ) - bytes, ",%d\n",
							 world != NULL && acousticsLogSnapshot.world == world && acousticsLogSnapshot.updated && acousticsLogSnapshot.portalStatesChanged ? 1 : 0 );
	if( !AcousticsLog_AppendRow( row, bytes ) )
	{
		return;
	}
	acousticsLogFrame++;
	acousticsLogSnapshot.updated = false;
	if( nowUs - acousticsLogLastFlushUs >= 1000000 )
	{
		if( !AcousticsLog_WriteBuffer() )
		{
			AcousticsLog_Close();
			return;
		}
		soundSystemLocal.acousticsLog->Flush();
		acousticsLogLastFlushUs = nowUs;
	}
}
//puncho end

//puncho start - SB3-T (R-SB3-17): command and the s_showVoices marker, no per-frame work while off
const char* idSoundWorldLocal::AcousticsState() const
{
	// The stored line also names the map. Keep the loader's complete stale-record explanation.
	const char* state = strstr( acousticsLine, " off (" );
	if( state == NULL )
	{
		state = strstr( acousticsLine, " on (" );
	}
	return ( state != NULL ) ? state + 1 : "off (no map)";
}

static void SndAcoustics_PrintReverb( const sndReverb_t& r )
{
	common->Printf( " decayTime %.6g %.6g %.6g earlyDb %.6g lateDb %.6g preDelay %.6g lateDelay %.6g roomSize %.6g\n",
					r.decayTime[0], r.decayTime[1], r.decayTime[2], r.earlyDb, r.lateDb, r.preDelay, r.lateDelay, r.roomSize );
}

void idSoundWorldLocal::ListAcoustics( const idCmdArgs& args ) const
{
	common->Printf( "%s\n", acousticsLine[0] != '\0' ? acousticsLine : "acoustics: off (no map)" );
	if( acoustics == NULL )
	{
		return;
	}
	const idSoundAcoustics& a = *acoustics;
	const sndFileHeader_t& h = a.GetHeader();
	common->Printf( "armed %d file %s.sndbake step1Bytes %u length %llu\n", a.IsArmed() ? 1 : 0, a.GetMapName(), h.step1Bytes,
					( unsigned long long )SND_HEADER_SIZE + h.payloadSize );
	common->Printf( "magic 0x%08x version %u compileId 0x%016llx mapGeometryCRC 0x%08x\n", h.magic, h.version,
					( unsigned long long )h.compileId, h.mapGeometryCRC );
	common->Printf( "bakerRevision 0x%08x toolVersion 0x%08x numBands %u entityKeysCRC 0x%08x materialHash 0x%08x inputHash 0x%016llx\n",
					h.bakerRevision, h.toolVersion, h.numBands, h.entityKeysCRC, h.materialHash, ( unsigned long long )h.inputHash );
	const sndSettings_t& s = h.settings;
	common->Printf( "settings probeSpacing %d probeSpacingZ %d probeClearance %d sealDoorways %d staticTriBudget %d preset %d\n",
					s.probeSpacing, s.probeSpacingZ, s.probeClearance, s.sealDoorways, s.staticTriBudget, s.preset );
	common->Printf( "settings rays %d bounces %d duration %.6g ambisonicOrder %d diffuseSamples %d irradianceMinDistance %.6g\n",
					s.rays, s.bounces, s.duration, s.ambisonicOrder, s.diffuseSamples, s.irradianceMinDistance );
	common->Printf( "payloadSize %u payloadCRC 0x%08x numSections %u\n", h.payloadSize, h.payloadCRC, h.numSections );
	const sndSection_t* sections = ( const sndSection_t* )( a.block + SND_HEADER_SIZE );
	for( uint32 i = 0; i < h.numSections; i++ )
	{
		common->Printf( "section 0x%x offset %u size %u count %u\n", sections[i].id, sections[i].offset, sections[i].size, sections[i].count );
	}
	common->Printf( "areas %d cells %d probes %d zones %d doors %d strings %d bytes\n",
					a.NumAreas(), a.NumCells(), a.NumProbes(), a.NumZones(), a.NumDoors(), a.doorStringBytes );
	common->Printf( "graph areas %d portals %d edges %d MaxAreaEdges %d area %d\n",
					a.NumAreas(), a.NumPortals(), a.NumEdges(), a.MaxAreaEdges(), a.MaxAreaEdgesArea() );
	// DR-209: the states, like the log frame, belong to SB4-Z.
	common->Printf( "effect and drain states come with SB4-Z\n" );
	const bool areas = idStr::Icmp( args.Argv( 1 ), "areas" ) == 0;
	const bool zones = idStr::Icmp( args.Argv( 1 ), "zones" ) == 0;
	const bool doors = idStr::Icmp( args.Argv( 1 ), "doors" ) == 0;
	const bool portals = idStr::Icmp( args.Argv( 1 ), "portals" ) == 0;
	for( int d = 0; d < a.NumDoors(); d++ )
	{
		const sndBakeDoor_t& door = a.GetDoors()[d];
		const sndDoorUse_t use = a.GetDoorUse( d );
		common->Printf( "door %d %s %s", d, a.GetDoorName( d ),
						use == SND_DOOR_USED ? "USED" : ( use == SND_DOOR_MISMATCH ? "MISMATCH" : "SHADOWED" ) );
		if( doors )
		{
			common->Printf( " portal %d areas %d %d lossDb %.6g %.6g %.6g", door.portal, door.area0, door.area1,
							door.lossDb[0], door.lossDb[1], door.lossDb[2] );
		}
		common->Printf( "\n" );
	}
	if( areas )
	{
		for( int i = 0; i < a.NumAreas(); i++ )
		{
			const sndBakeArea_t& area = a.GetAreas()[i];
			common->Printf( "area %d zone %d cellMin %d %d %d dims %d %d %d firstCell %d firstProbe %d probeCount %d firstEdge %d numEdges %d",
							i, area.zone, area.cellMin[0], area.cellMin[1], area.cellMin[2], area.dims[0], area.dims[1], area.dims[2],
							area.firstCell, area.firstProbe, area.probeCount, a.GetAreaEdges()[i].firstEdge, a.GetAreaEdges()[i].numEdges );
			SndAcoustics_PrintReverb( area.defaultReverb );
		}
	}
	if( zones )
	{
		for( int z = 0; z < a.NumZones(); z++ )
		{
			common->Printf( "zone %d", z );
			SndAcoustics_PrintReverb( a.GetZones()[z] );
		}
	}
	if( portals )
	{
		for( int i = 0; i < a.NumAreas(); i++ )
		{
			const sndPortalArea_t& area = a.GetAreaEdges()[i];
			for( int e = area.firstEdge; e < area.firstEdge + area.numEdges; e++ )
			{
				const sndPortalEdge_t& edge = a.GetEdges()[e];
				common->Printf( "edge %d area %d otherArea %d portal %d door %d state 0x%x points %d center %.6g %.6g %.6g\n",
								e, i, edge.otherArea, edge.portal, edge.door, ( unsigned int )a.GetPortalStates()[edge.portal],
								edge.w->GetNumPoints(), edge.center.x, edge.center.y, edge.center.z );
			}
		}
	}
}

void ListAcoustics_f( const idCmdArgs& args )
{
	//puncho - SB3-T (TL8-2): usage and hardware counts belong to the command, with or without a loaded bake/world.
	if( args.Argc() > 2 || ( args.Argc() == 2 && idStr::Icmp( args.Argv( 1 ), "areas" ) != 0
			&& idStr::Icmp( args.Argv( 1 ), "zones" ) != 0 && idStr::Icmp( args.Argv( 1 ), "doors" ) != 0
			&& idStr::Icmp( args.Argv( 1 ), "portals" ) != 0 ) )
	{
		common->Printf( "usage: listAcoustics [areas|zones|doors|portals]\n" );
	}
	const idSoundWorldLocal* world = soundSystemLocal.acousticsGameWorld;
	if( world != NULL )
	{
		world->ListAcoustics( args );
	}
	else
	{
		common->Printf( "acoustics: off (no map)\n" );
	}
	uint32 sourceVoices, submixVoices;
	if( soundSystemLocal.hardware.GetActiveVoiceCounts( sourceVoices, submixVoices ) )
	{
		common->Printf( "ActiveSourceVoiceCount %u ActiveSubmixVoiceCount %u\n", sourceVoices, submixVoices );
	}
	else
	{
		common->Printf( "ActiveSourceVoiceCount unavailable ActiveSubmixVoiceCount unavailable\n" );
	}
	soundSystemLocal.hardware.PrintReverbVoices();
}
//puncho end

//puncho start - SB3-T (R-SB3-16): one lazy mesh cache per bake, and shared drawing budgets per renderer frame
struct sndAcousticsMesh_t
{
	int numVerts;
	int numTris;
	int triOffset;
	int triMatOffset;
	// vertices immediately follow, then triangles and their material/plug flags
};
static sndAcousticsMesh_t acousticsMeshRefused = {};
static int acousticsDrawFrame = -1;
static int acousticsDrawBoxes = 0;
static int acousticsDrawTexts = 0;

// The directory and read scratch belong only to the first bit 16 draw, never Update's stack (D-46).
static sndAcousticsMesh_t* SndAcoustics_ReadMesh( const idSoundAcoustics& acoustics )
{
	idStrStatic< MAX_OSPATH > path = acoustics.GetMapName();
	path += ".sndmesh";
	idFile* file = fileSystem->OpenFileRead( path );
	sndAcousticsMesh_t* mesh = NULL;
	const char* refused = "open";
	do
	{
		if( file == NULL )
		{
			break;
		}
		sndFileHeader_t header;
		refused = "magic";
		if( file->Read( &header, SND_HEADER_SIZE ) != SND_HEADER_SIZE || header.magic != SND_MAGIC_MESH )
		{
			break;
		}
		refused = "version";
		if( header.version != SND_FORMAT_VERSION )
		{
			break;
		}
		refused = "compileId";
		if( header.compileId != acoustics.GetCompileId() )
		{
			break;
		}
		refused = "sections";
		const int length = file->Length();
		if( header.numSections == 0 || header.numSections > SND_MAX_SECTIONS
				|| ( uint64 )SND_HEADER_SIZE + header.payloadSize != ( uint64 )length )
		{
			break;
		}
		sndSection_t sections[SND_MAX_SECTIONS];
		const int dirBytes = ( int )header.numSections * SND_SECTION_SIZE;
		if( file->Read( sections, dirBytes ) != dirBytes )
		{
			break;
		}
		const sndSection_t* verts = NULL;
		const sndSection_t* tris = NULL;
		const sndSection_t* triMat = NULL;
		uint64 end = SND_HEADER_SIZE + dirBytes;
		bool valid = true;
		for( uint32 i = 0; i < header.numSections; i++ )
		{
			const sndSection_t& section = sections[i];
			if( section.offset < end || ( section.offset % 16 ) != 0
					|| ( uint64 )section.offset + section.size > ( uint64 )length
					|| ( i > 0 && section.id <= sections[i - 1].id ) )
			{
				valid = false;
				break;
			}
			end = ( uint64 )section.offset + section.size;
			if( section.id == MSH_VERTS )
			{
				verts = &section;
			}
			if( section.id == MSH_TRIS )
			{
				tris = &section;
			}
			if( section.id == MSH_TRIMAT )
			{
				triMat = &section;
			}
		}
		if( !valid || verts == NULL || tris == NULL || triMat == NULL
				|| ( uint64 )verts->size != ( uint64 )verts->count * SND_SIZE_MESH_VERT
				|| ( uint64 )tris->size != ( uint64 )tris->count * SND_SIZE_MESH_TRI
				|| ( uint64 )triMat->size != ( uint64 )triMat->count * SND_SIZE_MESH_TRIMAT || triMat->count != tris->count )
		{
			break;
		}
		// Non-overlapping sections inside an int-sized file; their sum plus this 16 B header fits an int too.
		const int bytes = ( int )sizeof( sndAcousticsMesh_t ) + ( int )verts->size + ( int )tris->size + ( int )triMat->size;
		mesh = ( sndAcousticsMesh_t* )Mem_Alloc16( bytes, TAG_AUDIO );
		refused = "memory";
		if( mesh == NULL )
		{
			break;
		}
		mesh->numVerts = ( int )verts->count;
		mesh->numTris = ( int )tris->count;
		mesh->triOffset = ( int )sizeof( sndAcousticsMesh_t ) + ( int )verts->size;
		mesh->triMatOffset = mesh->triOffset + ( int )tris->size;
		refused = "read";
		if( file->Seek( ( long )verts->offset, FS_SEEK_SET ) != 0 || file->Read( mesh + 1, ( int )verts->size ) != ( int )verts->size
				|| file->Seek( ( long )tris->offset, FS_SEEK_SET ) != 0 || file->Read( ( byte* )mesh + mesh->triOffset, ( int )tris->size ) != ( int )tris->size
				|| file->Seek( ( long )triMat->offset, FS_SEEK_SET ) != 0 || file->Read( ( byte* )mesh + mesh->triMatOffset, ( int )triMat->size ) != ( int )triMat->size )
		{
			break;
		}
		refused = "vertices";
		if( !SndFloatsFinite( mesh + 1, ( int )verts->size ) )
		{
			break;
		}
		const sndMeshTri_t* triangles = ( const sndMeshTri_t* )( ( const byte* )mesh + mesh->triOffset );
		refused = "indices";
		for( int t = 0; t < mesh->numTris; t++ )
		{
			for( int v = 0; v < 3; v++ )
			{
				if( triangles[t].v[v] >= verts->count )
				{
					valid = false;
				}
			}
		}
		if( !valid )
		{
			break;
		}
		refused = NULL;
	}
	while( false );
	if( file != NULL )
	{
		fileSystem->CloseFile( file );
	}
	if( refused != NULL )
	{
		Mem_Free16( mesh );
		common->Warning( "s_acousticsShow: refused %s (%s)", path.c_str(), refused );
		return &acousticsMeshRefused;	// no retry or repeated warning until a new bake object
	}
	return mesh;
}

void idSoundWorldLocal::DrawAcoustics( int show )
{
	if( renderWorld == NULL )
	{
		return;
	}
	idSoundAcoustics* const gate = acoustics;	//puncho - SB3-T (OP8-1): ListenerPass returned this member at W2
	char text[64];	//puncho - SB3-T (OP8-5): fixed scratch for the integer labels; va() returns an allocating idStr
	const int frame = renderSystem->GetFrameCount();
	if( acousticsDrawFrame != frame )
	{
		acousticsDrawFrame = frame;
		acousticsDrawBoxes = 0;
		acousticsDrawTexts = 0;
	}
	// Read on the first request even when another bit has already spent the drawing budget.
	if( ( show & 16 ) != 0 && gate->mesh == NULL )
	{
		gate->mesh = SndAcoustics_ReadMesh( *gate );
	}
	const float rangeSqr = 2048.0f * 2048.0f;
	const int lifetime = 20;
	const idVec3 spacing( ( float )gate->GetSettings().probeSpacing, ( float )gate->GetSettings().probeSpacing,
							 ( float )gate->GetSettings().probeSpacingZ );
	if( ( show & 1 ) != 0 )
	{
		const idBounds box( idVec3( -3.0f ), idVec3( 3.0f ) );
		for( int p = 0; p < gate->NumProbes() && acousticsDrawBoxes < 512; p++ )
		{
			const sndBakeProbe_t& probe = gate->GetProbes()[p];
			const idVec3 pos( probe.position[0], probe.position[1], probe.position[2] );
			if( ( pos - listener.pos ).LengthSqr() > rangeSqr )
			{
				continue;
			}
			renderWorld->DebugBounds( colorCyan, box, pos, lifetime );
			acousticsDrawBoxes++;
			if( acousticsDrawTexts < 64 )
			{
				idStr::snPrintf( text, sizeof( text ), "probe %d area %d", p, probe.area );
				renderWorld->DrawText( text, pos, 0.1f, colorCyan, listener.axis, 1, lifetime );
				acousticsDrawTexts++;
			}
		}
	}
	if( ( show & 2 ) != 0 )
	{
		for( int a = 0; a < gate->NumAreas() && acousticsDrawBoxes < 512; a++ )
		{
			const sndBakeArea_t& area = gate->GetAreas()[a];
			const int cells = area.dims[0] * area.dims[1] * area.dims[2];
			for( int c = 0; c < cells && acousticsDrawBoxes < 512; c++ )
			{
				const int coord[3] = { c % area.dims[0], ( c / area.dims[0] ) % area.dims[1], c / ( area.dims[0] * area.dims[1] ) };
				idBounds box;
				for( int k = 0; k < 3; k++ )
				{
					box[0][k] = ( float )( ( int64 )area.cellMin[k] + coord[k] ) * spacing[k];
					box[1][k] = box[0][k] + spacing[k];
				}
				const idVec3 pos = box.GetCenter();
				if( ( pos - listener.pos ).LengthSqr() > rangeSqr )
				{
					continue;
				}
				renderWorld->DebugBounds( colorYellow, box, vec3_origin, lifetime );
				acousticsDrawBoxes++;
				if( acousticsDrawTexts < 64 )
				{
					idStr::snPrintf( text, sizeof( text ), "cell %d probe %d", area.firstCell + c, gate->GetCells()[area.firstCell + c] );
					renderWorld->DrawText( text, pos, 0.1f, colorYellow, listener.axis, 1, lifetime );
					acousticsDrawTexts++;
				}
			}
		}
	}
	if( ( show & 4 ) != 0 )
	{
		// A zone is a set of areas. Draw each area's baked lattice extent in its zone's colour; no SB4 weights yet.
		for( int a = 0; a < gate->NumAreas() && acousticsDrawBoxes < 512; a++ )
		{
			const sndBakeArea_t& area = gate->GetAreas()[a];
			if( area.probeCount == 0 )
			{
				continue;
			}
			idBounds box;
			for( int k = 0; k < 3; k++ )
			{
				box[0][k] = ( float )area.cellMin[k] * spacing[k];
				box[1][k] = ( float )( ( int64 )area.cellMin[k] + area.dims[k] ) * spacing[k];
			}
			const idVec3 pos = box.GetCenter();
			if( ( pos - listener.pos ).LengthSqr() > rangeSqr )
			{
				continue;
			}
			const idVec4& color = idStr::ColorForIndex( 1 + area.zone % 7 );
			renderWorld->DebugBounds( color, box, vec3_origin, lifetime );
			acousticsDrawBoxes++;
			if( acousticsDrawTexts < 64 )
			{
				idStr::snPrintf( text, sizeof( text ), "zone %d area %d", area.zone, a );
				renderWorld->DrawText( text, pos, 0.1f, color, listener.axis, 1, lifetime );
				acousticsDrawTexts++;
			}
		}
	}
	if( ( show & 8 ) != 0 )
	{
		for( int e = 0; e < gate->NumEdges() && acousticsDrawBoxes < 512; e++ )
		{
			const sndPortalEdge_t& edge = gate->GetEdges()[e];
			// The used door's area0 draws one of the two reciprocal windings.
			if( edge.door < 0 || gate->GetDoorUse( edge.door ) != SND_DOOR_USED
					|| edge.otherArea != gate->GetDoors()[edge.door].area1 || ( edge.center - listener.pos ).LengthSqr() > rangeSqr )
			{
				continue;
			}
			const bool shut = ( gate->GetPortalStates()[edge.portal] & ( PS_BLOCK_VIEW | PS_BLOCK_AIR ) ) != 0;
			const idVec4& color = shut ? colorRed : colorGreen;
			// Debug lines use the renderer's fixed pool; copying a debug polygon can allocate a winding.
			for( int p = 0; p < edge.w->GetNumPoints(); p++ )
			{
				renderWorld->DebugLine( color, ( *edge.w )[p].ToVec3(), ( *edge.w )[( p + 1 ) % edge.w->GetNumPoints()].ToVec3(), lifetime );
			}
			acousticsDrawBoxes++;
			if( acousticsDrawTexts < 64 )
			{
				renderWorld->DrawText( gate->GetDoorName( edge.door ), edge.center, 0.1f, color, listener.axis, 1, lifetime );
				acousticsDrawTexts++;
			}
		}
	}
	if( ( show & 16 ) != 0 && gate->mesh != &acousticsMeshRefused )
	{
		const sndAcousticsMesh_t* mesh = ( const sndAcousticsMesh_t* )gate->mesh;
		const sndMeshVert_t* verts = ( const sndMeshVert_t* )( mesh + 1 );
		const sndMeshTri_t* tris = ( const sndMeshTri_t* )( ( const byte* )mesh + mesh->triOffset );
		const sndMeshTriMat_t* triMat = ( const sndMeshTriMat_t* )( ( const byte* )mesh + mesh->triMatOffset );
		for( int t = 0; t < mesh->numTris && acousticsDrawBoxes < 512; t++ )
		{
			if( ( triMat[t].flags & SND_TRI_PLUG ) == 0 )
			{
				continue;
			}
			idVec3 points[3];
			for( int v = 0; v < 3; v++ )
			{
				const sndMeshVert_t& vert = verts[tris[t].v[v]];
				points[v].Set( vert.xyz[0], vert.xyz[1], vert.xyz[2] );
			}
			const idVec3 pos = ( points[0] + points[1] + points[2] ) * ( 1.0f / 3.0f );
			if( ( pos - listener.pos ).LengthSqr() > rangeSqr )
			{
				continue;
			}
			for( int v = 0; v < 3; v++ )
			{
				renderWorld->DebugLine( colorMagenta, points[v], points[( v + 1 ) % 3], lifetime );
			}
			acousticsDrawBoxes++;
		}
	}
}
//puncho end

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
	//puncho - SB3-T (D-37): the one teardown covers unload, destructor and failed load; refusal holds no allocation
	if( it->mesh != &acousticsMeshRefused )
	{
		Mem_Free16( it->mesh );
	}
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
