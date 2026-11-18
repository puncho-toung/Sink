// lightrecord_core.inc.hlsl - the parts of one light record at one point that need no surface and no pixel stage
// ( track F's record, C1; made in package V3 of track V, by the refit of the owner's row 53 ). Two files include it:
// shaders/lightrecord.inc.hlsl, the light at a surface point ( the forward pass ), and
// shaders/builtin/volumetrics/froxel_lights.inc.hlsl, the light at a froxel sample ( the froxel inject, a compute
// stage, which cannot include the first ). What both evaluate the same way stands here once.
//
// The includer has, in front of this file: lightlists.inc.hlsl ( recordLight_t, shadowRow_t, RLF_* ) and
// StructuredBuffer<recordLight_t> t_LightRecords. What a pass does with a shadow row's coordinate ( a filter of
// several taps on a surface, one comparison in the air ) is the pass's own.

#ifndef LIGHTRECORD_CORE_INC_HLSL
#define LIGHTRECORD_CORE_INC_HLSL

// Whether a point whose texgens are these lies inside the light's volume: the one test of a light's planes
bool LL_PlanesHold( float projS, float projT, float projQ, float fallS )
{
	return !( projQ <= 0.0 || projS < 0.0 || projS > projQ || projT < 0.0 || projT > projQ || fallS < 0.0 || fallS > 1.0 );
}

// Record r's texgens at the point P ( the relative frame, w = 1 ): projection S, T, Q and the falloff. False outside
// the light's volume, where the light is exactly zero. The four planes are read on their own, in front of the
// record, so that a point outside the light's volume reads nothing else of it
bool LL_RecordPlanes( uint r, float4 P, out float projS, out float projT, out float projQ, out float fallS )
{
	projS = dot( P, t_LightRecords[ r ].lightProject[0] );
	projT = dot( P, t_LightRecords[ r ].lightProject[1] );
	projQ = dot( P, t_LightRecords[ r ].lightProject[2] );
	fallS = dot( P, t_LightRecords[ r ].lightProject[3] );
	return LL_PlanesHold( projS, projT, projQ, fallS );
}

// A point light's shadow face for toLight, the direction to the light itself ( origin - point ): interactionSM.ps.hlsl's
// pick, the face whose axis the direction is largest along, the first on a tie, in the order of the light's six rows
// ( -x, +x, -y, +y, -z, +z )
uint LL_PointLightFace( float3 toLight )
{
	float best = -toLight.x;
	uint face = 0;
	if( toLight.x > best )
	{
		best = toLight.x;
		face = 1;
	}
	if( -toLight.y > best )
	{
		best = -toLight.y;
		face = 2;
	}
	if( toLight.y > best )
	{
		best = toLight.y;
		face = 3;
	}
	if( -toLight.z > best )
	{
		best = -toLight.z;
		face = 4;
	}
	if( toLight.z > best )
	{
		best = toLight.z;
		face = 5;
	}
	return face;
}

// How far an emitter pattern's position moves for a step d of the point: d through the projection S and T planes
// and the falloff plane, before the pattern's axis scale ( 2 * EMPatternAxisScale ). A pass forms the pattern's
// footprint from such steps: the pixel's two screen derivatives, or a froxel's three edges
float3 LL_EmitterPatternStep( recordLight_t L, float3 d )
{
	return float3( dot( d, L.lightProject[0].xyz ), dot( d, L.lightProject[1].xyz ), dot( d, L.lightProject[3].xyz ) );
}

// A shadow row's lookup of the point P ( the relative frame, w = 1 ), as the legacy pass forms it
// ( interactionSM.ps.hlsl ): the atlas coordinate in xy, the receiver's depth times biasScale in z. The row must have a
// place in the atlas ( atlasRect.z >= 0 )
float3 LL_ShadowRowCoord( shadowRow_t R, float4 P, float biasScale )
{
	float4 shadowTexcoord;
	shadowTexcoord.x = dot( P, R.worldToShadow[0] );
	shadowTexcoord.y = dot( P, R.worldToShadow[1] );
	shadowTexcoord.z = dot( P, R.worldToShadow[2] );
	shadowTexcoord.w = dot( P, R.worldToShadow[3] );
	shadowTexcoord.xyz /= shadowTexcoord.w;
	return float3( shadowTexcoord.xy * R.atlasRect.z + R.atlasRect.xy, shadowTexcoord.z * biasScale );
}

#endif // LIGHTRECORD_CORE_INC_HLSL
