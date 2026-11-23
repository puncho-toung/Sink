/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013 Robert Beckebans

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
//puncho - C3e-C: the engine's mixer lifetime, publication and sample fence.
#pragma hdrstop
#include "precompiled.h"
#if defined( USE_SOUND_MIXER )
#include "../snd_local.h"
#include "../snd_acousticfile.h"
#include "../snd_acoustics.h"
#include <xmmintrin.h>

idCVar s_showLevelMeter( "s_showLevelMeter", "0", CVAR_BOOL | CVAR_ARCHIVE, "Show VU meter" );
idCVar s_meterTopTime( "s_meterTopTime", "1000", CVAR_INTEGER | CVAR_ARCHIVE, "Peak hold in milliseconds" );
idCVar s_meterPosition( "s_meterPosition", "100 100 20 200", CVAR_ARCHIVE, "VU meter location (x y w h)" );
idCVar s_showPerfData( "s_showPerfData", "0", CVAR_BOOL, "Print mixer performance once a second" );
idCVar s_showMixer( "s_showMixer", "0", CVAR_INTEGER, "Mixer overlay; 2 adds fence counters; console once a second", 0, 2 );
extern idCVar s_volume_dB;
extern idCVar s_debugHardware;
extern idCVar s_smoothMaxDtMs;
static idOverlayHandle mixerOverlay;
static idOverlayHandle reverbOverlay; //puncho - C4-R: a separate line, not the nearly full first buffer.
#ifndef ID_RETAIL
idCVar s_mixTestNoRoomMemory( "s_mixTestNoRoomMemory", "0", CVAR_SOUND | CVAR_BOOL, "Test a baked map with room allocation refused" );
idCVar s_mixTestDump( "s_mixTestDump", "0", CVAR_BOOL, "Dump mixer commands beside s_mixRecord's CSV" );

void idSoundHardware_Mixer::TestStall_f( const idCmdArgs& args )
{
	if( args.Argc() < 2 || args.Argc() > 3 || ( args.Argc() == 3 && idStr::Icmp( args.Argv( 2 ), "hold" ) != 0 ) )
	{
		idLib::Printf( "usage: s_mixTestStall <ms> [hold] (applied by the next Update)\n" );
		return;
	}
	idSoundHardware_Mixer& hw = soundSystemLocal.hardware;
	hw.testStallMS = idMath::ClampInt( 0, 60000, atoi( args.Argv( 1 ) ) );
	hw.testHold = args.Argc() == 3;
	idLib::Printf( "mixer test: queued %d ms %s\n", hw.testStallMS, hw.testHold ? "hold" : "zeros" );
}

void idSoundHardware_Mixer::TestPurge_f( const idCmdArgs& args )
{
	if( args.Argc() != 2 )
	{
		idLib::Printf( "usage: s_mixTestPurge <sample name from listSamples>\n" );
		return;
	}
	for( int i = 0; i < soundSystemLocal.samples.Num(); i++ )
	{
		idSoundSample* sample = soundSystemLocal.samples[i];
		if( idStr::Icmp( sample->GetName(), args.Argv( 1 ) ) == 0 )
		{
			if( sample == soundSystemLocal.defaultSample )
			{
				idLib::Printf( "mixer test: refusing the missing-sound sample '%s'\n", sample->GetName() );
				return;
			}
			const uint64 begin = Sys_Microseconds();
			sample->FreeData();
			sample->LoadResource();
			idLib::Printf( "mixer purge: %s elapsedUs=%llu loaded=%d\n", sample->GetName(),
				( unsigned long long )( Sys_Microseconds() - begin ), sample->IsLoaded() ? 1 : 0 );
			return;
		}
	}
	idLib::Printf( "mixer test: sample '%s' not found\n", args.Argv( 1 ) );
}

// A pacifier can make the loop keep a candidate. Observe it before another pass can free it.
void idSoundHardware_Mixer::ReportKeptCandidates()
{
	if( !testLoadActive || !s_debugHardware.GetBool() )
	{
		return;
	}
	for( int i = 0; i < soundSystemLocal.samples.Num(); i++ )
	{
		idSoundSample* sample = soundSystemLocal.samples[i];
		if( sample->WasMixerPurgeCandidate() )
		{
			idLib::Printf( "mixer purge kept: %s\n", sample->GetName() );
			sample->SetMixerPurgeCandidate( false );
			testLoadKept++;
		}
	}
}

