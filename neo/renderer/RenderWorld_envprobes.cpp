/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2020-2025 Robert Beckebans
Copyright (C) 2022 Stephen Pridham
Copyright (C) 2026 Phillip Melnichenko

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

#include "../libs/mesa/format_r11g11b10f.h"

#include "RenderCommon.h"
#include "CmdlineProgressbar.h"
#include "../framework/Common_local.h" // commonLocal.WaitGameThread();

#include "../libs/mesa/format_r11g11b10f.h"

#include <sys/DeviceManager.h>
extern DeviceManager* deviceManager;

/*
=============
R_SetEnvprobeDefViewEnvprobe

If the envprobeDef is not already on the viewEnvprobe list, create
a viewEnvprobe and add it to the list with an empty scissor rect.
=============
*/
viewEnvprobe_t* R_SetEnvprobeDefViewEnvprobe( RenderEnvprobeLocal* probe )
{
	if( probe->viewCount == tr.viewCount )
	{
		// already set up for this frame
		return probe->viewEnvprobe;
	}
	probe->viewCount = tr.viewCount;

	// add to the view light chain
	viewEnvprobe_t* vProbe = ( viewEnvprobe_t* )R_ClearedFrameAlloc( sizeof( *vProbe ), FRAME_ALLOC_VIEW_LIGHT );
	vProbe->envprobeDef = probe;

	// the scissorRect will be expanded as the envprobe bounds is accepted into visible portal chains
	// and the scissor will be reduced in R_AddSingleEnvprobe based on the screen space projection
	vProbe->scissorRect.Clear();

	// copy data used by backend
	// RB: this would normaly go into R_AddSingleEnvprobe
	// the origin is stored relative to this view's render origin, once; the bounds and the
	// projection stay absolute ( nothing live reads them )
	vProbe->originRel = probe->parms.origin;
	if( !tr.viewDef->renderOrigin.IsZero() )
	{
		vProbe->originRel = ( idVec3d( probe->parms.origin ) - tr.viewDef->renderOrigin ).ToVec3();
	}
	vProbe->globalProbeBounds = probe->globalProbeBounds;
	vProbe->inverseBaseProbeProject = probe->inverseBaseProbeProject;

	//if( probe->irradianceImage->IsLoaded() )
	{
		vProbe->irradianceImage = probe->irradianceImage;
	}
	//else
	//{
	//	vProbe->irradianceImage = globalImages->defaultUACIrradianceCube;
	//}

	//if( probe->radianceImage->IsLoaded() )
	{
		vProbe->radianceImage = probe->radianceImage;
	}
	//else
	//{
	//	vProbe->radianceImage = globalImages->defaultUACRadianceCube;
	//}

	// link the view light
	vProbe->next = tr.viewDef->viewEnvprobes;
	tr.viewDef->viewEnvprobes = vProbe;

	probe->viewEnvprobe = vProbe;

	return vProbe;
}


/*
================
CullEnvprobeByPortals

Return true if the light frustum does not intersect the current portal chain.
================
*/
bool idRenderWorldLocal::CullEnvprobeByPortals( const RenderEnvprobeLocal* probe, const portalStack_t* ps )
{
	if( r_useLightPortalCulling.GetInteger() == 1 )
	{
		ALIGNTYPE16 frustumCorners_t corners;
		idRenderMatrix::GetFrustumCorners( corners, probe->inverseBaseProbeProject, bounds_zeroOneCube );
		for( int i = 0; i < ps->numPortalPlanes; i++ )
		{
			if( idRenderMatrix::CullFrustumCornersToPlane( corners, ps->portalPlanes[i] ) == FRUSTUM_CULL_FRONT )
			{
				return true;
			}
		}

	}

	return false;
}

/*
===================
AddAreaViewEnvprobes

This is the only point where lights get added to the viewLights list.
Any lights that are visible through the current portalStack will have their scissor rect updated.
===================
*/
void idRenderWorldLocal::AddAreaViewEnvprobes( int areaNum, const portalStack_t* ps )
{
	portalArea_t* area = &portalAreas[ areaNum ];

	for( areaReference_t* lref = area->envprobeRefs.areaNext; lref != &area->envprobeRefs; lref = lref->areaNext )
	{
		RenderEnvprobeLocal* probe = lref->envprobe;

		// debug tool to allow viewing of only one light at a time
		if( r_singleEnvprobe.GetInteger() >= 0 && r_singleEnvprobe.GetInteger() != probe->index )
		{
			continue;
		}

#if 0
		// check for being closed off behind a door
		// a light that doesn't cast shadows will still light even if it is behind a door
		if( r_useLightAreaCulling.GetBool() //&& !envprobe->LightCastsShadows()
				&& probe->areaNum != -1 && !tr.viewDef->connectedAreas[ probe->areaNum ] )
		{
			continue;
		}

		// cull frustum
		if( CullEnvprobeByPortals( probe, ps ) )
		{
			// we are culled out through this portal chain, but it might
			// still be visible through others
			continue;
		}
#endif

		viewEnvprobe_t* vProbe = R_SetEnvprobeDefViewEnvprobe( probe );

		// expand the scissor rect
		vProbe->scissorRect.Union( ps->rect );
	}
}

/*
==================
R_SampleCubeMapHDR
==================
*/
void R_SampleCubeMapHDR( const idVec3& dir, int size, byte* buffers[6], float result[3], float& u, float& v )
{
	float	adir[3];
	int		axis, x, y;

	adir[0] = fabs( dir[0] );
	adir[1] = fabs( dir[1] );
	adir[2] = fabs( dir[2] );

	if( dir[0] >= adir[1] && dir[0] >= adir[2] )
	{
		axis = 0;
	}
	else if( -dir[0] >= adir[1] && -dir[0] >= adir[2] )
	{
		axis = 1;
	}
	else if( dir[1] >= adir[0] && dir[1] >= adir[2] )
	{
		axis = 2;
	}
	else if( -dir[1] >= adir[0] && -dir[1] >= adir[2] )
	{
		axis = 3;
	}
	else if( dir[2] >= adir[1] && dir[2] >= adir[2] )
	{
		axis = 4;
	}
	else
	{
		axis = 5;
	}

	float	fx = ( dir * tr.cubeAxis[axis][1] ) / ( dir * tr.cubeAxis[axis][0] );
	float	fy = ( dir * tr.cubeAxis[axis][2] ) / ( dir * tr.cubeAxis[axis][0] );

	fx = -fx;
	fy = -fy;
	x = size * 0.5 * ( fx + 1 );
	y = size * 0.5 * ( fy + 1 );
	if( x < 0 )
	{
		x = 0;
	}
	else if( x >= size )
	{
		x = size - 1;
	}
	if( y < 0 )
	{
		y = 0;
	}
	else if( y >= size )
	{
		y = size - 1;
	}

	u = x;
	v = y;

	// unpack RGBA8 to 3 floats
	union
	{
		uint32	i;
		byte	b[4];
	} tmp;

	tmp.b[0] = buffers[axis][( y * size + x ) * 4 + 0];
	tmp.b[1] = buffers[axis][( y * size + x ) * 4 + 1];
	tmp.b[2] = buffers[axis][( y * size + x ) * 4 + 2];
	tmp.b[3] = buffers[axis][( y * size + x ) * 4 + 3];

	//uint32_t value = ( *( const uint32_t* )buffers[axis][( y * size + x ) * 4 + 0] );

	r11g11b10f_to_float3( tmp.i, result );
}

//puncho - concurrency TB1c: the texel choice of R_SampleCubeMapHDR16F, moved out unchanged, so that the light-grid
// bake's table ( R_BuildLightGridShTable ) picks the texel this sampler reads
void R_CubeMapTexelHDR16F( const idVec3& dir, int size, int& axis, int& x, int& y )
{
	float	adir[3];

	adir[0] = fabs( dir[0] );
	adir[1] = fabs( dir[1] );
	adir[2] = fabs( dir[2] );

	if( dir[0] >= adir[1] && dir[0] >= adir[2] )
	{
		axis = 0;
	}
	else if( -dir[0] >= adir[1] && -dir[0] >= adir[2] )
	{
		axis = 1;
	}
	else if( dir[1] >= adir[0] && dir[1] >= adir[2] )
	{
		axis = 2;
	}
	else if( -dir[1] >= adir[0] && -dir[1] >= adir[2] )
	{
		axis = 3;
	}
	else if( dir[2] >= adir[1] && dir[2] >= adir[2] )
	{
		axis = 4;
	}
	else
	{
		axis = 5;
	}

	float	fx = ( dir * tr.cubeAxis[axis][1] ) / ( dir * tr.cubeAxis[axis][0] );
	float	fy = ( dir * tr.cubeAxis[axis][2] ) / ( dir * tr.cubeAxis[axis][0] );

	fx = -fx;
	fy = -fy;
	x = size * 0.5 * ( fx + 1 );
	y = size * 0.5 * ( fy + 1 );
	if( x < 0 )
	{
		x = 0;
	}
	else if( x >= size )
	{
		x = size - 1;
	}
	if( y < 0 )
	{
		y = 0;
	}
	else if( y >= size )
	{
		y = size - 1;
	}

}

void R_SampleCubeMapHDR16F( const idVec3& dir, int size, halfFloat_t* buffers[6], float result[3], float& u, float& v )
{
	int		axis, x, y;

	R_CubeMapTexelHDR16F( dir, size, axis, x, y );

	u = x;
	v = y;

	// unpack RGB16F to 3 floats
	result[0] = F16toF32( buffers[axis][( y * size + x ) * 3 + 0] );
	result[1] = F16toF32( buffers[axis][( y * size + x ) * 3 + 1] );
	result[2] = F16toF32( buffers[axis][( y * size + x ) * 3 + 2] );
}


