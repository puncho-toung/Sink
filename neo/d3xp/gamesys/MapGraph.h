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
//puncho start - P2b.1.4: the dungeon record's (DREC v2) and the recipe's (v1) byte forms as Schema.h's record struct
// lists, beside their structs; MapGraphRecord.cpp and MapGraphDirector.cpp expand them. A list's rows are its fields in
// byte order; a value or count that does not fit fails the write, and the reader refuses what the hand codec refused
#define DUNGEON_FLAGDEF_SCHEMA( X, s ) \
	X( s, name,		STR,	R,	RAW,	63,	SCHEMA_STOCK ) \
	X( s, index,	U8,		R,	RAW,	0,	SCHEMA_STOCK )
//puncho end
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
//puncho start - P2b.1.4
#define DUNGEON_NODE_SCHEMA( X, s ) \
	X( s, map,			STR,	R,	RAW,	63,	SCHEMA_STOCK ) \
	X( s, kindIndex,	U16,	R,	RAW,	0,	SCHEMA_STOCK ) \
	X( s, resetPolicy,	U8,		R,	RAW,	0,	SCHEMA_STOCK ) \
	X( s, resetSeconds,	U32,	R,	RAW,	0,	SCHEMA_STOCK ) \
	X( s, state,		U8,		R,	RAW,	0,	SCHEMA_STOCK ) \
	X( s, visits,		U16,	R,	RAW,	0,	SCHEMA_STOCK ) \
	X( s, depth,		U16,	R,	RAW,	0,	SCHEMA_STOCK ) \
	X( s, geometryCRC,	U32,	R,	RAW,	0,	SCHEMA_STOCK ) \
	X( s, landmarkCRC,	U32,	R,	RAW,	0,	SCHEMA_STOCK )
//puncho end
struct dungeonLink_t
{
	uint16 from, to;
	idStr fromLandmark, toLandmark;
	byte kind, state, conditionMode;
	int condFlag; // -1 in memory; conditionMode distinguishes none on disk.
	uint16 condValue;
};
//puncho start - P2b.1.4: CondFlag is the clamp, -1 written as 0; the reader's conditionMode 0 fix-up is CheckLink's
#define DUNGEON_LINK_SCHEMA( X, s ) \
	X( s, from,				U16,	R,	RAW,	0,	SCHEMA_STOCK ) \
	X( s, fromLandmark,		STR,	R,	RAW,	63,	SCHEMA_STOCK ) \
	X( s, to,				U16,	R,	RAW,	0,	SCHEMA_STOCK ) \
	X( s, toLandmark,		STR,	R,	RAW,	63,	SCHEMA_STOCK ) \
	X( s, kind,				U8,		R,	RAW,	0,	SCHEMA_STOCK ) \
	X( s, state,			U8,		R,	RAW,	0,	SCHEMA_STOCK ) \
	X( s, CondFlag,			HAND,	R,	RAW,	1,	SCHEMA_STOCK ) \
	X( s, conditionMode,	U8,		R,	RAW,	0,	SCHEMA_STOCK ) \
	X( s, condValue,		U16,	R,	RAW,	0,	SCHEMA_STOCK )
//puncho end
struct dungeonPending_t
{
	uint32 id;
	uint16 targetNode;
	idStr targetMap, targetName;
	byte action;
	int64 queuedWorldTime;
};
//puncho start - P2b.1.4
#define DUNGEON_PENDING_SCHEMA( X, s ) \
	X( s, id,				U32,	R,	RAW,	0,	SCHEMA_STOCK ) \
	X( s, targetNode,		U16,	R,	RAW,	0,	SCHEMA_STOCK ) \
	X( s, targetMap,		STR,	R,	RAW,	63,	SCHEMA_STOCK ) \
	X( s, targetName,		STR,	R,	RAW,	63,	SCHEMA_STOCK ) \
	X( s, action,			U8,		R,	RAW,	0,	SCHEMA_STOCK ) \
	X( s, queuedWorldTime,	I64,	R,	RAW,	0,	SCHEMA_STOCK )
