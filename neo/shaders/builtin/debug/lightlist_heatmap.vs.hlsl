// lightlist_heatmap.vs.hlsl - vertex stage of the light lists' heatmap ( r_showLightLists 1 ): a full-screen
// triangle from SV_VertexID, no vertex buffer; the viewport and scissor are the view.

// *INDENT-OFF*
struct VS_OUT
{
	float4 posClip	: SV_Position;
};
// *INDENT-ON*

void main( uint vertexId : SV_VertexID, out VS_OUT result )
{
	// the three corners of a full-screen triangle: uv ( 0, 0 ), ( 2, 0 ), ( 0, 2 )
	float2 uv = float2( float( ( vertexId << 1 ) & 2 ), float( vertexId & 2 ) );
	result.posClip = float4( uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0 );
}
