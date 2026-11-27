#ifndef DMAPSMOOTH_CORE_H
#define DMAPSMOOTH_CORE_H

#include <vector>
#include <algorithm>
#include <cstdint>
#include <cmath>

// The S4.1 smoothing core of Phase 2C stage 2C-4a, standalone C++17 without idLib, so the
// standalone checks (plans/map-graph/phase2c/stages/2C-4/replan/astra-1/check_smooth.cpp and
// ref_smooth.py) build against this very file; dress_smooth.cpp is its only caller in dmap.
// Rules and their reasons: that folder's ASSUMPTIONS.md. No persistent state, material
// identity, terrain identity or engine dependency.
//
// Preconditions (the caller checks them): binary32 float, finite unit face normals, cosine
// thresholds in [-1, 1], hard bits 0..2 only, every position within 2^23 grid steps of the
// origin on every axis, numTris <= UINT32_MAX / 6. Under them every integer below fits int64
// (the ray test needs no wider type, see SameRay).
namespace dmapsmooth
{
struct Tri
{
	int64_t p[3][3];
	float n[3];
	float cosAngle;
	bool smooth;
	uint8_t hard;
};

struct Corner
{
	float n[3];
	bool changed;
};

struct Stats
{
	uint32_t tris, corners, vertices, wedges, smoothed, planarKept, hardRays, weldedKeys;
};

namespace detail
{
constexpr uint32_t NONE = UINT32_MAX;

inline uint32_t Bits( float value )
{
	static_assert( sizeof( float ) == sizeof( uint32_t ), "binary32 required" );
	uint32_t bits;
	const unsigned char* from = reinterpret_cast<const unsigned char*>( &value );
	unsigned char* to = reinterpret_cast<unsigned char*>( &bits );
	for( unsigned i = 0; i < sizeof( bits ); ++i )
	{
		to[i] = from[i];
	}
	return bits;
}

inline bool SameNormal( const Tri& a, const Tri& b )
{
	return Bits( a.n[0] ) == Bits( b.n[0] ) && Bits( a.n[1] ) == Bits( b.n[1] ) && Bits( a.n[2] ) == Bits( b.n[2] );
}

struct Key
{
	int64_t p[3];
	uint32_t parent;
	uint32_t head; // Union size first; corner-list head after welding.
};

inline uint64_t Mix( uint64_t x )
{
	x ^= x >> 30;
	x *= UINT64_C( 0xbf58476d1ce4e5b9 );
	x ^= x >> 27;
	x *= UINT64_C( 0x94d049bb133111eb );
	return x ^ ( x >> 31 );
}

inline uint64_t Slot( const int64_t* p, const std::vector<uint32_t>& table, const std::vector<Key>& keys )
{
	const uint64_t mask = table.size() - 1;
	uint64_t slot = Mix( Mix( uint64_t( p[0] ) ) ^ Mix( uint64_t( p[1] ) + UINT64_C( 0x9e3779b97f4a7c15 ) ) ^ Mix( uint64_t( p[2] ) + UINT64_C( 0x243f6a8885a308d3 ) ) ) & mask;
	while( table[slot] != NONE )
	{
		const Key& key = keys[table[slot]];
		if( key.p[0] == p[0] && key.p[1] == p[1] && key.p[2] == p[2] )
		{
			break;
		}
		slot = ( slot + 1 ) & mask;
	}
	return slot;
}

inline uint32_t KeyRoot( std::vector<Key>& keys, uint32_t i )
{
	while( keys[i].parent != i )
	{
		keys[i].parent = keys[keys[i].parent].parent;
		i = keys[i].parent;
	}
	return i;
}

inline void JoinKeys( std::vector<Key>& keys, uint32_t a, uint32_t b )
{
	a = KeyRoot( keys, a );
	b = KeyRoot( keys, b );
	if( a == b )
	{
		return;
	}
	if( keys[a].head < keys[b].head )
	{
		std::swap( a, b );
	}
	keys[b].parent = a;
	keys[a].head += keys[b].head;
}

struct Ray
{
	int64_t v[3];
	int64_t lengthSquared;
	bool hard;
};

inline bool SameRay( const Ray& a, const Ray& b )
{
	if( a.hard || b.hard || a.lengthSquared < 16 || b.lengthSquared < 16 )
	{
		return false;
	}
	const int64_t dot = ( a.v[0] * b.v[0] + a.v[1] * b.v[1] ) + a.v[2] * b.v[2];
	if( dot <= 0 )
	{
		return false;
	}
	const int64_t x = a.v[1] * b.v[2] - a.v[2] * b.v[1];
	const int64_t y = a.v[2] * b.v[0] - a.v[0] * b.v[2];
	const int64_t z = a.v[0] * b.v[1] - a.v[1] * b.v[0];
	// |a x b| / |longer| is the shorter tip's distance to the longer line, so the test is
	// |a x b|^2 <= 4 |longer|^2, in exact integers. With every position within 2^23 steps a ray
	// component is at most 2^24 and |longer|^2 at most 3 * 2^48, so 4 |longer|^2 is below 2^52: a
	// cross component above 2^26 in magnitude already fails, and below that bound the sum of the
	// three squares (under 3 * 2^52) fits int64. The same verdicts as the draft's __int128 sum,
	// which MSVC does not have
	const int64_t bound = int64_t( 1 ) << 26;
	if( x > bound || x < -bound || y > bound || y < -bound || z > bound || z < -bound )
	{
		return false;
	}
	const int64_t crossSquared = ( x * x + y * y ) + z * z;
	return crossSquared <= 4 * std::max( a.lengthSquared, b.lengthSquared );
}

inline uint8_t RotatedHard( const Tri& t, uint32_t k )
{
	return uint8_t( ( ( t.hard >> k ) & 1 ) | ( ( ( t.hard >> ( ( k + 1 ) % 3 ) ) & 1 ) << 1 ) | ( ( ( t.hard >> ( ( k + 2 ) % 3 ) ) & 1 ) << 2 ) );
}

inline bool CornerLess( const Tri* tris, uint32_t a, uint32_t b )
{
	const Tri& ta = tris[a / 3];
	const Tri& tb = tris[b / 3];
	for( unsigned axis = 0; axis < 3; ++axis )
	{
		const uint32_t x = Bits( ta.n[axis] );
		const uint32_t y = Bits( tb.n[axis] );
		if( x != y )
		{
			return x < y;
		}
	}
	for( unsigned corner = 0; corner < 3; ++corner )
	{
		for( unsigned axis = 0; axis < 3; ++axis )
		{
			const int64_t x = ta.p[( a + corner ) % 3][axis];
			const int64_t y = tb.p[( b + corner ) % 3][axis];
			if( x != y )
			{
				return x < y;
			}
		}
	}
	if( Bits( ta.cosAngle ) != Bits( tb.cosAngle ) )
	{
		return Bits( ta.cosAngle ) < Bits( tb.cosAngle );
	}
	return RotatedHard( ta, a % 3 ) < RotatedHard( tb, b % 3 );
}

struct Work
{
	uint32_t source, parent;
	Ray rays[2];
	double sum[3];
	float result[3];
	bool planar, changed;
};

inline uint32_t WedgeRoot( std::vector<Work>& work, uint32_t i )
{
	while( work[i].parent != i )
	{
		work[i].parent = work[work[i].parent].parent;
		i = work[i].parent;
	}
	return i;
}

inline double Angle( const Work& w )
{
	const double ax = double( w.rays[0].v[0] ), ay = double( w.rays[0].v[1] ), az = double( w.rays[0].v[2] );
	const double bx = double( w.rays[1].v[0] ), by = double( w.rays[1].v[1] ), bz = double( w.rays[1].v[2] );
	const double x = ay * bz - az * by;
	const double y = az * bx - ax * bz;
	const double z = ax * by - ay * bx;
	const double square = ( x * x + y * y ) + z * z;
	if( square == 0.0 )
	{
		return 0.0; // Includes collinear opposite rays: a zero-area triangle has zero weight.
	}
	return std::atan2( std::sqrt( square ), ( ax * bx + ay * by ) + az * bz );
}
} // namespace detail

// wedgeIds is optional diagnostic output: NONE for nonparticipants, otherwise
// a component label (the partition, not label numbering, is order invariant).
inline void Smooth( const Tri* tris, uint32_t numTris, Corner* out, Stats* stats, uint32_t* wedgeIds = nullptr )
{
	using namespace detail;
	Stats count = {};
	count.tris = numTris;
	const uint32_t total = numTris * 3;
	for( uint32_t t = 0; t < numTris; ++t )
	{
		for( uint32_t k = 0; k < 3; ++k )
		{
			Corner& c = out[t * 3 + k];
			for( unsigned axis = 0; axis < 3; ++axis )
			{
				c.n[axis] = tris[t].n[axis];
			}
			c.changed = false;
			if( wedgeIds != nullptr )
			{
				wedgeIds[t * 3 + k] = NONE;
			}
		}
		if( tris[t].smooth )
		{
			count.corners += 3;
			for( unsigned k = 0; k < 3; ++k )
			{
				count.hardRays += ( ( tris[t].hard >> k ) & 1 ) * 2;
			}
		}
	}
	if( count.corners == 0 )
	{
		if( stats != nullptr )
		{
			*stats = count;
		}
		return;
	}

	uint64_t capacity = 1;
	while( capacity < uint64_t( count.corners ) * 2 )
	{
		capacity *= 2;
	}
	std::vector<uint32_t> table( capacity, NONE );
	std::vector<Key> keys;
	std::vector<uint32_t> next( total, NONE );
	for( uint32_t i = 0; i < total; ++i )
	{
		if( !tris[i / 3].smooth )
		{
			continue;
		}
		const int64_t* p = tris[i / 3].p[i % 3];
		const uint64_t slot = Slot( p, table, keys );
		if( table[slot] == NONE )
		{
			const uint32_t id = uint32_t( keys.size() );
			keys.push_back( { { p[0], p[1], p[2] }, id, 1 } );
			table[slot] = id;
		}
		next[i] = table[slot];
	}
	for( uint32_t i = 0; i < keys.size(); ++i )
	{
		// One of each +/- pair of the 26 neighbouring integer keys is sufficient.
		for( int dx = 0; dx <= 1; ++dx )
		{
			for( int dy = -1; dy <= 1; ++dy )
			{
				for( int dz = -1; dz <= 1; ++dz )
				{
					if( dx == 0 && ( dy < 0 || ( dy == 0 && dz <= 0 ) ) )
					{
						continue;
					}
					const int64_t p[3] = { keys[i].p[0] + dx, keys[i].p[1] + dy, keys[i].p[2] + dz };
					const uint32_t other = table[Slot( p, table, keys )];
					if( other != NONE )
					{
						JoinKeys( keys, i, other );
					}
				}
			}
		}
	}
	for( uint32_t i = 0; i < keys.size(); ++i )
	{
		keys[i].head = NONE;
		if( KeyRoot( keys, i ) == i )
		{
			++count.vertices;
		}
	}
	count.weldedKeys = uint32_t( keys.size() ) - count.vertices;
	for( uint32_t i = 0; i < total; ++i )
	{
		if( next[i] != NONE )
		{
			Key& vertex = keys[KeyRoot( keys, next[i] )];
			next[i] = vertex.head;
			vertex.head = i;
		}
	}

	std::vector<uint32_t> order;
	std::vector<Work> work;
	for( const Key& vertex : keys )
	{
		if( vertex.head == NONE )
		{
			continue;
		}
		order.clear();
		for( uint32_t i = vertex.head; i != NONE; i = next[i] )
		{
			order.push_back( i );
		}
		std::sort( order.begin(), order.end(), [tris]( uint32_t a, uint32_t b ) { return CornerLess( tris, a, b ); } );
		work.clear();
		work.resize( order.size() );
		for( uint32_t i = 0; i < order.size(); ++i )
		{
			Work& w = work[i];
			w.source = order[i];
			w.parent = i;
			w.planar = true;
			const Tri& t = tris[w.source / 3];
			const uint32_t k = w.source % 3;
			for( unsigned r = 0; r < 2; ++r )
			{
				Ray& ray = w.rays[r];
				ray.lengthSquared = 0;
				ray.hard = ( t.hard & ( 1 << ( r == 0 ? k : ( k + 2 ) % 3 ) ) ) != 0;
				for( unsigned axis = 0; axis < 3; ++axis )
				{
					ray.v[axis] = t.p[( k + r + 1 ) % 3][axis] - t.p[k][axis];
					ray.lengthSquared += ray.v[axis] * ray.v[axis];
				}
			}
		}
		for( uint32_t i = 0; i < work.size(); ++i )
		{
			const Tri& a = tris[work[i].source / 3];
			for( uint32_t j = 0; j < i; ++j )
			{
				const Tri& b = tris[work[j].source / 3];
				const double dot = ( double( a.n[0] ) * b.n[0] + double( a.n[1] ) * b.n[1] ) + double( a.n[2] ) * b.n[2];
				if( dot < double( std::max( a.cosAngle, b.cosAngle ) ) )
				{
					continue;
				}
				bool link = false;
				for( unsigned ar = 0; ar < 2 && !link; ++ar )
				{
					for( unsigned br = 0; br < 2; ++br )
					{
						link = link || SameRay( work[i].rays[ar], work[j].rays[br] );
					}
				}
				if( link )
				{
					const uint32_t x = WedgeRoot( work, i ), y = WedgeRoot( work, j );
					work[std::max( x, y )].parent = std::min( x, y );
				}
			}
		}
		for( uint32_t i = 0; i < work.size(); ++i )
		{
			Work& root = work[WedgeRoot( work, i )];
			root.planar = root.planar && SameNormal( tris[root.source / 3], tris[work[i].source / 3] );
		}
		// Canonical input-value order, including across hash insertion permutations.
		for( uint32_t i = 0; i < work.size(); ++i )
		{
			Work& root = work[WedgeRoot( work, i )];
			if( !root.planar )
			{
				const double alpha = Angle( work[i] );
				const Tri& t = tris[work[i].source / 3];
				for( unsigned axis = 0; axis < 3; ++axis )
				{
					root.sum[axis] += alpha * double( t.n[axis] );
				}
			}
		}
		for( uint32_t i = 0; i < work.size(); ++i )
		{
			Work& w = work[i];
			if( w.parent != i )
			{
				continue;
			}
			const uint32_t label = count.wedges++;
			const double square = ( w.sum[0] * w.sum[0] + w.sum[1] * w.sum[1] ) + w.sum[2] * w.sum[2];
			w.changed = !w.planar && square != 0.0;
			if( w.changed )
			{
				const double length = std::sqrt( square );
				for( unsigned axis = 0; axis < 3; ++axis )
				{
					w.result[axis] = float( w.sum[axis] / length );
				}
			}
			// source output index is no longer needed as a label holder on roots.
			w.source = label;
		}
		for( uint32_t i = 0; i < work.size(); ++i )
		{
			const Work& root = work[WedgeRoot( work, i )];
			Corner& c = out[order[i]];
			if( root.changed )
			{
				for( unsigned axis = 0; axis < 3; ++axis )
				{
					c.n[axis] = root.result[axis];
				}
				c.changed = true;
				++count.smoothed;
			}
			if( root.planar )
			{
				++count.planarKept;
			}
			if( wedgeIds != nullptr )
			{
				wedgeIds[order[i]] = root.source;
			}
		}
	}
	if( stats != nullptr )
	{
		*stats = count;
	}
}
} // namespace dmapsmooth

#endif
