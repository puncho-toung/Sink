// bloom_fft.cs.hlsl - the bloom's convolution by a fast Fourier transform ( WP-J24 ). The light that passed the
// bloom's first pass is laid on a field of W x H cells ( a quarter to a half of the view's size, by the quality,
// with a black margin around it ), convolved there with a procedural kernel by the transform, and composed back
// over level 0 by builtin/post/bloom_fft_compose.ps.hlsl. Six passes ( and WP-J27's seventh ), chosen by PASS:
//   0  the kernel: each cell of the field, centred on ( 0, 0 ) with wrap-round, the mean of its subsamples of the
//      chosen shape ( diffraction spikes summed over BLOOM_FFT_WAVES wavelengths, or the inverse-square glow ),
//      under one radial window inside the margin the picture leaves, so that no light can wrap onto the picture
//   1  the rows forward: two rows of the picture ( the chain level, read with a tent ) packed as one complex row per
//      channel ( row a the real part, row b the imaginary ), transformed, and unpacked into the two rows' half
//      spectra, bins 0 .. W / 2; rows from gh on are not run
//   2  the kernel's rows forward: as 1, from the kernel, every row
//   3  the columns: one column of half-spectrum bins forward ( rows from gh on read as 0 ), times the kernel's
//      spectrum, and back; only rows below gh are written
//   4  the kernel's columns forward, written as the kernel's spectrum in natural order ( real: the kernel is
//      even, K( x, y ) = K( -x, -y ) )
//   5  the rows back: each pair's full spectrum rebuilt from its two half spectra, transformed back, the real part
//      row a and the imaginary part row b, into the image's cells of the result
//   6  ( WP-J27 ) 3 for a light split into classes: each class's column forward times its own kernel, summed, then back
// The transforms are radix-2 in group-shared memory, in place: forward by decimation in frequency ( natural order in,
// bit-reversed out ), back by decimation in time ( bit-reversed in, natural out ), so nothing is reordered: a bin is
// read at its bit-reversed slot. One pass of 3 divides by W x H and by the kernel's sum ( its spectrum's first bin ), so
// the kernel sums to 1 and the result is the convolution.
// WP-J27, the sources' own blooms: where a view holds sources with styles of their own the light is split into classes,
// one a kernel ( classes.x of them, 2 to 4 ). PASS 1 runs once a class ( the group's y ), each source texel times its
// class's share ( bloom_share.cs.hlsl ), into the spectra's slice of that class; PASS 6, in PASS 3's place, transforms
// each class's column, multiplies it by its own kernel's spectrum ( the kernel spectra are slices too, one a kernel, kept
// between views ) and sums the products, then transforms the sum back once, into slice 0, which PASS 5 reads. PASS 3 is
// the one-class pass, without the sum's group-shared memory, so a view of one class keeps its occupancy. WP-J24 in plan section 07 is the specification. tools/fft_proto_2026-11-27.py in the run folder is this file's index math in
// numpy, checked against numpy's own transform.

#include "vulkan.hlsli"
#include "bloom_fft_cb.h"

// *INDENT-OFF*
cbuffer c_BloomFft : register( b1 )
{
	BloomFftConstants g_Fft;
};

