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

#ifndef __GAME_TRIGGER_H__
#define __GAME_TRIGGER_H__

extern const idEventDef EV_Enable;
extern const idEventDef EV_Disable;

/*
===============================================================================

  Trigger base.

===============================================================================
*/

//puncho - P2b.2.5b: idTrigger's list (K2) lists the keys only: KEYONLY, the key Spawn looks the script function up by;
// Save is hand-written, saving the script function by name
#define TRIGGER_SCHEMA( X, s ) \
	X( s, call,					STRING,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"call",			"",		"Script function to call when triggered.",	KEYONLY )

class idTrigger : public idEntity
{
public:
	CLASS_PROTOTYPE( idTrigger );

	static void			DrawDebugInfo();

	idTrigger();
	void				Spawn();

	const function_t* 	GetScriptFunction() const;

	void				Save( idSaveGame* savefile ) const;
	void				Restore( idRestoreGame* savefile );

	virtual void		Enable();
	virtual void		Disable();

protected:
	void				CallScript() const;

	void				Event_Enable();
	void				Event_Disable();

	const function_t* 	scriptFunction;
};


/*
===============================================================================

  Trigger which can be activated multiple times.

===============================================================================
*/

//puncho - P2b.2.5b: the wait rows idTrigger_Multi's and idTrigger_EntityName's lists open with, in their save order
#define TRIGGER_WAIT_SCHEMA( X, s ) \
	X( s, wait,					FLOAT,	S,		RAW,	0,	SCHEMA_STOCK,	"wait",			"0.5",	"Seconds between triggerings. Use '-1' to indicate one time only.",	OBJ ) \
	X( s, random,				FLOAT,	S,		RAW,	0,	SCHEMA_STOCK,	"random",		"0",	"Wait variance. Trigger waits wait + or - random after firing targets before triggering again.",	OBJ ) \
	X( s, delay,				FLOAT,	S,		RAW,	0,	SCHEMA_STOCK,	"delay",		"0",	"Seconds to wait before firing after triggered.",	OBJ ) \
	X( s, random_delay,			FLOAT,	S,		RAW,	0,	SCHEMA_STOCK,	"random_delay",	"0",	"Delay variance. Trigger waits delay + or - random_delay before firing targets.",	OBJ ) \
	X( s, nextTriggerTime,		INT,	S,		RAW,	0,	SCHEMA_STOCK,	NOKEY,			NODEF,	NOEDIT,	OBJ )

//puncho - P2b.2.5b: idTrigger_Multi's list (K2): Save and Restore expand the S rows, Spawn reads the keyed rows through
// one SCHEMA_SPAWN (the random clamps stay after it); KEYONLY, the keys Spawn tests in place and the touch reads late
#define TRIGGER_MULTI_SCHEMA( X, s ) \
	TRIGGER_WAIT_SCHEMA( X, s ) \
	X( s, requires,				STRING,	S,		RAW,	0,	SCHEMA_STOCK,	"requires",		"",		"Item required in players inventory to fire.",	OBJ ) \
	X( s, removeItem,			INT,	S,		RAW,	0,	SCHEMA_STOCK,	"removeItem",	"0",	"If above item is removed from inventory when used.",	OBJ ) \
	X( s, touchClient,			BOOL,	S,		RAW,	0,	SCHEMA_STOCK,	NOKEY,			NODEF,	NOEDIT,	OBJ ) \
	X( s, touchOther,			BOOL,	S,		RAW,	0,	SCHEMA_STOCK,	NOKEY,			NODEF,	NOEDIT,	OBJ ) \
	X( s, triggerFirst,			BOOL,	S,		RAW,	0,	SCHEMA_STOCK,	"triggerFirst",	"0",	"Don't activate until triggered.",	OBJ ) \
	X( s, triggerWithSelf,		BOOL,	S,		RAW,	0,	SCHEMA_STOCK,	"triggerWithSelf",	"0",	"Activate the targets with the trigger itself as the activator, not the entity that set it off.",	OBJ ) \
	X( s, anyTouch,				BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"anyTouch",		"0",	"Anything can touch it, players, mobs.",	KEYONLY ) \
	X( s, noTouch,				BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"noTouch",		"0",	"Cannot be touched, must be triggered.",	KEYONLY ) \
	X( s, noClient,				BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"noClient",		"0",	"Can't be touched by players, but can be touched by other entities (func_activators, for instance).",	KEYONLY ) \
	X( s, flashlight_trigger,	BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"flashlight_trigger",	"0",	"Triggered by the flashlight's beam instead of a touch.",	KEYONLY ) \
	X( s, facing,				BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"facing",		"0",	"A player triggers it only while facing along its axis, within angleLimit.",	KEYONLY ) \
	X( s, angleLimit,			FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"angleLimit",	"30",	"Angle limit in degrees that player must be within.",	KEYONLY ) \
	X( s, toggleTriggerFirst,	BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"toggleTriggerFirst",	"0",	"After each touch, wait for a trigger again before the next.",	KEYONLY )

