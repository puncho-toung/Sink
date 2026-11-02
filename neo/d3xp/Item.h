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

#ifndef __GAME_ITEM_H__
#define __GAME_ITEM_H__


/*
===============================================================================

  Items the player can pick up or use.

===============================================================================
*/

/*
================================================
These flags are passed to the Give functions
to set their behavior. We need to be able to
separate the feedback from the actual
state modification so that we can hide lag
on MP clients.

For the previous behavior of functions which
take a giveFlags parameter (this is usually
desired on the server too) pass
ITEM_GIVE_FEEDBACK | ITEM_GIVE_UPDATE_STATE.
================================================
*/
enum itemGiveFlags_t
{
	ITEM_GIVE_FEEDBACK			= BIT( 0 ),
	ITEM_GIVE_UPDATE_STATE		= BIT( 1 ),
	ITEM_GIVE_FROM_WEAPON		= BIT( 2 ),			// indicates this was given via a weapon's launchPowerup (for bloodstone powerups)
};

//puncho start - P2b.2.5a: the item classes' lists (K2), each above its class. Save and Restore expand the S rows; a Spawn
// reads its keyed rows through one SCHEMA_SPAWN; the KEYONLY rows are the keys a Spawn reads by hand (tested in place,
// derived, into locals or a pointer) and, P2b.2.5a (review), the keys the class's events and helpers read late, named for
// the key unless the key's member is a row already (spinKey, screenshotKey)
#define ITEM_SCHEMA( X, s ) \
	X( s, orgOrigin,			VEC3,		S,		RAW,	0,	SCHEMA_STOCK,	NOKEY,			NODEF,	NOEDIT,	OBJ ) \
	X( s, spin,					BOOL,		S,		RAW,	0,	SCHEMA_STOCK,	NOKEY,			NODEF,	NOEDIT,	OBJ ) \
	X( s, pulse,				BOOL,		S,		RAW,	0,	SCHEMA_STOCK,	NOKEY,			NODEF,	NOEDIT,	OBJ ) \
	X( s, canPickUp,			BOOL,		S,		RAW,	0,	SCHEMA_STOCK,	NOKEY,			NODEF,	NOEDIT,	OBJ ) \
	X( s, shellMaterial,		MATERIAL,	S,		RAW,	0,	SCHEMA_STOCK,	NOKEY,			NODEF,	NOEDIT,	OBJ ) \
	X( s, inView,				BOOL,		S,		RAW,	0,	SCHEMA_STOCK,	NOKEY,			NODEF,	NOEDIT,	OBJ ) \
	X( s, inViewTime,			INT,		S,		RAW,	0,	SCHEMA_STOCK,	NOKEY,			NODEF,	NOEDIT,	OBJ ) \
	X( s, lastCycle,			INT,		S,		RAW,	0,	SCHEMA_STOCK,	NOKEY,			NODEF,	NOEDIT,	OBJ ) \
	X( s, lastRenderViewTime,	INT,		S,		RAW,	0,	SCHEMA_STOCK,	NOKEY,			NODEF,	NOEDIT,	OBJ ) \
	X( s, dropToFloor,			BOOL,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"dropToFloor",	"0",	"Drop the item to the floor after it spawns.",	KEYONLY ) \
	X( s, triggersize,			FLOAT,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"triggersize",	NODEF,	"Amount to expand the pickup trigger by.",	KEYONLY ) \
	X( s, start_off,			BOOL,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"start_off",	"0",	"The item starts hidden, with no pickup trigger.",	KEYONLY ) \
	X( s, owner,				STRING,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"owner",		"",		"Name of an entity the item is given to when it spawns.",	KEYONLY ) \
	X( s, spinKey,				BOOL,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"spin",			"0",	"The item rotates and bobs; in multiplayer every item but a CTF flag does.",	KEYONLY ) \
	X( s, triggerFirst,			BOOL,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"triggerFirst",	"0",	"The item can be picked up only after it has been triggered.",	KEYONLY ) \
	X( s, no_touch,				BOOL,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"no_touch",		"0",	"Trigger to give to the player instead of picking up.",	KEYONLY ) \
	X( s, respawn,				FLOAT,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"respawn",		"0",	"Seconds after a pickup before the item respawns; 0 never, except in multiplayer (20).",	KEYONLY ) \
	X( s, no_respawn,			BOOL,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"no_respawn",	"0",	"The item never respawns.",	KEYONLY ) \
	X( s, fxRespawn,			STRING,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"fxRespawn",	"",		"FX to play half a second before the item respawns.",	KEYONLY ) \
	X( s, inv_carry,			BOOL,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"inv_carry",	"0",	"If set to 1, is carried as opposed to instant use.",	KEYONLY ) \
	X( s, inv_objective,		BOOL,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"inv_objective",	"0",	"An objective item, kept after its pickup rather than removed.",	KEYONLY )
