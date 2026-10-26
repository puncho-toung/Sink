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

static const int MAX_PREFETCH_ENTRIES = 4096;
struct prefetchEntry_t
{
	char path[MAX_OSPATH];
	resolvedFile_t source;
	byte* bytes;
	bool ready, used;
	int error;
};
static prefetchEntry_t prefetchEntries[MAX_PREFETCH_ENTRIES];
static int requestRing[MAX_PREFETCH_ENTRIES], completionRing[MAX_PREFETCH_ENTRIES];
static std::atomic<int> requestHead( 0 ), requestTail( 0 ), completionHead( 0 ), completionTail( 0 );
static std::atomic<bool> prefetchAbort( false ), requestsDone( false ), workerFinished( true );
static thread_local bool onPrefetchWorker;
static bool workerStarted, prefetchActive, prefetchServing;
static int mapEntryCount;
static int entryCount, resolveIndex, completedCount, hitCount, missCount, skippedCount;
static uint64 reservedBytes, hitBytes, maxSliceUs;
static uint64 maxResolveUs, maxResolveStepUs, maxCompletionUs, maxPublishUs, maxRequestUs;
// Admission history survives requests; reporting maxima below still describe one request.
// Forgetting earlier slow descriptors lets a new map's cheap prefix overfill its slice.
static uint64 admissionResolveStepUs, admissionCompletionUs, admissionPublishUs;
static int sliceOverruns, resolveOverruns, completionOverruns;
static char requestedMap[MAX_OSPATH];
static char pendingMap[MAX_OSPATH];
static bool pendingRequest;
static idHashIndex prefetchHash;

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
		while( !prefetchAbort.load( std::memory_order_acquire ) )
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
						while( opened && read < source.length && !prefetchAbort.load( std::memory_order_relaxed ) )
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
							while( read < source.length && !prefetchAbort.load( std::memory_order_relaxed ) )
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

