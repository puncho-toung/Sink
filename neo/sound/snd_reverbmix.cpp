//puncho - C4-R: the mailboxes fence ownership; no convolver query crosses it.
#include "snd_reverbmix.h"
#include <assert.h>
#include <math.h>
#include <string.h>
#include <xmmintrin.h>

static_assert( SND_IR_MAX_PARTITIONS == SND_CONV_MAX_PARTITIONS, "Room partition caps must agree" );

static_assert( sizeof( sndIRRecord_t ) == 7 * sizeof( float ), "Room builder record is seven floats" );

idSoundReverbMix::idSoundReverbMix() :
	instance{}, mapSerial( 0 ), playing( -1 ), fadingFrom( -1 ), progress( 0.0f ),
	initialized( false ), cutPending( false )
{
	instance[0].zone = instance[1].zone = -1;
}

bool idSoundReverbMix::Init( void* memoryA, void* memoryB )
{
	if ( initialized || !convolver[0].Init( memoryA, SND_CONV_MAX_PARTITIONS ) )
	{
		return false;
	}
	if ( !convolver[1].Init( memoryB, SND_CONV_MAX_PARTITIONS ) )
	{
		convolver[0].Shutdown();
		return false;
	}
	initialized = true;
	ReclaimAll();
	return true;
}

void idSoundReverbMix::ReclaimAll()
{
	if ( initialized )
	{
		convolver[0].Flush();
		convolver[1].Flush();
	}
	for ( int i = 0; i < 2; i++ )
	{
		instance[i].state = SND_RVB_FREE;
		instance[i].zone = -1;
		instance[i].zeroSequence = 0;
		instance[i].releaseSequence = 0;
		instance[i].closing = instance[i].discard = false;
	}
	mapSerial = 0;
	playing = fadingFrom = -1;
	progress = 0.0f;
	cutPending = false;
}

void idSoundReverbMix::Shutdown()
{
	if ( !initialized )
	{
		return;
	}
	ReclaimAll();
	convolver[0].Shutdown();
	convolver[1].Shutdown();
	initialized = false;
}

int idSoundReverbMix::State( int i ) const
{
	assert( i >= 0 && i < 2 );
	return instance[i].state;
}

int idSoundReverbMix::Zone( int i ) const
{
	assert( i >= 0 && i < 2 );
	return instance[i].zone;
}