class idTrigger_Multi : public idTrigger
{
public:
	CLASS_PROTOTYPE( idTrigger_Multi );

	idTrigger_Multi();

	void				Spawn();

	void				Save( idSaveGame* savefile ) const;
	void				Restore( idRestoreGame* savefile );

protected:

	float				wait;
	float				random;
	float				delay;
	float				random_delay;
	int					nextTriggerTime;
	idStr				requires;
	int					removeItem;
	bool				touchClient;
	bool				touchOther;
	bool				triggerFirst;
	bool				triggerWithSelf;

	bool				CheckFacing( idEntity* activator );
	void				TriggerAction( idEntity* activator );
	void				Event_TriggerAction( idEntity* activator );
	void				Event_Trigger( idEntity* activator );
	void				Event_Touch( idEntity* other, trace_t* trace );
};


/*
===============================================================================

  Trigger which can only be activated by an entity with a specific name.

===============================================================================
*/

//puncho - P2b.2.5b: idTrigger_EntityName's list (K2): Save and Restore expand the S rows, Spawn reads the keyed rows through
// one SCHEMA_SPAWN (the random clamps and entityname's check stay after it); testPartialName reads only when present
#define TRIGGER_ENTITYNAME_SCHEMA( X, s ) \
	TRIGGER_WAIT_SCHEMA( X, s ) \
	X( s, triggerFirst,			BOOL,	S,		RAW,	0,	SCHEMA_STOCK,	"triggerFirst",	"0",	"Don't activate until triggered.",	OBJ ) \
	X( s, entityName,			STRING,	S,		RAW,	0,	SCHEMA_STOCK,	"entityname",	"",		"Name of entity that may touch or activate the trigger.",	OBJ ) \
	X( s, testPartialName,		BOOL,	S,		RAW,	0,	SCHEMA_STOCK,	"testPartialName",	NODEF,	"Entityname can exist in any part touching entity.",	OBJ ) \
	X( s, noTouch,				BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"noTouch",		"0",	"Cannot be touched, must be triggered.",	KEYONLY )

class idTrigger_EntityName : public idTrigger
{
public:
	CLASS_PROTOTYPE( idTrigger_EntityName );

	idTrigger_EntityName();

	void				Save( idSaveGame* savefile ) const;
	void				Restore( idRestoreGame* savefile );

	void				Spawn();

private:
	float				wait;
	float				random;
	float				delay;
	float				random_delay;
	int					nextTriggerTime;
	bool				triggerFirst;
	idStr				entityName;
	bool				testPartialName;

	void				TriggerAction( idEntity* activator );
	void				Event_TriggerAction( idEntity* activator );
	void				Event_Trigger( idEntity* activator );
	void				Event_Touch( idEntity* other, trace_t* trace );
};

/*
===============================================================================

  Trigger which repeatedly fires targets.

===============================================================================
*/

//puncho - P2b.2.5b: idTrigger_Timer's list (K2): Save and Restore expand the S rows, Spawn reads the keyed rows through one
// SCHEMA_SPAWN (the random clamp and the first timer stay after it)
#define TRIGGER_TIMER_SCHEMA( X, s ) \
	X( s, random,				FLOAT,	S,		RAW,	0,	SCHEMA_STOCK,	"random",		"1",	"Wait variance. The timer fires every wait + or - random seconds.",	OBJ ) \
	X( s, wait,					FLOAT,	S,		RAW,	0,	SCHEMA_STOCK,	"wait",			"1",	"Seconds between firings.",	OBJ ) \
	X( s, on,					BOOL,	S,		RAW,	0,	SCHEMA_STOCK,	"start_on",		"0",	"Whether timer should be active when the level starts.",	OBJ ) \
	X( s, delay,				FLOAT,	S,		RAW,	0,	SCHEMA_STOCK,	"delay",		"0",	"Seconds after it is turned on before it first fires.",	OBJ ) \
	X( s, onName,				STRING,	S,		RAW,	0,	SCHEMA_STOCK,	"onName",		"",		"When set, only the entity of this name can turn the timer on.",	OBJ ) \
	X( s, offName,				STRING,	S,		RAW,	0,	SCHEMA_STOCK,	"offName",		"",		"When set, only the entity of this name can turn the timer off.",	OBJ )

