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

#include "precompiled.h"
#pragma hdrstop

//puncho start - Phase 2C P2.2: the double winding builder (DWinding.h)

/*
=============
idDWinding::BaseForPlane

idWinding::BaseForPlane with idVec3::NormalVectors, in double
=============
*/
void idDWinding::BaseForPlane( const idDPlane& plane, const double halfSize )
{
	const double dist = -plane.d;
	const double org[3] = { plane.n[0] * dist, plane.n[1] * dist, plane.n[2] * dist };

	// idVec3::NormalVectors( left, down ): left is the up vector, down the right vector of BaseForPlane
	double left[3];
	double xy = plane.n[0] * plane.n[0] + plane.n[1] * plane.n[1];
	if( xy == 0.0 )
	{
		left[0] = 1.0;
		left[1] = 0.0;
		left[2] = 0.0;
	}
	else
	{
		xy = 1.0 / sqrt( xy );
		left[0] = -plane.n[1] * xy;
		left[1] = plane.n[0] * xy;
		left[2] = 0.0;
	}
	// down = left x normal
	const double down[3] =
	{
		left[1] * plane.n[2] - left[2] * plane.n[1],
		left[2] * plane.n[0] - left[0] * plane.n[2],
		left[0] * plane.n[1] - left[1] * plane.n[0]
	};

	numPoints = 4;
	overflow = false;
	for( int j = 0; j < 3; j++ )
	{
		const double vup = left[j] * halfSize;
		const double vright = down[j] * halfSize;
		p[0][j] = ( org[j] - vright ) + vup;
		p[1][j] = ( org[j] + vright ) + vup;
		p[2][j] = ( org[j] + vright ) - vup;
		p[3][j] = ( org[j] - vright ) - vup;
	}
}

/*
=============
idDWinding::FromWinding
=============
*/
void idDWinding::FromWinding( const idWinding& w, const idVec3& centre )
{
	overflow = false;
	numPoints = w.GetNumPoints();
	if( numPoints > MAX_POINTS )
	{
		overflow = true;
		numPoints = 0;
		return;
	}
	for( int i = 0; i < numPoints; i++ )
	{
		for( int j = 0; j < 3; j++ )
		{
			p[i][j] = ( double )w[i][j] - ( double )centre[j];
		}
	}
}