//puncho end

class idItem : public idEntity
{
public:
	CLASS_PROTOTYPE( idItem );
	virtual bool CanCrossTransition() const override
	{
		return true;
	}

	idItem();
	virtual					~idItem();

	void					Save( idSaveGame* savefile ) const;
	void					Restore( idRestoreGame* savefile );

	void					Spawn();
	void					GetAttributes( idDict& attributes ) const;
	virtual bool			GiveToPlayer( idPlayer* player, unsigned int giveFlags );
	virtual bool			Pickup( idPlayer* player );
	virtual void			Think();
	virtual void			Present();

	enum
	{
		EVENT_PICKUP = idEntity::EVENT_MAXEVENTS,
		EVENT_RESPAWN,
		EVENT_RESPAWNFX,
		EVENT_TAKEFLAG,
		EVENT_DROPFLAG,
		EVENT_FLAGRETURN,
		EVENT_FLAGCAPTURE,
		EVENT_MAXEVENTS
	};

	void					ClientThink( const int curTime, const float fraction, const bool predict );
	virtual void			ClientPredictionThink();
	virtual bool			ClientReceiveEvent( int event, int time, const idBitMsg& msg );

	// networking
	virtual void			WriteToSnapshot( idBitMsg& msg ) const;
	virtual void			ReadFromSnapshot( const idBitMsg& msg );

protected:
	int						GetPredictPickupMilliseconds() const
	{
		return clientPredictPickupMilliseconds;
	}

private:
	idVec3					orgOrigin;
	bool					spin;
	bool					pulse;
	bool					canPickUp;

	// for item pulse effect
	int						itemShellHandle;
	const idMaterial* 		shellMaterial;

	// used to update the item pulse effect
	mutable bool			inView;
	mutable int				inViewTime;
	mutable int				lastCycle;
	mutable int				lastRenderViewTime;

	// used for prediction in mp
	int						clientPredictPickupMilliseconds;

	bool					UpdateRenderEntity( renderEntity_s* renderEntity, const renderView_t* renderView ) const;
	static bool				ModelCallback( renderEntity_s* renderEntity, const renderView_t* renderView );

	void					Event_DropToFloor();
	void					Event_Touch( idEntity* other, trace_t* trace );
	void					Event_Trigger( idEntity* activator );
	void					Event_Respawn();
	void					Event_RespawnFx();
};

//puncho - P2b.2.5a: idItemPowerup's list (K2)
#define ITEM_POWERUP_SCHEMA( X, s ) \
	X( s, time,					INT,		S,		RAW,	0,	SCHEMA_STOCK,	"time",			"30",	"Seconds the powerup lasts.",	OBJ ) \
	X( s, type,					INT,		S,		RAW,	0,	SCHEMA_STOCK,	"type",			"0",	"The powerup (Player.h): 0 berserk, 1 invisibility, 2 megahealth, 3 adrenaline, 4 invulnerability, 5 helltime, 6 envirosuit, 7 envirotime.",	OBJ )

