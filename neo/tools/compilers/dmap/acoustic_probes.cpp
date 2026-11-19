//puncho - SB1b (R-SB1b-7 to R-SB1b-15, R-SB1b-24, R-SB1b-26): dmap Stage A's probes. Every probe lies on the
// global lattice (or, for an area the lattice missed, inside one of its leaves), in its own area as the game's
// PointInArea will see it, at least SndProbeMinClearance( C ) from every triangle, outside every closed model and
// every brush static. Linked in the engine and in rbdmap: idLib, common, dmap's tree and the .proc writer's
// WritePlane only. Every order is by content or by dmap's own numbering (R-SB1b-20).
#include "precompiled.h"
#pragma hdrstop

#include "acoustic.h"

// R-SB1b-7: the worldspawn defaults and bounds (DR-55)
static const int	ACOUSTIC_SPACING_DEFAULT	= 160;
static const int	ACOUSTIC_CLEARANCE_DEFAULT	= 16;
static const int	ACOUSTIC_SPACING_MIN		= 32;
static const int	ACOUSTIC_SPACING_MAX		= 1024;
static const int	ACOUSTIC_CLEARANCE_MIN		= 8;

// R-SB1b-8: candidates per cell
static const int	ACOUSTIC_NUM_CANDIDATES		= 27;

// the cell mark, the bin entries, the cell table and the probe list are indexed by int; a map that needs more stops
// Stage A (review P4-3). The writer stops a .sndmesh past SND_FILE_MAX_SIZE before its builder does
static const int64	ACOUSTIC_MAX_LATTICE		= 0x40000000;

// review P4-7: a bound that is cleared, not finite or past 2^24 units (where a float no longer holds a unit) stops
// Stage A before floorf; inside it a cell is under 2^20 in magnitude, a box side under 2^21, a box under 2^63 cells
static const float	ACOUSTIC_MAX_COORD			= 16777216.0f;

// rule 6: two crossings of one ray on one side within this many units are one distance (a shared edge or vertex). The
// planes are solved in double from float corners, so two triangles meeting the ray at one point differ by about
// 1e-16 x |n| |a - q| / |n[axis]|: far below this up to 2^24 units and a triangle within 1e-6 rad of edge-on. Two
// real same-side crossings this close need two of the static's surfaces under 1/1024 unit apart
static const double	ACOUSTIC_CROSSING_TOLERANCE	= 1.0 / 1024.0;

// one node of the world tree as the game holds it after parsing the .proc (renderer areaNode_t)
struct acousticGameNode_t
{
	idPlane					plane;			// written by WritePlane, read back by idLexer::Parse1DMatrix
	int						children[2];	// > 0 a node, 0 solid, < 0 -1 - area
};

// an open leaf of the pruned world tree, depth first, child 0 first
struct acousticLeaf_t
{
	idBounds				bounds;
	int						area;
};

// a box of lattice cells, inclusive; maxs < mins on an axis: no cell
struct acousticCellBox_t
{
	int						mins[3];
	int						maxs[3];
};

// the items of one cell, items[first, first + count) in insertion order
struct acousticBin_t
{
	int						cell[3];
	int						first;
	int						count;
};

// R-SB1b-11: cell -> items; the hash only finds a bin, nothing walks it
struct acousticBins_t
{
	idHashIndex				hash;
	idList<acousticBin_t>	bins;
	idList<int>				items;
	int						mins[3];		// the cell box of every bin; maxs < mins when there is none
	int						maxs[3];
};

// a probe before the records: its position and area
struct acousticPlaced_t
{
	idVec3					position;
	int						area;
};

// review P4-4: a list about to grow doubles its capacity (acoustic.h's AcousticReserveOne), from 1,024
static const int	ACOUSTIC_PROBES_MIN_CAPACITY	= 1024;

struct acousticProbeWork_t
{
	const acousticMesh_t*		mesh;
	idList<acousticGameNode_t>	nodes;
	acousticBins_t				tris;		// triangles, bounds grown by C
	acousticBins_t				solids;		// brushes of static brush entities, their bounds only
	acousticBins_t				statics;	// rule 6: mesh.statics, by the bounds of their full lists
	idList<idBounds>			staticBounds;	// per mesh.statics entry
	idList<double>				crossings[2];	// rule 6 scratch, one ray: [0] entering, [1] leaving
	float						spacing[3];	// S, S, Sz
	idVec3						fractions[ACOUSTIC_NUM_CANDIDATES];
};

/*
==================
AcousticProbes_ReadSetting

R-SB1b-7: one worldspawn integer, clamped, one warning per clamp.
==================
*/
static int AcousticProbes_ReadSetting( const idDict& worldspawn, const char* key, int defaultValue, int minValue, int maxValue )
{
	const int value = worldspawn.GetInt( key, defaultValue );
	const int clamped = idMath::ClampInt( minValue, maxValue, value );
	if( clamped != value )
	{
		common->Warning( "acoustics: %s %i is outside [%i, %i], using %i", key, value, minValue, maxValue, clamped );
	}
	return clamped;
}

/*
==================
AcousticProbes_Fractions

R-SB1b-8: the 27 candidate fractions of a cell, ordered by the number of axes whose fraction is not 0.5, then by
( fz, fy, fx ) with 0.5 < 0.25 < 0.75. k = 0 is the centre.
==================
*/
static void AcousticProbes_Fractions( idVec3 fractions[ACOUSTIC_NUM_CANDIDATES] )
{
	static const float values[3] = { 0.5f, 0.25f, 0.75f };	// in rank order

	int keys[ACOUSTIC_NUM_CANDIDATES];
	int n = 0;
	for( int z = 0; z < 3; z++ )
	{
		for( int y = 0; y < 3; y++ )
		{
			for( int x = 0; x < 3; x++ )
			{
				fractions[n].Set( values[x], values[y], values[z] );
				keys[n] = ( ( x != 0 ) + ( y != 0 ) + ( z != 0 ) ) * 1000 + z * 100 + y * 10 + x;
				n++;
			}
		}
	}

	// insertion sort; the keys are unique, so the order is fixed
	for( int i = 1; i < ACOUSTIC_NUM_CANDIDATES; i++ )
	{
		const int key = keys[i];
		const idVec3 fraction = fractions[i];
		int j = i - 1;
		for( ; j >= 0 && keys[j] > key; j-- )
		{
			keys[j + 1] = keys[j];
			fractions[j + 1] = fractions[j];
		}
		keys[j + 1] = key;
		fractions[j + 1] = fraction;
	}
}

/*
==================
AcousticProbes_Cell

R-SB1b-8 and R-SB1b-9: the lattice cell of p, floorf on float, the same in the game.
==================
*/
static void AcousticProbes_Cell( const idVec3& p, const float spacing[3], int cell[3] )
{
	cell[0] = ( int )floorf( p.x / spacing[0] );
	cell[1] = ( int )floorf( p.y / spacing[1] );
	cell[2] = ( int )floorf( p.z / spacing[2] );
}

