#ifndef __FRAMEWORK_PREFETCH_H__
#define __FRAMEWORK_PREFETCH_H__

// Main resolves; the worker owns only this immutable description and its byte buffer.
struct resolvedFile_t
{
	int kind; // 0 loose, 1 resource, 2 zip
	char osPath[MAX_OSPATH];
	char entryName[2048];
	uint64 offset, length, compressedSize, archiveSize;
	ID_TIME_T timestamp, archiveTime;
	unsigned int crc;
};

#if defined( DMAP )
// The offline compiler shares the VFS but has no game transition worker.
ID_INLINE bool Prefetch_IsWorker() { return false; }
ID_INLINE void Prefetch_Clear() {}
ID_INLINE void Prefetch_EndLoad() {}
ID_INLINE idFile* Prefetch_Open( const char*, const char* ) { return NULL; }
#else
void Prefetch_Queue( const char* mapName );
void Prefetch_Request( const char* mapName );
void Prefetch_Pump();
void Prefetch_StopWorker();
void Prefetch_BeginLoad();
void Prefetch_EndLoad();
void Prefetch_Clear();
idFile* Prefetch_Open( const char* path, const char* gamedir );
bool Prefetch_IsWorker();
#endif
#endif
