/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013-2025 Robert Beckebans

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

//=================================================================================


#if 0

should we try and snap values very close to 0.5, 0.25, 0.125, etc ?

	do we write out normals, or just a "smooth shade" flag ?
		resolved : normals.  otherwise adjacent facet shaded surfaces get their
		vertexes merged, and they would have to be split apart before drawing

		do we save out "wings" for shadow silhouette info ?


#endif

			static int c_totalObjVerts = 0;

#define	AREANUM_DIFFERENT	-2
/*
=============
PruneNodes_r

Any nodes that have all children with the same
area can be combined into a single leaf node

Returns the area number of all children, or
AREANUM_DIFFERENT if not the same.
=============
*/
int	PruneNodes_r( node_t* node )
{
	int		a1, a2;

	if( node->planenum == PLANENUM_LEAF )
	{
		return node->area;
	}

	a1 = PruneNodes_r( node->children[0] );
	a2 = PruneNodes_r( node->children[1] );

	if( a1 != a2 || a1 == AREANUM_DIFFERENT )
	{
		return AREANUM_DIFFERENT;
	}

	// free all the nodes below this point
	FreeTreePortals_r( node->children[0] );
	FreeTreePortals_r( node->children[1] );
	FreeTree_r( node->children[0] );
	FreeTree_r( node->children[1] );

	// change this node to a leaf
	node->planenum = PLANENUM_LEAF;
	node->area = a1;

	return a1;
}

static void WriteFloat( idFile* f, float v )
{
	if( idMath::Fabs( v - idMath::Rint( v ) ) < 0.001 )
	{
		f->WriteFloatString( "%i ", ( int )idMath::Rint( v ) );
	}
	else
	{
		f->WriteFloatString( "%f ", v );
	}
}

void Write1DMatrix( idFile* f, int x, float* m )
{
	int		i;

	f->WriteFloatString( "( " );

	for( i = 0; i < x; i++ )
	{
		WriteFloat( f, m[i] );
	}

	f->WriteFloatString( ") " );
}


/*
==============
MatchVert
==============
*/
#define	XYZ_EPSILON	0.01
#define	ST_EPSILON	0.001
#define	COSINE_EPSILON	0.999

static bool MatchVert( const idDrawVert* a, const idDrawVert* b, const bool compareColor )
{
	if( idMath::Fabs( a->xyz[0] - b->xyz[0] ) > XYZ_EPSILON )
	{
		return false;
	}
	if( idMath::Fabs( a->xyz[1] - b->xyz[1] ) > XYZ_EPSILON )
	{
		return false;
	}
	if( idMath::Fabs( a->xyz[2] - b->xyz[2] ) > XYZ_EPSILON )
	{
		return false;
	}

	if( idMath::Fabs( a->GetTexCoordS() - b->GetTexCoordS() ) > ST_EPSILON )
	{
		return false;
	}
	if( idMath::Fabs( a->GetTexCoordT() - b->GetTexCoordT() ) > ST_EPSILON )
	{
		return false;
	}

	// only where the colour is written: the verts of a surface without colour hold whatever
	// colour the tris were made with, and comparing it would change which verts weld
	if( compareColor && a->GetColor() != b->GetColor() )
	{
		return false;
	}

	// RB begin
	// if the normal is 0 (smoothed normals), consider it a match
	if( a->GetNormal().Length() == 0 && b->GetNormal().Length() == 0 )
	{
		return true;
	}

	// otherwise do a dot-product cosine check
	if( ( a->GetNormal() * b->GetNormal() ) < COSINE_EPSILON )
	{
		return false;
	}
	// RB end

	return true;
}

