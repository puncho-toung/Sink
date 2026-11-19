/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013-2016 Robert Beckebans
Copyright (C) 2014-2016 Kot in Action Creative Artel

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

#ifndef __SND_LOCAL_H__
#define __SND_LOCAL_H__

#include "WaveFile.h"

// Maximum number of voices we can have allocated
#define MAX_HARDWARE_VOICES 48

// A single voice can play multiple channels (up to 5.1, but most commonly stereo)
// This is the maximum number of channels which can play simultaneously
// This is limited primarily by seeking on the optical drive, secondarily by memory consumption, and tertiarily by CPU time spent mixing
#define MAX_HARDWARE_CHANNELS 64

// We may need up to 3 buffers for each hardware voice if they are all long sounds
#define MAX_SOUND_BUFFERS ( MAX_HARDWARE_VOICES * 3 )

// Maximum number of channels in a sound sample
#define MAX_CHANNELS_PER_VOICE	8

/*
========================
MsecToSamples
SamplesToMsec
========================
*/
ID_INLINE_EXTERN uint32 MsecToSamples( uint32 msec, uint32 sampleRate )
{
	return ( msec * ( sampleRate / 100 ) ) / 10;
}
ID_INLINE_EXTERN uint32 SamplesToMsec( uint32 samples, uint32 sampleRate )
{
	return sampleRate < 100 ? 0 : ( samples * 10 ) / ( sampleRate / 100 );
}

/*
========================
DBtoLinear
LinearToDB
========================
*/
ID_INLINE_EXTERN float DBtoLinear( float db )
{
	return idMath::Pow( 2.0f, db * ( 1.0f / 6.0f ) );
}
ID_INLINE_EXTERN float LinearToDB( float linear )
{
	return ( linear > 0.0f ) ? ( idMath::Log( linear ) * ( 6.0f / 0.693147181f ) ) : -999.0f;
}

//puncho - SA3-E (DR-193): shared unchanged with the route reference curve
/*
========================
SoundDistanceDB

//puncho - R-E6-9: Att( d ), the distance term UpdateVolume always used: 0 inside minDistance, the squared falloff
between, -120 dB at or beyond maxDistance; never below -120. Distances in meters, as the emitter keeps them
========================
*/
ID_INLINE float SoundDistanceDB( const float distance, const float minDistance, const float maxDistance )
{
	if( distance >= maxDistance )
	{
		return -120.0f;
	}
	if( ( distance > minDistance ) && ( maxDistance > minDistance ) )
	{
		const float f = ( distance - minDistance ) / ( maxDistance - minDistance );
		return Max( -120.0f, LinearToDB( Square( 1.0f - f ) ) );
	}
	return 0.0f;
}

// demo sound commands
typedef enum
{
	SCMD_STATE,				// followed by a load game state
	SCMD_PLACE_LISTENER,
	SCMD_ALLOC_EMITTER,
	SCMD_FREE,
	SCMD_UPDATE,
	SCMD_START,
	SCMD_MODIFY,
	SCMD_STOP,
	SCMD_FADE,
	SCMD_CACHESOUNDSHADER,
} soundDemoCommand_t;

#include "SoundVoice.h"

#if defined(USE_OPENAL)

//#define AL_ALEXT_PROTOTYPES

// SRS - Added check on OSX for OpenAL Soft headers vs macOS SDK headers
#if defined(__APPLE__) && !defined(USE_OPENAL_SOFT_INCLUDES)
	#include <OpenAL/al.h>
	#include <OpenAL/alc.h>
#else
	#include <AL/al.h>
	#include <AL/alc.h>
	#include <AL/alext.h>
#endif

#include "OpenAL/AL_SoundSample.h"
#include "OpenAL/AL_SoundVoice.h"
#include "OpenAL/AL_SoundHardware.h"

