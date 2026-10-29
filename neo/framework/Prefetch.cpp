#include "precompiled.h"
#pragma hdrstop
#include "Prefetch.h"
#include "Unzip.h"
#include "../renderer/Image.h"
#include "../renderer/BinaryImage.h"
#include <atomic>
#include <sys/stat.h>

idCVar com_prefetch( "com_prefetch", "0", CVAR_NOCHEAT | CVAR_SYSTEM | CVAR_BOOL, "read the next map's cooked bytes on a private worker" );
idCVar com_prefetchBudgetMB( "com_prefetchBudgetMB", "256", CVAR_NOCHEAT | CVAR_SYSTEM | CVAR_INTEGER, "maximum prefetched byte buffers", 1, 2048 );
idCVar com_prefetchSliceUs( "com_prefetchSliceUs", "150", CVAR_NOCHEAT | CVAR_SYSTEM | CVAR_INTEGER, "main-thread prefetch resolution budget", 1, 10000 );
idCVar com_prefetchHoldMs( "com_prefetchHoldMs", "30000", CVAR_NOCHEAT | CVAR_SYSTEM | CVAR_INTEGER, "free a prefetched store no load has used after this many ms; 0 keeps it until the next request or load", 0, 600000 );

static const int MAX_PREFETCH_ENTRIES = 4096;
struct prefetchEntry_t
{
	char path[MAX_OSPATH];
	resolvedFile_t source;
	byte* bytes;
	bool ready, used;
	int error;
};
//puncho - transitions (stage 3 review): 4096 MAX_OSPATH paths and descriptors are 16 MiB. Allocated on the first request,
// so a process at com_prefetch 0 never holds it, and only [0, entryCount) is touched: Add initialises each entry
static prefetchEntry_t* prefetchEntries;
static int requestRing[MAX_PREFETCH_ENTRIES], completionRing[MAX_PREFETCH_ENTRIES];
static std::atomic<int> requestHead( 0 ), requestTail( 0 ), completionHead( 0 ), completionTail( 0 );
static std::atomic<bool> prefetchAbort( false ), requestsDone( false ), workerFinished( true );
static thread_local bool onPrefetchWorker;
static bool workerStarted, prefetchActive, prefetchServing;
static int mapEntryCount;
static int entryCount, resolveIndex, completedCount, hitCount, missCount, skippedCount;
static uint64 reservedBytes, hitBytes, maxSliceUs;
static uint64 maxResolveUs, maxResolveStepUs, maxCompletionUs, maxPublishUs, maxRequestUs;
//puncho - transitions (stage 3 review): admission step costs are decaying maxima that survive requests, so one slow OS
// call (a cold stat or unzOpen, a preemption, a large Mem_Free) raises the next slices' reservation for a few steps; as
// process-lifetime maxima it held every later pump to one resolve and one completion. The reporting maxima are per request
static uint64 admissionResolveStepUs, admissionCompletionUs;
static int sliceOverruns, resolveOverruns, completionOverruns;
static char requestedMap[MAX_OSPATH];
static char pendingMap[MAX_OSPATH];
static bool pendingRequest;
static idHashIndex prefetchHash;
// The preload manifest's walk (an asset lookup and string building per entry) runs in pump slices, not in the request.
static idPreloadManifest preloadManifest;
static int preloadIndex;
static int requestTime;

bool Prefetch_IsWorker()
{
	return onPrefetchWorker;
}

