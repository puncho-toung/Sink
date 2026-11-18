#pragma once
// Frozen overlay3 data model, readers, partition and exact table builders; no tests.
// overlay3.cpp: joint-consult exact area-state two-level routing experiment; see BRIEF.md.
// Reader, Profile, Partition adapted from ../exp-overlay/overlay.cpp; only offline builders use nested vectors.
// Build: g++ -O2 -std=c++17 -Wall -Wextra -o scratch/overlay3 overlay3.cpp
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <queue>
#include <tuple>
#include <string>
#include <type_traits>
#include <vector>

static void CheckImpl( bool ok, const char* expression, int line )
{
	if( !ok ) { fprintf( stderr, "CHECK line %d: %s\n", line, expression ); exit( 1 ); }
}
#define Check( expression ) CheckImpl( bool( expression ), #expression, __LINE__ )

typedef int64_t int64;
typedef uint64_t uint64;
const int64 INF = INT64_MAX / 4;

// Offline partition constants inherited from ATLAS.md; runtime caps and distance domains are defined below.
enum { ATLAS_B_BITS = 11, ATLAS_D_BITS = 15, ATLAS_K_BITS = 14, ATLAS_E_BITS = 16, ATLAS_F_BITS = 25 };
const uint32_t ATLAS_B = 1u << ATLAS_B_BITS, ATLAS_D = 1u << ATLAS_D_BITS, ATLAS_K = 1u << ATLAS_K_BITS;
const uint32_t ATLAS_E = 1u << ATLAS_E_BITS, ATLAS_F = 1u << ATLAS_F_BITS;
const uint32_t ATLAS_U16_BITS = 16, ATLAS_U16_SPACE = 1u << ATLAS_U16_BITS;
const uint16_t ATLAS_REGION_NONE = ATLAS_U16_SPACE - 1, ATLAS_LOCAL_NONE = ATLAS_U16_SPACE - 1;
enum class atlasDistance_t : uint16_t {};
enum class atlasSelector_t : uint16_t {};
enum class atlasOrdinal_t : uint16_t {};
const uint32_t ATLAS_DISTANCE_SPACE = 1u << 16, ATLAS_SELECTOR_SPACE = 1u << 16, ATLAS_ORDINAL_SPACE = 1u << 16;
const uint32_t ATLAS_ORDINAL_BITS = 10, ATLAS_ORDINAL_COUNT = 1u << ATLAS_ORDINAL_BITS;
const atlasDistance_t ATLAS_DISTANCE_UNREACHABLE = atlasDistance_t( 0 );
const atlasDistance_t ATLAS_DISTANCE_RESERVED = atlasDistance_t( ATLAS_DISTANCE_SPACE - 1 );   // 65535
const atlasDistance_t ATLAS_LOCAL_ENCODED_MAX = atlasDistance_t( ATLAS_DISTANCE_SPACE - 2 );   // 65534
const atlasSelector_t ATLAS_SELECTOR_NONE = atlasSelector_t( ATLAS_SELECTOR_SPACE - 1 );
const atlasOrdinal_t ATLAS_ORDINAL_INVALID = atlasOrdinal_t( ATLAS_ORDINAL_SPACE - 1 );
const int64 LOCAL_MAX = uint16_t( ATLAS_LOCAL_ENCODED_MAX ) - 1;                               // 65533
const uint64 ATLAS_BYTE_CAP = 268435456;                                                     // 256 MiB per class
const uint64 ATLAS_TIC_CREDIT = 16777216;
const uint32_t ROW_INF = INT_MAX;
static_assert( sizeof( atlasDistance_t ) == 2 && sizeof( atlasSelector_t ) == 2 && sizeof( atlasOrdinal_t ) == 2, "domain widths" );
static_assert( !std::is_convertible<atlasDistance_t, atlasSelector_t>::value, "no distance-to-selector conversion" );
static_assert( ATLAS_K < ATLAS_REGION_NONE && ATLAS_B < ATLAS_LOCAL_NONE, "sentinels inside the stored 16-bit space" );
static_assert( ATLAS_E == ATLAS_U16_SPACE, "E count needs 17 bits: uint32" );

struct atlasRegion_t { uint32_t firstArea, numAreas, firstArc, numArcs, firstRevArc, numRevArcs, diameter, flags; };
struct atlasArc_t { uint16_t from, to, fromSlot, flags; uint32_t weight, firstPolicy; };
struct atlasPolicy_t { atlasDistance_t distance; atlasSelector_t reachNumber; };
struct atlasLocalEdge_t { uint16_t sourceLocal; atlasOrdinal_t selector; uint32_t weight; };
static_assert( sizeof( atlasRegion_t ) == 32 && sizeof( atlasArc_t ) == 16 && sizeof( atlasPolicy_t ) == 4 && sizeof( atlasLocalEdge_t ) == 8, "record sizes" );

static bool Weight( int64 reachTime, int64 maxIncoming, int64 ledge, int64& result )
{
	if( reachTime < 0 || reachTime > 65535 || maxIncoming < 0 || maxIncoming > INT_MAX || ( ledge != 0 && ledge != 250 ) ) { return false; }
	result = reachTime + std::max<int64>( 1, maxIncoming ) + ledge;
	return result > 0 && result <= INT_MAX;
}
static bool TextFits( int64 length ) { return length >= 0 && length < INT_MAX; }

// ---------------------------------------------------------------- B. graph model and region cut (core-2 code)
struct edge_t { int from, to, ordinal; int64 weight; };
struct graph_t
{
	int n;
	std::vector<edge_t> edges;
	std::vector<std::vector<int> > out, in, neighbors;
	std::vector<int> enabled, mutableArea;
	std::vector<float> geo; // 6 floats per edge (start, end), filled by Profile only
	explicit graph_t( int count ) : n( count ), out( count ), in( count ), neighbors( count ), enabled( count, 1 ), mutableArea( count, 0 ) {}
	void Add( int a, int b, int64 w, int ordinal = -1 )
	{
		Check( a >= 0 && a < n && b >= 0 && b < n && w > 0 && w <= INT_MAX );
		int index = ( int )edges.size();
		edges.push_back( edge_t{ a, b, ordinal < 0 ? ( int )out[a].size() : ordinal, w } );
		out[a].push_back( index ); in[b].push_back( index );
	}
	void Finish( bool pruneIsolated = true )
	{
		for( int a = 0; a < n; a++ ) { neighbors[a].clear(); }
		for( const edge_t& e : edges )
		{
			if( enabled[e.from] && enabled[e.to] ) { neighbors[e.from].push_back( e.to ); neighbors[e.to].push_back( e.from ); }
		}
		for( int a = 0; a < n; a++ )
		{
			std::sort( neighbors[a].begin(), neighbors[a].end() );
			neighbors[a].erase( std::unique( neighbors[a].begin(), neighbors[a].end() ), neighbors[a].end() );
			if( pruneIsolated && neighbors[a].empty() ) { enabled[a] = 0; }
		}
	}
	int EdgeByOrdinal( int a, int ordinal ) const
	{
		for( int i : out[a] ) { if( edges[i].ordinal == ordinal ) { return i; } }
		return -1;
	}
};
struct arc_t { int from, to; std::vector<int> crossing; };
struct crossing_t { int from, to, area, ordinal, edge; };
static bool CrossingOrder( const crossing_t& a, const crossing_t& b )
{
	if( a.from != b.from ) { return a.from < b.from; }
	if( a.to != b.to ) { return a.to < b.to; }
	if( a.area != b.area ) { return a.area < b.area; }
	return a.ordinal < b.ordinal;
}
struct caps_t { uint64 k, e, f; caps_t( uint64 rk = ATLAS_K, uint64 re = ATLAS_E, uint64 rf = ATLAS_F ) : k( rk ), e( re ), f( rf ) {} };
struct census_t { uint64 k = 0, e = 0, f = 0, u = 0, crossing = 0, internal = 0, work = 0; };
struct partition_t
{
	std::vector<std::vector<int> > members, localOut, localIn;
	std::vector<arc_t> arcs;
	std::vector<int> region, local, group;
	census_t census;
};
static bool Census( const partition_t& p, const caps_t& caps, census_t& c )
{
	c = census_t(); c.k = p.members.size(); c.e = p.arcs.size();
	if( c.k > caps.k || c.e > caps.e ) { return false; }
	std::vector<uint64> internal( c.k, 0 );
	for( size_t r = 0; r < p.members.size(); r++ )
	{
		c.u += p.members[r].size();
		for( int a : p.members[r] ) { internal[r] += p.localOut[a].size(); }
		c.internal += internal[r];
	}
	for( const arc_t& arc : p.arcs )
	{
		uint64 count = p.members[arc.from].size();
		if( count > caps.f - c.f ) { return false; }
		c.f += count; c.crossing += arc.crossing.size(); c.work += internal[arc.from] + arc.crossing.size();
	}
	return true;
}
struct frame_t { int area; size_t cursor; };
static bool MinMember( const std::vector<int>& a, const std::vector<int>& b ) { return a[0] < b[0]; }

