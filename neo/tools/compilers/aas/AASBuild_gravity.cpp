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

#include "AASBuild_local.h"


/*
============
idAASBuild::SetPortalFlags_r
============
*/
void idAASBuild::SetPortalFlags_r( idBrushBSPNode* node )
{
	int s;
	idBrushBSPPortal* p;
	idVec3 normal;

	if( !node )
	{
		return;
	}

	if( node->GetContents() & AREACONTENTS_SOLID )
	{
		return;
	}

	if( !node->GetChild( 0 ) && !node->GetChild( 1 ) )
	{
		for( p = node->GetPortals(); p; p = p->Next( s ) )
		{
			s = ( p->GetNode( 1 ) == node );

			// if solid at the other side of the portal
			if( p->GetNode( !s )->GetContents() & AREACONTENTS_SOLID )
			{
				if( s )
				{
					normal = -p->GetPlane().Normal();
				}
				else
				{
					normal = p->GetPlane().Normal();
				}
				if( normal * aasSettings->invGravityDir > aasSettings->minFloorCos )
				{
					p->SetFlag( FACE_FLOOR );
				}
				else
				{
					p->SetFlag( FACE_SOLID );
				}
			}
		}
		return;
	}

	SetPortalFlags_r( node->GetChild( 0 ) );
	SetPortalFlags_r( node->GetChild( 1 ) );
}

/*
============
idAASBuild::PortalIsGap
============
*/
bool idAASBuild::PortalIsGap( idBrushBSPPortal* portal, int side )
{
	idVec3 normal;

	// if solid at the other side of the portal
	if( portal->GetNode( !side )->GetContents() & AREACONTENTS_SOLID )
	{
		return false;
	}

	if( side )
	{
		normal = -( portal->GetPlane().Normal() );
	}
	else
	{
		normal = portal->GetPlane().Normal();
	}
	if( normal * aasSettings->invGravityDir > aasSettings->minFloorCos )
	{
		return true;
	}
	return false;
}

/*
============
idAASBuild::GravSubdivLeafNode
============
*/
#define FACE_CHECKED			BIT(31)
#define GRAVSUBDIV_EPSILON		0.1f

//puncho start - Phase 2C P2.2: gravitational edge planes from the planes that meet at the edge
idCVar aas_gravityEdgePlanes( "aas_gravityEdgePlanes", "1", CVAR_TOOL | CVAR_BOOL, "1 = a gravitational split plane through a non-axial gap edge takes the edge direction from the two portal planes that meet there, crossed in double, the same wherever the content sits; 0 = from the edge's float vertices as in v0001.7k" );

