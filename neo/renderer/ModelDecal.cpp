/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2014-2016 Robert Beckebans

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

#include "RenderCommon.h"
#include "Model_local.h"

#include "../idlib/geometry/DrawVert_intrinsics.h"

// decalFade	filter 5 0.1
// polygonOffset
// {
// map invertColor( textures/splat )
// blend GL_ZERO GL_ONE_MINUS_SRC
// vertexColor
// clamp
// }

/*
==================
idRenderModelDecal::idRenderModelDecal
==================
*/
idRenderModelDecal::idRenderModelDecal() :
	firstDecal( 0 ),
	nextDecal( 0 ),
	firstDeferredDecal( 0 ),
	nextDeferredDecal( 0 ),
	numDecalMaterials( 0 )
{
	// SRS - initialize decals so members are defined for logical tests in CreateDecalFromWinding()
	memset( decals, 0, sizeof( decals ) );
}

/*
==================
idRenderModelDecal::~idRenderModelDecal
==================
*/
idRenderModelDecal::~idRenderModelDecal()
{
}

/*
=================
idRenderModelDecal::CreateProjectionParms
=================
*/
bool idRenderModelDecal::CreateProjectionParms( decalProjectionParms_t& parms, const idFixedWinding& winding, const idVec3& projectionOrigin, const bool parallel, const float fadeDepth, const idMaterial* material, const int startTime )
{
	if( !idDecalProjection::CreateParms( parms, winding, projectionOrigin, parallel, fadeDepth, material, startTime ) )
	{
		common->Printf( "idRenderModelDecal::CreateProjectionInfo: winding must have %d points\n", NUM_DECAL_BOUNDING_PLANES - 2 );
		return false;
	}
	return true;
}

/*
=================
idRenderModelDecal::GlobalProjectionParmsToLocal
=================
*/
void idRenderModelDecal::GlobalProjectionParmsToLocal( decalProjectionParms_t& localParms, const decalProjectionParms_t& globalParms, const idVec3& origin, const idMat3& axis )
{
	float modelMatrix[16];

	R_AxisToModelMatrix( axis, origin, modelMatrix );

	for( int j = 0; j < NUM_DECAL_BOUNDING_PLANES; j++ )
	{
		R_GlobalPlaneToLocal( modelMatrix, globalParms.boundingPlanes[j], localParms.boundingPlanes[j] );
	}
	R_GlobalPlaneToLocal( modelMatrix, globalParms.fadePlanes[0], localParms.fadePlanes[0] );
	R_GlobalPlaneToLocal( modelMatrix, globalParms.fadePlanes[1], localParms.fadePlanes[1] );
	R_GlobalPlaneToLocal( modelMatrix, globalParms.textureAxis[0], localParms.textureAxis[0] );
	R_GlobalPlaneToLocal( modelMatrix, globalParms.textureAxis[1], localParms.textureAxis[1] );
	R_GlobalPointToLocal( modelMatrix, globalParms.projectionOrigin, localParms.projectionOrigin );
	localParms.projectionBounds = globalParms.projectionBounds;
	localParms.projectionBounds.TranslateSelf( -origin );
	localParms.projectionBounds.RotateSelf( axis.Transpose() );
	localParms.material = globalParms.material;
	localParms.parallel = globalParms.parallel;
	localParms.fadeDepth = globalParms.fadeDepth;
	localParms.startTime = globalParms.startTime;
	localParms.force = globalParms.force;
}

/*
=================
idRenderModelDecal::ReUse
=================
*/
void idRenderModelDecal::ReUse()
{
	firstDecal = 0;
	nextDecal = 0;
	firstDeferredDecal = 0;
	nextDeferredDecal = 0;
	numDecalMaterials = 0;
}