class idPrefetchThread : public idSysThread
{
public:
	virtual int Run()
	{
		onPrefetchWorker = true;
		//puncho - transitions (stage 3 review): IsTerminating as well, so the static object's destructor (FatalError's
		// exit() runs it without fileSystem->Shutdown) can join a worker that is waiting for requestsDone
		while( !prefetchAbort.load( std::memory_order_acquire ) && !IsTerminating() )
		{
			const int tail = requestTail.load( std::memory_order_relaxed );
			if( tail == requestHead.load( std::memory_order_acquire ) )
			{
				if( requestsDone.load( std::memory_order_acquire ) ) { break; }
				Sys_Sleep( 1 );
				continue;
			}
			const int index = requestRing[tail % MAX_PREFETCH_ENTRIES];
			requestTail.store( tail + 1, std::memory_order_release );
			prefetchEntry_t& entry = prefetchEntries[index];
			const resolvedFile_t& source = entry.source;
			struct stat st;
			entry.error = 1;
			if( stat( source.osPath, &st ) == 0 && ( uint64 )st.st_size == source.archiveSize && st.st_mtime == source.archiveTime )
			{
				entry.bytes = ( byte* )Mem_Alloc( ( int )source.length, TAG_IDFILE );
				uint64 read = 0;
				if( source.kind == 2 )
				{
					// Never share the VFS zip handle or its mutable cursor.
					unzFile zip = unzOpen( source.osPath );
					unz_file_info64 info;
					char name[2048];
					bool opened = false;
					if( zip != NULL && unzSetOffset64( zip, source.offset ) == UNZ_OK &&
						unzGetCurrentFileInfo64( zip, &info, name, sizeof( name ), NULL, 0, NULL, 0 ) == UNZ_OK &&
						idStr::Cmp( name, source.entryName ) == 0 && info.crc == source.crc &&
						info.compressed_size == source.compressedSize && info.uncompressed_size == source.length )
					{
						opened = unzOpenCurrentFile( zip ) == UNZ_OK;
						while( opened && read < source.length && !prefetchAbort.load( std::memory_order_relaxed ) && !IsTerminating() )
						{
							const int amount = ( int )Min( ( uint64 )65536, source.length - read );
							const int got = unzReadCurrentFile( zip, entry.bytes + read, amount );
							if( got <= 0 ) { break; }
							read += got;
						}
					}
					if( opened && unzCloseCurrentFile( zip ) != UNZ_OK ) { read = 0; }
					if( zip != NULL ) { unzClose( zip ); }
				}
				else
				{
					FILE* file = fopen( source.osPath, "rb" );
					if( file != NULL )
					{
						if( fseek( file, ( long )source.offset, SEEK_SET ) == 0 )
						{
							while( read < source.length && !prefetchAbort.load( std::memory_order_relaxed ) && !IsTerminating() )
							{
								const int amount = ( int )Min( ( uint64 )65536, source.length - read );
								const size_t got = fread( entry.bytes + read, 1, amount, file );
								if( got == 0 ) { break; }
								read += got;
							}
						}
						fclose( file );
					}
				}
				entry.error = read == source.length ? 0 : 2;
				if( stat( source.osPath, &st ) != 0 || ( uint64 )st.st_size != source.archiveSize || st.st_mtime != source.archiveTime ) { entry.error = 3; }
			}
			// At most 4096 requests exist, so the completion ring cannot wrap onto unread data.
			const int head = completionHead.load( std::memory_order_relaxed );
			completionRing[head % MAX_PREFETCH_ENTRIES] = index;
			completionHead.store( head + 1, std::memory_order_release );
		}
		onPrefetchWorker = false;
		workerFinished.store( true, std::memory_order_release );
		return 0;
	}
};
static idPrefetchThread prefetchThread;

static uint64 Prefetch_DecayEstimate( uint64 estimate, uint64 sample )
{
	return Max( sample, estimate - ( estimate >> 2 ) );
}

static int Prefetch_AdmitCount( uint64 remainingUs, uint64 estimateUs )
{
	// Admission by count: the steps the estimate fits in what is left of the slice, at least one so a slow medium
	// still progresses, at most one batch. The pump's deadline only stops a run the estimate underrated.
	return ( int )Max( ( uint64 )1, Min( ( uint64 )32, remainingUs / Max( ( uint64 )1, estimateUs ) ) );
}

