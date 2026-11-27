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
#include "precompiled.h"
#pragma hdrstop

#include "BFGimgui.h"
#include "imgui_internal.h"		// the slider's and the switch's behaviour, NavMoveRequestCancel
#include "ImGui_Hooks.h"
#include "SystemOptionsPanel.h"
#include "../renderer/RenderCommon.h"	// vidMode_t, R_GetModeListForDisplay
#include "../sound/sound.h"				// DB_SILENCE

// SystemOptionsFonts.cpp: Inter regular and semibold, each the base64 text of a whole .ttf file
extern const char* const systemOptionsFontRegular;
extern const char* const systemOptionsFontSemiBold;

/*
===============================================================================

	The table

	One row is one control. Its kind says how it is drawn; a slider's range is the bar's, and a value typed into
	its box may go beyond it to the cvar's own bounds, which the cvar system keeps. A special row drives more than
	one cvar, or maps its value, the way the SWF screen did ( brightness, view distance, volume, display mode ).

===============================================================================
*/

enum optionKind_t
{
	OPTION_FLOAT,		// a slider and a box for a typed value
	OPTION_INT,			// the same, in whole numbers
	OPTION_TOGGLE,		// a switch
	OPTION_CHOICE,		// a dropdown
	OPTION_SPECIAL		// hand-coded below
};

enum optionFlags_t
{
	OPTF_LOG			= BIT( 0 ),		// the slider's scale is logarithmic
	OPTF_RESTART		= BIT( 1 ),		// takes effect when the game starts again
	OPTF_MAP_RELOAD		= BIT( 3 ),		// takes effect at the next map load
	OPTF_SUMMARY		= BIT( 4 ),		// the value is shown in its section's header
	OPTF_NO_REVERT		= BIT( 5 ),		// Revert and Defaults leave it alone ( the display mode, which has its own confirmation )
	OPTF_INVERT			= BIT( 6 ),		// a float shown as 1 - value ( TAA's strength is its new frame's weight turned round )
	OPTF_ADVANCED		= BIT( 7 ),		// shown only while the header's Advanced switch is on
	OPTF_NO_VIEW		= BIT( 8 )		// nothing to see in the game: a change does not fade the panel ( sound )
};

// a parent condition's mask: one bit for each value the parent may hold, from -1 up ( a fog quality of -1 is Custom )
static constexpr int V( const int value )
{
	return 1 << ( value + 1 );
}
static const int MAX_PARENTS = 4;		// WP-J24: a spike's row has four ( its shape, the transform, the bloom, the render mode )

enum specialOption_t
{
	SPECIAL_NONE,
	SPECIAL_DISPLAY_MODE,	// r_fullscreen and r_vidMode, from the Resolution screen's list
	SPECIAL_RENDER_API,		// r_graphicsAPI, a string ( Windows )
	SPECIAL_BRIGHTNESS,		// r_exposure 0 to 1 with r_lightScale 2 to 4, as a percentage
	SPECIAL_VIEW_DISTANCE,	// r_viewDistance in units, with r_distanceCull 0 at the top of the bar
	SPECIAL_VOLUME			// s_volume_dB as a percentage of loudness
};

struct optionChoice_t
{
	int				value;
	const char*		label;
};

struct optionRow_t
{
	const char*				label;
	const char*				cvarName;
	const char*				cvarName2;		// the second cvar of a special row, or NULL
	optionKind_t			kind;
	specialOption_t			special;
	float					minValue;		// the bar's range
	float					maxValue;
	float					step;			// one press of Left or Right
	const char*				format;			// printf, for the value as shown
	int						flags;
	const optionChoice_t*	choices;
	int						numChoices;
	const char*				parentCvar[MAX_PARENTS];	// the row is greyed out unless each parent's integer value is in its mask
	int						parentMask[MAX_PARENTS];	// V( value ) bits
	const char*				parentHint[MAX_PARENTS];	// why it is greyed out
	const char*				description;
	const char*				summaryLabel;	// the row's name in its section's header ( "" = the value alone )

	// the table below is written in these; each returns the row, so that they chain
	optionRow_t&	Desc( const char* text )
	{
		description = text;
		return *this;
	}
	optionRow_t&	Flags( const int f )
	{
		flags |= f;
		return *this;
	}
	optionRow_t&	Summary( const char* shortLabel )
	{
		flags |= OPTF_SUMMARY;
		summaryLabel = shortLabel;
		return *this;
	}
	optionRow_t&	Parent( const char* cvar, const int mask, const char* hint )
	{
		for( int i = 0; i < MAX_PARENTS; i++ )
		{
			if( parentCvar[i] == NULL )
			{
				parentCvar[i] = cvar;
				parentMask[i] = mask;
				parentHint[i] = hint;
				return *this;
			}
		}
		assert( false );	// a third parent: raise MAX_PARENTS
		return *this;
	}
};

struct optionSection_t
{
	const char*		name;
	int				column;
	int				firstRow;
	int				numRows;
};

// what the panel keeps of a row while it runs
struct optionState_t
{
	idCVar*			cvar;			// NULL hides the row ( a cvar this build does not have )
	idCVar*			cvar2;
	idStr			openValue;		// when the panel opened: Revert, and whether anything changed
	idStr			openValue2;
	bool			saved;			// a non-archived row the player set in the panel: kept in the config
	idStr			savedValue;
	bool			openSaved;
	idStr			openSavedValue;
	char			editText[32];	// the box's text while it is not being typed into
};

static const int MAX_SECTIONS = 16;

static idList<optionRow_t>		rows;
static idList<optionState_t>	states;
static idList<optionSection_t>	sections;

struct displayMode_t
{
	int				fullscreen;		// r_fullscreen
	int				vidMode;		// r_vidMode
	idStr			label;
};

static struct systemOptionsPanel_t
{
	bool						isOpen;
	bool						firstFrame;		// the frame after Open: the default focus
	bool						closeRequested;
	bool						revertRequested;
	bool						defaultsRequested;
	int							defaultsSection;	// -1 = every section
	int							focusRequest;		// a header to give the keyboard focus to, -1 = none
	int							focusedSection;		// the section that held the focus in the last frame
	bool						resetFocused;		// Delete or pad X: the focused row goes back to its default
	bool						previewHeld;		// pad Y held
	bool						previewButtonHeld;	// the Preview button held
	bool						padInUse;			// the last key came from a pad: the hints show its buttons
	bool						mouseSeen;			// the mouse moved since the panel opened: ImGui draws its pointer
	int							swallowRelease;		// the key whose release still belongs to the panel after it closed
	bool						navWasOn;			// ImGui's keyboard navigation before the panel opened
	float						mouseX;
	float						mouseY;
	char						filter[64];
	bool						initialized;		// openSection has its first value
	int							openSection;		// the one section that is open ( -1 = none ): opening another closes it
	int							scrollToSection;	// a section just opened, to scroll into view once its rows are drawn
	int							viewSection;		// in a game, the open section being adjusted: the rest of the panel fades
	bool						viewSectionDrawn;	// it was drawn this frame ( a search can hide it )
	float						viewFade;			// 0 = the whole panel, 1 = the rest at a tenth, eased over 0.15 s
	float						viewPlateHeight;	// the adjusted section's height last frame, for the plate behind it
	bool						sectionDrawn[MAX_SECTIONS];	// last frame: a search can leave a section with nothing to show
	bool						windowFocused;		// last frame: the panel's window had the focus
	int							contextGeneration;	// the ImGui context the panel last drew in: a renderer restart makes a new one
	int							scrollFrame;		// ImGui's frame after which scrollToSection may scroll ( the old section closes first )
	bool						viewFocusSeen;		// this frame, the keyboard focus was in the adjusted section
	int							heldAtOpen[8];		// keys down when the panel opened: their repeats and releases are not the panel's
	int							numHeldAtOpen;
	idList<displayMode_t>		displayModes;
} panel;

/*
========================
AddSection
========================
*/
static void AddSection( const char* name, const int column )
{
	assert( sections.Num() < MAX_SECTIONS );
	optionSection_t& section = sections.Alloc();
	section.name = name;
	section.column = column;
	section.firstRow = rows.Num();
	section.numRows = 0;
}

/*
========================
AddRow
========================
*/
static optionRow_t& AddRow( const char* label, const char* cvarName, const optionKind_t kind )
{
	assert( sections.Num() > 0 );
	optionRow_t& row = rows.Alloc();
	memset( &row, 0, sizeof( row ) );
	row.label = label;
	row.cvarName = cvarName;
	row.kind = kind;
	row.format = "%.2f";
	row.description = "";
	sections[sections.Num() - 1].numRows++;
	return row;
}

static optionRow_t& Slider( const char* label, const char* cvarName, const float minValue, const float maxValue, const float step, const char* format )
{
	optionRow_t& row = AddRow( label, cvarName, OPTION_FLOAT );
	row.minValue = minValue;
	row.maxValue = maxValue;
	row.step = step;
	row.format = format;
	return row;
}

static optionRow_t& SliderInt( const char* label, const char* cvarName, const int minValue, const int maxValue, const int step, const char* format )
{
	optionRow_t& row = AddRow( label, cvarName, OPTION_INT );
	row.minValue = ( float )minValue;
	row.maxValue = ( float )maxValue;
	row.step = ( float )step;
	row.format = format;
	return row;
}

static optionRow_t& Toggle( const char* label, const char* cvarName )
{
	return AddRow( label, cvarName, OPTION_TOGGLE );
}

template< int N >
static optionRow_t& Choice( const char* label, const char* cvarName, const optionChoice_t ( &choices )[N] )
{
	optionRow_t& row = AddRow( label, cvarName, OPTION_CHOICE );
	row.choices = choices;
	row.numChoices = N;
	return row;
}

static optionRow_t& Special( const char* label, const char* cvarName, const char* cvarName2, const specialOption_t special, const float minValue, const float maxValue, const float step )
{
	optionRow_t& row = AddRow( label, cvarName, OPTION_SPECIAL );
	row.cvarName2 = cvarName2;
	row.special = special;
	row.minValue = minValue;
	row.maxValue = maxValue;
	row.step = step;
	row.format = "%.0f";
	return row;
}

#include "SystemOptionsTable.inl"

/*
===============================================================================

	Values

===============================================================================
*/

/*
========================
RowLocked

A cvar that is not archived is a cheat to the cvar system, which refuses it in multiplayer ( idInternalCVar::Set )
========================
*/
static bool RowLocked( const idCVar* cvar )
{
	return cvar != NULL && ( cvar->GetFlags() & CVAR_CHEAT ) != 0 && common->IsMultiplayer() && !cvarSystem->GetCVarBool( "net_allowCheats" );
}

/*
========================
RowEnabled
========================
*/
static bool RowEnabled( const optionRow_t& row, const idCVar* cvar, const char** hint )
{
	*hint = NULL;
	for( int i = 0; i < MAX_PARENTS && row.parentCvar[i] != NULL; i++ )
	{
		const int value = cvarSystem->GetCVarInteger( row.parentCvar[i] );
		if( value < -1 || value > 29 || ( row.parentMask[i] & V( value ) ) == 0 )
		{
			*hint = row.parentHint[i];
			return false;
		}
	}
	if( RowLocked( cvar ) )
	{
		*hint = "Locked in multiplayer.";
		return false;
	}
	return true;
}

/*
========================
RowShown

The Advanced switch's rows hide while it is off
========================
*/
static idCVar ui_systemOptionsAdvanced( "ui_systemOptionsAdvanced", "1", CVAR_GUI | CVAR_BOOL | CVAR_ARCHIVE, "the System Options panel shows its advanced settings too" );

static bool RowShown( const optionRow_t& row, const optionState_t& state )
{
	if( state.cvar == NULL || ( row.cvarName2 != NULL && state.cvar2 == NULL ) )
	{
		return false;
	}
	return ( row.flags & OPTF_ADVANCED ) == 0 || ui_systemOptionsAdvanced.GetBool();
}

/*
========================
RowGetFloat

A float row's value as its bar shows it
========================
*/
static float RowGetFloat( const optionRow_t& row, const optionState_t& state )
{
	const float value = state.cvar->GetFloat();
	return ( row.flags & OPTF_INVERT ) ? 1.0f - value : value;
}

/*
========================
RowSetFloat

A value from the bar, the keys or the box. A cvar with bounds keeps to them by itself; one without is held to the
bar's range, since some are loop counts on the GPU ( r_ssrMaxDistance is also SSR's step cap )
========================
*/
static void RowSetFloat( const optionRow_t& row, optionState_t& state, float value )
{
	if( !( state.cvar->GetMinValue() < state.cvar->GetMaxValue() ) )
	{
		value = idMath::ClampFloat( row.minValue, row.maxValue, value );
	}
	state.cvar->SetFloat( ( row.flags & OPTF_INVERT ) ? 1.0f - value : value );
}

