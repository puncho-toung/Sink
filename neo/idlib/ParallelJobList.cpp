/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2020 Robert Beckebans

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
#include "precompiled.h"
#pragma hdrstop
#include "ParallelJobList.h"
#include <atomic> //puncho - C1a: publication must not use plain interlocked-wrapper loads
//puncho - C1a: use MXCSR intrinsics on the same targets as renderer initialization.
#if defined(USE_INTRINSICS_SSE)
	#include <xmmintrin.h>
#endif

//puncho - C1a: opt-in normalization; latch before starting the pool, retaining mismatch diagnostics when off.
static idCVar jobs_fpState( "jobs_fpState", "0", CVAR_BOOL | CVAR_SYSTEM | CVAR_INIT, "set job/game threads to main-thread MXCSR controls (startup only; default 0 preserves thread controls and reports mismatches)" );
#if defined(USE_INTRINSICS_SSE)
static const unsigned int JOB_FP_STATUS_MASK = 0x3fU;
static const unsigned int JOB_FP_STATE_UNCAPTURED = ~0U;
static std::atomic<unsigned int> jobFpState( JOB_FP_STATE_UNCAPTURED );
static bool setJobFpState;
#if defined( _DEBUG )
// A capture during an early wake must not assert until that thread's next setup attempt.
static thread_local unsigned int expectedJobFpState = JOB_FP_STATE_UNCAPTURED;
#endif
#endif

void CaptureJobFpState()
{
#if defined(USE_INTRINSICS_SSE)
	assert( jobFpState.load( std::memory_order_relaxed ) == JOB_FP_STATE_UNCAPTURED );
	const unsigned int control = _mm_getcsr() & ~JOB_FP_STATUS_MASK;
	jobFpState.store( control, std::memory_order_release );
#if defined( _DEBUG )
	// The main thread can execute lists inline without entering either worker Run().
	if( setJobFpState )
	{
		expectedJobFpState = control;
	}
#endif
#endif
}

static bool InitThreadJobFpState( const char* threadName )
{
#if defined(USE_INTRINSICS_SSE)
	const unsigned int control = jobFpState.load( std::memory_order_acquire );
	if( control == JOB_FP_STATE_UNCAPTURED )
	{
		return false;
	}
	const unsigned int previous = _mm_getcsr();
	if( setJobFpState )
	{
		_mm_setcsr( control | ( previous & JOB_FP_STATUS_MASK ) );
#if defined( _DEBUG )
		expectedJobFpState = control;
#endif
	}
	if( ( previous & ~JOB_FP_STATUS_MASK ) != control )
	{
#if defined( _DEBUG )
		idLib::Printf( "Job FP controls %s: thread=0x%08x main=0x%08x\n", threadName, previous & ~JOB_FP_STATUS_MASK, control );
#else
		common->DPrintf( "Job FP controls %s: thread=0x%08x main=0x%08x\n", threadName, previous & ~JOB_FP_STATUS_MASK, control );
#endif
	}
#else
	( void )threadName;
#endif
	return true;
}

void InitGameThreadFpState()
{
	// Run() also executes inline on main; keep the latch local to the actual OS thread.
	static thread_local bool initialized = false;
	if( !initialized )
	{
		initialized = InitThreadJobFpState( "Game/Draw" );
	}
}
//puncho end

/*
================================================================================================

	Job and Job-List names

================================================================================================
*/

//puncho - C1a: list ids are sparse; every valid id needs a positional name.
const char* jobNames[] =
{
	ASSERT_ENUM_STRING( JOBLIST_RENDERER_FRONTEND,	0 ),
	ASSERT_ENUM_STRING( JOBLIST_RENDERER_BACKEND,	1 ),
	"joblist2", "joblist3", "joblist4", "joblist5", "joblist6", "joblist7", "joblist8",
	ASSERT_ENUM_STRING( JOBLIST_UTILITY,			9 ),
	"joblist10", "joblist11", "joblist12", "joblist13", "joblist14", "joblist15",
	"joblist16", "joblist17", "joblist18", "joblist19", "joblist20", "joblist21", "joblist22", "joblist23",
	"joblist24", "joblist25", "joblist26", "joblist27", "joblist28", "joblist29", "joblist30", "joblist31"
};
compile_time_assert( sizeof( jobNames ) / sizeof( jobNames[0] ) == MAX_JOBLISTS );

static const int MAX_REGISTERED_JOBS = 128;
struct registeredJob
{
	jobRun_t		function;
	const char* 	name;
} registeredJobs[MAX_REGISTERED_JOBS];
static int numRegisteredJobs;

const char* GetJobListName( jobListId_t id )
{
	return jobNames[id];
}

/*
========================
IsRegisteredJob
========================
*/
static bool IsRegisteredJob( jobRun_t function )
{
	for( int i = 0; i < numRegisteredJobs; i++ )
	{
		if( registeredJobs[i].function == function )
		{
			return true;
		}
	}
	return false;
}

/*
========================
RegisterJob
========================
*/
void RegisterJob( jobRun_t function, const char* name )
{
	if( IsRegisteredJob( function ) )
	{
		return;
	}
	//puncho - C1a: registration is an init-time limit, not an unchecked array write.
	if( numRegisteredJobs >= MAX_REGISTERED_JOBS )
	{
		common->FatalError( "RegisterJob: more than %d job functions", MAX_REGISTERED_JOBS );
		return;
	}
	registeredJobs[numRegisteredJobs].function = function;
	registeredJobs[numRegisteredJobs].name = name;
	numRegisteredJobs++;
}

/*
========================
GetJobName
========================
*/
const char* GetJobName( jobRun_t function )
{
	for( int i = 0; i < numRegisteredJobs; i++ )
	{
		if( registeredJobs[i].function == function )
		{
			return registeredJobs[i].name;
		}
	}
	return "unknown";
}

/*
========================
idParallelJobRegistration::idParallelJobRegistration
========================
*/
idParallelJobRegistration::idParallelJobRegistration( jobRun_t function, const char* name )
{
	RegisterJob( function, name );
}

int globalSpuLocalStoreActive;
void* globalSpuLocalStoreStart;
void* globalSpuLocalStoreEnd;
idSysMutex globalSpuLocalStoreMutex;

/*
================================================================================================

	PS3

================================================================================================
*/


/*
================================================================================================

idParallelJobList_Threads

================================================================================================
*/

static idCVar jobs_longJobMicroSec( "jobs_longJobMicroSec", "10000", CVAR_INTEGER, "print a warning for jobs that take more than this number of microseconds" );


