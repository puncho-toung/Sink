/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.

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

#include "dmap.h"

//puncho start - Phase 2C P2 (S5)
idCVar dmap_brushBaseWindings( "dmap_brushBaseWindings", "1", CVAR_TOOL | CVAR_BOOL, "1 = a non-axial brush's windings, and the mid winding of every brush split, are made from a square sized to the brush and clipped in a brush-local frame, so copies of one corner agree at any distance from the origin (dmap_brushSourcePlanes and dmap_visibleHullFix act only with it); 0 = stock windings from 2^18 squares at the plane foot" );
static int c_brushBaseWindingFallbacks;	// brushes whose local windings bounded nothing valid, in this process
idCVar dmap_brushSourcePlanes( "dmap_brushSourcePlanes", "1", CVAR_TOOL | CVAR_BOOL, "a test switch, with dmap_brushBaseWindings 1 only: a non-axial Valve 220 side's local plane is made in double from the map's three points when they lie within 0.02 of its stored plane; 0 = the stored float plane, moved to the local frame (on some content worse than stock far from the origin)" );
//puncho end
//puncho start - Phase 2C P2.4
idCVar dmap_mapPointsDouble( "dmap_mapPointsDouble", "1", CVAR_TOOL | CVAR_BOOL, "with dmap_brushSourcePlanes 1: a Valve 220 side's local plane is made from its map points as the map's text has them (the parser keeps them in double where float does not hold them); 0 = the points rounded to float, as in v0001.7l" );
//puncho end

int		c_active_brushes;

int		c_nodes;

// if a brush just barely pokes onto the other side,
// let it slide by without chopping
#define	PLANESIDE_EPSILON	0.001
//0.1




/*
================
CountBrushList
================
*/
int	CountBrushList( uBrush_t* brushes )
{
	int	c;

	c = 0;
	for( ; brushes ; brushes = brushes->next )
	{
		c++;
	}
	return c;
}


int BrushSizeForSides( int numsides )
{
	int		c;

	// allocate a structure with a variable number of sides at the end
//	c = (int)&(((uBrush_t *)0)->sides[numsides]);	// bounds checker complains about this
	c = sizeof( uBrush_t ) + sizeof( side_t ) * ( numsides - 6 );

	return c;
}

/*
================
AllocBrush
================
*/
uBrush_t* AllocBrush( int numsides )
{
	uBrush_t*	bb;
	int			c;

	c = BrushSizeForSides( numsides );

	bb = ( uBrush_t* )Mem_Alloc( c, TAG_TOOLS );
	memset( bb, 0, c );
	c_active_brushes++;
	return bb;
}

/*
================
FreeBrush
================
*/
void FreeBrush( uBrush_t* brushes )
{
	int			i;

	for( i = 0 ; i < brushes->numsides ; i++ )
	{
		if( brushes->sides[i].winding )
		{
			delete brushes->sides[i].winding;
		}
		if( brushes->sides[i].visibleHull )
		{
			delete brushes->sides[i].visibleHull;
		}
	}
	Mem_Free( brushes );
	c_active_brushes--;
}


/*
================
FreeBrushList
================
*/
void FreeBrushList( uBrush_t* brushes )
{
	uBrush_t*	next;

	for( ; brushes ; brushes = next )
	{
		next = brushes->next;

		FreeBrush( brushes );
	}
}

/*
==================
CopyBrush

Duplicates the brush, the sides, and the windings
==================
*/
uBrush_t* CopyBrush( uBrush_t* brush )
{
	uBrush_t* newbrush;
	int			size;
	int			i;

	size = BrushSizeForSides( brush->numsides );

	newbrush = AllocBrush( brush->numsides );
	memcpy( newbrush, brush, size );

	for( i = 0 ; i < brush->numsides ; i++ )
	{
		if( brush->sides[i].winding )
		{
			newbrush->sides[i].winding = brush->sides[i].winding->Copy();
		}
	}

	return newbrush;
}

/*
=============
PrintBrush
=============
*/
void PrintBrush( uBrush_t* brush )
{
	int		i;

	common->Printf( "brush: %p\n", brush );
	for( i = 0; i < brush->numsides ; i++ )
	{
		brush->sides[i].winding->Print();
		common->Printf( "\n" );
	}
}

/*
==================
BoundBrush

Sets the mins/maxs based on the windings
returns false if the brush doesn't enclose a valid volume
==================
*/
bool BoundBrush( uBrush_t* brush )
{
	int			i, j;
	idWinding*	w;

	brush->bounds.Clear();
	for( i = 0; i < brush->numsides; i++ )
	{
		w = brush->sides[i].winding;
		if( !w )
		{
			continue;
		}
		for( j = 0; j < w->GetNumPoints(); j++ )
		{
			brush->bounds.AddPoint( ( *w )[j].ToVec3() );
		}
	}

	for( i = 0; i < 3; i++ )
	{
		if( brush->bounds[0][i] < MIN_WORLD_COORD || brush->bounds[1][i] > MAX_WORLD_COORD
				|| brush->bounds[0][i] >= brush->bounds[1][i] )
		{
			return false;
		}
	}

	return true;
}

//puncho start - Phase 2C P2 (S5b)
/*
==================
LocalSourcePlane

A plane made in float far from the origin is off by up to an ulp of its distance (about 0.001 at 16k), and faces that
meet at a few degrees turn that into corner errors past the 1/32 weld. A non-axial side that still has the map's
three points, each within BRUSHFRAME_SOURCE_PLANE_EPSILON of the stored plane, gets its local plane (for p = c + q)
from those points in double: the plane idPlane::FromPoints makes, without its rounding (idBrushFrame::PlaneFromPoints,
Phase 2C P2.1). The points are taken less the entity origin the parser took off before making the plane (Phase 2C P2.1,
Opus A review: compared as written, every side of an entity with an origin was refused unless n.origin was below the
tolerance, and then taken off by n.origin). Returns false, leaving `local` unset, otherwise (no points, an axial plane,
points that do not define the stored plane, or collinear points).
==================
*/
static bool LocalSourcePlane( const side_t& side, const idPlane& plane, const idBrushFrame& frame, idPlane& local )
{
	if( !side.hasSourcePts || dmapSourceBrush == NULL || side.sourceSide < 0 || side.sourceSide >= dmapSourceBrush->GetNumSides() )
	{
		return false;
	}
	const idMapBrushSide* mapSide = dmapSourceBrush->GetSide( side.sourceSide );
	//puncho start - Phase 2C P2.4: the points as the map's text has them
	double points[3][3];
	mapSide->GetPlanePoints( points, dmap_mapPointsDouble.GetBool() );
	return frame.PlaneFromPoints( points, mapSide->GetOrigin(), plane, local );
	//puncho end
}
//puncho end

//puncho start - Phase 2C P2.2: the brush windings in double
idCVar dmap_brushWindingsDouble( "dmap_brushWindingsDouble", "1", CVAR_TOOL | CVAR_BOOL, "with dmap_brushBaseWindings 1: a non-axial brush's windings and every split's mid winding are clipped in double in the brush's frame (the planes kept in double, a Valve 220 side's from its map points) and rounded once per coordinate; 0 = float clipping in the frame" );

// LocalSourcePlane with the plane kept in double
static bool LocalSourcePlaneDouble( const side_t& side, const idPlane& plane, const idBrushFrame& frame, idDPlane& local )
{
	if( !side.hasSourcePts || dmapSourceBrush == NULL || side.sourceSide < 0 || side.sourceSide >= dmapSourceBrush->GetNumSides() )
	{
		return false;
	}
	const idMapBrushSide* mapSide = dmapSourceBrush->GetSide( side.sourceSide );
	//puncho start - Phase 2C P2.4: the points as the map's text has them
	double points[3][3];
	mapSide->GetPlanePoints( points, dmap_mapPointsDouble.GetBool() );
	return frame.PlaneFromPointsDouble( points, mapSide->GetOrigin(), plane, local );
	//puncho end
}