// ATLAS.md §2: reserve mutable singletons, grow weakly connected groups under B/D in ascending FIFO order,
// refine each group into SCCs by iterative Kosaraju, sort by minimum member, group crossings into arcs.
// exp-cut: with a label, step 3's FIFO only enqueues neighbours carrying the seed's label (the only change).
static bool Partition( const graph_t& g, int areaCap, int reachCap, partition_t& p, const caps_t& caps = caps_t(), const std::vector<int>* label = NULL )
{
	if( areaCap < 1 || areaCap > int( ATLAS_B ) || reachCap < 0 || reachCap > int( ATLAS_D ) || caps.k > ATLAS_K || caps.e > ATLAS_E || caps.f > ATLAS_F ) { return false; }
	p.members.clear(); p.group.assign( g.n, -1 ); p.region.assign( g.n, -1 ); p.local.assign( g.n, -1 );
	std::vector<int> degree( g.n, 0 ), queued( g.n, -1 );
	int groups = 0;
	for( int a = 0; a < g.n; a++ )
	{
		if( !g.enabled[a] ) { continue; }
		for( int i : g.out[a] ) { degree[a] += g.enabled[g.edges[i].to] != 0; }
		if( degree[a] > reachCap ) { return false; }
		if( g.mutableArea[a] ) { p.group[a] = groups++; }
	}
	for( int seed = 0; seed < g.n; seed++ )
	{
		if( !g.enabled[seed] || p.group[seed] >= 0 ) { continue; }
		std::vector<int> queue( 1, seed ); queued[seed] = groups;
		int areas = 0, reaches = 0;
		for( size_t head = 0; head < queue.size(); head++ )
		{
			int a = queue[head];
			if( areas == areaCap || degree[a] > reachCap - reaches ) { break; } // deferred: later seeds take them
			p.group[a] = groups; areas++; reaches += degree[a];
			for( int b : g.neighbors[a] ) { if( p.group[b] < 0 && queued[b] != groups && ( !label || ( *label )[b] == ( *label )[seed] ) ) { queued[b] = groups; queue.push_back( b ); } }
		}
		groups++;
	}
	std::vector<int> seen( g.n, 0 ), finish;
	for( int seed = 0; seed < g.n; seed++ )
	{
		if( p.group[seed] < 0 || seen[seed] ) { continue; }
		std::vector<frame_t> stack( 1, frame_t{ seed, 0 } ); seen[seed] = 1;
		while( !stack.empty() )
		{
			frame_t& top = stack.back(); int a = top.area;
			if( top.cursor == g.out[a].size() ) { finish.push_back( a ); stack.pop_back(); continue; }
			int b = g.edges[g.out[a][top.cursor++]].to;
			if( p.group[b] == p.group[a] && !seen[b] ) { seen[b] = 1; stack.push_back( frame_t{ b, 0 } ); }
		}
	}
	seen.assign( g.n, 0 );
	for( size_t index = finish.size(); index > 0; index-- )
	{
		int seed = finish[index - 1];
		if( seen[seed] ) { continue; }
		std::vector<int> component, stack( 1, seed ); seen[seed] = 1;
		while( !stack.empty() )
		{
			int a = stack.back(); stack.pop_back(); component.push_back( a );
			for( int i : g.in[a] ) { int b = g.edges[i].from; if( p.group[b] == p.group[a] && !seen[b] ) { seen[b] = 1; stack.push_back( b ); } }
		}
		std::sort( component.begin(), component.end() ); p.members.push_back( component );
	}
	std::sort( p.members.begin(), p.members.end(), MinMember );
	for( size_t r = 0; r < p.members.size(); r++ )
	{
		for( size_t i = 0; i < p.members[r].size(); i++ ) { p.region[p.members[r][i]] = ( int )r; p.local[p.members[r][i]] = ( int )i; }
	}
	p.localOut.assign( g.n, {} ); p.localIn.assign( g.n, {} );
	std::vector<crossing_t> crossing; p.arcs.clear();
	for( size_t i = 0; i < g.edges.size(); i++ )
	{
		const edge_t& e = g.edges[i];
		if( p.region[e.from] >= 0 && p.region[e.from] == p.region[e.to] ) { p.localOut[e.from].push_back( ( int )i ); p.localIn[e.to].push_back( ( int )i ); }
		else if( p.region[e.from] >= 0 && p.region[e.to] >= 0 ) { crossing.push_back( crossing_t{ p.region[e.from], p.region[e.to], e.from, e.ordinal, ( int )i } ); }
	}
	std::sort( crossing.begin(), crossing.end(), CrossingOrder );
	for( const crossing_t& c : crossing )
	{
		if( p.arcs.empty() || p.arcs.back().from != c.from || p.arcs.back().to != c.to ) { p.arcs.push_back( arc_t{ c.from, c.to, {} } ); }
		p.arcs.back().crossing.push_back( c.edge );
	}
	return Census( p, caps, p.census );
}

static uint32_t Next( uint32_t& s ) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