//puncho - C1a: copying is only used by idList growth while the list is unsubmitted.
// Keep the existing RMW ordering, with acquire/release publication for plain accesses.
struct jobListCounter_t
{
	jobListCounter_t() : value( 0 ) {}
	jobListCounter_t( const jobListCounter_t& other ) : value( other.GetValue() ) {}
	jobListCounter_t& operator=( const jobListCounter_t& other )
	{
		SetValue( other.GetValue() );
		return *this;
	}
	int Increment()
	{
		return ++value;
	}
	int Decrement()
	{
		return --value;
	}
	int GetValue() const
	{
		return value.load( std::memory_order_acquire );
	}
	void SetValue( int v )
	{
		value.store( v, std::memory_order_release );
	}
private:
	std::atomic<int> value;
};
//puncho - C1a: unsupported atomic implementations must not hide a lock per job.
compile_time_assert( std::atomic<int>::is_always_lock_free );
compile_time_assert( std::atomic<unsigned int>::is_always_lock_free );
compile_time_assert( std::atomic<uint64>::is_always_lock_free );

#if defined( _DEBUG )
static idCVar jobs_warnOverrun( "jobs_warnOverrun", "1", CVAR_BOOL, "developer warning on the first job-list reservation overrun per map" );
#else
static idCVar jobs_warnOverrun( "jobs_warnOverrun", "0", CVAR_BOOL, "developer warning on the first job-list reservation overrun per map" );
#endif
//puncho end

const static int		MAX_THREADS	= 32;

struct threadJobListState_t
{
	threadJobListState_t() :
		jobList( NULL ),
		version( 0xFFFFFFFF ),
		signalIndex( 0 ),
		lastJobIndex( 0 ),
		nextJobIndex( -1 ) {}
	threadJobListState_t( int _version ) :
		jobList( NULL ),
		version( _version ),
		signalIndex( 0 ),
		lastJobIndex( 0 ),
		nextJobIndex( -1 ) {}
	idParallelJobList_Threads* 	jobList;
	int							version;
	int							signalIndex;
	int							lastJobIndex;
	int							nextJobIndex;
};

struct threadStats_t
{
	unsigned int	numExecutedJobs;
	unsigned int	numExecutedSyncs;
	uint64			submitTime;
	uint64			startTime;
	uint64			endTime;
	uint64			waitTime;
	uint64			threadExecTime[MAX_THREADS];
	uint64			threadTotalTime[MAX_THREADS];
};

class idParallelJobList_Threads
{
public:
	idParallelJobList_Threads( jobListId_t id, jobListPriority_t priority, unsigned int maxJobs, unsigned int maxSyncs );
	~idParallelJobList_Threads();

	//------------------------
	// These are called from the one thread that manages this list.
	//------------------------
	ID_INLINE void			AddJob( jobRun_t function, void* data );
	ID_INLINE void			InsertSyncPoint( jobSyncType_t syncType );
	void					Submit( int parallelism ); //puncho - C1a: no cross-list dependencies
	void					Wait();
	bool					TryWait();
	bool					IsSubmitted() const;

	unsigned int			GetNumExecutedJobs() const
	{
		return threadStats.numExecutedJobs;
	}
	unsigned int			GetNumSyncs() const
	{
		return threadStats.numExecutedSyncs;
	}
	uint64					GetSubmitTimeMicroSec() const
	{
		return threadStats.submitTime;
	}
	uint64					GetStartTimeMicroSec() const
	{
		return threadStats.startTime;
	}
	uint64					GetFinishTimeMicroSec() const
	{
		return threadStats.endTime;
	}
	uint64					GetWaitTimeMicroSec() const
	{
		return threadStats.waitTime;
	}
	uint64					GetTotalProcessingTimeMicroSec() const;
	uint64					GetTotalWastedTimeMicroSec() const;
	uint64					GetUnitProcessingTimeMicroSec( int unit ) const;
	uint64					GetUnitWastedTimeMicroSec( int unit ) const;

	jobListId_t				GetId() const
	{
		return listId;
	}
	jobListPriority_t		GetPriority() const
	{
		return listPriority;
	}
	int						GetVersion()
	{
		return version.GetValue();
	}

	//puncho - G1/P0: owner-thread access; frame reads/resets require completed lists.
	int						GetNumRealJobs() const
	{
		return jobList.Num() - 2 * numSyncs - 1;
	}
	void					ReadAndClearFrameStats( jobListFrameStats_t& stats )
	{
		assert( done );
		stats = frameStats;
		memset( &frameStats, 0, sizeof( frameStats ) );
	}
	void					ResetOverrunWarning()
	{
		assert( done );
		warnedOverrun = false;
		pendingOverrunJobs = 0; //puncho - RB2-02: discard any diagnostic from the previous map
	}

	//------------------------
	// This is thread safe and called from the job threads.
	//------------------------
	enum runResult_t
	{
		RUN_OK			= 0,
		RUN_PROGRESS	= BIT( 0 ),
		RUN_DONE		= BIT( 1 ),
		RUN_STALLED		= BIT( 2 )
	};

	int						RunJobs( unsigned int threadNum, threadJobListState_t& state, bool singleJob );

	//puncho start - L-D1 (E-R4-4): a job's error, kept by RunJobsInternal; read after Wait, when no job runs
	bool					HasError() const
	{
		return errorTaken.GetValue() > 0;
	}
	idStr					TakeError()
	{
		const idStr text = errorText;
		ClearError();
		return text;
	}
	void					ClearError()
	{
		errorTaken.SetValue( 0 );
		errorText[0] = '\0';
	}
	//puncho end

private:
	bool					threaded;
	bool					done;
	bool					hasSignal;
	bool					warnedOverrun; //puncho - C1a: reset by the map owner
	jobListId_t				listId;
	jobListPriority_t		listPriority;
	unsigned int			maxJobs;
	unsigned int			maxSyncs;
	unsigned int			numSyncs;
	unsigned int			pendingOverrunJobs; //puncho - RB2-02: id/reservation stay in listId/maxJobs
	int						lastSignalJob;
	jobListCounter_t			version; //puncho - C1a
	struct job_t
	{
		jobRun_t	function;
		void* 		data;
		int			executed;
	};
	idList< job_t, TAG_JOBLIST >		jobList;
	//puncho - C1a: acquire/release counters replace the plain interlocked-wrapper accesses.
	idList< jobListCounter_t, TAG_JOBLIST >	signalJobCount;
	jobListCounter_t					currentJob;
	jobListCounter_t					fetchLock;
	jobListCounter_t					numThreadsExecuting;

	std::atomic<uint64>				startTime; //puncho - C1a: elected once, copied after retirement
	jobListFrameStats_t				frameStats; //puncho - P0
	threadStats_t						deferredThreadStats;
	threadStats_t						threadStats;
	//puncho - L-D1 (E-R4-4): the first error a job of this submission raised (errorTaken above 0), for the list's owner
	jobListCounter_t					errorTaken;
	char								errorText[idException::MAX_ERROR_LEN];

	int						RunJobsInternal( unsigned int threadNum, threadJobListState_t& state, bool singleJob );

	static void				Nop( void* data ) {}

	static int				JOB_SIGNAL;
	static int				JOB_SYNCHRONIZE;
	static int				JOB_LIST_DONE;
};

int idParallelJobList_Threads::JOB_SIGNAL;
int idParallelJobList_Threads::JOB_SYNCHRONIZE;
int idParallelJobList_Threads::JOB_LIST_DONE;