// limit is the admitted count; deadline (0 for the unmeasured final drain) stops after the first step
static void Prefetch_Drain( int limit, uint64 deadline, uint64 sliceBudget )
{
	int tail = completionTail.load( std::memory_order_relaxed );
	const int head = completionHead.load( std::memory_order_acquire );
	int drained = 0;
	while( tail != head && drained < limit )
	{
		const uint64 start = deadline != 0 ? Sys_Microseconds() : 0;
		if( deadline != 0 && drained > 0 && start >= deadline )
		{
			break;
		}
		prefetchEntry_t& entry = prefetchEntries[completionRing[tail % MAX_PREFETCH_ENTRIES]];
		entry.ready = entry.error == 0;
		if( entry.error != 0 )
		{
			Mem_Free( entry.bytes );
			entry.bytes = NULL;
		}
		completedCount++;
		tail++;
		drained++;
		if( deadline != 0 )
		{
			const uint64 elapsed = Sys_Microseconds() - start;
			maxCompletionUs = Max( maxCompletionUs, elapsed );
			admissionCompletionUs = Prefetch_DecayEstimate( admissionCompletionUs, elapsed );
			completionOverruns += elapsed > sliceBudget;
		}
	}
	completionTail.store( tail, std::memory_order_release );
}

void Prefetch_StopWorker()
{
	if( !workerStarted ) { return; }
	const bool completedBeforeStop = workerFinished.load( std::memory_order_acquire ) && requestsDone.load( std::memory_order_acquire );
	prefetchAbort.store( true, std::memory_order_release );
	const int start = Sys_Milliseconds();
	while( !workerFinished.load( std::memory_order_acquire ) && Sys_Milliseconds() - start < 500 ) { Sys_Sleep( 1 ); }
	if( Sys_Milliseconds() - start > 100 ) { common->Printf( "PREFETCH stop wait %d ms\n", Sys_Milliseconds() - start ); }
	// Join before any buffer can be reclaimed, even when an OS read exceeds the diagnostic timeout.
	prefetchThread.StopThread( true );
	workerStarted = false;
	Prefetch_Drain( MAX_PREFETCH_ENTRIES, 0, 0 );
	// Report outside the timed pump; the final slice includes all drain/order/publish work.
	common->Printf( "PREFETCH done %s complete=%d skipped=%d bytes=%llu maxSliceUs=%llu resolved=%d entries=%d cancelled=%d\n", requestedMap, completedCount, skippedCount, ( unsigned long long )reservedBytes, ( unsigned long long )maxSliceUs, resolveIndex, entryCount, !completedBeforeStop );
	common->Printf( "PREFETCH budget maxResolveUs=%llu maxResolveStepUs=%llu maxCompletionUs=%llu maxPublishUs=%llu maxRequestUs=%llu sliceOverruns=%d resolveOverruns=%d completionOverruns=%d\n", ( unsigned long long )maxResolveUs, ( unsigned long long )maxResolveStepUs, ( unsigned long long )maxCompletionUs, ( unsigned long long )maxPublishUs, ( unsigned long long )maxRequestUs, sliceOverruns, resolveOverruns, completionOverruns );
	common->Printf( "PREFETCH admission resolveStepUs=%llu completionUs=%llu\n", ( unsigned long long )admissionResolveStepUs, ( unsigned long long )admissionCompletionUs );
}

void Prefetch_Clear()
{
	Prefetch_StopWorker();
	for( int i = 0; i < entryCount; i++ ) { Mem_Free( prefetchEntries[i].bytes ); }
	entryCount = resolveIndex = completedCount = hitCount = missCount = skippedCount = 0;
	reservedBytes = hitBytes = maxSliceUs = 0;
	maxResolveUs = maxResolveStepUs = maxCompletionUs = maxPublishUs = maxRequestUs = 0;
	sliceOverruns = resolveOverruns = completionOverruns = 0;
	requestHead.store( 0 ); requestTail.store( 0 );
	completionHead.store( 0 ); completionTail.store( 0 );
	prefetchHash.Clear();
	prefetchActive = prefetchServing = false;
	pendingRequest = false;
	preloadManifest.Clear();
	preloadIndex = 0;
	requestedMap[0] = 0;
	// The predicted map's container was opened only to resolve against; the load mounts its own.
	fileSystem->ReleasePrefetchResource();
}

