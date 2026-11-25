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

#ifndef __BRUSHFRAME_H__
#define __BRUSHFRAME_H__

//puncho start - Phase 2C P2.1: the brush-local frame rule of dmap (S5, S5b, SplitBrush) and the collision model (F1b)
/*
===============================================================================

	A brush winding made from a 2^18 square at the plane's foot and clipped in float at world coordinates puts the
	copies of one corner, made on different sides, up to about ulp(2^18) times the conditioning apart: past dmap's
	1/32 weld on steep or sloped brushes. In the brush's local frame each winding starts from a square sized to the
	brush, at the projection of the brush's rounded centre c, and is clipped with the planes re-expressed for
	p = c + q, the large terms summed in double; only the final c + q rounds at world scale.

	The rule:
	- c is the centre of the brush's bounds with each component rounded to an integer;
	- the base square's half size is 64 doubled while it is below the bounds' diagonal (and below MAX_WORLD_SIZE): a
	  face point lies within diagonal / 2 + 0.87 of the local foot, inside the square's inscribed circle;
	- a plane moves to the frame by d' = d + n.c in double (MovePlane);
	- a non-axial side whose three map points still define its stored float plane gets its local plane from the
	  points in double (PlaneFromPoints): the stored plane is off by an ulp of its distance far from the origin.

===============================================================================
*/

// a side's map points define its stored float plane when each lies within this of it. dmap stores a side as a
// neighbouring plane of its plane set when the two agree within 1e-5 in each normal component and 0.01 in distance, and
// the float plane is off by a few ulps of its distance: 0.0101 and 0.0116 were measured at 28k on rotated content, where
// 0.01 refused 4 sides. Beyond about 32k the ulps double and sides may be refused again (a refused side keeps the stored
// plane, moved to the frame). The caller passes as the offset whatever
// was taken off the points before the stored plane was made (an entity's origin, a collision model's origin offset)
#define BRUSHFRAME_SOURCE_PLANE_EPSILON		0.02f

class idBrushFrame
{
public:
					idBrushFrame();

	// the frame of a brush with these bounds
	void			FromBounds( const idBounds& bounds );

	const idVec3& 	Centre() const;
	float			HalfSize() const;

	// the plane for p = c + q
	idPlane			MovePlane( const idPlane& plane ) const;
	// the local plane through the three points ( p - offset ), as idPlane::FromPoints makes it but in double, when the
	// plane is non-axial and each point lies within BRUSHFRAME_SOURCE_PLANE_EPSILON of it; false, leaving `local` unset,
	// otherwise (an axial plane, points that do not define the plane, collinear points)
	bool			PlaneFromPoints( const idVec3 points[3], const idVec3& offset, const idPlane& plane, idPlane& local ) const;
	// the same plane kept in double (Phase 2C P2.2): PlaneFromPoints is this rounded component by component
	bool			PlaneFromPointsDouble( const idVec3 points[3], const idVec3& offset, const idPlane& plane, idDPlane& local ) const;	//puncho - Phase 2C P2.2
	// MovePlane kept in double (Phase 2C P2.2): the float plane widened, then d + n.c
	idDPlane		MovePlaneDouble( const idPlane& plane ) const;	//puncho - Phase 2C P2.2
	// adds c back to every point of a winding made in the frame
	void			ToWorld( idWinding& w ) const;

private:
	idVec3			centre;
	float			halfSize;
};

ID_INLINE idBrushFrame::idBrushFrame()
{
	centre = vec3_origin;
	halfSize = 0.0f;
}

ID_INLINE void idBrushFrame::FromBounds( const idBounds& bounds )
{
	centre = bounds.GetCenter();
	for( int i = 0; i < 3; i++ )
	{
		centre[i] = idMath::Rint( centre[i] );
	}
	const float diagonal = ( bounds[1] - bounds[0] ).Length();
	halfSize = 64.0f;
	while( halfSize < diagonal && halfSize < MAX_WORLD_SIZE )
	{
		halfSize *= 2.0f;
	}
}