ID_INLINE_EXTERN ALenum CheckALErrors_( const char* filename, int line )
{
	ALenum err = alGetError();
	if( err != AL_NO_ERROR )
	{
		idLib::Printf( "OpenAL Error: %s (0x%x), @ %s %d\n", alGetString( err ), err, filename, line );
	}
	return err;
}
#define CheckALErrors() CheckALErrors_(__FILE__, __LINE__)

ID_INLINE_EXTERN ALCenum CheckALCErrors_( ALCdevice* device, const char* filename, int linenum )
{
	ALCenum err = alcGetError( device );
	if( err != ALC_NO_ERROR )
	{
		idLib::Printf( "ALC Error: %s (0x%x), @ %s %d\n", alcGetString( device, err ), err, filename, linenum );
	}
	return err;
}
#define CheckALCErrors(x) CheckALCErrors_((x), __FILE__, __LINE__)

#elif defined(_MSC_VER) // DG: stub out xaudio for MinGW etc

#define OPERATION_SET 1

// RB: not available on Windows 8 SDK
#if defined(USE_WINRT) // (_WIN32_WINNT < 0x0602 /*_WIN32_WINNT_WIN8*/)
#include <mmdeviceapi.h>
#include <initguid.h> // For the pkey defines to be properly instantiated.
#include <propkeydef.h>
#include "functiondiscoverykeys_devpkey.h"
#include <string>
#include <vector>

DEFINE_PROPERTYKEY( PKEY_AudioEndpoint_Path, 0x9c119480, 0xddc2, 0x4954, 0xa1, 0x50, 0x5b, 0xd2, 0x40, 0xd4, 0x54, 0xad, 1 );

#pragma comment(lib,"xaudio2.lib")

struct AudioDevice
{
	std::wstring name;
	std::wstring id;
};
#else
#include <dxsdkver.h>
#endif
// RB end

#include <xaudio2.h>
#include <xaudio2fx.h>
#include <X3DAudio.h>

// RB: not available on Windows 8 SDK
#if !defined(USE_WINRT) // (_WIN32_WINNT < 0x0602 /*_WIN32_WINNT_WIN8*/)
	#include <xma2defs.h>
#endif
// RB end

#include "XAudio2/XA2_SoundSample.h"
#include "XAudio2/XA2_SoundVoice.h"
#include "XAudio2/XA2_SoundHardware.h"

//puncho - R-E6-39: the rate the mastering voice is created at, so the input rate of every voice's destination and the
// rate XAudio2 runs each voice's filter at (XA2_SoundHardware.cpp's mastering voice, XA2_SoundVoice.cpp)
const int SND_MIX_SAMPLE_RATE = 44100;

//puncho - R-E6-39, DR-201: the one-pole coefficient for a cutoff at filterRate, the input rate of the voice's
// destination (SND_MIX_SAMPLE_RATE), never the sample's rate; open (XAUDIO2_MAX_FILTER_FREQUENCY) when
// cutoff >= 100,000 Hz. Pure; the voice on both threads and s_smoothSelfTest call it. In XA2_SoundVoice.cpp.
float SoundFilterFrequency( float cutoffHz, float filterRate );

#else // not _MSC_VER => MinGW, GCC, ...
// just a stub for now
#include "stub/SoundStub.h"
#endif // _MSC_VER ; DG end


#include "../libs/oggvorbis/ogg/ogg.h"
#include "../libs/oggvorbis/vorbis/vorbisfile.h"

//
// idSoundDecoder_Vorbis
//
class idSoundDecoder_Vorbis
{
public:
	idSoundDecoder_Vorbis();
	~idSoundDecoder_Vorbis();

	virtual bool				Open( const char* fileName );
	virtual bool				IsEOS( void );
	virtual void				Seek( int samplePos );
	virtual int					Read( void* buffer, int bufferSize );
	virtual int64_t				Size( void );
	virtual int64_t				CompressedSize( void );
	virtual void				GetFormat( idWaveFile::waveFmt_t& format );

private:
	idSoundSample* sample;
	OggVorbis_File* vorbisFile;
	idFile* mhmmio;
};