static void Prefetch_Add( const char* path )
{
	if( entryCount == MAX_PREFETCH_ENTRIES || idStr::Length( path ) >= MAX_OSPATH ) { skippedCount++; return; }
	idStrStatic< MAX_OSPATH > name = path;
	name.BackSlashesToSlashes();
	name.ToLower();
	const int key = prefetchHash.GenerateKey( name, false );
	for( int i = prefetchHash.First( key ); i != -1; i = prefetchHash.Next( i ) )
	{
		if( name == prefetchEntries[i].path ) { return; }
	}
	// source stays unset until ResolveForRead fills it; nothing reads it before ready
	prefetchEntry_t& entry = prefetchEntries[entryCount];
	idStr::Copynz( entry.path, name, MAX_OSPATH );
	entry.bytes = NULL;
	entry.ready = entry.used = false;
	entry.error = 0;
	prefetchHash.Add( key, entryCount++ );
}

static void Prefetch_AddPreload( const preloadEntry_s& p )
{
	idStr name;
	if( p.resType == PRELOAD_IMAGE )
	{
		idImage* image = globalImages->GetImageWithParameters( p.resourceName, ( textureFilter_t )p.imgData.filter, ( textureRepeat_t )p.imgData.repeat, ( textureUsage_t )p.imgData.usage, ( cubeFiles_t )p.imgData.cubeMap );
		if( image != NULL && image->IsLoaded() ) { return; }
		name = p.resourceName;
		idImage::GetGeneratedName( name, ( textureUsage_t )p.imgData.usage, ( cubeFiles_t )p.imgData.cubeMap );
		idStr generated;
		idBinaryImage::GetGeneratedFileName( generated, name );
		name = generated;
	}
	else if( p.resType == PRELOAD_MODEL )
	{
		if( Prefetch_ModelResident( p.resourceName ) ) { return; }
		name = "generated/rendermodels/";
		name += p.resourceName;
		idStr extension;
		name.ExtractFileExtension( extension );
		name.SetFileExtension( va( "b%s", extension.c_str() ) );
	}
	else if( p.resType == PRELOAD_SAMPLE )
	{
		if( Prefetch_SampleResident( p.resourceName ) ) { return; }
		name = "generated/";
		name += p.resourceName;
		name.SetFileExtension( "idwav" );
	}
	else if( p.resType == PRELOAD_PARTICLE )
	{
		name = "generated/particles/";
		name += p.resourceName;
		name += ".bprt";
	}
	if( !name.IsEmpty() ) { Prefetch_Add( name ); }
}

void Prefetch_Queue( const char* mapName )
{
	// Game writes before the Game/Draw join; main consumes after that join.
	idStr::Copynz( pendingMap, mapName, sizeof( pendingMap ) );
	pendingRequest = true;
}