#if PASS == 0
VK_IMAGE_FORMAT( "rgba32f" ) RWTexture2D<float4> u_Kernel : register( u0 );
#elif PASS == 1
Texture2D t_Source : register( t0 );
Texture2D<float4> t_Shares : register( t1 );		// WP-J27: the four slots' shares of each source texel ( read where classes.x > 1 )
VK_IMAGE_FORMAT( "rgba32f" ) RWTexture2DArray<float4> u_SpecRG : register( u0 );
VK_IMAGE_FORMAT( "rg32f" ) RWTexture2DArray<float2> u_SpecB : register( u1 );
#elif PASS == 2
Texture2D<float4> t_Kernel : register( t0 );
VK_IMAGE_FORMAT( "rgba32f" ) RWTexture2DArray<float4> u_SpecRG : register( u0 );
VK_IMAGE_FORMAT( "rg32f" ) RWTexture2DArray<float2> u_SpecB : register( u1 );
#elif PASS == 3 || PASS == 6
Texture2DArray<float4> t_KernelSpec : register( t0 );
VK_IMAGE_FORMAT( "rgba32f" ) RWTexture2DArray<float4> u_SpecRG : register( u0 );
VK_IMAGE_FORMAT( "rg32f" ) RWTexture2DArray<float2> u_SpecB : register( u1 );
#elif PASS == 4
VK_IMAGE_FORMAT( "rgba32f" ) RWTexture2DArray<float4> u_SpecRG : register( u0 );
VK_IMAGE_FORMAT( "rg32f" ) RWTexture2DArray<float2> u_SpecB : register( u1 );
VK_IMAGE_FORMAT( "rgba32f" ) RWTexture2DArray<float4> u_KernelSpec : register( u2 );
#elif PASS == 5
VK_IMAGE_FORMAT( "rgba32f" ) RWTexture2DArray<float4> u_SpecRG : register( u0 );
VK_IMAGE_FORMAT( "rg32f" ) RWTexture2DArray<float2> u_SpecB : register( u1 );
VK_IMAGE_FORMAT( "rgba16f" ) RWTexture2D<float4> u_Result : register( u2 );
#endif
// *INDENT-ON*

static const float BLOOM_FFT_PI = 3.14159265358979;

#if PASS != 0
// the three channels' rows or columns, one complex value a cell. The columns' passes ( 3, 4, 6 ) hold a column of H cells,
// at most half the longest row: review R2-F1, PASS 6 with its sum stays at 24 KB, under D3D12's 32 KB
#if PASS == 3 || PASS == 4 || PASS == 6
groupshared float2 s_Fft[3][BLOOM_FFT_MAX / 2];
#else
groupshared float2 s_Fft[3][BLOOM_FFT_MAX];
#endif
#if PASS == 6
// WP-J27: the classes' products summed, a column ( H is at most half the longest transform )
groupshared float2 s_Acc[3][BLOOM_FFT_MAX / 2];
#endif

float2 CMul( float2 a, float2 b )
{
	return float2( a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x );
}

// exp( sign * i * pi * j / half ): the twiddle of butterfly j in a stage whose pairs are half apart
float2 Twiddle( uint j, uint halfSpan, float sign )
{
	float s, c;
	sincos( sign * BLOOM_FFT_PI * ( float )j / ( float )halfSpan, s, c );
	return float2( c, s );
}

uint BitReverse( uint i, uint logN )
{
	return reversebits( i ) >> ( 32u - logN );
}

// Forward transform in place by decimation in frequency: natural order in, bit-reversed order out. sign -1 is the
// forward direction. Every thread of the group calls it ( the barriers are the group's ); n / 2 butterflies a stage
void FftDif( uint tid, uint n, uint logN, float sign )
{
	for( uint stage = logN; stage >= 1u; stage-- )
	{
		const uint halfSpan = 1u << ( stage - 1u );
		for( uint b = tid; b < ( n >> 1u ); b += BLOOM_FFT_THREADS )
		{
			const uint j = b & ( halfSpan - 1u );
			const uint i0 = ( ( b >> ( stage - 1u ) ) << stage ) | j;
			const uint i1 = i0 + halfSpan;
			const float2 w = Twiddle( j, halfSpan, sign );
			[unroll]
			for( uint c = 0; c < 3; c++ )
			{
				const float2 a = s_Fft[c][i0];
				const float2 d = s_Fft[c][i1];
				s_Fft[c][i0] = a + d;
				s_Fft[c][i1] = CMul( a - d, w );
			}
		}
		GroupMemoryBarrierWithGroupSync();
	}
}

// Transform in place by decimation in time: bit-reversed order in, natural order out. sign +1 is the way back
void FftDit( uint tid, uint n, uint logN, float sign )
{
	for( uint stage = 1u; stage <= logN; stage++ )
	{
		const uint halfSpan = 1u << ( stage - 1u );
		for( uint b = tid; b < ( n >> 1u ); b += BLOOM_FFT_THREADS )
		{
			const uint j = b & ( halfSpan - 1u );
			const uint i0 = ( ( b >> ( stage - 1u ) ) << stage ) | j;
			const uint i1 = i0 + halfSpan;
			const float2 w = Twiddle( j, halfSpan, sign );
			[unroll]
			for( uint c = 0; c < 3; c++ )
			{
				const float2 a = s_Fft[c][i0];
				const float2 t = CMul( s_Fft[c][i1], w );
				s_Fft[c][i0] = a + t;
				s_Fft[c][i1] = a - t;
			}
		}
		GroupMemoryBarrierWithGroupSync();
	}
}
#endif