/*
==================
R_SampleCubeMapHDR16F_Bilinear

Same axis selection and projection as R_SampleCubeMapHDR16F, but interpolates the
four texels around the sample point instead of snapping to the nearest one. Edges
clamp to the face border (cross-face filtering is left to a future pass; at
ENVPROBE_CAPTURE_SIZE the 1-texel border error is below BC6H quantization).
==================
*/
static void R_SampleCubeMapHDR16F_Bilinear(const idVec3& dir, int size, halfFloat_t* buffers[6], float result[3], const float* halfToFloat)
{
	float	adir[3];
	int		axis;

	adir[0] = fabs(dir[0]);
	adir[1] = fabs(dir[1]);
	adir[2] = fabs(dir[2]);

	if (dir[0] >= adir[1] && dir[0] >= adir[2])
	{
		axis = 0;
	}
	else if (-dir[0] >= adir[1] && -dir[0] >= adir[2])
	{
		axis = 1;
	}
	else if (dir[1] >= adir[0] && dir[1] >= adir[2])
	{
		axis = 2;
	}
	else if (-dir[1] >= adir[0] && -dir[1] >= adir[2])
	{
		axis = 3;
	}
	else if (dir[2] >= adir[1] && dir[2] >= adir[2])
	{
		axis = 4;
	}
	else
	{
		axis = 5;
	}

	const float denom = dir * tr.cubeAxis[axis][0];
	float fx = (dir * tr.cubeAxis[axis][1]) / denom;
	float fy = (dir * tr.cubeAxis[axis][2]) / denom;

	fx = -fx;
	fy = -fy;

	// continuous texel-center coordinates (the -0.5 moves us into center space)
	const float xf = size * 0.5f * (fx + 1.0f) - 0.5f;
	const float yf = size * 0.5f * (fy + 1.0f) - 0.5f;

	int x0 = (int)idMath::Floor(xf);
	int y0 = (int)idMath::Floor(yf);

	// fractions are computed from the UNCLAMPED corner, before edge clamping
	const float tx = xf - (float)x0;
	const float ty = yf - (float)y0;

	int x1 = x0 + 1;
	int y1 = y0 + 1;

	// within-face bilinear: clamp the four taps to the face border
	x0 = idMath::ClampInt(0, size - 1, x0);
	x1 = idMath::ClampInt(0, size - 1, x1);
	y0 = idMath::ClampInt(0, size - 1, y0);
	y1 = idMath::ClampInt(0, size - 1, y1);

	const halfFloat_t* buf = buffers[axis];

	const int i00 = (y0 * size + x0) * 3;
	const int i10 = (y0 * size + x1) * 3;
	const int i01 = (y1 * size + x0) * 3;
	const int i11 = (y1 * size + x1) * 3;

	//puncho start - concurrency TB1a: inside a bake the half table, built from F16toF32 itself; the interpolation
	// is today's
	if (halfToFloat != NULL)
	{
		for (int c = 0; c < 3; c++)
		{
			const float c00 = halfToFloat[buf[i00 + c]];
			const float c10 = halfToFloat[buf[i10 + c]];
			const float c01 = halfToFloat[buf[i01 + c]];
			const float c11 = halfToFloat[buf[i11 + c]];

			const float a = c00 + (c10 - c00) * tx;
			const float b = c01 + (c11 - c01) * tx;
			result[c] = a + (b - a) * ty;
		}
	}
	else	// bakeEnvironmentProbes -exactHalf: today's loop
	//puncho end
	for (int c = 0; c < 3; c++)
	{
		const float c00 = F16toF32(buf[i00 + c]);
		const float c10 = F16toF32(buf[i10 + c]);
		const float c01 = F16toF32(buf[i01 + c]);
		const float c11 = F16toF32(buf[i11 + c]);

		const float a = c00 + (c10 - c00) * tx;
		const float b = c01 + (c11 - c01) * tx;
		result[c] = a + (b - a) * ty;
	}
}



// http://holger.dammertz.org/stuff/notes_HammersleyOnHemisphere.html

// To implement the Hammersley point set we only need an efficent way to implement the Van der Corput radical inverse phi2(i).
// Since it is in base 2 we can use some basic bit operations to achieve this.
// The brilliant book Hacker's Delight [warren01] provides us a a simple way to reverse the bits in a given 32bit integer. Using this, the following code then implements phi2(i)


// RB: radical inverse implementation from the Mitsuba PBR system

// Van der Corput radical inverse in base 2 with single precision
inline float RadicalInverse_VdC( uint32_t n, uint32_t scramble = 0U )
{
	/* Efficiently reverse the bits in 'n' using binary operations */
#if (defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 2))) || defined(__clang__)
	n = __builtin_bswap32( n );
#else
	n = ( n << 16 ) | ( n >> 16 );
	n = ( ( n & 0x00ff00ff ) << 8 ) | ( ( n & 0xff00ff00 ) >> 8 );
#endif
	n = ( ( n & 0x0f0f0f0f ) << 4 ) | ( ( n & 0xf0f0f0f0 ) >> 4 );
	n = ( ( n & 0x33333333 ) << 2 ) | ( ( n & 0xcccccccc ) >> 2 );
	n = ( ( n & 0x55555555 ) << 1 ) | ( ( n & 0xaaaaaaaa ) >> 1 );

	// Account for the available precision and scramble
	n = ( n >> ( 32 - 24 ) ) ^ ( scramble & ~ -( 1 << 24 ) );

	return ( float ) n / ( float )( 1U << 24 );
}

// The ith point xi is then computed by
inline idVec2 Hammersley2D( uint i, uint N )
{
	return idVec2( float( i ) / float( N ), RadicalInverse_VdC( i ) );
}

idVec3 ImportanceSampleGGX( const idVec2& Xi, const idVec3& N, float roughness )
{
	float a = roughness * roughness;

	// cosinus distributed direction (Z-up or tangent space) from the hammersley point xi
	float Phi = 2 * idMath::PI * Xi.x;
	float cosTheta = idMath::Sqrt( ( 1 - Xi.y ) / ( 1 + ( a * a - 1 ) * Xi.y ) );
	float sinTheta = idMath::Sqrt( 1 - cosTheta * cosTheta );

	idVec3 H;
	H.x = sinTheta * idMath::Cos( Phi );
	H.y = sinTheta * idMath::Sin( Phi );
	H.z = cosTheta;

	// rotate from tangent space to world space along N
	idVec3 upVector = abs( N.z ) < 0.999f ? idVec3( 0, 0, 1 ) : idVec3( 1, 0, 0 );
	idVec3 tangentX = upVector.Cross( N );
	tangentX.Normalize();
	idVec3 tangentY = N.Cross( tangentX );

	idVec3 sampleVec = tangentX * H.x + tangentY * H.y + N * H.z;
	sampleVec.Normalize();

	return sampleVec;
}

/*
==================
ImportanceSampleGGXTangent, R_BuildGGXSampleTable

//puncho - concurrency TB1b: the first half of ImportanceSampleGGX, its lines copied unchanged: the tangent-space H
depends only on the sample and the roughness, so a bake tabulates it once. CalculateRadianceJob does the second half,
the frame of N, once a texel with the same lines. Entry [( mip - 1 ) * samples + s] is H for mip 1 .. numMips - 1, with
CalculateRadianceJob's roughness for that mip and its Hammersley point for sample s.
==================
*/
static idVec3 ImportanceSampleGGXTangent( const idVec2& Xi, float roughness )
{
	float a = roughness * roughness;

	// cosinus distributed direction (Z-up or tangent space) from the hammersley point xi
	float Phi = 2 * idMath::PI * Xi.x;
	float cosTheta = idMath::Sqrt( ( 1 - Xi.y ) / ( 1 + ( a * a - 1 ) * Xi.y ) );
	float sinTheta = idMath::Sqrt( 1 - cosTheta * cosTheta );

	idVec3 H;
	H.x = sinTheta * idMath::Cos( Phi );
	H.y = sinTheta * idMath::Sin( Phi );
	H.z = cosTheta;

	return H;
}

// RB3-01: the caller allocates the table, ( numMips - 1 ) * samples entries, and records it in the bake context first
static void R_BuildGGXSampleTable( idVec3* table, int numMips, int samples )
{
	for( int mip = 1; mip < numMips; mip++ )
	{
		const float roughness = (numMips > 1) ? ((float)mip / (float)(numMips - 1)) : 0.0f;

		for( int s = 0; s < samples; s++ )
		{
			idVec2 Xi = Hammersley2D(s, samples);
			table[( mip - 1 ) * samples + s] = ImportanceSampleGGXTangent( Xi, roughness );
		}
	}
}

float Geometry_SchlickGGX( float NdotV, float roughness )
{
	// note that we use a different k for IBL
	float a = roughness;
	float k = ( a * a ) / 2.0;

	float nom = NdotV;
	float denom = NdotV * ( 1.0 - k ) + k;

	return nom / denom;
}

float Geometry_Smith( idVec3 N, idVec3 V, idVec3 L, float roughness )
{
	float NdotV = Max( ( N * V ), 0.0f );
	float NdotL = Max( ( N * L ), 0.0f );

	float ggx2 = Geometry_SchlickGGX( NdotV, roughness );
	float ggx1 = Geometry_SchlickGGX( NdotL, roughness );

	return ggx1 * ggx2;
}

idVec2 IntegrateBRDF( float NdotV, float roughness, int sampleCount )
{
	idVec3 V;
	V.x = sqrt( 1.0 - NdotV * NdotV );
	V.y = 0.0;
	V.z = NdotV;

	float A = 0.0;
	float B = 0.0;

	idVec3 N( 0.0f, 0.0f, 1.0f );
	for( int i = 0; i < sampleCount; ++i )
	{
		// generates a sample vector that's biased towards the
		// preferred alignment direction (importance sampling).
		idVec2 Xi = Hammersley2D( i, sampleCount );

		idVec3 H = ImportanceSampleGGX( Xi, N, roughness );
		idVec3 L = ( 2.0 * ( V * H ) * H - V );
		L.Normalize();

		float NdotL = Max( L.z, 0.0f );
		float NdotH = Max( H.z, 0.0f );
		float VdotH = Max( ( V * H ), 0.0f );

		if( NdotL > 0.0 )
		{
			float G = Geometry_Smith( N, V, L, roughness );
			float G_Vis = ( G * VdotH ) / ( NdotH * NdotV );
			float Fc = idMath::Pow( 1.0 - VdotH, 5.0 );

			A += ( 1.0 - Fc ) * G_Vis;
			B += Fc * G_Vis;
		}
	}

	A /= float( sampleCount );
	B /= float( sampleCount );

	return idVec2( A, B );
}


