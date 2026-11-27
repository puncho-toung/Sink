/*
===========================================================================

The System Options panel's table ( SystemOptionsPanel.cpp includes it once ): the sections, their column, and their
rows in order. A row names its cvar; a cvar this build does not have hides the row. Ranges are the bar's: a typed
value may go to the cvar's own bounds, or stays in the bar's range where the cvar has none.

Written from a survey of the renderer ( 2026-11-26 ): each row's cvar is read where its row says it takes effect.
Left out on purpose: debug, test and comparison cvars; a map's own fog media ( height fog, dust: fog is placed
volumes, the owner's rule of 2026-11-22 ); black and white points ( the owner keeps today's contrast, 2026-11-26 );
cvars this tree never reads ( r_useTrilinearFiltering, r_ssrMaxSteps, r_hdrKey ) or that do nothing ( r_gamma and
r_brightness, a hardware ramp; r_hdrAdaptionRate, fed the time since launch; r_motionBlur, compiled out ).

===========================================================================
*/

static const optionChoice_t choicesFrameRate[] =
{
	{ 60, "60 FPS" },
	{ 120, "120 FPS" },
};

static const optionChoice_t choicesVSync[] =
{
	{ 0, "Off" },
	{ 1, "Adaptive" },
	{ 2, "On" },
};

static const optionChoice_t choicesOverlay[] =
{
	{ 0, "Off" },
	{ 1, "Frame Rate" },
	{ 2, "Frame Timings" },
	{ 3, "Detailed Timings" },
};

#if ID_MSAA
static const optionChoice_t choicesAntiAliasing[] =
{
	{ ANTI_ALIASING_NONE, "Off" },
	{ ANTI_ALIASING_SMAA_1X, "SMAA 1x" },
	{ ANTI_ALIASING_TAA, "TAA" },
	{ ANTI_ALIASING_TAA_SMAA_1X, "TAA + SMAA 1x" },
	{ ANTI_ALIASING_MSAA_2X, "MSAA 2x" },
	{ ANTI_ALIASING_MSAA_4X, "MSAA 4x" },
};
static const int TAA_MODES = V( ANTI_ALIASING_TAA ) | V( ANTI_ALIASING_TAA_SMAA_1X );
#else
static const optionChoice_t choicesAntiAliasing[] =
{
	{ ANTI_ALIASING_NONE, "Off" },
	{ ANTI_ALIASING_SMAA_1X, "SMAA 1x" },
	{ ANTI_ALIASING_TAA, "TAA" },
};
static const int TAA_MODES = V( ANTI_ALIASING_TAA );
#endif

static const optionChoice_t choicesHistoryTaps[] =
{
	{ 5, "Fast ( 5 taps )" },
	{ 9, "Full ( 9 taps )" },
};

static const optionChoice_t choicesCRT[] =
{
	{ 0, "Off" },
	{ 1, "Mattias" },
	{ 2, "NewPixie" },
	{ 3, "Advanced" },
};

static const optionChoice_t choicesCRTCurvature[] =
{
	{ 0, "Flat" },
	{ 2, "Curved" },
};

static const optionChoice_t choicesRenderMode[] =
{
	{ 0, "Doom 3" },
	{ 1, "2-bit" },
	{ 2, "2-bit Hi" },
	{ 3, "Commodore 64" },
	{ 4, "Commodore 64 Hi" },
	{ 5, "Amstrad CPC 6128" },
	{ 6, "Amstrad CPC 6128 Hi" },
	{ 7, "Sega Genesis" },
	{ 8, "Sega Genesis Hi" },
	{ 9, "Sony PSX" },
};

static const optionChoice_t choicesShadows[] =
{
	{ 0, "On" },
	{ 1, "Off" },
};

static const optionChoice_t choicesShadowSamples[] =
{
	{ 1, "1 ( hard )" },
	{ 4, "4" },
	{ 8, "8" },
	{ 12, "12" },
	{ 16, "16" },
	{ 24, "24" },
	{ 32, "32" },
};

static const optionChoice_t choicesCascades[] =
{
	{ 0, "1" },
	{ 1, "2" },
	{ 2, "3" },
	{ 3, "4" },
	{ 4, "5" },
};