class idItemPowerup : public idItem
{
public:
	CLASS_PROTOTYPE( idItemPowerup );

	idItemPowerup();

	void					Save( idSaveGame* savefile ) const;
	void					Restore( idRestoreGame* savefile );

	void					Spawn();
	virtual bool			GiveToPlayer( idPlayer* player, unsigned int giveFlags );

private:
	int						time;
	int						type;
};

//puncho - P2b.2.5a: idObjective's list (K2)
#define OBJECTIVE_SCHEMA( X, s ) \
	X( s, playerPos,			VEC3,		S,		RAW,	0,	SCHEMA_STOCK,	NOKEY,			NODEF,	NOEDIT,	OBJ ) \
	X( s, screenshot,			MATERIAL,	S,		RAW,	0,	SCHEMA_STOCK,	NOKEY,			NODEF,	NOEDIT,	OBJ ) \
	X( s, screenshotKey,		STRING,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"screenshot",	"",		"Image name to save the shot to, under the map's name.",	KEYONLY ) \
	X( s, inv_objective,		STRING,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"inv_objective",	NODEF,	"When set, triggering gives the player the objective.",	KEYONLY ) \
	X( s, objectivetitle,		STRING,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"objectivetitle",	"",		"Objective title.",	KEYONLY ) \
	X( s, objectivetext,		STRING,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"objectivetext",	"",		"Objective description.",	KEYONLY )

class idObjective : public idItem
{
public:
	CLASS_PROTOTYPE( idObjective );

	idObjective();

	void					Save( idSaveGame* savefile ) const;
	void					Restore( idRestoreGame* savefile );

	void					Spawn();

private:
	idVec3					playerPos;
	const idMaterial* 		screenshot;

	void					Event_Trigger( idEntity* activator );
	void					Event_HideObjective( idEntity* e );
	void					Event_GetPlayerPos();
};

//puncho - P2b.2.5a (review): the keys GiveToPlayer reads (K2)
#define VIDEO_CD_ITEM_SCHEMA( X, s ) \
	X( s, video,				STRING,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"video",		"",		"Name of the .video file to give to the player.",	KEYONLY ) \
	X( s, inv_name,				STRING,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"inv_name",		"",		"Name referenced for triggers etc. requiring this to be in inventory to work.",	KEYONLY )

class idVideoCDItem : public idItem
{
public:
	CLASS_PROTOTYPE( idVideoCDItem );

	virtual bool			GiveToPlayer( idPlayer* player, unsigned int giveFlags );
};

//puncho - P2b.2.5a (review): the keys GiveToPlayer reads (K2)
#define PDA_ITEM_SCHEMA( X, s ) \
	X( s, pda_name,				STRING,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"pda_name",		"",		"Name of the .pda decl to give to the player.",	KEYONLY ) \
	X( s, inv_name,				STRING,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"inv_name",		"",		"Name referenced for triggers etc. requiring this to be in inventory to work.",	KEYONLY )

class idPDAItem : public idItem
{
public:
	CLASS_PROTOTYPE( idPDAItem );

	virtual bool			GiveToPlayer( idPlayer* player, unsigned int giveFlags );
};

