/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

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

#include "precompiled.h"
#pragma hdrstop

#include "Common_local.h"

idCVar com_logFile( "logFile", "0", CVAR_SYSTEM | CVAR_NOCHEAT, "1 = buffer log, 2 = flush after each print", 0, 2, idCmdSystem::ArgCompletion_Integer<0, 2> );
idCVar com_logFileName( "logFileName", "qconsole.log", CVAR_SYSTEM | CVAR_NOCHEAT, "name of log file, if empty, qconsole.log will be used" );
idCVar com_timestampPrints( "com_timestampPrints", "0", CVAR_SYSTEM, "print time with each console print, 1 = msec, 2 = sec", 0, 2, idCmdSystem::ArgCompletion_Integer<0, 2> );

// Fixed storage: worker diagnostics must not allocate or touch console/filesystem state.
idCVar com_printRelay( "com_printRelay", "1", CVAR_SYSTEM | CVAR_BOOL, "relay worker diagnostics to the console and logfile" );
static const int PRINT_RELAY_LINES = 256;
static const int PRINT_RELAY_LINE_BYTES = 1024;
struct printRelayLine_t
{
	char text[PRINT_RELAY_LINE_BYTES];
	bool warning;
};
static printRelayLine_t printRelay[PRINT_RELAY_LINES];
static idSysMutex printRelayMutex;
static int printRelayHead, printRelayCount, printRelayDropped, printRelayTruncated;
static bool printRelayDraining;

static void QueuePrintRelay( const char* text, bool warning )
{
	printRelayMutex.Lock();
	if( printRelayCount == PRINT_RELAY_LINES )
	{
		printRelayDropped++;
		printRelayMutex.Unlock();
		//puncho - print-relay (P2 review): plain printf, as stock wrote worker lines; posix Sys_Printf's tty_Hide and
		// tty_Show would race the main console. A warning has no newline until Warning prints it at drain time
		printf( warning ? "WARNING: %s\n" : "%s", text );
		return;
	}
	printRelayLine_t& line = printRelay[( printRelayHead + printRelayCount ) % PRINT_RELAY_LINES];
	const size_t length = strlen( text );
	idStr::Copynz( line.text, text, sizeof( line.text ) );
	if( length >= PRINT_RELAY_LINE_BYTES )
	{
		printRelayTruncated++;
		// a cut line keeps its end of line, or it runs into the next one (P2 review)
		if( text[length - 1] == '\n' )
		{
			line.text[PRINT_RELAY_LINE_BYTES - 2] = '\n';
		}
	}
	line.warning = warning;
	printRelayCount++;
	printRelayMutex.Unlock();
}

//puncho start - print-relay (P2 review): idSysThread::ThreadProc calls this when a worker's exception ends the
// process. stdout only, with plain printf: the console and the log file belong to the main thread
void DumpPrintRelay()
{
	printRelayMutex.Lock();
	for( ; printRelayCount > 0; printRelayCount-- )
	{
		const printRelayLine_t& line = printRelay[printRelayHead];
		printf( line.warning ? "WARNING: %s\n" : "%s", line.text );
		printRelayHead = ( printRelayHead + 1 ) % PRINT_RELAY_LINES;
	}
	printRelayMutex.Unlock();
	fflush( stdout );
}
//puncho end

void idCommonLocal::DrainPrintRelay()
{
	if( !idLib::IsMainThread() || printRelayDraining )
	{
		return;
	}
	printRelayDraining = true;
	for( ;; )
	{
		printRelayLine_t line;
		printRelayMutex.Lock();
		if( printRelayCount == 0 )
		{
			const int dropped = printRelayDropped;
			const int truncated = printRelayTruncated;
			printRelayDropped = printRelayTruncated = 0;
			printRelayMutex.Unlock();
			if( dropped || truncated )
			{
				Printf( "print relay: %d lines dropped to stdout, %d truncated\n", dropped, truncated );
			}
			break;
		}
		line = printRelay[printRelayHead];
		printRelayHead = ( printRelayHead + 1 ) % PRINT_RELAY_LINES;
		printRelayCount--;
		printRelayMutex.Unlock();
		if( line.warning )
		{
			Warning( "%s", line.text );
		}
		else
		{
			Printf( "%s", line.text );
		}
	}
	printRelayDraining = false;
}

#ifndef ID_RETAIL
	idCVar com_printFilter( "com_printFilter", "", CVAR_SYSTEM, "only print lines that contain this, add multiple filters with a ; delimeter" );
#endif