/*
========================
RowSetInteger
========================
*/
static void RowSetInteger( const optionRow_t& row, optionState_t& state, int value )
{
	if( !( state.cvar->GetMinValue() < state.cvar->GetMaxValue() ) )
	{
		value = idMath::ClampInt( idMath::Ftoi( row.minValue ), idMath::Ftoi( row.maxValue ), value );
	}
	state.cvar->SetInteger( value );
}

/*
========================
SpecialGet

The value a special row's bar shows
========================
*/
static float SpecialGet( const optionRow_t& row, const optionState_t& state )
{
	switch( row.special )
	{
		case SPECIAL_BRIGHTNESS:
			return idMath::ClampFloat( 0.0f, 100.0f, state.cvar->GetFloat() * 100.0f );
		case SPECIAL_VIEW_DISTANCE:
			// the top of the bar is distance culling off, whatever r_viewDistance holds
			return state.cvar2->GetBool() ? state.cvar->GetFloat() : row.maxValue;
		case SPECIAL_VOLUME:
			return 100.0f * Square( 1.0f - ( state.cvar->GetFloat() / DB_SILENCE ) );
		default:
			return 0.0f;
	}
}

/*
========================
SpecialSet
========================
*/
static void SpecialSet( const optionRow_t& row, optionState_t& state, float value )
{
	value = idMath::ClampFloat( row.minValue, row.maxValue, value );
	switch( row.special )
	{
		case SPECIAL_BRIGHTNESS:
		{
			// as the SWF screen: exposure 0 to 1 and the light scale 2 to 4 move together
			const float percent = idMath::Rint( value );
			state.cvar->SetFloat( percent / 100.0f );
			state.cvar2->SetFloat( 2.0f + 2.0f * percent / 100.0f );
			break;
		}
		case SPECIAL_VIEW_DISTANCE:
		{
			const float units = idMath::Rint( value );
			if( units >= row.maxValue )
			{
				state.cvar->SetFloat( row.maxValue );
				state.cvar2->SetBool( false );
			}
			else
			{
				state.cvar->SetFloat( units );
				state.cvar2->SetBool( true );
			}
			break;
		}
		case SPECIAL_VOLUME:
		{
			const float percent = idMath::Rint( value );
			state.cvar->SetFloat( DB_SILENCE - ( idMath::Sqrt( percent / 100.0f ) * DB_SILENCE ) );
			break;
		}
		default:
			break;
	}
}

/*
========================
PanelText

printf into one of eight rotating buffers that outlive the expression: this tree's va() returns a temporary
idStr's text, which a function cannot return
========================
*/
static const char* PanelText( const char* format, ... )
{
	static char buffers[8][128];
	static int next = 0;
	char* buffer = buffers[next];
	next = ( next + 1 ) & 7;
	va_list args;
	va_start( args, format );
	idStr::vsnPrintf( buffer, sizeof( buffers[0] ), format, args );
	va_end( args );
	return buffer;
}

/*
========================
SpecialFormat
========================
*/
static const char* SpecialFormat( const optionRow_t& row, const float value )
{
	switch( row.special )
	{
		case SPECIAL_VIEW_DISTANCE:
			return ( value >= row.maxValue ) ? "Unlimited" : PanelText( "%.0f", value );
		default:
			return PanelText( "%.0f%%", value );
	}
}

/*
========================
RowIsDefault
========================
*/
static bool RowIsDefault( const optionRow_t& row, const optionState_t& state )
{
	if( state.cvar == NULL )
	{
		return true;
	}
	switch( row.kind )
	{
		case OPTION_FLOAT:
			return idMath::Fabs( state.cvar->GetFloat() - atof( state.cvar->GetDefaultString() ) ) < 1e-5f;
		case OPTION_INT:
		case OPTION_CHOICE:
			return state.cvar->GetInteger() == atoi( state.cvar->GetDefaultString() );
		case OPTION_TOGGLE:
			return state.cvar->GetBool() == ( atoi( state.cvar->GetDefaultString() ) != 0 );
		default:
			break;
	}
	if( row.special == SPECIAL_DISPLAY_MODE )
	{
		return true;	// no default: it is the screen the player has
	}
	if( idStr::Icmp( state.cvar->GetString(), state.cvar->GetDefaultString() ) != 0 &&
			idMath::Fabs( state.cvar->GetFloat() - atof( state.cvar->GetDefaultString() ) ) > 1e-5f )
	{
		return false;
	}
	if( state.cvar2 != NULL && idMath::Fabs( state.cvar2->GetFloat() - atof( state.cvar2->GetDefaultString() ) ) > 1e-5f )
	{
		return false;
	}
	return true;
}

/*
========================
RowIsChanged

Since the panel opened
========================
*/
static bool RowIsChanged( const optionState_t& state )
{
	if( state.cvar == NULL )
	{
		return false;
	}
	if( idStr::Cmp( state.cvar->GetString(), state.openValue ) != 0 )
	{
		return true;
	}
	return state.cvar2 != NULL && idStr::Cmp( state.cvar2->GetString(), state.openValue2 ) != 0;
}

/*
========================
RowChanged

After every change the panel makes. The SWF screen did the same: the archive flag is cleared, so that a slider's
sweep does not write the config once a frame; Close sets it again when anything changed.
========================
*/
static void RowChanged()
{
	cvarSystem->ClearModifiedFlags( CVAR_ARCHIVE );
}

/*
========================
RowNoteSet

After the panel or a setSystemOption line set a row: a non-archived row off its default is kept in the config with
the value it now holds; one at its default needs no line, so that a later change of the default reaches this machine
========================
*/
static void RowNoteSet( const optionRow_t& row, optionState_t& state )
{
	if( state.cvar == NULL || ( row.flags & OPTF_NO_REVERT ) != 0 || ( state.cvar->GetFlags() & CVAR_ARCHIVE ) != 0 )
	{
		return;
	}
	if( RowIsDefault( row, state ) )
	{
		state.saved = false;
		state.savedValue.Clear();
	}
	else
	{
		state.saved = true;
		state.savedValue = state.cvar->GetString();
	}
}

/*
========================
RowSetDefault
========================
*/
static void RowSetDefault( const optionRow_t& row, optionState_t& state )
{
	if( state.cvar == NULL || ( row.flags & OPTF_NO_REVERT ) != 0 || RowLocked( state.cvar ) )
	{
		return;
	}
	state.cvar->SetString( state.cvar->GetDefaultString() );
	if( state.cvar2 != NULL )
	{
		state.cvar2->SetString( state.cvar2->GetDefaultString() );
	}
	RowNoteSet( row, state );
	RowChanged();
}

/*
========================
StepValue

The next step of a bar from a value, in whole steps from its low end: a value on a step moves one step; one between
steps ( typed, or a default off the steps ) lands on the next step in that direction. A hundredth of a step counts as
on it, so a float that the cvar's six decimals rounded is not taken for one between steps
========================
*/
static float StepValue( const float value, const float minValue, const float maxValue, const float step, const int direction )
{
	const float position = ( value - minValue ) / step;
	const float nearest = idMath::Rint( position );
	float index;
	if( idMath::Fabs( position - nearest ) < 0.01f )
	{
		index = nearest + direction;
	}
	else
	{
		index = ( direction > 0 ) ? idMath::Ceil( position ) : idMath::Floor( position );
	}
	return idMath::ClampFloat( minValue, maxValue, minValue + index * step );
}

/*
========================
RowStep

One press of Left or Right on a focused row: true when it moved
========================
*/
static bool RowStep( const optionRow_t& row, optionState_t& state, const int direction )
{
	switch( row.kind )
	{
		case OPTION_FLOAT:
		case OPTION_INT:
		{
			const float value = ( row.kind == OPTION_INT ) ? ( float )state.cvar->GetInteger() : RowGetFloat( row, state );
			const float stepped = StepValue( value, row.minValue, row.maxValue, row.step, direction );
			if( idMath::Fabs( stepped - value ) < row.step * 0.01f )
			{
				return false;
			}
			if( row.kind == OPTION_INT )
			{
				RowSetInteger( row, state, idMath::Ftoi( idMath::Rint( stepped ) ) );
			}
			else
			{
				RowSetFloat( row, state, stepped );
			}
			return true;
		}
		case OPTION_TOGGLE:
		{
			const bool on = direction > 0;
			if( state.cvar->GetBool() == on )
			{
				return false;
			}
			state.cvar->SetBool( on );
			return true;
		}
		case OPTION_CHOICE:
		{
			const int value = state.cvar->GetInteger();
			int next = -1;
			for( int i = 0; i < row.numChoices; i++ )
			{
				if( row.choices[i].value == value )
				{
					next = i + direction;
					break;
				}
			}
			if( next < 0 && ( row.numChoices == 0 || row.choices[0].value != value ) )
			{
				// a value the list does not hold ( typed in the console ): the nearest choice in that direction, the list
				// being in ascending order
				next = ( direction > 0 ) ? row.numChoices - 1 : 0;
				for( int i = 0; i < row.numChoices; i++ )
				{
					if( direction > 0 && row.choices[i].value > value )
					{
						next = i;
						break;
					}
					if( direction < 0 && row.choices[i].value < value )
					{
						next = i;
					}
				}
			}
			// a list does not wrap here: holding Right stops at the last choice
			next = idMath::ClampInt( 0, row.numChoices - 1, next );
			if( row.choices[next].value == value )
			{
				return false;
			}
			state.cvar->SetInteger( row.choices[next].value );
			return true;
		}
		case OPTION_SPECIAL:
		{
			if( row.special == SPECIAL_DISPLAY_MODE || row.special == SPECIAL_RENDER_API )
			{
				return false;	// they apply on Accept, through their dropdowns
			}
			const float value = SpecialGet( row, state );
			const float stepped = StepValue( value, row.minValue, row.maxValue, row.step, direction );
			if( idMath::Fabs( stepped - value ) < row.step * 0.01f )
			{
				return false;
			}
			SpecialSet( row, state, stepped );
			return true;
		}
	}
	return false;
}

/*
===============================================================================

	Display mode

	The same list the SWF Resolution screen offers, applied the way it applied it: a window or the borderless
	fullscreen at once, an exclusive fullscreen mode behind the engine's twenty-second accept-or-revert dialog.

===============================================================================
*/

/*
========================
BuildDisplayModes
========================
*/
static void BuildDisplayModes()
{
	panel.displayModes.Clear();

	displayMode_t& windowed = panel.displayModes.Alloc();
	windowed.fullscreen = 0;
	windowed.vidMode = 0;
	windowed.label = "Windowed";

	displayMode_t& borderless = panel.displayModes.Alloc();
	borderless.fullscreen = -2;
	borderless.vidMode = 0;
	borderless.label = "Borderless Fullscreen";

	idList< idList<vidMode_t> > displays;
	for( int displayNum = 0; ; displayNum++ )
	{
		idList<vidMode_t>& modeList = displays.Alloc();
		if( !R_GetModeListForDisplay( displayNum, modeList ) )
		{
			displays.RemoveIndex( displays.Num() - 1 );
			break;
		}
	}

	for( int displayNum = 0; displayNum < displays.Num(); displayNum++ )
	{
		const idList<vidMode_t>& modeList = displays[displayNum];
		for( int i = 0; i < modeList.Num(); i++ )
		{
			displayMode_t& mode = panel.displayModes.Alloc();
			mode.fullscreen = displayNum + 1;
			mode.vidMode = i;
			mode.label.Clear();
			if( displays.Num() > 1 )
			{
				mode.label += va( "Monitor %d: ", displayNum + 1 );
			}
			mode.label += va( "Fullscreen %d x %d", modeList[i].width, modeList[i].height );
			if( modeList[i].displayHz != 60 )
			{
				mode.label += va( " @ %d Hz", modeList[i].displayHz );
			}
		}
	}
}

/*
========================
CurrentDisplayMode
========================
*/
static int CurrentDisplayMode()
{
	const int fullscreen = cvarSystem->GetCVarInteger( "r_fullscreen" );
	const int vidMode = cvarSystem->GetCVarInteger( "r_vidMode" );
	for( int i = 0; i < panel.displayModes.Num(); i++ )
	{
		const displayMode_t& mode = panel.displayModes[i];
		if( mode.fullscreen == fullscreen && ( fullscreen <= 0 || mode.vidMode == vidMode ) )
		{
			return i;
		}
	}
	return -1;
}

/*
================================================
idSWFScriptFunction_AcceptDisplayMode

The dialog's two answers: keep the new mode, or go back to the one before ( also when the timer runs out )
================================================
*/
class idSWFScriptFunction_AcceptDisplayMode : public idSWFScriptFunction_RefCounted
{
public:
	idSWFScriptFunction_AcceptDisplayMode( const bool accept_, const int fullscreen_, const int vidMode_ ) :
		accept( accept_ ),
		fullscreen( fullscreen_ ),
		vidMode( vidMode_ )
	{
	}