// Caps at equality and +1 by construction (core-2 constructions), plus the frontier/certificate limits.
struct rawReach_t { int from, to, type, ordinal, time, line; float start[3], end[3]; };
struct fixture_t
{
	std::vector<int> flags, contents; std::vector<rawReach_t> reaches;
    std::vector<float> planes;std::vector<int> facePlane; // seam census includes floorless hull faces
	// exp-cut: area geometry for the cut only (the file keeps edges for floor and ladder faces only)
	std::vector<float> vertices; std::vector<int> edgeVertex, edgeIndex, faceFlags, faceFirstEdge, faceNumEdges, faceIndex, areaFirstFace, areaNumFaces;
	float low[3] = { INFINITY, INFINITY, INFINITY }, high[3] = { -INFINITY, -INFINITY, -INFINITY };
};
static bool EndpointValid( const fixture_t& input, const rawReach_t& r )
{
	for( int axis = 0; axis < 3; axis++ )
	{
		for( float v : { r.start[axis], r.end[axis] } ) { if( !std::isfinite( v ) || v < input.low[axis] - 2.0f || v > input.high[axis] + 2.0f ) { return false; } }
	}
	return true;
}
static bool ReadFixture( const char* path, fixture_t& result )
{
	std::ifstream file( path, std::ios::binary ); if( !file.good() ) { return false; }
	file.seekg( 0, std::ios::end ); int64 bytes = ( int64 )file.tellg(); Check( TextFits( bytes ) ); file.seekg( 0 );
	std::string line; int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		lineNumber++; int count;
		if( sscanf( line.c_str(), "planes %d {", &count ) == 1 )
        {
            for(int i=0;i<count;i++){Check(bool(std::getline(file,line)));lineNumber++;int id;float v[4];Check(sscanf(line.c_str()," %d ( %f %f %f %f )",&id,&v[0],&v[1],&v[2],&v[3])==5&&id==i);for(float x:v)result.planes.push_back(x);}
        }
        if( sscanf( line.c_str(), "vertices %d {", &count ) == 1 )
		{
			for( int i = 0; i < count; i++ )
			{
				Check( bool( std::getline( file, line ) ) ); lineNumber++;
				int id; float v[3]; Check( sscanf( line.c_str(), " %d ( %f %f %f )", &id, &v[0], &v[1], &v[2] ) == 4 && id == i );
				for( int axis = 0; axis < 3; axis++ ) { result.low[axis] = std::min( result.low[axis], v[axis] ); result.high[axis] = std::max( result.high[axis], v[axis] ); result.vertices.push_back( v[axis] ); }
			}
		}
		if( sscanf( line.c_str(), "edges %d {", &count ) == 1 )
		{
			for( int i = 0; i < count; i++ )
			{
				Check( bool( std::getline( file, line ) ) ); lineNumber++;
				int id, v0, v1; Check( sscanf( line.c_str(), " %d ( %d %d )", &id, &v0, &v1 ) == 3 && id == i );
				result.edgeVertex.push_back( v0 ); result.edgeVertex.push_back( v1 );
			}
		}
		if( sscanf( line.c_str(), "edgeIndex %d {", &count ) == 1 || sscanf( line.c_str(), "faceIndex %d {", &count ) == 1 )
		{
			std::vector<int>& index = line[0] == 'e' ? result.edgeIndex : result.faceIndex;
			for( int i = 0; i < count; i++ )
			{
				Check( bool( std::getline( file, line ) ) ); lineNumber++;
				int id, v; Check( sscanf( line.c_str(), " %d ( %d )", &id, &v ) == 2 && id == i ); index.push_back( v );
			}
		}
		if( sscanf( line.c_str(), "faces %d {", &count ) == 1 )
		{
			for( int i = 0; i < count; i++ )
			{
				Check( bool( std::getline( file, line ) ) ); lineNumber++;
				int id, plane, flags, a0, a1, first, num; Check( sscanf( line.c_str(), " %d ( %d %d %d %d %d %d )", &id, &plane, &flags, &a0, &a1, &first, &num ) == 7 && id == i );
				result.facePlane.push_back(plane);result.faceFlags.push_back( flags ); result.faceFirstEdge.push_back( first ); result.faceNumEdges.push_back( num );
			}
		}
		if( sscanf( line.c_str(), "areas %d {", &count ) == 1 )
		{
			result.flags.resize( count ); result.contents.resize( count );
			for( int i = 0; i < count; i++ )
			{
				Check( bool( std::getline( file, line ) ) ); lineNumber++;
				int id, face, faces, cluster, clusterArea, q;
				Check( sscanf( line.c_str(), " %d ( %d %d %d %d %d %d ) %d {", &id, &result.flags[i], &result.contents[i], &face, &faces, &cluster, &clusterArea, &q ) == 8 );
				Check( id == i && q >= 0 && q <= 1024 );
				result.areaFirstFace.push_back( face ); result.areaNumFaces.push_back( faces );
				std::vector<rawReach_t> area;
				for( int j = 0; j < q; j++ )
				{
					Check( bool( std::getline( file, line ) ) ); lineNumber++;
					rawReach_t r; int edge; r.from = i; r.ordinal = q - 1 - j; r.line = lineNumber; // parser prepends: loaded ordinal q-1-j
					Check( sscanf( line.c_str(), " %d %d ( %f %f %f ) ( %f %f %f ) %d %d", &r.type, &r.to, &r.start[0], &r.start[1], &r.start[2], &r.end[0], &r.end[1], &r.end[2], &edge, &r.time ) == 10 );
					Check( r.to > 0 && r.to < count ); area.push_back( r );
				}
				result.reaches.insert( result.reaches.end(), area.rbegin(), area.rend() );
				Check( bool( std::getline( file, line ) ) ); lineNumber++;
			}
			break;
		}
	}
	return !result.flags.empty();
}
// ATLAS.md §1/§3.1: admitted areas and the scalar cost w(r) = reachTime + max(1, max incoming A) + ledge.
static graph_t Profile( const fixture_t& input, int mask, bool removeBad, int& bad, int& emptyIncoming )
{
	graph_t g( ( int )input.flags.size() ); std::vector<int> rawIndex;
	for( int a = 0; a < g.n; a++ )
	{
		int areaTravel = input.contents[a] & 2 ? 1 << 21 : 1 << 22;                       // WATER else AIR
		g.enabled[a] = a > 0 && ( input.flags[a] & 192 ) && !( areaTravel & ~mask );       // REACHABLE_WALK|FLY
		g.mutableArea[a] = ( input.contents[a] & 12 ) != 0;                                // CLUSTERPORTAL|OBSTACLE
	}
	bad = 0;
	for( size_t i = 0; i < input.reaches.size(); i++ )
	{
		const rawReach_t& r = input.reaches[i];
		if( !g.enabled[r.from] || !g.enabled[r.to] || ( r.type & ~mask ) ) { continue; }
		if( !EndpointValid( input, r ) ) { bad++; if( removeBad ) { continue; } }
		g.Add( r.from, r.to, 1, r.ordinal ); rawIndex.push_back( ( int )i );
		for( int k = 0; k < 3; k++ ) { g.geo.push_back( r.start[k] ); }
		for( int k = 0; k < 3; k++ ) { g.geo.push_back( r.end[k] ); }
	}
	g.Finish( true ); emptyIncoming = 0;
	for( int a = 0; a < g.n; a++ ) { if( g.enabled[a] && g.in[a].empty() ) { emptyIncoming++; } }
	for( size_t i = 0; i < g.edges.size(); i++ )
	{
		edge_t& e = g.edges[i]; const rawReach_t& r = input.reaches[rawIndex[i]]; int64 maximum = 0;
		for( int incoming : g.in[e.from] )
		{
			const rawReach_t& j = input.reaches[rawIndex[incoming]];
			float x = r.start[0] - j.end[0], y = r.start[1] - j.end[1], z = r.start[2] - j.end[2];
			float squared = x * x + y * y + z * z;
			float distance = squared == 0.0f ? 0.0f : squared * std::sqrt( 1.0f / squared );
			distance *= ( input.contents[e.from] & 2 ) ? 100.0f / 150.0f : 100.0f / 300.0f;
			Check( std::isfinite( distance ) && distance < float( INT_MAX ) );
			maximum = std::max<int64>( maximum, distance < 1.0f ? 1 : ( int )distance );
		}
		Check( Weight( r.time, maximum, !( mask & 2048 ) && ( input.flags[e.from] & 4 ) ? 250 : 0, e.weight ) );
	}
	return g;
}
static std::vector<float> AreaCentres( const fixture_t& input, int& fromFloor, int& fromReach, int& none )
{
	int N = ( int )input.flags.size(); std::vector<float> c( 3 * N, 0.0f ); fromFloor = fromReach = none = 0;
	std::vector<double> sum( 3 * N, 0.0 ); std::vector<int> count( N, 0 );
	for( const rawReach_t& r : input.reaches )
	{
		if( !EndpointValid( input, r ) ) { continue; }
		for( int k = 0; k < 3; k++ ) { sum[3 * r.from + k] += r.start[k]; sum[3 * r.to + k] += r.end[k]; }
		count[r.from]++; count[r.to]++;
	}
	for( int a = 0; a < N; a++ )
	{
		double floor[3] = { 0, 0, 0 }; int floors = 0;
		for( int i = 0; i < input.areaNumFaces[a]; i++ )
		{
			int f = std::abs( input.faceIndex[input.areaFirstFace[a] + i] );
			if( !( input.faceFlags[f] & 4 ) || input.faceNumEdges[f] <= 0 ) { continue; }               // FACE_FLOOR with edges
			double centre[3] = { 0, 0, 0 };
			for( int k = 0; k < input.faceNumEdges[f]; k++ )
			{
				int e = input.edgeIndex[input.faceFirstEdge[f] + k]; int v = input.edgeVertex[2 * std::abs( e ) + ( e < 0 )];
				for( int j = 0; j < 3; j++ ) { centre[j] += input.vertices[3 * v + j]; }
			}
			for( int j = 0; j < 3; j++ ) { floor[j] += centre[j] / input.faceNumEdges[f]; }
			floors++;
		}
		if( floors ) { for( int j = 0; j < 3; j++ ) { c[3 * a + j] = float( floor[j] / floors ); } fromFloor++; }
		else if( count[a] ) { for( int j = 0; j < 3; j++ ) { c[3 * a + j] = float( sum[3 * a + j] / count[a] ); } fromReach++; }
		else { none++; }
	}
	return c;
}
// Variants 1 and 2: the visportal grid cell (x, y) containing the area centre; z is not cut (portals are full height).
static std::vector<int> GridLabels( const std::vector<float>& c, int n, float origin, float pitch, int cells )
{
	std::vector<int> label( n, 0 );
	for( int a = 0; a < n; a++ )
	{
		int ix = ( int )std::floor( ( c[3 * a] - origin ) / pitch ), iy = ( int )std::floor( ( c[3 * a + 1] - origin ) / pitch );
		ix = std::min( std::max( ix, 0 ), cells - 1 ); iy = std::min( std::max( iy, 0 ), cells - 1 ); label[a] = iy * cells + ix;
	}
	return label;
}
// Level-2 experiment: offline helpers above; runtime below uses flat arrays only.
#include <chrono>
#include <stdexcept>
#include <cstring>
#include <numeric>
#include <array>
#include <sstream>