/*
==================
idCommonLocal::BeginRedirect
==================
*/
void idCommonLocal::BeginRedirect( char* buffer, int buffersize, void ( *flush )( const char* ) )
{
	if( !buffer || !buffersize || !flush )
	{
		return;
	}
	rd_buffer = buffer;
	rd_buffersize = buffersize;
	rd_flush = flush;

	*rd_buffer = 0;
}

/*
==================
idCommonLocal::EndRedirect
==================
*/
void idCommonLocal::EndRedirect()
{
	if( rd_flush && rd_buffer[ 0 ] )
	{
		rd_flush( rd_buffer );
	}

	rd_buffer = NULL;
	rd_buffersize = 0;
	rd_flush = NULL;
}

/*
==================
idCommonLocal::CloseLogFile
==================
*/
void idCommonLocal::CloseLogFile()
{
	DrainPrintRelay();
	CloseMapRowsLog();
	if( logFile )
	{
		com_logFile.SetBool( false ); // make sure no further VPrintf attempts to open the log file again
		fileSystem->CloseFile( logFile );
		logFile = NULL;
	}
}

/*
==================
idCommonLocal::SetRefreshOnPrint
==================
*/
void idCommonLocal::SetRefreshOnPrint( bool set )
{
	com_refreshOnPrint = set;
}

/*
==================
idCommonLocal::VPrintf

A raw string should NEVER be passed as fmt, because of "%f" type crashes.
==================
*/
void idCommonLocal::VPrintf( const char* fmt, va_list args )
{
	static bool	logFileFailed = false;

	// if the cvar system is not initialized
	if( !cvarSystem->IsInitialized() )
	{
		return;
	}
	// optionally put a timestamp at the beginning of each print,
	// so we can see how long different init sections are taking
	int timeLength = 0;
	char msg[MAX_PRINT_MSG_SIZE];
	msg[ 0 ] = '\0';
	if( com_timestampPrints.GetInteger() && !( idLib::IsMainThread() && printRelayDraining ) )
	{
		int	t = Sys_Milliseconds();
		if( com_timestampPrints.GetInteger() == 1 )
		{
			idStr::snPrintf( msg, sizeof( msg ), "[%5.2f]", t * 0.001f );
		}
		else
		{
			idStr::snPrintf( msg, sizeof( msg ), "[%i]", t );
		}
	}
	timeLength = strlen( msg );
	// don't overflow
	if( idStr::vsnPrintf( msg + timeLength, MAX_PRINT_MSG_SIZE - timeLength - 1, fmt, args ) < 0 )
	{
		msg[sizeof( msg ) - 2] = '\n';
		msg[sizeof( msg ) - 1] = '\0'; // avoid output garbling
		Sys_Printf( "idCommon::VPrintf: truncated to %d characters\n", strlen( msg ) - 1 );
	}

	if( !idLib::IsMainThread() && com_printRelay.GetBool() )
	{
		QueuePrintRelay( msg, false );
		return;
	}

	if( rd_buffer )
	{
		if( ( int )( strlen( msg ) + strlen( rd_buffer ) ) > ( rd_buffersize - 1 ) )
		{
			rd_flush( rd_buffer );
			*rd_buffer = 0;
		}
		strcat( rd_buffer, msg );
		return;
	}
#ifndef ID_RETAIL
	if( com_printFilter.GetString() != NULL && com_printFilter.GetString()[ 0 ] != '\0' )
	{
		idStrStatic< 4096 > filterBuf = com_printFilter.GetString();
		idStrStatic< 4096 > msgBuf = msg;
		filterBuf.ToLower();
		msgBuf.ToLower();
		char* sp = strtok( &filterBuf[ 0 ], ";" );
		bool p = false;
		for( ; sp != NULL ; )
		{
			if( strstr( msgBuf, sp ) != NULL )
			{
				p = true;
				break;
			}
			sp = strtok( NULL, ";" );
		}
		if( !p )
		{
			return;
		}
	}
#endif


	if( !idLib::IsMainThread() )
	{
		// RB: printf should be thread-safe on Linux
#if defined(_WIN32)
		OutputDebugString( msg );
#else
		printf( "%s", msg );
#endif
		// RB end
		return;
	}

	// echo to console buffer
	console->Print( msg );

	// remove any color codes
	idStr::RemoveColors( msg );

	// echo to dedicated console and early console
	Sys_Printf( "%s", msg );

	// print to script debugger server
	// DebuggerServerPrint( msg );

#if 0	// !@#
#if defined(_DEBUG) && defined(WIN32)
	if( strlen( msg ) < 512 )
	{
		TRACE( msg );
	}
#endif
#endif

	// logFile
	if( com_logFile.GetInteger() && !logFileFailed && fileSystem->IsInitialized() )
	{
		static bool recursing;

		if( !logFile && !recursing )
		{
			const char* fileName = com_logFileName.GetString()[0] ? com_logFileName.GetString() : "qconsole.log";

			// fileSystem->OpenFileWrite can cause recursive prints into here
			recursing = true;

			logFile = fileSystem->OpenFileWrite( fileName );
			if( !logFile )
			{
				logFileFailed = true;
				FatalError( "failed to open log file '%s'\n", fileName );
			}

			recursing = false;

			if( com_logFile.GetInteger() > 1 )
			{
				// force it to not buffer so we get valid
				// data even if we are crashing
				logFile->ForceFlush();
			}

			time_t aclock;
			time( &aclock );
			struct tm* newtime = localtime( &aclock );
			Printf( "log file '%s' opened on %s\n", fileName, asctime( newtime ) );

			// print engine version
			Printf( "%s\n", com_version.GetString() );
		}
		if( logFile )
		{
			logFile->Write( msg, strlen( msg ) );
			logFile->Flush();	// ForceFlush doesn't help a whole lot
		}
	}

	// don't trigger any updates if we are in the process of doing a fatal error
	if( com_errorEntered != ERP_FATAL )
	{
		// update the console if we are in a long-running command, like dmap
		if( com_refreshOnPrint )
		{
			const bool captureToImage = false;
			UpdateScreen( captureToImage );
		}
	}
}