/*
=================
idRenderModelDecal::CreateDecalFromWinding
=================
*/
void idRenderModelDecal::CreateDecalFromWinding( const idWinding& w, const idMaterial* decalMaterial, const idPlane fadePlanes[2], float fadeDepth, int startTime )
{
	// Often we are appending a new triangle to an existing decal, so merge with the previous decal if possible
	int decalIndex = ( nextDecal - 1 ) & ( MAX_DECALS - 1 );
	if( decalIndex >= 0
			&& decals[decalIndex].material == decalMaterial
			&& decals[decalIndex].startTime == startTime
			&& decals[decalIndex].numVerts + w.GetNumPoints() <= MAX_DECAL_VERTS
			&& decals[decalIndex].numIndexes + 3 * ( w.GetNumPoints() - 2 ) <= MAX_DECAL_INDEXES )
	{
	}
	else
	{
		decalIndex = nextDecal++ & ( MAX_DECALS - 1 );
		decals[decalIndex].material = decalMaterial;
		decals[decalIndex].startTime = startTime;
		decals[decalIndex].numVerts = 0;
		decals[decalIndex].numIndexes = 0;
		assert( w.GetNumPoints() <= MAX_DECAL_VERTS );
		if( nextDecal - firstDecal > MAX_DECALS )
		{
			firstDecal = nextDecal - MAX_DECALS;
		}
	}

	decal_t& decal = decals[decalIndex];

	int firstVert = decal.numVerts;

	// create the vertices
	for( int i = 0; i < w.GetNumPoints(); i++ )
	{
		float depthFade = idDecalProjection::DepthFade( fadePlanes[0], fadeDepth, w[i].ToVec3() );
		if( depthFade < 0.0f )
		{
			depthFade = idDecalProjection::DepthFade( fadePlanes[1], fadeDepth, w[i].ToVec3() );
		}
		if( depthFade < 0.0f )
		{
			depthFade = 0.0f;
		}
		else if( depthFade > 0.99f )
		{
			depthFade = 1.0f;
		}
		decal.vertDepthFade[decal.numVerts] = 1.0f - depthFade;
		decal.verts[decal.numVerts].Clear();
		decal.verts[decal.numVerts].xyz = w[i].ToVec3();
		decal.verts[decal.numVerts].SetTexCoord( w[i].s, w[i].t );
		decal.numVerts++;
	}

	// create the indexes
	for( int i = 2; i < w.GetNumPoints(); i++ )
	{
		assert( decal.numIndexes + 3 <= MAX_DECAL_INDEXES );
		decal.indexes[decal.numIndexes + 0] = firstVert;
		decal.indexes[decal.numIndexes + 1] = firstVert + i - 1;
		decal.indexes[decal.numIndexes + 2] = firstVert + i;
		decal.numIndexes += 3;
	}

	// add degenerate triangles until the index size is a multiple of 16 bytes
	for( ; ( ( ( decal.numIndexes * sizeof( triIndex_t ) ) & 15 ) != 0 ); decal.numIndexes += 3 )
	{
		assert( decal.numIndexes + 3 <= MAX_DECAL_INDEXES );
		decal.indexes[decal.numIndexes + 0] = 0;
		decal.indexes[decal.numIndexes + 1] = 0;
		decal.indexes[decal.numIndexes + 2] = 0;
	}
}