// false (box empty): the bounds are cleared, not finite or past ACOUSTIC_MAX_COORD, and floorf may not see them (P4-7)
static bool AcousticProbes_CellBox( const idBounds& bounds, const float spacing[3], acousticCellBox_t& box )
{
	for( int i = 0; i < 3; i++ )
	{
		box.mins[i] = 0;
		box.maxs[i] = -1;
	}
	for( int i = 0; i < 3; i++ )
	{
		// written so that a NaN fails it
		if( !( bounds[0][i] >= -ACOUSTIC_MAX_COORD && bounds[0][i] <= bounds[1][i] && bounds[1][i] <= ACOUSTIC_MAX_COORD ) )
		{
			return false;
		}
	}
	AcousticProbes_Cell( bounds[0], spacing, box.mins );
	AcousticProbes_Cell( bounds[1], spacing, box.maxs );
	return true;
}

static int64 AcousticProbes_CellBoxSize( const acousticCellBox_t& box )
{
	int64 size = 1;
	for( int i = 0; i < 3; i++ )
	{
		if( box.maxs[i] < box.mins[i] )
		{
			return 0;
		}
		size *= ( int64 )box.maxs[i] - box.mins[i] + 1;
	}
	return size;
}

/*
==================
AcousticProbes_GameLookup

R-SB1b-9, the game's arithmetic (SB4's R-SB4-11): the probe of area a in q's cell, or -1. One cell of q addresses
every area's box.
==================
*/
static int AcousticProbes_GameLookup( const sndMeshArea_t& area, const int32* cells, const idVec3& q, const float spacing[3] )
{
	int c[3];
	AcousticProbes_Cell( q, spacing, c );

	int l[3];
	for( int i = 0; i < 3; i++ )
	{
		l[i] = c[i] - area.cellMin[i];
		if( l[i] < 0 || l[i] >= area.dims[i] )
		{
			return -1;
		}
	}
	return cells[area.firstCell + l[0] + area.dims[0] * ( l[1] + area.dims[1] * l[2] )];
}

/*
==================
AcousticProbes_GamePlane

R-SB1b-26: the node plane as the game will hold it, written by the .proc writer and read back by
idLexer::Parse1DMatrix as RenderWorld_load.cpp's ParseNodes reads it (review P4-8). Joined with v0001.5: since
.proc format 005 (R0 S1) WriteNode_r writes a node plane with WritePlane, nine significant digits and no snap
to integers, where it used Write1DMatrix. The same writer is called here, so the plane held is the game's
whatever the writer does.
==================
*/
static bool AcousticProbes_GamePlane( const idPlane& dmapPlane, idPlane& gamePlane )
{
	// "( a b c d ) " with %.9g: well under 128 characters
	char text[256];
	idFile_Memory file( "acousticPlane", text, sizeof( text ) );
	WritePlane( &file, dmapPlane );

	idLexer lexer( LEXFL_NOSTRINGCONCAT | LEXFL_NODOLLARPRECOMPILE );
	lexer.LoadMemory( text, file.Length(), "acousticPlane" );
	return lexer.Parse1DMatrix( 4, gamePlane.ToFloatPtr() ) != 0;
}

/*
==================
AcousticProbes_GameNodes_r

The pruned world tree in the order output.cpp's WriteNode_r writes it (preorder, child 0 first, nodes only), so
nodes[i] is the game's areaNodes[i]. A world that is one leaf is one node ( 0 0 0 0 ) -1 -1, as the writer has it.
Mirrors WriteNode_r: a change there must be made here.
==================
*/
static bool AcousticProbes_GameNodes_r( const node_t* node, int numAreas, idList<acousticGameNode_t>& nodes )
{
	if( node->planenum == PLANENUM_LEAF )
	{
		acousticGameNode_t& one = nodes.Alloc();
		one.plane.Zero();
		one.children[0] = -1;
		one.children[1] = -1;
		return true;
	}

	if( node->nodeNumber != nodes.Num() )
	{
		common->Warning( "acoustics: tree node %i is visited as node %i", node->nodeNumber, nodes.Num() );
		return false;
	}

	acousticGameNode_t gameNode;
	for( int i = 0; i < 2; i++ )
	{
		const node_t* child = node->children[i];
		if( child->planenum == PLANENUM_LEAF )
		{
			if( child->area < -1 || child->area >= numAreas )
			{
				common->Warning( "acoustics: tree node %i: leaf area %i out of range (%i areas)", node->nodeNumber, child->area, numAreas );
				return false;
			}
			gameNode.children[i] = -1 - child->area;
		}
		else
		{
			gameNode.children[i] = child->nodeNumber;
		}
	}
	if( !AcousticProbes_GamePlane( dmapGlobals.mapPlanes[node->planenum], gameNode.plane ) )
	{
		common->Warning( "acoustics: tree node %i: its plane does not read back", node->nodeNumber );
		return false;
	}
	AcousticReserveOne( nodes, ACOUSTIC_PROBES_MIN_CAPACITY );
	nodes.Append( gameNode );

	for( int i = 0; i < 2; i++ )
	{
		if( gameNode.children[i] > 0 && !AcousticProbes_GameNodes_r( node->children[i], numAreas, nodes ) )
		{
			return false;
		}
	}
	return true;
}

/*
==================
AcousticProbes_PointInArea

R-SB1b-26: the game's descent, idRenderWorldLocal::PointInArea's expression and rule: d > 0 goes to child 0. The
area, or -1 in solid.
==================
*/
static int AcousticProbes_PointInArea( const acousticGameNode_t* nodes, const idVec3& point )
{
	const acousticGameNode_t* node = nodes;
	while( 1 )
	{
		const float d = point * node->plane.Normal() + node->plane[3];
		const int child = ( d > 0 ) ? node->children[0] : node->children[1];
		if( child == 0 )
		{
			return -1;
		}
		if( child < 0 )
		{
			return -1 - child;
		}
		node = nodes + child;
	}
}

static void AcousticProbes_Leaves_r( const node_t* node, idList<acousticLeaf_t>& leaves )
{
	if( node->planenum == PLANENUM_LEAF )
	{
		// review P4R-6: a leaf with cleared bounds has no volume and can hold no probe; any other bad bound fails later
		if( node->area >= 0 && !node->bounds.IsCleared() )
		{
			AcousticReserveOne( leaves, ACOUSTIC_PROBES_MIN_CAPACITY );
			acousticLeaf_t& leaf = leaves.Alloc();
			leaf.bounds = node->bounds;
			leaf.area = node->area;
		}
		return;
	}
	AcousticProbes_Leaves_r( node->children[0], leaves );
	AcousticProbes_Leaves_r( node->children[1], leaves );
}

/*
==================
AcousticBins

R-SB1b-11. Built from one cell box per item, in item order, each box walked z, y, x; a bin's items keep that order.
==================
*/
static int AcousticBins_Key( const int cell[3] )
{
	return ( int )( ( ( uint32 )cell[0] * 73856093u ) ^ ( ( uint32 )cell[1] * 19349663u ) ^ ( ( uint32 )cell[2] * 83492791u ) );
}

