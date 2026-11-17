// froxel_apply.vs.hlsl - vertex stage of the froxel fog apply ( track V, package V1 ): one triangle over the
// view from SV_VertexID, no vertex buffer. The pass's viewport and scissor cut it to the fog view.

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