/*
====================
ShareMapTriVerts

Converts independent triangles to shared vertex triangles

Takes the tris in list order and stops before the first one whose new vertexes
would push the surface past maxVerts; that tri and all after it come back in
rest (NULL when everything fit) for the caller to share into the next surface.
A surface under the cap is built exactly as it was before the cap existed.

The colour is copied always and only tells verts apart when carriesColor is set,
the surfaces written with PROC_VERTEX_FORMAT_COLOR.
====================
*/
srfTriangles_t*	ShareMapTriVerts( const mapTri_t* tris, const int maxVerts, const mapTri_t** rest, const bool carriesColor )
{
	const mapTri_t*	step;
	int			count;
	int			i, j;
	int			numVerts;
	int			numIndexes;
	srfTriangles_t*	uTri;

	// a tri adds at most 3 vertexes, so with room for 3 the first tri always fits and
	// every call makes progress; below that the caller's loop would never end
	if( maxVerts < 3 )
	{
		common->Error( "ShareMapTriVerts: maxVerts %i < 3", maxVerts );
	}

	// unique the vertexes
	count = CountTriList( tris );

	// the tri that goes over the cap writes up to 3 vertex slots before it is taken back;
	// the indexes are not capped, every remaining tri may still fit
	uTri = R_AllocStaticTriSurf();
	R_AllocStaticTriSurfVerts( uTri, Min( count * 3, maxVerts + 3 ) );
	R_AllocStaticTriSurfIndexes( uTri, count * 3 );

	numVerts = 0;
	numIndexes = 0;
	*rest = NULL;

	for( step = tris ; step ; step = step->next )
	{
		// share the tri as usual, then take it back whole if it went over the cap:
		// the slots it wrote past the restored counts are simply never read
		const int prevNumVerts = numVerts;
		const int prevNumIndexes = numIndexes;

		for( i = 0 ; i < 3 ; i++ )
		{
			const idDrawVert*	dv;

			dv = &step->v[i];

			// search for a match
			for( j = 0 ; j < numVerts ; j++ )
			{
				if( MatchVert( &uTri->verts[j], dv, carriesColor ) )
				{
					break;
				}
			}

			if( j == numVerts )
			{
				numVerts++;
				uTri->verts[j].xyz = dv->xyz;
				//uTri->verts[j].SetNormal( dv->normal[0], dv->normal[1], dv->normal[2] );
				uTri->verts[j].SetNormal( dv->GetNormal() );
				uTri->verts[j].SetTexCoordS( dv->GetTexCoordS() );
				uTri->verts[j].SetTexCoordT( dv->GetTexCoordT() );
				uTri->verts[j].SetColor( dv->GetColor() );
			}

			uTri->indexes[numIndexes++] = j;
		}

		if( numVerts > maxVerts )
		{
			numVerts = prevNumVerts;
			numIndexes = prevNumIndexes;
			*rest = step;
			break;
		}
	}

	uTri->numVerts = numVerts;
	uTri->numIndexes = numIndexes;

	return uTri;
}

/*
==================
CleanupUTriangles
==================
*/
static void CleanupUTriangles( srfTriangles_t* tri )
{
	// perform cleanup operations

	R_RangeCheckIndexes( tri );
	R_CreateSilIndexes( tri );
//	R_RemoveDuplicatedTriangles( tri );	// this may remove valid overlapped transparent triangles
	R_RemoveDegenerateTriangles( tri );
//	R_RemoveUnusedVerts( tri );

	R_FreeStaticTriSurfSilIndexes( tri );
}