static const optionChoice_t choicesShadowCache[] =
{
	{ 0, "Off" },
	{ 1, "Sun Window" },
	{ 2, "Full" },
};

static const optionChoice_t choicesShadowCacheSize[] =
{
	{ 4096, "4096 ( 64 MiB )" },
	{ 8192, "8192 ( 256 MiB )" },
};

static const optionChoice_t choicesBloodReflections[] =
{
	{ 0, "Static" },
	{ 1, "Dynamic ( SSR )" },
};

static const optionChoice_t choicesFogQuality[] =
{
	{ -1, "Custom" },
	{ 0, "Low" },
	{ 1, "Medium" },
	{ 2, "High" },
};

// greyed-out reasons, shared by many rows
static const char* NEEDS_DOOM3_MODE = "Needs the render mode Doom 3 ( Retro Looks ).";
static const char* NEEDS_TAA = "Needs TAA.";
static const char* NEEDS_BLOOM = "Needs Bloom on.";
static const char* NEEDS_SHADOWS = "Needs Shadows on.";
static const char* NEEDS_FOG = "Needs Volumetric Fog on.";

/*
========================
BuildTable
========================
*/
static void BuildTable()
{
	if( rows.Num() > 0 )
	{
		return;
	}

	//
	// left column
	//

	AddSection( "Display", 0 );
	Special( "Display Mode", "r_fullscreen", "r_vidMode", SPECIAL_DISPLAY_MODE, 0, 0, 0 ).Flags( OPTF_NO_REVERT ).Summary( "" )
	.Desc( "A window, the borderless fullscreen, or an exclusive fullscreen mode of a monitor. An exclusive mode asks to be kept, and goes back by itself after twenty seconds." );
#ifdef _WIN32
	Special( "Graphics API", "r_graphicsAPI", NULL, SPECIAL_RENDER_API, 0, 0, 0 ).Flags( OPTF_RESTART )
	.Desc( "The graphics API the engine starts with." );
#endif
	Choice( "Vertical Sync", "r_swapInterval", choicesVSync ).Summary( "VSync" )
	.Desc( "Off: the lowest latency, and the picture may tear. Adaptive: waits for the screen, and tears when a frame is late ( on DirectX 12 it does not wait and does not tear ). On: always waits for the screen." );
	Toggle( "Tear-Free Without VSync", "r_vkPreferFastSync" ).Flags( OPTF_RESTART | OPTF_ADVANCED ).Parent( "r_swapInterval", V( 0 ), "Needs Vertical Sync off." )
	.Desc( "With Vertical Sync off, Vulkan shows the newest finished frame without tearing ( mailbox ) instead of at once with tearing." );
	Choice( "Frame Rate", "com_engineHz", choicesFrameRate ).Flags( OPTF_RESTART )
	.Desc( "How many times a second the game runs and draws. Match the screen: 120 on a 60 Hz screen judders." );
#ifdef _WIN32
	SliderInt( "Max Frame Latency", "r_dxMaxFrameLatency", 0, 3, 1, "%d frames" ).Flags( OPTF_RESTART | OPTF_ADVANCED )
	.Desc( "How many frames the CPU may queue ahead on DirectX 12. 1 gives the lowest input latency; 0 leaves it to the driver." );
#endif
	Choice( "Performance Overlay", "com_showFPS", choicesOverlay )
	.Desc( "The frame rate, or the frame, light and memory timings, over the game." );

	AddSection( "Brightness & Ambient", 0 );
	Special( "Brightness", "r_exposure", "r_lightScale", SPECIAL_BRIGHTNESS, 0.0f, 100.0f, 1.0f ).Summary( "Brightness" )
	.Desc( "Brightens the whole picture: the exposure and the lights move together. The bloom's threshold is in exposed units, so a brighter picture also glows more." );
	Slider( "Ambient Light", "r_forceAmbient", 0.0f, 1.0f, 0.01f, "%.2f" ).Summary( "Ambient" )
	.Desc( "An even fill light, so that unlit corners are not pure black; the reflections' ambient rides on it. 0 is classic Doom 3 darkness." );

	AddSection( "Anti-Aliasing", 0 );
	Choice( "Method", "r_antiAliasing", choicesAntiAliasing ).Summary( "" )
	.Desc( "Smooths jagged edges. SMAA finds edges in one frame; TAA averages jittered frames over time for steadier edges." );
	Slider( "TAA Strength", "r_taaNewFrameWeight", 0.5f, 0.96f, 0.01f, "%.2f" ).Flags( OPTF_INVERT ).Summary( "Strength" ).Parent( "r_antiAliasing", TAA_MODES, NEEDS_TAA ).Parent( "r_renderMode", V( 0 ), NEEDS_DOOM3_MODE )
	.Desc( "How much of the past frames a pixel keeps: stronger is smoother and steadier, weaker is sharper in motion and shimmers more. It is 1 less the new frame's weight." );
	Slider( "TAA Sharpening", "r_taaSharpen", 0.0f, 0.5f, 0.01f, "%.2f" ).Summary( "Sharpen" ).Parent( "r_antiAliasing", TAA_MODES, NEEDS_TAA ).Parent( "r_renderMode", V( 0 ), NEEDS_DOOM3_MODE )
	.Desc( "A slight sharpening of what is shown, never of the history. 0.15 measured best; above about 0.35 it adds contrast the scene does not have." );
	Slider( "Sharpness in Motion", "r_taaMotionFrameWeight", 0.0f, 0.6f, 0.01f, "%.2f" ).Flags( OPTF_ADVANCED ).Parent( "r_antiAliasing", TAA_MODES, NEEDS_TAA ).Parent( "r_renderMode", V( 0 ), NEEDS_DOOM3_MODE )
	.Desc( "Takes more of the new frame where a moving camera reads the history between its pixels: higher is sharper while turning and less steady. Below the new frame's weight it does nothing." );
	Slider( "Ghosting Clamp", "r_taaClampingFactor", 0.5f, 2.0f, 0.05f, "%.2f" ).Flags( OPTF_ADVANCED ).Parent( "r_antiAliasing", TAA_MODES, NEEDS_TAA ).Parent( "r_renderMode", V( 0 ), NEEDS_DOOM3_MODE )
	.Desc( "How far the history may stray from the current frame: lower cuts ghost trails and flickers more, higher is steadier and smears moving things." );
	Toggle( "Keep Thin Detail", "r_taaClampCenter" ).Flags( OPTF_ADVANCED ).Parent( "r_antiAliasing", TAA_MODES, NEEDS_TAA ).Parent( "r_renderMode", V( 0 ), NEEDS_DOOM3_MODE )
	.Desc( "One-pixel lines, lights and fine texels keep their history instead of being pulled toward their neighbours: thin detail stays bright and steady." );
	Choice( "History Filter", "r_taaHistoryTaps", choicesHistoryTaps ).Flags( OPTF_ADVANCED ).Parent( "r_antiAliasing", TAA_MODES, NEEDS_TAA ).Parent( "r_renderMode", V( 0 ), NEEDS_DOOM3_MODE )
	.Desc( "The taps of the history read: 5 leaves out four corner taps that add almost nothing; 9 is the full filter of before the repair." );

	AddSection( "Bloom", 0 );
	Toggle( "Bloom", "r_bloom" ).Summary( "" ).Parent( "r_renderMode", V( 0 ), NEEDS_DOOM3_MODE )
	.Desc( "Bright lamps and emitters glow. Off is the crisp picture with no glow at all." );
	Slider( "Strength", "r_bloomGlow", 0.0f, 1.0f, 0.01f, "%.2f" ).Summary( "Strength" ).Parent( "r_bloom", V( 1 ), NEEDS_BLOOM ).Parent( "r_renderMode", V( 0 ), NEEDS_DOOM3_MODE )
	.Desc( "How bright the glow is; 0 draws none. The owner's setting is 0.24." );
	Slider( "Tail", "r_bloomGlowTail", 0.25f, 1.0f, 0.01f, "%.2f" ).Summary( "Tail" ).Parent( "r_bloom", V( 1 ), NEEDS_BLOOM ).Parent( "r_renderMode", V( 0 ), NEEDS_DOOM3_MODE )
	.Desc( "How far the glow reaches: low is a tight glow, 1 a wide halo. The owner's setting is 0.34." );
	Slider( "Knee", "r_bloomFireflyKnee", 16.0f, 1024.0f, 16.0f, "%.0f" ).Flags( OPTF_LOG ).Parent( "r_bloom", V( 1 ), NEEDS_BLOOM ).Parent( "r_renderMode", V( 0 ), NEEDS_DOOM3_MODE )
	.Desc( "Caps what a lone very bright pixel adds to the glow ( about a third of this ): lower stops sparkles and specks from blooming, too low dims real lamps." );
	Slider( "Threshold", "r_bloomThreshold", 0.5f, 8.0f, 0.1f, "%.2f" ).Flags( OPTF_LOG ).Parent( "r_bloom", V( 1 ), NEEDS_BLOOM ).Parent( "r_renderMode", V( 0 ), NEEDS_DOOM3_MODE )
	.Desc( "How bright a pixel must be to glow, in exposed units ( a lit wall is about 1, a lamp 2 to 8 ): lower makes more of the scene glow." );
	Slider( "Threshold Softness", "r_bloomThresholdKnee", 0.0f, 1.0f, 0.05f, "%.2f" ).Flags( OPTF_ADVANCED ).Parent( "r_bloom", V( 1 ), NEEDS_BLOOM ).Parent( "r_renderMode", V( 0 ), NEEDS_DOOM3_MODE )
	.Desc( "A soft ramp round the threshold, as a share of it; 0 is a hard edge where surfaces pop into the glow." );
	Toggle( "Distance Tail", "r_bloomTailDistance" ).Parent( "r_bloom", V( 1 ), NEEDS_BLOOM ).Parent( "r_renderMode", V( 0 ), NEEDS_DOOM3_MODE )
	.Desc( "Far lamps get a smaller halo than near ones, as a glow in the air would; off gives every lamp the same tail." );
	Toggle( "Weapon Glow", "r_bloomViewWeapon" ).Flags( OPTF_ADVANCED ).Parent( "r_bloom", V( 1 ), NEEDS_BLOOM ).Parent( "r_renderMode", V( 0 ), NEEDS_DOOM3_MODE )
	.Desc( "On: the first-person weapon glows like the world, a halo against the dark. Off: it gives no glow, its lit parts and muzzle flash included." );

	AddSection( "Post Effects", 0 );
	Toggle( "Filmic Finish", "r_useFilmicPostFX" ).Summary( "Filmic" ).Parent( "r_renderMode", V( 0 ), NEEDS_DOOM3_MODE )
	.Desc( "A slight colour fringe toward the screen's edges and a fine moving dither that hides banding." );
	Choice( "CRT Filter", "r_useCRTPostFX", choicesCRT ).Summary( "CRT" )
	.Desc( "Draws the picture as an old CRT screen: scanlines, glow and colour bleed, in three styles." );
	Choice( "CRT Curvature", "r_crtCurvature", choicesCRTCurvature ).Parent( "r_useCRTPostFX", V( 1 ) | V( 2 ) | V( 3 ), "Needs a CRT filter." )
	.Desc( "Bends the picture like the glass of a tube, or keeps it flat." );
	Slider( "CRT Vignette", "r_crtVignette", 0.0f, 1.0f, 0.05f, "%.2f" ).Parent( "r_useCRTPostFX", V( 2 ), "Needs the NewPixie CRT filter." )
	.Desc( "How much the CRT picture darkens toward its borders." );

	AddSection( "Retro Looks", 0 );
	Choice( "Render Mode", "r_renderMode", choicesRenderMode ).Summary( "" )
	.Desc( "The palettes and resolutions of old machines. Any mode but Doom 3 turns TAA, the bloom and the filmic finish off." );
	Slider( "Retro Dither", "r_retroDitherScale", 0.0f, 1.0f, 0.05f, "%.2f" ).Parent( "r_renderMode", V( 1 ) | V( 2 ) | V( 3 ) | V( 4 ) | V( 5 ) | V( 6 ) | V( 7 ) | V( 8 ), "Needs a retro palette mode." )
	.Desc( "How strongly the retro palette is dithered: higher hides colour banding with a coarser pattern." );
	Slider( "PSX Vertex Wobble", "r_psxVertexJitter", 0.0f, 0.75f, 0.05f, "%.2f" ).Parent( "r_renderMode", V( 9 ), "Needs the Sony PSX mode." )
	.Desc( "Snaps vertices to a coarse grid, so geometry wobbles as on the PlayStation." );
	Toggle( "PSX Affine Textures", "r_psxAffineTextures" ).Parent( "r_renderMode", V( 9 ), "Needs the Sony PSX mode." )
	.Desc( "Textures warp across large polygons, without perspective correction, as on the PlayStation." );
	Toggle( "Pixelated Textures", "image_pixelLook" )
	.Desc( "Turns off texture smoothing on most surfaces, for the 90s software renderer's look." );

	//
	// right column
	//

	AddSection( "Shadows", 1 );
	Choice( "Shadows", "r_skipShadows", choicesShadows ).Summary( "" )
	.Desc( "Shadow-mapped shadows from every light. Off is much cheaper and looks flat." );
	Choice( "Filter Samples", "r_shadowMapSamples", choicesShadowSamples ).Summary( "Samples" ).Parent( "r_skipShadows", V( 0 ), NEEDS_SHADOWS )
	.Desc( "Taps that soften each shadow's edge: more is smoother and less noisy, and costs in step with the taps for every lit pixel of a shadowed light." );
	Slider( "Softness", "r_shadowMapJitterScale", 0.5f, 6.0f, 0.1f, "%.1f" ).Parent( "r_skipShadows", V( 0 ), NEEDS_SHADOWS )
	.Desc( "The radius of the filter in shadow texels: higher gives softer, wider penumbras." );
	Toggle( "Animated Noise", "r_shadowMapRandomizeJitter" ).Flags( OPTF_ADVANCED ).Parent( "r_skipShadows", V( 0 ), NEEDS_SHADOWS )
	.Desc( "Turns the filter's noise every frame so that TAA averages it smooth; off keeps one fixed pattern, steadier without TAA." );
	Slider( "Detail", "r_shadowMapLodScale", 0.5f, 4.0f, 0.1f, "%.1f" ).Parent( "r_skipShadows", V( 0 ), NEEDS_SHADOWS )
	.Desc( "Higher keeps sharper shadow maps for lights that are small or far on screen; lower drops them to coarser maps sooner. Bigger maps cost atlas space and draw time." );
	SliderInt( "Detail Bias", "r_shadowMapLodBias", -1, 2, 1, "%+d" ).Flags( OPTF_ADVANCED ).Parent( "r_skipShadows", V( 0 ), NEEDS_SHADOWS )
	.Desc( "Each step up halves every light's shadow resolution, and far lights pushed past the smallest size lose their shadow; -1 raises them one step." );
	Choice( "Sun Cascades", "r_shadowMapSplits", choicesCascades ).Parent( "r_skipShadows", V( 0 ), NEEDS_SHADOWS )
	.Desc( "How many shadow maps the sun's range is split into: more keeps sun shadows sharp further away. Each is one more 1024 x 1024 map drawn." );
	Slider( "Sun Distance", "r_shadowMapSplitFar", 2000.0f, 50000.0f, 500.0f, "%.0f" ).Flags( OPTF_LOG ).Parent( "r_skipShadows", V( 0 ), NEEDS_SHADOWS )
	.Desc( "How far sun shadows reach, in units; beyond it the sun casts none. Longer spreads the cascades' texels thinner." );
	Slider( "Cascade Spread", "r_shadowMapSplitWeight", 0.0f, 1.0f, 0.05f, "%.2f" ).Flags( OPTF_ADVANCED ).Parent( "r_skipShadows", V( 0 ), NEEDS_SHADOWS ).Parent( "r_shadowMapSplits", V( 1 ) | V( 2 ) | V( 3 ) | V( 4 ), "Needs two cascades or more." )
	.Desc( "1 packs the cascades near you ( sharp close, coarse far ); 0 spaces them evenly over the sun's distance." );
	Choice( "Shadow Caching", "r_shadowCache", choicesShadowCache ).Parent( "r_skipShadows", V( 0 ), NEEDS_SHADOWS )
	.Desc( "Reuses the shadow maps of things that do not move instead of drawing them again each frame: the same picture for less GPU time. Full takes its atlas's memory when first chosen." );
	Choice( "Cache Size", "r_shadowCacheSize", choicesShadowCacheSize ).Flags( OPTF_RESTART | OPTF_ADVANCED ).Parent( "r_skipShadows", V( 0 ), NEEDS_SHADOWS ).Parent( "r_shadowCache", V( 2 ), "Needs Shadow Caching at Full." )
	.Desc( "Room for cached shadow maps of lights: the larger atlas keeps more and draws less, at four times the memory." );

	AddSection( "Ambient Occlusion", 1 );
	Toggle( "Ambient Occlusion", "r_useSSAO" ).Summary( "" )
	.Desc( "Darkens creases, corners and the places where things touch, in the ambient light." );
	Slider( "Radius", "r_ssaoRadiusWorld", 0.25f, 3.0f, 0.05f, "%.2f m" ).Parent( "r_useSSAO", V( 1 ), "Needs Ambient Occlusion on." )
	.Desc( "How far round a point the occlusion looks: larger darkens broader areas, smaller only tight creases." );
	Slider( "Intensity", "r_ssaoAmount", 1.0f, 10.0f, 0.1f, "%.1f" ).Parent( "r_useSSAO", V( 1 ), "Needs Ambient Occlusion on." )
	.Desc( "How dark the occluded places get." );
	Slider( "Contrast", "r_ssaoPowerExponent", 0.5f, 4.0f, 0.1f, "%.1f" ).Parent( "r_useSSAO", V( 1 ), "Needs Ambient Occlusion on." )
	.Desc( "Higher keeps the darkening in the deepest corners and clears it elsewhere; lower spreads a softer grey." );

	AddSection( "Reflections", 1 );
	Choice( "Blood Reflections", "r_useSSR", choicesBloodReflections ).Summary( "" )
	.Desc( "Pools of blood and other bumpy reflective surfaces reflect what is on screen; Static shows the baked reflection probe only." );
	Slider( "Reach", "r_ssrMaxDistance", 10.0f, 200.0f, 5.0f, "%.0f m" ).Parent( "r_useSSR", V( 1 ), "Needs Dynamic reflections." )
	.Desc( "How far a reflection ray travels, and how many steps it may take: longer finds distant things and costs more for every reflecting pixel." );
	Slider( "Step Size", "r_ssrStride", 1.0f, 32.0f, 1.0f, "%.0f px" ).Parent( "r_useSSR", V( 1 ), "Needs Dynamic reflections." )
	.Desc( "Pixels a step: smaller finds thin contacts and reaches less far; larger reaches further and misses detail." );
	Slider( "Thickness", "r_ssrZThickness", 0.5f, 16.0f, 0.5f, "%.1f" ).Flags( OPTF_LOG ).Parent( "r_useSSR", V( 1 ), "Needs Dynamic reflections." )
	.Desc( "How thick things are taken to be behind their front: higher fills holes in reflections and may reflect what should be hidden." );
	Slider( "Dither", "r_ssrJitter", 0.0f, 1.0f, 0.05f, "%.2f" ).Flags( OPTF_ADVANCED ).Parent( "r_useSSR", V( 1 ), "Needs Dynamic reflections." )
	.Desc( "Starts each ray at a random offset: trades the step bands for fine noise, which TAA smooths." );

	AddSection( "Volumetric Fog", 1 );
	Toggle( "Volumetric Fog", "r_volumetricFog" ).Summary( "" )
	.Desc( "Fog lights and fog volumes become lit, shadowed fog in which light shafts show; off draws them with the classic flat fog pass. A view with no fog volume in it costs nothing." );
	Choice( "Quality", "r_volumetricFogQuality", choicesFogQuality ).Summary( "Quality" ).Parent( "r_volumetricFog", V( 1 ), NEEDS_FOG )
	.Desc( "The fog grid's resolution: higher gives finer shafts and edges. At 1920 x 1080: Low 19 MiB, Medium 44, High 148. Custom takes the column and slice sizes below ( as set, Medium )." );
	SliderInt( "Column Size", "r_volumetricFogColumnPixels", 4, 32, 1, "%d px" ).Flags( OPTF_ADVANCED ).Parent( "r_volumetricFog", V( 1 ), NEEDS_FOG ).Parent( "r_volumetricFogQuality", V( -1 ), "Needs Quality Custom." )
	.Desc( "Screen pixels a fog column: smaller is sharper fog, and costs memory in step with the inverse square." );
	SliderInt( "Depth Slices", "r_volumetricFogSlices", 16, 192, 8, "%d" ).Flags( OPTF_ADVANCED ).Parent( "r_volumetricFog", V( 1 ), NEEDS_FOG ).Parent( "r_volumetricFogQuality", V( -1 ), "Needs Quality Custom." )
	.Desc( "Depth layers of the fog grid: more keeps fog edges and shafts crisp in depth." );
	Slider( "Range", "r_volumetricFogFar", 512.0f, 16384.0f, 256.0f, "%.0f" ).Flags( OPTF_LOG ).Parent( "r_volumetricFog", V( 1 ), NEEDS_FOG )
	.Desc( "How far the fog grid reaches, in units; there is no volumetric fog beyond it. Longer spreads the same slices thinner." );
	Slider( "Glow Direction", "r_volumetricFogAnisotropy", -0.9f, 0.9f, 0.05f, "%.2f" ).Parent( "r_volumetricFog", V( 1 ), NEEDS_FOG )
	.Desc( "Above 0, lit fog is brighter looking toward the light ( strong shafts and halos ); 0 is the same from every side; below 0, brighter looking away." );
	Slider( "Smoothing", "r_volumetricFogHistoryWeight", 0.0f, 0.95f, 0.05f, "%.2f" ).Parent( "r_volumetricFog", V( 1 ), NEEDS_FOG )
	.Desc( "The share of last frame's fog kept: higher is smooth and steady, lower follows moving lights faster and is noisier." );
	Toggle( "Fog Shadows", "r_volumetricFogShadows" ).Parent( "r_volumetricFog", V( 1 ), NEEDS_FOG )
	.Desc( "Things cast shadows into the fog, which is what makes light shafts; off lights the fog as if nothing blocked the lights." );
	Toggle( "Fog on Glass", "r_volumetricFogTranslucent" ).Parent( "r_volumetricFog", V( 1 ), NEEDS_FOG )
	.Desc( "Glass, particles and other see-through surfaces are fogged at their own depth; off draws them unfogged in front of the fog." );
	Toggle( "Jitter", "r_volumetricFogJitter" ).Flags( OPTF_ADVANCED ).Parent( "r_volumetricFog", V( 1 ), NEEDS_FOG )
	.Desc( "Moves the fog's light samples inside each cell from frame to frame, so that the smoothing averages away the grid's blocks." );

	AddSection( "View Distance", 1 );
	Special( "View Distance", "r_viewDistance", "r_distanceCull", SPECIAL_VIEW_DISTANCE, 1024.0f, 8192.0f, 128.0f ).Summary( "" )
	.Desc( "Full detail out to this radius in units; things and lights beyond it fade out. The top of the bar turns distance culling off." );
	Slider( "Fade Width", "r_cullFadeFraction", 0.05f, 0.5f, 0.01f, "%.2f" ).Parent( "r_distanceCull", V( 1 ), "Needs a view distance below Unlimited." )
	.Desc( "The width of the band past the view distance over which things fade out, as a share of the distance: wider hides the pop." );

	AddSection( "Audio", 1 );
	Special( "Volume", "s_volume_dB", NULL, SPECIAL_VOLUME, 0.0f, 100.0f, 1.0f ).Flags( OPTF_NO_VIEW ).Summary( "Volume" )
	.Desc( "The game's master volume." );
	Slider( "Subwoofer Level", "s_subFraction", 0.0f, 1.0f, 0.05f, "%.2f" ).Flags( OPTF_NO_VIEW )
	.Desc( "The share of each sound sent to the subwoofer ( LFE ) channel; it matters only on 5.1 or 7.1 output." );

	states.SetNum( rows.Num() );
	for( int i = 0; i < states.Num(); i++ )
	{
		states[i].cvar = NULL;
		states[i].cvar2 = NULL;
		states[i].saved = false;
		states[i].openSaved = false;
		states[i].editText[0] = '\0';
	}
}
