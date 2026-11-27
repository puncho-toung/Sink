/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2014 Robert Beckebans

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/
#include <precompiled.h>
#pragma hdrstop

#include "renderer/RenderCommon.h"

#include "BloomPass.h"
#include "BloomPass_cb.h"

#include <limits>			// std::numeric_limits: the probe's NaN and infinity for the R11G11B10F decoder

// r_bloom, the glow's brightness and its tail are a player's settings and are archived; the levels, the knee, the
// ceiling, the threshold and the debug value are a developer's tunables and are not, because an archived cvar is
// written to the config at the first run, and a later change of its default then never reaches that machine
idCVar r_bloom( "r_bloom", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE | CVAR_NEW, "bloom: what is brighter than r_bloomThreshold glows: a chain of half-size levels of the HDR frame, added by the tonemapper ( main view only )" );
static idCVar r_bloomLevels( "r_bloomLevels", "6", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "bloom: levels at 1080 lines; one more for each doubling of the view's height", 1, 9 );
static idCVar r_bloomFireflyKnee( "r_bloomFireflyKnee", "256", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "bloom: a lone very bright pixel adds at most about a third of this ( in exposed units ) to a texel of the bloom's first level; 0 = off", 0.0f, 1024.0f );
static idCVar r_bloomCeiling( "r_bloomCeiling", "256", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "bloom: the frame's values are held to this before they enter the bloom ( the brightest emitter is 64 ); 7000 = no ceiling but the bound that keeps the chain finite", 1.0f, 7000.0f );
// The glow ( WP-G3; the owner, 2026-11-25: "is there a way to get nice bloom/glow without blurring? I like how crisp
// the base game is" ): only what a pixel has ABOVE a brightness threshold enters the chain, and the chain is ADDED
// to the frame, whose own weight stays 1: a pixel no glow reaches is the pixel of r_bloom 0, and no edge loses
// contrast. It is the bloom's only form since WP-G4 ( the owner, 2026-11-26: "remove the old bloom entirely" ): the
// mix of v0001.3r, which swapped a share of the picture for its blur, went with its cvars r_bloomMode,
// r_bloomStrength and r_bloomTail ( a config that still sets them makes three cvars nothing reads ). r_bloomGlow and
// r_bloomGlowTail keep their names, so that a saved config keeps the player's look; they have external linkage for
// the System Options screen's two bars ( MenuScreen_Shell_SystemOptions.cpp )
idCVar r_bloomGlow( "r_bloomGlow", "0.24", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE | CVAR_NEW, "bloom: the glow's brightness ( 0 = none; the System Options bar Bloom Strength runs from 0 to 1 )", 0.0f, 4.0f );
idCVar r_bloomGlowTail( "r_bloomGlowTail", "0.34", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE | CVAR_NEW, "bloom: each level's weight against the one above; 1 = a wide inverse-square halo, 0.5 = a tight inverse-cube one ( the System Options bar Bloom Tail sets it )", 0.25f, 1.0f );
static idCVar r_bloomThreshold( "r_bloomThreshold", "1.5", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "bloom: a pixel glows by what its largest channel has above this, in exposed units ( a lit wall is about 1, a lamp 2 to 8 )", 0.01f, 1024.0f );
static idCVar r_bloomThresholdKnee( "r_bloomThresholdKnee", "0.5", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "bloom: the soft knee round the threshold, as a share of it ( 0 = a hard edge )", 0.0f, 1.0f );
// WP-G3, the distance tail ( the owner, 2026-11-25: "Everything having the same sized tail no matter how far away it is
// seems kind of fake" ). A glow made in the eye is the same size at any distance; one made in the air is a size in the
// world, wide when the lamp is near and small when it is far. A lamp nearer than r_bloomTailNear
// keeps the whole tail the player set and one beyond r_bloomTailFar its core alone ( level 0 ). The switch is a
// player's and archived ( review F1-7: the owner judges it by playing, and the choice must outlive the session );
// the distances and the far share are a developer's tunables and are not
static idCVar r_bloomTailDistance( "r_bloomTailDistance", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE | CVAR_NEW, "bloom: a lamp's tail shrinks with the distance of the opaque surface at its pixels ( needs the hierarchical depth buffer, r_useHierarchicalDepthBuffer; a translucent emitter takes the distance of what is behind it )" );
static idCVar r_bloomTailNear( "r_bloomTailNear", "128", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "bloom: a lamp nearer than this many units keeps the whole tail", 0.0f, 65536.0f );
static idCVar r_bloomTailFar( "r_bloomTailFar", "1024", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "bloom: a lamp beyond this many units keeps only its core", 1.0f, 65536.0f );
static idCVar r_bloomTailFarShare( "r_bloomTailFarShare", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "bloom: the share of its tail a lamp beyond r_bloomTailFar keeps ( 0 = the core alone )", 0.0f, 1.0f );
// WP-G4, the view weapon ( the owner, 2026-11-26: "is there a way to fix the bloom so it stops haloing the weapons in
// total darkness?" ). A lamp-lit weapon in the player's hands is the brightest thing in a dark room and fills a
// good part of the picture, so its glow stood as a halo over the dark behind it. At 0 the weapon's own pixels are
// black to the bloom: they give no light to any level. They are known by their depth ( BLOOM_WEAPON_DEPTH_* below ),
// which the first pass reads from the hierarchical depth buffer: without that buffer the weapon glows like the
// world. A developer's tunable, not archived
// WP-J21 ( row 336, ruled ): under the gate the weapon's emissive parts ( an ammo display, the plasma gun's coils ) and its
// muzzle flash pass and glow, and the lamp-lit weapon writes no mask, so -1 leaves the weapon in under the gate and out
// under the open gate; 0 and 1 force
static idCVar r_bloomViewWeapon( "r_bloomViewWeapon", "-1", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "bloom: -1 = by r_bloomSource ( 1 under the gate, 0 under the open gate ); 1 = the first-person weapon's pixels glow like the world's; 0 = they give no glow, so a lit weapon has no halo against the dark ( needs the hierarchical depth buffer, r_useHierarchicalDepthBuffer )", -1, 1 );
static idCVar r_bloomDebug( "r_bloomDebug", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "bloom: 1 = the bloom alone, 2..9 = one level alone, 10 = one tap where the tonemapper reads the chain, 11/12/13 = a NaN / an Inf / a 65504 pixel put into the frame's centre, 14 = the convolution's result alone ( r_bloomFFT 1 )", 0, 14 );

// WP-J24, the convolution by a Fourier transform ( the owner, 2026-11-27: "I also want to upgrade it some more to fast
// fourier transform, because the simple glow isn't enough", with the parameters of a convolution bloom; plan section
// 07 maps each, with the owner's ranges and defaults ). Behind the gate the first pass and its crisp core ( level 0 )
// are today's; the wide light is convolved with a procedural kernel on a field of cells and added back into level 0.
// The Strength bar is the owner's Intensity, the Tail bar the kernel's share, r_bloomCeiling the Whitepoint. 0 = the
// chain of v0001.3v, unchanged. r_bloomFFT is archived; the look values are a developer's until the owner has seen the
// look sheet ( an archived default never changes on a machine that wrote its config )
idCVar r_bloomFFT( "r_bloomFFT", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE | CVAR_NEW, "bloom: 1 = the glow's tail is a convolution by a fast Fourier transform with the shape r_bloomFFTShape chooses; 0 = the chain of half-size levels" );
idCVar r_bloomFFTQuality( "r_bloomFFTQuality", "1", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "bloom convolution: the field, 0 = 256 x 128 cells ( fastest, softest ), 1 = 512 x 256, 2 = 1024 x 512 ( sharpest, for screenshots )", 0, 2 );
idCVar r_bloomFFTShape( "r_bloomFFTShape", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "bloom convolution: the kernel, 0 = diffraction spikes, 1 = an inverse-square glow", 0, 1 );
idCVar r_bloomFFTPadding( "r_bloomFFTPadding", "0.25", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "bloom convolution: the black margin round the picture, which is the kernel's reach; more reaches further with coarser cells, 0 leaves the shaped glow its centre alone", 0.0f, 0.5f );
idCVar r_bloomExposureBias( "r_bloomExposureBias", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "bloom: the brightness of what passes the gate, in stops, before the ceiling and the threshold ( the picture itself does not change ): above 0 the faint parts of emitters glow too, below only the brightest", -5.0f, 5.0f );
idCVar r_bloomSpikes( "r_bloomSpikes", "3", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "bloom diffraction spikes: the arms through a lamp ( n arms are 2n rays )", 1, 7 );
idCVar r_bloomSpikeRotation( "r_bloomSpikeRotation", "0.5", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "bloom diffraction spikes: the star's turn, 0 and 1 the same", 0.0f, 1.0f );
idCVar r_bloomSpikeRadius( "r_bloomSpikeRadius", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "bloom diffraction spikes: how far the arms reach ( 0 short and tight, 1 the whole reach )", 0.0f, 1.0f );
idCVar r_bloomSpikeBlur( "r_bloomSpikeBlur", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "bloom diffraction spikes: the softness of the arms' edges", 0.0f, 1.0f );
idCVar r_bloomSpikeSpread( "r_bloomSpikeSpread", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "bloom diffraction spikes: how much each arm fans out along its length ( 0 thin lines )", 0.0f, 1.0f );
idCVar r_bloomSpikeRatio( "r_bloomSpikeRatio", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "bloom diffraction spikes: stretches the star along x ( above 0 ) or y ( below 0 ), its area kept", -1.0f, 1.0f );
idCVar r_bloomSpikeRinging( "r_bloomSpikeRinging", "0.75", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "bloom diffraction spikes: the bright and dark bands along each arm", 0.0f, 1.0f );
idCVar r_bloomSpikeShift( "r_bloomSpikeShift", "0.1", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "bloom diffraction spikes: the spectral shift, each wavelength's pattern scaled by ( wavelength / 560 nm )^( 4 x shift ): a rainbow along the arms ( 0.25 is the physical scale )", 0.0f, 1.0f );
// not the owner's list: the arms' fall along their length, which section 07 leaves to the owner's look sheet ( the
// draft's 1 / ( 1 + d / 0.004 view heights ) holds most of an arm's light within a cell or two of the lamp )
idCVar r_bloomSpikeFalloff( "r_bloomSpikeFalloff", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "bloom diffraction spikes, a developer's knob for the look sheet: the arms fade as 1 / ( 1 + d / c ); 0 is the plan's c, 0.004 view heights; above 0, c is this share of the arms' length ( larger carries the light further out )", 0.0f, 1.0f );
idCVar r_bloomGlowRadius( "r_bloomGlowRadius", "1", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "bloom inverse-square glow: how far the glow reaches, a share of the reach", 0.0f, 1.0f );
idCVar r_bloomGlare( "r_bloomGlare", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "bloom inverse-square glow: a tight bright core added to the glow", 0.0f, 1.0f );
// WP-J27, the hot-pixel boost ( the owner, 2026-11-27: "Do the hot pixel boost" ): an emitter's pixels brighter than the
// threshold enter the convolution brighter, up to 2^boost at the hot level and beyond, so a lamp's spikes or glow carry
// where the lamp's own few levels above the threshold would not ( what the reference shader's Whitepoint does to its
// near-white pixels ). Only the convolution's source: the core ( level 0 ) and the picture are untouched, and the boosted
// pixel is still held to the ceiling ( r_bloomCeiling, the owner's Whitepoint ). Under the gate and r_bloomFFT 1 only; a
// light's bloom_hot_boost is its own. The same at every distance ( the owner had the size rule removed, 2026-11-27: it
// made a lamp's glow strong far away and gone up close ); 3 stops, since near strips now take it too. Not archived until
// the owner has seen the look
idCVar r_bloomHotBoost( "r_bloomHotBoost", "3", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "bloom convolution: how many stops brighter an emitter's hottest pixels enter the convolution ( 0 = off; the core and the picture do not change, the ceiling still holds )", 0.0f, 8.0f );
idCVar r_bloomHotLevel( "r_bloomHotLevel", "2", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "bloom convolution: the exposed brightness at which a pixel takes the whole hot boost; from the threshold up to it the boost rises smoothly ( retail lamps are 2 to 3 )", 1.0f, 256.0f );

// WP-J21, the bloom's gate ( the owner, 2026-11-27: "lock bloom behind a material key/emissive materials and fog so it
// doesn't automatically make the game glowing and blurry. It can keep the same shape it has, it just needs to be gated
// and not applied automatically to every bright surface." ). 1: only what the emitting stages drew into the glow mask
// reaches the bloom, by today's rules after it; 0: the open gate of v0001.3v, everything bright; 2: the mask is drawn
// as under 1 and the bloom reads the frame ( a developer's comparison, the picture of 0 ). Archived, 1 from its first build
idCVar r_bloomSource( "r_bloomSource", "1", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE | CVAR_NEW, "bloom: what feeds it, 1 = emitters and fog only ( emissive materials, the key glow, lit froxel fog ), 0 = everything bright, 2 = everything bright while the glow mask is drawn ( a developer's comparison )", 0, 2 );
// WP-J21, the fog's glow ( row 337 ): under r_bloomSource 1 the light the froxel fog scatters from lights glows, its ambient
// and a fog light's own colour do not. A developer's switch to compare, no look value ( row 335 )
static idCVar r_bloomFogGlow( "r_bloomFogGlow", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "bloom: 1 = under r_bloomSource 1 the froxel fog's light from lights glows ( a developer's switch )" );

// 1 forces the chain's format to RGBA16_FLOAT even where the device can draw into, blend into and filter
// R11G11B10F ( to compare the two formats )
#define BLOOM_CHAIN_RGBA16F 0

// the probe's copies are read this many bloom views after they were made: the frame that holds them has been handed
// to the GPU by then ( the map itself waits for the GPU )
static const int BLOOM_PROBE_WAIT = 3;

// the largest value r_bloomCeiling may take: equals BLOOM_SCRUB_BOUND of shaders/builtin/post/bloom_cb.h. The up
// passes add up to nine levels in the blender, and nine times this must stay under the largest finite value of the
// chain's format ( 64512 ), so the ceiling may never be set above it
static const float BLOOM_CEILING_MAX = 7000.0f;

// The view weapon's depths ( WP-G4 ). The first-person weapon is drawn with idRenderMatrix::ApplyDepthHack, which
// squeezes its projected z into the nearest quarter of the depth range: reversed, ndc' = 0.25 ndc + 0.75, above
// 0.75; forward, 0.25 ndc, below 0.25. The world has such a depth only nearer than 4/3 of the near plane, where the
// player's own bounds keep it from coming. The first pass's test is depth * SIGN > BOUND ( the forward pair has not
// been run: the engine is built reversed ). Only idWeapon::PresentWeapon sets weaponDepthHack ( the weapon, the PDA,
// the flashlight ), and only while g_useWeaponDepthHack is on: with that debug cvar at 0 the weapon has a world's
// depth and glows. Under TAA the frame the bloom reads is the resolved one and the depth is this frame's: while the
// world slides behind the weapon, a rim of history beside its silhouette gets through, measured at 0.2 to 0.6 % of
// the weapon's light ( review OH1-F2; tools/taa_turn_g4.sh in the run folder )
#if ID_REVERSED_Z
static const float BLOOM_WEAPON_DEPTH_BOUND = 0.75f;
static const float BLOOM_WEAPON_DEPTH_SIGN = 1.0f;
#else
static const float BLOOM_WEAPON_DEPTH_BOUND = -0.25f;
static const float BLOOM_WEAPON_DEPTH_SIGN = -1.0f;
#endif

// counts the chains this pass and every earlier BloomPass made ( BloomMix::chainSerial ). A file-scope static, not
// an in-class initializer
static int bloomChainSerial = 0;

// Set by the console command bloomProbe, taken by the next blooming view. Console commands run on the main thread,
// where the backend runs too, so nothing else touches it
static bool bloomProbeRequested = false;

// WP-J24: set by the console command bloomFftDump, taken by the next view under the convolution
static bool bloomFftDumpRequested = false;

/*
========================
BloomCvarFloat

A float cvar of the pass, held to its range. Review O1-2: a NaN typed into a cvar passes the cvar's own range and
idMath::ClampFloat ( every comparison with a NaN is false ) and would reach the tonemap draw's weights or the
shader, and r_bloomGlow and r_bloomGlowTail are archived, so it would come back at every launch.
A value that is not a number is taken as the fallback
========================
*/
static float BloomCvarFloat( const idCVar& cvar, float low, float high, float fallback )
{
	const float value = cvar.GetFloat();
	if( IEEE_FLT_IS_NAN( value ) )
	{
		return fallback;
	}
	return idMath::ClampFloat( low, high, value );
}

/*
================================================================================================

WP-J24: the convolution's CPU side. The kernel itself is drawn on the GPU ( bloom_fft.cs.hlsl, PASS 0 ) from the
constants made here: the wavelengths' table and each term's integral over the plane, so that the terms carry their
shares of the light whatever the parameters. The kernel is then divided by its own sum on the GPU, so these
integrals need only set the terms' ratios.

================================================================================================
*/

// the kernel's subsamples a cell side ( 9 a cell: the arms are narrower than a cell at every quality )
static const int BLOOM_FFT_SUBSAMPLES = 3;

// the smallest image, in cells, the convolution runs on ( a smaller view takes the chain )
static const int BLOOM_FFT_MIN_IMAGE = 16;

// Wyman, Sloan and Shirley 2013, "Simple Analytic Approximations to the CIE XYZ Color Matching Functions": the
// multi-lobe fit of the CIE 1931 2-degree observer
static double BloomFftLobe( double x, double mu, double s1, double s2 )
{
	const double t = ( x - mu ) / ( ( x < mu ) ? s1 : s2 );
	return exp( -0.5 * t * t );
}

/*
========================
BloomFftWaves

The diffraction kernel's wavelengths: BLOOM_FFT_WAVES of them over 420 to 700 nm, each with its share of the red,
green and blue channels ( the CIE fit to XYZ, XYZ to linear sRGB, negatives cut, each channel's shares summing to 1,
so a white lamp's spikes are white in sum ) and the scale of its pattern, ( lambda / 560 )^( 4 shift ): the owner's
Spectral Shift, never 0. At 0 every wavelength draws the same pattern and the spikes are white; at 0.25 the scale is the
physical one; at 1 it runs from 0.32 to 2.44
========================
*/
static void BloomFftWaves( float spectral, idVec4* waves )
{
	double share[BLOOM_FFT_WAVES][3];
	double sum[3] = { 0.0, 0.0, 0.0 };
	for( int i = 0; i < BLOOM_FFT_WAVES; i++ )
	{
		const double lam = 420.0 + 280.0 * i / ( BLOOM_FFT_WAVES - 1 );
		const double X = 1.056 * BloomFftLobe( lam, 599.8, 37.9, 31.0 ) + 0.362 * BloomFftLobe( lam, 442.0, 16.0, 26.7 ) - 0.065 * BloomFftLobe( lam, 501.1, 20.4, 26.2 );
		const double Y = 0.821 * BloomFftLobe( lam, 568.8, 46.9, 40.5 ) + 0.286 * BloomFftLobe( lam, 530.9, 16.3, 31.1 );
		const double Z = 1.217 * BloomFftLobe( lam, 437.0, 11.8, 36.0 ) + 0.681 * BloomFftLobe( lam, 459.0, 26.0, 13.8 );
		share[i][0] = Max( 0.0, 3.2406 * X - 1.5372 * Y - 0.4986 * Z );
		share[i][1] = Max( 0.0, -0.9689 * X + 1.8758 * Y + 0.0415 * Z );
		share[i][2] = Max( 0.0, 0.0557 * X - 0.2040 * Y + 1.0570 * Z );
		for( int c = 0; c < 3; c++ )
		{
			sum[c] += share[i][c];
		}
		waves[i].w = ( float )pow( lam / 560.0, 4.0 * spectral );
	}
	for( int i = 0; i < BLOOM_FFT_WAVES; i++ )
	{
		waves[i].x = ( float )( share[i][0] / sum[0] );
		waves[i].y = ( float )( share[i][1] / sum[1] );
		waves[i].z = ( float )( share[i][2] / sum[2] );
	}
}