static int AcousticBins_Find( const acousticBins_t& bins, const int cell[3] )
{
	for( int i = bins.hash.First( AcousticBins_Key( cell ) ); i != -1; i = bins.hash.Next( i ) )
	{
		const acousticBin_t& bin = bins.bins[i];
		if( bin.cell[0] == cell[0] && bin.cell[1] == cell[1] && bin.cell[2] == cell[2] )
		{
			return i;
		}
	}
	return -1;
}

static bool AcousticBins_Build( acousticBins_t& bins, const idList<acousticCellBox_t>& boxes, const char* what )
{
	// a box is under 2^63 cells (ACOUSTIC_MAX_COORD), so the sum is tested before it can overflow
	int64 numEntries = 0;
	for( int i = 0; i < boxes.Num(); i++ )
	{
		numEntries += AcousticProbes_CellBoxSize( boxes[i] );
		if( numEntries > ACOUSTIC_MAX_LATTICE )
		{
			common->Warning( "acoustics: the %s bins need %lld cell entries or more, more than %lld", what, numEntries, ACOUSTIC_MAX_LATTICE );
			return false;
		}
	}

	// bins <= entries <= 2^30; the bin list and the hash chain double (review P4-4), the items are sized once
	bins.hash.Clear( idMath::CeilPowerOfTwo( idMath::ClampInt( 1024, 1 << 20, ( int )numEntries ) ), 1024 );
	for( int i = 0; i < 3; i++ )
	{
		bins.mins[i] = 0;
		bins.maxs[i] = -1;
	}

	// pass 1: the bins in first-touch order and their counts
	for( int b = 0; b < boxes.Num(); b++ )
	{
		const acousticCellBox_t& box = boxes[b];
		if( AcousticProbes_CellBoxSize( box ) == 0 )
		{
			continue;
		}
		int cell[3];
		for( cell[2] = box.mins[2]; cell[2] <= box.maxs[2]; cell[2]++ )
		{
			for( cell[1] = box.mins[1]; cell[1] <= box.maxs[1]; cell[1]++ )
			{
				for( cell[0] = box.mins[0]; cell[0] <= box.maxs[0]; cell[0]++ )
				{
					int index = AcousticBins_Find( bins, cell );
					if( index < 0 )
					{
						index = bins.bins.Num();
						AcousticReserveOne( bins.bins, ACOUSTIC_PROBES_MIN_CAPACITY );
						if( index >= bins.hash.GetIndexSize() )
						{
							bins.hash.ResizeIndex( Max( 2 * bins.hash.GetIndexSize(), 1024 ) );
						}
						acousticBin_t& bin = bins.bins.Alloc();
						bin.cell[0] = cell[0];
						bin.cell[1] = cell[1];
						bin.cell[2] = cell[2];
						bin.first = 0;
						bin.count = 0;
						bins.hash.Add( AcousticBins_Key( cell ), index );

						for( int i = 0; i < 3; i++ )
						{
							if( bins.maxs[i] < bins.mins[i] )
							{
								bins.mins[i] = bins.maxs[i] = cell[i];
							}
							bins.mins[i] = Min( bins.mins[i], cell[i] );
							bins.maxs[i] = Max( bins.maxs[i], cell[i] );
						}
					}
					bins.bins[index].count++;
				}
			}
		}
	}

	int first = 0;
	for( int i = 0; i < bins.bins.Num(); i++ )
	{
		bins.bins[i].first = first;
		first += bins.bins[i].count;
	}

	// pass 2: the items, in item order inside each bin
	idList<int> fill;
	fill.SetNum( bins.bins.Num() );
	for( int i = 0; i < fill.Num(); i++ )
	{
		fill[i] = 0;
	}
	bins.items.SetNum( ( int )numEntries );
	for( int b = 0; b < boxes.Num(); b++ )
	{
		const acousticCellBox_t& box = boxes[b];
		if( AcousticProbes_CellBoxSize( box ) == 0 )
		{
			continue;
		}
		int cell[3];
		for( cell[2] = box.mins[2]; cell[2] <= box.maxs[2]; cell[2]++ )
		{
			for( cell[1] = box.mins[1]; cell[1] <= box.maxs[1]; cell[1]++ )
			{
				for( cell[0] = box.mins[0]; cell[0] <= box.maxs[0]; cell[0]++ )
				{
					const int index = AcousticBins_Find( bins, cell );
					bins.items[bins.bins[index].first + fill[index]] = b;
					fill[index]++;
				}
			}
		}
	}
	return true;
}

/*
==================
AcousticProbes_TriDistanceSqr

The squared distance from p to the triangle abc, in double (review P4-2): the nearest of the three edges, or the
plane distance when p projects inside the triangle (edges included). The plane distance is never more than the true
distance, and nothing divides one small difference by another, so a thin triangle cannot push the result up (the
float region test gave 47.4 for a true 0.64). A triangle with no area is its three edges. The formula and the
order of every operation are those of the review's independent check (check_probes.py).
==================
*/
static double AcousticProbes_Dot( const double a[3], const double b[3] )
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void AcousticProbes_Cross( const double a[3], const double b[3], double out[3] )
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

static void AcousticProbes_Sub( const idVec3& a, const idVec3& b, double out[3] )
{
	out[0] = ( double )a.x - ( double )b.x;
	out[1] = ( double )a.y - ( double )b.y;
	out[2] = ( double )a.z - ( double )b.z;
}

static double AcousticProbes_SegmentDistanceSqr( const idVec3& p, const idVec3& a, const idVec3& b )
{
	double ab[3], ap[3];
	AcousticProbes_Sub( b, a, ab );
	AcousticProbes_Sub( p, a, ap );
	const double lengthSqr = AcousticProbes_Dot( ab, ab );
	double t = 0.0;
	if( lengthSqr > 0.0 )
	{
		t = AcousticProbes_Dot( ap, ab ) / lengthSqr;
		t = ( t < 0.0 ) ? 0.0 : ( ( t > 1.0 ) ? 1.0 : t );
	}
	double d[3];
	for( int i = 0; i < 3; i++ )
	{
		d[i] = ( double )p[i] - ( ( double )a[i] + ab[i] * t );
	}
	return AcousticProbes_Dot( d, d );
}

