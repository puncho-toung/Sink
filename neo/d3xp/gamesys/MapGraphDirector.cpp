#include "../../idlib/precompiled.h"
#pragma hdrstop

#include "../Game_local.h"
#include "MapGraph.h"
#include "WorldStore.h"
#include "MapGraphLocal.h"

const idDeclMapGraph* FindGraph( const char* name )
{
	if( !name || !name[0] )
	{
		return NULL;
	}
	const idDeclMapGraph* decl = static_cast<const idDeclMapGraph*>( declManager->FindType( DECL_MAPGRAPH, name, false ) );
	return decl && decl->GetState() != DS_DEFAULTED ? decl : NULL;
}
//puncho start - P2.4 (bug review 2026-10-28): a node map's fingerprint parsed the whole .map on the game thread, inside
// the trigger touch, at every Start (once per distinct node map) and at every in-instance transition. It is kept per
// map with the parsed file's timestamp and re-parsed only when the file on disk changed; every map load also stores
// the fingerprint of the map it parsed, from the tables it already read (MapLoaded). A Start of a graph whose maps
// were neither loaded nor fingerprinted in this process still parses each once.
struct graphFingerprint_t
{
	idStr map;
	ID_TIME_T timestamp;
	uint32 geometry, landmarks;
};
static idList<graphFingerprint_t, TAG_GAME> graphFingerprints;
static void GraphStoreFingerprint( const idStr& map, ID_TIME_T timestamp, uint32 geometry, uint32 landmarks )
{
	int index = -1;
	for( int i = 0; i < graphFingerprints.Num() && index < 0; i++ )
	{
		index = graphFingerprints[i].map == map ? i : -1;
	}
	graphFingerprint_t& entry = index >= 0 ? graphFingerprints[index] : graphFingerprints.Alloc();
	entry.map = map;
	entry.timestamp = timestamp;
	entry.geometry = geometry;
	entry.landmarks = landmarks;
}
//puncho end
static bool GraphTablesFingerprint( const idWorldStore& data, uint32& landmarks )
{
	idList<int, TAG_GAME> order;
	for( int i = 0; i < data.landmarks.Num(); i++ )
	{
		if( data.landmarks[i].ordinal < 0 )
		{
			continue; // the town portal MapLoaded adds is no part of the map
		}
		int at = 0;
		while( at < order.Num() && data.landmarks[order[at]].id.Cmp( data.landmarks[i].id ) < 0 )
		{
			at++;
		}
		if( at < order.Num() && data.landmarks[order[at]].id == data.landmarks[i].id )
		{
			return false;
		}
		order.Insert( i, at );
	}
	idList<byte, TAG_GAME> bytes;
	idGraphBytes b( bytes );
	b.Put( order.Num(), 2 );
	for( int i = 0; i < order.Num(); i++ )
	{
		const landmark_t& lm = data.landmarks[order[i]];
		b.Text( lm.id );
		b.Text( lm.kind );
		b.Put( lm.flags, 1 );
		b.Text( lm.link );
		b.Text( lm.sealedBy );
		for( int v = 0; v < 13; v++ )
		{
			const float value = v < 3 ? lm.origin[v] : v < 12 ? lm.axis[( v - 3 ) / 3][( v - 3 ) % 3]
								: lm.radius;
			uint32 bits = 0;
			if( value != 0.0f )
			{
				memcpy( &bits, &value, 4 );
			}
			b.Put( bits, 4 );
		}
		const transitionVolume_t* volume = data.FindTransitionVolume( lm.id );
		b.Put( volume ? 1 : 0, 1 );
		if( volume )
		{
			b.Put( volume->cap, 1 );
			b.Put( volume->carryAll ? 1 : 0, 1 );
			for( int v = 0; v < 6; v++ )
			{
				const float value = volume->bounds[v / 3][v % 3];
				uint32 bits = 0;
				if( value != 0.0f )
				{
					memcpy( &bits, &value, 4 );
				}
				b.Put( bits, 4 );
			}
		}
	}
	if( !b.Good() )
	{
		return false;
	}
	landmarks = CRC32_BlockChecksum( bytes.Ptr(), bytes.Num() );
	return true;
}
static bool GraphMapFingerprint( const char* name, uint32& geometry, uint32& landmarks )
{
	ID_TIME_T timestamp = FILE_NOT_FOUND_TIMESTAMP;
	fileSystem->ReadFile( idStr( name ) + ".map", NULL, &timestamp );
	for( int i = 0; i < graphFingerprints.Num(); i++ )
	{
		if( graphFingerprints[i].map == name )
		{
			if( timestamp != FILE_NOT_FOUND_TIMESTAMP && graphFingerprints[i].timestamp == timestamp )
			{
				geometry = graphFingerprints[i].geometry;
				landmarks = graphFingerprints[i].landmarks;
				return true;
			}
			break;
		}
	}
	idMapFile map;
	if( !map.Parse( idStr( name ) + ".map", false, false, false, -1, false ) )
	{
		return false;
	}
	geometry = map.GetGeometryCRC();
	idWorldStore data;
	data.ReadMapData( &map );
	if( !GraphTablesFingerprint( data, landmarks ) )
	{
		return false;
	}
	GraphStoreFingerprint( name, map.GetFileTime(), geometry, landmarks );
	return true;
}
const dungeonRecipe_t& idMapGraphDirector::RecipeForLoad( const char* map )
{
	if( clientRecipe || ( recipeValid && recipeMap == GraphMapName( map ) ) )
	{
		return recipe;
	}
	recipeMap = GraphMapName( map );
	recipeValid = true;
	recipe.worldName.Clear();
	recipe.flags = 0;
	recipe.instanceId = recipe.rootSeed = recipe.variantFlags = 0;
	recipe.graphCRC = 0;
	recipe.currentNode = 0;
	recipe.nodeVisits = 0;
	if( !g_worldRecords.GetBool() && !worldStore.continueNextLoad )
	{
		return recipe;
	}
	if( worldStore.continueNextLoad && !graphName.IsEmpty() )
	{
		if( suspended && persistentHub.Num() )
		{
			idMapGraphDirector hub;
			if( hub.ReadRecord( persistentHub.Ptr(), persistentHub.Num() ) )
			{
				recipe.worldName = hub.graphName;
				recipe.graphCRC = hub.graphCRC;
				for( int i = 0; i < hub.nodes.Num(); i++ )
				{
					if( hub.nodes[i].map == recipeMap )
					{
						recipe.currentNode = i;
						recipe.nodeVisits = hub.nodes[i].visits;
						break;
					}
				}
				const idDeclMapGraph* hubDecl = FindGraph( hub.graphName );
				for( int i = 0; hubDecl && i < hubDecl->flags.Num(); i++ )
				{
					const graphFlag_t& f = hubDecl->flags[i];
					if( f.index < 64 && hub.GetFlag( f.name ) )
					{
						recipe.variantFlags |= 1ULL << f.index;
					}
				}
				return recipe;
			}
		}
		recipe.worldName = graphName;
		recipe.flags = instanceId ? 1 : 0;
		recipe.instanceId = instanceId;
		recipe.rootSeed = rootSeed;
		recipe.currentNode = partyNode;
		recipe.graphCRC = graphCRC;
		if( partyNode < nodes.Num() )
		{
			recipe.nodeVisits = nodes[partyNode].visits;
		}
		const idDeclMapGraph* liveDecl = FindGraph( graphName );
		for( int i = 0; liveDecl && i < liveDecl->flags.Num(); i++ )
		{
			const graphFlag_t& f = liveDecl->flags[i];
			if( f.index < 64 && GetFlag( f.name ) )
			{
				recipe.variantFlags |= 1ULL << f.index;
			}
		}
	}
	else
	{
		recipe.worldName = g_worldName.GetString()[0] ? g_worldName.GetString() : "default";
		const idDeclMapGraph* decl = FindGraph( recipe.worldName );
		if( decl )
		{
			recipe.graphCRC = decl->textCRC;
			int node = 0;
			for( int k = 0; k < decl->kinds.Num(); k++ )
			{
				for( int m = 0; m < decl->kinds[k].maps.Num(); m++, node++ )
				{
					if( decl->kinds[k].maps[m].name == recipeMap )
					{
						recipe.currentNode = node;
					}
				}
			}
			recipe.nodeVisits = 1;
		}
	}
	if( recipe.worldName.Length() > 31 )
	{
		gameLocal.Error( "world recipe name exceeds 31 bytes: %s", recipe.worldName.c_str() );
	}
	return recipe;
}
//puncho - P2b.1.0: the recipe's wire bytes; WriteMatchGameData's encoder and recordDump's
static int WriteRecipeBytes( const dungeonRecipe_t& r, byte* out, int max )
{
	if( r.worldName.IsEmpty() )
	{
		return 0;
	}
	idList<byte, TAG_GAME> bytes;
	idGraphBytes b( bytes );
	b.Put( 1, 1 );
	b.Put( r.flags, 1 );
	b.Put( r.instanceId, 8 );
	b.Put( r.rootSeed, 8 );
	b.Put( r.graphCRC, 4 );
	b.Put( r.currentNode, 2 );
	b.Put( r.nodeVisits, 2 );
	b.Put( r.variantFlags, 8 );
	b.Text( r.worldName, 31 );
	if( !b.Good() || bytes.Num() > max )
	{
		return -1;
	}
	memcpy( out, bytes.Ptr(), bytes.Num() );
	return bytes.Num();
}
int idMapGraphDirector::WriteMatchGameData( const char* map, byte* out, int max )
{
	if( !clientRecipe && !worldStore.continueNextLoad )
	{
		recipeValid = false;
		recipeNewWorld = true;
	}
	return WriteRecipeBytes( RecipeForLoad( map ), out, max );
}
#if !defined( ID_RETAIL )
//puncho - P2b.1.0: the bytes of the recipe this map loaded with: the cached one, which BeginMap stored or RestoreRecord
// recomputed, encoded as WriteMatchGameData encodes it. It is not what WriteMatchGameData would send now, which
// recomputes a new world's recipe when no load continues one. A copy resolves it, so the live director's recipe cache
// and the new-world latch, which belong to the next load, stay as they are
int idMapGraphDirector::PeekMatchGameData( const char* map, byte* out, int max ) const
{
	idMapGraphDirector probe = *this;
	return WriteRecipeBytes( probe.RecipeForLoad( map ), out, max );
}
#endif
bool idMapGraphDirector::SetMatchGameData( const byte* data, int size )
{
	Clear();
	clientRecipe = true;
	recipeValid = true;
	if( size == 0 )
	{
		return true;
	}
	if( size < 35 || size > 66 )
	{
		return false;
	}
	idGraphBytes b( data, size );
	if( b.Get( 1 ) != 1 )
	{
		return false;
	}
	recipe.flags = b.Get( 1 );
	recipe.instanceId = b.Get( 8 );
	recipe.rootSeed = b.Get( 8 );
	recipe.graphCRC = b.Get( 4 );
	recipe.currentNode = b.Get( 2 );
	recipe.nodeVisits = b.Get( 2 );
	recipe.variantFlags = b.Get( 8 );
	b.String( recipe.worldName, 31 );
	if( !b.Good() || recipe.flags > 1 || recipe.currentNode >= 256 || recipe.worldName.IsEmpty() || ( ( recipe.flags & 1 ) != ( recipe.instanceId != 0 ) ) )
	{
		Clear();
		return false;
	}
	return true;
}