/*
============
R_DecalPointCullStatic
============
*/
static void R_DecalPointCullStatic( byte* cullBits, const idPlane* planes, const idDrawVert* verts, const int numVerts )
{
	assert_16_byte_aligned( cullBits );
	assert_16_byte_aligned( verts );

#if defined(USE_INTRINSICS_SSE)
	idODSStreamedArray< idDrawVert, 16, SBT_DOUBLE, 4 > vertsODS( verts, numVerts );

	const __m128 vector_float_zero	= _mm_setzero_ps();
	const __m128i vector_int_mask0	= _mm_set1_epi32( 1 << 0 );
	const __m128i vector_int_mask1	= _mm_set1_epi32( 1 << 1 );
	const __m128i vector_int_mask2	= _mm_set1_epi32( 1 << 2 );
	const __m128i vector_int_mask3	= _mm_set1_epi32( 1 << 3 );
	const __m128i vector_int_mask4	= _mm_set1_epi32( 1 << 4 );
	const __m128i vector_int_mask5	= _mm_set1_epi32( 1 << 5 );

	const __m128 p0 = _mm_loadu_ps( planes[0].ToFloatPtr() );
	const __m128 p1 = _mm_loadu_ps( planes[1].ToFloatPtr() );
	const __m128 p2 = _mm_loadu_ps( planes[2].ToFloatPtr() );
	const __m128 p3 = _mm_loadu_ps( planes[3].ToFloatPtr() );
	const __m128 p4 = _mm_loadu_ps( planes[4].ToFloatPtr() );
	const __m128 p5 = _mm_loadu_ps( planes[5].ToFloatPtr() );

	const __m128 p0X = _mm_splat_ps( p0, 0 );
	const __m128 p0Y = _mm_splat_ps( p0, 1 );
	const __m128 p0Z = _mm_splat_ps( p0, 2 );
	const __m128 p0W = _mm_splat_ps( p0, 3 );

	const __m128 p1X = _mm_splat_ps( p1, 0 );
	const __m128 p1Y = _mm_splat_ps( p1, 1 );
	const __m128 p1Z = _mm_splat_ps( p1, 2 );
	const __m128 p1W = _mm_splat_ps( p1, 3 );

	const __m128 p2X = _mm_splat_ps( p2, 0 );
	const __m128 p2Y = _mm_splat_ps( p2, 1 );
	const __m128 p2Z = _mm_splat_ps( p2, 2 );
	const __m128 p2W = _mm_splat_ps( p2, 3 );

	const __m128 p3X = _mm_splat_ps( p3, 0 );
	const __m128 p3Y = _mm_splat_ps( p3, 1 );
	const __m128 p3Z = _mm_splat_ps( p3, 2 );
	const __m128 p3W = _mm_splat_ps( p3, 3 );

	const __m128 p4X = _mm_splat_ps( p4, 0 );
	const __m128 p4Y = _mm_splat_ps( p4, 1 );
	const __m128 p4Z = _mm_splat_ps( p4, 2 );
	const __m128 p4W = _mm_splat_ps( p4, 3 );

	const __m128 p5X = _mm_splat_ps( p5, 0 );
	const __m128 p5Y = _mm_splat_ps( p5, 1 );
	const __m128 p5Z = _mm_splat_ps( p5, 2 );
	const __m128 p5W = _mm_splat_ps( p5, 3 );

	for( int i = 0; i < numVerts; )
	{

		const int nextNumVerts = vertsODS.FetchNextBatch() - 4;

		for( ; i <= nextNumVerts; i += 4 )
		{
			const __m128 v0 = _mm_load_ps( vertsODS[i + 0].xyz.ToFloatPtr() );
			const __m128 v1 = _mm_load_ps( vertsODS[i + 1].xyz.ToFloatPtr() );
			const __m128 v2 = _mm_load_ps( vertsODS[i + 2].xyz.ToFloatPtr() );
			const __m128 v3 = _mm_load_ps( vertsODS[i + 3].xyz.ToFloatPtr() );

			const __m128 r0 = _mm_unpacklo_ps( v0, v2 );	// v0.x, v2.x, v0.z, v2.z
			const __m128 r1 = _mm_unpackhi_ps( v0, v2 );	// v0.y, v2.y, v0.w, v2.w
			const __m128 r2 = _mm_unpacklo_ps( v1, v3 );	// v1.x, v3.x, v1.z, v3.z
			const __m128 r3 = _mm_unpackhi_ps( v1, v3 );	// v1.y, v3.y, v1.w, v3.w

			const __m128 vX = _mm_unpacklo_ps( r0, r2 );	// v0.x, v1.x, v2.x, v3.x
			const __m128 vY = _mm_unpackhi_ps( r0, r2 );	// v0.y, v1.y, v2.y, v3.y
			const __m128 vZ = _mm_unpacklo_ps( r1, r3 );	// v0.z, v1.z, v2.z, v3.z

			const __m128 d0 = _mm_madd_ps( vX, p0X, _mm_madd_ps( vY, p0Y, _mm_madd_ps( vZ, p0Z, p0W ) ) );
			const __m128 d1 = _mm_madd_ps( vX, p1X, _mm_madd_ps( vY, p1Y, _mm_madd_ps( vZ, p1Z, p1W ) ) );
			const __m128 d2 = _mm_madd_ps( vX, p2X, _mm_madd_ps( vY, p2Y, _mm_madd_ps( vZ, p2Z, p2W ) ) );
			const __m128 d3 = _mm_madd_ps( vX, p3X, _mm_madd_ps( vY, p3Y, _mm_madd_ps( vZ, p3Z, p3W ) ) );
			const __m128 d4 = _mm_madd_ps( vX, p4X, _mm_madd_ps( vY, p4Y, _mm_madd_ps( vZ, p4Z, p4W ) ) );
			const __m128 d5 = _mm_madd_ps( vX, p5X, _mm_madd_ps( vY, p5Y, _mm_madd_ps( vZ, p5Z, p5W ) ) );

			__m128i c0 = __m128c( _mm_cmpgt_ps( d0, vector_float_zero ) );
			__m128i c1 = __m128c( _mm_cmpgt_ps( d1, vector_float_zero ) );
			__m128i c2 = __m128c( _mm_cmpgt_ps( d2, vector_float_zero ) );
			__m128i c3 = __m128c( _mm_cmpgt_ps( d3, vector_float_zero ) );
			__m128i c4 = __m128c( _mm_cmpgt_ps( d4, vector_float_zero ) );
			__m128i c5 = __m128c( _mm_cmpgt_ps( d5, vector_float_zero ) );

			c0 = _mm_and_si128( c0, vector_int_mask0 );
			c1 = _mm_and_si128( c1, vector_int_mask1 );
			c2 = _mm_and_si128( c2, vector_int_mask2 );
			c3 = _mm_and_si128( c3, vector_int_mask3 );
			c4 = _mm_and_si128( c4, vector_int_mask4 );
			c5 = _mm_and_si128( c5, vector_int_mask5 );

			c0 = _mm_or_si128( c0, c1 );
			c2 = _mm_or_si128( c2, c3 );
			c4 = _mm_or_si128( c4, c5 );

			c0 = _mm_or_si128( c0, c2 );
			c0 = _mm_or_si128( c0, c4 );

			__m128i s0 = _mm_packs_epi32( c0, c0 );
			__m128i b0 = _mm_packus_epi16( s0, s0 );

			*( unsigned int* )&cullBits[i] = _mm_cvtsi128_si32( b0 );
		}
	}

#else

	idODSStreamedArray< idDrawVert, 16, SBT_DOUBLE, 1 > vertsODS( verts, numVerts );

	for( int i = 0; i < numVerts; )
	{

		const int nextNumVerts = vertsODS.FetchNextBatch() - 1;

		for( ; i <= nextNumVerts; i++ )
		{
			const idVec3& v = vertsODS[i].xyz;

			const float d0 = planes[0].Distance( v );
			const float d1 = planes[1].Distance( v );
			const float d2 = planes[2].Distance( v );
			const float d3 = planes[3].Distance( v );
			const float d4 = planes[4].Distance( v );
			const float d5 = planes[5].Distance( v );

			byte bits;
			bits  = IEEE_FLT_SIGNBITNOTSET( d0 ) << 0;
			bits |= IEEE_FLT_SIGNBITNOTSET( d1 ) << 1;
			bits |= IEEE_FLT_SIGNBITNOTSET( d2 ) << 2;
			bits |= IEEE_FLT_SIGNBITNOTSET( d3 ) << 3;
			bits |= IEEE_FLT_SIGNBITNOTSET( d4 ) << 4;
			bits |= IEEE_FLT_SIGNBITNOTSET( d5 ) << 5;

			cullBits[i] = bits;
		}
	}

#endif
}

