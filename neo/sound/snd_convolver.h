//puncho - C1-K: engine-independent partitioned convolution, R-C1-1 to R-C1-16.
#ifndef __SND_CONVOLVER_H__
#define __SND_CONVOLVER_H__

#include <stddef.h>

#define SND_CONV_RATE				44100
#define SND_CONV_BLOCK			441
#ifndef SND_CONV_FFT
#define SND_CONV_FFT				960
#endif
#define SND_CONV_EARLY_PARTITIONS	8
//puncho - the director (DR-265): the 1.0 s cap of DR-253 is the engine's; a listening demo may be built at another
// cap to put it to the ear (WP-C1 R-C1-1)
#ifndef SND_CONV_MAX_PARTITIONS
#define SND_CONV_MAX_PARTITIONS	100
#endif

// R-C1-8: one owner at a time, including queries and Shutdown. The caller must
// remove an instance from the mix before loading it. No lock or atomic lives here.
// Init/Shutdown are explicit; the caller owns the aligned block until Shutdown.
// PFFFT at the pinned revision does not handle allocation failure internally:
// Init can reject a NULL setup, but cannot make PFFFT's unchecked malloc safe.
class idSoundConvolver
{
public:
						idSoundConvolver();
	static size_t		MemoryBytes( int maxPartitions );
	bool				Init( void* memory, int maxPartitions );
	void				Shutdown();
	void				BeginIR();
	void				LoadPartition( int k, const float* left, const float* right );
	void				EndIR( int numPartitions );
	// R-C1-13: a zero buffer with nonzero gain is live and keeps the tail alive.
	// A caller with nothing to send passes NULL; Process does not scan the input.
	//puncho - Adds to the supplied rows (NULL input and gain ramps too); idle calls leave them unchanged, and both rows may be the same.
	void				Process( const float* in, float inGain, float earlyGain, float lateGain, float* outLeft, float* outRight );
	int					TailBlocksLeft() const;
	void				Flush();

private:
						idSoundConvolver( const idSoundConvolver& ) = delete;
	idSoundConvolver&	operator=( const idSoundConvolver& ) = delete;
	void				ClearHistory();

	void*				setup;
	float*				irSpectra;
	float*				inputSpectra;
	float*				accumulators;
	float*				window;
	float*				timeBuffer;
	float*				work;
	unsigned char*		live;
	int					capacity;
	int					partitions;
	int					head;
	int					tail;
	float				previousInGain;
	float				previousEarlyGain;
	float				previousLateGain;
	bool				previousLive;
	bool				gainsValid;
#ifndef NDEBUG
	mutable bool		busy;
#endif
};

int SndConvolver_SelfTest( void ( *print )( const char* line ), void* ( *alloc16 )( size_t ), void ( *free16 )( void* ) );

#endif
