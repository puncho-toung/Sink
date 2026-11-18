//puncho - SB1a (R-SB1a-37): the acoustic functions dmap's Stage A and the game share
#ifndef __ACOUSTIC_SHARED_H__
#define __ACOUSTIC_SHARED_H__

/*
===============================================================================

	What dmap's Stage A and the game must compute alike: a material's acoustic numbers, the script walk and the
	record CRCs the game's working-tree check repeats. idLib only: idStr, idList, idHashIndex and the sized
	integers are supplied before inclusion (precompiled.h); no dmap or renderer header is included, the types
	below are only named. The bodies are in acoustic_material.cpp, linked in the engine and in rbdmap.

===============================================================================
*/

class idMaterial;
class idMapFile;
class idDict;

// 400 Hz, 2.5 kHz, 15 kHz bands
struct acousticNumbers_t
{
	float				absorption[3];
	float				scattering;
	float				transmission[3];
};																					// 28 B

// lower-cased, unique; hash key idStr::Hash of the lower-cased name
struct acousticNameSet_t
{
	idHashIndex			hash;
	idList<idStr>		names;
};

// lower-cases; a repeat is ignored
void	AcousticNameSet_Add( acousticNameSet_t& set, const char* text, int length );
// case-insensitive, confirmed by Icmp
bool	AcousticNameSet_Contains( const acousticNameSet_t& set, const char* name );

// called once per file, synchronously on the caller's thread: path is the lower-cased game path, text the file's
// length bytes followed by a 0, both valid only during the call; the sink cannot stop the walk
typedef void ( *acousticScriptFileFn_t )( void* context, const char* path, const char* text, int length );
struct acousticScriptSink_t
{
	acousticScriptFileFn_t	file;
	void*					context;
};

// context: acousticNameSet_t*; outside comments and '...' literals adds the [A-Za-z0-9_] run after each $ (past blanks
// and comments), every whole "..." string and the join of strings separated only by blanks or comments, and every
// name run in a #define line: more names than the game resolves, never fewer
void	AcousticTokenSink( void* context, const char* path, const char* text, int length );

// the game's compile order: script/doom_main.script, each layer's script/mod_main.script read from that layer, then
// the map's .script, each preceded by the compiler's implicit script/doom_defs.script and followed depth-first by its
// #include lines (outside comments, in conditional blocks too), each resolved as the script compiler's idParser
// resolves it; a resolved file seen before (lower-cased full path) or missing is skipped. mapName is exactly
// idMapFile::GetName() of the map (in dmap dmapGlobals.dmapFile->GetName(), in the game gameLocal.GetMapName()); the
// walk sets its extension to .script as idWorldspawn::Spawn does and changes nothing else
void	AcousticScriptWalk( const char* mapName, const acousticScriptSink_t& sink );

//puncho start - DR-148: no entity index and no entity order enters a record CRC, so the game matches by content
// the 32-bit sum, over the func_static and misc_model entities i < numKinds whose kinds[i] is 1 or 2 and whose non-empty
// name is in tokens, of the CRC32 of that name lower-cased with its 0; 0 when there is none
uint32	AcousticScriptCRC( const acousticNameSet_t& tokens, const idMapFile& mapFile, const uint32* kinds, int numKinds );

// staticNames: the names of the kind 1 and 2 func_static and misc_model entities; candidateClasses: the classnames of
// every kind 1 and 2 entity. 0 when no value of epairs is in staticNames, its model key is empty, its classname is
// neither func_static nor misc_model and not in candidateClasses; else CRC32 over the classname's bytes and its 0,
// then one byte 1 when the class is func_static or misc_model and the model key is not empty (Z1; never kind 0 in
// Stage A), then, in epairs order, each value in staticNames, its bytes and a 0, a computed 0 returned as 1
uint32	AcousticKind0CRC( const idDict& epairs, const acousticNameSet_t& staticNames, const acousticNameSet_t& candidateClasses );
//puncho end

// R-SB1a-21: the numbers depend on the material alone: sky, liquid (contents or type), glass (type), else the
// surface-type table. false: m is NULL
bool	AcousticResolveNumbers( const idMaterial* m, acousticNumbers_t& out );

// W4-4: the numbers Stage A records for a material name with no declaration: its implicit material's, the generic row
void	AcousticUndeclaredNumbers( acousticNumbers_t& out );

#endif /* !__ACOUSTIC_SHARED_H__ */
