/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#ifndef __DWINDING_H__
#define __DWINDING_H__

//puncho start - Phase 2C P2.2: one double-precision winding builder for the map compilers
/*
===============================================================================

	A brush face made from a base square and clipped in float puts the copies of one corner, made on different
	sides, apart by about the float rounding of the larger coordinates times the conditioning of the corner: where two
	sides are near coplanar the edge between them moves by eps |q| / sin( angle ), which opens cracks in the render
	and holes in the collision model, and moves AAS faces with where the content sits.

	idDWinding repeats idWinding::BaseForPlane, Clip and Split in double: the same base square and corner order,
	the same classing against the plane at epsilon, keepOn, the same point order, the same base point for every
	crossing and the same axial round-off rule. Only ToWinding rounds, once per output coordinate. With axial planes
	and integer distances every step is exact in float and in double, so such input keeps its float bytes.

	Planes are n.p + d = 0 (idPlane's convention: d = -Dist()). An idDWinding holds its points in place (about 6 KB)
	and Clip uses about 8 KB of stack, Split about 2 KB besides its two pieces; nothing is allocated and nothing is
	static. A clip that would need more than MAX_POINTS points leaves the winding as it was and sets Overflowed(): the
	caller then takes its float path.

===============================================================================
*/

class idDPlane
{
public:
	double			n[3];
	double			d;

	// the float plane widened exactly
	void			FromPlane( const idPlane& plane );
	// the plane rounded to float, component by component
	idPlane			ToPlane() const;
	// n.p + d, summed as idPlane::Distance sums: ( n0 p0 + n1 p1 ) + n2 p2, then + d
	double			Distance( const double p[3] ) const;
	idDPlane		operator-() const;
	// the plane for p = centre + q: d + n.centre in double
	idDPlane		Moved( const idVec3& centre ) const;
};

class idDWinding
{
public:
	static const int	MAX_POINTS = 256;

					idDWinding();

	int				GetNumPoints() const;
	bool			Overflowed() const;
	const double* 	operator[]( const int index ) const;

	// idWinding::BaseForPlane( normal, dist, halfSize ) in double: the square of half size halfSize at the plane's
	// foot, its corners in BaseForPlane's order (the points go counter clockwise seen from the plane's front)
	void			BaseForPlane( const idDPlane& plane, const double halfSize );
	// the points of a float winding less centre
	void			FromWinding( const idWinding& w, const idVec3& centre );
	// idWinding::Clip( plane, epsilon, keepOn ) in double: keeps the part in front of the plane. Returns false, the
	// winding then empty, when nothing is left. On overflow the winding is left as it was and true is returned
	bool			Clip( const idDPlane& plane, const double epsilon, const bool keepOn );
	// idWinding::Split( plane, epsilon, &front, &back ) in double: returns SIDE_FRONT (front alone holds the winding),
	// SIDE_BACK (back alone) or SIDE_CROSS (both); the other is left empty. A winding within epsilon of the plane goes to
	// the side its own normal faces (idWinding::GetPlane's normal, ( p1 - centre ) x ( p0 - centre )); each crossing is
	// made from the edge's front point. On overflow front.Overflowed() is set and the pieces are unusable
	int				Split( const idDPlane& plane, const double epsilon, idDWinding& front, idDWinding& back ) const;
	// a new idWinding with the points centre + q, each coordinate rounded once; s and t 0
	idWinding* 		ToWinding( const idVec3& centre ) const;
	// the same into a fixed winding (cleared first)
	void			ToFixedWinding( const idVec3& centre, idFixedWinding& w ) const;

private:
	int				numPoints;
	bool			overflow;
	double			p[MAX_POINTS][3];
};

ID_INLINE void idDPlane::FromPlane( const idPlane& plane )
{
	n[0] = plane[0];
	n[1] = plane[1];
	n[2] = plane[2];
	d = plane[3];
}

ID_INLINE idPlane idDPlane::ToPlane() const
{
	return idPlane( ( float )n[0], ( float )n[1], ( float )n[2], ( float )d );
}

ID_INLINE double idDPlane::Distance( const double p[3] ) const
{
	return ( ( n[0] * p[0] + n[1] * p[1] ) + n[2] * p[2] ) + d;
}

ID_INLINE idDPlane idDPlane::operator-() const
{
	idDPlane o;
	o.n[0] = -n[0];
	o.n[1] = -n[1];
	o.n[2] = -n[2];
	o.d = -d;
	return o;
}

ID_INLINE idDPlane idDPlane::Moved( const idVec3& centre ) const
{
	idDPlane o = *this;
	o.d = d + ( ( n[0] * ( double )centre[0] + n[1] * ( double )centre[1] ) + n[2] * ( double )centre[2] );
	return o;
}

ID_INLINE idDWinding::idDWinding()
{
	numPoints = 0;
	overflow = false;
}

ID_INLINE int idDWinding::GetNumPoints() const
{
	return numPoints;
}

ID_INLINE bool idDWinding::Overflowed() const
{
	return overflow;
}

ID_INLINE const double* idDWinding::operator[]( const int index ) const
{
	return p[index];
}
//puncho end

#endif /* !__DWINDING_H__ */