/*
====================
WriteUTriangles

Writes text verts and indexes to procfile: PROC_VERTEX_FORMAT_BASIC writes 8 floats per
vertex (xyz st normal), PROC_VERTEX_FORMAT_COLOR adds the colour as 4 more (rgba 0..1)
====================
*/
static void WriteUTriangles( idFile* procFile, const srfTriangles_t* uTris, const idVec3& offsetOrigin, const int vertexFormat )
{
	int			col;
	int			i;

	// ShareMapTriVerts has already stored the indexes as triIndex_t, so past 65535 vertexes they
	// wrapped and no later check (R_RangeCheckIndexes included) can see it; only a dmap_maxSurfaceVerts
	// above 65535 gets here
	if( uTris->numVerts > 65535 )
	{
		// no CloseFile: procFile is WriteOutputFile's idFileLocal, closed when Error unwinds; closing it here would free it twice
		common->Error( "WriteUTriangles: %i vertexes in one surface, the .proc index is 16-bit (max 65535)", uTris->numVerts );
	}

	// emit this chain
	procFile->WriteFloatString( "/* numVerts = */ %i /* numIndexes = */ %i /* vertexFormat = */ %i\n",
								uTris->numVerts, uTris->numIndexes, vertexFormat );

	// verts
	col = 0;
	for( i = 0 ; i < uTris->numVerts ; i++ )
	{
		float	vec[PROC_VERTEX_FLOATS_COLOR];
		const idDrawVert* dv;

		dv = &uTris->verts[i];

		vec[0] = dv->xyz[0] - offsetOrigin.x;
		vec[1] = dv->xyz[1] - offsetOrigin.y;
		vec[2] = dv->xyz[2] - offsetOrigin.z;

		idVec2 st = dv->GetTexCoord();
		vec[3] = st.x;
		vec[4] = st.y;

		idVec3 normal = dv->GetNormal();
		vec[5] = normal.x;
		vec[6] = normal.y;
		vec[7] = normal.z;

		if( vertexFormat == PROC_VERTEX_FORMAT_COLOR )
		{
			idVec4 color;
			UnpackColor( dv->GetColor(), color );
			vec[8] = color.x;
			vec[9] = color.y;
			vec[10] = color.z;
			vec[11] = color.w;
			Write1DMatrix( procFile, PROC_VERTEX_FLOATS_COLOR, vec );
		}
		else
		{
			Write1DMatrix( procFile, PROC_VERTEX_FLOATS_BASIC, vec );
		}

		if( ++col == 3 )
		{
			col = 0;
			procFile->WriteFloatString( "\n" );
		}
	}
	if( col != 0 )
	{
		procFile->WriteFloatString( "\n" );
	}

	// indexes
	col = 0;
	for( i = 0 ; i < uTris->numIndexes ; i++ )
	{
		procFile->WriteFloatString( "%i ", uTris->indexes[i] );

		if( ++col == 18 )
		{
			col = 0;
			procFile->WriteFloatString( "\n" );
		}
	}
	if( col != 0 )
	{
		procFile->WriteFloatString( "\n" );
	}
}

// RB begin
static void WriteObjTriangles( idFile* objFile, int area, const srfTriangles_t* uTris, const idMat4& entityToWorldTransform, const char* materialName )
{
	idVec4 c = PickDebugColor( area );

	objFile->Printf( "usemtl %s\n", materialName );

	// verts
	for( int i = 0 ; i < uTris->numVerts ; i++ )
	{
		const idDrawVert* dv = &uTris->verts[i];

		idVec3 pos = ( entityToWorldTransform * idVec4( dv->xyz[0], dv->xyz[1], dv->xyz[2], 1 ) ).ToVec3();
		idVec2 st = dv->GetTexCoord();
		idVec3 n = dv->GetNormal();

		objFile->Printf( "v %1.6f %1.6f %1.6f %1.6f %1.6f %1.6f\n", pos.x, pos.y, pos.z, c.x, c.y, c.z );
		objFile->Printf( "vt %1.6f %1.6f\n", st.x, 1.0f - st.y );
		objFile->Printf( "vn %1.6f %1.6f %1.6f\n", n.x, n.y, n.z );
	}

	// indexes

	// flip order for OBJ
	for( int j = 0; j < uTris->numIndexes; j += 3 )
	{
		objFile->Printf( "f %i/%i/%i %i/%i/%i %i/%i/%i\n",
						 uTris->indexes[j + 2] + 1 + c_totalObjVerts,
						 uTris->indexes[j + 2] + 1 + c_totalObjVerts,
						 uTris->indexes[j + 2] + 1 + c_totalObjVerts,

						 uTris->indexes[j + 1] + 1 + c_totalObjVerts,
						 uTris->indexes[j + 1] + 1 + c_totalObjVerts,
						 uTris->indexes[j + 1] + 1 + c_totalObjVerts,

						 uTris->indexes[j + 0] + 1 + c_totalObjVerts,
						 uTris->indexes[j + 0] + 1 + c_totalObjVerts,
						 uTris->indexes[j + 0] + 1 + c_totalObjVerts );
	}

	c_totalObjVerts += uTris->numVerts;

	objFile->Printf( "\n\n" );
}
// RB end


/*
=======================
GroupsAreSurfaceCompatible

Planes, texcoords, and groupLights can differ,
but the material and mergegroup must match
=======================
*/
static bool GroupsAreSurfaceCompatible( const optimizeGroup_t* a, const optimizeGroup_t* b )
{
	if( a->material != b->material )
	{
		return false;
	}
	if( a->mergeGroup != b->mergeGroup )
	{
		return false;
	}
	return true;
}