	idSWFScriptVar Call( idSWFScriptObject* thisObject, const idSWFParmList& parms )
	{
		common->Dialog().ClearDialog( GDM_CONFIRM_VIDEO_CHANGES );
		if( accept )
		{
			cvarSystem->SetModifiedFlags( CVAR_ARCHIVE );
		}
		else
		{
			cvarSystem->SetCVarInteger( "r_fullscreen", fullscreen );
			cvarSystem->SetCVarInteger( "r_vidMode", vidMode );
			cvarSystem->ClearModifiedFlags( CVAR_ARCHIVE );
			cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "vid_restart\n" );
		}
		return idSWFScriptVar();
	}

private:
	bool	accept;
	int		fullscreen;
	int		vidMode;
};

/*
========================
ApplyDisplayMode
========================
*/
static void ApplyDisplayMode( const int index )
{
	if( index < 0 || index >= panel.displayModes.Num() || index == CurrentDisplayMode() )
	{
		return;
	}
	const displayMode_t& mode = panel.displayModes[index];
	const int oldFullscreen = cvarSystem->GetCVarInteger( "r_fullscreen" );
	const int oldVidMode = cvarSystem->GetCVarInteger( "r_vidMode" );

	cvarSystem->SetCVarInteger( "r_fullscreen", mode.fullscreen );
	if( mode.fullscreen > 0 )
	{
		cvarSystem->SetCVarInteger( "r_vidMode", mode.vidMode );
	}
	cvarSystem->ClearModifiedFlags( CVAR_ARCHIVE );
	cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "vid_restart\n" );

	if( mode.fullscreen > 0 )
	{
		// an exclusive mode the screen cannot show would leave the player blind: keep it only when they say so
		common->Dialog().AddDialog( GDM_CONFIRM_VIDEO_CHANGES, DIALOG_TIMER_ACCEPT_REVERT,
									new( TAG_SWF ) idSWFScriptFunction_AcceptDisplayMode( true, mode.fullscreen, mode.vidMode ),
									new( TAG_SWF ) idSWFScriptFunction_AcceptDisplayMode( false, oldFullscreen, oldVidMode ), false );
	}
	else
	{
		cvarSystem->SetModifiedFlags( CVAR_ARCHIVE );
	}
}

/*
===============================================================================

	Look

===============================================================================
*/

// font indices in ImGui's atlas: the default font is 0 ( the tools keep it ), then three sizes of three faces
static const int	FONT_TIERS = 4;
static const float	fontTierSizes[FONT_TIERS] = { 15.0f, 18.0f, 24.0f, 36.0f };	// 900, 1080, 1440 and 2160 lines
static const float	TITLE_FONT_RATIO = 1.5f;
enum panelFont_t
{
	PANEL_FONT_BODY,
	PANEL_FONT_STRONG,
	PANEL_FONT_TITLE,
	PANEL_FONT_FACES
};

static const ImWchar glyphRanges[] =
{
	0x0020, 0x00FF,		// Latin-1
	0x2013, 0x2026,		// dashes, quotes, bullet, ellipsis
	0x2190, 0x2193,		// arrows
	0x21BA, 0x21BB,		// the reset arrow
	0x2212, 0x2212,		// minus
	0x2264, 0x2265,
	0x25B6, 0x25CF,		// triangles, circles
	0x26A0, 0x26A0,		// warning sign
	0x2713, 0x2713,		// check mark
	0
};

static byte* fontRegularData = NULL;
static int fontRegularSize = 0;
static byte* fontSemiBoldData = NULL;
static int fontSemiBoldSize = 0;

/*
========================
DecodeFont
========================
*/
static void DecodeFont( const char* base64, byte*& data, int& size )
{
	if( data != NULL )
	{
		return;	// once: the atlas is built again from the same bytes after every vid_restart
	}
	idBase64 text;
	text = idStr( base64 );
	data = ( byte* )Mem_Alloc( text.DecodeLength(), TAG_IDLIB );
	size = text.Decode( data );
}

namespace SystemOptions
{

/*
========================
SystemOptions::AddFonts

The atlas is the same at every call, so the font image built from the first one stays right after a renderer
restart builds ImGui again ( ImGuiHook::NotifyDisplaySizeChanged keeps that image )
========================
*/
void AddFonts( ImFontAtlas* atlas )
{
	DecodeFont( systemOptionsFontRegular, fontRegularData, fontRegularSize );
	DecodeFont( systemOptionsFontSemiBold, fontSemiBoldData, fontSemiBoldSize );

	atlas->AddFontDefault();	// index 0: the tools' font, as before

	ImFontConfig config;
	config.FontDataOwnedByAtlas = false;
	config.OversampleH = 1;
	config.OversampleV = 1;
	config.PixelSnapH = true;

	for( int tier = 0; tier < FONT_TIERS; tier++ )
	{
		const float size = fontTierSizes[tier];
		atlas->AddFontFromMemoryTTF( fontRegularData, fontRegularSize, size, &config, glyphRanges );
		atlas->AddFontFromMemoryTTF( fontSemiBoldData, fontSemiBoldSize, size, &config, glyphRanges );
		atlas->AddFontFromMemoryTTF( fontSemiBoldData, fontSemiBoldSize, idMath::Rint( size * TITLE_FONT_RATIO ), &config, atlas->GetGlyphRangesDefault() );
	}
}

} // namespace SystemOptions

/*
========================
PanelFont
========================
*/
static ImFont* PanelFont( const int tier, const panelFont_t face, const float scale )
{
	ImFontAtlas* atlas = ImGui::GetIO().Fonts;
	const int index = 1 + tier * PANEL_FONT_FACES + face;
	if( index >= atlas->Fonts.Size )
	{
		return atlas->Fonts[0];
	}
	ImFont* font = atlas->Fonts[index];
	font->Scale = scale;	// only the panel uses these faces, so its scale can live on the font itself, and the popups take it too
	return font;
}

// this frame's size of the panel's faces ( Draw sets them from the screen's height )
static int		panelFontTier = 0;
static float	panelFontScale = 1.0f;

// this frame's opacities: the adjusted section's, and the rest's ( a tenth of it while a section is adjusted in a game )
static float	panelBaseAlpha = 1.0f;
static float	panelRestAlpha = 1.0f;

// colours: a dark slate panel and one amber accent
static const ImVec4 colorPanel( 0.051f, 0.063f, 0.082f, 0.965f );
static const ImVec4 colorHeader( 0.086f, 0.106f, 0.137f, 1.0f );
static const ImVec4 colorHeaderHover( 0.114f, 0.141f, 0.180f, 1.0f );
static const ImVec4 colorHeaderActive( 0.137f, 0.169f, 0.216f, 1.0f );
static const ImVec4 colorFrame( 0.098f, 0.122f, 0.157f, 1.0f );
static const ImVec4 colorFrameHover( 0.133f, 0.161f, 0.204f, 1.0f );
static const ImVec4 colorFrameActive( 0.157f, 0.192f, 0.243f, 1.0f );
static const ImVec4 colorBorder( 0.180f, 0.212f, 0.259f, 1.0f );
static const ImVec4 colorText( 0.910f, 0.925f, 0.945f, 1.0f );
static const ImVec4 colorTextDim( 0.545f, 0.584f, 0.643f, 1.0f );
static const ImVec4 colorAccent( 0.949f, 0.624f, 0.251f, 1.0f );
static const ImVec4 colorAccentDim( 0.949f, 0.624f, 0.251f, 0.55f );
static const ImVec4 colorAccentFaint( 0.949f, 0.624f, 0.251f, 0.16f );
static const ImVec4 colorTrack( 0.200f, 0.235f, 0.290f, 1.0f );
static const ImVec4 colorWarning( 1.0f, 0.780f, 0.400f, 1.0f );

/*
========================
PushTheme
========================
*/
static int PushTheme( const float s )
{
	ImGui::PushStyleVar( ImGuiStyleVar_WindowRounding, 10.0f * s );
	ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, 1.0f );
	ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 22.0f * s, 18.0f * s ) );
	ImGui::PushStyleVar( ImGuiStyleVar_FrameRounding, 5.0f * s );
	ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 9.0f * s, 5.0f * s ) );
	ImGui::PushStyleVar( ImGuiStyleVar_FrameBorderSize, 0.0f );
	ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 10.0f * s, 6.0f * s ) );
	ImGui::PushStyleVar( ImGuiStyleVar_ItemInnerSpacing, ImVec2( 6.0f * s, 4.0f * s ) );
	ImGui::PushStyleVar( ImGuiStyleVar_PopupRounding, 6.0f * s );
	ImGui::PushStyleVar( ImGuiStyleVar_PopupBorderSize, 1.0f );
	ImGui::PushStyleVar( ImGuiStyleVar_ScrollbarSize, 12.0f * s );
	ImGui::PushStyleVar( ImGuiStyleVar_ScrollbarRounding, 6.0f * s );
	ImGui::PushStyleVar( ImGuiStyleVar_GrabRounding, 4.0f * s );
	ImGui::PushStyleVar( ImGuiStyleVar_GrabMinSize, 14.0f * s );
	ImGui::PushStyleVar( ImGuiStyleVar_ChildRounding, 6.0f * s );
	ImGui::PushStyleVar( ImGuiStyleVar_CellPadding, ImVec2( 10.0f * s, 0.0f ) );
	ImGui::PushStyleVar( ImGuiStyleVar_DisabledAlpha, 0.38f );
	const int numVars = 17;

	ImGui::PushStyleColor( ImGuiCol_WindowBg, colorPanel );
	ImGui::PushStyleColor( ImGuiCol_ChildBg, ImVec4( 0, 0, 0, 0 ) );
	ImGui::PushStyleColor( ImGuiCol_PopupBg, ImVec4( 0.067f, 0.082f, 0.106f, 0.985f ) );
	ImGui::PushStyleColor( ImGuiCol_Border, colorBorder );
	ImGui::PushStyleColor( ImGuiCol_Text, colorText );
	ImGui::PushStyleColor( ImGuiCol_TextDisabled, colorTextDim );
	ImGui::PushStyleColor( ImGuiCol_FrameBg, colorFrame );
	ImGui::PushStyleColor( ImGuiCol_FrameBgHovered, colorFrameHover );
	ImGui::PushStyleColor( ImGuiCol_FrameBgActive, colorFrameActive );
	ImGui::PushStyleColor( ImGuiCol_Header, colorHeader );
	ImGui::PushStyleColor( ImGuiCol_HeaderHovered, colorHeaderHover );
	ImGui::PushStyleColor( ImGuiCol_HeaderActive, colorHeaderActive );
	ImGui::PushStyleColor( ImGuiCol_Button, colorFrame );
	ImGui::PushStyleColor( ImGuiCol_ButtonHovered, colorFrameHover );
	ImGui::PushStyleColor( ImGuiCol_ButtonActive, colorFrameActive );
	ImGui::PushStyleColor( ImGuiCol_CheckMark, colorAccent );
	ImGui::PushStyleColor( ImGuiCol_SliderGrab, colorAccent );
	ImGui::PushStyleColor( ImGuiCol_SliderGrabActive, colorAccent );
	ImGui::PushStyleColor( ImGuiCol_ScrollbarBg, ImVec4( 0, 0, 0, 0 ) );
	ImGui::PushStyleColor( ImGuiCol_ScrollbarGrab, colorTrack );
	ImGui::PushStyleColor( ImGuiCol_ScrollbarGrabHovered, colorFrameActive );
	ImGui::PushStyleColor( ImGuiCol_ScrollbarGrabActive, colorAccentDim );
	ImGui::PushStyleColor( ImGuiCol_Separator, colorBorder );
	ImGui::PushStyleColor( ImGuiCol_NavHighlight, colorAccent );
	ImGui::PushStyleColor( ImGuiCol_TextSelectedBg, colorAccentDim );
	ImGui::PushStyleColor( ImGuiCol_TableBorderLight, ImVec4( 0, 0, 0, 0 ) );
	ImGui::PushStyleColor( ImGuiCol_ModalWindowDimBg, ImVec4( 0, 0, 0, 0.5f ) );
	const int numColors = 27;

	return numVars * 1000 + numColors;
}

/*
========================
PopTheme
========================
*/
static void PopTheme( const int pushed )
{
	ImGui::PopStyleColor( pushed % 1000 );
	ImGui::PopStyleVar( pushed / 1000 );
}

/*
===============================================================================

	Widgets

===============================================================================
*/