static double AcousticProbes_TriDistanceSqr( const idVec3& p, const idVec3& a, const idVec3& b, const idVec3& c )
{
	double best = Min( AcousticProbes_SegmentDistanceSqr( p, a, b ), Min( AcousticProbes_SegmentDistanceSqr( p, b, c ), AcousticProbes_SegmentDistanceSqr( p, c, a ) ) );

	double ab[3], ac[3], n[3];
	AcousticProbes_Sub( b, a, ab );
	AcousticProbes_Sub( c, a, ac );
	AcousticProbes_Cross( ab, ac, n );
	const double n2 = AcousticProbes_Dot( n, n );
	if( n2 > 0.0 )
	{
		double ap[3], bc[3], bp[3], ca[3], cp[3], e[3];
		AcousticProbes_Sub( p, a, ap );
		AcousticProbes_Sub( c, b, bc );
		AcousticProbes_Sub( p, b, bp );
		AcousticProbes_Sub( a, c, ca );
		AcousticProbes_Sub( p, c, cp );

		AcousticProbes_Cross( ab, ap, e );
		bool inside = AcousticProbes_Dot( e, n ) >= 0.0;
		AcousticProbes_Cross( bc, bp, e );
		inside = inside && AcousticProbes_Dot( e, n ) >= 0.0;
		AcousticProbes_Cross( ca, cp, e );
		inside = inside && AcousticProbes_Dot( e, n ) >= 0.0;
		if( inside )
		{
			const double h = AcousticProbes_Dot( ap, n );
			best = Min( best, h * h / n2 );
		}
	}
	return best;
}

/*
==================
AcousticProbes_AxisRayHit

The ray from q along +-axis against the triangle abc: true with t > 0, the distance along the ray, on a hit. The
inside test runs in double on the projection, edges included. Where its products are exact (q and the corners on
the grid, the common case) a ray through a shared edge hits both triangles and never slips through the seam;
elsewhere a ray within rounding of an edge can miss both, which costs that one ray its vote (review P4-8).
==================
*/
static bool AcousticProbes_AxisRayInside( const idVec3& q, int axis, const idVec3& a, const idVec3& b, const idVec3& c )
{
	const int i = ( axis + 1 ) % 3;
	const int j = ( axis + 2 ) % 3;

	const double e0 = ( ( double )b[i] - a[i] ) * ( ( double )q[j] - a[j] ) - ( ( double )b[j] - a[j] ) * ( ( double )q[i] - a[i] );
	const double e1 = ( ( double )c[i] - b[i] ) * ( ( double )q[j] - b[j] ) - ( ( double )c[j] - b[j] ) * ( ( double )q[i] - b[i] );
	const double e2 = ( ( double )a[i] - c[i] ) * ( ( double )q[j] - c[j] ) - ( ( double )a[j] - c[j] ) * ( ( double )q[i] - c[i] );
	// outside, or a projection with no area
	return ( ( e0 >= 0.0 && e1 >= 0.0 && e2 >= 0.0 ) || ( e0 <= 0.0 && e1 <= 0.0 && e2 <= 0.0 ) ) && ( e0 + e1 + e2 ) != 0.0;
}

static bool AcousticProbes_AxisRayHit( const idVec3& q, int axis, float sign, const idVec3& a, const idVec3& b, const idVec3& c, float& t )
{
	if( !AcousticProbes_AxisRayInside( q, axis, a, b, c ) )
	{
		return false;
	}

	// the plane: n . ( q + sign * t * axis - a ) = 0; a triangle seen edge-on is never hit
	const idVec3 n = ( c - a ).Cross( b - a );
	if( n[axis] == 0.0f )
	{
		return false;
	}
	t = ( n * ( a - q ) ) / ( sign * n[axis] );
	return t > 0.0f;
}

/*
==================
AcousticProbes_AxisRayCross

Rule 6: the ray from q along +-axis against the triangle abc of a static's full list, with rule 4's inside test and
the plane in double. 0: no crossing (missed, edge-on, or at or behind q); +1: the ray leaves through the front side,
from behind (front on ( v2 - v0 ) x ( v1 - v0 ), section 3); -1: it enters through the front. t: the distance.
==================
*/
static int AcousticProbes_AxisRayCross( const idVec3& q, int axis, double sign, const idVec3& a, const idVec3& b, const idVec3& c, double& t )
{
	if( !AcousticProbes_AxisRayInside( q, axis, a, b, c ) )
	{
		return 0;
	}
	double ab[3], ac[3], aq[3], n[3];
	AcousticProbes_Sub( b, a, ab );
	AcousticProbes_Sub( c, a, ac );
	AcousticProbes_Sub( a, q, aq );
	AcousticProbes_Cross( ac, ab, n );
	if( n[axis] == 0.0 )
	{
		return 0;
	}
	t = AcousticProbes_Dot( n, aq ) / ( sign * n[axis] );
	if( !( t > 0.0 ) )
	{
		return 0;
	}
	return ( sign * n[axis] > 0.0 ) ? 1 : -1;
}

// rule 6: t into a list kept ascending; a short list per ray, so insertion
static void AcousticProbes_InsertSorted( idList<double>& list, double t )
{
	AcousticReserveOne( list, ACOUSTIC_PROBES_MIN_CAPACITY );
	int i = list.Num();
	list.Alloc();
	for( ; i > 0 && list[i - 1] > t; i-- )
	{
		list[i] = list[i - 1];
	}
	list[i] = t;
}

// rule 6: crossings at one distance count once; one distance is a run of values each within the tolerance of the last
static int AcousticProbes_CountDistances( const idList<double>& list, double tolerance )
{
	int count = 0;
	for( int i = 0; i < list.Num(); i++ )
	{
		if( i == 0 || list[i] - list[i - 1] > tolerance )
		{
			count++;
		}
	}
	return count;
}

