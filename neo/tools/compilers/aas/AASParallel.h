/*
===========================================================================
R5 speed (puncho): a loop over an index range on several threads, for the AAS compiler.

The engine's job system is not started in rbdmap ( tools/compilers/main.cpp ), and its thread functions have no
join; the AAS compiler is built into rbdmap and into the game. So this is the standard library's thread, with
nothing else behind it: no pool, no state between two calls.
===========================================================================
*/

#ifndef __AASPARALLEL_H__
#define __AASPARALLEL_H__

// idlib/Str.h turns strcmp and strncmp into other names; the standard headers below are read without that
#pragma push_macro( "strcmp" )
#pragma push_macro( "strncmp" )
#undef strcmp
#undef strncmp
#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>
#pragma pop_macro( "strncmp" )
#pragma pop_macro( "strcmp" )

// Above 0 on a thread while that thread works in a loop of AAS_ParallelFor: its workers, and the thread that started
// it ( defined in AASReach.cpp ). On such a thread common->Error and common->FatalError raise nothing: they only
// throw their text as an idException or an idFatalException ( rbdmap: tools/compilers/main.cpp and main_posix.cpp;
// the game: framework/Common_printf.cpp, whose Error throws off the main thread in any case ). The loop catches it,
// stops, joins its threads and calls the function again from the thread that started the loop. rbdmap's functions
// print and call exit(): under running threads that lost the printed text and the exit code.
// This mark is the only test. It is the thread's own, so an error of any other thread of the game goes on as it
// always did. It covers every call the loop's threads can make: body and progress run between the ++ and the --
// in `run` below, on the workers and on the thread that started the loop alike; outside them that thread only
// starts and joins threads ( a thread that cannot be started is caught there ), and the error the loop raises
// itself comes after the last join, when the mark is down again.
extern thread_local int aasParallelThreadDepth;
bool AAS_ParallelThreadInLoop();

// what AAS_ParallelFor did
typedef struct aasParallelRun_s
{
	int					threads;			// the threads that worked, the calling thread among them
	int					controlWords;		// workers whose floating point control word was not the calling thread's: must be 0
} aasParallelRun_t;

/*
================
AAS_NumThreads

  the number of threads for a cvar value: 0 = one a logical processor
================
*/
ID_INLINE int AAS_NumThreads( const int requested )
{
	int num = requested;

	if( num <= 0 )
	{
		num = ( int )std::thread::hardware_concurrency();
	}
	if( num < 1 )
	{
		num = 1;
	}
	if( num > 64 )
	{
		num = 64;
	}
	return num;
}