//------------------------
// Listener data
//------------------------
struct listener_t
{
	idMat3	axis;		// orientation of the listener
	idVec3	pos;		// position in meters
	int		id;			// the entity number, used to detect when a sound is local
	int		area;		// area number the listener is in
};

class idSoundFade
{
public:
	int		fadeStartTime;
	int		fadeEndTime;
	float	fadeStartVolume;
	float	fadeEndVolume;


public:
	idSoundFade()
	{
		Clear();
	}

	void	Clear();
	void	SetVolume( float to );
	void	Fade( float to, int length, int soundTime );

	float	GetVolume( int soundTime ) const;
};

//puncho - R-E6-1, R-E6-2: one value moving toward a target by a fraction alpha per update, a step bounded by maxStep.
// The world fills one parameter set per kind and its alpha once per update (R-E6-3); Step never reads tauMs.
const float SMOOTH_NO_CAP = -1.0f;

struct soundSmoothParms_t
{
	float	tauMs;		// time constant; read only by the caller's Alpha( dtMs, tauMs ), never by Step
	float	maxStep;	// largest move of one Step, in the value's unit; < 0 (SMOOTH_NO_CAP): no cap; 0: no move
	float	arrive;		// |target - value| <= arrive after a Step sets value = target; >= 0
};

class idSoundSmooth
{
public:
	float			value;
	bool			valid;		// false: the next Step snaps; all zero = not valid

	void			Invalidate();
	void			Snap( float target );
	float			Step( float target, float alpha, const soundSmoothParms_t& p, bool snap );
	static float	Alpha( float dtMs, float tauMs );	// 1 when tauMs <= 0; 0 when dtMs <= 0; else 1 - exp( -dtMs / tauMs )
};

/*
================================================
idSoundChannel
================================================
*/
class idSoundChannel
{
public:
	bool	CanMute() const;

	void	Mute();
	bool	CheckForCompletion( int currentTime );

	void	UpdateVolume( int currentTime );
	void	UpdateHardware( float volumeAdd, int currentTime );
	//puncho - SB3-K (R-SB3-22, R-SB3-24): the entry points above call the bodies below < false >, today's code; the
	// ...Gated ones call them < true >. The emitter body calls the UpdateVolume pair and the world body the UpdateHardware
	// pair, each by its own arm. Defined in snd_emitter.cpp
	void	UpdateVolumeGated( int currentTime );
	void	UpdateHardwareGated( float volumeAdd, int currentTime );
private:
	template< bool gated >
	ID_FORCE_INLINE void	UpdateVolumeBody( int currentTime );
	template< bool gated >
	ID_FORCE_INLINE void	UpdateHardwareBody( float volumeAdd, int currentTime );
public:

	// returns true if this channel is marked as looping
	bool	IsLooping() const;

	class idSoundEmitterLocal* 	emitter;

	int						startTime;
	int						endTime;
	int						logicalChannel;
	bool					allowSlow;

	soundShaderParms_t		parms;				// combines shader parms and per-channel overrides
	const idSoundShader* 	soundShader;
	idSoundSample* 			leadinSample;
	idSoundSample* 			loopingSample;
	idSoundFade				volumeFade;

	float					volumeDB;			// last volume at which this channel will play (calculated in UpdateVolume)
	float					currentAmplitude;	// current amplitude on the hardware voice

	// hardwareVoice will be freed and NULL'd when a sound is out of range,
	// and reallocated when it comes back in range
	idSoundVoice* 			hardwareVoice;

	//puncho - R-E6-9, R-E6-11, R-E6-12, R-E6-42: written every update for every channel that reaches them; SB3's log reads
	// them. excessDB glides the occlusion excess over the straight distance term; panOffset glides the portal displacement
	// (spatializedOrigin - origin); occlusion glides the low-pass toward occTarget. Pitch is not smoothed. 48 bytes.
	float					excessTargetDB;
	float					occTarget;
	idSoundSmooth			excessDB;
	idSoundSmooth			occlusion;
	idSoundSmooth			panOffset[3];

