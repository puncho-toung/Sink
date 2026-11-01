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

#ifndef __GAME_MOVER_H__
#define __GAME_MOVER_H__

extern const idEventDef EV_TeamBlocked;
extern const idEventDef EV_PartBlocked;
extern const idEventDef EV_ReachedPos;
extern const idEventDef EV_ReachedAng;

/*
===============================================================================

  General movers.

===============================================================================
*/

//puncho - P2b.2.5a: idMover's spawn rows (K2), Spawn only (the Save is hand-written): the keyed members Spawn reads through
// one SCHEMA_SPAWN, health again over idEntity's read; KEYONLY, the keys it reads by hand (scaled to ms, tested in place)
#define MOVER_SCHEMA( X, s ) \
	X( s, move_speed,	FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"move_speed",	"0",	"How many units the mover travels per second; cannot be used with move_time.",	OBJ ) \
	X( s, damage,		FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"damage",		"0",	"How much damage to cause entities that block the mover.",	OBJ ) \
	X( s, health,		INT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"health",		"0",	"When non-zero, how much damage to take before triggering targets.",	OBJ ) \
	X( s, accel_time,	FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"accel_time",	"0",	"How long it takes the mover to accelerate to full speed.",	KEYONLY ) \
	X( s, decel_time,	FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"decel_time",	"0",	"How long it takes the mover to decelerate to a stop.",	KEYONLY ) \
	X( s, move_time,	FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"move_time",	"1",	"How long it takes the mover to complete a move; cannot be used with move_speed.",	KEYONLY ) \
	X( s, solid,		BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"solid",		"1",	"Whether the mover is solid to other entities.",	KEYONLY ) \
	X( s, nopush,		BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"nopush",		"0",	"Set to 1 to disable pushing objects that block the mover.",	KEYONLY )

class idMover : public idEntity
{
public:
	CLASS_PROTOTYPE( idMover );

	idMover();

	void					Spawn();

	void					Save( idSaveGame* savefile ) const;
	void					Restore( idRestoreGame* savefile );

	virtual void			Killed( idEntity* inflictor, idEntity* attacker, int damage, const idVec3& dir, int location );
	virtual void			ClientThink( const int curTime, const float fraction, const bool predict );
	virtual void			WriteToSnapshot( idBitMsg& msg ) const;
	virtual void			ReadFromSnapshot( const idBitMsg& msg );

	virtual void			Hide();
	virtual void			Show();

	void					SetPortalState( bool open );

protected:
	typedef enum
	{
		ACCELERATION_STAGE,
		LINEAR_STAGE,
		DECELERATION_STAGE,
		FINISHED_STAGE
	} moveStage_t;

	typedef enum
	{
		MOVER_NONE,
		MOVER_ROTATING,
		MOVER_MOVING,
		MOVER_SPLINE
	} moverCommand_t;

	//
	// mover directions.  make sure to change script/doom_defs.script if you add any, or change their order
	//
	typedef enum
	{
		DIR_UP				= -1,
		DIR_DOWN			= -2,
		DIR_LEFT			= -3,
		DIR_RIGHT			= -4,
		DIR_FORWARD			= -5,
		DIR_BACK			= -6,
		DIR_REL_UP			= -7,
		DIR_REL_DOWN		= -8,
		DIR_REL_LEFT		= -9,
		DIR_REL_RIGHT		= -10,
		DIR_REL_FORWARD		= -11,
		DIR_REL_BACK		= -12
	} moverDir_t;

	typedef struct
	{
		moveStage_t			stage;
		int					acceleration;
		int					movetime;
		int					deceleration;
		idVec3				dir;
	} moveState_t;

	typedef struct
	{
		moveStage_t			stage;
		int					acceleration;
		int					movetime;
		int					deceleration;
		idAngles			rot;
	} rotationState_t;

	idPhysics_Parametric	physicsObj;

	void					Event_OpenPortal();
	void					Event_ClosePortal();
	void					Event_PartBlocked( idEntity* blockingEntity );

	void					MoveToPos( const idVec3& pos );
	void					UpdateMoveSound( moveStage_t stage );
	void					UpdateRotationSound( moveStage_t stage );
	void					SetGuiStates( const char* state );
	void					FindGuiTargets();
	void					SetGuiState( const char* key, const char* val ) const;

