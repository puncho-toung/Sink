/* //puncho - SB0: helper of build_phonon.cmd (WP-SB0 R-SB0-13). Loads a phonon DLL by path and prints
   iplLocalPatchLevel(), or "missing" when the DLL has no such export (an unpatched build).
   Exit code: 0 printed a level, 1 missing, 2 the DLL did not load. */
#include <windows.h>
#include <stdio.h>

typedef unsigned int ( __stdcall * patchLevel_t )( void );

int main( int argc, char ** argv )
{
	HMODULE dll;
	patchLevel_t level;

	if( argc != 2 )
	{
		printf( "usage: patch_level <phonon dll>\n" );
		return 2;
	}
	dll = LoadLibraryA( argv[1] );
	if( dll == NULL )
	{
		printf( "load failed %lu\n", GetLastError() );
		return 2;
	}
	level = ( patchLevel_t )GetProcAddress( dll, "iplLocalPatchLevel" );
	if( level == NULL )
	{
		printf( "missing\n" );
		FreeLibrary( dll );
		return 1;
	}
	printf( "0x%x\n", level() );
	FreeLibrary( dll );
	return 0;
}