void idSoundReverbMix::Tick( const sndReverbMixIn_t& in, sndMixReverb_t out[2] )
{
	assert( initialized );
	memset( out, 0, 2 * sizeof( *out ) );
	if ( in.zones == NULL && instance[0].state == SND_RVB_FREE &&
		instance[1].state == SND_RVB_FREE && in.mapSerial == mapSerial )
	{
		return;
	}
	assert( in.zones == NULL || in.zoneStride >= (int)sizeof( sndIRRecord_t ) );
	const float* zones = in.zoneStride >= (int)sizeof( sndIRRecord_t ) ? in.zones : NULL;
	const float dtMs = in.dtMs > 0.0f && in.dtMs <= 1000000.0f ? in.dtMs : 0.0f;
	const float send = in.send >= 0.0f && in.send <= 4.0f ? in.send : 0.0f;
	const bool newMap = in.mapSerial != mapSerial;
	mapSerial = in.mapSerial;
	const int zone = zones != NULL && in.zone >= 0 && in.zone < in.numZones ? in.zone : -1;
	const int candidate = zones != NULL && in.candidate >= 0 && in.candidate < in.numZones ? in.candidate : -1;
	if ( newMap || in.cut )
	{
		for ( int i = 0; i < 2; i++ )
		{
			instance_t& s = instance[i];
			if ( newMap )
			{
				if ( s.state < SND_RVB_LIVE )
				{
					s.state = SND_RVB_FREE;
					s.zone = -1;
					s.zeroSequence = s.releaseSequence = 0;
				}
				else
				{
					s.discard = true;
				}
			}
			if ( s.state == SND_RVB_LIVE && !s.closing )
			{
				// A previous zero input does not prove that the return's zero was mixed.
				s.closing = true;
				s.zeroSequence = 0;
			}
		}
		playing = fadingFrom = -1;
		progress = 0.0f;
		cutPending = in.cut;
	}
	for ( int i = 0; i < 2; i++ )
	{
		instance_t& s = instance[i];
		if ( s.state == SND_RVB_RELEASING && in.acknowledged >= s.releaseSequence )
		{
			// Even a status claiming no tail is not permission to touch the instance.
			// The acknowledgement is permission; a forced close always flushes first.
			if ( s.closing || s.discard )
			{
				convolver[i].Flush();
			}
			s.state = s.discard ? SND_RVB_FREE : SND_RVB_READY;
			if ( s.discard ) { s.zone = -1; }
			s.closing = s.discard = false;
			s.zeroSequence = s.releaseSequence = 0;
		}
	}
	if ( zone < 0 )
	{
		playing = fadingFrom = -1;
		progress = 0.0f;
	}

	// A still-live previous room can reverse a fade without touching its history.
	for ( int i = 0; i < 2; i++ )
	{
		if ( zone >= 0 && i != playing && instance[i].state == SND_RVB_LIVE &&
			!instance[i].closing && instance[i].zone == zone )
		{
			fadingFrom = playing;
			progress = playing < 0 ? 0.0f : 1.0f - progress;
			playing = i;
			break;
		}
	}

	// One builder, one destination and at most twenty partitions in this tick.
	int wanted = playing < 0 || instance[playing].zone != zone ? zone : candidate;
	if ( playing >= 0 && wanted == instance[playing].zone ) { wanted = -1; }
	if ( zone < 0 ) { wanted = candidate; }
	int prepare = playing >= 0 ? 1 - playing :
		( instance[0].state >= SND_RVB_LIVE && instance[1].state < SND_RVB_LIVE ? 1 : 0 );
	// DR-314: losing the current zone does not interrupt the candidate's slice.
	bool waitingForRoom = false;
	for ( int i = 0; i < 2; i++ )
	{
		if ( playing < 0 && wanted >= 0 && instance[i].zone == wanted )
		{
			const instance_t& s = instance[i];
			//puncho - RM2-3: keep the other slot untouched while this retained room returns.
			if ( !s.discard && ( s.state == SND_RVB_RELEASING || ( s.state == SND_RVB_LIVE && s.closing ) ) )
			{
				prepare = i;
				waitingForRoom = true;
				break;
			}
			if ( s.state == SND_RVB_LOADING || s.state == SND_RVB_READY ) { prepare = i; }
		}
	}
	for ( int i = 0; i < 2; i++ )
	{
		if ( !waitingForRoom && ( ( i != prepare && instance[i].state == SND_RVB_LOADING ) ||
			( zones == NULL && instance[i].state < SND_RVB_LIVE ) ) )
		{
			instance[i].state = SND_RVB_FREE;
			instance[i].zone = -1;
		}
	}
	instance_t& next = instance[prepare];
	if ( next.state < SND_RVB_LIVE )
	{
		if ( wanted < 0 )
		{
			if ( next.state == SND_RVB_LOADING || zones == NULL )
			{
				next.state = SND_RVB_FREE;
				next.zone = -1;
			}
		}
		else if ( next.state == SND_RVB_FREE || next.zone != wanted || next.state == SND_RVB_LOADING )
		{
			const unsigned int savedMode = _mm_getcsr();
			_mm_setcsr( savedMode | 0x8040 );
			if ( next.state == SND_RVB_FREE || next.zone != wanted )
			{
				convolver[prepare].BeginIR();
				// DR-314: byte stride belongs to the caller; roomSize or padding is skipped.
				sndIRRecord_t record;
				memcpy( &record, (const unsigned char*)zones + (size_t)wanted * (size_t)in.zoneStride, sizeof( record ) );
				builder.Begin( record, 1000u + (unsigned int)wanted );
				next.state = SND_RVB_LOADING;
				next.zone = wanted;
			}
			if ( next.state == SND_RVB_LOADING )
			{
				float left[SND_IR_PARTITION], right[SND_IR_PARTITION];
				for ( int n = 0; n < 20 && builder.PartitionsDone() < builder.NumPartitions(); n++ )
				{
					const int partition = builder.PartitionsDone();
					builder.NextPartition( left, right );
					convolver[prepare].LoadPartition( partition, left, right );
				}
				if ( builder.PartitionsDone() == builder.NumPartitions() )
				{
					convolver[prepare].EndIR( builder.NumPartitions() );
					next.state = SND_RVB_READY;
				}
			}
			_mm_setcsr( savedMode );
		}
	}
	if ( zone >= 0 && next.state == SND_RVB_READY && next.zone == zone &&
		( playing < 0 || instance[playing].zone != zone ) )
	{
		fadingFrom = playing;
		playing = prepare;
		next.state = SND_RVB_LIVE;
		progress = cutPending ? 1.0f : 0.0f;
		cutPending = false;
	}
	else if ( playing >= 0 && progress < 1.0f )
	{
		progress += in.fadeMs > 0.0f ? dtMs / in.fadeMs : 1.0f;
		if ( progress > 1.0f ) { progress = 1.0f; }
	}
	float gains[2] = {};
	if ( playing >= 0 )
	{
		gains[playing] = progress >= 1.0f ? send : send * sinf( progress * 1.57079632679489661923f );
		if ( fadingFrom >= 0 && progress < 1.0f )
		{
			gains[fadingFrom] = send * cosf( progress * 1.57079632679489661923f );
		}
	}
	for ( int i = 0; i < 2; i++ )
	{
		instance_t& s = instance[i];
		if ( s.state != SND_RVB_LIVE ) { continue; }
		out[i].convolver = &convolver[i];
		out[i].live = 1;
		out[i].inGain = s.closing ? 0.0f : gains[i];
		out[i].outGain = s.closing || !in.heard ? 0.0f : in.returnGain;
		if ( out[i].inGain != 0.0f )
		{
			s.zeroSequence = 0;
		}
		else if ( s.zeroSequence == 0 )
		{
			s.zeroSequence = in.published + 1;
		}
		else if ( in.statusSequence >= s.zeroSequence && ( s.closing || in.tail[i] == 0 ) && i != playing )
		{
			// First fence: the zeros actually reached a block. Second fence: its
			// replacement frame must be acknowledged before the main side can call.
			s.state = SND_RVB_RELEASING;
			s.releaseSequence = in.published + 1;
			out[i].live = 0;
			if ( fadingFrom == i ) { fadingFrom = -1; }
		}
	}
}