/*
==================
AcousticProbes_Passes

R-SB1b-10 rules 2 to 6 on a candidate q whose rule 1 descent gave area; clearanceSqr is C squared for placement,
SndProbeMinClearance( C ) squared for the fallback (R-SB1b-24). First failure rejects. work: only its rule 6 scratch
changes.
==================
*/
static bool AcousticProbes_Passes( acousticProbeWork_t& work, const idVec3& q, int area, double clearanceSqr )
{
	const acousticMesh_t& mesh = *work.mesh;
	const acousticGameNode_t* nodes = work.nodes.Ptr();

	// rule 2: the area margin
	for( int axis = 0; axis < 3; axis++ )
	{
		for( int s = 0; s < 2; s++ )
		{
			idVec3 p = q;
			p[axis] += ( s == 0 ) ? 2.0f : -2.0f;
			if( AcousticProbes_PointInArea( nodes, p ) != area )
			{
				return false;
			}
		}
	}

	int cell[3];
	AcousticProbes_Cell( q, work.spacing, cell );

	// rule 3: clearance against every triangle of q's bin, plugs included; the bin holds every triangle within C
	const int triBin = AcousticBins_Find( work.tris, cell );
	if( triBin >= 0 )
	{
		const acousticBin_t& bin = work.tris.bins[triBin];
		for( int i = 0; i < bin.count; i++ )
		{
			const acousticTri_t& tri = mesh.tris[work.tris.items[bin.first + i]];
			if( AcousticProbes_TriDistanceSqr( q, mesh.verts[tri.v[0]], mesh.verts[tri.v[1]], mesh.verts[tri.v[2]] ) < clearanceSqr )
			{
				return false;
			}
		}
	}

	// rule 4: inside a closed model; six axis rays, each walking its cells to its nearest hit. A back face (one-sided,
	// not a plug, dir . n > 0) hit within ACOUSTIC_CROSSING_TOLERANCE of the nearest hit wins over whatever else is
	// hit there (review P4S-1): where two bodies touch, the body's own back face and its neighbour's front face lie at
	// one distance but are computed from different triangles, and the vote must not depend on which entity the map
	// lists first. Limit: a sheet modelled as two coincident faces back to back gives a ray that meets it an inside vote
	int insideVotes = 0;
	int insideRays = 0;		// bit axis * 2 + s: that ray voted inside (rule 6)
	const float window = ( float )ACOUSTIC_CROSSING_TOLERANCE;
	for( int axis = 0; axis < 3; axis++ )
	{
		const int other0 = ( axis + 1 ) % 3;
		const int other1 = ( axis + 2 ) % 3;
		const bool crossesMesh = ( work.tris.bins.Num() > 0 &&
								   cell[other0] >= work.tris.mins[other0] && cell[other0] <= work.tris.maxs[other0] &&
								   cell[other1] >= work.tris.mins[other1] && cell[other1] <= work.tris.maxs[other1] );

		for( int s = 0; s < 2; s++ )
		{
			const float sign = ( s == 0 ) ? 1.0f : -1.0f;
			const int step = ( s == 0 ) ? 1 : -1;
			bool found = false;
			float nearestT = 0.0f;
			bool backFound = false;
			float backT = 0.0f;

			int walk[3] = { cell[0], cell[1], cell[2] };
			for( ; crossesMesh && walk[axis] >= work.tris.mins[axis] && walk[axis] <= work.tris.maxs[axis]; walk[axis] += step )
			{
				const int index = AcousticBins_Find( work.tris, walk );
				if( index >= 0 )
				{
					const acousticBin_t& bin = work.tris.bins[index];
					for( int i = 0; i < bin.count; i++ )
					{
						const acousticTri_t& tri = mesh.tris[work.tris.items[bin.first + i]];
						const idVec3& v0 = mesh.verts[tri.v[0]];
						float hitT;
						if( !AcousticProbes_AxisRayHit( q, axis, sign, v0, mesh.verts[tri.v[1]], mesh.verts[tri.v[2]], hitT ) )
						{
							continue;
						}
						if( !found || hitT < nearestT )
						{
							found = true;
							nearestT = hitT;
						}
						// a plug or a two-sided triangle votes outside (DR-37); a back face: the ray leaves through a
						// front face, it started behind the surface
						if( ( tri.flags & ( SND_TRI_PLUG | SND_TRI_TWO_SIDED ) ) == 0 && ( !backFound || hitT < backT ) )
						{
							const idVec3 n = ( mesh.verts[tri.v[2]] - v0 ).Cross( mesh.verts[tri.v[1]] - v0 );
							if( sign * n[axis] > 0.0f )
							{
								backFound = true;
								backT = hitT;
							}
						}
					}
				}
				// stop when the nearest hit and its window lie inside the cells walked
				if( found )
				{
					const int hitCell = ( int )floorf( ( q[axis] + sign * ( nearestT + window ) ) / work.spacing[axis] );
					if( ( step > 0 ) ? ( hitCell <= walk[axis] ) : ( hitCell >= walk[axis] ) )
					{
						break;
					}
				}
			}

			// no hit, or a nearest hit with no back face in its window, votes outside
			if( backFound && backT <= nearestT + window )
			{
				insideVotes++;
				insideRays |= 1 << ( axis * 2 + s );
			}
		}
	}
	if( insideVotes >= 4 )
	{
		return false;
	}

	// rule 5: inside a brush static; its planes face out, Distance <= 0 for every one is inside
	const int solidBin = AcousticBins_Find( work.solids, cell );
	if( solidBin >= 0 )
	{
		const acousticBin_t& bin = work.solids.bins[solidBin];
		for( int i = 0; i < bin.count; i++ )
		{
			const acousticBrush_t& brush = mesh.brushes[work.solids.items[bin.first + i]];
			int p = 0;
			for( ; p < brush.numPlanes; p++ )
			{
				if( mesh.brushPlanes[brush.firstPlane + p].Distance( q ) > 0.0f )
				{
					break;
				}
			}
			if( p == brush.numPlanes && brush.numPlanes > 0 )
			{
				return false;
			}
		}
	}

	// rule 6 (review P4-1, P4R-1, P4S-1): a closed static where it meets the world. The faces that close it there are
	// the ones the cull removed, so rule 4 sees the world through them. Only for a candidate rule 4 already gave an
	// inside vote: from inside a body in the mesh, one ray meets one of its own back faces first, or ties it with a
	// touching neighbour's front face, which rule 4's window gives to the back face; from a room inside a shell, rule 4
	// meets the room's own walls. Nothing else, not even the bin lookup, is paid otherwise. On each ray rule 4 voted
	// outside, a static adds a vote when, over its full list and the whole ray, its leaving distances outnumber its
	// entering ones and one leaving crossing is through a culled triangle. Rule 4's votes plus one static's added votes
	// at 4 or more reject. Limits: a ray exactly through a vertex whose triangles face both ways nets 0 where the true
	// count can be 1. Single-sided panels (open shells) among a closed static's far faces can give a ray a false
	// leaving crossing, and so a false vote. A closed shell with culled faces around furnished air rejects a candidate
	// there that also sees one back face. An open-topped single-sided box flush in a corner is rejected, as the same
	// box standing free already is by rule 4. Bodies that interpenetrate (a static pushed into another) are judged by
	// rule 4 alone. A body built of several patch or mesh primitives whose closing faces are primitives of their own
	// has no culled exit (a wholly culled primitive keeps no record), so it too is judged by rule 4 alone (P4T-1).
	// Rule 4's tie also gives a ray that passes exactly through a body's silhouette edge to the back face: an
	// open-air candidate with four such rays takes its cell's next candidate (P4T-2).
	const int staticBin = ( insideVotes > 0 ) ? AcousticBins_Find( work.statics, cell ) : -1;
	if( staticBin >= 0 )
	{
		const acousticBin_t& bin = work.statics.bins[staticBin];
		for( int i = 0; i < bin.count; i++ )
		{
			const int s = work.statics.items[bin.first + i];
			if( !work.staticBounds[s].ContainsPoint( q ) )
			{
				continue;
			}
			const acousticStatic_t& record = mesh.statics[s];
			int votes = insideVotes;
			for( int axis = 0; axis < 3; axis++ )
			{
				for( int side = 0; side < 2; side++ )
				{
					if( ( insideRays & ( 1 << ( axis * 2 + side ) ) ) != 0 )
					{
						continue;		// rule 4's inside vote is counted already
					}
					const double sign = ( side == 0 ) ? 1.0 : -1.0;
					bool culledExit = false;
					work.crossings[0].SetNum( 0 );
					work.crossings[1].SetNum( 0 );
					for( int t = record.firstTri; t < record.firstTri + record.numTris; t++ )
					{
						const acousticStaticTri_t& tri = mesh.staticTris[t];
						if( ( tri.flags & SND_TRI_TWO_SIDED ) != 0 )
						{
							continue;
						}
						double hitT;
						const int cross = AcousticProbes_AxisRayCross( q, axis, sign, tri.v[0], tri.v[1], tri.v[2], hitT );
						if( cross != 0 )
						{
							AcousticProbes_InsertSorted( work.crossings[( cross > 0 ) ? 1 : 0], hitT );
							if( cross > 0 && ( tri.flags & ACOUSTIC_STATIC_TRI_CULLED ) != 0 )
							{
								culledExit = true;
							}
						}
					}
					if( culledExit && AcousticProbes_CountDistances( work.crossings[1], ACOUSTIC_CROSSING_TOLERANCE ) >
							AcousticProbes_CountDistances( work.crossings[0], ACOUSTIC_CROSSING_TOLERANCE ) )
					{
						votes++;
					}
				}
			}
			if( votes >= 4 )
			{
				return false;
			}
		}
	}
	return true;
}