void idSoundHardware_Mixer::TestViews_f( const idCmdArgs& args )
{
	const bool beginLoad = args.Argc() == 2 && idStr::Icmp( args.Argv( 1 ), "beginLoad" ) == 0;
	const bool endLoad = args.Argc() == 2 && idStr::Icmp( args.Argv( 1 ), "endLoad" ) == 0;
	if( args.Argc() != 1 && !beginLoad && !endLoad )
	{
		idLib::Printf( "usage: s_mixTestViews [beginLoad|endLoad]\n" );
		return;
	}
	idSoundHardware_Mixer& hw = soundSystemLocal.hardware;
	if( beginLoad )
	{
		hw.testLoadKept = 0;
		hw.testLoadWaits = hw.fenceWaits;
		hw.testLoadWaitUs = hw.fenceWaitUs;
		hw.testLoadActive = true;
		for( int i = 0; i < soundSystemLocal.samples.Num(); i++ )
		{
			soundSystemLocal.samples[i]->SetMixerPurgeCandidate( false );
		}
		idLib::Printf( "mixer load: begin waits=%u sumWaitUs=%llu\n", hw.testLoadWaits, ( unsigned long long )hw.testLoadWaitUs );
		return;
	}
	if( endLoad )
	{
		if( !hw.testLoadActive )
		{
			idLib::Printf( "mixer load: no beginLoad mark\n" );
			return;
		}
		hw.ReportKeptCandidates();
		idLib::Printf( "mixer load: waits=%u sumWaitUs=%llu kept=%u\n", hw.fenceWaits - hw.testLoadWaits,
			( unsigned long long )( hw.fenceWaitUs - hw.testLoadWaitUs ), hw.testLoadKept );
		hw.testLoadActive = false;
	}
	int loaded = 0, valid = 0;
	for( int i = 0; i < soundSystemLocal.samples.Num(); i++ )
	{
		const idSoundSample* sample = soundSystemLocal.samples[i];
		sndSampleView_t view;
		const char* reason = NULL;
		const bool hasView = sample->GetView( view, &reason );
		loaded += sample->IsLoaded() ? 1 : 0;
		valid += hasView ? 1 : 0;
		if( sample->IsLoaded() && !hasView )
		{
			idLib::Printf( "mixer view: %s: %s\n", sample->GetName(), reason );
		}
	}
	idLib::Printf( "mixer views: total=%d loaded=%d valid=%d loadedWithoutView=%d\n",
		soundSystemLocal.samples.Num(), loaded, valid, loaded - valid );
}
#endif

void idSoundHardware_Mixer::PrepareSettings( const float* values )
{
	// Copy masterdemo::CleanSettings' parameter mapping, including both HDR clean passes.
	float v[NUM_SETTINGS];
	memcpy( v, values, sizeof( v ) );
	const float oldHeadroom = hdrParms.headroomDb;
	SndHdr_DefaultParms( hdrParms );
	hdrParms.minTopDb = hdrParms.referenceDb + v[HDR_CEILING];
	hdrParms.headroomDb = -v[HDR_CEILING];
	hdrParms.releaseTime = v[HDR_RELEASE] * 0.001f;
	SndHdr_CleanParms( hdrParms );
	v[HDR_CEILING] = -hdrParms.headroomDb;
	hdrParms.minTopDb = hdrParms.referenceDb + v[HDR_CEILING];
	SndHdr_CleanParms( hdrParms );
	hdrOn = v[HDR_ON] != 0.0f;
	sndMasterParms_t p;
	SndMaster_DefaultParms( p );
	p.eqOn = v[EQ_ON] != 0.0f;
	p.compOn = v[COMP_ON] != 0.0f;
	p.lowHz = v[LOW_HZ];
	p.lowDb = v[LOW_DB];
	p.highHz = v[HIGH_HZ];
	p.highDb = v[HIGH_DB];
	p.compThresholdDb = v[HDR_CEILING] - v[COMP_UNDER];
	p.compRatio = v[COMP_RATIO];
	p.compAttackMs = v[COMP_ATTACK];
	p.compReleaseMs = v[COMP_RELEASE];
	p.makeupDb = v[MAKEUP];
	p.limitCeilingDb = v[LIMIT_CEILING];
	p.limitHoldMs = v[LIMIT_HOLD];
	p.limitReleaseMs = v[LIMIT_RELEASE];
	p.linkPower = 0;
	SndMaster_CleanParms( p );
	SndMaster_Prepare( masterCoefs, p );
	if( hdrParms.headroomDb != oldHeadroom )
	{
		hdrSerial++;
	}
}

idSoundHardware_Mixer::idSoundHardware_Mixer() : gate( 1 ), inBlock( 0 ), pulls( 0 ), maxPublishWaitUs( 0 )
{
	mixerMemory = NULL;
	freeHead = freeCount = 0;
	memset( freeSlots, 0, sizeof( freeSlots ) );
	generation = resetSerial = hdrSerial = 0;
	stopSequence = 0;
	memset( &hdrParms, 0, sizeof( hdrParms ) );
	memset( &masterCoefs, 0, sizeof( masterCoefs ) );
	memset( &lastStatus, 0, sizeof( lastStatus ) );
	reverbOwner = NULL;
	memset( reverbMemory, 0, sizeof( reverbMemory ) );
	memset( reverbOut, 0, sizeof( reverbOut ) );
	reverbMapSerial = 0;
	reverbTickUs = 0;
	reverbMaxTickUs = 0;
	vuMeterRMS = vuMeterPeak = NULL;
	lastResetTime = streamChannels = 0;
	memset( blockTimes, 0, sizeof( blockTimes ) );
	blockTimeIndex = lateBlocks = 0;
	lastSequence = 0;
	fences = fenceWaits = stalls = 0;
	maxFenceWaitUs = fenceWaitUs = 0;
	memset( settingValues, 0, sizeof( settingValues ) );
	hdrOn = meters = false;
	firstMeterSequence = 0;
	memset( meterPeakTimes, 0, sizeof( meterPeakTimes ) );
	lastPerfPrintTime = showMixer = 0;
	publishUs = 0;
	reportedMaxPublishWaitUs = 0;
#ifndef ID_RETAIL
	testLoadKept = testLoadWaits = 0;
	testLoadWaitUs = 0;
	testLoadActive = false;
	testStallMS = 0;
	testHold = false;
	testZeroUntilUs.store( 0 );
	testHoldMS.store( 0 );
	testHoldStartUs.store( 0 );
	testHoldEndUs.store( 0 );
#endif
#if defined( _DEBUG )
	insideSampleFree = false;
#endif
}