/*
========================
OptionSlider

A thin track, an amber fill up to a round knob. ImGui's own SliderBehavior does the dragging, the logarithmic
scale and the rounding to the format's decimals; the value text is in the box beside it, not on the bar.
========================
*/
static bool OptionSlider( const char* strId, const ImGuiDataType dataType, void* value, const void* minValue, const void* maxValue, const char* format, const ImGuiSliderFlags flags, const float width )
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	if( window->SkipItems )
	{
		return false;
	}

	ImGuiContext& g = *GImGui;
	const ImGuiStyle& style = g.Style;
	const ImGuiID id = window->GetID( strId );
	const ImVec2 pos = window->DC.CursorPos;
	const ImRect frameBB( pos, ImVec2( pos.x + width, pos.y + ImGui::GetFrameHeight() ) );

	ImGui::ItemSize( frameBB, style.FramePadding.y );
	if( !ImGui::ItemAdd( frameBB, id, &frameBB, 0 ) )
	{
		return false;
	}

	const bool hovered = ImGui::ItemHoverable( frameBB, id, g.LastItemData.InFlags );
	const bool clicked = hovered && ImGui::IsMouseClicked( 0, id );
	if( clicked || ( g.NavActivateId == id && ( g.NavActivateFlags & ImGuiActivateFlags_PreferInput ) == 0 ) )
	{
		if( clicked )
		{
			ImGui::SetKeyOwner( ImGuiKey_MouseLeft, id );
		}
		ImGui::SetActiveID( id, window );
		ImGui::SetFocusID( id, window );
		ImGui::FocusWindow( window );
		g.ActiveIdUsingNavDirMask |= ( 1 << ImGuiDir_Left ) | ( 1 << ImGuiDir_Right );
	}

	ImRect grabBB;
	const bool changed = ImGui::SliderBehavior( frameBB, id, dataType, value, minValue, maxValue, format, flags | ImGuiSliderFlags_NoInput, &grabBB );
	if( changed )
	{
		ImGui::MarkItemEdited( id );
	}

	// the track and the fill span what the knob's centre spans
	ImDrawList* draw = window->DrawList;
	const float s = ImGui::GetFontSize() / 17.0f;
	const float trackHalf = 2.5f * s;
	const float midY = ( frameBB.Min.y + frameBB.Max.y ) * 0.5f;
	const float knobX = ( grabBB.Min.x + grabBB.Max.x ) * 0.5f;
	const float left = frameBB.Min.x + style.GrabMinSize * 0.5f + 2.0f;
	const float right = frameBB.Max.x - style.GrabMinSize * 0.5f - 2.0f;
	const bool active = g.ActiveId == id;
	const bool disabled = ( g.CurrentItemFlags & ImGuiItemFlags_Disabled ) != 0;

	ImGui::RenderNavHighlight( frameBB, id );
	if( hovered || active )
	{
		draw->AddRectFilled( frameBB.Min, frameBB.Max, ImGui::GetColorU32( colorAccentFaint ), style.FrameRounding );
	}
	draw->AddRectFilled( ImVec2( left, midY - trackHalf ), ImVec2( right, midY + trackHalf ), ImGui::GetColorU32( colorTrack ), trackHalf );
	if( grabBB.Max.x > grabBB.Min.x )
	{
		draw->AddRectFilled( ImVec2( left, midY - trackHalf ), ImVec2( knobX, midY + trackHalf ), ImGui::GetColorU32( disabled ? colorTextDim : colorAccent ), trackHalf );
		const float knobRadius = ( active ? 8.0f : 7.0f ) * s;
		draw->AddCircleFilled( ImVec2( knobX, midY ), knobRadius + 1.5f * s, ImGui::GetColorU32( ImVec4( 0, 0, 0, 0.45f ) ), 20 );
		draw->AddCircleFilled( ImVec2( knobX, midY ), knobRadius, ImGui::GetColorU32( disabled ? colorTextDim : ( active || hovered ? ImVec4( 1, 0.8f, 0.55f, 1 ) : colorAccent ) ), 20 );
	}
	return changed;
}

/*
========================
OptionSwitch

An on-off switch: a pill with a knob that sits right when on. Accept or a click flips it.
========================
*/
static bool OptionSwitch( const char* strId, bool* on )
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	if( window->SkipItems )
	{
		return false;
	}

	const float height = ImGui::GetFrameHeight();
	const float pillHeight = height * 0.72f;
	const float pillWidth = pillHeight * 1.9f;
	const ImVec2 pos = window->DC.CursorPos;

	const bool pressed = ImGui::InvisibleButton( strId, ImVec2( pillWidth + 4.0f, height ) );
	if( pressed )
	{
		*on = !*on;
		ImGui::MarkItemEdited( ImGui::GetItemID() );
	}
	const bool hovered = ImGui::IsItemHovered();
	const bool disabled = ( GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled ) != 0;

	ImDrawList* draw = window->DrawList;
	const ImVec2 pillMin( pos.x + 2.0f, pos.y + ( height - pillHeight ) * 0.5f );
	const ImVec2 pillMax( pillMin.x + pillWidth, pillMin.y + pillHeight );
	const float radius = pillHeight * 0.5f;
	ImVec4 trackColor = *on ? colorAccent : colorTrack;
	if( hovered && !*on && !disabled )
	{
		trackColor = colorFrameActive;
	}
	draw->AddRectFilled( pillMin, pillMax, ImGui::GetColorU32( trackColor ), radius );
	const float knobX = *on ? pillMax.x - radius : pillMin.x + radius;
	draw->AddCircleFilled( ImVec2( knobX, pillMin.y + radius ), radius - 2.5f, ImGui::GetColorU32( *on ? ImVec4( 0.08f, 0.06f, 0.04f, 1 ) : colorText ), 20 );
	return pressed;
}

/*
========================
ResetButton

The round arrow that sets a row back to its default; a gap of the same width where the row is at its default,
so the columns stay straight
========================
*/
static bool ResetButton( const bool show, const char* tip )
{
	const float side = ImGui::GetFrameHeight();
	if( !show )
	{
		ImGui::Dummy( ImVec2( side, side ) );
		return false;
	}
	ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0, 0, 0, 0 ) );
	ImGui::PushStyleColor( ImGuiCol_Text, colorAccent );
	ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );
	const bool pressed = ImGui::Button( "\xE2\x86\xBA##reset", ImVec2( side, side ) );	// U+21BA
	ImGui::PopStyleVar();
	ImGui::PopStyleColor( 2 );
	if( ImGui::IsItemHovered( ImGuiHoveredFlags_DelayShort ) )
	{
		ImGui::SetTooltip( "%s", tip );
	}
	return pressed;
}

/*
========================
TakeHorizontalKeys

A focused value control takes Left and Right: they adjust it, as in the shell's lists, and do not move the focus
========================
*/
static int TakeHorizontalKeys()
{
	if( !ImGui::IsItemFocused() || ImGui::IsItemActive() || ImGui::GetIO().WantTextInput )
	{
		return 0;
	}
	int direction = 0;
	if( ImGui::IsKeyPressed( ImGuiKey_LeftArrow, true ) )
	{
		direction = -1;
	}
	else if( ImGui::IsKeyPressed( ImGuiKey_RightArrow, true ) )
	{
		direction = 1;
	}
	if( direction != 0 )
	{
		ImGui::NavMoveRequestCancel();
	}
	return direction;
}

/*
========================
RowTooltip
========================
*/
static void RowTooltip( const optionRow_t& row, const optionState_t& state, const char* disabledHint )
{
	if( !ImGui::BeginTooltip() )
	{
		return;
	}
	ImGui::PushTextWrapPos( ImGui::GetFontSize() * 26.0f );
	if( row.description[0] != '\0' )
	{
		ImGui::TextUnformatted( row.description );
	}
	if( disabledHint != NULL )
	{
		ImGui::TextColored( colorWarning, "%s", disabledHint );
	}
	if( row.flags & OPTF_RESTART )
	{
		ImGui::TextColored( colorWarning, "Takes effect after a restart of the game." );
	}
	if( row.flags & OPTF_MAP_RELOAD )
	{
		ImGui::TextColored( colorWarning, "Takes effect at the next map load." );
	}
	if( row.special != SPECIAL_DISPLAY_MODE )
	{
		ImGui::PushStyleColor( ImGuiCol_Text, colorTextDim );
		if( state.cvar2 != NULL )
		{
			ImGui::Text( "%s %s, %s %s ( defaults %s, %s )", state.cvar->GetName(), state.cvar->GetString(), state.cvar2->GetName(), state.cvar2->GetString(), state.cvar->GetDefaultString(), state.cvar2->GetDefaultString() );
		}
		else
		{
			ImGui::Text( "%s %s ( default %s )", state.cvar->GetName(), state.cvar->GetString(), state.cvar->GetDefaultString() );
		}
		ImGui::PopStyleColor();
	}
	ImGui::PopTextWrapPos();
	ImGui::EndTooltip();
}

/*
========================
ChoiceLabel
========================
*/
static const char* ChoiceLabel( const optionRow_t& row, const int value )
{
	for( int i = 0; i < row.numChoices; i++ )
	{
		if( row.choices[i].value == value )
		{
			return row.choices[i].label;
		}
	}
	return PanelText( "Custom ( %d )", value );
}

/*
========================
RowValueText

The value as the section's header shows it
========================
*/
static const char* RowValueText( const optionRow_t& row, const optionState_t& state )
{
	switch( row.kind )
	{
		case OPTION_FLOAT:
			return PanelText( row.format, RowGetFloat( row, state ) );
		case OPTION_INT:
			return PanelText( row.format, state.cvar->GetInteger() );
		case OPTION_TOGGLE:
			return state.cvar->GetBool() ? "On" : "Off";
		case OPTION_CHOICE:
			return ChoiceLabel( row, state.cvar->GetInteger() );
		default:
			break;
	}
	if( row.special == SPECIAL_DISPLAY_MODE )
	{
		const int current = CurrentDisplayMode();
		return current >= 0 ? panel.displayModes[current].label.c_str() : "Custom";
	}
	if( row.special == SPECIAL_RENDER_API )
	{
		return idStr::Icmp( state.cvar->GetString(), "vulkan" ) == 0 ? "Vulkan" : "DirectX 12";
	}
	return SpecialFormat( row, SpecialGet( row, state ) );
}

/*
========================
ValueBox

The box beside a slider: shows the value, and takes a typed one, which counts when the box is left ( Enter, Tab,
a click elsewhere ); Esc in the box gives the old text back
========================
*/
static bool ValueBox( optionState_t& state, const char* shown, const float width, float& typed )
{
	ImGuiInputTextFlags flags = ImGuiInputTextFlags_CharsScientific | ImGuiInputTextFlags_AutoSelectAll;
	ImGui::SetNextItemWidth( width );
	const ImGuiID id = ImGui::GetID( "##value" );
	if( ImGui::GetActiveID() != id )
	{
		idStr::Copynz( state.editText, shown, sizeof( state.editText ) );
	}
	ImGui::InputText( "##value", state.editText, sizeof( state.editText ), flags );
	if( ImGui::IsItemDeactivatedAfterEdit() )
	{
		const char* text = state.editText;
		while( *text == ' ' )
		{
			text++;
		}
		if( *text == '\0' )
		{
			return false;
		}
		typed = atof( text );
		return true;
	}
	return false;
}

/*
===============================================================================

	Drawing

===============================================================================
*/

/*
========================
RowMatchesFilter
========================
*/
static bool RowMatchesFilter( const optionSection_t& section, const optionRow_t& row )
{
	if( panel.filter[0] == '\0' )
	{
		return true;
	}
	if( idStr::FindText( section.name, panel.filter, false ) >= 0 || idStr::FindText( row.label, panel.filter, false ) >= 0 )
	{
		return true;
	}
	return row.cvarName != NULL && idStr::FindText( row.cvarName, panel.filter, false ) >= 0;
}

/*
========================
SameLineAt

The next item on this line at a screen x: SameLine's own offset counts from the cell's start less the indent the
cell began with, which differs between the two columns
========================
*/
static void SameLineAt( const float screenX )
{
	ImGui::SameLine();
	ImGui::SetCursorScreenPos( ImVec2( screenX, ImGui::GetCursorScreenPos().y ) );
}