/*
==================
CreateBrushWindingsDouble

CreateBrushWindingsLocal's windings clipped in double (idDWinding) and rounded once per coordinate. In float the edge
where two near-coplanar sides meet moves by about the rounding of the local coordinates over the sine of their angle,
and the faces of such sides leave strips that neither covers (inv B: 31 to 1,942 see-through rays on near-coplanar
rock sets). Returns false, setting no winding, when a clip overflows idDWinding: the caller then clips in float.
==================
*/
static bool CreateBrushWindingsDouble( uBrush_t* brush, const idBrushFrame& frame )
{
	const int numSides = brush->numsides;
	int i, j;

	// numSides <= MAX_BUILD_SIDES (512): at most 16 KB and 4 KB on the stack
	idDPlane* localPlanes = ( idDPlane* ) _alloca16( numSides * sizeof( localPlanes[0] ) );
	idWinding** windings = ( idWinding** ) _alloca16( numSides * sizeof( windings[0] ) );
	for( i = 0; i < numSides; i++ )
	{
		const idPlane& plane = dmapGlobals.mapPlanes[brush->sides[i].planenum];
		if( dmap_brushSourcePlanes.GetBool() && LocalSourcePlaneDouble( brush->sides[i], plane, frame, localPlanes[i] ) )
		{
			continue;
		}
		localPlanes[i] = frame.MovePlaneDouble( plane );
	}

	idDWinding dw;
	for( i = 0; i < numSides; i++ )
	{
		dw.BaseForPlane( localPlanes[i], frame.HalfSize() );
		bool left = true;
		for( j = 0; j < numSides && left; j++ )
		{
			if( i == j )
			{
				continue;
			}
			if( brush->sides[j].planenum == ( brush->sides[i].planenum ^ 1 ) )
			{
				continue;		// back side clipaway
			}
			left = dw.Clip( -localPlanes[j], 0.0, false );
		}
		if( dw.Overflowed() )
		{
			for( j = 0; j < i; j++ )
			{
				delete windings[j];
			}
			return false;
		}
		windings[i] = NULL;
		if( left )
		{
			windings[i] = dw.ToWinding( frame.Centre() );
			// where three or more planes meet, double puts the copies of a corner on one float point; repeated points
			// make idWinding::GetPlane (p[0], p[1] and the centre) return a zero normal, and FaceBSP's Split then sends a
			// face lying within ON_EPSILON of a node plane to the back (inv B: +80 to +1,100 rays without this)
			windings[i]->RemoveEqualPoints( 0.01f );
			// a side that touches the brush only at a corner or an edge (or a face under 0.01) keeps no polygon (Opus A
			// review: a 1- or 2-point winding gives GetPlane a zero normal)
			if( windings[i]->GetNumPoints() < 3 )
			{
				delete windings[i];
				windings[i] = NULL;
			}
		}
	}
	for( i = 0; i < numSides; i++ )
	{
		brush->sides[i].winding = windings[i];
	}
	return true;
}
//puncho end

//puncho start - Phase 2C P2 (S5)
/*
==================
CreateBrushWindingsLocal

Rebuilds the windings of a brush whose stock windings bound a valid volume. A stock winding starts from a 2^18
square at the plane's foot, so the copies of one corner made on different sides land up to about ulp(2^18) times
the conditioning apart, past the 1/32 weld: the cracks of steep brushes. Here each side starts from a square sized
to the brush, at the projection of the brush's rounded centre c, and is clipped with the planes re-expressed for
p = c + q (the large terms summed in double); only the final c + q rounds at world scale. A brush whose planes are
all axial is left as it is (the stock clip is exact there). If the new windings do not bound a valid volume, the
stock windings and bounds are kept.
==================
*/
static void CreateBrushWindingsLocal( uBrush_t* brush )
{
	const int numSides = brush->numsides;
	int i, j;

	for( i = 0; i < numSides; i++ )
	{
		if( dmapGlobals.mapPlanes[brush->sides[i].planenum].Type() >= PLANETYPE_TRUEAXIAL )
		{
			break;
		}
	}
	if( i == numSides )
	{
		return;
	}

	const idBounds stockBounds = brush->bounds;
	idBrushFrame frame;	//puncho - Phase 2C P2.1: the frame rule is idlib's (geometry/BrushFrame.h)
	frame.FromBounds( stockBounds );

	// numSides <= MAX_BUILD_SIDES (512): at most 12 KB on the stack
	idPlane* localPlanes = ( idPlane* ) _alloca16( numSides * sizeof( localPlanes[0] ) );
	idWinding** stockWindings = ( idWinding** ) _alloca16( numSides * sizeof( stockWindings[0] ) );
	for( i = 0; i < numSides; i++ )
	{
		const idPlane& plane = dmapGlobals.mapPlanes[brush->sides[i].planenum];
		stockWindings[i] = brush->sides[i].winding;
		//puncho - Phase 2C P2 (S5b): the side's own map points give an exact local plane when they still define it
		if( dmap_brushSourcePlanes.GetBool() && LocalSourcePlane( brush->sides[i], plane, frame, localPlanes[i] ) )
		{
			continue;
		}
		localPlanes[i] = frame.MovePlane( plane );
	}

	//puncho start - Phase 2C P2.2: in double, the float clip below only if a clip overflows
	if( !dmap_brushWindingsDouble.GetBool() || !CreateBrushWindingsDouble( brush, frame ) )
	//puncho end
	for( i = 0; i < numSides; i++ )
	{
		idWinding* w = new idWinding;
		w->BaseForPlane( localPlanes[i], frame.HalfSize() );
		for( j = 0; j < numSides && w; j++ )
		{
			if( i == j )
			{
				continue;
			}
			if( brush->sides[j].planenum == ( brush->sides[i].planenum ^ 1 ) )
			{
				continue;		// back side clipaway
			}
			w = w->Clip( -localPlanes[j], 0 );
		}
		if( w )
		{
			frame.ToWorld( *w );
		}
		brush->sides[i].winding = w;
	}

	const bool valid = BoundBrush( brush );
	for( i = 0; i < numSides; i++ )
	{
		if( valid )
		{
			delete stockWindings[i];
		}
		else
		{
			delete brush->sides[i].winding;
			brush->sides[i].winding = stockWindings[i];
		}
	}
	if( !valid )
	{
		brush->bounds = stockBounds;
		c_brushBaseWindingFallbacks++;
		common->Printf( "dmap_brushBaseWindings: fallback on entity %d brush %d (process total %d)\n", brush->entitynum, brush->brushnum, c_brushBaseWindingFallbacks );
	}
}
//puncho end

/*
==================
CreateBrushWindings

makes basewindigs for sides and mins / maxs for the brush
returns false if the brush doesn't enclose a valid volume
==================
*/
bool CreateBrushWindings( uBrush_t* brush )
{
	int			i, j;
	idWinding*	w;
	idPlane*		plane;
	side_t*		side;

	for( i = 0; i < brush->numsides; i++ )
	{
		side = &brush->sides[i];
		plane = &dmapGlobals.mapPlanes[side->planenum];
		w = new idWinding( *plane );
		for( j = 0; j < brush->numsides && w; j++ )
		{
			if( i == j )
			{
				continue;
			}
			if( brush->sides[j].planenum == ( brush->sides[i].planenum ^ 1 ) )
			{
				continue;		// back side clipaway
			}
			plane = &dmapGlobals.mapPlanes[brush->sides[j].planenum ^ 1];
			w = w->Clip( *plane, 0 );//CLIP_EPSILON);
		}
		if( side->winding )
		{
			delete side->winding;
		}
		side->winding = w;
	}

	//puncho start - Phase 2C P2 (S5): the stock verdict decides validity; a valid non-axial brush is then rebuilt
	if( !BoundBrush( brush ) )
	{
		return false;
	}
	if( dmap_brushBaseWindings.GetBool() )
	{
		CreateBrushWindingsLocal( brush );
	}
	return true;
	//puncho end
}

/*
==================
BrushFromBounds

Creates a new axial brush
==================
*/
uBrush_t*	BrushFromBounds( const idBounds& bounds )
{
	uBrush_t*	b;
	int			i;
	idPlane		plane;

	b = AllocBrush( 6 );
	b->numsides = 6;
	for( i = 0 ; i < 3 ; i++ )
	{
		plane[0] = plane[1] = plane[2] = 0;
		plane[i] = 1;
		plane[3] = -bounds[1][i];
		b->sides[i].planenum = FindFloatPlane( plane );

		plane[i] = -1;
		plane[3] = bounds[0][i];
		b->sides[3 + i].planenum = FindFloatPlane( plane );
	}

	CreateBrushWindings( b );

	return b;
}

/*
==================
BrushVolume

==================
*/
float BrushVolume( uBrush_t* brush )
{
	int			i;
	idWinding*	w;
	idVec3		corner;
	float		d, area, volume;
	idPlane*		plane;

	if( !brush )
	{
		return 0;
	}

	// grab the first valid point as the corner

	w = NULL;
	for( i = 0; i < brush->numsides; i++ )
	{
		w = brush->sides[i].winding;
		if( w )
		{
			break;
		}
	}
	if( !w )
	{
		return 0;
	}
	corner = ( *w )[0].ToVec3();

	// make tetrahedrons to all other faces

	volume = 0;
	for( ; i < brush->numsides; i++ )
	{
		w = brush->sides[i].winding;
		if( !w )
		{
			continue;
		}
		plane = &dmapGlobals.mapPlanes[brush->sides[i].planenum];
		d = -plane->Distance( corner );
		area = w->GetArea();
		volume += d * area;
	}

	volume /= 3;
	return volume;
}


/*
==================
WriteBspBrushMap

FIXME: use new brush format
==================
*/
void WriteBspBrushMap( const char* name, uBrush_t* list )
{
	idFile* 	f;
	side_t* 	s;
	int			i;
	idWinding* 	w;

	common->Printf( "writing %s\n", name );
	f = fileSystem->OpenFileWrite( name );

	if( !f )
	{
		common->Error( "Can't write %s\b", name );
	}

	f->Printf( "{\n\"classname\" \"worldspawn\"\n" );

	for( ; list ; list = list->next )
	{
		f->Printf( "{\n" );
		for( i = 0, s = list->sides ; i < list->numsides ; i++, s++ )
		{
			w = new idWinding( dmapGlobals.mapPlanes[s->planenum] );

			f->Printf( "( %i %i %i ) ", ( int )( *w )[0][0], ( int )( *w )[0][1], ( int )( *w )[0][2] );
			f->Printf( "( %i %i %i ) ", ( int )( *w )[1][0], ( int )( *w )[1][1], ( int )( *w )[1][2] );
			f->Printf( "( %i %i %i ) ", ( int )( *w )[2][0], ( int )( *w )[2][1], ( int )( *w )[2][2] );

			f->Printf( "notexture 0 0 0 1 1\n" );
			delete w;
		}
		f->Printf( "}\n" );
	}
	f->Printf( "}\n" );

	fileSystem->CloseFile( f );

}