/*
=============
idDWinding::Clip

idWinding::Clip in double, point for point
=============
*/
bool idDWinding::Clip( const idDPlane& plane, const double epsilon, const bool keepOn )
{
	double dists[MAX_POINTS + 4];
	byte sides[MAX_POINTS + 4];
	int counts[3] = { 0, 0, 0 };
	int i;

	for( i = 0; i < numPoints; i++ )
	{
		const double dot = plane.Distance( p[i] );
		dists[i] = dot;
		if( dot > epsilon )
		{
			sides[i] = SIDE_FRONT;
		}
		else if( dot < -epsilon )
		{
			sides[i] = SIDE_BACK;
		}
		else
		{
			sides[i] = SIDE_ON;
		}
		counts[sides[i]]++;
	}
	sides[i] = sides[0];
	dists[i] = dists[0];

	// if the winding is on the plane and we should keep it
	if( keepOn && !counts[SIDE_FRONT] && !counts[SIDE_BACK] )
	{
		return true;
	}
	// if nothing at the front of the clipping plane
	if( !counts[SIDE_FRONT] )
	{
		numPoints = 0;
		return false;
	}
	// if nothing at the back of the clipping plane
	if( !counts[SIDE_BACK] )
	{
		return true;
	}

	// idWinding::Clip's bound: the input's point count + 4
	const int maxpts = Min( numPoints + 4, ( int )MAX_POINTS );
	double newPoints[MAX_POINTS][3];
	int newNumPoints = 0;

	for( i = 0; i < numPoints; i++ )
	{
		const double* p1 = p[i];

		if( newNumPoints + 1 > maxpts )
		{
			overflow = true;
			return true;		// can't split -- fall back to original
		}

		if( sides[i] == SIDE_ON )
		{
			newPoints[newNumPoints][0] = p1[0];
			newPoints[newNumPoints][1] = p1[1];
			newPoints[newNumPoints][2] = p1[2];
			newNumPoints++;
			continue;
		}

		if( sides[i] == SIDE_FRONT )
		{
			newPoints[newNumPoints][0] = p1[0];
			newPoints[newNumPoints][1] = p1[1];
			newPoints[newNumPoints][2] = p1[2];
			newNumPoints++;
		}

		if( sides[i + 1] == SIDE_ON || sides[i + 1] == sides[i] )
		{
			continue;
		}

		if( newNumPoints + 1 > maxpts )
		{
			overflow = true;
			return true;		// can't split -- fall back to original
		}

		// generate a split point
		const double* p2 = p[( i + 1 ) % numPoints];
		const double dot = dists[i] / ( dists[i] - dists[i + 1] );
		for( int j = 0; j < 3; j++ )
		{
			// avoid round off error when possible
			if( plane.n[j] == 1.0 )
			{
				newPoints[newNumPoints][j] = -plane.d;
			}
			else if( plane.n[j] == -1.0 )
			{
				newPoints[newNumPoints][j] = plane.d;
			}
			else
			{
				newPoints[newNumPoints][j] = p1[j] + dot * ( p2[j] - p1[j] );
			}
		}
		newNumPoints++;
	}

	numPoints = newNumPoints;
	memcpy( p, newPoints, newNumPoints * sizeof( p[0] ) );
	return true;
}

