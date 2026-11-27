#ifndef DMAPSMOOTH_CORE_H
#define DMAPSMOOTH_CORE_H

#include <vector>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstring>

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

// SmoothCuts (Phase 2C P3.1, the fix for K1): the second pass, over Smooth's wedges.
//
// Smooth gives a wedge whose corners lie on one plane that plane's normal, and a wedge on two
// planes the angle-weighted mean of the two. Both are right at a corner of the brushwork and
// wrong at a vertex a cut put inside a face or in the middle of an edge (an area portal, a BSP
// split, a chunk line, the T-junction fix): the uncut surface interpolates there, so every cut
// line drew a kink. Such a wedge is a pass-through wedge, told from its rays with SameRay's
// tolerance. A wedge's rays are grouped into directions (the transitive closure of SameRay, each
// represented by its shortest ray, then the smallest vector; a ray under 4 steps, too short for
// SameRay, is left out when it is an inner edge of the fan); a direction is shared when rays of
// two corners hold it. Then:
//   in a face:      one plane, every direction shared (a closed fan);
//   on a hard edge: one plane, exactly two directions unshared, and they are opposite;
//   on a crease:    two planes, every direction shared, and the directions held by both planes
//                   are exactly two, opposite.
// Its lines: the crease, the hard edge, or in a face each pair of directions that are each
// other's only opposite. A wedge with a corner at a brush corner (originals, the brush sides'
// winding points, within one grid step) is never pass-through: the mesh cannot tell a brush
// corner on a straight border from a cut, and the brushwork's own vertices keep Smooth's value.
//
// Along each line the pass follows the mesh both ways, through pass-through wedges that continue
// the line, to the first wedge that does not (memoized per wedge and direction; a walk that never
// ends makes its wedge fixed). The wedge takes the mean over its lines (only those that run from
// brush corner to brush corner, when it has such: on a face that is not a triangle only an edge's
// lerp is the uncut shading) of the lerp of the two end
// values by distance, (d1 v0 + d0 v1) / (d0 + d1), unnormalized until the write, so on a
// triangular face it is the barycentric interpolant. An end that is itself pass-through (a line
// that stops at another) is an unknown too: Jacobi sweeps until no written bit changes. When every
// end value is the same vector the wedge takes that vector, so a flat floor keeps the stock bits.
// Last, a wedge that is neither pass-through nor at a brush corner and has a ray too short for
// SameRay (under 4 steps, a cut beside a vertex) takes the value of the wedge at the tip of its
// shortest such ray, when that one is pass-through or at a brush corner. The output does not
// depend on the tri order or the corner rotation: every choice is made from positions and normals,
// never from wedge labels, and an equal-ray tie that reaches two wedges gives no line.
//
// wedgeIds and numWedges are Smooth's (its optional output and stats.wedges); originals holds
// numOriginals positions in grid steps, three int64 each, and may be null; out holds Smooth's corners and is updated in place, changed meaning the value differs from
// the tri's input normal.
struct CutStats
{
	uint32_t inFace, onHardEdge, onCrease, lines, keptAtBrushCorners, brushEdgeOnly, ambiguous, demoted, shortCopies, shortStuck, sweeps, unconverged, moved;
};

