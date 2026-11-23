//puncho - C3a-R (R-C3a-1 to R-C3a-14): main-thread developer recorder, removed with today's mixer.
#include "precompiled.h"
#pragma hdrstop

#include "snd_local.h"
#if defined( USE_SOUND_MIXER )
#ifndef ID_RETAIL
#include <type_traits>
#endif
#endif

idCVar s_mixRecord( "s_mixRecord", "", 0, "write mixrecord/<plain name>.csv under fs_savepath; empty closes it" );
extern idCVar s_volume_dB;
extern idCVar s_subFraction;

static const int MIX_RECORD_BUFFER_SIZE = 262144;
static char* mixRecordBuffer = NULL;
static int mixRecordUsed = 0;
static char mixRecordName[64];
static uint64 mixRecordFrame = 0;
static uint64 mixRecordNextKey = 1;
static int mixRecordVoices = 0;
static int mixRecordOutChannels = 0;	//puncho - C3a-R (OR-7): the layout promised by this file
static int mixRecordChannelMask = 0;
static const idSoundWorldLocal* mixRecordWorld = NULL;
#if defined( USE_SOUND_MIXER )
//puncho - C3e-C: StopAllSounds may also call hardware.Update before the next Render.
static uint64 mixRecordLevelsFrame = ~( uint64 )0;
#ifndef ID_RETAIL
extern idCVar s_mixTestDump;
static idFile* mixCommandFile = NULL;
static char* mixCommandBuffer = NULL;
static int mixCommandUsed = 0;

static bool MixRecord_WriteCommands()
{
	const int bytes = mixCommandUsed;
	mixCommandUsed = 0;
	if( bytes != 0 && mixCommandFile->Write( mixCommandBuffer, bytes ) != bytes )
	{
		idLib::Warning( "s_mixTestDump: write failed; command dump disabled" );
		s_mixTestDump.SetBool( false );
		return false;
	}
	return true;
}

static void MixRecord_CloseCommands()
{
	if( mixCommandFile != NULL )
	{
		MixRecord_WriteCommands();
		fileSystem->CloseFile( mixCommandFile );
		mixCommandFile = NULL;
	}
	Mem_Free( mixCommandBuffer );
	mixCommandBuffer = NULL;
	mixCommandUsed = 0;
}

static void MixRecord_Command( const idSoundVoice* voice, uint64 key )
{
	if( !s_mixTestDump.GetBool() )
	{
		return;
	}
	if( mixCommandFile == NULL )
	{
		char path[96];
		idStr::snPrintf( path, sizeof( path ), "mixrecord/%s.cmd.csv", mixRecordName );
		mixCommandFile = fileSystem->OpenFileWrite( path, "fs_savepath" );
		if( mixCommandFile == NULL )
		{
			idLib::Warning( "s_mixTestDump: cannot open %s", path );
			s_mixTestDump.SetBool( false );
			return;
		}
		mixCommandBuffer = ( char* )Mem_Alloc( MIX_RECORD_BUFFER_SIZE, TAG_AUDIO );
		if( mixCommandBuffer == NULL )
		{
			idLib::Warning( "s_mixTestDump: cannot allocate the 256 KB buffer" );
			s_mixTestDump.SetBool( false );
			MixRecord_CloseCommands();
			return;
		}
		static const char header[] = "frame,key,started,generation,state,flags,startFrame,startInLoop,dryGain,pitch,filterCoef,sendGain,offsetMs,occlusion,leadinFrames,leadinRate,loopFrames,loopRate,srcChannels,m00,m01,m02,m03,m04,m05,m06,m07,m10,m11,m12,m13,m14,m15,m16,m17\n";
		memcpy( mixCommandBuffer, header, sizeof( header ) - 1 );
		mixCommandUsed = sizeof( header ) - 1;
	}
	const sndVoiceCmd_t& cmd = voice->GetCommand();
	char row[1024];
	int bytes = idStr::snPrintf( row, sizeof( row ), "%llu,%llu,%d,%u,%u,%u,%d,%u,%.9g,%.9g,%.9g,%.9g,%d,%.9g,%d,%d,%d,%d,%d",
		( unsigned long long )mixRecordFrame, ( unsigned long long )key, voice->HasStarted() ? 1 : 0, cmd.generation, ( unsigned int )cmd.state,
		( unsigned int )cmd.flags, cmd.startFrame, ( unsigned int )cmd.startInLoop, cmd.dryGain, cmd.pitch, cmd.filterCoef,
		cmd.sendGain, voice->GetStartOffsetMS(), voice->GetLastOcclusion(), cmd.leadin.numFrames, cmd.leadin.rate,
		cmd.loop.numFrames, cmd.loop.rate, cmd.leadin.channels );
	for( int s = 0; s < 2; s++ )
	{
		for( int o = 0; o < SND_MIX_MAX_OUT; o++ )
		{
			bytes += idStr::snPrintf( row + bytes, ( int )sizeof( row ) - bytes, ",%.9g", cmd.matrix[s][o] );
		}
	}
	row[bytes++] = '\n';
	if( bytes > MIX_RECORD_BUFFER_SIZE - mixCommandUsed && !MixRecord_WriteCommands() )
	{
		MixRecord_CloseCommands();
		return;
	}
	memcpy( mixCommandBuffer + mixCommandUsed, row, bytes );
	mixCommandUsed += bytes;
}
#endif
#endif

