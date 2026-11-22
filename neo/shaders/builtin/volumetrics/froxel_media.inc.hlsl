// froxel_media.inc.hlsl - the froxel fog's media ( track V, package V4; track M extends it ): the media list's entry, the
// shapes and the noise slots. A medium's density at a point of the relative frame is its shape's times its slots'.
// The C++ mirror of fogMedium_t is renderer/Passes/VolumetricFogPass_cb.h, lane for lane; the media pass
// ( froxel_media.cs.hlsl ) sums the media of a view into the media volume.

#ifndef FROXEL_MEDIA_INC_HLSL
#define FROXEL_MEDIA_INC_HLSL

struct fogMedium_t			// 208 bytes
{
	float4 sphere;			// xyz = the bounding sphere's centre in the relative frame, w = 1 / r^2; 0 = unbounded ( no group test )
	float4 sigma;			// rgb = sigma_s at full density ( the albedo times sigma_t ), a = sigma_t at full density, per unit times the store scale
	float4 shape;			// x = the shape ( FOG_SHAPE_* ); HEIGHT: y = the base height in the medium's frame ( its z ), z = 1 / the falloff; BOX: yzw = the half extents
	float4 frame[3];		// the medium's frame: coordinate i of a relative point p is dot( frame[i].xyz, p ) + frame[i].w
	float4 noiseOffset;		// xyz = added to the frame's coordinates where the slots read them ( the wind's drift, the map's wrapped offset ), w = the slots in use
	float4 slot[6];			// noise slots 1 and 2, three lanes each ( FroxelMediumSlots )
};

static const uint FOG_SHAPE_ALL		= 0;	// everywhere
static const uint FOG_SHAPE_HEIGHT	= 1;	// full below a height, falling off above it
static const uint FOG_SHAPE_BOX		= 2;	// inside a box of the medium's frame

// a point of the relative frame in the medium's frame
float3 FroxelMediumFrame( fogMedium_t m, float3 p )
{
	return float3( dot( m.frame[0].xyz, p ) + m.frame[0].w, dot( m.frame[1].xyz, p ) + m.frame[1].w, dot( m.frame[2].xyz, p ) + m.frame[2].w );
}

// the shape's density, 0 to 1, at a point whose place in the medium's frame is q
float FroxelMediumShape( fogMedium_t m, float3 q )
{
	const uint shape = uint( m.shape.x );
	if( shape == FOG_SHAPE_HEIGHT )
	{
		return exp( -max( q.z - m.shape.y, 0.0 ) * m.shape.z );
	}
	if( shape == FOG_SHAPE_BOX )
	{
		return all( abs( q ) <= m.shape.yzw ) ? 1.0 : 0.0;
	}
	return 1.0;
}

// V4: the noise slots ( slots 1 and 2 of a medium; track M adds slot 3, the edge ). Every slot is computed by math, never
// read from a texture: integer hashes ( PCG3D ), so every GPU gives the same field; lattices that repeat every `tile` cells
// of the base octave, so a medium fixed to the map, whose noise reads its coordinates plus the map's offset wrapped to
// 1024 units, sees the world's own field ( its cell divides 1024 ); a lacunarity of 2, so every octave's cell divides the
// base's. A slot stops at the octave whose cell is finer than two froxel widths and fades that octave in by the fraction:
// far froxels run fewer octaves, and each fBm there tends to 0.5 ( the gradient noise's middle; Worley's own middle is about
// 0.46, and Perlin-Worley's remap of two such values is 2/3: a far dust of those kinds is a little denser than its mean ).

static const uint FOG_NOISE_NONE			= 0;
static const uint FOG_NOISE_FBM				= 1;	// gradient-noise fBm: soft lumps
static const uint FOG_NOISE_WORLEY			= 2;	// cellular noise: 1 - F1, round billows; or F2 - F1 ( metric 1 ), ridges
static const uint FOG_NOISE_PERLINWORLEY	= 3;	// fBm remapped by Worley's fBm: puffy lumps with soft insides
static const uint FOG_NOISE_CURL			= 4;	// the curl of a noise potential: a warp of the points later slots read