/*
========================
DrawRow
========================
*/
static void DrawRow( const int sectionIndex, const int rowIndex )
{
	const optionRow_t& row = rows[rowIndex];
	optionState_t& state = states[rowIndex];
	if( !RowShown( row, state ) )
	{
		return;
	}

	const char* disabledHint;
	const bool enabled = RowEnabled( row, state.cvar, &disabledHint );
	const ImGuiStyle& style = ImGui::GetStyle();
	const float fontSize = ImGui::GetFontSize();
	const float rowWidth = ImGui::GetContentRegionAvail().x;
	const float frameHeight = ImGui::GetFrameHeight();
	const float labelWidth = Max( fontSize * 8.0f, rowWidth * 0.40f );
	const float resetWidth = frameHeight;
	const float controlWidth = Max( fontSize * 4.0f, rowWidth - labelWidth - resetWidth - style.ItemSpacing.x * 2.0f );
	const float boxWidth = fontSize * 4.4f;
	const float rowX = ImGui::GetCursorScreenPos().x;
	const float controlX = rowX + labelWidth;
	const float resetX = controlX + controlWidth + style.ItemSpacing.x;
	const bool isDefault = RowIsDefault( row, state );

	ImGui::PushID( rowIndex );
	if( !enabled )
	{
		ImGui::BeginDisabled();
	}

	// the label, cut with an ellipsis where a narrow screen leaves it too little room ( clipped glyph by glyph: a
	// clip rectangle would start a draw command a row ), with a mark left of it when the row is off its default
	const ImVec2 rowPos = ImGui::GetCursorScreenPos();
	const float labelRight = controlX - style.ItemSpacing.x;
	const ImVec2 labelSize = ImGui::CalcTextSize( row.label );
	ImGui::Dummy( ImVec2( labelRight - rowPos.x, frameHeight ) );
	bool hovered = ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled );
	const ImVec2 textPos( rowPos.x, rowPos.y + style.FramePadding.y );
	ImGui::RenderTextEllipsis( ImGui::GetWindowDrawList(), textPos, ImVec2( labelRight, textPos.y + labelSize.y ), labelRight, labelRight, row.label, NULL, &labelSize );
	if( !isDefault )
	{
		ImGui::GetWindowDrawList()->AddCircleFilled( ImVec2( rowPos.x - fontSize * 0.6f, textPos.y + fontSize * 0.58f ), fontSize * 0.15f, ImGui::GetColorU32( colorAccent ), 12 );
	}
	SameLineAt( controlX );

	bool changed = false;
	bool interacted = false;	// a control was taken hold of: a press on a bar, a box, a switch, a list opened
	bool rowFocused = false;
	int direction = 0;
	const bool isSlider = row.kind == OPTION_FLOAT || row.kind == OPTION_INT ||
						  ( row.kind == OPTION_SPECIAL && row.special != SPECIAL_DISPLAY_MODE && row.special != SPECIAL_RENDER_API );

	if( isSlider )
	{
		const float sliderWidth = Max( fontSize * 2.0f, controlWidth - boxWidth - style.ItemSpacing.x );
		const ImGuiSliderFlags flags = ( row.flags & OPTF_LOG ) ? ImGuiSliderFlags_Logarithmic : 0;
		if( row.kind == OPTION_INT )
		{
			int value = state.cvar->GetInteger();
			int minValue = idMath::Ftoi( row.minValue );
			int maxValue = idMath::Ftoi( row.maxValue );
			if( OptionSlider( "##slider", ImGuiDataType_S32, &value, &minValue, &maxValue, row.format, flags, sliderWidth ) )
			{
				RowSetInteger( row, state, value );
				changed = true;
			}
		}
		else if( row.kind == OPTION_FLOAT )
		{
			float value = RowGetFloat( row, state );
			if( OptionSlider( "##slider", ImGuiDataType_Float, &value, &row.minValue, &row.maxValue, row.format, flags, sliderWidth ) )
			{
				RowSetFloat( row, state, value );
				changed = true;
			}
		}
		else
		{
			float value = SpecialGet( row, state );
			if( OptionSlider( "##slider", ImGuiDataType_Float, &value, &row.minValue, &row.maxValue, "%.0f", 0, sliderWidth ) )
			{
				SpecialSet( row, state, value );
				changed = true;
			}
		}
		hovered |= ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled );
		interacted |= ImGui::IsItemActivated();
		rowFocused = ImGui::IsItemFocused();
		direction = TakeHorizontalKeys();
		const bool typeRequest = rowFocused && !ImGui::IsItemActive() && ( ImGui::IsKeyPressed( ImGuiKey_Enter, false ) || ImGui::IsKeyPressed( ImGuiKey_KeypadEnter, false ) );

		// the box: the value as text, and a typed value, which may leave the bar's range ( the cvar keeps to its
		// own bounds )
		SameLineAt( controlX + sliderWidth + style.ItemSpacing.x );
		const char* shown;
		if( row.kind == OPTION_INT )
		{
			shown = PanelText( "%d", state.cvar->GetInteger() );
		}
		else if( row.kind == OPTION_FLOAT )
		{
			shown = PanelText( row.format, RowGetFloat( row, state ) );
		}
		else
		{
			shown = SpecialFormat( row, SpecialGet( row, state ) );
		}
		float typed;
		if( typeRequest )
		{
			ImGui::SetKeyboardFocusHere();		// Enter on the bar: type into its box
		}
		const bool boxChanged = ValueBox( state, shown, boxWidth, typed );
		interacted |= ImGui::IsItemActivated();
		if( boxChanged )
		{
			if( row.kind == OPTION_INT )
			{
				RowSetInteger( row, state, idMath::Ftoi( idMath::Rint( typed ) ) );
			}
			else if( row.kind == OPTION_FLOAT )
			{
				RowSetFloat( row, state, typed );
			}
			else
			{
				SpecialSet( row, state, typed );
			}
			changed = true;
		}
		hovered |= ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled );
		if( ImGui::IsItemFocused() )
		{
			panel.focusedSection = sectionIndex;
			panel.viewFocusSeen |= ( sectionIndex == panel.viewSection );
		}
	}
	else if( row.kind == OPTION_TOGGLE )
	{
		bool on = state.cvar->GetBool();
		if( OptionSwitch( "##switch", &on ) )
		{
			state.cvar->SetBool( on );
			changed = true;
		}
		hovered |= ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled );
		interacted |= ImGui::IsItemActivated();
		rowFocused = ImGui::IsItemFocused();
		direction = TakeHorizontalKeys();
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ImGui::TextColored( state.cvar->GetBool() ? colorText : colorTextDim, "%s", state.cvar->GetBool() ? "On" : "Off" );
	}
	else if( row.kind == OPTION_CHOICE )
	{
		const int value = state.cvar->GetInteger();
		ImGui::SetNextItemWidth( controlWidth );
		const bool listOpen = ImGui::BeginCombo( "##choice", ChoiceLabel( row, value ), ImGuiComboFlags_HeightLarge );
		// the combo's own button is the last item until its list is drawn
		hovered |= ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled );
		interacted |= listOpen;
		rowFocused = ImGui::IsItemFocused();
		direction = TakeHorizontalKeys();
		if( listOpen )
		{
			for( int i = 0; i < row.numChoices; i++ )
			{
				const bool selected = row.choices[i].value == value;
				if( ImGui::Selectable( row.choices[i].label, selected ) && !selected )
				{
					state.cvar->SetInteger( row.choices[i].value );
					changed = true;
				}
				if( selected )
				{
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}
	}
	else
	{
		// the display mode and the render API: dropdowns that apply on Accept only, since a step through the display
		// modes would restart the renderer at every press
		ImGui::SetNextItemWidth( controlWidth );
		const bool listOpen = ImGui::BeginCombo( "##choice", RowValueText( row, state ), ImGuiComboFlags_HeightLarge );
		hovered |= ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled );
		rowFocused = ImGui::IsItemFocused();
		TakeHorizontalKeys();
		if( listOpen )
		{
			if( row.special == SPECIAL_DISPLAY_MODE )
			{
				const int current = CurrentDisplayMode();
				for( int i = 0; i < panel.displayModes.Num(); i++ )
				{
					if( ImGui::Selectable( panel.displayModes[i].label.c_str(), i == current ) && i != current )
					{
						ApplyDisplayMode( i );
					}
					if( i == current )
					{
						ImGui::SetItemDefaultFocus();
					}
				}
			}
			else
			{
				static const char* apis[2][2] = { { "dx12", "DirectX 12" }, { "vulkan", "Vulkan" } };
				for( int i = 0; i < 2; i++ )
				{
					const bool selected = idStr::Icmp( state.cvar->GetString(), apis[i][0] ) == 0;
					if( ImGui::Selectable( apis[i][1], selected ) && !selected )
					{
						state.cvar->SetString( apis[i][0] );
						changed = true;
					}
				}
			}
			ImGui::EndCombo();
		}
	}

	if( direction != 0 )
	{
		changed |= RowStep( row, state, direction );
	}
	if( rowFocused )
	{
		panel.focusedSection = sectionIndex;
		panel.viewFocusSeen |= ( sectionIndex == panel.viewSection );
		if( panel.resetFocused )
		{
			panel.resetFocused = false;
			RowSetDefault( row, state );
			changed = false;	// a default needs no line in the config
		}
	}

	// the default as the row shows its values
	const char* defaultText = NULL;
	const float defaultValue = atof( state.cvar->GetDefaultString() );
	switch( row.kind )
	{
		case OPTION_FLOAT:
			defaultText = PanelText( row.format, ( row.flags & OPTF_INVERT ) ? 1.0f - defaultValue : defaultValue );
			break;
		case OPTION_INT:
			defaultText = PanelText( row.format, idMath::Ftoi( defaultValue ) );
			break;
		case OPTION_TOGGLE:
			defaultText = ( defaultValue != 0.0f ) ? "On" : "Off";
			break;
		case OPTION_CHOICE:
			defaultText = ChoiceLabel( row, idMath::Ftoi( defaultValue ) );
			break;
		default:
			break;
	}

	SameLineAt( resetX );
	ImGui::PushItemFlag( ImGuiItemFlags_NoNav, true );		// the keyboard resets with Delete, the pad with X
	if( ResetButton( !isDefault && ( row.flags & OPTF_NO_REVERT ) == 0, defaultText != NULL ? va( "Back to the default: %s", defaultText ) : "Back to the default" ) )
	{
		RowSetDefault( row, state );
		changed = false;
	}
	ImGui::PopItemFlag();

	if( changed )
	{
		RowNoteSet( row, state );
		RowChanged();
	}

	// in a game, the first touch of a setting fades the rest of the panel, so that its effect shows on the game
	if( ( changed || interacted ) && game != NULL && game->IsInGame() && row.special != SPECIAL_DISPLAY_MODE && ( row.flags & ( OPTF_RESTART | OPTF_NO_VIEW ) ) == 0 )
	{
		panel.viewSection = sectionIndex;
		panel.viewSectionDrawn = true;		// it is being drawn, and has the focus: the end of the frame must not take
		panel.viewFocusSeen = true;			// the view back
	}

	if( !enabled )
	{
		ImGui::EndDisabled();
	}
	if( hovered && !ImGui::IsPopupOpen( "", ImGuiPopupFlags_AnyPopupId ) )
	{
		RowTooltip( row, state, disabledHint );
	}
	ImGui::PopID();
}

/*
========================
SectionSummary
========================
*/
static const char* SectionSummary( const optionSection_t& section )
{
	static char summary[256];
	summary[0] = '\0';
	for( int i = section.firstRow; i < section.firstRow + section.numRows; i++ )
	{
		const optionRow_t& row = rows[i];
		const optionState_t& state = states[i];
		if( !( row.flags & OPTF_SUMMARY ) || state.cvar == NULL || ( row.cvarName2 != NULL && state.cvar2 == NULL ) )
		{
			continue;
		}
		if( summary[0] != '\0' )
		{
			idStr::Append( summary, sizeof( summary ), "  \xC2\xB7  " );		// U+00B7
		}
		if( row.summaryLabel != NULL && row.summaryLabel[0] != '\0' )
		{
			idStr::Append( summary, sizeof( summary ), va( "%s ", row.summaryLabel ) );
		}
		idStr::Append( summary, sizeof( summary ), RowValueText( row, state ) );
	}
	return summary;
}