//puncho - L-D1 (E-R4-4): how deep this thread is inside RunJobs (a job may run a job list of its own). While it is above
// 0 idCommonLocal::Error raises nothing; it throws to the job's boundary in RunJobsInternal
static thread_local int jobDepth = 0;

/*
========================
idParallelJobList_Threads::idParallelJobList_Threads
========================
*/
idParallelJobList_Threads::idParallelJobList_Threads( jobListId_t id, jobListPriority_t priority, unsigned int maxJobs, unsigned int maxSyncs ) :
	threaded( true ),
	done( true ),
	hasSignal( false ),
	warnedOverrun( false ), //puncho - C1a
	listId( id ),
	listPriority( priority ),
	numSyncs( 0 ),
	pendingOverrunJobs( 0 ), //puncho - RB2-02
	lastSignalJob( 0 ),
	jobList(),
	startTime( 0 ) //puncho - C1a
{

	assert( listPriority != JOBLIST_PRIORITY_NONE );

	this->maxJobs = maxJobs;
	this->maxSyncs = maxSyncs;
	jobList.AssureSize( maxJobs + maxSyncs * 2 + 1 );	// syncs go in as dummy jobs and one more to update the doneCount
	jobList.SetNum( 0 );
	signalJobCount.AssureSize( maxSyncs + 1 );			// need one extra for submit
	signalJobCount.SetNum( 0 );

	memset( &deferredThreadStats, 0, sizeof( threadStats_t ) );
	memset( &threadStats, 0, sizeof( threadStats_t ) );
	memset( &frameStats, 0, sizeof( frameStats ) ); //puncho - P0
}

/*
========================
idParallelJobList_Threads::~idParallelJobList_Threads
========================
*/
idParallelJobList_Threads::~idParallelJobList_Threads()
{
	Wait();
}

/*
========================
idParallelJobList_Threads::AddJob
========================
*/
ID_INLINE void idParallelJobList_Threads::AddJob( jobRun_t function, void* data )
{
	assert( done );
#if defined( _DEBUG )
	// make sure there isn't already a job with the same function and data in the list
	if( jobList.Num() < 1000 )  	// don't do this N^2 slow check on big lists
	{
		for( int i = 0; i < jobList.Num(); i++ )
		{
			assert( jobList[i].function != function || jobList[i].data != data );
		}
	}
#endif
	if( 1 )    // JDC: this never worked in tech5!  !jobList.IsFull() ) {
	{
		job_t& job = jobList.Alloc();
		job.function = function;
		job.data = data;
		job.executed = 0;
	}
	else
	{
		// debug output to show us what is overflowing
		int currentJobCount[MAX_REGISTERED_JOBS] = {};

		for( int i = 0; i < jobList.Num(); ++i )
		{
			const char* jobName = GetJobName( jobList[ i ].function );
			for( int j = 0; j < numRegisteredJobs; ++j )
			{
				if( jobName == registeredJobs[ j ].name )
				{
					currentJobCount[ j ]++;
					break;
				}
			}
		}

		// print the quantity of each job type
		for( int i = 0; i < numRegisteredJobs; ++i )
		{
			if( currentJobCount[ i ] > 0 )
			{
				idLib::Printf( "Job: %s, # %d", registeredJobs[ i ].name, currentJobCount[ i ] );
			}
		}
		idLib::Error( "Can't add job '%s', too many jobs %d", GetJobName( function ), jobList.Num() );
	}
}

/*
========================
idParallelJobList_Threads::InsertSyncPoint
========================
*/
ID_INLINE void idParallelJobList_Threads::InsertSyncPoint( jobSyncType_t syncType )
{
	assert( done );
	switch( syncType )
	{
		case SYNC_SIGNAL:
		{
			assert( !hasSignal );
			if( jobList.Num() )
			{
				assert( !hasSignal );
				signalJobCount.Alloc();
				signalJobCount[signalJobCount.Num() - 1].SetValue( jobList.Num() - lastSignalJob );
				lastSignalJob = jobList.Num();
				job_t& job = jobList.Alloc();
				job.function = Nop;
				job.data = & JOB_SIGNAL;
				hasSignal = true;
			}
			break;
		}
		case SYNC_SYNCHRONIZE:
		{
			if( hasSignal )
			{
				job_t& job = jobList.Alloc();
				job.function = Nop;
				job.data = & JOB_SYNCHRONIZE;
				hasSignal = false;
				numSyncs++;
			}
			break;
		}
	}
}

/*
========================
idParallelJobList_Threads::Submit
========================
*/
void idParallelJobList_Threads::Submit( int parallelism ) //puncho - C1a
{
	assert( done );
	assert( numSyncs <= maxSyncs );
	//puncho - C1a: maxJobs is a reservation; AddJob may grow in every build.
	assert( fetchLock.GetValue() == 0 );

	done = false;
	currentJob.SetValue( 0 );
	ClearError();	//puncho - L-D1 (E-R4-4): a new submission starts without an error

	memset( &deferredThreadStats, 0, sizeof( deferredThreadStats ) );
	deferredThreadStats.numExecutedJobs = jobList.Num() - numSyncs * 2;
	deferredThreadStats.numExecutedSyncs = numSyncs;
	deferredThreadStats.submitTime = Sys_Microseconds();
	deferredThreadStats.startTime = 0;
	deferredThreadStats.endTime = 0;
	deferredThreadStats.waitTime = 0;
	//puncho - C1a/P0: reuse existing submission figures; no extra timer read.
	startTime.store( 0, std::memory_order_release );
	if( !warnedOverrun && deferredThreadStats.numExecutedJobs > maxJobs && jobs_warnOverrun.GetBool() )
	{
		warnedOverrun = true;
		//puncho - RB2-02: printing here could redraw into this unfinished submission.
		pendingOverrunJobs = deferredThreadStats.numExecutedJobs;
	}
	if( parallelJobManager->GetCollectFrameStats() )
	{
		frameStats.jobs += deferredThreadStats.numExecutedJobs;
		frameStats.submits++;
	}

	if( jobList.Num() == 0 )
	{
		return;
	}

	signalJobCount.Alloc();
	signalJobCount[signalJobCount.Num() - 1].SetValue( jobList.Num() - lastSignalJob );

	job_t& job = jobList.Alloc();
	job.function = Nop;
	job.data = & JOB_LIST_DONE;

	if( threaded )
	{
		// hand over to the manager
		void SubmitJobList( idParallelJobList_Threads * jobList, int parallelism );
		SubmitJobList( this, parallelism );
	}
	else
	{
		// run all the jobs right here
		threadJobListState_t state( GetVersion() );
		RunJobs( 0, state, false );
	}
}