// Stored pointers are identity tokens only: never dereference a previous update's channel or world.
struct mixRecordVoice_t
{
	const idSoundChannel* channel;
	uint64 key;
	int startTime;
};
static mixRecordVoice_t mixRecordTable[MAX_HARDWARE_VOICES];

static const char mixRecordHeaders[] =
	"H,version,rate,block,outChannels,channelMask,subFraction,backEnd,buildNumber\n"
	"F,frame,us,ms,dtMs,world,paused,muted,masterDb,voices,zone,candidate,cut,map\n"
	"L,frame,rms0,peak0,rms1,peak1,rms2,peak2,rms3,peak3,rms4,peak4,rms5,peak5,rms6,peak6,rms7,peak7\n"
	"S,frame,key,emitter,channel,shader,leadin,loop,offsetMs,flags,shakes,soundClass,srcChannels,leadinRate,loopRate,leadinFrames,loopFrames,minDistance,maxDistance\n"
	"V,frame,key,volumeDb,straightDb,excessDb,gain,pitch,occlusion,send,m0,m1,m2,m3,m4,m5,m6,m7,m8,m9,m10,m11,m12,m13,m14,m15\n"
	"E,frame,key\n";

static bool MixRecord_WriteBuffer()
{
	if( mixRecordUsed == 0 )
	{
		return true;
	}
	const int bytes = mixRecordUsed;
	mixRecordUsed = 0;	// A short write must never be retried by Close.
	if( soundSystemLocal.mixRecordFile->Write( mixRecordBuffer, bytes ) != bytes )
	{
		idLib::Warning( "s_mixRecord: writing mixrecord/%s.csv failed; the recorder is off", mixRecordName );
		s_mixRecord.SetString( "" );
		return false;
	}
	return true;
}

void MixRecord_Close()
{
#if defined( USE_SOUND_MIXER )
	mixRecordLevelsFrame = ~( uint64 )0;
#ifndef ID_RETAIL
	MixRecord_CloseCommands();
#endif
#endif
	if( soundSystemLocal.mixRecordFile == NULL && mixRecordBuffer == NULL )
	{
		return;
	}
	if( soundSystemLocal.mixRecordFile != NULL )
	{
		MixRecord_WriteBuffer();
		fileSystem->CloseFile( soundSystemLocal.mixRecordFile );
		soundSystemLocal.mixRecordFile = NULL;
	}
	Mem_Free( mixRecordBuffer );
	mixRecordBuffer = NULL;
	mixRecordUsed = 0;
	mixRecordName[0] = '\0';
	mixRecordFrame = 0;
	mixRecordNextKey = 1;
	mixRecordVoices = 0;
	mixRecordOutChannels = 0;
	mixRecordChannelMask = 0;
	mixRecordWorld = NULL;
	memset( mixRecordTable, 0, sizeof( mixRecordTable ) );
}

//puncho - C3a-R (OR-7): every row-producing entry checks before appending, including frames with no world.
static bool MixRecord_CheckLayout()
{
	int outChannels, channelMask;
	idSoundVoice::GetSurroundLayout( outChannels, channelMask );
	if( outChannels != mixRecordOutChannels || channelMask != mixRecordChannelMask )
	{
		idLib::Warning( "s_mixRecord: the output layout changed; the recorder is off" );
		s_mixRecord.SetString( "" );
		MixRecord_Close();
		return false;
	}
	return true;
}

static bool MixRecord_AppendRow( const char* row, int bytes )
{
	assert( bytes >= 0 && bytes <= MIX_RECORD_BUFFER_SIZE );
	if( bytes > MIX_RECORD_BUFFER_SIZE - mixRecordUsed && !MixRecord_WriteBuffer() )
	{
		MixRecord_Close();
		return false;
	}
	memcpy( mixRecordBuffer + mixRecordUsed, row, bytes );
	mixRecordUsed += bytes;
	return true;
}

