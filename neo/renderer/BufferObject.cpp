/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013 Robert Beckebans
Copyright (C) 2016-2017 Dustin Land
Copyright (C) 2022 Stephen Pridham

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
#include "RenderCommon.h"

idCVar r_showBuffers( "r_showBuffers", "0", CVAR_INTEGER, "" );

#ifdef _WIN32
/*
==================
IsWriteCombined
==================
*/
bool IsWriteCombined( void* base )
{
	MEMORY_BASIC_INFORMATION info;
	SIZE_T size = VirtualQueryEx( GetCurrentProcess(), base, &info, sizeof( info ) );
	if( size == 0 )
	{
		DWORD error = GetLastError();
		error = error;
		return false;
	}
	bool isWriteCombined = ( ( info.AllocationProtect & PAGE_WRITECOMBINE ) != 0 );
	return isWriteCombined;
}
#endif

#if defined(USE_INTRINSICS_SSE)

void CopyBuffer( byte* dst, const byte* src, int numBytes )
{
	assert_16_byte_aligned( dst );
	assert_16_byte_aligned( src );

	int i = 0;
	for( ; i + 128 <= numBytes; i += 128 )
	{
		__m128i d0 = _mm_load_si128( ( __m128i* )&src[i + 0 * 16] );
		__m128i d1 = _mm_load_si128( ( __m128i* )&src[i + 1 * 16] );
		__m128i d2 = _mm_load_si128( ( __m128i* )&src[i + 2 * 16] );
		__m128i d3 = _mm_load_si128( ( __m128i* )&src[i + 3 * 16] );
		__m128i d4 = _mm_load_si128( ( __m128i* )&src[i + 4 * 16] );
		__m128i d5 = _mm_load_si128( ( __m128i* )&src[i + 5 * 16] );
		__m128i d6 = _mm_load_si128( ( __m128i* )&src[i + 6 * 16] );
		__m128i d7 = _mm_load_si128( ( __m128i* )&src[i + 7 * 16] );
		_mm_stream_si128( ( __m128i* )&dst[i + 0 * 16], d0 );
		_mm_stream_si128( ( __m128i* )&dst[i + 1 * 16], d1 );
		_mm_stream_si128( ( __m128i* )&dst[i + 2 * 16], d2 );
		_mm_stream_si128( ( __m128i* )&dst[i + 3 * 16], d3 );
		_mm_stream_si128( ( __m128i* )&dst[i + 4 * 16], d4 );
		_mm_stream_si128( ( __m128i* )&dst[i + 5 * 16], d5 );
		_mm_stream_si128( ( __m128i* )&dst[i + 6 * 16], d6 );
		_mm_stream_si128( ( __m128i* )&dst[i + 7 * 16], d7 );
	}
	for( ; i + 16 <= numBytes; i += 16 )
	{
		__m128i d = _mm_load_si128( ( __m128i* )&src[i] );
		_mm_stream_si128( ( __m128i* )&dst[i], d );
	}
	for( ; i + 4 <= numBytes; i += 4 )
	{
		*( uint32* )&dst[i] = *( const uint32* )&src[i];
	}
	for( ; i < numBytes; i++ )
	{
		dst[i] = src[i];
	}
	_mm_sfence();
}

#else

void CopyBuffer( byte* dst, const byte* src, int numBytes )
{
	assert_16_byte_aligned( dst );
	assert_16_byte_aligned( src );
	memcpy( dst, src, numBytes );
}

#endif

/*
================================================================================================

	idBufferObject

================================================================================================
*/

/*
========================
idBufferObject::idBufferObject
========================
*/
idBufferObject::idBufferObject()
{
	size = 0;
	//puncho start - static index (2R R3.2): the flags are bools; an object without a buffer is an owner that is not mapped
	offsetInOtherBuffer = 0;
	mapped = false;
	ownsBuffer = true;
	//puncho end
	usage = BU_STATIC;

	bufferHandle.Reset();
	inputLayout.Reset();
	buffer = NULL;

#if defined( USE_AMD_ALLOCATOR )
	vkBuffer = VK_NULL_HANDLE;
	allocation = NULL;
	allocationInfo = {};
#endif
}

//puncho start - static index (2R R3.2)
#if !defined( ID_RETAIL )
// testStaticAllocFail's count: the n-th AllocBufferObject from now returns false. It is not a member of idVertexCache, whose
// Init must not disarm it: in autoexec.cfg the command runs before the back end makes its buffers
static int bufferAllocFailCountdown = 0;