	virtual void			DoneMoving();
	virtual void			DoneRotating();
	virtual void			BeginMove( idThread* thread = NULL );
	virtual void			BeginRotation( idThread* thread, bool stopwhendone );
	moveState_t				move;

private:
	rotationState_t			rot;

	int						move_thread;
	int						rotate_thread;
	idAngles				dest_angles;
	idAngles				angle_delta;
	idVec3					dest_position;
	idVec3					move_delta;
	float					move_speed;
	int						move_time;
	int						deceltime;
	int						acceltime;
	bool					stopRotation;
	bool					useSplineAngles;
	idEntityPtr<idEntity>	splineEnt;
	moverCommand_t			lastCommand;
	float					damage;

	qhandle_t				areaPortal;		// 0 = no portal

	idList< idEntityPtr<idEntity>, TAG_MOVER >	guiTargets;

	void					VectorForDir( float dir, idVec3& vec );
	idCurve_Spline<idVec3>* GetSpline( idEntity* splineEntity ) const;

	void					Event_SetCallback();
	void					Event_TeamBlocked( idEntity* blockedPart, idEntity* blockingEntity );
	void					Event_StopMoving();
	void					Event_StopRotating();
	void					Event_UpdateMove();
	void					Event_UpdateRotation();
	void					Event_SetMoveSpeed( float speed );
	void					Event_SetMoveTime( float time );
	void					Event_SetDecelerationTime( float time );
	void					Event_SetAccellerationTime( float time );
	void					Event_MoveTo( idEntity* ent );
	void					Event_MoveToPos( idVec3& pos );
	void					Event_MoveDir( float angle, float distance );
	void					Event_MoveAccelerateTo( float speed, float time );
	void					Event_MoveDecelerateTo( float speed, float time );
	void					Event_RotateDownTo( int axis, float angle );
	void					Event_RotateUpTo( int axis, float angle );
	void					Event_RotateTo( idAngles& angles );
	void					Event_Rotate( idAngles& angles );
	void					Event_RotateOnce( idAngles& angles );
	void					Event_Bob( float speed, float phase, idVec3& depth );
	void					Event_Sway( float speed, float phase, idAngles& depth );
	void					Event_SetAccelSound( const char* sound );
	void					Event_SetDecelSound( const char* sound );
	void					Event_SetMoveSound( const char* sound );
	void					Event_FindGuiTargets();
	void					Event_InitGuiTargets();
	void					Event_EnableSplineAngles();
	void					Event_DisableSplineAngles();
	void					Event_RemoveInitialSplineAngles();
	void					Event_StartSpline( idEntity* splineEntity );
	void					Event_StopSpline();
	void					Event_Activate( idEntity* activator );
	void					Event_PostRestore( int start, int total, int accel, int decel, int useSplineAng );
	void					Event_IsMoving();
	void					Event_IsRotating();
};

class idSplinePath : public idEntity
{
public:
	CLASS_PROTOTYPE( idSplinePath );

	idSplinePath();

	void					Spawn();
};


struct floorInfo_s
{
	idVec3					pos;
	idStr					door;
	int						floor;
};

//puncho - P2b.1.2: idElevator's map record section (Record.h, RECORD_SECTION_ELEVATOR): the floor it is at, HAND in
// Mover.cpp; the elevator's Pose is this row's (SkipPose). P2b.2.5a: the class's one list (C6), the record row first, so
// RF_Floor stays 0; then the spawn rows (K2), Spawn only, which Spawn reads through one SCHEMA_SPAWN
#define ELEVATOR_SCHEMA( X, s ) \
	X( s, Floor,		HAND,	R,		RAW,	1,	BUILD_NUMBER_SAVE_VERSION_RECORD_SCHEMA,	NOKEY,	NODEF,	NOEDIT,	OBJ ) \
	X( s, pendingFloor,	INT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"floor",		"1",	"Floor number to start on, 1 through the number of floors.",	OBJ ) \
	X( s, returnTime,	FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"returnTime",	"0",	"Seconds after arriving at another floor before the elevator goes back to returnFloor; 0 stays.",	OBJ ) \
	X( s, returnFloor,	INT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"returnFloor",	"0",	"Floor the elevator goes back to after returnTime.",	OBJ )

class idElevator : public idMover
{
public:
	CLASS_PROTOTYPE( idElevator );

