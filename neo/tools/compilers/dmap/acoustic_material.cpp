//puncho - SB1a (R-SB1a-21, R-SB1a-32, R-SB1a-34, R-SB1a-37): the acoustic functions dmap's Stage A and the game share
#include "precompiled.h"
#pragma hdrstop

#include "acoustic.h"

/*
===============================================================================

	The material rule and the surface-type table

	No acoustic keyword exists: a material is read only through what every material already has, its surface
	type word (metal, stone ...), sky, contents, coverage, drawing, deform and sort. The numbers depend on the
	material alone, so dmap and the game resolve the same numbers for it.

===============================================================================
*/

// R-SB1a-21's surface-type table, one row per acousticPreset_t: absorption L M H, scattering, transmission L M H.
// Valve's presets (phonon.h) except flesh, cardboard, plastic, liquid (inferred) and sky (all absorbed).
// A change here raises SND_BAKER_REVISION_A.
static const acousticNumbers_t acousticPresetTable[ACOUSTIC_PRESET_COUNT] =
{
	{ { 0.10f, 0.20f, 0.30f }, 0.05f, { 0.100f, 0.050f, 0.030f } },	// generic: NONE, surftype10 ... surftype15
	{ { 0.20f, 0.07f, 0.06f }, 0.05f, { 0.200f, 0.025f, 0.010f } },	// metal: metal, ricochet
	{ { 0.05f, 0.07f, 0.08f }, 0.05f, { 0.015f, 0.002f, 0.001f } },	// concrete: stone
	{ { 0.30f, 0.50f, 0.65f }, 0.40f, { 0.050f, 0.020f, 0.010f } },	// flesh
	{ { 0.11f, 0.07f, 0.06f }, 0.05f, { 0.070f, 0.014f, 0.005f } },	// wood
	{ { 0.20f, 0.30f, 0.35f }, 0.30f, { 0.300f, 0.150f, 0.080f } },	// cardboard
	{ { 0.01f, 0.01f, 0.02f }, 0.05f, { 0.000f, 0.000f, 0.000f } },	// liquid: liquid, and liquid contents
	{ { 0.06f, 0.03f, 0.02f }, 0.05f, { 0.060f, 0.044f, 0.011f } },	// glass
	{ { 0.05f, 0.06f, 0.07f }, 0.10f, { 0.080f, 0.040f, 0.020f } },	// plastic
	{ { 1.00f, 1.00f, 1.00f }, 0.00f, { 0.000f, 0.000f, 0.000f } },	// sky: no type, all absorbed
};

// the row each surfTypes_t reads
static const int acousticSurfaceTypePreset[16] =
{
	ACOUSTIC_PRESET_GENERIC,	// SURFTYPE_NONE
	ACOUSTIC_PRESET_METAL,		// SURFTYPE_METAL
	ACOUSTIC_PRESET_CONCRETE,	// SURFTYPE_STONE
	ACOUSTIC_PRESET_FLESH,		// SURFTYPE_FLESH
	ACOUSTIC_PRESET_WOOD,		// SURFTYPE_WOOD
	ACOUSTIC_PRESET_CARDBOARD,	// SURFTYPE_CARDBOARD
	ACOUSTIC_PRESET_LIQUID,		// SURFTYPE_LIQUID
	ACOUSTIC_PRESET_GLASS,		// SURFTYPE_GLASS
	ACOUSTIC_PRESET_PLASTIC,	// SURFTYPE_PLASTIC
	ACOUSTIC_PRESET_METAL,		// SURFTYPE_RICOCHET
	ACOUSTIC_PRESET_GENERIC,	// SURFTYPE_10
	ACOUSTIC_PRESET_GENERIC,	// SURFTYPE_11
	ACOUSTIC_PRESET_GENERIC,	// SURFTYPE_12
	ACOUSTIC_PRESET_GENERIC,	// SURFTYPE_13
	ACOUSTIC_PRESET_GENERIC,	// SURFTYPE_14
	ACOUSTIC_PRESET_GENERIC,	// SURFTYPE_15
};

static const char* const acousticSurfaceTypeNames[16] =
{
	"NONE", "METAL", "STONE", "FLESH", "WOOD", "CARDBOARD", "LIQUID", "GLASS",
	"PLASTIC", "RICOCHET", "10", "11", "12", "13", "14", "15"
};

static_assert( SURFTYPE_15 == 15, "the acoustic tables hold one row per surface type" );

static const int ACOUSTIC_LIQUID_CONTENTS = CONTENTS_WATER | CONTENTS_LAVA | CONTENTS_SLIME;

