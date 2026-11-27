// bloom_fft_cb.h - the constants of the bloom's convolution ( WP-J24 ): one buffer, written once a view by BloomPass
// and read by every pass of builtin/post/bloom_fft.cs.hlsl and by the re-entry, builtin/post/bloom_fft_compose.ps.hlsl.
// The C++ twin is renderer/Passes/BloomFft_cb.h, member for member.
//
// The field is W x H cells ( W = 256, 512 or 1024 by the quality, H = W / 2 ); the view's picture is laid into its
// corner from ( 0, 0 ) at k_s cells a screen pixel as a gw x gh picture, and the rest of the field is the black margin
// the light spreads into, the kernel's reach R = min( H - gh, W - gw ) cells. A kernel position is measured in view
// heights, so a shape is the same at every quality.

#ifndef BLOOM_FFT_CB_H
#define BLOOM_FFT_CB_H

// the wavelengths the diffraction kernel is summed over ( their weights and scales are in waves[] )
#define BLOOM_FFT_WAVES 8
// the longest transform: the field's width at the highest quality
#define BLOOM_FFT_MAX 1024
// the threads of a transform's group
#define BLOOM_FFT_THREADS 256
// WP-J27: the classes the light can be split into, one a kernel ( BLOOM_STYLE_SLOTS in renderer/RenderWorld.h )
#define BLOOM_FFT_CLASSES 4

struct BloomFftConstants
{
	uint4	field;		// x W, y H, z log2 W, w log2 H
	uint4	image;		// x gw, y gh: the picture's cells; z, w the source level's rectangle ( texels )
	float4	source;		// x s, the source level's texels a cell spans ( the tent's half-width ); y 1 / s^2; zw 0
	float4	shape;		// x the reach R in cells ( the window's radius ); y the re-entry's factor ( 1 - w0 ) / w0; z k_s, the cells a screen pixel; w the spikes' arms' fall c in view heights ( 0.004 by the plan )
	float4	kernelA;	// x cells a view height ( k_s x vh ); y the shape ( 0 spikes, 1 glow ); z the spike arms n; w the rotation ( 0 .. 1 of the pattern's period )
	float4	kernelB;	// x the arms' length L ( view heights ); y the arms' half-width at the lamp w_b; z the spread; w the ratio ( -1 .. 1 )
	float4	kernelC;	// x the ringing; y the glow's reach R_g ( view heights ); z the glare; w the cell's subsamples a side
	float4	norms;		// the terms' integrals over the plane ( view heights squared ): x the halo, y the arms, z the wide glow, w the glare
	float4	waves[BLOOM_FFT_WAVES];	// rgb the wavelength's share of each channel ( each channel's shares sum to 1 ); w its pattern scale k
	// WP-J27, the sources' own blooms: the light is split into classes, one a kernel, each convolved by its own kernel; the
	// products are summed before the one transform back
	uint4	classes;	// x the classes ( 1 to BLOOM_FFT_CLASSES; 1: no split, PASS 1 reads no share ); y the kernel slice PASS 0, 2 and 4 make; zw 0
	uint4	classKernel;	// each class's kernel slice
	float4	classMask[BLOOM_FFT_CLASSES];	// each class's weights of the four slots' shares ( bloom_share.cs.hlsl ): its share is their dot product
};

#endif // BLOOM_FFT_CB_H