/*
====================
WriteOutputSurfaces
====================
*/
static void WriteOutputSurfaces( int entityNum, int areaNum, idFile* procFile, idFile* objFile )
{
	mapTri_t*	ambient, *copy;
	int			surfaceNum;
	idMapEntity*	entity;
	uArea_t*		area;
	optimizeGroup_t*	group, *groupStep;
//	int			col;
	srfTriangles_t*	uTri;
//	mapTri_t	*tri;
	const char*	name = "";

	// all .proc surfaces of the area are built before anything is written, because the
	// model header must carry the real count once a surface was split at the vertex cap
	typedef struct
	{
		const idMaterial*	material;
		srfTriangles_t*		tri;
		int					vertexFormat;
	} outputSurface_t;

	idList<outputSurface_t>	surfaces;
	const int				maxVerts = dmap_maxSurfaceVerts.GetInteger();

	area = &dmapGlobals.uEntities[entityNum].areas[areaNum];
	entity = dmapGlobals.uEntities[entityNum].mapEntity;

	if( entityNum != 0 )
	{
		entity->epairs.GetString( "name", "", &name );
		if( !name[0] )
		{
			common->Error( "Entity %i has surfaces, but no name key", entityNum );
		}
	}

	for( group = area->groups ; group ; group = group->nextGroup )
	{
		if( group->surfaceEmited )
		{
			continue;
		}

		// combine all groups compatible with this one
		// usually several optimizeGroup_t can be combined into a single
		// surface, even though they couldn't be merged together to save
		// vertexes because they had different planes, texture coordinates, or lights.
		// Different mergeGroups will stay in separate surfaces.
		ambient = NULL;

		for( groupStep = group ; groupStep ; groupStep = groupStep->nextGroup )
		{
			if( groupStep->surfaceEmited )
			{
				continue;
			}
			if( !GroupsAreSurfaceCompatible( group, groupStep ) )
			{
				continue;
			}

			// the colour pass flags every group of a merge group, so one surface never mixes;
			// if one did, the writer would drop the colour of part of it
			if( groupStep->vertexColor != group->vertexColor )
			{
				common->Error( "WriteOutputSurfaces: merge group of '%s' mixes coloured and plain groups", group->material->GetName() );
			}

			// copy it out to the ambient list
			copy = CopyTriList( groupStep->triList );
			ambient = MergeTriLists( ambient, copy );
			groupStep->surfaceEmited = true;
		}

		if( !ambient )
		{
			continue;
		}

		// a surface is one draw with 16 bit indexes: share the vertexes into as many
		// surfaces as it takes to keep each one within maxVerts
		const int		firstSurface = surfaces.Num();
		int				totalVerts = 0;
		const mapTri_t*	tris = ambient;
		while( tris != NULL )
		{
			outputSurface_t& surface = surfaces.Alloc();
			surface.material = ambient->material;
			surface.tri = ShareMapTriVerts( tris, maxVerts, &tris, group->vertexColor );
			surface.vertexFormat = group->vertexColor ? PROC_VERTEX_FORMAT_COLOR : PROC_VERTEX_FORMAT_BASIC;
			totalVerts += surface.tri->numVerts;
		}

		if( surfaces.Num() - firstSurface > 1 )
		{
			common->Printf( "%s: surface %i (%s) split into %i parts (%i vertexes)\n", ( entityNum == 0 ) ? va( "_area%i", areaNum ) : name,
							firstSurface, ambient->material->GetName(), surfaces.Num() - firstSurface, totalVerts );
		}

		FreeTriList( ambient );
	}

	if( entityNum == 0 )
	{
		procFile->WriteFloatString( "model { /* name = */ \"_area%i\" /* numSurfaces = */ %i\n\n",
									areaNum, surfaces.Num() );

		if( objFile )
		{
			objFile->Printf( "g _area%i\n", areaNum );
		}
	}
	else
	{
		procFile->WriteFloatString( "model { /* name = */ \"%s\" /* numSurfaces = */ %i\n\n",
									name, surfaces.Num() );

		if( objFile )
		{
			objFile->Printf( "g model_%s\n", name );
		}
	}

	for( surfaceNum = 0 ; surfaceNum < surfaces.Num() ; surfaceNum++ )
	{
		uTri = surfaces[surfaceNum].tri;
		const char* matName = surfaces[surfaceNum].material->GetName();

		procFile->WriteFloatString( "/* surface %i */ { ", surfaceNum );
		procFile->WriteFloatString( "\"%s\" ", matName );

		CleanupUTriangles( uTri );
		WriteUTriangles( procFile, uTri, entity->originOffset, surfaces[surfaceNum].vertexFormat );

		// RB
		if( objFile )
		{
			idMat4 entityToWorldTransform( mat3_identity, entity->originOffset );

			if( entityNum != 0 )
			{
				idVec3 origin;
				origin.Zero();

				idMat3 rot;
				rot.Identity();

				origin = entity->epairs.GetVector( "origin", "0 0 0" );

				if( !entity->epairs.GetMatrix( "rotation", "1 0 0 0 1 0 0 0 1", rot ) )
				{
					idAngles angles;

					if( entity->epairs.GetAngles( "angles", "0 0 0", angles ) )
					{
						if( angles.pitch != 0.0f || angles.yaw != 0.0f || angles.roll != 0.0f )
						{
							rot = angles.ToMat3();
						}
						else
						{
							rot.Identity();
						}
					}
					else
					{
						float angle = entity->epairs.GetFloat( "angle" );
						if( angle != 0.0f )
						{
							rot = idAngles( 0.0f, angle, 0.0f ).ToMat3();
						}
						else
						{
							rot.Identity();
						}
					}
				}

				idMat4 transform( rot, origin );
				entityToWorldTransform = transform;
			}

			int debugArea = ( entityNum == 0 ) ? areaNum : -1;
			WriteObjTriangles( objFile, debugArea, uTri, entityToWorldTransform, matName );
		}
		// RB end

		R_FreeStaticTriSurf( uTri );

		procFile->WriteFloatString( "}\n\n" );
	}

	procFile->WriteFloatString( "}\n\n" );
}