void Prefetch_Request( const char* mapName )
{
	assert( !Prefetch_IsWorker() );
	if( !com_prefetch.GetBool() ) { common->Printf( "PREFETCH disabled\n" ); return; }
	idStr map = mapName;
	map.BackSlashesToSlashes();
	map.StripLeadingOnce( "maps/" );
	map.StripFileExtension();
	if( map.IsEmpty() || map.Find( ".." ) >= 0 || map.Find( ':' ) >= 0 ) { common->Warning( "invalid prefetch map" ); return; }
	//puncho - transitions (stage 3 review): the store is keyed by the predicted map; a repeated prediction (a resent
	// reliable, the intent after its prefetch) keeps the work done and restarts the hold, a new map replaces it
	if( !prefetchServing && requestedMap[0] && !map.Icmp( requestedMap ) )
	{
		requestTime = Sys_Milliseconds();
		return;
	}
	Prefetch_Clear();
	if( prefetchEntries == NULL )
	{
		prefetchEntries = ( prefetchEntry_t* )Mem_Alloc( MAX_PREFETCH_ENTRIES * sizeof( prefetchEntry_t ), TAG_IDFILE );
	}
	idStr::Copynz( requestedMap, map, sizeof( requestedMap ) );
	requestTime = Sys_Milliseconds();
	idStr base;
	map.ExtractFileBase( base );
	if( fileSystem->UsingResourceFiles() )
	{
		fileSystem->MountPrefetchResource( va( "%s.resources", base.c_str() ) );
	}
	Prefetch_Add( va( "maps/%s.map", map.c_str() ) );
	Prefetch_Add( va( "generated/maps/%s.bproc", map.c_str() ) );
	Prefetch_Add( va( "maps/%s.proc", map.c_str() ) );
	Prefetch_Add( va( "generated/maps/%s.bcm", map.c_str() ) );
	Prefetch_Add( va( "maps/%s.cm", map.c_str() ) );
	Prefetch_Add( va( "generated/guis/%s.bgui", map.c_str() ) );
	Prefetch_Add( va( "generated/maps/%s.bpvs", map.c_str() ) );
	Prefetch_Add( va( "maps/%s.lightgrid", map.c_str() ) );
	const idDeclEntityDef* aasTypes = static_cast<const idDeclEntityDef*>( declManager->FindType( DECL_ENTITYDEF, "aas_types", false ) );
	if( aasTypes != NULL )
	{
		for( const idKeyValue* kv = aasTypes->dict.MatchPrefix( "type" ); kv != NULL; kv = aasTypes->dict.MatchPrefix( "type", kv ) )
		{
			Prefetch_Add( va( "maps/%s.%s", map.c_str(), kv->GetValue().c_str() ) );
		}
	}
	// The recorded file manifest includes mod-defined AAS classes and environment files.
	idFileManifest files;
	if( files.LoadManifest( va( "maps/%s.manifest", base.c_str() ) ) )
	{
		for( int i = 0; i < files.NumFiles(); i++ ) { Prefetch_Add( files.GetFileNameByIndex( i ) ); }
	}
	mapEntryCount = entryCount;
	idStr manifestName;
	idCommon::PreloadManifestName( map, manifestName );
	// LoadManifest leaves the entries alone when the file is missing
	preloadManifest.Clear();
	preloadManifest.LoadManifest( manifestName );
	preloadIndex = 0;
	prefetchAbort.store( false );
	requestsDone.store( false );
	workerFinished.store( false );
	workerStarted = prefetchThread.StartThread( "map-prefetch", CORE_ANY, THREAD_BELOW_NORMAL );
	prefetchActive = workerStarted;
	if( !workerStarted ) { workerFinished.store( true ); common->Warning( "prefetch worker creation failed" ); }
	common->Printf( "PREFETCH request %s entries=%d preload=%d budget=%d MB\n", requestedMap, entryCount, preloadManifest.NumResources(), com_prefetchBudgetMB.GetInteger() );
}