/*
========================
idParallelJobList_Threads::Wait
========================
*/
void idParallelJobList_Threads::Wait()
{
	if( jobList.Num() > 0 )
	{
		// don't lock up but return if the job list was never properly submitted
		if( !verify( !done && signalJobCount.Num() > 0 ) )
		{
			return;
		}

		bool waited = false;
		uint64 waitStart = Sys_Microseconds();

		while( signalJobCount[signalJobCount.Num() - 1].GetValue() > 0 )
		{
			Sys_Yield();
			waited = true;
		}
		version.Increment();
		while( numThreadsExecuting.GetValue() > 0 )
		{
			Sys_Yield();
			waited = true;
		}

		jobList.SetNum( 0 );
		signalJobCount.SetNum( 0 );
		numSyncs = 0;
		lastSignalJob = 0;

		uint64 waitEnd = Sys_Microseconds();
		deferredThreadStats.waitTime = waited ? ( waitEnd - waitStart ) : 0;
	}
	//puncho - C1a: every accepted visitor has published its statistics before the drain.
	deferredThreadStats.startTime = startTime.load( std::memory_order_acquire );
	memcpy( & threadStats, & deferredThreadStats, sizeof( threadStats ) );
	//puncho - P0: repeated Wait/TryWait calls must not count a submission again.
	if( parallelJobManager->GetCollectFrameStats() )
	{
		if( !done )
		{
			frameStats.execUs += GetTotalProcessingTimeMicroSec();
			frameStats.wallUs += threadStats.endTime > 0 ? threadStats.endTime - threadStats.submitTime : 0;
			frameStats.waitUs += threadStats.waitTime;
		}
	}
	done = true;
	//puncho - RB2-02: the owner has retired the list and finished all statistics writes.
	// DPrintf suppresses main-thread refresh; off-main output uses the existing print relay.
	if( pendingOverrunJobs != 0 )
	{
		const unsigned int overrunJobs = pendingOverrunJobs;
		pendingOverrunJobs = 0;
		common->DPrintf( "WARNING: Job list %d exceeded reservation %u with %u jobs\n", listId, maxJobs, overrunJobs );
	}
}

/*
========================
idParallelJobList_Threads::TryWait
========================
*/
bool idParallelJobList_Threads::TryWait()
{
	if( jobList.Num() == 0 || signalJobCount[signalJobCount.Num() - 1].GetValue() <= 0 )
	{
		Wait();
		return true;
	}
	return false;
}

/*
========================
idParallelJobList_Threads::IsSubmitted
========================
*/
bool idParallelJobList_Threads::IsSubmitted() const
{
	return !done;
}

/*
========================
idParallelJobList_Threads::GetTotalProcessingTimeMicroSec
========================
*/
uint64 idParallelJobList_Threads::GetTotalProcessingTimeMicroSec() const
{
	uint64 total = 0;
	for( int unit = 0; unit < MAX_THREADS; unit++ )
	{
		total += threadStats.threadExecTime[unit];
	}
	return total;
}

/*
========================
idParallelJobList_Threads::GetTotalWastedTimeMicroSec
========================
*/
uint64 idParallelJobList_Threads::GetTotalWastedTimeMicroSec() const
{
	uint64 total = 0;
	for( int unit = 0; unit < MAX_THREADS; unit++ )
	{
		total += threadStats.threadTotalTime[unit] - threadStats.threadExecTime[unit];
	}
	return total;
}

/*
========================
idParallelJobList_Threads::GetUnitProcessingTimeMicroSec
========================
*/
uint64 idParallelJobList_Threads::GetUnitProcessingTimeMicroSec( int unit ) const
{
	if( unit < 0 || unit >= MAX_THREADS )
	{
		return 0;
	}
	return threadStats.threadExecTime[unit];
}

/*
========================
idParallelJobList_Threads::GetUnitWastedTimeMicroSec
========================
*/
uint64 idParallelJobList_Threads::GetUnitWastedTimeMicroSec( int unit ) const
{
	if( unit < 0 || unit >= MAX_THREADS )
	{
		return 0;
	}
	return threadStats.threadTotalTime[unit] - threadStats.threadExecTime[unit];
}

/*
========================
idParallelJobList_Threads::RunJobsInternal
========================
*/
int idParallelJobList_Threads::RunJobsInternal( unsigned int threadNum, threadJobListState_t& state, bool singleJob )
{
	assert( threadNum < MAX_THREADS );

	//puncho - C1a: only the winning visitor publishes the first execution time.
	if( startTime.load( std::memory_order_acquire ) == 0 )
	{
		uint64 expected = 0;
		startTime.compare_exchange_strong( expected, Sys_Microseconds(), std::memory_order_acq_rel );
	}

	int result = RUN_OK;

	do
	{

		// run through all signals and syncs before the last job that has been or is being executed
		// this loop is really an optimization to minimize the time spent in the fetchLock section below
		for( ; state.lastJobIndex < ( int ) currentJob.GetValue() && state.lastJobIndex < jobList.Num(); state.lastJobIndex++ )
		{
			if( jobList[state.lastJobIndex].data == & JOB_SIGNAL )
			{
				state.signalIndex++;
				assert( state.signalIndex < signalJobCount.Num() );
			}
			else if( jobList[state.lastJobIndex].data == & JOB_SYNCHRONIZE )
			{
				assert( state.signalIndex > 0 );
				if( signalJobCount[state.signalIndex - 1].GetValue() > 0 )
				{
					// stalled on a synchronization point
					return ( result | RUN_STALLED );
				}
			}
			else if( jobList[state.lastJobIndex].data == & JOB_LIST_DONE )
			{
				if( signalJobCount[signalJobCount.Num() - 1].GetValue() > 0 )
				{
					// stalled on a synchronization point
					return ( result | RUN_STALLED );
				}
			}
		}

		// try to lock to fetch a new job
		if( fetchLock.Increment() == 1 )
		{

			// grab a new job
			state.nextJobIndex = currentJob.Increment() - 1;

			// run through any remaining signals and syncs (this should rarely iterate more than once)
			for( ; state.lastJobIndex <= state.nextJobIndex && state.lastJobIndex < jobList.Num(); state.lastJobIndex++ )
			{
				if( jobList[state.lastJobIndex].data == & JOB_SIGNAL )
				{
					state.signalIndex++;
					assert( state.signalIndex < signalJobCount.Num() );
				}
				else if( jobList[state.lastJobIndex].data == & JOB_SYNCHRONIZE )
				{
					assert( state.signalIndex > 0 );
					if( signalJobCount[state.signalIndex - 1].GetValue() > 0 )
					{
						// return this job to the list
						currentJob.Decrement();
						// release the fetch lock
						fetchLock.Decrement();
						// stalled on a synchronization point
						return ( result | RUN_STALLED );
					}
				}
				else if( jobList[state.lastJobIndex].data == & JOB_LIST_DONE )
				{
					if( signalJobCount[signalJobCount.Num() - 1].GetValue() > 0 )
					{
						// return this job to the list
						currentJob.Decrement();
						// release the fetch lock
						fetchLock.Decrement();
						// stalled on a synchronization point
						return ( result | RUN_STALLED );
					}
				}
			}
			// release the fetch lock
			fetchLock.Decrement();
		}
		else
		{
			// release the fetch lock
			fetchLock.Decrement();
			// another thread is fetching right now so consider stalled
			return ( result | RUN_STALLED );
		}

		// if at the end of the job list we're done
		if( state.nextJobIndex >= jobList.Num() )
		{
			return ( result | RUN_DONE );
		}

		// execute the next job
		{
#if defined( _DEBUG ) && defined(USE_INTRINSICS_SSE)
			//puncho - C1a: status bits may change during arithmetic; controls must not drift.
			if( expectedJobFpState != JOB_FP_STATE_UNCAPTURED )
			{
				assert( ( _mm_getcsr() & ~JOB_FP_STATUS_MASK ) == expectedJobFpState );
			}
#endif
			uint64 jobStart = Sys_Microseconds();

			//puncho start - L-D1 (E-R4-4, E-TDM-1): an error raised in a job ended the process (idlib/Thread.cpp:257) or, on a
			// thread that runs a list's jobs itself, unwound through this loop and left the list's counters behind. The first
			// error of a submission is kept for the list's owner, which raises it again after Wait (idParallelJobList::Wait);
			// the job counts as run, so the counters move on and Wait returns
			try
			{
				jobList[state.nextJobIndex].function( jobList[state.nextJobIndex].data );
			}
			catch( idException& ex )
			{
				if( errorTaken.Increment() == 1 )
				{
					idStr::Copynz( errorText, ex.GetError(), sizeof( errorText ) );
				}
			}
			//puncho end
			jobList[state.nextJobIndex].executed = 1;

			uint64 jobEnd = Sys_Microseconds();
			deferredThreadStats.threadExecTime[threadNum] += jobEnd - jobStart;

#ifndef _DEBUG
			if( jobs_longJobMicroSec.GetInteger() > 0 )
			{
				if( jobEnd - jobStart > jobs_longJobMicroSec.GetInteger()
						&& GetId() != JOBLIST_UTILITY )
				{
					//puncho - C1a: diagnostics belong to this callback, not shared globals.
					const float longJobTime = ( jobEnd - jobStart ) * ( 1.0f / 1000.0f );
					const jobRun_t longJobFunc = jobList[state.nextJobIndex].function;
					void* const longJobData = jobList[state.nextJobIndex].data;
					( void )longJobData;
					const char* jobName = GetJobName( longJobFunc );
					const char* jobListName = GetJobListName( GetId() );
					idLib::Printf( "%1.1f milliseconds for a single '%s' job from job list %s on thread %d\n", longJobTime, jobName, jobListName, threadNum );
				}
			}
#endif
		}

		result |= RUN_PROGRESS;

		// decrease the job count for the current signal
		if( signalJobCount[state.signalIndex].Decrement() == 0 )
		{
			// if this was the very last job of the job list
			if( state.signalIndex == signalJobCount.Num() - 1 )
			{
				deferredThreadStats.endTime = Sys_Microseconds();
				return ( result | RUN_DONE );
			}
		}

	}
	while( ! singleJob );

	return result;
}

