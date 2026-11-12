// shadow_cache_copy.ps.hlsl - pixel stage of the shadow-cache tile copy ( D36 ): X0 landed this as a
// stub that copies the source texel straight to the depth target; track S replaces the body later.
// The stub loads the source at the destination's pixel position, which is right only while source and
// destination share one layout; a tile copy needs a source-to-destination offset ( track S, S2 ).

// *INDENT-OFF*
Texture2D<float> t_CacheDepth : register( t0 );

struct PS_IN
{
	float4 position	: SV_Position;
	float2 uv		: TEXCOORD0;
};
// *INDENT-ON*

float main( PS_IN fragment ) : SV_Depth
{
	return t_CacheDepth.Load( int3( int2( fragment.position.xy ), 0 ) );
}
