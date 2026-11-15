// froxel_skymask.vs.hlsl - vertex stage of the froxel sky-mask pass ( D33 ): X0 landed this as a
// stub; track V owns this file ( V5 ). Full-screen triangle from SV_VertexID, no vertex buffer.

// *INDENT-OFF*
struct VS_OUT
{
	float4 posClip	: SV_Position;
	float2 uv		: TEXCOORD0;
};
// *INDENT-ON*

void main( uint vertexId : SV_VertexID, out VS_OUT result )
{
	// the three corners of a full-screen triangle: uv ( 0, 0 ), ( 2, 0 ), ( 0, 2 )
	float2 uv = float2( float( ( vertexId << 1 ) & 2 ), float( vertexId & 2 ) );

	result.uv = uv;
	result.posClip = float4( uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0 );
}