//=====================================================================================

/*
====================
FilterBrushIntoTree_r
====================
*/
int FilterBrushIntoTree_r( uBrush_t* b, node_t* node )
{
	uBrush_t*		front, *back;
	int				c;

	if( !b )
	{
		return 0;
	}

	// add it to the leaf list
	if( node->planenum == PLANENUM_LEAF )
	{
		b->next = node->brushlist;
		node->brushlist = b;

		// classify the leaf by the structural brush
		if( b->opaque )
		{
			if( !node->opaque )
			{
				node->opaque = true;
				return 1;
			}
		}

		return 0;
	}

	// split it by the node plane
	SplitBrush( b, node->planenum, &front, &back );
	FreeBrush( b );

	c = 0;
	c += FilterBrushIntoTree_r( front, node->children[0] );
	c += FilterBrushIntoTree_r( back, node->children[1] );

	return c;
}


//puncho start - Phase 2C P2.3: the leaf repair
/*
==============================================================================

LEAF REPAIR

FaceBSP classes a face that crosses a node plane by less than ON_EPSILON (0.1) wholly on one side, while SplitBrush
splits the face's brush when the brush fragment crosses the plane by 0.1 or more. At a corner made only by node planes
(a thin wedge between near-coplanar faces) a fragment then lands in a leaf that its own sides never cut, and
FilterBrushIntoTree_r marks the whole leaf opaque: the air in it and the faces on its boundary vanish from the .proc
and, through the .proc tree, from the collision model (P2.2 inv B). Such a leaf is split by an own side of the fragment
that cuts it, and its fragments are filtered again.

SplitBrush also sends a brush that crosses a plane by less than 0.1 wholly to one side, so a thin leaf can lie inside a
brush that left no fragment in it: opened, such a part is air inside solid (P2.2 Opus A: a sealed cave leaked). So the
part of the leaf beyond a cutting side is opened only when it touches another leaf through a portal piece that
SplitNodePortals keeps, and no opaque brush of the entity that has no fragment in the leaf holds a point of it
more than LEAF_REPAIR_INSIDE_EPSILON behind every side (the planes the map wrote, in double); else the next
cutting side is tried, and the leaf stays opaque when none is left. After the split, a child left without an opaque
fragment is tested against every opaque brush of the entity and stays opaque when one holds a point of it.

A part is held by a brush when the two share a point more than LEAF_REPAIR_INSIDE_EPSILON behind every side of the
brush: the part's faces and corners are found by clipping a square on every plane of its path and of the tree's box by
all the others (in double), a brush with a side that has every corner beyond it is apart, and for any other the faces of
the part and the sides of the brush pulled in by the epsilon are clipped the same way: a winding left is a shared
volume. A part that cannot be bounded this way counts as held. The portal windings are not used: they can reach up to
CLIP_EPSILON beyond the leaf (MakeNodePortal clips them with it); sampled at its portal points, the wedge of air in
front of a boulder face (g_vr_s5 brush 529) lay 0.021 inside the boulder, and sampled at its centroid and midpoints
a part of an organic shell kept a point 0.115 inside a tile out of reach.

==============================================================================
*/
idCVar dmap_leafRepair( "dmap_leafRepair", "1", CVAR_TOOL | CVAR_BOOL, "1 = a leaf made opaque only by brush fragments whose own sides cut it is split by such a side and its fragments filtered again, where no opaque brush of the entity holds a point of the part that opens; 0 = v0001.7l" );
idCVar dmap_leafRepairPrint( "dmap_leafRepairPrint", "0", CVAR_TOOL | CVAR_BOOL, "1 = print every leaf repair and, for every child it opens, the planes of its path (REPAIR, CHILD and PATH lines, read by the gate's airchk)" );

// a brush side cuts a leaf when the leaf's portal points reach this far beyond it (SplitBrush's threshold)
#define LEAF_REPAIR_CUT_EPSILON		0.1f
// a point lies inside a brush when it is this far behind every side: above the float error of a stored plane at
// 64k (about 0.008), so a point on a node plane is not inside the brush whose side made the plane
#define LEAF_REPAIR_INSIDE_EPSILON	0.02
// a cutting side must have the leaf's portal points this far behind it (a part left for the fragment)
#define LEAF_REPAIR_BEHIND_EPSILON	0.001f
// the tree's bounds grown by this bound every part: MakeHeadnodePortals' box (SIDESPACE 8) holds every leaf, and a leaf
// whose outside portal SplitNodePortals dropped as tiny would otherwise reach out without end
#define LEAF_REPAIR_BOX_MARGIN		16.0
// idWinding::IsTiny's edge length: SplitNodePortals drops a portal piece with fewer than three longer edges
#define LEAF_REPAIR_EDGE_LENGTH		0.2
// the cutting sides tried on one leaf
#define LEAF_REPAIR_MAX_SIDES		16

typedef struct leafRepair_s
{
	idList<const uBrush_t*>	brushes;	// the entity's opaque brushes
	idList<int>				firstPlane;	// per brush its first plane in planes; one plane per side follows
	idList<idDPlane>		planes;		// world planes in double, a Valve 220 side's from its map points
	idList<idDPlane>		path;		// scratch: a part's portal planes and every plane above it, facing the part, unit length
	idList<idDPlane>		faces;		// scratch: the planes of path that bound the part
	idList<double>			points;		// scratch: the part's corners, three doubles a point
	idList<idDPlane>		inner;		// scratch: a brush's sides pulled in by LEAF_REPAIR_INSIDE_EPSILON, facing inwards
	idList<idDPlane>		both;		// scratch: faces and inner
	idDPlane				box[6];		// the tree's bounds and LEAF_REPAIR_BOX_MARGIN, facing in
	int						numBoxParts;	// for dmap_leafRepairPrint: parts that reached the box
	int						numFlatParts;	// for dmap_leafRepairPrint: parts with a plane and its exact opposite on the path
	int						numFewFaceParts;	// for dmap_leafRepairPrint: parts with fewer than four faces
	int						numSplits;
	int						numRefused;	// leafs left opaque: no cutting side leaves a part to open
	int						numKept;	// children without an opaque fragment that a brush still holds
	//puncho start - Phase 2C P2.6: a grid of the brushes' bounds for LeafRepairHeld (LeafRepairGridCandidates)
	bool					gridBuilt;
	int64					gridScanTests;	// the bounds tests of the scans of every brush before the grid was built
	double					gridOrigin[3];
	double					gridScale;		// cells per unit
	int						gridSize[3];
	int						gridCells;
	idList<int>				gridFirst;		// per cell its first entry in gridBrushes, and the end after the last cell
	idList<int>				gridBrushes;	// per cell the brushes whose bounds reach into it, in index order
	idList<int>				gridLarge;		// brushes whose bounds reach into more than LEAF_REPAIR_GRID_MAX_CELLS cells
	idList<int>				gridMark;		// per brush the query that took it last
	int						gridQuery;
	idList<int>				gridCandidates;	// scratch: a query's brushes, in index order
	//puncho end
} leafRepair_t;

static bool LeafRepairPlaneOnPath( const node_t* leaf, int planenum )
{
	for( const node_t* n = leaf->parent; n != NULL; n = n->parent )
	{
		if( ( n->planenum & ~1 ) == ( planenum & ~1 ) )
		{
			return true;
		}
	}
	return false;
}

// how far the leaf's portal points reach behind and beyond a plane
static void LeafRepairExtent( const node_t* leaf, const idPlane& plane, float& dmin, float& dmax )
{
	dmin = idMath::INFINITUM;
	dmax = -idMath::INFINITUM;
	for( const uPortal_t* p = leaf->portals; p != NULL; p = p->next[ p->nodes[1] == leaf ] )
	{
		for( int i = 0; i < p->winding->GetNumPoints(); i++ )
		{
			const float d = plane.Distance( ( *p->winding )[i].ToVec3() );
			dmin = Min( dmin, d );
			dmax = Max( dmax, d );
		}
	}
}