static const uint FOG_OP_MULTIPLY	= 0;		// v = v * n
static const uint FOG_OP_ADD		= 1;		// v = saturate( v + n * strength )
static const uint FOG_OP_ERODE		= 2;		// v = saturate( ( v - n * strength ) / ( 1 - n * strength ) ): thins the low parts first
static const uint FOG_OP_REMAP		= 3;		// v = saturate( ( v - n ) / ( 1 - n ) )
static const uint FOG_OP_WARP		= 4;		// the curl's: the point moves by the curl times strength for every later slot ( a curl slot always warps; any other kind with this op adds nothing )

// PCG3D ( Jarzynski and Olano 2020 ): three 32-bit hashes of three integers
uint3 FroxelPcg3d( uint3 v )
{
	v = v * 1664525u + 1013904223u;
	v.x += v.y * v.z;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	v ^= v >> 16u;
	v.x += v.y * v.z;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	return v;
}

// the hash of a lattice cell: the cell wrapped to the tile ( 0: no wrap ), and a seed per slot, octave and use
uint3 FroxelCellHash( int3 cell, uint tile, uint seed )
{
	if( tile > 0 )
	{
		const int t = int( tile );
		cell = ( cell % t + t ) % t;
	}
	return FroxelPcg3d( uint3( cell ) + seed * uint3( 0x9E3779B9u, 0x7F4A7C15u, 0x6A09E667u ) );
}

// three numbers in [ 0, 1 ) from a hash: its 24 high bits each, exact in a float
float3 FroxelHash01( uint3 h )
{
	return float3( h >> 8u ) * ( 1.0 / 16777216.0 );
}

// the lattice cells round a cell c, wrapped to the tile ( 0: no wrap ) once: lo the cell before c, c0 c itself, hi the cell
// after it, each component on its own. The neighbours' hashes then need no wrap of their own ( review O2-1: two signed
// modulos a component on every hash cost more than the hash itself )
void FroxelWrapCells( int3 c, uint tile, out int3 lo, out int3 c0, out int3 hi )
{
	if( tile == 0 )
	{
		lo = c - 1;
		c0 = c;
		hi = c + 1;
		return;
	}
	const int t = int( tile );
	c0 = ( c % t + t ) % t;
	lo = int3( ( c0.x == 0 ) ? t - 1 : c0.x - 1, ( c0.y == 0 ) ? t - 1 : c0.y - 1, ( c0.z == 0 ) ? t - 1 : c0.z - 1 );
	hi = int3( ( c0.x == t - 1 ) ? 0 : c0.x + 1, ( c0.y == t - 1 ) ? 0 : c0.y + 1, ( c0.z == t - 1 ) ? 0 : c0.z + 1 );
}

// a cell's hash by its offset o from c ( each component -1, 0 or 1, a constant where the caller unrolls ), from the cells
// FroxelWrapCells wrapped
uint3 FroxelNeighbourHash( int3 lo, int3 c0, int3 hi, int3 o, uint seed )
{
	const int3 cell = int3( ( o.x < 0 ) ? lo.x : ( ( o.x > 0 ) ? hi.x : c0.x ), ( o.y < 0 ) ? lo.y : ( ( o.y > 0 ) ? hi.y : c0.y ),
							( o.z < 0 ) ? lo.z : ( ( o.z > 0 ) ? hi.z : c0.z ) );
	return FroxelCellHash( cell, 0u, seed );
}