// Each caller reserves MAX_STRING_CHARS for each name, plus its fixed numeric fields. A name that cannot
// fit must stop the recording, not silently change a sample's identity. No idStr or row allocation per voice.
static int MixRecord_CopyName( char* dest, const char* name )
{
	const int bytes = idStr::Length( name );
	if( bytes >= MAX_STRING_CHARS )
	{
		idLib::Warning( "s_mixRecord: name exceeds MAX_STRING_CHARS; the recorder is off" );
		s_mixRecord.SetString( "" );
		MixRecord_Close();
		return -1;
	}
	for( int i = 0; i < bytes; i++ )
	{
		dest[i] = name[i] == ',' ? ';' : name[i];
	}
	return bytes;
}

static bool MixRecord_EndWorld()
{
	char row[80];
	for( int i = 0; i < MAX_HARDWARE_VOICES; i++ )
	{
		if( mixRecordTable[i].key != 0 )
		{
			const int bytes = idStr::snPrintf( row, sizeof( row ), "E,%llu,%llu\n",
											 ( unsigned long long )mixRecordFrame, ( unsigned long long )mixRecordTable[i].key );
			if( !MixRecord_AppendRow( row, bytes ) )
			{
				return false;
			}
		}
	}
	memset( mixRecordTable, 0, sizeof( mixRecordTable ) );
	mixRecordWorld = NULL;
	return true;
}

void idSoundWorldLocal::MixRecord_World( const idStaticList< idActiveChannel, MAX_HARDWARE_VOICES >& channels, int currentTime )
{
	if( soundSystemLocal.mixRecordFile == NULL || soundSystemLocal.currentSoundWorld != this
			|| idStr::Cmp( s_mixRecord.GetString(), mixRecordName ) != 0 )
	{
		return;
	}
	if( !MixRecord_CheckLayout() )
	{
		return;
	}
	if( mixRecordWorld != this && !MixRecord_EndWorld() )
	{
		return;
	}
	mixRecordWorld = this;
#if defined( USE_SOUND_MIXER )
#ifndef ID_RETAIL
	//puncho - compile out the per-voice hook when off: one dump switch per world walk.
	const auto recordWorld = [&]( auto dumpCommands )
	{
#endif
#endif

	mixRecordVoice_t next[MAX_HARDWARE_VOICES] = {};
	int count = 0;
	char row[3 * MAX_STRING_CHARS + 1024];
	for( int i = 0; i < channels.Num(); i++ )
	{
		const idSoundChannel* chan = channels[i].channel;
		if( chan->hardwareVoice == NULL )
		{
			continue;
		}
		assert( count < MAX_HARDWARE_VOICES );
		uint64 key = 0;
		for( int j = 0; j < MAX_HARDWARE_VOICES; j++ )
		{
			if( mixRecordTable[j].key != 0 && mixRecordTable[j].channel == chan && mixRecordTable[j].startTime == chan->startTime )
			{
				key = mixRecordTable[j].key;
				mixRecordTable[j].key = 0;
				break;
			}
		}
		const idSoundEmitterLocal* emitter = chan->emitter;
		const idSoundSample* leadin = chan->leadinSample;
		const idSoundSample* loop = chan->loopingSample;
		const int srcChannels = leadin->NumChannels();
		if( srcChannels < 1 || srcChannels > 2 )
		{
			idLib::Warning( "s_mixRecord: %d source channels cannot fit version 1; the recorder is off", srcChannels );
			s_mixRecord.SetString( "" );
			MixRecord_Close();
			return;
		}
		if( key == 0 )
		{
			key = mixRecordNextKey++;
			int bytes = idStr::snPrintf( row, sizeof( row ), "S,%llu,%llu,%d,%d,", ( unsigned long long )mixRecordFrame,
											( unsigned long long )key, emitter->index, chan->logicalChannel );
			const char* names[3] = { chan->soundShader->GetName(), leadin->GetName(), loop != NULL ? loop->GetName() : "" };
			for( int n = 0; n < 3; n++ )
			{
				const int length = MixRecord_CopyName( row + bytes, names[n] );
				if( length < 0 )
				{
					return;
				}
				bytes += length;
				row[bytes++] = ',';
			}
			bytes += idStr::snPrintf( row + bytes, ( int )sizeof( row ) - bytes, "%d,%x,%.9g,%d,%d,%d,%d,%d,%d,%.9g,%.9g\n",
					currentTime - chan->startTime, ( unsigned int )chan->parms.soundShaderFlags, chan->parms.shakes, chan->parms.soundClass,
					srcChannels, leadin->SampleRate(), loop != NULL ? loop->SampleRate() : 0, leadin->NumSamples(), loop != NULL ? loop->NumSamples() : 0,
					chan->parms.minDistance, chan->parms.maxDistance );
			if( !MixRecord_AppendRow( row, bytes ) )
			{
				return;
			}
		}
		next[count].channel = chan;
		next[count].startTime = chan->startTime;
		next[count++].key = key;

		const float straightDb = ( ( chan->parms.soundShaderFlags & SSF_GLOBAL ) != 0 || emitter->emitterId == listener.id ) ? 0.0f
				: SoundDistanceDB( emitter->directDistance, chan->parms.minDistance, chan->parms.maxDistance );
		float matrix[MAX_CHANNELS_PER_VOICE * MAX_CHANNELS_PER_VOICE] = {};
		chan->hardwareVoice->CalculateSurround( srcChannels, matrix, 1.0f );
		int bytes = idStr::snPrintf( row, sizeof( row ), "V,%llu,%llu,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g",
				( unsigned long long )mixRecordFrame, ( unsigned long long )key, chan->volumeDB, straightDb, chan->excessDB.value,
				chan->hardwareVoice->GetGain(), chan->hardwareVoice->GetPitch(), chan->occlusion.value, chan->hardwareVoice->GetReverbSend() );
		for( int m = 0; m < 16; m++ )
		{
			row[bytes++] = ',';
			if( m < srcChannels * mixRecordOutChannels )
			{
				bytes += idStr::snPrintf( row + bytes, ( int )sizeof( row ) - bytes, "%.9g", matrix[m] );
			}
		}
		row[bytes++] = '\n';
		if( !MixRecord_AppendRow( row, bytes ) )
		{
			return;
		}
#if defined( USE_SOUND_MIXER )
#ifndef ID_RETAIL
		if constexpr( decltype( dumpCommands )::value )
		{
			MixRecord_Command( chan->hardwareVoice, key ); //puncho - the exact V walk supplies identity
		}
#endif
#endif
		mixRecordVoices++;
	}
	// Unmatched keys ended. EndWorld never dereferences the previous update's pointers.
	if( !MixRecord_EndWorld() )
	{
		return;
	}
	memcpy( mixRecordTable, next, sizeof( mixRecordTable ) );
	mixRecordWorld = this;
#if defined( USE_SOUND_MIXER )
#ifndef ID_RETAIL
	};
	if( s_mixTestDump.GetBool() )
	{
		recordWorld( std::true_type() );
	}
	else
	{
		recordWorld( std::false_type() );
	}
#endif
#endif
}