	// only allocated by the soundWorld block allocator
	idSoundChannel();
	~idSoundChannel();
};

// Maximum number of SoundChannels for a single SoundEmitter.
// This is probably excessive...
const int MAX_CHANNELS_PER_EMITTER = 16;

//puncho start - SB3-T (DR-210): moved unchanged from snd_world.cpp for the out-of-line L1 reader
/*
========================
idActiveChannel
========================
*/
class idActiveChannel
{
public:
	idActiveChannel() :
		channel( NULL ),
		sortKey( 0 ) {}
	idActiveChannel( idSoundChannel* channel_, int sortKey_ ) :
		channel( channel_ ),
		sortKey( sortKey_ ) {}

	idSoundChannel* 	channel;
	int					sortKey;
};
//puncho end

/*
===================================================================================

idSoundWorldLocal

===================================================================================
*/

class idSoundAcoustics;		//puncho - SB3: snd_acoustics.h
class idSoundOcclusion;		//puncho - SA3-E (R-SA3-23): only a baked world owns one
#include "snd_reverb.h"			//puncho - SB4-L (D-48): idSoundReverb, the world's member reverb; after idSoundSmooth

class idSoundWorldLocal : public idSoundWorld
{
public:
	idSoundWorldLocal();
	virtual					~idSoundWorldLocal();

	//------------------------
	// Functions from idSoundWorld, implemented in SoundWorld.cpp
	//------------------------

	// Called at map start
	virtual void			ClearAllSoundEmitters();
	virtual bool CarryEmitter( idSoundEmitter* emitter, const char* shader );
	virtual idSoundEmitter* AdoptCarriedEmitter( const char* shader );
	virtual void TransitionArrival();
	virtual void TouchTransitionMedia();
	bool IsCarriedEmitter( const idSoundEmitter* emitter ) const;
	idSoundEmitterLocal* transitionEmitters[4];
	char transitionShaders[4][256];
	int transitionExpire[4];

	//puncho start - SB3 (R-SB3-1, R-SB3-21, R-SB3-30): the map's baked acoustics, in snd_acoustics.cpp. acoustics is
	// set only when every check of the load passed; NULL is today's sound. Nothing here is saved or sent.
	virtual void			LoadMapAcoustics( const char* mapName, unsigned int mapGeometryCRC, const idMapFile* mapFile );
	virtual void			FreeMapAcoustics();
	idSoundAcoustics*		ListenerPass( idSoundAcoustics* current );		// SB3-K: the gate of this update, or NULL
private:
	template< bool gated >
	ID_FORCE_INLINE void	UpdateBody( idSoundAcoustics* gate, bool logRows );	// SB3-K: Update's body, one per arm
public:
	idSoundAcoustics*		acoustics;						// read by SB4's and SA3's gated lines
	char					acousticsLine[128];				// the last console line of the load or free
	//puncho start - SB3-T: out-of-line tools keep their locals out of Update's inlined body (D-46)
	void					DrawAcoustics( int show );	//puncho - SB3-T (OP8-1): read the gate out of line
	void					ListAcoustics( const idCmdArgs& args ) const;
	const char*				AcousticsState() const;
	void					AcousticsLog_World( const idStaticList< idActiveChannel, MAX_HARDWARE_VOICES >& channels, int currentTime ) const;	//puncho - SB3-T (OP8-1)
	//puncho end
private:
	static void				DeleteAcoustics( idSoundAcoustics* it );	// the one teardown of the object (L3-3)
	// SB3-W (W4-3): what a load in progress holds, reachable from the world so that an Error inside the load leaks
	// nothing up to step 9; NULL outside LoadMapAcoustics. ReleaseAcousticsLoad is their one release. The cover ends
	// at step 9's graph allocation: from there to the publish at step 13 the graph and the object are held by the
	// load's locals only, so nothing in that window may raise an Error, the hooks of steps 11 and 12 included (D-40)
	void					ReleaseAcousticsLoad();
	idFile*					acousticsLoadFile;				// the open .sndbake
	byte*					acousticsLoadBlock;				// the step 1 block until step 10 hands it to the object
	byte*					acousticsLoadScratch;			// step 8's buffer
public:
	//puncho end