// M2: a portal sky or a skybox texgen
static bool AcousticMaterialIsSky( const idMaterial* m )
{
	const texgen_t texgen = m->Texgen();
	return m->IsPortalSky() || texgen == TG_SKYBOX_CUBE || texgen == TG_WOBBLESKY_CUBE;
}

// the one place a material's numbers are chosen, so AcousticMaterialPasses and AcousticResolveNumbers agree:
// sky, then liquid by contents or type, then the type's row (GLASS reads the glass row)
static int AcousticMaterialPreset( const idMaterial* m )
{
	if( AcousticMaterialIsSky( m ) )
	{
		return ACOUSTIC_PRESET_SKY;
	}
	if( m->GetContentFlags() & ACOUSTIC_LIQUID_CONTENTS )
	{
		return ACOUSTIC_PRESET_LIQUID;
	}
	return acousticSurfaceTypePreset[m->GetSurfaceType()];
}

/*
============
AcousticMaterialPasses

R-SB1a-21, first match wins:
M1 NULL out; M2 sky in; M3 area portal out; M4 undrawn and opaque (caulk) in by type; M5 undrawn out;
M6 deform or decal sort out; M7 liquid contents in; M8 opaque in by type; M9 glass type in; M10 out.
============
*/
bool AcousticMaterialPasses( const idMaterial* material, int& source, int& preset )
{
	if( material == NULL )
	{
		return false;
	}

	const int contents = material->GetContentFlags();
	const int surfaceType = material->GetSurfaceType();
	const bool opaque = ( material->Coverage() == MC_OPAQUE );

	if( AcousticMaterialIsSky( material ) )
	{
		source = ACOUSTIC_SOURCE_SKY;
	}
	else if( contents & CONTENTS_AREAPORTAL )
	{
		return false;
	}
	else if( !material->IsDrawn() )
	{
		if( !opaque )
		{
			return false;
		}
		// caulk: liquid contents choose the liquid row before the type's, so the source names that row
		source = ( contents & ACOUSTIC_LIQUID_CONTENTS ) ? ACOUSTIC_SOURCE_LIQUID : surfaceType;
	}
	else if( material->Deform() != DFRM_NONE || material->GetSort() == ( float )SS_DECAL )
	{
		return false;
	}
	else if( contents & ACOUSTIC_LIQUID_CONTENTS )
	{
		source = ACOUSTIC_SOURCE_LIQUID;
	}
	else if( opaque )
	{
		source = surfaceType;
	}
	else if( surfaceType == SURFTYPE_GLASS )
	{
		source = ACOUSTIC_SOURCE_GLASS;
	}
	else
	{
		return false;
	}

	preset = AcousticMaterialPreset( material );
	return true;
}

/*
============
AcousticPresetNumbers
============
*/
void AcousticPresetNumbers( int preset, acousticNumbers_t& out )
{
	assert( preset >= 0 && preset < ACOUSTIC_PRESET_COUNT );
	if( preset < 0 || preset >= ACOUSTIC_PRESET_COUNT )
	{
		preset = ACOUSTIC_PRESET_GENERIC;
	}
	out = acousticPresetTable[preset];
}

/*
============
AcousticResolveNumbers
============
*/
bool AcousticResolveNumbers( const idMaterial* m, acousticNumbers_t& out )
{
	if( m == NULL )
	{
		return false;
	}
	out = acousticPresetTable[AcousticMaterialPreset( m )];
	return true;
}

/*
============
AcousticUndeclaredNumbers

W4-4: the numbers Stage A records for a material name with no declaration. dmap makes such a name an implicit
material (no surface type, not sky, not liquid), which AcousticMaterialPreset gives the generic row; the game's
loader compares a record against this without creating that material.
============
*/
void AcousticUndeclaredNumbers( acousticNumbers_t& out )
{
	out = acousticPresetTable[ACOUSTIC_PRESET_GENERIC];
}

/*
============
AcousticSurfaceTypeName
============
*/
const char* AcousticSurfaceTypeName( int surfaceType )
{
	assert( surfaceType >= 0 && surfaceType < 16 );
	if( surfaceType < 0 || surfaceType >= 16 )
	{
		return "?";
	}
	return acousticSurfaceTypeNames[surfaceType];
}

/*
===============================================================================

	Name sets

===============================================================================
*/

