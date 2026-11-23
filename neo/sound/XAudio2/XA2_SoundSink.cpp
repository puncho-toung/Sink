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
//puncho - C3e-C: device selection copied from today's backend; one pull per voice pass.
#pragma hdrstop
#include "precompiled.h"
#include "../snd_local.h"
#include <ks.h>
#include <ksmedia.h>
#if defined( USE_DOOMCLASSIC )
#include "../../../doomclassic/doom/i_sound.h"
#endif

idCVar s_device( "s_device", "-1", CVAR_INTEGER | CVAR_ARCHIVE, "Which audio device to use (listDevices to list, -1 for default)" );
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

void idSoundSink_XAudio2::ListDevices( const idCmdArgs& args )
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
		idLib::Printf( "%s %3d: %S %S\n", vAudioDevices[i].id == defaultDevice.id ? "*" : " ", ( int )i, vAudioDevices[i].name.c_str(), vAudioDevices[i].id.c_str() );
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

idSoundSink_XAudio2::idSoundSink_XAudio2() : lost( false )
{
	owner = NULL;
	pXAudio2 = NULL;
	pMasterVoice = NULL;
	pSourceVoice = NULL;
	buffers = NULL;
	nextBuffer = outputChannels = channelMask = 0;
	classicInitialized = false;
}

bool idSoundSink_XAudio2::Open( idSoundHardware_Mixer* owner_ )
{
	assert( pXAudio2 == NULL );
	owner = owner_;
	lost.store( false, std::memory_order_seq_cst );
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
				idLib::Warning( "Failed to create XAudio2 engine.  Try installing the latest DirectX." );
				return false;
			}
		}
		else
#endif
// RB end
		{
			idLib::Warning( "Failed to create XAudio2 engine.  Try installing the latest DirectX." );
			return false;
		}
	}
#ifdef _DEBUG
	XAUDIO2_DEBUG_CONFIGURATION debugConfiguration = { 0 };
	debugConfiguration.TraceMask = XAUDIO2_LOG_WARNINGS;
	debugConfiguration.BreakMask = XAUDIO2_LOG_ERRORS;
	pXAudio2->SetDebugConfiguration( &debugConfiguration );
#endif

	// Register the sound engine callback
	pXAudio2->RegisterForCallbacks( this );
	DWORD outputSampleRate = SND_MIX_SAMPLE_RATE; //puncho - R-E6-39: the voices' filter rate is this rate (snd_local.h) // Max( (DWORD)XAUDIO2FX_REVERB_MIN_FRAMERATE, Min( (DWORD)XAUDIO2FX_REVERB_MAX_FRAMERATE, deviceDetails.OutputFormat.Format.nSamplesPerSec ) );

	idCmdArgs args;
	ListDevices( args );

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
			if( outputChannels > SND_MIX_MAX_OUT )
			{
				pMasterVoice->DestroyVoice();
				pMasterVoice = NULL;
				if( FAILED( pXAudio2->CreateMasteringVoice( &pMasterVoice, SND_MIX_MAX_OUT, outputSampleRate, 0,
					selectedDevice.id.c_str(), NULL, AudioCategory_GameEffects ) ) )
				{
					idLib::Warning( "Failed to create 8-channel master voice" );
					return false;
				}
				outputChannels = SND_MIX_MAX_OUT;
				channelMask = SPEAKER_7POINT1_SURROUND;
				pMasterVoice->SetVolume( DBtoLinear( s_volume_dB.GetFloat() ) );
			}
			idLib::Printf( "Using device: %S\n", selectedDevice.name.c_str() );
		}
		else
		{
			idLib::Warning( "Failed to create master voice" );
			return false;
		}
	}

#else
	UINT32 deviceCount = 0;
	if( pXAudio2->GetDeviceCount( &deviceCount ) != S_OK || deviceCount == 0 )
	{
		idLib::Warning( "No audio devices found" );
		return false;
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
		return false;
	}


	if( FAILED( pXAudio2->CreateMasteringVoice( &pMasterVoice, XAUDIO2_DEFAULT_CHANNELS, outputSampleRate, 0, preferredDevice, NULL ) ) )
	{
		idLib::Warning( "Failed to create master voice" );
		return false;
	}
	pMasterVoice->SetVolume( DBtoLinear( s_volume_dB.GetFloat() ) );

	outputChannels = deviceDetails.OutputFormat.Format.nChannels;
	channelMask = deviceDetails.OutputFormat.dwChannelMask;
	if( outputChannels > SND_MIX_MAX_OUT )
	{
		pMasterVoice->DestroyVoice();
		pMasterVoice = NULL;
		if( FAILED( pXAudio2->CreateMasteringVoice( &pMasterVoice, SND_MIX_MAX_OUT, outputSampleRate, 0, preferredDevice, NULL ) ) )
		{
			idLib::Warning( "Failed to create 8-channel master voice" );
			return false;
		}
		outputChannels = SND_MIX_MAX_OUT;
		channelMask = SPEAKER_7POINT1_SURROUND;
		pMasterVoice->SetVolume( DBtoLinear( s_volume_dB.GetFloat() ) );
	}