/*
=================
idRenderModelDecal::CreateDecal
=================
*/
void idRenderModelDecal::CreateDecal( const idRenderModel* model, const decalProjectionParms_t& localParms )
{
	int maxVerts = 0;
	for( int surfNum = 0; surfNum < model->NumSurfaces(); surfNum++ )
	{
		const modelSurface_t* surf = model->Surface( surfNum );
		if( surf->geometry != NULL && surf->shader != NULL )
		{
			maxVerts = Max( maxVerts, surf->geometry->numVerts );
		}
	}

	idTempArray< byte > cullBits( ALIGN( maxVerts, 4 ) );

	// check all model surfaces
	for( int surfNum = 0; surfNum < model->NumSurfaces(); surfNum++ )
	{
		const modelSurface_t* surf = model->Surface( surfNum );

		// if no geometry or no shader
		if( surf->geometry == NULL || surf->shader == NULL )
		{
			continue;
		}

		// decals and overlays use the same rules
		if( !localParms.force && !surf->shader->AllowOverlays() )
		{
			continue;
		}

		srfTriangles_t* tri = surf->geometry;

		// if the triangle bounds do not overlap with the projection bounds
		if( !localParms.projectionBounds.IntersectsBounds( tri->bounds ) )
		{
			continue;
		}

		// decals don't work on animated models
		assert( tri->staticModelWithJoints == NULL );

		// catagorize all points by the planes
		R_DecalPointCullStatic( cullBits.Ptr(), localParms.boundingPlanes, tri->verts, tri->numVerts );

		// start streaming the indexes
		idODSStreamedArray< triIndex_t, 256, SBT_QUAD, 3 > indexesODS( tri->indexes, tri->numIndexes );

		// find triangles inside the projection volume
		for( int i = 0; i < tri->numIndexes; )
		{

			const int nextNumIndexes = indexesODS.FetchNextBatch() - 3;

			for( ; i <= nextNumIndexes; i += 3 )
			{
				const int i0 = indexesODS[i + 0];
				const int i1 = indexesODS[i + 1];
				const int i2 = indexesODS[i + 2];

				// skip triangles completely off one side
				if( cullBits[i0] & cullBits[i1] & cullBits[i2] )
				{
					continue;
				}

				const int orBits = cullBits[i0] | cullBits[i1] | cullBits[i2];

				// skip back facing triangles, then clip the exact surface triangle to the projection
				// volume, only against the planes the cull bits say it crosses
				idFixedWinding fw;
				if( !idDecalProjection::ClipTriangle( localParms, tri->verts[i0].xyz, tri->verts[i1].xyz, tri->verts[i2].xyz, orBits, fw ) )
				{
					continue;
				}

				// there is a part of the triangle between the bounding planes, so clip the
				// triangle based on depth and add decals for the depth faded parts
				idFixedWinding back;

				if( fw.Split( &back, localParms.fadePlanes[0], 0.1f ) == SIDE_CROSS )
				{
					CreateDecalFromWinding( back, localParms.material, localParms.fadePlanes, localParms.fadeDepth, localParms.startTime );
				}

				if( fw.Split( &back, localParms.fadePlanes[1], 0.1f ) == SIDE_CROSS )
				{
					CreateDecalFromWinding( back, localParms.material, localParms.fadePlanes, localParms.fadeDepth, localParms.startTime );
				}

				CreateDecalFromWinding( fw, localParms.material, localParms.fadePlanes, localParms.fadeDepth, localParms.startTime );
			}
		}
	}
}

