/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.
Copyright (C) 2026 Phillip Melnichenko

This file is part of the Doom 3 GPL Source Code (?Doom 3 Source Code?).

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#include "precompiled.h"
#pragma hdrstop

#include "dress.h"

/*

  World-surface chunking ( dmap -chunkGrid, renderer upgrade package F6 )

  The renderer gives every drawn surface the list of lights that touch it, and a surface
  whose list is long takes the slower per-pixel path. A wall of 4,096 units in a lit
  street touches hundreds of lights, so this pass cuts the world's surfaces on a grid:
  every world group whose material receives light is cut on the planes x, y, z = k * grid
  ( multiples of the grid on the map's axes from the origin ), and the pieces that fall in
  one cell of the grid become a group of their own whose merge group is unique to ( the
  parent's merge group, the cell ). The output stage merges the groups of an area that
  share material and merge group into one surface, so each cell's piece of a large
  surface is written as its own .proc surface, with its own bounds: at run time it gets
  its own short light list and is culled on its own. Nothing about the grid is stored;
  at run time a chunk is an ordinary surface.

  It runs in DressAreasPostOptimize, after the optimizer, which would merge the pieces
  again, and before FixGlobalTjunctions, which repairs the T-junctions the cut leaves
  where a neighbouring group was not cut at the same point.

  Off unless asked for, and experimental: by the dmap option -chunkGrid, else by the
  worldspawn key chunkGrid ( the same values ). Measured on F6's fixtures at 256
  ( plans/renderer-upgrade/f6-2026-11-20 ), the forward pass moved by -2 to +6 % ( 161 to
  157, 258 to 266, 649 to 687 microseconds on the three fixtures ), while the frontend's
  time and the shadow draws grew with the surface count: no measure shows a gain, so a
  map opts in.

  Left alone:
  - discrete materials ( guis, mirrors, deforms, decal macros ): FixGlobalTjunctions skips
    their groups, so a cut through one would never be repaired. This also leaves the
    func_decal fragments whole, whose merge group names their projector for
    DressDecalsPostTjunction: dress_decal.cpp refuses a material that is not discrete
  - materials that receive no light: no light list to shorten, only draws to add
  - noFragment materials: their author asked dmap to break them only at area boundaries

  The cut interpolates every vertex attribute itself ( position, texture coordinates,
  normal, colour, colour2, and the tangent fields, though dmap writes none and the .proc
  keeps only position, st, normal and colour ), because the T-junction fix
  interpolates only texture coordinates and normals at the vertices it adds. It does so in
  float ( chunkVert_t ) and packs a new corner into an idDrawVert once, at the end, so a
  corner cut from corners that were cut before carries no rounding of theirs; a corner of
  the source tri keeps its bytes. An edge is cut from its endpoints in a fixed order ( the
  lower one on the cut axis first ) and the planes are taken in a fixed order ( x, y, z,
  each ascending ), so two tris whose shared edge has the same endpoints bit for bit ( the
  tris of one group ) get the same corner bit for bit. Across groups, where two coincident
  edges may differ by a float step, the corners differ by as little, and the T-junction
  fix's 1/32 weld makes them one. A vertex within CHUNK_ON_EPSILON of a plane counts as on
  it, so no piece is thinner than the T-junction fix's snap. Two or more corners in a row
  within that margin go to both sides too, so the two pieces overlap within 1/16 of the
  plane: a sliver of most faces, but a wide band of a face nearly parallel to the plane
  ( review F1-4: 15 % of such a tri ). Invisible on an opaque surface, a double blend on a
  lit translucent one; counted ( review O1-4; a rule by the neighbours would cut a shared
  edge differently in its two tris ).

  The seams against the groups the pass does not cut: FixGlobalTjunctions repairs them for
  every group that is not discrete. A discrete group it skips, so where a grid plane meets
  the edge a cut group shares with a discrete one ( the wall around a gui panel ), the cut
  group gets a corner the discrete group's edge lacks. ChunkRepairDiscreteEdges splits that
  edge at that corner ( the tri in two, every attribute interpolated, the group still one
  surface with its own merge group ), except for a material with a deform, whose quads a
  deform reads: those corners are counted and left.

  The dress contract ( dress.cpp ): the walk takes each eligible group's tris and bins
  them by cell; the new groups are put into the areas only after the walk. No randomness.

*/

// a vertex this close to a grid plane counts as on it: twice the T-junction fix's snap
// ( 1 / SNAP_FRACTIONS, tritjunction.cpp ), so a piece is never thinner than one snap step
static const float	CHUNK_ON_EPSILON = 1.0f / 16.0f;

// a tri cut to one cell has at most 3 + 2 * 3 corners ( the two planes of a slab on each
// axis ); room to spare for the points within CHUNK_ON_EPSILON of a plane, which go to both
// sides. ChunkSplitPoly refuses a split that could pass it
static const int	CHUNK_MAX_POINTS = 16;

// a corner of a piece while it is cut: every attribute of idDrawVert in float
typedef struct
{
	idVec3			xyz;
	idVec2			st;
	idVec3			normal;
	idVec3			tangent;
	idVec3			bitangent;
	float			color[4];		// 0 to 255
	float			color2[4];
	int				corner;			// 0 to 2: this corner of the source tri, whose bytes it keeps; -1: made by a cut
} chunkVert_t;

typedef struct
{
	int				numPoints;
	chunkVert_t		points[CHUNK_MAX_POINTS];
} chunkPoly_t;

// a chunk's merge group: the address of one element of chunkMergeGroups. One element
// per ( parent merge group, cell ), so the groups of one material in one cell of an area
// still merge into one surface, and different cells never do. Every parent's merge group
// is NULL today ( only discrete materials set one before this pass, SC1 ); the key keeps it
// so a later pass that gives lit groups a merge group of their own keeps its separation
typedef struct
{
	const void*		parentMergeGroup;
	int				cell[3];
} chunkMergeGroup_t;