/*
========================
BloomFftArmsNorm

The integral over the plane of the spikes' arms as the shader draws them ( BloomFftSpikes ), in view heights
squared. Across an arm the profile exp( -( a / w )^2 ) / w integrates to sqrt( pi ) whatever its width ( the blur and
the spread move no light ); along it, in both directions, the fall 1 / ( 1 + t / 0.004 ) times the length's
exp( -( t / L )^2 ) times the ringing, summed here by the midpoint rule
========================
*/
static float BloomFftArmsNormSum( int pairs, float armLength, float ringing, float falloff );

static float BloomFftArmsNorm( int pairs, float armLength, float ringing, float falloff )
{
	// the sum is asked for in every bloom view to build the kernel's key, so the answers are kept ( review R1-10; the
	// backend's thread alone calls it ). WP-J27: one a kernel a view can hold, and a spare, replaced in turn
	static const int CACHED = BLOOM_FFT_CLASSES * 2;
	static int cachedPairs[CACHED] = { -1, -1, -1, -1, -1, -1, -1, -1 };
	static float cachedLength[CACHED], cachedRinging[CACHED], cachedFalloff[CACHED], cachedNorm[CACHED];
	static int next = 0;
	for( int i = 0; i < CACHED; i++ )
	{
		if( pairs == cachedPairs[i] && armLength == cachedLength[i] && ringing == cachedRinging[i] && falloff == cachedFalloff[i] )
		{
			return cachedNorm[i];
		}
	}
	const int i = next;
	next = ( next + 1 ) % CACHED;
	cachedPairs[i] = pairs;
	cachedLength[i] = armLength;
	cachedRinging[i] = ringing;
	cachedFalloff[i] = falloff;
	cachedNorm[i] = BloomFftArmsNormSum( pairs, armLength, ringing, falloff );
	return cachedNorm[i];
}

static float BloomFftArmsNormSum( int pairs, float armLength, float ringing, float falloff )
{
	const double L = Max( ( double )armLength, 1e-6 );
	const double c = ( double )falloff;
	const double P = 0.010 + 0.08 * L;
	const int steps = 8000;
	const double dt = 4.0 * L / steps;
	double along = 0.0;
	for( int i = 0; i < steps; i++ )
	{
		const double t = ( i + 0.5 ) * dt;
		const double ring = sin( idMath::PI * t / P );
		along += exp( -( t * t ) / ( L * L ) ) / ( 1.0 + t / c ) * ( 1.0 - ringing * ring * ring );
	}
	return ( float )( pairs * sqrt( idMath::PI ) * 2.0 * along * dt );
}

/*
========================
BloomFftWideNorm

The integral over the plane of the inverse-square glow's wide term ( BloomFftGlow ): 2 pi r ( 1 - ( r / R )^2 )^2 /
( r^2 + r0^2 ) out to its reach R, by the midpoint rule fine enough for r0 = 0.002
========================
*/
static float BloomFftWideNormSum( float glowReach );

static float BloomFftWideNorm( float glowReach )
{
	// kept as BloomFftArmsNorm's ( review R1-10, WP-J27 )
	static const int CACHED = BLOOM_FFT_CLASSES * 2;
	static float cachedReach[CACHED] = { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };
	static float cachedNorm[CACHED];
	static int next = 0;
	for( int i = 0; i < CACHED; i++ )
	{
		if( glowReach == cachedReach[i] )
		{
			return cachedNorm[i];
		}
	}
	const int i = next;
	next = ( next + 1 ) % CACHED;
	cachedReach[i] = glowReach;
	cachedNorm[i] = BloomFftWideNormSum( glowReach );
	return cachedNorm[i];
}

static float BloomFftWideNormSum( float glowReach )
{
	const double R = Max( ( double )glowReach, 1e-6 );
	const double r0 = 0.002;
	const int steps = 40000;
	const double dr = R / steps;
	double sum = 0.0;
	for( int i = 0; i < steps; i++ )
	{
		const double r = ( i + 0.5 ) * dr;
		const double t = 1.0 - ( r / R ) * ( r / R );
		sum += 2.0 * idMath::PI * r * t * t / ( r * r + r0 * r0 );
	}
	return ( float )( sum * dr );
}

// true where two constant sets draw the same kernel: the field, the image's cells, the kernel's parameters, the terms'
// integrals and the wavelengths. The source, the gain and the whitepoint are not the kernel's
static bool BloomFftKernelKeyEquals( const BloomFftConstants& a, const BloomFftConstants& b )
{
	return memcmp( a.field, b.field, sizeof( a.field ) ) == 0 && a.image[0] == b.image[0] && a.image[1] == b.image[1]
		   && a.kernelA == b.kernelA && a.kernelB == b.kernelB && a.kernelC == b.kernelC && a.norms == b.norms
		   && a.shape.w == b.shape.w
		   && memcmp( a.waves, b.waves, sizeof( a.waves ) ) == 0;
}

/*
========================
BloomKernelValues

WP-J27: one kernel's values, from the System Options rows ( the view's kernel ) or from a source's style ( a light's
bloom_* keys ); BloomFftSetKernel makes from them the constants PASS 0 reads for this view's field. Every value is held to
its row's range, and a NaN takes the row's default
========================
*/
struct BloomKernelValues
{
	int		shape;			// r_bloomFFTShape's numbers: 0 the spikes, 1 the inverse-square glow
	int		arms;
	float	rotation;
	float	radius;
	float	blur;
	float	spread;
	float	ratio;
	float	ringing;
	float	shift;
	float	falloffShare;
	float	glowRadius;
	float	glare;
};

static void BloomKernelValuesFromCvars( BloomKernelValues& v )
{
	v.shape = idMath::ClampInt( 0, 1, r_bloomFFTShape.GetInteger() );
	v.arms = idMath::ClampInt( 1, 7, r_bloomSpikes.GetInteger() );
	v.rotation = BloomCvarFloat( r_bloomSpikeRotation, 0.0f, 1.0f, 0.5f );
	v.radius = BloomCvarFloat( r_bloomSpikeRadius, 0.0f, 1.0f, 0.0f );
	v.blur = BloomCvarFloat( r_bloomSpikeBlur, 0.0f, 1.0f, 0.0f );
	v.spread = BloomCvarFloat( r_bloomSpikeSpread, 0.0f, 1.0f, 0.0f );
	v.ratio = BloomCvarFloat( r_bloomSpikeRatio, -1.0f, 1.0f, 0.0f );
	v.ringing = BloomCvarFloat( r_bloomSpikeRinging, 0.0f, 1.0f, 0.75f );
	v.shift = BloomCvarFloat( r_bloomSpikeShift, 0.0f, 1.0f, 0.1f );
	v.falloffShare = BloomCvarFloat( r_bloomSpikeFalloff, 0.0f, 1.0f, 0.0f );
	v.glowRadius = BloomCvarFloat( r_bloomGlowRadius, 0.0f, 1.0f, 1.0f );
	v.glare = BloomCvarFloat( r_bloomGlare, 0.0f, 1.0f, 0.0f );
}

// a style's value held to its range; a NaN ( every comparison false ) takes the fallback
static float BloomStyleValue( float value, float low, float high, float fallback )
{
	if( value >= low && value <= high )
	{
		return value;
	}
	return ( value > high ) ? high : ( value < low ) ? low : fallback;
}

static void BloomKernelValuesFromStyle( const renderBloomStyle_t& style, BloomKernelValues& v )
{
	v.shape = ( style.shape == BLOOM_STYLE_GLOW ) ? 1 : 0;
	v.arms = idMath::ClampInt( 1, 7, style.spikes );
	v.rotation = BloomStyleValue( style.spikeRotation, 0.0f, 1.0f, 0.5f );
	v.radius = BloomStyleValue( style.spikeRadius, 0.0f, 1.0f, 0.0f );
	v.blur = BloomStyleValue( style.spikeBlur, 0.0f, 1.0f, 0.0f );
	v.spread = BloomStyleValue( style.spikeSpread, 0.0f, 1.0f, 0.0f );
	v.ratio = BloomStyleValue( style.spikeRatio, -1.0f, 1.0f, 0.0f );
	v.ringing = BloomStyleValue( style.spikeRinging, 0.0f, 1.0f, 0.75f );
	v.shift = BloomStyleValue( style.spikeShift, 0.0f, 1.0f, 0.1f );
	v.falloffShare = BloomStyleValue( style.spikeFalloff, 0.0f, 1.0f, 0.0f );
	v.glowRadius = BloomStyleValue( style.glowRadius, 0.0f, 1.0f, 1.0f );
	v.glare = BloomStyleValue( style.glare, 0.0f, 1.0f, 0.0f );
}

/*
========================
BloomFftSetKernel

The kernel's constants ( shape.w, kernelA to kernelC, the norms, the waves ) for values v in a view whose reach is m view
heights and whose field has cellsPerViewHeight cells a view height. Only the chosen shape's values enter, so a change in the
other shape's values does not make the kernel again. The formulas are section 07's ( WP-J24 )
========================
*/
static void BloomFftSetKernel( BloomFftConstants& c, const BloomKernelValues& v, float m, float cellsPerViewHeight )
{
	if( v.shape == 0 )
	{
		const float L = m * ( 0.12f + 0.88f * v.radius * v.radius );
		const float falloff = v.falloffShare > 0.0f ? v.falloffShare * L : 0.004f;
		c.shape.w = falloff;		// the arms' fall, read by PASS 0 only
		c.kernelA.Set( cellsPerViewHeight, 0.0f, ( float )v.arms, v.rotation );
		c.kernelB.Set( L, 0.0012f + 0.012f * v.blur, v.spread, v.ratio );
		c.kernelC.Set( v.ringing, 0.0f, 0.0f, ( float )BLOOM_FFT_SUBSAMPLES );
		c.norms.Set( 2.0f * idMath::PI, BloomFftArmsNorm( v.arms, L, v.ringing, falloff ), 1.0f, 1.0f );
		BloomFftWaves( v.shift, c.waves );
	}
	else
	{
		const float Rg = m * ( 0.15f + 0.85f * v.glowRadius );
		c.shape.w = 0.0f;
		c.kernelA.Set( cellsPerViewHeight, 1.0f, 0.0f, 0.0f );
		c.kernelB.Zero();
		c.kernelC.Set( 0.0f, Rg, v.glare, ( float )BLOOM_FFT_SUBSAMPLES );
		c.norms.Set( 1.0f, 1.0f, BloomFftWideNorm( Rg ), idMath::PI * 0.004f * 0.004f );
		for( int i = 0; i < BLOOM_FFT_WAVES; i++ )
		{
			c.waves[i].Zero();
		}
	}
}

CONSOLE_COMMAND( bloomFftDump, "WP-J24's gates: reads back the next convolution's source level, kernel and result and writes bloomfft_*.raw and bloomfft_dump.txt to the save path", NULL )
{
	bloomFftDumpRequested = true;
}

CONSOLE_COMMAND( bloomProbe, "prints the next blooming view's chain: its levels' rectangles and the mean of each level after the down passes, and the mixed term", NULL )
{
	bloomProbeRequested = true;
	if( !r_bloom.GetBool() )
	{
		common->Printf( "bloomProbe: r_bloom is 0, the probe waits for the first bloom view\n" );
	}
}

// The level count of a view: r_bloomLevels plus one for each doubling of the view's height, clamped to the chain,
// then lowered while the smallest level is under 8 texels on a side ( unless level 0 itself
// is ). Fills the level rectangles for the count that comes out: level i's rectangle has the view's size halved
// i + 1 times, halves rounded up
static int BloomLevelCount( int viewWidth, int viewHeight, int* levelW, int* levelH )
{
	int n = r_bloomLevels.GetInteger() + ( int )floor( log2( ( double )viewHeight / 1080.0 ) + 0.5 );
	n = idMath::ClampInt( 1, BLOOM_MAX_LEVELS, n );
	levelW[0] = ( viewWidth + 1 ) / 2;
	levelH[0] = ( viewHeight + 1 ) / 2;
	for( int i = 1; i < n; i++ )
	{
		levelW[i] = ( levelW[i - 1] + 1 ) / 2;
		levelH[i] = ( levelH[i - 1] + 1 ) / 2;
	}
	while( n > 1 && Min( levelW[n - 1], levelH[n - 1] ) < 8 )
	{
		n--;
	}
	return n;
}

// One field of an R11G11B10F texel: exponent e, mantissa m of mantissaBits bits, no sign. A subnormal is
// m * 2^-14 / 2^M, e = 31 is a NaN ( m not 0 ) or an infinity, else the value is ( 1 + m / 2^M ) * 2^( e - 15 )
static double BloomDecode11( uint field, int mantissaBits )
{
	const uint e = ( field >> mantissaBits ) & 31u;
	const uint m = field & ( ( 1u << mantissaBits ) - 1u );
	if( e == 0 )
	{
		return ldexp( ( double )m, -14 - mantissaBits );
	}
	if( e == 31 )
	{
		return ( m != 0 ) ? std::numeric_limits<double>::quiet_NaN() : std::numeric_limits<double>::infinity();
	}
	return ldexp( 1.0 + ( double )m / ( double )( 1u << mantissaBits ), ( int )e - 15 );
}

// The mean of a rectangle's texels, decoded by the chain's format, in double. The rectangle starts at
// ( x0, y0 ) of the mapped data: the levels' stagings are read at ( 0, 0 ), the frame's staging holds the whole
// frame and is read at the view rectangle's origin
static void BloomProbeMean( const byte* data, size_t rowPitch, int x0, int y0, int width, int height, bool r11g11b10, double mean[3] )
{
	double sum[3] = { 0.0, 0.0, 0.0 };
	for( int y = 0; y < height; y++ )
	{
		const byte* row = data + ( y0 + y ) * rowPitch;
		for( int x = 0; x < width; x++ )
		{
			if( r11g11b10 )
			{
				// red and green are 11 bits ( 5 exponent, 6 mantissa ), blue 10 ( 5 and 5 ), no sign, red in the
				// low bits: bits = r | ( g << 11 ) | ( b << 22 )
				const uint bits = *( const uint* )( row + ( x0 + x ) * 4 );
				sum[0] += BloomDecode11( bits & 0x7FFu, 6 );
				sum[1] += BloomDecode11( ( bits >> 11 ) & 0x7FFu, 6 );
				sum[2] += BloomDecode11( ( bits >> 22 ) & 0x3FFu, 5 );
			}
			else
			{
				const halfFloat_t* texel = ( const halfFloat_t* )( row + ( x0 + x ) * 8 );
				sum[0] += F16toF32( texel[0] );
				sum[1] += F16toF32( texel[1] );
				sum[2] += F16toF32( texel[2] );
			}
		}
	}
	const double inv = 1.0 / ( ( double )width * ( double )height );
	mean[0] = sum[0] * inv;
	mean[1] = sum[1] * inv;
	mean[2] = sum[2] * inv;
}

bool BloomPass::WantedForView( const viewDef_t* viewDef )
{
	return r_bloom.GetBool() && viewDef->isMainView && r_renderMode.GetInteger() == RENDERMODE_DOOM;
}

// WP-J21: the stage loops of such a view write the glow mask; the bloom then reads it under r_bloomSource 1
bool BloomPass::GateWantedForView( const viewDef_t* viewDef )
{
	return WantedForView( viewDef ) && r_bloomSource.GetInteger() >= 1;
}

// WP-J21: the froxel fog of such a view runs its glow forms ( VolumetricFogPass::Render ): only where the bloom can use
// them, so not without _hdrGlow ( MSAA, no independent blend: the open gate ) nor at Strength 0 ( review R1-8 )
bool BloomPass::FogGlowWantedForView( const viewDef_t* viewDef )
{
	return WantedForView( viewDef ) && r_bloomSource.GetInteger() == 1 && r_bloomFogGlow.GetBool()
		   && globalFramebuffers.hdrGlowFBO != NULL && r_bloomGlow.GetFloat() > 0.0f;
}

BloomPass::BloomPass( nvrhi::IDevice* device, CommonRenderPasses* commonPasses )
	: m_Device( device )
	, commonRenderPasses( commonPasses )
	, m_Dead( false )
	, m_ChainWidth( 0 )
	, m_ChainHeight( 0 )
	, m_ChainFresh( false )
	, m_FirstSource( NULL )
	, m_FirstExposure( NULL )
	, m_FirstDepthSource( NULL )
	, m_FirstDepthExposure( NULL )
	, m_FirstDepthImage( NULL )
	, m_PokeWarned( false )
	, m_ProbeWait( 0 )
	, m_ProbeFft( 0 )
	, m_ProbeFftLevel( 0 )
	, m_ProbeFftRebuilt( false )
	, m_ProbeFftQuality( 0 )
	, m_ProbeClasses( 1 )
	, m_ProbeSlots( 0 )
	, m_ProbeOverflow( 0 )
	, m_ProbeKernelsBuilt( 0 )
	, m_ProbeHotBoost( 0.0f )
	, m_ProbeHotLevel( 0.0f )
	, m_ProbeSource( 0 )
	, m_ProbeMaskWritten( 0 )
	, m_ProbeBias( 1.0f )
	, m_GateTried( false )
	, m_GateDead( false )
	, m_GateFrame( NULL )
	, m_GateMask( NULL )
	, m_GateExposure( NULL )
	, m_GatedExposure( NULL )
	, m_GatedDepthExposure( NULL )
	, m_GatedDepthImage( NULL )
	, m_ProbeFogGlow( 0 )
	, m_FftTried( false )
	, m_FftDead( false )
	, m_FftW( 0 )
	, m_FftH( 0 )
	, m_FftSlices( 1 )
	, m_FftSplitRefused( false )
	, m_FftSet1Source( NULL )
	, m_FftFresh( false )
	, m_FftSerial( 0 )
	, m_DumpWait( 0 )
	, m_DumpLevelIndex( 0 )
{
	memset( &m_DumpConstants, 0, sizeof( m_DumpConstants ) );
	memset( m_FogSetKey, 0, sizeof( m_FogSetKey ) );
	m_DumpLevelRect[0] = m_DumpLevelRect[1] = 0;
	memset( &m_ProbeFftConstants, 0, sizeof( m_ProbeFftConstants ) );
	memset( m_FftKernelKey, 0, sizeof( m_FftKernelKey ) );
	memset( m_FftKernelValid, 0, sizeof( m_FftKernelValid ) );
	memset( m_FftKernelUsed, 0, sizeof( m_FftKernelUsed ) );
	// The chain's format, chosen once: R11G11B10F where the device can draw into it, blend into it and filter it
	// linearly ( Vulkan does not promise the first two for this format ), else RGBA16_FLOAT
	m_ChainFormat = nvrhi::Format::RGBA16_FLOAT;
#if !BLOOM_CHAIN_RGBA16F
	const nvrhi::FormatSupport required = nvrhi::FormatSupport::Texture | nvrhi::FormatSupport::RenderTarget
										  | nvrhi::FormatSupport::Blendable | nvrhi::FormatSupport::ShaderSample;
	if( ( device->queryFormatSupport( nvrhi::Format::R11G11B10_FLOAT ) & required ) == required )
	{
		m_ChainFormat = nvrhi::Format::R11G11B10_FLOAT;
	}
#endif

	// The shaders are found here, at the pass's first use ( the fog's way ): a run with the bloom off never loads
	// them. The three permutations of bloom_down are three entries: FindShader knows an entry by name, stage and
	// suffix, and the permutation inside the blob is picked by the macro list, whose names, values and order must
	// equal the cfg line's
	const idList<shaderMacro_t> downMacros = { { "FIRST", "0" } };
	const idList<shaderMacro_t> firstMacros = { { "FIRST", "1" } };
	const idList<shaderMacro_t> firstDepthMacros = { { "FIRST", "2" } };
	const idList<shaderMacro_t> noMacros;
	m_DownPS = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/post/bloom_down", SHADER_STAGE_FRAGMENT, "", downMacros, true, LAYOUT_DRAW_VERT ) );
	m_DownFirstPS = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/post/bloom_down", SHADER_STAGE_FRAGMENT, "_first", firstMacros, true, LAYOUT_DRAW_VERT ) );
	m_DownFirstDepthPS = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/post/bloom_down", SHADER_STAGE_FRAGMENT, "_depth", firstDepthMacros, true, LAYOUT_DRAW_VERT ) );
	m_UpPS = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/post/bloom_up", SHADER_STAGE_FRAGMENT, "", noMacros, true, LAYOUT_DRAW_VERT ) );
	m_VS = renderProgManager.GetProgramInfo( BUILTIN_TONEMAPPING ).vs;		// the full-screen quad: four vertices, no inputs
	if( !m_VS || !m_DownFirstPS || !m_DownPS || !m_UpPS )
	{
		// A missing blob file stops the engine in GetBytecode, and a missing permutation reaches the device's
		// message callback at Error severity, which is a common->FatalError too: as for every builtin shader the
		// engine does not get this far. The test only guards a null handle from any other cause
		m_Dead = true;
		common->Warning( "BloomPass: the bloom shaders are missing, no bloom" );
	}

	// the first down pass reads the frame and the exposure buffer; the other draws read one level alone
	nvrhi::BindingLayoutDesc firstLayoutDesc;
	firstLayoutDesc.visibility = nvrhi::ShaderType::Pixel;
	firstLayoutDesc.bindings =
	{
		nvrhi::BindingLayoutItem::PushConstants( 0, sizeof( BloomConstants ) ),
		nvrhi::BindingLayoutItem::Texture_SRV( 0 ),
		nvrhi::BindingLayoutItem::TypedBuffer_SRV( 1 ),
		nvrhi::BindingLayoutItem::Sampler( 0 )
	};
	m_FirstLayout = device->createBindingLayout( firstLayoutDesc );

	// the first pass that reads the frame's depth too ( the distance tail, the view weapon ): the same and the depth
	nvrhi::BindingLayoutDesc firstDepthLayoutDesc;
	firstDepthLayoutDesc.visibility = nvrhi::ShaderType::Pixel;
	firstDepthLayoutDesc.bindings =
	{
		nvrhi::BindingLayoutItem::PushConstants( 0, sizeof( BloomConstants ) ),
		nvrhi::BindingLayoutItem::Texture_SRV( 0 ),
		nvrhi::BindingLayoutItem::TypedBuffer_SRV( 1 ),
		nvrhi::BindingLayoutItem::Texture_SRV( 2 ),
		nvrhi::BindingLayoutItem::Sampler( 0 )
	};
	m_FirstDepthLayout = device->createBindingLayout( firstDepthLayoutDesc );

	nvrhi::BindingLayoutDesc downUpLayoutDesc;
	downUpLayoutDesc.visibility = nvrhi::ShaderType::Pixel;
	downUpLayoutDesc.bindings =
	{
		nvrhi::BindingLayoutItem::PushConstants( 0, sizeof( BloomConstants ) ),
		nvrhi::BindingLayoutItem::Texture_SRV( 0 ),
		nvrhi::BindingLayoutItem::Sampler( 0 )
	};
	m_DownUpLayout = device->createBindingLayout( downUpLayoutDesc );
}

