#include "../../idlib/precompiled.h"
#pragma hdrstop

#include "../Game_local.h"
#include "MapGraph.h"
#include "WorldStore.h"

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
static idStr GraphMapName( const char* name )
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
static idStr GraphEffectMapName( const char* name )
{
	idStr map = name;
	map.ToLower();
	map.BackSlashesToSlashes();
	map.StripFileExtension();
	return map.Find( '/' ) < 0 ? map : GraphMapName( map );
}
static bool GraphEffectMatches( const idStr& target, const idStr& graphMap )
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

// Fixed-width little-endian records. Every read checks the remaining bytes before
// advancing; restore parses into a temporary director and publishes only on success.
class idGraphBytes
{
public:
	idGraphBytes( idList<byte, TAG_GAME>& dest ) : output( &dest ), data( NULL ), size( 0 ), pos( 0 ), good( true )
	{
		dest.Clear();
	}
	idGraphBytes( const byte* bytes, int count ) : output( NULL ), data( bytes ), size( count ), pos( 0 ), good( count >= 0 && ( bytes != NULL || count == 0 ) )
	{
	}
	void Number( uint64& value, int count )
	{
		if( output )
		{
			Reserve( count );
			for( int i = 0; i < count; i++ )
			{
				output->Append( ( byte )( value >> ( i * 8 ) ) );
			}
		}
		else
		{
			value = 0;
			if( !good || count > size - pos )
			{
				good = false;
				return;
			}
			for( int i = 0; i < count; i++ )
			{
				value |= ( uint64 )data[pos++] << ( i * 8 );
			}
		}
	}
	uint64 Get( int count )
	{
		uint64 n = 0;
		Number( n, count );
		return n;
	}
	void Put( uint64 n, int count )
	{
		Number( n, count );
	}
	void String( idStr& str, int max = 63 )
	{
		if( output )
		{
			if( str.Length() > max )
			{
				good = false;
				return;
			}
			Put( str.Length(), 1 );
			Reserve( str.Length() );
			for( int i = 0; i < str.Length(); i++ )
			{
				output->Append( ( byte )str[i] );
			}
		}
		else
		{
			const int len = Get( 1 );
			str.Clear();
			if( !good || len > max || len > size - pos )
			{
				good = false;
				return;
			}
			for( int i = 0; i < len; i++ )
			{
				if( data[pos] == 0 )
				{
					good = false;
				}
				str.Append( ( char )data[pos++] );
			}
		}
	}
	void Text( const idStr& str, int max = 63 )
	{
		idStr copy = str;
		String( copy, max );
	}
	bool Good() const
	{
		return good && ( output || pos == size );
	}
	bool ReadingOK() const
	{
		return good;
	}

private:
	//puncho - P2.4 (bug review 2026-10-28): idList grows by its granularity (16), so a byte-by-byte record of a few
	// hundred KiB reallocated and copied thousands of times; the capacity doubles instead
	void Reserve( int count )
	{
		if( output->Num() + count > output->NumAllocated() )
		{
			output->Resize( Max( 256, ( output->Num() + count ) * 2 ) );
		}
	}
	idList<byte, TAG_GAME>* output;
	const byte* data;
	int size, pos;
	bool good;
};
bool idMapGraphDirector::WriteRecord( idList<byte, TAG_GAME>& bytes ) const
{
	if( nodes.Num() > 256 || links.Num() > 1024 || pending.Num() > 1024 || flagDefinitions.Num() > 256 )
	{
		return false;
	}
	idGraphBytes b( bytes );
	b.Put( 2, 2 );
	b.Put( instanceId, 8 );
	b.Put( rootSeed, 8 );
	b.Text( graphName );
	b.Put( graphCRC, 4 );
	b.Put( 1, 2 );
	b.Put( createdWorldTime, 8 );
	b.Text( parentMap );
	b.Text( parentLandmark );
	b.Put( nodes.Num(), 2 );
	for( int i = 0; i < nodes.Num(); i++ )
	{
		const dungeonNode_t& n = nodes[i];
		b.Text( n.map );
		b.Put( n.kindIndex, 2 );
		b.Put( n.resetPolicy, 1 );
		b.Put( n.resetSeconds, 4 );
		b.Put( n.state, 1 );
		b.Put( n.visits, 2 );
		b.Put( n.depth, 2 );
		b.Put( n.geometryCRC, 4 );
		b.Put( n.landmarkCRC, 4 );
	}
	b.Put( links.Num(), 2 );
	for( int i = 0; i < links.Num(); i++ )
	{
		const dungeonLink_t& l = links[i];
		b.Put( l.from, 2 );
		b.Text( l.fromLandmark );
		b.Put( l.to, 2 );
		b.Text( l.toLandmark );
		b.Put( l.kind, 1 );
		b.Put( l.state, 1 );
		b.Put( l.condFlag < 0 ? 0 : l.condFlag, 1 );
		b.Put( l.conditionMode, 1 );
		b.Put( l.condValue, 2 );
	}
	int count = 0;
	for( int i = 0; i < 256; i++ )
	{
		if( flagValues[i] )
		{
			count++;
		}
	}
	b.Put( count, 2 );
	for( int i = 0; i < 256; i++ )
	{
		if( flagValues[i] )
		{
			b.Put( i, 1 );
			b.Put( flagValues[i], 2 );
		}
	}
	b.Put( partyNode, 2 );
	b.Text( partyLandmark );
	b.Put( portalNode, 2 );
	b.Text( portalLandmark );
	b.Put( pending.Num(), 2 );
	for( int i = 0; i < pending.Num(); i++ )
	{
		const dungeonPending_t& p = pending[i];
		b.Put( p.id, 4 );
		b.Put( p.targetNode, 2 );
		b.Text( p.targetMap );
		b.Text( p.targetName );
		b.Put( p.action, 1 );
		b.Put( p.queuedWorldTime, 8 );
	}
	b.Text( instanceEndKind );
	b.Put( flagDefinitions.Num(), 2 );
	for( int i = 0; i < flagDefinitions.Num(); i++ )
	{
		b.Text( flagDefinitions[i].name );
		b.Put( flagDefinitions[i].index, 1 );
	}
	b.Put( suspended ? 1 : 0, 1 );
	b.Put( portalOpen ? 1 : 0, 1 );
	for( int i = 0; i < 3; i++ )
	{
		uint32 bits;
		const float coordinate = portalOrigin[i];
		memcpy( &bits, &coordinate, 4 );
		b.Put( bits, 4 );
	}
	return b.Good();
}
bool idMapGraphDirector::ReadRecord( const byte* data, int size )
{
	if( size <= 0 || size > 512 * 1024 )
	{
		return false;
	}
	idMapGraphDirector temp;
	idGraphBytes b( data, size );
	if( b.Get( 2 ) != 2 )
	{
		return false;
	}
	temp.instanceId = b.Get( 8 );
	temp.rootSeed = b.Get( 8 );
	b.String( temp.graphName );
	temp.graphCRC = b.Get( 4 );
	if( b.Get( 2 ) != 1 )
	{
		return false;
	}
	temp.createdWorldTime = ( int64 )b.Get( 8 );
	b.String( temp.parentMap );
	b.String( temp.parentLandmark );
	int count = b.Get( 2 );
	if( count > 256 )
	{
		return false;
	}
	for( int i = 0; i < count; i++ )
	{
		dungeonNode_t n;
		b.String( n.map );
		n.kindIndex = b.Get( 2 );
		n.resetPolicy = b.Get( 1 );
		n.resetSeconds = b.Get( 4 );
		n.state = b.Get( 1 );
		n.visits = b.Get( 2 );
		n.depth = b.Get( 2 );
		n.geometryCRC = b.Get( 4 );
		n.landmarkCRC = b.Get( 4 );
		if( !b.ReadingOK() || n.map.IsEmpty() || n.kindIndex >= 256 || n.resetPolicy > 2 || n.state > 3 || n.depth > 256 )
		{
			return false;
		}
		temp.nodes.Append( n );
	}
	count = b.Get( 2 );
	if( count > 1024 )
	{
		return false;
	}
	for( int i = 0; i < count; i++ )
	{
		dungeonLink_t l;
		l.from = b.Get( 2 );
		b.String( l.fromLandmark );
		l.to = b.Get( 2 );
		b.String( l.toLandmark );
		l.kind = b.Get( 1 );
		l.state = b.Get( 1 );
		l.condFlag = b.Get( 1 );
		l.conditionMode = b.Get( 1 );
		l.condValue = b.Get( 2 );
		if( l.conditionMode == 0 )
		{
			l.condFlag = -1;
		}
		if( !b.ReadingOK() || l.from >= temp.nodes.Num() || ( l.to != 0xffff && l.to >= temp.nodes.Num() ) || l.state > 3 || l.kind > 1 || l.conditionMode > 2 || l.fromLandmark.IsEmpty() || ( l.to == 0xffff && l.state != 3 ) )
		{
			return false;
		}
		temp.links.Append( l );
	}
	count = b.Get( 2 );
	if( count > 256 )
	{
		return false;
	}
	int previous = -1;
	for( int i = 0; i < count; i++ )
	{
		const int index = b.Get( 1 );
		const uint16 value = b.Get( 2 );
		if( index <= previous || !value )
		{
			return false;
		}
		temp.flagValues[index] = value;
		previous = index;
	}
	temp.partyNode = b.Get( 2 );
	b.String( temp.partyLandmark );
	temp.portalNode = b.Get( 2 );
	b.String( temp.portalLandmark );
	if( ( temp.nodes.Num() && temp.partyNode >= temp.nodes.Num() ) || ( temp.portalNode != 0xffff && temp.portalNode >= temp.nodes.Num() ) )
	{
		return false;
	}
	count = b.Get( 2 );
	if( count > 1024 )
	{
		return false;
	}
	for( int i = 0; i < count; i++ )
	{
		dungeonPending_t p;
		p.id = b.Get( 4 );
		p.targetNode = b.Get( 2 );
		b.String( p.targetMap );
		b.String( p.targetName );
		p.action = b.Get( 1 );
		p.queuedWorldTime = ( int64 )b.Get( 8 );
		if( !b.ReadingOK() || p.action > 3 || p.targetMap.IsEmpty() || p.targetName.IsEmpty() || ( p.targetNode != 0xffff && p.targetNode >= temp.nodes.Num() ) )
		{
			return false;
		}
		for( int j = 0; j < temp.pending.Num(); j++ )
		{
			if( temp.pending[j].id == p.id )
			{
				return false;
			}
		}
		if( temp.pending.Num() )
		{
			const dungeonPending_t& prev = temp.pending[temp.pending.Num() - 1];
			if( p.queuedWorldTime < prev.queuedWorldTime || ( p.queuedWorldTime == prev.queuedWorldTime && p.id <= prev.id ) )
			{
				return false;
			}
		}
		temp.pending.Append( p );
	}
	b.String( temp.instanceEndKind );
	count = b.Get( 2 );
	if( count > 256 )
	{
		return false;
	}
	for( int i = 0; i < count; i++ )
	{
		graphFlag_t f;
		b.String( f.name );
		f.index = b.Get( 1 );
		if( f.name.IsEmpty() )
		{
			return false;
		}
		for( int j = 0; j < temp.flagDefinitions.Num(); j++ )
		{
			if( f.index == temp.flagDefinitions[j].index || !f.name.Icmp( temp.flagDefinitions[j].name ) )
			{
				return false;
			}
		}
		temp.flagDefinitions.Append( f );
	}
	const int suspendState = b.Get( 1 );
	const int portalState = b.Get( 1 );
	if( suspendState > 1 || portalState > 1 )
	{
		return false;
	}
	temp.suspended = suspendState != 0;
	temp.portalOpen = portalState != 0;
	for( int i = 0; i < 3; i++ )
	{
		const uint32 bits = b.Get( 4 );
		memcpy( &temp.portalOrigin[i], &bits, 4 );
		if( ( bits & 0x7f800000U ) == 0x7f800000U )
		{
			return false;
		}
	}
	if( !b.Good() )
	{
		return false;
	}
	*this = temp;
	return true;
}
void idMapGraphDirector::StoreRecord()
{
	if( common->IsClient() || !worldStore.IsActive() )
	{
		return;
	}
	//puncho - P2.4 (bug review 2026-10-28): an overlong name keeps the previous record with a warning instead of an
	// Error at the leave or the save; the parser, ReadMapData and QueueEffect refuse names over 63 bytes
	idList<byte, TAG_GAME> record;
	if( !WriteRecord( record ) )
	{
		gameLocal.Warning( "dungeon record not stored: a name exceeds 63 bytes; the previous record is kept" );
		return;
	}
	worldStore.dungeonRecords.Clear();
	if( persistentHub.Num() )
	{
		worldStore.dungeonRecords.Append( persistentHub );
	}
	worldStore.dungeonRecords.Append( record );
}
bool idMapGraphDirector::RestoreRecord()
{
	Clear();
	if( worldStore.dungeonRecords.Num() == 0 )
	{
		return true;
	}
	if( worldStore.dungeonRecords.Num() > 2 )
	{
		return false;
	}
	for( int i = 0; i < worldStore.dungeonRecords.Num(); i++ )
	{
		const idList<byte, TAG_GAME>& bytes = worldStore.dungeonRecords[i];
		idMapGraphDirector record;
		if( !record.ReadRecord( bytes.Ptr(), bytes.Num() ) )
		{
			Clear();
			return false;
		}
		if( i == 0 && worldStore.dungeonRecords.Num() == 2 )
		{
			if( record.instanceId != 0 )
			{
				return false;
			}
			persistentHub = bytes;
		}
		else
		{
			idList<byte, TAG_GAME> hub = persistentHub;
			*this = record;
			persistentHub = hub;
		}
	}
	recipeValid = false;
	const bool continuation = worldStore.continueNextLoad;
	worldStore.continueNextLoad = true;
	RecipeForLoad( gameLocal.GetMapName() );
	worldStore.continueNextLoad = continuation;
	return true;
}