CONSOLE_COMMAND( testStaticAllocFail, "test only: the n-th buffer object allocation from now is refused (n from 1; default 1)", NULL )
{
	long long n = 1;
	if( args.Argc() > 1 )
	{
		const char* text = args.Argv( 1 );
		char* end = NULL;
		n = strtoll( text, &end, 10 );
		if( end == text || *end != '\0' )
		{
			n = 0;
		}
	}
	if( n < 1 || n > INT_MAX )
	{
		idLib::Printf( "usage: testStaticAllocFail [n], n from 1 to %d; the pending count stays %d\n", INT_MAX, bufferAllocFailCountdown );
		return;
	}
	bufferAllocFailCountdown = ( int )n;
	idLib::Printf( "testStaticAllocFail: buffer object allocation %d from now is refused\n", bufferAllocFailCountdown );
}
#endif

/*
========================
idBufferObject::TestAllocFail

Called by the three AllocBufferObject before they make anything: true for the allocation testStaticAllocFail chose, which
then returns false as it does when the device refuses the buffer. Main thread and back end start only.
========================
*/
bool idBufferObject::TestAllocFail()
{
#if !defined( ID_RETAIL )
	if( bufferAllocFailCountdown > 0 && --bufferAllocFailCountdown == 0 )
	{
		return true;
	}
#endif
	return false;
}

/*
========================
idBufferObject::TakeBufferFrom

Moves other's buffer into this object and leaves other empty: afterwards this object owns the buffer and frees it once, and
other frees nothing. Refused, with neither object changed, unless this object is empty (no buffer, no reference, not
mapped) and other owns a buffer that is not mapped. The caller answers for the GPU's side: nothing may still be recording
into, or drawing from, the buffer under other's name. The three kinds call it through their typed TakeFrom; its first
caller is the growth of a static set (2R R3.3).
========================
*/
bool idBufferObject::TakeBufferFrom( idBufferObject& other )
{
	if( this == &other || bufferHandle || !ownsBuffer || mapped || !other.bufferHandle || !other.ownsBuffer || other.mapped )
	{
		return false;
	}

	size = other.size;
	offsetInOtherBuffer = other.offsetInOtherBuffer;
	usage = other.usage;
	inputLayout = other.inputLayout;
	bufferHandle = other.bufferHandle;
	buffer = other.buffer;
	debugName = other.debugName;
#if defined( USE_AMD_ALLOCATOR )
	vkBuffer = other.vkBuffer;
	allocation = other.allocation;
	allocationInfo = other.allocationInfo;
#endif

	other.inputLayout.Reset();
	other.SetEmpty();
	return true;
}
//puncho end

/*
================================================================================================

	idVertexBuffer

================================================================================================
*/

/*
========================
idVertexBuffer::~idVertexBuffer
========================
*/
idVertexBuffer::~idVertexBuffer()
{
	FreeBufferObject();
}

/*
========================
idVertexBuffer::Reference
========================
*/
void idVertexBuffer::Reference( const idVertexBuffer& other )
{
	assert( IsMapped() == false );
	//assert( other.IsMapped() == false );	// this happens when building idTriangles while at the same time setting up idDrawVerts
	assert( other.GetSize() > 0 );

	FreeBufferObject();
	size = other.GetSize();
	offsetInOtherBuffer = other.GetOffset();
	ownsBuffer = false;		//puncho - static index (2R R3.2): the flag is a bool; before, GetOffset left bit 31 clear here
	usage = other.usage;
	bufferHandle = other.bufferHandle;

#if defined ( USE_AMD_ALLOCATOR )
	allocationInfo = other.allocationInfo;
#endif
	assert( OwnsBuffer() == false );
}

/*
========================
idVertexBuffer::Reference
========================
*/
void idVertexBuffer::Reference( const idVertexBuffer& other, int64 refOffset, int refSize )
{
	assert( IsMapped() == false );
	//assert( other.IsMapped() == false );	// this happens when building idTriangles while at the same time setting up idDrawVerts
	assert( refOffset >= 0 );
	assert( refSize >= 0 );
	assert( refOffset <= other.GetSize() && refSize <= other.GetSize() - refOffset );	//puncho - static index (2R R3.2): int64, and no sum that could wrap

	FreeBufferObject();
	size = refSize;
	offsetInOtherBuffer = other.GetOffset() + refOffset;
	ownsBuffer = false;		//puncho - static index (2R R3.2): the flag is a bool; before, an offset below 2 GiB left bit 31 clear
	usage = other.usage;
	bufferHandle = other.bufferHandle;

#if defined ( USE_AMD_ALLOCATOR )
	allocationInfo = other.allocationInfo;
#endif
	assert( OwnsBuffer() == false );
}

