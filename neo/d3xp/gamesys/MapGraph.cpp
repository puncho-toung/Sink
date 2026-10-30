#include "../../idlib/precompiled.h"
#pragma hdrstop

#include "../Game_local.h"
#include "MapGraph.h"
#include "WorldStore.h"
#include "MapGraphLocal.h"

idMapGraphDirector mapGraphDirector;
idCVar g_dungeonSeed( "g_dungeonSeed", "0", CVAR_GAME | CVAR_INTEGER | CVAR_NOCHEAT, "fixed dungeon seed; zero chooses once at instance start" );

static bool GraphName( idLexer& src, idStr& value )
{
	idToken token;
	if( !src.ReadToken( &token ) || token.Length() == 0 || token.Length() > 63 )
	{
		src.Warning( "expected a name of 1..63 bytes" );
		return false;
	}
	value = token;
	return true;
}
static bool GraphNumber( idLexer& src, uint32& value, uint32 min, uint32 max )
{
	idToken token;
	if( !src.ReadToken( &token ) || token.type != TT_NUMBER || !( token.subtype & TT_INTEGER ) )
	{
		return false;
	}
	const uint64 n = strtoull( token.c_str(), NULL, 10 );
	if( n < min || n > max )
	{
		return false;
	}
	value = ( uint32 )n;
	return true;
}
// The one spelling of a map name that every comparison and file path in the graph uses: lower case, forward
// slashes, no extension, always under maps/. The match parameters carry the bare name ("mg_hub") while
// gameLocal.GetMapName() carries "maps/mg_hub.map"; with two spellings the host memoized its recipe under one
// and the client looked its node up under the other, so they inhibited different variants and desynced.
idStr GraphMapName( const char* name )
{
	idStr map = name;
	map.ToLower();
	map.BackSlashesToSlashes();
	map.StripFileExtension();
	if( map.Icmpn( "maps/", 5 ) != 0 )
	{
		map = "maps/" + map;
	}
	return map;
}
//puncho start - P2.4 (bug review 2026-10-28): the map a pending effect names. A stock target_levelTrigger names the bare
// map ("mg_hub"), which idPlayer::Event_LevelTrigger matches against the base name of whatever map loads, in any
// folder; world mode queued it as "maps/mg_hub" and never matched a map under a subfolder. A bare name keeps the stock
// meaning; a name with a path is a node map in GraphMapName's spelling and matches only that path. QueueEffect's node
// lookup and ApplyPending both match through GraphEffectMatches, so the two sides cannot disagree.
idStr GraphEffectMapName( const char* name )
{
	idStr map = name;
	map.ToLower();
	map.BackSlashesToSlashes();
	map.StripFileExtension();
	return map.Find( '/' ) < 0 ? map : GraphMapName( map );
}
bool GraphEffectMatches( const idStr& target, const idStr& graphMap )
{
	if( target.Find( '/' ) >= 0 )
	{
		return target == graphMap;
	}
	return idStr::Cmp( graphMap.c_str() + graphMap.Last( '/' ) + 1, target ) == 0;
}
//puncho end
size_t idDeclMapGraph::Size() const
{
	return sizeof( *this );
}
const char* idDeclMapGraph::DefaultDefinition() const
{
	return "{ persistent 1 }";
}
void idDeclMapGraph::FreeData()
{
	persistent = false;
	maxNodes = 256;
	maxDepth = 32;
	textCRC = 0;
	root.Clear();
	instanceEnd.Clear();
	kinds.Clear();
	rules.Clear();
	flags.Clear();
	landmarkKinds.Clear();
}
void idDeclMapGraph::Print()
{
	common->Printf( "%s: %d kinds, %d rules, %d flags\n", GetName(), kinds.Num(), rules.Num(), flags.Num() );
}
int idDeclMapGraph::FindKind( const char* name ) const
{
	for( int i = 0; i < kinds.Num(); i++ )
	{
		if( !kinds[i].name.Icmp( name ) )
		{
			return i;
		}
	}
	return -1;
}
int idDeclMapGraph::FindFlag( const char* name ) const
{
	for( int i = 0; i < flags.Num(); i++ )
	{
		if( !flags[i].name.Icmp( name ) )
		{
			return flags[i].index;
		}
	}
	return -1;
}
bool idDeclMapGraph::Parse( const char* text, int textLength, bool allowBinaryVersion )
{
	FreeData();
	idLexer src( text, textLength, GetFileName(), DECL_LEXER_FLAGS );
	idToken t;
	uint32 number;
	if( !src.SkipUntilString( "{" ) )
	{
		return false;
	}
	bool closed = false;
	while( src.ReadToken( &t ) )
	{
		if( t == "}" )
		{
			closed = true;
			break;
		}
		if( t == "persistent" )
		{
			if( !GraphNumber( src, number, 0, 1 ) )
			{
				return false;
			}
			persistent = number != 0;
		}
		else if( t == "root" )
		{
			if( !GraphName( src, root ) )
			{
				return false;
			}
		}
		else if( t == "instanceEnd" )
		{
			if( !GraphName( src, instanceEnd ) )
			{
				return false;
			}
		}
		else if( t == "maxNodes" || t == "maxDepth" )
		{
			if( !GraphNumber( src, number, t == "maxNodes" ? 1 : 0, 256 ) )
			{
				return false;
			}
			if( t == "maxNodes" )
			{
				maxNodes = number;
			}
			else
			{
				maxDepth = number;
			}
		}
		else if( t == "flag" )
		{
			graphFlag_t f;
			if( !GraphName( src, f.name ) || !GraphNumber( src, number, 0, 255 ) || FindFlag( f.name ) >= 0 )
			{
				return false;
			}
			for( int i = 0; i < flags.Num(); i++ )
			{
				if( flags[i].index == ( int )number )
				{
					return false;
				}
			}
			f.index = number;
			flags.Append( f );
		}
		else if( t == "landmarkKind" )
		{
			idStr name;
			if( !GraphName( src, name ) || landmarkKinds.FindIndex( name ) >= 0 )
			{
				return false;
			}
			landmarkKinds.Append( name );
		}
		else if( t == "nodeKind" )
		{
			graphKind_t k;
			k.cap = 256;
			k.resetPolicy = 0;
			k.resetSeconds = 0;
			if( !GraphName( src, k.name ) || FindKind( k.name ) >= 0 || !src.ExpectTokenString( "{" ) )
			{
				return false;
			}
			while( src.ReadToken( &t ) && t != "}" )
			{
				if( t == "cap" )
				{
					if( !GraphNumber( src, number, 1, 256 ) )
					{
						return false;
					}
					k.cap = number;
				}
				else if( t == "reset" )
				{
					if( !src.ReadToken( &t ) )
					{
						return false;
					}
					k.resetPolicy = t == "never" ? 0 : t == "timed" ? 1
									: t == "cleared" ? 2
									: -1;
					if( k.resetPolicy < 0 || ( k.resetPolicy && !GraphNumber( src, k.resetSeconds, 0, 0x7fffffff ) ) )
					{
						return false;
					}
				}
				else if( t == "map" )
				{
					graphMap_t m;
					m.weight = 1;
					if( !GraphName( src, m.name ) || !src.ExpectTokenString( "{" ) )
					{
						return false;
					}
					m.name = GraphMapName( m.name );
					//puncho - P2.4 (bug review 2026-10-28): the maps/ prefix can take a 63-byte name past what a record holds
					if( m.name.Length() > 63 )
					{
						src.Warning( "map name %s exceeds 63 bytes under maps/", m.name.c_str() );
						return false;
					}
					while( src.ReadToken( &t ) && t != "}" )
					{
						if( t == "weight" )
						{
							if( !GraphNumber( src, m.weight, 1, 0xffffffffU ) )
							{
								return false;
							}
						}
						else if( t == "landmark" )
						{
							graphLandmark_t l;
							l.flags = 0;
							if( !GraphName( src, l.id ) || !GraphName( src, l.kind ) )
							{
								return false;
							}
							while( src.ReadToken( &t ) )
							{
								if( t == "entry" )
								{
									l.flags |= 1;
								}
								else if( t == "exit" )
								{
									l.flags |= 2;
								}
								else if( t == "required" )
								{
									l.flags |= 4;
								}
								else
								{
									src.UnreadToken( &t );
									break;
								}
							}
							int at = 0;
							while( at < m.landmarks.Num() && m.landmarks[at].id.Cmp( l.id ) < 0 )
							{
								at++;
							}
							if( at < m.landmarks.Num() && m.landmarks[at].id == l.id )
							{
								return false;
							}
							m.landmarks.Insert( l, at );
						}
						else
						{
							src.Warning( "unknown map keyword %s", t.c_str() );
							return false;
						}
					}
					if( t != "}" || k.maps.Num() >= 256 )
					{
						return false;
					}
					for( int i = 0; i < k.maps.Num(); i++ )
					{
						if( k.maps[i].name == m.name )
						{
							return false;
						}
					}
					k.maps.Append( m );
				}
				else
				{
					src.Warning( "unknown nodeKind keyword %s", t.c_str() );
					return false;
				}
			}
			if( t != "}" || k.maps.Num() == 0 || kinds.Num() >= 256 )
			{
				return false;
			}
			kinds.Append( k );
		}
		else if( t == "rule" || t == "loop" || t == "endcap" )
		{
			graphRule_t r;
			r.weight = 1;
			r.state = 2;
			r.value = 0;
			r.loop = t == "loop";
			r.endcap = t == "endcap";
			if( !r.endcap && !GraphName( src, r.from ) )
			{
				return false;
			}
			if( !GraphName( src, r.landmark ) || ( !r.endcap && !src.ExpectTokenString( "->" ) ) || !GraphName( src, r.to ) )
			{
				return false;
			}
			while( src.ReadToken( &t ) )
			{
				if( t == "weight" )
				{
					if( !GraphNumber( src, r.weight, 1, 0xffffffffU ) )
					{
						return false;
					}
				}
				else if( t == "open" )
				{
					r.state = 2;
				}
				else if( t == "locked" )
				{
					r.state = 1;
				}
				else if( t == "if" )
				{
					if( !GraphName( src, r.flag ) )
					{
						return false;
					}
					r.value = -1;
					if( src.ReadToken( &t ) )
					{
						src.UnreadToken( &t );
						if( t.type == TT_NUMBER )
						{
							if( !GraphNumber( src, number, 0, 65535 ) )
							{
								return false;
							}
							r.value = number;
						}
					}
				}
				else
				{
					src.UnreadToken( &t );
					break;
				}
			}
			if( rules.Num() >= 1024 )
			{
				return false;
			}
			rules.Append( r );
		}
		else
		{
			src.Warning( "unknown mapGraph keyword %s", t.c_str() );
			return false;
		}
	}
	//puncho - P2.4 (bug review 2026-10-28, implementer E's request): an instanceEnd that names no declared landmarkKind
	// never matches an exit, so the instance would never end; refuse it as a rule's landmark kind is refused below
	if( !closed || src.HadError() || ( !persistent && FindKind( root ) < 0 ) || ( !instanceEnd.IsEmpty() && landmarkKinds.FindIndex( instanceEnd ) < 0 ) )
	{
		return false;
	}
	for( int i = 0; i < rules.Num(); i++ )
	{
		const graphRule_t& r = rules[i];
		if( landmarkKinds.FindIndex( r.landmark ) < 0 || FindKind( r.to ) < 0 || ( !r.endcap && FindKind( r.from ) < 0 ) || ( !r.flag.IsEmpty() && FindFlag( r.flag ) < 0 ) )
		{
			return false;
		}
	}
	for( int k = 0; k < kinds.Num(); k++ )
	{
		for( int m = 0; m < kinds[k].maps.Num(); m++ )
		{
			const graphMap_t& map = kinds[k].maps[m];
			if( map.landmarks.Num() > 1024 || map.name.Find( ".." ) >= 0 )
			{
				return false;
			}
			for( int l = 0; l < map.landmarks.Num(); l++ )
			{
				if( landmarkKinds.FindIndex( map.landmarks[l].kind ) < 0 )
				{
					return false;
				}
			}
		}
	}
	textCRC = CRC32_BlockChecksum( text, textLength );
	return true;
}