#if PASS == 1 || PASS == 2
// After the forward rows: bin k of the two packed rows, read at its bit-reversed slot with its mirror W - k.
// z = a + i b, so A( k ) = ( Z( k ) + conj( Z( W - k ) ) ) / 2 and B( k ) = ( Z( k ) - conj( Z( W - k ) ) ) / 2i.
// Writes row y0's bins and, where it is run, row y1's
void UnpackRows( uint tid, uint W, uint logW, uint y0, uint y1, bool writeB, uint slice )
{
	for( uint k = tid; k <= ( W >> 1u ); k += BLOOM_FFT_THREADS )
	{
		const uint sk = BitReverse( k, logW );
		const uint sn = BitReverse( ( W - k ) & ( W - 1u ), logW );
		float2 A[3];
		float2 B[3];
		[unroll]
		for( uint c = 0; c < 3; c++ )
		{
			const float2 zk = s_Fft[c][sk];
			const float2 zn = s_Fft[c][sn];
			const float2 zc = float2( zn.x, -zn.y );
			A[c] = 0.5 * ( zk + zc );
			const float2 d = zk - zc;
			B[c] = 0.5 * float2( d.y, -d.x );
		}
		u_SpecRG[ uint3( k, y0, slice ) ] = float4( A[0], A[1] );
		u_SpecB[ uint3( k, y0, slice ) ] = A[2];
		if( writeB )
		{
			u_SpecRG[ uint3( k, y1, slice ) ] = float4( B[0], B[1] );
			u_SpecB[ uint3( k, y1, slice ) ] = B[2];
		}
	}
}
#endif

#if PASS == 1
// The picture's cell ( x, y ): the source level under a tent one cell wide on either side, divided by s^2 ( R8-1 ): the
// tents of neighbouring cells sum to 1 over every source texel, so the field's sum times s^2 is the level's sum exactly,
// wherever a small lamp stands. Texels beyond the level's rectangle hold no light of the view and are not read. WP-J27:
// where the light is split ( split ), each texel counts times its class's share, the dot product of its four slots' shares
// with the class's mask; the classes' masks sum to 1 over the slots, so the classes' inputs sum to the whole light
float3 BloomFftInput( uint x, uint y, bool split, float4 mask )
{
	const float s = g_Fft.source.x;
	const float2 u = ( float2( x, y ) + 0.5 ) * s;
	const int2 lo = max( int2( ceil( u - s - 0.5 ) ), int2( 0, 0 ) );
	const int2 hi = min( int2( floor( u + s - 0.5 ) ), int2( g_Fft.image.zw ) - int2( 1, 1 ) );
	float3 sum = float3( 0.0, 0.0, 0.0 );
	[loop]
	for( int ty = lo.y; ty <= hi.y && ty <= lo.y + 6; ty++ )
	{
		const float wy = max( 1.0 - abs( ( float )ty + 0.5 - u.y ) / s, 0.0 );
		[loop]
		for( int tx = lo.x; tx <= hi.x && tx <= lo.x + 6; tx++ )
		{
			const float3 t = t_Source.Load( int3( tx, ty, 0 ) ).rgb * ( wy * max( 1.0 - abs( ( float )tx + 0.5 - u.x ) / s, 0.0 ) );
			sum += split ? t * dot( t_Shares.Load( int3( tx, ty, 0 ) ), mask ) : t;
		}
	}
	return sum * g_Fft.source.y;
}
#endif