void idSoundHardware_Mixer::Init()
{
	assert( idLib::IsMainThread() );
	assert( gate.load( std::memory_order_seq_cst ) != 0 );
	maxPublishWaitUs.store( 0, std::memory_order_relaxed ); //puncho - DR-318: a new reporting interval.
	reportedMaxPublishWaitUs = 0;
	cmdSystem->AddCommand( "listDevices", idSoundSink_XAudio2::ListDevices, 0, "Lists the connected sound devices", NULL );
#ifndef ID_RETAIL
	cmdSystem->AddCommand( "s_mixTestStall", TestStall_f, CMD_FL_SOUND, "Temporarily zero or hold the mixer pull" );
	cmdSystem->AddCommand( "s_mixTestPurge", TestPurge_f, CMD_FL_SOUND, "Free and reload a named sound sample" );
	cmdSystem->AddCommand( "s_mixTestViews", TestViews_f, CMD_FL_SOUND, "Report views, or mark/report a load with beginLoad/endLoad" );
	testStallMS = 0;
	testHold = false;
	testZeroUntilUs.store( 0 );
	testHoldMS.store( 0 );
#endif
	Sys_Microseconds();	// initialise the clock divisor before XAudio2 can pull
	if( !sink.Open( this ) )
	{
		Shutdown();
		return;
	}
	idSoundVoice::InitSurround( sink.OutputChannels(), sink.ChannelMask() );
	mixerMemory = Mem_Alloc16( idSoundMixer::MemoryBytes(), TAG_AUDIO );
	if( mixerMemory == NULL || !mixer.Init( mixerMemory ) )
	{
		idLib::Warning( "sound mixer: could not allocate or initialise the mixer" );
		Shutdown();
		return;
	}
	//puncho - C4-R: Init runs behind the shut/wait gate, before a new sequence domain.
	if( reverbMemory[0] != NULL ) { reverbMix.ReclaimAll(); }
	memset( reverbOut, 0, sizeof( reverbOut ) );
	link.Init();
	freeHead = 0;
	freeCount = SND_MIX_MAX_VOICES;
	for( int i = 0; i < SND_MIX_MAX_VOICES; i++ )
	{
		freeSlots[i] = i;
		voices[i].slot = i;
		memset( &voices[i].command, 0, sizeof( voices[i].command ) );
		memset( &voices[i].leadinView, 0, sizeof( voices[i].leadinView ) );
		memset( &voices[i].loopView, 0, sizeof( voices[i].loopView ) );
		voices[i].started = voices[i].noMeter = false;
	}
	stopSequence = 0;
	vuMeterRMS = console->CreateGraph( sink.OutputChannels() );
	vuMeterPeak = console->CreateGraph( sink.OutputChannels() );
	vuMeterRMS->Enable( false );
	vuMeterPeak->Enable( false );
	vuMeterPeak->SetFillMode( idDebugGraph::GRAPH_LINE );
	vuMeterPeak->SetBackgroundColor( idVec4( 0.0f, 0.0f, 0.0f, 0.0f ) );
	vuMeterRMS->AddGridLine( 0.500f, idVec4( 0.5f, 0.5f, 0.5f, 1.0f ) );
	vuMeterRMS->AddGridLine( 0.250f, idVec4( 0.5f, 0.5f, 0.5f, 1.0f ) );
	vuMeterRMS->AddGridLine( 0.125f, idVec4( 0.5f, 0.5f, 0.5f, 1.0f ) );
	const char* channelNames[] = { "L", "R", "C", "S", "Lb", "Rb", "Lf", "Rf", "Cb", "Ls", "Rs" };
	for( int i = 0, ci = 0; ci < ( int )( sizeof( channelNames ) / sizeof( channelNames[0] ) ) && i < sink.OutputChannels(); ci++ )
	{
		if( ( sink.ChannelMask() & BIT( ci ) ) != 0 )
		{
			vuMeterRMS->SetLabel( i++, channelNames[ci] );
		}
	}

	for( int i = 0; i < NUM_SETTINGS; i++ )
	{
		settingValues[i] = mixerSettings[i].GetFloat();
	}
	PrepareSettings( settingValues );
	meters = false;
	firstMeterSequence = 0;
	memset( meterPeakTimes, 0, sizeof( meterPeakTimes ) );
	resetSerial++;
	streamChannels = sink.OutputChannels();
	memset( &lastStatus, 0, sizeof( lastStatus ) );
	memset( blockTimes, 0, sizeof( blockTimes ) );
	blockTimeIndex = lateBlocks = 0;
	lastSequence = 0;
	// The first frame of this link is empty even when an owner survived Shutdown.
	if( reverbOwner != NULL )
	{
		AcquireReverbCarrier();
		reverbOwner->OnCarrierReacquired();
	}
	PublishFrame( false ); // ReclaimAll permits this one empty restart frame.
	gate.store( 0, std::memory_order_seq_cst );	// last: every object a pull may read is ready
}

