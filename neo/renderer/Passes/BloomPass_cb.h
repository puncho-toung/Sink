/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2014 Robert Beckebans

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

#pragma once

// The bloom draws' constants, sent as push constants by BloomPass. shaders/builtin/post/bloom_cb.h has the same
// struct, member for member ( the tonemap pass's ToneMappingConstants and renderer/Passes/TonemapPass_cb.h are the
// model for keeping the two twins ).
struct BloomConstants
{
	idVec4	srcRect;		// xy the source rectangle's origin in source texels, zw its size in texels
	idVec4	srcInvSize;		// xy 1 / the source TEXTURE's size; z the tail ( up ), the glow threshold's knee ( first pass, FIRST 2 and 3 ); w the firefly knee ( first pass; <= 0: off )
	idVec4	exposure;		// first pass: x the exposure scale exp2( r_exposure ), y r_hdrMinLuminance, z the ceiling ( r_bloomCeiling ); w the glow threshold ( FIRST 2 and 3; 0 in the mix )
	idVec4	tailDepth;	// first pass, FIRST 3 ( the distance tail ): xy the projection's two depth terms ( a view distance is x / ( depth + y ) ); zw 0
	idVec4	tailDist;	// first pass, FIRST 3: x the far distance, y 1 / ( far - near ), z the share of its tail a far lamp keeps; w 0
};
compile_time_assert( sizeof( BloomConstants ) == 80 );	// push constants: Vulkan promises 128 bytes, the D3D12 device manager 240