#if PASS == 0
// One wavelength's diffraction pattern at ( u, v ) view heights from the lamp ( plan section 07, "The kernel" ): the halo
// round the source and the arms, each divided by its integral over the plane ( norms ), the halo carrying 0.6 of the
// light and the arms 0.4. floorW is the half-cell floor in the pattern's own units: no arm and no halo is narrower than
// half a cell on screen, so 3 x 3 subsamples never miss one ( R8-3 ); widths change no term's integral
float BloomFftSpikes( float u, float v, float floorW )
{
	const float r2 = u * u + v * v;
	const float rc = max( 0.003, floorW );
	const float halo = pow( 1.0 + r2 / ( rc * rc ), -1.5 ) / ( rc * rc );
	const uint n = ( uint )g_Fft.kernelA.z;
	const float L = g_Fft.kernelB.x;					// the arms' length
	const float wb = g_Fft.kernelB.y;					// their half-width at the lamp
	const float P = 0.010 + 0.08 * L;					// the ringing's period along an arm
	const float g = g_Fft.kernelC.x;
	const float c = g_Fft.shape.w;						// the arms' fall, 0.004 by the plan
	float arms = 0.0;
	[loop]
	for( uint i = 0; i < n; i++ )
	{
		const float phi = ( g_Fft.kernelA.w + ( float )i ) * BLOOM_FFT_PI / ( float )n;
		float sp, cp;
		sincos( phi, sp, cp );
		const float along = abs( u * cp + v * sp );
		const float across = abs( -u * sp + v * cp );
		const float w = max( wb + g_Fft.kernelB.z * 0.18 * along, floorW );
		const float t = across / w;
		const float ring = sin( BLOOM_FFT_PI * along / P );
		arms += exp( -t * t ) / w / ( 1.0 + along / c ) * exp( -( along * along ) / ( L * L ) ) * ( 1.0 - g * ring * ring );
	}
	return 0.6 * halo / g_Fft.norms.x + 0.4 * arms / g_Fft.norms.y;
}

// the inverse-square glow at ( u, v ) view heights: the wide glow to its reach R_g, and the glare's tight core times the
// glare, each term of energy 1 before its factor ( the kernel is then divided by its sum: its energy is 1 )
float BloomFftGlow( float u, float v )
{
	const float r2 = u * u + v * v;
	const float r0 = 0.002;
	const float R = g_Fft.kernelC.y;
	const float t = min( sqrt( r2 ) / R, 1.0 );
	const float wide = ( 1.0 - t * t ) * ( 1.0 - t * t ) / ( r2 + r0 * r0 );
	const float rg = 0.004;
	const float q = 1.0 + r2 / ( rg * rg );
	return wide / g_Fft.norms.z + g_Fft.kernelC.z / ( q * q ) / g_Fft.norms.w;
}

[numthreads( 8, 8, 1 )]
void main( uint3 id : SV_DispatchThreadID )
{
	const uint W = g_Fft.field.x;
	const uint H = g_Fft.field.y;
	if( id.x >= W || id.y >= H )
	{
		return;
	}
	const float dx = ( id.x < ( W >> 1u ) ) ? ( float )id.x : ( float )id.x - ( float )W;
	const float dy = ( id.y < ( H >> 1u ) ) ? ( float )id.y : ( float )id.y - ( float )H;
	// the unpaired column and row ( x = W / 2, y = H / 2 ) stay 0, so the kernel is even
	if( id.x == ( W >> 1u ) || id.y == ( H >> 1u ) )
	{
		u_Kernel[ id.xy ] = float4( 0.0, 0.0, 0.0, 0.0 );
		return;
	}
	const float cellsPerViewHeight = g_Fft.kernelA.x;
	const uint sub = max( ( uint )g_Fft.kernelC.w, 1u );
	// the window: radial, ( 1 - ( r / R )^2 )^2 inside the reach R cells, 0 beyond, so no light wraps onto the picture
	const float rCells2 = ( dx * dx + dy * dy ) / ( g_Fft.shape.x * g_Fft.shape.x );
	if( rCells2 >= 1.0 )
	{
		u_Kernel[ id.xy ] = float4( 0.0, 0.0, 0.0, 0.0 );
		return;
	}
	// the ratio stretches the pattern by 2^ratio along x and 2^-ratio along y, keeping its area
	const float sx = exp2( -g_Fft.kernelB.w );
	const float sy = exp2( g_Fft.kernelB.w );
	// half a cell on screen, in the pattern's units before the wavelength's scale
	const float floorBase = 0.5 * exp2( abs( g_Fft.kernelB.w ) ) / cellsPerViewHeight;
	float3 sum = float3( 0.0, 0.0, 0.0 );
	[loop]
	for( uint oy = 0; oy < sub; oy++ )
	{
		[loop]
		for( uint ox = 0; ox < sub; ox++ )
		{
			const float cu = ( dx + ( ( float )ox + 0.5 ) / ( float )sub - 0.5 ) / cellsPerViewHeight;
			const float cv = ( dy + ( ( float )oy + 0.5 ) / ( float )sub - 0.5 ) / cellsPerViewHeight;
			if( g_Fft.kernelA.y < 0.5 )
			{
				// each wavelength's pattern scaled by its wavelength, 1 / k^2 keeping its light
				[loop]
				for( uint i = 0; i < BLOOM_FFT_WAVES; i++ )
				{
					const float k = g_Fft.waves[i].w;
					sum += g_Fft.waves[i].rgb * ( BloomFftSpikes( cu * sx / k, cv * sy / k, floorBase / k ) / ( k * k ) );
				}
			}
			else
			{
				sum += BloomFftGlow( cu, cv ).xxx;
			}
		}
	}
	sum /= ( float )( sub * sub );
	const float window = 1.0 - rCells2;
	u_Kernel[ id.xy ] = float4( sum * ( window * window ), 0.0 );
}