// Compute normalized oct coord, mapping top left of top left pixel to (-1,-1)
idVec2 NormalizedOctCoord( int x, int y, const int probeWithBorderSide )
{
#if 0
	// 1 pixel border
	const int margin = 1;

	int probeSideLength = Max( 2, probeWithBorderSide - ( margin * 2 ) );

	idVec2 octFragCoord;
	octFragCoord.x = idMath::ClampInt( 0, probeSideLength - 1, x - margin );
	octFragCoord.y = idMath::ClampInt( 0, probeSideLength - 1, y - margin );

	return ( idVec2( octFragCoord ) ) * ( 2.0f / float( probeSideLength ) ) - idVec2( 1.0f, 1.0f );
#else

	const int margin = 2;

	// RB: FIXME - margin * 2 is wrong but looks better
	// figure out why
	int probeSideLength = Max( 2, probeWithBorderSide - ( margin * 2 ) );

	idVec2 octFragCoord = idVec2( ( x - margin ) % probeWithBorderSide, ( y - margin ) % probeWithBorderSide );

	// Add back the half pixel to get pixel center normalized coordinates
	return ( idVec2( octFragCoord ) + idVec2( 0.5f, 0.5f ) ) * ( 2.0f / float( probeSideLength ) ) - idVec2( 1.0f, 1.0f );

#endif
}

static inline idVec2 NormalizedOctCoordNoBorder( int x, int y, const int probeWithBorderSide )
{
	int probeSideLength = probeWithBorderSide;

	idVec2 octFragCoord = idVec2( x % probeWithBorderSide, y % probeWithBorderSide );

	// Add back the half pixel to get pixel center normalized coordinates
	return ( idVec2( octFragCoord ) + idVec2( 0.5f, 0.5f ) ) * ( 2.0f / float( probeSideLength ) ) - idVec2( 1.0f, 1.0f );
}

/*
static inline float LatLongTexelArea( const idVec2i& pos, const idVec2i& imageSize )
{
	idVec2 uv0;
	uv0.x = pos.x / imageSize.x;
	uv0.y = pos.y / imageSize.y;

	idVec2 uv1;
	uv1.x = ( pos.x + 1 ) / imageSize.x;
	uv1.y = ( pos.y + 1 ) / imageSize.y;

	float theta0 = idMath::PI * ( uv0.x * 2.0f - 1.0f );
	float theta1 = idMath::PI * ( uv1.x * 2.0f - 1.0f );

	float phi0 = idMath::PI * ( uv0.y - 0.5f );
	float phi1 = idMath::PI * ( uv1.y - 0.5f );

	return abs( theta1 - theta0 ) * abs( sin( phi1 ) - sin( phi0 ) );
}


static inline idVec2 CartesianToLatLongTexcoord( const idVec3& p )
{
	// http://gl.ict.usc.edu/Data/HighResProbes

	float u = ( 1.0f + idMath::ATan( p.x, -p.z ) / idMath::PI );
	float v = idMath::ACos( p.y ) / idMath::PI;

	return idVec2( u * 0.5f, v );
}
*/


/// http://www.mpia-hd.mpg.de/~mathar/public/mathar20051002.pdf
/// http://www.rorydriscoll.com/2012/01/15/cubemap-texel-solid-angle/
static inline float AreaElement( float _x, float _y )
{
	return atan2f( _x * _y, sqrtf( _x * _x + _y * _y + 1.0f ) );
}

/// u and v should be center adressing and in [-1.0 + invSize.. 1.0 - invSize] range.
static inline float CubemapTexelSolidAngle( float u, float v, float _invFaceSize )
{
	// Specify texel area.
	const float x0 = u - _invFaceSize;
	const float x1 = u + _invFaceSize;
	const float y0 = v - _invFaceSize;
	const float y1 = v + _invFaceSize;

	// Compute solid angle of texel area.
	const float solidAngle = AreaElement( x1, y1 )
							 - AreaElement( x0, y1 )
							 - AreaElement( x1, y0 )
							 + AreaElement( x0, y0 )
							 ;

	return solidAngle;
}

static inline idVec3 MapXYSToDirection( uint64 x, uint64 y, uint64 s, uint64 width, uint64 height )
{
	float u = ( ( x + 0.5f ) / float( width ) ) * 2.0f - 1.0f;
	float v = ( ( y + 0.5f ) / float( height ) ) * 2.0f - 1.0f;
	v *= -1.0f;

	idVec3 dir( 0, 0, 0 );

	// +x, -x, +y, -y, +z, -z
	switch( s )
	{
		case 0:
			dir = idVec3( 1.0f, v, -u );
			break;
		case 1:
			dir = idVec3( -1.0f, v, u );
			break;
		case 2:
			dir = idVec3( u, 1.0f, -v );
			break;
		case 3:
			dir = idVec3( u, -1.0f, v );
			break;
		case 4:
			dir = idVec3( u, v, 1.0f );
			break;
		case 5:
			dir = idVec3( -u, v, -1.0f );
			break;
	}

	dir.Normalize();

	return dir;
}

void CalculateIrradianceJob( calcEnvprobeParms_t* parms )
{
	halfFloat_t*		buffers[6];

	int	start = Sys_Milliseconds();

	for( int i = 0; i < 6; i++ )
	{
		buffers[ i ] = ( halfFloat_t* ) parms->radiance[ i ];
	}

	const float invDstSize = 1.0f / float( ENVPROBE_CAPTURE_SIZE );
	const idVec2i sourceImageSize( ENVPROBE_CAPTURE_SIZE, ENVPROBE_CAPTURE_SIZE );

	CommandlineProgressBar progressBar( R_CalculateUsedAtlasPixels( parms->outHeight ), parms->printWidth, parms->printHeight );
	if( parms->printProgress )
	{
		progressBar.Start();
	}

	// build L4 Spherical Harmonics from source image
	SphericalHarmonicsT<idVec3, 4> shRadiance;

	for( int i = 0; i < shSize( 4 ); i++ )
	{
		shRadiance[i].Zero();
	}

	// build SH by iterating over all cubemap pixels
	idVec4 dstRect = R_CalculateMipRect( parms->outHeight, 0 );

	for( int side = 0; side < 6; side++ )
	{
		for( int x = 0; x < sourceImageSize.x; x++ )
		{
			for( int y = 0; y < sourceImageSize.y; y++ )
			{
				// convert UV coord to 3D direction
				idVec3 dir = MapXYSToDirection( x, y, side, sourceImageSize.x, sourceImageSize.y );

				float u, v;
				idVec3 radiance;
				R_SampleCubeMapHDR16F( dir, ENVPROBE_CAPTURE_SIZE, buffers, &radiance[0], u, v );

				const float uu = 2.0f * ( u * invDstSize ) - 1.0f;
				const float vv = 2.0f * ( v * invDstSize ) - 1.0f;

				float texelArea = CubemapTexelSolidAngle( uu, vv, invDstSize );

				const SphericalHarmonicsT<float, 4>& sh = shEvaluate<4>( dir );

				bool shValid = true;
				for( int i = 0; i < shSize( 4 ); i++ )
				{
					if( IsNAN( sh[i] ) )
					{
						shValid = false;
						break;
					}
				}

				if( shValid )
				{
					shAddWeighted( shRadiance, sh, radiance * texelArea );
				}
			}
		}
	}

	for (int i = 0; i < 9; i++)
	{
		parms->shIrradiance[i] = shRadiance[i];
	}

	// reset image to black
	for( int x = 0; x < parms->outWidth; x++ )
	{
		for( int y = 0; y < parms->outHeight; y++ )
		{
			parms->outBuffer[( y * parms->outWidth + x ) * 3 + 0] = F32toF16( 0 );
			parms->outBuffer[( y * parms->outWidth + x ) * 3 + 1] = F32toF16( 0 );
			parms->outBuffer[( y * parms->outWidth + x ) * 3 + 2] = F32toF16( 0 );
		}
	}

	const int numMips = idMath::BitsForInteger( parms->outHeight );

	for( int mip = 0; mip < numMips; mip++ )
	{
		idVec4 dstRect = R_CalculateMipRect( parms->outHeight, mip );

		for( int x = dstRect.x; x < ( dstRect.x + dstRect.z ); x++ )
		{
			for( int y = dstRect.y; y < ( dstRect.y + dstRect.w ); y++ )
			{
				idVec2 octCoord;
				if( mip > 0 )
				{
					// move back to [0, 1] coords
					octCoord = NormalizedOctCoordNoBorder( x - dstRect.x, y - dstRect.y, dstRect.z );
				}
				else
				{
					octCoord = NormalizedOctCoordNoBorder( x, y, dstRect.z );
				}

				// convert UV coord to 3D direction
				idVec3 dir;

				dir.FromOctahedral( octCoord );

				idVec3 outColor( 0, 0, 0 );

#if 1
				// generate ambient colors by evaluating the L4 Spherical Harmonics
				SphericalHarmonicsT<float, 4> shDirection = shEvaluate<4>( dir );

				idVec3 sampleIrradianceSh = shEvaluateDiffuse<idVec3, 4>( shRadiance, dir ) / idMath::PI;

				outColor[0] = Max( 0.0f, sampleIrradianceSh.x );
				outColor[1] = Max( 0.0f, sampleIrradianceSh.y );
				outColor[2] = Max( 0.0f, sampleIrradianceSh.z );
#else
				// generate ambient colors using Monte Carlo method
				for( int s = 0; s < parms->samples; s++ )
				{
					idVec2 Xi = Hammersley2D( s, parms->samples );
					idVec3 H = ImportanceSampleGGX( Xi, dir, 0.95f );

					float u, v;
					idVec3 radiance;
					R_SampleCubeMapHDR( L, parms->outHeight, buffers, &radiance[0], u, v );

					outColor[0] += radiance[0];
					outColor[1] += radiance[1];
					outColor[2] += radiance[2];
				}

				outColor[0] /= parms->samples;
				outColor[1] /= parms->samples;
				outColor[2] /= parms->samples;
#endif

				//outColor = dir * 0.5 + idVec3( 0.5f, 0.5f, 0.5f );
				parms->outBuffer[( y * parms->outWidth + x ) * 3 + 0] = F32toF16( outColor[0] );
				parms->outBuffer[( y * parms->outWidth + x ) * 3 + 1] = F32toF16( outColor[1] );
				parms->outBuffer[( y * parms->outWidth + x ) * 3 + 2] = F32toF16( outColor[2] );

				if( parms->printProgress )
				{
					progressBar.Increment( true );
				}
			}
		}
	}

	int	end = Sys_Milliseconds();

	parms->time = end - start;
}