//puncho - SB1a (cost): idHashIndex keeps only a key's low bits, and idStr::Hash is a sum of character times
// position, so names a few characters apart share few buckets; fold the high bits down (the mesh's AcousticMesh_MixKey)
static int AcousticNameSet_Key( int stringHash )
{
	uint32 h = ( uint32 )stringHash;
	h ^= h >> 16;
	h *= 0x85ebca6bu;
	h ^= h >> 13;
	h *= 0xc2b2ae35u;
	h ^= h >> 16;
	return ( int )h;
}

//puncho start - DR-148 (Z3): the chain walk, forced inline into AcousticNameSet_Add (about 21,000 calls per script
// scan) and AcousticNameSet_Contains, so the scan's cost does not depend on how many callers Contains has
static ID_FORCE_INLINE bool AcousticNameSet_Find( const acousticNameSet_t& set, int key, const char* name )
{
	for( int i = set.hash.First( key ); i != -1; i = set.hash.Next( i ) )
	{
		if( !set.names[i].Icmp( name ) )
		{
			return true;
		}
	}
	return false;
}
//puncho end

/*
============
AcousticNameSet_Add

Lower-cased with idStr::ToLower( char ), the case rule of idStr::IHash and Icmp, so Hash of the stored name
equals IHash of any spelling of it. Copied a character at a time: text points into a whole script, and idStr's
range constructor would run strlen over the rest of it for every token.
============
*/
void AcousticNameSet_Add( acousticNameSet_t& set, const char* text, int length )
{
	idStr lowered;
	for( int i = 0; i < length && text[i] != '\0'; i++ )
	{
		lowered.Append( idStr::ToLower( text[i] ) );
	}
	//puncho start - DR-148 (Z3): one key for the lookup and the add; Hash of the lowered name is IHash of it, the key
	// Contains forms
	const int key = AcousticNameSet_Key( idStr::Hash( lowered.c_str() ) );
	if( AcousticNameSet_Find( set, key, lowered.c_str() ) )
	{
		return;
	}
	const int index = set.names.Append( lowered );
	set.hash.Add( key, index );
	//puncho end

	//puncho - SB1a (cost): a script's token set reaches tens of thousands of names; past four names per bucket the
	// table grows fourfold and every name is re-added in index order. Only chains change: Contains answers yes or
	// no and nothing iterates the index, so no order or output follows from it
	if( set.names.Num() > 4 * set.hash.GetHashSize() )
	{
		set.hash.Clear( 4 * set.hash.GetHashSize(), Max( set.names.Num(), 1024 ) );
		for( int i = 0; i < set.names.Num(); i++ )
		{
			set.hash.Add( AcousticNameSet_Key( idStr::Hash( set.names[i].c_str() ) ), i );
		}
	}
}

/*
============
AcousticNameSet_Contains
============
*/
bool AcousticNameSet_Contains( const acousticNameSet_t& set, const char* name )
{
	return AcousticNameSet_Find( set, AcousticNameSet_Key( idStr::IHash( name ) ), name );	//puncho - SB1a (cost), DR-148 (Z3)
}

/*
===============================================================================

	The script walk (R-SB1a-34)

===============================================================================
*/

// the comment that starts at text[i], skipped; i itself when none starts there
static int AcousticSkipComment( const char* text, int length, int i )
{
	if( text[i] != '/' || i + 1 >= length )
	{
		return i;
	}
	if( text[i + 1] == '/' )
	{
		i += 2;
		while( i < length && text[i] != '\n' )
		{
			i++;
		}
		return i;
	}
	if( text[i + 1] == '*' )
	{
		i += 2;
		while( i + 1 < length && !( text[i] == '*' && text[i + 1] == '/' ) )
		{
			i++;
		}
		return Min( i + 2, length );
	}
	return i;
}

// the string or literal whose opening quote is text[i]: the index past the same closing quote, which a newline also
// ends (the script compiler's strings are one line); contentStart and contentLength name its inside
static int AcousticSkipString( const char* text, int length, int i, int& contentStart, int& contentLength )
{
	const char quote = text[i];
	i++;
	contentStart = i;
	while( i < length && text[i] != quote && text[i] != '\n' )
	{
		if( text[i] == '\\' && i + 1 < length && text[i + 1] != '\n' )
		{
			i++;
		}
		i++;
	}
	contentLength = i - contentStart;
	if( i < length && text[i] == quote )
	{
		i++;
	}
	return i;
}

static bool AcousticIsNameChar( char c )
{
	return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '_';
}

