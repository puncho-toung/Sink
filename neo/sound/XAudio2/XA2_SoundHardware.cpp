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
#pragma hdrstop
#include "precompiled.h"
#include "../snd_local.h"
#if defined(USE_DOOMCLASSIC)
	#include "../../../doomclassic/doom/i_sound.h"
#endif

idCVar s_showLevelMeter( "s_showLevelMeter", "0", CVAR_BOOL | CVAR_ARCHIVE, "Show VU meter" );
idCVar s_meterTopTime( "s_meterTopTime", "1000", CVAR_INTEGER | CVAR_ARCHIVE, "How long (in milliseconds) peaks are displayed on the VU meter" );
idCVar s_meterPosition( "s_meterPosition", "100 100 20 200", CVAR_ARCHIVE, "VU meter location (x y w h)" );
idCVar s_device( "s_device", "-1", CVAR_INTEGER | CVAR_ARCHIVE, "Which audio device to use (listDevices to list, -1 for default)" );
idCVar s_showPerfData( "s_showPerfData", "0", CVAR_BOOL, "Show XAudio2 Performance data" );
extern idCVar s_volume_dB;

#if defined(USE_WINRT)
HRESULT GetAudioDeviceDetails( _In_ IMMDevice* immDevice, _Out_ AudioDevice* pInfo )
{
	IPropertyStore* propStore = nullptr;
	PROPVARIANT     varName;
	PROPVARIANT     varId;

	PropVariantInit( &varId );
	PropVariantInit( &varName );

	HRESULT hResult = immDevice->OpenPropertyStore( STGM_READ, &propStore );

	if( SUCCEEDED( hResult ) )
	{
		hResult = propStore->GetValue( PKEY_AudioEndpoint_Path, &varId );
	}

	if( SUCCEEDED( hResult ) )
	{
		hResult = propStore->GetValue( PKEY_Device_FriendlyName, &varName );
	}

	if( SUCCEEDED( hResult ) )
	{
		assert( varId.vt == VT_LPWSTR );
		assert( varName.vt == VT_LPWSTR );

		// Now save somewhere the device display name & id
		pInfo->name = varName.pwszVal;
		pInfo->id = varId.pwszVal;
	}

	PropVariantClear( &varName );
	PropVariantClear( &varId );

	if( propStore != nullptr )
	{
		propStore->Release();
	}

	return hResult;
}

std::vector<AudioDevice> EnumerateAudioDevices( _Out_opt_ AudioDevice* defaultDevice = nullptr )
{
	UINT32                   deviceCount      = 0;
	IMMDeviceEnumerator*     immDevEnum       = nullptr;
	IMMDeviceCollection*     immDevCollection = nullptr;
	IMMDevice*               immDev           = nullptr;
	std::vector<AudioDevice> vAudioDevices;

	HRESULT hResult = CoCreateInstance(
						  __uuidof( MMDeviceEnumerator ), NULL,
						  CLSCTX_ALL, __uuidof( IMMDeviceEnumerator ), ( void** ) &immDevEnum );

	if( FAILED( hResult ) )
	{
		idLib::Warning( "Failed to get audio enumerator" );
		return std::move( vAudioDevices );
	}

	if( defaultDevice != nullptr )
	{
		ZeroMemory( defaultDevice, sizeof( AudioDevice ) );

		IMMDevice* defaultImmDev = nullptr;

		// @pjb: get the default audio endpoint and make it the first one in the list
		if( SUCCEEDED( immDevEnum->GetDefaultAudioEndpoint( eRender, eConsole, &defaultImmDev ) ) )
		{
			GetAudioDeviceDetails( defaultImmDev, defaultDevice );
			defaultImmDev->Release();
		}
	}

	hResult = immDevEnum->EnumAudioEndpoints( eRender, DEVICE_STATE_ACTIVE, &immDevCollection );
	if( FAILED( hResult ) )
	{
		idLib::Warning( "Failed to get audio endpoints" );
		return std::move( vAudioDevices );
	}

	hResult = immDevCollection->GetCount( &deviceCount );
	if( FAILED( hResult ) )
	{
		idLib::Warning( "No audio devices found" );
		return std::move( vAudioDevices );
	}

	for( UINT i = 0; i < deviceCount; i++ )
	{
		AudioDevice ad;

		hResult = immDevCollection->Item( i, &immDev );
		if( SUCCEEDED( hResult ) )
		{
			hResult = GetAudioDeviceDetails( immDev, &ad );
		}

		if( SUCCEEDED( hResult ) )
		{
			vAudioDevices.push_back( ad );
		}

		if( immDev != nullptr )
		{
			immDev->Release();
		}
	}

	immDevCollection->Release();
	immDevEnum->Release();

	return std::move( vAudioDevices );
}
#endif

/*
========================
idSoundHardware_XAudio2::idSoundHardware_XAudio2
========================
*/
idSoundHardware_XAudio2::idSoundHardware_XAudio2()
{
	pXAudio2 = NULL;
	pMasterVoice = NULL;

	//puncho start - WP-SB4: no graph, no owner, nothing listed
	pReverbVoices[ 0 ] = NULL;
	pReverbVoices[ 1 ] = NULL;
	pReverbBus = NULL;
	reverbOwner = NULL;
	reverbSampleRate = 0;
	reverbBusVoices = 0;
	reverbCarrierLive = false;
	reverbDisabledMidTail = false;
	reverbBusAttached = false;
	ResetReverbSent( 0 );
	ResetReverbSent( 1 );
	memset( reverbSentParams, 0, sizeof( reverbSentParams ) );
	//puncho end

	vuMeterRMS = NULL;
	vuMeterPeak = NULL;

	outputChannels = 0;
	channelMask = 0;

	voices.SetNum( 0 );
	zombieVoices.SetNum( 0 );
	freeVoices.SetNum( 0 );

	lastResetTime = 0;
}

