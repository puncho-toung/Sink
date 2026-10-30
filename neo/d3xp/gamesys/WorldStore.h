#ifndef __GAME_WORLDSTORE_H__
#define __GAME_WORLDSTORE_H__

class idEntity;
class idSaveGame;
class idRestoreGame;
class idRecordIn;
struct recordFrame_t;

struct mapTransition_t
{
	idStr nextMap, departLandmark, arrivalLandmark;
};

//puncho - P2b.1.2: a carried created entry (MREC v3, Record.h): 82 bytes of head (identity, a 63-byte def name, time,
// flags, section count) and 82 of section 1 (its header, Pose, Health, Visibility, Motion, Bind) is 164; the transition's
// buffer (Game_transition.cpp) is the same 168
static const int MAX_WORLD_CREATED_CARRY_BYTES = 168;

//puncho start - P2b.1.2: recordDump's view of the last transition's carried created entries while the transition holds
// them, from its commit to the next ClearMapTransition (Game_transition.cpp, whose MAX_TRANSITION_CARRY this equals)
static const int MAX_TRANSITION_CARRY_RECORDS = 64;
#if !defined( ID_RETAIL )
	int TransitionCarryRecords( const byte* records[], int lengths[], int max );
#endif
//puncho end

//puncho - P2.4 (bug review 2026-10-28): the removed bits are indexed by map ordinal, which the host and every client
// share, not by spawned row, which differs when one side skips or fails a spawn; BeginSpawn bounds ordinals by 65535
static const int MAX_WORLD_MAP_ORDINALS = 65536;

struct mapSpawnRow_t
{
	int ordinal, entityNumber, spawnId;
	bool departed;
};

//puncho - P2b.1.3: a created entry's bind (ApplyRecordBind) until BindCreatedRecords applies it, by the entity's slot
// and spawn id; the spawn id fails for an entity deleted since (a spawn that failed after its Bind field applied)
struct pendingBind_t
{
	uint64 parent;
	int entityNumber, spawnId;
	short joint, body;
	bool orientated;
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
	void AddPendingBind( const idEntity* ent, uint64 parent, int joint, int body, bool orientated );	//puncho - P2b.1.3
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
	// the next BeginMap continues this world instead of clearing it, then clears the flag; CommitMapTransition and the
	// director set it before a load, the director puts the previous value back when its load is refused
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
	// one more gibbed corpse; the map record keeps only the count, saturating at 65535 (16 bits)
	void AddRemains()
	{
		if( remains < 65535 )
		{
			remains++;
		}
	}
	// one bit per map ordinal, NumRemovedBytes() bytes: the host's snapshot writer reads them, a client's reader
	// zeroes and ORs them
	static const int REMOVED_BYTES = MAX_WORLD_MAP_ORDINALS / 8;
	const byte* RemovedBits() const
	{
		return removed;
	}
	byte* RemovedBits()
	{
		return removed;
	}
	int NumRemovedBytes() const
	{
		return REMOVED_BYTES;
	}
	int NumSpawnRows() const
	{
		return mapSpawned.Num();
	}
	//puncho start - P2b.1.2: the committed map records, for recordTest's fuzz and codec checks
	int NumMapRecords() const
	{
		return records.Num();
	}
	const idList<byte, TAG_GAME>& MapRecordBytes( int i ) const
	{
		return records[i].bytes;
	}
	uint64 GetNextCreatedId() const
	{
		return nextCreatedId;
	}
	//puncho end
	const mapSpawnRow_t& SpawnRow( int i ) const
	{
		return mapSpawned[i];
	}
	//puncho end
	int recordMaps, recordEntries, recordCreated, recordBytes, recordDropped, corpseEvicted;
	int recordInvalid;	//puncho - P2b.1.2: entries and chunks the reader dropped (logged), since the store cleared
	unsigned int commitUs, applyUs;
	//puncho - P2b.1.1b: public only for the snapshot's removed-ordinal writer and reader in Game_network.cpp (stage 6's
	// file); stage 6 switches them to RemovedBits(), NumRemovedBytes(), NumSpawnRows() and SpawnRow( i ) at its next edit
	// of that file, and these two go private then. Its sizeof( worldStore.removed ) must become NumRemovedBytes(), since
	// sizeof of the accessor's result is a pointer's
	idList<mapSpawnRow_t, TAG_GAME> mapSpawned;
	byte removed[REMOVED_BYTES];

private:
	void Commit( bool leaving );
	bool EncodeCreated( idEntity* ent, const recordFrame_t& frame, idFile_Memory& out, bool assign );
	idEntity* SpawnCreatedEntry( const char* unit, const char* def, uint64 id, int64 when, idRecordIn& rec, const recordFrame_t& frame );	//puncho - P2b.1.2
	bool continueNextLoad;
	idStr worldName;
	uint64 instanceId;
	unsigned short currentNode; // load identity, never taken from the director during departure
	idList<idList<byte, TAG_GAME>, TAG_GAME> dungeonRecords;
	unsigned short remains;
	idList<worldMapRecord_t, TAG_GAME> records;
	//puncho - P2b.1.3: derived per load, never saved: a save cannot fall between an apply and its BindCreatedRecords (both
	// run inside the load); BindCreatedRecords empties it, Clear and BeginSpawn too (a slot's spawn ids start again with
	// each map). A carry's spawns add nothing: they bind through the transition's own rows
	idList<pendingBind_t, TAG_GAME> pendingBinds;
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