void CalculateRadianceJob(calcEnvprobeParms_t* parms)
{
	halfFloat_t* buffers[6];

	int	start = Sys_Milliseconds();

	for (int i = 0; i < 6; i++)
	{
		buffers[i] = (halfFloat_t*)parms->radiance[i];	// captured faces (ENVPROBE_CAPTURE_SIZE)
	}

	// parms->outHeight is now the cube face size, cube faces get hardware seam filtering,
	// all mips are usable
	const int faceSize = parms->outHeight;
	const int numMips = idMath::BitsForInteger(faceSize);

	CommandlineProgressBar progressBar(faceSize * faceSize * 6, parms->printWidth, parms->printHeight);
	if (parms->printProgress)
	{
		progressBar.Start();
	}

	for (int mip = 0; mip < numMips; mip++)
	{
		const float roughness = (numMips > 1) ? ((float)mip / (float)(numMips - 1)) : 0.0f;
		const int   mipSize = Max(1, faceSize >> mip);

		for (int face = 0; face < 6; face++)
		{
			for (int y = 0; y < mipSize; y++)
			{
				for (int x = 0; x < mipSize; x++)
				{
					// cube-face texel -> world direction (replaces FromOctahedral)
					idVec3 N = MapXYSToDirection(x, y, face, mipSize, mipSize);

					// Epic split-sum: V = R = N (unchanged from the octahedral path)
					const idVec3 R = N;
					const idVec3 V = R;

					idVec3 outColor(0, 0, 0);

					if (mip == 0)
					{
						// 'samples'-tap weighted average collapses to a single tap along N.
						float sample[3];
						R_SampleCubeMapHDR16F_Bilinear(N, ENVPROBE_CAPTURE_SIZE, buffers, sample, parms->halfToFloat);

						outColor[0] = sample[0];
						outColor[1] = sample[1];
						outColor[2] = sample[2];
					}
					else
					{
						const idVec3 R = N;
						const idVec3 V = R;
						float totalWeight = 0.0f;

						//puncho start - concurrency TB1b: with the bake's table, ImportanceSampleGGX in its two halves:
						// H from the table, and the frame of N once a texel, the same lines as ImportanceSampleGGX
						const idVec3* ggxH = ( parms->ggxH != NULL ) ? &parms->ggxH[( mip - 1 ) * parms->samples] : NULL;
						idVec3 tangentX;
						idVec3 tangentY;
						if( ggxH != NULL )
						{
							idVec3 upVector = abs( N.z ) < 0.999f ? idVec3( 0, 0, 1 ) : idVec3( 1, 0, 0 );
							tangentX = upVector.Cross( N );
							tangentX.Normalize();
							tangentY = N.Cross( tangentX );
						}
						//puncho end

						for (int s = 0; s < parms->samples; s++)
						{
							//puncho start - concurrency TB1b
							idVec3 H;
							if( ggxH != NULL )
							{
								H = tangentX * ggxH[s].x + tangentY * ggxH[s].y + N * ggxH[s].z;
								H.Normalize();
							}
							else	// bakeEnvironmentProbes -exactGgx: today's call
							{
								idVec2 Xi = Hammersley2D(s, parms->samples);
								H = ImportanceSampleGGX(Xi, N, roughness);
							}
							//puncho end
							idVec3 L = (2.0 * (H * (V * H)) - V);

							float NdotL = Max((N * L), 0.0f);
							if (NdotL > 0.0)
							{
								float sample[3];
								R_SampleCubeMapHDR16F_Bilinear(L, ENVPROBE_CAPTURE_SIZE, buffers, sample, parms->halfToFloat);

								outColor[0] += sample[0] * NdotL;
								outColor[1] += sample[1] * NdotL;
								outColor[2] += sample[2] * NdotL;
								totalWeight += NdotL;
							}
						}

						outColor /= Max(totalWeight, 1e-4f);
					}

					// write the prefiltered texel into this face+mip's RGB16F buffer,
					// using the same F32toF16 idiom the octahedron path used:
					halfFloat_t* dst = parms->outFaces[face][mip];	// RGB16F, mipSize x mipSize
					const int    idx = (y * mipSize + x) * 3;
					dst[idx + 0] = F32toF16(outColor[0]);
					dst[idx + 1] = F32toF16(outColor[1]);
					dst[idx + 2] = F32toF16(outColor[2]);

					if (parms->printProgress)
					{
						progressBar.Increment(true);
					}
				}
			}
		}
	}

	int	end = Sys_Milliseconds();
	parms->time = end - start;
}

//puncho - concurrency TB1b: the radiance job's sample count, which R_BuildGGXSampleTable tabulates
static const int ENVPROBE_RADIANCE_SAMPLES = 1000;

REGISTER_PARALLEL_JOB( CalculateIrradianceJob, "CalculateIrradianceJob" );
REGISTER_PARALLEL_JOB( CalculateRadianceJob, "CalculateRadianceJob" );

// The farthest probe distance: what the decode reports for a pixel with no geometry ( the depth
// clear ) and for anything farther, and the G channel's scale. The R channel, the linear distance
// the IBL shaders read, is R11F, which saturates at 65024 ( f32_to_uf11 ), so nothing farther can be
// stored anyway. Nothing reads the G channel ( ( distance / PROBE_DEPTH_FAR )^2 ) at runtime.
static const float PROBE_DEPTH_FAR = 65024.0f;

//puncho - camera-relative WP-CRV: not static, so dumpProbeDepth (RenderVerify.cpp) prints the bake's own decode
// zNear is the near plane R_SetupProjectionMatrix gave the capture ( r_znear, a quarter of it with cramZNear )
float LinearizeProbeDepth(float hwDepth, float zNear, int face, int pixel, int size)
{
	// the exact inverse of the capture's infinite-far projection; the threshold also catches the
	// depth clear, which the half-float read-back cannot tell from the farthest geometry
#if ID_REVERSED_Z
	// reversed: ndc z = zNear / distance, cleared to 0
	const float zEye = ( hwDepth * PROBE_DEPTH_FAR > zNear ) ? ( zNear / hwDepth ) : PROBE_DEPTH_FAR;
#else
	// forward with the -0.999 crunch: ndc z = 0.999 - zNear / distance, cleared to 1
	const float farDepth = 0.999f - hwDepth;
	const float zEye = ( farDepth * PROBE_DEPTH_FAR > zNear ) ? ( zNear / farDepth ) : PROBE_DEPTH_FAR;
#endif

	int    x = pixel % size, y = pixel / size;
	idVec3 dir = MapXYSToDirection(x, y, face, size, size);
	float  cosTheta = dir * tr.cubeAxis[face][0];	// [0] = face forward
	if (cosTheta < 1e-4f)
	{
		cosTheta = 1e-4f;
	}
	return zEye / cosTheta;		// world units from the probe center
}

//puncho - bake case (R0): an .exr bake is read back through R_LoadImage, which lower-cases the whole path, and a
// case-sensitive filesystem folds only the file name ( OpenOSFile ), never a directory: the bake is written, listed and
// deleted under the lower-cased name or a mixed-case map never finds it. A .bimage bake keeps its image's own name,
// which idBinaryImage looks up unfolded
void R_BakeImageName( idStr& name )
{
	name.ToLower();
}