void Prefetch_Pump()
{
	if( !pendingRequest && !prefetchActive )
	{
		//puncho - transitions (stage 3 review): a speculative store no load has consumed (up to com_prefetchBudgetMB) is
		// freed after com_prefetchHoldMs instead of staying resident until the next request or load
		if( entryCount > 0 && !prefetchServing && com_prefetchHoldMs.GetInteger() > 0 && Sys_Milliseconds() - requestTime >= com_prefetchHoldMs.GetInteger() )
		{
			common->Printf( "PREFETCH expired %s after %d ms\n", requestedMap, Sys_Milliseconds() - requestTime );
			Prefetch_Clear();
		}
		return;
	}
	if( pendingRequest )
	{
		//puncho - transitions (stage 3 review): the request (the store's first allocation, two manifest reads, the
		// worker's start) takes a frame of its own outside the resolution slice; its preload walk is sliced below
		const uint64 requestStart = Sys_Microseconds();
		pendingRequest = false;
		Prefetch_Request( pendingMap );
		maxRequestUs = Max( maxRequestUs, Sys_Microseconds() - requestStart );
		return;
	}
	const uint64 start = Sys_Microseconds();
	const uint64 sliceBudget = ( uint64 )com_prefetchSliceUs.GetInteger();
	const uint64 deadline = start + sliceBudget;
	Prefetch_Drain( Prefetch_AdmitCount( sliceBudget, admissionCompletionUs ), deadline, sliceBudget );
	// The preload walk: up to 32 entries, within the first half of the slice after the first.
	for( int walked = 0; preloadIndex < preloadManifest.NumResources() && walked < 32; walked++ )
	{
		if( walked > 0 && Sys_Microseconds() - start >= sliceBudget / 2 )
		{
			break;
		}
		Prefetch_AddPreload( preloadManifest.GetPreloadByIndex( preloadIndex++ ) );
	}
	const bool walkDone = preloadIndex >= preloadManifest.NumResources();
	if( walkDone && preloadManifest.NumResources() > 0 )
	{
		preloadManifest.Clear();
		preloadIndex = 0;
	}
	const uint64 budget = ( uint64 )com_prefetchBudgetMB.GetInteger() * 1024 * 1024;
	const uint64 walkedUs = Sys_Microseconds() - start;
	const int admitted = Prefetch_AdmitCount( sliceBudget - Min( walkedUs, sliceBudget ), admissionResolveStepUs );
	int batch[32];
	int batchCount = 0;
	for( int attempts = 0; resolveIndex < entryCount && attempts < admitted; attempts++ )
	{
		const uint64 stepStart = Sys_Microseconds();
		if( attempts > 0 && stepStart >= deadline )
		{
			break;
		}
		const int index = resolveIndex++;
		prefetchEntry_t& entry = prefetchEntries[index];
		const bool resolved = fileSystem->ResolveForRead( entry.path, entry.source );
		const uint64 resolveUs = Sys_Microseconds() - stepStart;
		maxResolveUs = Max( maxResolveUs, resolveUs );
		resolveOverruns += resolveUs > sliceBudget;
		if( !resolved || entry.source.length == 0 || entry.source.length > INT_MAX || entry.source.length > budget - Min( reservedBytes, budget ) )
		{
			skippedCount++;
		}
		else
		{
			reservedBytes += entry.source.length;
			// Charge ordering to the admitted step. Preserve map-own priority and
			// stable container/offset ordering within this bounded ready batch.
			int j = batchCount;
			while( j > 0 && index >= mapEntryCount && batch[j - 1] >= mapEntryCount )
			{
				const resolvedFile_t& a = entry.source;
				const resolvedFile_t& b = prefetchEntries[batch[j - 1]].source;
				const int order = idStr::Cmp( a.osPath, b.osPath );
				if( order > 0 || ( order == 0 && a.offset >= b.offset ) ) { break; }
				batch[j] = batch[j - 1];
				j--;
			}
			batch[j] = index;
			batchCount++;
		}
		const uint64 stepUs = Sys_Microseconds() - stepStart;
		maxResolveStepUs = Max( maxResolveStepUs, stepUs );
		admissionResolveStepUs = Prefetch_DecayEstimate( admissionResolveStepUs, stepUs );
	}
	const uint64 publishStart = Sys_Microseconds();
	const int head = requestHead.load( std::memory_order_relaxed );
	for( int i = 0; i < batchCount; i++ )
	{
		requestRing[( head + i ) % MAX_PREFETCH_ENTRIES] = batch[i];
	}
	requestHead.store( head + batchCount, std::memory_order_release );
	// Until the walk is done it can still add entries the resolver has caught up with.
	if( resolveIndex == entryCount && walkDone ) { requestsDone.store( true, std::memory_order_release ); }
	if( workerFinished.load( std::memory_order_acquire ) && completionTail.load( std::memory_order_relaxed ) == completionHead.load( std::memory_order_acquire ) )
	{
		prefetchActive = false;
	}
	maxPublishUs = Max( maxPublishUs, Sys_Microseconds() - publishStart );
	const uint64 elapsed = Sys_Microseconds() - start;
	maxSliceUs = Max( maxSliceUs, elapsed );
	sliceOverruns += elapsed > sliceBudget;
}