#elif PASS == 1 || PASS == 2

[numthreads( BLOOM_FFT_THREADS, 1, 1 )]
void main( uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID )
{
	const uint tid = gtid.x;
	const uint W = g_Fft.field.x;
	const uint logW = g_Fft.field.z;
	const uint y0 = gid.x * 2u;
	const uint y1 = y0 + 1u;
#if PASS == 1
	const uint gw = g_Fft.image.x;
	const uint gh = g_Fft.image.y;
	// WP-J27: the group's class ( its y ), its slice of the spectra, its mask of the slots' shares
	const uint slice = gid.y;
	const bool split = g_Fft.classes.x > 1u;
	const float4 mask = g_Fft.classMask[ min( slice, ( uint )( BLOOM_FFT_CLASSES - 1 ) ) ];
	for( uint x = tid; x < W; x += BLOOM_FFT_THREADS )
	{
		float3 a = float3( 0.0, 0.0, 0.0 );
		float3 b = float3( 0.0, 0.0, 0.0 );
		if( x < gw )
		{
			a = BloomFftInput( x, y0, split, mask );
			if( y1 < gh )
			{
				b = BloomFftInput( x, y1, split, mask );
			}
		}
		s_Fft[0][x] = float2( a.r, b.r );
		s_Fft[1][x] = float2( a.g, b.g );
		s_Fft[2][x] = float2( a.b, b.b );
	}
	const bool writeB = y1 < gh;
#else
	for( uint x = tid; x < W; x += BLOOM_FFT_THREADS )
	{
		const float3 a = t_Kernel.Load( int3( x, y0, 0 ) ).rgb;
		const float3 b = t_Kernel.Load( int3( x, y1, 0 ) ).rgb;
		s_Fft[0][x] = float2( a.r, b.r );
		s_Fft[1][x] = float2( a.g, b.g );
		s_Fft[2][x] = float2( a.b, b.b );
	}
	const bool writeB = true;
	const uint slice = 0u;		// the kernel's rows: slice 0 is the kernel's scratch, before the light's rows fill it
#endif
	GroupMemoryBarrierWithGroupSync();
	FftDif( tid, W, logW, -1.0 );
	UnpackRows( tid, W, logW, y0, y1, writeB, slice );
}

#elif PASS == 3 || PASS == 4 || PASS == 6