// the merge groups of the map being compiled. Filled by DressChunkWorldSurfaces after its
// walk, in one allocation, and not changed until the next compile clears it there: the
// addresses of its elements are the merge groups of the chunk groups, so they live through
// output like the decal projectors ( dress_decal.cpp ) and every other merge group target
static idList<chunkMergeGroup_t>	chunkMergeGroups;

// splits ChunkSplitPoly refused for want of room; reported, expected 0
static int							numSplitsRefused;

// splits with two or more points in a row within the margin: their pieces overlap near the
// plane ( review O1-4, F1-4 ); reported
static int							numSplitsOverlapping;

// the T-junction fix's snap ( SNAP_FRACTIONS, tritjunction.cpp ): a corner ChunkRepairDiscreteEdges
// adds to a discrete edge is snapped as GetHashVert snaps its twin in the cut group ( it snaps no
// vertex of a discrete group ). GetHashVert may then weld the twin to a hash vertex one snap step
// away, so the two can still differ by up to 1/32 on an axis: a notch far below a pixel
static const float	CHUNK_TJUNCTION_SNAP = 32.0f;

// a corner of the cut is on a discrete edge when it lies this close to the edge's line ( review O1-2 )
static const float	CHUNK_EDGE_EPSILON = 1.0f / 64.0f;

// the repair splits one source tri at most this many times; a split past it is not made and is counted
// ( review O1-1: a cycle between two planes must end even if a rule above has a hole; review F1-2: a cap
// of depth stopped an edge longer than 32 cells )
static const int	CHUNK_REPAIR_MAX_SPLITS = 4096;
static int			numRepairsCapped;

// discrete edge splits not made because a half would turn over
static int			numRepairsTurned;

// the corners the cut made, found again by position: 1/4-unit cells, a corner within
// CHUNK_ON_EPSILON of a candidate in this or a neighbouring cell matches
typedef struct
{
	idList<idVec3>		points;
	idHashIndex			hash;
} chunkCutPoints_t;

// one cell's tris of one parent group, collected by the walk
typedef struct
{
	uArea_t*			area;
	optimizeGroup_t*	parent;
	int					cell[3];
	mapTri_t*			tris;
	int					mergeGroup;		// into chunkMergeGroups, set after the walk
	idVec2				stBase;			// whole tiles near the st of its first tri: every tri of the bin is packed against it
} chunkBin_t;

/*
====================
ChunkPolyFromTri
====================
*/
static void ChunkPolyFromTri( const idDrawVert source[3], chunkPoly_t& poly )
{
	poly.numPoints = 3;
	for( int k = 0; k < 3; k++ )
	{
		const idDrawVert& in = source[k];
		chunkVert_t& out = poly.points[k];
		out.xyz = in.xyz;
		out.st = in.GetTexCoord();
		out.normal = in.GetNormal();
		out.tangent = in.GetTangent();
		out.bitangent = in.GetBiTangent();
		for( int c = 0; c < 4; c++ )
		{
			out.color[c] = in.color[c];
			out.color2[c] = in.color2[c];
		}
		out.corner = k;
	}
}

/*
====================
ChunkHalfRound

The half float nearer to f. F32toF16 truncates: a cut corner's st, blended from exact half
floats, would sit off the source's mapping by up to a whole step, 1/64 of a tile at |st| 16
to 32; rounded, by half of one ( review O1-5 ).
====================
*/
static halfFloat_t ChunkHalfRound( float f )
{
	const halfFloat_t h = F32toF16( f );
	const halfFloat_t h1 = ( halfFloat_t )( h + 1 );		// the next step away from zero
	if( ( h & 0x7C00 ) == 0x7C00 || ( h1 & 0x7C00 ) == 0x7C00 )
	{
		return h;
	}
	return ( idMath::Fabs( F16toF32( h1 ) - f ) < idMath::Fabs( F16toF32( h ) - f ) ) ? h1 : h;
}

/*
====================
ChunkPackVert

A corner of the source tri is its bytes; a corner a cut made is packed from its floats, from
a cleared vertex, so it depends on nothing but them: the neighbour that cut the same edge
packs the same bytes. The normal, tangent and bitangent are normalized as LerpAll does. The st
are moved by stShift, whole tiles from the source tri's st base to the chunk's ( O1-5b ).
====================
*/
static void ChunkPackVert( const chunkVert_t& in, const idDrawVert source[3], const idVec2& stShift, idDrawVert& out )
{
	if( in.corner >= 0 )
	{
		out = source[in.corner];
		if( stShift.x != 0.0f || stShift.y != 0.0f )
		{
			// whole tiles, to a value near the chunk's base: half a step there
			const idVec2 st = out.GetTexCoord() + stShift;
			out.SetTexCoordNative( ChunkHalfRound( st.x ), ChunkHalfRound( st.y ) );
		}
		return;
	}

	memset( &out, 0, sizeof( out ) );
	out.xyz = in.xyz;
	out.SetTexCoordNative( ChunkHalfRound( in.st.x + stShift.x ), ChunkHalfRound( in.st.y + stShift.y ) );
	idVec3 normal = in.normal;
	idVec3 tangent = in.tangent;
	idVec3 bitangent = in.bitangent;
	normal.Normalize();
	tangent.Normalize();
	bitangent.Normalize();
	out.SetNormal( normal );
	out.SetTangent( tangent );
	out.SetBiTangent( bitangent );
	for( int c = 0; c < 4; c++ )
	{
		out.color[c] = ( byte )idMath::ClampInt( 0, 255, idMath::Ftoi( in.color[c] + 0.5f ) );
		out.color2[c] = ( byte )idMath::ClampInt( 0, 255, idMath::Ftoi( in.color2[c] + 0.5f ) );
	}
}