/*
========================
DrawSection
========================
*/
static void DrawSection( const int sectionIndex )
{
	const optionSection_t& section = sections[sectionIndex];

	int matching = 0;
	int notDefault = 0;
	for( int i = section.firstRow; i < section.firstRow + section.numRows; i++ )
	{
		if( states[i].cvar == NULL )
		{
			continue;
		}
		if( RowShown( rows[i], states[i] ) && RowMatchesFilter( section, rows[i] ) )
		{
			matching++;
		}
		if( !RowIsDefault( rows[i], states[i] ) )
		{
			notDefault++;
		}
	}
	panel.sectionDrawn[sectionIndex] = matching > 0;
	if( matching == 0 )
	{
		return;
	}

	ImGui::PushID( sectionIndex );
	const float s = ImGui::GetFontSize() / 17.0f;
	const bool filtering = panel.filter[0] != '\0';
	const bool viewing = ( panel.viewSection == sectionIndex );

	// while a section is adjusted in a game, the rest of the panel fades to a tenth so that the game shows; this
	// section stays whole, on a dark plate as tall as it was last frame
	ImGui::PushStyleVar( ImGuiStyleVar_Alpha, viewing ? panelBaseAlpha : panelRestAlpha );
	const ImVec2 sectionTop = ImGui::GetCursorScreenPos();
	if( viewing )
	{
		panel.viewSectionDrawn = true;
		const float width = ImGui::GetContentRegionAvail().x;
		const ImVec4 plate( colorPanel.x, colorPanel.y, colorPanel.z, 0.88f * panel.viewFade );
		ImGui::GetWindowDrawList()->AddRectFilled( ImVec2( sectionTop.x - 4.0f * s, sectionTop.y - 4.0f * s ), ImVec2( sectionTop.x + width + 4.0f * s, sectionTop.y + panel.viewPlateHeight + 4.0f * s ), ImGui::GetColorU32( plate ), 8.0f * s );
	}

	// the header: the name in the strong face, the section's main values right-aligned beside it. One section is
	// open at a time ( a search opens every section that has a match )
	const bool wasOpen = filtering || panel.openSection == sectionIndex;
	ImGui::SetNextItemOpen( wasOpen, ImGuiCond_Always );
	if( panel.focusRequest == sectionIndex )
	{
		ImGui::SetKeyboardFocusHere();
		ImGui::SetScrollHereY( 0.0f );
		panel.focusRequest = -1;
	}
	ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 12.0f * s, 9.0f * s ) );
	ImGui::PushFont( PanelFont( panelFontTier, PANEL_FONT_STRONG, panelFontScale ) );
	const bool open = ImGui::CollapsingHeader( section.name, ImGuiTreeNodeFlags_SpanAvailWidth );
	ImGui::PopFont();
	ImGui::PopStyleVar();
	if( !filtering && open != wasOpen )
	{
		if( open )
		{
			panel.openSection = sectionIndex;		// the one open before closes next frame
			panel.scrollToSection = sectionIndex;
			panel.scrollFrame = ImGui::GetFrameCount() + 1;
		}
		else
		{
			panel.openSection = -1;
		}
		panel.viewSection = -1;			// a section closed or another opened: the whole panel again
	}
	if( ImGui::IsItemFocused() )
	{
		panel.focusedSection = sectionIndex;
		panel.viewFocusSeen |= viewing;
	}

	const ImVec2 headerMin = ImGui::GetItemRectMin();
	const ImVec2 headerMax = ImGui::GetItemRectMax();
	ImDrawList* draw = ImGui::GetWindowDrawList();
	const char* summary = SectionSummary( section );
	const float summaryRight = headerMax.x - 12.0f * s;
	const ImVec2 summarySize = ImGui::CalcTextSize( summary );
	const float nameRight = headerMin.x + ImGui::GetFontSize() * 2.2f + ImGui::CalcTextSize( section.name ).x * 1.05f;
	float textX = summaryRight - summarySize.x;
	if( textX > nameRight )
	{
		draw->AddText( ImVec2( textX, ( headerMin.y + headerMax.y - summarySize.y ) * 0.5f ), ImGui::GetColorU32( colorTextDim ), summary );
	}
	else
	{
		textX = summaryRight;
	}
	if( notDefault > 0 )
	{
		// an amber bar down the header's left edge: this system has settings off their defaults
		draw->AddRectFilled( ImVec2( headerMin.x, headerMin.y + 3.0f * s ), ImVec2( headerMin.x + 3.0f * s, headerMax.y - 3.0f * s ), ImGui::GetColorU32( colorAccent ), 1.5f * s );
	}

	if( open )
	{
		ImGui::Dummy( ImVec2( 0, 2.0f * s ) );
		ImGui::Indent( 14.0f * s );
		for( int i = section.firstRow; i < section.firstRow + section.numRows; i++ )
		{
			if( RowMatchesFilter( section, rows[i] ) )
			{
				DrawRow( sectionIndex, i );
			}
		}
		if( viewing )
		{
			ImGui::PushStyleColor( ImGuiCol_Text, colorTextDim );
			ImGui::TextUnformatted( panel.padInUse ? "B: the whole panel again" : "Esc: the whole panel again" );
			ImGui::PopStyleColor();
		}
		if( notDefault > 0 && !filtering )
		{
			ImGui::PushItemFlag( ImGuiItemFlags_NoNav, true );
			ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0, 0, 0, 0 ) );
			ImGui::PushStyleColor( ImGuiCol_Text, colorTextDim );
			if( ImGui::SmallButton( va( "\xE2\x86\xBA  Reset %s to defaults", section.name ) ) )
			{
				panel.defaultsRequested = true;
				panel.defaultsSection = sectionIndex;
			}
			ImGui::PopStyleColor( 2 );
			ImGui::PopItemFlag();
		}
		ImGui::Unindent( 14.0f * s );
		ImGui::Dummy( ImVec2( 0, 6.0f * s ) );
	}
	else
	{
		ImGui::Dummy( ImVec2( 0, 1.0f * s ) );
	}

	const float sectionBottom = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y;
	if( viewing )
	{
		panel.viewPlateHeight = sectionBottom - sectionTop.y;
	}
	if( open && panel.scrollToSection == sectionIndex && ImGui::GetFrameCount() > panel.scrollFrame )
	{
		// a section just opened scrolls into view, its header first where all of it does not fit
		ImGui::ScrollToRect( ImGui::GetCurrentWindow(), ImRect( headerMin, ImVec2( headerMax.x, sectionBottom ) ), ImGuiScrollFlags_KeepVisibleEdgeY );
		panel.scrollToSection = -1;
	}
	ImGui::PopStyleVar();
	ImGui::PopID();
}

/*
========================
AnyRowChanged
========================
*/
static bool AnyRowChanged( const int flags )
{
	for( int i = 0; i < rows.Num(); i++ )
	{
		if( ( rows[i].flags & OPTF_NO_REVERT ) == 0 && ( flags == 0 || ( rows[i].flags & flags ) != 0 ) && RowIsChanged( states[i] ) )
		{
			return true;
		}
	}
	return false;
}

/*
========================
RestartNotice

The labels of the changed rows that wait for a restart, or NULL
========================
*/
static const char* RestartNotice()
{
	static char notice[256];
	notice[0] = '\0';
	for( int i = 0; i < rows.Num(); i++ )
	{
		if( ( rows[i].flags & ( OPTF_RESTART | OPTF_MAP_RELOAD ) ) == 0 || !RowIsChanged( states[i] ) )
		{
			continue;
		}
		if( notice[0] != '\0' )
		{
			idStr::Append( notice, sizeof( notice ), ", " );
		}
		idStr::Append( notice, sizeof( notice ), rows[i].label );
	}
	return notice[0] != '\0' ? notice : NULL;
}

/*
========================
DrawHeader
========================
*/
static void DrawHeader( const float s )
{
	const ImGuiStyle& style = ImGui::GetStyle();
	const float width = ImGui::GetContentRegionAvail().x;
	const ImVec2 start = ImGui::GetCursorPos();

	ImGui::PushFont( PanelFont( panelFontTier, PANEL_FONT_TITLE, panelFontScale ) );
	ImGui::TextUnformatted( "SYSTEM OPTIONS" );
	const ImVec2 titleMin = ImGui::GetItemRectMin();
	const ImVec2 titleMax = ImGui::GetItemRectMax();
	const float titleHeight = titleMax.y - titleMin.y;
	ImGui::PopFont();
	ImGui::GetWindowDrawList()->AddRectFilled( ImVec2( titleMin.x, titleMax.y + 2.0f * s ), ImVec2( titleMin.x + 46.0f * s, titleMax.y + 5.0f * s ), ImGui::GetColorU32( colorAccent ), 1.5f * s );

	// the search box, right, on the title's line
	const float searchWidth = Min( width * 0.32f, ImGui::GetFontSize() * 20.0f );
	ImGui::SetCursorPos( ImVec2( start.x + width - searchWidth, start.y + ( titleHeight - ImGui::GetFrameHeight() ) * 0.5f ) );
	ImGui::SetNextItemWidth( searchWidth );
	ImGui::InputTextWithHint( "##filter", "Search settings\xE2\x80\xA6", panel.filter, sizeof( panel.filter ) );	// U+2026
	if( ImGui::IsItemHovered( ImGuiHoveredFlags_DelayNormal ) )
	{
		ImGui::SetTooltip( "Shows the settings whose name, section or cvar holds this text" );
	}

	// the Advanced switch, left of the search box
	const char* advancedLabel = "Advanced";
	const float switchWidth = ImGui::GetFrameHeight() * 0.72f * 1.9f + 4.0f;
	const float advancedWidth = switchWidth + style.ItemInnerSpacing.x + ImGui::CalcTextSize( advancedLabel ).x;
	ImGui::SetCursorPos( ImVec2( start.x + width - searchWidth - 24.0f * s - advancedWidth, start.y + ( titleHeight - ImGui::GetFrameHeight() ) * 0.5f ) );
	bool advanced = ui_systemOptionsAdvanced.GetBool();
	if( OptionSwitch( "##advanced", &advanced ) )
	{
		ui_systemOptionsAdvanced.SetBool( advanced );
	}
	const bool advancedHovered = ImGui::IsItemHovered( ImGuiHoveredFlags_DelayShort );
	ImGui::SameLine( 0.0f, style.ItemInnerSpacing.x );
	ImGui::AlignTextToFramePadding();
	ImGui::TextColored( advanced ? colorText : colorTextDim, "%s", advancedLabel );
	if( advancedHovered || ImGui::IsItemHovered( ImGuiHoveredFlags_DelayShort ) )
	{
		ImGui::SetTooltip( "Shows the finer settings too: TAA's clamp and history, the bloom's threshold softness, shadow cascades, fog grid sizes" );
	}

	ImGui::SetCursorPos( ImVec2( start.x, start.y + titleHeight + 10.0f * s ) );
	ImGui::PushStyleColor( ImGuiCol_Text, colorTextDim );
	ImGui::TextUnformatted( "Display, graphics and sound. Changes show at once and are saved when you close the panel." );
	ImGui::PopStyleColor();
	ImGui::Dummy( ImVec2( 0, 4.0f * s ) );
	ImGui::Separator();
	ImGui::Dummy( ImVec2( 0, style.ItemSpacing.y ) );
}

/*
========================
FooterButton
========================
*/
static float FooterButtonWidth( const char* label )
{
	return ImGui::CalcTextSize( label, NULL, true ).x + ImGui::GetStyle().FramePadding.x * 4.0f;
}

/*
========================
DrawFooter
========================
*/
static void DrawFooter( const float s )
{
	const ImGuiStyle& style = ImGui::GetStyle();
	ImGui::Separator();
	ImGui::Dummy( ImVec2( 0, 2.0f * s ) );

	const char* hints = panel.padInUse ?
						"A  select   \xC2\xB7   \xE2\x86\x90 \xE2\x86\x92  adjust   \xC2\xB7   X  default   \xC2\xB7   Y  hold to preview   \xC2\xB7   LB RB  sections   \xC2\xB7   B  close" :
						"\xE2\x86\x91\xE2\x86\x93  move   \xC2\xB7   \xE2\x86\x90\xE2\x86\x92  adjust   \xC2\xB7   Enter  type a value   \xC2\xB7   Del  default   \xC2\xB7   PgUp PgDn  sections   \xC2\xB7   Esc  close";
	const char* notice = RestartNotice();

	const char* labels[4] = { "Hold to Preview", "Defaults", "Revert", "Close" };
	float buttonsWidth = 0.0f;
	for( int i = 0; i < 4; i++ )
	{
		buttonsWidth += FooterButtonWidth( labels[i] ) + style.ItemSpacing.x;
	}
	const float width = ImGui::GetContentRegionAvail().x;
	const float lineY = ImGui::GetCursorPosY();
	const float buttonHeight = ImGui::GetFrameHeight() + 6.0f * s;

	// left: the restart notice, else the keys
	ImGui::SetCursorPosY( lineY + ( buttonHeight - ImGui::GetTextLineHeight() ) * 0.5f );
	ImGui::PushTextWrapPos( ImGui::GetWindowContentRegionMax().x - buttonsWidth - 20.0f * s );
	if( notice != NULL )
	{
		ImGui::TextColored( colorWarning, "\xE2\x9A\xA0  Waits for a restart: %s", notice );	// U+26A0
	}
	else
	{
		ImGui::TextColored( colorTextDim, "%s", hints );
	}
	ImGui::PopTextWrapPos();

	ImGui::SetCursorPos( ImVec2( ImGui::GetWindowContentRegionMax().x - buttonsWidth + style.ItemSpacing.x, lineY ) );

	ImGui::Button( labels[0], ImVec2( FooterButtonWidth( labels[0] ), buttonHeight ) );
	panel.previewButtonHeld = ImGui::IsItemActive();
	if( ImGui::IsItemHovered( ImGuiHoveredFlags_DelayShort ) && !panel.previewButtonHeld )
	{
		ImGui::SetTooltip( "Hides the panel while held, to see the picture" );
	}
	ImGui::SameLine();

	if( ImGui::Button( labels[1], ImVec2( FooterButtonWidth( labels[1] ), buttonHeight ) ) )
	{
		panel.defaultsRequested = true;
		panel.defaultsSection = -1;
	}
	if( ImGui::IsItemHovered( ImGuiHoveredFlags_DelayShort ) )
	{
		ImGui::SetTooltip( "Every setting here back to its default ( the display mode stays ); Revert undoes it" );
	}
	ImGui::SameLine();

	const bool anyChanged = AnyRowChanged( 0 );
	ImGui::BeginDisabled( !anyChanged );
	if( ImGui::Button( labels[2], ImVec2( FooterButtonWidth( labels[2] ), buttonHeight ) ) )
	{
		panel.revertRequested = true;
	}
	ImGui::EndDisabled();
	if( ImGui::IsItemHovered( ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled ) )
	{
		ImGui::SetTooltip( "Every setting back to what it was when the panel opened" );
	}
	ImGui::SameLine();

	ImGui::PushStyleColor( ImGuiCol_Button, colorAccent );
	ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 1.0f, 0.72f, 0.38f, 1.0f ) );
	ImGui::PushStyleColor( ImGuiCol_ButtonActive, ImVec4( 0.85f, 0.52f, 0.18f, 1.0f ) );
	ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 0.08f, 0.06f, 0.04f, 1.0f ) );
	if( ImGui::Button( labels[3], ImVec2( FooterButtonWidth( labels[3] ), buttonHeight ) ) )
	{
		panel.closeRequested = true;
	}
	ImGui::PopStyleColor( 4 );
}

