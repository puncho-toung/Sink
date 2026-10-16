/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.
Copyright (C) 2015 Daniel Gibson
Copyright (C) 2016-2023 Robert Beckebans

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

// a GUI light editor, based loosely on the one from original Doom3 (neo/tools/radiant/LightDlg.*)
// LightInfo was CLightInfo, the LightEditor itself was written from scratch.

#ifndef NEO_TOOLS_EDITORS_LIGHTEDITOR_H_
#define NEO_TOOLS_EDITORS_LIGHTEDITOR_H_

#include <idlib/Dict.h>
#include "../../edit_public.h"

#include "../imgui/BFGimgui.h"
#include "../imgui/ImGuizmo.h"

#include "../../renderer/MathLight.h"

struct viewDef_t;

namespace ImGuiTools
{

enum ELightType
{
	LIGHT_POINT,
	LIGHT_SPOT,
	LIGHT_SUN
};

class LightInfo
{
public:
	ELightType	lightType;

	idStr		strTexture;
	bool		equalRadius;
	bool		explicitStartEnd;
	idVec3		lightStart;
	idVec3		lightEnd;
	idVec3		lightUp;
	idVec3		lightRight;
	idVec3		lightTarget;
	idVec3		lightCenter;
	idVec3		color;

	bool		hasLightOrigin;
	idVec3		origin;
	idAngles	angles;			// RBDOOM specific, saved to map as "angles"
	idVec3		scale;			// not saved to .map

	idVec3		lightRadius;
	bool		castShadows;
	bool		skipSpecular;
	bool		hasCenter;
	int			lightStyle;		// RBDOOM specific, saved to map as "style"

	mathLightData_t     mathLight;

	LightInfo();

	void		SetLightType( ELightType type );	// D45: also sets mathLight.type
	void		Defaults();
	void		DefaultPoint();
	void		DefaultProjected();
	void		DefaultSun();
	void		FromDict( const idDict* e );
	void		ToDict( idDict* e );
};

// D24 / plan 3.5: one light committed in this session, keyed by entityName. The locator part
// (isNew .. indexHint) is resolved once, at the light's first commit.
class LightCommit
{
public:
	idStr		entityName;
	bool		isNew;			// created in this session (Ctrl+D): in the in-memory map, nowhere on disk
	bool		writeName;		// the name was given to an unnamed map light and must reach the file
	bool		pending;		// dict not yet spliced into the .map
	idStr		name;			// the light's name key; may exist only in memory
	idStr		classname;
	idVec3		onDiskOrigin;	// meaningless while isNew
	int			indexHint;		// in-memory map index at resolution, -1 if unknown; never the file order

	idDict		dict;			// ToDict's dict of the last commit ("" = delete the key)
};

// plan 3.3: what AUTO keeps from the moment it was (re)entered ("autoBase")
class LightAutoBase
{
public:
	float		farHalfWidth;		// spot W0: half width of the far plane
	float		farHalfHeight;		// spot H0
	float		handedness;			// spot h0 = sign( ( right0 x up0 ) . target0 ), 0 counts as -1
	float		zNear;				// spot zNear0, used with explicit start/end
	float		centerLength;		// sun: max( |lightCenter0|, 1 )
	bool		haveFallback;		// 'fallback' holds the previous frame's horizontal vector
	idVec3		fallback;			// spot: the previous r (light-local)
};

// plan 3.1: easy mode's states
enum EEasyState
{
	EASY_NONE,			// E0: editLights on, no light selected
	EASY_AUTO,			// E1: the selected light tracks the reticle
	EASY_CHECKPOINT,	// E2: frozen at cur while the player roams
	EASY_MODAL			// E3: Q, E, R or T toggled on; camera and movement locked
};

class LightEditor
{
public:
	static const int	NUM_EASY_KEYS = 12;		// Q E R T F H, MOUSE1, MOUSE2, MWHEELUP, MWHEELDOWN, ENTER, KP_ENTER

private:
	bool				isShown;

	idStr				title;
	idStr				entityName;
	idVec3				entityPos;

	LightInfo			original;
	idDict				originalMath;	// the light's math_* keys at selection or its last commit; Cancel puts back exactly these
	LightInfo			cur; // current status of the light
	LightInfo			curNotMoving;

	idEntity*			lightEntity;

	idList<idStr>		textureNames;
	int					currentTextureIndex;
	idImage*			currentTexture;
	const idMaterial*	currentTextureMaterial;

	// RB: light style support
	idList<idStr>		styleNames;
	int					currentStyleIndex;

	// D62: the LightKind tab the panel last showed (true = Math); it mirrors ImGui's tab bar, not the light
	bool				tabShowsMath;

	ImGuizmo::OPERATION mCurrentGizmoOperation = ImGuizmo::TRANSLATE;
	ImGuizmo::MODE		mCurrentGizmoMode = ImGuizmo::WORLD;

	bool				useSnap = false;
	float				gridSnap[3] = { 4.0f, 4.0f, 4.0f };
	float				angleSnap = 15.0f;
	float				scaleSnap = 0.1f;
	float				bounds[6] = { -0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f };
	float				boundsSnap[3] = { 0.1f, 0.1f, 0.1f };
	bool				boundSizing = false;
	bool				boundSizingSnap = false;