/*
====================
ChunkSplitPoly

Splits in by the plane xyz[axis] = dist. A point within CHUNK_ON_EPSILON of the plane
goes to both sides unmoved; an edge that crosses the plane is cut from its lower
endpoint on the axis, so the tri on the other side of the edge gets the same vertex.
Returns false when in lies on one side only ( back and front are not written ).
====================
*/
static bool ChunkSplitPoly( const chunkPoly_t& in, int axis, float dist, chunkPoly_t& back, chunkPoly_t& front )
{
	int		sides[CHUNK_MAX_POINTS];
	bool	anyBack = false;
	bool	anyFront = false;

	for( int i = 0; i < in.numPoints; i++ )
	{
		const float d = in.points[i].xyz[axis] - dist;
		if( d > CHUNK_ON_EPSILON )
		{
			sides[i] = SIDE_FRONT;
			anyFront = true;
		}
		else if( d < -CHUNK_ON_EPSILON )
		{
			sides[i] = SIDE_BACK;
			anyBack = true;
		}
		else
		{
			sides[i] = SIDE_ON;
		}
	}

	if( !anyBack || !anyFront )
	{
		return false;
	}

	// two points in a row within the margin, not both on the plane: the pieces will overlap there
	for( int i = 0; i < in.numPoints; i++ )
	{
		const int j = ( i + 1 ) % in.numPoints;
		if( sides[i] == SIDE_ON && sides[j] == SIDE_ON && ( in.points[i].xyz[axis] != dist || in.points[j].xyz[axis] != dist ) )
		{
			numSplitsOverlapping++;
			break;
		}
	}

	// a side gets at most every point and two cut points; never reached ( see CHUNK_MAX_POINTS ),
	// and the piece left uncut is still a correct, larger chunk
	if( in.numPoints + 2 > CHUNK_MAX_POINTS )
	{
		numSplitsRefused++;
		return false;
	}

	back.numPoints = 0;
	front.numPoints = 0;

	for( int i = 0; i < in.numPoints; i++ )
	{
		const int j = ( i + 1 ) % in.numPoints;
		const chunkVert_t& p1 = in.points[i];
		const chunkVert_t& p2 = in.points[j];

		if( sides[i] != SIDE_FRONT )
		{
			back.points[back.numPoints++] = p1;
		}
		if( sides[i] != SIDE_BACK )
		{
			front.points[front.numPoints++] = p1;
		}

		if( sides[i] == SIDE_ON || sides[j] == SIDE_ON || sides[i] == sides[j] )
		{
			continue;
		}

		// the lower endpoint first, whatever this tri's winding: the neighbour that shares
		// the edge computes the same vertex
		const bool p1Low = p1.xyz[axis] < p2.xyz[axis];
		const chunkVert_t& lo = p1Low ? p1 : p2;
		const chunkVert_t& hi = p1Low ? p2 : p1;
		const float f = ( dist - lo.xyz[axis] ) / ( hi.xyz[axis] - lo.xyz[axis] );

		chunkVert_t mid;
		mid.xyz = ::Lerp( lo.xyz, hi.xyz, f );
		mid.xyz[axis] = dist;	// exactly on the plane, which is on the T-junction fix's snap grid
		mid.st = ::Lerp( lo.st, hi.st, f );
		mid.normal = ::Lerp( lo.normal, hi.normal, f );
		mid.tangent = ::Lerp( lo.tangent, hi.tangent, f );
		mid.bitangent = ::Lerp( lo.bitangent, hi.bitangent, f );
		for( int c = 0; c < 4; c++ )
		{
			mid.color[c] = lo.color[c] + f * ( hi.color[c] - lo.color[c] );
			mid.color2[c] = lo.color2[c] + f * ( hi.color2[c] - lo.color2[c] );
		}
		mid.corner = -1;

		back.points[back.numPoints++] = mid;
		front.points[front.numPoints++] = mid;
	}

	assert( back.numPoints <= CHUNK_MAX_POINTS && front.numPoints <= CHUNK_MAX_POINTS );
	return true;
}

/*
====================
ChunkCutAxis

Cuts every poly of in on the planes xyz[axis] = k * grid that cross it, ascending, and
appends the pieces to out.
====================
*/
static void ChunkCutAxis( const idList<chunkPoly_t>& in, int axis, float grid, idList<chunkPoly_t>& out )
{
	for( int i = 0; i < in.Num(); i++ )
	{
		float lo = in[i].points[0].xyz[axis];
		float hi = lo;
		for( int p = 1; p < in[i].numPoints; p++ )
		{
			lo = Min( lo, in[i].points[p].xyz[axis] );
			hi = Max( hi, in[i].points[p].xyz[axis] );
		}

		chunkPoly_t rest = in[i];
		const int first = ( int )idMath::Floor( lo / grid ) + 1;
		const int last = ( int )idMath::Ceil( hi / grid ) - 1;
		for( int k = first; k <= last; k++ )
		{
			chunkPoly_t back, front;
			if( ChunkSplitPoly( rest, axis, k * grid, back, front ) )
			{
				out.Append( back );
				rest = front;
			}
		}
		out.Append( rest );
	}
}

/*
====================
ChunkCell

The cell of a piece: the cell the middle of its bounds lies in. A piece lies between two
planes of each axis, less CHUNK_ON_EPSILON, so every corner lies in that cell or within
CHUNK_ON_EPSILON of its sides ( the mean of the corners would let a thin piece reach three
margins past, review QR1-F2 ).
====================
*/
static void ChunkCell( const chunkPoly_t& poly, float grid, int cell[3] )
{
	for( int a = 0; a < 3; a++ )
	{
		float lo = poly.points[0].xyz[a];
		float hi = lo;
		for( int i = 1; i < poly.numPoints; i++ )
		{
			lo = Min( lo, poly.points[i].xyz[a] );
			hi = Max( hi, poly.points[i].xyz[a] );
		}
		cell[a] = ( int )idMath::Floor( ( ( lo + hi ) * 0.5f ) / grid );
	}
}

/*
====================
ChunkTriInOneCell

Whether a tri needs no cut: no grid plane crosses its bounds, less CHUNK_ON_EPSILON at each
end. Its cell is then the cell of its bounds' middle, the rule ChunkCell gives a piece.
====================
*/
static bool ChunkTriInOneCell( const mapTri_t* tri, float grid, int cell[3] )
{
	for( int a = 0; a < 3; a++ )
	{
		float lo = tri->v[0].xyz[a];
		float hi = lo;
		for( int k = 1; k < 3; k++ )
		{
			lo = Min( lo, tri->v[k].xyz[a] );
			hi = Max( hi, tri->v[k].xyz[a] );
		}
		// no plane k * grid strictly crosses [ lo + e, hi - e ]
		const int first = ( int )idMath::Floor( ( lo + CHUNK_ON_EPSILON ) / grid ) + 1;
		const int last = ( int )idMath::Ceil( ( hi - CHUNK_ON_EPSILON ) / grid ) - 1;
		if( first <= last )
		{
			return false;
		}
		cell[a] = ( int )idMath::Floor( ( ( lo + hi ) * 0.5f ) / grid );
	}
	return true;
}

