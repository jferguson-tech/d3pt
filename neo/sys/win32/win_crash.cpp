/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

// When the program crashes, write where it was to crash.txt beside the
// executable: the exception, and the call stack with function names and line
// numbers where the .pdb files are at hand. 64-bit build only.

#include "../../idlib/precompiled.h"
#pragma hdrstop

#include "win_local.h"

#ifdef _WIN64

#include <dbghelp.h>

static LONG WINAPI Sys_CrashFilter( EXCEPTION_POINTERS *info ) {
	static bool	entered = false;
	char		path[MAX_PATH];
	char		*slash;
	FILE		*f;

	if ( entered ) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	entered = true;

	GetModuleFileName( NULL, path, sizeof( path ) );
	slash = strrchr( path, '\\' );
	strcpy( slash ? slash + 1 : path, "crash.txt" );
	f = fopen( path, "w" );
	if ( !f ) {
		return EXCEPTION_CONTINUE_SEARCH;
	}

	const EXCEPTION_RECORD *rec = info->ExceptionRecord;
	fprintf( f, "exception 0x%08X at %p\n", (unsigned int) rec->ExceptionCode, rec->ExceptionAddress );
	if ( rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2 ) {
		fprintf( f, "%s address %p\n", rec->ExceptionInformation[0] == 0 ? "reading" : ( rec->ExceptionInformation[0] == 1 ? "writing" : "executing" ),
				(void *) rec->ExceptionInformation[1] );
	}

	HANDLE process = GetCurrentProcess();
	SymSetOptions( SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS );
	SymInitialize( process, NULL, TRUE );

	CONTEXT			context = *info->ContextRecord;
	STACKFRAME64	frame;
	memset( &frame, 0, sizeof( frame ) );
	frame.AddrPC.Offset = context.Rip;
	frame.AddrPC.Mode = AddrModeFlat;
	frame.AddrFrame.Offset = context.Rbp;
	frame.AddrFrame.Mode = AddrModeFlat;
	frame.AddrStack.Offset = context.Rsp;
	frame.AddrStack.Mode = AddrModeFlat;

	for ( int i = 0; i < 48; i++ ) {
		if ( !StackWalk64( IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context, NULL,
							SymFunctionTableAccess64, SymGetModuleBase64, NULL ) || frame.AddrPC.Offset == 0 ) {
			break;
		}

		char			buffer[ sizeof( SYMBOL_INFO ) + 512 ];
		SYMBOL_INFO		*symbol = (SYMBOL_INFO *) buffer;
		DWORD64			offset = 0;
		IMAGEHLP_LINE64	line;
		DWORD			column = 0;
		char			module[MAX_PATH] = "?";
		DWORD64			base = SymGetModuleBase64( process, frame.AddrPC.Offset );

		if ( base ) {
			GetModuleFileName( (HMODULE) base, module, sizeof( module ) );
		}
		slash = strrchr( module, '\\' );

		memset( buffer, 0, sizeof( buffer ) );
		symbol->SizeOfStruct = sizeof( SYMBOL_INFO );
		symbol->MaxNameLen = 511;
		memset( &line, 0, sizeof( line ) );
		line.SizeOfStruct = sizeof( line );

		fprintf( f, "%2d %s", i, slash ? slash + 1 : module );
		if ( SymFromAddr( process, frame.AddrPC.Offset, &offset, symbol ) ) {
			fprintf( f, " %s+0x%X", symbol->Name, (unsigned int) offset );
		} else {
			fprintf( f, " +0x%llX", (unsigned long long)( frame.AddrPC.Offset - base ) );
		}
		if ( SymGetLineFromAddr64( process, frame.AddrPC.Offset, &column, &line ) ) {
			fprintf( f, "  %s(%d)", line.FileName, (int) line.LineNumber );
		}
		fprintf( f, "\n" );
	}
	fclose( f );

	return EXCEPTION_CONTINUE_SEARCH;
}

void Sys_InstallCrashHandler( void ) {
	SetUnhandledExceptionFilter( Sys_CrashFilter );
}

#else

void Sys_InstallCrashHandler( void ) {
}

#endif