/*
================================================================================================

idIndexBuffer

================================================================================================
*/

/*
========================
idIndexBuffer::~idIndexBuffer
========================
*/
idIndexBuffer::~idIndexBuffer()
{
	FreeBufferObject();
}

/*
========================
idIndexBuffer::Reference
========================
*/
void idIndexBuffer::Reference( const idIndexBuffer& other )
{
	assert( IsMapped() == false );
	//assert( other.IsMapped() == false );	// this happens when building idTriangles while at the same time setting up triIndex_t
	assert( other.GetSize() > 0 );

	FreeBufferObject();
	size = other.GetSize();
	offsetInOtherBuffer = other.GetOffset();
	ownsBuffer = false;		//puncho - static index (2R R3.2): the flag is a bool; before, GetOffset left bit 31 clear here
	usage = other.usage;
	bufferHandle = other.bufferHandle;

#if defined ( USE_AMD_ALLOCATOR )
	allocationInfo = other.allocationInfo;
#endif
	assert( OwnsBuffer() == false );
}

/*
========================
idIndexBuffer::Reference
========================
*/
void idIndexBuffer::Reference( const idIndexBuffer& other, int64 refOffset, int refSize )
{
	assert( IsMapped() == false );
	//assert( other.IsMapped() == false );	// this happens when building idTriangles while at the same time setting up triIndex_t
	assert( refOffset >= 0 );
	assert( refSize >= 0 );
	assert( refOffset <= other.GetSize() && refSize <= other.GetSize() - refOffset );	//puncho - static index (2R R3.2): int64, and no sum that could wrap

	FreeBufferObject();
	size = refSize;
	offsetInOtherBuffer = other.GetOffset() + refOffset;
	ownsBuffer = false;		//puncho - static index (2R R3.2): the flag is a bool; before, an offset below 2 GiB left bit 31 clear
	usage = other.usage;
	bufferHandle = other.bufferHandle;

#if defined ( USE_AMD_ALLOCATOR )
	allocationInfo = other.allocationInfo;
#endif
	assert( OwnsBuffer() == false );
}

/*
================================================================================================

idUniformBuffer

================================================================================================
*/

/*
========================
idUniformBuffer::~idUniformBuffer
========================
*/
idUniformBuffer::~idUniformBuffer()
{
	FreeBufferObject();
}

/*
========================
idUniformBuffer::Reference
========================
*/
void idUniformBuffer::Reference( const idUniformBuffer& other )
{
	assert( IsMapped() == false );
	assert( other.IsMapped() == false );
	assert( other.GetSize() > 0 );

	FreeBufferObject();
	size = other.GetSize();
	offsetInOtherBuffer = other.GetOffset();
	ownsBuffer = false;		//puncho - static index (2R R3.2): the flag is a bool; before, GetOffset left bit 31 clear here
	usage = other.usage;
	bufferHandle = other.bufferHandle;

#if defined ( USE_AMD_ALLOCATOR )
	allocationInfo = other.allocationInfo;
#endif
	assert( OwnsBuffer() == false );
}

/*
========================
idUniformBuffer::Reference
========================
*/
void idUniformBuffer::Reference( const idUniformBuffer& other, int64 refOffset, int refSize )
{
	assert( IsMapped() == false );
	assert( other.IsMapped() == false );
	assert( refOffset >= 0 );
	assert( refSize >= 0 );
	assert( refOffset <= other.GetSize() && refSize <= other.GetSize() - refOffset );	//puncho - static index (2R R3.2): int64, and no sum that could wrap

	FreeBufferObject();
	size = refSize;
	offsetInOtherBuffer = other.GetOffset() + refOffset;
	ownsBuffer = false;		//puncho - static index (2R R3.2): the flag is a bool; before, an offset below 2 GiB left bit 31 clear
	usage = other.usage;
	bufferHandle = other.bufferHandle;

#if defined ( USE_AMD_ALLOCATOR )
	allocationInfo = other.allocationInfo;
#endif
	assert( OwnsBuffer() == false );
}