class idTrigger_Timer : public idTrigger
{
public:
	CLASS_PROTOTYPE( idTrigger_Timer );

	idTrigger_Timer();

	void				Save( idSaveGame* savefile ) const;
	void				Restore( idRestoreGame* savefile );

	void				Spawn();

	virtual void		Enable();
	virtual void		Disable();

private:
	float				random;
	float				wait;
	bool				on;
	float				delay;
	idStr				onName;
	idStr				offName;

	void				Event_Timer();
	void				Event_Use( idEntity* activator );
};


/*
===============================================================================

  Trigger which fires targets after being activated a specific number of times.

===============================================================================
*/

//puncho - P2b.2.5b: idTrigger_Count's list (K2): Save and Restore expand the S rows, Spawn reads the keyed rows through one
// SCHEMA_SPAWN; KEYONLY, the key the trigger reads late
#define TRIGGER_COUNT_SCHEMA( X, s ) \
	X( s, goal,					INT,	S,		RAW,	0,	SCHEMA_STOCK,	"count",		"1",	"Number of times to be triggered before firing.",	OBJ ) \
	X( s, count,				INT,	S,		RAW,	0,	SCHEMA_STOCK,	NOKEY,			NODEF,	NOEDIT,	OBJ ) \
	X( s, delay,				FLOAT,	S,		RAW,	0,	SCHEMA_STOCK,	"delay",		"0",	"Seconds to wait before firing after triggered.",	OBJ ) \
	X( s, repeat,				BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"repeat",		"0",	"Defaults to zero, if 1 the trigger will reset and count up again, if 0 it will only fire once.",	KEYONLY )

class idTrigger_Count : public idTrigger
{
public:
	CLASS_PROTOTYPE( idTrigger_Count );

	idTrigger_Count();

	void				Save( idSaveGame* savefile ) const;
	void				Restore( idRestoreGame* savefile );

	void				Spawn();

private:
	int					goal;
	int					count;
	float				delay;

	void				Event_Trigger( idEntity* activator );
	void				Event_TriggerAction( idEntity* activator );
};


/*
===============================================================================

  Trigger which hurts touching entities.

===============================================================================
*/

//puncho - P2b.2.5b: idTrigger_Hurt's list (K2): Save and Restore expand the S rows, Spawn reads the keyed rows through one
// SCHEMA_SPAWN; KEYONLY, the keys the touch reads late
#define TRIGGER_HURT_SCHEMA( X, s ) \
	X( s, on,					BOOL,	S,		RAW,	0,	SCHEMA_STOCK,	"on",			"1",	"Whether or not it is active.",	OBJ ) \
	X( s, delay,				FLOAT,	S,		RAW,	0,	SCHEMA_STOCK,	"delay",		"1.0",	"Delay between damage in seconds.",	OBJ ) \
	X( s, nextTime,				INT,	S,		RAW,	0,	SCHEMA_STOCK,	NOKEY,			NODEF,	NOEDIT,	OBJ ) \
	X( s, playerOnly,			BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"playerOnly",	"0",	"Only players are hurt.",	KEYONLY ) \
	X( s, def_damage,			STRING,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"def_damage",	"damage_painTrigger",	"Damage def to use, common ones are damage_painTrigger, damage_triggerhurt_100, damage_triggerhurt_1000.",	KEYONLY ) \
	X( s, kick_from_center,		BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"kick_from_center",	"0",	"The damage pushes away from the trigger's origin.",	KEYONLY )

class idTrigger_Hurt : public idTrigger
{
public:
	CLASS_PROTOTYPE( idTrigger_Hurt );

	idTrigger_Hurt();

	void				Save( idSaveGame* savefile ) const;
	void				Restore( idRestoreGame* savefile );

	void				Spawn();

private:
	bool				on;
	float				delay;
	int					nextTime;

	void				Event_Touch( idEntity* other, trace_t* trace );
	void				Event_Toggle( idEntity* activator );
};