/*
================
AAS_ParallelFor

  Calls body( first, last, worker ) for chunks [first, last) that together are [0, count), every index once.
  The chunks come from one counter, so which worker gets which chunk differs from run to run: body may write
  only what belongs to the indexes of its chunk and to its own worker number ( 0 .. numThreads - 1 ), and
  what it computes for an index may not depend on the chunk or the worker. Then the result is the same for
  every thread count.

  The calling thread is worker 0 and works too. After each of its chunks it calls progress( done ), done =
  the indexes all workers have finished so far; after every worker has ended it calls progress( count ) once
  more. progress runs on the calling thread only, so it may print and may call the pacifier. It must not call
  code that catches an idException itself: an error caught there would not stop the loop.

  body must not call common->Printf, common->Warning or a pacifier function ( in rbdmap they are not made for
  two threads ). It may call common->Error and common->FatalError, and so may progress: on a thread of the loop
  they only throw their text. An error stops every worker from taking another chunk; chunks already taken are
  finished. Then all threads are joined, and the error is raised once, with its text, by common->Error or
  common->FatalError on the calling thread; the work is not complete. Anything else that is thrown is thrown
  again on the calling thread after the join.
  WHICH error, when several chunks raise one: the one of the chunk with the lowest indexes. Chunks are handed
  out in ascending order, so every chunk below a chunk that raised an error was handed out before it and runs
  to its end or to its own error. A body that goes through its chunk in ascending order therefore reports the
  error of the lowest index that has one: the error a loop on one thread reports, whatever the thread count.

  Workers run with the calling thread's floating point control word ( rounding, masks, flush to zero ); the
  result says how many did not. That is done where USE_INTRINSICS_SSE is defined. On other processors ( ARM )
  the control state is NOT copied: there the workers run with the default state of a new thread, which is the
  calling thread's unless the program changed it ( rbdmap does not; the game does on x86 only ).
  With numThreads 1, or when no thread can be started, the calling thread does all of it.
================
*/
template< class _body_, class _progress_ >
aasParallelRun_t AAS_ParallelFor( const int count, const int chunk, const int numThreads, _body_ body, _progress_ progress )
{
	std::atomic<long long> next( 0 );		// 64 bits: every worker adds chunk once more after the last index
	std::atomic<int> done( 0 );
	std::atomic<int> controlWords( 0 );
	std::atomic<bool> stop( false );
	std::atomic<bool> thrown( false );
	std::mutex errorLock;					// the kept error, below; an error is rare
	long long errorFirst = 0;				// the first index of the chunk whose error is kept
	std::exception_ptr exception;
	bool haveError = false;
	bool haveText = false;
	bool fatal = false;
	char errorText[idException::MAX_ERROR_LEN];
	int numStarted = 1;
#if defined( USE_INTRINSICS_SSE )
	const unsigned int controlWord = _mm_getcsr() & ~0x3fU;		// without the six status flags
#endif

	errorText[0] = '\0';

	// keeps an error if it is the first, or of a chunk with lower indexes than the one kept. kind: 0 = the text of
	// common->Error, 1 = the text of common->FatalError, 2 = anything else ( kept as it was thrown )
	auto keepError = [&]( const long long chunkFirst, const int kind, const char* text )
	{
		std::lock_guard<std::mutex> lock( errorLock );
		if( !haveError || chunkFirst < errorFirst )
		{
			haveError = true;
			errorFirst = chunkFirst;
			haveText = ( kind != 2 );
			fatal = ( kind == 1 );
			if( kind == 2 )
			{
				exception = std::current_exception();
			}
			else
			{
				exception = nullptr;
				idStr::Copynz( errorText, text, sizeof( errorText ) );
			}
		}
		thrown.store( true );
		stop.store( true );
	};

	auto run = [&]( const int worker )
	{
		long long first = 0;

#if defined( USE_INTRINSICS_SSE )
		if( worker != 0 )
		{
			_mm_setcsr( controlWord );
			if( ( _mm_getcsr() & ~0x3fU ) != controlWord )
			{
				controlWords.fetch_add( 1 );
			}
		}
#endif
		aasParallelThreadDepth++;
		try
		{
			while( !stop.load() )
			{
				first = next.fetch_add( chunk );
				if( first >= count )
				{
					break;
				}
				const int last = ( count - first > chunk ) ? ( int )first + chunk : count;
				body( ( int )first, last, worker );
				const int finished = done.fetch_add( last - ( int )first ) + ( last - ( int )first );
				if( worker == 0 )
				{
					progress( finished );
				}
			}
		}
		catch( idException& e )
		{
			keepError( first, 0, e.GetError() );
		}
		catch( idFatalException& e )
		{
			keepError( first, 1, e.GetError() );
		}
		catch( ... )
		{
			keepError( first, 2, NULL );
		}
		aasParallelThreadDepth--;
	};

	{
		// nothing in this block calls common->Error or common->FatalError outside `run`
		std::vector<std::thread> threads;
		for( int worker = 1; worker < numThreads; worker++ )
		{
			try
			{
				threads.emplace_back( run, worker );
			}
			catch( ... )
			{
				// no further thread: the workers that run take the chunks
				break;
			}
		}
		run( 0 );
		for( size_t t = 0; t < threads.size(); t++ )
		{
			threads[t].join();
		}
		numStarted += ( int )threads.size();
	}

	// no thread of the loop runs any more, and this thread's mark is down: now the kept error is raised, once,
	// where errors are raised
	if( thrown.load() )
	{
		if( haveText )
		{
			if( fatal )
			{
				common->FatalError( "%s", errorText );
			}
			common->Error( "%s", errorText );
		}
		if( exception )
		{
			std::rethrow_exception( exception );
		}
	}
	progress( count );

	aasParallelRun_t result;
	result.threads = numStarted;
	result.controlWords = controlWords.load();
	return result;
}

#endif /* !__AASPARALLEL_H__ */
