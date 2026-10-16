/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 GPL Source Code (?Doom 3 Source Code?).

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

#ifndef __EDIT_PUBLIC_H__
#define __EDIT_PUBLIC_H__

/*
===============================================================================

	Editors.

===============================================================================
*/


namespace ImGuiTools
{

void	SetReleaseToolMouse( bool doRelease );

bool	ReleaseMouseForTools();

bool	AreEditorsActive();

void	DrawToolWindows();

void	LightEditorInit( const idDict* dict, idEntity* entity );

// light editor easy mode (light-editor-easy-mode plan 3.2)
bool	EasyModeActive();								// light editor open and in easy mode
bool	ToolWantsViewLock();							// a modal (Q/E/R/T) is active: camera and movement locked
bool	ToolKeyEvent( int keyNum, bool down );			// every SE_KEY, first thing in ImGuiHook::InjectSysEvent; true = swallow
void	RunToolFrame();									// once per frame from idCommonLocal::Draw; no ImGui frame or draw call
bool	LightEditorIsOpen();							// between LightEditorOpen and LightEditorClose; decides the editLights toggle
void	LightEditorOpen();								// editLights on: nothing shown until a light is picked (then the advanced panel), or the easy overlay with le_startEasy 1
void	LightEditorClose();								// editLights off, map shutdown: never saves, idempotent; one console warning for dropped panel Applies, no ImGui call
void	LightEditorDeselect();							// the deselect routine with revert, for the editLights off path (D30)
void	LightEditorEntityRemoved( idEntity* ent );		// from idGameLocal::UnregisterEntity (D32)

void	AfEditorInit();

}

#endif /* !__EDIT_PUBLIC_H__ */