/*
====================
ChunkCutPointCell
====================
*/
static void ChunkCutPointCell( const idVec3& p, int c[3] )
{
	for( int a = 0; a < 3; a++ )
	{
		c[a] = ( int )idMath::Floor( p[a] * 4.0f );
	}
}

static int ChunkCutPointKey( const int c[3] )
{
	return ( int )( ( unsigned int )c[0] * 73856093u ^ ( unsigned int )c[1] * 19349663u ^ ( unsigned int )c[2] * 83492791u );
}

/*
====================
ChunkAddCutPoint
====================
*/
static void ChunkAddCutPoint( chunkCutPoints_t& cuts, const idVec3& p )
{
	int c[3];
	ChunkCutPointCell( p, c );
	const int key = ChunkCutPointKey( c );
	for( int i = cuts.hash.First( key ); i != -1; i = cuts.hash.Next( i ) )
	{
		if( cuts.points[i] == p )
		{
			return;
		}
	}
	cuts.hash.Add( key, cuts.points.Append( p ) );
}

/*
====================
ChunkFindCutPoint

A corner the cut made that lies on the edge a b where the edge crosses the plane
xyz[axis] = dist: on that plane itself ( a cut puts its corner there exactly ), within
CHUNK_ON_EPSILON of the edge's own crossing, within 1/64 of the edge's line and more than
CHUNK_ON_EPSILON from both its ends. Returns its index and its place on the edge from a
( f ), or -1. A corner of another plane, or one beside the edge or at its end, is not
taken: it would make a split without area, turned over, or found again without end
( reviews QR2-1, O1-1, O1-2 ).
====================
*/
static int ChunkFindCutPoint( const chunkCutPoints_t& cuts, const idVec3& a, const idVec3& b, const idVec3& crossing, int axis, float dist, float& f )
{
	const idVec3 edge = b - a;
	const float lengthSqr = edge.LengthSqr();
	if( lengthSqr <= 0.0f )
	{
		return -1;
	}
	const float length = idMath::Sqrt( lengthSqr );

	// a corner on the plane lies in the crossing's cell on that axis: the two others are searched
	int c[3];
	ChunkCutPointCell( crossing, c );
	for( int d0 = -1; d0 <= 1; d0++ )
	{
		for( int d1 = -1; d1 <= 1; d1++ )
		{
			int n[3] = { c[0], c[1], c[2] };
			n[( axis + 1 ) % 3] += d0;
			n[( axis + 2 ) % 3] += d1;
			const int key = ChunkCutPointKey( n );
			for( int i = cuts.hash.First( key ); i != -1; i = cuts.hash.Next( i ) )
			{
				const idVec3& p = cuts.points[i];
				if( p[axis] != dist || ( p - crossing ).LengthSqr() > CHUNK_ON_EPSILON * CHUNK_ON_EPSILON )
				{
					continue;
				}
				const float t = ( ( p - a ) * edge ) / lengthSqr;
				if( t * length <= CHUNK_ON_EPSILON || ( 1.0f - t ) * length <= CHUNK_ON_EPSILON )
				{
					continue;
				}
				if( ( p - ( a + edge * t ) ).LengthSqr() > CHUNK_EDGE_EPSILON * CHUNK_EDGE_EPSILON )
				{
					continue;
				}
				f = t;
				return i;
			}
		}
	}
	return -1;
}

/*
====================
ChunkHalvesFace

Whether both halves of tri split at p on its edge k face as tri does: a corner moved by
the snap can turn a sliver over ( FixTriangleAgainstHashVert refuses such a split too ).
====================
*/
static bool ChunkHalvesFace( const mapTri_t* tri, int k, const idVec3& p )
{
	const idVec3 facing = ( tri->v[1].xyz - tri->v[0].xyz ).Cross( tri->v[2].xyz - tri->v[0].xyz );
	idVec3 h0[3] = { tri->v[0].xyz, tri->v[1].xyz, tri->v[2].xyz };
	idVec3 h1[3] = { tri->v[0].xyz, tri->v[1].xyz, tri->v[2].xyz };
	h0[( k + 1 ) % 3] = p;
	h1[k] = p;
	return ( h0[1] - h0[0] ).Cross( h0[2] - h0[0] ) * facing > 0.0f && ( h1[1] - h1[0] ).Cross( h1[2] - h1[0] ) * facing > 0.0f;
}