//puncho end
struct dungeonRecipe_t
{
	byte flags;
	uint64 instanceId, rootSeed, variantFlags;
	uint32 graphCRC;
	uint16 currentNode, nodeVisits;
	idStr worldName;
};
//puncho start - P2b.1.4: the recipe after its version byte, a wire message in the match parameters; the world name's
// bound is 31. P2b.6.6 appends rows, and changes the version and the two sizes with them
static const int DUNGEON_RECIPE_VERSION = 1;		// the recipe's first byte, before DUNGEON_RECIPE_SCHEMA's rows
static const int DUNGEON_RECIPE_MIN_BYTES = 1 + 1 + 8 + 8 + 4 + 2 + 2 + 8 + 1;	// the version, the rows, a name's length
static const int DUNGEON_RECIPE_MAX_BYTES = DUNGEON_RECIPE_MIN_BYTES + 31;
#define DUNGEON_RECIPE_SCHEMA( X, s ) \
	X( s, flags,		U8,		R,	RAW,	0,	SCHEMA_STOCK ) \
	X( s, instanceId,	U64,	R,	RAW,	0,	SCHEMA_STOCK ) \
	X( s, rootSeed,		U64,	R,	RAW,	0,	SCHEMA_STOCK ) \
	X( s, graphCRC,		U32,	R,	RAW,	0,	SCHEMA_STOCK ) \
	X( s, currentNode,	U16,	R,	RAW,	0,	SCHEMA_STOCK ) \
	X( s, nodeVisits,	U16,	R,	RAW,	0,	SCHEMA_STOCK ) \
	X( s, variantFlags,	U64,	R,	RAW,	0,	SCHEMA_STOCK ) \
	X( s, worldName,	STR,	R,	RAW,	31,	SCHEMA_STOCK )
// the dungeon record: an idMapGraphDirector's persistent state. HAND rows: the two literals, the flag values written
// sparse from their 256 slots, and the two bools
static const int DUNGEON_RECORD_VERSION = 2;
static const int DUNGEON_GENERATOR_VERSION = 1;
#define DUNGEON_RECORD_SCHEMA( X, s ) \
	X( s, Version,			HAND,	R,	RAW,	2,						SCHEMA_STOCK ) \
	X( s, instanceId,		U64,	R,	RAW,	0,						SCHEMA_STOCK ) \
	X( s, rootSeed,			U64,	R,	RAW,	0,						SCHEMA_STOCK ) \
	X( s, graphName,		STR,	R,	RAW,	63,						SCHEMA_STOCK ) \
	X( s, graphCRC,			U32,	R,	RAW,	0,						SCHEMA_STOCK ) \
	X( s, GeneratorVersion,	HAND,	R,	RAW,	2,						SCHEMA_STOCK ) \
	X( s, createdWorldTime,	I64,	R,	RAW,	0,						SCHEMA_STOCK ) \
	X( s, parentMap,		STR,	R,	RAW,	63,						SCHEMA_STOCK ) \
	X( s, parentLandmark,	STR,	R,	RAW,	63,						SCHEMA_STOCK ) \
	X( s, nodes,			LIST,	R,	RAW,	( Node, 2, 256 ),		SCHEMA_STOCK ) \
	X( s, links,			LIST,	R,	RAW,	( Link, 2, 1024 ),		SCHEMA_STOCK ) \
	X( s, FlagValues,		HAND,	R,	RAW,	0,						SCHEMA_STOCK ) \
	X( s, partyNode,		U16,	R,	RAW,	0,						SCHEMA_STOCK ) \
	X( s, partyLandmark,	STR,	R,	RAW,	63,						SCHEMA_STOCK ) \
	X( s, portalNode,		U16,	R,	RAW,	0,						SCHEMA_STOCK ) \
	X( s, portalLandmark,	STR,	R,	RAW,	63,						SCHEMA_STOCK ) \
	X( s, pending,			LIST,	R,	RAW,	( Pending, 2, 1024 ),	SCHEMA_STOCK ) \
	X( s, instanceEndKind,	STR,	R,	RAW,	63,						SCHEMA_STOCK ) \
	X( s, flagDefinitions,	LIST,	R,	RAW,	( FlagDef, 2, 256 ),	SCHEMA_STOCK ) \
	X( s, Suspended,		HAND,	R,	RAW,	1,						SCHEMA_STOCK ) \
	X( s, PortalOpen,		HAND,	R,	RAW,	1,						SCHEMA_STOCK ) \
	X( s, portalOrigin,		VEC3,	R,	RAW,	0,						SCHEMA_STOCK )
//puncho end
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
#if !defined( ID_RETAIL )
	int PeekMatchGameData( const char* map, byte* out, int max ) const;	//puncho - P2b.1.0: recordDump's recipe bytes
	void TestRecipe( const char* map ) const;	//puncho - P2b.1.4: recordTest recipe
#endif
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
	//puncho - P2b.1.4: public with the rest of the record's state, for DUNGEON_RECORD_SCHEMA's rows
	bool suspended, portalOpen;
	idVec3 portalOrigin;

private:
	void UpdateExitDoors( bool atLoad );
	int AddNode( const idDeclMapGraph* decl, int kind, int depth );
	const graphMap_t* NodeMap( const idDeclMapGraph* decl, int node ) const;
	bool AddChild( const idDeclMapGraph* decl, int node, const graphLandmark_t& exit, const graphRule_t& rule );
	dungeonRecipe_t recipe;
	idStr recipeMap;
	bool recipeValid, clientRecipe, recipeNewWorld;
	idList<byte, TAG_GAME> persistentHub;
	idStr predictedMap; // last prediction sent; cleared per map load so each load announces its own
	int predictedFrame;
	bool predictionPending;
};
extern idMapGraphDirector mapGraphDirector;
#endif
