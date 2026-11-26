// bloom_cb.h - the bloom chain shaders' push constants, sent whole by every down and up draw. The C++ twin is
// renderer/Passes/BloomPass_cb.h, member for member.

#ifndef BLOOM_CB_H
#define BLOOM_CB_H

// The largest value the scrub's ceiling may take ( the C++ holds r_bloomCeiling to it, and sends the ceiling
// in exposure.z ). The up passes ADD the levels in the blender, so level 0 can reach this bound times
// ( 1 + tail + ... + tail^8 ) <= 9 bounds, and that must stay under the largest finite value of the chain's
// format ( 64512 in the 10-bit blue lane ): 9 * 7000 = 63000. Above it the stored value would be an Inf on
// some drivers, and an Inf under a bilinear tap whose other weight is 0 gives a NaN. No light of this engine
// is near 7000 ( emitters stop at 64 )
#define BLOOM_SCRUB_BOUND 7000.0

struct BloomConstants
{
	float4 srcRect;		// xy the source rectangle's origin in source texels, zw its size in texels
	float4 srcInvSize;	// xy 1 / the source TEXTURE's size; z the tail ( up ), the glow threshold's knee ( first pass, FIRST 2 and 3 ); w the firefly knee ( first pass; <= 0: off )
	float4 exposure;	// first pass: x the exposure scale exp2( r_exposure ), y r_hdrMinLuminance, z the ceiling ( r_bloomCeiling ); w the glow threshold ( FIRST 2 and 3; 0 in the mix )
	float4	tailDepth;	// first pass, FIRST 3 ( the distance tail ): xy the projection's two depth terms ( a view distance is x / ( depth + y ) ); zw 0
	float4	tailDist;	// first pass, FIRST 3: x the far distance, y 1 / ( far - near ), z the share of its tail a far lamp keeps; w 0
};

#endif // BLOOM_CB_H
