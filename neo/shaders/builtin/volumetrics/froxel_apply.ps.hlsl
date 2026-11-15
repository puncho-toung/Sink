// froxel_apply.ps.hlsl - pixel stage of the froxel fog apply ( D33 ): X0 landed this as a stub;
// track V owns this file ( V5 ). Returns fog-off: black in-scatter, full transmittance.

// *INDENT-OFF*
struct PS_IN
{
	float4 posClip	: SV_Position;
	float2 uv		: TEXCOORD0;
};
// *INDENT-ON*

float4 main( PS_IN fragment ) : SV_Target
{
	return float4( 0.0, 0.0, 0.0, 1.0 );
}