/*
========================
JumpSection

LB and RB, PgUp and PgDn: the focus to the previous or next section's header, in reading order ( down the left
column, then down the right )
========================
*/
static void JumpSection( const int direction )
{
	idStaticList<int, MAX_SECTIONS> order;
	for( int column = 0; column < 2; column++ )
	{
		for( int i = 0; i < sections.Num(); i++ )
		{
			if( sections[i].column == column && panel.sectionDrawn[i] )
			{
				order.Append( i );
			}
		}
	}
	if( order.Num() == 0 )
	{
		return;
	}
	int at = order.FindIndex( panel.focusedSection );
	if( at < 0 )
	{
		at = ( direction > 0 ) ? -1 : order.Num();
	}
	panel.focusRequest = order[idMath::ClampInt( 0, order.Num() - 1, at + direction )];
}

/*
========================
Revert
========================
*/
static void Revert()
{
	for( int i = 0; i < rows.Num(); i++ )
	{
		optionState_t& state = states[i];
		if( state.cvar == NULL || ( rows[i].flags & OPTF_NO_REVERT ) != 0 || RowLocked( state.cvar ) )
		{
			continue;
		}
		state.cvar->SetString( state.openValue );
		if( state.cvar2 != NULL )
		{
			state.cvar2->SetString( state.openValue2 );
		}
		state.saved = state.openSaved;
		state.savedValue = state.openSavedValue;
	}
	RowChanged();
}

/*
========================
SetDefaults
========================
*/
static void SetDefaults( const int sectionIndex )
{
	for( int s = 0; s < sections.Num(); s++ )
	{
		if( sectionIndex >= 0 && s != sectionIndex )
		{
			continue;
		}
		for( int i = sections[s].firstRow; i < sections[s].firstRow + sections[s].numRows; i++ )
		{
			RowSetDefault( rows[i], states[i] );
		}
	}
}

/*
========================
ResolveCVars

The table names its cvars; a name this build does not have hides its row
========================
*/
static void ResolveCVars()
{
	BuildTable();
	static bool reported = false;
	for( int i = 0; i < rows.Num(); i++ )
	{
		states[i].cvar = cvarSystem->Find( rows[i].cvarName );
		states[i].cvar2 = ( rows[i].cvarName2 != NULL ) ? cvarSystem->Find( rows[i].cvarName2 ) : NULL;
		if( !reported && ( states[i].cvar == NULL || ( rows[i].cvarName2 != NULL && states[i].cvar2 == NULL ) ) )
		{
			common->Warning( "System Options: no cvar %s, its row \"%s\" is hidden", states[i].cvar == NULL ? rows[i].cvarName : rows[i].cvarName2, rows[i].label );
		}
	}
	reported = true;
}

/*
========================
FindRow
========================
*/
static int FindRow( const char* cvarName )
{
	BuildTable();
	for( int i = 0; i < rows.Num(); i++ )
	{
		if( rows[i].kind != OPTION_SPECIAL && idStr::Icmp( rows[i].cvarName, cvarName ) == 0 )
		{
			return i;
		}
	}
	return -1;
}

/*
========================
InjectKey

A key as another key to ImGui: a pad's buttons and directions become the keys ImGui's keyboard navigation knows
========================
*/
static void InjectKey( const sysEvent_t& event, const int key )
{
	sysEvent_t translated = event;
	translated.evValue = key;
	ImGuiHook::InjectSysEvent( &translated );
}

/*
========================
HandleKey
========================
*/
static void HandleKey( const sysEvent_t& event )
{
	const int key = event.evValue;
	const bool down = event.evValue2 != 0;
	const bool typing = ImGui::GetIO().WantTextInput;

	if( down )
	{
		panel.padInUse = ( key >= K_JOY1 && key < K_MOUSE1 );
	}

	// Back: one level a press, as everywhere in the menus: an open dropdown or a value being typed first, then
	// the panel itself. Start always closes it. Backspace is Back only when no text is being typed
	if( key == K_ESCAPE || key == K_JOY2 || key == K_JOY9 || ( key == K_BACKSPACE && !typing ) )
	{
		if( down )
		{
			const bool inner = typing || ImGui::IsPopupOpen( "", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel ) || ImGui::IsAnyItemActive();
			if( inner && key != K_JOY9 )
			{
				InjectKey( event, K_ESCAPE );
			}
			else if( panel.viewSection >= 0 && key != K_JOY9 )
			{
				// out of the view: the whole panel comes back, the section stays open and the focus where it was
				panel.viewSection = -1;
			}
			else
			{
				panel.swallowRelease = key;
				SystemOptions::Close();
			}
		}
		else
		{
			InjectKey( event, K_ESCAPE );
		}
		return;
	}

	if( typing && key < K_JOY1 )
	{
		// a text box has every key: Backspace, the arrows, Enter
		ImGuiHook::InjectSysEvent( &event );
		return;
	}

	switch( key )
	{
		case K_JOY_DPAD_UP:
		case K_JOY_STICK1_UP:
			InjectKey( event, K_UPARROW );
			return;
		case K_JOY_DPAD_DOWN:
		case K_JOY_STICK1_DOWN:
			InjectKey( event, K_DOWNARROW );
			return;
		case K_JOY_DPAD_LEFT:
		case K_JOY_STICK1_LEFT:
			InjectKey( event, K_LEFTARROW );
			return;
		case K_JOY_DPAD_RIGHT:
		case K_JOY_STICK1_RIGHT:
			InjectKey( event, K_RIGHTARROW );
			return;
		case K_JOY1:
			InjectKey( event, K_SPACE );		// accept: ImGui's activate
			return;
		case K_JOY3:
		case K_DEL:
			if( down )
			{
				panel.resetFocused = true;
			}
			return;
		case K_JOY4:
			panel.previewHeld = down;
			return;
		case K_JOY5:
		case K_PGUP:
			if( down )
			{
				JumpSection( -1 );
			}
			return;
		case K_JOY6:
		case K_PGDN:
			if( down )
			{
				JumpSection( 1 );
			}
			return;
		case K_MWHEELUP:
			if( down )
			{
				ImGuiHook::InjectMouseWheel( 1 );
			}
			return;
		case K_MWHEELDOWN:
			if( down )
			{
				ImGuiHook::InjectMouseWheel( -1 );
			}
			return;
		default:
			if( key < K_JOY1 || ( key >= K_MOUSE1 && key <= K_MOUSE5 ) )
			{
				ImGuiHook::InjectSysEvent( &event );
			}
			return;
	}
}

/*
================================================
idSWFScriptFunction_SystemOptionsRestart

The two answers of the restart dialog, as the SWF screen had them
================================================
*/
class idSWFScriptFunction_SystemOptionsRestart : public idSWFScriptFunction_RefCounted
{
public:
	idSWFScriptFunction_SystemOptionsRestart( const bool restart_ ) :
		restart( restart_ )
	{
	}

	idSWFScriptVar Call( idSWFScriptObject* thisObject, const idSWFParmList& parms )
	{
		common->Dialog().ClearDialog( GDM_GAME_RESTART_REQUIRED );
		if( restart )
		{
			Sys_ReLaunch();
		}
		return idSWFScriptVar();
	}

private:
	bool	restart;
};

