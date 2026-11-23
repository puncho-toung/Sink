//puncho - C4-R: main-thread room preparation and fenced ownership, no engine state.
#ifndef __SND_REVERBMIX_H__
#define __SND_REVERBMIX_H__

#include "snd_mixer.h"
#include "snd_irbuilder.h"

enum { SND_RVB_FREE, SND_RVB_LOADING, SND_RVB_READY, SND_RVB_LIVE, SND_RVB_RELEASING };

struct sndReverbMixIn_t
{
	const float*		zones;			// Valid throughout Tick; first seven floats are an sndIRRecord_t.
	int					numZones;
	int					zoneStride;		// Byte stride >= sizeof(sndIRRecord_t); invalid stride means no zones (asserts in debug).
	unsigned int		mapSerial;		// Change whenever the records change, including a map change.
	int					zone;
	int					candidate;
	float				dtMs;			// Finite, 0..1,000,000 ms; outside that interval counts as zero.
	float				send;			// Finite, 0..4; outside that interval counts as zero.
	float				fadeMs;
	float				returnGain;
	bool				cut;			// A one-tick latch, consumed by the caller.
	bool				heard;
	unsigned long long	published;		// Link value after the last publish, before this one; sequences must not wrap.
	unsigned long long	acknowledged;	// Never above the link's acknowledgement.
	unsigned long long	statusSequence;	// This and tail come from the same status snapshot.
	int					tail[2];		// All three sequences belong to the new link after a restart.
};

class idSoundReverbMix
{
public:
					idSoundReverbMix();
	// Two 16-byte-aligned MemoryBytes(SND_CONV_MAX_PARTITIONS) blocks, kept until Shutdown.
	bool			Init( void* memoryA, void* memoryB );
	void			Shutdown();	// Audio stopped and waited for; free the two blocks only afterwards.
	// Once immediately before every publish: copy both entries whole, at the same indices, live or not.
	// Never after failed Init. The caller never calls a convolver itself.
	void			Tick( const sndReverbMixIn_t& in, sndMixReverb_t out[2] );
	// At every link restart, with audio stopped and waited for. The restart's first frame is empty.
	void			ReclaimAll();
	int				State( int i ) const;
	int				Zone( int i ) const;

private:
					idSoundReverbMix( const idSoundReverbMix& ) = delete;
	idSoundReverbMix& operator=( const idSoundReverbMix& ) = delete;

	struct instance_t
	{
		int					state;
		int					zone;
		unsigned long long	zeroSequence;
		unsigned long long	releaseSequence;
		bool				closing;
		bool				discard;
	};

	idSoundConvolver	convolver[2];
	instance_t			instance[2];
	idSoundIRBuilder	builder;
	unsigned int		mapSerial;
	int					playing;
	int					fadingFrom;
	float				progress;
	bool				initialized;
	bool				cutPending;
};

#endif