//puncho - P2b.2.5a: idMoveableItem's list (K2). The trigger's clip model and the smoke particle have no schema type:
// HAND rows (K8), Save<n> and Restore<n> in Item.cpp
#define MOVEABLE_ITEM_SCHEMA( X, s ) \
	X( s, physicsObj,			STATIC,		S,		RAW,	0,	SCHEMA_STOCK,	NOKEY,			NODEF,	NOEDIT,	OBJ ) \
	X( s, Trigger,				HAND,		S,		RAW,	0,	SCHEMA_STOCK,	NOKEY,			NODEF,	NOEDIT,	OBJ ) \
	X( s, Smoke,				HAND,		S,		RAW,	0,	SCHEMA_STOCK,	NOKEY,			NODEF,	NOEDIT,	OBJ ) \
	X( s, smokeTime,			INT,		S,		RAW,	0,	SCHEMA_STOCK,	NOKEY,			NODEF,	NOEDIT,	OBJ ) \
	X( s, nextSoundTime,		INT,		S,		RAW,	0,	SCHEMA_STOCK,	NOKEY,			NODEF,	NOEDIT,	OBJ ) \
	X( s, repeatSmoke,			BOOL,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"repeatSmoke",	"0",	"Never stop updating the smoke_trail particles.",	OBJ ) \
	X( s, triggersize,			FLOAT,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"triggersize",	"16.0",	"Amount to expand the pickup trigger by.",	KEYONLY ) \
	X( s, clipmodel,			STRING,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"clipmodel",	"",		"Model to be used for collision detection.",	KEYONLY ) \
	X( s, clipshrink,			BOOL,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"clipshrink",	"0",	"Shrink the clip model with an epsilon.",	KEYONLY ) \
	X( s, density,				FLOAT,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"density",		"0.5",	"Volume of the object times the density is the mass.",	KEYONLY ) \
	X( s, friction,				FLOAT,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"friction",		"0.05",	"Friction with contacting surfaces in the range [0-1] where 0 is no friction.",	KEYONLY ) \
	X( s, bouncyness,			FLOAT,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"bouncyness",	"0.6",	"How much the object bounces in the range [0-1] where 0 is no bounce.",	KEYONLY ) \
	X( s, nodrop,				BOOL,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"nodrop",		"0",	"Do not drop to the floor.",	KEYONLY ) \
	X( s, smoke_trail,			STRING,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"smoke_trail",	"",		"Particle system to trail while the item moves.",	KEYONLY ) \
	X( s, model,				STRING,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"model",		NODEF,	"Visual model; also the clip model when clipmodel is not set.",	KEYONLY ) \
	X( s, smoke_gib,			STRING,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"smoke_gib",	"",		"Particle system to emit when the item is gibbed.",	KEYONLY )

class idMoveableItem : public idItem
{
public:
	CLASS_PROTOTYPE( idMoveableItem );

	idMoveableItem();
	virtual					~idMoveableItem();

	void					Save( idSaveGame* savefile ) const;
	void					Restore( idRestoreGame* savefile );

	void					Spawn();
	virtual void			Think();
	void					ClientThink( const int curTime, const float fraction, const bool predict );
	virtual bool			Collide( const trace_t& collision, const idVec3& velocity );
	virtual bool			Pickup( idPlayer* player );

	static void				DropItems( idAnimatedEntity* ent, const char* type, idList<idEntity*>* list, bool essential );
	static idEntity*			DropItem( const char* classname, const idVec3& origin, const idMat3& axis, const idVec3& velocity, int activateDelay, int removeDelay, bool essential );
	bool					RepeatSmoke() const	//puncho - P2b.2.5a: testSpawnRead's row
	{
		return repeatSmoke;
	}

	virtual void			WriteToSnapshot( idBitMsg& msg ) const;
	virtual void			ReadFromSnapshot( const idBitMsg& msg );

protected:
	idPhysics_RigidBody		physicsObj;
	idClipModel* 			trigger;
	const idDeclParticle* 	smoke;
	int						smokeTime;

	int						nextSoundTime;
	bool					repeatSmoke;	// never stop updating the particles

	void					Gib( const idVec3& dir, const char* damageDefName );
	//puncho start - P2b.2.5a: MOVEABLE_ITEM_SCHEMA's HAND rows (K8)
	void					SaveTrigger( idSaveGame* savefile ) const;
	void					RestoreTrigger( idRestoreGame* savefile );
	void					SaveSmoke( idSaveGame* savefile ) const;
	void					RestoreSmoke( idRestoreGame* savefile );
	//puncho end