BloomPass::~BloomPass()
{
	ReleaseChain();
	FftReleaseField();
}

/*
========================
BloomPass::ReleaseChain

Every handle of the chain goes: the levels, their framebuffers, the pipelines, the binding sets and the probe's
staging textures ( a probe under way was of the old chain ). The handles are refcounted: setting one to nullptr
releases it. The poke textures, the shaders and the layouts are not the chain's: they live as long as the pass.
========================
*/
void BloomPass::ReleaseChain()
{
	if( m_ProbeWait > 0 )
	{
		common->Printf( "bloomProbe: the chain was released, the probe is dropped\n" );
	}
	for( int i = 0; i < BLOOM_MAX_LEVELS; i++ )
	{
		m_Levels[i] = nullptr;
		m_Frames[i] = nullptr;
		m_DownSets[i] = nullptr;
		m_UpSets[i] = nullptr;
		m_ProbeStagings[i] = nullptr;
	}
	m_DownFirstPipeline = nullptr;
	m_DownPipeline = nullptr;
	m_UpPipeline = nullptr;
	m_FirstSet = nullptr;
	m_FirstSource = NULL;
	m_FirstExposure = NULL;
	m_Wide = nullptr;
	m_FirstFrame = nullptr;
	m_DownFirstDepthPipeline = nullptr;
	m_DownWideSet = nullptr;
	m_FirstDepthSet = nullptr;
	m_FirstDepthSource = NULL;
	m_FirstDepthExposure = NULL;
	m_FirstDepthImage = NULL;
	m_ProbeUpStaging = nullptr;
	m_ProbeFrameStaging = nullptr;
	m_ProbeWait = 0;
	// WP-J24: what of the convolution names the chain's textures or draws into them ( its field stays )
	m_FftReentryPipeline = nullptr;
	m_FftSets[1] = nullptr;
	m_FftSet1Source = NULL;
	// WP-J21: the gate's textures are the frame's size and go with the chain, and so does what names them
	m_Gated = nullptr;
	m_GateFlags = nullptr;
	m_DownGatedPipeline = nullptr;
	m_DownGatedDepthPipeline = nullptr;
	m_GateSet = nullptr;
	m_GateFrame = NULL;
	m_GateMask = NULL;
	m_GateExposure = NULL;
	m_GatedSet = nullptr;
	m_GatedExposure = NULL;
	m_GatedDepthSet = nullptr;
	m_GatedDepthExposure = NULL;
	m_GatedDepthImage = NULL;
	m_DownFogPipeline = nullptr;
	m_FogSet = nullptr;
	memset( m_FogSetKey, 0, sizeof( m_FogSetKey ) );
	// WP-J27: the styles' sums and shares are the frame's size too ( the rows' set, released above, names the shares )
	m_StyleSums = nullptr;
	m_StyleShares = nullptr;
	m_ShareSet = nullptr;
}

/*
========================
BloomPass::CheckChain

Makes the chain for a frame texture size: the level textures and their framebuffers, the three pipelines ( made
against level 0's framebuffer: every level has the same format, so each serves every level ) and the binding sets
of the down and up passes. The old chain goes first and the chain counter steps. Returns false when the device
could not make a part of the chain: the size is remembered, so it is not tried again in every frame.
========================
*/
bool BloomPass::CheckChain( int frameWidth, int frameHeight )
{
	if( frameWidth == m_ChainWidth && frameHeight == m_ChainHeight )
	{
		return m_Levels[0] != nullptr;		// this size's chain exists, or the device could not make it
	}
	ReleaseChain();
	bloomChainSerial++;
	m_ChainWidth = frameWidth;
	m_ChainHeight = frameHeight;

	// one texture per level, level i of the size the frame's halved i + 1 times with halves rounded up, so a view
	// rectangle of any size up to the frame's fits its level. A rounded-up half is never 0, so all nine levels
	// exist for any frame ( a 1 x 1 frame has nine 1 x 1 levels )
	nvrhi::TextureDesc desc;
	desc.dimension = nvrhi::TextureDimension::Texture2D;
	desc.mipLevels = 1;
	desc.format = m_ChainFormat;
	desc.isRenderTarget = true;
	desc.initialState = nvrhi::ResourceStates::ShaderResource;
	desc.keepInitialState = true;

	int width = ( frameWidth + 1 ) / 2;
	int height = ( frameHeight + 1 ) / 2;
	for( int i = 0; i < BLOOM_MAX_LEVELS; i++ )
	{
		desc.width = width;
		desc.height = height;
		desc.debugName = va( "Bloom/Level%d", i );
		m_Levels[i] = m_Device->createTexture( desc );
		m_TexW[i] = width;
		m_TexH[i] = height;
		width = ( width + 1 ) / 2;
		height = ( height + 1 ) / 2;
	}
	for( int i = 0; i < BLOOM_MAX_LEVELS; i++ )
	{
		if( !m_Levels[i] )
		{
			// the device could not make a level ( out of memory ): no bloom. The size is kept, so that it is not
			// tried again in every view
			ReleaseChain();
			common->Warning( "BloomPass: the device could not make the bloom chain of a %d x %d frame, no bloom until the frame size changes", frameWidth, frameHeight );
			return false;
		}
	}

	for( int i = 0; i < BLOOM_MAX_LEVELS; i++ )
	{
		m_Frames[i] = m_Device->createFramebuffer( nvrhi::FramebufferDesc().addColorAttachment( m_Levels[i] ) );
		if( !m_Frames[i] )
		{
			// tested here, before the pipelines are made against level 0's framebuffer. The size is kept, so that
			// the device is not asked again in every view
			ReleaseChain();
			common->Warning( "BloomPass: the device could not make the bloom chain's framebuffers of a %d x %d frame, no bloom until the frame size changes", frameWidth, frameHeight );
			return false;
		}
	}

	nvrhi::GraphicsPipelineDesc psoDesc;
	psoDesc.VS = m_VS;
	psoDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
	psoDesc.renderState.rasterState.setCullNone();
	psoDesc.renderState.depthStencilState.depthTestEnable = false;
	psoDesc.renderState.depthStencilState.depthWriteEnable = false;
	psoDesc.renderState.depthStencilState.stencilEnable = false;

	psoDesc.bindingLayouts = { m_FirstLayout };

	// The first pass writes two targets, level 0 and the wide levels' source ( a texture of level 0's size ), which
	// level 1's reduction reads. The texture, the framebuffer of the two, the first pass's pipeline and level 1's set
	// are part of the chain ( tested with the pipelines below ). The permutation that reads the frame's depth has a
	// pipeline of its own for the same framebuffer; where it cannot be made the tail is the same at every distance
	// and the view weapon glows. A shader blob built from an older shader list has other permutations and stops the
	// engine where the pass is made, as a missing permutation of any builtin shader does; "cannot be made" here means
	// the device refusing the pipeline
	desc.width = m_TexW[0];
	desc.height = m_TexH[0];
	desc.debugName = "Bloom/Wide";
	m_Wide = m_Device->createTexture( desc );
	if( m_Wide )
	{
		m_FirstFrame = m_Device->createFramebuffer( nvrhi::FramebufferDesc().addColorAttachment( m_Levels[0] ).addColorAttachment( m_Wide ) );
	}
	if( m_FirstFrame )
	{
		psoDesc.PS = m_DownFirstPS;		// m_FirstLayout: the frame, the exposure buffer, a sampler
		m_DownFirstPipeline = m_Device->createGraphicsPipeline( psoDesc, m_FirstFrame );
		if( m_DownFirstDepthPS && m_FirstDepthLayout )
		{
			psoDesc.bindingLayouts = { m_FirstDepthLayout };
			psoDesc.PS = m_DownFirstDepthPS;
			m_DownFirstDepthPipeline = m_Device->createGraphicsPipeline( psoDesc, m_FirstFrame );
			if( !m_DownFirstDepthPipeline )
			{
				common->Warning( "BloomPass: the device could not make the first pass's depth pipeline of a %d x %d frame: the tail is the same at every distance and the view weapon glows", frameWidth, frameHeight );
			}
		}
		nvrhi::BindingSetDesc wideSetDesc;
		wideSetDesc.bindings =
		{
			nvrhi::BindingSetItem::PushConstants( 0, sizeof( BloomConstants ) ),
			nvrhi::BindingSetItem::Texture_SRV( 0, m_Wide ),
			nvrhi::BindingSetItem::Sampler( 0, commonRenderPasses->m_LinearClampSampler )
		};
		m_DownWideSet = m_Device->createBindingSet( wideSetDesc, m_DownUpLayout );
	}

	psoDesc.bindingLayouts = { m_DownUpLayout };
	psoDesc.PS = m_DownPS;
	m_DownPipeline = m_Device->createGraphicsPipeline( psoDesc, m_Frames[0] );

	psoDesc.PS = m_UpPS;
	// the up passes add a level into the one below: source * ONE + destination * ONE, colour and alpha. The chain's
	// alpha is never read, so what accumulates there is of no consequence
	psoDesc.renderState.blendState.targets[0].blendEnable = true;
	psoDesc.renderState.blendState.targets[0].srcBlend = nvrhi::BlendFactor::One;
	psoDesc.renderState.blendState.targets[0].destBlend = nvrhi::BlendFactor::One;
	psoDesc.renderState.blendState.targets[0].srcBlendAlpha = nvrhi::BlendFactor::One;
	psoDesc.renderState.blendState.targets[0].destBlendAlpha = nvrhi::BlendFactor::One;
	m_UpPipeline = m_Device->createGraphicsPipeline( psoDesc, m_Frames[0] );

	// the pass owns its sets ( not the backend's binding cache, whose sets would keep a dead chain's textures
	// alive ): the down pass into level i reads level i - 1 ( level 1 reads m_Wide: m_DownWideSet ), the up pass into
	// level i reads level i + 1
	for( int i = 2; i < BLOOM_MAX_LEVELS; i++ )
	{
		nvrhi::BindingSetDesc setDesc;
		setDesc.bindings =
		{
			nvrhi::BindingSetItem::PushConstants( 0, sizeof( BloomConstants ) ),
			nvrhi::BindingSetItem::Texture_SRV( 0, m_Levels[i - 1] ),
			nvrhi::BindingSetItem::Sampler( 0, commonRenderPasses->m_LinearClampSampler )
		};
		m_DownSets[i] = m_Device->createBindingSet( setDesc, m_DownUpLayout );
	}
	for( int i = 0; i + 1 < BLOOM_MAX_LEVELS; i++ )
	{
		nvrhi::BindingSetDesc setDesc;
		setDesc.bindings =
		{
			nvrhi::BindingSetItem::PushConstants( 0, sizeof( BloomConstants ) ),
			nvrhi::BindingSetItem::Texture_SRV( 0, m_Levels[i + 1] ),
			nvrhi::BindingSetItem::Sampler( 0, commonRenderPasses->m_LinearClampSampler )
		};
		m_UpSets[i] = m_Device->createBindingSet( setDesc, m_DownUpLayout );
	}

	// the pipelines and the down and up sets must all stand too: a null one would be used by the next draw. As for
	// the textures, the remembered size keeps the device from being asked again in every view
	bool missing = !m_DownFirstPipeline || !m_DownWideSet || !m_DownPipeline || !m_UpPipeline;
	for( int i = 2; i < BLOOM_MAX_LEVELS; i++ )
	{
		missing = missing || !m_DownSets[i];
	}
	for( int i = 0; i + 1 < BLOOM_MAX_LEVELS; i++ )
	{
		missing = missing || !m_UpSets[i];
	}
	if( missing )
	{
		ReleaseChain();
		common->Warning( "BloomPass: the device could not make the bloom chain's pipelines or the first pass's second target of a %d x %d frame, no bloom until the frame size changes", frameWidth, frameHeight );
		return false;
	}
	m_ChainFresh = true;		// Render clears every level once before the first draw touches it
	return true;
}

/*
========================
BloomPass::CheckFirstSet

The first down pass's binding set. The frame texture and the tonemap pass's exposure buffer belong to the engine
and can be made again under a living pass, so the set is made again when either is another object than the set was
made for. While the set lives it holds both, so neither address can be another object's.
========================
*/
bool BloomPass::CheckFirstSet( nvrhi::ITexture* sourceTexture, nvrhi::IBuffer* exposureBuffer )
{
	if( m_FirstSet && m_FirstSource == sourceTexture && m_FirstExposure == exposureBuffer )
	{
		return true;
	}
	nvrhi::BindingSetDesc setDesc;
	setDesc.bindings =
	{
		nvrhi::BindingSetItem::PushConstants( 0, sizeof( BloomConstants ) ),
		nvrhi::BindingSetItem::Texture_SRV( 0, sourceTexture ),
		nvrhi::BindingSetItem::TypedBuffer_SRV( 1, exposureBuffer ),
		nvrhi::BindingSetItem::Sampler( 0, commonRenderPasses->m_LinearClampSampler )
	};
	m_FirstSet = m_Device->createBindingSet( setDesc, m_FirstLayout );
	if( m_FirstSet == nullptr )
	{
		// the set did not stand: record nothing, so neither pointer can later name another object's
		m_FirstSource = NULL;
		m_FirstExposure = NULL;
		return false;
	}
	m_FirstSource = sourceTexture;
	m_FirstExposure = exposureBuffer;
	return true;
}

/*
========================
BloomPass::CheckFirstDepthSet

The binding set of the first pass that reads the frame's depth ( the distance tail, the view weapon ): CheckFirstSet's
rule with a third object, the
frame's depth ( the Hi-Z image, which the engine can make again too ). The caller has m_DownFirstDepthPipeline,
which is made only where m_FirstDepthLayout stands, so the layout is not tested here
========================
*/
bool BloomPass::CheckFirstDepthSet( nvrhi::ITexture* sourceTexture, nvrhi::IBuffer* exposureBuffer, nvrhi::ITexture* depthTexture )
{
	if( m_FirstDepthSet && m_FirstDepthSource == sourceTexture && m_FirstDepthExposure == exposureBuffer && m_FirstDepthImage == depthTexture )
	{
		return true;
	}
	nvrhi::BindingSetDesc setDesc;
	setDesc.bindings =
	{
		nvrhi::BindingSetItem::PushConstants( 0, sizeof( BloomConstants ) ),
		nvrhi::BindingSetItem::Texture_SRV( 0, sourceTexture ),
		nvrhi::BindingSetItem::TypedBuffer_SRV( 1, exposureBuffer ),
		nvrhi::BindingSetItem::Texture_SRV( 2, depthTexture ),
		nvrhi::BindingSetItem::Sampler( 0, commonRenderPasses->m_LinearClampSampler )
	};
	m_FirstDepthSet = m_Device->createBindingSet( setDesc, m_FirstDepthLayout );
	if( m_FirstDepthSet == nullptr )
	{
		m_FirstDepthSource = NULL;
		m_FirstDepthExposure = NULL;
		m_FirstDepthImage = NULL;
		return false;
	}
	m_FirstDepthSource = sourceTexture;
	m_FirstDepthExposure = exposureBuffer;
	m_FirstDepthImage = depthTexture;
	return true;
}

/*
========================
BloomPass::DrawLevel

One full-screen draw into a level: the tonemapping program's vertex shader and a bloom pixel shader, the constants
as push constants after the graphics state, as CommonRenderPasses::BlitTexture does on every API ( up to 17 draws
a view would outrun a volatile buffer's versions ).
========================
*/
void BloomPass::DrawLevel( nvrhi::ICommandList* commandList, nvrhi::IGraphicsPipeline* pipeline, nvrhi::IBindingSet* bindingSet,
						   nvrhi::IFramebuffer* framebuffer, int width, int height, const BloomConstants& constants )
{
	const nvrhi::Viewport viewport( 0.0f, ( float )width, 0.0f, ( float )height, 0.0f, 1.0f );
	nvrhi::GraphicsState state;
	state.indexBuffer = {};					// the fork leaves this one member uninitialized
	state.pipeline = pipeline;
	state.framebuffer = framebuffer;
	state.bindings = { bindingSet };
	state.viewport.addViewportAndScissorRect( viewport );
	commandList->setGraphicsState( state );
	commandList->setPushConstants( &constants, sizeof( constants ) );
	nvrhi::DrawArguments args;
	args.instanceCount = 1;
	args.vertexCount = 4;
	commandList->draw( args );
	// a direct NVRHI draw goes past DrawElementsWithCounters
	if( backEnd.concurrencyStats )
	{
		backEnd.pc.passDraws[backEnd.currentPass]++;
	}
}