void idSoundHardware_Mixer::ShutAndWait()
{
	gate.store( 1, std::memory_order_seq_cst );
	while( inBlock.load( std::memory_order_seq_cst ) != 0 )
	{
		Sys_Sleep( 1 );
	}
}

void idSoundHardware_Mixer::Shutdown()
{
	ShutAndWait();
	//puncho - C4-R: no callback can retain either instance beyond this point.
	if( reverbMemory[0] != NULL )
	{
		reverbMix.ReclaimAll();
		memset( reverbOut, 0, sizeof( reverbOut ) );
		reverbMix.Shutdown();
		Mem_Free16( reverbMemory[0] );
		Mem_Free16( reverbMemory[1] );
		reverbMemory[0] = reverbMemory[1] = NULL;
	}
	reverbTickUs = 0;
#ifndef ID_RETAIL
	if( testHoldStartUs.load() != 0 )
	{
		idLib::Printf( "mixer hold: pullStartUs=%llu pullEndUs=%llu shutdownJoinedUs=%llu\n",
			testHoldStartUs.load(), testHoldEndUs.load(), ( unsigned long long )Sys_Microseconds() );
		testHoldStartUs.store( 0 );
	}
#endif
	sink.Close();
	mixer.Shutdown();
	if( mixerMemory != NULL )
	{
		Mem_Free16( mixerMemory );
		mixerMemory = NULL;
	}
	if( vuMeterRMS != NULL )
	{
		console->DestroyGraph( vuMeterRMS );
		vuMeterRMS = NULL;
	}
	if( vuMeterPeak != NULL )
	{
		console->DestroyGraph( vuMeterPeak );
		vuMeterPeak = NULL;
	}
	// CO-12: slots and their free list survive the samples freed after this call.
}

void idSoundHardware_Mixer::TickReverb()
{
	//puncho - C4-R: Update and a sample-fence publish use the same snapshot rules.
	if( reverbMemory[0] != NULL )
	{
		const uint64 nowUs = Sys_Microseconds();
		sndReverbMixIn_t in = {};
		in.zone = in.candidate = -1;
		in.published = link.Published();
		in.acknowledged = link.Acknowledged();
		in.statusSequence = lastStatus.sequence;
		memcpy( in.tail, lastStatus.reverbTail, sizeof( in.tail ) );
		in.mapSerial = reverbMapSerial;
		in.dtMs = reverbTickUs == 0 ? 0.0f : idMath::ClampFloat( 0.0f, s_smoothMaxDtMs.GetFloat(), ( float )( nowUs - reverbTickUs ) * 0.001f );
		reverbTickUs = nowUs;
		in.send = s_acousticReverbSend.GetFloat();
		in.fadeMs = s_acousticReverbFadeMs.GetFloat();
		in.returnGain = streamChannels == 1 ? 0.7071f : 1.0f;
		if( reverbOwner != NULL )
		{
			const idSoundAcoustics* bake = reverbOwner->acoustics;
			if( bake != NULL )
			{
				// The builder consumes seven floats, not the record's eighth roomSize field.
				static_assert( offsetof( sndReverb_t, decayTime ) == 0 && offsetof( sndReverb_t, earlyDb ) == 12 &&
					offsetof( sndReverb_t, lateDb ) == 16 && offsetof( sndReverb_t, preDelay ) == 20 &&
					offsetof( sndReverb_t, lateDelay ) == 24 && sizeof( sndReverb_t ) == 32, "room record layout" );
				in.zones = ( const float* )bake->GetZones();
				in.numZones = bake->NumZones();
				in.zoneStride = sizeof( sndReverb_t );
			}
			in.zone = reverbOwner->zone;
			in.candidate = reverbOwner->candidate;
			in.heard = reverbOwner->world == soundSystemLocal.currentSoundWorld;
			in.cut = reverbOwner->mixerCutPending;
			reverbOwner->mixerCutPending = false;
		}
		reverbMix.Tick( in, reverbOut );
		reverbMaxTickUs = Max( reverbMaxTickUs, ( unsigned int )( Sys_Microseconds() - nowUs ) );
	}
}

