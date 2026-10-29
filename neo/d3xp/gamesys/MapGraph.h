#ifndef __GAME_MAPGRAPH_H__
#define __GAME_MAPGRAPH_H__

class idEntity;

// Decl data is immutable and shared. Dungeon state is server-owned and persistent;
// the load recipe is replicated, and spawn decisions are derived from that recipe.
struct graphLandmark_t
{
	idStr id, kind;
	int flags;
};
struct graphMap_t
{
	idStr name;
	uint32 weight;
	idList<graphLandmark_t, TAG_GAME> landmarks;
};
struct graphKind_t
{
	idStr name;
	int cap, resetPolicy;
	uint32 resetSeconds;
	idList<graphMap_t, TAG_GAME> maps;
};
struct graphFlag_t
{
	idStr name;
	int index;
};
struct graphRule_t
{
	idStr from, landmark, to, flag;
	uint32 weight;
	int state, value;
	bool loop, endcap;
};
class idDeclMapGraph : public idDecl
{
public:
	virtual size_t Size() const;
	virtual const char* DefaultDefinition() const;
	virtual bool Parse( const char* text, int textLength, bool allowBinaryVersion );
	virtual void FreeData();
	virtual void Print();
	int FindKind( const char* name ) const;
	int FindFlag( const char* name ) const;
	bool persistent;
	int maxNodes, maxDepth;
	uint32 textCRC;
	idStr root, instanceEnd;
	idList<graphKind_t, TAG_GAME> kinds;
	idList<graphRule_t, TAG_GAME> rules;
	idList<graphFlag_t, TAG_GAME> flags;
	idStrList landmarkKinds;
};
struct dungeonNode_t
{
	idStr map;
	uint16 kindIndex, visits, depth;
	byte resetPolicy, state;
	uint32 resetSeconds, geometryCRC, landmarkCRC;
};
struct dungeonLink_t
{
	uint16 from, to;
	idStr fromLandmark, toLandmark;
	byte kind, state, conditionMode;
	int condFlag; // -1 in memory; conditionMode distinguishes none on disk.
	uint16 condValue;
};
struct dungeonPending_t
{
	uint32 id;
	uint16 targetNode;
	idStr targetMap, targetName;
	byte action;
	int64 queuedWorldTime;
};
struct dungeonRecipe_t
{
	byte flags;
	uint64 instanceId, rootSeed, variantFlags;
	uint32 graphCRC;
	uint16 currentNode, nodeVisits;
	idStr worldName;
};
class idMapGraphDirector
{
public:
	idMapGraphDirector();
	static unsigned int transitions, refused, inhibited, prefetches;
	void Init();
	void Shutdown();
	void Clear();
	bool Generate( const idDeclMapGraph* decl, uint64 seed );
	bool Start( const char* graph, uint64 seed, const char* parentMap = "", const char* parentLandmark = "" );
	void BeginMap( const char* map );
	void MapLoaded();
	bool RequestTransition( const char* landmark );
	int GetFlag( const char* flag ) const;
	void SetFlag( const char* flag, int value );
	void SetExitLocked( const char* landmark, bool locked );
	int GetNodeVisits() const;
	bool QueueEffect( const char* map, const char* entity, const char* action );
	void ApplyPending();
	void OpenTownPortal();
	void OpenTownPortalFor( idEntity* caster );
	const dungeonRecipe_t& RecipeForLoad( const char* map );
	int WriteMatchGameData( const char* map, byte* out, int max );
	bool SetMatchGameData( const byte* data, int size );
	bool InhibitSpawn( const idDict& args, const idMapFile* map ) const;
	bool GetResetPolicy( const char* map, int& policy, uint32& seconds ) const;
	void StoreRecord();
	bool RestoreRecord();
	bool WriteRecord( idList<byte, TAG_GAME>& bytes ) const;
	bool ReadRecord( const byte* data, int size );
	uint32 WorldDigest() const;
	void Dump( idFile* file ) const;
	int ResolveLink( int node, const char* landmark ) const;
	void PredictNext();
	void RunFrame();
	static uint64 Mix64( uint64 x );
	static uint64 Hash( uint64 a, uint64 b, uint64 c, uint64 d );

	idStr graphName, parentMap, parentLandmark, partyLandmark, portalLandmark, instanceEndKind;
	idList<graphFlag_t, TAG_GAME> flagDefinitions;
	uint64 instanceId, rootSeed;
	uint32 graphCRC;
	int64 createdWorldTime;
	uint16 partyNode, portalNode;
	uint16 flagValues[256];
	idList<dungeonNode_t, TAG_GAME> nodes;
	idList<dungeonLink_t, TAG_GAME> links;
	idList<dungeonPending_t, TAG_GAME> pending;
	int sealedRequired;

private:
	void UpdateExitDoors( bool atLoad );
	int AddNode( const idDeclMapGraph* decl, int kind, int depth );
	const graphMap_t* NodeMap( const idDeclMapGraph* decl, int node ) const;
	bool AddChild( const idDeclMapGraph* decl, int node, const graphLandmark_t& exit, const graphRule_t& rule );
	dungeonRecipe_t recipe;
	idStr recipeMap;
	bool recipeValid, clientRecipe, suspended, recipeNewWorld, portalOpen;
	idVec3 portalOrigin;
	idList<byte, TAG_GAME> persistentHub;
	idStr predictedMap; // last prediction sent; cleared per map load so each load announces its own
	int predictedFrame;
	bool predictionPending;
};
extern idMapGraphDirector mapGraphDirector;
#endif