namespace SystemOptions
{

/*
========================
SystemOptions::IsOpen
========================
*/
bool IsOpen()
{
	return panel.isOpen;
}

/*
========================
SystemOptions::HidesShell
========================
*/
bool HidesShell()
{
	return panel.isOpen && ( panel.previewHeld || panel.previewButtonHeld || panel.viewFade > 0.5f );
}

/*
========================
SystemOptions::Open
========================
*/
bool Open()
{
	if( !ImGuiHook::IsInitialized() )
	{
		return false;
	}
	if( panel.isOpen )
	{
		return true;
	}

	ResolveCVars();
	for( int i = 0; i < rows.Num(); i++ )
	{
		optionState_t& state = states[i];
		if( state.cvar != NULL )
		{
			state.openValue = state.cvar->GetString();
		}
		if( state.cvar2 != NULL )
		{
			state.openValue2 = state.cvar2->GetString();
		}
		state.openSaved = state.saved;
		state.openSavedValue = state.savedValue;
		state.editText[0] = '\0';
	}
	BuildDisplayModes();

	panel.isOpen = true;
	panel.firstFrame = true;
	panel.closeRequested = false;
	panel.revertRequested = false;
	panel.defaultsRequested = false;
	panel.defaultsSection = -1;
	panel.focusRequest = -1;
	panel.focusedSection = -1;
	panel.resetFocused = false;
	panel.previewHeld = false;
	panel.previewButtonHeld = false;
	panel.mouseSeen = false;
	if( !panel.initialized )
	{
		panel.openSection = -1;		// the first time, every section closed: the overview
		panel.initialized = true;
	}
	panel.scrollToSection = -1;
	panel.viewSection = -1;
	panel.viewSectionDrawn = false;
	panel.viewFade = 0.0f;
	panel.viewPlateHeight = 0.0f;
	panel.swallowRelease = 0;
	panel.filter[0] = '\0';

	// the default focus: the first section of the left column
	for( int i = 0; i < sections.Num(); i++ )
	{
		if( sections[i].column == 0 )
		{
			panel.focusRequest = i;
			break;
		}
	}

	ImGuiIO& io = ImGui::GetIO();
	panel.navWasOn = ( io.ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard ) != 0;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	panel.mouseX = io.DisplaySize.x * 0.5f;
	panel.mouseY = io.DisplaySize.y * 0.5f;
	panel.windowFocused = false;
	panel.contextGeneration = ImGuiHook::ContextGeneration();
	panel.scrollFrame = 0;
	// E: the key that opened the panel is still down; its repeats and its release belong to the shell's press
	panel.numHeldAtOpen = 0;
	for( int key = 0; key < K_LAST_KEY && panel.numHeldAtOpen < 8; key++ )
	{
		if( idKeyInput::IsDown( key ) && key != K_LSHIFT && key != K_RSHIFT && key != K_LCTRL && key != K_RCTRL && key != K_LALT && key != K_RALT )
		{
			panel.heldAtOpen[panel.numHeldAtOpen++] = key;
		}
	}
	for( int i = 0; i < MAX_SECTIONS; i++ )
	{
		panel.sectionDrawn[i] = true;
	}
	ImGuiHook::ResetInputLatches();
	ImGuiHook::ForgetMouse();
	return true;
}

/*
========================
SystemOptions::Close
========================
*/
void Close()
{
	if( !panel.isOpen )
	{
		return;
	}
	panel.isOpen = false;

	bool changed = false;
	bool restart = false;
	for( int i = 0; i < rows.Num(); i++ )
	{
		const optionRow_t& row = rows[i];
		optionState_t& state = states[i];
		if( RowIsChanged( state ) )
		{
			changed = true;
			if( row.flags & OPTF_RESTART )
			{
				restart = true;
			}
		}
		if( state.saved != state.openSaved || idStr::Cmp( state.savedValue, state.openSavedValue ) != 0 )
		{
			changed = true;		// a setSystemOption line to add or to drop
		}
	}
	if( changed )
	{
		cvarSystem->SetModifiedFlags( CVAR_ARCHIVE );
	}

	ImGuiIO& io = ImGui::GetIO();
	if( !panel.navWasOn )
	{
		io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
	}
	// no ImGui frame runs while the panel is closed: a value being typed or an open list would come back with it
	ImGui::ClearActiveID();
	if( GImGui->OpenPopupStack.Size > 0 )
	{
		ImGui::ClosePopupToLevel( 0, false );
	}
	ImGuiHook::ResetInputLatches();

	if( restart )
	{
		idStaticList<idSWFScriptFunction*, 4> callbacks;
		idStaticList<idStrId, 4> optionText;
		callbacks.Append( new( TAG_SWF ) idSWFScriptFunction_SystemOptionsRestart( false ) );
		callbacks.Append( new( TAG_SWF ) idSWFScriptFunction_SystemOptionsRestart( true ) );
		optionText.Append( idStrId( "#str_00100113" ) );	// Continue
		optionText.Append( idStrId( "#str_02487" ) );		// Restart Now
		common->Dialog().AddDynamicDialog( GDM_GAME_RESTART_REQUIRED, callbacks, optionText, true, idStr() );
	}
}

/*
========================
SystemOptions::HandleEvent
========================
*/
bool HandleEvent( const sysEvent_t* event )
{
	if( event == NULL )
	{
		return false;
	}

	if( !panel.isOpen )
	{
		// the key that closed the panel has its repeats and its release still to come: they are the panel's, not the
		// shell's, which would back out of its screens at each repeat
		if( panel.swallowRelease != 0 && event->evType == SE_KEY && event->evValue == panel.swallowRelease )
		{
			if( event->evValue2 == 0 )
			{
				panel.swallowRelease = 0;
			}
			return true;
		}
		return false;
	}

	if( event->evType == SE_KEY )
	{
		// a key held when the panel opened ( the press that opened it ): its repeats would act once more inside the panel
		for( int i = 0; i < panel.numHeldAtOpen; i++ )
		{
			if( panel.heldAtOpen[i] == event->evValue )
			{
				if( event->evValue2 == 0 )
				{
					panel.heldAtOpen[i] = panel.heldAtOpen[--panel.numHeldAtOpen];
				}
				return true;
			}
		}
	}

	switch( event->evType )
	{
		case SE_KEY:
			HandleKey( *event );
			break;

		case SE_MOUSE_ABSOLUTE:
			panel.mouseSeen = true;
			panel.mouseX = ( float )event->evValue;
			panel.mouseY = ( float )event->evValue2;
			ImGuiHook::InjectSysEvent( event );
			break;

		case SE_MOUSE:
		{
			// a grabbed mouse sends motion, not a place: follow it in window pixels
			panel.mouseSeen = true;
			const ImVec2 display = ImGui::GetIO().DisplaySize;
			panel.mouseX = idMath::ClampFloat( 0.0f, Max( 0.0f, display.x - 1.0f ), panel.mouseX + event->evValue );
			panel.mouseY = idMath::ClampFloat( 0.0f, Max( 0.0f, display.y - 1.0f ), panel.mouseY + event->evValue2 );
			sysEvent_t absolute = *event;
			absolute.evType = SE_MOUSE_ABSOLUTE;
			absolute.evValue = idMath::Ftoi( panel.mouseX );
			absolute.evValue2 = idMath::Ftoi( panel.mouseY );
			ImGuiHook::InjectSysEvent( &absolute );
			break;
		}

		case SE_CHAR:
		case SE_MOUSE_LEAVE:
			ImGuiHook::InjectSysEvent( event );
			break;

		default:
			break;	// a stick's axes, touch: nothing reaches the shell while the panel is open
	}
	return true;
}

/*
========================
SystemOptions::Draw
========================
*/
void Draw()
{
	if( !panel.isOpen )
	{
		return;
	}

	// the panel lives over the shell: when the shell goes away ( a map starts, the game unpauses ), so does it
	if( game == NULL || !game->Shell_IsActive() )
	{
		Close();
		return;
	}

	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;	// again after a vid_restart, which builds ImGui anew
	if( ImGuiHook::ContextGeneration() != panel.contextGeneration )
	{
		// a renderer restart ( a display mode ) built ImGui anew: the focus goes back to the first section
		panel.contextGeneration = ImGuiHook::ContextGeneration();
		panel.firstFrame = true;
		panel.focusRequest = ( panel.focusedSection >= 0 ) ? panel.focusedSection : 0;
	}
	if( console->Active() || common->Dialog().IsDialogActive() )
	{
		// the console or a dialog takes the keys, their releases too: let ImGui forget them, or one would stay down
		ImGuiHook::ResetInputLatches();
	}

	// the text is 18 pixels at 1080 lines and grows with the screen; the nearest of the three baked sizes, scaled
	const float display = Max( 1.0f, io.DisplaySize.y );
	const float target = idMath::ClampFloat( 12.0f, 54.0f, 18.0f * display / 1080.0f );
	int tier = 0;
	for( int i = 1; i < FONT_TIERS; i++ )
	{
		if( idMath::Fabs( idMath::Log( fontTierSizes[i] / target ) ) < idMath::Fabs( idMath::Log( fontTierSizes[tier] / target ) ) )
		{
			tier = i;
		}
	}
	// a baked size within an eighth of the target is drawn as baked, pixel for pixel; a scaled glyph is soft
	panelFontTier = tier;
	panelFontScale = target / fontTierSizes[tier];
	if( idMath::Fabs( panelFontScale - 1.0f ) < 0.125f )
	{
		panelFontScale = 1.0f;
	}
	panelFontScale = idMath::ClampFloat( 0.75f, 1.4f, panelFontScale );
	const float s = fontTierSizes[tier] * panelFontScale / 17.0f;

	const int pushed = PushTheme( s );
	ImGui::PushFont( PanelFont( panelFontTier, PANEL_FONT_BODY, panelFontScale ) );

	// no pointer until the mouse moves: ImGui would draw it in the corner where it never was
	if( !panel.mouseSeen )
	{
		ImGui::SetMouseCursor( ImGuiMouseCursor_None );
	}

	// the view: in a game, a section being adjusted keeps its opacity and the rest fades to a tenth, eased
	if( !game->IsInGame() || ( panel.viewSection >= 0 && panel.filter[0] == '\0' && panel.openSection != panel.viewSection ) )
	{
		panel.viewSection = -1;
	}
	const float fadeTarget = ( panel.viewSection >= 0 ) ? 1.0f : 0.0f;
	const float fadeStep = Max( 0.0f, io.DeltaTime ) / 0.15f;
	panel.viewFade = ( fadeTarget > panel.viewFade ) ? Min( fadeTarget, panel.viewFade + fadeStep ) : Max( fadeTarget, panel.viewFade - fadeStep );
	panel.viewSectionDrawn = false;
	panel.viewFocusSeen = false;

	const bool preview = panel.previewHeld || panel.previewButtonHeld;
	const float restShare = 1.0f - 0.9f * panel.viewFade;
	panelBaseAlpha = preview ? 0.06f : 1.0f;
	panelRestAlpha = panelBaseAlpha * restShare;
	if( !preview )
	{
		ImGui::GetBackgroundDrawList()->AddRectFilled( ImVec2( 0, 0 ), io.DisplaySize, IM_COL32( 0, 0, 0, idMath::Ftoi( 120.0f * restShare ) ) );
	}

	const ImVec2 size( idMath::Rint( io.DisplaySize.x * 0.75f ), idMath::Rint( io.DisplaySize.y * 0.75f ) );
	ImGui::SetNextWindowPos( ImVec2( io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f ), ImGuiCond_Always, ImVec2( 0.5f, 0.5f ) );
	ImGui::SetNextWindowSize( size, ImGuiCond_Always );
	if( panel.firstFrame || ( !panel.windowFocused && !ImGui::IsPopupOpen( "", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel ) ) )
	{
		// a click beside the panel would leave no window focused, and the keys and the pad with nothing to move
		ImGui::SetNextWindowFocus();
	}
	ImGui::PushStyleVar( ImGuiStyleVar_Alpha, panelBaseAlpha );
	ImGui::PushStyleColor( ImGuiCol_WindowBg, ImVec4( colorPanel.x, colorPanel.y, colorPanel.z, colorPanel.w * restShare ) );
	ImGui::PushStyleColor( ImGuiCol_Border, ImVec4( colorBorder.x, colorBorder.y, colorBorder.z, colorBorder.w * restShare ) );

	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
								   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
	if( ImGui::Begin( "System Options##panel", NULL, flags ) )
	{
		panel.windowFocused = ImGui::IsWindowFocused( ImGuiFocusedFlags_RootAndChildWindows );
		ImGui::PushStyleVar( ImGuiStyleVar_Alpha, panelRestAlpha );
		DrawHeader( s );
		ImGui::PopStyleVar();

		const float footerHeight = ImGui::GetFrameHeight() + 6.0f * s + ImGui::GetStyle().ItemSpacing.y * 3.0f + 4.0f * s;
		if( ImGui::BeginChild( "##body", ImVec2( 0, -footerHeight ), false, ImGuiWindowFlags_NavFlattened ) )
		{
			if( ImGui::BeginTable( "##columns", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings ) )
			{
				for( int column = 0; column < 2; column++ )
				{
					ImGui::TableNextColumn();
					ImGui::Indent( 6.0f * s );	// room for a changed row's mark left of its label
					for( int i = 0; i < sections.Num(); i++ )
					{
						if( sections[i].column == column )
						{
							DrawSection( i );
						}
					}
					ImGui::Unindent( 6.0f * s );
				}
				ImGui::EndTable();
			}
			if( panel.filter[0] != '\0' && ImGui::GetCursorPosY() < ImGui::GetFrameHeight() )
			{
				ImGui::TextColored( colorTextDim, "No setting holds \"%s\".", panel.filter );
			}
		}
		ImGui::EndChild();

		ImGui::PushStyleVar( ImGuiStyleVar_Alpha, panelRestAlpha );
		DrawFooter( s );
		ImGui::PopStyleVar();
	}
	ImGui::End();
	ImGui::PopStyleColor( 2 );
	ImGui::PopStyleVar();

	if( panel.viewSection >= 0 && !panel.viewSectionDrawn )
	{
		panel.viewSection = -1;		// a search hid it
	}
	if( panel.viewSection >= 0 && !panel.viewFocusSeen && !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen( "", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel ) )
	{
		// the focus went to a faded part ( another section, the search box, the buttons ): the whole panel again, so
		// that what has the focus can be seen
		panel.viewSection = -1;
	}

	if( preview )
	{
		const char* text = "Previewing  \xC2\xB7  release to return";
		const ImVec2 textSize = ImGui::CalcTextSize( text );
		const ImVec2 at( ( io.DisplaySize.x - textSize.x ) * 0.5f, io.DisplaySize.y * 0.875f + size.y * 0.0f );
		ImDrawList* draw = ImGui::GetForegroundDrawList();
		draw->AddRectFilled( ImVec2( at.x - 16.0f * s, at.y - 8.0f * s ), ImVec2( at.x + textSize.x + 16.0f * s, at.y + textSize.y + 8.0f * s ), IM_COL32( 0, 0, 0, 170 ), 14.0f * s );
		draw->AddText( at, ImGui::GetColorU32( colorText ), text );
	}

	ImGui::PopFont();
	PopTheme( pushed );

	panel.firstFrame = false;
	panel.resetFocused = false;		// a reset asked for with no row focused does nothing
	panel.focusRequest = -1;		// one asked for a section with nothing to show does nothing

	if( panel.revertRequested )
	{
		panel.revertRequested = false;
		Revert();
	}
	if( panel.defaultsRequested )
	{
		panel.defaultsRequested = false;
		SetDefaults( panel.defaultsSection );
	}
	if( panel.closeRequested )
	{
		panel.closeRequested = false;
		Close();
	}
}

/*
========================
SystemOptions::WriteConfig
========================
*/
void WriteConfig( idFile* f )
{
	if( rows.Num() == 0 )
	{
		return;	// neither the panel nor a setSystemOption line ran: nothing of the panel's to keep
	}
	ResolveCVars();
	for( int i = 0; i < rows.Num(); i++ )
	{
		const optionState_t& state = states[i];
		if( state.saved && state.cvar != NULL && ( state.cvar->GetFlags() & CVAR_ARCHIVE ) == 0 && !state.savedValue.IsEmpty() )
		{
			f->Printf( "setSystemOption %s \"%s\"\n", state.cvar->GetName(), state.savedValue.c_str() );
		}
	}
}

} // namespace SystemOptions

/*
========================
systemOptions_f
========================
*/
CONSOLE_COMMAND_SHIP( systemOptions, "opens the System Options panel ( Settings > System in the menus ); again closes it", NULL )
{
	if( SystemOptions::IsOpen() )
	{
		SystemOptions::Close();
		return;
	}
	if( game == NULL )
	{
		return;
	}
	if( !game->Shell_IsActive() )
	{
		// in a game: the pause menu first, so that the game stops and the mouse is free, as from Esc
		game->Shell_Show( true );
		game->Shell_SyncWithSession();
	}
	console->Close();
	if( !SystemOptions::Open() )
	{
		common->Printf( "The System Options panel needs ImGui, which is not up.\n" );
	}
}

/*
========================
setSystemOption_f

The config's lines for the panel's settings that are not archived: the cvar takes the value, and the panel keeps
it, so that the next write of the config has it again
========================
*/
CONSOLE_COMMAND_SHIP( setSystemOption, "sets a System Options setting that is not archived and keeps it in the config: setSystemOption <cvar> <value>", NULL )
{
	if( args.Argc() != 3 )
	{
		common->Printf( "usage: setSystemOption <cvar> <value>\n" );
		return;
	}
	const int index = FindRow( args.Argv( 1 ) );
	if( index < 0 )
	{
		common->Warning( "setSystemOption: %s is not a System Options setting", args.Argv( 1 ) );
		return;
	}
	ResolveCVars();
	optionState_t& state = states[index];
	if( state.cvar == NULL )
	{
		return;
	}
	state.cvar->SetString( args.Argv( 2 ) );
	RowNoteSet( rows[index], state );
}