/*
===============
WriteNode_r

===============
*/
static void WriteNode_r( node_t* node, idFile* procFile )
{
	int		child[2];
	int		i;
	idPlane*	plane;

	if( node->planenum == PLANENUM_LEAF )
	{
		// we shouldn't get here unless the entire world
		// was a single leaf
		procFile->WriteFloatString( "/* node 0 */ ( 0 0 0 0 ) -1 -1\n" );
		return;
	}

	for( i = 0 ; i < 2 ; i++ )
	{
		if( node->children[i]->planenum == PLANENUM_LEAF )
		{
			child[i] = -1 - node->children[i]->area;
		}
		else
		{
			child[i] = node->children[i]->nodeNumber;
		}
	}

	plane = &dmapGlobals.mapPlanes[node->planenum];

	procFile->WriteFloatString( "/* node %i */ ", node->nodeNumber );
	Write1DMatrix( procFile, 4, plane->ToFloatPtr() );
	procFile->WriteFloatString( "%i %i\n", child[0], child[1] );

	if( child[0] > 0 )
	{
		WriteNode_r( node->children[0], procFile );
	}
	if( child[1] > 0 )
	{
		WriteNode_r( node->children[1], procFile );
	}
}

int NumberNodes_r( node_t* node, int nextNumber )
{
	if( node->planenum == PLANENUM_LEAF )
	{
		return nextNumber;
	}
	node->nodeNumber = nextNumber;
	nextNumber++;
	nextNumber = NumberNodes_r( node->children[0], nextNumber );
	nextNumber = NumberNodes_r( node->children[1], nextNumber );

	return nextNumber;
}