/*
===============================================================================

  Trigger which fades the player view.

===============================================================================
*/

//puncho - P2b.2.5b: the keys idTrigger_Fade's trigger reads (K2); fadeColor is a 4-vector, listed as a string
#define TRIGGER_FADE_SCHEMA( X, s ) \
	X( s, fadeColor,			STRING,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"fadeColor",	"0, 0, 0, 1",	"Color to fade to.",	KEYONLY ) \
	X( s, fadeTime,				FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"fadeTime",		"0.5",	"How long it takes to fade.",	KEYONLY )

class idTrigger_Fade : public idTrigger
{
public:

	CLASS_PROTOTYPE( idTrigger_Fade );

private:
	void				Event_Trigger( idEntity* activator );
};


/*
===============================================================================

  Trigger which continuously tests whether other entities are touching it.

===============================================================================
*/

//puncho - P2b.2.5b: idTrigger_Touch's list (K2) lists the keys only: KEYONLY, the key Spawn tests in place;
// Save is hand-written for the clip model
#define TRIGGER_TOUCH_SCHEMA( X, s ) \
	X( s, start_on,				BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"start_on",		"0",	"Set to 1 to test for touching entities from the start.",	KEYONLY )

class idTrigger_Touch : public idTrigger
{
public:
	~idTrigger_Touch();

	CLASS_PROTOTYPE( idTrigger_Touch );

	idTrigger_Touch();

	void				Spawn();
	virtual void		Think();

	void				Save( idSaveGame* savefile );
	void				Restore( idRestoreGame* savefile );

	virtual void		Enable();
	virtual void		Disable();

	void				TouchEntities();

private:
	idClipModel* 		clipModel;

	void				Event_Trigger( idEntity* activator );
};

/*
===============================================================================

  Trigger that responces to CTF flags

===============================================================================
*/
//puncho - P2b.2.5b: idTrigger_Flag's list (K2), Spawn only (the class has no Save): the keyed members its Spawn reads through
// one SCHEMA_SPAWN, KEYONLY the event name it looks up
#define TRIGGER_FLAG_SCHEMA( X, s ) \
	X( s, team,					INT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"team",			"0",	"Team of the flag the trigger answers: 0 red, 1 blue, -1 either.",	OBJ ) \
	X( s, player,				BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"player",		"0",	"The flag must be carried by a player of the team.",	OBJ ) \
	X( s, eventflag,			STRING,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"eventflag",	"",		"Event to post on the flag that touches the trigger.",	KEYONLY )

class idTrigger_Flag : public idTrigger_Multi
{
public:
	CLASS_PROTOTYPE( idTrigger_Flag );

	idTrigger_Flag();
	void				Spawn();
	//puncho start - P2b.2.5b: testSpawnRead's rows
	int					Team() const
	{
		return team;
	}
	bool				Player() const
	{
		return player;
	}
	//puncho end

private:
	int					team;
	bool				player;			// flag must be attached/carried by player

	const idEventDef* 	eventFlag;

	void				Event_Touch( idEntity* other, trace_t* trace );
};

//puncho - P2b.2.5b: the keys idTrigger_ChangeLevel's transition reads (K2); arrivalLandmark defaults to landmark
#define TRIGGER_CHANGE_LEVEL_SCHEMA( X, s ) \
	X( s, landmark,				STRING,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"landmark",		"",		"Landmark this exit leaves from.",	KEYONLY ) \
	X( s, wait_party,			BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"wait_party",	"1",	"In a party, wait until every player is at the exit.",	KEYONLY ) \
	X( s, nextMap,				STRING,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"nextMap",		"",		"Map to go to.",	KEYONLY ) \
	X( s, arrivalLandmark,		STRING,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"arrivalLandmark",	NODEF,	"Landmark to arrive at in nextMap; without it, landmark.",	KEYONLY )

class idTrigger_ChangeLevel : public idTrigger_Multi
{
public:
	CLASS_PROTOTYPE( idTrigger_ChangeLevel );
	idTrigger_ChangeLevel();
private:
	bool Transition( idEntity* activator );
	void Event_TransitionTouch( idEntity* other, trace_t* trace );
	void Event_TransitionActivate( idEntity* activator );
	int refusedTouchTime; // not saved: a load re-arms the latch, costing at most one more refusal
};

#endif /* !__GAME_TRIGGER_H__ */