/*
=============
idDWinding::Split

idWinding::Split in double, point for point
=============
*/
int idDWinding::Split( const idDPlane& plane, const double epsilon, idDWinding& front, idDWinding& back ) const
{
	double dists[MAX_POINTS + 4];
	byte sides[MAX_POINTS + 4];
	int counts[3] = { 0, 0, 0 };
	int i;

	front.numPoints = 0;
	front.overflow = false;
	back.numPoints = 0;
	back.overflow = false;

	// determine sides for each point
	for( i = 0; i < numPoints; i++ )
	{
		const double dot = plane.Distance( p[i] );
		dists[i] = dot;
		if( dot > epsilon )
		{
			sides[i] = SIDE_FRONT;
		}
		else if( dot < -epsilon )
		{
			sides[i] = SIDE_BACK;
		}
		else
		{
			sides[i] = SIDE_ON;
		}
		counts[sides[i]]++;
	}
	sides[i] = sides[0];
	dists[i] = dists[0];

	// if coplanar, put on the front side if the normals match
	if( !counts[SIDE_FRONT] && !counts[SIDE_BACK] )
	{
		double facing = 0.0;
		if( numPoints >= 3 )
		{
			double centre[3] = { 0.0, 0.0, 0.0 };
			for( i = 0; i < numPoints; i++ )
			{
				centre[0] += p[i][0];
				centre[1] += p[i][1];
				centre[2] += p[i][2];
			}
			const double scale = 1.0 / numPoints;
			const double v1[3] = { p[0][0] - centre[0] * scale, p[0][1] - centre[1] * scale, p[0][2] - centre[2] * scale };
			const double v2[3] = { p[1][0] - centre[0] * scale, p[1][1] - centre[1] * scale, p[1][2] - centre[2] * scale };
			// the sign of ( v2 x v1 ) . n
			facing = ( v2[1] * v1[2] - v2[2] * v1[1] ) * plane.n[0] + ( v2[2] * v1[0] - v2[0] * v1[2] ) * plane.n[1] + ( v2[0] * v1[1] - v2[1] * v1[0] ) * plane.n[2];
		}
		idDWinding& piece = ( facing > 0.0 ) ? front : back;
		piece.numPoints = numPoints;
		memcpy( piece.p, p, numPoints * sizeof( p[0] ) );
		return ( facing > 0.0 ) ? SIDE_FRONT : SIDE_BACK;
	}
	// if nothing at the front of the clipping plane
	if( !counts[SIDE_FRONT] )
	{
		back.numPoints = numPoints;
		memcpy( back.p, p, numPoints * sizeof( p[0] ) );
		return SIDE_BACK;
	}
	// if nothing at the back of the clipping plane
	if( !counts[SIDE_BACK] )
	{
		front.numPoints = numPoints;
		memcpy( front.p, p, numPoints * sizeof( p[0] ) );
		return SIDE_FRONT;
	}

	for( i = 0; i < numPoints; i++ )
	{
		const double* p1 = p[i];

		// a point and a crossing at most go to each piece in one step
		if( front.numPoints + 2 > MAX_POINTS || back.numPoints + 2 > MAX_POINTS )
		{
			front.overflow = true;
			return SIDE_CROSS;
		}

		if( sides[i] == SIDE_ON )
		{
			memcpy( front.p[front.numPoints++], p1, sizeof( p[0] ) );
			memcpy( back.p[back.numPoints++], p1, sizeof( p[0] ) );
			continue;
		}

		if( sides[i] == SIDE_FRONT )
		{
			memcpy( front.p[front.numPoints++], p1, sizeof( p[0] ) );
		}

		if( sides[i] == SIDE_BACK )
		{
			memcpy( back.p[back.numPoints++], p1, sizeof( p[0] ) );
		}

		if( sides[i + 1] == SIDE_ON || sides[i + 1] == sides[i] )
		{
			continue;
		}

		// generate a split point, always from the front point of the edge
		const double* p2 = p[( i + 1 ) % numPoints];
		const double* from = p1;
		const double* to = p2;
		double dot;
		if( sides[i] == SIDE_FRONT )
		{
			dot = dists[i] / ( dists[i] - dists[i + 1] );
		}
		else
		{
			from = p2;
			to = p1;
			dot = dists[i + 1] / ( dists[i + 1] - dists[i] );
		}
		double* mid = front.p[front.numPoints];
		for( int j = 0; j < 3; j++ )
		{
			// avoid round off error when possible
			if( plane.n[j] == 1.0 )
			{
				mid[j] = -plane.d;
			}
			else if( plane.n[j] == -1.0 )
			{
				mid[j] = plane.d;
			}
			else
			{
				mid[j] = from[j] + dot * ( to[j] - from[j] );
			}
		}
		memcpy( back.p[back.numPoints], mid, sizeof( p[0] ) );
		front.numPoints++;
		back.numPoints++;
	}

	return SIDE_CROSS;
}

/*
=============
idDWinding::ToWinding
=============
*/
idWinding* idDWinding::ToWinding( const idVec3& centre ) const
{
	idWinding* w = new( TAG_IDLIB_WINDING ) idWinding( numPoints );
	for( int i = 0; i < numPoints; i++ )
	{
		idVec5 v;
		v.x = ( float )( ( double )centre[0] + p[i][0] );
		v.y = ( float )( ( double )centre[1] + p[i][1] );
		v.z = ( float )( ( double )centre[2] + p[i][2] );
		v.s = 0.0f;
		v.t = 0.0f;
		w->AddPoint( v );
	}
	return w;
}

/*
=============
idDWinding::ToFixedWinding
=============
*/
void idDWinding::ToFixedWinding( const idVec3& centre, idFixedWinding& w ) const
{
	w.Clear();
	for( int i = 0; i < numPoints; i++ )
	{
		w.AddPoint( idVec3( ( float )( ( double )centre[0] + p[i][0] ), ( float )( ( double )centre[1] + p[i][1] ), ( float )( ( double )centre[2] + p[i][2] ) ) );
	}
}
//puncho end