	void					Event_DropToFloor();
	void					Event_Gib( const char* damageDefName );
};

//puncho - P2b.2.5a: idItemTeam's list (K2), Spawn only (the class has no Save). Its CLASS_DECLARATION names idItem, so the
// type chain skips idMoveableItem, but its Spawn calls idMoveableItem::Spawn() itself: KEYONLY the keys that call reads
// (P2b.2.5a review), and the script keys LoadScript reads
#define ITEM_TEAM_SCHEMA( X, s ) \
	X( s, team,					INT,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"team",			"0",	"The flag's team: 0 red, 1 blue.",	OBJ ) \
	X( s, skin_carried,			STRING,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"skin_carried",	"",		"Skin while a player carries the flag.",	KEYONLY ) \
	X( s, nugget_name,			STRING,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"nugget_name",	"",		"Entity def the carried flag drops as nuggets.",	KEYONLY ) \
	X( s, gravity,				INT,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"gravity",		"-30",	"Gravity on the flag along z.",	KEYONLY ) \
	X( s, skin,					STRING,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"skin",			NODEF,	"Skin while no player carries the flag.",	KEYONLY ) \
	X( s, pickupDelay,			INT,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"pickupDelay",	"500",	"Milliseconds after a drop before the flag can be picked up again.",	KEYONLY ) \
	X( s, script_taken,			STRING,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"script_taken",	"",		"Script function to call when the flag is taken.",	KEYONLY ) \
	X( s, script_dropped,		STRING,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"script_dropped",	"",		"Script function to call when the flag is dropped.",	KEYONLY ) \
	X( s, script_returned,		STRING,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"script_returned",	"",		"Script function to call when the flag is returned.",	KEYONLY ) \
	X( s, script_captured,		STRING,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"script_captured",	"",		"Script function to call when the flag is captured.",	KEYONLY ) \
	X( s, triggersize,			FLOAT,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"triggersize",	"16.0",	"Amount to expand the pickup trigger by.",	KEYONLY ) \
	X( s, clipmodel,			STRING,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"clipmodel",	"",		"Model to be used for collision detection.",	KEYONLY ) \
	X( s, model,				STRING,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"model",		NODEF,	"Visual model; also the clip model when clipmodel is not set.",	KEYONLY ) \
	X( s, clipshrink,			BOOL,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"clipshrink",	"0",	"Shrink the clip model with an epsilon.",	KEYONLY ) \
	X( s, density,				FLOAT,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"density",		"0.5",	"Volume of the object times the density is the mass.",	KEYONLY ) \
	X( s, friction,				FLOAT,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"friction",		"0.05",	"Friction with contacting surfaces in the range [0-1] where 0 is no friction.",	KEYONLY ) \
	X( s, bouncyness,			FLOAT,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"bouncyness",	"0.6",	"How much the object bounces in the range [0-1] where 0 is no bounce.",	KEYONLY ) \
	X( s, nodrop,				BOOL,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"nodrop",		"0",	"Do not drop to the floor.",	KEYONLY ) \
	X( s, smoke_trail,			STRING,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"smoke_trail",	"",		"Particle system to trail while the item moves.",	KEYONLY ) \
	X( s, repeatSmoke,			BOOL,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"repeatSmoke",	"0",	"Never stop updating the smoke_trail particles.",	KEYONLY )

class idItemTeam : public idMoveableItem
{
public:
	CLASS_PROTOTYPE( idItemTeam );

	idItemTeam();
	virtual					~idItemTeam();

	void                    Spawn();
	virtual bool			Pickup( idPlayer* player );
	virtual bool			ClientReceiveEvent( int event, int time, const idBitMsg& msg );
	virtual void			Think();

	void					Drop( bool death = false );	// was the drop caused by death of carrier?
	void					Return( idPlayer* player = NULL );
	void					Capture();