void idSoundHardware_Mixer::PublishFrame( bool tickRoom )
{
	// FenceSamples can publish without Update. Its frame needs its own room tick too.
	if( tickRoom && reverbMemory[0] != NULL )
	{
		lastStatus = link.ReadStatus();
		TickReverb();
	}
	sndMixFrame_t& frame = link.WriteFrame();
	frame.sequence = 0;	// Publish supplies the sequence.
	frame.resetSerial = resetSerial;
	frame.hdrSerial = hdrSerial;
	int mask;
	idSoundVoice::GetSurroundLayout( frame.numOut, mask );
	const int lfe = idSoundVoice::GetSurroundLfe();
	frame.lfeOut = frame.numOut > 1 && lfe >= 0 && lfe < frame.numOut ? lfe : -1;
	frame.wetOut[0] = frame.lfeOut == 0 ? 1 : 0;
	frame.wetOut[1] = frame.wetOut[0];
	for( int i = frame.wetOut[0] + 1; i < frame.numOut; i++ )
	{
		if( i != frame.lfeOut )
		{
			frame.wetOut[1] = i;
			break;
		}
	}
	frame.hdrOn = hdrOn;
	frame.meters = meters;
	frame.reserved[0] = frame.reserved[1] = 0;
	frame.hdr = hdrParms;
	frame.master = masterCoefs;
	memcpy( frame.reverb, reverbOut, sizeof( frame.reverb ) ); //puncho - C4-R: preserve indices and nonlive entries too.
	for( int i = 0; i < SND_MIX_MAX_VOICES; i++ )
	{
		frame.voices[i] = voices[i].command;
	}
	assert( frame.numOut >= 1 && frame.numOut <= SND_MIX_MAX_OUT && frame.numOut == streamChannels );
	assert( frame.lfeOut >= -1 && frame.lfeOut < frame.numOut && ( frame.numOut != 1 || frame.lfeOut == -1 ) );
	assert( frame.wetOut[0] >= 0 && frame.wetOut[0] < frame.numOut && frame.wetOut[0] != frame.lfeOut );
	assert( frame.wetOut[1] >= 0 && frame.wetOut[1] < frame.numOut && frame.wetOut[1] != frame.lfeOut );
	frame.publishUs = ( unsigned int )Sys_Microseconds();
	const unsigned long long sequence = link.Publish();
	if( meters && firstMeterSequence == 0 )
	{
		firstMeterSequence = sequence;
	}
}