/*
============
AcousticSkipSpace

What idLexer::ReadWhiteSpace skips (idlib/Lexer.cpp:319-403): characters <= ' ' (a 0 ends the text, as there) and
both comment forms. crossedLine, when given, is set if a newline was passed, inside a comment too: the lexer's
linesCrossed, which ends a directive line.
============
*/
static int AcousticSkipSpace( const char* text, int length, int i, bool* crossedLine )
{
	while( true )
	{
		while( i < length && text[i] <= ' ' && text[i] != '\0' )
		{
			if( text[i] == '\n' && crossedLine != NULL )
			{
				*crossedLine = true;
			}
			i++;
		}
		const int next = AcousticSkipComment( text, length, i );
		if( next == i )
		{
			return i;
		}
		if( crossedLine != NULL && memchr( text + i, '\n', next - i ) != NULL )
		{
			*crossedLine = true;
		}
		i = next;
	}
}

// the directive name that follows the # at text[i] on the same line, when it is name (a whole name run)
static bool AcousticIsDirective( const char* text, int length, int i, const char* name, int& end )
{
	bool crossedLine = false;
	const int j = AcousticSkipSpace( text, length, i + 1, &crossedLine );
	const int n = idStr::Length( name );
	if( crossedLine || j + n > length || idStr::Cmpn( text + j, name, n ) || ( j + n < length && AcousticIsNameChar( text[j + n] ) ) )
	{
		return false;
	}
	end = j + n;
	return true;
}

/*
============
AcousticTokenSink

R-SB1a-10's S4 tokens, outside comments and '...' literals, taking the safe side of what the script compiler could
resolve (bar B4: more names than the game resolves, never fewer):
- the name run after a $, past blanks and comments, as the compiler reads the next token (Script_Compiler.cpp:947-951);
- every whole "..." string, and the join of strings separated only by blanks or comments, which the compiler's
  lexer reads as one string (Lexer.cpp:672-713; its flags lack LEXFL_NOSTRINGCONCAT);
- every name run in a #define line, its backslash-continued lines included, since a $ name goes through define
  expansion (Parser.cpp:3024-3037) and is not evaluated here.
Stored lower-cased, without the $ or the quotes. Exact tokens, not a substring search.
============
*/
void AcousticTokenSink( void* context, const char* path, const char* text, int length )
{
	acousticNameSet_t& set = *( acousticNameSet_t* )context;
	bool inDefine = false;
	int i = 0;
	while( i < length )
	{
		const char c = text[i];
		if( c == '\n' )
		{
			// a #define line goes on past a newline that a backslash escapes
			const bool continued = ( i > 0 && text[i - 1] == '\\' ) || ( i > 1 && text[i - 1] == '\r' && text[i - 2] == '\\' );
			if( !continued )
			{
				inDefine = false;
			}
			i++;
			continue;
		}
		const int next = AcousticSkipComment( text, length, i );
		if( next != i )
		{
			// a block comment over a newline ends the #define line, as the next token crosses a line
			if( memchr( text + i, '\n', next - i ) != NULL )
			{
				inDefine = false;
			}
			i = next;
			continue;
		}
		if( c == '\'' )
		{
			int start, count;
			i = AcousticSkipString( text, length, i, start, count );
			continue;
		}
		if( c == '"' )
		{
			int start, count;
			i = AcousticSkipString( text, length, i, start, count );
			if( count > 0 )
			{
				AcousticNameSet_Add( set, text + start, count );
			}
			idStr joined( "" );
			for( int k = 0; k < count && text[start + k] != '\0'; k++ )
			{
				joined.Append( text[start + k] );
			}
			int pieces = 1;
			while( true )
			{
				const int k = AcousticSkipSpace( text, length, i, NULL );
				if( k >= length || text[k] != '"' )
				{
					break;
				}
				i = AcousticSkipString( text, length, k, start, count );
				if( count > 0 )
				{
					AcousticNameSet_Add( set, text + start, count );
				}
				for( int m = 0; m < count && text[start + m] != '\0'; m++ )
				{
					joined.Append( text[start + m] );
				}
				pieces++;
			}
			if( pieces > 1 && joined.Length() > 0 )
			{
				AcousticNameSet_Add( set, joined.c_str(), joined.Length() );
			}
			continue;
		}
		if( c == '$' )
		{
			const int start = AcousticSkipSpace( text, length, i + 1, NULL );
			int end = start;
			while( end < length && AcousticIsNameChar( text[end] ) )
			{
				end++;
			}
			if( end > start )
			{
				AcousticNameSet_Add( set, text + start, end - start );
			}
			i = Max( end, i + 1 );
			continue;
		}
		if( c == '#' )
		{
			int end = i;
			if( AcousticIsDirective( text, length, i, "define", end ) )
			{
				inDefine = true;
				i = end;
				continue;
			}
			i++;
			continue;
		}
		if( AcousticIsNameChar( c ) )
		{
			int end = i;
			while( end < length && AcousticIsNameChar( text[end] ) )
			{
				end++;
			}
			if( inDefine )
			{
				AcousticNameSet_Add( set, text + i, end - i );
			}
			i = end;
			continue;
		}
		i++;
	}
}