	// stop all playing sounds
	virtual void			StopAllSounds();

	// get a new emitter that can play sounds in this world
	virtual idSoundEmitter* AllocSoundEmitter();

	// for load games
	virtual idSoundEmitter* EmitterForIndex( int index );

	// query data from all emitters in the world
	virtual float			CurrentShakeAmplitude();

	// where is the camera
	virtual void			PlaceListener( const idVec3& origin, const idMat3& axis, const int listenerId );
	virtual void			ListenerCut( int reasons );		//puncho - R-E6-6

	// fade all sounds in the world with a given shader soundClass
	// to is in Db, over is in seconds
	virtual void			FadeSoundClasses( const int soundClass, const float to, const float over );

	// menu sounds
	virtual int				PlayShaderDirectly( const char* name, int channel = -1 );

	virtual void			Skip( int time );

	virtual void			Pause();
	virtual void			UnPause();
	virtual bool			IsPaused()
	{
		return isPaused;
	}

	virtual int				GetSoundTime();

	// SaveGame Support
	virtual void			WriteToSaveGame( idFile* savefile );
	virtual void			ReadFromSaveGame( idFile* savefile );

	virtual void			SetSlowmoSpeed( float speed );
	virtual void			SetEnviroSuit( bool active );

	//=======================================

	//------------------------
	// Random stuff that's not exposed outside the sound system
	//------------------------
	void			Update();
	void			OnReloadSound( const idDecl* decl );

	idSoundChannel* 	AllocSoundChannel();
	void				FreeSoundChannel( idSoundChannel* );

public:
	// even though all these variables are public, nobody outside the sound system includes SoundWorld_local.h
	// so this is equivalent to making it private and friending all the other classes in the sound system

	idSoundFade			volumeFade;						// master volume knob for the entire world
	idSoundFade			soundClassFade[SOUND_MAX_CLASSES];

	idRenderWorld* 		renderWorld;	// for debug visualization and light amplitude sampling

	float				currentCushionDB;	// channels at or below this level will be faded to 0
	float				shakeAmp;			// last calculated shake amplitude

	listener_t			listener;
	idList<idSoundEmitterLocal*, TAG_AUDIO>	emitters;

	idSoundEmitter* 	localSound;			// for PlayShaderDirectly()

	idBlockAlloc<idSoundEmitterLocal, 16>	emitterAllocator;
	idBlockAlloc<idSoundChannel, 16>		channelAllocator;

	idSoundFade				pauseFade;
	int						pausedTime;
	int						accumulatedPauseTime;
	bool					isPaused;

	float					slowmoSpeed;
	bool					enviroSuitActive;

	//puncho - R-E6-5, R-E6-6: the listener cut. ListenerCut ORs into cutPending (into cutHeld while cutOnNextPlace
	// is set, so a respawn raised inside a load lands in the load's own cut); the top of Update moves cutPending into
	// listenerCut / listenerCutReasons for exactly one update. Nothing here is saved or sent.
	// R-E6-19: a listener in a wall keeps its last valid area for a moment instead of silencing every emitter.
	// Ints, then the bools together, so no padding gap opens between them.
	int						listenerCutReasons;
	int						cutPending;
	int						cutHeld;
	int						lastValidListenerArea;	// -1 = none since the last clear, restore or cut
	int						areaHeldTime;			// GetSoundTime() when the hold began
	bool					listenerCut;
	bool					cutOnNextPlace;
	bool					lastListenerPosValid;
	bool					listenerAreaHeld;		// listener.area is lastValidListenerArea, not PointInArea's -1
	idVec3					lastListenerPos;		// units, for R-E6-8's distance net
	idSoundFade				areaFade;				// to DB_SILENCE after s_smoothAreaHoldMs held, back to 0 in an area