	idElevator();
	//puncho start - P2b.1.2: the map record
	enum
	{
		ELEVATOR_SCHEMA( SCHEMA_RECORD_INDEX, _ )
		ELEVATOR_RECORD_FIELDS
	};
	compile_time_assert( ELEVATOR_RECORD_FIELDS <= 64 );
	virtual void			WriteRecord( idRecordOut& rec, const recordFrame_t& frame ) const;
	virtual recordApply_t	ApplyRecord( idRecordIn& rec, const recordFrame_t& frame );
	static bool				CheckRecordSection( idRecordIn& rec );
	bool					RecordFloorChanged( const idRecordOut& rec ) const;
	void					WriteRecordFloor( idRecordOut& rec ) const;
	bool					ApplyRecordFloor( idRecordIn& rec );
	static bool				CheckRecordFloor( idRecordIn& rec );
	//puncho end

	void					Spawn();

	void					Save( idSaveGame* savefile ) const;
	void					Restore( idRestoreGame* savefile );

	virtual bool			HandleSingleGuiCommand( idEntity* entityGui, idLexer* src );
	void					Event_GotoFloor( int floor );
	floorInfo_s* 			GetFloorInfo( int floor );
	int						RecordFloor() const	//puncho - P2b.1.0: dumpMapState's floor column
	{
		return currentFloor;
	}

protected:
	virtual void			DoneMoving();
	virtual void			BeginMove( idThread* thread = NULL );
	void					SpawnTrigger( const idVec3& pos );
	void					GetLocalTriggerPosition();
	void					Event_Touch( idEntity* other, trace_t* trace );
	void					BindDoors();	//puncho - P2b.1.2: Think's first pass, shared with ApplyRecordFloor

private:
	typedef enum
	{
		INIT,
		IDLE,
		WAITING_ON_DOORS
	} elevatorState_t;

	elevatorState_t			state;
	idList<floorInfo_s, TAG_MOVER>		floorInfo;
	int						currentFloor;
	int						pendingFloor;
	int						lastFloor;
	bool					controlsDisabled;
	float					returnTime;
	int						returnFloor;
	int						lastTouchTime;

	class idDoor* 			GetDoor( const char* name );
	void					Think();
	void					OpenInnerDoor();
	void					OpenFloorDoor( int floor );
	void					CloseAllDoors();
	void					DisableAllDoors();
	void					EnableProperDoors();

	void					Event_TeamBlocked( idEntity* blockedEntity, idEntity* blockingEntity );
	void					Event_Activate( idEntity* activator );
	void					Event_PostFloorArrival();

	void					Event_SetGuiStates();

};


/*
===============================================================================

  Binary movers.

===============================================================================
*/

typedef enum
{
	MOVER_POS1,
	MOVER_POS2,
	MOVER_1TO2,
	MOVER_2TO1
} moverState_t;

//puncho - P2b.1.2: idMover_Binary's map record section (Record.h, RECORD_SECTION_MOVER_BINARY): the position and the
// lock, HAND in Mover.cpp; a binary mover's Pose is this row's (SkipPose). P2b.2.5a: the class's one list (C6), the record
// row first, so RF_MoverState stays 0; then the spawn rows (K2), Spawn only: the keyed members Spawn reads through one
// SCHEMA_SPAWN, and KEYONLY the keys it reads by hand (team into a local, solid and nopush tested in place)
#define MOVER_BINARY_SCHEMA( X, s ) \
	X( s, MoverState,	HAND,	R,		RAW,	2,	BUILD_NUMBER_SAVE_VERSION_RECORD_SCHEMA,	NOKEY,	NODEF,	NOEDIT,	OBJ ) \
	X( s, wait,			FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"wait",			"0",	"Wait before returning (-1 = never return).",	OBJ ) \
	X( s, updateStatus,	INT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"updateStatus",	"0",	"Set to 2 to set shaderparm 7 of the buddy entities to 1 when the mover reaches its second position and 0 at its first.",	OBJ ) \
	X( s, team,			STRING,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"team",			"",		"Binary movers with the same team name move together; the first one spawned is the master.",	KEYONLY ) \
	X( s, solid,		BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"solid",		"1",	"Whether the mover is solid to other entities.",	KEYONLY ) \
	X( s, nopush,		BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"nopush",		"0",	"Set to 1 to disable pushing objects that block the mover.",	KEYONLY )