// gradient noise at p ( in cells ): x = the value, mostly within -1 .. 1 and at most 1.5 where the eight gradients line up
// ( the slots' 0.5 + 0.5 x is cut there ), yzw = its gradient in cells ( the curl reads it ).
// Random gradients in the cube at the corners, a quintic blend; the analytic derivative of the blend
float4 FroxelGradientNoise( float3 p, uint tile, uint seed )
{
	const float3 i = floor( p );
	const float3 f = p - i;
	const float3 u = f * f * f * ( f * ( f * 6.0 - 15.0 ) + 10.0 );
	const float3 du = 30.0 * f * f * ( f * ( f - 2.0 ) + 1.0 );
	const int3 c = int3( i );
	int3 lo, c0, hi;
	FroxelWrapCells( c, tile, lo, c0, hi );
	const float3 ga = FroxelHash01( FroxelNeighbourHash( lo, c0, hi, int3( 0, 0, 0 ), seed ) ) * 2.0 - 1.0;
	const float3 gb = FroxelHash01( FroxelNeighbourHash( lo, c0, hi, int3( 1, 0, 0 ), seed ) ) * 2.0 - 1.0;
	const float3 gc = FroxelHash01( FroxelNeighbourHash( lo, c0, hi, int3( 0, 1, 0 ), seed ) ) * 2.0 - 1.0;
	const float3 gd = FroxelHash01( FroxelNeighbourHash( lo, c0, hi, int3( 1, 1, 0 ), seed ) ) * 2.0 - 1.0;
	const float3 ge = FroxelHash01( FroxelNeighbourHash( lo, c0, hi, int3( 0, 0, 1 ), seed ) ) * 2.0 - 1.0;
	const float3 gf = FroxelHash01( FroxelNeighbourHash( lo, c0, hi, int3( 1, 0, 1 ), seed ) ) * 2.0 - 1.0;
	const float3 gg = FroxelHash01( FroxelNeighbourHash( lo, c0, hi, int3( 0, 1, 1 ), seed ) ) * 2.0 - 1.0;
	const float3 gh = FroxelHash01( FroxelNeighbourHash( lo, c0, hi, int3( 1, 1, 1 ), seed ) ) * 2.0 - 1.0;
	const float va = dot( ga, f - float3( 0.0, 0.0, 0.0 ) );
	const float vb = dot( gb, f - float3( 1.0, 0.0, 0.0 ) );
	const float vc = dot( gc, f - float3( 0.0, 1.0, 0.0 ) );
	const float vd = dot( gd, f - float3( 1.0, 1.0, 0.0 ) );
	const float ve = dot( ge, f - float3( 0.0, 0.0, 1.0 ) );
	const float vf = dot( gf, f - float3( 1.0, 0.0, 1.0 ) );
	const float vg = dot( gg, f - float3( 0.0, 1.0, 1.0 ) );
	const float vh = dot( gh, f - float3( 1.0, 1.0, 1.0 ) );
	const float k0 = vb - va;
	const float k1 = vc - va;
	const float k2 = ve - va;
	const float k3 = va - vb - vc + vd;
	const float k4 = va - vc - ve + vg;
	const float k5 = va - vb - ve + vf;
	const float k6 = -va + vb + vc - vd + ve - vf - vg + vh;
	const float value = va + u.x * k0 + u.y * k1 + u.z * k2 + u.x * u.y * k3 + u.y * u.z * k4 + u.z * u.x * k5 + u.x * u.y * u.z * k6;
	const float3 gradient = ga + u.x * ( gb - ga ) + u.y * ( gc - ga ) + u.z * ( ge - ga ) + u.x * u.y * ( ga - gb - gc + gd )
							+ u.y * u.z * ( ga - gc - ge + gg ) + u.z * u.x * ( ga - gb - ge + gf ) + u.x * u.y * u.z * ( -ga + gb + gc - gd + ge - gf - gg + gh )
							+ du * float3( k0 + u.y * k3 + u.z * k5 + u.y * u.z * k6, k1 + u.z * k4 + u.x * k3 + u.z * u.x * k6, k2 + u.x * k5 + u.y * k4 + u.x * u.y * k6 );
	return float4( value, gradient );
}