/*
========================
BloomPass::PokeFrame

r_bloomDebug 11, 12, 13: one 1 x 1 texture of the frame's own format copied into the frame at the view rectangle's
centre texel, before the first down pass: a NaN, a +Inf, the largest half float ( 65504 ). Test switches for the
scrub and the knee: they write into the frame the tonemapper then reads ( the picture shows the bad pixel itself,
by design ), and they act only where the bloom runs. The frame is written again every frame, so a poke
never outlives its frame. The poke textures are made on first use; the frame's format must be RGBA16_FLOAT, else
the poke is skipped with one warning.
========================
*/
void BloomPass::PokeFrame( nvrhi::ICommandList* commandList, nvrhi::ITexture* frame, int centreX, int centreY, int debug )
{
	if( debug < 11 || debug > 13 )
	{
		return;
	}
	if( frame->getDesc().format != nvrhi::Format::RGBA16_FLOAT )
	{
		if( !m_PokeWarned )
		{
			m_PokeWarned = true;
			common->Warning( "BloomPass: r_bloomDebug %d pokes a half-float pixel, but the frame is not RGBA16_FLOAT: the poke does nothing", debug );
		}
		return;
	}
	// the half-float bits: 0x7E00 a NaN, 0x7C00 +Inf, 0x7BFF 65504; the alpha is 0x3C00 ( 1.0 )
	static const uint pokeBits[3] = { 0x7E00, 0x7C00, 0x7BFF };
	const uint bits = pokeBits[debug - 11];
	nvrhi::TextureHandle& poke = m_Pokes[debug - 11];
	if( !poke )
	{
		nvrhi::TextureDesc desc;
		desc.width = 1;
		desc.height = 1;
		desc.format = nvrhi::Format::RGBA16_FLOAT;
		desc.initialState = nvrhi::ResourceStates::CopyDest;
		desc.keepInitialState = true;
		desc.debugName = "Bloom/Poke";
		poke = m_Device->createTexture( desc );
		if( !poke )
		{
			if( !m_PokeWarned )
			{
				m_PokeWarned = true;
				common->Warning( "BloomPass: the device could not make the poke texture, the debug poke does nothing" );
			}
			return;
		}
		const halfFloat_t texel[4] = { ( halfFloat_t )bits, ( halfFloat_t )bits, ( halfFloat_t )bits, ( halfFloat_t )0x3C00 };
		commandList->writeTexture( poke, 0, 0, texel, sizeof( texel ) );
	}
	commandList->copyTexture( frame, nvrhi::TextureSlice().setOrigin( centreX, centreY, 0 ).setSize( 1, 1, 1 ),
							  poke, nvrhi::TextureSlice() );
}

/*
========================
BloomPass::ProbeCopy

The probe's first half: each level, as the down passes left it, and the whole frame copied into staging textures
of the source's format and size ( made at first use; the levels' die with the chain, so their sizes always match
the chain's, and the frame's matches the frame the chain was made for ). The copies are of the WHOLE textures,
not of the rectangles: Vulkan packs a rectangle copy's rows at the rectangle's width, while mapStagingTexture
reports the whole staging texture's pitch, so whenever the view is smaller than the frame a rectangle copy would
be walked at the wrong pitch by ProbeRead. Copying whole keeps the two pitches equal; ProbeRead still reads only
the rectangle's texels. The frame's staging is made only when the frame is RGBA16_FLOAT, the only format whose
texels ProbeRead can decode as the frame's. ProbeStageUp, behind the up passes of the same view, sends level 0
again into its staging texture; ProbeRead prints them some bloom views later.
========================
*/
bool BloomPass::ProbeCopy( nvrhi::ICommandList* commandList, nvrhi::ITexture* frame, int numLevels )
{
	if( !m_ProbeUpStaging )
	{
		nvrhi::TextureDesc desc;
		desc.width = m_TexW[0];
		desc.height = m_TexH[0];
		desc.format = m_ChainFormat;
		desc.debugName = "Bloom/ProbeUpStaging";
		m_ProbeUpStaging = m_Device->createStagingTexture( desc, nvrhi::CpuAccessMode::Read );
	}
	const nvrhi::TextureDesc& frameDesc = frame->getDesc();
	if( !m_ProbeFrameStaging && frameDesc.format == nvrhi::Format::RGBA16_FLOAT )
	{
		nvrhi::TextureDesc desc;
		desc.width = frameDesc.width;
		desc.height = frameDesc.height;
		desc.format = nvrhi::Format::RGBA16_FLOAT;
		desc.debugName = "Bloom/ProbeFrameStaging";
		m_ProbeFrameStaging = m_Device->createStagingTexture( desc, nvrhi::CpuAccessMode::Read );
	}
	for( int i = 0; i < numLevels; i++ )
	{
		if( !m_ProbeStagings[i] )
		{
			nvrhi::TextureDesc desc;
			desc.width = m_TexW[i];
			desc.height = m_TexH[i];
			desc.format = m_ChainFormat;
			desc.debugName = va( "Bloom/ProbeStaging%d", i );
			m_ProbeStagings[i] = m_Device->createStagingTexture( desc, nvrhi::CpuAccessMode::Read );
		}
		if( !m_ProbeStagings[i] || !m_ProbeUpStaging )
		{
			common->Printf( "bloomProbe: the staging textures could not be made, nothing was read\n" );
			return false;
		}
		commandList->copyTexture( m_ProbeStagings[i], nvrhi::TextureSlice(), m_Levels[i], nvrhi::TextureSlice() );
	}
	if( m_ProbeFrameStaging )
	{
		commandList->copyTexture( m_ProbeFrameStaging, nvrhi::TextureSlice(), frame, nvrhi::TextureSlice() );
	}
	m_ProbeWait = BLOOM_PROBE_WAIT;
	return true;
}

// the probe's second copy: level 0 as the up passes ( or the convolution's re-entry ) left it, for the term's mean
void BloomPass::ProbeStageUp( nvrhi::ICommandList* commandList )
{
	commandList->copyTexture( m_ProbeUpStaging, nvrhi::TextureSlice(), m_Levels[0], nvrhi::TextureSlice() );
}

/*
========================
BloomPass::ProbeRead

Prints the probe: the view and its parameters as the probe's view saw them, the mean of the frame's view
rectangle, then each level's rectangle and the mean of its texels after the down passes, then level 0's mean
after the up passes times w0, and that times the glow's brightness: the term the tonemapper adds. The means are
worked out here in double, from the
staging textures decoded by the chain's ( or, for the frame, its own ) format. The frame's mean prints as
-1 -1 -1 when the frame is not RGBA16_FLOAT and was not staged. The exposure buffer is not read back: the
adapted luminance prints as -1.
========================
*/
void BloomPass::ProbeRead()
{
	const bool r11g11b10 = ( m_ChainFormat == nvrhi::Format::R11G11B10_FLOAT );
	common->Printf( "bloomProbe: view %d %d %d %d levels %d glow %g tail %g w0 %g exposureScale %g knee %g debug %d ceiling %g\n",
					m_ProbeView[0], m_ProbeView[1], m_ProbeView[2], m_ProbeView[3], m_ProbeLevels, m_ProbeGlowGain, m_ProbeTail,
					m_ProbeW0, m_ProbeExposureScale, m_ProbeKnee, m_ProbeDebug, m_ProbeCeiling );
	common->Printf( "bloomProbe: threshold %g knee %g\n", m_ProbeThreshold, m_ProbeThresholdKnee );
	common->Printf( "bloomProbe: tail distance %d near %g far %g share %g\n", m_ProbeTailDistance, m_ProbeTailNear, m_ProbeTailFar, m_ProbeTailFarShare );
	// not "bloomProbe: view weapon ...": a reader that takes the last line beginning "bloomProbe: view " for the view
	// line would take this one ( review OH1-F1 )
	common->Printf( "bloomProbe: weapon left out %d\n", m_ProbeWeaponOut );
	common->Printf( "bloomProbe: mix weights frame %.9g bloom %.9g\n", m_ProbeMixFrame, m_ProbeMixBloom );
	// WP-J21 and WP-J24: the gate, the bias and the convolution ( "gate", "bias", "fft " and "fftout", which no other probe
	// line begins with )
	common->Printf( "bloomProbe: gate source %d mask %d fog %d\n", m_ProbeSource, m_ProbeMaskWritten, m_ProbeFogGlow );
	common->Printf( "bloomProbe: bias %g\n", m_ProbeBias );
	// WP-J27: the sources' own blooms and the hot boost ( classes 0 where the convolution did not run )
	common->Printf( "bloomProbe: styles slots %d overflow %d classes %d kernels made %d hot boost %g level %g\n",
					m_ProbeSlots, m_ProbeOverflow, m_ProbeClasses, m_ProbeKernelsBuilt, m_ProbeHotBoost, m_ProbeHotLevel );
	if( m_ProbeFft )
	{
		const BloomFftConstants& f = m_ProbeFftConstants;
		common->Printf( "bloomProbe: fft quality %d field %u %u picture %u %u scale %g reach %g level %d step %g shape %g kernel %s\n",
						m_ProbeFftQuality, f.field[0], f.field[1], f.image[0], f.image[1], f.shape.z, f.shape.x, m_ProbeFftLevel, f.source.x,
						f.kernelA.y, m_ProbeFftRebuilt ? "made" : "kept" );
		if( m_ProbeFftStaging )
		{
			size_t pitch = 0;
			const byte* fftData = ( const byte* )m_Device->mapStagingTexture( m_ProbeFftStaging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &pitch );
			if( fftData != NULL )
			{
				double fftMean[3];
				BloomProbeMean( fftData, pitch, 0, 0, ( int )f.image[0], ( int )f.image[1], false, fftMean );
				m_Device->unmapStagingTexture( m_ProbeFftStaging );
				common->Printf( "bloomProbe: fftout mean %.9g %.9g %.9g\n", fftMean[0], fftMean[1], fftMean[2] );
			}
		}
	}
	else
	{
		common->Printf( "bloomProbe: fft off\n" );
	}
	common->Printf( "bloomProbe: format %s frame %d %d adapted -1\n",
					r11g11b10 ? "R11G11B10_FLOAT" : "RGBA16_FLOAT", m_ProbeFrame[0], m_ProbeFrame[1] );
	if( m_ProbeFrameStaging )
	{
		size_t frameRowPitch = 0;
		const byte* frameData = ( const byte* )m_Device->mapStagingTexture( m_ProbeFrameStaging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &frameRowPitch );
		if( frameData == NULL )
		{
			common->Printf( "bloomProbe: the read-back failed\n" );
			return;
		}
		double frameMean[3];
		BloomProbeMean( frameData, frameRowPitch, m_ProbeView[0], m_ProbeView[1], m_ProbeView[2], m_ProbeView[3], false, frameMean );
		m_Device->unmapStagingTexture( m_ProbeFrameStaging );
		common->Printf( "bloomProbe: frame rect %d %d mean %.9g %.9g %.9g\n", m_ProbeView[2], m_ProbeView[3], frameMean[0], frameMean[1], frameMean[2] );
	}
	else
	{
		common->Printf( "bloomProbe: frame rect %d %d mean -1 -1 -1\n", m_ProbeView[2], m_ProbeView[3] );
	}
	for( int i = 0; i < m_ProbeLevels; i++ )
	{
		size_t rowPitch = 0;
		const byte* data = ( const byte* )m_Device->mapStagingTexture( m_ProbeStagings[i], nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch );
		if( data == NULL )
		{
			common->Printf( "bloomProbe: the read-back failed\n" );
			return;
		}
		double mean[3];
		BloomProbeMean( data, rowPitch, 0, 0, m_ProbeLevelW[i], m_ProbeLevelH[i], r11g11b10, mean );
		m_Device->unmapStagingTexture( m_ProbeStagings[i] );
		common->Printf( "bloomProbe: level %d rect %d %d mean %.9g %.9g %.9g\n", i, m_ProbeLevelW[i], m_ProbeLevelH[i], mean[0], mean[1], mean[2] );
	}
	size_t rowPitch = 0;
	const byte* data = ( const byte* )m_Device->mapStagingTexture( m_ProbeUpStaging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch );
	if( data == NULL )
	{
		common->Printf( "bloomProbe: the read-back failed\n" );
		return;
	}
	double mean[3];
	BloomProbeMean( data, rowPitch, 0, 0, m_ProbeLevelW[0], m_ProbeLevelH[0], r11g11b10, mean );
	m_Device->unmapStagingTexture( m_ProbeUpStaging );
	common->Printf( "bloomProbe: term mean %.9g %.9g %.9g\n", mean[0] * m_ProbeW0, mean[1] * m_ProbeW0, mean[2] * m_ProbeW0 );
	common->Printf( "bloomProbe: glow term mean %.9g %.9g %.9g\n", mean[0] * m_ProbeW0 * m_ProbeGlowGain, mean[1] * m_ProbeW0 * m_ProbeGlowGain, mean[2] * m_ProbeW0 * m_ProbeGlowGain );
	common->Printf( "bloomProbe: done\n" );
}

/*
================================================================================================

WP-J21: the bloom's gate on the GPU ( plan section 07, "The bloom's first pass" ). bloom_gate.cs.hlsl leaves the light
that passes the gate, from the TAA-resolved frame and the glow mask the emitting stages drew; the first pass's FIRST 3
and 4 read it in place of the frame and keep today's rules after it.

================================================================================================
*/

// the gate's push constants ( bloom_gate.cs.hlsl's BloomGateConstants )
struct BloomGateConstants
{
	int		rect[4];		// xy the view rectangle's origin in the frame, zw its size
	idVec4	params;			// x the cap's factor; WP-J27: y 1 = write the blocks' slot sums; z the hot level, w the threshold ( exposed, the bias divided out )
	idVec4	expo;			// WP-J27: x the exposure scale, y r_hdrMinLuminance ( the first pass's E ); zw 0
	idVec4	slotGain;		// WP-J27: each style slot's strength
	idVec4	slotBoost;		// WP-J27: each slot's hot boost in stops ( 0: none )
};

// the share pass's push constants ( bloom_share.cs.hlsl's BloomShareConstants )
struct BloomShareConstants
{
	int		size[4];		// x, y the source level's rectangle; z the blocks a texel spans an axis; w the spread in blocks
	int		blocks[4];		// x, y the blocks' rectangle
};

// the cap's factor: a pure emitter's pixel passes its own mask despite the mask's 16-bit rounding ( plan section 07 )
static const float BLOOM_GATE_CAP = 1.002f;

/*
========================
BloomPass::GateInit

At the first view under the gate: the formats, the three shaders ( the gate, FIRST 3 and FIRST 4 ), their layouts and
the gate's compute pipeline. Returns false for good where a part is missing; the bloom then keeps the open gate.
========================
*/
bool BloomPass::GateInit()
{
	if( m_GateTried )
	{
		return !m_GateDead;
	}
	m_GateTried = true;
	m_GateDead = true;

	const nvrhi::FormatSupport store = nvrhi::FormatSupport::Texture | nvrhi::FormatSupport::ShaderUavStore | nvrhi::FormatSupport::ShaderLoad;
	if( ( m_Device->queryFormatSupport( nvrhi::Format::RGBA16_FLOAT ) & store ) != store
			|| ( m_Device->queryFormatSupport( nvrhi::Format::R32_UINT ) & store ) != store )
	{
		common->Warning( "BloomPass: the device cannot store the gate's formats, the bloom takes everything bright ( r_bloomSource 0 )" );
		return false;
	}

	const idList<shaderMacro_t> noMacros;
	const idList<shaderMacro_t> gatedMacros = { { "FIRST", "3" } };
	const idList<shaderMacro_t> gatedDepthMacros = { { "FIRST", "4" } };
	// WP-J27: the gate's two forms, STYLES 0 for a view with no style of its own ( the common one, leaner ), 1 for a view with
	const idList<shaderMacro_t> gateMacros = { { "STYLES", "0" } };
	const idList<shaderMacro_t> gateStylesMacros = { { "STYLES", "1" } };
	m_GateCS = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/post/bloom_gate", SHADER_STAGE_COMPUTE, "", gateMacros, true, LAYOUT_DRAW_VERT ) );
	m_GateStylesCS = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/post/bloom_gate", SHADER_STAGE_COMPUTE, "_styles", gateStylesMacros, true, LAYOUT_DRAW_VERT ) );
	// the fog's glow, FIRST 5: where it cannot be made the fog gives no glow under the gate, and the gate itself stands
	const idList<shaderMacro_t> fogMacros = { { "FIRST", "5" } };
	m_DownFogPS = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/post/bloom_down", SHADER_STAGE_FRAGMENT, "_fog", fogMacros, true, LAYOUT_DRAW_VERT ) );
	nvrhi::BindingLayoutDesc fogDesc;
	fogDesc.visibility = nvrhi::ShaderType::Pixel;
	fogDesc.bindings =
	{
		nvrhi::BindingLayoutItem::PushConstants( 0, sizeof( BloomConstants ) ),
		nvrhi::BindingLayoutItem::Texture_SRV( 0 ),
		nvrhi::BindingLayoutItem::TypedBuffer_SRV( 1 ),
		nvrhi::BindingLayoutItem::Texture_SRV( 2 ),
		nvrhi::BindingLayoutItem::Texture_SRV( 3 ),
		nvrhi::BindingLayoutItem::Texture_SRV( 4 ),
		nvrhi::BindingLayoutItem::Sampler( 0 ),
		nvrhi::BindingLayoutItem::VolatileConstantBuffer( 1 ),
		nvrhi::BindingLayoutItem::Texture_SRV( 15 ),
		nvrhi::BindingLayoutItem::Sampler( 5 )
	};
	m_FogLayout = m_DownFogPS ? m_Device->createBindingLayout( fogDesc ) : nullptr;
	m_DownGatedPS = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/post/bloom_down", SHADER_STAGE_FRAGMENT, "_gated", gatedMacros, true, LAYOUT_DRAW_VERT ) );
	m_DownGatedDepthPS = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/post/bloom_down", SHADER_STAGE_FRAGMENT, "_gated_depth", gatedDepthMacros, true, LAYOUT_DRAW_VERT ) );
	if( !m_GateCS || !m_GateStylesCS || !m_DownGatedPS || !m_DownGatedDepthPS )
	{
		common->Warning( "BloomPass: the gate's shaders are missing, the bloom takes everything bright ( r_bloomSource 0 )" );
		return false;
	}

	nvrhi::BindingLayoutDesc gateDesc;
	gateDesc.visibility = nvrhi::ShaderType::Compute;
	gateDesc.bindings =
	{
		nvrhi::BindingLayoutItem::PushConstants( 0, sizeof( BloomGateConstants ) ),
		nvrhi::BindingLayoutItem::Texture_SRV( 0 ),
		nvrhi::BindingLayoutItem::Texture_SRV( 1 ),
		nvrhi::BindingLayoutItem::TypedBuffer_SRV( 2 ),		// WP-J27: the exposure, for the hot boost
		nvrhi::BindingLayoutItem::Texture_UAV( 0 ),
		nvrhi::BindingLayoutItem::Texture_UAV( 1 ),
		nvrhi::BindingLayoutItem::Texture_UAV( 2 )			// WP-J27: the blocks' slot sums
	};
	m_GateLayout = m_Device->createBindingLayout( gateDesc );

	nvrhi::BindingLayoutDesc gatedDesc;
	gatedDesc.visibility = nvrhi::ShaderType::Pixel;
	gatedDesc.bindings =
	{
		nvrhi::BindingLayoutItem::PushConstants( 0, sizeof( BloomConstants ) ),
		nvrhi::BindingLayoutItem::Texture_SRV( 0 ),
		nvrhi::BindingLayoutItem::TypedBuffer_SRV( 1 ),
		nvrhi::BindingLayoutItem::Texture_SRV( 3 ),
		nvrhi::BindingLayoutItem::Sampler( 0 )
	};
	m_GatedLayout = m_Device->createBindingLayout( gatedDesc );
	gatedDesc.bindings.push_back( nvrhi::BindingLayoutItem::Texture_SRV( 2 ) );
	m_GatedDepthLayout = m_Device->createBindingLayout( gatedDesc );

	if( m_GateLayout )
	{
		nvrhi::ComputePipelineDesc pipelineDesc;
		pipelineDesc.bindingLayouts = { m_GateLayout };
		pipelineDesc.CS = m_GateCS;
		m_GatePipeline = m_Device->createComputePipeline( pipelineDesc );
		pipelineDesc.CS = m_GateStylesCS;
		m_GateStylesPipeline = m_Device->createComputePipeline( pipelineDesc );
	}
	if( !m_GatePipeline || !m_GateStylesPipeline || !m_GatedLayout || !m_GatedDepthLayout )
	{
		common->Warning( "BloomPass: the device could not make the gate's pipeline or layouts, the bloom takes everything bright ( r_bloomSource 0 )" );
		return false;
	}
	m_GateDead = false;
	return true;
}