/*
=====================
idRenderModelDecal::CreateDeferredDecals
=====================
*/
void idRenderModelDecal::CreateDeferredDecals( const idRenderModel* model )
{
	for( unsigned int i = firstDeferredDecal; i < nextDeferredDecal; i++ )
	{
		decalProjectionParms_t& parms = deferredDecals[i & ( MAX_DEFERRED_DECALS - 1 )];
		if( parms.startTime > tr.viewDef->renderView.time[0] -  DEFFERED_DECAL_TIMEOUT )
		{
			CreateDecal( model, parms );
		}
	}
	firstDeferredDecal = 0;
	nextDeferredDecal = 0;
}

/*
=====================
idRenderModelDecal::AddDeferredDecal
=====================
*/
void idRenderModelDecal::AddDeferredDecal( const decalProjectionParms_t& localParms )
{
	deferredDecals[nextDeferredDecal++ & ( MAX_DEFERRED_DECALS - 1 )] = localParms;
	if( nextDeferredDecal - firstDeferredDecal > MAX_DEFERRED_DECALS )
	{
		firstDeferredDecal = nextDeferredDecal - MAX_DEFERRED_DECALS;
	}
}

/*
=====================
idRenderModelDecal::RemoveFadedDecals
=====================
*/
void idRenderModelDecal::RemoveFadedDecals( int time )
{
	for( unsigned int i = firstDecal; i < nextDecal; i++ )
	{
		decal_t& decal = decals[i & ( MAX_DECALS - 1 )];

		const decalInfo_t decalInfo = decal.material->GetDecalInfo();
		const int minTime = time - ( decalInfo.stayTime + decalInfo.fadeTime );

		if( decal.startTime <= minTime )
		{
			decal.numVerts = 0;
			decal.numIndexes = 0;
			if( i == firstDecal )
			{
				firstDecal++;
			}
		}
	}
	if( firstDecal == nextDecal )
	{
		firstDecal = 0;
		nextDecal = 0;
	}
}