namespace detail
{
constexpr uint32_t CUT_MAX_SWEEPS = 64;
constexpr uint32_t CUT_MAX_RAYS = 4096;	// a wedge with more rays than this is left fixed
constexpr uint16_t NO_CLUSTER = UINT16_MAX;

inline Ray CornerRay( const Tri* tris, uint32_t corner, unsigned r )
{
	const Tri& t = tris[corner / 3];
	const uint32_t k = corner % 3;
	Ray ray;
	ray.lengthSquared = 0;
	ray.hard = ( t.hard & ( 1 << ( r == 0 ? k : ( k + 2 ) % 3 ) ) ) != 0;
	for( unsigned axis = 0; axis < 3; ++axis )
	{
		ray.v[axis] = t.p[( k + r + 1 ) % 3][axis] - t.p[k][axis];
		ray.lengthSquared += ray.v[axis] * ray.v[axis];
	}
	return ray;
}

// the corner at the tip of ray r of a corner, in the same tri
inline uint32_t TipCorner( uint32_t corner, unsigned r )
{
	const uint32_t k = corner % 3;
	return corner - k + ( k + r + 1 ) % 3;
}

// the ray of a tip corner that points back at the corner it was reached from
inline unsigned BackRay( uint32_t from, uint32_t tip )
{
	return ( tip % 3 + 1 ) % 3 == from % 3 ? 0u : 1u;
}

inline bool OppositeRay( const Ray& a, const Ray& b )
{
	Ray negated = b;
	for( unsigned axis = 0; axis < 3; ++axis )
	{
		negated.v[axis] = -b.v[axis];
	}
	return SameRay( a, negated );
}

// shorter first, then the smaller vector: a geometric order, the same in every tri order
inline bool RayLess( const Ray& a, const Ray& b )
{
	if( a.lengthSquared != b.lengthSquared )
	{
		return a.lengthSquared < b.lengthSquared;
	}
	for( unsigned axis = 0; axis < 3; ++axis )
	{
		if( a.v[axis] != b.v[axis] )
		{
			return a.v[axis] < b.v[axis];
		}
	}
	return false;
}

inline bool SameVector( const Ray& a, const Ray& b )
{
	return a.v[0] == b.v[0] && a.v[1] == b.v[1] && a.v[2] == b.v[2];
}

struct CutRay
{
	Ray ray;
	uint32_t corner;
	uint32_t parent;	// union-find over SameRay, then the cluster index
	uint8_t r;
	uint8_t plane;
};

struct CutCluster
{
	uint32_t rep;			// index into the wedge's rays: the shortest, then the smallest vector
	uint32_t firstCorner;
	uint32_t partner;		// the only opposite cluster, or NONE
	uint32_t numOpposite;
	bool shared;			// rays of two corners
	bool usable;			// the rep's equal-vector rays all reach one wedge
	uint8_t planes;			// bit per plane
};

// a line of a pass-through wedge: its two sides, each a direction (the cluster's rep ray)
struct CutSide
{
	uint32_t corner;
	uint16_t cluster;
	uint8_t r;
};

struct CutLine
{
	uint32_t wedge;
	CutSide side[2];
};

inline uint32_t RayRoot( std::vector<CutRay>& rays, uint32_t i )
{
	while( rays[i].parent != i )
	{
		rays[i].parent = rays[rays[i].parent].parent;
		i = rays[i].parent;
	}
	return i;
}

inline bool SameBits3( const double* a, const double* b )
{
	for( unsigned axis = 0; axis < 3; ++axis )
	{
		uint64_t x, y;
		static_assert( sizeof( double ) == sizeof( uint64_t ), "binary64 required" );
		std::memcpy( &x, &a[axis], sizeof( x ) );
		std::memcpy( &y, &b[axis], sizeof( y ) );
		if( x != y )
		{
			return false;
		}
	}
	return true;
}

inline void Unit( const double* v, float* out, const float* keep )
{
	const double square = ( v[0] * v[0] + v[1] * v[1] ) + v[2] * v[2];
	if( !( square > 0.0 ) )
	{
		out[0] = keep[0];
		out[1] = keep[1];
		out[2] = keep[2];
		return;
	}
	const double length = std::sqrt( square );
	for( unsigned axis = 0; axis < 3; ++axis )
	{
		out[axis] = float( v[axis] / length );
	}
}
} // namespace detail