/*
============
AcousticLowerGamePath

The sink's spelling of a game path, and the seen set's of a full path: forward slashes, lower case.
============
*/
static void AcousticLowerGamePath( idStr& path )
{
	path.BackSlashesToSlashes();
	for( int i = 0; i < path.Length(); i++ )
	{
		path[i] = idStr::ToLower( path[i] );
	}
}

// the walk's state, one per AcousticScriptWalk call
struct acousticScriptWalk_t
{
	const acousticScriptSink_t*	sink;
	acousticNameSet_t			seen;			// lower-cased full paths of the files read
	bool						throughLayers;	// the script compiler's layer mode
	bool						depthWarned;
};

// a script include chain deeper than this is not followed: two files including each other under different
// spellings of their paths would otherwise recurse until the OS refuses the path, one file buffer per level
static const int ACOUSTIC_SCRIPT_MAX_DEPTH = 64;

static void AcousticScriptVisit_r( acousticScriptWalk_t& walk, idFile* f, const char* parserName, const char* gamePath, bool root, int depth );

/*
============
AcousticScriptIncludeOpened_r

One resolved #include: read once per lower-cased resolved path, the file's full path, so two files the game tells
apart (a loose copy opened by OS path, a packed one of the same game path) are both read. Takes f and closes it.
The key lives only in memory and decides nothing about order, so no machine path reaches an output.
============
*/
static void AcousticScriptIncludeOpened_r( acousticScriptWalk_t& walk, idFile* f, const char* gamePath, int depth )
{
	if( depth > ACOUSTIC_SCRIPT_MAX_DEPTH )
	{
		if( !walk.depthWarned )
		{
			walk.depthWarned = true;
			common->Warning( "acoustics: script includes nest deeper than %i at '%s'; not followed further", ACOUSTIC_SCRIPT_MAX_DEPTH, gamePath );
		}
		fileSystem->CloseFile( f );
		return;
	}

	// idLexer::LoadFile names an included file by fp->GetFullPath() (idlib/Lexer.cpp:2254); its own #includes
	// resolve against that name
	const idStr parserName = f->GetFullPath();
	idStr key = parserName;
	AcousticLowerGamePath( key );
	if( AcousticNameSet_Contains( walk.seen, key.c_str() ) )
	{
		fileSystem->CloseFile( f );
		return;
	}
	AcousticNameSet_Add( walk.seen, key.c_str(), key.Length() );

	AcousticScriptVisit_r( walk, f, parserName.c_str(), gamePath, false, depth );
}

/*
============
AcousticScriptInclude_r

#include "<token>" as idParser::Directive_include resolves it (idlib/Parser.cpp:1147-1195) for the script compiler's
parser (d3xp/script/Script_Compiler.cpp:3253: LEXFL_INCLUDETHROUGHLAYERS exactly when there is more than one
content layer; the parser's OSPath is false and no include path is ever set on it). The first candidate that opens
is the file:
	one layer:	(1) next to the including file by the lexer's name, opened as an explicit OS path (:1150-1166);
				(2) the path as given, through the file system (:1168-1170)
	layers:		(1) the path as given (:1168-1170); (2) next to the including file by its relative path (:1173-1183)
Then includepath + token (:1186-1194), which with no include path is the path as given again. The same file system
calls as the game's, on the same names, so dmap, rbdmap and the game resolve alike.
============
*/
static void AcousticScriptInclude_r( acousticScriptWalk_t& walk, const char* token, const char* includerName, const char* includerGamePath, int depth )
{
	// the token's own directory, stripped once off the includer's directory when it ends with it (:1155-1156)
	idStr tokenPath = token;
	tokenPath.StripFilename();

	if( !walk.throughLayers )
	{
		idStr path = includerName;
		path.StripFilename();
		if( !path.StripTrailingOnce( tokenPath ) )
		{
			path += "/";
		}
		path += token;
		idFile* f = fileSystem->OpenExplicitFileRead( path.c_str() );
		if( f != NULL )
		{
			// the same file named by the game path: the includer's game directory, joined the same way
			idStr gamePath = includerGamePath;
			gamePath.StripFilename();
			if( !gamePath.StripTrailingOnce( tokenPath ) )
			{
				gamePath += "/";
			}
			gamePath += token;
			AcousticScriptIncludeOpened_r( walk, f, gamePath.c_str(), depth );
			return;
		}
	}

	idFile* f = fileSystem->OpenFileRead( token );
	if( f != NULL )
	{
		AcousticScriptIncludeOpened_r( walk, f, token, depth );
		return;
	}

	if( walk.throughLayers )
	{
		idStr path = fileSystem->OSPathToRelativePath( includerName );
		path.StripFilename();
		if( !path.StripTrailingOnce( tokenPath ) )
		{
			path += "/";
		}
		path += token;
		f = fileSystem->OpenFileRead( path.c_str() );
		if( f != NULL )
		{
			AcousticScriptIncludeOpened_r( walk, f, path.c_str(), depth );
		}
	}
}