/*
========================
BloomPass::GateCheckSets

The gate's textures ( the frame's size, made with the chain at the first gated view and released with it ), the first
pass's gated pipelines against m_FirstFrame, and the sets: the gate's ( the frame and the mask ), FIRST 3's ( the
exposure buffer ) and, where depthTexture is given, FIRST 4's. Each set is made again when an object it names is
another one. Returns false where the gate itself cannot run; FIRST 4's set or pipeline missing only leaves
m_GatedDepthSet null.
========================
*/
bool BloomPass::GateCheckSets( nvrhi::ITexture* sourceTexture, nvrhi::IBuffer* exposureBuffer, nvrhi::ITexture* depthTexture )
{
	nvrhi::ITexture* mask = ( globalImages->glowMaskImage != NULL ) ? globalImages->glowMaskImage->GetTextureHandle().Get() : NULL;
	if( mask == NULL || mask->getDesc().width != ( uint32_t )m_ChainWidth || mask->getDesc().height != ( uint32_t )m_ChainHeight )
	{
		return false;
	}
	if( !m_Gated || !m_GateFlags )
	{
		nvrhi::TextureDesc desc;
		desc.dimension = nvrhi::TextureDimension::Texture2D;
		desc.mipLevels = 1;
		desc.isUAV = true;
		desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
		desc.keepInitialState = true;
		desc.width = m_ChainWidth;
		desc.height = m_ChainHeight;
		desc.format = nvrhi::Format::RGBA16_FLOAT;
		desc.debugName = "Bloom/Gated";
		m_Gated = m_Device->createTexture( desc );
		desc.width = ( m_ChainWidth + 15 ) / 16;
		desc.height = ( m_ChainHeight + 15 ) / 16;
		desc.format = nvrhi::Format::R32_UINT;
		desc.debugName = "Bloom/GateFlags";
		m_GateFlags = m_Device->createTexture( desc );
		if( !m_Gated || !m_GateFlags )
		{
			m_Gated = nullptr;
			m_GateFlags = nullptr;
			return false;
		}
	}
	if( !StyleCheckTextures() )
	{
		return false;
	}
	if( !m_DownGatedPipeline )
	{
		nvrhi::GraphicsPipelineDesc psoDesc;
		psoDesc.VS = m_VS;
		psoDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
		psoDesc.renderState.rasterState.setCullNone();
		psoDesc.renderState.depthStencilState.depthTestEnable = false;
		psoDesc.renderState.depthStencilState.depthWriteEnable = false;
		psoDesc.renderState.depthStencilState.stencilEnable = false;
		psoDesc.bindingLayouts = { m_GatedLayout };
		psoDesc.PS = m_DownGatedPS;
		m_DownGatedPipeline = m_Device->createGraphicsPipeline( psoDesc, m_FirstFrame );
		psoDesc.bindingLayouts = { m_GatedDepthLayout };
		psoDesc.PS = m_DownGatedDepthPS;
		m_DownGatedDepthPipeline = m_Device->createGraphicsPipeline( psoDesc, m_FirstFrame );
		if( !m_DownGatedPipeline )
		{
			return false;
		}
	}
	if( !m_GateSet || m_GateFrame != sourceTexture || m_GateMask != mask || m_GateExposure != exposureBuffer )
	{
		nvrhi::BindingSetDesc setDesc;
		setDesc.bindings =
		{
			nvrhi::BindingSetItem::PushConstants( 0, sizeof( BloomGateConstants ) ),
			nvrhi::BindingSetItem::Texture_SRV( 0, sourceTexture ),
			nvrhi::BindingSetItem::Texture_SRV( 1, mask ),
			nvrhi::BindingSetItem::TypedBuffer_SRV( 2, exposureBuffer ),
			nvrhi::BindingSetItem::Texture_UAV( 0, m_Gated ),
			nvrhi::BindingSetItem::Texture_UAV( 1, m_GateFlags ),
			nvrhi::BindingSetItem::Texture_UAV( 2, m_StyleSums )
		};
		m_GateSet = m_Device->createBindingSet( setDesc, m_GateLayout );
		m_GateFrame = m_GateSet ? sourceTexture : NULL;
		m_GateMask = m_GateSet ? mask : NULL;
		m_GateExposure = m_GateSet ? exposureBuffer : NULL;
		if( !m_GateSet )
		{
			return false;
		}
	}
	if( !m_GatedSet || m_GatedExposure != exposureBuffer )
	{
		nvrhi::BindingSetDesc setDesc;
		setDesc.bindings =
		{
			nvrhi::BindingSetItem::PushConstants( 0, sizeof( BloomConstants ) ),
			nvrhi::BindingSetItem::Texture_SRV( 0, m_Gated ),
			nvrhi::BindingSetItem::TypedBuffer_SRV( 1, exposureBuffer ),
			nvrhi::BindingSetItem::Texture_SRV( 3, m_GateFlags ),
			nvrhi::BindingSetItem::Sampler( 0, commonRenderPasses->m_LinearClampSampler )
		};
		m_GatedSet = m_Device->createBindingSet( setDesc, m_GatedLayout );
		m_GatedExposure = m_GatedSet ? exposureBuffer : NULL;
		if( !m_GatedSet )
		{
			return false;
		}
	}
	if( depthTexture != NULL && m_DownGatedDepthPipeline
			&& ( !m_GatedDepthSet || m_GatedDepthExposure != exposureBuffer || m_GatedDepthImage != depthTexture ) )
	{
		nvrhi::BindingSetDesc setDesc;
		setDesc.bindings =
		{
			nvrhi::BindingSetItem::PushConstants( 0, sizeof( BloomConstants ) ),
			nvrhi::BindingSetItem::Texture_SRV( 0, m_Gated ),
			nvrhi::BindingSetItem::TypedBuffer_SRV( 1, exposureBuffer ),
			nvrhi::BindingSetItem::Texture_SRV( 3, m_GateFlags ),
			nvrhi::BindingSetItem::Sampler( 0, commonRenderPasses->m_LinearClampSampler ),
			nvrhi::BindingSetItem::Texture_SRV( 2, depthTexture )
		};
		m_GatedDepthSet = m_Device->createBindingSet( setDesc, m_GatedDepthLayout );
		m_GatedDepthExposure = m_GatedDepthSet ? exposureBuffer : NULL;
		m_GatedDepthImage = m_GatedDepthSet ? depthTexture : NULL;
	}
	return true;
}

/*
========================
BloomPass::FogCheckSet

WP-J21, the fog's glow: FIRST 5's pipeline against level 1's framebuffer ( made with the chain ) and its set, which
names the wide levels' source, the exposure buffer, the Hi-Z image's mip 2, the resolved frame, the fog's never-glowing
integral and the engine's fog set ( its parms buffer and this view's integrated volume ): made again when one of them
is another object. False where any part is missing: level 1 is then reduced without the fog's glow
========================
*/
bool BloomPass::FogCheckSet( nvrhi::ITexture* sourceTexture, nvrhi::IBuffer* exposureBuffer )
{
	if( !m_FogLayout || !m_Frames[1] || !m_Wide )
	{
		return false;
	}
	nvrhi::ITexture* hiZ = ( globalImages->hierarchicalZbufferImage != NULL ) ? globalImages->hierarchicalZbufferImage->GetTextureHandle().Get() : NULL;
	if( hiZ == NULL || hiZ->getDesc().mipLevels < 3 || backEnd.froxelNoGlowVolume == NULL || !backEnd.froxelVolume || !backEnd.froxelFogParms )
	{
		return false;
	}
	if( !m_DownFogPipeline )
	{
		nvrhi::GraphicsPipelineDesc psoDesc;
		psoDesc.VS = m_VS;
		psoDesc.PS = m_DownFogPS;
		psoDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
		psoDesc.renderState.rasterState.setCullNone();
		psoDesc.renderState.depthStencilState.depthTestEnable = false;
		psoDesc.renderState.depthStencilState.depthWriteEnable = false;
		psoDesc.renderState.depthStencilState.stencilEnable = false;
		psoDesc.bindingLayouts = { m_FogLayout };
		m_DownFogPipeline = m_Device->createGraphicsPipeline( psoDesc, m_Frames[1] );
		if( !m_DownFogPipeline )
		{
			return false;
		}
	}
	void* key[8] = { m_Wide.Get(), exposureBuffer, hiZ, sourceTexture, backEnd.froxelNoGlowVolume, backEnd.froxelVolume.Get(), backEnd.froxelFogParms.Get(), commonRenderPasses->m_LinearClampSampler.Get() };
	if( m_FogSet && memcmp( key, m_FogSetKey, sizeof( key ) ) == 0 )
	{
		return true;
	}
	nvrhi::BindingSetDesc setDesc;
	setDesc.bindings =
	{
		nvrhi::BindingSetItem::PushConstants( 0, sizeof( BloomConstants ) ),
		nvrhi::BindingSetItem::Texture_SRV( 0, m_Wide ),
		nvrhi::BindingSetItem::TypedBuffer_SRV( 1, exposureBuffer ),
		nvrhi::BindingSetItem::Texture_SRV( 2, hiZ, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet( 2, 1, 0, 1 ) ),
		nvrhi::BindingSetItem::Texture_SRV( 3, sourceTexture ),
		nvrhi::BindingSetItem::Texture_SRV( 4, backEnd.froxelNoGlowVolume ),
		nvrhi::BindingSetItem::Sampler( 0, commonRenderPasses->m_LinearClampSampler ),
		nvrhi::BindingSetItem::ConstantBuffer( 1, backEnd.froxelFogParms ),
		nvrhi::BindingSetItem::Texture_SRV( 15, backEnd.froxelVolume ),
		nvrhi::BindingSetItem::Sampler( 5, commonRenderPasses->m_LinearClampSampler )
	};
	m_FogSet = m_Device->createBindingSet( setDesc, m_FogLayout );
	if( !m_FogSet )
	{
		memset( m_FogSetKey, 0, sizeof( m_FogSetKey ) );
		return false;
	}
	memcpy( m_FogSetKey, key, sizeof( key ) );
	return true;
}

/*
================================================================================================

WP-J24: the convolution's GPU side. bloom_fft.cs.hlsl's six passes and the re-entry draw. The kernel ( passes 0, 2,
4 ) is made again only when one of its parameters, the quality or the view's size changes; every bloom view runs the
rows forward ( 1 ), the columns with the product ( 3 ), the rows back ( 5 ) and the re-entry into level 0.

================================================================================================
*/

/*
========================
BloomPass::FftInit

At the first view that asks for the convolution: the device's formats are checked, the seven shaders found ( a
run with the convolution off never loads them ), the layouts, the six compute pipelines and the constant buffer
made. Returns false for good where any part is missing; the bloom then keeps the chain.
========================
*/
bool BloomPass::FftInit()
{
	if( m_FftTried )
	{
		return !m_FftDead;
	}
	m_FftTried = true;
	m_FftDead = true;

	// the field's spectra are read and written as typed UAVs; the result is stored as one and sampled by the re-entry
	const nvrhi::FormatSupport uav = nvrhi::FormatSupport::Texture | nvrhi::FormatSupport::ShaderUavLoad | nvrhi::FormatSupport::ShaderUavStore;
	const nvrhi::FormatSupport result = nvrhi::FormatSupport::Texture | nvrhi::FormatSupport::ShaderUavStore | nvrhi::FormatSupport::ShaderSample;
	if( ( m_Device->queryFormatSupport( nvrhi::Format::RGBA32_FLOAT ) & uav ) != uav
			|| ( m_Device->queryFormatSupport( nvrhi::Format::RG32_FLOAT ) & uav ) != uav
			|| ( m_Device->queryFormatSupport( nvrhi::Format::RGBA16_FLOAT ) & result ) != result )
	{
		common->Warning( "BloomPass: the device cannot store the convolution's formats, the bloom keeps the chain" );
		return false;
	}

	// the seven passes are seven permutations of one blob: FindShader knows each by its suffix, the macro list picks it
	for( int pass = 0; pass < BLOOM_FFT_PASSES; pass++ )
	{
		const idList<shaderMacro_t> macros = { { "PASS", va( "%d", pass ) } };
		m_FftCS[pass] = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/post/bloom_fft", SHADER_STAGE_COMPUTE, va( "_p%d", pass ), macros, true, LAYOUT_DRAW_VERT ) );
		if( !m_FftCS[pass] )
		{
			common->Warning( "BloomPass: the convolution's shaders are missing, the bloom keeps the chain" );
			return false;
		}
	}
	const idList<shaderMacro_t> noMacros;
	m_FftReentryPS = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/post/bloom_fft_compose", SHADER_STAGE_FRAGMENT, "", noMacros, true, LAYOUT_DRAW_VERT ) );
	if( !m_FftReentryPS )
	{
		common->Warning( "BloomPass: the convolution's re-entry shader is missing, the bloom keeps the chain" );
		return false;
	}

	// each pass's bindings, as bloom_fft.cs.hlsl declares them; the constants at b1 in every pass
	nvrhi::BindingLayoutDesc layoutDesc[ BLOOM_FFT_PASSES ];
	for( int pass = 0; pass < BLOOM_FFT_PASSES; pass++ )
	{
		layoutDesc[pass].visibility = nvrhi::ShaderType::Compute;
		layoutDesc[pass].bindings.push_back( nvrhi::BindingLayoutItem::VolatileConstantBuffer( 1 ) );
	}
	layoutDesc[0].bindings.push_back( nvrhi::BindingLayoutItem::Texture_UAV( 0 ) );
	for( int pass = 1; pass <= 3; pass++ )
	{
		layoutDesc[pass].bindings.push_back( nvrhi::BindingLayoutItem::Texture_SRV( 0 ) );
	}
	layoutDesc[1].bindings.push_back( nvrhi::BindingLayoutItem::Texture_SRV( 1 ) );		// WP-J27: the slots' shares
	for( int pass = 1; pass < 6; pass++ )
	{
		layoutDesc[pass].bindings.push_back( nvrhi::BindingLayoutItem::Texture_UAV( 0 ) );
		layoutDesc[pass].bindings.push_back( nvrhi::BindingLayoutItem::Texture_UAV( 1 ) );
	}
	layoutDesc[4].bindings.push_back( nvrhi::BindingLayoutItem::Texture_UAV( 2 ) );
	layoutDesc[5].bindings.push_back( nvrhi::BindingLayoutItem::Texture_UAV( 2 ) );
	// WP-J27: PASS 6 binds as PASS 3
	layoutDesc[6].bindings.push_back( nvrhi::BindingLayoutItem::Texture_SRV( 0 ) );
	layoutDesc[6].bindings.push_back( nvrhi::BindingLayoutItem::Texture_UAV( 0 ) );
	layoutDesc[6].bindings.push_back( nvrhi::BindingLayoutItem::Texture_UAV( 1 ) );
	for( int pass = 0; pass < BLOOM_FFT_PASSES; pass++ )
	{
		m_FftLayouts[pass] = m_Device->createBindingLayout( layoutDesc[pass] );
		nvrhi::ComputePipelineDesc pipelineDesc;
		pipelineDesc.bindingLayouts = { m_FftLayouts[pass] };
		pipelineDesc.CS = m_FftCS[pass];
		m_FftPipelines[pass] = m_FftLayouts[pass] ? m_Device->createComputePipeline( pipelineDesc ) : nullptr;
		if( !m_FftPipelines[pass] )
		{
			common->Warning( "BloomPass: the device could not make the convolution's pipelines, the bloom keeps the chain" );
			return false;
		}
	}

	nvrhi::BindingLayoutDesc reentryDesc;
	reentryDesc.visibility = nvrhi::ShaderType::Pixel;
	reentryDesc.bindings =
	{
		nvrhi::BindingLayoutItem::VolatileConstantBuffer( 1 ),
		nvrhi::BindingLayoutItem::Texture_SRV( 0 ),
		nvrhi::BindingLayoutItem::Sampler( 0 )
	};
	m_FftReentryLayout = m_Device->createBindingLayout( reentryDesc );

	// WP-J27: the share pass ( bloom_share.cs.hlsl ), which splits the light by the sources' styles
	m_ShareCS = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/post/bloom_share", SHADER_STAGE_COMPUTE, "", noMacros, true, LAYOUT_DRAW_VERT ) );
	nvrhi::BindingLayoutDesc shareDesc;
	shareDesc.visibility = nvrhi::ShaderType::Compute;
	shareDesc.bindings =
	{
		nvrhi::BindingLayoutItem::PushConstants( 0, sizeof( BloomShareConstants ) ),
		nvrhi::BindingLayoutItem::Texture_SRV( 0 ),
		nvrhi::BindingLayoutItem::Texture_UAV( 0 )
	};
	m_ShareLayout = m_ShareCS ? m_Device->createBindingLayout( shareDesc ) : nullptr;
	if( m_ShareLayout )
	{
		nvrhi::ComputePipelineDesc pipelineDesc;
		pipelineDesc.bindingLayouts = { m_ShareLayout };
		pipelineDesc.CS = m_ShareCS;
		m_SharePipeline = m_Device->createComputePipeline( pipelineDesc );
	}
	if( !m_SharePipeline )
	{
		// review R2-N8: the transform stands without it; only the sources' own kernels are lost ( one class a view )
		common->Warning( "BloomPass: the convolution's share pass could not be made, the lights' own bloom shapes are off" );
	}

	nvrhi::BufferDesc constantBufferDesc;
	constantBufferDesc.byteSize = sizeof( BloomFftConstants );
	constantBufferDesc.debugName = "BloomFftConstants";
	constantBufferDesc.isConstantBuffer = true;
	constantBufferDesc.isVolatile = true;
	// WP-J27: a view writes the constants once a kernel it remakes and once for the light, up to five times ( review R2-N7 )
	constantBufferDesc.maxVersions = Max( ( uint32_t )c_MaxRenderPassConstantBufferVersions, ( uint32_t )64 );
	m_FftConstantBuffer = m_Device->createBuffer( constantBufferDesc );
	if( !m_FftReentryLayout || !m_FftConstantBuffer )
	{
		common->Warning( "BloomPass: the device could not make the convolution's constants or re-entry layout, the bloom keeps the chain" );
		return false;
	}
	m_FftDead = false;
	return true;
}

// the field's textures and every set that names them go; the kernel must be made again
void BloomPass::FftReleaseField()
{
	m_FftKernel = nullptr;
	m_FftSpecRG = nullptr;
	m_FftSpecB = nullptr;
	m_FftKernelSpec = nullptr;
	m_FftResult = nullptr;
	for( int i = 0; i < BLOOM_FFT_PASSES; i++ )
	{
		m_FftSets[i] = nullptr;
	}
	m_FftSet1Source = NULL;
	m_FftReentrySet = nullptr;
	m_ProbeFftStaging = nullptr;
	m_FftW = 0;
	m_FftH = 0;
	memset( m_FftKernelValid, 0, sizeof( m_FftKernelValid ) );
}

