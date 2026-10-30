#ifndef __GAME_WORLDSTORE_H__
#define __GAME_WORLDSTORE_H__

class idEntity;
class idSaveGame;
class idRestoreGame;

struct mapTransition_t
{
	idStr nextMap, departLandmark, arrivalLandmark;
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

// Server persistent: records/counter/world identity. Derived per load: ordinal table and
// removed bits (the landmark tables are gameLocal.mapLandmarks'). Nothing here is predicted or updated per tic.
class idWorldStore
{
public:
	idWorldStore();
	void Clear();
	void Shutdown();
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
	bool MeasureCreated( idEntity* ent, const idVec3& landmark, idFile_Memory& out );
	idEntity* SpawnCreated( idFile_Memory& in, const idMat3& rotation, const idVec3& landmark );
	void PrepareSave();
	void Save( idSaveGame* save );
	int StoreFixedBytes() const;
	void Restore( idRestoreGame* save );
	void RestoreOrdinals();
	void ListRecords() const;
#if !defined( ID_RETAIL )
	void DumpRecords( idFile* out ) const;	//puncho - P2b.1.0: recordDump
#endif
	void DrawDebug() const;
#if !defined( ID_RETAIL )
	void TestCodec();
#endif
	void RetireInstanceAfterActivation( uint64 identity, const char* destination );
	void FinishMapActivation();
	void ReplaceMapEntity( idEntity* oldEntity, idEntity* replacement );
	//puncho start - P2b.1.1b: the director, the game and the dev commands reach the store's state through these
	void SetWorld( const char* name, uint64 instance )
	{
		worldName = name;
		instanceId = instance;
	}
	uint64 GetInstanceId() const
	{
		return instanceId;
	}
	unsigned short GetCurrentNode() const
	{
		return currentNode;
	}
	void SetCurrentNode( unsigned short node )
	{
		currentNode = node;
	}
	// the next BeginMap continues this world instead of clearing it; the director sets it around a load it starts
	// and puts the previous value back when the load is refused
	bool GetContinue() const
	{
		return continueNextLoad;
	}
	void SetContinue( bool continueLoad )
	{
		continueNextLoad = continueLoad;
	}
	void SetDungeonRecords( const idList<byte, TAG_GAME>& hub, const idList<byte, TAG_GAME>& record );
	int NumDungeonRecords() const
	{
		return dungeonRecords.Num();
	}
	const idList<byte, TAG_GAME>& DungeonRecord( int i ) const
	{
		return dungeonRecords[i];
	}
	// a gibbed corpse: the map record keeps only their count, in 16 bits
	void AddRemains()
	{
		if( remains < 65535 )
		{
			remains++;
		}
	}
	// a client's snapshot reader writes these bits: MAX_WORLD_MAP_ORDINALS / 8 bytes, indexed by map ordinal
	byte* RemovedBits()
	{
		return removed;
	}
	int NumSpawnRows() const
	{
		return mapSpawned.Num();
	}
	const mapSpawnRow_t& SpawnRow( int i ) const
	{
		return mapSpawned[i];
	}
	//puncho end
	int recordMaps, recordEntries, recordCreated, recordBytes, recordDropped, corpseEvicted;
	unsigned int commitUs, applyUs;
	//puncho - P2b.1.1b: public only for the snapshot's removed-ordinal writer and reader in Game_network.cpp (stage 6's
	// file); stage 6 switches them to RemovedBits(), NumSpawnRows() and SpawnRow( i ) at its next edit of that file, and
	// these two go private then
	idList<mapSpawnRow_t, TAG_GAME> mapSpawned;
	byte removed[MAX_WORLD_MAP_ORDINALS / 8];

private:
	void Commit( bool leaving );
	bool EncodeCreated( idEntity* ent, const idVec3& landmark, idFile_Memory& out, bool assign );
	bool continueNextLoad;
	idStr worldName;
	uint64 instanceId;
	unsigned short currentNode; // load identity, never taken from the director during departure
	idList<idList<byte, TAG_GAME>, TAG_GAME> dungeonRecords;
	unsigned short remains;
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