//puncho - concurrency TB1: halfToFloat and ggxH are the bake's tables ( bakeContext_t ), NULL for today's code
//puncho - concurrency TB2: jobList is the list the job is added to when useThreads; NULL leaves that to the caller
// ( the bake's streaming adds a batch's jobs at its Submit, so a list never holds jobs it has not submitted, RA2-01 )
void R_MakeAmbientMap( const char* baseName, byte* buffers[6], const char* suffix, int outSize, bool specular, bool useThreads, int probeIndex, const float* halfToFloat, const idVec3* ggxH, idParallelJobList* jobList )
{
	idStr		fullname;
	renderView_t	ref;
	viewDef_t	primary;
	//byte*		buffers[6];
	//int			width = 0, height = 0;

	// set up the job
	calcEnvprobeParms_t* jobParms = new calcEnvprobeParms_t;

	for( int i = 0; i < 6; i++ )
	{
		jobParms->radiance[ i ] = buffers[ i ];
	}

	jobParms->freeRadiance = specular ? 1 : 0;

	jobParms->samples = ENVPROBE_RADIANCE_SAMPLES;
	jobParms->probeIndex = probeIndex;
	jobParms->halfToFloat = halfToFloat;
	jobParms->ggxH = specular ? ggxH : NULL;
	jobParms->filename.Format( "env/%s%s.exr", baseName, suffix );

	jobParms->printProgress = !useThreads;
	jobParms->printWidth = renderSystem->GetWidth();
	jobParms->printHeight = renderSystem->GetHeight();

	jobParms->outWidth = int(outSize * 1.5f);		
	jobParms->outHeight = outSize;					

	if (specular)	// radiance -> GGX cube
	{
		jobParms->outBuffer = NULL;												// unused; the cube writes outFaces
		const int numMips = idMath::BitsForInteger(outSize);					// 512 -> 10, 256 -> 9
		for (int mip = 0, m = outSize; mip < numMips; mip++, m = Max(1, m >> 1))
		{
			for (int side = 0; side < 6; side++)
			{
				jobParms->outFaces[side][mip] =
					(halfFloat_t*)R_StaticAlloc(m * m * 3 * sizeof(halfFloat_t), TAG_IMAGE);
			}
		}
	}
	else			// irradiance -> packed octahedron (unchanged)
	{
		jobParms->outBuffer =
			(halfFloat_t*)R_StaticAlloc(idMath::Ceil(outSize * outSize * 3 * sizeof(halfFloat_t) * 1.5f), TAG_IMAGE);
	}

	tr.envprobeJobs.Append( jobParms );

	if( useThreads && jobList != NULL )	//puncho - concurrency TB2
	{
		if( specular )
		{
			jobList->AddJob( ( jobRun_t )CalculateRadianceJob, jobParms );
		}
		else
		{
			jobList->AddJob( ( jobRun_t )CalculateIrradianceJob, jobParms );
		}
	}
	else if( !useThreads )	//puncho - concurrency TB2
	{
		if( specular )
		{
			CalculateRadianceJob( jobParms );
		}
		else
		{
			CalculateIrradianceJob( jobParms );
		}
	}
}

/*
==================
R_EnvprobeStreamSubmit, R_EnvprobeStreamRetire

//puncho - concurrency TB2: streaming. Every ENVPROBE_STREAM_BATCH probes the irradiance and specular jobs captured
since the last batch are added to the current list and submitted, and the capture goes on, after the other list's
batch is waited for and its six faces freed. Jobs are added only at the submit ( RA2-01 ). Both jobs of a probe read the same six faces and sit in the
same batch, so the faces are freed only after both have run. Everything else ( outBuffer, outFaces, the parms ) stays
in tr.envprobeJobs for today's finalisation after the capture, which is unchanged; it frees radiance only where it is
still set.
==================
*/
static const int ENVPROBE_STREAM_BATCH = 8;		// probes a batch, two jobs each

struct envprobeStream_t
{
	idParallelJobList*		lists[2];
	int						first[2];		// the batch each list runs: [first, last) of tr.envprobeJobs; first -1: none
	int						last[2];
	int						current;		// the list the capture adds jobs to
	int						batchFirst;	// the first job of tr.envprobeJobs not yet submitted
};

static void R_EnvprobeStreamRetire( envprobeStream_t& stream, int list )
{
	if( stream.first[list] < 0 )
	{
		return;
	}

	stream.lists[list]->Wait();

	// the specular job owns the faces ( freeRadiance ); its irradiance twin holds the same pointers
	for( int j = stream.first[list]; j < stream.last[list]; j++ )
	{
		calcEnvprobeParms_t* job = tr.envprobeJobs[j];
		if( job->freeRadiance > 0 )
		{
			for( int i = 0; i < 6; i++ )
			{
				if( job->radiance[i] )
				{
					Mem_Free( job->radiance[i] );
				}
			}
		}
	}
	for( int j = stream.first[list]; j < stream.last[list]; j++ )
	{
		for( int i = 0; i < 6; i++ )
		{
			tr.envprobeJobs[j]->radiance[i] = NULL;
		}
	}

	stream.first[list] = -1;
}

// submits the jobs added since the last batch, then retires the other list's batch before the capture adds to it
static void R_EnvprobeStreamSubmit( envprobeStream_t& stream, int numThreads )
{
	const int last = tr.envprobeJobs.Num();
	if( last == stream.batchFirst )
	{
		return;
	}

	idParallelJobList* jobList = stream.lists[stream.current];
	for( int j = stream.batchFirst; j < last; j++ )
	{
		calcEnvprobeParms_t* job = tr.envprobeJobs[j];
		jobList->AddJob( ( job->freeRadiance > 0 ) ? ( jobRun_t )CalculateRadianceJob : ( jobRun_t )CalculateIrradianceJob, job );
	}
	jobList->Submit( NULL, numThreads );

	stream.first[stream.current] = stream.batchFirst;
	stream.last[stream.current] = last;
	stream.batchFirst = last;

	stream.current ^= 1;
	R_EnvprobeStreamRetire( stream, stream.current );
}

/*
==================
R_FreeEnvprobeJobs

//puncho - concurrency RA2-01: R_BakeEnd's release of every environment job a bake left in tr.envprobeJobs, after the
job lists are quiet. A specular job ( freeRadiance ) owns the probe's six faces, which its irradiance twin shares, and its
cube mips; an irradiance job owns its outBuffer. Entries the finalisation already deleted are NULL. Empty after a bake
that ended normally.
==================
*/
void R_FreeEnvprobeJobs()
{
	for( int j = 0; j < tr.envprobeJobs.Num(); j++ )
	{
		calcEnvprobeParms_t* job = tr.envprobeJobs[j];
		if( job == NULL )
		{
			continue;
		}

		if( job->freeRadiance > 0 )
		{
			for( int i = 0; i < 6; i++ )
			{
				if( job->radiance[i] )
				{
					Mem_Free( job->radiance[i] );
				}
			}

			const int numMips = idMath::BitsForInteger( job->outHeight );
			for( int side = 0; side < 6; side++ )
			{
				for( int mip = 0; mip < numMips; mip++ )
				{
					if( job->outFaces[side][mip] )
					{
						R_StaticFree( job->outFaces[side][mip] );
					}
				}
			}
		}
		else if( job->outBuffer != NULL )
		{
			Mem_Free( job->outBuffer );
		}

		delete job;
	}

	tr.envprobeJobs.Clear();
}

