/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
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
#ifndef __BUFFEROBJECT_H__
#define __BUFFEROBJECT_H__


#include <nvrhi/nvrhi.h>

#if defined( USE_AMD_ALLOCATOR )
	#include <nvrhi/vulkan.h>
	#include "vk_mem_alloc.h"
#endif


enum bufferMapType_t
{
	BM_READ,			// map for reading
	BM_WRITE			// map for writing
};

enum bufferUsageType_t
{
	BU_STATIC,			// GPU R
	BU_DYNAMIC,			// GPU R, CPU R/W
};

// Returns all targets to virtual memory use instead of buffer object use.
// Call this before doing any conventional buffer reads, like screenshots.
void UnbindBufferObjects();
bool IsWriteCombined( void* base );
void CopyBuffer( byte* dst, const byte* src, int numBytes );

/*
================================================================================================

idBufferObject

================================================================================================
*/

class idBufferObject
{
public:
	idBufferObject();

	//puncho start - static index (2R R3.2): a size and an offset are int64 bytes. "Mapped" and "owns the buffer" were bit 31
	// of size and of offsetInOtherBuffer, which a size or an offset of 2 GiB would have set; they are bools of their own
	int64				GetSize() const
	{
		return size;
	}
	int64				GetAllocedSize() const
	{
		return ( size + 15 ) & ~( int64 )15;
	}
	//puncho end
	bufferUsageType_t	GetUsage() const
	{
		return usage;
	}
	nvrhi::IBuffer*		GetAPIObject() const
	{
		return bufferHandle;
	}
	int64				GetOffset() const	//puncho - static index (2R R3.2): int64 bytes; 0 in an owner
	{
		return offsetInOtherBuffer;
	}

	bool				IsMapped() const
	{
		return mapped;
	}

	void				SetDebugName( idStr str )
	{
		debugName = str;
	}

protected:
	void				SetMapped() const
	{
		mapped = true;
	}
	void				SetUnmapped() const
	{
		mapped = false;
	}
	bool				OwnsBuffer() const
	{
		return ownsBuffer;
	}

	//puncho start - static index (2R R3.2)
	// The state of an object that holds no buffer: the constructor's, and the one FreeBufferObject leaves. The usage, the
	// input layout and the debug name are not part of it
	void				SetEmpty()
	{
		size = 0;
		offsetInOtherBuffer = 0;
		mapped = false;
		ownsBuffer = true;
		bufferHandle.Reset();
		buffer = NULL;
#if defined( USE_AMD_ALLOCATOR )
		vkBuffer = VK_NULL_HANDLE;
		allocation = NULL;
		allocationInfo = {};
#endif
	}
	// bytes rounded up to a multiple of alignment, in 64 bits. False, with rounded left as it was, when bytes is negative,
	// alignment is not a power of two, or the result would pass INT64_MAX
	static bool			RoundUpBytes( int64 bytes, int64 alignment, int64& rounded )
	{
		if( bytes < 0 || alignment <= 0 || ( alignment & ( alignment - 1 ) ) != 0 )
		{
			return false;
		}
		const int64 pad = ( alignment - ( bytes & ( alignment - 1 ) ) ) & ( alignment - 1 );
		if( pad > INT64_MAX - bytes )
		{
			return false;
		}
		rounded = bytes + pad;
		return true;
	}
	// Update's test, the same for the three kinds of buffer. numBytes becomes the update's length rounded up to 16 (the copy
	// works in 16-byte steps). True when the bytes [ offset, offset + numBytes ) lie inside this buffer or reference. False
	// when they do not, when the length is negative, or when the rounded length would not fit CopyBuffer's int (the longest
	// update is INT_MAX - 15 bytes). Each of the first two tests is one unsigned compare for both ends of its range, and
	// nothing is added that could wrap. The order matters: the subtraction is safe only after the second test
	bool				UpdateSpan( int updateSize, int64 offset, int64& numBytes ) const
	{
		numBytes = ( ( int64 )updateSize + 15 ) & ~( int64 )15;
		return ( uint32 )updateSize <= ( uint32 )( INT_MAX - 15 ) && ( uint64 )offset <= ( uint64 )size && numBytes <= size - offset;
	}
	// AllocBufferObject's fault for tests, the same for the three kinds: true for the n-th call after the dev command
	// testStaticAllocFail n. The caller then returns false, as it does when the device refuses the buffer
	static bool			TestAllocFail();
	// Moves other's buffer into this object, which must be empty; see BufferObject.cpp
	bool				TakeBufferFrom( idBufferObject& other );
	//puncho end

protected:
	int64						size;					// size in bytes	//puncho - static index (2R R3.2): int64, and no flag in it
	int64						offsetInOtherBuffer;	// offset in bytes	//puncho - static index (2R R3.2): int64; 0 in an owner
	bufferUsageType_t			usage;
	mutable bool				mapped;					//puncho - static index (2R R3.2): it was MAPPED_FLAG, bit 31 of size; mutable for the const setters above
	bool						ownsBuffer;				//puncho - static index (2R R3.2): it was OWNS_BUFFER_FLAG, bit 31 of offsetInOtherBuffer; only Reference clears it

	nvrhi::InputLayoutHandle	inputLayout;
	nvrhi::BufferHandle			bufferHandle;
	void*						buffer;
	idStr						debugName;

#if defined( USE_AMD_ALLOCATOR )
	VkBuffer					vkBuffer;
	VmaAllocation				allocation;
	VmaAllocationInfo			allocationInfo;
#endif
};