/*
=====================
R_CopyDecalSurface
=====================
*/
static void R_CopyDecalSurface( idDrawVert* verts, int numVerts, triIndex_t* indexes, int numIndexes,
								const decal_t* decal, const float fadeColor[4] )
{
	assert_16_byte_aligned( &verts[numVerts] );
	assert_16_byte_aligned( &indexes[numIndexes] );
	assert_16_byte_aligned( decal->indexes );
	assert_16_byte_aligned( decal->verts );
	assert( ( ( decal->numVerts * sizeof( idDrawVert ) ) & 15 ) == 0 );
	assert( ( ( decal->numIndexes * sizeof( triIndex_t ) ) & 15 ) == 0 );
	assert_16_byte_aligned( fadeColor );

#if defined(USE_INTRINSICS_SSE)

	const __m128i vector_int_num_verts = _mm_shuffle_epi32( _mm_cvtsi32_si128( numVerts ), 0 );
	const __m128i vector_short_num_verts = _mm_packs_epi32( vector_int_num_verts, vector_int_num_verts );
	const __m128 vector_fade_color = _mm_load_ps( fadeColor );
	const __m128i vector_color_mask = _mm_set_epi32( 0, -1, 0, 0 );

	// copy vertices and apply depth/time based fading
	assert_offsetof( idDrawVert, color, 6 * 4 );
	for( int i = 0; i < decal->numVerts; i++ )
	{
		const idDrawVert& srcVert = decal->verts[i];
		idDrawVert& dstVert = verts[numVerts + i];

		__m128i v0 = _mm_load_si128( ( const __m128i* )( ( byte* )&srcVert +  0 ) );
		__m128i v1 = _mm_load_si128( ( const __m128i* )( ( byte* )&srcVert + 16 ) );
		__m128 depthFade = _mm_splat_ps( _mm_load_ss( decal->vertDepthFade + i ), 0 );

		__m128 timeDepthFade = _mm_mul_ps( depthFade, vector_fade_color );
		__m128i colorInt = _mm_cvtps_epi32( timeDepthFade );
		__m128i colorShort = _mm_packs_epi32( colorInt, colorInt );
		__m128i colorByte = _mm_packus_epi16( colorShort, colorShort );
		v1 = _mm_or_si128( v1, _mm_and_si128( colorByte, vector_color_mask ) );

		_mm_stream_si128( ( __m128i* )( ( byte* )&dstVert +  0 ), v0 );
		_mm_stream_si128( ( __m128i* )( ( byte* )&dstVert + 16 ), v1 );
	}

	// copy indexes
	assert( ( decal->numIndexes & 7 ) == 0 );
	assert( sizeof( triIndex_t ) == 2 );
	for( int i = 0; i < decal->numIndexes; i += 8 )
	{
		__m128i vi = _mm_load_si128( ( const __m128i* )&decal->indexes[i] );

		vi = _mm_add_epi16( vi, vector_short_num_verts );

		_mm_stream_si128( ( __m128i* )&indexes[numIndexes + i], vi );
	}

	_mm_sfence();

#else

	// copy vertices and apply depth/time based fading
	for( int i = 0; i < decal->numVerts; i++ )
	{
		// NOTE: bad out-of-order write-combined write, SIMD code does the right thing
		verts[numVerts + i] = decal->verts[i];
		for( int j = 0; j < 4; j++ )
		{
			verts[numVerts + i].color[j] = idMath::Ftob( fadeColor[j] * decal->vertDepthFade[i] );
		}
	}

	// copy indices
	assert( ( decal->numIndexes & 1 ) == 0 );
	for( int i = 0; i < decal->numIndexes; i += 2 )
	{
		assert( decal->indexes[i + 0] < decal->numVerts && decal->indexes[i + 1] < decal->numVerts );
		WriteIndexPair( &indexes[numIndexes + i], numVerts + decal->indexes[i + 0], numVerts + decal->indexes[i + 1] );
	}

#endif
}

/*
=====================
idRenderModelDecal::GetNumDecalDrawSurfs
=====================
*/
unsigned int idRenderModelDecal::GetNumDecalDrawSurfs()
{
	numDecalMaterials = 0;

	for( unsigned int i = firstDecal; i < nextDecal; i++ )
	{
		const decal_t& decal = decals[i & ( MAX_DECALS - 1 )];

		unsigned int j = 0;
		for( ; j < numDecalMaterials; j++ )
		{
			if( decalMaterials[j] == decal.material )
			{
				break;
			}
		}
		if( j >= numDecalMaterials )
		{
			decalMaterials[numDecalMaterials++] = decal.material;
		}
	}

	return numDecalMaterials;
}