[numthreads( BLOOM_FFT_THREADS, 1, 1 )]
void main( uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID )
{
	const uint tid = gtid.x;
	const uint k = gid.x;
	const uint H = g_Fft.field.y;
	const uint logH = g_Fft.field.w;
#if PASS == 3
	// one class: the column forward, times the kernel's spectrum, and back
	const uint rows = g_Fft.image.y;		// rows from gh on hold no light
	for( uint y = tid; y < H; y += BLOOM_FFT_THREADS )
	{
		float4 rg = float4( 0.0, 0.0, 0.0, 0.0 );
		float2 bb = float2( 0.0, 0.0 );
		if( y < rows )
		{
			rg = u_SpecRG[ uint3( k, y, 0 ) ];
			bb = u_SpecB[ uint3( k, y, 0 ) ];
		}
		s_Fft[0][y] = rg.xy;
		s_Fft[1][y] = rg.zw;
		s_Fft[2][y] = bb;
	}
	GroupMemoryBarrierWithGroupSync();
	FftDif( tid, H, logH, -1.0 );
	// times the kernel's spectrum, read at the bin each slot holds; one divide by W x H for the two transforms back
	// and by the kernel's sum, its spectrum's first bin: the kernel's energy is 1, its centre included
	const float3 total = t_KernelSpec.Load( int4( 0, 0, 0, 0 ) ).rgb;
	const float3 norm = 1.0 / ( ( float )g_Fft.field.x * ( float )H * max( total, float3( 1e-30, 1e-30, 1e-30 ) ) );
	for( uint s = tid; s < H; s += BLOOM_FFT_THREADS )
	{
		const float3 ks = t_KernelSpec.Load( int4( k, BitReverse( s, logH ), 0, 0 ) ).rgb * norm;
		s_Fft[0][s] *= ks.r;
		s_Fft[1][s] *= ks.g;
		s_Fft[2][s] *= ks.b;
	}
	GroupMemoryBarrierWithGroupSync();
	FftDit( tid, H, logH, 1.0 );
	for( uint y = tid; y < rows; y += BLOOM_FFT_THREADS )
	{
		u_SpecRG[ uint3( k, y, 0 ) ] = float4( s_Fft[0][y], s_Fft[1][y] );
		u_SpecB[ uint3( k, y, 0 ) ] = s_Fft[2][y];
	}
#elif PASS == 6
	const uint rows = g_Fft.image.y;		// rows from gh on hold no light
	// WP-J27: one column forward a class ( its slice of the spectra ), times its own kernel's spectrum, the products summed
	// in s_Acc and the sum transformed back once
	const uint classes = clamp( g_Fft.classes.x, 1u, ( uint )BLOOM_FFT_CLASSES );
	for( uint c = 0; c < classes; c++ )
	{
		for( uint y = tid; y < H; y += BLOOM_FFT_THREADS )
		{
			float4 rg = float4( 0.0, 0.0, 0.0, 0.0 );
			float2 bb = float2( 0.0, 0.0 );
			if( y < rows )
			{
				rg = u_SpecRG[ uint3( k, y, c ) ];
				bb = u_SpecB[ uint3( k, y, c ) ];
			}
			s_Fft[0][y] = rg.xy;
			s_Fft[1][y] = rg.zw;
			s_Fft[2][y] = bb;
		}
		GroupMemoryBarrierWithGroupSync();
		FftDif( tid, H, logH, -1.0 );
		// times the kernel's spectrum, read at the bin each slot holds; one divide by W x H for the two transforms back
		// and by the kernel's sum, its spectrum's first bin: the kernel's energy is 1, its centre included
		const uint kernelSlice = g_Fft.classKernel[c];
		const float3 total = t_KernelSpec.Load( int4( 0, 0, kernelSlice, 0 ) ).rgb;
		const float3 norm = 1.0 / ( ( float )g_Fft.field.x * ( float )H * max( total, float3( 1e-30, 1e-30, 1e-30 ) ) );
		for( uint s = tid; s < H; s += BLOOM_FFT_THREADS )
		{
			const float3 ks = t_KernelSpec.Load( int4( k, BitReverse( s, logH ), kernelSlice, 0 ) ).rgb * norm;
			const float2 p0 = s_Fft[0][s] * ks.r;
			const float2 p1 = s_Fft[1][s] * ks.g;
			const float2 p2 = s_Fft[2][s] * ks.b;
			s_Acc[0][s] = ( c == 0u ) ? p0 : s_Acc[0][s] + p0;
			s_Acc[1][s] = ( c == 0u ) ? p1 : s_Acc[1][s] + p1;
			s_Acc[2][s] = ( c == 0u ) ? p2 : s_Acc[2][s] + p2;
		}
		GroupMemoryBarrierWithGroupSync();
	}
	for( uint s = tid; s < H; s += BLOOM_FFT_THREADS )
	{
		s_Fft[0][s] = s_Acc[0][s];
		s_Fft[1][s] = s_Acc[1][s];
		s_Fft[2][s] = s_Acc[2][s];
	}
	GroupMemoryBarrierWithGroupSync();
	FftDit( tid, H, logH, 1.0 );
	for( uint y = tid; y < rows; y += BLOOM_FFT_THREADS )
	{
		u_SpecRG[ uint3( k, y, 0 ) ] = float4( s_Fft[0][y], s_Fft[1][y] );
		u_SpecB[ uint3( k, y, 0 ) ] = s_Fft[2][y];
	}
#else
	// the kernel's columns: the scratch slice 0 the kernel's rows left
	for( uint y = tid; y < H; y += BLOOM_FFT_THREADS )
	{
		const float4 rg = u_SpecRG[ uint3( k, y, 0 ) ];
		const float2 bb = u_SpecB[ uint3( k, y, 0 ) ];
		s_Fft[0][y] = rg.xy;
		s_Fft[1][y] = rg.zw;
		s_Fft[2][y] = bb;
	}
	GroupMemoryBarrierWithGroupSync();
	FftDif( tid, H, logH, -1.0 );
	// the kernel's spectrum in natural order ( a real, even kernel has a real spectrum ), into the slice of the kernel
	// being made ( classes.y )
	for( uint s = tid; s < H; s += BLOOM_FFT_THREADS )
	{
		u_KernelSpec[ uint3( k, BitReverse( s, logH ), g_Fft.classes.y ) ] = float4( s_Fft[0][s].x, s_Fft[1][s].x, s_Fft[2][s].x, 0.0 );
	}
#endif
}

