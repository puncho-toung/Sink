#ifndef __FRAMEWORK_PREFETCH_H__
#define __FRAMEWORK_PREFETCH_H__

// Main resolves; the worker owns only this immutable description and its byte buffer.
struct resolvedFile_t
{
	int kind; // 0 loose, 1 resource, 2 zip; -1 an open's source with nothing to compare (P2b.1.5)
	char osPath[MAX_OSPATH];
	char entryName[2048];
	uint64 offset, length, compressedSize, archiveSize;
	ID_TIME_T timestamp, archiveTime;
	unsigned int crc;
	//puncho start - P2b.1.5: idFileSystemLocal::FindInLayer's container and entry, read by the open that searched: a
	// packed file's entry in its container's table, or a loose file's open handle (the searcher's to open from or close),
	// search path and OS path (osPath). FillIdentity zeroes them, so a stored descriptor holds no pointer
	const idZipCacheEntry* zipEntry;
	const idResourceCacheEntry* resourceEntry;
	idFileHandle handle;
	int searchPath;
	//puncho end
};

#if defined( DMAP )
// The offline compiler shares the VFS but has no game transition worker.
ID_INLINE bool Prefetch_IsWorker() { return false; }
ID_INLINE void Prefetch_Clear() {}
ID_INLINE void Prefetch_EndLoad() {}
static const bool prefetchServing = false;
ID_INLINE const resolvedFile_t* Prefetch_Ready( const char* ) { return NULL; }
ID_INLINE idFile* Prefetch_Take( const char*, const resolvedFile_t& ) { return NULL; }
#else
void Prefetch_Queue( const char* mapName );
void Prefetch_Request( const char* mapName );
void Prefetch_Pump();
void Prefetch_StopWorker();
void Prefetch_BeginLoad();
void Prefetch_EndLoad();
void Prefetch_Clear();
//puncho start - P2b.1.5: an open consults the store only while prefetchServing (Prefetch_BeginLoad to the load's
// end); Prefetch_Ready is the walk's descriptor of a path the store holds ready, or NULL (a held path that is not
// ready counts a miss), and Prefetch_Take its bytes as an owning file when source, the identity of what the open's own
// search found, is that descriptor, else NULL and a miss (source.kind -1: nothing comparable)
extern bool prefetchServing;
const resolvedFile_t* Prefetch_Ready( const char* path );
idFile* Prefetch_Take( const char* path, const resolvedFile_t& source );
//puncho end
bool Prefetch_IsWorker();
#endif
#endif