/*
========================
idParallelJobList_Threads::RunJobs
========================
*/
int idParallelJobList_Threads::RunJobs( unsigned int threadNum, threadJobListState_t& state, bool singleJob )
{
	uint64 start = Sys_Microseconds();

	numThreadsExecuting.Increment();

	//puncho - C1a: count before validating; stale visitors may not touch statistics.
	if( state.version != version.GetValue() )
	{
		numThreadsExecuting.Decrement();
		return RUN_DONE;
	}

	jobDepth++;	//puncho - L-D1 (E-R4-4): idCommonLocal::Error throws to the job's boundary while this is above 0
	int result = RunJobsInternal( threadNum, state, singleJob );
	jobDepth--;

	deferredThreadStats.threadTotalTime[threadNum] += Sys_Microseconds() - start;
	//puncho - C1a: publish every write before Wait can observe retirement.
	numThreadsExecuting.Decrement();

	return result;
}

/*
================================================================================================

idParallelJobList

================================================================================================
*/

/*
========================
idParallelJobList::idParallelJobList
========================
*/
idParallelJobList::idParallelJobList( jobListId_t id, jobListPriority_t priority, unsigned int maxJobs, unsigned int maxSyncs, const idColor* color )
{
	assert( priority > JOBLIST_PRIORITY_NONE );
	this->jobListThreads = new( TAG_JOBLIST ) idParallelJobList_Threads( id, priority, maxJobs, maxSyncs );
	this->color = color;
}

/*
========================
idParallelJobList::~idParallelJobList
========================
*/
idParallelJobList::~idParallelJobList()
{
	delete jobListThreads;
}

/*
========================
idParallelJobList::AddJob
========================
*/
void idParallelJobList::AddJob( jobRun_t function, void* data )
{
	assert( IsRegisteredJob( function ) );
	jobListThreads->AddJob( function, data );
}

/*
========================
idParallelJobList::AddJobSPURS
========================
*/
CellSpursJob128* idParallelJobList::AddJobSPURS()
{
	return NULL;
}

/*
========================
idParallelJobList::InsertSyncPoint
========================
*/
void idParallelJobList::InsertSyncPoint( jobSyncType_t syncType )
{
	jobListThreads->InsertSyncPoint( syncType );
}

/*
========================
idParallelJobList::Wait
========================
*/
void idParallelJobList::Wait()
{
	if( jobListThreads != NULL )
	{
		jobListThreads->Wait();
		//puncho start - L-D1 (E-R4-4): a job of this submission raised an error; it is raised again here, on the list's
		// owner. On the main thread idCommonLocal::Error stops the game, so every job list is done first; on another thread,
		// or inside a job, it throws on up to that thread's boundary
		if( jobListThreads->HasError() )
		{
			const idStr text = jobListThreads->TakeError();
			if( idLib::IsMainThread() && !InsideJob() )
			{
				parallelJobManager->WaitForAllJobLists();
			}
			idLib::Error( "%s", text.c_str() );
		}
		//puncho end
	}
}

/*
========================
idParallelJobList::InsideJob

L-D1 (E-R4-4): true while this thread runs a job (RunJobs)
========================
*/
bool idParallelJobList::InsideJob()
{
	return jobDepth > 0;
}

/*
========================
idParallelJobList::WaitAndDropError

L-D1 (E-R4-4): waits as Wait does and drops an error a job kept, without raising it: for a list that is freed or quieted
after an error, where an exception cannot pass
========================
*/
void idParallelJobList::WaitAndDropError()
{
	if( jobListThreads != NULL )
	{
		jobListThreads->Wait();
		jobListThreads->ClearError();
	}
}

/*
========================
idParallelJobList::TryWait
========================
*/
bool idParallelJobList::TryWait()
{
	bool done = true;
	if( jobListThreads != NULL )
	{
		done &= jobListThreads->TryWait();
	}
	return done;
}

