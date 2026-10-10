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

// What win_cpu.cpp does, for the 64-bit build: that file is written in inline
// assembly, which the 64-bit compiler does not take. Here the processor is
// asked through the compiler's intrinsics, and since 64-bit code does its
// arithmetic in SSE and leaves the x87 stack alone, the calls that look after
// that stack have nothing to do.

#include "../../idlib/precompiled.h"
#pragma hdrstop

#include "win_local.h"

#include <intrin.h>
#include <float.h>
#include <xmmintrin.h>
#include <pmmintrin.h>

/*
==============================================================

	Clock ticks

==============================================================
*/

/*
================
Sys_GetClockTicks
================
*/
double Sys_GetClockTicks( void ) {
	return (double) __rdtsc();
}

/*
================
Sys_ClockTicksPerSecond
================
*/
double Sys_ClockTicksPerSecond( void ) {
	static double ticks = 0;

	if ( !ticks ) {
		HKEY hKey;
		DWORD ProcSpeed;
		DWORD buflen, ret;

		if ( !RegOpenKeyEx( HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hKey ) ) {
			ProcSpeed = 0;
			buflen = sizeof( ProcSpeed );
			ret = RegQueryValueEx( hKey, "~MHz", NULL, NULL, (LPBYTE) &ProcSpeed, &buflen );
			RegCloseKey( hKey );
			if ( ret == ERROR_SUCCESS ) {
				ticks = (double) ProcSpeed * 1000000;
			}
		}
		if ( !ticks ) {
			// never zero: callers divide by it
			ticks = 1000000000.0;
		}
	}
	return ticks;
}


/*
==============================================================

	CPU

==============================================================
*/

#define _REG_EAX		0
#define _REG_EBX		1
#define _REG_ECX		2
#define _REG_EDX		3

/*
================
CPUID
================
*/
static void CPUID( int func, unsigned regs[4] ) {
	int	info[4];

	__cpuid( info, func );
	regs[_REG_EAX] = info[0];
	regs[_REG_EBX] = info[1];
	regs[_REG_ECX] = info[2];
	regs[_REG_EDX] = info[3];
}

/*
================
IsAMD
================
*/
static bool IsAMD( void ) {
	char pstring[16];
	char processorString[13];

	// get name of processor
	CPUID( 0, ( unsigned int * ) pstring );
	processorString[0] = pstring[4];
	processorString[1] = pstring[5];
	processorString[2] = pstring[6];
	processorString[3] = pstring[7];
	processorString[4] = pstring[12];
	processorString[5] = pstring[13];
	processorString[6] = pstring[14];
	processorString[7] = pstring[15];
	processorString[8] = pstring[8];
	processorString[9] = pstring[9];
	processorString[10] = pstring[10];
	processorString[11] = pstring[11];
	processorString[12] = 0;

	return strcmp( processorString, "AuthenticAMD" ) == 0;
}

/*
================
Has3DNow
================
*/
static bool Has3DNow( void ) {
	unsigned regs[4];

	// check AMD-specific functions
	CPUID( 0x80000000, regs );
	if ( regs[_REG_EAX] < 0x80000000 ) {
		return false;
	}

	// bit 31 of EDX denotes 3DNow! support
	CPUID( 0x80000001, regs );
	return ( regs[_REG_EDX] & ( 1 << 31 ) ) != 0;
}

/*
================
Sys_GetCPUId
================
*/
cpuid_t Sys_GetCPUId( void ) {
	int flags;
	unsigned regs[4];

	flags = IsAMD() ? CPUID_AMD : CPUID_INTEL;

	CPUID( 1, regs );

	// every processor that runs 64-bit code has these
	flags |= CPUID_MMX | CPUID_SSE | CPUID_SSE2 | CPUID_FTZ | CPUID_CMOV | CPUID_DAZ;

	if ( Has3DNow() ) {
		flags |= CPUID_3DNOW;
	}

	// bit 0 of ECX denotes SSE3 existence
	if ( regs[_REG_ECX] & ( 1 << 0 ) ) {
		flags |= CPUID_SSE3;
	}

	// bit 28 of EDX denotes more than one logical processor in the package
	if ( regs[_REG_EDX] & ( 1 << 28 ) ) {
		flags |= CPUID_HTT;
	}

	return (cpuid_t)flags;
}


/*
===============================================================================

	FPU

===============================================================================
*/

/*
===============
Sys_FPU_StackIsEmpty
===============
*/
bool Sys_FPU_StackIsEmpty( void ) {
	return true;
}

/*
===============
Sys_FPU_ClearStack
===============
*/
void Sys_FPU_ClearStack( void ) {
}

/*
===============
Sys_FPU_GetState
===============
*/
const char *Sys_FPU_GetState( void ) {
	static char	fpuString[128];
	unsigned int control = 0;

	_controlfp_s( &control, 0, 0 );
	sprintf( fpuString, "floating point control word = 0x%08X, MXCSR = 0x%08X\n", control, _mm_getcsr() );
	return fpuString;
}

/*
===============
Sys_FPU_EnableExceptions
===============
*/
void Sys_FPU_EnableExceptions( int exceptions ) {
	unsigned int control;
	unsigned int masked = _MCW_EM;

	// a set bit in the control word masks the exception
	if ( exceptions & FPU_EXCEPTION_INVALID_OPERATION ) {
		masked &= ~_EM_INVALID;
	}
	if ( exceptions & FPU_EXCEPTION_DENORMALIZED_OPERAND ) {
		masked &= ~_EM_DENORMAL;
	}
	if ( exceptions & FPU_EXCEPTION_DIVIDE_BY_ZERO ) {
		masked &= ~_EM_ZERODIVIDE;
	}
	if ( exceptions & FPU_EXCEPTION_NUMERIC_OVERFLOW ) {
		masked &= ~_EM_OVERFLOW;
	}
	if ( exceptions & FPU_EXCEPTION_NUMERIC_UNDERFLOW ) {
		masked &= ~_EM_UNDERFLOW;
	}
	if ( exceptions & FPU_EXCEPTION_INEXACT_RESULT ) {
		masked &= ~_EM_INEXACT;
	}
	_controlfp_s( &control, masked, _MCW_EM );
}

/*
===============
Sys_FPU_SetPrecision
===============
*/
void Sys_FPU_SetPrecision( int precision ) {
	// SSE arithmetic has the precision of its operands
}

/*
================
Sys_FPU_SetRounding
================
*/
void Sys_FPU_SetRounding( int rounding ) {
	static const unsigned int roundingTable[4] = { _RC_NEAR, _RC_DOWN, _RC_UP, _RC_CHOP };
	unsigned int control;

	_controlfp_s( &control, roundingTable[rounding & 3], _MCW_RC );
}

/*
================
Sys_FPU_SetDAZ
================
*/
void Sys_FPU_SetDAZ( bool enable ) {
	_MM_SET_DENORMALS_ZERO_MODE( enable ? _MM_DENORMALS_ZERO_ON : _MM_DENORMALS_ZERO_OFF );
}

/*
================
Sys_FPU_SetFTZ
================
*/
void Sys_FPU_SetFTZ( bool enable ) {
	_MM_SET_FLUSH_ZERO_MODE( enable ? _MM_FLUSH_ZERO_ON : _MM_FLUSH_ZERO_OFF );
}