/*
==================
AcousticProbes_BrushBounds

The bounds of a brush given only by its planes (facing out): each plane's base winding clipped by the inside of
every other. A brush that bounds nothing gives cleared bounds and joins no bin.
==================
*/
static void AcousticProbes_BrushBounds( const acousticMesh_t& mesh, const acousticBrush_t& brush, idBounds& bounds )
{
	bounds.Clear();
	for( int i = 0; i < brush.numPlanes; i++ )
	{
		idWinding w( mesh.brushPlanes[brush.firstPlane + i] );
		bool kept = true;
		for( int j = 0; j < brush.numPlanes && kept; j++ )
		{
			if( j != i )
			{
				kept = w.ClipInPlace( -mesh.brushPlanes[brush.firstPlane + j], 0.0f );
			}
		}
		if( !kept )
		{
			continue;
		}
		for( int k = 0; k < w.GetNumPoints(); k++ )
		{
			bounds.AddPoint( w[k].ToVec3() );
		}
	}
	if( !bounds.IsCleared() )
	{
		bounds.ExpandSelf( 1.0f );	// the clip's float error must not drop a cell the brush touches
	}
}

/*
==================
AcousticProbes

R-SB1b-7 to R-SB1b-15 and R-SB1b-24: the settings, the game's planes, the bins, the placement over the map's cells,
the fallback, the records and the self-check. Fills settings' probeSpacing, probeSpacingZ, probeClearance and the
MSH_AREAS, MSH_CELLS and MSH_PROBES records. False: Stage A fails; a warning has named the cause.
Called by acoustic_write.cpp's AcousticFinishStageA, after its validation.
==================
*/
bool AcousticProbes( const acousticMesh_t& mesh, sndMeshSettings_t& settings, idList<sndMeshArea_t>& areas, idList<int32>& cells, idList<sndMeshProbe_t>& probes )
{
	const int numAreas = mesh.numAreas;

	// R-SB1b-7: settings; C after S and Sz, its upper bound depends on them
	const idDict& worldspawn = mesh.mapFile->GetEntity( 0 )->epairs;
	const int S = AcousticProbes_ReadSetting( worldspawn, "acoustic_probeSpacing", ACOUSTIC_SPACING_DEFAULT, ACOUSTIC_SPACING_MIN, ACOUSTIC_SPACING_MAX );
	const int Sz = AcousticProbes_ReadSetting( worldspawn, "acoustic_probeSpacingZ", ACOUSTIC_SPACING_DEFAULT, ACOUSTIC_SPACING_MIN, ACOUSTIC_SPACING_MAX );
	// review P4-6: with S or Sz below 64 the default is above the bound; an absent key takes the bound, unwarned
	const int clearanceMax = Min( S, Sz ) / 4;
	const int C = AcousticProbes_ReadSetting( worldspawn, "acoustic_probeClearance", Min( ACOUSTIC_CLEARANCE_DEFAULT, clearanceMax ), ACOUSTIC_CLEARANCE_MIN, clearanceMax );
	settings.probeSpacing = S;
	settings.probeSpacingZ = Sz;
	settings.probeClearance = C;

	acousticProbeWork_t work;
	work.mesh = &mesh;
	work.spacing[0] = ( float )S;
	work.spacing[1] = ( float )S;
	work.spacing[2] = ( float )Sz;
	AcousticProbes_Fractions( work.fractions );

	// R-SB1b-26: the game's planes, one table per compile
	if( !AcousticProbes_GameNodes_r( mesh.worldTree->headnode, numAreas, work.nodes ) )
	{
		return false;
	}
	const acousticGameNode_t* nodes = work.nodes.Ptr();

	// R-SB1b-11: triangles by their bounds grown by C, in triangle order; brushes by their bounds
	{
		idList<acousticCellBox_t> boxes;
		boxes.SetNum( mesh.tris.Num() );
		for( int t = 0; t < mesh.tris.Num(); t++ )
		{
			const acousticTri_t& tri = mesh.tris[t];
			idBounds bounds;
			bounds.Clear();
			bounds.AddPoint( mesh.verts[tri.v[0]] );
			bounds.AddPoint( mesh.verts[tri.v[1]] );
			bounds.AddPoint( mesh.verts[tri.v[2]] );
			bounds.ExpandSelf( ( float )C );
			if( !AcousticProbes_CellBox( bounds, work.spacing, boxes[t] ) )
			{
				common->Warning( "acoustics: triangle %i lies beyond %.0f units or is not finite", t, ACOUSTIC_MAX_COORD );
				return false;
			}
		}
		if( !AcousticBins_Build( work.tris, boxes, "triangle" ) )
		{
			return false;
		}

		boxes.SetNum( mesh.brushes.Num() );
		for( int b = 0; b < mesh.brushes.Num(); b++ )
		{
			idBounds bounds;
			AcousticProbes_BrushBounds( mesh, mesh.brushes[b], bounds );
			// cleared: the brush bounds nothing and its box stays empty
			if( !AcousticProbes_CellBox( bounds, work.spacing, boxes[b] ) && !bounds.IsCleared() )
			{
				common->Warning( "acoustics: brush %i of entity %i lies beyond %.0f units or is not finite", b, mesh.brushes[b].entity, ACOUSTIC_MAX_COORD );
				return false;
			}
		}
		if( !AcousticBins_Build( work.solids, boxes, "brush" ) )
		{
			return false;
		}

		// rule 6: mesh.statics by the bounds of their full lists, exact (q must lie in them)
		boxes.SetNum( mesh.statics.Num() );
		work.staticBounds.SetNum( mesh.statics.Num() );
		for( int s = 0; s < mesh.statics.Num(); s++ )
		{
			const acousticStatic_t& record = mesh.statics[s];
			idBounds& bounds = work.staticBounds[s];
			bounds.Clear();
			if( record.firstTri < 0 || record.numTris < 0 || record.firstTri + record.numTris > mesh.staticTris.Num() )
			{
				common->Warning( "acoustics: static %i (entity %i): triangles [%i, +%i) out of range", s, record.entity, record.firstTri, record.numTris );
				return false;
			}
			for( int t = record.firstTri; t < record.firstTri + record.numTris; t++ )
			{
				bounds.AddPoint( mesh.staticTris[t].v[0] );
				bounds.AddPoint( mesh.staticTris[t].v[1] );
				bounds.AddPoint( mesh.staticTris[t].v[2] );
			}
			// cleared (no triangle): its box stays empty and it joins no bin
			if( !AcousticProbes_CellBox( bounds, work.spacing, boxes[s] ) && !bounds.IsCleared() )
			{
				common->Warning( "acoustics: static entity %i lies beyond %.0f units or is not finite", record.entity, ACOUSTIC_MAX_COORD );
				return false;
			}
		}
		if( !AcousticBins_Build( work.statics, boxes, "static" ) )
		{
			return false;
		}
	}

	// R-SB1b-12: mark every cell an open leaf's bounds touch, in a bit array over the open leaves' cell box
	idList<acousticLeaf_t> leaves;
	AcousticProbes_Leaves_r( mesh.worldTree->headnode, leaves );
	for( int l = 0; l < leaves.Num(); l++ )
	{
		if( leaves[l].area >= numAreas )
		{
			common->Warning( "acoustics: open leaf %i: area %i out of range (%i areas)", l, leaves[l].area, numAreas );
			return false;
		}
		// review P4-7: both cell boxes below are taken from these bounds; no path to a bad one is known
		acousticCellBox_t leafBox;
		if( !AcousticProbes_CellBox( leaves[l].bounds, work.spacing, leafBox ) )
		{
			common->Warning( "acoustics: open leaf %i of area %i: its bounds are inside out, not finite or beyond %.0f units", l, leaves[l].area, ACOUSTIC_MAX_COORD );
			return false;
		}
	}

	acousticCellBox_t box;
	for( int i = 0; i < 3; i++ )
	{
		box.mins[i] = 0;
		box.maxs[i] = -1;
	}
	for( int l = 0; l < leaves.Num(); l++ )
	{
		acousticCellBox_t leafBox;
		AcousticProbes_CellBox( leaves[l].bounds, work.spacing, leafBox );		// true: tested above, here and below
		for( int i = 0; i < 3; i++ )
		{
			box.mins[i] = ( l == 0 ) ? leafBox.mins[i] : Min( box.mins[i], leafBox.mins[i] );
			box.maxs[i] = ( l == 0 ) ? leafBox.maxs[i] : Max( box.maxs[i], leafBox.maxs[i] );
		}
	}
	const int64 numBoxCells = AcousticProbes_CellBoxSize( box );
	if( numBoxCells > ACOUSTIC_MAX_LATTICE )
	{
		common->Warning( "acoustics: the open leaves span %lld lattice cells, more than %lld", numBoxCells, ACOUSTIC_MAX_LATTICE );
		return false;
	}
	const int dimX = box.maxs[0] - box.mins[0] + 1;
	const int dimY = box.maxs[1] - box.mins[1] + 1;

	idList<uint32> marks;
	marks.SetNum( ( int )( ( numBoxCells + 31 ) / 32 ) );
	for( int i = 0; i < marks.Num(); i++ )
	{
		marks[i] = 0;
	}
	for( int l = 0; l < leaves.Num(); l++ )
	{
		acousticCellBox_t leafBox;
		AcousticProbes_CellBox( leaves[l].bounds, work.spacing, leafBox );
		for( int z = leafBox.mins[2]; z <= leafBox.maxs[2]; z++ )
		{
			for( int y = leafBox.mins[1]; y <= leafBox.maxs[1]; y++ )
			{
				for( int x = leafBox.mins[0]; x <= leafBox.maxs[0]; x++ )
				{
					const int bit = ( x - box.mins[0] ) + dimX * ( ( y - box.mins[1] ) + dimY * ( z - box.mins[2] ) );
					marks[bit >> 5] |= 1u << ( bit & 31 );
				}
			}
		}
	}

	// R-SB1b-12: one pass, z then y then x; per cell at most one probe per area, the first candidate that passes
	const double clearanceSqr = ( double )( C * C );
	idList<acousticPlaced_t> placed;
	for( int z = box.mins[2]; z <= box.maxs[2]; z++ )
	{
		for( int y = box.mins[1]; y <= box.maxs[1]; y++ )
		{
			for( int x = box.mins[0]; x <= box.maxs[0]; x++ )
			{
				const int bit = ( x - box.mins[0] ) + dimX * ( ( y - box.mins[1] ) + dimY * ( z - box.mins[2] ) );
				if( ( marks[bit >> 5] & ( 1u << ( bit & 31 ) ) ) == 0 )
				{
					continue;
				}

				int held[ACOUSTIC_NUM_CANDIDATES];
				int numHeld = 0;
				for( int k = 0; k < ACOUSTIC_NUM_CANDIDATES; k++ )
				{
					const idVec3& f = work.fractions[k];
					const idVec3 q( ( ( float )x + f.x ) * work.spacing[0], ( ( float )y + f.y ) * work.spacing[1], ( ( float )z + f.z ) * work.spacing[2] );

					// rule 1 names the area
					const int area = AcousticProbes_PointInArea( nodes, q );
					if( area < 0 )
					{
						continue;
					}
					int h = 0;
					for( ; h < numHeld && held[h] != area; h++ )
					{
					}
					if( h < numHeld )
					{
						continue;		// this cell already holds a probe for that area
					}
					if( !AcousticProbes_Passes( work, q, area, clearanceSqr ) )
					{
						continue;
					}
					// up to 27 per marked cell: past 2^30 the count, or the list's doubling, could leave int
					if( placed.Num() >= ACOUSTIC_MAX_LATTICE )
					{
						common->Warning( "acoustics: more than %lld probes", ACOUSTIC_MAX_LATTICE );
						return false;
					}
					held[numHeld++] = area;
					AcousticReserveOne( placed, ACOUSTIC_PROBES_MIN_CAPACITY );
					acousticPlaced_t& probe = placed.Alloc();
					probe.position = q;
					probe.area = area;
				}
			}
		}
	}
	marks.Clear();

	// R-SB1b-13: the fallback, per area with no probe, over its leaves in leaf order, clearance C' (R-SB1b-24)
	idList<int> areaCount;
	areaCount.SetNum( numAreas );
	for( int a = 0; a < numAreas; a++ )
	{
		areaCount[a] = 0;
	}
	for( int p = 0; p < placed.Num(); p++ )
	{
		areaCount[placed[p].area]++;
	}

	idList<int> leafFirst;		// leaves of area a: leafOrder[leafFirst[a], leafFirst[a + 1])
	idList<int> leafOrder;
	leafFirst.SetNum( numAreas + 1 );
	leafOrder.SetNum( leaves.Num() );
	for( int a = 0; a <= numAreas; a++ )
	{
		leafFirst[a] = 0;
	}
	for( int l = 0; l < leaves.Num(); l++ )
	{
		leafFirst[leaves[l].area + 1]++;
	}
	for( int a = 0; a < numAreas; a++ )
	{
		leafFirst[a + 1] += leafFirst[a];
	}
	{
		idList<int> fill;
		fill.SetNum( numAreas );
		for( int a = 0; a < numAreas; a++ )
		{
			fill[a] = leafFirst[a];
		}
		for( int l = 0; l < leaves.Num(); l++ )
		{
			leafOrder[fill[leaves[l].area]++] = l;
		}
	}

	const int minClearance = SndProbeMinClearance( C );
	const double fallbackSqr = ( double )( minClearance * minClearance );
	idList<bool> fallback;
	fallback.SetNum( numAreas );
	for( int a = 0; a < numAreas; a++ )
	{
		fallback[a] = false;
		if( areaCount[a] > 0 )
		{
			continue;
		}

		bool found = false;
		idBounds areaBounds;
		areaBounds.Clear();
		for( int o = leafFirst[a]; o < leafFirst[a + 1] && !found; o++ )
		{
			const idBounds& bounds = leaves[leafOrder[o]].bounds;
			areaBounds.AddBounds( bounds );
			const idVec3 size = bounds[1] - bounds[0];
			// the centre, then the 27 fractions of R-SB1b-8 over the leaf bounds
			for( int k = -1; k < ACOUSTIC_NUM_CANDIDATES && !found; k++ )
			{
				idVec3 q;
				if( k < 0 )
				{
					q = bounds.GetCenter();
				}
				else
				{
					const idVec3& f = work.fractions[k];
					q.Set( bounds[0].x + f.x * size.x, bounds[0].y + f.y * size.y, bounds[0].z + f.z * size.z );
				}
				if( AcousticProbes_PointInArea( nodes, q ) != a || !AcousticProbes_Passes( work, q, a, fallbackSqr ) )
				{
					continue;
				}
				if( placed.Num() >= ACOUSTIC_MAX_LATTICE )
				{
					common->Warning( "acoustics: more than %lld probes", ACOUSTIC_MAX_LATTICE );
					return false;
				}
				AcousticReserveOne( placed, ACOUSTIC_PROBES_MIN_CAPACITY );
				acousticPlaced_t& probe = placed.Alloc();
				probe.position = q;
				probe.area = a;
				areaCount[a] = 1;
				fallback[a] = true;
				found = true;
			}
		}
		if( !found )
		{
			if( areaBounds.IsCleared() )
			{
				common->Warning( "acoustics: area %i has no open leaf and no probe", a );
			}
			else
			{
				common->Warning( "acoustics: area %i has no probe (bounds %.1f %.1f %.1f to %.1f %.1f %.1f)", a,
								 areaBounds[0].x, areaBounds[0].y, areaBounds[0].z, areaBounds[1].x, areaBounds[1].y, areaBounds[1].z );
			}
		}
	}

	// R-SB1b-14: records, probes grouped by area ascending, inside an area in walk order (a stable count sort)
	areas.SetNum( numAreas );
	probes.SetNum( placed.Num() );
	{
		idList<int> fill;
		fill.SetNum( numAreas );
		int running = 0;
		for( int a = 0; a < numAreas; a++ )
		{
			areas[a].firstProbe = running;
			areas[a].probeCount = areaCount[a];
			fill[a] = running;
			running += areaCount[a];
		}
		const float radius = ( float )Min( S, Sz );
		for( int p = 0; p < placed.Num(); p++ )
		{
			sndMeshProbe_t& probe = probes[fill[placed[p].area]++];
			probe.position[0] = placed[p].position.x;
			probe.position[1] = placed[p].position.y;
			probe.position[2] = placed[p].position.z;
			probe.radius = radius;
			probe.area = placed[p].area;
		}
	}

	// each area's cell box is the tightest box of its probes' cells; none: dims 0, 0, 0 (SEAMS D-39)
	int64 numCells = 0;
	for( int a = 0; a < numAreas; a++ )
	{
		sndMeshArea_t& area = areas[a];
		for( int i = 0; i < 3; i++ )
		{
			area.cellMin[i] = 0;
			area.dims[i] = 0;
		}
		area.firstCell = ( int )numCells;

		int cellMax[3] = { 0, 0, 0 };
		for( int p = area.firstProbe; p < area.firstProbe + area.probeCount; p++ )
		{
			int c[3];
			AcousticProbes_Cell( idVec3( probes[p].position[0], probes[p].position[1], probes[p].position[2] ), work.spacing, c );
			for( int i = 0; i < 3; i++ )
			{
				area.cellMin[i] = ( p == area.firstProbe ) ? c[i] : Min( area.cellMin[i], c[i] );
				cellMax[i] = ( p == area.firstProbe ) ? c[i] : Max( cellMax[i], c[i] );
			}
		}
		if( area.probeCount > 0 )
		{
			for( int i = 0; i < 3; i++ )
			{
				area.dims[i] = cellMax[i] - area.cellMin[i] + 1;
			}
			numCells += ( int64 )area.dims[0] * area.dims[1] * area.dims[2];
			if( numCells > ACOUSTIC_MAX_LATTICE )
			{
				common->Warning( "acoustics: the areas' cell boxes need more than %lld cells (area %i)", ACOUSTIC_MAX_LATTICE, a );
				return false;
			}
		}
	}

	cells.SetNum( ( int )numCells );
	for( int c = 0; c < cells.Num(); c++ )
	{
		cells[c] = -1;
	}
	for( int a = 0; a < numAreas; a++ )
	{
		const sndMeshArea_t& area = areas[a];
		for( int p = area.firstProbe; p < area.firstProbe + area.probeCount; p++ )
		{
			int c[3];
			AcousticProbes_Cell( idVec3( probes[p].position[0], probes[p].position[1], probes[p].position[2] ), work.spacing, c );
			const int l[3] = { c[0] - area.cellMin[0], c[1] - area.cellMin[1], c[2] - area.cellMin[2] };
			cells[area.firstCell + l[0] + area.dims[0] * ( l[1] + area.dims[1] * l[2] )] = p;
		}
	}

	// R-SB1b-15: every probe's own cell, read by the game's arithmetic in its own area, returns its own index
	for( int a = 0; a < numAreas; a++ )
	{
		const sndMeshArea_t& area = areas[a];
		for( int p = area.firstProbe; p < area.firstProbe + area.probeCount; p++ )
		{
			const idVec3 q( probes[p].position[0], probes[p].position[1], probes[p].position[2] );
			if( AcousticProbes_GameLookup( area, cells.Ptr(), q, work.spacing ) != p )
			{
				common->Warning( "acoustics: probe %i of area %i at %.3f %.3f %.3f fails its own cell lookup", p, a, q.x, q.y, q.z );
				return false;
			}
		}
	}

	if( dmap_acousticVerbose.GetBool() )
	{
		for( int a = 0; a < numAreas; a++ )
		{
			common->Printf( "acoustics: area %i: %i probes, fallback %i, %i cells\n", a, areas[a].probeCount, fallback[a] ? 1 : 0,
							areas[a].dims[0] * areas[a].dims[1] * areas[a].dims[2] );
		}
	}
	return true;
}