/*
========================
idParallelJobList::Submit
========================
*/
void idParallelJobList::Submit( idParallelJobList* waitForJobList, int parallelism )
{
	//puncho - C1a: retain source compatibility, with no cross-list dependency state.
	assert( waitForJobList == NULL );
	( void )waitForJobList;
	jobListThreads->Submit( parallelism );
}

/*
========================
idParallelJobList::IsSubmitted
========================
*/
bool idParallelJobList::IsSubmitted() const
{
	return jobListThreads->IsSubmitted();
}

/*
========================
idParallelJobList::GetNumExecutedJobs
========================
*/
unsigned int idParallelJobList::GetNumExecutedJobs() const
{
	return jobListThreads->GetNumExecutedJobs();
}

/*
========================
idParallelJobList::GetNumSyncs
========================
*/
unsigned int idParallelJobList::GetNumSyncs() const
{
	return jobListThreads->GetNumSyncs();
}

/*
========================
idParallelJobList::GetSubmitTimeMicroSec
========================
*/
uint64 idParallelJobList::GetSubmitTimeMicroSec() const
{
	return jobListThreads->GetSubmitTimeMicroSec();
}

/*
========================
idParallelJobList::GetStartTimeMicroSec
========================
*/
uint64 idParallelJobList::GetStartTimeMicroSec() const
{
	return jobListThreads->GetStartTimeMicroSec();
}

/*
========================
idParallelJobList::GetFinishTimeMicroSec
========================
*/
uint64 idParallelJobList::GetFinishTimeMicroSec() const
{
	return jobListThreads->GetFinishTimeMicroSec();
}

/*
========================
idParallelJobList::GetWaitTimeMicroSec
========================
*/
uint64 idParallelJobList::GetWaitTimeMicroSec() const
{
	return jobListThreads->GetWaitTimeMicroSec();
}

/*
========================
idParallelJobList::GetTotalProcessingTimeMicroSec
========================
*/
uint64 idParallelJobList::GetTotalProcessingTimeMicroSec() const
{
	return jobListThreads->GetTotalProcessingTimeMicroSec();
}

/*
========================
idParallelJobList::GetTotalWastedTimeMicroSec
========================
*/
uint64 idParallelJobList::GetTotalWastedTimeMicroSec() const
{
	return jobListThreads->GetTotalWastedTimeMicroSec();
}

/*
========================
idParallelJobList::GetUnitProcessingTimeMicroSec
========================
*/
uint64 idParallelJobList::GetUnitProcessingTimeMicroSec( int unit ) const
{
	return jobListThreads->GetUnitProcessingTimeMicroSec( unit );
}

/*
========================
idParallelJobList::GetUnitWastedTimeMicroSec
========================
*/
uint64 idParallelJobList::GetUnitWastedTimeMicroSec( int unit ) const
{
	return jobListThreads->GetUnitWastedTimeMicroSec( unit );
}

/*
========================
idParallelJobList::GetId
========================
*/
jobListId_t idParallelJobList::GetId() const
{
	return jobListThreads->GetId();
}

/*
================================================================================================

idJobThread

================================================================================================
*/

const int JOB_THREAD_STACK_SIZE		= 256 * 1024;	// same size as the SPU local store

struct threadJobList_t
{
	idParallelJobList_Threads* 	jobList;
	int							version;
};

static idCVar jobs_prioritize( "jobs_prioritize", "1", CVAR_BOOL | CVAR_NOCHEAT, "prioritize job lists" );

class idJobThread : public idSysThread
{
public:
	idJobThread();
	~idJobThread();

	void						Start( core_t core, unsigned int threadNum );

	void						AddJobList( idParallelJobList_Threads* jobList );

private:
	threadJobList_t				jobLists[MAX_JOBLISTS];	// cyclic buffer with job lists
	//puncho - C1a: release/acquire in both directions protects ring-slot reuse.
	std::atomic<unsigned int>	firstJobList;			// index of the last job list the thread grabbed
	std::atomic<unsigned int>	lastJobList;			// index where the next job list to work on will be added
	idSysMutex					addJobMutex;

	unsigned int				threadNum;
	bool						fpStateInitialized; //puncho - C1a: one setup per OS thread

	virtual int					Run();
};

/*
========================
idJobThread::idJobThread
========================
*/
idJobThread::idJobThread() :
	firstJobList( 0 ),
	lastJobList( 0 ),
	threadNum( 0 ),
	fpStateInitialized( false ) //puncho - C1a
{
}

/*
========================
idJobThread::~idJobThread
========================
*/
idJobThread::~idJobThread()
{
}

/*
========================
idJobThread::Start
========================
*/
void idJobThread::Start( core_t core, unsigned int threadNum )
{
	this->threadNum = threadNum;
	fpStateInitialized = false; //puncho - C1a: a restarted OS thread needs its own setup
	// DG: change threadname from "JobListProcessor_%d" to "JLProc_%d", because Linux
	// has a 15 (+ \0) char limit for threadnames.
	// furthermore: va is not thread safe, use snPrintf instead
	char name[16];
	idStr::snPrintf( name, 16, "JLProc_%d", threadNum );
	StartWorkerThread( name, core, THREAD_NORMAL, JOB_THREAD_STACK_SIZE );
	// DG end
}

/*
========================
idJobThread::AddJobList
========================
*/
void idJobThread::AddJobList( idParallelJobList_Threads* jobList )
{
	// must lock because multiple threads may try to add new job lists at the same time
	addJobMutex.Lock();
	// wait until there is space available because in rare cases multiple versions of the same job lists may still be queued
	//puncho - C1a: producers are serialized by addJobMutex; publish the complete slot.
	const unsigned int last = lastJobList.load( std::memory_order_relaxed );
	while( last - firstJobList.load( std::memory_order_acquire ) >= MAX_JOBLISTS )
	{
		Sys_Yield();
	}
	assert( last - firstJobList.load( std::memory_order_acquire ) < MAX_JOBLISTS );
	jobLists[last & ( MAX_JOBLISTS - 1 )].jobList = jobList;
	jobLists[last & ( MAX_JOBLISTS - 1 )].version = jobList->GetVersion();
	lastJobList.store( last + 1, std::memory_order_release );
	addJobMutex.Unlock();
}