/*
==================
idCommonLocal::Printf

Both client and server can use this, and it will output to the appropriate place.

A raw string should NEVER be passed as fmt, because of "%f" type crashers.
==================
*/
void idCommonLocal::Printf( const char* fmt, ... )
{
	va_list argptr;
	va_start( argptr, fmt );
	VPrintf( fmt, argptr );
	va_end( argptr );
}

/*
==================
idCommonLocal::Printf

RB: only for dmap and AAS builder
==================
*/
void idCommonLocal::VerbosePrintf( const char* fmt, ... )
{
	if( !dmap_verbose.GetBool() )
	{
		return;
	}

	va_list argptr;
	va_start( argptr, fmt );
	VPrintf( fmt, argptr );
	va_end( argptr );
}

/*
==================
idCommonLocal::DPrintf

prints message that only shows up if the "developer" cvar is set
==================
*/
void idCommonLocal::DPrintf( const char* fmt, ... )
{
	va_list		argptr;
	char		msg[MAX_PRINT_MSG_SIZE];

	if( !cvarSystem->IsInitialized() || !com_developer.GetBool() )
	{
		return;			// don't confuse non-developers with techie stuff...
	}

	va_start( argptr, fmt );
	idStr::vsnPrintf( msg, sizeof( msg ), fmt, argptr );
	va_end( argptr );
	msg[sizeof( msg ) - 1] = '\0';

	// never refresh the screen, which could cause reentrency problems
	if( !idLib::IsMainThread() )
	{
		Printf( S_COLOR_RED "%s", msg );
		return;
	}
	bool temp = com_refreshOnPrint;
	com_refreshOnPrint = false;

	Printf( S_COLOR_RED"%s", msg );

	com_refreshOnPrint = temp;
}

/*
==================
idCommonLocal::DWarning

prints warning message in yellow that only shows up if the "developer" cvar is set
==================
*/
void idCommonLocal::DWarning( const char* fmt, ... )
{
	va_list		argptr;
	char		msg[MAX_PRINT_MSG_SIZE];

	if( !com_developer.GetBool() )
	{
		return;			// don't confuse non-developers with techie stuff...
	}

	va_start( argptr, fmt );
	idStr::vsnPrintf( msg, sizeof( msg ), fmt, argptr );
	va_end( argptr );
	msg[sizeof( msg ) - 1] = '\0';

	Printf( S_COLOR_YELLOW"WARNING: %s\n", msg );
}