// cellular noise at p ( in cells ): one feature point a cell, moved inside it by jitter; metric 0 gives 1 - F1, 1 gives
// F2 - F1, each cut to 0 .. 1. The 27 cells round p hold the nearest feature in practice for any jitter up to 1 ( review O1-10:
// not a theorem; 0 of 2,000,000 points differed from a search of 125 cells ), and the second nearest
// only for a jitter up to 0.5 ( consultant CN3-1 ): F2 - F1 takes at most 0.5. Distances are compared squared, one root each
float FroxelWorley( float3 p, uint tile, uint seed, float jitter, uint metric )
{
	const float3 i = floor( p );
	const float3 f = p - i;
	const int3 c = int3( i );
	const float spread = ( metric == 1 ) ? min( jitter, 0.5 ) : jitter;
	float f1 = 64.0;
	float f2 = 64.0;
	int3 lo, c0, hi;
	FroxelWrapCells( c, tile, lo, c0, hi );
	[unroll]
	for( int z = -1; z <= 1; z++ )
	{
		[unroll]
		for( int y = -1; y <= 1; y++ )
		{
			[unroll]
			for( int x = -1; x <= 1; x++ )
			{
				const int3 o = int3( x, y, z );
				const float3 feature = float3( o ) + 0.5 + spread * ( FroxelHash01( FroxelNeighbourHash( lo, c0, hi, o, seed ) ) - 0.5 );
				const float3 e = feature - f;
				const float d = dot( e, e );
				f2 = ( d < f1 ) ? f1 : min( f2, d );
				f1 = min( f1, d );
			}
		}
	}
	return saturate( ( metric == 1 ) ? sqrt( f2 ) - sqrt( f1 ) : 1.0 - sqrt( f1 ) );
}

// one kind of noise as fBm: the octaves from the base cell ( 1 / frequency ) down, each half the cell and gain times the
// amplitude, stopping at the octave whose cell is finer than two froxel widths ( width ) and fading that one in. The value
// is 0 .. 1, its middle 0.5; a left-out octave adds its middle
float FroxelNoiseOctaves( uint type, float3 q, float frequency, uint octaves, float gain, float jitter, uint metric, uint tile, uint seed, float width )
{
	const float wanted = clamp( log2( 1.0 / max( frequency * 2.0 * width, 1e-6 ) ) + 1.0, 0.0, float( octaves ) );
	float sum = 0.0;
	float total = 0.0;
	float amplitude = 1.0;
	float f = frequency;
	uint t = tile;
	for( uint o = 0; o < octaves; o++ )
	{
		total += amplitude;
		const float fade = saturate( wanted - float( o ) );
		if( fade > 0.0 )
		{
			const float n = ( type == FOG_NOISE_WORLEY ) ? FroxelWorley( q * f, t, seed + o, jitter, metric ) : saturate( 0.5 + 0.5 * FroxelGradientNoise( q * f, t, seed + o ).x );
			sum += amplitude * fade * ( n - 0.5 );
		}
		amplitude *= gain;
		f *= 2.0;
		t *= 2u;
	}
	return saturate( 0.5 + sum / max( total, 1e-6 ) );
}

// the curl of a potential of three gradient noises, as fBm with the same octave cut: a field without sources, in cells
// of each octave ( about -1 .. 1 a component ), which a warp multiplies by its strength in units
float3 FroxelCurl( float3 q, float frequency, uint octaves, float gain, uint tile, uint seed, float width )
{
	const float wanted = clamp( log2( 1.0 / max( frequency * 2.0 * width, 1e-6 ) ) + 1.0, 0.0, float( octaves ) );
	float3 sum = float3( 0.0, 0.0, 0.0 );
	float total = 0.0;
	float amplitude = 1.0;
	float f = frequency;
	uint t = tile;
	for( uint o = 0; o < octaves; o++ )
	{
		total += amplitude;
		const float fade = saturate( wanted - float( o ) );
		if( fade > 0.0 )
		{
			const float3 a = FroxelGradientNoise( q * f, t, seed + 3 * o ).yzw;
			const float3 b = FroxelGradientNoise( q * f, t, seed + 3 * o + 1 ).yzw;
			const float3 c = FroxelGradientNoise( q * f, t, seed + 3 * o + 2 ).yzw;
			sum += amplitude * fade * float3( c.y - b.z, a.z - c.x, b.x - a.y );
		}
		amplitude *= gain;
		f *= 2.0;
		t *= 2u;
	}
	return sum / max( total, 1e-6 );
}

