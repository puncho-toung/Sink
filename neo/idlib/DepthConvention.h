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
#ifndef __DEPTHCONVENTION_H__
#define __DEPTHCONVENTION_H__

/*
================================================================================================

Depth convention

One switch for idLib, the renderer, rbdmap ( C++, through RenderMatrix.h ) and every shader
( HLSL, through vulkan.hlsli ). Keep this file plain preprocessor code that C, C++ and HLSL all
parse, and never put an #include in it: ShaderMake resolves every #include line it finds for its
dependency times, even inside #if blocks.

ID_REVERSED_Z 1: the main, probe-capture and GUI-editor depth attachments ( D32S8 ) clear to 0,
the near plane maps to 1 and the infinite far plane to 0 ( ndc z = zNear / distance ), the "less"
depth test becomes GREATER_EQUAL and the depth bias changes sign. The shadow maps ( D32 ) keep the
forward convention in both builds, so every per-framebuffer choice is keyed to the attachment
format ( R_IsReversedDepth ), never to this switch alone.

The CMake option REVERSED_Z ( default ON ) defines ID_REVERSED_Z for C++ and for the shader
compile, and compiles the reversed shader blobs into renderprogs2/<api>_rz, the directory the
reversed engine loads them from: a build mismatched with its blobs stops at the missing-.bin
FatalError instead of rendering with the wrong convention.

================================================================================================
*/

#ifndef ID_REVERSED_Z
	#define ID_REVERSED_Z					1
#endif

// DX12 and Vulkan clip Z to [0, W] ( idRenderMatrix used to define this locally )
#define CLIP_SPACE_D3D						1

// depthConvention: the values of the main depth attachment's convention
#if ID_REVERSED_Z
	#define DEPTH_CONVENTION_CLEAR			0.0f	// the depth clear value: the far plane
	#define DEPTH_CONVENTION_NEAR_NDC		1.0f
	#define DEPTH_CONVENTION_FAR_NDC		0.0f	// exactly infinite: ndc z = zNear / distance
	#define DEPTH_CONVENTION_BIAS_SIGN		-1.0f	// the sign that turns a forward bias into a reversed one
	#define DEPTH_CONVENTION_NEARER( a, b )	( ( a ) > ( b ) )	// the "less" test: depth a is nearer than depth b
#else
	#define DEPTH_CONVENTION_CLEAR			1.0f
	#define DEPTH_CONVENTION_NEAR_NDC		0.0f
	#define DEPTH_CONVENTION_FAR_NDC		1.0f
	#define DEPTH_CONVENTION_BIAS_SIGN		1.0f
	#define DEPTH_CONVENTION_NEARER( a, b )	( ( a ) < ( b ) )
#endif

#endif /* !__DEPTHCONVENTION_H__ */