void idSoundHardware_Mixer::Update()
{
#if defined( _DEBUG )
	AssertOutsideSampleFree();
#endif
	assert( idLib::IsMainThread() );
	if( sink.Lost() )
	{
		soundSystemLocal.SetNeedsRestart();
	}
	if( !sink.IsOpen() )
	{
		const int now = Sys_Milliseconds();
		if( now - lastResetTime > 1000 )
		{
			lastResetTime = now;
			Init();
		}
		return;
	}
	float values[NUM_SETTINGS];
	for( int i = 0; i < NUM_SETTINGS; i++ )
	{
		values[i] = mixerSettings[i].GetFloat();
	}
	if( memcmp( values, settingValues, sizeof( values ) ) != 0 )
	{
		memcpy( settingValues, values, sizeof( values ) );
		PrepareSettings( values );
	}
#ifndef ID_RETAIL
	if( testStallMS != 0 )
	{
		if( testHold )
		{
			testHoldStartUs.store( 0 );
			testHoldEndUs.store( 0 );
			testHoldMS.store( testStallMS );
		}
		else
		{
			testZeroUntilUs.store( Sys_Microseconds() + ( uint64 )testStallMS * 1000 );
		}
		testStallMS = 0;
	}
#endif
	const float masterGain = soundSystem->IsMuted() ? 0.0f : DBtoLinear( s_volume_dB.GetFloat() );
	sink.SetMasterVolume( masterGain );
	lastStatus = link.ReadStatus();
	if( reverbMemory[0] != NULL ) { TickReverb(); }
	showMixer = s_showMixer.GetInteger();
	const bool perfEnabled = s_showPerfData.GetBool();
	const bool showLevels = s_showLevelMeter.GetBool();
	const bool nextMeters = showLevels || soundSystemLocal.mixRecordFile != NULL;
	if( !meters && nextMeters )
	{
		firstMeterSequence = 0;
	}
	meters = nextMeters;
	const bool measurePublish = showMixer != 0 || perfEnabled;
	const uint64 publishStart = measurePublish ? Sys_Microseconds() : 0;
	PublishFrame( false );
	publishUs = measurePublish ? ( unsigned int )( Sys_Microseconds() - publishStart ) : 0;

	const int now = ( showMixer || perfEnabled || showLevels ) ? Sys_Milliseconds() : 0;
	const bool printPerf = ( showMixer || perfEnabled ) && now - lastPerfPrintTime >= 1000;
	if( showMixer || printPerf )
	{
		//puncho - DR-318: overlay refreshes reuse the second's figure without draining it.
		if( printPerf )
		{
			reportedMaxPublishWaitUs = maxPublishWaitUs.exchange( 0, std::memory_order_relaxed );
		}
		char scene[32];
		if( lastStatus.sceneDb == SND_HDR_SILENT )
		{
			idStr::snPrintf( scene, sizeof( scene ), "silent" );
		}
		else
		{
			idStr::snPrintf( scene, sizeof( scene ), "%.1f", lastStatus.sceneDb );
		}
		char line[512];
		idStr::snPrintf( line, sizeof( line ),
			"mixer: mixed=%d skipped=%d scene=%s top=%.1f factor=%.5f compDb=%.2f limitDb=%.2f peak=%.5f blockUs=%u maxUs=%u maxPublishWaitUs=%u late=%u waitUs=%u latencyFrames=%d publishUs=%u blocks=%u seq=%llu",
			lastStatus.voicesMixed, lastStatus.voicesSkipped, scene, lastStatus.windowTopDb, lastStatus.windowFactor,
			lastStatus.master.compSumReductionDb / SND_MASTER_PIECE, -LinearToDB( Max( 1e-20f, lastStatus.master.limitMinGain ) ),
			lastStatus.master.outPeak, lastStatus.blockUs, lastStatus.maxBlockUs, reportedMaxPublishWaitUs, lastStatus.lateBlocks, lastStatus.waitUs,
			sink.LatencyFrames(), publishUs, lastStatus.blocks, lastStatus.sequence );
		if( showMixer == 2 )
		{
			const int used = strlen( line );
			idStr::snPrintf( line + used, sizeof( line ) - used, " fences=%u waits=%u sumWaitUs=%llu maxWaitUs=%llu stalls=%u pulls=%u",
				fences, fenceWaits, ( unsigned long long )fenceWaitUs, ( unsigned long long )maxFenceWaitUs, stalls, pulls.load() );
		}
		if( showMixer )
		{
			console->PrintOverlay( mixerOverlay, JUSTIFY_LEFT, "%s", line );
		}
		if( printPerf )
		{
			idLib::Printf( "%s\n", line );
		}
		char roomLine[256];
		idStr::snPrintf( roomLine, sizeof( roomLine ),
			"room: zone=%d candidate=%d state/zone=%d/%d,%d/%d in=%.5f,%.5f tail=%d,%d memory=%d tickMaxUs=%u",
			reverbOwner && reverbOwner->acoustics ? reverbOwner->zone : -1,
			reverbOwner && reverbOwner->acoustics ? reverbOwner->candidate : -1, //puncho - RM2-5: no bake, no displayed room.
			reverbMix.State( 0 ), reverbMix.Zone( 0 ), reverbMix.State( 1 ), reverbMix.Zone( 1 ),
			reverbOut[0].inGain, reverbOut[1].inGain, lastStatus.reverbTail[0], lastStatus.reverbTail[1],
			reverbMemory[0] != NULL ? 1 : 0, reverbMaxTickUs );
		if( showMixer ) { console->PrintOverlay( reverbOverlay, JUSTIFY_LEFT, "%s", roomLine ); }
		if( printPerf ) { idLib::Printf( "%s\n", roomLine ); }
		if( printPerf )
		{
			lastPerfPrintTime = now;
		}
#ifndef ID_RETAIL
		if( printPerf && perfEnabled )
		{
			for( int i = 0; i < SND_MIX_MAX_VOICES; i++ )
			{
				if( voices[i].started && voices[i].command.state == SND_VOICE_PLAY )
				{
					idLib::Printf( "mixer amplitude: slot=%d generation=%u noMeter=%d amplitude=%.9g\n",
						i, voices[i].command.generation, voices[i].noMeter ? 1 : 0, voices[i].GetAmplitude() );
				}
			}
		}
#endif
	}
	vuMeterRMS->Enable( showLevels );
	vuMeterPeak->Enable( showLevels );
	if( !meters || firstMeterSequence == 0 || lastStatus.sequence < firstMeterSequence )
	{
		return;
	}
	float rms[SND_MIX_MAX_OUT], peak[SND_MIX_MAX_OUT];
	for( int i = 0; i < streamChannels; i++ )
	{
		rms[i] = lastStatus.outRms[i] * masterGain;
		peak[i] = lastStatus.outPeak[i] * masterGain;
	}
	if( soundSystemLocal.mixRecordFile != NULL )
	{
		MixRecord_Levels( rms, peak, streamChannels );
	}
	if( showLevels )
	{
		float left = 100.0f, top = 100.0f, width = 20.0f, height = 200.0f;
		sscanf( s_meterPosition.GetString(), "%f %f %f %f", &left, &top, &width, &height );
		vuMeterRMS->SetPosition( left, top, width * streamChannels, height );
		vuMeterPeak->SetPosition( left, top, width * streamChannels, height );
		for( int i = 0; i < streamChannels; i++ )
		{
			if( meterPeakTimes[i] < now )
			{
				vuMeterPeak->SetValue( i, vuMeterPeak->GetValue( i ) * 0.9f, colorRed );
			}
			vuMeterRMS->SetValue( i, rms[i], idVec4( 0.5f, 1.0f, 0.0f, 1.0f ) );
			if( peak[i] >= vuMeterPeak->GetValue( i ) )
			{
				vuMeterPeak->SetValue( i, peak[i], colorRed );
				meterPeakTimes[i] = now + s_meterTopTime.GetInteger();
			}
		}
	}
}

idSoundVoice* idSoundHardware_Mixer::AllocateVoice( const idSoundSample* leadinSample, const idSoundSample* loopingSample )
{
	if( leadinSample == NULL || !sink.IsOpen() || gate.load( std::memory_order_seq_cst ) != 0 || freeCount == 0 )
	{
		return NULL;
	}
	idSoundVoice* voice = &voices[freeSlots[freeHead]];
	freeHead = ( freeHead + 1 ) % SND_MIX_MAX_VOICES;
	freeCount--;
	voice->Create( leadinSample, loopingSample );
	return voice;
}

void idSoundHardware_Mixer::FreeVoice( idSoundVoice* voice )
{
	assert( idLib::IsMainThread() );
	assert( voice != NULL && voice->slot >= 0 && voice->slot < SND_MIX_MAX_VOICES );
	assert( freeCount < SND_MIX_MAX_VOICES );
	voice->Stop();
	freeSlots[( freeHead + freeCount ) % SND_MIX_MAX_VOICES] = voice->slot;
	freeCount++;
}