void MixRecord_Levels( const float* rms, const float* peak, int channels )
{
	if( soundSystemLocal.mixRecordFile == NULL || idStr::Cmp( s_mixRecord.GetString(), mixRecordName ) != 0 )
	{
		return;
	}
	if( !MixRecord_CheckLayout() )
	{
		return;
	}
#if defined( USE_SOUND_MIXER )
	if( mixRecordLevelsFrame == mixRecordFrame )
	{
		return;
	}
	mixRecordLevelsFrame = mixRecordFrame;
#endif
	assert( channels >= 1 && channels <= 8 );
	char row[512];
	int bytes = idStr::snPrintf( row, sizeof( row ), "L,%llu", ( unsigned long long )mixRecordFrame );
	for( int i = 0; i < 8; i++ )
	{
		if( i < channels )
		{
			bytes += idStr::snPrintf( row + bytes, ( int )sizeof( row ) - bytes, ",%.9g,%.9g", rms[i], peak[i] );
		}
		else
		{
			row[bytes++] = ',';
			row[bytes++] = ',';
		}
	}
	row[bytes++] = '\n';
	MixRecord_AppendRow( row, bytes );
}

void MixRecord_Frame()
{
	const char* name = s_mixRecord.GetString();
	if( name[0] == '\0' )
	{
		MixRecord_Close();
		return;
	}
	if( soundSystemLocal.mixRecordFile != NULL && idStr::Cmp( name, mixRecordName ) != 0 )
	{
		MixRecord_Close();
		name = s_mixRecord.GetString();	// A failed final write clears and may invalidate the old cvar string.
		if( name[0] == '\0' )
		{
			return;
		}
	}
	if( soundSystemLocal.mixRecordFile == NULL )
	{
		int length = 0;
		while( name[length] != '\0' && length < 64 )
		{
			const char c = name[length];
			if( !( ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '_' ) )
			{
				break;
			}
			length++;
		}
		if( length == 0 || length > 63 || name[length] != '\0' )
		{
			idLib::Warning( "s_mixRecord requires a plain name of 1 to 63 letters, digits or underscores" );
			s_mixRecord.SetString( "" );
			return;
		}
		int outChannels, channelMask;
		idSoundVoice::GetSurroundLayout( outChannels, channelMask );
		if( outChannels < 1 || outChannels > 8 )
		{
			// BLOCKED R-C3a-3/7: OpenAL never initializes InitSurround; the stub has no version-1 backend name.
			idLib::Warning( "s_mixRecord: surround layout unavailable; no recording opened" );
			s_mixRecord.SetString( "" );
			return;
		}
		char path[96];
		idStr::snPrintf( path, sizeof( path ), "mixrecord/%s.csv", name );
		soundSystemLocal.mixRecordFile = fileSystem->OpenFileWrite( path, "fs_savepath" );
		if( soundSystemLocal.mixRecordFile == NULL )
		{
			idLib::Warning( "s_mixRecord: could not open %s", path );
			s_mixRecord.SetString( "" );
			return;
		}
		mixRecordBuffer = ( char* )Mem_Alloc( MIX_RECORD_BUFFER_SIZE, TAG_AUDIO );
		idStr::Copynz( mixRecordName, name, sizeof( mixRecordName ) );
		if( mixRecordBuffer == NULL )
		{
			idLib::Warning( "s_mixRecord: could not allocate the 256 KB buffer" );
			s_mixRecord.SetString( "" );
			MixRecord_Close();
			return;
		}
		mixRecordOutChannels = outChannels;
		mixRecordChannelMask = channelMask;
		MixRecord_AppendRow( mixRecordHeaders, ( int )sizeof( mixRecordHeaders ) - 1 );
#if defined( _MSC_VER ) && !defined( USE_OPENAL )	//puncho - C3a-R (OR-9): the other backends refuse the open above
#if defined( USE_SOUND_MIXER )
		const char* backEnd = "mixer"; //puncho - C3e-C (R-C3e-22)
#else
		const char* backEnd = "xaudio2";
#endif
		// The engine never changes LC_NUMERIC from the process's initial C locale (as in AcousticsLog).
		char header[256];
		const int bytes = idStr::snPrintf( header, sizeof( header ), "H,1,%d,%d,%d,%u,%.9g,%s,%d\n",
											 SND_MIX_SAMPLE_RATE, SND_MIX_SAMPLE_RATE / 100, outChannels,
											 ( unsigned int )channelMask, s_subFraction.GetFloat(), backEnd, BUILD_NUMBER );
		MixRecord_AppendRow( header, bytes );
#else	//puncho - C3a-R (N-2): no recording without a named backend and its H row
		idLib::Warning( "s_mixRecord: no back end name; the recorder is off" );
		s_mixRecord.SetString( "" );
		MixRecord_Close();
		return;
#endif
	}
	else if( !MixRecord_CheckLayout() )
	{
		return;
	}

	idSoundWorldLocal* world = soundSystemLocal.currentSoundWorld;
	if( mixRecordWorld != world && !MixRecord_EndWorld() )
	{
		return;
	}
	char row[MAX_STRING_CHARS + 512];
	int bytes = idStr::snPrintf( row, sizeof( row ), "F,%llu,%llu,%d,%.9g,%d,%d,%d,%.9g,%d,%d,%d,%d,",
			( unsigned long long )mixRecordFrame, ( unsigned long long )Sys_Microseconds(), world != NULL ? world->GetSoundTime() : 0,
			world != NULL ? world->updateDtMs : 0.0f, world == NULL ? 0 : ( world->renderWorld != NULL ? 1 : 2 ),
			world != NULL && world->IsPaused() ? 1 : 0, soundSystemLocal.muted ? 1 : 0, s_volume_dB.GetFloat(), mixRecordVoices,
			world != NULL && world->reverb.armed ? world->reverb.zone : -1, world != NULL && world->reverb.armed ? world->reverb.candidate : -1,
			world != NULL ? world->listenerCutReasons : 0 );
	const char* map = session->GetState() == idSession::INGAME ? session->GetActingGameStateLobbyBase().GetMatchParms().mapName.c_str() : "";
	const int length = MixRecord_CopyName( row + bytes, map );
	if( length < 0 )
	{
		return;
	}
	bytes += length;
	row[bytes++] = '\n';
	if( MixRecord_AppendRow( row, bytes ) )
	{
		mixRecordFrame++;
		mixRecordVoices = 0;
	}
}