static void Prefetch_Drain( int limit = MAX_PREFETCH_ENTRIES, uint64 deadline = 0, uint64 sliceBudget = 0 )
{
	int tail = completionTail.load( std::memory_order_relaxed );
	const int head = completionHead.load( std::memory_order_acquire );
	int drained = 0;
	while( tail != head && drained < limit )
	{
		const uint64 start = deadline != 0 ? Sys_Microseconds() : 0;
		if( deadline != 0 )
		{
			if( start >= deadline ) { break; }
			const bool initialProbe = drained == 0 && deadline - start > sliceBudget / 2 &&
				( admissionCompletionUs == 0 || admissionCompletionUs >= sliceBudget || start + admissionCompletionUs + 1 < deadline );
			if( !initialProbe && ( admissionCompletionUs == 0 || start + 2 * admissionCompletionUs + 1 >= deadline ) ) { break; }
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
			admissionCompletionUs = Max( admissionCompletionUs, elapsed );
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
	Prefetch_Drain();
	// Report outside the timed pump; the final slice includes all drain/order/publish work.
	common->Printf( "PREFETCH done %s complete=%d skipped=%d bytes=%llu maxSliceUs=%llu resolved=%d entries=%d cancelled=%d\n", requestedMap, completedCount, skippedCount, ( unsigned long long )reservedBytes, ( unsigned long long )maxSliceUs, resolveIndex, entryCount, !completedBeforeStop );
	common->Printf( "PREFETCH budget maxResolveUs=%llu maxResolveStepUs=%llu maxCompletionUs=%llu maxPublishUs=%llu maxRequestUs=%llu sliceOverruns=%d resolveOverruns=%d completionOverruns=%d\n", ( unsigned long long )maxResolveUs, ( unsigned long long )maxResolveStepUs, ( unsigned long long )maxCompletionUs, ( unsigned long long )maxPublishUs, ( unsigned long long )maxRequestUs, sliceOverruns, resolveOverruns, completionOverruns );
	common->Printf( "PREFETCH admission resolveStepUs=%llu completionUs=%llu publishUs=%llu\n", ( unsigned long long )admissionResolveStepUs, ( unsigned long long )admissionCompletionUs, ( unsigned long long )admissionPublishUs );
}

void Prefetch_Clear()
{
	Prefetch_StopWorker();
	for( int i = 0; i < entryCount; i++ ) { Mem_Free( prefetchEntries[i].bytes ); }
	memset( prefetchEntries, 0, sizeof( prefetchEntries ) );
	entryCount = resolveIndex = completedCount = hitCount = missCount = skippedCount = 0;
	reservedBytes = hitBytes = maxSliceUs = 0;
	maxResolveUs = maxResolveStepUs = maxCompletionUs = maxPublishUs = maxRequestUs = 0;
	sliceOverruns = resolveOverruns = completionOverruns = 0;
	requestHead.store( 0 ); requestTail.store( 0 );
	completionHead.store( 0 ); completionTail.store( 0 );
	prefetchHash.Clear();
	prefetchActive = prefetchServing = false;
	pendingRequest = false;
}

static void Prefetch_Add( const char* path )
{
	if( entryCount == MAX_PREFETCH_ENTRIES || idStr::Length( path ) >= MAX_OSPATH ) { skippedCount++; return; }
	idStr name = path;
	name.BackSlashesToSlashes();
	name.ToLower();
	const int key = prefetchHash.GenerateKey( name, false );
	for( int i = prefetchHash.First( key ); i != -1; i = prefetchHash.Next( i ) )
	{
		if( name == prefetchEntries[i].path ) { return; }
	}
	idStr::Copynz( prefetchEntries[entryCount].path, name, MAX_OSPATH );
	prefetchHash.Add( key, entryCount++ );
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
	Prefetch_Clear();
	idStr map = mapName;
	map.BackSlashesToSlashes();
	map.StripLeadingOnce( "maps/" );
	map.StripFileExtension();
	if( map.IsEmpty() || map.Find( ".." ) >= 0 || map.Find( ':' ) >= 0 ) { common->Warning( "invalid prefetch map" ); return; }
	idStr::Copynz( requestedMap, map, sizeof( requestedMap ) );
	idStr base;
	map.ExtractFileBase( base );
	if( fileSystem->UsingResourceFiles() )
	{
		fileSystem->MountPrefetchResource( va( "%s.resources", base.c_str() ) );
		common->Printf( "PREFETCH early mount %s.resources affects current-map on-demand precedence\n", base.c_str() );
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
	idPreloadManifest manifest;
	manifest.LoadManifest( manifestName );
	for( int i = 0; i < manifest.NumResources(); i++ )
	{
		const preloadEntry_s& p = manifest.GetPreloadByIndex( i );
		idStr name;
		if( p.resType == PRELOAD_IMAGE )
		{
			idImage* image = globalImages->GetImageWithParameters( p.resourceName, ( textureFilter_t )p.imgData.filter, ( textureRepeat_t )p.imgData.repeat, ( textureUsage_t )p.imgData.usage, ( cubeFiles_t )p.imgData.cubeMap );
			if( image != NULL && image->IsLoaded() ) { continue; }
			name = p.resourceName;
			idImage::GetGeneratedName( name, ( textureUsage_t )p.imgData.usage, ( cubeFiles_t )p.imgData.cubeMap );
			idStr generated;
			idBinaryImage::GetGeneratedFileName( generated, name );
			name = generated;
		}
		else if( p.resType == PRELOAD_MODEL )
		{
			if( Prefetch_ModelResident( p.resourceName ) ) { continue; }
			name = "generated/rendermodels/";
			name += p.resourceName;
			idStr extension;
			name.ExtractFileExtension( extension );
			name.SetFileExtension( va( "b%s", extension.c_str() ) );
		}
		else if( p.resType == PRELOAD_SAMPLE )
		{
			if( Prefetch_SampleResident( p.resourceName ) ) { continue; }
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
	prefetchAbort.store( false );
	requestsDone.store( false );
	workerFinished.store( false );
	workerStarted = prefetchThread.StartThread( "map-prefetch", CORE_ANY, THREAD_BELOW_NORMAL );
	prefetchActive = workerStarted;
	if( !workerStarted ) { workerFinished.store( true ); common->Warning( "prefetch worker creation failed" ); }
	common->Printf( "PREFETCH request %s entries=%d budget=%d MB\n", requestedMap, entryCount, com_prefetchBudgetMB.GetInteger() );
}

void Prefetch_Pump()
{
	if( !pendingRequest && !prefetchActive ) { return; }
	const uint64 start = Sys_Microseconds();
	const uint64 sliceBudget = ( uint64 )com_prefetchSliceUs.GetInteger();
	if( pendingRequest )
	{
		pendingRequest = false;
		Prefetch_Request( pendingMap );
		maxRequestUs = Max( maxRequestUs, Sys_Microseconds() - start );
	}
	if( !prefetchActive )
	{
		const uint64 elapsed = Sys_Microseconds() - start;
		maxSliceUs = Max( maxSliceUs, elapsed );
		sliceOverruns += elapsed > sliceBudget;
		return;
	}
	Prefetch_Drain( 32, start + sliceBudget, sliceBudget );
	const uint64 budget = ( uint64 )com_prefetchBudgetMB.GetInteger() * 1024 * 1024;
	int batch[32];
	int batchCount = 0;
	int attempts = 0;
	while( resolveIndex < entryCount && batchCount < 32 && attempts < 32 )
	{
		const uint64 stepStart = Sys_Microseconds();
		const uint64 elapsed = stepStart - start;
		// Resolution contains indivisible OS calls. Reserve twice the observed step
		// high-water cost, plus publication, rather than starting with a few us left.
		// One initial probe makes progress when there is no estimate (or one call
		// itself exceeds the budget); it still reports its full measured duration.
		const bool initialProbe = attempts == 0 && elapsed < Max( ( uint64 )1, sliceBudget / 2 ) &&
			( admissionResolveStepUs == 0 || admissionResolveStepUs >= sliceBudget || elapsed + admissionResolveStepUs + admissionPublishUs + 1 < sliceBudget );
		if( elapsed >= sliceBudget || ( !initialProbe && ( admissionResolveStepUs == 0 || elapsed + 2 * admissionResolveStepUs + admissionPublishUs + 1 >= sliceBudget ) ) )
		{
			break;
		}
		const bool firstEstimate = maxResolveStepUs == 0;
		const int index = resolveIndex++;
		prefetchEntry_t& entry = prefetchEntries[index];
		const bool resolved = fileSystem->ResolveForRead( entry.path, entry.source );
		const uint64 resolveUs = Sys_Microseconds() - stepStart;
		maxResolveUs = Max( maxResolveUs, resolveUs );
		resolveOverruns += resolveUs > sliceBudget;
		attempts++;
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
		admissionResolveStepUs = Max( admissionResolveStepUs, stepUs );
		if( firstEstimate ) { break; }
	}
	const uint64 publishStart = Sys_Microseconds();
	const int head = requestHead.load( std::memory_order_relaxed );
	for( int i = 0; i < batchCount; i++ )
	{
		requestRing[( head + i ) % MAX_PREFETCH_ENTRIES] = batch[i];
	}
	requestHead.store( head + batchCount, std::memory_order_release );
	if( resolveIndex == entryCount ) { requestsDone.store( true, std::memory_order_release ); }
	if( workerFinished.load( std::memory_order_acquire ) && completionTail.load( std::memory_order_relaxed ) == completionHead.load( std::memory_order_acquire ) )
	{
		prefetchActive = false;
	}
	const uint64 publishUs = Sys_Microseconds() - publishStart;
	maxPublishUs = Max( maxPublishUs, publishUs );
	admissionPublishUs = Max( admissionPublishUs, publishUs );
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

idFile* Prefetch_Open( const char* path, const char* gamedir )
{
	if( !prefetchServing ) { return NULL; }
	idStr name = path;
	name.BackSlashesToSlashes();
	name.ToLower();
	const int key = prefetchHash.GenerateKey( name, false );
	for( int i = prefetchHash.First( key ); i != -1; i = prefetchHash.Next( i ) )
	{
		prefetchEntry_t& entry = prefetchEntries[i];
		if( name != entry.path ) { continue; }
		resolvedFile_t current;
		if( entry.ready && fileSystem->ResolveForRead( path, current, gamedir ) && memcmp( &current, &entry.source, sizeof( current ) ) == 0 )
		{
			idFile_Memory* file = new( TAG_IDFILE ) idFile_Memory( path, ( const char* )entry.bytes, ( int )entry.source.length );
			file->SetTimestamp( entry.source.timestamp );
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
	for( int i = 0; i < entryCount; i++ ) { if( prefetchEntries[i].ready && !prefetchEntries[i].used ) { unused += prefetchEntries[i].source.length; } }
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