/*
====================
ChunkFindDiscreteSplit

The first corner the cut left on one of tri's edges in the mask edges ( bit k: the edge v[k]
to v[k+1] ), tested where the edge crosses a grid plane by the cut's own rule ( an endpoint
within CHUNK_ON_EPSILON of the plane is on it: the cut made no corner there ). With
countOnly, true when there is one. Else the corner is made: the found position, snapped
as GetHashVert will snap its twin ( before that fix's weld of hash vertices one step
apart, which can move the twin up to 1/32 further ), or as found if the snap turns a half
over; a corner that turns a half over either way is not taken and is counted. The rest is
blended along the edge in float at the corner's place on it. Returns its edge and the
corner, or false.
====================
*/
static bool ChunkFindDiscreteSplit( const mapTri_t* tri, int edges, float grid, const chunkCutPoints_t& cuts, bool countOnly, int& splitEdge, idDrawVert& corner )
{
	for( int k = 0; k < 3; k++ )
	{
		if( ( edges & ( 1 << k ) ) == 0 )
		{
			continue;
		}
		const idDrawVert& a = tri->v[k];
		const idDrawVert& b = tri->v[( k + 1 ) % 3];
		for( int axis = 0; axis < 3; axis++ )
		{
			const float lo = Min( a.xyz[axis], b.xyz[axis] );
			const float hi = Max( a.xyz[axis], b.xyz[axis] );
			const int first = ( int )idMath::Floor( lo / grid ) + 1;
			const int last = ( int )idMath::Ceil( hi / grid ) - 1;
			for( int plane = first; plane <= last; plane++ )
			{
				const float dist = plane * grid;
				if( dist - lo <= CHUNK_ON_EPSILON || hi - dist <= CHUNK_ON_EPSILON )
				{
					continue;
				}
				idVec3 crossing = ::Lerp( a.xyz, b.xyz, ( dist - a.xyz[axis] ) / ( b.xyz[axis] - a.xyz[axis] ) );
				crossing[axis] = dist;
				float f;
				const int found = ChunkFindCutPoint( cuts, a.xyz, b.xyz, crossing, axis, dist, f );
				if( found < 0 )
				{
					continue;
				}
				if( countOnly )
				{
					return true;
				}

				idVec3 at = cuts.points[found];
				for( int c = 0; c < 3; c++ )
				{
					at[c] = idMath::Floor( ( at[c] + 0.5f / CHUNK_TJUNCTION_SNAP ) * CHUNK_TJUNCTION_SNAP ) / CHUNK_TJUNCTION_SNAP;
				}
				if( !ChunkHalvesFace( tri, k, at ) )
				{
					at = cuts.points[found];
					if( !ChunkHalvesFace( tri, k, at ) )
					{
						numRepairsTurned++;
						continue;
					}
				}

				chunkPoly_t poly;
				ChunkPolyFromTri( tri->v, poly );
				const chunkVert_t& pa = poly.points[k];
				const chunkVert_t& pb = poly.points[( k + 1 ) % 3];
				chunkVert_t mid;
				mid.xyz = at;
				mid.st = ::Lerp( pa.st, pb.st, f );
				mid.normal = ::Lerp( pa.normal, pb.normal, f );
				mid.tangent = ::Lerp( pa.tangent, pb.tangent, f );
				mid.bitangent = ::Lerp( pa.bitangent, pb.bitangent, f );
				for( int c = 0; c < 4; c++ )
				{
					mid.color[c] = pa.color[c] + f * ( pb.color[c] - pa.color[c] );
					mid.color2[c] = pa.color2[c] + f * ( pb.color2[c] - pa.color2[c] );
				}
				mid.corner = -1;
				ChunkPackVert( mid, tri->v, idVec2( 0.0f, 0.0f ), corner );	// the discrete tri keeps its st base
				splitEdge = k;
				return true;
			}
		}
	}
	return false;
}

typedef struct
{
	mapTri_t*		tri;
	int				edges;			// the mask of ChunkFindDiscreteSplit: the pieces of the source tri's own edges
} chunkRepairItem_t;

/*
====================
ChunkSplitDiscreteTri

Splits tri at every corner the cut left on its edges, and appends the tris left at *tail in
order. An explicit stack, not a recursion one level deep per corner, which a depth cap
would stop on a long edge ( review F1-2 ): each half goes back on it with its mask, half1
below half0, so the halves leave in the source's order. Only the pieces of the source tri's
own edges are tested: a half's inner edge, from the new corner to the opposite one, is
never a seam, and a corner lies exactly on its plane, so no ( edge, plane ) splits twice.
At most CHUNK_REPAIR_MAX_SPLITS splits per source tri; a split past it is not made and is
counted. Frees each tri it splits. Returns the number of splits, or with repair false 1
when tri has such an edge.
====================
*/
static int ChunkSplitDiscreteTri( mapTri_t* tri, float grid, const chunkCutPoints_t& cuts, bool repair, mapTri_t**& tail )
{
	int splitEdge;
	idDrawVert corner;
	if( !repair )
	{
		return ChunkFindDiscreteSplit( tri, 7, grid, cuts, true, splitEdge, corner ) ? 1 : 0;
	}

	idList<chunkRepairItem_t> stack;
	chunkRepairItem_t& source = stack.Alloc();
	source.tri = tri;
	source.edges = 7;
	int numSplits = 0;
	while( stack.Num() > 0 )
	{
		const chunkRepairItem_t item = stack[stack.Num() - 1];
		stack.SetNum( stack.Num() - 1 );

		if( numSplits < CHUNK_REPAIR_MAX_SPLITS && ChunkFindDiscreteSplit( item.tri, item.edges, grid, cuts, false, splitEdge, corner ) )
		{
			// two tris in the source's winding: half0 ( a, corner, opposite ) keeps edge k's first
			// piece and the edge before it, half1 ( corner, b, opposite ) edge k's second piece and
			// the edge after it; CopyMapTri copies tri's next too, which the append overwrites
			const int k = splitEdge;
			mapTri_t* half0 = CopyMapTri( item.tri );
			mapTri_t* half1 = CopyMapTri( item.tri );
			half0->v[( k + 1 ) % 3] = corner;
			half1->v[k] = corner;
			for( int c = 0; c < 3; c++ )
			{
				half0->hashVert[c] = half1->hashVert[c] = NULL;
				half0->optVert[c] = half1->optVert[c] = NULL;
			}
			chunkRepairItem_t& second = stack.Alloc();
			second.tri = half1;
			second.edges = ( 1 << k ) | ( item.edges & ( 1 << ( ( k + 1 ) % 3 ) ) );
			chunkRepairItem_t& first = stack.Alloc();
			first.tri = half0;
			first.edges = ( 1 << k ) | ( item.edges & ( 1 << ( ( k + 2 ) % 3 ) ) );
			FreeTri( item.tri );
			numSplits++;
			continue;
		}
		if( numSplits >= CHUNK_REPAIR_MAX_SPLITS && ChunkFindDiscreteSplit( item.tri, item.edges, grid, cuts, true, splitEdge, corner ) )
		{
			numRepairsCapped++;		// kept with its T-junction
		}
		item.tri->next = NULL;
		*tail = item.tri;
		tail = &item.tri->next;
	}
	return numSplits;
}