/*
============
GravEdgeNormal

The normal of the gravitational plane through the gap portal's edge v1 -> v2, with the edge direction taken from the
planes that meet there: the gap portal's plane crossed in double with the plane, at the largest angle to it, of a
portal of either node the gap portal separates that holds both ends within GRAVSUBDIV_EPSILON and meets the gap plane
along the edge (see the test below). Those planes come from
the plane set and do not change with where the content sits. The difference of the edge's float vertices carries
their rounding: at 28k a short edge's plane tilts by about 5e-5 rad, five times the plane set's 1e-5 normal epsilon,
so another gravitational plane is chosen and the leaves merge differently (P2.2 plan D, 66 maps, with
aas_brushWindingsDouble: the mean aas48 area spread over copies at 0, 28k and 30k fell from 3.16 to 0.90 % for integer
content and from 4.91 to 4.29 % for rotated, the far copies' bias from +2.7 to -0.2 % and from +3.8 to +3.0 %).
An axis-parallel edge keeps the float normal: its direction is exact, and idVec3::Normalize leaves 0.99999994 for some
lengths where double gives 1, which would change all-axial maps. So does an edge no portal plane meets at an angle
along the edge.
============
*/
static void GravEdgeNormal( const idBrushBSPPortal* gap, const idVec3& v1, const idVec3& v2, const idVec3& invGravityDir, idVec3& normal )
{
	if( ( v1[0] == v2[0] && v1[1] == v2[1] ) || ( v1[0] == v2[0] && v1[2] == v2[2] ) || ( v1[1] == v2[1] && v1[2] == v2[2] ) )
	{
		return;
	}

	const idPlane& gapPlane = gap->GetPlane();
	double dir[3] = { 0.0, 0.0, 0.0 };
	double bestLength = 0.0;
	//puncho start - Phase 2C P2.2 (Opus C review): a plane counts only if it meets the gap plane along the edge. Holding
	// both ends within GRAVSUBDIV_EPSILON is not enough on a short edge: a portal plane through one end that crosses the
	// edge passes too, and as the plane at the largest angle it won. Its line ran up to 90 degrees off the edge, and its
	// gravitational plane cut the gap winding it was made to bound (66 maps, runAAS on v0001.7k's .proc: 3,464 planes
	// more than sin 0.1 off the float edge, 1,639 gap windings cut; the float normal cuts none). The line must now run
	// within the float edge's own error of it: each end is rounded once to float (half an ulp, at most 2^-24 of the
	// largest coordinate, per component), so the float edge's direction is off by at most sqrt( 3 ) 2^-23 M / length;
	// twice that, plus 1e-4 for the float planes' own normals crossed at 1e-3 rad or more. When no plane is that close,
	// the plane through the float edge stays. Measured on the 66 maps: 0 gap windings cut, the largest accepted
	// deviation sin 0.042, the float normal kept on 2,303 of 716,342 non-axial edges; mean aas48 area spread over the
	// copies at 0, 28k and 30k 0.90 -> 0.86 % (integer), 4.29 -> 4.27 % (rotated)
	const double edge[3] = { ( double )v2[0] - v1[0], ( double )v2[1] - v1[1], ( double )v2[2] - v1[2] };
	const double edgeLength = sqrt( edge[0] * edge[0] + edge[1] * edge[1] + edge[2] * edge[2] );
	double largest = 0.0;
	for( int k = 0; k < 3; k++ )
	{
		largest = Max( largest, ( double )idMath::Fabs( v1[k] ) );
		largest = Max( largest, ( double )idMath::Fabs( v2[k] ) );
	}
	const double maxSin = 1e-4 + 2.0 * 1.7320508 * ( 1.0 / 8388608.0 ) * largest / edgeLength;
	//puncho end
	for( int n = 0; n < 2; n++ )
	{
		const idBrushBSPNode* node = gap->GetNode( n );
		int s;
		for( const idBrushBSPPortal* p = node->GetPortals(); p; p = p->Next( s ) )
		{
			s = ( p->GetNode( 1 ) == node );
			if( p == gap )
			{
				continue;
			}
			const idPlane& edgePlane = p->GetPlane();
			if( idMath::Fabs( edgePlane.Distance( v1 ) ) > GRAVSUBDIV_EPSILON || idMath::Fabs( edgePlane.Distance( v2 ) ) > GRAVSUBDIV_EPSILON )
			{
				continue;
			}
			const double cross[3] =
			{
				( double )gapPlane[1] * edgePlane[2] - ( double )gapPlane[2] * edgePlane[1],
				( double )gapPlane[2] * edgePlane[0] - ( double )gapPlane[0] * edgePlane[2],
				( double )gapPlane[0] * edgePlane[1] - ( double )gapPlane[1] * edgePlane[0]
			};
			const double length = sqrt( cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2] );
			//puncho start - Phase 2C P2.2 (Opus C review): the line where the two planes meet must run along the edge (a
			// plane under 1e-3 rad to the gap plane is never taken, see below, and is not tested)
			if( length > 1e-3 )
			{
				const double off[3] =
				{
					cross[1] * edge[2] - cross[2] * edge[1],
					cross[2] * edge[0] - cross[0] * edge[2],
					cross[0] * edge[1] - cross[1] * edge[0]
				};
				const double offSin = sqrt( off[0] * off[0] + off[1] * off[1] + off[2] * off[2] ) / ( length * edgeLength );
				if( !( offSin <= maxSin ) )
				{
					continue;
				}
			}
			//puncho end
			if( length > bestLength )
			{
				bestLength = length;
				dir[0] = cross[0];
				dir[1] = cross[1];
				dir[2] = cross[2];
			}
		}
	}
	// planes less than 1e-3 rad apart do not define the edge; no plane along the edge: the float normal stays
	if( bestLength <= 1e-3 )
	{
		return;
	}

	// along the edge, crossed with the gravity direction as the float normal is
	if( dir[0] * ( ( double )v2[0] - v1[0] ) + dir[1] * ( ( double )v2[1] - v1[1] ) + dir[2] * ( ( double )v2[2] - v1[2] ) < 0.0 )
	{
		dir[0] = -dir[0];
		dir[1] = -dir[1];
		dir[2] = -dir[2];
	}
	const double n[3] =
	{
		dir[1] * invGravityDir[2] - dir[2] * invGravityDir[1],
		dir[2] * invGravityDir[0] - dir[0] * invGravityDir[2],
		dir[0] * invGravityDir[1] - dir[1] * invGravityDir[0]
	};
	const double length = sqrt( n[0] * n[0] + n[1] * n[1] + n[2] * n[2] );
	if( length > 1e-6 )
	{
		normal.Set( ( float )( n[0] / length ), ( float )( n[1] / length ), ( float )( n[2] / length ) );
	}
}
//puncho end