// the own sides of the leaf's opaque fragments that cut it (plane numbers, the brush behind each); false when the leaf
// holds no opaque fragment or one that no own side cuts (it fills the leaf)
static bool LeafRepairCutSides( const node_t* leaf, idStaticList<int, LEAF_REPAIR_MAX_SIDES>& sides )
{
	sides.Clear();
	bool opaque = false;
	for( const uBrush_t* b = leaf->brushlist; b != NULL; b = b->next )
	{
		if( !b->opaque )
		{
			continue;
		}
		opaque = true;
		bool cut = false;
		for( int i = 0; i < b->numsides; i++ )
		{
			const side_t& s = b->sides[i];
			if( s.material == NULL || s.winding == NULL || LeafRepairPlaneOnPath( leaf, s.planenum ) )
			{
				continue;
			}
			float dmin, dmax;
			LeafRepairExtent( leaf, dmapGlobals.mapPlanes[s.planenum], dmin, dmax );
			if( dmax > LEAF_REPAIR_CUT_EPSILON && dmin < -LEAF_REPAIR_BEHIND_EPSILON )
			{
				cut = true;
				if( sides.Num() < LEAF_REPAIR_MAX_SIDES && sides.FindIndex( s.planenum ) < 0 )
				{
					sides.Append( s.planenum );
				}
			}
		}
		if( !cut )
		{
			return false;
		}
	}
	return opaque;
}

// whether a portal of the leaf keeps a piece in front of the plane with three edges longer than LEAF_REPAIR_EDGE_LENGTH:
// SplitNodePortals would keep it, and the part of the leaf in front would touch another leaf
static bool LeafRepairTouches( const node_t* leaf, const idPlane& plane )
{
	idDPlane dplane;
	dplane.FromPlane( plane );
	for( const uPortal_t* p = leaf->portals; p != NULL; p = p->next[ p->nodes[1] == leaf ] )
	{
		idDWinding w;
		w.FromWinding( *p->winding, vec3_origin );
		if( !w.Clip( dplane, 0.0, false ) )
		{
			continue;
		}
		int edges = 0;
		for( int i = 0; i < w.GetNumPoints(); i++ )
		{
			const double* a = w[i];
			const double* b = w[( i + 1 ) % w.GetNumPoints()];
			const double d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
			if( d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > LEAF_REPAIR_EDGE_LENGTH * LEAF_REPAIR_EDGE_LENGTH )
			{
				edges++;
			}
		}
		if( edges >= 3 )
		{
			return true;
		}
	}
	return false;
}

// the leaf's path into r.path, every plane facing the leaf: `extra` first when one is given (the part in front of it),
// then the planes of its portals (the outside box's among them), then every plane above it, then the tree's box
static void LeafRepairAddPathPlane( leafRepair_t& r, const idPlane& plane, bool front )
{
	idDPlane p;
	p.FromPlane( plane );
	// the float normal is unit only to float precision: a square at its foot would lie off the plane far out
	const double scale = ( front ? 1.0 : -1.0 ) / sqrt( p.n[0] * p.n[0] + p.n[1] * p.n[1] + p.n[2] * p.n[2] );
	p.n[0] *= scale;
	p.n[1] *= scale;
	p.n[2] *= scale;
	p.d *= scale;
	r.path.Append( p );
}

static void LeafRepairPath( leafRepair_t& r, const node_t* leaf, const idPlane* extra )
{
	r.path.SetNum( 0 );
	if( extra != NULL )
	{
		LeafRepairAddPathPlane( r, *extra, true );
	}
	for( const uPortal_t* p = leaf->portals; p != NULL; p = p->next[ p->nodes[1] == leaf ] )
	{
		LeafRepairAddPathPlane( r, p->plane, p->nodes[0] == leaf );
	}
	const node_t* child = leaf;
	for( const node_t* n = leaf->parent; n != NULL; child = n, n = n->parent )
	{
		LeafRepairAddPathPlane( r, dmapGlobals.mapPlanes[n->planenum], n->children[0] == child );
	}
	for( int i = 0; i < 6; i++ )
	{
		r.path.Append( r.box[i] );
	}
}

static bool LeafRepairSamePlane( const idDPlane& a, const idDPlane& b )
{
	return a.n[0] == b.n[0] && a.n[1] == b.n[1] && a.n[2] == b.n[2] && a.d == b.d;
}

// whether the half spaces of planes share a volume: a square of half size MAX_WORLD_SIZE on each, clipped in double by
// all the others, keeps a winding (a clip that overflows idDWinding counts as sharing)
static bool LeafRepairEncloses( const idList<idDPlane>& planes )
{
	idDWinding w;
	for( int i = 0; i < planes.Num(); i++ )
	{
		w.BaseForPlane( planes[i], MAX_WORLD_SIZE );
		bool left = true;
		for( int j = 0; j < planes.Num() && left; j++ )
		{
			if( j != i && !LeafRepairSamePlane( planes[j], planes[i] ) )
			{
				left = w.Clip( planes[j], 0.0, false );
			}
		}
		if( w.Overflowed() || ( left && w.GetNumPoints() >= 3 ) )
		{
			return true;
		}
	}
	return false;
}

// the faces and corners of the leaf, in front of `extra` too when one is given: a square of half size MAX_WORLD_SIZE on
// every distinct plane of the path, clipped in double by all the others; the planes that keep a winding go to r.faces,
// its points to r.points. Neither is filled (the part then counts as held) when a clip overflows idDWinding, when a
// plane of the path meets its exact opposite (no volume), or when fewer than four planes keep a winding
static void LeafRepairCorners( leafRepair_t& r, const node_t* leaf, const idPlane* extra )
{
	LeafRepairPath( r, leaf, extra );
	r.faces.SetNum( 0 );
	r.points.SetNum( 0 );
	for( int i = 0; i < r.path.Num(); i++ )
	{
		const idDPlane flipped = -r.path[i];
		for( int j = i + 1; j < r.path.Num(); j++ )
		{
			if( LeafRepairSamePlane( r.path[j], flipped ) )
			{
				r.numFlatParts++;
				return;
			}
		}
	}
	bool reachedBox = false;
	idDWinding w;
	for( int i = 0; i < r.path.Num(); i++ )
	{
		int j;
		for( j = 0; j < i && !LeafRepairSamePlane( r.path[j], r.path[i] ); j++ )
		{
		}
		if( j < i )
		{
			continue;
		}
		w.BaseForPlane( r.path[i], MAX_WORLD_SIZE );
		bool left = true;
		for( j = 0; j < r.path.Num() && left; j++ )
		{
			if( !LeafRepairSamePlane( r.path[j], r.path[i] ) )
			{
				left = w.Clip( r.path[j], 0.0, false );
			}
		}
		if( w.Overflowed() )
		{
			r.faces.SetNum( 0 );
			r.points.SetNum( 0 );
			return;
		}
		if( !left || w.GetNumPoints() < 3 )
		{
			continue;
		}
		r.faces.Append( r.path[i] );
		for( int k = 0; k < w.GetNumPoints(); k++ )
		{
			r.points.Append( w[k][0] );
			r.points.Append( w[k][1] );
			r.points.Append( w[k][2] );
		}
		if( i >= r.path.Num() - 6 )
		{
			reachedBox = true;
		}
	}
	if( r.faces.Num() < 4 )
	{
		r.faces.SetNum( 0 );
		r.points.SetNum( 0 );
		r.numFewFaceParts++;
		return;
	}
	if( reachedBox )
	{
		r.numBoxParts++;
	}
}

static void LeafRepairAddBrush( leafRepair_t& r, const uEntity_t* e, const uBrush_t* b )
{
	const idMapBrush* mapBrush = NULL;
	if( e->mapEntity != NULL && b->brushnum >= 0 && b->brushnum < e->mapEntity->GetNumPrimitives() )
	{
		const idMapPrimitive* prim = e->mapEntity->GetPrimitive( b->brushnum );
		if( prim->GetType() == idMapPrimitive::TYPE_BRUSH )
		{
			mapBrush = static_cast<const idMapBrush*>( prim );
		}
	}
	const idBrushFrame world;	// centre at the origin: PlaneFromPointsDouble gives the world plane
	r.brushes.Append( b );
	r.firstPlane.Append( r.planes.Num() );
	for( int i = 0; i < b->numsides; i++ )
	{
		const side_t& s = b->sides[i];
		const idPlane& plane = dmapGlobals.mapPlanes[s.planenum];
		idDPlane dplane;
		bool exact = false;
		if( mapBrush != NULL && s.hasSourcePts && s.sourceSide >= 0 && s.sourceSide < mapBrush->GetNumSides() )
		{
			// the points as the map's text has them when dmap_mapPointsDouble is on (Phase 2C P2.4)
			const idMapBrushSide* mapSide = mapBrush->GetSide( s.sourceSide );
			double points[3][3];
			mapSide->GetPlanePoints( points, dmap_mapPointsDouble.GetBool() );
			exact = world.PlaneFromPointsDouble( points, mapSide->GetOrigin(), plane, dplane );
		}
		if( !exact )
		{
			dplane.FromPlane( plane );
			const double scale = 1.0 / sqrt( dplane.n[0] * dplane.n[0] + dplane.n[1] * dplane.n[1] + dplane.n[2] * dplane.n[2] );
			dplane.n[0] *= scale;
			dplane.n[1] *= scale;
			dplane.n[2] *= scale;
			dplane.d *= scale;
		}
		r.planes.Append( dplane );
	}
}

