#ifndef __GAME_WORLDSTORE_H__
#define __GAME_WORLDSTORE_H__

class idEntity;
class idSaveGame;
class idRestoreGame;

struct mapTransition_t
{
	idStr nextMap, departLandmark, arrivalLandmark;
};

struct landmark_t
{
	idStr id, kind;
	idVec3 origin;
	idMat3 axis;
	int flags;
	float radius;
	idStr link, sealedBy;
	int ordinal;
};

struct transitionVolume_t
{
	idBounds bounds;
	idStr landmark;
	int cap;
	bool carryAll;
	int ordinal;
};

// 63-byte def name plus identity/time, MOVED/MOVER/HEALTH/MOTION/BIND.
static const int MAX_WORLD_CREATED_CARRY_BYTES = 151;

enum recordAspect_t
{
	RECORD_REMOVED = BIT( 0 ),
	RECORD_MOVED = BIT( 1 ),
	RECORD_MOVER = BIT( 2 ),
	RECORD_HEALTH = BIT( 3 ),
	RECORD_BROKEN = BIT( 4 ),
	RECORD_HIDDEN = BIT( 5 ),
	RECORD_CORPSE = BIT( 6 ),
	RECORD_MOTION = BIT( 7 ),
	RECORD_BIND = BIT( 8 ),
	RECORD_SHOWN = BIT( 9 ) //puncho - P2.4 (bug review 2026-10-28): HIDDEN or SHOWN, a visibility that differs from the "hide" key
};

//puncho - P2.4 (bug review 2026-10-28): the removed bits are indexed by map ordinal, which the host and every client
// share, not by spawned row, which differs when one side skips or fails a spawn; BeginSpawn bounds ordinals by 65535
static const int MAX_WORLD_MAP_ORDINALS = 65536;

struct mapSpawnRow_t
{
	int ordinal, entityNumber, spawnId;
	bool departed;
};

struct worldMapRecord_t
{
	idStr world, map;
	uint64 instance;
	unsigned short node;
	idList<byte, TAG_GAME> bytes;
};

// Server persistent: records/counter/world identity. Derived per load: ordinal table,
// removed bits and landmark tables. Nothing here is predicted or updated per tic.
class idWorldStore
{
public:
	idWorldStore();
	void Clear();
	uint64 AllocateInstanceId( uint64 seed );
	void BeginMap( const char* map );
	bool IsActive() const
	{
		return !worldName.IsEmpty();
	}
	void BeginSpawn();
	void TrackSpawn( int ordinal, idEntity* ent );
	int FindSpawnRow( int ordinal ) const;
	void EntityRemoved( idEntity* ent );
	void MarkDeparted( idEntity* ent );
	void MarkTaken( idEntity* ent );
	void CommitCurrentMap();
	void ApplyCurrentMap();
	void BindCreatedRecords();
	bool WriteCreated( idEntity* ent, const idVec3& landmark, idFile_Memory& out );
	idEntity* SpawnCreated( idFile_Memory& in, const idMat3& rotation, const idVec3& landmark );
	void PrepareSave();
	void Save( idSaveGame* save );
	int StoreFixedBytes() const;
	void Restore( idRestoreGame* save );
	void RestoreOrdinals();
	void ListRecords() const;
	void DrawDebug() const;
	void TestCodec();
	void RetireInstanceAfterActivation( uint64 identity, const char* destination );
	void FinishMapActivation();
	int recordMaps, recordEntries, recordCreated, recordBytes, recordDropped, corpseEvicted;
	unsigned int commitUs, applyUs;
	void ReadMapData( idMapFile* map );
	const landmark_t* FindLandmark( const char* name ) const;
	const transitionVolume_t* FindTransitionVolume( const char* name ) const;

	bool continueNextLoad;
	idStr worldName;
	uint64 instanceId;
	unsigned short currentNode; // load identity, never taken from the director during departure
	idList<landmark_t, TAG_GAME> landmarks;
	idList<transitionVolume_t, TAG_GAME> transitionVolumes;
	idList<mapSpawnRow_t, TAG_GAME> mapSpawned;
	byte removed[MAX_WORLD_MAP_ORDINALS / 8];
	idList<idList<byte, TAG_GAME>, TAG_GAME> dungeonRecords;
	unsigned short remains;
	void ReplaceMapEntity( idEntity* oldEntity, idEntity* replacement );

private:
	void Commit( bool leaving );
	idList<worldMapRecord_t, TAG_GAME> records;
	uint64 nextCreatedId;
	uint64 retiringInstance;
	idStr retiringDestination;
	bool committed;
	bool applyingReset;
	bool resetApplied;
};

extern idWorldStore worldStore;
extern idCVar g_worldRecords;
extern idCVar g_worldName;
extern idCVar g_recordCapCorpses;
#endif