/*
=====================
idRenderModelDecal::CreateDecalDrawSurf
=====================
*/
drawSurf_t* idRenderModelDecal::CreateDecalDrawSurf( const viewEntity_t* space, unsigned int index )
{
	if( index < 0 || index >= numDecalMaterials )
	{
		return NULL;
	}

	const idMaterial* material = decalMaterials[index];

	int maxVerts = 0;
	int maxIndexes = 0;
	for( unsigned int i = firstDecal; i < nextDecal; i++ )
	{
		const decal_t& decal = decals[i & ( MAX_DECALS - 1 )];
		if( decal.material == material )
		{
			maxVerts += decal.numVerts;
			maxIndexes += decal.numIndexes;
		}
	}

	if( maxVerts == 0 || maxIndexes == 0 )
	{
		return NULL;
	}

	// create a new triangle surface in frame memory so it gets automatically disposed of
	srfTriangles_t* newTri = ( srfTriangles_t* )R_ClearedFrameAlloc( sizeof( *newTri ), FRAME_ALLOC_SURFACE_TRIANGLES );
	newTri->numVerts = maxVerts;
	newTri->numIndexes = maxIndexes;

	newTri->ambientCache = vertexCache.AllocVertex( NULL, maxVerts );
	newTri->indexCache = vertexCache.AllocIndex( NULL, maxIndexes );

	idDrawVert* mappedVerts = ( idDrawVert* )vertexCache.MappedVertexBuffer( newTri->ambientCache );
	triIndex_t* mappedIndexes = ( triIndex_t* )vertexCache.MappedIndexBuffer( newTri->indexCache );

	const decalInfo_t decalInfo = material->GetDecalInfo();
	const int maxTime = decalInfo.stayTime + decalInfo.fadeTime;
	const int time = tr.viewDef->renderView.time[0];

	int numVerts = 0;
	int numIndexes = 0;
	for( unsigned int i = firstDecal; i < nextDecal; i++ )
	{
		const decal_t& decal = decals[i & ( MAX_DECALS - 1 )];

		if( decal.numVerts == 0 )
		{
			if( i == firstDecal )
			{
				firstDecal++;
			}
			continue;
		}

		if( decal.material != material )
		{
			continue;
		}

		const int deltaTime = time - decal.startTime;
		const int fadeTime = deltaTime - decalInfo.stayTime;
		if( deltaTime > maxTime )
		{
			continue;	// already completely faded away, but not yet removed
		}

		const float f = ( deltaTime > decalInfo.stayTime ) ? ( ( float ) fadeTime / decalInfo.fadeTime ) : 0.0f;

		ALIGNTYPE16 float fadeColor[4];
		for( int j = 0; j < 4; j++ )
		{
			fadeColor[j] = 255.0f * ( decalInfo.start[j] + ( decalInfo.end[j] - decalInfo.start[j] ) * f );
		}

		// use SIMD optimized routine to copy the vertices and indices directly to write-combined memory
		// this also applies any depth/time based fading while copying
		R_CopyDecalSurface( mappedVerts, numVerts, mappedIndexes, numIndexes, &decal, fadeColor );

		numVerts += decal.numVerts;
		numIndexes += decal.numIndexes;
	}
	newTri->numVerts = numVerts;
	newTri->numIndexes = numIndexes;

	// create the drawsurf
	drawSurf_t* drawSurf = ( drawSurf_t* )R_FrameAlloc( sizeof( *drawSurf ), FRAME_ALLOC_DRAW_SURFACE );
	drawSurf->frontEndGeo = newTri;
	drawSurf->numIndexes = newTri->numIndexes;
	drawSurf->ambientCache = newTri->ambientCache;
	drawSurf->indexCache = newTri->indexCache;
	drawSurf->jointCache = 0;
	drawSurf->space = space;
	drawSurf->scissorRect = space->scissorRect;
	drawSurf->extraGLState = 0;

	R_SetupDrawSurfShader( drawSurf, material, &space->entityDef->parms );

	return drawSurf;
}

/*
=====================
R_DecalSelfTestPrintParms
=====================
*/
static void R_DecalSelfTestPrintParms( const char* label, const decalProjectionParms_t& parms )
{
	common->Printf( "%s parms: parallel %i, fadeDepth %.6f\n", label, ( int )parms.parallel, parms.fadeDepth );
	for( int i = 0; i < NUM_DECAL_BOUNDING_PLANES; i++ )
	{
		const idPlane& plane = parms.boundingPlanes[i];
		common->Printf( "  boundingPlane %i: %.6f %.6f %.6f %.6f\n", i, plane[0], plane[1], plane[2], plane[3] );
	}
	for( int i = 0; i < 2; i++ )
	{
		const idPlane& plane = parms.fadePlanes[i];
		common->Printf( "  fadePlane %i: %.6f %.6f %.6f %.6f\n", i, plane[0], plane[1], plane[2], plane[3] );
	}
	for( int i = 0; i < 2; i++ )
	{
		const idPlane& plane = parms.textureAxis[i];
		common->Printf( "  textureAxis %i: %.6f %.6f %.6f %.6f\n", i, plane[0], plane[1], plane[2], plane[3] );
	}
	const idBounds& bounds = parms.projectionBounds;
	common->Printf( "  bounds: %.6f %.6f %.6f to %.6f %.6f %.6f\n", bounds[0][0], bounds[0][1], bounds[0][2], bounds[1][0], bounds[1][1], bounds[1][2] );
	common->Printf( "  origin: %.6f %.6f %.6f\n", parms.projectionOrigin[0], parms.projectionOrigin[1], parms.projectionOrigin[2] );
}