void listDevices_f( const idCmdArgs& args )
{

	IXAudio2* pXAudio2 = soundSystemLocal.hardware.GetIXAudio2();

	if( pXAudio2 == NULL )
	{
		idLib::Warning( "No xaudio object" );
		return;
	}

// RB: not available on Windows 8 SDK
#if defined(USE_WINRT) //(_WIN32_WINNT >= 0x0602 /*_WIN32_WINNT_WIN8*/)
	AudioDevice defaultDevice;
	auto vAudioDevices = EnumerateAudioDevices( &defaultDevice );
	if( vAudioDevices.size() == 0 )
	{
		idLib::Warning( "No audio devices found" );
		return;
	}

	for( size_t i = 0; i < vAudioDevices.size(); ++i )
	{
		idLib::Printf( "%s %3d: %S %S\n", vAudioDevices[i].id == defaultDevice.id ? "*" : " ", i, vAudioDevices[i].name.c_str(), vAudioDevices[i].id.c_str() );
	}
#else
	UINT32 deviceCount = 0;
	if( pXAudio2->GetDeviceCount( &deviceCount ) != S_OK || deviceCount == 0 )
	{
		idLib::Warning( "No audio devices found" );
		return;
	}

	for( unsigned int i = 0; i < deviceCount; i++ )
	{
		XAUDIO2_DEVICE_DETAILS deviceDetails;
		if( pXAudio2->GetDeviceDetails( i, &deviceDetails ) != S_OK )
		{
			continue;
		}
		idStaticList< const char*, 5 > roles;
		if( deviceDetails.Role & DefaultConsoleDevice )
		{
			roles.Append( "Console Device" );
		}
		if( deviceDetails.Role & DefaultMultimediaDevice )
		{
			roles.Append( "Multimedia Device" );
		}
		if( deviceDetails.Role & DefaultCommunicationsDevice )
		{
			roles.Append( "Communications Device" );
		}
		if( deviceDetails.Role & DefaultGameDevice )
		{
			roles.Append( "Game Device" );
		}
		idStaticList< const char*, 11 > channelNames;
		if( deviceDetails.OutputFormat.dwChannelMask & SPEAKER_FRONT_LEFT )
		{
			channelNames.Append( "Front Left" );
		}
		if( deviceDetails.OutputFormat.dwChannelMask & SPEAKER_FRONT_RIGHT )
		{
			channelNames.Append( "Front Right" );
		}
		if( deviceDetails.OutputFormat.dwChannelMask & SPEAKER_FRONT_CENTER )
		{
			channelNames.Append( "Front Center" );
		}
		if( deviceDetails.OutputFormat.dwChannelMask & SPEAKER_LOW_FREQUENCY )
		{
			channelNames.Append( "Low Frequency" );
		}
		if( deviceDetails.OutputFormat.dwChannelMask & SPEAKER_BACK_LEFT )
		{
			channelNames.Append( "Back Left" );
		}
		if( deviceDetails.OutputFormat.dwChannelMask & SPEAKER_BACK_RIGHT )
		{
			channelNames.Append( "Back Right" );
		}
		if( deviceDetails.OutputFormat.dwChannelMask & SPEAKER_FRONT_LEFT_OF_CENTER )
		{
			channelNames.Append( "Front Left of Center" );
		}
		if( deviceDetails.OutputFormat.dwChannelMask & SPEAKER_FRONT_RIGHT_OF_CENTER )
		{
			channelNames.Append( "Front Right of Center" );
		}
		if( deviceDetails.OutputFormat.dwChannelMask & SPEAKER_BACK_CENTER )
		{
			channelNames.Append( "Back Center" );
		}
		if( deviceDetails.OutputFormat.dwChannelMask & SPEAKER_SIDE_LEFT )
		{
			channelNames.Append( "Side Left" );
		}
		if( deviceDetails.OutputFormat.dwChannelMask & SPEAKER_SIDE_RIGHT )
		{
			channelNames.Append( "Side Right" );
		}
		char mbcsDisplayName[ 256 ];
		wcstombs( mbcsDisplayName, deviceDetails.DisplayName, sizeof( mbcsDisplayName ) );
		idLib::Printf( "%3d: %s\n", i, mbcsDisplayName );
		idLib::Printf( "     %d channels, %d Hz\n", deviceDetails.OutputFormat.Format.nChannels, deviceDetails.OutputFormat.Format.nSamplesPerSec );
		if( channelNames.Num() != deviceDetails.OutputFormat.Format.nChannels )
		{
			idLib::Printf( S_COLOR_YELLOW "WARNING: " S_COLOR_RED "Mismatch between # of channels and channel mask\n" );
		}
		if( channelNames.Num() == 1 )
		{
			idLib::Printf( "     %s\n", channelNames[0] );
		}
		else if( channelNames.Num() == 2 )
		{
			idLib::Printf( "     %s and %s\n", channelNames[0], channelNames[1] );
		}
		else if( channelNames.Num() > 2 )
		{
			idLib::Printf( "     %s", channelNames[0] );
			for( int i = 1; i < channelNames.Num() - 1; i++ )
			{
				idLib::Printf( ", %s", channelNames[i] );
			}
			idLib::Printf( ", and %s\n", channelNames[channelNames.Num() - 1] );
		}
		if( roles.Num() == 1 )
		{
			idLib::Printf( "     Default %s\n", roles[0] );
		}
		else if( roles.Num() == 2 )
		{
			idLib::Printf( "     Default %s and %s\n", roles[0], roles[1] );
		}
		else if( roles.Num() > 2 )
		{
			idLib::Printf( "     Default %s", roles[0] );
			for( int i = 1; i < roles.Num() - 1; i++ )
			{
				idLib::Printf( ", %s", roles[i] );
			}
			idLib::Printf( ", and %s\n", roles[roles.Num() - 1] );
		}
	}
#endif
// RB end
}