inline void SmoothCuts( const Tri* tris, uint32_t numTris, const uint32_t* wedgeIds, uint32_t numWedges,
						const int64_t* originals, uint32_t numOriginals, Corner* out, CutStats* stats )
{
	using namespace detail;
	CutStats count = {};
	const uint32_t total = numTris * 3;
	if( numWedges == 0 )
	{
		if( stats != nullptr )
		{
			*stats = count;
		}
		return;
	}

	// the corners of each wedge (a counting sort); the phase-1 value of each (one per wedge)
	std::vector<uint32_t> start( size_t( numWedges ) + 1, 0 );
	for( uint32_t i = 0; i < total; ++i )
	{
		if( wedgeIds[i] != NONE )
		{
			++start[wedgeIds[i] + 1];
		}
	}
	for( uint32_t w = 0; w < numWedges; ++w )
	{
		start[w + 1] += start[w];
	}
	std::vector<uint32_t> members( start[numWedges] );
	{
		std::vector<uint32_t> fill( start.begin(), start.end() - 1 );
		for( uint32_t i = 0; i < total; ++i )
		{
			if( wedgeIds[i] != NONE )
			{
				members[fill[wedgeIds[i]]++] = i;
			}
		}
	}

	// the brush corners, hashed like Smooth's keys; a corner within one step of one is a brush corner
	std::vector<uint32_t> originalTable;
	std::vector<Key> originalKeys;
	if( numOriginals > 0 )
	{
		uint64_t capacity = 1;
		while( capacity < uint64_t( numOriginals ) * 2 )
		{
			capacity *= 2;
		}
		originalTable.assign( capacity, NONE );
		for( uint32_t i = 0; i < numOriginals; ++i )
		{
			const int64_t* o = originals + size_t( i ) * 3;
			const uint64_t slot = Slot( o, originalTable, originalKeys );
			if( originalTable[slot] == NONE )
			{
				originalTable[slot] = uint32_t( originalKeys.size() );
				originalKeys.push_back( { { o[0], o[1], o[2] }, 0, 0 } );
			}
		}
	}
	// 0 unknown, 1 no, 2 yes
	std::vector<uint8_t> atBrushCorner( numWedges, 0 );
	auto AtBrushCorner = [&]( uint32_t w ) -> bool
	{
		if( atBrushCorner[w] == 0 )
		{
			atBrushCorner[w] = 1;
			for( uint32_t m = start[w]; m < start[w + 1] && atBrushCorner[w] == 1 && !originalKeys.empty(); ++m )
			{
				const int64_t* p = tris[members[m] / 3].p[members[m] % 3];
				for( int dx = -1; dx <= 1 && atBrushCorner[w] == 1; ++dx )
				{
					for( int dy = -1; dy <= 1 && atBrushCorner[w] == 1; ++dy )
					{
						for( int dz = -1; dz <= 1; ++dz )
						{
							const int64_t q[3] = { p[0] + dx, p[1] + dy, p[2] + dz };
							if( originalTable[Slot( q, originalTable, originalKeys )] != NONE )
							{
								atBrushCorner[w] = 2;
								break;
							}
						}
					}
				}
			}
		}
		return atBrushCorner[w] == 2;
	};

	// classify; the lines of each pass-through wedge, lineStart[w]..lineStart[w + 1]; the cluster of
	// each ray of a pass-through wedge's corners (for the walks' way back)
	std::vector<uint32_t> lineStart( size_t( numWedges ) + 1, 0 );
	std::vector<CutLine> lines;
	std::vector<uint16_t> rayCluster( size_t( total ) * 2, NO_CLUSTER );
	std::vector<CutRay> rays;
	std::vector<CutCluster> clusters;
	std::vector<uint32_t> clusterOf, rootOf;
	for( uint32_t w = 0; w < numWedges; ++w )
	{
		lineStart[w] = uint32_t( lines.size() );
		const uint32_t first = start[w], last = start[w + 1];
		if( ( last - first ) * 2 > CUT_MAX_RAYS )
		{
			continue;
		}
		const Tri* planeTri[2] = { &tris[members[first] / 3], nullptr };
		bool tooMany = false;
		for( uint32_t m = first; m < last && !tooMany; ++m )
		{
			const Tri& t = tris[members[m] / 3];
			if( SameNormal( t, *planeTri[0] ) )
			{
				continue;
			}
			if( planeTri[1] == nullptr )
			{
				planeTri[1] = &t;
			}
			else if( !SameNormal( t, *planeTri[1] ) )
			{
				tooMany = true;
			}
		}
		if( tooMany )
		{
			continue;
		}
		const unsigned numPlanes = planeTri[1] == nullptr ? 1 : 2;

		// the rays and their directions
		rays.clear();
		for( uint32_t m = first; m < last; ++m )
		{
			const uint32_t corner = members[m];
			const uint8_t plane = uint8_t( SameNormal( tris[corner / 3], *planeTri[0] ) ? 0 : 1 );
			for( unsigned r = 0; r < 2; ++r )
			{
				const Ray ray = CornerRay( tris, corner, r );
				if( ray.lengthSquared < 16 )
				{
					// too short for SameRay: an inner edge of the fan (another corner holds it too) holds no
					// direction and is left out; one on the fan's border stays, unshared, so the wedge stays
					// fixed (a reflex corner with both border neighbours that close must not look like a face)
					bool inner = false;
					for( uint32_t m2 = first; m2 < last && !inner; ++m2 )
					{
						for( unsigned r2 = 0; r2 < 2 && m2 != m; ++r2 )
						{
							inner = inner || SameVector( ray, CornerRay( tris, members[m2], r2 ) );
						}
					}
					if( inner )
					{
						continue;
					}
				}
				const uint32_t index = uint32_t( rays.size() );
				rays.push_back( { ray, corner, index, uint8_t( r ), plane } );
			}
		}
		for( uint32_t i = 0; i < rays.size(); ++i )
		{
			for( uint32_t j = 0; j < i; ++j )
			{
				if( SameRay( rays[i].ray, rays[j].ray ) )
				{
					const uint32_t a = RayRoot( rays, i ), b = RayRoot( rays, j );
					rays[std::max( a, b )].parent = std::min( a, b );
				}
			}
		}
		clusters.clear();
		clusterOf.assign( rays.size(), NONE );
		rootOf.resize( rays.size() );
		for( uint32_t i = 0; i < rays.size(); ++i )
		{
			const uint32_t root = RayRoot( rays, i );
			rootOf[i] = root;
			if( clusterOf[root] == NONE )
			{
				clusterOf[root] = uint32_t( clusters.size() );
				clusters.push_back( { i, rays[i].corner, NONE, 0, false, true, 0 } );
			}
			CutCluster& c = clusters[clusterOf[root]];
			c.shared = c.shared || rays[i].corner != c.firstCorner;
			c.planes |= uint8_t( 1 << rays[i].plane );
			if( RayLess( rays[i].ray, rays[c.rep].ray ) )
			{
				c.rep = i;
			}
		}
		// parent becomes the cluster index (after every root is known)
		for( uint32_t i = 0; i < rays.size(); ++i )
		{
			rays[i].parent = clusterOf[rootOf[i]];
		}
		// an equal-vector tie that reaches two wedges: no line through that direction
		for( uint32_t i = 0; i < rays.size(); ++i )
		{
			CutCluster& c = clusters[rays[i].parent];
			const CutRay& rep = rays[c.rep];
			if( i != c.rep && SameVector( rays[i].ray, rep.ray ) && wedgeIds[TipCorner( rays[i].corner, rays[i].r )] != wedgeIds[TipCorner( rep.corner, rep.r )] )
			{
				c.usable = false;
			}
		}
		uint32_t numUnshared = 0;
		for( const CutCluster& c : clusters )
		{
			numUnshared += c.shared ? 0 : 1;
		}
		auto AddLine = [&]( uint32_t a, uint32_t b )
		{
			// the side with the smaller rep first, so a wedge's lines sum in a geometric order
			if( RayLess( rays[clusters[b].rep].ray, rays[clusters[a].rep].ray ) )
			{
				std::swap( a, b );
			}
			const CutRay& ra = rays[clusters[a].rep];
			const CutRay& rb = rays[clusters[b].rep];
			lines.push_back( { w, { { ra.corner, uint16_t( a ), ra.r }, { rb.corner, uint16_t( b ), rb.r } } } );
		};

		bool passThrough = false;
		if( numPlanes == 1 && numUnshared == 0 )
		{
			for( uint32_t a = 0; a < clusters.size(); ++a )
			{
				for( uint32_t b = a + 1; b < clusters.size(); ++b )
				{
					if( OppositeRay( rays[clusters[a].rep].ray, rays[clusters[b].rep].ray ) )
					{
						clusters[a].partner = b;
						clusters[b].partner = a;
						++clusters[a].numOpposite;
						++clusters[b].numOpposite;
					}
				}
			}
			uint32_t numAmbiguous = 0;
			for( uint32_t a = 0; a < clusters.size(); ++a )
			{
				numAmbiguous += clusters[a].numOpposite > 1 ? 1 : 0;
				const uint32_t b = clusters[a].partner;
				if( clusters[a].numOpposite == 1 && b > a && clusters[b].numOpposite == 1 && clusters[a].usable && clusters[b].usable )
				{
					AddLine( a, b );
				}
			}
			count.ambiguous += numAmbiguous > 0 ? 1 : 0;
			// the lines in a geometric order (their smaller rep's), not the clusters' numbering
			std::sort( lines.begin() + lineStart[w], lines.end(), [&]( const CutLine& x, const CutLine& y )
			{
				return RayLess( rays[clusters[x.side[0].cluster].rep].ray, rays[clusters[y.side[0].cluster].rep].ray );
			} );
			passThrough = lines.size() > lineStart[w];
			if( passThrough && AtBrushCorner( w ) )
			{
				lines.resize( lineStart[w] );
				++count.keptAtBrushCorners;
				continue;
			}
			count.inFace += passThrough ? 1 : 0;
		}
		else if( numPlanes == 1 && numUnshared == 2 )
		{
			uint32_t u[2], n = 0;
			for( uint32_t c = 0; c < clusters.size(); ++c )
			{
				if( !clusters[c].shared )
				{
					u[n++] = c;
				}
			}
			if( clusters[u[0]].usable && clusters[u[1]].usable && OppositeRay( rays[clusters[u[0]].rep].ray, rays[clusters[u[1]].rep].ray ) )
			{
				if( AtBrushCorner( w ) )
				{
					++count.keptAtBrushCorners;
					continue;
				}
				AddLine( u[0], u[1] );
				passThrough = true;
				++count.onHardEdge;
			}
		}
		else if( numPlanes == 2 && numUnshared == 0 )
		{
			uint32_t u[3], n = 0;
			for( uint32_t c = 0; c < clusters.size() && n < 3; ++c )
			{
				if( clusters[c].planes == 3 )
				{
					u[n++] = c;
				}
			}
			if( n == 2 && clusters[u[0]].usable && clusters[u[1]].usable && OppositeRay( rays[clusters[u[0]].rep].ray, rays[clusters[u[1]].rep].ray ) )
			{
				if( AtBrushCorner( w ) )
				{
					++count.keptAtBrushCorners;
					continue;
				}
				AddLine( u[0], u[1] );
				passThrough = true;
				++count.onCrease;
			}
		}
		if( passThrough )
		{
			for( const CutRay& ray : rays )
			{
				rayCluster[size_t( ray.corner ) * 2 + ray.r] = uint16_t( ray.parent );
			}
		}
	}
	lineStart[numWedges] = uint32_t( lines.size() );

	std::vector<double> value( size_t( numWedges ) * 3 );
	for( uint32_t w = 0; w < numWedges; ++w )
	{
		for( unsigned axis = 0; axis < 3; ++axis )
		{
			value[size_t( w ) * 3 + axis] = out[members[start[w]]].n[axis];
		}
	}
	std::vector<bool> passWedge( numWedges, false );
	for( const CutLine& line : lines )
	{
		passWedge[line.wedge] = true;
	}

	// the walks, memoized per side (line l's side s is 2l + s): the end wedge and the distance to it
	const uint32_t numSides = uint32_t( lines.size() ) * 2;
	std::vector<uint32_t> sideEnd( numSides, NONE ), sideNext( numSides, NONE );
	std::vector<double> sideLength( numSides, 0.0 ), sideDistance( numSides, 0.0 );
	std::vector<uint8_t> sideState( numSides, 0 );	// 0 open, 1 on the path, 2 ends, 3 never ends
	std::vector<uint32_t> path;
	std::vector<uint8_t> lineUsed( lines.size(), 1 );
	// The walks, then which lines each wedge uses. When some wedge uses only its brush-edge lines, the walks
	// once more, going on through a wedge only along a line that wedge uses (a cut line stops where it crosses
	// a brush edge inside a merged coplanar region: the Opus review, m1), and the lines used once more
	for( int pass = 0; pass < 2; ++pass )
	{
		for( bool again = true; again; )
		{
			again = false;
			std::fill( sideState.begin(), sideState.end(), uint8_t( 0 ) );
			// one step of every side of a pass-through wedge: the next wedge, and the side it goes on by
			for( uint32_t s = 0; s < numSides; ++s )
			{
				const CutLine& line = lines[s / 2];
				if( !passWedge[line.wedge] )
				{
					continue;
				}
				const CutSide& side = line.side[s % 2];
				const uint32_t tip = TipCorner( side.corner, side.r );
				const uint32_t next = wedgeIds[tip];
				sideLength[s] = std::sqrt( double( CornerRay( tris, side.corner, side.r ).lengthSquared ) );
				sideEnd[s] = next;
				sideNext[s] = NONE;
				if( passWedge[next] )
				{
					const uint16_t back = rayCluster[size_t( tip ) * 2 + BackRay( side.corner, tip )];
					for( uint32_t l = lineStart[next]; l < lineStart[next + 1] && back != NO_CLUSTER; ++l )
					{
						for( unsigned u = 0; u < 2; ++u )
						{
							if( lines[l].side[u].cluster == back && ( pass == 0 || lineUsed[l] != 0 ) )
							{
								sideNext[s] = l * 2 + ( 1 - u );
							}
						}
					}
				}
			}
			// follow each side to its end, resolving the path backwards
			for( uint32_t s0 = 0; s0 < numSides; ++s0 )
			{
				if( !passWedge[lines[s0 / 2].wedge] || sideState[s0] != 0 )
				{
					continue;
				}
				path.clear();
				uint32_t s = s0;
				uint8_t verdict = 2;
				for( ;; )
				{
					sideState[s] = 1;
					path.push_back( s );
					const uint32_t n = sideNext[s];
					if( n == NONE )
					{
						break;
					}
					if( sideState[n] == 1 || sideState[n] == 3 )
					{
						verdict = 3;
						break;
					}
					if( sideState[n] == 2 )
					{
						break;
					}
					s = n;
				}
				for( size_t i = path.size(); i-- > 0; )
				{
					const uint32_t p = path[i];
					sideState[p] = verdict;
					if( verdict == 2 )
					{
						const uint32_t n = sideNext[p];
						if( n == NONE )
						{
							sideDistance[p] = sideLength[p];
						}
						else
						{
							sideEnd[p] = sideEnd[n];
							sideDistance[p] = sideLength[p] + sideDistance[n];
						}
					}
				}
			}
			// a side that never ends (a loop within SameRay's tolerance) makes its wedge fixed; then again
			for( uint32_t s = 0; s < numSides; ++s )
			{
				if( sideState[s] == 3 && passWedge[lines[s / 2].wedge] )
				{
					passWedge[lines[s / 2].wedge] = false;
					++count.demoted;
					again = true;
				}
			}
		}

		// a wedge with lines that run from brush corner to brush corner (a brush edge, where the uncut
		// faces agree, or a line between two brush corners) uses only those: on a face that is not a
		// triangle the uncut shading is not linear, and a cut line's lerp is not it, a brush edge's is
		count.brushEdgeOnly = 0;
		for( uint32_t w = 0; w < numWedges; ++w )
		{
			if( !passWedge[w] || lineStart[w + 1] - lineStart[w] < 2 )
			{
				continue;
			}
			bool anyBrushEdge = false;
			for( uint32_t l = lineStart[w]; l < lineStart[w + 1]; ++l )
			{
				lineUsed[l] = AtBrushCorner( sideEnd[l * 2] ) && AtBrushCorner( sideEnd[l * 2 + 1] ) ? 1 : 0;
				anyBrushEdge = anyBrushEdge || lineUsed[l] != 0;
			}
			for( uint32_t l = lineStart[w]; l < lineStart[w + 1] && !anyBrushEdge; ++l )
			{
				lineUsed[l] = 1;
			}
			count.brushEdgeOnly += anyBrushEdge ? 1 : 0;
		}

		if( count.brushEdgeOnly == 0 )
		{
			break;
		}
	}
	for( uint32_t w = 0; w < numWedges; ++w )
	{
		if( passWedge[w] )
		{
			count.lines += lineStart[w + 1] - lineStart[w];
		}
	}

	// Jacobi sweeps over the pass-through wedges, until no written bit changes
	std::vector<double> next( value );
	std::vector<float> written( size_t( numWedges ) * 3 ), previous;
	std::vector<uint32_t> sameEnd( numWedges, NONE );	// every end the same: that end, whose bits it writes
	for( uint32_t w = 0; w < numWedges; ++w )
	{
		for( unsigned axis = 0; axis < 3; ++axis )
		{
			written[size_t( w ) * 3 + axis] = out[members[start[w]]].n[axis];
		}
	}
	for( count.sweeps = 0; count.sweeps < CUT_MAX_SWEEPS && count.lines > 0; )
	{
		++count.sweeps;
		for( uint32_t w = 0; w < numWedges; ++w )
		{
			if( !passWedge[w] )
			{
				continue;
			}
			uint32_t firstLine = lineStart[w];
			while( !lineUsed[firstLine] )
			{
				++firstLine;
			}
			const double* firstEnd = &value[size_t( sideEnd[firstLine * 2] ) * 3];
			bool allSame = true;
			double sum[3] = { 0.0, 0.0, 0.0 };
			double numLines = 0.0;
			for( uint32_t l = firstLine; l < lineStart[w + 1]; ++l )
			{
				if( !lineUsed[l] )
				{
					continue;
				}
				numLines += 1.0;
				const double* a = &value[size_t( sideEnd[l * 2] ) * 3];
				const double* b = &value[size_t( sideEnd[l * 2 + 1] ) * 3];
				const double d0 = sideDistance[l * 2], d1 = sideDistance[l * 2 + 1];
				allSame = allSame && SameBits3( a, firstEnd ) && SameBits3( b, firstEnd );
				for( unsigned axis = 0; axis < 3; ++axis )
				{
					sum[axis] += ( d1 * a[axis] + d0 * b[axis] ) / ( d0 + d1 );
				}
			}
			for( unsigned axis = 0; axis < 3; ++axis )
			{
				next[size_t( w ) * 3 + axis] = allSame ? firstEnd[axis] : sum[axis] / numLines;
			}
			sameEnd[w] = allSame ? sideEnd[firstLine * 2] : NONE;
		}
		value.swap( next );
		previous = written;
		count.unconverged = 0;
		for( uint32_t w = 0; w < numWedges; ++w )
		{
			if( !passWedge[w] )
			{
				continue;
			}
			for( unsigned axis = 0; axis < 3; ++axis )
			{
				next[size_t( w ) * 3 + axis] = value[size_t( w ) * 3 + axis];
			}
			float* v = &written[size_t( w ) * 3];
			if( sameEnd[w] != NONE )
			{
				// not normalized again: a flat floor keeps the stock bits
				for( unsigned axis = 0; axis < 3; ++axis )
				{
					v[axis] = previous[size_t( sameEnd[w] ) * 3 + axis];
				}
			}
			else
			{
				Unit( &value[size_t( w ) * 3], v, out[members[start[w]]].n );
			}
			count.unconverged += ( Bits( v[0] ) != Bits( previous[size_t( w ) * 3] ) || Bits( v[1] ) != Bits( previous[size_t( w ) * 3 + 1] ) || Bits( v[2] ) != Bits( previous[size_t( w ) * 3 + 2] ) ) ? 1 : 0;
		}
		if( count.unconverged == 0 )
		{
			break;
		}
	}

	// a cut beside a vertex (a ray too short for SameRay): the value at the short ray's tip
	std::vector<uint32_t> copyFrom( numWedges, NONE );
	for( uint32_t w = 0; w < numWedges; ++w )
	{
		if( passWedge[w] )
		{
			continue;
		}
		uint32_t bestCorner = NONE;
		unsigned bestR = 0;
		Ray best = {};
		bool tie = false;
		for( uint32_t m = start[w]; m < start[w + 1]; ++m )
		{
			for( unsigned r = 0; r < 2; ++r )
			{
				const Ray ray = CornerRay( tris, members[m], r );
				if( ray.lengthSquared >= 16 || ray.lengthSquared == 0 )
				{
					continue;
				}
				if( bestCorner == NONE || RayLess( ray, best ) )
				{
					tie = false;
					best = ray;
					bestCorner = members[m];
					bestR = r;
				}
				else if( SameVector( ray, best ) && wedgeIds[TipCorner( members[m], r )] != wedgeIds[TipCorner( bestCorner, bestR )] )
				{
					tie = true;
				}
			}
		}
		if( bestCorner == NONE || AtBrushCorner( w ) )
		{
			continue;
		}
		const uint32_t from = wedgeIds[TipCorner( bestCorner, bestR )];
		if( !tie && ( passWedge[from] || AtBrushCorner( from ) ) )
		{
			copyFrom[w] = from;
			++count.shortCopies;
		}
		else
		{
			++count.shortStuck;	// e.g. two cut vertices under 4 steps apart: they keep Smooth's value (K1b)
		}
	}

	// write
	for( uint32_t w = 0; w < numWedges; ++w )
	{
		const uint32_t source = copyFrom[w] != NONE ? copyFrom[w] : w;
		if( !passWedge[w] && copyFrom[w] == NONE )
		{
			continue;
		}
		const float* v = &written[size_t( source ) * 3];
		const float* before = out[members[start[w]]].n;
		if( Bits( v[0] ) != Bits( before[0] ) || Bits( v[1] ) != Bits( before[1] ) || Bits( v[2] ) != Bits( before[2] ) )
		{
			++count.moved;
		}
		for( uint32_t m = start[w]; m < start[w + 1]; ++m )
		{
			Corner& c = out[members[m]];
			const float* input = tris[members[m] / 3].n;
			for( unsigned axis = 0; axis < 3; ++axis )
			{
				c.n[axis] = v[axis];
			}
			c.changed = Bits( v[0] ) != Bits( input[0] ) || Bits( v[1] ) != Bits( input[1] ) || Bits( v[2] ) != Bits( input[2] );
		}
	}
	if( stats != nullptr )
	{
		*stats = count;
	}
}
} // namespace dmapsmooth

#endif