void idMapGraphDirector::BeginMap( const char* map )
{
	const dungeonRecipe_t loadRecipe = RecipeForLoad( map );
	if( !clientRecipe && ( recipeNewWorld || ( !suspended && ( graphName != loadRecipe.worldName || instanceId != loadRecipe.instanceId ) ) ) )
	{
		Clear();
		const idDeclMapGraph* decl = FindGraph( loadRecipe.worldName );
		if( decl && decl->persistent )
		{
			Generate( decl, 0 );
		}
		else
		{
			graphName = loadRecipe.worldName;
		}
	}
	recipe = loadRecipe;
	recipeValid = true;
	recipeMap = GraphMapName( map );
	predictedMap.Clear();
	predictedFrame = -1;
	predictionPending = false;
	worldStore.worldName = recipe.worldName;
	worldStore.instanceId = recipe.instanceId;
	if( !clientRecipe && !suspended )
	{
		if( !instanceId )
		{
			for( int i = 0; i < nodes.Num(); i++ )
			{
				if( nodes[i].map == recipeMap )
				{
					partyNode = i;
					break;
				}
			}
		}
		if( partyNode < nodes.Num() )
		{
			nodes[partyNode].state |= 1;
			nodes[partyNode].visits = Max( 1, ( int )nodes[partyNode].visits );
		}
	}
}
bool idMapGraphDirector::Start( const char* name, uint64 seed, const char* parent, const char* landmark )
{
	if( instanceId != 0 && suspended )
	{
		gameLocal.Warning( "Resume the suspended dungeon through townportal and leave its root before starting another instance" );
		return false;
	}
	if( common->IsClient() )
	{
		return false;
	}
	const idDeclMapGraph* decl = FindGraph( name );
	if( !decl || decl->persistent || idStr::Length( name ) > 31 )
	{
		gameLocal.Warning( "invalid instance graph %s", name );
		return false;
	}
	const idMapGraphDirector previous = *this;
	const bool previousContinue = worldStore.continueNextLoad;
	// Resolve parent strings before Generate clears the previous director.
	idStr returnMap = GraphMapName( parent[0] ? parent : gameLocal.GetMapName() ), returnLandmark = landmark;
	if( returnLandmark.IsEmpty() && worldStore.landmarks.Num() )
	{
		returnLandmark = worldStore.landmarks[0].id;
	}
	idList<byte, TAG_GAME> hub;
	if( instanceId == 0 )
	{
		WriteRecord( hub );
	}
	else
	{
		hub = persistentHub;
	}
	if( seed == 0 )
	{
		seed = g_dungeonSeed.GetInteger();
	}
	if( seed == 0 )
	{
		seed = Mix64( Sys_Microseconds() ^ ( uint64 )gameLocal.GetWorldTime() );
	}
	if( !Generate( decl, seed ) || nodes.Num() == 0 )
	{
		*this = previous;
		return false;
	}
	for( int i = 0; i < nodes.Num(); i++ )
	{
		int previousMap = -1;
		for( int j = 0; j < i; j++ )
		{
			if( nodes[j].map == nodes[i].map )
			{
				previousMap = j;
				break;
			}
		}
		if( previousMap >= 0 )
		{
			nodes[i].geometryCRC = nodes[previousMap].geometryCRC;
			nodes[i].landmarkCRC = nodes[previousMap].landmarkCRC;
		}
		else if( !GraphMapFingerprint( nodes[i].map, nodes[i].geometryCRC, nodes[i].landmarkCRC ) )
		{
			gameLocal.Warning( "Cannot start dungeon: invalid map %s", nodes[i].map.c_str() );
			*this = previous;
			return false;
		}
	}
	persistentHub = hub;
	instanceId = worldStore.AllocateInstanceId( seed );
	createdWorldTime = gameLocal.GetWorldTime();
	parentMap = returnMap;
	parentLandmark = returnLandmark;
	nodes[0].visits = 1;
	partyNode = 0;
	mapTransition_t transition;
	transition.nextMap = nodes[0].map;
	transition.departLandmark = returnLandmark;
	const graphMap_t* rootMap = NodeMap( decl, 0 );
	for( int i = 0; rootMap && i < rootMap->landmarks.Num(); i++ )
	{
		if( rootMap->landmarks[i].flags & 1 )
		{
			transition.arrivalLandmark = rootMap->landmarks[i].id;
			break;
		}
	}
	partyLandmark = transition.arrivalLandmark;
	recipeValid = false;
	worldStore.continueNextLoad = true;
	if( !gameLocal.BeginMapTransition( transition ) )
	{
		*this = previous;
		worldStore.continueNextLoad = previousContinue;
		return false;
	}
	//puncho - P2.4 (bug review 2026-10-28): started from inside an instance (a suspended one is refused above), the
	// old instance can never be re-entered; retire its map records on arrival as leaving it through its end does
	if( previous.instanceId != 0 )
	{
		worldStore.RetireInstanceAfterActivation( previous.instanceId, transition.nextMap );
	}
	return true;
}
int idMapGraphDirector::ResolveLink( int node, const char* landmark ) const
{
	int fallback = -1;
	for( int i = 0; i < links.Num(); i++ )
	{
		const dungeonLink_t& l = links[i];
		if( l.from != node || l.fromLandmark.Icmp( landmark ) || l.state == 1 || l.state == 3 )
		{
			continue;
		}
		if( l.conditionMode && l.condFlag >= 0 && ( l.conditionMode == 1 ? flagValues[l.condFlag] != 0 : flagValues[l.condFlag] == l.condValue ) )
		{
			return i;
		}
		if( !l.conditionMode && l.state == 2 && fallback < 0 )
		{
			fallback = i;
		}
	}
	return fallback;
}
bool idMapGraphDirector::RequestTransition( const char* landmark )
{
	if( common->IsClient() || !worldStore.IsActive() )
	{
		refused++;
		return false;
	}
	const landmark_t* lm = worldStore.FindLandmark( landmark );
	if( !lm )
	{
		gameLocal.Warning( "unknown exit %s", landmark );
		refused++;
		return false;
	}
	const idMapGraphDirector previous = *this;
	const bool previousContinue = worldStore.continueNextLoad;
	mapTransition_t t;
	t.departLandmark = landmark;
	if( instanceId && !suspended && portalOpen && !idStr::Icmp( landmark, "_townportal" ) )
	{
		t.nextMap = parentMap;
		t.arrivalLandmark = "townportal";
		suspended = true;
	}
	if( t.nextMap.IsEmpty() && instanceId && !suspended && partyNode == 0 )
	{
		if( lm->kind == instanceEndKind && !parentMap.IsEmpty() )
		{
			t.nextMap = parentMap;
			t.arrivalLandmark = parentLandmark;
			idList<byte, TAG_GAME> hub = persistentHub;
			if( hub.Num() )
			{
				ReadRecord( hub.Ptr(), hub.Num() );
			}
			else
			{
				Clear();
			}
		}
	}
	if( t.nextMap.IsEmpty() && suspended && portalOpen && portalNode != 0xffff && !idStr::Icmp( landmark, "townportal" ) && GraphMapName( gameLocal.GetMapName() ) == parentMap )
	{
		partyNode = portalNode;
		t.nextMap = nodes[partyNode].map;
		t.arrivalLandmark = portalLandmark;
		portalOpen = false;
		suspended = false;
	}
	if( t.nextMap.IsEmpty() && ( !instanceId || suspended ) && !lm->link.IsEmpty() )
	{
		if( lm->link[0] == '@' )
		{
			return Start( lm->link.c_str() + 1, 0, gameLocal.GetMapName(), landmark );
		}
		const int colon = lm->link.Find( ':' );
		if( colon > 0 )
		{
			t.nextMap = lm->link.Left( colon );
			t.arrivalLandmark = lm->link.Mid( colon + 1, lm->link.Length() );
		}
	}
	if( t.nextMap.IsEmpty() )
	{
		const int index = ResolveLink( partyNode, landmark );
		if( index >= 0 && links[index].to < nodes.Num() )
		{
			partyNode = links[index].to;
			t.nextMap = nodes[partyNode].map;
			t.arrivalLandmark = links[index].toLandmark;
			nodes[partyNode].visits = Min( 65535, ( int )nodes[partyNode].visits + 1 );
		}
	}
	if( t.nextMap.IsEmpty() )
	{
		gameLocal.Printf( "Exit '%s' is closed.\n", landmark );
		idPlayer* player = gameLocal.GetLocalPlayer();
		if( player )
		{
			player->ShowTip( "Exit closed", "This route is not available.", true );
		}
		refused++;
		return false;
	}
	if( instanceId && !suspended && partyNode < nodes.Num() && t.nextMap == nodes[partyNode].map )
	{
		uint32 geometry = 0, landmarkHash = 0;
		if( !GraphMapFingerprint( t.nextMap, geometry, landmarkHash ) || geometry != nodes[partyNode].geometryCRC || landmarkHash != nodes[partyNode].landmarkCRC )
		{
			gameLocal.Warning( "Ending dungeon: map geometry or landmarks changed in %s", t.nextMap.c_str() );
			t.nextMap = parentMap;
			t.arrivalLandmark = parentLandmark;
			idList<byte, TAG_GAME> hub = persistentHub;
			if( hub.Num() )
			{
				ReadRecord( hub.Ptr(), hub.Num() );
			}
			else
			{
				Clear();
			}
			if( t.nextMap.IsEmpty() )
			{
				*this = previous;
				refused++;
				return false;
			}
		}
	}
	if( !instanceId )
	{
		for( int i = 0; i < nodes.Num(); i++ )
		{
			if( nodes[i].map == GraphMapName( t.nextMap ) )
			{
				partyNode = i;
				nodes[i].visits = Min( 65535, ( int )nodes[i].visits + 1 );
				break;
			}
		}
	}
	partyLandmark = t.arrivalLandmark;
	recipeValid = false;
	worldStore.continueNextLoad = true;
	gameLocal.Printf( "dungeon transition node %d map %s arrival %s\n", partyNode, t.nextMap.c_str(), t.arrivalLandmark.c_str() );
	if( !gameLocal.BeginMapTransition( t ) )
	{
		*this = previous;
		worldStore.continueNextLoad = previousContinue;
		refused++;
		return false;
	}
	if( previous.instanceId != 0 && instanceId == 0 && !suspended )
	{
		worldStore.RetireInstanceAfterActivation( previous.instanceId, t.nextMap );
	}
	transitions++;
	return true;
}
int idMapGraphDirector::GetFlag( const char* name ) const
{
	if( suspended && persistentHub.Num() )
	{
		idMapGraphDirector hub;
		if( hub.ReadRecord( persistentHub.Ptr(), persistentHub.Num() ) )
		{
			return hub.GetFlag( name );
		}
	}
	int index = -1;
	for( int i = 0; i < flagDefinitions.Num(); i++ )
	{
		if( !flagDefinitions[i].name.Icmp( name ) )
		{
			index = flagDefinitions[i].index;
			break;
		}
	}
	return index >= 0 ? flagValues[index] : 0;
}
void idMapGraphDirector::SetFlag( const char* name, int value )
{
	if( common->IsClient() )
	{
		return;
	}
	if( suspended && persistentHub.Num() )
	{
		idMapGraphDirector hub;
		if( hub.ReadRecord( persistentHub.Ptr(), persistentHub.Num() ) )
		{
			hub.SetFlag( name, value );
			hub.WriteRecord( persistentHub );
		}
		return;
	}
	int index = -1;
	for( int i = 0; i < flagDefinitions.Num(); i++ )
	{
		if( !flagDefinitions[i].name.Icmp( name ) )
		{
			index = flagDefinitions[i].index;
			break;
		}
	}
	if( index < 0 )
	{
		gameLocal.Warning( "unknown dungeon flag %s", name );
		return;
	}
	flagValues[index] = idMath::ClampInt( 0, 65535, value );
	UpdateExitDoors( false );
	PredictNext();
}
void idMapGraphDirector::SetExitLocked( const char* landmark, bool locked )
{
	if( common->IsClient() )
	{
		return;
	}
	for( int i = 0; i < links.Num(); i++ )
	{
		if( links[i].from == partyNode && !links[i].fromLandmark.Icmp( landmark ) && links[i].state != 3 )
		{
			links[i].state = locked ? 1 : links[i].conditionMode ? 0
							 : 2;
		}
	}
	UpdateExitDoors( false );
	PredictNext();
}
int idMapGraphDirector::GetNodeVisits() const
{
	return suspended || clientRecipe ? recipe.nodeVisits : partyNode < nodes.Num() ? nodes[partyNode].visits
		   : 0;
}
void idMapGraphDirector::PredictNext()
{
	if( common->IsClient() || !worldStore.IsActive() )
	{
		return;
	}
	idStr predicted, exitName;
	if( !suspended && instanceId )
	{
		for( int i = 0; i < links.Num(); i++ )
		{
			if( links[i].from != partyNode || ResolveLink( partyNode, links[i].fromLandmark ) != i || links[i].to >= nodes.Num() )
			{
				continue;
			}
			if( predicted.IsEmpty() || links[i].fromLandmark.Cmp( exitName ) < 0 )
			{
				predicted = nodes[links[i].to].map;
				exitName = links[i].fromLandmark;
			}
		}
	}
	else
	{
		for( int i = 0; i < worldStore.landmarks.Num(); i++ )
		{
			const landmark_t& lm = worldStore.landmarks[i];
			idStr map;
			if( !( lm.flags & 2 ) )
			{
				continue;
			}
			if( suspended && portalOpen && lm.id == "townportal" && portalNode < nodes.Num() )
			{
				map = nodes[portalNode].map;
			}
			else
			{
				const int colon = lm.link.Find( ':' );
				if( colon > 0 )
				{
					map = lm.link.Left( colon );
				}
			}
			if( !map.IsEmpty() && ( predicted.IsEmpty() || lm.id.Cmp( exitName ) < 0 ) )
			{
				predicted = map;
				exitName = lm.id;
			}
		}
	}
	//puncho start - P2.4 (bug review 2026-10-28): Prefetch_Request restarts the prefetch, and every SetFlag and
	// SetExitLocked sent the reliable, so a script setting flags in a loop could overflow the peer's 63-entry reliable
	// queue and drop it. Only a change of the predicted map is announced, at most once per game frame; a change made
	// later in the same frame waits for RunFrame, which announces whatever the prediction is then
	if( predicted.IsEmpty() || predicted == predictedMap )
	{
		predictionPending = false;
		return;
	}
	if( predictedFrame == gameLocal.framenum )
	{
		predictionPending = true;
		return;
	}
	predictedMap = predicted;
	predictedFrame = gameLocal.framenum;
	predictionPending = false;
	//puncho end
	prefetches++;
	gameLocal.PrefetchMap( predicted );
	if( common->IsMultiplayer() )
	{
		byte buffer[64];
		idBitMsg message;
		message.InitWrite( buffer, sizeof( buffer ) );
		message.BeginWriting();
		message.WriteString( predicted );
		session->GetActingGameStateLobbyBase().SendReliable( GAME_RELIABLE_MESSAGE_PREFETCH, message, false );
	}
}
// Once per game frame on the server, after the events (idGameLocal::RunFrame).
void idMapGraphDirector::RunFrame()
{
	if( predictionPending && predictedFrame != gameLocal.framenum )
	{
		PredictNext();
	}
}
// atLoad snaps a sealed exit shut while the map populates, before anyone stands in it. At runtime a flag or a lock
// change goes through the door's own Lock(): its sounds, and the normal close path that stops for a player in the
// doorway, where the load-time snap teleported the door shut through them.
void idMapGraphDirector::UpdateExitDoors( bool atLoad )
{
	if( common->IsClient() || !worldStore.IsActive() )
	{
		return;
	}
	for( int i = 0; i < worldStore.landmarks.Num(); i++ )
	{
		const landmark_t& lm = worldStore.landmarks[i];
		if( lm.sealedBy.IsEmpty() )
		{
			continue;
		}
		idEntity* entity = gameLocal.FindEntity( lm.sealedBy );
		if( !entity || !entity->IsType( idDoor::Type ) )
		{
			continue;
		}
		idDoor* door = static_cast<idDoor*>( entity );
		const bool open = suspended || !instanceId ? !lm.link.IsEmpty() : ResolveLink( partyNode, lm.id ) >= 0 || ( partyNode == 0 && lm.kind == instanceEndKind );
		if( atLoad )
		{
			if( open )
			{
				door->ApplyRecordLock( false );
			}
			else
			{
				door->ApplyRecordState( false, true );
			}
		}
		else if( open )
		{
			if( door->IsLocked() )
			{
				door->Lock( 0 );
			}
		}
		else if( !door->IsLocked() || door->IsOpen() )
		{
			door->Lock( 1 );
		}
	}
}
void idMapGraphDirector::MapLoaded()
{
	// MapPopulate and a restore call this while the map loads; OpenTownPortalFor calls it in play
	const bool atLoad = gameLocal.GameState() != GAMESTATE_ACTIVE;
	const idMapFile* loaded = gameLocal.GetLoadedMap();
	uint32 loadedLandmarks;
	if( atLoad && loaded != NULL && GraphTablesFingerprint( worldStore, loadedLandmarks ) )
	{
		GraphStoreFingerprint( GraphMapName( gameLocal.GetMapName() ), loaded->GetFileTime(), loaded->GetGeometryCRC(), loadedLandmarks );
	}
	if( !common->IsClient() && !suspended && portalNode != 0xffff && portalNode == partyNode && partyNode < nodes.Num() && GraphMapName( gameLocal.GetMapName() ) == nodes[partyNode].map )
	{
		for( int i = worldStore.landmarks.Num() - 1; i >= 0; i-- )
		{
			if( worldStore.landmarks[i].id == "_townportal" )
			{
				worldStore.landmarks.RemoveIndex( i );
			}
		}
		for( int i = worldStore.transitionVolumes.Num() - 1; i >= 0; i-- )
		{
			if( worldStore.transitionVolumes[i].landmark == "_townportal" )
			{
				worldStore.transitionVolumes.RemoveIndex( i );
			}
		}
		landmark_t landmark;
		landmark.id = "_townportal";
		landmark.kind = "townportal";
		landmark.origin = portalOrigin;
		landmark.axis.Identity();
		landmark.flags = 3;
		landmark.radius = 256.0f;
		landmark.ordinal = -1;
		worldStore.landmarks.Append( landmark );
		transitionVolume_t volume;
		volume.landmark = landmark.id;
		volume.bounds = idBounds( portalOrigin - idVec3( 256, 256, 128 ), portalOrigin + idVec3( 256, 256, 128 ) );
		volume.cap = 64;
		volume.carryAll = true;
		volume.ordinal = -1;
		worldStore.transitionVolumes.Append( volume );
		if( portalOpen && !gameLocal.FindEntity( "_dungeon_townportal" ) )
		{
			idDict args;
			args.Set( "classname", "trigger_changelevel" );
			args.Set( "name", "_dungeon_townportal" );
			args.Set( "landmark", "_townportal" );
			args.SetVector( "origin", portalOrigin );
			args.Set( "mins", "-32 -32 0" );
			args.Set( "maxs", "32 32 96" );
			args.Set( "wait_party", "1" );
			args.Set( "persist_lifetime", "0" );
			gameLocal.SpawnEntityDef( args );
		}
	}
	UpdateExitDoors( atLoad );
	PredictNext();
}
bool idMapGraphDirector::GetResetPolicy( const char* map, int& policy, uint32& seconds ) const
{
	const idStr name = GraphMapName( map );
	if( partyNode < nodes.Num() && nodes[partyNode].map == name )
	{
		policy = nodes[partyNode].resetPolicy;
		seconds = nodes[partyNode].resetSeconds;
		return true;
	}
	return false;
}
bool idMapGraphDirector::InhibitSpawn( const idDict& args, const idMapFile* map ) const
{
	if( recipe.worldName.IsEmpty() )
	{
		return false;
	}
	const idDeclMapGraph* decl = FindGraph( recipe.worldName );
	const char* keys[2] = { "dungeon_if", "dungeon_unless" };
	for( int k = 0; k < 2; k++ )
	{
		const char* flag = args.GetString( keys[k] );
		if( !flag[0] )
		{
			continue;
		}
		const int index = decl ? decl->FindFlag( flag ) : -1;
		if( index < 0 || index >= 64 )
		{
			gameLocal.Warning( "invalid variant flag %s", flag );
			inhibited++;
			return true;
		}
		const bool value = ( recipe.variantFlags & ( 1ULL << index ) ) != 0;
		if( k == 0 ? !value : value )
		{
			inhibited++;
			return true;
		}
	}
	const idStr variant = args.GetString( "dungeon_variant" );
	const int colon = variant.Find( ':' );
	if( variant.IsEmpty() )
	{
		return false;
	}
	if( colon <= 0 || colon >= variant.Length() - 1 || !map )
	{
		inhibited++;
		return true;
	}
	idStr group = variant.Left( colon );
	group.ToLower();
	const idStr choice = variant.Mid( colon + 1, variant.Length() );
	idStrList choices;
	for( int i = 0; i < map->GetNumEntities(); i++ )
	{
		const idStr v = map->GetEntity( i )->epairs.GetString( "dungeon_variant" );
		const int c = v.Find( ':' );
		if( c <= 0 || v.Left( c ).Icmp( group ) )
		{
			continue;
		}
		const idStr candidate = v.Mid( c + 1, v.Length() );
		int at = 0;
		while( at < choices.Num() && choices[at].Icmp( candidate ) < 0 )
		{
			at++;
		}
		if( at == choices.Num() || choices[at].Icmp( candidate ) )
		{
			choices.Insert( candidate, at );
		}
	}
	if( choices.Num() == 0 )
	{
		inhibited++;
		return true;
	}
	const uint64 seed = Hash( recipe.rootSeed, recipe.currentNode, 0, 0 );
	const uint64 pick = Mix64( seed ^ CRC32_BlockChecksum( group.c_str(), group.Length() ) ^ 3 ) % choices.Num();
	const bool hide = choices[( int )pick].Icmp( choice ) != 0;
	inhibited += hide ? 1 : 0;
	return hide;
}
bool idMapGraphDirector::QueueEffect( const char* map, const char* entity, const char* action )
{
	if( common->IsClient() || !worldStore.IsActive() || idStr::Length( map ) > 63 || !entity[0] || idStr::Length( entity ) > 63 )
	{
		return false;
	}
	dungeonPending_t p;
	p.targetMap = GraphEffectMapName( map );
	p.targetName = entity;
	p.targetNode = 0xffff;
	p.action = !idStr::Icmp( action, "activate" ) ? 0 : !idStr::Icmp( action, "remove" ) ? 1
			   : !idStr::Icmp( action, "show" )	 ? 2
			   : !idStr::Icmp( action, "hide" )	 ? 3
			   : 255;
	if( p.action == 255 || p.targetMap.IsEmpty() || p.targetMap.Length() > 63 )
	{
		return false;
	}
	for( int i = 0; i < nodes.Num(); i++ )
	{
		if( GraphEffectMatches( p.targetMap, nodes[i].map ) )
		{
			p.targetNode = i;
			break;
		}
	}
	CRC32_InitChecksum( p.id );
	CRC32_UpdateChecksum( p.id, p.targetMap.c_str(), p.targetMap.Length() + 1 );
	CRC32_UpdateChecksum( p.id, p.targetName.c_str(), p.targetName.Length() + 1 );
	CRC32_UpdateChecksum( p.id, &p.action, 1 );
	CRC32_FinishChecksum( p.id );
	for( int i = 0; i < pending.Num(); i++ )
	{
		if( pending[i].id == p.id )
		{
			return true;
		}
	}
	if( pending.Num() >= 1024 )
	{
		gameLocal.Warning( "dungeon pending limit reached" );
		return false;
	}
	p.queuedWorldTime = gameLocal.GetWorldTime();
	int at = 0;
	while( at < pending.Num() && ( pending[at].queuedWorldTime < p.queuedWorldTime || ( pending[at].queuedWorldTime == p.queuedWorldTime && pending[at].id < p.id ) ) )
	{
		at++;
	}
	pending.Insert( p, at );
	ApplyPending();
	return true;
}
void idMapGraphDirector::ApplyPending()
{
	if( common->IsClient() || !worldStore.IsActive() )
	{
		return;
	}
	const idStr current = GraphMapName( gameLocal.GetMapName() );
	if( suspended && persistentHub.Num() )
	{
		idMapGraphDirector hub;
		if( hub.ReadRecord( persistentHub.Ptr(), persistentHub.Num() ) )
		{
			for( int i = 0; i < hub.nodes.Num(); i++ )
			{
				if( hub.nodes[i].map == current )
				{
					hub.partyNode = i;
					break;
				}
			}
			hub.ApplyPending();
			hub.WriteRecord( persistentHub );
		}
	}
	//puncho - P2.4 (bug review 2026-10-28): an ACTIVATE is posted with a live player as activator, 1 ms out, as stock
	// idPlayer::Event_LevelTrigger does. MapPopulate calls this before any player exists, before the events and the
	// map's main(); an ACTIVATE then stays pending until a player's spawn calls this again (idPlayer::Event_LevelTrigger)
	idEntity* activator = NULL;
	for( int n = 0; n < MAX_CLIENTS; n++ )
	{
		if( gameLocal.entities[n] && gameLocal.entities[n]->health > 0 )
		{
			activator = gameLocal.entities[n];
			break;
		}
	}
	for( int i = 0; i < pending.Num(); )
	{
		const dungeonPending_t p = pending[i];
		if( !GraphEffectMatches( p.targetMap, current ) || ( p.targetNode != 0xffff && p.targetNode != partyNode ) || ( p.action == 0 && activator == NULL ) )
		{
			i++;
			continue;
		}
		// Consume before dispatch, so a dispatch that queues another effect cannot meet this one again.
		pending.RemoveIndex( i );
		idEntity* ent = gameLocal.FindEntity( p.targetName );
		if( !ent )
		{
			gameLocal.Warning( "pending target %s:%s missing", p.targetMap.c_str(), p.targetName.c_str() );
			continue;
		}
		if( p.action == 0 )
		{
			ent->PostEventMS( &EV_Activate, 1, activator );
		}
		else if( p.action == 1 )
		{
			//puncho - P2.4 (bug review 2026-10-28): posted, as stock remove() is; a synchronous delete could free the
			// activator inside its own activation chain
			ent->PostEventMS( &EV_Remove, 0 );
		}
		else if( p.action == 2 )
		{
			ent->Show();
		}
		else
		{
			ent->Hide();
		}
	}
}
void idMapGraphDirector::OpenTownPortal()
{
	if( common->IsMultiplayer() )
	{
		gameLocal.Warning( "openTownPortal() is SP-only; multiplayer scripts must use openTownPortalFor( caster )" );
		return;
	}
	OpenTownPortalFor( gameLocal.GetLocalPlayer() );
}
void idMapGraphDirector::OpenTownPortalFor( idEntity* caster )
{
	if( common->IsClient() || !instanceId || suspended || parentMap.IsEmpty() || partyNode >= nodes.Num() )
	{
		return;
	}
	if( !caster || !caster->IsType( idPlayer::Type ) || caster->health <= 0 || caster->entityNumber < 0 || caster->entityNumber >= MAX_CLIENTS || gameLocal.entities[caster->entityNumber] != caster )
	{
		gameLocal.Warning( "openTownPortalFor requires a live player in this session" );
		return;
	}
	if( common->IsMultiplayer() && !session->GetActingGameStateLobbyBase().IsLobbyUserValid( gameLocal.lobbyUserIDs[caster->entityNumber] ) )
	{
		gameLocal.Warning( "openTownPortalFor caster is not in this session" );
		return;
	}
	portalNode = partyNode;
	portalLandmark = "_townportal";
	portalOrigin = caster->GetPhysics()->GetOrigin();
	portalOpen = true;
	idEntity* previous = gameLocal.FindEntity( "_dungeon_townportal" );
	if( previous )
	{
		delete previous;
	}
	MapLoaded();
}