	//puncho - R-E6-3, R-E6-14, R-E6-38: filled once at the top of Update for every channel's steps: the measured time,
	// the three parameter sets and their alphas, and whether this update's steps snap (a cut, s_smooth 0). R-E6-24: the
	// s_smoothLog file for this update's rows. Nothing here is saved or sent.
	uint64					smoothLastUpdateUs;		// Sys_Microseconds() of the last update; 0 = none yet
	idFile* 				smoothLog;				// NULL while s_smoothLog is empty
	soundSmoothParms_t		gainParms;				// excessDB (R-E6-9)
	soundSmoothParms_t		panParms;				// panOffset (R-E6-11)
	soundSmoothParms_t		filterParms;			// occlusion (R-E6-12)
	float					updateDtMs;				// ms since the last update, [0, s_smoothMaxDtMs]; 0 on the first
	float					gainAlpha;
	float					panAlpha;
	float					filterAlpha;
	float					cutRampLeftMs;			// R-E6-38: ms left of the cut ramp; 0 while s_smoothCutRampMs is 0
	bool					smoothSnap;				// listenerCut (unless ramped) or s_smooth 0
	bool					smoothStepsSkipped;		// V3S-1: the last update ran no emitter's gain step (area -1, muted, not playing)

	idSoundOcclusion*		occlusion;				//puncho - SA3-E (C-SB89d): NULL without a bake; nothing saved

	//puncho - SB4-L (R-SB4-1, D-48): the reverb of this world, whose hooks LoadMapAcoustics and FreeMapAcoustics call and
	// whose Update is W1's one line. Public: SB3-T's log reads it from the sound system. Last, so no member before it
	// moves; zeroed by Init in the constructor (C-SB6m). Nothing here is saved or sent
	idSoundReverb			reverb;

public:
	struct soundPortalTrace_t
	{
		int		portalArea;
		const soundPortalTrace_t* prevStack;
	};

	void			ResolveOrigin( const int stackDepth, const soundPortalTrace_t* prevStack, const int soundArea, const float dist, const idVec3& soundOrigin, idSoundEmitterLocal* def );
};


//puncho - R-E6-20: the offsets an emitter whose origin is in no area retries, in order: distance outer (8, 16, 32 units),
// then +z, -z, +x, -x, +y, -y; the first one in an area wins. Defined in snd_emitter.cpp; holds no state.
const int SOUND_SOLID_RETRIES = 18;
extern const idVec3 soundSolidRetryOffsets[ SOUND_SOLID_RETRIES ];

//puncho - R-E6-24: closes the s_smoothLog file if one is open; safe with none. idSoundSystemLocal::Shutdown calls it.
// In snd_world.cpp
void SmoothLog_Close();
void SmoothLog_WriteFailed();	// the log stops on a failed row: a console line, s_smoothLog cleared, the file closed

/*
================================================
idSoundEmitterLocal
================================================
*/
class idSoundEmitterLocal : public idSoundEmitter
{
public:
	virtual void	Free( bool immediate );

	virtual void	Reset();

	virtual void	UpdateEmitter( const idVec3& origin, int listenerId, const soundShaderParms_t* parms );

	virtual int		StartSound( const idSoundShader* shader, const s_channelType channel, float diversity = 0, int shaderFlags = 0, bool allowSlow = true );

	virtual void	ModifySound( const s_channelType channel, const soundShaderParms_t* parms );
	virtual void	StopSound( const s_channelType channel );

	virtual void	FadeSound( const s_channelType channel, float to, float over );

	virtual bool	CurrentlyPlaying( const s_channelType channel = SCHANNEL_ANY ) const;

	virtual	float	CurrentAmplitude();

	virtual	int		Index() const;

	//----------------------------------------------

	void			Init( int i, idSoundWorldLocal* sw );

	// Returns true if the emitter should be freed.
	bool			CheckForCompletion( int currentTime );