// whether the part (its corners in r.points, its faces in r.faces) and the brush pulled in by `inside` share a volume
static bool LeafRepairMeets( leafRepair_t& r, int brush, double inside )	//puncho - Phase 2C P2.5: the depth is the caller's (it was LEAF_REPAIR_INSIDE_EPSILON)
{
	const int first = r.firstPlane[brush];
	const int numSides = r.brushes[brush]->numsides;
	const int numPoints = r.points.Num() / 3;
	r.inner.SetNum( 0 );
	for( int i = 0; i < numSides; i++ )
	{
		// a side with every corner of the part beyond its pulled-in plane parts the two
		const idDPlane& side = r.planes[first + i];
		int j;
		for( j = 0; j < numPoints && side.Distance( &r.points[j * 3] ) > -inside; j++ )	//puncho - Phase 2C P2.5: was LEAF_REPAIR_INSIDE_EPSILON
		{
		}
		if( j == numPoints )
		{
			return false;
		}
		idDPlane in = -side;
		in.d -= inside;	//puncho - Phase 2C P2.5: was LEAF_REPAIR_INSIDE_EPSILON
		r.inner.Append( in );
	}
	r.both = r.faces;
	r.both.Append( r.inner );
	return LeafRepairEncloses( r.both );
}

//puncho start - Phase 2C P2.6
/*
==================
LeafRepairGridCandidates

LeafRepairHeld tested the bounds of every opaque brush of the entity for each part (Opus E MINOR 2: 0.13 to 0.16 ms a
repair on organic shells of 226 to 8,136 brushes, so brushes x repairs). Once the scans have made LEAF_REPAIR_GRID_AFTER
times as many bounds tests as the entity has brushes (an entity with a few repairs, as om64k's, never builds a grid),
the brushes go into a grid of cells about their mean size, each brush in every cell its bounds reach (a brush reaching
into more than LEAF_REPAIR_GRID_MAX_CELLS cells is taken for every part). A brush whose bounds meet the part's bounds
shares a point with them, and that point's cell lies in both cell ranges (the cell of a coordinate never decreases as
it grows), so the brushes of the cells the part's bounds reach, sorted, are a superset of the brushes the scan found,
in its order: LeafRepairHeld makes the same bounds test on each, and so the same LeafRepairMeets calls in the same
order. Returns their number, or -1 for a scan of every brush (before the grid, a small entity, or a part whose bounds
reach into more than an eighth of the cells).
==================
*/
#define LEAF_REPAIR_GRID_AFTER			32
#define LEAF_REPAIR_GRID_MIN_BRUSHES	64
#define LEAF_REPAIR_GRID_MAX_CELLS		64

// the grid cell of a coordinate on an axis
static int LeafRepairGridCell( const leafRepair_t& r, const int axis, const float x )
{
	const double c = floor( ( ( double )x - r.gridOrigin[axis] ) * r.gridScale );
	if( c < 0.0 )
	{
		return 0;
	}
	if( c >= ( double )r.gridSize[axis] )
	{
		return r.gridSize[axis] - 1;
	}
	return ( int )c;
}

// the cell ranges of bounds and how many cells they hold
static int64 LeafRepairGridRange( const leafRepair_t& r, const idBounds& bounds, int lo[3], int hi[3] )
{
	int64 cells = 1;
	for( int i = 0; i < 3; i++ )
	{
		lo[i] = LeafRepairGridCell( r, i, bounds[0][i] );
		hi[i] = LeafRepairGridCell( r, i, bounds[1][i] );
		cells *= ( hi[i] >= lo[i] ) ? ( hi[i] - lo[i] + 1 ) : 0;
	}
	return cells;
}

static void LeafRepairGridBuild( leafRepair_t& r )
{
	const int numBrushes = r.brushes.Num();
	idBounds all;
	all.Clear();
	double size = 0.0;
	int sized = 0;
	for( int b = 0; b < numBrushes; b++ )
	{
		const idBounds& bb = r.brushes[b]->bounds;
		if( bb[0][0] > bb[1][0] || bb[0][1] > bb[1][1] || bb[0][2] > bb[1][2] )
		{
			continue;	// cleared: in no cell (LeafRepairGridRange), so taken for every part
		}
		all.AddBounds( bb );
		size += Max( Max( bb[1][0] - bb[0][0], bb[1][1] - bb[0][1] ), bb[1][2] - bb[0][2] );
		sized++;
	}
	r.gridBuilt = true;
	if( sized == 0 )
	{
		r.gridCells = 0;	// every query scans
		return;
	}
	// cells of the brushes' mean size, larger where that would make more than four cells a brush
	double cell = Max( size / sized, 1.0 );
	for( ;; )
	{
		int64 cells = 1;
		for( int i = 0; i < 3; i++ )
		{
			r.gridSize[i] = ( int )Min( ( double )( all[1][i] - all[0][i] ) / cell + 1.0, 1048576.0 );
			cells *= r.gridSize[i];
		}
		if( cells <= 4 * ( int64 )numBrushes )
		{
			r.gridCells = ( int )cells;
			break;
		}
		cell *= 1.5;
	}
	for( int i = 0; i < 3; i++ )
	{
		r.gridOrigin[i] = all[0][i];
	}
	r.gridScale = 1.0 / cell;

	// count the brushes of each cell, then fill each cell's list in index order
	r.gridFirst.AssureSize( r.gridCells + 1, 0 );
	r.gridLarge.SetGranularity( 1024 );
	r.gridCandidates.SetGranularity( 256 );
	int lo[3], hi[3];
	for( int pass = 0; pass < 2; pass++ )
	{
		for( int b = 0; b < numBrushes; b++ )
		{
			// (bounds that reach into no cell, cleared ones, are taken for every part as well)
			const int64 cells = LeafRepairGridRange( r, r.brushes[b]->bounds, lo, hi );
			if( cells > LEAF_REPAIR_GRID_MAX_CELLS || cells == 0 )
			{
				if( pass == 0 )
				{
					r.gridLarge.Append( b );
				}
				continue;
			}
			for( int z = lo[2]; z <= hi[2]; z++ )
			{
				for( int y = lo[1]; y <= hi[1]; y++ )
				{
					for( int x = lo[0]; x <= hi[0]; x++ )
					{
						const int c = ( z * r.gridSize[1] + y ) * r.gridSize[0] + x;
						if( pass == 0 )
						{
							r.gridFirst[c + 1]++;
						}
						else
						{
							r.gridBrushes[r.gridFirst[c]++] = b;
						}
					}
				}
			}
		}
		if( pass == 0 )
		{
			for( int c = 0; c < r.gridCells; c++ )
			{
				r.gridFirst[c + 1] += r.gridFirst[c];
			}
			r.gridBrushes.SetNum( r.gridFirst[r.gridCells] );
		}
	}
	// the fill moved each cell's start to the next cell's: move them back
	for( int c = r.gridCells; c > 0; c-- )
	{
		r.gridFirst[c] = r.gridFirst[c - 1];
	}
	r.gridFirst[0] = 0;
	r.gridMark.AssureSize( numBrushes, 0 );
	r.gridQuery = 0;
}

static int LeafRepairGridCandidates( leafRepair_t& r, const idBounds& bounds )
{
	const int numBrushes = r.brushes.Num();
	if( !r.gridBuilt )
	{
		if( numBrushes < LEAF_REPAIR_GRID_MIN_BRUSHES || r.gridScanTests < LEAF_REPAIR_GRID_AFTER * ( int64 )numBrushes )
		{
			r.gridScanTests += numBrushes;
			return -1;
		}
		LeafRepairGridBuild( r );
	}
	int lo[3], hi[3];
	if( r.gridCells == 0 || LeafRepairGridRange( r, bounds, lo, hi ) > r.gridCells / 8 )
	{
		return -1;
	}
	r.gridQuery++;
	r.gridCandidates.SetNum( 0 );
	for( int z = lo[2]; z <= hi[2]; z++ )
	{
		for( int y = lo[1]; y <= hi[1]; y++ )
		{
			for( int x = lo[0]; x <= hi[0]; x++ )
			{
				const int c = ( z * r.gridSize[1] + y ) * r.gridSize[0] + x;
				for( int i = r.gridFirst[c]; i < r.gridFirst[c + 1]; i++ )
				{
					const int b = r.gridBrushes[i];
					if( r.gridMark[b] != r.gridQuery )
					{
						r.gridMark[b] = r.gridQuery;
						r.gridCandidates.Append( b );
					}
				}
			}
		}
	}
	r.gridCandidates.Append( r.gridLarge );
	r.gridCandidates.SortWithTemplate( idSort_QuickDefault<int>() );
	return r.gridCandidates.Num();
}
//puncho end

