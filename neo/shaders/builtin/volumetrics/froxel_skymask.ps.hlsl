// froxel_skymask.ps.hlsl - pixel stage of the froxel sky-mask pass ( D33 ): X0 landed this as a
// stub; track V owns this file ( V5 ). Returns 0: no sky pixels marked yet.

// *INDENT-OFF*
struct PS_IN
{
	float4 posClip	: SV_Position;
	float2 uv		: TEXCOORD0;
};
// *INDENT-ON*

float main( PS_IN fragment ) : SV_Target
{
	return 0.0;
}
