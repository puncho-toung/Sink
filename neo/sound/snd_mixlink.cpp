//puncho - C3d-L: exchanges transfer ownership in both directions; keep their acquire and release halves.
#include "snd_mixlink.h"
#include <string.h>
#include <assert.h>

idSoundMixLink::idSoundMixLink() :
	mid( 1 ), midPadding{}, acked( 0 ), blocksBegun( 0 ), ackPadding{},
	statusMid( 1 ), statusPadding{}, published( 0 ), back( 0 ), statusFront( 2 ),
	writerPadding{}, front( 2 ), statusBack( 0 ), inBlock( 0 ), readerPadding{}, frames{}, status{}
{
}

void idSoundMixLink::Init( unsigned long long firstSequence )
{
	memset( frames, 0, sizeof( frames ) );
	memset( status, 0, sizeof( status ) );
	back = 0;
	mid.store( 1, std::memory_order_relaxed );
	front = 2;
	inBlock = 0;
	statusBack = 0;
	statusMid.store( 1, std::memory_order_relaxed );
	statusFront = 2;
	published = firstSequence;
	acked.store( 0, std::memory_order_relaxed );
	blocksBegun.store( 0, std::memory_order_relaxed );
}

sndMixFrame_t& idSoundMixLink::WriteFrame()
{
	return frames[back].value;
}

unsigned long long idSoundMixLink::Publish()
{
	frames[back].value.sequence = ++published;
	const unsigned char old = mid.exchange( ( unsigned char )( back | FRESH ), std::memory_order_acq_rel );
	back = old & INDEX;
	return published;
}

unsigned long long idSoundMixLink::Published() const
{
	return published;
}

unsigned long long idSoundMixLink::Acknowledged() const
{
	return acked.load( std::memory_order_acquire );
}

unsigned int idSoundMixLink::BlocksBegun() const
{
	return blocksBegun.load( std::memory_order_relaxed );
}

const sndMixStatus_t& idSoundMixLink::ReadStatus()
{
	if ( statusMid.load( std::memory_order_relaxed ) & FRESH )
	{
		const unsigned char old = statusMid.exchange( statusFront, std::memory_order_acq_rel );
		statusFront = old & INDEX;
	}
	return status[statusFront].value;
}

const sndMixFrame_t& idSoundMixLink::BeginBlock()
{
	assert( !inBlock );
	inBlock = 1;
	blocksBegun.fetch_add( 1, std::memory_order_relaxed );
	if ( mid.load( std::memory_order_relaxed ) & FRESH )
	{
		const unsigned char old = mid.exchange( front, std::memory_order_acq_rel );
		front = old & INDEX;
	}
	return frames[front].value;
}

sndMixStatus_t& idSoundMixLink::WriteStatus()
{
	return status[statusBack].value;
}

void idSoundMixLink::EndBlock()
{
	assert( inBlock );
	inBlock = 0;
	const unsigned long long sequence = frames[front].value.sequence;
	status[statusBack].value.sequence = sequence;
	const unsigned char old = statusMid.exchange( ( unsigned char )( statusBack | FRESH ), std::memory_order_acq_rel );
	statusBack = old & INDEX;
	// R-C3d-5/6: publish the status BEFORE acknowledging the frame this completed block used.
	acked.store( sequence, std::memory_order_release );
}