/*
========================
idJobThread::Run
========================
*/
int idJobThread::Run()
{
	//puncho - C1a: later wakes only test this bool; pre-capture wakes retry next time.
	if( !fpStateInitialized )
	{
		fpStateInitialized = InitThreadJobFpState( GetName() );
	}

	threadJobListState_t threadJobListState[MAX_JOBLISTS];
	int numJobLists = 0;
	int lastStalledJobList = -1;

	while( !IsTerminating() )
	{

		// fetch any new job lists and add them to the local list
		//puncho - C1a: acquire the slot, then release it only after copying its contents.
		const unsigned int first = firstJobList.load( std::memory_order_relaxed );
		if( numJobLists < MAX_JOBLISTS && first != lastJobList.load( std::memory_order_acquire ) )
		{
			threadJobListState[numJobLists].jobList = jobLists[first & ( MAX_JOBLISTS - 1 )].jobList;
			threadJobListState[numJobLists].version = jobLists[first & ( MAX_JOBLISTS - 1 )].version;
			threadJobListState[numJobLists].signalIndex = 0;
			threadJobListState[numJobLists].lastJobIndex = 0;
			threadJobListState[numJobLists].nextJobIndex = -1;
			numJobLists++;
			firstJobList.store( first + 1, std::memory_order_release );
		}
		if( numJobLists == 0 )
		{
			break;
		}

		int currentJobList = 0;
		jobListPriority_t priority = JOBLIST_PRIORITY_NONE;
		if( lastStalledJobList < 0 )
		{
			// find the job list with the highest priority
			for( int i = 0; i < numJobLists; i++ )
			{
				if( threadJobListState[i].jobList->GetPriority() > priority )
				{
					priority = threadJobListState[i].jobList->GetPriority();
					currentJobList = i;
				}
			}
		}
		else
		{
			// try to hide the stall with a job from a list that has equal or higher priority
			currentJobList = lastStalledJobList;
			priority = threadJobListState[lastStalledJobList].jobList->GetPriority();
			for( int i = 0; i < numJobLists; i++ )
			{
				if( i != lastStalledJobList && threadJobListState[i].jobList->GetPriority() >= priority )
				{
					priority = threadJobListState[i].jobList->GetPriority();
					currentJobList = i;
				}
			}
		}

		// if the priority is high then try to run through the whole list to reduce the overhead
		// otherwise run a single job and re-evaluate priorities for the next job
		bool singleJob = ( priority == JOBLIST_PRIORITY_HIGH ) ? false : jobs_prioritize.GetBool();

		// try running one or more jobs from the current job list
		int result = threadJobListState[currentJobList].jobList->RunJobs( threadNum, threadJobListState[currentJobList], singleJob );

		if( ( result & idParallelJobList_Threads::RUN_DONE ) != 0 )
		{
			// done with this job list so remove it from the local list
			for( int i = currentJobList; i < numJobLists - 1; i++ )
			{
				threadJobListState[i] = threadJobListState[i + 1];
			}
			numJobLists--;
			lastStalledJobList = -1;
		}
		else if( ( result & idParallelJobList_Threads::RUN_STALLED ) != 0 )
		{
			// yield when stalled on the same job list again without making any progress
			if( currentJobList == lastStalledJobList )
			{
				if( ( result & idParallelJobList_Threads::RUN_PROGRESS ) == 0 )
				{
					Sys_Yield();
				}
			}
			lastStalledJobList = currentJobList;
		}
		else
		{
			lastStalledJobList = -1;
		}
	}
	return 0;
}

/*
================================================================================================

idParallelJobManagerLocal

================================================================================================
*/

extern void Sys_CPUCount( int& logicalNum, int& coreNum, int& packageNum );


// WINDOWS LOGICAL PROCESSOR LIMITS:
//
// http://download.microsoft.com/download/5/7/7/577a5684-8a83-43ae-9272-ff260a9c20e2/Hyper-thread_Windows.doc
//
//											Physical	Logical (Cores + HT)
// Windows XP Home Edition					1			2
// Windows XP Professional					2			4
// Windows Server 2003, Standard Edition	4			8
// Windows Server 2003, Enterprise Edition	8			16
// Windows Server 2003, Datacenter Edition	32			32
//
// Windows Vista							?			?
//
// Windows 7 Starter						1			32/64
// Windows 7 Home Basic						1			32/64
// Windows 7 Professional					2			32/64
//
//
// Hyperthreading is not dead yet.  Intel's Core i7 Processor is quad-core with HT for 8 logicals.

// DOOM3: We don't have that many jobs, so just set this fairly low so we don't spin up a ton of idle threads
#define MAX_JOB_THREADS		32
#define NUM_JOB_THREADS		"2"
#define JOB_THREAD_CORES	{	CORE_ANY, CORE_ANY, CORE_ANY, CORE_ANY,	\
								CORE_ANY, CORE_ANY, CORE_ANY, CORE_ANY,	\
								CORE_ANY, CORE_ANY, CORE_ANY, CORE_ANY,	\
								CORE_ANY, CORE_ANY, CORE_ANY, CORE_ANY,	\
								CORE_ANY, CORE_ANY, CORE_ANY, CORE_ANY,	\
								CORE_ANY, CORE_ANY, CORE_ANY, CORE_ANY,	\
								CORE_ANY, CORE_ANY, CORE_ANY, CORE_ANY,	\
								CORE_ANY, CORE_ANY, CORE_ANY, CORE_ANY }


//puncho - G1: automatic is opt-in; retain the default of two workers.
idCVar jobs_numThreads( "jobs_numThreads", NUM_JOB_THREADS, CVAR_INTEGER | CVAR_NOCHEAT, "number of job workers (-1: physical cores minus two, minimum two)", -1, MAX_JOB_THREADS );

static int ResolveJobThreads( int requested, int physicalCores )
{
	if( requested >= 0 )
	{
		static bool warnedSingleWorker = false;
		if( requested == 1 && !warnedSingleWorker )
		{
			warnedSingleWorker = true;
			idLib::Warning( "jobs_numThreads 1 uses one worker; use 0 for inline execution" );
		}
		return idMath::ClampInt( 0, MAX_JOB_THREADS, requested );
	}
	return idMath::ClampInt( 2, MAX_JOB_THREADS, physicalCores - 2 );
}

class idParallelJobManagerLocal : public idParallelJobManager
{
public:
	virtual						~idParallelJobManagerLocal() {}

	virtual void				Init();
	virtual void				Shutdown();

	virtual idParallelJobList* 	AllocJobList( jobListId_t id, jobListPriority_t priority, unsigned int maxJobs, unsigned int maxSyncs, const idColor* color );
	virtual void				FreeJobList( idParallelJobList* jobList );

	virtual int					GetNumJobLists() const;
	virtual int					GetNumFreeJobLists() const;
	virtual idParallelJobList* 	GetJobList( int index );

	virtual int					GetNumProcessingUnits();
	virtual int					GetLogicalCpuCores() const	// RB
	{
		return numLogicalCpuCores;
	}

	virtual void				WaitForAllJobLists();
	//puncho - P0/C1a: read/reset only at quiescent frame/map boundaries.
	virtual void				ReadAndClearJobListFrameStats( int index, jobListFrameStats_t& stats );
	virtual void				ResetJobListOverrunWarnings();

	void						Submit( idParallelJobList_Threads* jobList, int parallelism );

private:
	idJobThread						threads[MAX_JOB_THREADS];
	unsigned int					maxThreads;
	int								numPhysicalCpuCores;
	int								numLogicalCpuCores;
	int								numCpuPackages;
	idStaticList< idParallelJobList*, MAX_JOBLISTS >	jobLists;
};

idParallelJobManagerLocal parallelJobManagerLocal;
idParallelJobManager* parallelJobManager = &parallelJobManagerLocal;