#elif PASS == 5

[numthreads( BLOOM_FFT_THREADS, 1, 1 )]
void main( uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID )
{
	const uint tid = gtid.x;
	const uint W = g_Fft.field.x;
	const uint logW = g_Fft.field.z;
	const uint halfW = W >> 1u;
	const uint gw = g_Fft.image.x;
	const uint gh = g_Fft.image.y;
	const uint y0 = gid.x * 2u;
	const uint y1 = y0 + 1u;
	const bool hasB = y1 < gh;
	// the pair's full spectrum, z = A + i B, into bit-reversed slots; the upper bins are the lower ones' conjugates
	for( uint k = tid; k < W; k += BLOOM_FFT_THREADS )
	{
		const uint kk = ( k <= halfW ) ? k : W - k;
		const float4 arg = u_SpecRG[ uint3( kk, y0, 0 ) ];
		const float2 ab = u_SpecB[ uint3( kk, y0, 0 ) ];
		float4 brg = float4( 0.0, 0.0, 0.0, 0.0 );
		float2 bb = float2( 0.0, 0.0 );
		if( hasB )
		{
			brg = u_SpecRG[ uint3( kk, y1, 0 ) ];
			bb = u_SpecB[ uint3( kk, y1, 0 ) ];
		}
		float2 A[3] = { arg.xy, arg.zw, ab };
		float2 B[3] = { brg.xy, brg.zw, bb };
		const float conjSign = ( k <= halfW ) ? 1.0 : -1.0;
		const uint slot = BitReverse( k, logW );
		[unroll]
		for( uint c = 0; c < 3; c++ )
		{
			const float2 a = float2( A[c].x, conjSign * A[c].y );
			const float2 b = float2( B[c].x, conjSign * B[c].y );
			s_Fft[c][slot] = float2( a.x - b.y, a.y + b.x );
		}
	}
	GroupMemoryBarrierWithGroupSync();
	FftDit( tid, W, logW, 1.0 );
	for( uint x = tid; x < gw; x += BLOOM_FFT_THREADS )
	{
		// the rounding of the transforms leaves tiny negatives where there is no light
		u_Result[ uint2( x, y0 ) ] = float4( max( float3( s_Fft[0][x].x, s_Fft[1][x].x, s_Fft[2][x].x ), 0.0 ), 1.0 );
		if( hasB )
		{
			u_Result[ uint2( x, y1 ) ] = float4( max( float3( s_Fft[0][x].y, s_Fft[1][x].y, s_Fft[2][x].y ), 0.0 ), 1.0 );
		}
	}
}

#endif