class idMover_Binary : public idEntity
{
public:
	CLASS_PROTOTYPE( idMover_Binary );

	bool RecordAtPosition2() const;
	void ApplyRecordState( bool spawnPosition2, bool locked );
	void CarryRecordPositions( const idVec3& fromOrigin, const idMat3& rotation, const idVec3& toOrigin, bool rotate );	//puncho - P2b.1.2 (review)
	//puncho start - P2b.1.2: the map record
	enum
	{
		MOVER_BINARY_SCHEMA( SCHEMA_RECORD_INDEX, _ )
		MOVER_BINARY_RECORD_FIELDS
	};
	compile_time_assert( MOVER_BINARY_RECORD_FIELDS <= 64 );
	virtual void			WriteRecord( idRecordOut& rec, const recordFrame_t& frame ) const;
	virtual recordApply_t	ApplyRecord( idRecordIn& rec, const recordFrame_t& frame );
	static bool				CheckRecordSection( idRecordIn& rec );
	bool					RecordMoverStateChanged( const idRecordOut& rec ) const;
	void					WriteRecordMoverState( idRecordOut& rec ) const;
	bool					ApplyRecordMoverState( idRecordIn& rec );
	static bool				CheckRecordMoverState( idRecordIn& rec );
	//puncho end
	moverState_t RecordMoverState() const	//puncho - P2b.1.0: dumpMapState's moverPos column, the moving states included
	{
		return moverState;
	}
	idMover_Binary();
	~idMover_Binary();

	void					Spawn();

	void					Save( idSaveGame* savefile ) const;
	void					Restore( idRestoreGame* savefile );

	virtual void			PreBind();
	virtual void			PostBind();

	void					Enable( bool b );
	void					InitSpeed( idVec3& mpos1, idVec3& mpos2, float mspeed, float maccelTime, float mdecelTime );
	void					InitTime( idVec3& mpos1, idVec3& mpos2, float mtime, float maccelTime, float mdecelTime );
	void					GotoPosition1();
	void					GotoPosition2();
	void					Use_BinaryMover( idEntity* activator );
	void					SetGuiStates( const char* state );
	void					UpdateBuddies( int val );
	idMover_Binary* 		GetActivateChain() const
	{
		return activateChain;
	}
	idMover_Binary* 		GetMoveMaster() const
	{
		return moveMaster;
	}
	void					BindTeam( idEntity* bindTo );
	void					SetBlocked( bool b );
	bool					IsBlocked();
	idEntity* 				GetActivator() const;

	virtual void			WriteToSnapshot( idBitMsg& msg ) const;
	virtual void			ReadFromSnapshot( const idBitMsg& msg );

	void					SetPortalState( bool open );

protected:
	idVec3					pos1;
	idVec3					pos2;
	moverState_t			moverState;
	idMover_Binary* 		moveMaster;
	idMover_Binary* 		activateChain;
	int						soundPos1;
	int						sound1to2;
	int						sound2to1;
	int						soundPos2;
	int						soundLoop;
	float					wait;
	float					damage;
	int						duration;
	int						accelTime;
	int						decelTime;
	idEntityPtr<idEntity>	activatedBy;
	int						stateStartTime;
	idStr					team;
	bool					enabled;
	int						move_thread;
	int						updateStatus;		// 1 = lock behaviour, 2 = open close status
	idStrList				buddies;
	idPhysics_Parametric	physicsObj;
	qhandle_t				areaPortal;			// 0 = no portal
	bool					blocked;
	bool					playerOnly;
	idList< idEntityPtr<idEntity>, TAG_MOVER >	guiTargets;

	void					MatchActivateTeam( moverState_t newstate, int time );
	void					JoinActivateTeam( idMover_Binary* master );

	void					UpdateMoverSound( moverState_t state );
	void					SetMoverState( moverState_t newstate, int time );
	moverState_t			GetMoverState() const
	{
		return moverState;
	}
	void					FindGuiTargets();
	void					SetGuiState( const char* key, const char* val ) const;

	void					Event_SetCallback();
	void					Event_ReturnToPos1();
	void					Event_Use_BinaryMover( idEntity* activator );
	void					Event_Reached_BinaryMover();
	void					Event_MatchActivateTeam( moverState_t newstate, int time );
	void					Event_Enable();
	void					Event_Disable();
	void					Event_OpenPortal();
	void					Event_ClosePortal();
	void					Event_FindGuiTargets();
	void					Event_InitGuiTargets();