ID_INLINE const idVec3& idBrushFrame::Centre() const
{
	return centre;
}

ID_INLINE float idBrushFrame::HalfSize() const
{
	return halfSize;
}

ID_INLINE idPlane idBrushFrame::MovePlane( const idPlane& plane ) const
{
	const double dot = ( ( double )plane[0] * ( double )centre[0] + ( double )plane[1] * ( double )centre[1] ) + ( double )plane[2] * ( double )centre[2];
	idPlane local = plane;
	local[3] = ( float )( ( double )plane[3] + dot );
	return local;
}

ID_INLINE bool idBrushFrame::PlaneFromPoints( const idVec3 points[3], const idVec3& offset, const idPlane& plane, idPlane& local ) const
{
	//puncho start - Phase 2C P2.2: the double rule, rounded once per component (the same arithmetic as before)
	idDPlane dlocal;
	if( !PlaneFromPointsDouble( points, offset, plane, dlocal ) )
	{
		return false;
	}
	local = dlocal.ToPlane();
	return true;
	//puncho end
}

//puncho start - Phase 2C P2.2
ID_INLINE idDPlane idBrushFrame::MovePlaneDouble( const idPlane& plane ) const
{
	idDPlane local;
	local.FromPlane( plane );
	return local.Moved( centre );
}
//puncho end

ID_INLINE bool idBrushFrame::PlaneFromPointsDouble( const idVec3 points[3], const idVec3& offset, const idPlane& plane, idDPlane& local ) const	//puncho - Phase 2C P2.2: was PlaneFromPoints
{
	if( plane.Type() < PLANETYPE_TRUEAXIAL )
	{
		return false;
	}
	double p[3][3];
	for( int k = 0; k < 3; k++ )
	{
		const double x = ( double )points[k][0] - ( double )offset[0];
		const double y = ( double )points[k][1] - ( double )offset[1];
		const double z = ( double )points[k][2] - ( double )offset[2];
		const double dist = ( ( double )plane[0] * x + ( double )plane[1] * y ) + ( double )plane[2] * z + ( double )plane[3];
		if( idMath::Fabs( ( float )dist ) > BRUSHFRAME_SOURCE_PLANE_EPSILON )
		{
			return false;
		}
		p[k][0] = x - ( double )centre[0];
		p[k][1] = y - ( double )centre[1];
		p[k][2] = z - ( double )centre[2];
	}
	// idPlane::FromPoints( p0, p1, p2 ): normal = ( p0 - p1 ) x ( p2 - p1 ), d = -normal . p1
	const double u[3] = { p[0][0] - p[1][0], p[0][1] - p[1][1], p[0][2] - p[1][2] };
	const double v[3] = { p[2][0] - p[1][0], p[2][1] - p[1][1], p[2][2] - p[1][2] };
	double n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
	const double length = sqrt( n[0] * n[0] + n[1] * n[1] + n[2] * n[2] );
	if( length < 1e-6 )
	{
		return false;
	}
	n[0] /= length;
	n[1] /= length;
	n[2] /= length;
	if( n[0] * plane[0] + n[1] * plane[1] + n[2] * plane[2] < 0.99999 )
	{
		return false;
	}
	local.n[0] = n[0];	//puncho - Phase 2C P2.2: kept in double
	local.n[1] = n[1];
	local.n[2] = n[2];
	local.d = -( n[0] * p[1][0] + n[1] * p[1][1] + n[2] * p[1][2] );
	return true;
}

ID_INLINE void idBrushFrame::ToWorld( idWinding& w ) const
{
	for( int i = 0; i < w.GetNumPoints(); i++ )
	{
		w[i].ToVec3() = centre + w[i].ToVec3();
	}
}
//puncho end

#endif /* !__BRUSHFRAME_H__ */