/*
=====================
idRenderModelDecal::SelfTest

The projection maths on fixed input, printed so that a change to them shows up as a
diff between two runs. A tilted, non-square projector off the world origin is aimed
at a wall with a floor in front of it: the wall crosses the far fade plane, the floor
both fade planes and the near cap, and the side planes clip both. The parallel and the
perspective decals get different start times, so they never share a decal_t.
Deterministic: no clock, no random, no map.
=====================
*/
void idRenderModelDecal::SelfTest()
{
	const idVec3 base( 96.0f, 48.0f, 32.0f );

	// the far cap lies in the plane x = 4 + y / 8 + z / 16 (around base), its normal faces
	// the projection origin at +x; s runs along +y and t along -z
	idFixedWinding winding;
	winding += idVec5( base + idVec3( 2.875f, -15.0f,  12.0f ), idVec2( 0.0f, 0.0f ) );
	winding += idVec5( base + idVec3( 6.625f,  15.0f,  12.0f ), idVec2( 1.0f, 0.0f ) );
	winding += idVec5( base + idVec3( 5.125f,  15.0f, -12.0f ), idVec2( 1.0f, 1.0f ) );
	winding += idVec5( base + idVec3( 1.375f, -15.0f, -12.0f ), idVec2( 0.0f, 1.0f ) );

	const idVec3 projectionOrigin = base + idVec3( 30.0f, 1.0f, 2.0f );
	const float fadeDepth = 6.0f;

	common->Printf( "decalSelfTest: begin\n" );

	decalProjectionParms_t parallelParms;
	decalProjectionParms_t perspectiveParms;
	if( !CreateProjectionParms( parallelParms, winding, projectionOrigin, true, fadeDepth, tr.defaultMaterial, 0 )
			|| !CreateProjectionParms( perspectiveParms, winding, projectionOrigin, false, fadeDepth, tr.defaultMaterial, 1 ) )
	{
		common->Printf( "decalSelfTest: CreateProjectionParms failed\n" );
		return;
	}
	parallelParms.force = true;			// the test surface's material does not have to allow overlays
	perspectiveParms.force = true;

	R_DecalSelfTestPrintParms( "parallel", parallelParms );
	R_DecalSelfTestPrintParms( "perspective", perspectiveParms );

	// a wall at x = 10 facing +x and a floor at z = -8 facing +z in front of it, sharing the
	// wall's bottom edge
	static const idVec3 cornerVerts[6] =
	{
		idVec3( 10.0f, -20.0f, -8.0f ),
		idVec3( 10.0f,  20.0f, -8.0f ),
		idVec3( 10.0f,  20.0f, 20.0f ),
		idVec3( 10.0f, -20.0f, 20.0f ),
		idVec3( 40.0f,  20.0f, -8.0f ),
		idVec3( 40.0f, -20.0f, -8.0f )
	};
	static const triIndex_t cornerIndexes[12] = { 1, 0, 3,  1, 3, 2,  5, 0, 1,  5, 1, 4 };

	srfTriangles_t* tri = R_AllocStaticTriSurf();
	R_AllocStaticTriSurfVerts( tri, 6 );
	R_AllocStaticTriSurfIndexes( tri, 12 );
	tri->numVerts = 6;
	tri->numIndexes = 12;
	for( int i = 0; i < 6; i++ )
	{
		tri->verts[i].Clear();
		tri->verts[i].xyz = base + cornerVerts[i];
	}
	memcpy( tri->indexes, cornerIndexes, sizeof( cornerIndexes ) );
	R_BoundTriSurf( tri );

	// the model owns the geometry from here on
	idRenderModelStatic* model = new( TAG_MODEL ) idRenderModelStatic;
	model->InitEmpty( "_decalSelfTest" );
	modelSurface_t surface;
	surface.id = 0;
	surface.shader = tr.defaultMaterial;
	surface.geometry = tri;
	model->AddSurface( surface );

	idRenderModelDecal* decalModel = new( TAG_MODEL ) idRenderModelDecal();
	decalModel->CreateDecal( model, parallelParms );
	decalModel->CreateDecal( model, perspectiveParms );

	int totalVerts = 0;
	int totalIndexes = 0;
	for( unsigned int i = decalModel->firstDecal; i < decalModel->nextDecal; i++ )
	{
		const decal_t& decal = decalModel->decals[i & ( MAX_DECALS - 1 )];

		common->Printf( "decal %u: startTime %i, %i verts, %i indexes\n", i, decal.startTime, decal.numVerts, decal.numIndexes );
		for( int j = 0; j < decal.numVerts; j++ )
		{
			const idDrawVert& vert = decal.verts[j];
			const idVec2 st = vert.GetTexCoord();
			common->Printf( "  vert %i: %.6f %.6f %.6f st %.4f %.4f fade %.6f\n", j, vert.xyz[0], vert.xyz[1], vert.xyz[2], st.x, st.y, decal.vertDepthFade[j] );
		}
		for( int j = 0; j + 2 < decal.numIndexes; j += 3 )
		{
			common->Printf( "  tri %i: %i %i %i\n", j / 3, decal.indexes[j + 0], decal.indexes[j + 1], decal.indexes[j + 2] );
		}
		totalVerts += decal.numVerts;
		totalIndexes += decal.numIndexes;
	}
	common->Printf( "decalSelfTest: %i decals, %i verts, %i indexes\n", ( int )( decalModel->nextDecal - decalModel->firstDecal ), totalVerts, totalIndexes );

	delete decalModel;
	delete model;
}