	void			OverrideParms( const soundShaderParms_t* base, const soundShaderParms_t* over, soundShaderParms_t* out );

	void			Update( int currentTime );
	//puncho - SB3-K (R-SB3-22, R-SB3-24): Update calls the body below < false >, today's code; UpdateGated calls it
	// < true >. The world body calls one of the two by its own arm. Defined in snd_emitter.cpp
	void			UpdateGated( int currentTime );
private:
	template< bool gated >
	ID_FORCE_INLINE void	UpdateBody( int currentTime );
public:
	void			OnReloadSound( const idDecl* decl );

	//----------------------------------------------

	idSoundWorldLocal* 		soundWorld;						// the world that holds this emitter

	int			index;							// in world emitter list
	bool		canFree;						// if true, this emitter can be canFree (once channels.Num() == 0)

	// a single soundEmitter can have many channels playing from the same point
	idStaticList<idSoundChannel*, MAX_CHANNELS_PER_EMITTER> channels;

	//----- set by UpdateEmitter -----
	idVec3				origin;
	soundShaderParms_t	parms;
	int					emitterId;						// sounds will be full volume when emitterId == listenerId

	//----- set by Update -----
	int			lastValidPortalArea;
	float		directDistance;
	float		spatializedDistance;
	idVec3		spatializedOrigin;

	//puncho - R-E6-20: every retry around origin missed; plays as today until origin moves more than 1 unit from solidOrigin
	bool		inSolid;
	bool		noPath;		//puncho - R-E6-11 (V3R-4): this update's portal walk found no way to the listener; in inSolid's padding
	idVec3		solidOrigin;

	//puncho - SA3-E (R-SA3-19): E1 targets only, no smoothing or saved state; C-SB89b, 8 B of members,
	// 16 B per emitter allocated because the block allocator rounds to 16-byte slots (DR-195).
	float		portalLossDb;
	float		portalOcclusion;

	// sound emitters are only allocated by the soundWorld block allocator
	idSoundEmitterLocal();
	virtual			~idSoundEmitterLocal();
};


/*
===================================================================================

idSoundSystemLocal

===================================================================================
*/
class idSoundSystemLocal : public idSoundSystem
{
public:
	// all non-hardware initialization
	virtual void			Init();

	// shutdown routine
	virtual	void			Shutdown();

	virtual idSoundWorld* 	AllocSoundWorld( idRenderWorld* rw );
	virtual void			FreeSoundWorld( idSoundWorld* sw );

	// specifying NULL will cause silence to be played
	virtual void			SetPlayingSoundWorld( idSoundWorld* soundWorld );

	// some tools, like the sound dialog, may be used in both the game and the editor
	// This can return NULL, so check!
	virtual idSoundWorld* 	GetPlayingSoundWorld();

	// sends the current playing sound world information to the sound hardware
	virtual void			Render();

	// Mutes the SSG_MUSIC group
	virtual void			MuteBackgroundMusic( bool mute )
	{
		musicMuted = mute;
	}

	// sets the final output volume to 0
	// This should only be used when the app is deactivated
	// Since otherwise there will be problems with different subsystems muting and unmuting at different times
	virtual void			SetMute( bool mute )
	{
		muted = mute;
	}
	virtual bool			IsMuted()
	{
		return muted;
	}

	virtual void			OnReloadSound( const idDecl* sound );

	virtual void			StopAllSounds();

	virtual void			InitStreamBuffers();
	virtual void			FreeStreamBuffers();

	virtual void* 			GetIXAudio2() const; // FIXME: stupid name; get rid of this? not sure if it's really needed..

	// RB begin
	virtual void*			GetOpenALDevice() const;
	// RB end

	// for the sound level meter window
	virtual cinData_t		ImageForTime( const int milliseconds, const bool waveform );

	// Free all sounds loaded during the last map load
	virtual	void			BeginLevelLoad();

	// We might want to defer the loading of new sounds to this point
	virtual	void			EndLevelLoad();
	virtual void PurgeUnusedLevelCache();