// whether an opaque brush of the entity, other than the originals of the fragments in skip, holds a point of the part
// whose corners r.points holds more than LEAF_REPAIR_INSIDE_EPSILON inside (a part without corners counts as held)
static bool LeafRepairHeld( leafRepair_t& r, const uBrush_t* skip )
{
	const int numPoints = r.points.Num() / 3;
	if( numPoints == 0 )
	{
		return true;
	}
	idBounds bounds;
	bounds.Clear();
	for( int i = 0; i < numPoints; i++ )
	{
		const double* p = &r.points[i * 3];
		bounds.AddPoint( idVec3( ( float )p[0], ( float )p[1], ( float )p[2] ) );
	}
	bounds.ExpandSelf( LEAF_REPAIR_CUT_EPSILON );

	//puncho start - Phase 2C P2.6: the grid's brushes for these bounds, in index order, or every brush
	const int numCandidates = LeafRepairGridCandidates( r, bounds );
	const int numTests = ( numCandidates < 0 ) ? r.brushes.Num() : numCandidates;
	for( int c = 0; c < numTests; c++ )
	{
		const int b = ( numCandidates < 0 ) ? c : r.gridCandidates[c];
		//puncho end
		if( !r.brushes[b]->bounds.IntersectsBounds( bounds ) )
		{
			continue;
		}
		const uBrush_t* f = skip;
		while( f != NULL && f->original != r.brushes[b] )
		{
			f = f->next;
		}
		if( f == NULL && LeafRepairMeets( r, b, LEAF_REPAIR_INSIDE_EPSILON ) )	//puncho - Phase 2C P2.5: the depth passed
		{
			return true;
		}
	}
	return false;
}

static void LeafRepair_r( leafRepair_t& r, node_t* node )
{
	if( node->planenum != PLANENUM_LEAF )
	{
		LeafRepair_r( r, node->children[0] );
		LeafRepair_r( r, node->children[1] );
		return;
	}
	if( !node->opaque || node->portals == NULL )
	{
		return;
	}
	idStaticList<int, LEAF_REPAIR_MAX_SIDES> sides;
	if( !LeafRepairCutSides( node, sides ) )
	{
		return;
	}

	// the first cutting side whose far part touches another leaf and holds no point of a brush without a fragment here
	int cut = -1;
	for( int i = 0; i < sides.Num() && cut < 0; i++ )
	{
		const idPlane& plane = dmapGlobals.mapPlanes[sides[i]];
		if( !LeafRepairTouches( node, plane ) )
		{
			continue;
		}
		LeafRepairCorners( r, node, &plane );
		if( !LeafRepairHeld( r, node->brushlist ) )
		{
			cut = sides[i] & ~1;
		}
	}
	if( cut < 0 )
	{
		r.numRefused++;
		return;
	}
	r.numSplits++;
	if( dmap_leafRepairPrint.GetBool() )
	{
		common->Printf( "REPAIR %i bounds %s %s cut %i\n", r.numSplits, node->bounds[0].ToString( 3 ), node->bounds[1].ToString( 3 ), cut );
	}

	// the leaf becomes a node on the side's plane, and its fragments go down again
	uBrush_t* list = node->brushlist;
	node->brushlist = NULL;
	node->opaque = false;
	node->planenum = cut;
	for( int i = 0; i < 2; i++ )
	{
		node->children[i] = AllocNode();
		node->children[i]->parent = node;
		node->children[i]->planenum = PLANENUM_LEAF;
		node->children[i]->bounds = node->bounds;
	}
	MakeTreePortals_r( node );
	uBrush_t* next;
	for( uBrush_t* b = list; b != NULL; b = next )
	{
		next = b->next;
		b->next = NULL;
		FilterBrushIntoTree_r( b, node );
	}

	for( int i = 0; i < 2; i++ )
	{
		node_t* child = node->children[i];
		if( !child->opaque )
		{
			LeafRepairCorners( r, child, NULL );
			if( LeafRepairHeld( r, NULL ) )
			{
				child->opaque = true;
				r.numKept++;
			}
		}
		if( dmap_leafRepairPrint.GetBool() )
		{
			common->Printf( "CHILD %i opaque %i bounds %s %s\n", i, child->opaque ? 1 : 0, child->bounds[0].ToString( 3 ), child->bounds[1].ToString( 3 ) );
			if( !child->opaque )
			{
				LeafRepairPath( r, child, NULL );
				for( int j = 0; j < r.path.Num(); j++ )
				{
					const idDPlane& p = r.path[j];
					common->Printf( "PATH %.17g %.17g %.17g %.17g\n", p.n[0], p.n[1], p.n[2], p.d );
				}
			}
		}
		LeafRepair_r( r, child );
	}
}

//puncho start - Phase 2C P2.5: the set-up is shared with the leak fill
static void LeafRepairInit( leafRepair_t& r, const uEntity_t* e )
{
	r.numSplits = 0;
	r.numRefused = 0;
	r.numKept = 0;
	r.numBoxParts = 0;
	r.numFlatParts = 0;
	r.numFewFaceParts = 0;
	r.gridBuilt = false;	//puncho - Phase 2C P2.6: the grid is built on demand (LeafRepairGridCandidates)
	r.gridScanTests = 0;
	r.gridCells = 0;
	r.gridQuery = 0;
	const idBounds& bounds = e->tree->bounds;
	for( int i = 0; i < 3; i++ )
	{
		idDPlane& low = r.box[i];
		idDPlane& high = r.box[i + 3];
		low.n[0] = low.n[1] = low.n[2] = 0.0;
		high.n[0] = high.n[1] = high.n[2] = 0.0;
		low.n[i] = 1.0;
		low.d = LEAF_REPAIR_BOX_MARGIN - ( double )bounds[0][i];
		high.n[i] = -1.0;
		high.d = LEAF_REPAIR_BOX_MARGIN + ( double )bounds[1][i];
	}

	// sized once: idList grows by its granularity, and appending every side of a large map one by one is quadratic
	int numBrushes = 0;
	int numSides = 0;
	for( const primitive_t* prim = e->primitives; prim != NULL; prim = prim->next )
	{
		if( prim->brush != NULL && prim->brush->opaque )
		{
			numBrushes++;
			numSides += prim->brush->numsides;
		}
	}
	r.brushes.Resize( numBrushes );
	r.firstPlane.Resize( numBrushes );
	r.planes.Resize( numSides );
	r.path.SetGranularity( 256 );
	r.faces.SetGranularity( 256 );
	r.points.SetGranularity( 1024 );
	r.inner.SetGranularity( 256 );
	r.both.SetGranularity( 256 );
	for( const primitive_t* prim = e->primitives; prim != NULL; prim = prim->next )
	{
		if( prim->brush != NULL && prim->brush->opaque )
		{
			LeafRepairAddBrush( r, e, prim->brush );
		}
	}
}

static void LeafRepair( uEntity_t* e )
{
	leafRepair_t r;
	LeafRepairInit( r, e );
	//puncho end
	LeafRepair_r( r, e->tree->headnode );
	if( r.numSplits > 0 || r.numRefused > 0 )
	{
		common->Printf( "leaf repair: entity %i, %i leafs split, %i children kept opaque, %i leafs left opaque\n", dmapGlobals.entityNum, r.numSplits, r.numKept, r.numRefused );
	}
	if( dmap_leafRepairPrint.GetBool() && ( r.numBoxParts > 0 || r.numFlatParts > 0 || r.numFewFaceParts > 0 ) )
	{
		common->Printf( "leaf repair parts: entity %i, %i reached the tree's box, %i flat, %i with fewer than four faces\n", dmapGlobals.entityNum, r.numBoxParts, r.numFlatParts, r.numFewFaceParts );
	}
}
//puncho end

//puncho start - Phase 2C P2.5: the leak fill
/*
==============================================================================

LEAK FILL

SplitBrush gives a brush wholly to one side of a plane it crosses by less than 0.1 or whose mid winding is tiny (fewer
than three edges longer than 0.2), and frees a piece of less than 1 unit^3. A fragment that earlier planes have cut thin
(a block plane, the planes of the tiles beside it) then loses a piece that reaches into the leaf beyond: on the organic
shells a wedge 0.2 thick and 55 long inside a tile (sp_s1_0, a tiny mid winding), a corner 0.41 deep of 0.015 units^3
(sh_s11_30k, the volume rule), the thin overlap of two tiles that split their shared face along different diagonals
(sh_s4_30k, both brushes cross the other's plane by less than 0.1). No fragment lands there, the leaf is air inside
solid, and a sealed shell leaks through it.

When the entities reach the outside, the leafs of the leak path (the chain LeakFile writes) are tried against the opaque
brushes whose fragments are in the leafs beside them. A leaf whose corners (the leaf repair's, exact in double) lie
within LEAF_REPAIR_INSIDE_EPSILON behind every side of such a brush (the map's planes in double) is marked opaque and
given a copy of the brush as its fragment. A leaf the brush holds a point of more than LEAK_FILL_DEEP_EPSILON inside
(the leaf repair's exact test) is split by a side of the brush, off its path, that its corners reach more than
LEAF_REPAIR_INSIDE_EPSILON beyond; both parts are tried again, that brush first. A brush with a side on the leaf's path
that has the leaf in front of it is not tried. The entities then flood again, until the map is sealed or no leaf of the
path was filled. A map that does not leak is not changed.

Filling every such leaf, on a leak path or not, was measured and not taken: where tiles overlap (so_s6_0, so_s2_0) a
face crossing a filled leaf's plane at a shallow angle loses its part within ON_EPSILON in front of that plane to the
filled leaf (ClipSideByTree_r), and see-through rays on those shells rose from 8 to 37.

==============================================================================
*/
// a leaf the brush holds a point of this far inside is split by the brush's sides: above the float error of a stored
// plane at 30k (about 0.004; the thin overlap of two tiles on sh_s4_30k is held 0.013 deep). At 64k that error (about
// 0.008) can exceed it: a leaf of a leak path that only touches a brush is then split along the brush's planes and the
// sliver behind them filled, which is the brush's own volume as the tree's planes bound it
#define LEAK_FILL_DEEP_EPSILON		0.005
// the brushes tried on one leaf
#define LEAK_FILL_MAX_BRUSHES		32
// floods after the first; each one follows a leak path that the passes before it left open
#define LEAK_FILL_MAX_PASSES		64