/*
================================================================================================

idVertexBuffer

================================================================================================
*/
class idVertexBuffer : public idBufferObject
{
public:
	idVertexBuffer();
	~idVertexBuffer();

	// Allocate or free the buffer.
	bool				AllocBufferObject( const void* data, int64 allocSize, bufferUsageType_t usage, nvrhi::ICommandList* commandList );	//puncho - static index (2R R3.2): int64 bytes; false when the device refuses the buffer
	void				FreeBufferObject();

	// Make this buffer a reference to another buffer.
	void				Reference( const idVertexBuffer& other );
	void				Reference( const idVertexBuffer& other, int64 refOffset, int refSize );	//puncho - static index (2R R3.2): int64 offset
	//puncho - static index (2R R3.2): takes over other's buffer and leaves other empty. False, with nothing changed, unless this
	// object is empty and other owns a buffer that is not mapped (idBufferObject::TakeBufferFrom)
	bool				TakeFrom( idVertexBuffer& other )
	{
		return TakeBufferFrom( other );
	}

	// Copies data to the buffer. 'size' may be less than the originally allocated size.
	void				Update( const void* data, int size, int64 offset, bool initialUpdate, nvrhi::ICommandList* commandList ) const;	//puncho - static index (2R R3.2): int64 offset

	void* 				MapBuffer( bufferMapType_t mapType );
	idDrawVert* 		MapVertexBuffer( bufferMapType_t mapType )
	{
		return static_cast< idDrawVert* >( MapBuffer( mapType ) );
	}
	void				UnmapBuffer();

private:
	void				ClearWithoutFreeing();

	DISALLOW_COPY_AND_ASSIGN( idVertexBuffer );
};

/*
================================================================================================

idIndexBuffer

================================================================================================
*/
class idIndexBuffer : public idBufferObject
{
public:
	idIndexBuffer();
	~idIndexBuffer();

	// Allocate or free the buffer.
	bool				AllocBufferObject( const void* data, int64 allocSize, bufferUsageType_t usage, nvrhi::ICommandList* commandList );	//puncho - static index (2R R3.2): int64 bytes; false when the device refuses the buffer
	void				FreeBufferObject();

	// Make this buffer a reference to another buffer.
	void				Reference( const idIndexBuffer& other );
	void				Reference( const idIndexBuffer& other, int64 refOffset, int refSize );	//puncho - static index (2R R3.2): int64 offset
	//puncho - static index (2R R3.2): takes over other's buffer and leaves other empty. False, with nothing changed, unless this
	// object is empty and other owns a buffer that is not mapped (idBufferObject::TakeBufferFrom)
	bool				TakeFrom( idIndexBuffer& other )
	{
		return TakeBufferFrom( other );
	}

	// Copies data to the buffer. 'size' may be less than the originally allocated size.
	void				Update( const void* data, int size, int64 offset, bool initialUpdate, nvrhi::ICommandList* commandList ) const;	//puncho - static index (2R R3.2): int64 offset

	void* 				MapBuffer( bufferMapType_t mapType );
	triIndex_t* 		MapIndexBuffer( bufferMapType_t mapType )
	{
		return static_cast< triIndex_t* >( MapBuffer( mapType ) );
	}
	void				UnmapBuffer();

private:
	void				ClearWithoutFreeing();

	DISALLOW_COPY_AND_ASSIGN( idIndexBuffer );
};

/*
================================================================================================

idUniformBuffer

IMPORTANT NOTICE: on the PC, binding to an offset in uniform buffer objects
is limited to GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, which is 256 on current nvidia cards,
so joint offsets, which are multiples of 48 bytes, must be in multiples of 16 = 768 bytes.
================================================================================================
*/
class idUniformBuffer : public idBufferObject
{
public:
	idUniformBuffer();
	~idUniformBuffer();

	// Allocate or free the buffer.
	bool				AllocBufferObject( const void* data, int64 allocSize, bufferUsageType_t usage, nvrhi::ICommandList* commandList );	//puncho - static index (2R R3.2): int64 bytes; false when the device refuses the buffer
	void				FreeBufferObject();

	// Make this buffer a reference to another buffer.
	void				Reference( const idUniformBuffer& other );
	void				Reference( const idUniformBuffer& other, int64 refOffset, int refSize );	//puncho - static index (2R R3.2): int64 offset
	//puncho - static index (2R R3.2): takes over other's buffer and leaves other empty. False, with nothing changed, unless this
	// object is empty and other owns a buffer that is not mapped (idBufferObject::TakeBufferFrom)
	bool				TakeFrom( idUniformBuffer& other )
	{
		return TakeBufferFrom( other );
	}

	// Copies data to the buffer. 'size' may be less than the originally allocated size.
	void				Update( const void* data, int size, int64 offset, bool initialUpdate, nvrhi::ICommandList* commandList ) const;	//puncho - static index (2R R3.2): int64 offset

	void* 				MapBuffer( bufferMapType_t mapType );
	void				UnmapBuffer();

private:
	void				ClearWithoutFreeing();

	DISALLOW_COPY_AND_ASSIGN( idUniformBuffer );
};

#endif // !__BUFFEROBJECT_H__
