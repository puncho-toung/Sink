// froxel_integrate_glow.cs.hlsl - WP-J21, the fog's glow: the integrate's glow form, froxel_integrate.cs.hlsl with
// USE_GLOW 1 ( it also marches the inject's never-glowing luminance into its own volume ). A file of its own, as
// tonemapping_bloom.ps.hlsl is, because the integrate's own blob has a permutation without a define

#define USE_GLOW 1
#include "froxel_integrate.cs.hlsl"
