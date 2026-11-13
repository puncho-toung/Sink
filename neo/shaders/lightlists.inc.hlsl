// lightlists.inc.hlsl - the HLSL mirror of the light lists' two GPU structs ( track F ). Field for field
// renderer/LightLists.h: recordLight_t ( C1, 192 bytes ) and shadowRow_t ( C2, 80 bytes ), whose sizes and
// offsets that header asserts. The includer declares the buffers: the forward pass at t16 and t17 of its
// image set, the volumetrics at theirs.

#ifndef LIGHTLISTS_INC_HLSL
#define LIGHTLISTS_INC_HLSL

struct recordLight_t			// 192 bytes, the relative frame ( everything minus the view's render origin )
{
	float4	originInvRadiusSq;	// xyz = the shading origin, w = 1 / r^2 of the binning sphere
	float4	color;				// rgb = the light stage's colour x r_lightScale x cullFade, w = spot scale ( 0 )
	float4	spotDir;			// w = spot bias ( 1 )
	int		shadowRow;			// the first of the light's shadow rows; -1 = none
	uint	channelMask;
	uint	flags;				// RLF_*
	int		uniqueId;
	float4	lightProject[4];	// the planes S, T, Q and falloff
	float4	mathLight[3];		// rpUser0..2 as R_MathLightToParms encodes them; zero unless RLF_MATH
	float4	ids;				// x = importance rank, y = light stage, z = the record's index in the previous view, w = fog albedo
};

struct shadowRow_t				// 80 bytes
{
	float4	worldToShadow[4];	// the relative position to the map's window space ( the legacy matrices without the model matrix )
	float4	atlasRect;			// xy = the map's corner in the atlas, z = its size ( -1: the light has no placed map; -2: it has, but not this face or cascade ), w = the fade, or the cascade's far end on the sun's rows
};

// recordLight_t::flags
static const uint RLF_SUN				= 1u << 0;
static const uint RLF_POINT				= 1u << 1;
static const uint RLF_PARALLEL			= 1u << 2;
static const uint RLF_MATH				= 1u << 3;
static const uint RLF_DEFAULT_TEXTURES	= 1u << 4;
static const uint RLF_CLUSTERED			= 1u << 5;
static const uint RLF_AMBIENT			= 1u << 6;
static const uint RLF_FOGLIGHT			= 1u << 7;
static const uint RLF_NOSPECULAR		= 1u << 8;
static const uint RLF_MOVED_THIS_FRAME	= 1u << 9;

// rpSurfLights.w, the features of a draw's run ( LLF_* of the light-list core )
static const uint LLF_SHADOWED			= 1u << 0;
static const uint LLF_MATH				= 1u << 1;
static const uint LLF_PROJECTED			= 1u << 2;
static const uint LLF_POINT				= 1u << 3;

#endif // LIGHTLISTS_INC_HLSL