	static void				GetMovedir( float dir, idVec3& movedir );
};

//puncho - P2b.2.5a: idDoor's spawn rows (K2), Spawn only: the keyed members Spawn reads through one SCHEMA_SPAWN (wait again
// over idMover_Binary's read, with the door's default), and KEYONLY the keys it reads by hand (into locals, tested in place
// or read late)
#define DOOR_SCHEMA( X, s ) \
	X( s, wait,			FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"wait",			"3",	"Wait before returning (-1 = never return).",	OBJ ) \
	X( s, damage,		FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"damage",		"0",	"Damage to inflict on entities that block the door.",	OBJ ) \
	X( s, triggersize,	FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"triggersize",	"120",	"Sets the amount the trigger extends from the door.",	OBJ ) \
	X( s, crusher,		BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"crusher",		"0",	"The door does not reverse when blocked.",	OBJ ) \
	X( s, noTouch,		BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"no_touch",		"0",	"The door should not be triggered by the player touching it, only by another trigger; in multiplayer this door can't be shot to open.",	OBJ ) \
	X( s, playerOnly,	BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"player_only",	"0",	"Only a player opens the door; while it is closed, monsters route as if it were a wall.",	OBJ ) \
	X( s, syncLock,		STRING,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"syncLock",		"",		"Name of a door that must be closed before this door will open.",	OBJ ) \
	X( s, buddyStr,		STRING,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"buddy",		"",		"Will toggle shaderparm 7 of the entity given as a buddy; this allows shaders to be remapped for lock status etc.",	OBJ ) \
	X( s, requires,		STRING,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"requires",		"",		"Inventory item the activator must hold for the door to open.",	OBJ ) \
	X( s, removeItem,	INT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"removeItem",	"0",	"Set to 1 to take the required item from the activator.",	OBJ ) \
	X( s, movedir,		FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"movedir",		NODEF,	"Determines the opening direction; if set, angle determines orientation.",	KEYONLY ) \
	X( s, angle,		FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"angle",		"0",	"Without movedir, determines the opening direction.",	KEYONLY ) \
	X( s, speed,		FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"speed",		"400",	"Movement speed.",	KEYONLY ) \
	X( s, lip,			FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"lip",			"8",	"Lip remaining at end of move.",	KEYONLY ) \
	X( s, start_open,	BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"start_open",	"0",	"The door moves to its destination when spawned, and operates in reverse.",	KEYONLY ) \
	X( s, time,			FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"time",			NODEF,	"Movement time in seconds; overrides speed. Used for doors that move different distances but still need to be synced.",	KEYONLY ) \
	X( s, snd_locked,	STRING,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"snd_locked",	"",		"Sound to play if the door is locked and a player approaches.",	KEYONLY ) \
	X( s, locked,		INT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"locked",		"0",	"With no_touch, the door must be triggered first: 1 then opens it, 2 only unlocks it.",	KEYONLY ) \
	X( s, continuous,	BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"continuous",	"0",	"The door triggers itself at spawn and after each wait, so it keeps opening and closing.",	KEYONLY ) \
	X( s, delay,		FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"delay",		"0",	"Seconds after spawn before a continuous door first triggers itself.",	KEYONLY )

class idDoor : public idMover_Binary
{
public:
	CLASS_PROTOTYPE( idDoor );

	idDoor();
	~idDoor();

	void					Spawn();

	void					Save( idSaveGame* savefile ) const;
	void					Restore( idRestoreGame* savefile );

	virtual void			ClientThink( const int curTime, const float fraction, const bool predict );
	virtual void			Think();
	virtual void			PreBind();
	virtual void			PostBind();
	virtual void			Hide();
	virtual void			Show();

