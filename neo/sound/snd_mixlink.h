//puncho - C3d-L: single-writer/single-reader latest-value mailboxes, R-C3d-1 to 10.
#ifndef __SND_MIXLINK_H__
#define __SND_MIXLINK_H__

#include "snd_mixer.h"
#include <atomic>

static_assert( std::atomic<unsigned char>::is_always_lock_free, "Sound mailbox byte must be lock-free" );
static_assert( std::atomic<unsigned int>::is_always_lock_free, "Sound block count must be lock-free" );
static_assert( std::atomic<unsigned long long>::is_always_lock_free, "Sound acknowledgement must be lock-free" );
static_assert( sizeof( unsigned long long ) == 8, "Sound sequences must be 64 bits" );
static_assert( sizeof( sndMixFrame_t::sequence ) == 8, "Sound frame sequence must be 64 bits" );
static_assert( sizeof( sndMixStatus_t::sequence ) == 8, "Sound status sequence must be 64 bits" );

class idSoundMixLink
{
public:
						idSoundMixLink();
	// Resets BlocksBegun; nothing read before Init means anything afterward.
	// The caller's gate orders these writes against the audio thread, not this class.
	void				Init( unsigned long long firstSequence = 0 ); // only while no block can run

	// Main thread. References remain owned until the corresponding next hand-off.
	sndMixFrame_t&		WriteFrame();
	unsigned long long	Publish();
	unsigned long long	Published() const;
	unsigned long long	Acknowledged() const;
	unsigned int		BlocksBegun() const;
	const sndMixStatus_t& ReadStatus();

	// Audio thread, once per real mix, in this order. No pointer survives the next BeginBlock.
	const sndMixFrame_t& BeginBlock();
	// Holds an older block's bytes: write every field every block; keep running counts outside it.
	sndMixStatus_t&		WriteStatus();
	void				EndBlock();

private:
	friend struct sndMixLinkTest_t; // quiescent layout/ownership inspection in the standalone test
	enum { FRESH = 4, INDEX = 3 };

	// Arrays need an aligned stride, not merely an aligned first element.
	struct alignas( 128 ) frameSlot_t
	{
		sndMixFrame_t	value;
		unsigned char	padding[128 - sizeof( value ) % 128];
	};
	struct alignas( 128 ) statusSlot_t
	{
		sndMixStatus_t	value;
		unsigned char	padding[128 - sizeof( value ) % 128];
	};

	alignas( 128 ) std::atomic<unsigned char> mid;
	unsigned char		midPadding[128 - sizeof( mid )];
	alignas( 128 ) std::atomic<unsigned long long> acked;
	std::atomic<unsigned int> blocksBegun;
	unsigned char		ackPadding[128 - sizeof( acked ) - sizeof( blocksBegun )];
	alignas( 128 ) std::atomic<unsigned char> statusMid;
	unsigned char		statusPadding[128 - sizeof( statusMid )];

	alignas( 128 ) unsigned long long published;
	unsigned char		back;
	unsigned char		statusFront;
	unsigned char		writerPadding[128 - sizeof( published ) - sizeof( back ) - sizeof( statusFront )];
	alignas( 128 ) unsigned char front;
	unsigned char		statusBack;
	unsigned char		inBlock;
	unsigned char		readerPadding[128 - sizeof( front ) - sizeof( statusBack ) - sizeof( inBlock )];

	frameSlot_t			frames[3];
	statusSlot_t		status[3];
};

#endif // __SND_MIXLINK_H__
