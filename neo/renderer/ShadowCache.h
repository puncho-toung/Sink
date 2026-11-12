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
#ifndef __SHADOWCACHE_H__
#define __SHADOWCACHE_H__

/*
==========================================================================================

SHADOW CACHE SEAMS ( X0 )

The hooks track S fills; in X0 every body is a no-op; the calls are in R_RenderView,
SwapCommandBuffers_FinishRendering, RenderWorld.cpp and DBG_RenderDebugTools.
Included from RenderCommon.h, so it sees the complete idRenderEntityLocal /
idRenderLightLocal / renderEntity_t / renderLight_t.

==========================================================================================
*/

enum shadowCacheNotify_t
{
	SC_UPDATE,
	SC_FREE
};

struct shadowCacheView_t;		// opaque; track S defines it later

void	R_ShadowCacheDecide();		// frontend, called from R_RenderView just before R_AddDrawViewCmd
void	R_ShadowCachePublish();		// called from SwapCommandBuffers_FinishRendering after GL_BlockingSwapBuffers

// re / rl is the NEW description on SC_UPDATE ( the hook compares it with def->parms /
// light->parms ) and NULL on SC_FREE
void	R_ShadowCacheNotifyEntity( const idRenderEntityLocal* def, const renderEntity_t* re, shadowCacheNotify_t op );
void	R_ShadowCacheNotifyLight( const idRenderLightLocal* light, const renderLight_t* rl, shadowCacheNotify_t op );

void	R_ShadowCacheDebugTools( nvrhi::ICommandList* commandList, const viewDef_t* viewDef );	// track S's debug overlay, called from DBG_RenderDebugTools

// C3: the one test of "this view light has a shadow map to sample". The body is track S's and lives in
// this header so that it inlines at its call sites in the backend ( once per interaction draw ).
ID_INLINE bool R_ViewLightHasShadowMap( const viewLight_t* vLight )
{
	return vLight->shadowLOD != -1 && vLight->globalShadows != NULL;
}

#endif // __SHADOWCACHE_H__