CONSOLE_COMMAND_SHIP( bakeEnvironmentProbes, "Bake environment probes", NULL )
{
	idStr			fullname;
	idStr			baseName;
	renderView_t	ref;
	int				captureSize;

	if( !tr.primaryWorld )
	{
		common->Printf( "No primary world loaded.\n" );
		return;
	}

	int sysWidth = renderSystem->GetWidth();
	int sysHeight = renderSystem->GetHeight();

	bool useThreads = true;
	int numThreads = JOBLIST_PARALLELISM_MAX_CORES;

	//puncho - concurrency TB1: each switch runs today's code in place of one of the bake's tables
	bool exactHalf = false;
	bool exactGgx = false;
	bool noPreviewBlit = false;		// TB3a, opt-in: do not show each capture on the screen
	bool exactStream = false;		// TB2: capture every probe, then run the jobs, as today

	bool helpRequested = false;
	idStr option;

	for( int i = 1; i < args.Argc(); i++ )
	{
		option = args.Argv( i );
		option.StripLeading( '-' );

		if( option.IcmpPrefix( "mt" ) == 0 )
		{
			option.StripLeading( "mt" );
			int threads = atoi( option );
			if( threads > 0 )
			{
				int maxCores = parallelJobManager->GetLogicalCpuCores();
				numThreads = idMath::ClampInt( 1, maxCores, threads );
			}
		}
		else if( option.Icmp( "exactHalf" ) == 0 )
		{
			exactHalf = true;
		}
		else if( option.Icmp( "exactGgx" ) == 0 )
		{
			exactGgx = true;
		}
		else if( option.Icmp( "noPreviewBlit" ) == 0 )
		{
			noPreviewBlit = true;
		}
		else if( option.Icmp( "exactStream" ) == 0 )
		{
			exactStream = true;
		}
		else if( option.Icmp( "h" ) == 0 || option.Icmp( "help" ) == 0 )
		{
			helpRequested = true;
			break;
		}
	}

	if( helpRequested )
	{
		idLib::Printf( "USAGE: bakeEnvironmentProbes [<switches>...]\n\n" );
		idLib::Printf( "<Switches>\n" );
		idLib::Printf( " mt[num] : number of threads used for baking (default max logical cores)\n" );
		idLib::Printf( " exactHalf : convert half floats with F16toF32 instead of the bake's table\n" );
		idLib::Printf( " exactGgx : compute each GGX sample direction with today's call instead of the bake's table\n" );
		idLib::Printf( " noPreviewBlit : do not show each captured face on the screen ( refused on Vulkan )\n" );
		idLib::Printf( " ( the readback stays one wait a face here; fastReadback is a bakeLightGrids switch )\n" );
		idLib::Printf( " exactStream : capture every probe before running the jobs instead of batches of %i probes\n", ENVPROBE_STREAM_BATCH );
		return;
	}

	baseName = tr.primaryWorld->mapName;
	baseName.StripFileExtension();

	captureSize = ENVPROBE_CAPTURE_SIZE;

	if( !tr.primaryView )
	{
		common->Printf( "No primary view.\n" );
		return;
	}

	//puncho start - concurrency TB0/TB1: no return below this; R_BakeEnd at the end restores every switch the bake
	// changes and frees the tables. stageTime is the end of the last timed stage ( R_BakeStage )
	//puncho - concurrency RA2-03: a capture that is not shown can present a Vulkan swapchain image that never reached the
	// present layout, so the switch is refused there
	if( noPreviewBlit && deviceManager->GetGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN )
	{
		common->Printf( "noPreviewBlit is refused on Vulkan; the preview blits run\n" );
		noPreviewBlit = false;
	}

	bakeContext_t* bake = R_BakeBegin( exactHalf );
	if( !exactGgx )
	{
		//puncho - concurrency RB3-01: recorded in the context before it is filled; a failed allocation is the engine's
		// recoverable error, after which R_BakeEndAfterError gives back what the bake already holds
		const int ggxMips = idMath::BitsForInteger( RADIANCE_OCTAHEDRON_SIZE );
		bake->ggxH = ( idVec3* )Mem_Alloc( ( ggxMips - 1 ) * ENVPROBE_RADIANCE_SAMPLES * sizeof( idVec3 ), TAG_RENDER );
		if( bake->ggxH == NULL )
		{
			common->Error( "bakeEnvironmentProbes: out of memory for the GGX table" );
		}
		R_BuildGGXSampleTable( bake->ggxH, ggxMips, ENVPROBE_RADIANCE_SAMPLES );
	}
	uint64 stageTime = bake->startMicroSec;
	int queuedJobs = 0;		// D18: jobs on tr.envprobeJobList since its last Submit

	// TB2: batches on two alternating lists while the capture continues; needs the job lists ( useThreads )
	const bool streamJobs = !exactStream && useThreads;
	envprobeStream_t epStream;
	epStream.lists[0] = tr.envprobeJobList;
	epStream.lists[1] = tr.envprobeJobList2;
	epStream.first[0] = epStream.first[1] = -1;
	epStream.last[0] = epStream.last[1] = 0;
	epStream.current = 0;
	epStream.batchFirst = tr.envprobeJobs.Num();
	//puncho end

	const viewDef_t primary = *tr.primaryView;

	//--------------------------------------------
	// CONVOLVE CUBEMAPS
	//--------------------------------------------

	// make sure the game / draw thread has completed
	commonLocal.WaitGameThread();

	// turn vsync off for faster capturing of the probes
	r_swapInterval.SetInteger( 0 );	//puncho - concurrency RA2-04: R_BakeBegin saved it, R_BakeEnd restores it

	// turn off clear in between views so we keep the progress bar visible
	r_clear.SetInteger( 0 );	//puncho - concurrency RA2-04: R_BakeBegin saved it, R_BakeEnd restores it

	// disable scissor, so we don't need to adjust all those rects
	r_useScissor.SetBool( false );

	// RB: this really sucks but prevents a crash I couldn't track down
	extern idCVar r_useParallelAddModels;
	extern idCVar r_useParallelAddShadows;
	extern idCVar r_useParallelAddLights;

	r_useParallelAddModels.SetBool( false );
	r_useParallelAddShadows.SetBool( false );
	r_useParallelAddLights.SetBool( false );

	stageTime = R_BakeStage( bake, BAKE_STAGE_SETUP, stageTime );	//puncho - concurrency TB0

	// discard anything currently on the list (this triggers SwapBuffers)
	tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );

	stageTime = R_BakeStage( bake, BAKE_STAGE_CLOSEOUT, stageTime );	//puncho - concurrency TB0

	tr.takingEnvprobe = true;

	int totalProcessedProbes = 0;
	int	totalStart = Sys_Milliseconds();

	for( int i = 0; i < tr.primaryWorld->envprobeDefs.Num(); i++ )
	{
		RenderEnvprobeLocal* def = tr.primaryWorld->envprobeDefs[i];
		if( def == NULL )
		{
			continue;
		}

		totalProcessedProbes++;
	}

	idLib::Printf( "Deleting old probes...\n" );

	fullname.Format( "env/%s", baseName.c_str() );
	R_BakeImageName( fullname );

	idFileList* files = fileSystem->ListFilesTree( fullname, "envprobe*.exr", true );
	for( int i = 0; i < files->GetNumFiles(); i++ )
	{
		idLib::Printf( "deleting old envprobe data '%s'\n", files->GetFile( i ) );
		fileSystem->RemoveFile( files->GetFile( i ) );
	}
	fileSystem->FreeFileList( files );

	idLib::Printf( "Shooting %i environment probes...\n", totalProcessedProbes );

	stageTime = R_BakeStage( bake, BAKE_STAGE_SETUP, stageTime );	//puncho - concurrency TB0

	CommandlineProgressBar progressBar( totalProcessedProbes, sysWidth, sysHeight );
	progressBar.Start();

	stageTime = R_BakeStage( bake, BAKE_STAGE_PROGRESS, stageTime );	//puncho - concurrency TB0

	int	start = Sys_Milliseconds();

	for( int i = 0; i < tr.primaryWorld->envprobeDefs.Num(); i++ )
	{
		RenderEnvprobeLocal* def = tr.primaryWorld->envprobeDefs[i];
		if( def == NULL )
		{
			continue;
		}

		byte* buffers[6];
		byte* depthBuffers[6];

		// the near plane of every face: the faces copy primary.renderView, cramZNear included
		const float captureZNear = ( primary.renderView.cramZNear ) ? ( r_znear.GetFloat() * 0.25f ) : r_znear.GetFloat();

		int areaNum = tr.primaryWorld->PointInArea( def->parms.origin );
		idVec3 point = def->parms.origin;
		point.SnapInt();

		for( int j = 0; j < 6; j++ )
		{
			ref = primary.renderView;

			ref.rdflags = RDF_NOAMBIENT | RDF_IRRADIANCE;

			//puncho - concurrency TB3a: the capture is not shown; the backend skips its preview blits
			if( noPreviewBlit )
			{
				ref.rdflags |= RDF_BAKE_NODISPLAY;
			}
			ref.fov_x = ref.fov_y = 90;

			ref.vieworg = def->parms.origin;
			ref.viewaxis = tr.cubeAxis[j];

			// discard anything currently on the list
			//tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );

			// build commands to render the scene
			tr.primaryWorld->RenderScene( &ref );

			stageTime = R_BakeStage( bake, BAKE_STAGE_FRONTEND, stageTime );	//puncho - concurrency TB0

			// finish off these commands
			const emptyCommand_t* cmd = tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );

			stageTime = R_BakeStage( bake, BAKE_STAGE_CLOSEOUT, stageTime );	//puncho - concurrency TB0

			// issue the commands to the GPU
			tr.RenderCommandBuffers( cmd );

			stageTime = R_BakeStage( bake, BAKE_STAGE_BACKEND, stageTime );	//puncho - concurrency TB0

			// discard anything currently on the list (this triggers SwapBuffers)
			tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );

			stageTime = R_BakeStage( bake, BAKE_STAGE_CLOSEOUT, stageTime );	//puncho - concurrency TB0

			byte* floatRGB16F = NULL;
			byte* depthPacked = NULL;

			bool  depthOK = R_ReadProbeColorAndDepthRGB16FInBake(deviceManager->GetDevice(), &backEnd.GetCommonPasses(),
				globalImages->envprobeHDRImage->GetTextureHandle(),
				globalImages->envprobeDepthImage->GetTextureHandle(),
				&floatRGB16F, &depthPacked, captureSize, captureSize, bake);

			stageTime = Sys_Microseconds();	//puncho - concurrency TB0: the readback timed its own wait and convert

#if 0
			idStr testName;
			testName.Format( "env/test/%s/area%i_envprobe_%i_%i_%i_side_%i.exr", baseName.c_str(), areaNum, int( point.x ), int( point.y ), int( point.z ), j );
			R_WriteEXR( testName, floatRGB16F, 3, captureSize, captureSize, "fs_basepath" );

			if( !validCapture )
			{
				common->Printf( "failed to capture side %s\n", testName.c_str() );
			}