/*
====================
ChunkRepairDiscreteEdges

The world's discrete groups: each edge a corner of the cut lies on is split there, the
group's tris kept in their order. A material with a deform reads its quads, and a gui or a
subview takes its axes or its plane from its first tri, so for those the tris that have
such an edge are only counted ( the T-junction stays ). Returns the splits made; numLeft
gets the tris left with a T-junction.
====================
*/
static int ChunkRepairDiscreteEdges( uEntity_t* e, float grid, const chunkCutPoints_t& cuts, int& numLeft )
{
	int numSplits = 0;
	numLeft = 0;
	if( cuts.points.Num() == 0 )
	{
		return 0;
	}
	for( int a = 0; a < e->numAreas; a++ )
	{
		for( optimizeGroup_t* group = e->areas[a].groups; group; group = group->nextGroup )
		{
			if( group->material == NULL || !group->material->IsDiscrete() )
			{
				continue;
			}
			if( group->material->Deform() != DFRM_NONE || group->material->HasGui() || group->material->GetSort() == SS_SUBVIEW )
			{
				mapTri_t** none = NULL;
				for( mapTri_t* tri = group->triList; tri; tri = tri->next )
				{
					numLeft += ChunkSplitDiscreteTri( tri, grid, cuts, false, none );
				}
				continue;
			}

			mapTri_t* out = NULL;
			mapTri_t** tail = &out;
			mapTri_t* next;
			for( mapTri_t* tri = group->triList; tri; tri = next )
			{
				next = tri->next;
				numSplits += ChunkSplitDiscreteTri( tri, grid, cuts, true, tail );
			}
			group->triList = out;
		}
	}
	return numSplits;
}

/*
====================
ChunkStBase

Whole tiles nearest st, relative to base: a chunk's st base ( O1-5b ).
====================
*/
static idVec2 ChunkStBase( const idVec2& base, const idVec2& st )
{
	return idVec2( idMath::Floor( base.x + st.x + 0.5f ), idMath::Floor( base.y + st.y + 0.5f ) );
}

/*
====================
ChunkRebaseTri

A whole tri moved to its chunk's st base ( whole tiles ), each st packed again to half a step at
its new value, which lies within a few tiles of the base.
====================
*/
static void ChunkRebaseTri( mapTri_t* tri, const idVec2& base )
{
	if( tri->stBase == base )
	{
		return;
	}
	const idVec2 shift = tri->stBase - base;
	for( int k = 0; k < 3; k++ )
	{
		const idVec2 st = tri->v[k].GetTexCoord() + shift;
		tri->v[k].SetTexCoordNative( ChunkHalfRound( st.x ), ChunkHalfRound( st.y ) );
	}
	tri->stBase = base;
}

/*
====================
ChunkFindBin
====================
*/
static int ChunkFindBin( idList<chunkBin_t>& bins, idHashIndex& hash, int firstBin, uArea_t* area, optimizeGroup_t* parent, const int cell[3], const idVec2& stBase )
{
	// the hash holds this group's bins only, by their index from firstBin
	const int key = ( int )( ( unsigned int )cell[0] * 73856093u ^ ( unsigned int )cell[1] * 19349663u ^ ( unsigned int )cell[2] * 83492791u );
	for( int i = hash.First( key ); i != -1; i = hash.Next( i ) )
	{
		const chunkBin_t& bin = bins[firstBin + i];
		if( bin.cell[0] == cell[0] && bin.cell[1] == cell[1] && bin.cell[2] == cell[2] )
		{
			return firstBin + i;
		}
	}

	chunkBin_t& bin = bins.Alloc();
	bin.area = area;
	bin.parent = parent;
	bin.cell[0] = cell[0];
	bin.cell[1] = cell[1];
	bin.cell[2] = cell[2];
	bin.tris = NULL;
	bin.mergeGroup = -1;
	bin.stBase = stBase;
	hash.Add( key, bins.Num() - 1 - firstBin );
	return bins.Num() - 1;
}

/*
====================
ChunkEligible
====================
*/
static bool ChunkEligible( const optimizeGroup_t* group )
{
	// FixGlobalTjunctions allows for a group without a material; so does this pass
	if( group->material == NULL )
	{
		return false;
	}
	// every func_decal fragment group is discrete too ( dress_decal.cpp refuses any other
	// material ), so its merge group, which names its projector, is never replaced
	if( group->material->IsDiscrete() )
	{
		return false;
	}
	if( !group->material->ReceivesLighting() )
	{
		return false;
	}
	// the author asked dmap to break these only at area boundaries ( sky boxes )
	if( group->material->NoFragment() )
	{
		return false;
	}
	return true;
}

