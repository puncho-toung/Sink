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

// r_bloom, the strength and the tail are a player's settings and are archived; the levels, the knee, the ceiling
// and the debug value are a developer's tunables and are not, because an archived cvar is written to the config at
// the first run, and a later change of its default then never reaches that machine. The strength and the tail have
// external linkage for the System Options screen's two bars ( MenuScreen_Shell_SystemOptions.cpp )
idCVar r_bloom( "r_bloom", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE | CVAR_NEW, "bloom: a chain of half-size levels of the HDR frame, mixed in by the tonemapper ( main view only )" );
idCVar r_bloomStrength( "r_bloomStrength", "0.06", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE | CVAR_NEW, "bloom: the share of the picture that is its blur ( 0 = none, 1 = only the blur )", 0.0f, 1.0f );
idCVar r_bloomTail( "r_bloomTail", "0.7", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE | CVAR_NEW, "bloom, mode 0: each level's weight against the one above; 1 = a wide inverse-square halo, 0.5 = a tight inverse-cube one ( the glow's is r_bloomGlowTail )", 0.25f, 1.0f );
static idCVar r_bloomLevels( "r_bloomLevels", "6", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "bloom: levels at 1080 lines; one more for each doubling of the view's height", 1, 9 );
static idCVar r_bloomFireflyKnee( "r_bloomFireflyKnee", "256", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "bloom: a lone very bright pixel adds at most about a third of this ( in exposed units ) to a texel of the bloom's first level; 0 = off", 0.0f, 1024.0f );
static idCVar r_bloomCeiling( "r_bloomCeiling", "256", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "bloom: the frame's values are held to this before they enter the bloom ( the brightest emitter is 64 ); 7000 = no ceiling but the bound that keeps the chain finite", 1.0f, 7000.0f );
// WP-G3 ( the owner, 2026-11-25: "is there a way to get nice bloom/glow without blurring? I like how crisp the base
// game is" ): the glow mode. Only what a pixel has ABOVE a brightness threshold enters the chain, and the chain is ADDED
// to the frame, whose own weight stays 1: a pixel no glow reaches is the pixel of r_bloom 0, and no edge loses
// contrast. r_bloomGlow and r_bloomGlowTail are a player's settings and archived; the mode, the threshold and its
// knee are a developer's tunables and are not. The glow has its OWN tail ( review F1-5 ): while r_bloomTail served
// both modes, a config saved by v0001.3r ( 0.7, the mix's ) set the glow's tail too. r_bloomMode, r_bloomGlow and
// r_bloomGlowTail have external linkage for the System Options screen's two bars
idCVar r_bloomMode( "r_bloomMode", "1", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "bloom: 0 = the mix ( a share of the picture is its blur ), 1 = the glow ( what is brighter than r_bloomThreshold glows, added on top of the untouched picture )", 0, 1 );
idCVar r_bloomGlow( "r_bloomGlow", "0.24", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE | CVAR_NEW, "bloom, mode 1: the glow's brightness ( 0 = none; the System Options bar runs from 0 to 1 )", 0.0f, 4.0f );
idCVar r_bloomGlowTail( "r_bloomGlowTail", "0.34", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE | CVAR_NEW, "bloom, mode 1: each level's weight against the one above ( the glow's tail; in this mode the System Options bar Bloom Tail sets it )", 0.25f, 1.0f );
static idCVar r_bloomThreshold( "r_bloomThreshold", "1.5", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "bloom, mode 1: a pixel glows by what its largest channel has above this, in exposed units ( a lit wall is about 1, a lamp 2 to 8 )", 0.01f, 1024.0f );
static idCVar r_bloomThresholdKnee( "r_bloomThresholdKnee", "0.5", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "bloom, mode 1: the soft knee round the threshold, as a share of it ( 0 = a hard edge )", 0.0f, 1.0f );
// WP-G3, the distance tail ( the owner, 2026-11-25: "Everything having the same sized tail no matter how far away it is
// seems kind of fake" ). A glow made in the eye is the same size at any distance; one made in the air is a size in the
// world, wide when the lamp is near and small when it is far. In the glow mode a lamp nearer than r_bloomTailNear
// keeps the whole tail the player set and one beyond r_bloomTailFar its core alone ( level 0 ). The switch is a
// player's and archived ( review F1-7: the owner judges it by playing, and the choice must outlive the session );
// the distances and the far share are a developer's tunables and are not
static idCVar r_bloomTailDistance( "r_bloomTailDistance", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE | CVAR_NEW, "bloom, mode 1: a lamp's tail shrinks with the distance of the opaque surface at its pixels ( needs the hierarchical depth buffer, r_useHierarchicalDepthBuffer; a translucent emitter takes the distance of what is behind it )" );
static idCVar r_bloomTailNear( "r_bloomTailNear", "128", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "bloom, mode 1: a lamp nearer than this many units keeps the whole tail", 0.0f, 65536.0f );
static idCVar r_bloomTailFar( "r_bloomTailFar", "1024", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "bloom, mode 1: a lamp beyond this many units keeps only its core", 1.0f, 65536.0f );
static idCVar r_bloomTailFarShare( "r_bloomTailFarShare", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "bloom, mode 1: the share of its tail a lamp beyond r_bloomTailFar keeps ( 0 = the core alone )", 0.0f, 1.0f );
static idCVar r_bloomDebug( "r_bloomDebug", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "bloom: 1 = the bloom alone, 2..9 = one level alone, 10 = one tap in the mix, 11/12/13 = a NaN / an Inf / a 65504 pixel put into the frame's centre", 0, 13 );

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