	virtual void			FreeLightDef();
	virtual void			Present();
	const char* 			NuggetName() const	//puncho - P2b.2.5a: testSpawnRead's row
	{
		return nuggetName;
	}

	// networking
	virtual void			WriteToSnapshot( idBitMsg& msg ) const;
	virtual void			ReadFromSnapshot( const idBitMsg& msg );

public:
	int                     team;
	// TODO : turn this into a state :
	bool					carried;			// is it beeing carried by a player?
	bool					dropped;			// was it dropped?

private:
	idVec3					returnOrigin;
	idMat3					returnAxis;
	int						lastDrop;

	const idDeclSkin* 		skinDefault;
	const idDeclSkin* 		skinCarried;

	const function_t* 		scriptTaken;
	const function_t* 		scriptDropped;
	const function_t* 		scriptReturned;
	const function_t* 		scriptCaptured;

	renderLight_t           itemGlow;           // Used by flags when they are picked up
	int                     itemGlowHandle;

	int						lastNuggetDrop;
	const char* 			nuggetName;

private:

	void					Event_TakeFlag( idPlayer* player );
	void					Event_DropFlag( bool death );
	void					Event_FlagReturn( idPlayer* player = NULL );
	void					Event_FlagCapture();

	void					PrivateReturn();
	function_t* 			LoadScript( const char* script );

	void					SpawnNugget( idVec3 pos );
	void                    UpdateGuis();
};

//puncho - P2b.2.5a (review): the keys GiveToPlayer reads (K2)
#define MOVEABLE_PDA_ITEM_SCHEMA( X, s ) \
	X( s, pda_name,				STRING,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"pda_name",		"",		"Name of the .pda decl to give to the player.",	KEYONLY ) \
	X( s, inv_name,				STRING,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"inv_name",		"",		"Name referenced for triggers etc. requiring this to be in inventory to work.",	KEYONLY )

class idMoveablePDAItem : public idMoveableItem
{
public:
	CLASS_PROTOTYPE( idMoveablePDAItem );

	virtual bool			GiveToPlayer( idPlayer* player, unsigned int giveFlags );
};

/*
===============================================================================

  Item removers.

===============================================================================
*/

//puncho - P2b.2.5a (review): the key RemoveItem reads (K2)
#define ITEM_REMOVER_SCHEMA( X, s ) \
	X( s, remove,				STRING,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"remove",		"",		"Name of the inventory item to remove, e.g. objective or key_red.",	KEYONLY )

class idItemRemover : public idEntity
{
public:
	CLASS_PROTOTYPE( idItemRemover );

	void					Spawn();
	void					RemoveItem( idPlayer* player );

private:
	void					Event_Trigger( idEntity* activator );
};

//puncho - P2b.2.5a: idObjectiveComplete's list (K2)
#define OBJECTIVE_COMPLETE_SCHEMA( X, s ) \
	X( s, playerPos,			VEC3,		S,		RAW,	0,	SCHEMA_STOCK,	NOKEY,			NODEF,	NOEDIT,	OBJ ) \
	X( s, inv_objective,		STRING,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"inv_objective",	NODEF,	"When set, triggering completes the objective named by objectivetitle.",	KEYONLY ) \
	X( s, objectivetitle,		STRING,		NOCH,	RAW,	0,	SCHEMA_STOCK,	"objectivetitle",	"",		"Title of the objective to complete.",	KEYONLY )

class idObjectiveComplete : public idItemRemover
{
public:
	CLASS_PROTOTYPE( idObjectiveComplete );

	idObjectiveComplete();

	void					Save( idSaveGame* savefile ) const;
	void					Restore( idRestoreGame* savefile );

	void					Spawn();

private:
	idVec3					playerPos;

	void					Event_Trigger( idEntity* activator );
	void					Event_HideObjective( idEntity* e );
	void					Event_GetPlayerPos();
};

#endif /* !__GAME_ITEM_H__ */