uint32 idMapGraphDirector::WorldDigest() const
{
	if( recipe.worldName.IsEmpty() )
	{
		return 0;
	}
	const idDeclMapGraph* decl = FindGraph( recipe.worldName );
	if( !decl )
	{
		return 0;
	}
	static idStr cachedName;
	static uint32 cachedText = 0, cachedCRC = 0;
	if( cachedName == decl->GetName() && cachedText == decl->textCRC )
	{
		return cachedCRC;
	}
	uint32 crc;
	CRC32_InitChecksum( crc );
	CRC32_UpdateChecksum( crc, decl->GetName(), idStr::Length( decl->GetName() ) + 1 );
	uint32 le = LittleLong( decl->textCRC );
	CRC32_UpdateChecksum( crc, &le, sizeof( le ) );
	const idDict* aas = gameLocal.FindEntityDefDict( "aas_types", false );
	for( int k = 0; k < decl->kinds.Num(); k++ )
	{
		for( int m = 0; m < decl->kinds[k].maps.Num(); m++ )
		{
			const idStr& map = decl->kinds[k].maps[m].name;
			CRC32_UpdateChecksum( crc, map.c_str(), map.Length() + 1 );
			idStrList extensions;
			extensions.Append( "cm" );
			for( int a = 0; aas && a < aas->GetNumKeyVals(); a++ )
			{
				const idKeyValue* kv = aas->GetKeyVal( a );
				if( !kv->GetKey().Icmpn( "type", 4 ) )
				{
					extensions.Append( kv->GetValue() );
				}
			}
			for( int a = 0; a < extensions.Num(); a++ )
			{
				idStr file = map;
				file.SetFileExtension( extensions[a] );
				uint32 headerCRC = 0xffffffffU;
				idFile* source = fileSystem->OpenFileRead( file );
				if( source )
				{
					char header[512];
					const int length = source->Read( header, sizeof( header ) - 1 );
					header[Max( 0, length )] = 0;
					idLexer lexer( header, Max( 0, length ), file, LEXFL_NOERRORS | LEXFL_NOWARNINGS | LEXFL_NOFATALERRORS );
					idToken token;
					if( lexer.ReadToken( &token ) && lexer.ReadToken( &token ) && lexer.ReadToken( &token ) && token.type == TT_NUMBER )
					{
						headerCRC = ( uint32 )strtoull( token.c_str(), NULL, 10 );
					}
					fileSystem->CloseFile( source );
				}
				le = LittleLong( headerCRC );
				CRC32_UpdateChecksum( crc, &le, sizeof( le ) );
			}
		}
	}
	CRC32_FinishChecksum( crc );
	cachedName = decl->GetName();
	cachedText = decl->textCRC;
	cachedCRC = crc;
	return crc;
}
void idMapGraphDirector::Dump( idFile* file ) const
{
	if( !file )
	{
		return;
	}
	file->Printf( "graph %s seed %llu crc %08x\n", graphName.c_str(), ( unsigned long long )rootSeed, graphCRC );
	for( int i = 0; i < nodes.Num(); i++ )
	{
		file->Printf( "node %d %s kind %d depth %d reset %d %u\n", i, nodes[i].map.c_str(), nodes[i].kindIndex, nodes[i].depth, nodes[i].resetPolicy, nodes[i].resetSeconds );
	}
	for( int i = 0; i < links.Num(); i++ )
	{
		const dungeonLink_t& l = links[i];
		file->Printf( "link %d %s %d %s kind %d state %d condition %d %d %d\n", l.from, l.fromLandmark.c_str(), l.to, l.toLandmark.c_str(), l.kind, l.state, l.condFlag, l.conditionMode, l.condValue );
	}
}
void idMapGraphDirector::Init()
{
	transitions = refused = inhibited = prefetches = 0;
	Clear();
#if !defined( ID_RETAIL )
	cmdSystem->AddCommand( "mapGraphPortalTest", MapGraphPortalTest_f, CMD_FL_GAME | CMD_FL_CHEAT, "verify an explicit player portal caster" );
	cmdSystem->AddCommand( "mapGraphTest", MapGraphTest_f, CMD_FL_GAME, "test graph replay and bounded record round trips" );
#endif
	cmdSystem->AddCommand( "mapGraphStart", MapGraphStart_f, CMD_FL_GAME | CMD_FL_CHEAT, "start a generated dungeon" );
	cmdSystem->AddCommand( "mapGraphDump", MapGraphDump_f, CMD_FL_GAME, "dump deterministic graph topology" );
	cmdSystem->AddCommand( "listMapGraphs", ListMapGraphs_f, CMD_FL_GAME, "list mapGraph declarations" );
	cmdSystem->AddCommand( "listDungeon", ListDungeon_f, CMD_FL_GAME, "list current dungeon" );
	cmdSystem->AddCommand( "setDungeonFlag", SetDungeonFlag_f, CMD_FL_GAME | CMD_FL_CHEAT, "set an absolute dungeon flag" );
}
void idMapGraphDirector::Shutdown()
{
#if !defined( ID_RETAIL )
	cmdSystem->RemoveCommand( "mapGraphPortalTest" );
	cmdSystem->RemoveCommand( "mapGraphTest" );
#endif
	cmdSystem->RemoveCommand( "mapGraphStart" );
	cmdSystem->RemoveCommand( "mapGraphDump" );
	cmdSystem->RemoveCommand( "listMapGraphs" );
	cmdSystem->RemoveCommand( "listDungeon" );
	cmdSystem->RemoveCommand( "setDungeonFlag" );
	graphFingerprints.Clear();
	Clear();
}

int idGameLocal::WriteMatchGameData( const char* mapName, byte* out, int max )
{
	return mapGraphDirector.WriteMatchGameData( mapName, out, max );
}
void idGameLocal::SetMatchGameData( const byte* data, int size )
{
	if( !mapGraphDirector.SetMatchGameData( data, size ) )
	{
		Error( "Malformed dungeon recipe" );
	}
}