#endif
			buffers[ j ] = floatRGB16F;
			bake->probeFaces[j] = floatRGB16F;	//puncho - concurrency RA2-01: the bake owns it until the probe's jobs do

			float* dist = (float*)R_StaticAlloc(captureSize * captureSize * sizeof(float));

			if (!depthOK || depthPacked == NULL)
			{
				common->Warning("depth read-back FAILED, probe %i side %i", i, j);
				for (int p = 0; p < captureSize * captureSize; p++) { dist[p] = 0.0f; }
				if (depthPacked != NULL)
				{
					R_StaticFree(depthPacked);	//puncho - concurrency RB3-05: a rejected readback still allocated it
				}
			}
			else
			{
				const halfFloat_t* dsrc = (const halfFloat_t*)depthPacked;
				for (int p = 0; p < captureSize * captureSize; p++)
				{
					dist[p] = LinearizeProbeDepth(F16toF32(dsrc[p * 3 + 0]), captureZNear, j, p, captureSize);
				}
#if 0 // TEMP: validate depth read-back — delete once confirmed
				if (i == 0)	// first probe only -> 6 files, not hundreds
				{
					idStr dbgName;
					dbgName.Format("env/test/depth_raw_side_%i.exr", j);
					R_WriteEXR(dbgName, depthPacked, 3, captureSize, captureSize, "fs_basepath");

					halfFloat_t* dbg = (halfFloat_t*)R_StaticAlloc(captureSize * captureSize * 3 * sizeof(halfFloat_t));
					for (int p = 0; p < captureSize * captureSize; p++)
					{
						halfFloat_t h = F32toF16(dist[p]);
						dbg[p * 3 + 0] = h;
						dbg[p * 3 + 1] = h;
						dbg[p * 3 + 2] = h;	// grey
					}
					dbgName.Format("env/test/depth_lin_side_%i.exr", j);
					R_WriteEXR(dbgName, (byte*)dbg, 3, captureSize, captureSize, "fs_basepath");
					R_StaticFree(dbg);
				}
#endif
				R_StaticFree(depthPacked);
			}
			depthBuffers[j] = (byte*)dist;
			bake->probeDepth[j] = (byte*)dist;	//puncho - concurrency RA2-01

			stageTime = R_BakeStage( bake, BAKE_STAGE_READBACK_CONVERT, stageTime );	//puncho - concurrency TB0: the depth's linearisation
		}

		tr.takingEnvprobe = false;
		progressBar.Increment( true );
		tr.takingEnvprobe = true;

		stageTime = R_BakeStage( bake, BAKE_STAGE_PROGRESS, stageTime );	//puncho - concurrency TB0

		fullname.Format( "%s/area%i_envprobe_%i_%i_%i", baseName.c_str(), areaNum, int( point.x ), int( point.y ), int( point.z ) );
		fullname.ReplaceChar( '-', '_' );

		//puncho start - concurrency D18: the list reserves ENVPROBE_JOBLIST_MAX_JOBS jobs and each probe adds two, so
		// past that the jobs added so far are submitted and waited for first; the jobs are independent and the
		// results are read after the last wait, in the order of tr.envprobeJobs, so the output is the same
		if( useThreads && !streamJobs && queuedJobs + 2 > ENVPROBE_JOBLIST_MAX_JOBS )
		{
			tr.envprobeJobList->Submit( NULL, numThreads );
			tr.envprobeJobList->Wait();
			queuedJobs = 0;
		}
		queuedJobs += 2;
		//puncho end

		// create 2 jobs
		idParallelJobList* probeJobList = streamJobs ? NULL : tr.envprobeJobList;	//puncho - concurrency TB2: streaming adds the jobs at its submit
		R_MakeAmbientMap( fullname.c_str(), buffers, "_amb", IRRADIANCE_OCTAHEDRON_SIZE, false, useThreads, def->index, bake->halfToFloat, bake->ggxH, probeJobList );
		R_MakeAmbientMap( fullname.c_str(), buffers, "_spec", RADIANCE_OCTAHEDRON_SIZE, true, useThreads, def->index, bake->halfToFloat, bake->ggxH, probeJobList );
		memset( bake->probeFaces, 0, sizeof( bake->probeFaces ) );	//puncho - concurrency RA2-01: the two jobs in tr.envprobeJobs own the faces now

		//puncho - concurrency TB2: a full batch goes to its list, and the other list's batch is waited for and its
		// faces freed before the capture adds to it
		if( streamJobs && tr.envprobeJobs.Num() - epStream.batchFirst >= 2 * ENVPROBE_STREAM_BATCH )
		{
			R_EnvprobeStreamSubmit( epStream, numThreads );
		}

		stageTime = R_BakeStage( bake, BAKE_STAGE_JOB_WAIT, stageTime );	//puncho - concurrency TB0

		// write per-probe distance cube in memory
		idStr depthName;
		// env/ like the _amb/_spec maps (R_MakeAmbientMap) and the loader in R_DeriveEnvprobeData,
		// otherwise the cube is unfindable after a reload and zip-lightdata.bat never packs it
		depthName.Format("env/%s_depth", fullname.c_str());

		const byte* depthFaces[6];
		const float invZFar = 1.0f / PROBE_DEPTH_FAR;	// G channel scale; nothing reads G at runtime
		const int   outSize = 64;					// occluder-field resolution
		const int   block = captureSize / outSize;	// 4:1 per axis at 256 -> 64
		assert(captureSize% outSize == 0);			// captureSize is ENVPROBE_CAPTURE_SIZE = 256 (Framebuffer.h:41)

		for (int j = 0; j < 6; j++)
		{
			// MIN keeps the nearest occluder so thin geometry survives distance
			uint32_t* packed = (uint32_t*)R_StaticAlloc(outSize * outSize * 4, TAG_IMAGE);
			const float* d = (const float*)depthBuffers[j];

			for (int oy = 0; oy < outSize; oy++)
			{
				for (int ox = 0; ox < outSize; ox++)
				{
					float m = 1e30f;
					for (int sy = 0; sy < block; sy++)
					{
						for (int sx = 0; sx < block; sx++)
						{
							const float s = d[(oy * block + sy) * captureSize + (ox * block + sx)];
							m = (s < m) ? s : m;
						}
					}

					float nd = m * invZFar;
					float rgb[3] = { m, nd * nd, 0.0f };
					packed[oy * outSize + ox] = float3_to_r11g11b10f(rgb);
				}
			}

			depthFaces[j] = (const byte*)packed;
			R_StaticFree(depthBuffers[j]);
			bake->probeDepth[j] = (byte*)packed;	//puncho - concurrency RA2-01
		}

		{
			idStrStatic< MAX_OSPATH > genName = depthName;
			idImage::GetGeneratedName(genName, TD_HDRI, CF_NATIVE_HDR);	
			idBinaryImage im(genName);
			textureFormat_t fmt = FMT_R11G11B10F;
			im.LoadCubeFromMemory(outSize, depthFaces, 1, fmt, false);
			im.WriteGeneratedFile(FILE_NOT_FOUND_TIMESTAMP);
		}

		stageTime = R_BakeStage( bake, BAKE_STAGE_WRITE, stageTime );	//puncho - concurrency TB0

		def->depthImage = globalImages->ImageFromFile(depthName, TF_LINEAR, TR_CLAMP, TD_HDRI, CF_NATIVE_HDR);

		for (int j = 0; j < 6; j++)
		{
			R_StaticFree((void*)depthFaces[j]);
			bake->probeDepth[j] = NULL;	//puncho - concurrency RA2-01
		}
		def->depthImage = globalImages->ImageFromFile(depthName, TF_LINEAR, TR_CLAMP, TD_HDRI, CF_NATIVE_HDR);

		stageTime = R_BakeStage( bake, BAKE_STAGE_RELOAD, stageTime );	//puncho - concurrency TB0
	}

	int	end = Sys_Milliseconds();

	tr.takingEnvprobe = false;

	//puncho - concurrency D15: back to what they were before the bake, not to true
	R_BakeRestoreCaptureSwitches( bake );

	common->Printf( "captured environment probes %5.1f seconds\n\n", ( end - start ) * 0.001f );

	//puncho start - concurrency TB2: the last, partial batch, then both lists retired, the older batch first; after
	// this every job has run and every face is freed, and the finalisation below reads the outputs as before
	if( streamJobs )
	{
		idLib::Printf( "Processing the last probes on all available cores... Please wait.\n" );
		common->UpdateScreen( false );
		common->UpdateScreen( false );

		stageTime = R_BakeStage( bake, BAKE_STAGE_PROGRESS, stageTime );

		R_EnvprobeStreamSubmit( epStream, numThreads );
		R_EnvprobeStreamRetire( epStream, epStream.current ^ 1 );
		R_EnvprobeStreamRetire( epStream, epStream.current );
	}
	//puncho end

	if( useThreads && !streamJobs )	//puncho - concurrency TB2: bakeEnvironmentProbes -exactStream, today's path
	{
		idLib::Printf( "Processing probes on all available cores... Please wait.\n" );
		common->UpdateScreen( false );
		common->UpdateScreen( false );

		stageTime = R_BakeStage( bake, BAKE_STAGE_PROGRESS, stageTime );	//puncho - concurrency TB0

		//tr.envprobeJobList->Submit();
		tr.envprobeJobList->Submit( NULL, numThreads );
		tr.envprobeJobList->Wait();
	}

	stageTime = R_BakeStage( bake, BAKE_STAGE_JOB_WAIT, stageTime );	//puncho - concurrency TB0

	const float* halfToFloat = bake->halfToFloat;	//puncho - concurrency TB1a: the finalisation's conversion

	for (int j = 0; j < tr.envprobeJobs.Num(); j++)
	{
		calcEnvprobeParms_t* job = tr.envprobeJobs[j];

		if (job->outHeight == RADIANCE_OCTAHEDRON_SIZE)	// _spec (radiance) -> GGX cube .bimage
		{
			const int faceSize = job->outHeight;						// must match the bake & NUM_RADIANCE_MIPS
			const int numLevels = idMath::BitsForInteger(faceSize);		// 512 -> 10, 256 -> 9

			// (1) convert each prefiltered RGB16F face/level to packed R11G11B10F (BC6 input)
			idList<byte*> faceMips;
			faceMips.SetNum(6 * numLevels);	
			for (int side = 0; side < 6; side++)
			{
				int mipSize = faceSize;
				for (int level = 0; level < numLevels; level++)
				{
					const halfFloat_t* s = job->outFaces[side][level];	
					byte* packed = (byte*)R_StaticAlloc(mipSize * mipSize * 4, TAG_IMAGE);
					//puncho start - concurrency TB1a: inside a bake the half table, built from F16toF32 itself
					if (halfToFloat != NULL)
					{
						for (int p = 0; p < mipSize * mipSize; p++)
						{
							float rgb[3] = { halfToFloat[s[p * 3 + 0]], halfToFloat[s[p * 3 + 1]], halfToFloat[s[p * 3 + 2]] };
							((uint32_t*)packed)[p] = float3_to_r11g11b10f(rgb);
						}
					}
					else	// bakeEnvironmentProbes -exactHalf: today's loop
					//puncho end
					for (int p = 0; p < mipSize * mipSize; p++)
					{
						float rgb[3] = { F16toF32(s[p * 3 + 0]), F16toF32(s[p * 3 + 1]), F16toF32(s[p * 3 + 2]) };
						((uint32_t*)packed)[p] = float3_to_r11g11b10f(rgb);
					}
					faceMips[side * numLevels + level] = packed;
					bake->finalMips[side * numLevels + level] = packed;	//puncho - concurrency RB3-03
					mipSize = Max(1, mipSize >> 1);
				}
			}

			// no EXR round-trip
			idStrStatic< MAX_OSPATH > baseName = job->filename;
			baseName.StripFileExtension();                                          
			idStrStatic< MAX_OSPATH > genName = baseName;
			idImage::GetGeneratedName(genName, TD_HDR_LIGHTPROBE, CF_NATIVE_HDR); 
			idBinaryImage im(genName);
			textureFormat_t fmt = FMT_BC6H;
			im.LoadCubeMipchainFromMemory(faceSize, (const byte* const*)faceMips.Ptr(), numLevels, fmt);
			im.WriteGeneratedFile(FILE_NOT_FOUND_TIMESTAMP);

			stageTime = R_BakeStage( bake, BAKE_STAGE_WRITE, stageTime );	//puncho - concurrency TB0

			globalImages->ImageFromFile(baseName, TF_DEFAULT, TR_CLAMP, TD_HDR_LIGHTPROBE, CF_NATIVE_HDR);

			stageTime = R_BakeStage( bake, BAKE_STAGE_RELOAD, stageTime );	//puncho - concurrency TB0

			
			for (int i = 0; i < faceMips.Num(); i++)
			{
				R_StaticFree(faceMips[i]);
				bake->finalMips[i] = NULL;	//puncho - concurrency RB3-03
			}
			for (int side = 0; side < 6; side++)
			{
				for (int level = 0; level < numLevels; level++)
				{
					R_StaticFree(job->outFaces[side][level]);
				}
			}
			
		}
		else												
		{
			R_BakeImageName( job->filename );
			R_WriteEXR(job->filename, (byte*)job->outBuffer, 3, job->outWidth, job->outHeight, "fs_basepath");
			stageTime = R_BakeStage( bake, BAKE_STAGE_WRITE, stageTime );	//puncho - concurrency TB0
			globalImages->ImageFromFile(job->filename, TF_LINEAR, TR_CLAMP, TD_HDR_LIGHTPROBE, CF_2D_PACKED_MIPCHAIN);
			stageTime = R_BakeStage( bake, BAKE_STAGE_RELOAD, stageTime );	//puncho - concurrency TB0
			Mem_Free(job->outBuffer);
			
			if (job->probeIndex >= 0 && job->probeIndex < tr.primaryWorld->envprobeDefs.Num())
			{
				RenderEnvprobeLocal* shDef = tr.primaryWorld->envprobeDefs[job->probeIndex];
				if (shDef != NULL)
				{
					for (int i = 0; i < 9; i++)
					{
						shDef->shIrradiance[i] = job->shIrradiance[i];
					}
				}
			}
		}

		common->Printf("%s convolved in %5.1f seconds\n\n", job->filename.c_str(), job->time * 0.001f);

		// freeRadiance is 1 only for the radiance job, so this fires once per probe.
		if (job->freeRadiance > 0)
		{
			for (int i = 0; i < 6; i++)
			{
				if (job->radiance[i])
				{
					Mem_Free(job->radiance[i]);
				}
			}
		}

		delete job;		// PRESERVED FROM STOCK (L1181)
		tr.envprobeJobs[j] = NULL;	//puncho - concurrency RA2-01: R_FreeEnvprobeJobs skips it if an error stops this loop
	}

	tr.envprobeJobs.Clear();

	// per-probe SH sidecar
	{
		idStr shName;
		shName.Format("%s.shprobes", baseName.c_str());

		idFile* fp = fileSystem->OpenFileWrite(shName, "fs_basepath");
		if (fp == NULL)
		{
			common->Warning("bakeEnvironmentProbes: couldn't write %s", shName.c_str());
		}
		else
		{
			int numProbes = 0;
			for (int i = 0; i < tr.primaryWorld->envprobeDefs.Num(); i++)
			{
				if (tr.primaryWorld->envprobeDefs[i] != NULL)
				{
					numProbes++;
				}
			}

			fp->WriteFloatString("SHPROBES \"1\"\n\n");
			fp->WriteFloatString("%i\n", numProbes);

			for (int i = 0; i < tr.primaryWorld->envprobeDefs.Num(); i++)
			{
				RenderEnvprobeLocal* def = tr.primaryWorld->envprobeDefs[i];
				if (def == NULL)
				{
					continue;
				}

				int probeAreaNum = tr.primaryWorld->PointInArea(def->parms.origin);

				idVec3 point = def->parms.origin;
				point.SnapInt();

				fp->WriteFloatString("%i ( %i %i %i ) ( ",
					probeAreaNum, int(point.x), int(point.y), int(point.z));

				for (int j = 0; j < 9; j++)
				{
					fp->WriteFloatString("%f %f %f ",
						def->shIrradiance[j].x, def->shIrradiance[j].y, def->shIrradiance[j].z);
				}

				fp->WriteFloatString(")\n");
			}

			fileSystem->CloseFile(fp);
			common->Printf("wrote %s (%i probes)\n", shName.c_str(), numProbes);
		}
	}

	stageTime = R_BakeStage( bake, BAKE_STAGE_WRITE, stageTime );	//puncho - concurrency TB0

	int	totalEnd = Sys_Milliseconds();

	nvrhi::CommandListHandle commandList = deviceManager->GetDevice()->createCommandList();
	commandList->open();

	//--------------------------------------------
	// LOAD CONVOLVED OCTAHEDRONS INTO THE GPU
	//--------------------------------------------
	for( int i = 0; i < tr.primaryWorld->envprobeDefs.Num(); i++ )
	{
		RenderEnvprobeLocal* def = tr.primaryWorld->envprobeDefs[i];
		if( def == NULL )
		{
			continue;
		}

		def->irradianceImage->Reload( true, commandList );
		def->radianceImage->Reload( true, commandList );
	}

	globalImages->LoadDeferredImages( commandList );

	commandList->close();
	deviceManager->GetDevice()->executeCommandList( commandList );

	stageTime = R_BakeStage( bake, BAKE_STAGE_RELOAD, stageTime );	//puncho - concurrency TB0

	idLib::Printf( "----------------------------------\n" );
	idLib::Printf( "Processed %i light probes\n", totalProcessedProbes );
	common->Printf( "Baked SH irradiance and GGX mip maps in %5.1f minutes\n\n", ( totalEnd - totalStart ) / ( 1000.0f * 60 ) );

	//puncho - concurrency TB0/RA2-04: restores every setting the bake changed ( vsync and clear among them ), prints
	// "bake phase" lines, frees the tables
	R_BakeEnd( true );
}