// the noise slots' value, 0 to 1, at a point q of the noise's frame, for a froxel of the given width across the view: slots
// 1 and 2 in order from v = 1. Slot s holds three lanes: [ 3s ] = type, op, octaves, metric; [ 3s + 1 ] = frequency
// ( 1 / the base cell in units ), gain, strength, bias; [ 3s + 2 ] = jitter ( Worley ), tile ( cells of the base octave
// before the lattice repeats; 0 = never ), 0, 0. A slot's noise n is its kind's value plus its bias, cut to 0 .. 1
float FroxelMediumSlots( fogMedium_t m, float3 q, float width )
{
	float v = 1.0;
	const uint slots = min( uint( m.noiseOffset.w ), 2u );
	// unrolled, so that every index into the medium's slots is a constant ( review O1-11: a dynamic one may go to scratch )
	[unroll]
	for( uint s = 0; s < 2; s++ )
	{
		if( s >= slots )
		{
			break;
		}
		const float4 kind = m.slot[ 3 * s ];
		const float4 shape = m.slot[ 3 * s + 1 ];
		const float4 lattice = m.slot[ 3 * s + 2 ];
		const uint type = uint( kind.x );
		const uint op = uint( kind.y );
		const uint octaves = clamp( uint( kind.z ), 1u, 8u );
		const uint tile = uint( lattice.y );
		const uint seed = 1u + 64u * s;
		if( type == FOG_NOISE_CURL )
		{
			q += FroxelCurl( q, shape.x, octaves, shape.y, tile, seed, width ) * shape.z;
			continue;
		}
		if( type == FOG_NOISE_NONE )
		{
			continue;
		}
		float n;
		if( type == FOG_NOISE_PERLINWORLEY )
		{
			// Schneider's remap of the fBm by the Worley fBm ( inverted F1 ): remap( fbm, worley - 1, 1, 0, 1 )
			const float fbm = FroxelNoiseOctaves( FOG_NOISE_FBM, q, shape.x, octaves, shape.y, lattice.x, 0, tile, seed, width );
			const float worley = FroxelNoiseOctaves( FOG_NOISE_WORLEY, q, shape.x, octaves, shape.y, lattice.x, 0, tile, seed + 32u, width );
			n = saturate( ( fbm - ( worley - 1.0 ) ) / max( 2.0 - worley, 1e-3 ) );
		}
		else
		{
			n = FroxelNoiseOctaves( type, q, shape.x, octaves, shape.y, lattice.x, uint( kind.w ), tile, seed, width );
		}
		n = saturate( n + shape.w );
		if( op == FOG_OP_ADD )
		{
			v = saturate( v + n * shape.z );
		}
		else if( op == FOG_OP_ERODE )
		{
			v = saturate( ( v - n * shape.z ) / max( 1.0 - n * shape.z, 1e-3 ) );
		}
		else if( op == FOG_OP_REMAP )
		{
			v = saturate( ( v - n ) / max( 1.0 - n, 1e-3 ) );
		}
		else if( op != FOG_OP_WARP )
		{
			v *= n;
		}
	}
	return v;
}

// a medium's density, 0 to 1, at a point p of the relative frame, for a froxel of the given width across the view
float FroxelMediumDensity( fogMedium_t m, float3 p, float width )
{
	const float3 q = FroxelMediumFrame( m, p );
	const float shape = FroxelMediumShape( m, q );
	if( shape <= 0.0 || m.noiseOffset.w < 0.5 )
	{
		return shape;
	}
	return shape * FroxelMediumSlots( m, q + m.noiseOffset.xyz, width );
}

#endif // FROXEL_MEDIA_INC_HLSL