void idAASBuild::GravSubdivLeafNode( idBrushBSPNode* node )
{
	int s1, s2, i, j, k, side1;
	int numSplits, numSplitters;
	idBrushBSPPortal* p1, *p2;
	idWinding* w1, *w2;
	idVec3 normal;
	idPlane plane;
	idPlaneSet planeList;
	float d, min, max;
	int* splitterOrder;
	int* bestNumSplits;
	int floor, gap, numFloorChecked;

	// if this leaf node is already classified it cannot have a combination of floor and gap portals
	if( node->GetFlags() & ( AREA_FLOOR | AREA_GAP ) )
	{
		return;
	}

	floor = gap = 0;

	// check if the area has a floor
	for( p1 = node->GetPortals(); p1; p1 = p1->Next( s1 ) )
	{
		s1 = ( p1->GetNode( 1 ) == node );

		if( p1->GetFlags() & FACE_FLOOR )
		{
			floor++;
		}
	}

	// find seperating planes between gap and floor portals
	for( p1 = node->GetPortals(); p1; p1 = p1->Next( s1 ) )
	{
		s1 = ( p1->GetNode( 1 ) == node );

		// if the portal is a gap seen from this side
		if( PortalIsGap( p1, s1 ) )
		{
			gap++;
			// if the area doesn't have a floor
			if( !floor )
			{
				break;
			}
		}
		else
		{
			continue;
		}

		numFloorChecked = 0;

		w1 = p1->GetWinding();

		// test all edges of the gap
		for( i = 0; i < w1->GetNumPoints(); i++ )
		{

			// create a plane through the edge of the gap parallel to the direction of gravity
			normal = ( *w1 )[( i + 1 ) % w1->GetNumPoints()].ToVec3() - ( *w1 )[i].ToVec3();
			normal = normal.Cross( aasSettings->invGravityDir );
			if( normal.Normalize() < 0.2f )
			{
				continue;
			}
			//puncho start - Phase 2C P2.2: the edge direction from the planes that meet at the edge
			if( aas_gravityEdgePlanes.GetBool() )
			{
				GravEdgeNormal( p1, ( *w1 )[i].ToVec3(), ( *w1 )[( i + 1 ) % w1->GetNumPoints()].ToVec3(), aasSettings->invGravityDir, normal );
			}
			//puncho end
			plane.SetNormal( normal );
			plane.FitThroughPoint( ( *w1 )[i].ToVec3() );

			// get the side of the plane the gap is on
			side1 = w1->PlaneSide( plane, GRAVSUBDIV_EPSILON );
			if( side1 == SIDE_ON )
			{
				break;
			}

			// test if the plane through the edge of the gap seperates the gap from a floor portal
			for( p2 = node->GetPortals(); p2; p2 = p2->Next( s2 ) )
			{
				s2 = ( p2->GetNode( 1 ) == node );

				if( !( p2->GetFlags() & FACE_FLOOR ) )
				{
					continue;
				}

				if( p2->GetFlags() & FACE_CHECKED )
				{
					continue;
				}

				w2 = p2->GetWinding();

				min = 2.0f * GRAVSUBDIV_EPSILON;
				max = GRAVSUBDIV_EPSILON;
				if( side1 == SIDE_FRONT )
				{
					for( j = 0; j < w2->GetNumPoints(); j++ )
					{
						d = plane.Distance( ( *w2 )[j].ToVec3() );
						if( d >= GRAVSUBDIV_EPSILON )
						{
							break;	// point at the same side of the plane as the gap
						}
						d = idMath::Fabs( d );
						if( d < min )
						{
							min = d;
						}
						if( d > max )
						{
							max = d;
						}
					}
				}
				else
				{
					for( j = 0; j < w2->GetNumPoints(); j++ )
					{
						d = plane.Distance( ( *w2 )[j].ToVec3() );
						if( d <= -GRAVSUBDIV_EPSILON )
						{
							break;	// point at the same side of the plane as the gap
						}
						d = idMath::Fabs( d );
						if( d < min )
						{
							min = d;
						}
						if( d > max )
						{
							max = d;
						}
					}
				}

				// a point of the floor portal was found to be at the same side of the plane as the gap
				if( j < w2->GetNumPoints() )
				{
					continue;
				}

				// if the floor portal touches the plane
				if( min < GRAVSUBDIV_EPSILON && max > GRAVSUBDIV_EPSILON )
				{
					planeList.FindPlane( plane, 0.00001f, 0.1f );
				}

				p2->SetFlag( FACE_CHECKED );
				numFloorChecked++;

			}
			if( numFloorChecked == floor )
			{
				break;
			}
		}

		for( p2 = node->GetPortals(); p2; p2 = p2->Next( s2 ) )
		{
			s2 = ( p2->GetNode( 1 ) == node );
			p2->RemoveFlag( FACE_CHECKED );
		}
	}

	// if the leaf node does not have both floor and gap portals
	if( !( gap && floor ) )
	{
		if( floor )
		{
			node->SetFlag( AREA_FLOOR );
		}
		else if( gap )
		{
			node->SetFlag( AREA_GAP );
		}
		return;
	}

	// if no valid seperators found
	if( planeList.Num() == 0 )
	{
		// NOTE: this should never happend, if it does the leaf node has degenerate portals
		return;
	}

	splitterOrder = ( int* ) _alloca( planeList.Num() * sizeof( int ) );
	bestNumSplits = ( int* ) _alloca( planeList.Num() * sizeof( int ) );
	numSplitters = 0;

	// test all possible seperators and sort them from best to worst
	for( i = 0; i < planeList.Num(); i += 2 )
	{
		numSplits = 0;

		for( p1 = node->GetPortals(); p1; p1 = p1->Next( s1 ) )
		{
			s1 = ( p1->GetNode( 1 ) == node );
			if( p1->GetWinding()->PlaneSide( planeList[i], 0.1f ) == SIDE_CROSS )
			{
				numSplits++;
			}
		}

		for( j = 0; j < numSplitters; j++ )
		{
			if( numSplits < bestNumSplits[j] )
			{
				for( k = numSplitters; k > j; k-- )
				{
					bestNumSplits[k] = bestNumSplits[k - 1];
					splitterOrder[k] = splitterOrder[k - 1];
				}
				bestNumSplits[j] = numSplits;
				splitterOrder[j] = i;
				numSplitters++;
				break;
			}
		}
		if( j >= numSplitters )
		{
			bestNumSplits[j] = numSplits;
			splitterOrder[j] = i;
			numSplitters++;
		}
	}

	// try all seperators in order from best to worst
	for( i = 0; i < numSplitters; i++ )
	{
		if( node->Split( planeList[splitterOrder[i]], -1 ) )
		{
			// we found a seperator that works
			break;
		}
	}
	if( i >= numSplitters )
	{
		return;
	}

	DisplayRealTimeString( "\r%6d", ++numGravitationalSubdivisions );

	// test children for further splits
	GravSubdivLeafNode( node->GetChild( 0 ) );
	GravSubdivLeafNode( node->GetChild( 1 ) );
}

/*
============
idAASBuild::GravSubdiv_r
============
*/
void idAASBuild::GravSubdiv_r( idBrushBSPNode* node )
{

	if( !node )
	{
		return;
	}

	if( node->GetContents() & AREACONTENTS_SOLID )
	{
		return;
	}

	if( !node->GetChild( 0 ) && !node->GetChild( 1 ) )
	{
		GravSubdivLeafNode( node );
		return;
	}

	GravSubdiv_r( node->GetChild( 0 ) );
	GravSubdiv_r( node->GetChild( 1 ) );
}

/*
============
idAASBuild::GravitationalSubdivision
============
*/
void idAASBuild::GravitationalSubdivision( idBrushBSP& bsp )
{
	numGravitationalSubdivisions = 0;

	common->Printf( "[Gravitational Subdivision]\n" );

	SetPortalFlags_r( bsp.GetRootNode() );
	GravSubdiv_r( bsp.GetRootNode() );

	common->Printf( "\r%6d subdivisions\n", numGravitationalSubdivisions );
}