CONSOLE_COMMAND( makeBrdfLUT, "make a GGX BRDF lookup table", NULL )
{
	int			outSize = 256;
	int			width = 0, height = 0;

	//if( args.Argc() != 2 )
	//{
	//	common->Printf( "USAGE: makeBrdfLut [size]\n" );
	//	return;
	//}

	//if( args.Argc() == 2 )
	//{
	//	outSize = atoi( args.Argv( 1 ) );
	//}

	// resample with hemispherical blending
	int	samples = 1024;

	int ldrBufferSize = outSize * outSize * 4;
	byte* ldrBuffer = ( byte* )Mem_Alloc( ldrBufferSize, TAG_TEMP );

	int hdrBufferSize = outSize * outSize * 2 * sizeof( halfFloat_t );
	halfFloat_t* hdrBuffer = ( halfFloat_t* )Mem_Alloc( hdrBufferSize, TAG_TEMP );

	int sysWidth = renderSystem->GetWidth();
	int sysHeight = renderSystem->GetHeight();

	CommandlineProgressBar progressBar( outSize * outSize, sysWidth, sysHeight );

	int	start = Sys_Milliseconds();

	for( int x = 0 ; x < outSize ; x++ )
	{
		float NdotV = ( x + 0.5f ) / outSize;

		for( int y = 0 ; y < outSize ; y++ )
		{
			float roughness = ( y + 0.5f ) / outSize;

			idVec2 output = IntegrateBRDF( NdotV, roughness, samples );

			ldrBuffer[( y * outSize + x ) * 4 + 0] = byte( output.x * 255 );
			ldrBuffer[( y * outSize + x ) * 4 + 1] = byte( output.y * 255 );
			ldrBuffer[( y * outSize + x ) * 4 + 2] = 0;
			ldrBuffer[( y * outSize + x ) * 4 + 3] = 255;

			halfFloat_t half1 = F32toF16( output.x );
			halfFloat_t half2 = F32toF16( output.y );

			hdrBuffer[( y * outSize + x ) * 2 + 0] = half1;
			hdrBuffer[( y * outSize + x ) * 2 + 1] = half2;
			//hdrBuffer[( y * outSize + x ) * 4 + 2] = 0;
			//hdrBuffer[( y * outSize + x ) * 4 + 3] = 1;

			progressBar.Increment( true );
		}
	}

	idStr fullname = "env/_brdfLut.png";
	idLib::Printf( "writing %s\n", fullname.c_str() );

	R_WritePNG( fullname, ldrBuffer, 4, outSize, outSize, "fs_basepath" );
	//R_WriteEXR( "env/_brdfLut.exr", hdrBuffer, 4, outSize, outSize, "fs_basepath" );


	idFileLocal headerFile( fileSystem->OpenFileWrite( "env/Image_brdfLut.h", "fs_basepath" ) );

	static const char* intro = R"(
#ifndef BRDFLUT_TEX_H
#define BRDFLUT_TEX_H

#define BRDFLUT_TEX_WIDTH 256
#define BRDFLUT_TEX_HEIGHT 256
#define BRDFLUT_TEX_PITCH (BRDFLUT_TEX_WIDTH * 2)
#define BRDFLUT_TEX_SIZE (BRDFLUT_TEX_WIDTH * BRDFLUT_TEX_PITCH)

// Stored in R16G16F format
static const unsigned char brfLutTexBytes[] =
{
)";

	headerFile->Printf( "%s\n", intro );

	const byte* hdrBytes = (const byte* ) hdrBuffer;
	for( int i = 0; i < hdrBufferSize; i++ )
	{
		byte b = hdrBytes[i];

		if( i < ( hdrBufferSize - 1 ) )
		{
			headerFile->Printf( "0x%02hhx, ", b );
		}
		else
		{
			headerFile->Printf( "0x%02hhx", b );
		}

		if( i % 12 == 0 )
		{
			headerFile->Printf( "\n" );
		}
	}
	headerFile->Printf( "\n};\n#endif\n" );

	int	end = Sys_Milliseconds();

	common->Printf( "%s integrated in %5.1f seconds\n\n", fullname.c_str(), ( end - start ) * 0.001f );

	Mem_Free( ldrBuffer );
	Mem_Free( hdrBuffer );
}