	bool				shortcutSaveMapEnabled;
	bool				shortcutDuplicateLightEnabled;

	// D24: the locator inputs of the selected light, set by Init
	idVec3				spawnOrigin;			// the spawn dict's "origin"
	bool				entityNameFromDict;		// entityName is the light's own name key, not invented by Init
	bool				lightIsNew;				// the selected light was created by DuplicateLight
	bool				duplicateIsNew;			// set by DuplicateLight, read and cleared by Init

	// plan 3.5: survives Reset() and re-selection; cleared by closing the editor. D48: a pending entry
	// is a panel Apply not yet saved (Ctrl+S / Save Map); nothing writes it on its own
	idList<LightCommit>	committed;
	idStr				lastWarning;

	// plan 3.1 easy mode; outside Reset() because a selection re-enters Init -> Reset from RunFrame,
	// and Init resets only the per-light part (D31)
	bool				editorOpen;				// between LightEditorOpen and LightEditorClose
	bool				easyMode;				// S6: easy, or advanced (the panel)
	EEasyState			easyState;				// kept while advanced, restored by F
	int					modalKey;				// K_Q, K_E, K_R or K_T in EASY_MODAL
	idStaticList<int, 16>	pressQueue;			// plan 3.2: tool presses in arrival order
	bool				keyLatched[ NUM_EASY_KEYS ];	// down seen, up not yet: filters WM_KEYDOWN auto-repeat
	bool				keySwallowed[ NUM_EASY_KEYS ];	// the down was taken, so its up is taken too
	int					lastMouseX;				// usercmdGen->MouseState() reference for the deltas
	int					lastMouseY;
	int					modalX;					// raw mouse counts since modal entry (y > 0 = moved down)
	int					modalY;
	int					modalWheel;				// wheel clicks since modal entry, up positive
	int					lastRunFrame;			// idLib::frameNumber of the last RunFrame
	LightInfo			checkpointUndo;			// E2: what RMB restores
	LightInfo			modalBase;				// E3: modal results are functions of this and the counts (D17)
	LightAutoBase		autoBase;				// captured at every E1 entry
	idVec3				lastAim;				// the aim point P of the last AUTO frame, for the reticle
	bool				lastAimHit;
	bool				applyPending;			// ST-3: cur was written this frame and is not yet on the entity
	idStr				hintText;				// UI-5 / D48: a transient note on the state line ("saved <name>")
	int					hintUntilFrame;			// ... shown until this frame

	bool				HandlePress( int key );
	void				SwitchMode();
	void				EnterModal( int key );
	void				DrawEasyOverlay();

	void				LoadLightStyles();
	static bool			StyleItemsGetter( void* data, int idx, const char** out_text );

	void				Init( const idDict* dict, idEntity* light );
	void				Reset();

	void				LoadLightTextures();
	static bool			TextureItemsGetter( void* data, int idx, const char** out_text );
	void				LoadCurrentTexture();

	void				TempApplyChanges();
	bool				SaveChanges( bool flushNow, bool onlyThisLight );
	int					Flush( const char* onlyEntity );		// D48: only SaveChanges writes the .map
	int					PendingApplies() const;
	void				WarnUnsavedApplies() const;
	void				CancelChanges();

	void				DuplicateLight();

	int					FindCommit( const char* name ) const;
	idStr				InventLightName() const;
	int					ResolveLocator( LightCommit& commit, idDict& dict );
	bool				SetupToolView( viewDef_t& vd ) const;

	LightEditor()
	{
		isShown = false;

		spawnOrigin.Zero();
		entityNameFromDict = false;
		lightIsNew = false;
		duplicateIsNew = false;

		editorOpen = false;
		easyMode = true;
		easyState = EASY_NONE;
		modalKey = 0;
		for( int i = 0; i < NUM_EASY_KEYS; i++ )
		{
			keyLatched[i] = false;
			keySwallowed[i] = false;
		}
		lastMouseX = 0;
		lastMouseY = 0;
		modalX = 0;
		modalY = 0;
		modalWheel = 0;
		lastRunFrame = -2;
		memset( &autoBase, 0, sizeof( autoBase ) );
		lastAim.Zero();
		lastAimHit = false;
		applyPending = false;
		hintUntilFrame = 0;
		tabShowsMath = true;	// a new tab bar opens on its first tab

		Reset();
	}

public:

	static LightEditor&	Instance();
	static void			ReInit( const idDict* dict, idEntity* light );

	inline void			ShowIt( bool show )
	{
		isShown = show;
	}

	inline bool			IsShown() const
	{
		return isShown;
	}

	void				Draw();

	inline bool			IsOpen() const
	{
		return editorOpen;
	}

	inline bool			EasyModeActive() const
	{
		return editorOpen && easyMode;
	}

	inline bool			WantsViewLock() const
	{
		return EasyModeActive() && easyState == EASY_MODAL;
	}

	bool				KeyEvent( int keyNum, bool down );
	void				RunFrame();
	void				Open();
	void				Close();
	bool				Deselect( bool commit );
	void				EntityRemoved( idEntity* ent );
};

} //namespace ImGuiTools

#endif