static const idDeclMapGraph* FindGraph( const char* name )
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
int idMapGraphDirector::WriteMatchGameData( const char* map, byte* out, int max )
{
	if( !clientRecipe && !worldStore.continueNextLoad )
	{
		recipeValid = false;
		recipeNewWorld = true;
	}
	const dungeonRecipe_t& r = RecipeForLoad( map );
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
static void MapGraphStart_f( const idCmdArgs& args )
{
	if( args.Argc() < 2 )
	{
		common->Printf( "mapGraphStart <graph> [seed]\n" );
		return;
	}
	mapGraphDirector.Start( args.Argv( 1 ), args.Argc() > 2 ? strtoull( args.Argv( 2 ), NULL, 0 ) : 0 );
}
static void MapGraphDump_f( const idCmdArgs& args )
{
	if( args.Argc() != 4 )
	{
		common->Printf( "mapGraphDump <graph> <seed> <file>\n" );
		return;
	}
	const idDeclMapGraph* decl = FindGraph( args.Argv( 1 ) );
	idMapGraphDirector generated;
	const uint64 start = Sys_Microseconds();
	if( !decl || !generated.Generate( decl, strtoull( args.Argv( 2 ), NULL, 0 ) ) )
	{
		common->Warning( "graph generation failed" );
		return;
	}
	idFile* file = fileSystem->OpenFileWrite( args.Argv( 3 ) );
	generated.Dump( file );
	if( file )
	{
		fileSystem->CloseFile( file );
	}
	common->Printf( "generated %d nodes %d links %d sealed required exits in %.3f ms\n", generated.nodes.Num(), generated.links.Num(), generated.sealedRequired, ( Sys_Microseconds() - start ) * 0.001 );
}
static void ListDungeon_f( const idCmdArgs& args )
{
	common->Printf( "dungeon %s node %d visits %d nodes %d links %d pending %d\n", mapGraphDirector.graphName.c_str(), mapGraphDirector.partyNode, mapGraphDirector.GetNodeVisits(), mapGraphDirector.nodes.Num(), mapGraphDirector.links.Num(), mapGraphDirector.pending.Num() );
}
static void ListMapGraphs_f( const idCmdArgs& args )
{
	declManager->ListType( args, DECL_MAPGRAPH );
}
static void SetDungeonFlag_f( const idCmdArgs& args )
{
	if( args.Argc() == 3 )
	{
		mapGraphDirector.SetFlag( args.Argv( 1 ), atoi( args.Argv( 2 ) ) );
	}
}
//puncho - P2.4 (bug review 2026-10-28): the replay and recipe self-test is developer scaffolding, not in retail builds
#if !defined( ID_RETAIL )
static void MapGraphTest_f( const idCmdArgs& args )
{
	if( args.Argc() < 2 )
	{
		common->Printf( "mapGraphTest <graph> [seeds=1000]\n" );
		return;
	}
	const idDeclMapGraph* decl = FindGraph( args.Argv( 1 ) );
	if( !decl )
	{
		common->Warning( "mapGraphTest: graph missing" );
		return;
	}
	const int seeds = args.Argc() > 2 ? idMath::ClampInt( 1, 10000, atoi( args.Argv( 2 ) ) ) : 1000;
	int failures = 0, truncations = 0;
	const uint64 start = Sys_Microseconds();
	for( int seed = 0; seed < seeds; seed++ )
	{
		idMapGraphDirector a, b, restored;
		idList<byte, TAG_GAME> first, second, roundTrip;
		if( !a.Generate( decl, seed ) || !b.Generate( decl, seed ) || !a.WriteRecord( first ) || !b.WriteRecord( second ) )
		{
			failures++;
			continue;
		}
		if( first.Num() != second.Num() || memcmp( first.Ptr(), second.Ptr(), first.Num() ) )
		{
			failures++;
		}
		if( !restored.ReadRecord( first.Ptr(), first.Num() ) || !restored.WriteRecord( roundTrip ) || first.Num() != roundTrip.Num() || memcmp( first.Ptr(), roundTrip.Ptr(), first.Num() ) )
		{
			failures++;
		}
		if( seed == 0 )
		{
			for( int length = 0; length < first.Num(); length++ )
			{
				if( restored.ReadRecord( first.Ptr(), length ) )
				{
					failures++;
				}
				truncations++;
			}
			second = first;
			second[0] = 255;
			if( restored.ReadRecord( second.Ptr(), second.Num() ) )
			{
				failures++;
			}
		}
		bool reachable[256] = { false };
		reachable[0] = a.nodes.Num() != 0;
		for( int pass = 0; pass < a.nodes.Num(); pass++ )
		{
			for( int link = 0; link < a.links.Num(); link++ )
			{
				const dungeonLink_t& l = a.links[link];
				if( l.to < a.nodes.Num() && reachable[l.from] )
				{
					reachable[l.to] = true;
				}
			}
		}
		for( int node = 0; node < a.nodes.Num(); node++ )
		{
			if( !decl->persistent && ( !reachable[node] || a.nodes[node].depth > decl->maxDepth ) )
			{
				failures++;
			}
		}
		for( int kind = 0; kind < decl->kinds.Num(); kind++ )
		{
			int count = 0;
			for( int node = 0; node < a.nodes.Num(); node++ )
			{
				count += a.nodes[node].kindIndex == kind;
			}
			if( !decl->persistent && count > decl->kinds[kind].cap )
			{
				failures++;
			}
		}
		// Flags affect resolution; the serialized topology dump must remain identical.
		idFile_Memory before, after;
		a.Dump( &before );
		a.flagValues[0] = 65535;
		a.Dump( &after );
		if( before.Length() != after.Length() || memcmp( before.GetDataPtr(), after.GetDataPtr(), before.Length() ) )
		{
			failures++;
		}
		if( seed == 0 && a.links.Num() )
		{
			a.links[0].conditionMode = 2;
			a.links[0].condFlag = 255;
			a.links[0].condValue = 65535;
			a.flagValues[255] = 65535;
			if( !a.WriteRecord( first ) || !restored.ReadRecord( first.Ptr(), first.Num() ) || restored.links[0].condFlag != 255 || restored.links[0].condValue != 65535 || restored.links[0].conditionMode != 2 )
			{
				failures++;
			}
		}
	}
	idList<byte, TAG_GAME> recipeBytes;
	idGraphBytes wire( recipeBytes );
	wire.Put( 1, 1 );
	wire.Put( 1, 1 );
	wire.Put( 17, 8 );
	wire.Put( 12345, 8 );
	wire.Put( 0x12345678, 4 );
	wire.Put( 2, 2 );
	wire.Put( 3, 2 );
	wire.Put( 0x8000000000000001ULL, 8 );
	wire.Text( idStr( "mg_test" ), 31 );
	idMapGraphDirector decoded;
	byte encoded[96];
	if( !decoded.SetMatchGameData( recipeBytes.Ptr(), recipeBytes.Num() ) || decoded.WriteMatchGameData( "unused", encoded, sizeof( encoded ) ) != recipeBytes.Num() || memcmp( encoded, recipeBytes.Ptr(), recipeBytes.Num() ) )
	{
		failures++;
	}
	for( int length = 1; length < recipeBytes.Num(); length++ )
	{
		if( decoded.SetMatchGameData( recipeBytes.Ptr(), length ) )
		{
			failures++;
		}
		truncations++;
	}
	recipeBytes[1] = 4;
	if( decoded.SetMatchGameData( recipeBytes.Ptr(), recipeBytes.Num() ) )
	{
		failures++;
	}
	common->Printf( "mapGraphTest: %s seeds=%d truncations=%d failures=%d time=%.3fms\n", failures ? "FAIL" : "PASS", seeds, truncations, failures, ( Sys_Microseconds() - start ) * 0.001 );
}
#endif
static void MapGraphPortalTest_f( const idCmdArgs& args )
{
	const int player = args.Argc() > 1 ? atoi( args.Argv( 1 ) ) : -1;
	if( player < 0 || player >= MAX_CLIENTS || !gameLocal.entities[player] )
	{
		common->Warning( "mapGraphPortalTest <live player slot>" );
		return;
	}
	const idVec3 origin = gameLocal.entities[player]->GetPhysics()->GetOrigin();
	mapGraphDirector.OpenTownPortalFor( gameLocal.entities[player] );
	const landmark_t* portal = worldStore.FindLandmark( "_townportal" );
	const bool pass = portal && portal->origin == origin && gameLocal.FindEntity( "_dungeon_townportal" );
	common->Printf( "mapGraphPortalTest: %s caster=%d origin=%s\n", pass ? "PASS" : "FAIL", player, origin.ToString() );
}
void idMapGraphDirector::Init()
{
	transitions = refused = inhibited = prefetches = 0;
	Clear();
	cmdSystem->AddCommand( "mapGraphPortalTest", MapGraphPortalTest_f, CMD_FL_GAME | CMD_FL_CHEAT, "verify an explicit player portal caster" );
#if !defined( ID_RETAIL )
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
	cmdSystem->RemoveCommand( "mapGraphPortalTest" );
	cmdSystem->RemoveCommand( "mapGraphTest" );
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