uint64 idMapGraphDirector::Mix64( uint64 x )
{
	x = ( x ^ ( x >> 30 ) ) * 0xbf58476d1ce4e5b9ULL;
	x = ( x ^ ( x >> 27 ) ) * 0x94d049bb133111ebULL;
	return x ^ ( x >> 31 );
}
uint64 idMapGraphDirector::Hash( uint64 a, uint64 b, uint64 c, uint64 d )
{
	return Mix64( a ^ Mix64( b ^ Mix64( c ^ Mix64( d ) ) ) );
}
idMapGraphDirector::idMapGraphDirector()
{
	Clear();
}
unsigned int idMapGraphDirector::transitions;
unsigned int idMapGraphDirector::refused;
unsigned int idMapGraphDirector::inhibited;
unsigned int idMapGraphDirector::prefetches;
unsigned int idMapGraphDirector::hubWrites;
unsigned int idMapGraphDirector::hubDecodes;

void idMapGraphDirector::Clear()
{
	instanceEndKind.Clear();
	flagDefinitions.Clear();
	graphName.Clear();
	parentMap.Clear();
	parentLandmark.Clear();
	partyLandmark.Clear();
	portalLandmark.Clear();
	instanceId = rootSeed = 0;
	graphCRC = 0;
	createdWorldTime = 0;
	partyNode = 0;
	portalNode = 0xffff;
	memset( flagValues, 0, sizeof( flagValues ) );
	nodes.Clear();
	links.Clear();
	pending.Clear();
	sealedRequired = 0;
	recipeValid = clientRecipe = suspended = recipeNewWorld = portalOpen = false;
	portalOrigin.Zero();
	persistentHub.Clear();
	hubFlagDefinitions.Clear();	//puncho - P2b.1.10e: the cache follows persistentHub; Clear is no write to count
	hubFlagsState = HUB_FLAGS_STALE;
	recipeMap.Clear();
	recipe.worldName.Clear();
	recipe.flags = 0;
	recipe.instanceId = recipe.rootSeed = recipe.variantFlags = 0;
	recipe.graphCRC = 0;
	recipe.currentNode = recipe.nodeVisits = 0;
	predictedMap.Clear();
	predictedFrame = -1;
	predictionPending = false;
}
const graphMap_t* idMapGraphDirector::NodeMap( const idDeclMapGraph* decl, int node ) const
{
	if( node < 0 || node >= nodes.Num() || nodes[node].kindIndex >= decl->kinds.Num() )
	{
		return NULL;
	}
	const graphKind_t& k = decl->kinds[nodes[node].kindIndex];
	for( int i = 0; i < k.maps.Num(); i++ )
	{
		if( k.maps[i].name == nodes[node].map )
		{
			return &k.maps[i];
		}
	}
	return NULL;
}
int idMapGraphDirector::AddNode( const idDeclMapGraph* decl, int kind, int depth )
{
	if( kind < 0 || kind >= decl->kinds.Num() || nodes.Num() >= decl->maxNodes || depth > decl->maxDepth )
	{
		return -1;
	}
	const graphKind_t& k = decl->kinds[kind];
	int count = 0;
	uint64 total = 0;
	for( int i = 0; i < nodes.Num(); i++ )
	{
		count += nodes[i].kindIndex == kind;
	}
	if( count >= k.cap )
	{
		return -1;
	}
	for( int i = 0; i < k.maps.Num(); i++ )
	{
		total += k.maps[i].weight;
	}
	if( total == 0 )
	{
		return -1;
	}
	uint64 pick = Hash( rootSeed, nodes.Num(), 0, 1 ) % total;
	int chosen = 0;
	for( ; chosen < k.maps.Num() - 1; chosen++ )
	{
		if( pick < k.maps[chosen].weight )
		{
			break;
		}
		pick -= k.maps[chosen].weight;
	}
	dungeonNode_t n;
	n.map = k.maps[chosen].name;
	n.kindIndex = kind;
	n.depth = depth;
	n.visits = 0;
	n.state = 0;
	n.resetPolicy = k.resetPolicy;
	n.resetSeconds = k.resetSeconds;
	n.geometryCRC = n.landmarkCRC = 0;
	return nodes.Append( n );
}
bool idMapGraphDirector::AddChild( const idDeclMapGraph* decl, int node, const graphLandmark_t& exit, const graphRule_t& rule )
{
	if( links.Num() > 1022 )
	{
		return false;
	}
	const int kind = decl->FindKind( rule.to );
	int child = -1;
	idStr entry;
	if( rule.loop )
	{
		for( int i = 0; i < nodes.Num() && child < 0; i++ )
		{
			if( i == node || nodes[i].kindIndex != kind )
			{
				continue;
			}
			const graphMap_t* m = NodeMap( decl, i );
			for( int j = 0; m && j < m->landmarks.Num(); j++ )
			{
				const graphLandmark_t& l = m->landmarks[j];
				if( !( l.flags & 1 ) || l.kind != exit.kind )
				{
					continue;
				}
				bool used = false;
				for( int p = 0; p < links.Num(); p++ )
				{
					if( links[p].from == i && links[p].fromLandmark == l.id )
					{
						used = true;
						break;
					}
				}
				if( !used )
				{
					child = i;
					entry = l.id;
					break;
				}
			}
		}
	}
	else
	{
		child = AddNode( decl, kind, nodes[node].depth + 1 );
		if( child >= 0 )
		{
			const graphMap_t* m = NodeMap( decl, child );
			for( int j = 0; m && j < m->landmarks.Num(); j++ )
			{
				if( ( m->landmarks[j].flags & 1 ) && m->landmarks[j].kind == exit.kind )
				{
					entry = m->landmarks[j].id;
					break;
				}
			}
			if( entry.IsEmpty() )
			{
				nodes.RemoveIndex( child );
				child = -1;
			}
		}
	}
	if( child < 0 )
	{
		return false;
	}
	dungeonLink_t l;
	l.from = node;
	l.to = child;
	l.fromLandmark = exit.id;
	l.toLandmark = entry;
	l.kind = 0;
	l.condFlag = rule.flag.IsEmpty() ? -1 : decl->FindFlag( rule.flag );
	l.conditionMode = l.condFlag < 0 ? 0 : rule.value < 0 ? 1
					  : 2;
	l.condValue = rule.value < 0 ? 0 : rule.value;
	l.state = l.condFlag >= 0 && rule.state != 1 ? 0 : rule.state;
	links.Append( l );
	l.from = child;
	l.to = node;
	l.fromLandmark = entry;
	l.toLandmark = exit.id;
	l.condFlag = -1;
	l.conditionMode = 0;
	l.condValue = 0;
	l.kind = 0;
	l.state = 2;
	links.Append( l );
	return true;
}
bool idMapGraphDirector::Generate( const idDeclMapGraph* decl, uint64 seed )
{
	Clear();
	if( !decl )
	{
		return false;
	}
	graphName = decl->GetName();
	graphCRC = decl->textCRC;
	rootSeed = seed;
	instanceEndKind = decl->instanceEnd;
	flagDefinitions = decl->flags;
	if( decl->persistent )
	{
		for( int k = 0; k < decl->kinds.Num(); k++ )
		{
			for( int m = 0; m < decl->kinds[k].maps.Num(); m++ )
			{
				if( nodes.Num() >= 256 )
				{
					return false;
				}
				dungeonNode_t n;
				n.map = decl->kinds[k].maps[m].name;
				n.kindIndex = k;
				n.depth = n.visits = 0;
				n.state = 0;
				n.resetPolicy = decl->kinds[k].resetPolicy;
				n.resetSeconds = decl->kinds[k].resetSeconds;
				n.geometryCRC = n.landmarkCRC = 0;
				nodes.Append( n );
			}
		}
		return true;
	}
	if( AddNode( decl, decl->FindKind( decl->root ), 0 ) < 0 )
	{
		return false;
	}
	for( int n = 0; n < nodes.Num(); n++ )
	{
		// Copy: adding a child may relocate nodes, but decl maps remain immutable.
		const graphMap_t* m = NodeMap( decl, n );
		const idStr fromKind = decl->kinds[nodes[n].kindIndex].name;
		for( int e = 0; m && e < m->landmarks.Num(); e++ )
		{
			const graphLandmark_t& exit = m->landmarks[e];
			if( !( exit.flags & 2 ) )
			{
				continue;
			}
			bool linked = false;
			for( int p = 0; p < links.Num(); p++ )
			{
				if( links[p].from == n && links[p].fromLandmark == exit.id )
				{
					linked = true;
					break;
				}
			}
			if( linked || ( n == 0 && exit.kind == decl->instanceEnd ) )
			{
				continue;
			}
			uint64 total = 0;
			bool loopLinked = false;
			for( int r = 0; r < decl->rules.Num(); r++ )
			{
				const graphRule_t& rule = decl->rules[r];
				// kinds are matched without case, as Parse validated rule.from with FindKind
				if( rule.endcap || rule.from.Icmp( fromKind ) != 0 || rule.landmark != exit.kind )
				{
					continue;
				}
				if( rule.loop )
				{
					if( !loopLinked )
					{
						loopLinked = AddChild( decl, n, exit, rule );
						linked |= loopLinked;
					}
				}
				else if( !rule.flag.IsEmpty() )
				{
					linked |= AddChild( decl, n, exit, rule );
				}
				else
				{
					total += rule.weight;
				}
			}
			if( total && !loopLinked )
			{
				uint64 pick = Hash( seed, n, e, 2 ) % total;
				for( int r = 0; r < decl->rules.Num(); r++ )
				{
					const graphRule_t& rule = decl->rules[r];
					if( rule.endcap || rule.loop || !rule.flag.IsEmpty() || rule.from.Icmp( fromKind ) != 0 || rule.landmark != exit.kind )
					{
						continue;
					}
					if( pick < rule.weight )
					{
						linked |= AddChild( decl, n, exit, rule );
						break;
					}
					pick -= rule.weight;
				}
			}
			if( !linked && ( exit.flags & 4 ) )
			{
				for( int r = 0; r < decl->rules.Num(); r++ )
				{
					if( decl->rules[r].endcap && decl->rules[r].landmark == exit.kind )
					{
						linked = AddChild( decl, n, exit, decl->rules[r] );
						break;
					}
				}
				if( !linked )
				{
					sealedRequired++;
				}
			}
			if( !linked )
			{
				if( links.Num() >= 1024 )
				{
					return false;
				}
				dungeonLink_t l;
				l.from = n;
				l.to = 0xffff;
				l.fromLandmark = exit.id;
				l.kind = 0;
				l.state = 3;
				l.condFlag = -1;
				l.conditionMode = 0;
				l.condValue = 0;
				links.Append( l );
			}
		}
	}
	return true;
}