typedef struct leakFill_s
{
	leafRepair_t	r;			// the entity's opaque brushes, their planes, the repair's scratch lists
	idHashIndex		hash;		// r.brushes by brushnum
	int				numSplits;
	int				numFilled;
} leakFill_t;

// the index of an original brush in r.brushes, -1 when it is not there
static int LeakFillBrushIndex( const leakFill_t& f, const uBrush_t* b )
{
	for( int i = f.hash.First( b->brushnum ); i >= 0; i = f.hash.Next( i ) )
	{
		if( f.r.brushes[i] == b )
		{
			return i;
		}
	}
	return -1;
}

// `first` (when >= 0), then the brushes of the opaque fragments in the leafs beside the leaf
static void LeakFillCandidates( const leakFill_t& f, const node_t* leaf, int first, idStaticList<int, LEAK_FILL_MAX_BRUSHES>& brushes )
{
	brushes.Clear();
	if( first >= 0 )
	{
		brushes.Append( first );
	}
	for( const uPortal_t* p = leaf->portals; p != NULL; p = p->next[ p->nodes[1] == leaf ] )
	{
		const node_t* other = p->nodes[ p->nodes[0] == leaf ];
		for( const uBrush_t* b = other->brushlist; b != NULL; b = b->next )
		{
			if( !b->opaque || b->original == NULL )
			{
				continue;
			}
			const int i = LeakFillBrushIndex( f, b->original );
			if( i >= 0 && brushes.Num() < LEAK_FILL_MAX_BRUSHES && brushes.FindIndex( i ) < 0 )
			{
				brushes.Append( i );
			}
		}
	}
}

// whether a side of the brush is on the leaf's path with the leaf in front of it: the tree puts the leaf outside
static bool LeakFillOutside( const leafRepair_t& r, const node_t* leaf, int brush )
{
	const uBrush_t* b = r.brushes[brush];
	for( int i = 0; i < b->numsides; i++ )
	{
		const int planenum = b->sides[i].planenum;
		const node_t* child = leaf;
		for( const node_t* n = leaf->parent; n != NULL; child = n, n = n->parent )
		{
			if( ( n->planenum & ~1 ) == ( planenum & ~1 ) )
			{
				if( ( n->children[0] == child ) == ( n->planenum == planenum ) )
				{
					return true;
				}
				break;
			}
		}
	}
	return false;
}

static void LeakFill_r( leakFill_t& f, node_t* node, int first )
{
	if( node->planenum != PLANENUM_LEAF )
	{
		LeakFill_r( f, node->children[0], first );
		LeakFill_r( f, node->children[1], first );
		return;
	}
	if( node->opaque || node->portals == NULL )
	{
		return;
	}
	leafRepair_t& r = f.r;
	idStaticList<int, LEAK_FILL_MAX_BRUSHES> brushes;
	LeakFillCandidates( f, node, first, brushes );
	bool corners = false;
	for( int c = 0; c < brushes.Num(); c++ )
	{
		const int b = brushes[c];
		if( LeakFillOutside( r, node, b ) )
		{
			continue;
		}
		if( !corners )
		{
			// a leaf that cannot be bounded is left as it is
			LeafRepairCorners( r, node, NULL );
			corners = true;
			if( r.points.Num() == 0 )
			{
				return;
			}
		}

		// whether every corner lies within the epsilon behind every side, else the first side off the path that a
		// corner reaches more than that beyond
		const uBrush_t* brush = r.brushes[b];
		const int numPoints = r.points.Num() / 3;
		bool inside = true;
		int cut = -1;
		for( int i = 0; i < brush->numsides; i++ )
		{
			const idDPlane& side = r.planes[r.firstPlane[b] + i];
			int j;
			for( j = 0; j < numPoints && side.Distance( &r.points[j * 3] ) <= LEAF_REPAIR_INSIDE_EPSILON; j++ )
			{
			}
			if( j < numPoints )
			{
				inside = false;
				if( cut < 0 && !LeafRepairPlaneOnPath( node, brush->sides[i].planenum ) )
				{
					cut = brush->sides[i].planenum & ~1;
				}
			}
		}

		if( inside )
		{
			uBrush_t* fragment = CopyBrush( const_cast<uBrush_t*>( brush ) );
			fragment->next = node->brushlist;
			node->brushlist = fragment;
			node->opaque = true;
			f.numFilled++;
			return;
		}
		if( cut < 0 || !LeafRepairMeets( r, b, LEAK_FILL_DEEP_EPSILON ) )
		{
			continue;
		}
		// FilterMeshesIntoTree ran before the flood: a mesh areaportal's triangles would not reach the children
		if( node->areaPortalTris != NULL )
		{
			continue;
		}

		// the leaf becomes a node on the side's plane, its fragments go down again, and both parts are tried again
		f.numSplits++;
		uBrush_t* list = node->brushlist;
		node->brushlist = NULL;
		node->planenum = cut;
		for( int i = 0; i < 2; i++ )
		{
			node->children[i] = AllocNode();
			node->children[i]->parent = node;
			node->children[i]->planenum = PLANENUM_LEAF;
			node->children[i]->bounds = node->bounds;
		}
		MakeTreePortals_r( node );
		uBrush_t* next;
		for( uBrush_t* fb = list; fb != NULL; fb = next )
		{
			next = fb->next;
			fb->next = NULL;
			FilterBrushIntoTree_r( fb, node );
		}
		LeakFill_r( f, node->children[0], b );
		LeakFill_r( f, node->children[1], b );
		return;
	}
}

static void LeakFillClear_r( node_t* node )
{
	if( node->planenum != PLANENUM_LEAF )
	{
		LeakFillClear_r( node->children[0] );
		LeakFillClear_r( node->children[1] );
		return;
	}
	node->occupied = 0;
	node->occupant = NULL;
}

/*
==================
LeakFill

The entities have reached the outside: fill the leafs of the leak path that an opaque brush holds and flood again,
until the map is sealed or a path has no such leaf. Returns whether the map is sealed.
==================
*/
bool LeakFill( uEntity_t* e )
{
	tree_t* tree = e->tree;
	if( !tree->outside_node.occupied )
	{
		// no entity in the open: nothing leaked
		return false;
	}
	leakFill_t f;
	LeafRepairInit( f.r, e );
	f.numSplits = 0;
	f.numFilled = 0;
	f.hash.Clear( 4096, Max( f.r.brushes.Num(), 1 ) );
	for( int i = 0; i < f.r.brushes.Num(); i++ )
	{
		f.hash.Add( f.r.brushes[i]->brushnum, i );
	}

	bool enclosed = false;
	int floods = 0;
	idList<node_t*> path;
	for( int pass = 0; pass < LEAK_FILL_MAX_PASSES && !enclosed; pass++ )
	{
		// the leak path, as LeakFile follows it from the outside to an occupant
		path.SetNum( 0 );
		node_t* node = &tree->outside_node;
		while( node->occupied > 1 )
		{
			node_t* next = NULL;
			int best = node->occupied;
			const uPortal_t* p = node->portals;
			while( p != NULL )
			{
				const int s = ( p->nodes[0] == node );
				node_t* other = p->nodes[s];
				if( other->occupied && other->occupied < best )
				{
					next = other;
					best = other->occupied;
				}
				p = p->next[!s];
			}
			if( next == NULL )
			{
				break;
			}
			node = next;
			path.Append( node );
		}

		// an occupant's own leaf is left as it is: filled, it would put the entity in solid
		const int numFilled = f.numFilled;
		const int numSplits = f.numSplits;
		for( int i = 0; i < path.Num(); i++ )
		{
			if( path[i]->occupant == NULL )
			{
				LeakFill_r( f, path[i], -1 );
			}
		}
		if( f.numFilled == numFilled )
		{
			if( f.numSplits != numSplits )
			{
				// the leafs this pass split have no flood distances yet: flood again so that LeakFile follows the
				// whole path. A split re-files only non-opaque fragments, but SplitNodePortals drops tiny portal
				// pieces, so this flood can be the one that seals: its verdict is the answer
				LeakFillClear_r( tree->headnode );
				enclosed = FloodEntities( tree );
				floods++;
			}
			break;
		}
		LeakFillClear_r( tree->headnode );
		enclosed = FloodEntities( tree );
		floods++;
	}
	common->Printf( "leak fill: entity %i, %i floods, %i leafs split, %i leafs filled, %s\n", dmapGlobals.entityNum, floods, f.numSplits, f.numFilled, enclosed ? "sealed" : "still leaking" );
	return enclosed;
}
//puncho end