/*
==================
idCommonLocal::Warning

prints WARNING %s and adds the warning message to a queue to be printed later on
==================
*/
void idCommonLocal::Warning( const char* fmt, ... )
{
	va_list		argptr;
	char		msg[MAX_PRINT_MSG_SIZE];

	va_start( argptr, fmt );
	idStr::vsnPrintf( msg, sizeof( msg ), fmt, argptr );
	va_end( argptr );
	msg[sizeof( msg ) - 1] = 0;

	if( !idLib::IsMainThread() )
	{
		if( com_printRelay.GetBool() )
		{
			QueuePrintRelay( msg, true );
		}
		return;
	}

	Printf( S_COLOR_YELLOW "WARNING: " S_COLOR_RED "%s\n", msg );

	if( warningList.Num() < MAX_WARNING_LIST )
	{
		warningList.AddUnique( msg );
	}
}

/*
==================
idCommonLocal::PrintWarnings
==================
*/
void idCommonLocal::PrintWarnings()
{
	int i;

	if( !warningList.Num() )
	{
		return;
	}

	Printf( "------------- Warnings ---------------\n" );
	Printf( "during %s...\n", warningCaption.c_str() );

	for( i = 0; i < warningList.Num(); i++ )
	{
		Printf( S_COLOR_YELLOW "WARNING: " S_COLOR_RED "%s\n", warningList[i].c_str() );
	}
	if( warningList.Num() )
	{
		if( warningList.Num() >= MAX_WARNING_LIST )
		{
			Printf( "more than %d warnings\n", MAX_WARNING_LIST );
		}
		else
		{
			Printf( "%d warnings\n", warningList.Num() );
		}
	}
}

/*
==================
idCommonLocal::ClearWarnings
==================
*/
void idCommonLocal::ClearWarnings( const char* reason )
{
	warningCaption = reason;
	warningList.Clear();
}

/*
==================
idCommonLocal::DumpWarnings
==================
*/
void idCommonLocal::DumpWarnings()
{
	int			i;
	idFile*		warningFile;

	if( !warningList.Num() )
	{
		return;
	}

	warningFile = fileSystem->OpenFileWrite( "warnings.txt", "fs_savepath" );
	if( warningFile )
	{

		warningFile->Printf( "------------- Warnings ---------------\n\n" );
		warningFile->Printf( "during %s...\n", warningCaption.c_str() );
		for( i = 0; i < warningList.Num(); i++ )
		{
			warningList[i].RemoveColors();
			warningFile->Printf( "WARNING: %s\n", warningList[i].c_str() );
		}
		if( warningList.Num() >= MAX_WARNING_LIST )
		{
			warningFile->Printf( "\nmore than %d warnings!\n", MAX_WARNING_LIST );
		}
		else
		{
			warningFile->Printf( "\n%d warnings.\n", warningList.Num() );
		}

		warningFile->Printf( "\n\n-------------- Errors ---------------\n\n" );
		for( i = 0; i < errorList.Num(); i++ )
		{
			errorList[i].RemoveColors();
			warningFile->Printf( "ERROR: %s", errorList[i].c_str() );
		}

		warningFile->ForceFlush();

		fileSystem->CloseFile( warningFile );

		// RB begin
#if defined(_WIN32) && !defined(_DEBUG)
		// RB end
		idStr	osPath;
		osPath = fileSystem->RelativePathToOSPath( "warnings.txt", "fs_savepath" );
		WinExec( va( "Notepad.exe %s", osPath.c_str() ), SW_SHOW );
#endif
	}
}

/*
==================
idCommonLocal::Error
==================
*/
void idCommonLocal::Error( const char* fmt, ... )
{
	va_list		argptr;
	static int	lastErrorTime;
	static int	errorCount;
	int			currentTime;

	errorParm_t code = ERP_DROP;

	// always turn this off after an error
	com_refreshOnPrint = false;

	if( com_productionMode.GetInteger() == 3 )
	{
		Sys_Quit();
	}

	// when we are running automated scripts, make sure we
	// know if anything failed
	if( cvarSystem->GetCVarInteger( "fs_copyfiles" ) )
	{
		code = ERP_FATAL;
	}

	// if we don't have GL running, make it a fatal error
	if( !renderSystem->IsOpenGLRunning() )
	{
		code = ERP_FATAL;
	}

	// if we got a recursive error, make it fatal
	if( com_errorEntered )
	{
		// if we are recursively erroring while exiting
		// from a fatal error, just kill the entire
		// process immediately, which will prevent a
		// full screen rendering window covering the
		// error dialog
		if( com_errorEntered == ERP_FATAL )
		{
			Sys_Quit();
		}
		code = ERP_FATAL;
	}

	// if we are getting a solid stream of ERP_DROP, do an ERP_FATAL
	currentTime = Sys_Milliseconds();
	if( currentTime - lastErrorTime < 100 )
	{
		if( ++errorCount > 3 )
		{
			code = ERP_FATAL;
		}
	}
	else
	{
		errorCount = 0;
	}
	lastErrorTime = currentTime;

	com_errorEntered = code;

	va_start( argptr, fmt );
	idStr::vsnPrintf( errorMessage, sizeof( errorMessage ), fmt, argptr );
	va_end( argptr );
	errorMessage[sizeof( errorMessage ) - 1] = '\0';


	// copy the error message to the clip board
	Sys_SetClipboardData( errorMessage );

	// add the message to the error list
	errorList.AddUnique( errorMessage );

	Stop();

	if( code == ERP_DISCONNECT )
	{
		com_errorEntered = ERP_NONE;
		throw idException( errorMessage );
	}
	else if( code == ERP_DROP )
	{
		Printf( "********************\nERROR: %s\n********************\n", errorMessage );
		com_errorEntered = ERP_NONE;
		throw idException( errorMessage );
	}
	else
	{
		Printf( "********************\nERROR: %s\n********************\n", errorMessage );
	}

	if( cvarSystem->GetCVarBool( "r_fullscreen" ) )
	{
		cmdSystem->BufferCommandText( CMD_EXEC_NOW, "vid_restart partial windowed\n" );
	}

	Sys_Error( "%s", errorMessage );

}