void Prefetch_BeginLoad()
{
	Prefetch_StopWorker();
	prefetchActive = false;
	prefetchServing = com_prefetch.GetBool() && entryCount > 0;
}

idFile* Prefetch_Open( const char* path, bool searchDirs, const char* gamedir )
{
	if( !prefetchServing ) { return NULL; }
	idStrStatic< MAX_OSPATH > name = path;
	name.BackSlashesToSlashes();
	name.ToLower();
	const int key = prefetchHash.GenerateKey( name, false );
	for( int i = prefetchHash.First( key ); i != -1; i = prefetchHash.Next( i ) )
	{
		prefetchEntry_t& entry = prefetchEntries[i];
		if( name != entry.path ) { continue; }
		resolvedFile_t current;
		// A loose file serves only a caller that searches directories, as OpenLayerFile does.
		if( entry.ready && ( searchDirs || entry.source.kind != 0 ) && fileSystem->ResolveForRead( path, current, gamedir ) && memcmp( &current, &entry.source, sizeof( current ) ) == 0 )
		{
			//puncho - transitions (stage 3 review): the file takes the buffer (TakeDataOwnership frees it with Mem_Free,
			// as it was allocated), so an opener that keeps it past EndLevelLoad (a RoQ cinematic) never reads freed
			// bytes; a second open of the path goes to the VFS
			idFile_Memory* file = new( TAG_IDFILE ) idFile_Memory( path, ( const char* )entry.bytes, ( int )entry.source.length );
			file->TakeDataOwnership();
			file->SetTimestamp( entry.source.timestamp );
			entry.bytes = NULL;
			entry.ready = false;
			entry.used = true;
			hitCount++;
			hitBytes += entry.source.length;
			return file;
		}
		missCount++;
		return NULL;
	}
	return NULL;
}

void Prefetch_EndLoad()
{
	if( !prefetchServing ) { return; }
	uint64 unused = 0;
	for( int i = 0; i < entryCount; i++ ) { if( prefetchEntries[i].ready ) { unused += prefetchEntries[i].source.length; } }
	common->Printf( "PREFETCH load hit=%d miss=%d kb=%llu unused_kb=%llu\n", hitCount, missCount, ( unsigned long long )( hitBytes / 1024 ), ( unsigned long long )( unused / 1024 ) );
	common->ReportTransitionMetric( TRANSITION_PF_HIT, hitCount );
	common->ReportTransitionMetric( TRANSITION_PF_MISS, missCount );
	common->ReportTransitionMetric( TRANSITION_PF_KB, hitBytes / 1024 );
	Prefetch_Clear();
}

CONSOLE_COMMAND( prefetchMap, "prefetch a map's cooked bytes", idCmdSystem::ArgCompletion_MapName )
{
	if( args.Argc() == 2 ) { Prefetch_Request( args.Argv( 1 ) ); }
}
CONSOLE_COMMAND( cancelPrefetch, "cancel and free the next-map byte store", NULL )
{
	Prefetch_Clear();
	common->Printf( "PREFETCH cancelled\n" );
}
CONSOLE_COMMAND( listPrefetch, "inspect the next-map byte store", NULL )
{
	common->Printf( "PREFETCH %s entries=%d resolved=%d complete=%d skipped=%d bytes=%llu hit=%d miss=%d maxSliceUs=%llu\n", requestedMap, entryCount, resolveIndex, completedCount, skippedCount, ( unsigned long long )reservedBytes, hitCount, missCount, ( unsigned long long )maxSliceUs );
	for( int i = 0; i < entryCount; i++ )
	{
		common->Printf( "%4d %s ready=%d used=%d\n", i, prefetchEntries[i].path, prefetchEntries[i].ready, prefetchEntries[i].used );
	}
}