/*
========================
SubmitJobList
========================
*/
void SubmitJobList( idParallelJobList_Threads* jobList, int parallelism )
{
	parallelJobManagerLocal.Submit( jobList, parallelism );
}

/*
========================
idParallelJobManagerLocal::Init
========================
*/
void idParallelJobManagerLocal::Init()
{
#if defined(USE_INTRINSICS_SSE)
	//puncho - C1a: read the startup switch once, before any job thread is created.
	setJobFpState = jobs_fpState.GetBool();
#endif
	// on consoles this will have specific cores for the threads, but on PC they will all be CORE_ANY
	core_t cores[] = JOB_THREAD_CORES;
	assert( sizeof( cores ) / sizeof( cores[0] ) >= MAX_JOB_THREADS );

	for( int i = 0; i < MAX_JOB_THREADS; i++ )
	{
		threads[i].Start( cores[i], i );
	}
	Sys_CPUCount( numLogicalCpuCores, numPhysicalCpuCores, numCpuPackages );
	//puncho - G1: topology resolution is init/cvar-change work, never per job.
	maxThreads = ResolveJobThreads( jobs_numThreads.GetInteger(), numPhysicalCpuCores );
	jobs_numThreads.ClearModified();
}

/*
========================
idParallelJobManagerLocal::Shutdown
========================
*/
void idParallelJobManagerLocal::Shutdown()
{
	for( int i = 0; i < MAX_JOB_THREADS; i++ )
	{
		threads[i].StopThread();
	}
}

/*
========================
idParallelJobManagerLocal::AllocJobList
========================
*/
idParallelJobList* idParallelJobManagerLocal::AllocJobList( jobListId_t id, jobListPriority_t priority, unsigned int maxJobs, unsigned int maxSyncs, const idColor* color )
{
	for( int i = 0; i < jobLists.Num(); i++ )
	{
		if( jobLists[i]->GetId() == id )
		{
			// idStudio may cause job lists to be allocated multiple times
		}
	}
	//puncho - C1a: diagnose the capacity before idStaticList's debug assertion.
	if( jobLists.Num() >= jobLists.Max() )
	{
		idLib::FatalError( "AllocJobList: more than %d job lists", MAX_JOBLISTS );
	}
	idParallelJobList* jobList = new( TAG_JOBLIST ) idParallelJobList( id, priority, maxJobs, maxSyncs, color );
	if( jobLists.Append( jobList ) < 0 )
	{
		idLib::FatalError( "AllocJobList: could not append job list %d", id );
	}
	return jobList;
}

/*
========================
idParallelJobManagerLocal::FreeJobList
========================
*/
void idParallelJobManagerLocal::FreeJobList( idParallelJobList* jobList )
{
	if( jobList == NULL )
	{
		return;
	}
	//puncho - RB2-01: owners must be quiescent; explicit parallelism and old settings
	// can leave references on any worker. Idle workers already have their done event raised.
	for( int i = 0; i < MAX_JOB_THREADS; i++ )
	{
		threads[i].WaitForThread();
	}
	int index = jobLists.FindIndex( jobList );
	assert( index >= 0 && jobLists[index] == jobList );
	jobLists[index]->WaitAndDropError();	//puncho - L-D1 (E-R4-4): Wait may raise a kept error, and the list must still go
	delete jobLists[index];
	jobLists.RemoveIndexFast( index );
}

/*
========================
idParallelJobManagerLocal::GetNumJobLists
========================
*/
int idParallelJobManagerLocal::GetNumJobLists() const
{
	return jobLists.Num();
}

/*
========================
idParallelJobManagerLocal::GetNumFreeJobLists
========================
*/
int idParallelJobManagerLocal::GetNumFreeJobLists() const
{
	return MAX_JOBLISTS - jobLists.Num();
}

/*
========================
idParallelJobManagerLocal::GetJobList
========================
*/
idParallelJobList* idParallelJobManagerLocal::GetJobList( int index )
{
	return jobLists[index];
}

/*
========================
idParallelJobManagerLocal::GetNumProcessingUnits
========================
*/
int idParallelJobManagerLocal::GetNumProcessingUnits()
{
	return maxThreads;
}

/*
========================
idParallelJobManagerLocal::WaitForAllJobLists
========================
*/
void idParallelJobManagerLocal::WaitForAllJobLists()
{
	// wait for all job lists to complete
	for( int i = 0; i < jobLists.Num(); i++ )
	{
		//puncho - L-D1 (E-R4-4): a wait that raises nothing: this runs before an error that is being raised already stops the
		// game (idParallelJobList::Wait, idGameThread::RaiseError). A job's error kept by a list is dropped with it
		jobLists[i]->WaitAndDropError();
	}
}

//puncho - P0: job-list index follows GetJobList; no allocation or worker-side counters.
void idParallelJobManagerLocal::ReadAndClearJobListFrameStats( int index, jobListFrameStats_t& stats )
{
	jobLists[index]->jobListThreads->ReadAndClearFrameStats( stats );
}

//puncho - C1a: the map owner resets the warning latch once per list per map.
void idParallelJobManagerLocal::ResetJobListOverrunWarnings()
{
	for( int i = 0; i < jobLists.Num(); i++ )
	{
		jobLists[i]->jobListThreads->ResetOverrunWarning();
	}
}

/*
========================
idParallelJobManagerLocal::Submit
========================
*/
void idParallelJobManagerLocal::Submit( idParallelJobList_Threads* jobList, int parallelism )
{
	if( jobs_numThreads.IsModified() )
	{
		maxThreads = ResolveJobThreads( jobs_numThreads.GetInteger(), numPhysicalCpuCores ); //puncho - G1
		jobs_numThreads.ClearModified();
	}

	// determine the number of threads to use
	int numThreads = maxThreads;
	if( parallelism == JOBLIST_PARALLELISM_DEFAULT )
	{
		numThreads = maxThreads;
	}
	else if( parallelism == JOBLIST_PARALLELISM_MAX_CORES )
	{
		numThreads = numLogicalCpuCores;
	}
	else if( parallelism == JOBLIST_PARALLELISM_MAX_THREADS )
	{
		numThreads = MAX_JOB_THREADS;
	}
	else if( parallelism > MAX_JOB_THREADS )
	{
		numThreads = MAX_JOB_THREADS;
	}
	else
	{
		numThreads = parallelism;
	}

	// RB: users might have processors these days with more than 32 logical cores
	if( numThreads > MAX_JOB_THREADS )
	{
		numThreads = MAX_JOB_THREADS;
	}

	if( numThreads <= 0 )
	{
		threadJobListState_t state( jobList->GetVersion() );
		jobList->RunJobs( 0, state, false );
		return;
	}

	//puncho - G1: markers do not justify waking another worker.
	numThreads = Min( numThreads, jobList->GetNumRealJobs() );
	for( int i = 0; i < numThreads; i++ )
	{
		threads[i].AddJobList( jobList );
		threads[i].SignalWork();
	}
}