/*
==================
idCommonLocal::FatalError

Dump out of the game to a system dialog
==================
*/
void idCommonLocal::FatalError( const char* fmt, ... )
{
	va_list		argptr;

	if( com_productionMode.GetInteger() == 3 )
	{
		Sys_Quit();
	}
	// if we got a recursive error, make it fatal
	if( com_errorEntered )
	{
		// if we are recursively erroring while exiting
		// from a fatal error, just kill the entire
		// process immediately, which will prevent a
		// full screen rendering window covering the
		// error dialog

		Sys_Printf( "FATAL: recursed fatal error:\n%s\n", errorMessage );

		va_start( argptr, fmt );
		idStr::vsnPrintf( errorMessage, sizeof( errorMessage ), fmt, argptr );
		va_end( argptr );
		errorMessage[sizeof( errorMessage ) - 1] = '\0';

		Sys_Printf( "%s\n", errorMessage );

		// write the console to a log file?
		Sys_Quit();
	}
	com_errorEntered = ERP_FATAL;

	va_start( argptr, fmt );
	idStr::vsnPrintf( errorMessage, sizeof( errorMessage ), fmt, argptr );
	va_end( argptr );
	errorMessage[sizeof( errorMessage ) - 1] = '\0';

	if( cvarSystem->GetCVarBool( "r_fullscreen" ) )
	{
		cmdSystem->BufferCommandText( CMD_EXEC_NOW, "vid_restart partial windowed\n" );
	}

	Sys_SetFatalError( errorMessage );

	Sys_Error( "%s", errorMessage );

}

//puncho - dev guard (P2 review): test scaffolding; common_frame.cpp's game-thread hook stays and returns at once in
// retail builds, as GameVerify's do
#ifndef ID_RETAIL
static void PrintRelayTestJob( void* data )
{
	const int count = *( const int* )data;
	if( count < 0 )
	{
		throw idException( "print relay test" );
	}
	for( int i = 0; i < count; i++ )
	{
		common->Printf( "print relay test job %d\n", i );
	}
}
REGISTER_PARALLEL_JOB( PrintRelayTestJob, "PrintRelayTestJob" );
idCVar com_printRelayTest( "com_printRelayTest", "0", CVAR_SYSTEM | CVAR_INTEGER, "developer: emit N worker lines and two game-thread lines; -1 tests fatal thread handling" );
#endif

void RunPrintRelayTest()
{
#ifndef ID_RETAIL
	static bool gameLinesPending;
	if( gameLinesPending )
	{
		common->Printf( "print relay test game Printf\n" );
		common->DPrintf( "print relay test game DPrintf\n" );
		gameLinesPending = false;
	}
	const int count = com_printRelayTest.GetInteger();
	if( count == 0 || !com_developer.GetBool() )
	{
		return;
	}
	com_printRelayTest.SetInteger( 0 );
	idParallelJobList* jobs = parallelJobManager->AllocJobList( JOBLIST_UTILITY, JOBLIST_PRIORITY_LOW, 1, 0, NULL );
	jobs->AddJob( PrintRelayTestJob, ( void* )&count );
	jobs->Submit();
	jobs->Wait();
	parallelJobManager->FreeJobList( jobs );
	gameLinesPending = true;
#endif
}
