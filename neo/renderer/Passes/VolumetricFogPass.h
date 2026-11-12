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
#ifndef RENDERER_PASSES_VOLUMETRICFOGPASS_H_
#define RENDERER_PASSES_VOLUMETRICFOGPASS_H_

struct froxelView_t;			// opaque; track V defines it later

class VolumetricFogPass
{
public:
	VolumetricFogPass( nvrhi::IDevice* device, CommonRenderPasses* commonPasses );

	void	DrawSkyMask( nvrhi::ICommandList* commandList, const viewDef_t* viewDef );	// opens no main block
	void	Render( nvrhi::ICommandList* commandList, const viewDef_t* viewDef );		// inject, resolve, integrate
	void	Apply( nvrhi::ICommandList* commandList, const viewDef_t* viewDef );		// the composite
	void	DebugTools( nvrhi::ICommandList* commandList, const viewDef_t* viewDef );	// track V's debug overlay, called from DBG_RenderDebugTools

private:
	nvrhi::DeviceHandle		m_Device;
	CommonRenderPasses*		commonRenderPasses;
};

#endif // RENDERER_PASSES_VOLUMETRICFOGPASS_H_