namespace overlay_two {
using U = uint32_t; using W = uint64_t;
constexpr U NONE = UINT32_MAX;
constexpr W FAR = UINT64_MAX / 4;
constexpr U BITS_K = 14, BITS_STATE = 20, BITS_CROSS = 24, BITS_TABLE = 27;
constexpr U K_CAP = 1u << BITS_K, STATE_CAP = 1u << BITS_STATE, CROSS_CAP = 1u << BITS_CROSS, TABLE_CAP = 1u << BITS_TABLE;
constexpr W BYTE_CAP = W(1) << 28, CREDIT = W(1) << 24, COLD_CREDIT = W(1) << 22;
constexpr uint16_t TABLE_INF = 65535, TABLE_RESERVED = 65534;
static void Require(bool b, const char* s) { if(!b) throw std::runtime_error(s); }
static void Cap(const char* name, W count, W limit) {
    if(count > limit) { std::ostringstream s; s << "overlay3: " << name << " " << count << " > " << limit; throw std::runtime_error(s.str()); }
}
static W Plus(W a, W b) { Require(a < FAR && b < FAR && b < FAR-a, "overlay3: distance overflow before publication"); return a+b; }
static uint16_t Encode(W d, U owner, U entry, U exit) {
    if(d==FAR)return TABLE_INF;
    if(d>65533) { std::ostringstream s; s<<"overlay3: table cell "<<owner<<" entry "<<entry<<" exit "<<exit<<" travel time "<<d<<" > 65533"; throw std::runtime_error(s.str()); }
    return uint16_t(d);
}
struct WorkCounter {
    W value=0;W*total=nullptr;operator W()const{return value;}
    WorkCounter&operator+=(W amount){Require(amount<=UINT64_MAX-value&&(!total||amount<=UINT64_MAX-*total),"overlay3: work counter overflow before publication");value+=amount;if(total)*total+=amount;return *this;}
    WorkCounter&operator++(){return *this+=1;}void operator++(int){*this+=1;}
};
struct Work {
    W total=0;WorkCounter scan{0,&total},compare{0,&total},labels{0,&total},resolve{0,&total},cache{0,&total};
    Work()=default;Work(const Work&b):Work(){*this=b;}
    Work&operator=(const Work&b){total=b.total;scan.value=b.scan.value;compare.value=b.compare.value;labels.value=b.labels.value;resolve.value=b.resolve.value;cache.value=b.cache.value;return *this;}
    W Total()const{return total;}
    void Add(const Work&b){scan+=b.scan;compare+=b.compare;labels+=b.labels;resolve+=b.resolve;cache+=b.cache;}
};
// ATLAS work: examinations, heap comparisons, logical label/slot initialization, copying and reset; ordinary relax writes are not charged again.
// Every routing heap uses one flat preallocated array per field; touched labels/slots cost two init and two reset units.
struct Search {
    std::vector<W> d; std::vector<int32_t> pos; std::vector<U> heap,touched;
    explicit Search(U n=0): d(n,FAR),pos(n,-1){heap.reserve(n);touched.reserve(n);}
    bool Less(U a,U b,Work& w){w.compare++;return d[a]<d[b]||(d[a]==d[b]&&a<b);}
    void Swap(U a,U b){std::swap(heap[a],heap[b]);pos[heap[a]]=a;pos[heap[b]]=b;}
    bool Offer(U v,W value,Work& w){
        if(value>=d[v])return false;
        Require(pos[v]!=-2,"overlay3: settled label decreased");
        if(d[v]==FAR){touched.push_back(v);w.labels+=2;}
        d[v]=value;
        if(pos[v]<0){pos[v]=int32_t(heap.size());heap.push_back(v);}
        U i=U(pos[v]);while(i&&Less(heap[i],heap[(i-1)/2],w)){Swap(i,(i-1)/2);i=(i-1)/2;}return true;
    }
    U Pop(Work& w){U v=heap[0];Swap(0,U(heap.size()-1));heap.pop_back();pos[v]=-2;U i=0;
        while(2*i+1<heap.size()){U c=2*i+1;if(c+1<heap.size()&&Less(heap[c+1],heap[c],w))c++;if(!Less(heap[c],heap[i],w))break;Swap(i,c);i=c;}return v;}
    void Reset(Work& w){for(U v:touched){d[v]=FAR;pos[v]=-1;w.labels+=2;}touched.clear();heap.clear();}
};
struct Edge { U from,to,w,ordinal; };
struct LegacyReach { U to,ordinal; const LegacyReach* next; };
struct FlatGraph {
    U n=0;std::vector<Edge> edges;std::vector<U> out,in,rev;
    std::vector<uint8_t> enabled,mutableArea;
    std::vector<U> degree,legacyStart;std::vector<LegacyReach> legacy;std::vector<const LegacyReach*> resolver,heads;
    W revision=1;
    void Index(){
        Cap("areas",n,W(1)<<25);W originals=0;for(U q:degree)originals+=q;Cap("original reaches",originals,W(1)<<30);
        std::sort(edges.begin(),edges.end(),[](const Edge&a,const Edge&b){return std::tie(a.from,a.ordinal,a.to)<std::tie(b.from,b.ordinal,b.to);});
        out.assign(n+1,0);in.assign(n+1,0);for(const auto&e:edges){out[e.from+1]++;in[e.to+1]++;}
        std::partial_sum(out.begin(),out.end(),out.begin());std::partial_sum(in.begin(),in.end(),in.begin());rev.resize(edges.size());auto cur=in;
        for(U i=0;i<edges.size();i++)rev[cur[edges[i].to]++]=i;
        legacyStart.assign(n+1,0);for(U a=0;a<n;a++){Cap("original degree",degree[a],1u<<10);legacyStart[a+1]=legacyStart[a]+degree[a];}
        legacy.resize(legacyStart.back());resolver.resize(legacy.size());heads.resize(n);
        for(U a=0;a<n;a++)for(U j=0;j<degree[a];j++){U i=legacyStart[a]+j;legacy[i]={NONE,j,j+1<degree[a]?&legacy[i+1]:nullptr};resolver[i]=&legacy[i];}
        for(U a=0;a<n;a++)heads[a]=degree[a]?&legacy[legacyStart[a]]:nullptr;
        for(const auto&e:edges)legacy[legacyStart[e.from]+e.ordinal].to=e.to;
    }
    bool Toggle(U a,bool open){Require(a<n&&mutableArea[a],"overlay3: mutation is not a declared singleton; refused before writes");
        if(bool(enabled[a])==open){return false;}Require(revision!=UINT64_MAX,"overlay3: revision overflow before writes");enabled[a]=open;revision++;return true;}
    U Resolve(U a,U ordinal,bool keep,Work&w)const{
        Cap("selector",ordinal,1023);Require(a<n,"overlay3: invalid source before dereference");
        const LegacyReach*r=nullptr;
        if(keep){Require(ordinal<legacyStart[a+1]-legacyStart[a],"overlay3: invalid selector before dereference");r=resolver[legacyStart[a]+ordinal];w.resolve++;}
        else{r=heads[a];for(U j=0;r&&j<=ordinal;j++,r=r->next){w.resolve++;if(j==ordinal)break;}}
        Require(r&&r->ordinal==ordinal&&r->to!=NONE&&enabled[r->to],"overlay3: reach identity/profile/endpoint mismatch");return r->to;
    }
};
struct Anchor {U area;float p[3];};
struct Seam {U a,b,w,type=2048;};
struct FixtureBase {graph_t graph;partition_t cut;std::vector<float>centres;std::vector<U>degree;std::array<std::vector<Anchor>,4>border;std::array<std::vector<Seam>,2>seams;
    explicit FixtureBase(int n):graph(n){}
};
static FixtureBase MakeBase(const fixture_t&input,int mask,bool badFly){
    int bad,empty;FixtureBase b(int(input.flags.size()));b.graph=Profile(input,mask,badFly,bad,empty);
    int ff,fr,nn;b.centres=AreaCentres(input,ff,fr,nn);auto labels=GridLabels(b.centres,b.graph.n,-4096,2048,4);
    Require(Partition(b.graph,2048,32768,b.cut,caps_t(),&labels),"overlay3: base partition refused");
    b.degree.assign(b.graph.n,0);for(const auto&r:input.reaches)b.degree[r.from]++;
    // Candidate border anchors: average stored geometry/endpoints within 2 u of the compiled hull plane +/-4056.
    std::vector<double> sum(size_t(b.graph.n)*12,0);std::vector<U>count(size_t(b.graph.n)*4,0);
    auto point=[&](U a,const float*p){if(!b.graph.enabled[a])return;for(U side=0;side<4;side++){U ax=side/2;float bound=(side%2)?4056.0f:-4056.0f;
        if(std::abs(p[ax]-bound)<=2){U k=a*4+side;count[k]++;for(U z=0;z<3;z++)sum[k*3+z]+=p[z];}}};
    for(U a=0;a<input.flags.size();a++)for(int i=0;i<input.areaNumFaces[a];i++){int f=std::abs(input.faceIndex[input.areaFirstFace[a]+i]);for(int j=0;j<input.faceNumEdges[f];j++){
        int e=input.edgeIndex[input.faceFirstEdge[f]+j];int v=input.edgeVertex[2*std::abs(e)+(e<0)];point(a,&input.vertices[3*v]);}}
    for(const auto&r:input.reaches)if(EndpointValid(input,r)){point(r.from,r.start);point(r.to,r.end);}
    U planeOnly=0;
    for(U a=0;a<b.degree.size();a++)if(b.graph.enabled[a])for(int i=0;i<input.areaNumFaces[a];i++){
        U f=U(std::abs(input.faceIndex[input.areaFirstFace[a]+i]));const float*pl=&input.planes[4*input.facePlane[f]];
        for(U ax=0;ax<2;ax++)if(std::abs(pl[ax])>0.999999f&&std::abs(pl[1-ax])<1e-6f&&std::abs(pl[2])<1e-6f){float at=pl[3]/pl[ax];U side=ax*2+(at>0);if(std::abs(std::abs(at)-4056.0f)<=2&&!count[a*4+side]){
            U k=a*4+side;count[k]=1;for(U d=0;d<3;d++)sum[k*3+d]=(d==ax?at:b.centres[3*a+d]);planeOnly++;
        }}
    }
    printf("M hullPlaneOnlyBorderAnchors mask=%d count=%u (floorless faces retained)\n",mask,planeOnly);
    for(U a=0;a<b.degree.size();a++)for(U side=0;side<4;side++)if(count[a*4+side]){Anchor x;x.area=a;for(U z=0;z<3;z++)x.p[z]=float(sum[(a*4+side)*3+z]/count[a*4+side]);b.border[side].push_back(x);}
    for(U ax=0;ax<2;ax++){
        const auto&A=b.border[ax*2+1];const auto&B=b.border[ax*2];std::vector<std::pair<U,U>> pairs;
        auto nearest=[&](const auto&left,const auto&right,bool flip){for(const auto&a:left){double best=256.0*256.0;U chosen=NONE;for(U j=0;j<right.size();j++){double d=0;for(U k=0;k<3;k++)if(k!=ax){double x=a.p[k]-right[j].p[k];d+=x*x;}
                if(d<best||(d==best&&(chosen==NONE||right[j].area<right[chosen].area))){best=d;chosen=j;}}
            if(chosen!=NONE)pairs.emplace_back(flip?right[chosen].area:a.area,flip?a.area:right[chosen].area);}};
        nearest(A,B,false);nearest(B,A,true);std::sort(pairs.begin(),pairs.end());pairs.erase(std::unique(pairs.begin(),pairs.end()),pairs.end());
        for(auto [a,c]:pairs){const Anchor*pa=nullptr,*pc=nullptr;for(const auto&v:A)if(v.area==a)pa=&v;for(const auto&v:B)if(v.area==c)pc=&v;Require(pa&&pc,"anchor lookup");double ds=0;
            for(U k=0;k<3;k++){double delta=pc->p[k]-pa->p[k]+(k==ax?8192.0:0.0);ds+=delta*delta;}
            U cost=1+U(std::ceil(std::sqrt(ds)*100.0/300.0));b.seams[ax].push_back({a,c,cost,(mask&2048)?2048u:2u});}
    }
    printf("M base mask=%d bad=%d emptyIncoming=%d areas=%d K=%zu border W/E/S/N=%zu/%zu/%zu/%zu matched X/Y=%zu/%zu\n",mask,bad,empty,b.graph.n,b.cut.members.size(),b.border[0].size(),b.border[1].size(),b.border[2].size(),b.border[3].size(),b.seams[0].size(),b.seams[1].size());
    return b;
}
static FlatGraph Replicate(const FixtureBase&b,const fixture_t&input,U side,bool fly){
    FlatGraph g;U copies=side*side,N=b.graph.n;Cap("areas",W(N)*copies,W(1)<<25);g.n=N*copies;g.enabled.resize(g.n);g.mutableArea.resize(g.n);g.degree.resize(g.n);
    g.edges.reserve(b.graph.edges.size()*copies+100000);
    for(U t=0;t<copies;t++){for(U a=0;a<N;a++){g.enabled[t*N+a]=b.graph.enabled[a];g.mutableArea[t*N+a]=b.graph.mutableArea[a];g.degree[t*N+a]=b.degree[a];}
        for(const auto&e:b.graph.edges)g.edges.push_back({t*N+U(e.from),t*N+U(e.to),U(e.weight),U(e.ordinal)});}
    std::vector<Edge>allSeams;U seamEdges=0,minCost=UINT32_MAX,maxCost=0;
    auto add=[&](U a,U c,U cost,bool legal){U ord=g.degree[a]++;allSeams.push_back({a,c,cost,ord});if(!legal)return;g.edges.push_back({a,c,cost,ord});seamEdges++;minCost=std::min(minCost,cost);maxCost=std::max(maxCost,cost);};
    for(U y=0;y<side;y++)for(U x=0;x<side;x++)for(U ax=0;ax<2;ax++)if((ax==0?x:y)+1<side){U t=y*side+x,u=t+(ax?side:1);
        for(const auto&s:b.seams[ax]){U a=t*N+s.a,c=u*N+s.b;add(a,c,s.w+(!fly&&(input.flags[s.a]&4)?250:0),fly||s.type==2);add(c,a,s.w+(!fly&&(input.flags[s.b]&4)?250:0),fly||s.type==2);}}
    g.Index();for(U t=0;t<copies;t++)for(const auto&r:input.reaches)g.legacy[g.legacyStart[t*N+r.from]+r.ordinal].to=t*N+r.to;for(const auto&e:allSeams)g.legacy[g.legacyStart[e.from]+e.ordinal].to=e.to;printf("M graph copies=%u areasIncludingDummy=%u legalEdges=%zu allOriginalSlots=%zu syntheticSeamEdges=%u seamCostMinMax=%u/%u\n",copies,g.n,g.edges.size(),g.legacy.size(),seamEdges,seamEdges?minCost:0,maxCost);return g;
}
struct LocalEdge {uint16_t from,ordinal;U w;};static_assert(sizeof(LocalEdge)==8,"local edge");
struct Cell {U first,count,revFirst,startFirst,super;};
struct State {U area,owner;};
struct Cross {U from,to,w,ordinal;};
struct Block {U first,nEntry,nExit,table,firstCross,nCross;};
// Column-major entry->exit tables: reverse sweeps read contiguous predecessor columns.
struct Network {std::vector<Block>block;std::vector<State>state;std::vector<Cross>cross;std::vector<U>revStart,revCross;std::vector<uint16_t>table;};
struct Layout {std::vector<Cell>cell;std::vector<U>members;std::vector<uint16_t>areaCell,areaLocal,revStart;std::vector<LocalEdge>rev;};
struct Seed {W d;U state,selector;Seed(U st,W distance,U sel=NONE):d(distance),state(st),selector(sel){}};
static_assert(sizeof(Seed)==16 && sizeof(State)==8 && sizeof(Cross)==16 && sizeof(Block)==24 && sizeof(Cell)==20,"resident layouts");
static Layout BuildLayout(const FixtureBase&b,const FlatGraph&g,U copies,U areaCap=0){
    Layout p;p.areaCell.assign(g.n,UINT16_MAX);p.areaLocal.assign(g.n,UINT16_MAX);U N=b.graph.n,K=U(b.cut.members.size());W pieces=0;for(const auto&m:b.cut.members)pieces+=areaCap?(m.size()+areaCap-1)/areaCap:1;Cap("L1 K",pieces*copies,K_CAP);
    for(U t=0;t<copies;t++)for(U c=0;c<K;c++)for(U offset=0;offset<b.cut.members[c].size();){Cell x{U(p.members.size()),std::min(areaCap?areaCap:U(b.cut.members[c].size()),U(b.cut.members[c].size())-offset),U(p.rev.size()),U(p.revStart.size()),0};Cap("cell areas",x.count,1u<<11);
        U id=U(p.cell.size()),outCount=0;for(U j=0;j<x.count;j++){U a=t*N+b.cut.members[c][offset+j];p.members.push_back(a);p.areaCell[a]=uint16_t(id);p.areaLocal[a]=uint16_t(j);outCount+=g.out[a+1]-g.out[a];}Cap("cell outgoing",outCount,1u<<15);
        for(U j=0;j<x.count;j++){U a=p.members[x.first+j];p.revStart.push_back(uint16_t(p.rev.size()-x.revFirst));for(U i=g.in[a];i<g.in[a+1];i++){const Edge&e=g.edges[g.rev[i]];if(p.areaCell[e.from]==id)p.rev.push_back({p.areaLocal[e.from],uint16_t(e.ordinal),e.w});}}
        p.revStart.push_back(uint16_t(p.rev.size()-x.revFirst));p.cell.push_back(x);offset+=x.count;}
    return p;
}
static Network NetworkFromCrosses(U owners,U n,const std::vector<U>&owner,const std::vector<Edge>&crossing){
    Network net;Cap("owner count",owners,K_CAP);net.block.resize(owners);std::vector<std::pair<U,U>>entries,exits;
    for(const auto&e:crossing){Require(owner[e.from]!=NONE&&owner[e.to]!=NONE&&owner[e.from]!=owner[e.to],"cross owner");entries.emplace_back(owner[e.to],e.to);exits.emplace_back(owner[e.from],e.from);}
    auto unique=[](auto&v){std::sort(v.begin(),v.end());v.erase(std::unique(v.begin(),v.end()),v.end());};unique(entries);unique(exits);
    std::vector<U>entry(n,NONE),exit(n,NONE);U ei=0,xi=0;W tables=0;
    for(U c=0;c<owners;c++){Block&v=net.block[c];v.first=U(net.state.size());v.table=U(tables);
        while(ei<entries.size()&&entries[ei].first==c){U a=entries[ei++].second;entry[a]=U(net.state.size());net.state.push_back({a,c});v.nEntry++;}
        while(xi<exits.size()&&exits[xi].first==c){U a=exits[xi++].second;exit[a]=U(net.state.size());net.state.push_back({a,c});v.nExit++;}
        tables+=W(v.nEntry)*v.nExit;Cap("table entries",tables,TABLE_CAP);}
    Cap("boundary states",net.state.size(),STATE_CAP);Cap("crossings",crossing.size(),CROSS_CAP);net.table.assign(size_t(tables),TABLE_RESERVED);
    for(const auto&e:crossing)net.cross.push_back({exit[e.from],entry[e.to],e.w,e.ordinal});
    std::sort(net.cross.begin(),net.cross.end(),[&](const Cross&a,const Cross&b){return std::tie(net.state[a.from].owner,a.from,a.ordinal,a.to)<std::tie(net.state[b.from].owner,b.from,b.ordinal,b.to);});
    U at=0;for(U c=0;c<owners;c++){auto&v=net.block[c];v.firstCross=at;while(at<net.cross.size()&&net.state[net.cross[at].from].owner==c)at++;v.nCross=at-v.firstCross;}
    net.revStart.assign(net.state.size()+1,0);for(const auto&e:net.cross)net.revStart[e.to+1]++;std::partial_sum(net.revStart.begin(),net.revStart.end(),net.revStart.begin());auto pos=net.revStart;net.revCross.resize(net.cross.size());
    for(U i=0;i<net.cross.size();i++){net.revCross[pos[net.cross[i].to]++]=i;}return net;
}
static void RunNetwork(const Network&net,const FlatGraph&g,const std::vector<U>*projection,U restrictGroup,const std::vector<Seed>&seeds,Search&s,Work&w,const std::vector<uint8_t>*overrideFlags=nullptr){
    const auto&flags=overrideFlags?*overrideFlags:g.enabled;
    Require(s.touched.empty(),"dirty network scratch");for(const auto&v:seeds){w.scan++;w.labels+=3;if(v.d<FAR&&flags[net.state[v.state].area])s.Offer(v.state,v.d,w);}
    while(!s.heap.empty()){U t=s.Pop(w);const State&st=net.state[t];const Block&b=net.block[st.owner];
        if(t<b.first+b.nEntry){for(U i=net.revStart[t];i<net.revStart[t+1];i++){const Cross&e=net.cross[net.revCross[i]];w.scan++;
            if(flags[net.state[e.from].area]&&(!projection||(*projection)[e.from]==restrictGroup))s.Offer(e.from,Plus(s.d[t],e.w),w);}}
        else{U col=t-b.first-b.nEntry;for(U i=0;i<b.nEntry;i++){w.scan++;uint16_t d=net.table[b.table+size_t(col)*b.nEntry+i];if(d!=TABLE_INF)s.Offer(b.first+i,Plus(s.d[t],d),w);}}
    }
}
// Tables are directed entry->exit distances. Cutting and expanding paths at each level proves equality.
// Source-group seeding includes the goal and every external exit, allowing leave/re-enter paths.
struct Atlas {
    Layout local;Network one,two;std::vector<U>groupStart,groupStates,stateGroup,stateLocal,upperEntry,down;
    U groupCount=0,maxGroup=0,maxCell=0,maxTopTable=0;bool isolateMutable=false;W buildWork=0;double buildMs=0;
};
static Atlas BuildAtlas(const FixtureBase&base,const FlatGraph&g,U copies,bool verbose=true,U groupWidth=0,bool isolateMutable=false,U areaCap=0){
    const auto begin=std::chrono::steady_clock::now();Atlas a;a.isolateMutable=isolateMutable;a.local=BuildLayout(base,g,copies,areaCap);std::vector<U>owners(g.n,NONE);for(U i=0;i<g.n;i++)if(a.local.areaCell[i]!=UINT16_MAX)owners[i]=a.local.areaCell[i];
    std::vector<Edge>cross;for(const auto&e:g.edges)if(owners[e.from]!=owners[e.to])cross.push_back(e);a.one=NetworkFromCrosses(U(a.local.cell.size()),g.n,owners,cross);
    Search bake(g.n);Work bw;
    for(U c=0;c<a.local.cell.size();c++){const Block&b=a.one.block[c];a.maxCell=std::max(a.maxCell,a.local.cell[c].count);
        for(U i=0;i<b.nEntry;i++){bake.Offer(a.one.state[b.first+i].area,0,bw);while(!bake.heap.empty()){U v=bake.Pop(bw);for(U j=g.out[v];j<g.out[v+1];j++){const auto&e=g.edges[j];bw.scan++;if(owners[e.to]==c)bake.Offer(e.to,Plus(bake.d[v],e.w),bw);}}
            for(U j=0;j<b.nExit;j++){U t=b.first+b.nEntry+j;a.one.table[b.table+size_t(j)*b.nEntry+i]=Encode(bake.d[a.one.state[t].area],c,a.one.state[b.first+i].area,a.one.state[t].area);bw.labels++;}bake.Reset(bw);}}
    // L1 mutable cells remain singletons. Plain 4x4 L2 supercells require transactional table customization; optional isolation is a measured alternative.
    U baseK=U(a.local.cell.size())/copies;if(!groupWidth)groupWidth=std::max(1u,baseK);
    if(areaCap){
        U group=NONE,areas=0;
        for(U c=0;c<a.local.cell.size();c++){
            Cell&cell=a.local.cell[c];bool mut=g.mutableArea[a.local.members[cell.first]];
            Require(!mut||cell.count==1,"mutable cell not singleton");
            Require(cell.count<=areaCap,"overlay6: split cell exceeds area cap");
            if(c%groupWidth==0){group=NONE;areas=0;}
            if(mut&&isolateMutable){cell.super=a.groupCount++;continue;}
            if(group==NONE||areas+cell.count>areaCap){group=a.groupCount++;areas=0;}
            cell.super=group;areas+=cell.count;
        }
    }else{
        a.groupCount=(U(a.local.cell.size())+groupWidth-1)/groupWidth;
        for(U c=0;c<a.local.cell.size();c++){Cell&cell=a.local.cell[c];bool mut=g.mutableArea[a.local.members[cell.first]];Require(!mut||cell.count==1,"mutable cell not singleton");cell.super=(mut&&isolateMutable)?a.groupCount++:c/groupWidth;}
    }
    Cap("L2 K",a.groupCount,K_CAP);for(U v=0;v<g.n;v++)if(owners[v]!=NONE)owners[v]=a.local.cell[owners[v]].super;
    cross.clear();for(const auto&e:a.one.cross){U from=a.one.state[e.from].area,to=a.one.state[e.to].area;if(owners[from]!=owners[to])cross.push_back({from,to,e.w,e.ordinal});}
    a.two=NetworkFromCrosses(a.groupCount,g.n,owners,cross);for(const auto&b:a.two.block)a.maxTopTable=std::max(a.maxTopTable,b.nEntry*b.nExit);
    std::vector<U>entry(g.n,NONE),exit(g.n,NONE);a.stateGroup.resize(a.one.state.size());a.stateLocal.resize(a.one.state.size());a.groupStart.assign(a.groupCount+1,0);
    for(U t=0;t<a.one.state.size();t++){const auto&s=a.one.state[t];const auto&b=a.one.block[s.owner];(t<b.first+b.nEntry?entry:exit)[s.area]=t;a.stateGroup[t]=owners[s.area];a.groupStart[owners[s.area]+1]++;}
    std::partial_sum(a.groupStart.begin(),a.groupStart.end(),a.groupStart.begin());auto cur=a.groupStart;a.groupStates.resize(a.one.state.size());for(U t=0;t<a.one.state.size();t++){U c=a.stateGroup[t],i=cur[c]++;a.groupStates[i]=t;a.stateLocal[t]=i-a.groupStart[c];}
    for(U c=0;c<a.groupCount;c++)a.maxGroup=std::max(a.maxGroup,a.groupStart[c+1]-a.groupStart[c]);
    a.down.resize(a.two.state.size());a.upperEntry.assign(a.one.state.size(),NONE);
    for(U t=0;t<a.two.state.size();t++){const auto&s=a.two.state[t];const auto&b=a.two.block[s.owner];bool ent=t<b.first+b.nEntry;U d=(ent?entry:exit)[s.area];Require(d!=NONE,"L2 boundary missing L1 role");a.down[t]=d;if(ent)a.upperEntry[d]=t;}
    Search low(U(a.one.state.size()));std::vector<Seed>seeds;seeds.reserve(1);
    for(U c=0;c<a.groupCount;c++){const auto&b=a.two.block[c];for(U j=0;j<b.nExit;j++){seeds.clear();U t=b.first+b.nEntry+j;seeds.push_back({a.down[t],0});RunNetwork(a.one,g,&a.stateGroup,c,seeds,low,bw);
            for(U i=0;i<b.nEntry;i++){U e=b.first+i;a.two.table[b.table+size_t(j)*b.nEntry+i]=Encode(low.d[a.down[e]],c,a.two.state[e].area,a.two.state[t].area);bw.labels++;}low.Reset(bw);}}
    if(verbose){for(const Network*n:{&a.one,&a.two}){U mx=0;W missing=0;for(uint16_t t:n->table){Require(t!=TABLE_RESERVED,"unpublished table entry");if(t==TABLE_INF)missing++;else mx=std::max(mx,U(t));}printf("M table %s max=%u unreachable=%llu\n",n==&a.one?"L1":"L2",mx,(unsigned long long)missing);}}
    a.buildWork=bw.Total();a.buildMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    if(verbose){printf("M atlas L1 K=%zu states=%zu crossings=%zu tableEntries=%zu; L2 K=%zu states=%zu crossings=%zu tableEntries=%zu; maxLocal=%u maxSuperStates=%u bakeWork=%llu bakeMs=%.3f\n",a.one.block.size(),a.one.state.size(),a.one.cross.size(),a.one.table.size(),a.two.block.size(),a.two.state.size(),a.two.cross.size(),a.two.table.size(),a.maxCell,a.maxGroup,(unsigned long long)a.buildWork,a.buildMs);}fflush(stdout);return a;
}
static std::vector<W> Truth(const FlatGraph&g,U goal){
    std::vector<W>d(g.n,FAR);using Pair=std::pair<W,U>;std::priority_queue<Pair,std::vector<Pair>,std::greater<Pair>>q;
    if(!g.enabled[goal]){return d;}d[goal]=0;q.push({0,goal});while(!q.empty()){auto [dd,v]=q.top();q.pop();if(dd!=d[v])continue;
        for(U k=g.in[v];k<g.in[v+1];k++){const auto&e=g.edges[g.rev[k]];if(!g.enabled[e.from])continue;W t=dd+e.w;if(t<d[e.from]){d[e.from]=t;q.push({t,e.from});}}}return d;
}
static U OrderedEdge(const FlatGraph&g,U from,const std::vector<W>&d){
    U best=NONE;for(U i=g.out[from];i<g.out[from+1];i++){const auto&e=g.edges[i];if(g.enabled[e.to]&&d[e.to]<FAR&&e.w+d[e.to]==d[from]&&(best==NONE||e.ordinal<g.edges[best].ordinal))best=i;}return best;
}
static std::vector<U> Active(const FlatGraph&g){std::vector<U>v;for(U a=0;a<g.n;a++)if(g.enabled[a])v.push_back(a);return v;}
static double Quantile(std::vector<double>v,int percent){Require(!v.empty(),"empty percentile");std::sort(v.begin(),v.end());return v[(v.size()*percent+99)/100-1];}
static W Quantile(std::vector<W>v,int percent){Require(!v.empty(),"empty percentile");std::sort(v.begin(),v.end());return v[(v.size()*percent+99)/100-1];}
static FlatGraph SmallGraph(const graph_t&old){FlatGraph g;g.n=old.n;g.enabled.resize(g.n);g.mutableArea.resize(g.n);g.degree.resize(g.n);for(U i=0;i<g.n;i++){g.enabled[i]=old.enabled[i];g.mutableArea[i]=old.mutableArea[i];g.degree[i]=U(old.out[i].size());}for(const auto&e:old.edges)if(old.enabled[e.from]&&old.enabled[e.to])g.edges.push_back({U(e.from),U(e.to),U(e.weight),U(e.ordinal)});g.Index();return g;}
static void SharedSeams(FixtureBase&walk,FixtureBase&fly){
    for(U ax=0;ax<2;ax++){auto all=fly.seams[ax];all.insert(all.end(),walk.seams[ax].begin(),walk.seams[ax].end());std::sort(all.begin(),all.end(),[](const Seam&a,const Seam&b){return std::tie(a.a,a.b,a.type)<std::tie(b.a,b.b,b.type);});std::vector<Seam>out;
        for(const auto&s:all){if(out.empty()||out.back().a!=s.a||out.back().b!=s.b)out.push_back(s);}walk.seams[ax]=fly.seams[ax]=out;U walking=0;for(const auto&s:out)walking+=s.type==2;
        printf("A synthetic seam axis=%u matchedPairs=%zu walkPairs=%u flyIncludesWalk=1 nearestTangentialRadius=256 nativeWeightsFrozen=1 seamScalar=1+ceil(anchorGap*100/300)+sourceWalkLedge250\n",ax,out.size(),walking);}
}
}
namespace overlay_three {
using namespace overlay_two;
constexpr U ROWS=16, GROUP_SLOTS=8, LANDMARKS=8;
constexpr W ARC_CAP=W(1)<<28, DOOR_CAP=W(1)<<16, BATCH_CAP=W(1)<<25;
struct Box {float lo[3]={INFINITY,INFINITY,INFINITY},hi[3]={-INFINITY,-INFINITY,-INFINITY};};
struct Door {std::string name;std::vector<U>areas;};
struct CensusGeometry {std::vector<std::array<int,3>>nodes;std::vector<Box>doors;float boxLo[3],boxHi[3];};
static CensusGeometry ReadCensus(const char*aas,const char*map){
    CensusGeometry c;std::ifstream f(aas);std::string line;bool box=false;while(std::getline(f,line)){float l[3],h[3];if(sscanf(line.c_str()," (%f %f %f)-(%f %f %f)",&l[0],&l[1],&l[2],&h[0],&h[1],&h[2])==6&&!box){std::copy(l,l+3,c.boxLo);std::copy(h,h+3,c.boxHi);box=true;}int n;if(sscanf(line.c_str(),"nodes %d {",&n)==1){Require(n>1,"overlay3: no BSP nodes");c.nodes.resize(n);for(int i=0;i<n;i++){Require(bool(std::getline(f,line)),"nodes truncated");int id;Require(sscanf(line.c_str()," %d ( %d %d %d )",&id,&c.nodes[i][0],&c.nodes[i][1],&c.nodes[i][2])==4&&id==i,"node parse");}break;}}
    Require(box&&!c.nodes.empty(),"overlay3: census AAS settings/nodes missing");
    std::ifstream m(map);int depth=0;bool door=false;Box b;while(std::getline(m,line)){if(depth==0&&line=="{"){depth=1;door=false;b=Box();continue;}if(depth==1&&line.find("\"classname\" \"func_door\"")!=std::string::npos)door=true;
        if(door){float p[9];if(sscanf(line.c_str()," ( %f %f %f ) ( %f %f %f ) ( %f %f %f )",p,p+1,p+2,p+3,p+4,p+5,p+6,p+7,p+8)==9)for(U i=0;i<3;i++)for(U k=0;k<3;k++){b.lo[k]=std::min(b.lo[k],p[3*i+k]);b.hi[k]=std::max(b.hi[k],p[3*i+k]);}}
        if(line=="{"){depth++;}
        if(line=="}"){depth--;if(depth==0&&door){Require(std::isfinite(b.lo[0]),"empty door brush");c.doors.push_back(b);}}}
    Require(c.doors.size()==4,"overlay3: expected four func_door brushes");return c;
}
static std::vector<Door> CensusDoors(const CensusGeometry&c,const fixture_t&f,const graph_t&g,const char*profile){
    std::vector<Door>result;for(U id=0;id<c.doors.size();id++){Box b=c.doors[id];for(U k=0;k<3;k++){b.lo[k]-=c.boxHi[k];b.hi[k]-=c.boxLo[k];}Door d;d.name="census_"+std::to_string(id);std::vector<int>stack{1};W visited=0,flagged=0;std::vector<uint8_t>seen(g.n,0);
        while(!stack.empty()){int v=stack.back();stack.pop_back();if(!v)continue;if(v<0){U a=U(-v);Require(a<U(g.n),"census leaf range");if((f.contents[a]&12)&&!seen[a]){seen[a]=1;flagged++;if(g.enabled[a])d.areas.push_back(a);}continue;}Require(U(v)<c.nodes.size(),"BSP range");visited++;const auto&node=c.nodes[v];const float*p=&f.planes[4*node[0]];float center[3];for(U k=0;k<3;k++)center[k]=(b.lo[k]+b.hi[k])*0.5f;float d1=p[0]*center[0]+p[1]*center[1]+p[2]*center[2]-p[3];float d2=std::abs((b.hi[0]-center[0])*p[0])+std::abs((b.hi[1]-center[1])*p[1])+std::abs((b.hi[2]-center[2])*p[2]);
            // idBounds::PlaneSide default ON_EPSILON=0.1f; front child[0], back child[1].
            if(d1-d2>0.1f)stack.push_back(node[1]);else if(d1+d2< -0.1f)stack.push_back(node[2]);else{stack.push_back(node[2]);stack.push_back(node[1]);}}
        std::sort(d.areas.begin(),d.areas.end());printf("M census %s door=%u rawBounds=(%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f) expanded=(%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f) BSPnodes=%llu flaggedLeaves=%llu admitted=%zu areas=",profile,id,c.doors[id].lo[0],c.doors[id].lo[1],c.doors[id].lo[2],c.doors[id].hi[0],c.doors[id].hi[1],c.doors[id].hi[2],b.lo[0],b.lo[1],b.lo[2],b.hi[0],b.hi[1],b.hi[2],(unsigned long long)visited,(unsigned long long)flagged,d.areas.size());for(U a:d.areas)printf("%u,",a);printf("\n");result.push_back(std::move(d));}return result;
}
static void Recut(FixtureBase&b){b.graph.Finish();auto labels=GridLabels(b.centres,b.graph.n,-4096,2048,4);Require(Partition(b.graph,2048,32768,b.cut,caps_t(),&labels),"overlay3: census partition refused");}
static void SetCensus(FixtureBase&b,const std::vector<Door>&doors){std::fill(b.graph.mutableArea.begin(),b.graph.mutableArea.end(),0);for(const auto&d:doors)for(U a:d.areas)b.graph.mutableArea[a]=1;Recut(b);}
static U PlantBridge(FixtureBase&b){
    U anchor=NONE;double best=INFINITY;for(U v=1;v<U(b.graph.n);v++)if(b.graph.enabled[v]&&!b.graph.mutableArea[v]){double z=std::abs(b.centres[3*v])+std::abs(b.centres[3*v+1]);if(z<best){best=z;anchor=v;}}Require(anchor!=NONE,"bridge anchor");
    U door=U(b.graph.n);b.graph.n+=17;b.graph.out.resize(b.graph.n);b.graph.in.resize(b.graph.n);b.graph.neighbors.resize(b.graph.n);b.graph.enabled.resize(b.graph.n,1);b.graph.mutableArea.resize(b.graph.n,0);b.graph.mutableArea[door]=1;b.degree.resize(b.graph.n,0);b.centres.resize(size_t(b.graph.n)*3);for(U v=door;v<U(b.graph.n);v++)for(U k=0;k<3;k++)b.centres[3*v+k]=b.centres[3*anchor+k];
    auto add=[&](U u,U v,U w){b.graph.Add(int(u),int(v),w,int(b.degree[u]++));};add(anchor,door,37);add(door,anchor,37);for(U i=1;i<=16;i++){add(door,door+i,11+i);add(door+i,door,11+i);}
    // An independently declared old control, not mislabeled as a census-owned area.
    Require(b.graph.enabled[424],"control 424 excluded");b.graph.mutableArea[424]=1;Recut(b);printf("A planted bridge=%u anchor=%u leaves=%u..%u; removal disconnects all sixteen leaves; synthetic w=37 and 11+leafIndex; control=424\n",door,anchor,door+1,door+16);return door;
}
struct Arc3 {U from,to,w;};
struct Top {std::vector<Arc3>edges;std::vector<U>out,in,rev;W dense=0,kept=0,identities=0;};
static Top SparseTop(const Network&net){
    Top t;t.dense=net.table.size();for(const auto&b:net.block){std::vector<std::pair<U,U>>both;for(U i=0;i<b.nEntry;i++)for(U j=0;j<b.nExit;j++)if(net.state[b.first+i].area==net.state[b.first+b.nEntry+j].area){both.emplace_back(i,j);t.edges.push_back({b.first+b.nEntry+j,b.first+i,0});t.identities++;break;}
        for(U j=0;j<b.nExit;j++)for(U i=0;i<b.nEntry;i++){uint16_t d=net.table[b.table+size_t(j)*b.nEntry+i];if(d==TABLE_INF)continue;bool redundant=false;U ai=net.state[b.first+i].area,aj=net.state[b.first+b.nEntry+j].area;
            if(d)for(auto [ki,kj]:both){U ak=net.state[b.first+ki].area;if(ak==ai||ak==aj)continue;U x=net.table[b.table+size_t(kj)*b.nEntry+i],y=net.table[b.table+size_t(j)*b.nEntry+ki];if(x!=TABLE_INF&&y!=TABLE_INF&&x+y==d){redundant=true;break;}}
            if(!redundant){t.edges.push_back({b.first+i,b.first+b.nEntry+j,d});t.kept++;}}}
    for(const auto&e:net.cross){t.edges.push_back({e.from,e.to,e.w});}
    Cap("top graph arcs",t.edges.size(),ARC_CAP);std::sort(t.edges.begin(),t.edges.end(),[](const Arc3&a,const Arc3&b){return std::tie(a.from,a.to,a.w)<std::tie(b.from,b.to,b.w);});t.edges.shrink_to_fit();U n=U(net.state.size());t.out.assign(n+1,0);t.in.assign(n+1,0);for(const auto&e:t.edges){t.out[e.from+1]++;t.in[e.to+1]++;}std::partial_sum(t.out.begin(),t.out.end(),t.out.begin());std::partial_sum(t.in.begin(),t.in.end(),t.in.begin());t.rev.resize(t.edges.size());auto cur=t.in;for(U i=0;i<t.edges.size();i++)t.rev[cur[t.edges[i].to]++]=i;
    printf("M sparse L2 dense=%llu kept=%llu roleIdentity=%llu crossings=%zu totalArcs=%zu\n",(unsigned long long)t.dense,(unsigned long long)t.kept,(unsigned long long)t.identities,net.cross.size(),t.edges.size());return t;
}
template<class T>static void Tight(std::vector<T>&v){v.shrink_to_fit();}
static void TightAtlas(Atlas&a){auto&p=a.local;Tight(p.cell);Tight(p.members);Tight(p.areaCell);Tight(p.areaLocal);Tight(p.revStart);Tight(p.rev);for(Network*n:{&a.one,&a.two}){Tight(n->block);Tight(n->state);Tight(n->cross);Tight(n->revStart);Tight(n->revCross);Tight(n->table);}Tight(a.groupStart);Tight(a.groupStates);Tight(a.stateGroup);Tight(a.stateLocal);Tight(a.upperEntry);Tight(a.down);}
static std::vector<Door> DoorCopies(const std::vector<Door>&base,U N,U copies){std::vector<Door>out;Cap("door records",base.size()*copies,DOOR_CAP);for(U t=0;t<copies;t++)for(const auto&d:base){Door c;c.name=d.name+"_tile"+std::to_string(t);for(U v:d.areas)c.areas.push_back(t*N+v);out.push_back(std::move(c));}return out;}
}
#pragma once
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <functional>
namespace overlay_four {
using namespace overlay_three;
struct Point4 { double x=0,y=0,z=0; };
static double Distance4(const Point4&a,const Point4&b) {
    return std::sqrt((a.x-b.x)*(a.x-b.x)+(a.y-b.y)*(a.y-b.y)+(a.z-b.z)*(a.z-b.z));
}
struct Geo4 { Point4 start,end; };
struct Fixture4 {
    std::string name;
    bool fly=false,synthetic=false;
    FlatGraph g;
    std::vector<Point4> centres;
    std::vector<Geo4> geo;
    std::vector<uint8_t> water;
    std::vector<Door> census,bridges;
    std::vector<std::pair<U,U>> plants;
    std::unique_ptr<FixtureBase> base;
    U side=1,bridge=NONE,control=NONE;
};
struct Result4 {
    bool reachable=false,coarse=false,exact=false,cached=false;
    long double length=0,bound=0;
    W travel=0;
    std::vector<U> path;
};
}