/*
=====================
FilterBrushesIntoTree

Mark the leafs as opaque and areaportals and put brush
fragments in each leaf so portal surfaces can be matched
to materials
=====================
*/
void FilterBrushesIntoTree( uEntity_t* e )
{
	primitive_t*			prim;
	uBrush_t*			b, *newb;
	int					r;
	int					c_unique, c_clusters;

	common->VerbosePrintf( "----- FilterBrushesIntoTree -----\n" );

	c_unique = 0;
	c_clusters = 0;
	for( prim = e->primitives ; prim ; prim = prim->next )
	{
		b = prim->brush;
		if( !b )
		{
			continue;
		}
		c_unique++;
		newb = CopyBrush( b );
		r = FilterBrushIntoTree_r( newb, e->tree->headnode );
		c_clusters += r;
	}

	//puncho start - Phase 2C P2.3: the leaf repair
	if( dmap_leafRepair.GetBool() )
	{
		LeafRepair( e );
	}
	//puncho end

	common->VerbosePrintf( "%5i total brushes\n", c_unique );
	common->VerbosePrintf( "%5i cluster references\n", c_clusters );
}



/*
================
AllocTree
================
*/
tree_t* AllocTree()
{
	tree_t*	tree;

	tree = ( tree_t* )Mem_Alloc( sizeof( *tree ), TAG_TOOLS );
	memset( tree, 0, sizeof( *tree ) );
	tree->bounds.Clear();

	return tree;
}

/*
================
AllocNode
================
*/
node_t* AllocNode()
{
	node_t*	node;

	node = ( node_t* )Mem_Alloc( sizeof( *node ), TAG_TOOLS );
	memset( node, 0, sizeof( *node ) );

	return node;
}

//============================================================

/*
==================
BrushMostlyOnSide

==================
*/
int BrushMostlyOnSide( uBrush_t* brush, idPlane& plane )
{
	int			i, j;
	idWinding*	w;
	float		d, max;
	int			side;

	max = 0;
	side = PSIDE_FRONT;
	for( i = 0; i < brush->numsides; i++ )
	{
		w = brush->sides[i].winding;
		if( !w )
		{
			continue;
		}
		for( j = 0; j < w->GetNumPoints(); j++ )
		{
			d = plane.Distance( ( *w )[j].ToVec3() );
			if( d > max )
			{
				max = d;
				side = PSIDE_FRONT;
			}
			if( -d > max )
			{
				max = -d;
				side = PSIDE_BACK;
			}
		}
	}
	return side;
}

/*
================
SplitBrush

Generates two new brushes, leaving the original
unchanged
================
*/
void SplitBrush( uBrush_t* brush, int planenum, uBrush_t** front, uBrush_t** back )
{
	uBrush_t*	b[2];
	int			i, j;
	idWinding*	w, *cw[2], *midwinding;
	side_t*		s, *cs;
	float		d, d_front, d_back;

	*front = *back = NULL;
	idPlane& plane = dmapGlobals.mapPlanes[planenum];

	// check all points
	d_front = d_back = 0;
	for( i = 0; i < brush->numsides; i++ )
	{
		w = brush->sides[i].winding;
		if( !w )
		{
			continue;
		}
		for( j = 0; j < w->GetNumPoints(); j++ )
		{
			d = plane.Distance( ( *w )[j].ToVec3() );
			if( d > 0 && d > d_front )
			{
				d_front = d;
			}
			if( d < 0 && d < d_back )
			{
				d_back = d;
			}
		}
	}
	if( d_front < 0.1 ) // PLANESIDE_EPSILON)
	{
		// only on back
		*back = CopyBrush( brush );
		return;
	}
	if( d_back > -0.1 ) // PLANESIDE_EPSILON)
	{
		// only on front
		*front = CopyBrush( brush );
		return;
	}

	// create a new winding from the split plane

	//puncho start - Phase 2C P2.1 (S5 in SplitBrush), Opus MAX review: from a 2^18 square the mid winding is off by up to
	// about ulp(2^18) times the conditioning, which misfiled thin-wedge fragments at the forced block splits (air leaves
	// marked opaque, whole face regions lost); here it is made in the brush's frame as CreateBrushWindingsLocal does
	if( dmap_brushBaseWindings.GetBool() && !brush->bounds.IsCleared() )
	{
		idBrushFrame frame;
		frame.FromBounds( brush->bounds );
		//puncho start - Phase 2C P2.2: the mid winding in double, the float clip below only if a clip overflows
		bool made = false;
		w = NULL;
		if( dmap_brushWindingsDouble.GetBool() )
		{
			idDWinding dw;
			dw.BaseForPlane( frame.MovePlaneDouble( plane ), frame.HalfSize() );
			bool left = true;
			for( i = 0; i < brush->numsides && left; i++ )
			{
				left = dw.Clip( frame.MovePlaneDouble( dmapGlobals.mapPlanes[brush->sides[i].planenum ^ 1] ), 0.0, false );
			}
			if( !dw.Overflowed() )
			{
				made = true;
				if( left )
				{
					w = dw.ToWinding( frame.Centre() );
					w->RemoveEqualPoints( 0.01f );	// as CreateBrushWindingsDouble
				}
			}
		}
		if( !made )
		{
		//puncho end
		w = new idWinding;
		w->BaseForPlane( frame.MovePlane( plane ), frame.HalfSize() );
		for( i = 0; i < brush->numsides && w; i++ )
		{
			w = w->Clip( frame.MovePlane( dmapGlobals.mapPlanes[brush->sides[i].planenum ^ 1] ), 0 );
		}
		if( w )
		{
			frame.ToWorld( *w );
		}
		}	//puncho - Phase 2C P2.2
	}
	else
	{
		w = new idWinding( plane );
		for( i = 0; i < brush->numsides && w; i++ )
		{
			idPlane& plane2 = dmapGlobals.mapPlanes[brush->sides[i].planenum ^ 1];
			w = w->Clip( plane2, 0 ); // PLANESIDE_EPSILON);
		}
	}
	//puncho end

	if( !w || w->IsTiny() )
	{
		// the brush isn't really split
		int		side;

		delete w;	//puncho - Phase 2C P2.1: a tiny mid winding was leaked here (stock)

		side = BrushMostlyOnSide( brush, plane );
		if( side == PSIDE_FRONT )
		{
			*front = CopyBrush( brush );
		}
		if( side == PSIDE_BACK )
		{
			*back = CopyBrush( brush );
		}
		return;
	}

	if( w->IsHuge() )
	{
		common->Printf( "WARNING: huge winding\n" );
	}

	midwinding = w;

	// split it for real

	for( i = 0; i < 2; i++ )
	{
		b[i] = AllocBrush( brush->numsides + 1 );
		memcpy( b[i], brush, sizeof( uBrush_t ) - sizeof( brush->sides ) );
		b[i]->numsides = 0;
		b[i]->next = NULL;
		b[i]->original = brush->original;
	}

	// split all the current windings

	for( i = 0; i < brush->numsides; i++ )
	{
		s = &brush->sides[i];
		w = s->winding;
		if( !w )
		{
			continue;
		}
		w->Split( plane, 0 /*PLANESIDE_EPSILON*/, &cw[0], &cw[1] );
		for( j = 0; j < 2; j++ )
		{
			if( !cw[j] )
			{
				continue;
			}
			/*
						if ( cw[j]->IsTiny() )
						{
							delete cw[j];
							continue;
						}
			*/
			cs = &b[j]->sides[b[j]->numsides];
			b[j]->numsides++;
			*cs = *s;
			cs->winding = cw[j];
		}
	}


	// see if we have valid polygons on both sides

	for( i = 0 ; i < 2 ; i++ )
	{
		if( !BoundBrush( b[i] ) )
		{
			break;
		}

		if( b[i]->numsides < 3 )
		{
			FreeBrush( b[i] );
			b[i] = NULL;
		}
	}

	if( !( b[0] && b[1] ) )
	{
		if( !b[0] && !b[1] )
		{
			common->Printf( "split removed brush\n" );
		}
		else
		{
			common->Printf( "split not on both sides\n" );
		}
		if( b[0] )
		{
			FreeBrush( b[0] );
			*front = CopyBrush( brush );
		}
		if( b[1] )
		{
			FreeBrush( b[1] );
			*back = CopyBrush( brush );
		}
		delete midwinding;	//puncho - Phase 2C P2.1: it belongs to no brush on this path and was leaked (stock)
		return;
	}

	// add the midwinding to both sides
	for( i = 0 ; i < 2 ; i++ )
	{
		cs = &b[i]->sides[b[i]->numsides];
		b[i]->numsides++;

		cs->planenum = planenum ^ i ^ 1;
		cs->material = NULL;
		if( i == 0 )
		{
			cs->winding = midwinding->Copy();
		}
		else
		{
			cs->winding = midwinding;
		}
	}

	{
		float	v1;
		int		i;

		for( i = 0 ; i < 2 ; i++ )
		{
			v1 = BrushVolume( b[i] );
			if( v1 < 1.0 )
			{
				FreeBrush( b[i] );
				b[i] = NULL;
//			common->Printf ("tiny volume after clip\n");
			}
		}
	}

	*front = b[0];
	*back = b[1];
}
