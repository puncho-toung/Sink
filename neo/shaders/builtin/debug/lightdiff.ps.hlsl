// lightdiff.ps.hlsl - pixel stage of the light-difference debug view ( D33 ): X0 landed this as a
// stub; track F owns this file. Loads the two HDR light buffers at the pixel and shows a - b.

// *INDENT-OFF*
Texture2D<float4> t_A : register( t0 );
Texture2D<float4> t_B : register( t1 );

struct PS_IN
{
	float4 position	: SV_Position;
	float2 uv		: TEXCOORD0;
};
// *INDENT-ON*

float4 main( PS_IN fragment ) : SV_Target
{
	int3 p = int3( int2( fragment.position.xy ), 0 );

	float3 a = t_A.Load( p ).rgb;
	float3 b = t_B.Load( p ).rgb;

	return float4( a - b, 1.0 );
}