// RB begin
// https://stackoverflow.com/questions/801740/c-how-to-draw-a-binary-tree-to-the-console
//
static int WriteASCIIArtNode_r( node_t* node, bool is_left, int offset, int depth, char s[20][2048], idFile* procFile )
{
	char b[20];
	int width = 5;

	if( node->planenum == PLANENUM_LEAF )
	{
		int val = -1 - node->area;

		if( val == 0 )
		{
			// is in solid / touches the outside void
			idStr::snPrintf( b, 20, "(666)", val );
		}
		else
		{
			// leaf is area
			idStr::snPrintf( b, 20, "(A%02d)", val );
		}
	}
	else
	{
		int val = node->nodeNumber;
		idStr::snPrintf( b, 20, "(%03d)", val );
	}

	int left = 0;
	int right = 0;

	if( node->planenum != PLANENUM_LEAF )
	{
		/*
		int		child[2];

		for( int i = 0 ; i < 2 ; i++ )
		{
			if( node->children[i]->planenum == PLANENUM_LEAF )
			{
				child[i] = -1 - node->children[i]->area;
			}
			else
			{
				child[i] = node->children[i]->nodeNumber;
			}
		}
		*/

		if( depth < 19 )
		{
			//if( child[0] > 0 )
			{
				left = WriteASCIIArtNode_r( node->children[0], true, offset, depth + 1, s, procFile );
			}

			//if( child[1] > 0 )
			{
				right = WriteASCIIArtNode_r( node->children[1], false, offset + left + width, depth + 1, s, procFile );
			}
		}
	}

	for( int i = 0; i < width; i++ )
	{
		s[depth][offset + left + i] = b[i];
	}

	if( depth && is_left )
	{
		for( int i = 0; i < width + right; i++ )
		{
			s[depth - 1][offset + left + width / 2 + i] = '-';
		}

		s[depth - 1][offset + left + width / 2] = '.';
	}
	else if( depth && !is_left )
	{
		for( int i = 0; i < left + width; i++ )
		{
			s[depth - 1][offset - width / 2 + i] = '-';
		}

		s[depth - 1][offset + left + width / 2] = '.';
	}

	return left + width + right;
}

static void WriteVisualBSPTree( node_t* node, idFile* procFile )
{
	// TODO calculuate depth instead of assuming 20

	int s_len = 20;
	char s[20][2048];

	// output
	procFile->WriteFloatString( "/* BSP tree visualization:\n\n" );

	for( int i = 0; i < 20; i++ )
	{
		idStr::snPrintf( s[i], 2048, "%640s", " " );
	}

	WriteASCIIArtNode_r( node, 0, 0, 0, s, procFile );

	for( int i = 0; i < 20; i++ )
	{
		procFile->WriteFloatString( "%s\n", s[i] );
	}

	procFile->WriteFloatString( "*/\n\n" );
}
// RB end

/*
====================
WriteOutputNodes
====================
*/
static void WriteOutputNodes( node_t* node, idFile* procFile )
{
	int		numNodes;

	// prune unneeded nodes and count
	PruneNodes_r( node );
	numNodes = NumberNodes_r( node, 0 );

	// a world that pruned down to a single leaf still writes the one
	// dummy node of WriteNode_r, so count it or the loaders choke on it
	if( numNodes == 0 )
	{
		numNodes = 1;
	}

	// output
	procFile->WriteFloatString( "nodes { /* numNodes = */ %i\n\n", numNodes );
	procFile->WriteFloatString( "/* node format is: ( planeVector ) positiveChild negativeChild */\n" );
	procFile->WriteFloatString( "/* a child number of 0 is an opaque, solid area */\n" );
	procFile->WriteFloatString( "/* negative child numbers are areas: (-1-child) */\n" );

	// RB: draw an extra ASCII BSP tree visualization for YouTube tutorial
	if( dmapGlobals.asciiTree )
	{
		WriteVisualBSPTree( node, procFile );
	}
	// RB end

	WriteNode_r( node, procFile );

	procFile->WriteFloatString( "}\n\n" );
}

/*
====================
WriteOutputPortals
====================
*/
static void WriteOutputPortals( uEntity_t* e, idFile* procFile )
{
	int			i, j;
	interAreaPortal_t*	iap;
	idWinding*			w;

	procFile->WriteFloatString( "interAreaPortals { /* numAreas = */ %i /* numIAP = */ %i\n\n",
								e->numAreas, interAreaPortals.Num() );
	procFile->WriteFloatString( "/* interAreaPortal format is: numPoints positiveSideArea negativeSideArea ( point) ... */\n" );
	for( i = 0 ; i < interAreaPortals.Num() ; i++ )
	{
		iap = &interAreaPortals[i];

		// RB: support new area portals
		if( iap->side )
		{
			w = iap->side->winding;
		}
		else
		{
			w = & iap->w;
		}
		// RB end

		procFile->WriteFloatString( "/* iap %i */ %i %i %i ", i, w->GetNumPoints(), iap->area0, iap->area1 );
		for( j = 0 ; j < w->GetNumPoints() ; j++ )
		{
			Write1DMatrix( procFile, 3, ( *w )[j].ToFloatPtr() );
		}
		procFile->WriteFloatString( "\n" );
	}

	procFile->WriteFloatString( "}\n\n" );
}