/*
====================
DressChunkWorldSurfaces
====================
*/
void DressChunkWorldSurfaces( uEntity_t* e )
{
	chunkMergeGroups.Clear();
	numSplitsRefused = 0;
	numSplitsOverlapping = 0;

	// the option wins; without it the map's own key decides, and without that nothing is cut
	int gridSize = dmapGlobals.chunkGrid;
	bool asked = gridSize >= 0;
	if( gridSize < 0 )
	{
		const char* key = e->mapEntity->epairs.GetString( "chunkGrid", "0" );
		asked = e->mapEntity->epairs.FindKey( "chunkGrid" ) != NULL;
		gridSize = atoi( key );
		// only the number itself, as for the option ( dmap.cpp )
		if( idStr::Cmp( va( "%i", gridSize ), key ) != 0 || ( gridSize != 0 && ( gridSize < 128 || gridSize > 1024 || !idMath::IsPowerOfTwo( gridSize ) ) ) )
		{
			common->Warning( "worldspawn chunkGrid '%s': not a power of two from 128 to 1024 or 0, world surfaces not chunked", key );
			gridSize = 0;
		}
	}
	if( gridSize == 0 )
	{
		// a compile that never asked prints nothing ( review O1-11 )
		if( asked )
		{
			common->Printf( "chunkGrid 0: world surfaces not chunked\n" );
		}
		return;
	}
	const float grid = ( float )gridSize;

	idList<chunkBin_t>		bins;
	idList<chunkPoly_t>		pieces[4];
	idHashIndex				binHash;
	chunkCutPoints_t		cuts;
	numRepairsCapped = 0;
	numRepairsTurned = 0;
	// at idList's granularity of 16 every growth copies the whole list: quadratic on a large map
	// ( reviews O1-6, O2-8; dress_decal.cpp does the same )
	bins.SetGranularity( 4096 );
	for( int i = 0; i < 4; i++ )
	{
		pieces[i].SetGranularity( 4096 );	// review F1-3: one large tri at grid 128 is 100,000 pieces
	}
	cuts.points.SetGranularity( 4096 );
	cuts.hash.Clear( 1 << 16, 4096 );
	int						numGroups = 0;
	int						numTrisIn = 0;
	int						numTrisCut = 0;
	int						numTrisOut = 0;
	int						numTrisDropped = 0;

	// the walk: each eligible group's tris are taken out of it and binned by cell
	for( int a = 0; a < e->numAreas; a++ )
	{
		uArea_t* area = &e->areas[a];
		for( optimizeGroup_t* group = area->groups; group; group = group->nextGroup )
		{
			if( !ChunkEligible( group ) )
			{
				continue;
			}

			numGroups += ( group->triList != NULL );
			binHash.Clear();
			const int firstBin = bins.Num();

			mapTri_t* next;
			for( mapTri_t* tri = group->triList; tri; tri = next )
			{
				next = tri->next;
				numTrisIn++;

				int cell[3];
				if( ChunkTriInOneCell( tri, grid, cell ) )
				{
					chunkBin_t& bin = bins[ChunkFindBin( bins, binHash, firstBin, area, group, cell, ChunkStBase( tri->stBase, tri->v[0].GetTexCoord() ) )];
					ChunkRebaseTri( tri, bin.stBase );
					tri->next = bin.tris;
					bin.tris = tri;
					numTrisOut++;
					continue;
				}

				numTrisCut++;
				const idVec3 facing = ( tri->v[1].xyz - tri->v[0].xyz ).Cross( tri->v[2].xyz - tri->v[0].xyz );
				pieces[0].SetNum( 1 );
				ChunkPolyFromTri( tri->v, pieces[0][0] );
				for( int axis = 0; axis < 3; axis++ )
				{
					pieces[axis + 1].SetNum( 0 );
					ChunkCutAxis( pieces[axis], axis, grid, pieces[axis + 1] );
				}

				for( int p = 0; p < pieces[3].Num(); p++ )
				{
					const chunkPoly_t& poly = pieces[3][p];
					if( poly.numPoints < 3 )
					{
						continue;
					}
					ChunkCell( poly, grid, cell );
					chunkBin_t& bin = bins[ChunkFindBin( bins, binHash, firstBin, area, group, cell, ChunkStBase( tri->stBase, poly.points[0].st ) )];
					const idVec2 stShift = tri->stBase - bin.stBase;

					idDrawVert corners[CHUNK_MAX_POINTS];
					for( int k = 0; k < poly.numPoints; k++ )
					{
						ChunkPackVert( poly.points[k], tri->v, stShift, corners[k] );
						if( poly.points[k].corner < 0 )
						{
							ChunkAddCutPoint( cuts, corners[k].xyz );
						}
					}

					// a fan in the source tri's winding
					for( int k = 2; k < poly.numPoints; k++ )
					{
						mapTri_t* piece = CopyMapTri( tri );
						piece->v[0] = corners[0];
						piece->v[1] = corners[k - 1];
						piece->v[2] = corners[k];
						piece->stBase = bin.stBase;
						piece->hashVert[0] = piece->hashVert[1] = piece->hashVert[2] = NULL;
						piece->optVert[0] = piece->optVert[1] = piece->optVert[2] = NULL;
						// a fan tri without area, or one facing against its source ( review CN1-1: none was
						// found reachable from a convex piece, but an area test would not see one )
						const idVec3 pieceFacing = ( piece->v[1].xyz - piece->v[0].xyz ).Cross( piece->v[2].xyz - piece->v[0].xyz );
						if( pieceFacing * facing <= 0.0f )
						{
							FreeTri( piece );
							numTrisDropped++;
							continue;
						}
						piece->next = bin.tris;
						bin.tris = piece;
						numTrisOut++;
					}
				}
				FreeTri( tri );
			}
			group->triList = NULL;
		}
	}

	// the merge groups: one per ( parent merge group, cell ), allocated once, so the
	// addresses do not move
	{
		idList<chunkMergeGroup_t>	keys;
		idHashIndex					keyHash( idMath::CeilPowerOfTwo( Max( 1024, bins.Num() ) ), Max( 1024, bins.Num() ) );
		keys.SetGranularity( 4096 );
		for( int b = 0; b < bins.Num(); b++ )
		{
			chunkBin_t& bin = bins[b];
			const int key = ( int )( ( unsigned int )bin.cell[0] * 73856093u ^ ( unsigned int )bin.cell[1] * 19349663u ^ ( unsigned int )bin.cell[2] * 83492791u ^ ( unsigned int )( ( uintptr_t )bin.parent->mergeGroup >> 4 ) );
			int m;
			for( m = keyHash.First( key ); m != -1; m = keyHash.Next( m ) )
			{
				if( keys[m].parentMergeGroup == bin.parent->mergeGroup && keys[m].cell[0] == bin.cell[0] && keys[m].cell[1] == bin.cell[1] && keys[m].cell[2] == bin.cell[2] )
				{
					break;
				}
			}
			if( m == -1 )
			{
				chunkMergeGroup_t& k = keys.Alloc();
				k.parentMergeGroup = bin.parent->mergeGroup;
				k.cell[0] = bin.cell[0];
				k.cell[1] = bin.cell[1];
				k.cell[2] = bin.cell[2];
				m = keys.Num() - 1;
				keyHash.Add( key, m );
			}
			bin.mergeGroup = m;
		}
		chunkMergeGroups.SetNum( keys.Num() );
		for( int m = 0; m < keys.Num(); m++ )
		{
			chunkMergeGroups[m] = keys[m];
		}
	}

	// after the walk: the parents leave their areas, one group per bin goes in
	for( int b = 0; b < bins.Num(); b++ )
	{
		const chunkBin_t& bin = bins[b];
		if( bin.tris == NULL )
		{
			continue;
		}

		void* mergeGroup = &chunkMergeGroups[bin.mergeGroup];
		for( mapTri_t* tri = bin.tris; tri; tri = tri->next )
		{
			tri->mergeGroup = mergeGroup;
		}

		optimizeGroup_t* chunk = ( optimizeGroup_t* )Mem_Alloc( sizeof( *chunk ), TAG_TOOLS );
		*chunk = *bin.parent;
		chunk->mergeGroup = mergeGroup;
		chunk->triList = bin.tris;
		chunk->regeneratedTris = NULL;
		chunk->surfaceEmited = false;
		chunk->stBase = bin.stBase;		// every tri of the bin is packed against it ( O1-5b )
		BoundTriList( chunk->triList, chunk->bounds );
		chunk->nextGroup = bin.area->groups;
		bin.area->groups = chunk;
	}

	// the parents: every eligible group the walk emptied, and an eligible group that was empty
	// before the pass ( nothing holds it ); a chunk group is never empty
	for( int a = 0; a < e->numAreas; a++ )
	{
		optimizeGroup_t** link = &e->areas[a].groups;
		while( *link )
		{
			optimizeGroup_t* group = *link;
			if( group->triList == NULL && ChunkEligible( group ) )
			{
				*link = group->nextGroup;
				FreeTriList( group->regeneratedTris );
				Mem_Free( group );
				continue;
			}
			link = &group->nextGroup;
		}
	}
	int numChunkGroups = 0;
	for( int b = 0; b < bins.Num(); b++ )
	{
		numChunkGroups += ( bins[b].tris != NULL );
	}

	bins.Clear();		// the repair needs only the corners ( review O2-14 )
	for( int i = 0; i < 4; i++ )
	{
		pieces[i].Clear();
	}

	// the discrete groups' edges the cut met
	int numDiscreteLeft;
	const int numDiscreteSplits = ChunkRepairDiscreteEdges( e, grid, cuts, numDiscreteLeft );

	// what the renderer pays: each frame it walks every surface of a visible area against every
	// light that reaches the area, culled surfaces included, and a static interaction holds a slot
	// per surface per light ( reviews O2-3, CN2-1, O1-7; F0-MERGE.md "Later": the table inverted
	// per chunk is the cure ). The surfaces counted here are the area's merge groups by material.
	int worstArea = -1;
	int worstSurfaces = 0;
	int worstLights = 0;
	for( int a = 0; a < e->numAreas; a++ )
	{
		idList<const optimizeGroup_t*> surfaces;
		idBounds areaBounds;
		areaBounds.Clear();
		for( const optimizeGroup_t* group = e->areas[a].groups; group; group = group->nextGroup )
		{
			if( group->triList == NULL )
			{
				continue;
			}
			idBounds b;
			BoundTriList( group->triList, b );
			areaBounds.AddBounds( b );
			int s;
			for( s = 0; s < surfaces.Num(); s++ )
			{
				if( surfaces[s]->material == group->material && surfaces[s]->mergeGroup == group->mergeGroup )
				{
					break;
				}
			}
			if( s == surfaces.Num() )
			{
				surfaces.Append( group );
			}
		}
		int lights = 0;
		for( int i = 0; i < dmapGlobals.mapLights.Num(); i++ )
		{
			lights += dmapGlobals.mapLights[i]->def.globalLightBounds.IntersectsBounds( areaBounds );
		}
		if( worstArea < 0 || ( int64 )surfaces.Num() * lights > ( int64 )worstSurfaces * worstLights )
		{
			worstArea = a;
			worstSurfaces = surfaces.Num();
			worstLights = lights;
		}
	}

	common->Printf( "chunkGrid %i: %i world groups chunked into %i groups, %i merge groups; %i tris, %i of them cut, %i after ( %i fan tris dropped )\n",
					gridSize, numGroups, numChunkGroups, chunkMergeGroups.Num(), numTrisIn, numTrisCut, numTrisOut, numTrisDropped );
	common->Printf( "chunkGrid %i: %i discrete edges split at the cut's corners, %i tris of deform, gui or subview materials left with one\n",
					gridSize, numDiscreteSplits, numDiscreteLeft );
	if( worstArea >= 0 )
	{
		const int64 pairs = ( int64 )worstSurfaces * worstLights;
		common->Printf( "chunkGrid %i: the largest area, %i: %i surfaces x %i map lights = %lld surface-light pairs\n", gridSize, worstArea, worstSurfaces, worstLights, ( long long )pairs );
		if( pairs > 100000 )
		{
			common->Warning( "chunkGrid %i: area %i has %lld surface-light pairs; the renderer walks them every frame the area is seen, and its static interactions hold 16 bytes each: a coarser grid, or none", gridSize, worstArea, ( long long )pairs );
		}
	}
	if( numDiscreteLeft > 0 )
	{
		common->Warning( "chunkGrid %i: %i tris of deform, gui or subview materials keep a T-junction where the cut met their edge", gridSize, numDiscreteLeft );
	}
	if( numSplitsOverlapping > 0 )
	{
		common->Printf( "chunkGrid %i: %i cuts with two or more corners in a row within 1/16 of the plane: their pieces overlap there ( a double blend on a lit translucent surface )\n", gridSize, numSplitsOverlapping );
	}
	if( numSplitsRefused > 0 )
	{
		common->Warning( "chunkGrid: %i cuts refused for want of room ( CHUNK_MAX_POINTS ); those pieces span more than one cell", numSplitsRefused );
	}
	if( numRepairsTurned > 0 )
	{
		common->Printf( "chunkGrid %i: %i discrete edge splits not made, a half would turn over ( slivers ); those edges keep a T-junction\n", gridSize, numRepairsTurned );
	}
	if( numRepairsCapped > 0 )
	{
		common->Warning( "chunkGrid: %i discrete edge splits past %i on one tri not made; those edges keep a T-junction", numRepairsCapped, CHUNK_REPAIR_MAX_SPLITS );
	}
}