/*
============
AcousticScriptVisit_r

Reads f (and closes it), hands its whole text to the sink, then follows its #include lines depth-first where they
stand. A root first follows the #include "script/doom_defs.script" that idCompiler::CompileFile puts before every
compiled file's text (SCRIPT_DEFAULTDEFS, Script_Compiler.cpp:3260-3276). Conditionals are not evaluated: every
#include in the text is followed, in a block the game skips or not, which can only read more than the game.
A # starts a directive anywhere outside comments, strings and literals (idParser::ReadDirective, Parser.cpp:2757);
the directive name and its argument must stand on the # line, past blanks and comments (:2767-2773, :1142-1146).
#include <path> loads includepath + path with the parser's OSPath, false (Parser.cpp:1197-1231), so the walk opens
the path between the brackets, its blanks and comments dropped as the parser's token concatenation drops them.
============
*/
static void AcousticScriptVisit_r( acousticScriptWalk_t& walk, idFile* f, const char* parserName, const char* gamePath, bool root, int depth )
{
	const int length = f->Length();
	idTempArray<char> buffer( length + 1 );
	const int read = f->Read( buffer.Ptr(), length );
	fileSystem->CloseFile( f );
	const int textLength = Max( 0, Min( read, length ) );
	buffer[textLength] = '\0';
	const char* text = buffer.Ptr();

	idStr lowered = gamePath;
	AcousticLowerGamePath( lowered );
	walk.sink->file( walk.sink->context, lowered.c_str(), text, textLength );

	if( root )
	{
		// SCRIPT_DEFAULTDEFS of d3xp/Game.h, which dmap does not include
		AcousticScriptInclude_r( walk, "script/doom_defs.script", parserName, gamePath, depth + 1 );
	}

	int i = 0;
	while( i < textLength )
	{
		const int next = AcousticSkipComment( text, textLength, i );
		if( next != i )
		{
			i = next;
			continue;
		}
		const char c = text[i];
		if( c == '"' || c == '\'' )
		{
			// strings, and the script's '...' vector and character literals, hide a # or a comment opener
			int start, count;
			i = AcousticSkipString( text, textLength, i, start, count );
			continue;
		}
		int j = i;
		if( c != '#' || !AcousticIsDirective( text, textLength, i, "include", j ) )
		{
			i++;
			continue;
		}
		bool crossedLine = false;
		j = AcousticSkipSpace( text, textLength, j, &crossedLine );
		if( crossedLine )
		{
			// "#include without file name" in the game; nothing to follow
			i++;
			continue;
		}
		if( j < textLength && text[j] == '"' )
		{
			int start, count;
			j = AcousticSkipString( text, textLength, j, start, count );
			if( count > 0 )
			{
				idStr include;
				for( int k = 0; k < count && text[start + k] != '\0'; k++ )
				{
					include.Append( text[start + k] );
				}
				AcousticScriptInclude_r( walk, include.c_str(), parserName, gamePath, depth + 1 );
			}
		}
		else if( j < textLength && text[j] == '<' )
		{
			j++;
			idStr include;
			while( j < textLength && text[j] != '>' && text[j] != '\n' )
			{
				const int afterComment = AcousticSkipComment( text, textLength, j );
				if( afterComment != j )
				{
					if( memchr( text + j, '\n', afterComment - j ) != NULL )
					{
						break;
					}
					j = afterComment;
					continue;
				}
				if( text[j] > ' ' )
				{
					include.Append( text[j] );
				}
				j++;
			}
			if( j < textLength && text[j] == '>' )
			{
				j++;
			}
			if( include.Length() > 0 )
			{
				idFile* angle = fileSystem->OpenFileRead( include.c_str() );
				if( angle != NULL )
				{
					AcousticScriptIncludeOpened_r( walk, angle, include.c_str(), depth + 1 );
				}
			}
		}
		i = Max( j, i + 1 );
	}
}