// RB begin
static void WriteObjOutputPortals( uEntity_t* e, idFile* objFile )
{
	int			i, j;
	interAreaPortal_t*	iap;
	idWinding*			w;

	if( !objFile )
	{
		return;
	}

	idVec4 c = colorBlack;

	for( i = 0 ; i < interAreaPortals.Num() ; i++ )
	{
		iap = &interAreaPortals[i];

		if( iap->side )
		{
			w = iap->side->winding;
		}
		else
		{
			w = & iap->w;
		}

		objFile->WriteFloatString( "g interAreaPortal.%i\n", i );
		for( j = 0 ; j < w->GetNumPoints() ; j++ )
		{
			idVec5 v = ( *w )[j];

			objFile->Printf( "v %1.6f %1.6f %1.6f %1.6f %1.6f %1.6f\n",
							 v.x, v.y, v.z, c.x, c.y, c.z );
		}

		objFile->Printf( "f " );
		for( j = 0; j < w->GetNumPoints(); j++ )
		{
			objFile->Printf( "%i// ", c_totalObjVerts + 1 + j );
		}

		c_totalObjVerts += w->GetNumPoints();

		objFile->Printf( "\n\n" );
	}
}
// RB end


/*
====================
WriteOutputEntity
====================
*/
static void WriteOutputEntity( int entityNum, idFile* procFile, idFile* objFile )
{
	int		i;
	uEntity_t* e;

	e = &dmapGlobals.uEntities[entityNum];

	if( entityNum != 0 )
	{
		// entities may have enclosed, empty areas that we don't need to write out
		if( e->numAreas > 1 )
		{
			e->numAreas = 1;
		}
	}

	for( i = 0 ; i < e->numAreas ; i++ )
	{
		WriteOutputSurfaces( entityNum, i, procFile, objFile );
	}

	// a single area world gets the portals (none) and nodes as well:
	// without nodes the renderer falls back to ClearWorld(), whose dummy node
	// makes PointInArea() return area 0 for every point, even inside solid
	// brushes, so the light grid bake, env probe placement and everything
	// else that asks PointInArea() for "solid" can never get -1
	if( entityNum == 0 && e->numAreas > 0 )
	{
		// output the area portals
		WriteOutputPortals( e, procFile );
		WriteObjOutputPortals( e, objFile );

		// output the nodes
		WriteOutputNodes( e->tree->headnode, procFile );
	}
}


/*
====================
WriteOutputFile
====================
*/
void WriteOutputFile()
{
	int				i;
	uEntity_t*		entity;

	// write the file
	common->Printf( "----- WriteOutputFile -----\n" );

	idStrStatic< MAX_OSPATH > qpath;
	qpath.Format( "%s." PROC_FILE_EXT, dmapGlobals.mapFileBase );
	common->Printf( "writing %s\n", qpath.c_str() );
	idFileLocal procFile = fileSystem->OpenFileWrite( qpath, "fs_basepath" );
	if( !procFile )
	{
		common->Error( "Error opening %s", qpath.c_str() );
	}

	// RB: write area models as OBJ file
	idFile* objFile = NULL;
	if( dmapGlobals.exportObj )
	{
		idStrStatic< MAX_OSPATH > qpath;
		qpath.Format( "%s.obj", dmapGlobals.mapFileBase );
		objFile = fileSystem->OpenFileWrite( qpath, "fs_basepath" );
		if( !objFile )
		{
			common->Error( "Error opening %s", qpath.c_str() );
		}

		c_totalObjVerts = 0;
	}

	procFile->WriteFloatString( "%s\n\n", PROC_FILE_ID );

	// write the entity models and information, writing entities first
	for( i = dmapGlobals.numEntities - 1 ; i >= 0 ; i-- )
	{
		entity = &dmapGlobals.uEntities[i];

		if( !entity->primitives )
		{
			continue;
		}

		WriteOutputEntity( i, procFile, objFile );
	}

	if( objFile )
	{
		delete objFile;
	}
}