/*
========================
idSoundHardware_XAudio2::Init
========================
*/
void idSoundHardware_XAudio2::Init()
{

	cmdSystem->AddCommand( "listDevices", listDevices_f, 0, "Lists the connected sound devices", NULL );

	DWORD xAudioCreateFlags = 0;

// RB: not available on Windows 8 SDK
#if !defined(USE_WINRT) && defined(_DEBUG) // (_WIN32_WINNT < 0x0602 /*_WIN32_WINNT_WIN8*/) && defined(_DEBUG)
	xAudioCreateFlags |= XAUDIO2_DEBUG_ENGINE;
#endif
// RB end

	XAUDIO2_PROCESSOR xAudioProcessor = XAUDIO2_DEFAULT_PROCESSOR;

// RB: not available on Windows 8 SDK
	if( FAILED( XAudio2Create( &pXAudio2, xAudioCreateFlags, xAudioProcessor ) ) )
	{
#if !defined(USE_WINRT) && defined(_DEBUG) // (_WIN32_WINNT < 0x0602 /*_WIN32_WINNT_WIN8*/) && defined(_DEBUG)
		if( xAudioCreateFlags & XAUDIO2_DEBUG_ENGINE )
		{
			// in case the debug engine isn't installed
			xAudioCreateFlags &= ~XAUDIO2_DEBUG_ENGINE;
			if( FAILED( XAudio2Create( &pXAudio2, xAudioCreateFlags, xAudioProcessor ) ) )
			{
				idLib::FatalError( "Failed to create XAudio2 engine.  Try installing the latest DirectX." );
				return;
			}
		}
		else
#endif
// RB end
		{
			idLib::FatalError( "Failed to create XAudio2 engine.  Try installing the latest DirectX." );
			return;
		}
	}
#ifdef _DEBUG
	XAUDIO2_DEBUG_CONFIGURATION debugConfiguration = { 0 };
	debugConfiguration.TraceMask = XAUDIO2_LOG_WARNINGS;
	debugConfiguration.BreakMask = XAUDIO2_LOG_ERRORS;
	pXAudio2->SetDebugConfiguration( &debugConfiguration );
#endif

	// Register the sound engine callback
	pXAudio2->RegisterForCallbacks( &soundEngineCallback );
	soundEngineCallback.hardware = this;
	DWORD outputSampleRate = SND_MIX_SAMPLE_RATE; //puncho - R-E6-39: the voices' filter rate is this rate (snd_local.h) // Max( (DWORD)XAUDIO2FX_REVERB_MIN_FRAMERATE, Min( (DWORD)XAUDIO2FX_REVERB_MAX_FRAMERATE, deviceDetails.OutputFormat.Format.nSamplesPerSec ) );

	idCmdArgs args;
	listDevices_f( args );

	// RB: not available on Windows 8 SDK
#if defined(USE_WINRT) //(_WIN32_WINNT >= 0x0602 /*_WIN32_WINNT_WIN8*/)
	AudioDevice defaultDevice;
	std::vector<AudioDevice> vAudioDevices = EnumerateAudioDevices( &defaultDevice );

	if( !vAudioDevices.empty() )
	{

		AudioDevice selectedDevice;

		int preferredDevice = s_device.GetInteger();
		bool validPreference = ( preferredDevice >= 0 && preferredDevice < ( int )vAudioDevices.size() );
		// Do we select a device automatically?
		if( validPreference )
		{
			// Use the user's selected device
			selectedDevice = vAudioDevices[preferredDevice];
		}
		else if( !defaultDevice.id.empty() )
		{
			// Fall back to the default device if there is one
			selectedDevice = defaultDevice;
		}
		else
		{
			// Fall back to first device
			selectedDevice = vAudioDevices[0];
		}

		if( SUCCEEDED( pXAudio2->CreateMasteringVoice( &pMasterVoice,
					   XAUDIO2_DEFAULT_CHANNELS,
					   outputSampleRate,
					   0,
					   selectedDevice.id.c_str(),
					   NULL,
					   AudioCategory_GameEffects ) ) )
		{
			XAUDIO2_VOICE_DETAILS deviceDetails;
			pMasterVoice->GetVoiceDetails( &deviceDetails );

			pMasterVoice->SetVolume( DBtoLinear( s_volume_dB.GetFloat() ) );

			outputChannels = deviceDetails.InputChannels;
			DWORD win8_channelMask;
			pMasterVoice->GetChannelMask( &win8_channelMask );

			channelMask = ( unsigned int )win8_channelMask;
			idLib::Printf( "Using device: %S\n", selectedDevice.name.c_str() );
		}
		else
		{
			idLib::Warning( "Failed to create master voice" );
			pXAudio2->Release();
			pXAudio2 = NULL;
			return;
		}
	}

#else
	UINT32 deviceCount = 0;
	if( pXAudio2->GetDeviceCount( &deviceCount ) != S_OK || deviceCount == 0 )
	{
		idLib::Warning( "No audio devices found" );
		pXAudio2->Release();
		pXAudio2 = NULL;
		return;
	}

	int preferredDevice = s_device.GetInteger();
	if( preferredDevice < 0 || preferredDevice >= ( int )deviceCount )
	{
		int preferredChannels = 0;
		for( unsigned int i = 0; i < deviceCount; i++ )
		{
			XAUDIO2_DEVICE_DETAILS deviceDetails;
			if( pXAudio2->GetDeviceDetails( i, &deviceDetails ) != S_OK )
			{
				continue;
			}

			if( deviceDetails.Role & DefaultGameDevice )
			{
				// if we find a device the user marked as their preferred 'game' device, then always use that
				preferredDevice = i;
				preferredChannels = deviceDetails.OutputFormat.Format.nChannels;
				break;
			}

			if( deviceDetails.OutputFormat.Format.nChannels > preferredChannels )
			{
				preferredDevice = i;
				preferredChannels = deviceDetails.OutputFormat.Format.nChannels;
			}
		}
	}

	idLib::Printf( "Using device %d\n", preferredDevice );

	XAUDIO2_DEVICE_DETAILS deviceDetails;
	if( pXAudio2->GetDeviceDetails( preferredDevice, &deviceDetails ) != S_OK )
	{
		// One way this could happen is if a device is removed between the loop and this line of code
		// Highly unlikely but possible
		idLib::Warning( "Failed to get device details" );
		pXAudio2->Release();
		pXAudio2 = NULL;
		return;
	}


	if( FAILED( pXAudio2->CreateMasteringVoice( &pMasterVoice, XAUDIO2_DEFAULT_CHANNELS, outputSampleRate, 0, preferredDevice, NULL ) ) )
	{
		idLib::Warning( "Failed to create master voice" );
		pXAudio2->Release();
		pXAudio2 = NULL;
		return;
	}
	pMasterVoice->SetVolume( DBtoLinear( s_volume_dB.GetFloat() ) );

	outputChannels = deviceDetails.OutputFormat.Format.nChannels;
	channelMask = deviceDetails.OutputFormat.dwChannelMask;

#endif // #if (_WIN32_WINNT < 0x0602 /*_WIN32_WINNT_WIN8*/)

	idSoundVoice::InitSurround( outputChannels, channelMask );

#if defined(USE_DOOMCLASSIC)
	// ---------------------
	// Initialize the Doom classic sound system.
	// ---------------------
	I_InitSoundHardware( outputChannels, channelMask );
#endif

	// ---------------------
	// Create VU Meter Effect
	// ---------------------
	IUnknown* vuMeter = NULL;
	XAudio2CreateVolumeMeter( &vuMeter, 0 );

	XAUDIO2_EFFECT_DESCRIPTOR descriptor;
	descriptor.InitialState = true;
	descriptor.OutputChannels = outputChannels;
	descriptor.pEffect = vuMeter;

	XAUDIO2_EFFECT_CHAIN chain;
	chain.EffectCount = 1;
	chain.pEffectDescriptors = &descriptor;

	pMasterVoice->SetEffectChain( &chain );

	vuMeter->Release();

	// ---------------------
	// Create VU Meter Graph
	// ---------------------

	vuMeterRMS = console->CreateGraph( outputChannels );
	vuMeterPeak = console->CreateGraph( outputChannels );
	vuMeterRMS->Enable( false );
	vuMeterPeak->Enable( false );

	memset( vuMeterPeakTimes, 0, sizeof( vuMeterPeakTimes ) );

	vuMeterPeak->SetFillMode( idDebugGraph::GRAPH_LINE );
	vuMeterPeak->SetBackgroundColor( idVec4( 0.0f, 0.0f, 0.0f, 0.0f ) );

	vuMeterRMS->AddGridLine( 0.500f, idVec4( 0.5f, 0.5f, 0.5f, 1.0f ) );
	vuMeterRMS->AddGridLine( 0.250f, idVec4( 0.5f, 0.5f, 0.5f, 1.0f ) );
	vuMeterRMS->AddGridLine( 0.125f, idVec4( 0.5f, 0.5f, 0.5f, 1.0f ) );

	const char* channelNames[] = { "L", "R", "C", "S", "Lb", "Rb", "Lf", "Rf", "Cb", "Ls", "Rs" };
	for( int i = 0, ci = 0; ci < sizeof( channelNames ) / sizeof( channelNames[0] ); ci++ )
	{
		if( ( channelMask & BIT( ci ) ) == 0 )
		{
			continue;
		}
		vuMeterRMS->SetLabel( i, channelNames[ ci ] );
		i++;
	}

	//puncho - R-SB4-3: the submix made here in every process, which nothing sent to and whose failure was fatal, is gone

	// XAudio doesn't really impose a maximum number of voices
	voices.SetNum( voices.Max() );
	freeVoices.SetNum( voices.Max() );
	zombieVoices.SetNum( 0 );
	for( int i = 0; i < voices.Num(); i++ )
	{
		freeVoices[i] = &voices[i];
	}
// RB end

	//puncho start - R-SB4-32: Shutdown kept reverbOwner, so the graph is made again here (C-SB60b: one test). From
	// Restart (a device loss at the top of Render, or s_restart between frames) this runs before the next world update
	// makes any voice. From Update's retry while there is no device it runs after that Render's world update, which made
	// no voice (the pool was empty), so the next Render's voices are the first. The master's rate is kept for the graph
	reverbSampleRate = outputSampleRate;
	if( reverbOwner != NULL )
	{
		if( AcquireReverbCarrier() )
		{
			reverbOwner->OnCarrierReacquired();
		}
		else
		{
			idLib::Warning( "reverb carrier: the graph could not be made again after the device restart; the map plays dry" );
			reverbOwner = NULL;
			reverbCarrierLive = false;
		}
	}
	//puncho end
}

