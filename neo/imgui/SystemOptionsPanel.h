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

===========================================================================
*/

#ifndef NEO_IMGUI_SYSTEMOPTIONSPANEL_H_
#define NEO_IMGUI_SYSTEMOPTIONSPANEL_H_

#include "../sys/sys_public.h"

class idFile;
struct ImFontAtlas;

/*
================================================
SystemOptions

The System Options panel ( 2026-11-26 ): the display, graphics and sound settings as a Dear ImGui window over the
shell menus, three quarters of the screen, two columns of collapsible sections, one for each renderer system, each
row a slider with a box for a typed value, a switch or a dropdown. It replaces the SWF System Options list, which
showed one value at a time and stepped through it with Left and Right; that screen stays in the tree and is shown
when ImGui is not up.

The panel is modal. While it is open it takes every input event after the console and an open dialog
( idCommonLocal::ProcessEvent ), so the shell beneath it never sees a key. Changes apply as they are made and the
panel saves on closing, as the SWF screen did: archived cvars through the config's own lines, the panel's other
cvars as setSystemOption lines in the same file ( only those the player set in the panel ). Revert gives back
the values the panel opened with.
================================================
*/
namespace SystemOptions
{

// opens the panel over the shell; false when ImGui is not up, and the caller shows the SWF screen instead
bool	Open();

// closes the panel: saves the config if a value changed, and offers a restart for a setting that needs one
void	Close();

bool	IsOpen();

// true while the game should show through: the player holds Preview ( the panel hidden ), or adjusts a section in a
// game ( the rest of the panel at a tenth ); the shell's menu is then not drawn, so that the picture shows undimmed
bool	HidesShell();

// true when the panel took the event: every input event while it is open, and the release of the key that closed it
bool	HandleEvent( const sysEvent_t* event );

// draws the panel inside ImGuiHook's frame, after the tool windows
void	Draw();

// adds the panel's fonts to ImGui's atlas, after the default font, before the atlas is built ( ImGuiHook::Init )
void	AddFonts( ImFontAtlas* atlas );

// appends the setSystemOption lines to the config file ( idCommonLocal::WriteConfigToFile )
void	WriteConfig( idFile* f );

} // namespace SystemOptions

#endif /* NEO_IMGUI_SYSTEMOPTIONSPANEL_H_ */