/*
============
AcousticScriptRoot

One file the game compiles on its own, under the name the game's lexer gives it: CompileText's
RelativePathToOSPath( source ) for doom_main and the map script (Script_Program.cpp:2195-2203), and
"<layer name>/script/mod_main.script" for a layer's mod_main (:2412). A root is read whatever was seen before,
since the game compiles each root; its full path still enters the seen set.
============
*/
static void AcousticScriptRoot( acousticScriptWalk_t& walk, const char* gamePath, int layer )
{
	idFile* f;
	idStr parserName;
	if( layer >= 0 )
	{
		f = fileSystem->OpenFileReadFromLayer( gamePath, layer );
		parserName = va( "%s/%s", fileSystem->GetLayerName( layer ), gamePath );
	}
	else
	{
		f = fileSystem->OpenFileRead( gamePath );
		parserName = fileSystem->RelativePathToOSPath( gamePath );
	}
	if( f == NULL )
	{
		return;
	}
	idStr key = f->GetFullPath();
	AcousticLowerGamePath( key );
	AcousticNameSet_Add( walk.seen, key.c_str(), key.Length() );
	AcousticScriptVisit_r( walk, f, parserName.c_str(), gamePath, true, 0 );
}

/*
============
AcousticScriptWalk

The order the game compiles its scripts in (idProgram::Startup, then idWorldspawn::Spawn). No directory is listed:
every file is reached by a name the text gives, in the text's order. mapName is idMapFile::GetName(), and the map
script's name is formed from it exactly as idWorldspawn::Spawn forms it from gameLocal.GetMapName(), which is that
same GetName() (d3xp/Game_local.cpp:1064, d3xp/WorldSpawn.cpp:84-85).
============
*/
void AcousticScriptWalk( const char* mapName, const acousticScriptSink_t& sink )
{
	acousticScriptWalk_t walk;
	walk.sink = &sink;
	// the script compiler's layer mode, read as it reads it (Script_Compiler.cpp:3253)
	walk.throughLayers = ( fileSystem->GetNumLayers() > 1 );
	walk.depthWarned = false;

	// SCRIPT_DEFAULT of d3xp/Game.h
	AcousticScriptRoot( walk, "script/doom_main.script", -1 );

	for( int layer = 0; layer < fileSystem->GetNumLayers(); layer++ )
	{
		AcousticScriptRoot( walk, "script/mod_main.script", layer );
	}

	idStr mapScript = mapName;
	mapScript.SetFileExtension( ".script" );
	AcousticScriptRoot( walk, mapScript.c_str(), -1 );
}

/*
===============================================================================

	Record CRCs (R-SB1a-32)

===============================================================================
*/

// a uint32's bytes low byte first, whatever the host's order
static void AcousticCRCUpdateUInt32( unsigned int& crc, uint32 value )
{
	const byte bytes[4] = { ( byte )( value & 0xff ), ( byte )( ( value >> 8 ) & 0xff ), ( byte )( ( value >> 16 ) & 0xff ), ( byte )( ( value >> 24 ) & 0xff ) };
	CRC32_UpdateChecksum( crc, bytes, 4 );
}

static void AcousticCRCUpdateFloat( unsigned int& crc, float value )
{
	uint32 bits;
	memcpy( &bits, &value, sizeof( bits ) );
	AcousticCRCUpdateUInt32( crc, bits );
}

/*
============
AcousticScriptCRC
============
*/
uint32 AcousticScriptCRC( const acousticNameSet_t& tokens, const idMapFile& mapFile, const uint32* kinds, int numKinds )
{
	assert( numKinds <= mapFile.GetNumEntities() );
	const int count = Min( numKinds, mapFile.GetNumEntities() );
	//puncho start - DR-148: a sum of the names' CRCs, so neither the entities' indices nor their order enter it
	uint32 sum = 0;
	for( int i = 0; i < count; i++ )
	{
		if( kinds[i] != 1 && kinds[i] != 2 )
		{
			continue;
		}
		// Y2: only a static's name can change a bake (Stage A's S4 test is in the func_static / misc_model branch)
		const idDict& epairs = mapFile.GetEntity( i )->epairs;
		const char* classname = epairs.GetString( "classname" );
		if( idStr::Icmp( classname, "func_static" ) != 0 && idStr::Icmp( classname, "misc_model" ) != 0 )
		{
			continue;
		}
		const char* name = epairs.GetString( "name" );
		if( name[0] != '\0' && AcousticNameSet_Contains( tokens, name ) )
		{
			// the name lower-cased as the name sets hold it, then its 0
			unsigned int crc;
			CRC32_InitChecksum( crc );
			for( const char* c = name; ; c++ )
			{
				const char lower = idStr::ToLower( *c );
				CRC32_UpdateChecksum( crc, &lower, 1 );
				if( *c == '\0' )
				{
					break;
				}
			}
			CRC32_FinishChecksum( crc );
			sum += ( uint32 )crc;
		}
	}
	return sum;
	//puncho end
}