unsigned int idSoundHardware_Mixer::NextGeneration()
{
	if( ++generation == 0 )
	{
		generation++;
	}
	return generation;
}

void idSoundHardware_Mixer::NoteStop()
{
	if( sink.IsOpen() && gate.load( std::memory_order_seq_cst ) == 0 )
	{
		const unsigned long long nextStop = link.Published() + 1;
		if( s_debugHardware.GetBool() && stopSequence != nextStop )
		{
			idLib::Printf( "mixer stop: sequence=%llu\n", nextStop );
		}
		stopSequence = nextStop;
	}
}

void idSoundHardware_Mixer::TracePurgeCandidate( idSoundSample* sample )
{
	if( s_debugHardware.GetBool() )
	{
		idLib::Printf( "mixer purge candidate: %s\n", sample != NULL ? sample->GetName() : "<begin pass>" );
#ifndef ID_RETAIL
		if( sample == NULL )
		{
			ReportKeptCandidates();
		}
		if( testLoadActive && sample != NULL && sample->IsLoaded() )
		{
			sample->SetMixerPurgeCandidate( true );
		}
#endif
	}
}

void idSoundHardware_Mixer::FenceSamples( const char* sampleName )
{
	assert( idLib::IsMainThread() );
	if( s_debugHardware.GetBool() )
	{
		idLib::Printf( "mixer %s: %s stop=%llu published=%llu\n", sampleName != NULL ? "free" : "purge pass end",
			sampleName != NULL ? sampleName : "<load pass>", stopSequence, link.Published() );
	}
	if( !sink.IsOpen() || gate.load( std::memory_order_seq_cst ) != 0 )
	{
		return;
	}
	fences++;
	if( link.Acknowledged() >= stopSequence )
	{
		return;
	}
	if( link.Published() < stopSequence )
	{
		PublishFrame();
	}
	const unsigned long long target = Min( stopSequence, link.Published() );
	const uint64 start = Sys_Microseconds();
	uint64 lastBlockTime = start;
	unsigned int begun = link.BlocksBegun();
	fenceWaits++;
	while( link.Acknowledged() < target )
	{
		Sys_Sleep( 1 );
		if( link.Acknowledged() >= target )
		{
			break;
		}
		const uint64 now = Sys_Microseconds();
		const unsigned int nextBegun = link.BlocksBegun();
		if( nextBegun != begun )
		{
			begun = nextBegun;
			lastBlockTime = now;
		}
		else if( now - lastBlockTime >= 250000 )
		{
			ShutAndWait();
			soundSystemLocal.SetNeedsRestart();
			stalls++;
			break;
		}
	}
	const uint64 waited = Sys_Microseconds() - start;
	maxFenceWaitUs = Max( maxFenceWaitUs, waited );
	fenceWaitUs += waited;
	if( s_debugHardware.GetBool() )
	{
		idLib::Printf( "mixer fence: sample=%s waitUs=%llu fences=%u waits=%u maxWaitUs=%llu stalls=%u stop=%llu ack=%llu\n",
			sampleName != NULL ? sampleName : "<load pass>", ( unsigned long long )waited,
			fences, fenceWaits, ( unsigned long long )maxFenceWaitUs, stalls, stopSequence, link.Acknowledged() );
	}
}

void idSoundHardware_Mixer::BeginSampleFree()
{
	assert( idLib::IsMainThread() );
#if defined( _DEBUG )
	assert( !insideSampleFree );
	insideSampleFree = true;
#endif
}

void idSoundHardware_Mixer::EndSampleFree()
{
#if defined( _DEBUG )
	assert( insideSampleFree );
	insideSampleFree = false;
#endif
}

void idSoundHardware_Mixer::AssertOutsideSampleFree() const
{
#if defined( _DEBUG )
	assert( !insideSampleFree );	// CO-22: Start and Update test this; PublishFrame must not.
#endif
}