	bool					IsOpen();
	bool					IsNoTouch();
	bool					AllowPlayerOnly( idEntity* ent );
	void ApplyRecordLock( bool locked );
	//puncho start - P2b.1.2 (review): whether pos1 and pos2 are a start_open door's swapped pair, the one source of the
	// swap for RecordAtPosition2 and ApplyRecordState; transient, not saved (Restore derives it)
	bool					StartOpenSwapped() const
	{
		return startOpenSwapped;
	}
	bool					ApplyRecordStartOpen();
	//puncho end
	int						IsLocked();
	void					Lock( int f );
	void					Use( idEntity* other, idEntity* activator );
	void					Close();
	void					Open();
	void					SetCompanion( idDoor* door );

private:
	float					triggersize;
	bool					crusher;
	bool					noTouch;
	bool					aas_area_closed;
	idStr					buddyStr;
	idClipModel* 			trigger;
	idClipModel* 			sndTrigger;
	int						nextSndTriggerTime;
	idVec3					localTriggerOrigin;
	idMat3					localTriggerAxis;
	idStr					requires;
	int						removeItem;
	idStr					syncLock;
	int						normalAxisIndex;		// door faces X or Y for spectator teleports
	idDoor* 				companionDoor;
	bool					startOpenSwapped;		//puncho - P2b.1.2 (review): Event_StartOpen or the record's apply swapped pos1 and pos2

	void					SetAASAreaState( bool closed );

	void					GetLocalTriggerPosition( const idClipModel* trigger );
	void					CalcTriggerBounds( float size, idBounds& bounds );

	void					Event_Reached_BinaryMover();
	void					Event_TeamBlocked( idEntity* blockedEntity, idEntity* blockingEntity );
	void					Event_PartBlocked( idEntity* blockingEntity );
	void					Event_Touch( idEntity* other, trace_t* trace );
	void					Event_Activate( idEntity* activator );
	void					Event_StartOpen();
	void					Event_SpawnDoorTrigger();
	void					Event_SpawnSoundTrigger();
	void					Event_Close();
	void					Event_Open();
	void					Event_Lock( int f );
	void					Event_IsOpen();
	void					Event_Locked();
	void					Event_SpectatorTouch( idEntity* other, trace_t* trace );
	void					Event_OpenPortal();
	void					Event_ClosePortal();
};

//puncho - P2b.2.5a: idPlat's spawn rows (K2), Spawn only: the keyed members Spawn reads through one SCHEMA_SPAWN (wait again
// over idMover_Binary's read, with the plat's default), and KEYONLY the keys it reads by hand into locals
#define PLAT_SCHEMA( X, s ) \
	X( s, damage,		FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"damage",		"0",	"Damage to inflict on entities that block the plat.",	OBJ ) \
	X( s, wait,			FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"wait",			"1",	"Wait before returning (-1 = never return).",	OBJ ) \
	X( s, speed,		FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"speed",		"100",	"Speed to move up and down at.",	KEYONLY ) \
	X( s, lip,			FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"lip",			"8",	"Protrusion above rest position.",	KEYONLY ) \
	X( s, accel_time,	FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"accel_time",	"0.25",	"Seconds to accelerate to full speed.",	KEYONLY ) \
	X( s, decel_time,	FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"decel_time",	"0.25",	"Seconds to decelerate to a stop.",	KEYONLY ) \
	X( s, height,		FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"height",		NODEF,	"Total height of movement; without it the model's height less lip.",	KEYONLY ) \
	X( s, no_touch,		BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"no_touch",		"0",	"The plat should not be triggered by the player touching it, only by another trigger.",	KEYONLY ) \
	X( s, time,			FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"time",			NODEF,	"Movement time in seconds; overrides speed.",	KEYONLY )

class idPlat : public idMover_Binary
{
public:
	CLASS_PROTOTYPE( idPlat );

	idPlat();
	~idPlat();

	void					Spawn();

	void					Save( idSaveGame* savefile ) const;
	void					Restore( idRestoreGame* savefile );
	void					RunPhysics_NoBlocking();
	virtual void			ClientThink( const int curTime, const float fraction, const bool predict );
	virtual void			Think();
	virtual void			PreBind();
	virtual void			PostBind();

private:
	idClipModel* 			trigger;
	idVec3					localTriggerOrigin;
	idMat3					localTriggerAxis;

	void					GetLocalTriggerPosition( const idClipModel* trigger );
	void					SpawnPlatTrigger( idVec3& pos );

	void					Event_TeamBlocked( idEntity* blockedEntity, idEntity* blockingEntity );
	void					Event_PartBlocked( idEntity* blockingEntity );
	void					Event_Touch( idEntity* other, trace_t* trace );
};


/*
===============================================================================

  Special periodic movers.

===============================================================================
*/