	// prints memory info
	virtual void			PrintMemInfo( MemInfo_t* mi );
	virtual int64			GetSampleBytes( int& count ) const;

	//-------------------------

	// Before a sound is reloaded, any active voices using it must
	// be stopped.  Returns true if any were playing, and should be
	// restarted after the sound is reloaded.
	void					StopVoicesWithSample( const idSoundSample* const sample );

	void					Restart();
	void					SetNeedsRestart()
	{
		needsRestart = true;
	}

	int						SoundTime() const;

	// may return NULL if there are no more voices left
	idSoundVoice* 			AllocateVoice( const idSoundSample* leadinSample, const idSoundSample* loopingSample );
	void					FreeVoice( idSoundVoice* );

	idSoundSample* 			LoadSample( const char* name );
	virtual bool			IsSampleResident( const char* name ) const;

	virtual void			Preload( idPreloadManifest& preload );

	struct bufferContext_t
	{
		bufferContext_t() :
			voice( NULL ),
			sample( NULL ),
			bufferNumber( 0 )
		{ }

#if defined(USE_OPENAL)
		idSoundVoice_OpenAL* 	voice;
		idSoundSample_OpenAL*	sample;
#elif defined(_MSC_VER) // XAudio backend
		// DG: because the inheritance is kinda strange (idSoundVoice is derived
		// from idSoundVoice_XAudio2), casting the latter to the former isn't possible
		// so we need this ugly #ifdef ..
		idSoundVoice_XAudio2* 	voice;
		idSoundSample_XAudio2* sample;
#else // not _MSC_VER
		// from stub or something..
		idSoundVoice* 	voice;
		idSoundSample* sample;
#endif // _MSC_VER ; DG end

		int bufferNumber;
	};

	// Get a stream buffer from the free pool, returns NULL if none are available
	bufferContext_t* 			ObtainStreamBufferContext();
	void						ReleaseStreamBufferContext( bufferContext_t* p );

	idSysMutex					streamBufferMutex;
	idStaticList< bufferContext_t*, MAX_SOUND_BUFFERS > freeStreamBufferContexts;
	idStaticList< bufferContext_t*, MAX_SOUND_BUFFERS > activeStreamBufferContexts;
	idStaticList< bufferContext_t, MAX_SOUND_BUFFERS > bufferContexts;

	idSoundWorldLocal* 			currentSoundWorld;
	//puncho start - SB3 (D-8): the acoustics log. acousticsGameWorld is the world LoadMapAcoustics was last called on
	// (cleared in that world's destructor); acousticsLog and acousticsLogWorld are SB3-T's and NULL while the log is off
	idFile*						acousticsLog;
	idSoundWorldLocal*			acousticsGameWorld;
	idSoundWorldLocal*			acousticsLogWorld;
	//puncho end
	idStaticList<idSoundWorldLocal*, 32>	soundWorlds;

	idList<idSoundSample*, TAG_AUDIO>		samples;
	idHashIndex					sampleHash;
	idSoundSample*				defaultSample = NULL;	//puncho - DR-175: read once at Init, so a voice's swap to the noise never reads a file

	idSoundHardware				hardware;

	idRandom2					random;

	int							soundTime;
	bool						muted;
	bool						musicMuted;
	bool						needsRestart;

	bool						insideLevelLoad;
	bool						levelSeen = false;	//puncho - R-E6-23 (DR-150): true after the first EndLevelLoad following engine startup

	//-------------------------

	idSoundSystemLocal() :
		currentSoundWorld( NULL ),
		acousticsLog( NULL ),			//puncho - SB3 (D-8)
		acousticsGameWorld( NULL ),
		acousticsLogWorld( NULL ),
		soundTime( 0 ),
		muted( false ),
		musicMuted( false ),
		needsRestart( false )
	{}
};

extern	idSoundSystemLocal	soundSystemLocal;

#endif /* !__SND_LOCAL_H__ */