// counts the chains this pass and every earlier BloomPass made ( BloomMix::chainSerial ). A file-scope static, not
// an in-class initializer
static int bloomChainSerial = 0;

// Set by the console command bloomProbe, taken by the next blooming view. Console commands run on the main thread,
// where the backend runs too, so nothing else touches it
static bool bloomProbeRequested = false;

/*
========================
BloomCvarFloat

A float cvar of the pass, held to its range. Review O1-2: a NaN typed into a cvar passes the cvar's own range and
idMath::ClampFloat ( every comparison with a NaN is false ) and would reach the mix's weights or the shader, and
r_bloomGlow, r_bloomGlowTail, r_bloomStrength and r_bloomTail are archived, so it would come back at every launch.
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

BloomPass::BloomPass( nvrhi::IDevice* device, CommonRenderPasses* commonPasses )
	: m_Device( device )
	, commonRenderPasses( commonPasses )
	, m_Dead( false )
	, m_ChainWidth( 0 )
	, m_ChainHeight( 0 )
	, m_ChainFresh( false )
	, m_FirstSource( NULL )
	, m_FirstExposure( NULL )
	, m_FirstDistSource( NULL )
	, m_FirstDistExposure( NULL )
	, m_FirstDistDepth( NULL )
	, m_PokeWarned( false )
	, m_ProbeWait( 0 )
{
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
	// them. The four permutations of bloom_down are four entries: FindShader knows an entry by name, stage and
	// suffix, and the permutation inside the blob is picked by the macro list, whose names, values and order must
	// equal the cfg line's
	const idList<shaderMacro_t> downMacros = { { "FIRST", "0" } };
	const idList<shaderMacro_t> firstMacros = { { "FIRST", "1" } };
	const idList<shaderMacro_t> firstGlowMacros = { { "FIRST", "2" } };
	const idList<shaderMacro_t> firstDistMacros = { { "FIRST", "3" } };
	const idList<shaderMacro_t> noMacros;
	m_DownPS = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/post/bloom_down", SHADER_STAGE_FRAGMENT, "", downMacros, true, LAYOUT_DRAW_VERT ) );
	m_DownFirstPS = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/post/bloom_down", SHADER_STAGE_FRAGMENT, "_first", firstMacros, true, LAYOUT_DRAW_VERT ) );
	m_DownFirstGlowPS = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/post/bloom_down", SHADER_STAGE_FRAGMENT, "_glow", firstGlowMacros, true, LAYOUT_DRAW_VERT ) );
	m_DownFirstDistPS = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/post/bloom_down", SHADER_STAGE_FRAGMENT, "_dist", firstDistMacros, true, LAYOUT_DRAW_VERT ) );
	m_UpPS = renderProgManager.GetShader( renderProgManager.FindShader( "builtin/post/bloom_up", SHADER_STAGE_FRAGMENT, "", noMacros, true, LAYOUT_DRAW_VERT ) );
	m_VS = renderProgManager.GetProgramInfo( BUILTIN_TONEMAPPING ).vs;		// the full-screen quad: four vertices, no inputs
	if( !m_VS || !m_DownFirstPS || !m_DownFirstGlowPS || !m_DownPS || !m_UpPS )
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

	// WP-G3, the distance tail's first pass: the same and the frame's depth
	nvrhi::BindingLayoutDesc firstDistLayoutDesc;
	firstDistLayoutDesc.visibility = nvrhi::ShaderType::Pixel;
	firstDistLayoutDesc.bindings =
	{
		nvrhi::BindingLayoutItem::PushConstants( 0, sizeof( BloomConstants ) ),
		nvrhi::BindingLayoutItem::Texture_SRV( 0 ),
		nvrhi::BindingLayoutItem::TypedBuffer_SRV( 1 ),
		nvrhi::BindingLayoutItem::Texture_SRV( 2 ),
		nvrhi::BindingLayoutItem::Sampler( 0 )
	};
	m_FirstDistLayout = device->createBindingLayout( firstDistLayoutDesc );

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
	m_DownFirstGlowPipeline = nullptr;
	m_DownPipeline = nullptr;
	m_UpPipeline = nullptr;
	m_FirstSet = nullptr;
	m_FirstSource = NULL;
	m_FirstExposure = NULL;
	m_Wide = nullptr;
	m_FirstDistFrame = nullptr;
	m_DownFirstDistPipeline = nullptr;
	m_DownWideSet = nullptr;
	m_FirstDistSet = nullptr;
	m_FirstDistSource = NULL;
	m_FirstDistExposure = NULL;
	m_FirstDistDepth = NULL;
	m_ProbeUpStaging = nullptr;
	m_ProbeFrameStaging = nullptr;
	m_ProbeWait = 0;
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
	psoDesc.PS = m_DownFirstPS;
	m_DownFirstPipeline = m_Device->createGraphicsPipeline( psoDesc, m_Frames[0] );

	// WP-G3: the glow's first pass writes two targets, level 0 and the wide levels' source ( a texture of level 0's
	// size ); level 1's reduction reads that source in the glow mode. The texture, the framebuffer of the two, the
	// glow's first-pass pipeline and level 1's set are part of the chain ( tested with the pipelines below ). The
	// distance tail's permutation has a pipeline of its own for the same framebuffer; where it cannot be made the
	// tail is the same at every distance. A shader blob built from an older shader list has no FIRST 2 or 3 and
	// stops the engine where the pass is made, as a missing permutation of any builtin shader does; "cannot be
	// made" here means the device refusing the pipeline
	desc.width = m_TexW[0];
	desc.height = m_TexH[0];
	desc.debugName = "Bloom/Wide";
	m_Wide = m_Device->createTexture( desc );
	if( m_Wide )
	{
		m_FirstDistFrame = m_Device->createFramebuffer( nvrhi::FramebufferDesc().addColorAttachment( m_Levels[0] ).addColorAttachment( m_Wide ) );
	}
	if( m_FirstDistFrame )
	{
		psoDesc.PS = m_DownFirstGlowPS;		// the mix's first-pass layout: the frame, the exposure buffer, a sampler
		m_DownFirstGlowPipeline = m_Device->createGraphicsPipeline( psoDesc, m_FirstDistFrame );
		if( m_DownFirstDistPS && m_FirstDistLayout )
		{
			psoDesc.bindingLayouts = { m_FirstDistLayout };
			psoDesc.PS = m_DownFirstDistPS;
			m_DownFirstDistPipeline = m_Device->createGraphicsPipeline( psoDesc, m_FirstDistFrame );
			if( !m_DownFirstDistPipeline )
			{
				common->Warning( "BloomPass: the device could not make the distance tail's pipeline of a %d x %d frame, the tail is the same at every distance", frameWidth, frameHeight );
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
	// alive ): the down pass into level i reads level i - 1, the up pass into level i reads level i + 1
	for( int i = 1; i < BLOOM_MAX_LEVELS; i++ )
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
	bool missing = !m_DownFirstPipeline || !m_DownFirstGlowPipeline || !m_DownWideSet || !m_DownPipeline || !m_UpPipeline;
	for( int i = 1; i < BLOOM_MAX_LEVELS; i++ )
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
		common->Warning( "BloomPass: the device could not make the bloom chain's pipelines or the glow's second target of a %d x %d frame, no bloom until the frame size changes", frameWidth, frameHeight );
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
BloomPass::CheckFirstDistSet

WP-G3: the binding set of the first pass with the distance tail, CheckFirstSet's rule with a third object, the
frame's depth ( the Hi-Z image, which the engine can make again too ). The caller has m_DownFirstDistPipeline,
which is made only where m_FirstDistLayout stands, so the layout is not tested here
========================
*/
bool BloomPass::CheckFirstDistSet( nvrhi::ITexture* sourceTexture, nvrhi::IBuffer* exposureBuffer, nvrhi::ITexture* depthTexture )
{
	if( m_FirstDistSet && m_FirstDistSource == sourceTexture && m_FirstDistExposure == exposureBuffer && m_FirstDistDepth == depthTexture )
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
	m_FirstDistSet = m_Device->createBindingSet( setDesc, m_FirstDistLayout );
	if( m_FirstDistSet == nullptr )
	{
		m_FirstDistSource = NULL;
		m_FirstDistExposure = NULL;
		m_FirstDistDepth = NULL;
		return false;
	}
	m_FirstDistSource = sourceTexture;
	m_FirstDistExposure = exposureBuffer;
	m_FirstDistDepth = depthTexture;
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

// the probe's second copy: level 0 as the up passes left it, for the term's mean
void BloomPass::ProbeStageUp( nvrhi::ICommandList* commandList )
{
	commandList->copyTexture( m_ProbeUpStaging, nvrhi::TextureSlice(), m_Levels[0], nvrhi::TextureSlice() );
}

/*
========================
BloomPass::ProbeRead

Prints the probe: the view and its parameters as the probe's view saw them, the mean of the frame's view
rectangle, then each level's rectangle and the mean of its texels after the down passes, then level 0's mean
after the up passes times w0: the term the tonemapper adds. The means are worked out here in double, from the
staging textures decoded by the chain's ( or, for the frame, its own ) format. The frame's mean prints as
-1 -1 -1 when the frame is not RGBA16_FLOAT and was not staged. The exposure buffer is not read back: the
adapted luminance prints as -1.
========================
*/
void BloomPass::ProbeRead()
{
	const bool r11g11b10 = ( m_ChainFormat == nvrhi::Format::R11G11B10_FLOAT );
	common->Printf( "bloomProbe: view %d %d %d %d levels %d strength %g tail %g w0 %g exposureScale %g knee %g debug %d ceiling %g\n",
					m_ProbeView[0], m_ProbeView[1], m_ProbeView[2], m_ProbeView[3], m_ProbeLevels, m_ProbeStrength, m_ProbeTail,
					m_ProbeW0, m_ProbeExposureScale, m_ProbeKnee, m_ProbeDebug, m_ProbeCeiling );
	common->Printf( "bloomProbe: glow mode %d gain %g threshold %g knee %g\n", m_ProbeGlowMode, m_ProbeGlowGain, m_ProbeThreshold, m_ProbeThresholdKnee );
	common->Printf( "bloomProbe: tail distance %d near %g far %g share %g\n", m_ProbeTailDistance, m_ProbeTailNear, m_ProbeTailFar, m_ProbeTailFarShare );
	common->Printf( "bloomProbe: mix weights frame %.9g bloom %.9g\n", m_ProbeMixFrame, m_ProbeMixBloom );
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
	if( m_ProbeGlowMode == 1 )
	{
		common->Printf( "bloomProbe: glow term mean %.9g %.9g %.9g\n", mean[0] * m_ProbeW0 * m_ProbeGlowGain, mean[1] * m_ProbeW0 * m_ProbeGlowGain, mean[2] * m_ProbeW0 * m_ProbeGlowGain );
	}
	common->Printf( "bloomProbe: done\n" );
}

/*
========================
BloomPass::Render

Builds the chain of the view, in the backend's MRB_TONE_MAP_PASS block: the frame's view rectangle down to level 0
and from there down to the last level, then the levels up and added back, and the mix the tonemap draw reads. The
block and the marker wrap steps 2 to 7 and open only after the chain stands; no path leaves them open.
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
	if( debug < 0 || debug > 13 )
	{
		debug = 0;
	}
	// ( a NaN: nothing mixed in, the default tail and ceiling; the knee's own test in the shader, k > 0, sends a
	// NaN to "off" )
	// WP-G3: the mode is read first, because each mode has its own tail ( review F1-5 )
	const bool glow = ( r_bloomMode.GetInteger() == 1 );
	const float strength = BloomCvarFloat( r_bloomStrength, 0.0f, 1.0f, 0.0f );
	const float tail = glow ? BloomCvarFloat( r_bloomGlowTail, 0.25f, 1.0f, 0.34f ) : BloomCvarFloat( r_bloomTail, 0.25f, 1.0f, 0.7f );
	const float knee = r_bloomFireflyKnee.GetFloat();
	const float ceiling = BloomCvarFloat( r_bloomCeiling, 1.0f, BLOOM_CEILING_MAX, 256.0f );
	// w0: the level 0 weight that makes the whole tail weigh s: ( 1 - tail ) / ( 1 - tail^N ), and 1 / N where the
	// tail is 1 ( the geometric sum's limit )
	const float w0 = ( tail >= 0.9999f ) ? 1.0f / n : ( 1.0f - tail ) / ( 1.0f - powf( tail, n ) );
	const float exposureScale = ::exp2f( r_exposure.GetFloat() );

	// WP-G3, the glow mode: its first pass is another permutation, which reads the threshold; the mix's does not
	const float glowGain = BloomCvarFloat( r_bloomGlow, 0.0f, 4.0f, 0.0f );
	const float threshold = glow ? BloomCvarFloat( r_bloomThreshold, 0.01f, 1024.0f, 1.5f ) : 0.0f;
	const float thresholdKnee = threshold * BloomCvarFloat( r_bloomThresholdKnee, 0.0f, 1.0f, 0.5f );

	// WP-G3: with nothing to mix in no chain is drawn and the tonemap pass takes its plain draw, so the bar's
	// lowest notch draws nothing ( the chain stays allocated ). A probe and a debug view still get their chain
	if( debug == 0 && !bloomProbeRequested && ( glow ? glowGain : strength ) <= 0.0f )
	{
		return NULL;
	}

	// WP-G3, the distance tail: in the glow mode, with a second level to narrow, its pipeline made, and this frame's
	// depth at the frame's own size ( the Hi-Z image's mip 0, filled for every 3D view while R_UseHiZ ). Without any
	// of them the glow's first pass is the one without a depth and the tail is the same at every distance
	bool tailDistance = glow && r_bloomTailDistance.GetBool() && n >= 2 && m_DownFirstDistPipeline && R_UseHiZ();
	if( tailDistance )
	{
		nvrhi::ITexture* depthTexture = ( globalImages->hierarchicalZbufferImage != NULL ) ? globalImages->hierarchicalZbufferImage->GetTextureHandle().Get() : NULL;
		tailDistance = depthTexture != NULL && depthTexture->getDesc().width == frameDesc.width && depthTexture->getDesc().height == frameDesc.height
					   && CheckFirstDistSet( sourceTexture, exposureBuffer, depthTexture );
	}
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

	renderLog.OpenMainBlock( MRB_BLOOM );
	commandList->beginMarker( "Bloom" );

	// 2: the debug poke into the frame, before the first down pass
	if( debug >= 11 && debug <= 13 )
	{
		PokeFrame( commandList, sourceTexture, vx + vw / 2, vy + vh / 2, debug );
	}

	// 3: the first down pass, the frame's view rectangle to level 0: the scrub and the firefly knee act there
	BloomConstants constants;
	constants.srcRect.Set( ( float )vx, ( float )vy, ( float )vw, ( float )vh );
	constants.srcInvSize.Set( 1.0f / ( float )frameDesc.width, 1.0f / ( float )frameDesc.height, thresholdKnee, knee );
	constants.exposure.Set( exposureScale, r_hdrMinLuminance.GetFloat(), ceiling, threshold );
	constants.tailDepth.Zero();
	constants.tailDist.Zero();
	if( tailDistance )
	{
		// the froxel fog's pair ( VolumetricFogPass: depthParms.zw ): a view distance is x / ( depth + y )
		constants.tailDepth.Set( viewDef->projectionMatrix[3 * 4 + 2], viewDef->projectionMatrix[2 * 4 + 2], 0.0f, 0.0f );
		constants.tailDist.Set( tailFar, 1.0f / ( tailFar - tailNear ), tailFarShare, 0.0f );
		DrawLevel( commandList, m_DownFirstDistPipeline, m_FirstDistSet, m_FirstDistFrame, levelW[0], levelH[0], constants );
		constants.tailDepth.Zero();
		constants.tailDist.Zero();
	}
	else
	{
		// the mix's thirteen bilinear taps into level 0, or the glow's same taps from thirty-six pixels thresholded one
		// by one, into level 0 and the wide levels' source
		DrawLevel( commandList, glow ? m_DownFirstGlowPipeline : m_DownFirstPipeline, m_FirstSet, glow ? m_FirstDistFrame : m_Frames[0], levelW[0], levelH[0], constants );
	}

	// 4: the down passes, level i - 1 to level i
	for( int i = 1; i < n; i++ )
	{
		constants.srcRect.Set( 0.0f, 0.0f, ( float )levelW[i - 1], ( float )levelH[i - 1] );
		constants.srcInvSize.Set( 1.0f / ( float )m_TexW[i - 1], 1.0f / ( float )m_TexH[i - 1], 0.0f, 0.0f );
		constants.exposure.Zero();
		// in the glow mode level 1 is reduced from the first pass's second target, the wide levels' source
		DrawLevel( commandList, m_DownPipeline, ( glow && i == 1 ) ? m_DownWideSet : m_DownSets[i], m_Frames[i], levelW[i], levelH[i], constants );
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
		m_ProbeLevels = n;
		m_ProbeStrength = strength;
		m_ProbeTail = tail;
		m_ProbeW0 = w0;
		m_ProbeExposureScale = exposureScale;
		m_ProbeKnee = knee;
		m_ProbeCeiling = ceiling;
		m_ProbeDebug = debug;
		m_ProbeGlowMode = glow ? 1 : 0;
		m_ProbeGlowGain = glowGain;
		m_ProbeThreshold = threshold;
		m_ProbeThresholdKnee = thresholdKnee;
		m_ProbeTailDistance = tailDistance ? 1 : 0;
		m_ProbeTailNear = tailNear;
		m_ProbeTailFar = tailFar;
		m_ProbeTailFarShare = tailFarShare;
		m_ProbeFrame[0] = ( int )frameDesc.width;
		m_ProbeFrame[1] = ( int )frameDesc.height;
		memcpy( m_ProbeLevelW, levelW, n * sizeof( int ) );
		memcpy( m_ProbeLevelH, levelH, n * sizeof( int ) );
		probeOn = ProbeCopy( commandList, sourceTexture, n );
	}

	// 6: the up passes, level i + 1 into level i, unless the view is looking at one level alone
	if( debug < 2 || debug > 9 )
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
	// the mix swaps a share of the frame for the chain; the glow ( WP-G3 ) leaves the frame whole and adds the chain
	float frameWeight = glow ? 1.0f : 1.0f - strength;
	float bloomWeight = glow ? glowGain * w0 : strength * w0;
	float oneTap = 0.0f;
	if( debug == 1 )
	{
		frameWeight = 0.0f;
		bloomWeight = w0;
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

	commandList->endMarker();
	renderLog.CloseMainBlock( MRB_BLOOM );
	return &m_Mix;
}