//puncho - P2b.2.5a: the periodic movers' spawn rows (K2), Spawn only: idMover_Periodic reads damage through one
// SCHEMA_SPAWN; the KEYONLY rows are the keys each Spawn reads by hand (into locals or tested in place)
#define MOVER_PERIODIC_SCHEMA( X, s ) \
	X( s, damage,		FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"damage",		"0",	"Damage to inflict on entities that block the mover.",	OBJ ) \
	X( s, solid,		BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"solid",		"1",	"Whether the mover is solid to other entities.",	KEYONLY )

#define ROTATER_SCHEMA( X, s ) \
	X( s, nopush,		BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"nopush",		"0",	"Set to 1 to disable pushing objects that block the mover.",	KEYONLY ) \
	X( s, start_on,		BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"start_on",		"0",	"Set to 1 to rotate when spawned.",	KEYONLY )

#define BOBBER_SCHEMA( X, s ) \
	X( s, speed,		FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"speed",		"4",	"Seconds to complete a bob cycle.",	KEYONLY ) \
	X( s, height,		FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"height",		"32",	"Amplitude of bob.",	KEYONLY ) \
	X( s, phase,		FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"phase",		"0",	"The 0.0 to 1.0 offset in the cycle to start at.",	KEYONLY ) \
	X( s, x_axis,		BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"x_axis",		"0",	"Set to 1 to bob along the x-axis.",	KEYONLY ) \
	X( s, y_axis,		BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"y_axis",		"0",	"Set to 1 to bob along the y-axis.",	KEYONLY ) \
	X( s, nopush,		BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"nopush",		"0",	"Set to 1 to disable pushing objects that block the mover.",	KEYONLY )

#define PENDULUM_SCHEMA( X, s ) \
	X( s, speed,		FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"speed",		"30",	"The number of degrees each way the pendulum swings.",	KEYONLY ) \
	X( s, phase,		FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"phase",		"0",	"The 0.0 to 1.0 offset in the cycle to start at.",	KEYONLY ) \
	X( s, freq,			FLOAT,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"freq",			NODEF,	"If set, overrides the frequency from the beam length and gravity.",	KEYONLY ) \
	X( s, nopush,		BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"nopush",		"0",	"Set to 1 to disable pushing objects that block the mover.",	KEYONLY )

#define RISER_SCHEMA( X, s ) \
	X( s, solid,		BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"solid",		"1",	"Whether the mover is solid to other entities.",	KEYONLY ) \
	X( s, nopush,		BOOL,	NOCH,	RAW,	0,	SCHEMA_STOCK,	"nopush",		"0",	"Set to 1 to disable pushing objects that block the mover.",	KEYONLY )

class idMover_Periodic : public idEntity
{
public:
	CLASS_PROTOTYPE( idMover_Periodic );

	idMover_Periodic();

	void					Spawn();

	void					Save( idSaveGame* savefile ) const;
	void					Restore( idRestoreGame* savefile );

	virtual void			Think();

	virtual void			WriteToSnapshot( idBitMsg& msg ) const;
	virtual void			ReadFromSnapshot( const idBitMsg& msg );
	virtual void			WriteRecord( idRecordOut& rec, const recordFrame_t& frame ) const;	//puncho - P2b.1.2: no Pose

protected:
	idPhysics_Parametric	physicsObj;
	float					damage;

	void					Event_TeamBlocked( idEntity* blockedEntity, idEntity* blockingEntity );
	void					Event_PartBlocked( idEntity* blockingEntity );
};

class idRotater : public idMover_Periodic
{
public:
	CLASS_PROTOTYPE( idRotater );

	idRotater();

	void					Spawn();

	void					Save( idSaveGame* savefile ) const;
	void					Restore( idRestoreGame* savefile );

private:
	idEntityPtr<idEntity>	activatedBy;

	void					Event_Activate( idEntity* activator );
};

class idBobber : public idMover_Periodic
{
public:
	CLASS_PROTOTYPE( idBobber );

	idBobber();

	void					Spawn();

private:
};

class idPendulum : public idMover_Periodic
{
public:
	CLASS_PROTOTYPE( idPendulum );

	idPendulum();

	void					Spawn();

private:
};

class idRiser : public idMover_Periodic
{
public:
	CLASS_PROTOTYPE( idRiser );

	idRiser();

	void					Spawn();

private:
	void					Event_Activate( idEntity* activator );
};

#endif /* !__GAME_MOVER_H__ */