/*
========================
BloomPass::FftCheckField

The field's five textures for W x H cells and the sets of the passes that name only them ( 0, 2, 3, 4, 5 and the
re-entry ). Made again when the quality changes the field; the old ones go first. Returns false where the device
refuses a part: the size is then forgotten, so the next view tries again only if the quality moves. WP-J27: the spectra
and the kernel spectra are arrays of slices ( one a class, one a kernel ): one slice until a view splits its light by the
sources' styles, then BLOOM_FFT_CLASSES for as long as the field stands.
========================
*/
bool BloomPass::FftCheckField( int fieldW, int fieldH, int slices )
{
	slices = ( slices > 1 && !m_FftSplitRefused ) ? BLOOM_FFT_CLASSES : 1;
	if( fieldW == m_FftW && fieldH == m_FftH && slices <= m_FftSlices )
	{
		return m_FftResult != nullptr;
	}
	FftReleaseField();
	m_FftW = fieldW;
	m_FftH = fieldH;
	m_FftSlices = slices;

	nvrhi::TextureDesc desc;
	desc.dimension = nvrhi::TextureDimension::Texture2D;
	desc.mipLevels = 1;
	desc.isUAV = true;
	desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
	desc.keepInitialState = true;

	const int bins = fieldW / 2 + 1;
	desc.width = fieldW;
	desc.height = fieldH;
	desc.format = nvrhi::Format::RGBA32_FLOAT;
	desc.debugName = "Bloom/FftKernel";
	m_FftKernel = m_Device->createTexture( desc );
	desc.format = nvrhi::Format::RGBA16_FLOAT;
	desc.debugName = "Bloom/FftOut";
	m_FftResult = m_Device->createTexture( desc );
	// WP-J27: the spectra, a slice a class, and the kernel spectra, a slice a kernel, as 2D arrays ( the shaders' types
	// are arrays at one slice too )
	desc.dimension = nvrhi::TextureDimension::Texture2DArray;
	desc.arraySize = slices;
	desc.width = bins;
	desc.format = nvrhi::Format::RGBA32_FLOAT;
	desc.debugName = "Bloom/FftSpecRG";
	m_FftSpecRG = m_Device->createTexture( desc );
	desc.debugName = "Bloom/FftKernelSpec";
	m_FftKernelSpec = m_Device->createTexture( desc );
	desc.format = nvrhi::Format::RG32_FLOAT;
	desc.debugName = "Bloom/FftSpecB";
	m_FftSpecB = m_Device->createTexture( desc );

	if( m_FftKernel && m_FftResult && m_FftSpecRG && m_FftKernelSpec && m_FftSpecB )
	{
		const nvrhi::BindingSetItem cb = nvrhi::BindingSetItem::ConstantBuffer( 1, m_FftConstantBuffer );
		const nvrhi::TextureSubresourceSet all( 0, 1, 0, ( nvrhi::ArraySlice )slices );
		const nvrhi::BindingSetItem specRG = nvrhi::BindingSetItem::Texture_UAV( 0, m_FftSpecRG, nvrhi::Format::UNKNOWN, all, nvrhi::TextureDimension::Texture2DArray );
		const nvrhi::BindingSetItem specB = nvrhi::BindingSetItem::Texture_UAV( 1, m_FftSpecB, nvrhi::Format::UNKNOWN, all, nvrhi::TextureDimension::Texture2DArray );
		nvrhi::BindingSetDesc setDesc[ BLOOM_FFT_PASSES ];
		setDesc[0].bindings = { cb, nvrhi::BindingSetItem::Texture_UAV( 0, m_FftKernel ) };
		setDesc[2].bindings = { cb, nvrhi::BindingSetItem::Texture_SRV( 0, m_FftKernel ), specRG, specB };
		setDesc[3].bindings = { cb, nvrhi::BindingSetItem::Texture_SRV( 0, m_FftKernelSpec, nvrhi::Format::UNKNOWN, all, nvrhi::TextureDimension::Texture2DArray ),
								specRG, specB
							  };
		setDesc[4].bindings = { cb, specRG, specB,
								nvrhi::BindingSetItem::Texture_UAV( 2, m_FftKernelSpec, nvrhi::Format::UNKNOWN, all, nvrhi::TextureDimension::Texture2DArray )
							  };
		setDesc[5].bindings = { cb, specRG, specB, nvrhi::BindingSetItem::Texture_UAV( 2, m_FftResult ) };
		setDesc[6].bindings = setDesc[3].bindings;		// WP-J27
		bool ok = true;
		for( int pass = 0; pass < BLOOM_FFT_PASSES; pass++ )
		{
			if( pass != 1 )
			{
				m_FftSets[pass] = m_Device->createBindingSet( setDesc[pass], m_FftLayouts[pass] );
				ok = ok && m_FftSets[pass] != nullptr;
			}
		}
		nvrhi::BindingSetDesc reentryDesc;
		reentryDesc.bindings =
		{
			cb,
			nvrhi::BindingSetItem::Texture_SRV( 0, m_FftResult ),
			nvrhi::BindingSetItem::Sampler( 0, commonRenderPasses->m_LinearClampSampler )
		};
		m_FftReentrySet = m_Device->createBindingSet( reentryDesc, m_FftReentryLayout );
		if( ok && m_FftReentrySet )
		{
			// a new texture holds whatever the allocation held: the result's cells beyond the picture are read by the
			// re-entry's B-spline at weight 0 only, and a NaN times 0 is a NaN, so FftRender clears it once
			m_FftFresh = true;
			return true;
		}
	}
	FftReleaseField();
	if( slices > 1 )
	{
		// review R2-F3: the split's slices refused, the one-class field is made again and every view keeps the transform
		// with the view's kernel alone
		common->Warning( "BloomPass: the device could not make the convolution's field of %d x %d cells in %d slices, the lights' own bloom shapes are off", fieldW, fieldH, slices );
		m_FftSplitRefused = true;
		return FftCheckField( fieldW, fieldH, 1 );
	}
	common->Warning( "BloomPass: the device could not make the convolution's field of %d x %d cells, the bloom keeps the chain", fieldW, fieldH );
	// the refused size is kept ( FftReleaseField forgets it ): with no result the test above answers false until the
	// quality moves, with no new attempt and no new warning each view ( review R1-6 )
	m_FftW = fieldW;
	m_FftH = fieldH;
	m_FftSlices = BLOOM_FFT_CLASSES;
	return false;
}

// the rows' set: the chain level they read and ( WP-J27 ) the slots' shares, made again when the level is another texture
// ( the chain was made again ) or the field was ( FftReleaseField drops it )
bool BloomPass::FftCheckSourceSet( nvrhi::ITexture* source )
{
	if( m_FftSets[1] && m_FftSet1Source == source )
	{
		return true;
	}
	if( !StyleCheckTextures() )
	{
		return false;
	}
	const nvrhi::TextureSubresourceSet all( 0, 1, 0, ( nvrhi::ArraySlice )m_FftSlices );
	nvrhi::BindingSetDesc setDesc;
	setDesc.bindings =
	{
		nvrhi::BindingSetItem::ConstantBuffer( 1, m_FftConstantBuffer ),
		nvrhi::BindingSetItem::Texture_SRV( 0, source ),
		nvrhi::BindingSetItem::Texture_SRV( 1, m_StyleShares ),
		nvrhi::BindingSetItem::Texture_UAV( 0, m_FftSpecRG, nvrhi::Format::UNKNOWN, all, nvrhi::TextureDimension::Texture2DArray ),
		nvrhi::BindingSetItem::Texture_UAV( 1, m_FftSpecB, nvrhi::Format::UNKNOWN, all, nvrhi::TextureDimension::Texture2DArray )
	};
	m_FftSets[1] = m_Device->createBindingSet( setDesc, m_FftLayouts[1] );
	m_FftSet1Source = m_FftSets[1] ? source : NULL;
	return m_FftSets[1] != nullptr;
}

// the re-entry's pipeline: into level 0 with the up pass's blend, ONE, ONE, made at the first convolution of a chain
bool BloomPass::FftCheckReentry()
{
	if( m_FftReentryPipeline )
	{
		return true;
	}
	if( !m_Frames[0] )
	{
		return false;
	}
	nvrhi::GraphicsPipelineDesc psoDesc;
	psoDesc.VS = m_VS;
	psoDesc.PS = m_FftReentryPS;
	psoDesc.primType = nvrhi::PrimitiveType::TriangleStrip;
	psoDesc.renderState.rasterState.setCullNone();
	psoDesc.renderState.depthStencilState.depthTestEnable = false;
	psoDesc.renderState.depthStencilState.depthWriteEnable = false;
	psoDesc.renderState.depthStencilState.stencilEnable = false;
	psoDesc.renderState.blendState.targets[0].blendEnable = true;
	psoDesc.renderState.blendState.targets[0].srcBlend = nvrhi::BlendFactor::One;
	psoDesc.renderState.blendState.targets[0].destBlend = nvrhi::BlendFactor::One;
	psoDesc.renderState.blendState.targets[0].srcBlendAlpha = nvrhi::BlendFactor::One;
	psoDesc.renderState.blendState.targets[0].destBlendAlpha = nvrhi::BlendFactor::One;
	psoDesc.bindingLayouts = { m_FftReentryLayout };
	m_FftReentryPipeline = m_Device->createGraphicsPipeline( psoDesc, m_Frames[0] );
	return m_FftReentryPipeline != nullptr;
}

void BloomPass::FftDispatch( nvrhi::ICommandList* commandList, int pass, nvrhi::IBindingSet* bindingSet, int groupsX, int groupsY )
{
	nvrhi::ComputeState state;
	state.pipeline = m_FftPipelines[pass];
	state.bindings = { bindingSet };
	commandList->setComputeState( state );
	commandList->dispatch( groupsX, groupsY, 1 );
}

/*
========================
BloomPass::StyleCheckTextures

WP-J27: the textures the sources' styles split the light with, a texel a 4 x 4 block of the frame, made with the chain at
their first use ( the gate writes the first, the rows forward read the second, so both stand wherever either pass runs ),
and the share pass's set
========================
*/
bool BloomPass::StyleCheckTextures()
{
	if( !m_StyleSums || !m_StyleShares )
	{
		m_ShareSet = nullptr;
		if( m_ChainWidth <= 0 || m_ChainHeight <= 0 )
		{
			return false;
		}
		nvrhi::TextureDesc desc;
		desc.dimension = nvrhi::TextureDimension::Texture2D;
		desc.mipLevels = 1;
		desc.isUAV = true;
		desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
		desc.keepInitialState = true;
		desc.width = ( m_ChainWidth + 3 ) / 4;
		desc.height = ( m_ChainHeight + 3 ) / 4;
		desc.format = nvrhi::Format::RGBA32_FLOAT;		// a block of boosted lamps passes 65504 ( review R2-N2 )
		desc.debugName = "Bloom/StyleSums";
		m_StyleSums = m_Device->createTexture( desc );
		desc.format = nvrhi::Format::RGBA16_FLOAT;
		desc.debugName = "Bloom/StyleShares";
		m_StyleShares = m_Device->createTexture( desc );
		if( !m_StyleSums || !m_StyleShares )
		{
			m_StyleSums = nullptr;
			m_StyleShares = nullptr;
			return false;
		}
	}
	// the share pass's set, once the convolution's layouts stand ( the gate alone needs only the sums )
	if( !m_ShareSet && m_ShareLayout && m_SharePipeline )
	{
		nvrhi::BindingSetDesc setDesc;
		setDesc.bindings =
		{
			nvrhi::BindingSetItem::PushConstants( 0, sizeof( BloomShareConstants ) ),
			nvrhi::BindingSetItem::Texture_SRV( 0, m_StyleSums ),
			nvrhi::BindingSetItem::Texture_UAV( 0, m_StyleShares )
		};
		m_ShareSet = m_Device->createBindingSet( setDesc, m_ShareLayout );
	}
	return true;
}

/*
========================
BloomPass::FftKernelSlice

WP-J27: the kernel slice class classIndex reads its kernel from. Class 0, the view's kernel, has slice 0. Another class
takes a slice that holds its kernel already ( kept from an earlier view ), else one that no class of this view has taken,
an empty one first, then the one read longest ago; taken[] marks this view's slices. Returns the slice, with
m_FftKernelValid false where the kernel must be made
========================
*/
int BloomPass::FftKernelSlice( const BloomFftConstants& kernel, int classIndex, const int* taken )
{
	if( classIndex == 0 )
	{
		return 0;
	}
	for( int s = 1; s < BLOOM_FFT_CLASSES; s++ )
	{
		if( !taken[s] && m_FftKernelValid[s] && BloomFftKernelKeyEquals( kernel, m_FftKernelKey[s] ) )
		{
			return s;
		}
	}
	int pick = -1;
	for( int s = 1; s < BLOOM_FFT_CLASSES; s++ )
	{
		if( taken[s] )
		{
			continue;
		}
		if( pick < 0 || ( !m_FftKernelValid[s] && m_FftKernelValid[pick] )
				|| ( m_FftKernelValid[s] == m_FftKernelValid[pick] && m_FftKernelUsed[s] < m_FftKernelUsed[pick] ) )
		{
			pick = s;
		}
	}
	// a view has at most BLOOM_FFT_CLASSES classes, so a slice is always left
	assert( pick > 0 );
	m_FftKernelValid[pick] = false;
	return pick;
}

/*
========================
BloomPass::FftRender

The convolution of one view, after the down passes have drawn the level it reads: the constants, the kernels where
they changed, the passes of the light, and the re-entry into level 0. The caller has checked the field, the rows' set
and the re-entry's pipeline. WP-J27: constants.classes.x classes, each with its kernel's constants in classKernels[]
( class 0's are the view's, constants itself ) and its slice in constants.classKernel; with more than one the share pass
first spreads the gate's block sums into the source texels' shares ( fftLevel's texels, the view's blocks )
========================
*/
void BloomPass::FftRender( nvrhi::ICommandList* commandList, const int* levelW, const int* levelH, const BloomFftConstants& constants,
						   const BloomFftConstants* classKernels, int fftLevel, int viewW, int viewH )
{
	const int W = ( int )constants.field[0];
	const int H = ( int )constants.field[1];
	const int bins = W / 2 + 1;
	const int pairs = ( ( int )constants.image[1] + 1 ) / 2;
	const int classes = idMath::ClampInt( 1, BLOOM_FFT_CLASSES, ( int )constants.classes[0] );
	m_FftSerial++;

	if( m_FftFresh )
	{
		commandList->clearTextureFloat( m_FftResult, nvrhi::AllSubresources, nvrhi::Color( 0.0f ) );
		m_FftFresh = false;
	}

	// the kernels and their spectra, only those that changed: each made from its own constants into its slice ( the
	// kernel's rows use slice 0 of the spectra as scratch, before the light's rows fill it )
	m_ProbeFftRebuilt = false;
	m_ProbeKernelsBuilt = 0;
	for( int c = 0; c < classes; c++ )
	{
		const int slice = ( int )constants.classKernel[c];
		m_FftKernelUsed[slice] = m_FftSerial;
		if( m_FftKernelValid[slice] && BloomFftKernelKeyEquals( classKernels[c], m_FftKernelKey[slice] ) )
		{
			continue;
		}
		BloomFftConstants kernelConstants = classKernels[c];
		kernelConstants.classes[1] = ( uint )slice;
		commandList->writeBuffer( m_FftConstantBuffer, &kernelConstants, sizeof( kernelConstants ) );
		commandList->beginMarker( "BloomFftKernel" );
		FftDispatch( commandList, 0, m_FftSets[0], ( W + 7 ) / 8, ( H + 7 ) / 8 );
		FftDispatch( commandList, 2, m_FftSets[2], H / 2, 1 );
		FftDispatch( commandList, 4, m_FftSets[4], bins, 1 );
		commandList->endMarker();
		m_FftKernelKey[slice] = classKernels[c];
		m_FftKernelValid[slice] = true;
		m_ProbeFftRebuilt = true;
		m_ProbeKernelsBuilt++;
	}

	commandList->writeBuffer( m_FftConstantBuffer, &constants, sizeof( constants ) );
	commandList->beginMarker( "BloomFft" );
	if( classes > 1 )
	{
		// WP-J27: each source texel's shares of the slots, from the gate's block sums over the footprint the chain's filters
		// spread a texel's light from: f blocks a texel an axis, 2f - 1 blocks beyond ( level 1: its own and one each side )
		const int f = 1 << ( fftLevel - 1 );
		BloomShareConstants share;
		share.size[0] = levelW[fftLevel];
		share.size[1] = levelH[fftLevel];
		share.size[2] = f;
		share.size[3] = 2 * f - 1;
		share.blocks[0] = ( viewW + 3 ) / 4;
		share.blocks[1] = ( viewH + 3 ) / 4;
		share.blocks[2] = 0;
		share.blocks[3] = 0;
		nvrhi::ComputeState shareState;
		shareState.pipeline = m_SharePipeline;
		shareState.bindings = { m_ShareSet };
		commandList->setComputeState( shareState );
		commandList->setPushConstants( &share, sizeof( share ) );
		commandList->dispatch( ( share.size[0] + 7 ) / 8, ( share.size[1] + 7 ) / 8, 1 );
	}
	FftDispatch( commandList, 1, m_FftSets[1], pairs, classes );
	// the columns: PASS 3 for one class, PASS 6 for a split light ( WP-J27 )
	const int columnPass = ( classes > 1 ) ? 6 : 3;
	FftDispatch( commandList, columnPass, m_FftSets[columnPass], bins, 1 );
	FftDispatch( commandList, 5, m_FftSets[5], pairs, 1 );
	commandList->endMarker();

	// the re-entry over level 0's rectangle, ONE, ONE
	const nvrhi::Viewport viewport( 0.0f, ( float )levelW[0], 0.0f, ( float )levelH[0], 0.0f, 1.0f );
	nvrhi::GraphicsState state;
	state.indexBuffer = {};
	state.pipeline = m_FftReentryPipeline;
	state.framebuffer = m_Frames[0];
	state.bindings = { m_FftReentrySet };
	state.viewport.addViewportAndScissorRect( viewport );
	commandList->setGraphicsState( state );
	nvrhi::DrawArguments args;
	args.instanceCount = 1;
	args.vertexCount = 4;
	commandList->draw( args );
	if( backEnd.concurrencyStats )
	{
		backEnd.pc.passDraws[backEnd.currentPass]++;
	}
}