#endif // #if (_WIN32_WINNT < 0x0602 /*_WIN32_WINNT_WIN8*/)


	if( pMasterVoice == NULL || outputChannels < 1 || outputChannels > SND_MIX_MAX_OUT )
	{
		idLib::Warning( "sound mixer: no supported output device (1 to 8 channels required)" );
		return false;
	}
#if defined( USE_DOOMCLASSIC )
	I_InitSoundHardware( outputChannels, channelMask );
	classicInitialized = true;
#endif
	buffers = ( float* )Mem_Alloc16( 4 * SND_MIX_BLOCK * outputChannels * sizeof( float ), TAG_AUDIO );
	if( buffers == NULL )
	{
		idLib::Warning( "sound mixer: could not allocate stream buffers" );
		return false;
	}
	nextBuffer = 0;
	WAVEFORMATEXTENSIBLE format = {};
	format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	format.Format.nChannels = ( WORD )outputChannels;
	format.Format.nSamplesPerSec = SND_MIX_SAMPLE_RATE;
	format.Format.wBitsPerSample = 32;
	format.Format.nBlockAlign = ( WORD )( outputChannels * sizeof( float ) );
	format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
	format.Format.cbSize = sizeof( WAVEFORMATEXTENSIBLE ) - sizeof( WAVEFORMATEX );
	format.Samples.wValidBitsPerSample = 32;
	format.dwChannelMask = channelMask;
	format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
	XAUDIO2_SEND_DESCRIPTOR send = { 0, pMasterVoice };
	XAUDIO2_VOICE_SENDS sends = { 1, &send };
	if( FAILED( pXAudio2->CreateSourceVoice( &pSourceVoice, &format.Format, 0, 1.0f, this, &sends ) ) )
	{
		idLib::Warning( "sound mixer: could not create stream voice" );
		return false;
	}
	float matrix[SND_MIX_MAX_OUT * SND_MIX_MAX_OUT] = {};
	for( int i = 0; i < outputChannels; i++ )
	{
		matrix[i * outputChannels + i] = 1.0f;
	}
	if( FAILED( pSourceVoice->SetOutputMatrix( pMasterVoice, outputChannels, outputChannels, matrix ) ) ||
		FAILED( pSourceVoice->Start( 0 ) ) )
	{
		idLib::Warning( "sound mixer: could not route or start stream voice" );
		return false;
	}
	return true;
}

void idSoundSink_XAudio2::Close()
{
	// The owner's gate has joined the mixer. DestroyVoice joins this voice's callbacks
	// before the ring or its dimensions can change, including zero-only pulls.
	if( pSourceVoice != NULL )
	{
		pSourceVoice->DestroyVoice();
		pSourceVoice = NULL;
	}
#if defined( USE_DOOMCLASSIC )
	if( classicInitialized )
	{
		I_ShutdownSoundHardware();
		classicInitialized = false;
	}
#endif
	if( pXAudio2 != NULL )
	{
		pXAudio2->UnregisterForCallbacks( this );
	}
	if( pMasterVoice != NULL )
	{
		pMasterVoice->DestroyVoice();
		pMasterVoice = NULL;
	}
	if( pXAudio2 != NULL )
	{
		pXAudio2->Release();
		pXAudio2 = NULL;
	}
	if( buffers != NULL )
	{
		Mem_Free16( buffers );
		buffers = NULL;
	}
	owner = NULL;
	outputChannels = channelMask = nextBuffer = 0;
}

bool idSoundSink_XAudio2::Lost()
{
	return lost.exchange( false, std::memory_order_seq_cst );
}

void idSoundSink_XAudio2::SetMasterVolume( float volume )
{
	if( pMasterVoice != NULL )
	{
		pMasterVoice->SetVolume( volume );
	}
}

int idSoundSink_XAudio2::LatencyFrames() const
{
	if( pXAudio2 == NULL )
	{
		return 0;
	}
	XAUDIO2_PERFORMANCE_DATA data;
	pXAudio2->GetPerformanceData( &data );
	return data.CurrentLatencyInSamples;
}

void STDMETHODCALLTYPE idSoundSink_XAudio2::OnCriticalError( HRESULT error )
{
	lost.store( true, std::memory_order_seq_cst );
}

void STDMETHODCALLTYPE idSoundSink_XAudio2::OnVoiceProcessingPassStart( UINT32 bytesRequired )
{
	if( bytesRequired == 0 )
	{
		return;
	}
	float* out = buffers + nextBuffer * SND_MIX_BLOCK * outputChannels;
	nextBuffer = ( nextBuffer + 1 ) % 4;
	owner->PullBlock( out, outputChannels );
	XAUDIO2_BUFFER buffer = {};
	buffer.AudioBytes = SND_MIX_BLOCK * outputChannels * sizeof( float );
	buffer.pAudioData = ( const BYTE* )out;
	pSourceVoice->SubmitSourceBuffer( &buffer );
}