void idSoundHardware_Mixer::PullBlock( float* out, int numOut )
{
	pulls.fetch_add( 1, std::memory_order_seq_cst );
	assert( inBlock.load( std::memory_order_seq_cst ) == 0 );
	inBlock.store( 1, std::memory_order_seq_cst );
	if( gate.load( std::memory_order_seq_cst ) != 0 || numOut != streamChannels )
	{
		memset( out, 0, SND_MIX_BLOCK * numOut * sizeof( float ) );
		inBlock.store( 0, std::memory_order_seq_cst );
		return;
	}
#ifndef ID_RETAIL
	const unsigned int holdMS = testHoldMS.exchange( 0 );
	if( holdMS != 0 )
	{
		testHoldStartUs.store( Sys_Microseconds() );
		Sys_Sleep( holdMS ); // deliberate developer-only hold with inBlock set
	}
	unsigned long long zeroUntil = testZeroUntilUs.load();
	if( zeroUntil != 0 )
	{
		if( Sys_Microseconds() < zeroUntil )
		{
			memset( out, 0, SND_MIX_BLOCK * numOut * sizeof( float ) );
			if( holdMS != 0 )
			{
				testHoldEndUs.store( Sys_Microseconds() );
			}
			inBlock.store( 0, std::memory_order_seq_cst );
			return;
		}
		// Do not erase a new request published while this expired one was read.
		testZeroUntilUs.compare_exchange_strong( zeroUntil, 0 );
	}
#endif
	const unsigned int oldCSR = _mm_getcsr();
	_mm_setcsr( oldCSR | 0x8040 );
	const uint64 start = Sys_Microseconds();
	const sndMixFrame_t& frame = link.BeginBlock();
	sndMixStatus_t& status = link.WriteStatus();
	mixer.Block( frame, status, out );
	const unsigned int elapsed = ( unsigned int )( Sys_Microseconds() - start );
	blockTimes[blockTimeIndex] = elapsed;
	blockTimeIndex = ( blockTimeIndex + 1 ) % 100;
	unsigned int maximum = 0;
	for( int i = 0; i < 100; i++ )
	{
		maximum = Max( maximum, blockTimes[i] );
	}
	lateBlocks += elapsed > 5000 ? 1 : 0;
	status.blockUs = elapsed;
	status.maxBlockUs = maximum;
	status.lateBlocks = lateBlocks;
	if( frame.sequence != lastSequence )
	{
		//puncho - RM2-4: BeginBlock may adopt a frame stamped after this block's clock read.
		status.waitUs = ( unsigned int )start >= frame.publishUs ? ( unsigned int )start - frame.publishUs : 0;
		//puncho - DR-318: approximate diagnostic, a racing exchange may lose or carry a value.
		const unsigned int largestWait = maxPublishWaitUs.load( std::memory_order_relaxed );
		if( status.waitUs > largestWait )
		{
			maxPublishWaitUs.store( status.waitUs, std::memory_order_relaxed );
		}
	}
	else
	{
		status.waitUs = 0;
	}
	lastSequence = frame.sequence;
	link.EndBlock();
	_mm_setcsr( oldCSR );
#ifndef ID_RETAIL
	if( holdMS != 0 )
	{
		testHoldEndUs.store( Sys_Microseconds() );
	}
#endif
	inBlock.store( 0, std::memory_order_seq_cst );
}

//puncho - C4-R: allocation failure removes only the room, never the bake or its occlusion.
bool idSoundHardware_Mixer::AcquireReverbCarrier()
{
	assert( idLib::IsMainThread() );
	if( !sink.IsOpen() ) { return false; }
	if( reverbMemory[0] != NULL ) { return true; }
	bool take = true;
#ifndef ID_RETAIL
	take = !s_mixTestNoRoomMemory.GetBool();
#endif
	if( take )
	{
		const size_t bytes = idSoundConvolver::MemoryBytes( SND_CONV_MAX_PARTITIONS );
		reverbMemory[0] = Mem_Alloc16( bytes, TAG_AUDIO );
		reverbMemory[1] = Mem_Alloc16( bytes, TAG_AUDIO );
		if( reverbMemory[0] != NULL && reverbMemory[1] != NULL && reverbMix.Init( reverbMemory[0], reverbMemory[1] ) )
		{
			reverbTickUs = 0;
			return true;
		}
	}
	Mem_Free16( reverbMemory[0] );
	Mem_Free16( reverbMemory[1] );
	reverbMemory[0] = reverbMemory[1] = NULL;
	idLib::Warning( "sound mixer: no memory for the room, baked occlusion retained, playing dry" );
	return true;
}

void idSoundHardware_Mixer::SubmitReverbFrame( const reverbCarrierFrame_t& frame )
{
	//puncho - C4-R: retained shared face. Update takes the plain class input directly.
}

void idSoundHardware_Mixer::BeginReverbMap( idSoundReverb* owner )
{
	reverbMapSerial++; //puncho - C4-R: records have a new lifetime.
	reverbOwner = owner;
}

void idSoundHardware_Mixer::EndReverbMap( bool soundCarried )
{
	reverbMapSerial++; //puncho - C4-R: a carried owner can remain, its records cannot.
	if( !soundCarried )
	{
		reverbOwner = NULL;
	}
}

void idSoundHardware_Mixer::DetachReverb( const idSoundReverb* owner )
{
	if( reverbOwner == owner )
	{
		reverbOwner = NULL;
	}
}

void idSoundHardware_Mixer::PrintReverbVoices() const
{
	idLib::Printf( "room: zone=%d candidate=%d state/zone=%d/%d,%d/%d in=%.5f,%.5f tail=%d,%d memory=%d tickMaxUs=%u\n",
		reverbOwner && reverbOwner->acoustics ? reverbOwner->zone : -1,
		reverbOwner && reverbOwner->acoustics ? reverbOwner->candidate : -1, //puncho - RM2-5: match the overlay.
		reverbMix.State( 0 ), reverbMix.Zone( 0 ), reverbMix.State( 1 ), reverbMix.Zone( 1 ),
		reverbOut[0].inGain, reverbOut[1].inGain, lastStatus.reverbTail[0], lastStatus.reverbTail[1],
		reverbMemory[0] != NULL ? 1 : 0, reverbMaxTickUs );
}

bool idSoundHardware_Mixer::GetActiveVoiceCounts( uint32& sourceVoices, uint32& submixVoices ) const
{
	if( !sink.IsOpen() )
	{
		return false;
	}
	sourceVoices = lastStatus.voicesMixed;
	submixVoices = 0;
	return true;
}

#endif // USE_SOUND_MIXER