/*
========================
BloomPass::DumpCopy, DumpWrite

WP-J24's gates ( plan section 07: the result against a direct spatial convolution on the CPU of the field made from
the read-back level with the read-back kernel ). DumpCopy copies the level the rows read ( whole, in the chain's
format ), the kernel ( RGBA32F ) and the result ( RGBA16F ) into staging textures; DumpWrite, some bloom views later,
writes each as tightly packed rows to bloomfft_level.raw, bloomfft_kernel.raw and bloomfft_result.raw in the save
path, and the constants to bloomfft_dump.txt. tools/fft_check_*.py in the run folder does the rest
========================
*/
void BloomPass::DumpCopy( nvrhi::ICommandList* commandList, const BloomFftConstants& constants, int level, int levelW, int levelH )
{
	nvrhi::TextureDesc desc;
	desc.width = m_TexW[level];
	desc.height = m_TexH[level];
	desc.format = m_ChainFormat;
	desc.debugName = "Bloom/DumpLevel";
	m_DumpLevel = m_Device->createStagingTexture( desc, nvrhi::CpuAccessMode::Read );
	desc.width = m_FftW;
	desc.height = m_FftH;
	desc.format = nvrhi::Format::RGBA32_FLOAT;
	desc.debugName = "Bloom/DumpKernel";
	m_DumpKernel = m_Device->createStagingTexture( desc, nvrhi::CpuAccessMode::Read );
	desc.format = nvrhi::Format::RGBA16_FLOAT;
	desc.debugName = "Bloom/DumpResult";
	m_DumpResult = m_Device->createStagingTexture( desc, nvrhi::CpuAccessMode::Read );
	if( !m_DumpLevel || !m_DumpKernel || !m_DumpResult )
	{
		common->Printf( "bloomFftDump: the staging textures could not be made\n" );
		return;
	}
	commandList->copyTexture( m_DumpLevel, nvrhi::TextureSlice(), m_Levels[level], nvrhi::TextureSlice() );
	commandList->copyTexture( m_DumpKernel, nvrhi::TextureSlice(), m_FftKernel, nvrhi::TextureSlice() );
	commandList->copyTexture( m_DumpResult, nvrhi::TextureSlice(), m_FftResult, nvrhi::TextureSlice() );
	// WP-J27: a view split into classes: the shares the rows read and each class's kernel spectrum, from its slice
	m_DumpShares = nullptr;
	for( int c = 0; c < BLOOM_FFT_CLASSES; c++ )
	{
		m_DumpKernelSpec[c] = nullptr;
	}
	const int classes = idMath::ClampInt( 1, BLOOM_FFT_CLASSES, ( int )constants.classes[0] );
	if( classes > 1 && m_StyleShares )
	{
		desc.width = m_StyleShares->getDesc().width;
		desc.height = m_StyleShares->getDesc().height;
		desc.format = nvrhi::Format::RGBA16_FLOAT;
		desc.debugName = "Bloom/DumpShares";
		m_DumpShares = m_Device->createStagingTexture( desc, nvrhi::CpuAccessMode::Read );
		desc.width = m_FftW / 2 + 1;
		desc.height = m_FftH;
		desc.format = nvrhi::Format::RGBA32_FLOAT;
		desc.debugName = "Bloom/DumpKernelSpec";
		for( int c = 0; c < classes; c++ )
		{
			m_DumpKernelSpec[c] = m_Device->createStagingTexture( desc, nvrhi::CpuAccessMode::Read );
			nvrhi::TextureSlice slice;
			slice.arraySlice = ( nvrhi::ArraySlice )constants.classKernel[c];
			if( m_DumpKernelSpec[c] )
			{
				commandList->copyTexture( m_DumpKernelSpec[c], nvrhi::TextureSlice(), m_FftKernelSpec, slice );
			}
		}
		if( m_DumpShares )
		{
			commandList->copyTexture( m_DumpShares, nvrhi::TextureSlice(), m_StyleShares, nvrhi::TextureSlice() );
		}
	}
	m_DumpConstants = constants;
	m_DumpLevelIndex = level;
	m_DumpLevelRect[0] = levelW;
	m_DumpLevelRect[1] = levelH;
	m_DumpWait = BLOOM_PROBE_WAIT;
}

// one staging texture's rows, bytesPerTexel each texel, written tightly packed
static bool BloomDumpTexture( nvrhi::IDevice* device, nvrhi::IStagingTexture* staging, int width, int height, int bytesPerTexel, const char* name )
{
	size_t pitch = 0;
	const byte* data = ( const byte* )device->mapStagingTexture( staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &pitch );
	if( data == NULL )
	{
		return false;
	}
	idFile* file = fileSystem->OpenFileWrite( name );
	if( file != NULL )
	{
		for( int y = 0; y < height; y++ )
		{
			file->Write( data + y * pitch, width * bytesPerTexel );
		}
		fileSystem->CloseFile( file );
	}
	device->unmapStagingTexture( staging );
	return file != NULL;
}

void BloomPass::DumpWrite()
{
	const BloomFftConstants& f = m_DumpConstants;
	const nvrhi::TextureDesc& levelDesc = m_DumpLevel->getDesc();
	const bool r11 = ( levelDesc.format == nvrhi::Format::R11G11B10_FLOAT );
	bool ok = BloomDumpTexture( m_Device, m_DumpLevel, ( int )levelDesc.width, ( int )levelDesc.height, r11 ? 4 : 8, "bloomfft_level.raw" );
	ok = ok && BloomDumpTexture( m_Device, m_DumpKernel, m_FftW, m_FftH, 16, "bloomfft_kernel.raw" );
	ok = ok && BloomDumpTexture( m_Device, m_DumpResult, m_FftW, m_FftH, 8, "bloomfft_result.raw" );
	// WP-J27: a split view's shares and kernel spectra ( none for one class )
	const int classes = idMath::ClampInt( 1, BLOOM_FFT_CLASSES, ( int )f.classes[0] );
	if( m_DumpShares )
	{
		ok = ok && BloomDumpTexture( m_Device, m_DumpShares, ( int )m_DumpShares->getDesc().width, ( int )m_DumpShares->getDesc().height, 8, "bloomfft_shares.raw" );
	}
	for( int c = 0; c < classes; c++ )
	{
		if( m_DumpKernelSpec[c] )
		{
			ok = ok && BloomDumpTexture( m_Device, m_DumpKernelSpec[c], m_FftW / 2 + 1, m_FftH, 16, va( "bloomfft_kspec%d.raw", c ) );
		}
	}
	idFile* file = fileSystem->OpenFileWrite( "bloomfft_dump.txt" );
	if( file != NULL )
	{
		file->Printf( "level %d texture %u %u rect %d %d format %s\n", m_DumpLevelIndex, levelDesc.width, levelDesc.height, m_DumpLevelRect[0], m_DumpLevelRect[1], r11 ? "R11G11B10F" : "RGBA16F" );
		file->Printf( "field %u %u picture %u %u\n", f.field[0], f.field[1], f.image[0], f.image[1] );
		file->Printf( "source %.9g %.9g\n", f.source.x, f.source.y );
		file->Printf( "shape %.9g %.9g %.9g %.9g\n", f.shape.x, f.shape.y, f.shape.z, f.shape.w );
		file->Printf( "kernelA %.9g %.9g %.9g %.9g\n", f.kernelA.x, f.kernelA.y, f.kernelA.z, f.kernelA.w );
		file->Printf( "kernelB %.9g %.9g %.9g %.9g\n", f.kernelB.x, f.kernelB.y, f.kernelB.z, f.kernelB.w );
		file->Printf( "kernelC %.9g %.9g %.9g %.9g\n", f.kernelC.x, f.kernelC.y, f.kernelC.z, f.kernelC.w );
		file->Printf( "norms %.9g %.9g %.9g %.9g\n", f.norms.x, f.norms.y, f.norms.z, f.norms.w );
		// WP-J27: more than one class is no single convolution: fft_check's gates hold for classes 1, fft_split_check's
		// for more ( the shares' texture size, each class's kernel slice and its mask of the four slots )
		file->Printf( "classes %u kernels %u %u %u %u shares %u %u\n", f.classes[0], f.classKernel[0], f.classKernel[1], f.classKernel[2], f.classKernel[3],
					  m_DumpShares ? m_DumpShares->getDesc().width : 0, m_DumpShares ? m_DumpShares->getDesc().height : 0 );
		for( int c = 0; c < BLOOM_FFT_CLASSES; c++ )
		{
			file->Printf( "classmask %d %.9g %.9g %.9g %.9g\n", c, f.classMask[c].x, f.classMask[c].y, f.classMask[c].z, f.classMask[c].w );
		}
		for( int i = 0; i < BLOOM_FFT_WAVES; i++ )
		{
			file->Printf( "wave %d %.9g %.9g %.9g %.9g\n", i, f.waves[i].x, f.waves[i].y, f.waves[i].z, f.waves[i].w );
		}
		fileSystem->CloseFile( file );
	}
	common->Printf( "bloomFftDump: %s\n", ( ok && file != NULL ) ? "written" : "failed" );
	m_DumpLevel = nullptr;
	m_DumpKernel = nullptr;
	m_DumpResult = nullptr;
	m_DumpShares = nullptr;
	for( int c = 0; c < BLOOM_FFT_CLASSES; c++ )
	{
		m_DumpKernelSpec[c] = nullptr;
	}
}