/*
============
AcousticKind0CRC

DR-148 (Y1): 0 means an entity the game lets come, go and change: it names no static candidate, has no model key, is
not a func_static or misc_model and is not of a candidate's class (a plain light, a speaker). Every other entity
keeps a CRC that holds its classname. The model key, not the primitives: it is in the key/values on both sides, where
primitives are not (the game strips a map-only helper's). A CRC that comes out 0 is returned as 1.
Z1: a func_static or misc_model with a model key adds one byte 1 after its classname, so it never takes the CRC of
an inert one (no model key) and the reverse. Stage A never computes this form: a static with a model key is never
kind 0 (AcousticMesh_Decide), unless its class's own entityDef is a map-only helper (dmapOnly), which func_static and
misc_model are not; then both sides add the byte and an unchanged map still passes. Otherwise only an unclaimed map
entity in the game computes it, and it then matches no record.
============
*/
uint32 AcousticKind0CRC( const idDict& epairs, const acousticNameSet_t& staticNames, const acousticNameSet_t& candidateClasses )
{
	unsigned int crc;
	CRC32_InitChecksum( crc );
	const char* classname = epairs.GetString( "classname" );
	CRC32_UpdateChecksum( crc, classname, idStr::Length( classname ) + 1 );
	//puncho start - DR-148 (Y1, Z1)
	const bool model = ( epairs.GetString( "model" )[0] != '\0' );
	const bool staticClass = !idStr::Icmp( classname, "func_static" ) || !idStr::Icmp( classname, "misc_model" );
	if( staticClass && model )
	{
		static const byte modelStatic = 1;
		CRC32_UpdateChecksum( crc, &modelStatic, 1 );
	}
	bool held = model || staticClass || AcousticNameSet_Contains( candidateClasses, classname );
	//puncho end
	for( int k = 0; k < epairs.GetNumKeyVals(); k++ )
	{
		const idStr& value = epairs.GetKeyVal( k )->GetValue();
		if( AcousticNameSet_Contains( staticNames, value.c_str() ) )
		{
			CRC32_UpdateChecksum( crc, value.c_str(), value.Length() + 1 );
			held = true;	//puncho - DR-148
		}
	}
	//puncho start - DR-148
	if( !held )
	{
		return 0;
	}
	CRC32_FinishChecksum( crc );
	return ( crc != 0 ) ? ( uint32 )crc : 1;
	//puncho end
}

/*
============
AcousticEntityKeysCRC
============
*/
uint32 AcousticEntityKeysCRC( const idList<acousticEntity_t>& entities )
{
	unsigned int crc;
	CRC32_InitChecksum( crc );
	for( int i = 0; i < entities.Num(); i++ )
	{
		AcousticCRCUpdateUInt32( crc, entities[i].kind );
		AcousticCRCUpdateUInt32( crc, entities[i].crc );
	}
	CRC32_FinishChecksum( crc );
	return crc;
}

/*
============
AcousticMaterialHash
============
*/
uint32 AcousticMaterialHash( const idList<acousticMaterial_t>& materials )
{
	unsigned int crc;
	CRC32_InitChecksum( crc );
	for( int i = 0; i < materials.Num(); i++ )
	{
		const acousticMaterial_t& material = materials[i];
		CRC32_UpdateChecksum( crc, material.name.c_str(), material.name.Length() + 1 );
		for( int b = 0; b < 3; b++ )
		{
			AcousticCRCUpdateFloat( crc, material.n.absorption[b] );
		}
		AcousticCRCUpdateFloat( crc, material.n.scattering );
		for( int b = 0; b < 3; b++ )
		{
			AcousticCRCUpdateFloat( crc, material.n.transmission[b] );
		}
	}
	CRC32_FinishChecksum( crc );
	return crc;
}
