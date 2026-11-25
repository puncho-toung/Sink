// tonemapping_bloom.ps.hlsl - the tonemap pass's bloom variant: tonemapping.ps.hlsl compiled with BLOOM on,
// which mixes the bloom chain into the HDR frame before the exposure is applied. The cfg fixes SOURCE_ARRAY
// and QUAD_Z to 0 for it and carries the tonemapping pixel shader's other macros, so the pass draws it with
// the same vertex shader and the same constants plus the two bloom float4s

#define BLOOM 1
#include "tonemapping.ps.hlsl"