/*
========================
BloomPass::Render

Builds the chain of the view, in the backend's MRB_TONE_MAP_PASS block: the frame's view rectangle down to level 0
and from there down to the last level, then the levels up and added back, and the mix the tonemap draw reads. Under
the gate ( WP-J21 ) the first pass reads the gate's output instead of the frame; under the convolution ( WP-J24 ) the
levels below the one it reads are not drawn and its re-entry takes the up passes' place. The block and the marker
wrap steps 2 to 7 and open only after the chain stands; no path leaves them open.
========================
*/
const BloomMix* BloomPass::Render( nvrhi::ICommandList* commandList, const viewDef_t* viewDef, nvrhi::ITexture* sourceTexture, nvrhi::IBuffer* exposureBuffer )
{
	if( m_Dead )
	{
		return NULL;
	}

	// a probe's copies are read some bloom views after they were made
	if( m_ProbeWait > 0 && --m_ProbeWait == 0 )
	{
		ProbeRead();
	}
	if( m_DumpWait > 0 && --m_DumpWait == 0 )
	{
		DumpWrite();
	}

	// 1: the view rectangle, the level count, and the chain for the frame texture's size
	const int vx = viewDef->viewport.x1;
	const int vy = viewDef->viewport.y1;
	const int vw = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	const int vh = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;

	int levelW[ BLOOM_MAX_LEVELS ];
	int levelH[ BLOOM_MAX_LEVELS ];
	const int n = BloomLevelCount( vw, vh, levelW, levelH );

	const nvrhi::TextureDesc& frameDesc = sourceTexture->getDesc();
	if( !CheckChain( ( int )frameDesc.width, ( int )frameDesc.height ) )
	{
		return NULL;
	}
	if( !CheckFirstSet( sourceTexture, exposureBuffer ) )
	{
		return NULL;
	}

	// A chain that was just made is cleared once, before any draw touches it: the texels of a level outside its
	// rectangle are never drawn into, yet a bilinear tap on the rectangle's last texel centre also reads the texel
	// beyond it ( at weight 0 ) - and a NaN pattern times 0 is a NaN. Zero makes what is stale at least finite
	if( m_ChainFresh )
	{
		for( int i = 0; i < BLOOM_MAX_LEVELS; i++ )
		{
			commandList->clearTextureFloat( m_Levels[i], nvrhi::AllSubresources, nvrhi::Color( 0.0f ) );
		}
		if( m_Wide )
		{
			commandList->clearTextureFloat( m_Wide, nvrhi::AllSubresources, nvrhi::Color( 0.0f ) );
		}
		m_ChainFresh = false;
	}

	// the cvars, read once for the whole pass. A debug value outside the table behaves as 0
	int debug = r_bloomDebug.GetInteger();
	if( debug < 0 || debug > 14 )
	{
		debug = 0;
	}
	// ( a NaN: nothing added, the default tail and ceiling; the knee's own test in the shader, k > 0, sends a
	// NaN to "off" )
	const float tail = BloomCvarFloat( r_bloomGlowTail, 0.25f, 1.0f, 0.34f );
	const float knee = r_bloomFireflyKnee.GetFloat();
	const float ceiling = BloomCvarFloat( r_bloomCeiling, 1.0f, BLOOM_CEILING_MAX, 256.0f );
	// w0: the level 0 weight that makes the whole tail weigh s: ( 1 - tail ) / ( 1 - tail^N ), and 1 / N where the
	// tail is 1 ( the geometric sum's limit )
	const float w0 = ( tail >= 0.9999f ) ? 1.0f / n : ( 1.0f - tail ) / ( 1.0f - powf( tail, n ) );
	const float exposureScale = ::exp2f( r_exposure.GetFloat() );

	// the glow's brightness, and the threshold the first pass reads
	const float glowGain = BloomCvarFloat( r_bloomGlow, 0.0f, 4.0f, 0.0f );
	const float threshold = BloomCvarFloat( r_bloomThreshold, 0.01f, 1024.0f, 1.5f );
	const float thresholdKnee = threshold * BloomCvarFloat( r_bloomThresholdKnee, 0.0f, 1.0f, 0.5f );

	// WP-J24, the owner's Log Exposure Bias, in both forms: a gain g = 2^bias on what passes the gate, before the ceiling
	// and the threshold. By the constants ( plan section 07 ): the first pass's threshold, its knee, the firefly knee and
	// the ceiling are each divided by g and the mix's weight is multiplied by g, which is exactly the biased input ( each
	// is homogeneous in the gain ), with no shader change; at bias 0 g is exactly 1 and every constant is today's. The
	// ceiling sent is held to the chain's finite bound
	const float biasGain = ::exp2f( BloomCvarFloat( r_bloomExposureBias, -5.0f, 5.0f, 0.0f ) );
	const float firstThreshold = threshold / biasGain;
	const float firstThresholdKnee = thresholdKnee / biasGain;
	const float firstKnee = knee / biasGain;
	const float firstCeiling = Min( ceiling / biasGain, BLOOM_CEILING_MAX );
	// WP-J27, the hot-pixel boost: the view's stops and the hot level ( exposed units, as the threshold )
	const float hotBoostView = BloomCvarFloat( r_bloomHotBoost, 0.0f, 8.0f, 3.0f );
	const float hotLevelSet = BloomCvarFloat( r_bloomHotLevel, 1.0f, 256.0f, 2.0f );

	// WP-J21, the gate. r_bloomSource 1: only the light the emitting stages drew into the glow mask passes ( bloom_gate,
	// then FIRST 3 or 4 ); 2: the mask is drawn as under 1 and the bloom reads the frame ( a developer's comparison ); 0:
	// the open gate of v0001.3v. Where the stage loops of this view wrote no mask ( MSAA, no _hdrGlow ) or the gate
	// cannot run, the view takes the open gate. A view under the gate in which no stage emitted adds nothing
	int source = idMath::ClampInt( 0, 2, r_bloomSource.GetInteger() );
	if( source >= 1 && !backEnd.glowGateView )
	{
		source = 0;
	}
	const bool maskWritten = backEnd.glowMaskWritten;

	// with nothing to add no chain is drawn and the tonemap pass takes its plain draw, so the bar's lowest notch draws
	// nothing ( the chain stays allocated ). A probe and a debug view still get their chain
	if( debug == 0 && !bloomProbeRequested && glowGain <= 0.0f )
	{
		return NULL;
	}

	// The first pass that reads the frame's depth serves two things: the distance tail ( WP-G3; it needs a second
	// level to narrow ) and the view weapon left out of the glow ( WP-G4 ). It is drawn when either is wanted, its
	// pipeline is made, and this frame's depth is there at the frame's own size ( the Hi-Z image's mip 0, filled for
	// every 3D view while R_UseHiZ ). Without any of them the first pass is the one without a depth: the tail is the
	// same at every distance and the view weapon glows like the world. The depth is fetched before the gate's
	// decision, wherever the tail or the weapon may want it, so the gate's depth set can be made ( review R1-3 )
	const bool wantTailDistance = r_bloomTailDistance.GetBool() && n >= 2;
	nvrhi::ITexture* depthTexture = NULL;
	if( ( wantTailDistance || r_bloomViewWeapon.GetInteger() != 1 ) && R_UseHiZ() )
	{
		depthTexture = ( globalImages->hierarchicalZbufferImage != NULL ) ? globalImages->hierarchicalZbufferImage->GetTextureHandle().Get() : NULL;
		if( depthTexture != NULL && ( depthTexture->getDesc().width != frameDesc.width || depthTexture->getDesc().height != frameDesc.height ) )
		{
			depthTexture = NULL;
		}
	}
	// the gate's own fallback comes before anything decided on the source: a view whose gate cannot run takes the open
	// gate whole, its chain and its weapon rule ( review R1-3 )
	bool gated = ( source == 1 );
	if( gated && !( GateInit() && GateCheckSets( sourceTexture, exposureBuffer, depthTexture ) ) )
	{
		gated = false;
		source = 0;
	}
	const bool fogGlowView = gated && backEnd.froxelGlowView && r_bloomFogGlow.GetBool();
	if( debug == 0 && !bloomProbeRequested && gated && !maskWritten && !fogGlowView )
	{
		return NULL;
	}
	// WP-J21: r_bloomViewWeapon -1 leaves the weapon out except under the gate ( 0 and 2 read the frame and leave it
	// out alike, review R1-3 ); under the gate its emissive parts pass and the lamp-lit weapon writes no mask
	int weaponMode = r_bloomViewWeapon.GetInteger();
	if( weaponMode < 0 || weaponMode > 1 )
	{
		weaponMode = gated ? 1 : 0;
	}
	const bool wantWeaponOut = weaponMode == 0;
	// WP-J21, the fog's glow: level 1's reduction adds the light this view's froxel fog scatters from lights ( FIRST 5 )
	const bool fogGlow = fogGlowView && n >= 2 && R_UseHiZ() && FogCheckSet( sourceTexture, exposureBuffer );
	bool depthPass = false;
	if( depthTexture != NULL && ( wantTailDistance || wantWeaponOut ) )
	{
		depthPass = gated ? ( m_DownGatedDepthPipeline && m_GatedDepthSet )
					: ( m_DownFirstDepthPipeline && CheckFirstDepthSet( sourceTexture, exposureBuffer, depthTexture ) );
	}
	const bool tailDistance = depthPass && wantTailDistance;
	const bool weaponOut = depthPass && wantWeaponOut;
	// a NaN typed into a cvar fails every comparison, so each value is taken only if it passes its own test
	float tailNear = r_bloomTailNear.GetFloat();
	if( !( tailNear >= 0.0f ) )
	{
		tailNear = 0.0f;
	}
	float tailFar = r_bloomTailFar.GetFloat();
	if( !( tailFar >= tailNear + 1.0f ) )
	{
		tailFar = tailNear + 1.0f;
	}
	float tailFarShare = r_bloomTailFarShare.GetFloat();
	if( !( tailFarShare >= 0.0f ) )
	{
		tailFarShare = 0.0f;
	}
	if( tailFarShare > 1.0f )
	{
		tailFarShare = 1.0f;
	}

	// WP-J27: the sources' own blooms in this view: the style slots its stage loops drew with ( under the gate, where an
	// emitting stage wrote the mask; backEnd.GlowStyleSlot ), and whether any asks for a kernel of its own
	bool slotIn[ BLOOM_STYLE_SLOTS ];
	slotIn[0] = true;
	int slotsInView = 0;
	bool styleKernels = false;
	for( int i = 1; i < BLOOM_STYLE_SLOTS; i++ )
	{
		slotIn[i] = gated && maskWritten && backEnd.glowStyleSerial > 0 && backEnd.glowStyleView[i] == backEnd.glowStyleSerial;
		if( slotIn[i] )
		{
			slotsInView++;
			styleKernels |= ( backEnd.glowStyles[i].shape != BLOOM_STYLE_VIEW );
		}
	}

	// WP-J24: the convolution, where it is asked for and can run: not in a view that looks at one level alone, nor in a
	// view too small for its field or its chain ( it reads level 1 or deeper ). The field is W x H cells by the quality;
	// the view is laid into it at k_s cells a pixel with the padding's share left as black margin on the tighter axis;
	// the margin is the kernel's reach. Where any part is missing the view takes the chain
	bool fft = false;
	int fftLevel = 1;
	BloomFftConstants fftConstants;
	memset( &fftConstants, 0, sizeof( fftConstants ) );
	BloomFftConstants classKernels[ BLOOM_FFT_CLASSES ];		// WP-J27: each class's kernel constants ( [0] the view's )
	memset( classKernels, 0, sizeof( classKernels ) );
	if( ( debug < 2 || debug > 9 ) && n >= 2 && r_bloomFFT.GetBool() && FftInit() )
	{
		const int quality = idMath::ClampInt( 0, 2, r_bloomFFTQuality.GetInteger() );
		const int fieldW = 256 << quality;
		const int fieldH = fieldW / 2;
		const float padding = BloomCvarFloat( r_bloomFFTPadding, 0.0f, 0.5f, 0.25f );
		const float ks = ( 1.0f - padding ) * Min( ( float )fieldH / ( float )vh, ( float )fieldW / ( float )vw );
		const int gw = Min( ( int )ceilf( ks * ( float )vw ), fieldW );
		const int gh = Min( ( int )ceilf( ks * ( float )vh ), fieldH );
		// the reach in cells: the margin, at least the centre cell ( Padding 0: the shaped glow is its centre alone )
		const int reach = Max( 1, Min( fieldH - gh, fieldW - gw ) );
		// the deepest level l >= 1 of which a cell spans at least one texel ( level i's texel is 2^( i + 1 ) pixels );
		// at Quality 2 a cell is under one level-1 texel and level 1 is read. Never level 0: it is the core, and the
		// distance tail and the fog's glow enter the wide levels
		while( fftLevel + 1 < n && 1.0f / ( ks * ( float )( 2 << ( fftLevel + 1 ) ) ) >= 1.0f )
		{
			fftLevel++;
		}
		// PASS 1's tent reads at most 7 texels an axis, which holds a step s of up to 3 texels a cell; a deeper step
		// ( a chain cut short by r_bloomLevels on a tall view ) would lose light, so the view keeps the chain ( review
		// R1-13 )
		const bool tentHolds = 1.0f / ( ks * ( float )( 2 << fftLevel ) ) <= 3.0f;
		if( tentHolds && gw >= BLOOM_FFT_MIN_IMAGE && gh >= BLOOM_FFT_MIN_IMAGE && FftCheckField( fieldW, fieldH, styleKernels ? BLOOM_FFT_CLASSES : 1 )
				&& FftCheckSourceSet( m_Levels[fftLevel] ) && FftCheckReentry() )
		{
			fft = true;
			const float s = 1.0f / ( ks * ( float )( 2 << fftLevel ) );
			const float m = ( float )reach / ( ks * ( float )vh );		// the reach in view heights
			fftConstants.field[0] = fieldW;
			fftConstants.field[1] = fieldH;
			fftConstants.field[2] = 8 + quality;
			fftConstants.field[3] = 7 + quality;
			fftConstants.image[0] = gw;
			fftConstants.image[1] = gh;
			fftConstants.image[2] = levelW[fftLevel];
			fftConstants.image[3] = levelH[fftLevel];
			fftConstants.source.Set( s, 1.0f / ( s * s ), 0.0f, 0.0f );
			fftConstants.shape.Set( ( float )reach, ( 1.0f - w0 ) / w0, ks, 0.0f );
			// the view's kernel ( class 0 ), from the System Options rows
			BloomKernelValues viewValues;
			BloomKernelValuesFromCvars( viewValues );
			BloomFftSetKernel( fftConstants, viewValues, m, ks * ( float )vh );
			fftConstants.classes[0] = 1;
			fftConstants.classMask[0].Set( 1.0f, 1.0f, 1.0f, 1.0f );

			// WP-J27: a class for each kernel of the sources' own in view. A slot of the view's shape, and a slot whose
			// kernel is a class's already, joins that class; each class's mask names its slots, so the classes' shares sum
			// to the whole light. The share pass's set must stand ( the convolution's layouts made it )
			classKernels[0] = fftConstants;
			if( styleKernels && m_ShareSet && m_FftSlices >= BLOOM_FFT_CLASSES )
			{
				int classes = 1;
				idVec4 masks[ BLOOM_FFT_CLASSES ];
				for( int c = 0; c < BLOOM_FFT_CLASSES; c++ )
				{
					masks[c].Zero();
				}
				masks[0].x = 1.0f;
				for( int i = 1; i < BLOOM_STYLE_SLOTS; i++ )
				{
					if( !slotIn[i] )
					{
						continue;
					}
					if( backEnd.glowStyles[i].shape == BLOOM_STYLE_VIEW )
					{
						masks[0][i] = 1.0f;
						continue;
					}
					BloomKernelValues styleValues;
					BloomKernelValuesFromStyle( backEnd.glowStyles[i], styleValues );
					BloomFftConstants styleKernel = fftConstants;
					BloomFftSetKernel( styleKernel, styleValues, m, ks * ( float )vh );
					int c = 0;
					while( c < classes && !BloomFftKernelKeyEquals( styleKernel, classKernels[c] ) )
					{
						c++;
					}
					if( c == classes )
					{
						classKernels[classes++] = styleKernel;
					}
					masks[c][i] = 1.0f;
				}
				// review R2-F2: the view's class takes every slot no other class names, a slot not in view among them, so
				// the classes' shares always sum to 1 ( light the gate reads with such a slot is the view's )
				for( int i = 0; i < BLOOM_STYLE_SLOTS; i++ )
				{
					float named = 0.0f;
					for( int c = 1; c < BLOOM_FFT_CLASSES; c++ )
					{
						named += masks[c][i];
					}
					masks[0][i] = 1.0f - named;
				}
				if( classes > 1 )
				{
					// the view's kernel keeps slice 0; each other class the slice that holds its kernel ( every class matched
					// first, so a new kernel cannot take a slice a later class would have kept: review R2-N5 ), else a free one
					int taken[ BLOOM_FFT_CLASSES ] = { 1, 0, 0, 0 };
					int sliceOf[ BLOOM_FFT_CLASSES ] = { 0, -1, -1, -1 };
					for( int c = 1; c < classes; c++ )
					{
						for( int s = 1; s < BLOOM_FFT_CLASSES; s++ )
						{
							if( !taken[s] && m_FftKernelValid[s] && BloomFftKernelKeyEquals( classKernels[c], m_FftKernelKey[s] ) )
							{
								sliceOf[c] = s;
								taken[s] = 1;
								break;
							}
						}
					}
					for( int c = 1; c < classes; c++ )
					{
						if( sliceOf[c] < 0 )
						{
							sliceOf[c] = FftKernelSlice( classKernels[c], c, taken );
							taken[ sliceOf[c] ] = 1;
						}
						fftConstants.classKernel[c] = ( uint )sliceOf[c];
					}
					fftConstants.classes[0] = ( uint )classes;
					for( int c = 0; c < BLOOM_FFT_CLASSES; c++ )
					{
						fftConstants.classMask[c] = masks[c];
					}
				}
			}
		}
	}

	renderLog.OpenMainBlock( MRB_BLOOM );
	commandList->beginMarker( "Bloom" );

	// 2: the debug poke into the frame, before the first down pass
	if( debug >= 11 && debug <= 13 )
	{
		PokeFrame( commandList, sourceTexture, vx + vw / 2, vy + vh / 2, debug );
	}

	// 2b ( WP-J21 ): the gate, from the frame and the glow mask, into the gated light and its tiles' flags. A probe or
	// a debug view under the gate in a view no stage emitted in reads a mask cleared here
	bool boostWritten = false;		// WP-J27: a slot's boost was above 0, so the gate's alpha holds boosts
	if( gated )
	{
		if( !maskWritten )
		{
			commandList->clearTextureFloat( globalImages->glowMaskImage->GetTextureHandle(), nvrhi::AllSubresources, nvrhi::Color( 0.0f ) );
		}
		BloomGateConstants gateConstants;
		gateConstants.rect[0] = vx;
		gateConstants.rect[1] = vy;
		gateConstants.rect[2] = vw;
		gateConstants.rect[3] = vh;
		// WP-J27: the hot level and the threshold as the first pass compares them ( exposed, the bias divided out ), the level
		// above the threshold; the blocks' slot sums where the convolution splits the light
		const float hotLevel = Max( hotLevelSet / biasGain, firstThreshold * 1.01f );
		gateConstants.params.Set( BLOOM_GATE_CAP, ( fft && fftConstants.classes[0] > 1 ) ? 1.0f : 0.0f, hotLevel, firstThreshold );
		gateConstants.expo.Set( exposureScale, r_hdrMinLuminance.GetFloat(), 0.0f, 0.0f );
		// each slot's strength, and its boost under the convolution ( the view's, or the source's own )
		gateConstants.slotGain.Set( 1.0f, 1.0f, 1.0f, 1.0f );
		// every slot as the view's to start with: a pixel the gate reads with a slot not in this view takes the view's ( R2-F2 )
		gateConstants.slotBoost.Set( fft ? hotBoostView : 0.0f, fft ? hotBoostView : 0.0f, fft ? hotBoostView : 0.0f, fft ? hotBoostView : 0.0f );
		for( int i = 1; i < BLOOM_STYLE_SLOTS; i++ )
		{
			if( slotIn[i] )
			{
				const renderBloomStyle_t& style = backEnd.glowStyles[i];
				gateConstants.slotGain[i] = BloomStyleValue( 1.0f + style.intensityBias, 0.0f, 4.0f, 1.0f );
				gateConstants.slotBoost[i] = !fft ? 0.0f : style.hotBoostOwn ? BloomStyleValue( style.hotBoost, 0.0f, 8.0f, 0.0f ) : hotBoostView;
			}
		}
		for( int i = 0; i < BLOOM_STYLE_SLOTS; i++ )
		{
			boostWritten |= ( gateConstants.slotBoost[i] > 0.0f );
		}
		nvrhi::ComputeState gateState;
		gateState.pipeline = ( slotsInView > 0 ) ? m_GateStylesPipeline : m_GatePipeline;		// WP-J27
		gateState.bindings = { m_GateSet };
		commandList->setComputeState( gateState );
		commandList->setPushConstants( &gateConstants, sizeof( gateConstants ) );
		commandList->dispatch( ( vw + 15 ) / 16, ( vh + 15 ) / 16, 1 );
	}

	// 3: the first down pass, the frame's view rectangle ( or the gated light ) to level 0: the scrub and the firefly
	// knee act there, by the constants the bias divided
	BloomConstants constants;
	constants.srcRect.Set( ( float )vx, ( float )vy, ( float )vw, ( float )vh );
	constants.srcInvSize.Set( 1.0f / ( float )frameDesc.width, 1.0f / ( float )frameDesc.height, firstThresholdKnee, firstKnee );
	constants.exposure.Set( exposureScale, r_hdrMinLuminance.GetFloat(), firstCeiling, firstThreshold );
	constants.tailDepth.Zero();
	constants.tailDist.Zero();
	// WP-J27: the gated first pass applies the boost the gate wrote only where the gate wrote one
	const float boostFlag = ( gated && boostWritten ) ? 1.0f : 0.0f;
	if( depthPass )
	{
		// the froxel fog's pair ( VolumetricFogPass: depthParms.zw ): a view distance is x / ( depth + y ). The view
		// weapon's test is depth * w > z; with z 1 and w 0 no pixel passes it and the weapon glows like the world
		constants.tailDepth.Set( viewDef->projectionMatrix[3 * 4 + 2], viewDef->projectionMatrix[2 * 4 + 2],
								   weaponOut ? BLOOM_WEAPON_DEPTH_BOUND : 1.0f, weaponOut ? BLOOM_WEAPON_DEPTH_SIGN : 0.0f );
		if( tailDistance )
		{
			constants.tailDist.Set( tailFar, 1.0f / ( tailFar - tailNear ), tailFarShare, boostFlag );
		}
		else
		{
			// no distance tail: a far share of 1 makes every pixel's nearness exactly 1, whatever its distance
			constants.tailDist.Set( 0.0f, 0.0f, 1.0f, boostFlag );
		}
		if( gated )
		{
			DrawLevel( commandList, m_DownGatedDepthPipeline, m_GatedDepthSet, m_FirstFrame, levelW[0], levelH[0], constants );
		}
		else
		{
			DrawLevel( commandList, m_DownFirstDepthPipeline, m_FirstDepthSet, m_FirstFrame, levelW[0], levelH[0], constants );
		}
		constants.tailDepth.Zero();
		constants.tailDist.Zero();
	}
	else if( gated )
	{
		constants.tailDist.w = boostFlag;
		DrawLevel( commandList, m_DownGatedPipeline, m_GatedSet, m_FirstFrame, levelW[0], levelH[0], constants );
		constants.tailDist.w = 0.0f;
	}
	else
	{
		// the thirteen taps from thirty-six frame pixels thresholded one by one, into level 0 and the wide levels' source
		DrawLevel( commandList, m_DownFirstPipeline, m_FirstSet, m_FirstFrame, levelW[0], levelH[0], constants );
	}

	// 4: the down passes, level i - 1 to level i; under the convolution only down to the level it reads
	const int nDown = fft ? fftLevel + 1 : n;
	for( int i = 1; i < nDown; i++ )
	{
		constants.srcRect.Set( 0.0f, 0.0f, ( float )levelW[i - 1], ( float )levelH[i - 1] );
		constants.srcInvSize.Set( 1.0f / ( float )m_TexW[i - 1], 1.0f / ( float )m_TexH[i - 1], 0.0f, 0.0f );
		constants.exposure.Zero();
		// level 1 is reduced from the first pass's second target, the wide levels' source; under the gate with fog, with the
		// fog's glow added ( WP-J21, FIRST 5: today's threshold by the bias's divided constants, the frame's origin and
		// 1 / its size, the factor )
		if( i == 1 && fogGlow )
		{
			BloomConstants fogConstants = constants;
			fogConstants.srcInvSize.z = firstThresholdKnee;
			fogConstants.exposure.Set( exposureScale, r_hdrMinLuminance.GetFloat(), firstCeiling, firstThreshold );
			fogConstants.tailDepth.Set( ( float )vx, ( float )vy, 1.0f / ( float )frameDesc.width, 1.0f / ( float )frameDesc.height );
			fogConstants.tailDist.Set( 1.0f, 0.0f, 0.0f, 0.0f );
			DrawLevel( commandList, m_DownFogPipeline, m_FogSet, m_Frames[1], levelW[1], levelH[1], fogConstants );
			continue;
		}
		DrawLevel( commandList, m_DownPipeline, ( i == 1 ) ? m_DownWideSet : m_DownSets[i], m_Frames[i], levelW[i], levelH[i], constants );
	}

	// 5: the probe's copies of the levels as the down passes left them
	bool probeOn = false;
	if( bloomProbeRequested && m_ProbeWait == 0 )
	{
		bloomProbeRequested = false;
		m_ProbeView[0] = vx;
		m_ProbeView[1] = vy;
		m_ProbeView[2] = vw;
		m_ProbeView[3] = vh;
		m_ProbeLevels = nDown;
		m_ProbeTail = tail;
		m_ProbeW0 = w0;
		m_ProbeExposureScale = exposureScale;
		m_ProbeKnee = firstKnee;
		m_ProbeCeiling = firstCeiling;
		m_ProbeDebug = debug;
		m_ProbeGlowGain = glowGain * biasGain;
		m_ProbeThreshold = firstThreshold;
		m_ProbeThresholdKnee = firstThresholdKnee;
		m_ProbeTailDistance = tailDistance ? 1 : 0;
		m_ProbeWeaponOut = weaponOut ? 1 : 0;
		m_ProbeTailNear = tailNear;
		m_ProbeTailFar = tailFar;
		m_ProbeTailFarShare = tailFarShare;
		m_ProbeFrame[0] = ( int )frameDesc.width;
		m_ProbeFrame[1] = ( int )frameDesc.height;
		m_ProbeFft = fft ? 1 : 0;
		m_ProbeFftConstants = fftConstants;
		m_ProbeFftLevel = fftLevel;
		m_ProbeFftQuality = idMath::ClampInt( 0, 2, r_bloomFFTQuality.GetInteger() );
		m_ProbeClasses = fft ? ( int )fftConstants.classes[0] : 0;
		m_ProbeSlots = slotsInView;
		m_ProbeOverflow = gated ? backEnd.glowStyleOverflow : 0;
		m_ProbeHotBoost = ( fft && gated ) ? hotBoostView : 0.0f;
		m_ProbeHotLevel = hotLevelSet;
		m_ProbeSource = source;
		m_ProbeMaskWritten = maskWritten ? 1 : 0;
		m_ProbeFogGlow = fogGlow ? 1 : 0;
		m_ProbeBias = biasGain;
		memcpy( m_ProbeLevelW, levelW, nDown * sizeof( int ) );
		memcpy( m_ProbeLevelH, levelH, nDown * sizeof( int ) );
		probeOn = ProbeCopy( commandList, sourceTexture, nDown );
	}

	// 6: the up passes, level i + 1 into level i, unless the view is looking at one level alone; or the convolution
	// ( WP-J24 ), whose re-entry adds the shaped glow into level 0
	if( fft )
	{
		// a dump reads the kernel texture, which holds the last kernel made: the view's is made again for it ( the dump's
		// checks hold for a view of one class )
		if( bloomFftDumpRequested && m_DumpWait == 0 )
		{
			m_FftKernelValid[0] = false;
		}
		FftRender( commandList, levelW, levelH, fftConstants, classKernels, fftLevel, vw, vh );
		if( bloomFftDumpRequested && m_DumpWait == 0 )
		{
			bloomFftDumpRequested = false;
			DumpCopy( commandList, fftConstants, fftLevel, levelW[fftLevel], levelH[fftLevel] );
		}
		if( probeOn )
		{
			if( !m_ProbeFftStaging )
			{
				nvrhi::TextureDesc desc;
				desc.width = m_FftW;
				desc.height = m_FftH;
				desc.format = nvrhi::Format::RGBA16_FLOAT;
				desc.debugName = "Bloom/ProbeFftStaging";
				m_ProbeFftStaging = m_Device->createStagingTexture( desc, nvrhi::CpuAccessMode::Read );
			}
			if( m_ProbeFftStaging )
			{
				commandList->copyTexture( m_ProbeFftStaging, nvrhi::TextureSlice(), m_FftResult, nvrhi::TextureSlice() );
			}
		}
	}
	else if( debug < 2 || debug > 9 )
	{
		for( int i = n - 2; i >= 0; i-- )
		{
			constants.srcRect.Set( 0.0f, 0.0f, ( float )levelW[i + 1], ( float )levelH[i + 1] );
			constants.srcInvSize.Set( 1.0f / ( float )m_TexW[i + 1], 1.0f / ( float )m_TexH[i + 1], tail, 0.0f );
			DrawLevel( commandList, m_UpPipeline, m_UpSets[i], m_Frames[i], levelW[i], levelH[i], constants );
		}
	}
	if( probeOn )
	{
		ProbeStageUp( commandList );
	}

	// 7: what the tonemap draw needs to mix the chain in
	int level = 0;
	// the frame stays whole and the chain is added to it, times the bias's gain ( 1 at bias 0 )
	float frameWeight = 1.0f;
	float bloomWeight = glowGain * w0 * biasGain;
	float oneTap = 0.0f;
	if( debug == 1 )
	{
		frameWeight = 0.0f;
		bloomWeight = w0 * biasGain;
	}
	else if( debug >= 2 && debug <= 9 )
	{
		level = Min( debug - 2, n - 1 );
		frameWeight = 0.0f;
		bloomWeight = 1.0f;
	}
	else if( debug == 10 )
	{
		oneTap = 1.0f;
	}
	if( probeOn )
	{
		// the weights this frame's mix was sent, for the probe's own line
		m_ProbeMixFrame = frameWeight;
		m_ProbeMixBloom = bloomWeight;
	}
	m_Mix.texture = m_Levels[level];
	m_Mix.chainSerial = bloomChainSerial;
	m_Mix.mix.Set( frameWeight, bloomWeight, ( float )level, oneTap );
	m_Mix.rect.Set( ( float )vx, ( float )vy, ( float )levelW[level], ( float )levelH[level] );
	m_Mix.scale.Set( 0.5f / ( float )( 1 << level ), 0.0f, 1.0f / ( float )m_TexW[level], 1.0f / ( float )m_TexH[level] );
	if( debug == 14 && fft )
	{
		// WP-J24: the convolution's result alone, _bloomFFTOut, in the picture's cells at k_s cells a pixel
		m_Mix.texture = m_FftResult;
		m_Mix.mix.Set( 0.0f, 1.0f, 0.0f, 0.0f );
		m_Mix.rect.Set( ( float )vx, ( float )vy, ( float )fftConstants.image[0], ( float )fftConstants.image[1] );
		m_Mix.scale.Set( fftConstants.shape.z, 0.0f, 1.0f / ( float )m_FftW, 1.0f / ( float )m_FftH );
	}

	commandList->endMarker();
	renderLog.CloseMainBlock( MRB_BLOOM );
	return &m_Mix;
}