/*
========================
idSoundHardware_XAudio2::Shutdown
========================
*/
void idSoundHardware_XAudio2::Shutdown()
{
	for( int i = 0; i < voices.Num(); i++ )
	{
		voices[ i ].DestroyInternal();
	}
	voices.Clear();
	freeVoices.Clear();
	zombieVoices.Clear();

#if defined(USE_DOOMCLASSIC)
	// ---------------------
	// Shutdown the Doom classic sound system.
	// ---------------------
	I_ShutdownSoundHardware();
#endif

	if( pXAudio2 != NULL )
	{
		// Unregister the sound engine callback
		pXAudio2->UnregisterForCallbacks( &soundEngineCallback );
	}

	//puncho - R-SB4-4, R-SB4-32: the reverb graph after the source voices, before the master (a device loss or restart
	// included). Every source voice was destroyed above, so none lists the bus. reverbOwner and the carrier flag stay
	// for the reacquire at the end of Init; the count is reset (D-47)
	DestroyReverbGraph();
	reverbBusVoices = 0;
	if( pMasterVoice != NULL )
	{
		// release the vu meter effect
		pMasterVoice->SetEffectChain( NULL );
		pMasterVoice->DestroyVoice();
		pMasterVoice = NULL;
	}
	if( pXAudio2 != NULL )
	{
		XAUDIO2_PERFORMANCE_DATA perfData;
		pXAudio2->GetPerformanceData( &perfData );
		//idLib::Printf( "Final pXAudio2 performanceData: Voices: %d/%d CPU: %.2f%% Mem: %dkb\n", perfData.ActiveSourceVoiceCount, perfData.TotalSourceVoiceCount, perfData.AudioCyclesSinceLastQuery / ( float )perfData.TotalCyclesSinceLastQuery, perfData.MemoryUsageInBytes / 1024 );
		pXAudio2->Release();
		pXAudio2 = NULL;
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
}

/*
========================
idSoundHardware_XAudio2::AllocateVoice
========================
*/
idSoundVoice* idSoundHardware_XAudio2::AllocateVoice( const idSoundSample* leadinSample, const idSoundSample* loopingSample )
{
	if( leadinSample == NULL )
	{
		return NULL;
	}
	if( loopingSample != NULL )
	{
		if( ( leadinSample->format.basic.formatTag != loopingSample->format.basic.formatTag ) || ( leadinSample->format.basic.numChannels != loopingSample->format.basic.numChannels ) )
		{
			idLib::Warning( "Leadin/looping format mismatch: %s & %s", leadinSample->GetName(), loopingSample->GetName() );
			loopingSample = NULL;
		}
	}

	//puncho - R-E6-15: prefer a created free voice of the same format (reused, nothing destroyed),
	// then a never-created one, then the last not-playing free one (destroy and create, as before).
	// Cheap tests first: a never-created voice cannot be playing, and IsPlaying (an XAudio2 GetState)
	// is paid only on a created voice whose format matches; only the last fallback pays it on the rest
	idSoundVoice_XAudio2* found = NULL;
	idSoundVoice_XAudio2* neverCreated = NULL;
	for( int i = 0; i < freeVoices.Num(); i++ )
	{
		idSoundVoice_XAudio2* candidate = freeVoices[i];
		if( candidate->pSourceVoice == NULL )
		{
			if( neverCreated == NULL )
			{
				neverCreated = candidate;
			}
			continue;
		}
		if( !idSoundVoice_XAudio2::FormatsMatch( candidate->voiceFormat, leadinSample->format ) || candidate->IsPlaying() )
		{
			continue;
		}
		found = candidate;
		break;
	}
	if( found == NULL )
	{
		found = neverCreated;
	}
	if( found == NULL )
	{
		// today's fallback: the last free voice that is not playing
		for( int i = 0; i < freeVoices.Num(); i++ )
		{
			if( !freeVoices[i]->IsPlaying() )
			{
				found = freeVoices[i];
			}
		}
	}
	idSoundVoice* voice = ( idSoundVoice* )found;
	if( voice != NULL )
	{
		voice->Create( leadinSample, loopingSample );
		freeVoices.Remove( voice );
		return voice;
	}

	return NULL;
}

/*
========================
idSoundHardware_XAudio2::FreeVoice
========================
*/
void idSoundHardware_XAudio2::FreeVoice( idSoundVoice* voice )
{
	voice->Stop();

	// Stop() is asyncronous, so we won't flush bufferes until the
	// voice on the zombie channel actually returns !IsPlaying()
	zombieVoices.Append( voice );
}

/*
========================
idSoundHardware_XAudio2::Update
========================
*/
void idSoundHardware_XAudio2::Update()
{
	if( pXAudio2 == NULL )
	{
		int nowTime = Sys_Milliseconds();
		if( lastResetTime + 1000 < nowTime )
		{
			lastResetTime = nowTime;
			Init();
		}
		return;
	}
	if( soundSystem->IsMuted() )
	{
		pMasterVoice->SetVolume( 0.0f, OPERATION_SET );
	}
	else
	{
		pMasterVoice->SetVolume( DBtoLinear( s_volume_dB.GetFloat() ), OPERATION_SET );
	}

	//puncho - R-SB4-6 (C-SB60): the owner loaded once. Its tick runs every Render, paused or draining, and its sets
	// (OPERATION_SET) land in the commit below
	idSoundReverb* const owner = reverbOwner;
	if( owner != NULL )
	{
		owner->Tick( Sys_Milliseconds() );
	}

	pXAudio2->CommitChanges( XAUDIO2_COMMIT_ALL );

	// IXAudio2SourceVoice::Stop() has been called for every sound on the
	// zombie list, but it is documented as asyncronous, so we have to wait
	// until it actually reports that it is no longer playing.
	for( int i = 0; i < zombieVoices.Num(); i++ )
	{
		zombieVoices[i]->FlushSourceBuffers();
		if( !zombieVoices[i]->IsPlaying() )
		{
			freeVoices.Append( zombieVoices[i] );
			zombieVoices.RemoveIndexFast( i );
			i--;
		}
		else
		{
			static int playingZombies;
			playingZombies++;
		}
	}

	//puncho - R-SB4-29 (C-SB60): while a reverb owns the carrier, a voice freed this frame is fixed before the next
	// frame's AllocateVoice, so the menu world never reuses a voice with a send
	if( owner != NULL )
	{
		UpdateReverbFreeVoices();
	}

	if( s_showPerfData.GetBool() )
	{
		//puncho - R-E6-30: a percent, at most once a second (the cycle counts then cover that second)
		static int lastPerfPrintTime = 0;
		const int nowTime = Sys_Milliseconds();
		if( nowTime - lastPerfPrintTime >= 1000 )
		{
			lastPerfPrintTime = nowTime;
			XAUDIO2_PERFORMANCE_DATA perfData;
			pXAudio2->GetPerformanceData( &perfData );
			const float cpuPercent = ( perfData.TotalCyclesSinceLastQuery != 0 ) ? 100.0f * ( float )perfData.AudioCyclesSinceLastQuery / ( float )perfData.TotalCyclesSinceLastQuery : 0.0f;
			//puncho - D-47: the submixes too, so A-SB4-1 and A-SB4-2 have a count to read
			idLib::Printf( "Voices: %d/%d Submixes: %d CPU: %.2f%% Mem: %dkb\n", perfData.ActiveSourceVoiceCount, perfData.TotalSourceVoiceCount, perfData.ActiveSubmixVoiceCount, cpuPercent, perfData.MemoryUsageInBytes / 1024 );
		}
	}

	if( vuMeterRMS == NULL )
	{
		// Init probably hasn't been called yet
		return;
	}

	vuMeterRMS->Enable( s_showLevelMeter.GetBool() );
	vuMeterPeak->Enable( s_showLevelMeter.GetBool() );

	if( !s_showLevelMeter.GetBool() )
	{
		pMasterVoice->DisableEffect( 0 );
		return;
	}
	else
	{
		pMasterVoice->EnableEffect( 0 );
	}

	float peakLevels[ 8 ];
	float rmsLevels[ 8 ];

	XAUDIO2FX_VOLUMEMETER_LEVELS levels;
	levels.ChannelCount = outputChannels;
	levels.pPeakLevels = peakLevels;
	levels.pRMSLevels = rmsLevels;

	if( levels.ChannelCount > 8 )
	{
		levels.ChannelCount = 8;
	}

	pMasterVoice->GetEffectParameters( 0, &levels, sizeof( levels ) );

	int currentTime = Sys_Milliseconds();
	for( int i = 0; i < outputChannels; i++ )
	{
		if( vuMeterPeakTimes[i] < currentTime )
		{
			vuMeterPeak->SetValue( i, vuMeterPeak->GetValue( i ) * 0.9f, colorRed );
		}
	}

	float width = 20.0f;
	float height = 200.0f;
	float left = 100.0f;
	float top = 100.0f;

	sscanf( s_meterPosition.GetString(), "%f %f %f %f", &left, &top, &width, &height );

	vuMeterRMS->SetPosition( left, top, width * levels.ChannelCount, height );
	vuMeterPeak->SetPosition( left, top, width * levels.ChannelCount, height );

	for( uint32 i = 0; i < levels.ChannelCount; i++ )
	{
		vuMeterRMS->SetValue( i, rmsLevels[ i ], idVec4( 0.5f, 1.0f, 0.0f, 1.00f ) );
		if( peakLevels[ i ] >= vuMeterPeak->GetValue( i ) )
		{
			vuMeterPeak->SetValue( i, peakLevels[ i ], colorRed );
			vuMeterPeakTimes[i] = currentTime + s_meterTopTime.GetInteger();
		}
	}
}

/*
================================================
//puncho start - WP-SB4: the reverb carrier (R-SB4-1 to R-SB4-7, R-SB4-24 to R-SB4-32)

Every function here runs on the main thread: inside SB3's load and free hooks, the world's destructor, Init, or
Update (R-SB4-28). XAudio2's thread runs only the DSP; OnBufferStart never touches a send.
================================================
*/

// P-SB5: how a reverb submix's stereo output reaches the master. Mode 0, the one built: one output channel takes L and
// R at 0.5; two or more take L on channel 0 and R on channel 1 (the front pair), the rest 0
static const int REVERB_SURROUND_MODE = 0;
static_assert( REVERB_SURROUND_MODE == 0, "only REVERB_SURROUND_MODE 0 is built (R-SB4-2)" );

// C6R-2: the cache's unknown bus level or return, far below every level XAudio2 takes (-XAUDIO2_MAX_VOLUME_LEVEL), so it
// compares unequal to every value a frame or a cut sends and is sent again
static const float REVERB_SENT_UNKNOWN = -1.0e30f;

/*
========================
ReverbParametersFromFrame

//puncho - R-SB4-16's first sentence, SEAMS section 3: param[] holds the 24 fields of XAUDIO2FX_REVERB_PARAMETERS in
the SDK's order, as floats. The integer fields are rounded here, so xaudio2fx.h stays out of snd_reverb.cpp
========================
*/
static void ReverbParametersFromFrame( const float* param, XAUDIO2FX_REVERB_PARAMETERS& out )
{
	out.WetDryMix = param[ 0 ];
	out.ReflectionsDelay = ( UINT32 )Max( 0, idMath::Ftoi( idMath::Rint( param[ 1 ] ) ) );
	out.ReverbDelay = idMath::Ftob( idMath::Rint( param[ 2 ] ) );
	out.RearDelay = idMath::Ftob( idMath::Rint( param[ 3 ] ) );
	out.SideDelay = idMath::Ftob( idMath::Rint( param[ 4 ] ) );
	out.PositionLeft = idMath::Ftob( idMath::Rint( param[ 5 ] ) );
	out.PositionRight = idMath::Ftob( idMath::Rint( param[ 6 ] ) );
	out.PositionMatrixLeft = idMath::Ftob( idMath::Rint( param[ 7 ] ) );
	out.PositionMatrixRight = idMath::Ftob( idMath::Rint( param[ 8 ] ) );
	out.EarlyDiffusion = idMath::Ftob( idMath::Rint( param[ 9 ] ) );
	out.LateDiffusion = idMath::Ftob( idMath::Rint( param[ 10 ] ) );
	out.LowEQGain = idMath::Ftob( idMath::Rint( param[ 11 ] ) );
	out.LowEQCutoff = idMath::Ftob( idMath::Rint( param[ 12 ] ) );
	out.HighEQGain = idMath::Ftob( idMath::Rint( param[ 13 ] ) );
	out.HighEQCutoff = idMath::Ftob( idMath::Rint( param[ 14 ] ) );
	out.RoomFilterFreq = param[ 15 ];
	out.RoomFilterMain = param[ 16 ];
	out.RoomFilterHF = param[ 17 ];
	out.ReflectionsGain = param[ 18 ];
	out.ReverbGain = param[ 19 ];
	out.DecayTime = param[ 20 ];
	out.Density = param[ 21 ];
	out.RoomSize = param[ 22 ];
	out.DisableLateField = ( idMath::Ftoi( idMath::Rint( param[ 23 ] ) ) != 0 ) ? TRUE : FALSE;
}

/*
========================
ReverbRatioMoved
========================
*/
static bool ReverbRatioMoved( float sent, float next )
{
	return idMath::Fabs( next - sent ) > 0.005f * idMath::Fabs( sent );
}

/*
========================
ReverbParametersMoved

//puncho - R-SB4-7's last two sentences: an integer field that moved at all, a dB field by more than 0.01 dB, any
other float field by more than 0.5 % of the value last sent
========================
*/
static bool ReverbParametersMoved( const XAUDIO2FX_REVERB_PARAMETERS& sent, const XAUDIO2FX_REVERB_PARAMETERS& next )
{
	// ReflectionsDelay to HighEQCutoff are one run of integer fields, packed (xaudio2fx.h's pack 1)
	const size_t integerBytes = offsetof( XAUDIO2FX_REVERB_PARAMETERS, RoomFilterFreq ) - offsetof( XAUDIO2FX_REVERB_PARAMETERS, ReflectionsDelay );
	if( memcmp( &sent.ReflectionsDelay, &next.ReflectionsDelay, integerBytes ) != 0 || sent.DisableLateField != next.DisableLateField )
	{
		return true;
	}
	if( idMath::Fabs( next.RoomFilterMain - sent.RoomFilterMain ) > 0.01f || idMath::Fabs( next.RoomFilterHF - sent.RoomFilterHF ) > 0.01f
			|| idMath::Fabs( next.ReflectionsGain - sent.ReflectionsGain ) > 0.01f || idMath::Fabs( next.ReverbGain - sent.ReverbGain ) > 0.01f )
	{
		return true;
	}
	return ReverbRatioMoved( sent.WetDryMix, next.WetDryMix ) || ReverbRatioMoved( sent.RoomFilterFreq, next.RoomFilterFreq )
		   || ReverbRatioMoved( sent.DecayTime, next.DecayTime ) || ReverbRatioMoved( sent.Density, next.Density )
		   || ReverbRatioMoved( sent.RoomSize, next.RoomSize );
}

/*
========================
idSoundHardware_XAudio2::ResetReverbSent

//puncho - what a submix holds when it is made: effect disabled (InitialState FALSE), no parameter block sent yet (the
first frame sends one), and the bus level and return unknown (XAudio2's default matrices) until the initial zero sets
record what XAudio2 took (C6R-2)
========================
*/
void idSoundHardware_XAudio2::ResetReverbSent( int submix )
{
	reverbSentEnabled[ submix ] = 0;
	reverbSentParamsValid[ submix ] = false;
	reverbSentBus[ submix ] = REVERB_SENT_UNKNOWN;
	reverbSentReturn[ submix ] = REVERB_SENT_UNKNOWN;
}

/*
========================
idSoundHardware_XAudio2::SetReverbReturn

//puncho - D-31, DR-119: the return is Mout x gain on the submix's output matrix to the master, never its SetVolume,
which acts before the effect chain and stays 1. The cache takes the gain, or unknown when XAudio2 refused it (C6R-2)
========================
*/
void idSoundHardware_XAudio2::SetReverbReturn( int submix, float gain, uint32 operationSet )
{
	// XAudio2's matrix: source channel c to destination channel d at [ d * 2 + c ]
	float levels[ 2 * XAUDIO2_MAX_AUDIO_CHANNELS ];
	const int numOut = idMath::ClampInt( 1, XAUDIO2_MAX_AUDIO_CHANNELS, outputChannels );
	memset( levels, 0, 2 * numOut * sizeof( float ) );
	if( numOut == 1 )
	{
		levels[ 0 ] = 0.5f * gain;		// L to the one channel
		levels[ 1 ] = 0.5f * gain;		// R to the one channel
	}
	else
	{
		levels[ 0 ] = gain;				// L to channel 0
		levels[ 3 ] = gain;				// R to channel 1
	}
	const HRESULT result = pReverbVoices[ submix ]->SetOutputMatrix( pMasterVoice, 2, numOut, levels, operationSet );
	reverbSentReturn[ submix ] = SUCCEEDED( result ) ? gain : REVERB_SENT_UNKNOWN;
}

/*
========================
idSoundHardware_XAudio2::SetReverbBusLevel

//puncho - the bus to one submix, L to L and R to R. The cache takes the level, or unknown when refused (C6R-2)
========================
*/
void idSoundHardware_XAudio2::SetReverbBusLevel( int submix, float level, uint32 operationSet )
{
	float levels[ 4 ] = { level, 0.0f, 0.0f, level };	// L to L, R to R
	const HRESULT result = pReverbBus->SetOutputMatrix( pReverbVoices[ submix ], 2, 2, levels, operationSet );
	reverbSentBus[ submix ] = SUCCEEDED( result ) ? level : REVERB_SENT_UNKNOWN;
}

/*
========================
idSoundHardware_XAudio2::SetReverbEnabled

//puncho - one submix's effect on or off. The cache takes 1 or 0, or -1 (unknown) when refused (C6R-2)
========================
*/
void idSoundHardware_XAudio2::SetReverbEnabled( int submix, bool enable, uint32 operationSet )
{
	const HRESULT result = enable ? pReverbVoices[ submix ]->EnableEffect( 0, operationSet ) : pReverbVoices[ submix ]->DisableEffect( 0, operationSet );
	reverbSentEnabled[ submix ] = SUCCEEDED( result ) ? ( enable ? 1 : 0 ) : -1;
}

/*
========================
idSoundHardware_XAudio2::SilenceReverbGraph

//puncho - R-SB4-26 (a), and a failed remake (DR-187): both returns and bus levels to 0 and both effects disabled, at
once (XAUDIO2_COMMIT_NOW). A call is made only where the cache does not say 0 or off; an unknown entry is sent again
(C6R-2). After a failed attach the cache holds bus level 0 for the pair the bus does not feed, so that pair gets no
bus call. An effect that was on, or may have been (unknown), may hold a tail (M-SB0-18): disabling it marks A and B
to be made again at the next begin; a mark set before stays. The caller has XAudio2 and the graph
========================
*/
void idSoundHardware_XAudio2::SilenceReverbGraph()
{
	for( int i = 0; i < 2; i++ )
	{
		if( reverbSentReturn[ i ] != 0.0f )
		{
			SetReverbReturn( i, 0.0f, XAUDIO2_COMMIT_NOW );
		}
		if( reverbSentBus[ i ] != 0.0f )
		{
			SetReverbBusLevel( i, 0.0f, XAUDIO2_COMMIT_NOW );
		}
		if( reverbSentEnabled[ i ] != 0 )
		{
			SetReverbEnabled( i, false, XAUDIO2_COMMIT_NOW );
			reverbDisabledMidTail = true;
		}
	}
}

/*
========================
idSoundHardware_XAudio2::CreateReverbSubmix

//puncho - R-SB4-2: one reverb submix (A or B): 2 channels in, the master's rate, stage 1, sending to the master,
the reverb effect disabled; its SetVolume stays 1. The caller sets its return Mout x 0 once it has its index (C6R-2)
========================
*/
bool idSoundHardware_XAudio2::CreateReverbSubmix( IXAudio2SubmixVoice** submix )
{
	*submix = NULL;
	IUnknown* reverb = NULL;
	if( FAILED( XAudio2CreateReverb( &reverb, 0 ) ) || reverb == NULL )
	{
		return false;
	}

	XAUDIO2_EFFECT_DESCRIPTOR descriptor;
	descriptor.InitialState = FALSE;
	descriptor.OutputChannels = 2;
	descriptor.pEffect = reverb;

	XAUDIO2_EFFECT_CHAIN chain;
	chain.EffectCount = 1;
	chain.pEffectDescriptors = &descriptor;

	const HRESULT result = pXAudio2->CreateSubmixVoice( submix, 2, reverbSampleRate, 0, 1, NULL, &chain );
	reverb->Release();		// the voice holds its own reference, as the master's meter does
	if( FAILED( result ) )
	{
		*submix = NULL;
		return false;
	}
	return true;
}

/*
========================
idSoundHardware_XAudio2::DestroyReverbGraph

//puncho - R-SB4-4: the one destroy of the graph, run only by Shutdown (a device loss or restart included) and by the
owning world's DetachReverb. Each first leaves no source voice sending to the bus: Shutdown destroys every source
voice, DetachReverb relists each that lists the bus or is pending and destroys one it cannot relist (C6-1). The bus
goes first, so A and B have no sender left
========================
*/
void idSoundHardware_XAudio2::DestroyReverbGraph()
{
	if( pReverbBus != NULL )
	{
		pReverbBus->DestroyVoice();
		pReverbBus = NULL;
	}
	reverbBusAttached = false;
	for( int i = 0; i < 2; i++ )
	{
		if( pReverbVoices[ i ] != NULL )
		{
			pReverbVoices[ i ]->DestroyVoice();
			pReverbVoices[ i ] = NULL;
		}
	}
}

/*
========================
idSoundHardware_XAudio2::AcquireReverbCarrier

//puncho - R-SB4-1, R-SB4-2, R-SB4-25: SB3's step 11, and the reacquire at the end of Init. The graph is made when
XAudio2 is up and there is none: A, B, then the bus that sends to them, with the initial sets (OPERATION_SET); a
graph that exists is reused. A failure destroys what it made: no submix, no voice's list touched (section 9). It runs
after the load's cover has ended, so it raises no Error (D-40, D-43)
========================
*/
bool idSoundHardware_XAudio2::AcquireReverbCarrier()
{
	if( pXAudio2 == NULL || pMasterVoice == NULL )
	{
		return false;
	}
	if( pReverbBus != NULL )
	{
		return true;
	}

	IXAudio2SubmixVoice* made[ 2 ] = { NULL, NULL };
	if( CreateReverbSubmix( &made[ 0 ] ) && CreateReverbSubmix( &made[ 1 ] ) )
	{
		XAUDIO2_SEND_DESCRIPTOR sends[ 2 ] = { { 0, made[ 0 ] }, { 0, made[ 1 ] } };
		XAUDIO2_VOICE_SENDS sendList = { 2, sends };
		if( SUCCEEDED( pXAudio2->CreateSubmixVoice( &pReverbBus, 2, reverbSampleRate, 0, 0, &sendList, NULL ) ) && pReverbBus != NULL )
		{
			for( int i = 0; i < 2; i++ )
			{
				pReverbVoices[ i ] = made[ i ];
				ResetReverbSent( i );
				SetReverbReturn( i, 0.0f, OPERATION_SET );
				SetReverbBusLevel( i, 0.0f, OPERATION_SET );
			}
			reverbBusAttached = true;
			reverbDisabledMidTail = false;	// two new effects: no tail
			return true;
		}
		pReverbBus = NULL;
	}
	for( int i = 0; i < 2; i++ )
	{
		if( made[ i ] != NULL )
		{
			made[ i ]->DestroyVoice();
		}
	}
	return false;
}

/*
========================
idSoundHardware_XAudio2::RemakeReverbSubmixes

//puncho - R-SB4-5 step 2 (DR-80): an effect disabled while its tail rang keeps that tail (M-SB0-18), so A and B are
made again inside the begin hook. The new pair is created first (DR-181 (1)), then R-SB4-5's calls in its order: the bus
to no submix, the old A and B destroyed, the bus to the new pair, its matrices 0. False when it failed (C6-1, C6-2):
- a new submix could not be made, or the bus's list could not be emptied: only the new submixes, which nothing sends
  to, are destroyed; the old A and B stay, fed by the bus unless an earlier failed attach left its list empty;
- the bus could not be attached to the new pair: the old pair is gone already; the bus keeps an empty list and the new
  pair, which nothing sends to, waits for the next begin; with no route its bus levels are recorded 0.
Either way the graph is whole (three voices, none destroyed while sent to) and the mark stays set
========================
*/
bool idSoundHardware_XAudio2::RemakeReverbSubmixes()
{
	IXAudio2SubmixVoice* made[ 2 ] = { NULL, NULL };
	XAUDIO2_VOICE_SENDS noSends = { 0, NULL };
	if( !CreateReverbSubmix( &made[ 0 ] ) || !CreateReverbSubmix( &made[ 1 ] ) || FAILED( pReverbBus->SetOutputVoices( &noSends ) ) )
	{
		for( int i = 0; i < 2; i++ )
		{
			if( made[ i ] != NULL )
			{
				made[ i ]->DestroyVoice();
			}
		}
		return false;
	}
	reverbBusAttached = false;		// the bus's list is empty now

	for( int i = 0; i < 2; i++ )
	{
		pReverbVoices[ i ]->DestroyVoice();
		pReverbVoices[ i ] = made[ i ];
		ResetReverbSent( i );
		SetReverbReturn( i, 0.0f, OPERATION_SET );
	}

	// SetOutputVoices gives each new destination XAudio2's default matrix, not 0, so both are cut at once (D-30)
	XAUDIO2_SEND_DESCRIPTOR sends[ 2 ] = { { 0, pReverbVoices[ 0 ] }, { 0, pReverbVoices[ 1 ] } };
	XAUDIO2_VOICE_SENDS sendList = { 2, sends };
	if( FAILED( pReverbBus->SetOutputVoices( &sendList ) ) )
	{
		// no route from the bus: nothing passes, and a bus call toward a voice it does not feed would be refused
		reverbSentBus[ 0 ] = 0.0f;
		reverbSentBus[ 1 ] = 0.0f;
		return false;
	}
	reverbBusAttached = true;
	SetReverbBusLevel( 0, 0.0f, XAUDIO2_COMMIT_NOW );
	SetReverbBusLevel( 1, 0.0f, XAUDIO2_COMMIT_NOW );
	reverbDisabledMidTail = false;
	return true;
}

/*
========================
idSoundHardware_XAudio2::FlushZombieVoices

//puncho - R-SB4-5: FreeVoice only queues the Stop, which takes effect at the next commit, so the commit comes first.
A zombie that no longer plays goes free; one that still plays stays a zombie and is held. The one routine of the
begin hook, the end hook and the release; Update keeps its own loop.
A failed commit may leave deferred sets of A, B and the bus not taken, so the cache can no longer say what they hold:
every entry becomes unknown, as a refused setter's does, and the cut or frame that follows sends it all again
(C6S-1). A bus level recorded 0 because the bus has no route to that submix (a failed attach, reverbBusAttached
false) stays 0, so no call goes toward a submix the bus does not feed
========================
*/
void idSoundHardware_XAudio2::FlushZombieVoices()
{
	if( pXAudio2 != NULL && FAILED( pXAudio2->CommitChanges( XAUDIO2_COMMIT_ALL ) ) )
	{
		for( int i = 0; i < 2; i++ )
		{
			reverbSentEnabled[ i ] = -1;
			reverbSentParamsValid[ i ] = false;
			reverbSentReturn[ i ] = REVERB_SENT_UNKNOWN;
			if( reverbBusAttached )
			{
				reverbSentBus[ i ] = REVERB_SENT_UNKNOWN;
			}
		}
	}
	for( int i = 0; i < zombieVoices.Num(); i++ )
	{
		zombieVoices[i]->FlushSourceBuffers();
		if( !zombieVoices[i]->IsPlaying() )
		{
			freeVoices.Append( zombieVoices[i] );
			zombieVoices.RemoveIndexFast( i );
			i--;
		}
	}
}

/*
========================
idSoundHardware_XAudio2::BeginReverbMap

//puncho - R-SB4-5, the begin hook's hardware part, at SB3's step 11 on the load screen, after AcquireReverbCarrier
returned true. Step 2's draining cancel is SB4-Z's; open[] and the blend table are SB4-L's. Raises no Error
========================
*/
void idSoundHardware_XAudio2::BeginReverbMap( idSoundReverb* owner )
{
	// step 1: commit, then flush the zombies (at most 96)
	FlushZombieVoices();

	// step 2: A and B made again when the graph is reused and an effect was disabled mid-tail. If that fails the map
	// plays dry (C6-2): whatever pair the bus still feeds is silenced as end hook (a) does, since a carried drain's frame
	// may have left an effect on with a return (DR-187); then no owner, so no tick enables an effect with an old tail in
	// it; the carrier stays off and no voice's list is touched; the mark stays set, so the next begin tries again. The
	// end hook still runs with no owner
	if( reverbDisabledMidTail && pXAudio2 != NULL && pReverbBus != NULL && !RemakeReverbSubmixes() )
	{
		idLib::Warning( "reverb carrier: submixes A and B could not be made again; the map plays dry" );
		SilenceReverbGraph();
		reverbOwner = NULL;
		reverbCarrierLive = false;
		return;
	}

	// the carrier is live only with a bus to list, which AcquireReverbCarrier has made before this hook
	reverbOwner = owner;
	reverbCarrierLive = ( pReverbBus != NULL );

	// step 4 first: every created voice is marked held. Step 3: the free ones, which no channel holds and which no
	// longer play, go on {master, bus} with a bus matrix of 0, which clears the mark. A held voice (a carried SSF_GLOBAL
	// voice or a menu voice: neither sends) keeps its list while it plays; the free-voice walk lists it once free
	for( int i = 0; i < voices.Num(); i++ )
	{
		if( voices[i].pSourceVoice != NULL )
		{
			voices[i].SetReverbFlags( voices[i].listHasBus, true );
		}
	}
	for( int i = 0; i < freeVoices.Num(); i++ )
	{
		if( freeVoices[i]->pSourceVoice != NULL )
		{
			freeVoices[i]->SetReverbList( reverbCarrierLive );
		}
	}
}

/*
========================
idSoundHardware_XAudio2::EndReverbMap

//puncho - R-SB4-26, the end hook's hardware part, at map shutdown (and on a live s_acoustics 0, D-48). SB4-L's hook
decides soundCarried; the submix states are SB4-Z's. Nothing is destroyed (DR-122)
========================
*/
void idSoundHardware_XAudio2::EndReverbMap( bool soundCarried )
{
	reverbCarrierLive = false;

	FlushZombieVoices();

	// a voice that lists the bus is marked held first, and a mark on one that does not is dropped (its list is
	// {master} already). The free ones then go back to {master}, which clears the mark; a held one keeps its list
	// while it plays, never a SetOutputVoices on a playing voice (S1B-9)
	for( int i = 0; i < voices.Num(); i++ )
	{
		voices[i].SetReverbFlags( voices[i].listHasBus, voices[i].listHasBus );
	}
	for( int i = 0; i < freeVoices.Num(); i++ )
	{
		if( freeVoices[i]->listHasBus || freeVoices[i]->listPending )
		{
			freeVoices[i]->SetReverbList( false );
		}
	}

	if( soundCarried )
	{
		// (b): the reverb drains; the tick and the walk run on while it owns the carrier, and the graph is kept
		return;
	}

	// (a): no tick runs before the next map's first frame, so the returns and the bus levels are cut to 0 and both
	// effects disabled at once, as the dry voices are cut. The graph is kept, idle. The cache holds what XAudio2 took
	// once the flush above has committed the deferred sets, or unknown where it refused a call; when that commit failed,
	// the flush made every entry unknown (but a no-route bus level), and unknown is sent again (C6R-2, C6S-1);
	// a call is made only where it does not say 0 or off. Only an effect that was on, or may have been, can hold a
	// tail (M-SB0-18), so only disabling one marks A and B to be made again at the next begin (C6, question 11)
	if( pXAudio2 != NULL && pReverbBus != NULL )
	{
		SilenceReverbGraph();
	}
	reverbOwner = NULL;
}

/*
========================
idSoundHardware_XAudio2::DetachReverb

//puncho - R-SB4-31: the world's destructor, through SB4-L's Detach. Only the owning world's detach acts: every voice
goes back to {master}, a held one too (the one SetOutputVoices on a playing voice, D-47: its master matrix is reissued
at once, a one-sample step in a world being deleted), then the graph is destroyed at once
========================
*/
void idSoundHardware_XAudio2::DetachReverb( const idSoundReverb* owner )
{
	if( reverbOwner != owner )
	{
		return;
	}
	reverbOwner = NULL;
	reverbCarrierLive = false;
	for( int i = 0; i < voices.Num(); i++ )
	{
		// C6-1: a voice whose list cannot be reset may still send to the bus, and DestroyVoice on a voice that is still
		// sent to is invalid, so that voice's source voice goes first, with its resets. The pooled object stays: a
		// channel that holds it (this world's, freed right after this detach, or another world's) gets silence and
		// no fault, because the calls that reach XAudio2 test pSourceVoice, DestroyInternal clears the meter state
		// (GetAmplitude reads no effect) and the reverb flags (SetReverbSend returns at once), and DestroyVoice has
		// finished the voice's callbacks before it returns; its next sound makes the voice again. The limit, only
		// on a failed SetOutputVoices: a looping sound of another world whose voice this destroyed stays silent until
		// that channel is retriggered or reallocated
		if( ( voices[i].listHasBus || voices[i].listPending ) && !voices[i].SetReverbList( false ) )
		{
			voices[i].DestroyInternal();
		}
	}
	DestroyReverbGraph();
}

/*
========================
idSoundHardware_XAudio2::ResetIdleReverbLists

//puncho - R-SB4-25: ReleaseIdleReverbGraph's work at an unbaked load while a voice lists the bus or is pending (a
drain's leftovers, or a held voice that still lists the bus): commit and flush as R-SB4-5, then every voice not held
back to {master}. Held voices keep theirs to the next load. Nothing is destroyed
========================
*/
void idSoundHardware_XAudio2::ResetIdleReverbLists()
{
	FlushZombieVoices();
	for( int i = 0; i < freeVoices.Num(); i++ )
	{
		if( freeVoices[i]->listHasBus || freeVoices[i]->listPending )
		{
			freeVoices[i]->SetReverbList( false );
		}
	}
}

/*
========================
idSoundHardware_XAudio2::UpdateReverbFreeVoices

//puncho - R-SB4-29: after Update's zombie loop, while a reverb owns the carrier. A free voice marked pending gets
step 3 while the carrier is live, {master} while it drains; one with a send has it cut to 0. At most 96 compares
========================
*/
void idSoundHardware_XAudio2::UpdateReverbFreeVoices()
{
	for( int i = 0; i < freeVoices.Num(); i++ )
	{
		idSoundVoice_XAudio2* voice = freeVoices[i];
		if( voice->listPending )
		{
			voice->SetReverbList( reverbCarrierLive );
		}
		else if( voice->GetReverbSend() != 0.0f )
		{
			voice->ClearReverbSend();
		}
	}
}

/*
========================
idSoundHardware_XAudio2::SubmitReverbFrame

//puncho - R-SB4-7's last two sentences, R-SB4-16's first, SEAMS section 3: SB4-Z's tick hands its frame here, before
Update's commit. Every call is OPERATION_SET, and a value goes to XAudio2 only when it moved: the bus level or the
return by more than 1e-4, any field of the parameter block (which then goes whole) as ReverbParametersMoved says. The
delay fields go as given: SB4-Z decides when they move. With no XAudio2 or no graph only the frame's two flags apply
(D-47); they apply last.
Precondition, for SB4-Z (C6S): called only through the owner's Tick from Update. It checks that the bus exists, not
that the bus is attached to A and B; that holds because a failed remake (the one way the bus is left unattached)
leaves no owner, so no Tick and no call here until a begin hook succeeds, and a successful one attaches the bus
========================
*/
void idSoundHardware_XAudio2::SubmitReverbFrame( const reverbCarrierFrame_t& frame )
{
	static_assert( sizeof( XAUDIO2FX_REVERB_PARAMETERS ) == 57, "XAUDIO2FX_REVERB_PARAMETERS: 24 fields, SideDelay included (R-SB4-16)" );
	static_assert( REVERB_LOG_PARAMS == 24, "param[] holds the 24 fields of XAUDIO2FX_REVERB_PARAMETERS (SEAMS section 3)" );

	if( pXAudio2 != NULL && pReverbBus != NULL )
	{
		for( int i = 0; i < 2; i++ )
		{
			const reverbCarrierSubmix_t& in = frame.submix[ i ];
			IXAudio2SubmixVoice* submix = pReverbVoices[ i ];

			XAUDIO2FX_REVERB_PARAMETERS params;
			ReverbParametersFromFrame( in.param, params );
			if( !reverbSentParamsValid[ i ] || ReverbParametersMoved( reverbSentParams[ i ], params ) )
			{
				// C6R-2: a refused block leaves the cache invalid, so the next frame sends it again
				reverbSentParamsValid[ i ] = SUCCEEDED( submix->SetEffectParameters( 0, &params, sizeof( params ), OPERATION_SET ) );
				reverbSentParams[ i ] = params;
			}

			// the setters below record what XAudio2 took, or unknown, which differs from every value asked (C6R-2)
			const int enable = ( in.enabled != 0 ) ? 1 : 0;
			if( enable != reverbSentEnabled[ i ] )
			{
				SetReverbEnabled( i, enable != 0, OPERATION_SET );
			}

			if( idMath::Fabs( in.busLevel - reverbSentBus[ i ] ) > 1e-4f )
			{
				SetReverbBusLevel( i, in.busLevel, OPERATION_SET );
			}

			if( idMath::Fabs( in.returnGain - reverbSentReturn[ i ] ) > 1e-4f )
			{
				SetReverbReturn( i, in.returnGain, OPERATION_SET );
			}
		}
	}

	if( frame.disabledMidTail )
	{
		reverbDisabledMidTail = true;
	}
	if( frame.releaseOwner )
	{
		reverbOwner = NULL;
	}
}

/*
========================
idSoundHardware_XAudio2::PrintReverbVoices

//puncho - R-SB3-17: for SB3's listAcoustics, each pooled voice's listHasBus and reverbSend, menu voices included
========================
*/
void idSoundHardware_XAudio2::PrintReverbVoices() const
{
	idLib::Printf( "reverb carrier: %s, owner %s, graph %s, %d voices list the bus or are pending\n", reverbCarrierLive ? "live" : "not live",
				   reverbOwner != NULL ? "set" : "none", pReverbBus != NULL ? "made" : "none", reverbBusVoices );
	for( int i = 0; i < voices.Num(); i++ )
	{
		const idSoundVoice_XAudio2& voice = voices[i];
		idLib::Printf( "%3d: %s listHasBus %d pending %d reverbSend %.3f\n", i, voice.pSourceVoice != NULL ? "created" : "unused ",
					   voice.listHasBus ? 1 : 0, voice.listPending ? 1 : 0, voice.GetReverbSend() );
	}
}
//puncho end


/*
================================================
idSoundEngineCallback
================================================
*/

/*
========================
idSoundEngineCallback::OnCriticalError
========================
*/
void idSoundEngineCallback::OnCriticalError( HRESULT Error )
{
	soundSystemLocal.SetNeedsRestart();
}
