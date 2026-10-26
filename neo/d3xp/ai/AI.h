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

#ifndef __AI_H__
#define __AI_H__

/*
===============================================================================

	idAI

===============================================================================
*/

const float	SQUARE_ROOT_OF_2			= 1.414213562f;
const float	AI_TURN_PREDICTION			= 0.2f;
const float	AI_TURN_SCALE				= 60.0f;
const float	AI_SEEK_PREDICTION			= 0.3f;
const float	AI_FLY_DAMPENING			= 0.15f;
const float	AI_HEARING_RANGE			= 2048.0f;
const int	DEFAULT_FLY_OFFSET			= 68;

#define ATTACK_IGNORE			0
#define ATTACK_ON_DAMAGE		1
#define ATTACK_ON_ACTIVATE		2
#define ATTACK_ON_SIGHT			4

typedef struct ballistics_s
{
	float				angle;		// angle in degrees in the range [-180, 180]
	float				time;		// time it takes before the projectile arrives
} ballistics_t;

extern int Ballistics( const idVec3& start, const idVec3& end, float speed, float gravity, ballistics_t bal[2] );

// defined in script/ai_base.script.  please keep them up to date.
typedef enum
{
	MOVETYPE_DEAD,
	MOVETYPE_ANIM,
	MOVETYPE_SLIDE,
	MOVETYPE_FLY,
	MOVETYPE_STATIC,
	NUM_MOVETYPES
} moveType_t;

typedef enum
{
	MOVE_NONE,
	MOVE_FACE_ENEMY,
	MOVE_FACE_ENTITY,

	// commands < NUM_NONMOVING_COMMANDS don't cause a change in position
	NUM_NONMOVING_COMMANDS,

	MOVE_TO_ENEMY = NUM_NONMOVING_COMMANDS,
	MOVE_TO_ENEMYHEIGHT,
	MOVE_TO_ENTITY,
	MOVE_OUT_OF_RANGE,
	MOVE_TO_ATTACK_POSITION,
	MOVE_TO_COVER,
	MOVE_TO_POSITION,
	MOVE_TO_POSITION_DIRECT,
	MOVE_SLIDE_TO_POSITION,
	MOVE_WANDER,
	NUM_MOVE_COMMANDS
} moveCommand_t;

typedef enum
{
	TALK_NEVER,
	TALK_DEAD,
	TALK_OK,
	TALK_BUSY,
	NUM_TALK_STATES
} talkState_t;

//
// status results from move commands
// make sure to change script/doom_defs.script if you add any, or change their order
//
typedef enum
{
	MOVE_STATUS_DONE,
	MOVE_STATUS_MOVING,
	MOVE_STATUS_WAITING,
	MOVE_STATUS_DEST_NOT_FOUND,
	MOVE_STATUS_DEST_UNREACHABLE,
	MOVE_STATUS_BLOCKED_BY_WALL,
	MOVE_STATUS_BLOCKED_BY_OBJECT,
	MOVE_STATUS_BLOCKED_BY_ENEMY,
	MOVE_STATUS_BLOCKED_BY_MONSTER
} moveStatus_t;

#define	DI_NODIR	-1

//puncho start - ai-avoidance (plans/ai-avoidance/00-PLAN.md §3.1, WP-A1): the tier of a body against every player
typedef enum
{
	AVOID_TIER_CLOSE,			// flat distance < 256 of any player
	AVOID_TIER_SEEN,			// in the merged player PVS and < 1024
	AVOID_TIER_UNSEEN			// otherwise
} avoidTier_t;

// (§3.6, WP-A6/A7): the role byte, from the def's minion_class or the ai_squad marker; a follower walks the flow field
typedef enum
{
	MINION_ROLE_ELITE,			// permanent minions, bosses: the stock path and planner
	MINION_ROLE_LEADER,			// a squad leader: as elite
	MINION_ROLE_FOLLOWER		// trash, squad members: the flow field toward the squad's goal, separation, the planner only when stuck
} minionRole_t;

// the avoidance class a squad marker or sys.aiSetAvoid forces; BY_ROLE lets the role decide
typedef enum
{
	AVOID_CLASS_BY_ROLE,
	AVOID_CLASS_FULL,			// the stock planner whatever the role
	AVOID_CLASS_LIGHT,			// the follower rules whatever the role
	AVOID_CLASS_OFF				// ignore_obstacles
} avoidClass_t;
//puncho end

// obstacle avoidance
typedef struct obstaclePath_s
{
	idVec3				seekPos;					// seek position avoiding obstacles
	idEntity* 			firstObstacle;				// if != NULL the first obstacle along the path
	idVec3				startPosOutsideObstacles;	// start position outside obstacles
	idEntity* 			startPosObstacle;			// if != NULL the obstacle containing the start position
	idVec3				seekPosOutsideObstacles;	// seek position outside obstacles
	idEntity* 			seekPosObstacle;			// if != NULL the obstacle containing the seek position
} obstaclePath_t;

// path prediction
typedef enum
{
	SE_BLOCKED			= BIT( 0 ),
	SE_ENTER_LEDGE_AREA	= BIT( 1 ),
	SE_ENTER_OBSTACLE	= BIT( 2 ),
	SE_FALL				= BIT( 3 ),
	SE_LAND				= BIT( 4 )
} stopEvent_t;

typedef struct predictedPath_s
{
	idVec3				endPos;						// final position
	idVec3				endVelocity;				// velocity at end position
	idVec3				endNormal;					// normal of blocking surface
	int					endTime;					// time predicted
	int					endEvent;					// event that stopped the prediction
	const idEntity* 	blockingEntity;				// entity that blocks the movement
} predictedPath_t;

//
// events
//
extern const idEventDef AI_BeginAttack;
extern const idEventDef AI_EndAttack;
extern const idEventDef AI_MuzzleFlash;
extern const idEventDef AI_CreateMissile;
extern const idEventDef AI_AttackMissile;
extern const idEventDef AI_FireMissileAtTarget;
extern const idEventDef AI_LaunchProjectile;
extern const idEventDef AI_TriggerFX;
extern const idEventDef AI_StartEmitter;
extern const idEventDef AI_StopEmitter;
extern const idEventDef AI_AttackMelee;
extern const idEventDef AI_DirectDamage;
extern const idEventDef AI_JumpFrame;
extern const idEventDef AI_EnableClip;
extern const idEventDef AI_DisableClip;
extern const idEventDef AI_EnableGravity;
extern const idEventDef AI_DisableGravity;
extern const idEventDef AI_TriggerParticles;
extern const idEventDef AI_RandomPath;
extern const idEventDef AI_MoveToPosition;		//puncho - ai-harness: external linkage for ai_moveMinions (SysCmds.cpp)

class idPathCorner;

typedef struct particleEmitter_s
{
	particleEmitter_s()
	{
		particle = NULL;
		time = 0;
		joint = INVALID_JOINT;
	};
	const idDeclParticle* particle;
	int					time;
	jointHandle_t		joint;
} particleEmitter_t;

typedef struct funcEmitter_s
{
	char				name[64];
	idFuncEmitter*		particle;
	jointHandle_t		joint;
} funcEmitter_t;

class idMoveState
{
public:
	idMoveState();

	void					Save( idSaveGame* savefile ) const;
	void					Restore( idRestoreGame* savefile );

	moveType_t				moveType;
	moveCommand_t			moveCommand;
	moveStatus_t			moveStatus;
	idVec3					moveDest;
	idVec3					moveDir;			// used for wandering and slide moves
	idEntityPtr<idEntity>	goalEntity;
	idVec3					goalEntityOrigin;	// move to entity uses this to avoid checking the floor position every frame
	int						toAreaNum;
	int						startTime;
	int						duration;
	float					speed;				// only used by flying creatures
	float					range;
	float					wanderYaw;
	int						nextWanderTime;
	int						blockTime;
	idEntityPtr<idEntity>	obstacle;
	idVec3					lastMoveOrigin;
	int						lastMoveTime;
	int						anim;
};

class idAASFindCover : public idAASCallback
{
public:
	idAASFindCover( const idVec3& hideFromPos );
	~idAASFindCover();

	virtual bool		TestArea( const idAAS* aas, int areaNum );

private:
	pvsHandle_t			hidePVS;
	int					PVSAreas[ idEntity::MAX_PVS_AREAS ];
};

class idAASFindAreaOutOfRange : public idAASCallback
{
public:
	idAASFindAreaOutOfRange( const idVec3& targetPos, float maxDist );

	virtual bool		TestArea( const idAAS* aas, int areaNum );

private:
	idVec3				targetPos;
	float				maxDistSqr;
};

class idAASFindAttackPosition : public idAASCallback
{
public:
	idAASFindAttackPosition( const idAI* self, const idMat3& gravityAxis, idEntity* target, const idVec3& targetPos, const idVec3& fireOffset );
	~idAASFindAttackPosition();

	virtual bool		TestArea( const idAAS* aas, int areaNum );

private:
	const idAI*			self;
	idEntity*			target;
	idBounds			excludeBounds;
	idVec3				targetPos;
	idVec3				fireOffset;
	idMat3				gravityAxis;
	pvsHandle_t			targetPVS;
	int					PVSAreas[ idEntity::MAX_PVS_AREAS ];
};

class idAI : public idActor
{
public:
	CLASS_PROTOTYPE( idAI );

	idAI();
	~idAI();

	//puncho start - ai-harness (plans/ai-harness/00-PLAN.md §2.2): this AI's share of the last tic's census, the
	// difference of gameLocal.aiStats around Think. Host-side debug state: not saved, not in Save/Restore order
	aiTicStats_t			lastThinkStats;
	//puncho end

	void					Save( idSaveGame* savefile ) const;
	void					Restore( idRestoreGame* savefile );

	void					Spawn();
	void					HeardSound( idEntity* ent, const char* action );
	virtual float			DormantRadiusScale() const;
	virtual void			DormantCatchUp( int elapsedMs );
	idActor*					GetEnemy() const;
	void					TalkTo( idActor* actor );
	talkState_t				GetTalkState() const;

	//puncho start - ai-combat (plans/ai-combat/07-DESIGN-c8-levers.md §B): clipMask is MASK_SHOT_RENDERMODEL for every
	// launch and the test mask for canHitEnemyFromAnim, so the test can trace boxes while the shot aims at meshes
	bool					GetAimDir( const idVec3& firePos, idEntity* aimAtEnt, const idEntity* ignore, int clipMask, idVec3& aimDir ) const;
	//puncho end

	void					TouchedByFlashlight( idActor* flashlight_owner );

	// Outputs a list of all monsters to the console.
	static void				List_f( const idCmdArgs& args );

	//puncho start - ai-avoidance (WP-A1): the tier of this tic, AVOID_TIER_CLOSE..UNSEEN, for the rate classes that key off it
	int						GetAvoidTier();
	//puncho start - ai-combat (plans/ai-combat/08-PLAN-shooter-tokens.md §3.3, WP-T3)
	// once per tic after the think loop: the shooter tokens the next tic's canHitEnemyFromAnim calls read (AI_crowd.cpp);
	// static so it reads every monster's protected state
	static void				AssignShooterTokens();
	//puncho end
	// WP-A5: a monster without a move order holds its ground: infinite mass to the separation of the ones walking into it
	bool					IsHoldingPosition() const
	{
		return move.moveCommand < NUM_NONMOVING_COMMANDS || !allowMove;
	}
	//puncho end

	// Finds a path around dynamic obstacles.
	static bool				FindPathAroundObstacles( const idPhysics* physics, const idAAS* aas, const idEntity* ignore, const idVec3& startPos, const idVec3& seekPos, obstaclePath_t& path );
	// Frees any nodes used for the dynamic obstacle avoidance.
	static void				FreeObstacleAvoidanceNodes();
	// Predicts movement, returns true if a stop event was triggered.
	static bool				PredictPath( const idEntity* ent, const idAAS* aas, const idVec3& start, const idVec3& velocity, int totalTime, int frameTime, int stopEvent, predictedPath_t& path );
	// Return true if the trajectory of the clip model is collision free.
	static bool				TestTrajectory( const idVec3& start, const idVec3& end, float zVel, float gravity, float time, float max_height, const idClipModel* clip, int clipmask, const idEntity* ignore, const idEntity* targetEntity, int drawtime );
	// Finds the best collision free trajectory for a clip model.
	static bool				PredictTrajectory( const idVec3& firePos, const idVec3& target, float projectileSpeed, const idVec3& projGravity, const idClipModel* clip, int clipmask, float max_height, const idEntity* ignore, const idEntity* targetEntity, int drawtime, idVec3& aimDir );

	virtual void			Gib( const idVec3& dir, const char* damageDefName );

protected:
	// navigation
	idAAS* 					aas;
	int						travelFlags;

	idMoveState				move;
	idMoveState				savedMove;

	float					kickForce;
	bool					ignore_obstacles;
	float					blockedRadius;
	int						blockedMoveTime;
	int						blockedAttackTime;
	//puncho start - ai-avoidance (plans/ai-avoidance/00-PLAN.md): the cached avoidance plan that ai_avoidPeriod and
	// ai_avoidTiers reuse between full plans. Host-side, rebuilt by the next plan: not saved, not in Save/Restore order
	int						avoidPlanTic;		// gameLocal.framenum of the last full plan, -1 none
	int						avoidMoveStart;		// move.startTime at that plan: a new order plans at once
	idVec3					avoidDir;			// flat unit direction that plan steered along
	bool					avoidDetour;		// that plan steered off the line to the goal (false: the goal itself is the answer)
	bool					avoidBlocked;		// that plan, or the physics step after it, reported blocked: plan again next tic
	bool					avoidPlanWasBlocked;	// the previous plan was blocked too (ai_avoidBlockedReplan 1 re-plans on the edge only)
	bool					avoidStepWasBlocked;	// the previous physics step was MM_BLOCKED too: same edge rule
	// WP-A1: the tier scheduler. One tier per body per tic, the source of every per-body rate (the avoidance period here,
	// the brain deadline and the snapshot class when those land); a skipped tic holds the whole last plan, its move status
	// included, so a script polling moveStatus() sees what the plan tic saw (plan Q5: the stock check_blocked contract)
	byte					avoidTier;			// avoidTier_t of avoidTierTic
	bool					avoidHeldInPath;	// AI_OBSTACLE_IN_PATH as the last plan left it
	int						avoidTierTic;		// gameLocal.framenum avoidTier was computed for, -1 none
	moveStatus_t			avoidHeldStatus;	// move.moveStatus as the last plan left it
	idEntityPtr<idEntity>	avoidHeldObstacle;	// move.obstacle as the last plan left it
	// WP-A5: separation steering (plan §3.3, D4: it bends the heading and rides the move delta, never the origin)
	idVec3					sepLastDir;			// last tic's smoothed heading, zero when no neighbour pushed last tic
	idVec3					sepCorrection;		// this tic's overlap correction, added to the move delta so StepMove clips it
	float					sepDeltaScale;		// this tic's scale on the animation delta: below 1 when the push opposes the heading
	// WP-A6/A7: the flow field follower. Role and class are map data (the def, the ai_squad marker, sys.aiSetAvoid), the
	// rest is tier-scheduler state, one byte each, never a script decision (plan D6); nothing of it is saved
	byte					minionRole;			// minionRole_t
	byte					avoidClass;			// avoidClass_t forced on this body; the squad's class wins when it is pinned
	byte					flowStuckTics;		// tics running with a step asked for and under 0.5 units gained
	bool					flowOnField;		// this tic's seek point came from the field: no plan, walls and separation bend it
	short					aiSquad;			// index into gameLocal.aiSquads from the key 'squad', -1 none (WP-A7)
	int						flowAreaNum;		// the area GetMovePos found this tic, for the wall repulsion
	int						flowPromotedUntil;	// gameLocal.framenum until which a stuck follower runs the stock path and planner
	//puncho end

	// turning
	float					ideal_yaw;
	float					current_yaw;
	float					turnRate;
	float					turnVel;
	float					anim_turn_yaw;
	float					anim_turn_amount;
	float					anim_turn_angles;

	// physics
	idPhysics_Monster		physicsObj;

	// flying
	jointHandle_t			flyTiltJoint;
	float					fly_speed;
	float					fly_bob_strength;
	float					fly_bob_vert;
	float					fly_bob_horz;
	int						fly_offset;					// prefered offset from player's view
	float					fly_seek_scale;
	float					fly_roll_scale;
	float					fly_roll_max;
	float					fly_roll;
	float					fly_pitch_scale;
	float					fly_pitch_max;
	float					fly_pitch;

	bool					allowMove;					// disables any animation movement
	bool					allowHiddenMovement;		// allows character to still move around while hidden
	bool					disableGravity;				// disables gravity and allows vertical movement by the animation
	bool					af_push_moveables;			// allow the articulated figure to push moveable objects

	// weapon/attack vars
	bool					lastHitCheckResult;
	//puncho start - ai-combat (plans/ai-combat/07-DESIGN-c8-levers.md §B): the def key fire_near_friends, read in Spawn,
	// not saved. 0 tests the shot against boxes (a friend in the arc blocks it), 1 against meshes as stock
	bool					fireNearFriends;
	//puncho end
	//puncho start - ai-combat (plans/ai-combat/08-PLAN-shooter-tokens.md §3.1): the lever sniper (squad, def, 0), set
	// with fireNearFriends by ReadCombatLevers; not saved. A sniper tests its shots without a shooter token
	bool					shooterSniper;
	//puncho end
	//puncho start - ai-combat (plans/ai-combat/07-DESIGN-c8-levers.md §C): the accuracy lever, the squad's over the def's
	// over 1, set with fireNearFriends by ReadCombatLevers at spawn and after restore; not saved. ai_accuracyScale times
	// this scales both spreads of LaunchProjectile, above 1 is less accurate
	float					accuracyScale;
	//puncho end
	int						lastHitCheckTime;
	//puncho start - ai-combat (plans/ai-combat/07-DESIGN-c8-levers.md §A): the decision clock. Host-side, not saved: a
	// restore costs at most one extra test
	int						canHitFailTime;				// gameLocal.time of the last canHitEnemyFromAnim that said no; -1 none (ai_canHitRetryMs)
	//puncho end
	//puncho start - ai-combat (plans/ai-combat/08-PLAN-shooter-tokens.md §3.5): the shooter token. Host-side derived
	// state, not saved: a restore starts without a token and the next pass ranks the monster again
	int						shooterAskTime;			// gameLocal.time of the last canHitEnemyFromAnim that met the token gate; -1 never
	int						shooterPassTime;		// gameLocal.time of the last canHitEnemyFromAnim test that said yes; -1 never
	int						shooterTokenGen;		// gameLocal.shooterTokenGen of the pass that gave the token; -1 none
	int						shooterTokenTime;		// gameLocal.time that token was first given: the hold runs from here
	//puncho end
	int						lastAttackTime;
	float					melee_range;
	float					projectile_height_to_distance_ratio;	// calculates the maximum height a projectile can be thrown
	idList<idVec3, TAG_AI>			missileLaunchOffset;

	const idDict* 			projectileDef;
	mutable idClipModel*		projectileClipModel;
	float					projectileRadius;
	float					projectileSpeed;
	idVec3					projectileVelocity;
	idVec3					projectileGravity;
	idEntityPtr<idProjectile> projectile;
	idStr					attack;
	idVec3					homingMissileGoal;

	// chatter/talking
	const idSoundShader*		chat_snd;
	int						chat_min;
	int						chat_max;
	int						chat_time;
	talkState_t				talk_state;
	idEntityPtr<idActor>	talkTarget;

	// cinematics
	int						num_cinematics;
	int						current_cinematic;

	bool					allowJointMod;
	idEntityPtr<idEntity>	focusEntity;
	idVec3					currentFocusPos;
	int						focusTime;
	int						alignHeadTime;
	int						forceAlignHeadTime;
	idAngles				eyeAng;
	idAngles				lookAng;
	idAngles				destLookAng;
	idAngles				lookMin;
	idAngles				lookMax;
	idList<jointHandle_t, TAG_AI>	lookJoints;
	idList<idAngles, TAG_AI>		lookJointAngles;
	float					eyeVerticalOffset;
	float					eyeHorizontalOffset;
	float					eyeFocusRate;
	float					headFocusRate;
	int						focusAlignTime;

	// special fx
	bool					restartParticles;			// should smoke emissions restart
	bool					useBoneAxis;				// use the bone vs the model axis
	idList<particleEmitter_t, TAG_AI> particles;				// particle data

	renderLight_t			worldMuzzleFlash;			// positioned on world weapon bone
	int						worldMuzzleFlashHandle;
	jointHandle_t			flashJointWorld;
	int						muzzleFlashEnd;
	int						flashTime;

	// joint controllers
	idAngles				eyeMin;
	idAngles				eyeMax;
	jointHandle_t			focusJoint;
	jointHandle_t			orientationJoint;

	// enemy variables
	idEntityPtr<idActor>	enemy;
	idVec3					lastVisibleEnemyPos;
	idVec3					lastVisibleEnemyEyeOffset;
	idVec3					lastVisibleReachableEnemyPos;
	idVec3					lastReachableEnemyPos;
	bool					wakeOnFlashlight;

	bool					spawnClearMoveables;

	idHashTable<funcEmitter_t> funcEmitters;

	idEntityPtr<idHarvestable>	harvestEnt;

	// script variables
	idScriptBool			AI_TALK;
	idScriptBool			AI_DAMAGE;
	idScriptBool			AI_PAIN;
	idScriptFloat			AI_SPECIAL_DAMAGE;
	idScriptBool			AI_DEAD;
	idScriptBool			AI_ENEMY_VISIBLE;
	idScriptBool			AI_ENEMY_IN_FOV;
	idScriptBool			AI_ENEMY_DEAD;
	idScriptBool			AI_MOVE_DONE;
	idScriptBool			AI_ONGROUND;
	idScriptBool			AI_ACTIVATED;
	idScriptBool			AI_FORWARD;
	idScriptBool			AI_JUMP;
	idScriptBool			AI_ENEMY_REACHABLE;
	idScriptBool			AI_BLOCKED;
	idScriptBool			AI_OBSTACLE_IN_PATH;
	idScriptBool			AI_DEST_UNREACHABLE;
	idScriptBool			AI_HIT_ENEMY;
	idScriptBool			AI_PUSHED;

	//
	// ai/ai.cpp
	//
	void					SetAAS();
	virtual	void			DormantBegin();	// called when entity becomes dormant
	virtual	void			DormantEnd();		// called when entity wakes from being dormant
	void					Think();
	void					Activate( idEntity* activator );
public:
	int						ReactionTo( const idEntity* ent );
protected:
	bool					CheckForEnemy();
	void					EnemyDead();
	virtual bool			CanPlayChatterSounds() const;
	void					SetChatSound();
	void					PlayChatter();
	virtual void			Hide();
	virtual void			Show();
	idVec3					FirstVisiblePointOnPath( const idVec3 origin, const idVec3& target, int travelFlags ) const;
	void					CalculateAttackOffsets();
	void					PlayCinematic();

	// movement
	virtual void			ApplyImpulse( idEntity* ent, int id, const idVec3& point, const idVec3& impulse );
	void					GetMoveDelta( const idMat3& oldaxis, const idMat3& axis, idVec3& delta );
	void					CheckObstacleAvoidance( const idVec3& goalPos, idVec3& newPos );
	//puncho start - ai-avoidance
	int						AvoidPeriod();
	void					UpdateAvoidTier();	// WP-A1: this tic's tier from the players' positions and the merged PVS, once per tic
	void					SetCollisionGroup();	// WP-A3: the clip model's group from the team, at spawn and after restore
	void					Separation( idVec3& seekPos );	// WP-A5: bend seekPos away from the neighbours of the grid (AI_crowd.cpp)
	bool					FlowFollower() const;	// WP-A6: this move walks the flow field
	void					WallRepulsion( idVec3& seekPos );	// WP-A6: bend seekPos away from the wall quads of the area (AI_crowd.cpp)
	void					ReadSquadRole();		// WP-A7: role and class from the def and the squad marker, at spawn and after restore
	int						EffectiveAvoidClass() const;	// WP-A7: the squad's pinned class, else this body's
	//puncho end
	//puncho start - ai-combat (plans/ai-combat/07-DESIGN-c8-levers.md §C): the combat levers, squad over def over default
	float					GetCombatKey( const char* key, float def ) const;	// at spawn, restore and script init, never per tic
	void					ReadCombatLevers();		// fireNearFriends, accuracyScale and shooterSniper, at spawn and after restore
	//puncho end
	//puncho start - ai-combat (plans/ai-combat/08-PLAN-shooter-tokens.md §3.4): the token gate and the decision clock
	int						CanHitRetryMs();		// the decision clock's interval this tic: ai_canHitRetryMs times the tier factor (07 §A)
	int						ShooterPool() const;	// the enemy's token pool: a player's entity number, else MAX_CLIENTS + team; -1 no enemy
	bool					ShooterMayTest();		// the token gate of canHitEnemyFromAnim; records the ask (AI_crowd.cpp)
	//puncho end
	void					DeadMove();
	void					AnimMove();
	void					SlideMove();
	void					AdjustFlyingAngles();
	void					AddFlyBob( idVec3& vel );
	void					AdjustFlyHeight( idVec3& vel, const idVec3& goalPos );
	void					FlySeekGoal( idVec3& vel, idVec3& goalPos );
	void					AdjustFlySpeed( idVec3& vel );
	void					FlyTurn();
	void					FlyMove();
	void					StaticMove();

	// damage
	virtual bool			Pain( idEntity* inflictor, idEntity* attacker, int damage, const idVec3& dir, int location );
	virtual void			Killed( idEntity* inflictor, idEntity* attacker, int damage, const idVec3& dir, int location );

	// navigation
	void					KickObstacles( const idVec3& dir, float force, idEntity* alwaysKick );
	bool					ReachedPos( const idVec3& pos, const moveCommand_t moveCommand ) const;
	float					TravelDistance( const idVec3& start, const idVec3& end ) const;
	int						PointReachableAreaNum( const idVec3& pos, const float boundsScale = 2.0f ) const;
	bool					PathToGoal( aasPath_t& path, int areaNum, const idVec3& origin, int goalAreaNum, const idVec3& goalOrigin ) const;
	void					DrawRoute() const;
	bool					GetMovePos( idVec3& seekPos );
	bool					MoveDone() const;
	bool					EntityCanSeePos( idActor* actor, const idVec3& actorOrigin, const idVec3& pos );
	void					BlockedFailSafe();

	// movement control
	void					StopMove( moveStatus_t status );
	bool					FaceEnemy();
	bool					FaceEntity( idEntity* ent );
	bool					DirectMoveToPosition( const idVec3& pos );
	bool					MoveToEnemyHeight();
	bool					MoveOutOfRange( idEntity* entity, float range );
	bool					MoveToAttackPosition( idEntity* ent, int attack_anim );
	bool					MoveToEnemy();
	bool					MoveToEntity( idEntity* ent );
	bool					MoveToPosition( const idVec3& pos );
	bool					MoveToCover( idEntity* entity, const idVec3& pos );
	bool					SlideToPosition( const idVec3& pos, float time );
	bool					WanderAround();
	bool					StepDirection( float dir );
	bool					NewWanderDir( const idVec3& dest );

	// effects
	const idDeclParticle*	SpawnParticlesOnJoint( particleEmitter_t& pe, const char* particleName, const char* jointName );
	void					SpawnParticles( const char* keyName );
	bool					ParticlesActive();

	// turning
	bool					FacingIdeal();
	void					Turn();
	bool					TurnToward( float yaw );
	bool					TurnToward( const idVec3& pos );

	// enemy management
	void					ClearEnemy();
	bool					EnemyPositionValid() const;
	void					SetEnemyPosition();
	void					UpdateEnemyPosition();
	void					SetEnemy( idActor* newEnemy );

	// attacks
	void					CreateProjectileClipModel() const;
	idProjectile*			CreateProjectile( const idVec3& pos, const idVec3& dir );
	void					RemoveProjectile();
	idProjectile*			LaunchProjectile( const char* jointname, idEntity* target, bool clampToAttackCone );
	virtual void			DamageFeedback( idEntity* victim, idEntity* inflictor, int& damage );
	void					DirectDamage( const char* meleeDefName, idEntity* ent );
	bool					TestMelee() const;
	bool					AttackMelee( const char* meleeDefName );
	void					BeginAttack( const char* name );
	void					EndAttack();
	void					PushWithAF();

	// special effects
	void					GetMuzzle( const char* jointname, idVec3& muzzle, idMat3& axis );
	void					InitMuzzleFlash();
	void					TriggerWeaponEffects( const idVec3& muzzle );
	void					UpdateMuzzleFlash();
	virtual bool			UpdateAnimationControllers();
	void					UpdateParticles();
	void					TriggerParticles( const char* jointName );

	void					TriggerFX( const char* joint, const char* fx );
	idEntity*				StartEmitter( const char* name, const char* joint, const char* particle );
	idEntity*				GetEmitter( const char* name );
	void					StopEmitter( const char* name );

	// AI script state management
	void					LinkScriptVariables();
	void					UpdateAIScript();

	//
	// ai/ai_events.cpp
	//
	void					Event_Activate( idEntity* activator );
	void					Event_Touch( idEntity* other, trace_t* trace );
	void					Event_FindEnemy( int useFOV );
	void					Event_FindEnemyAI( int useFOV );
	void					Event_FindEnemyInCombatNodes();
	void					Event_ClosestReachableEnemyOfEntity( idEntity* team_mate );
	void					Event_HeardSound( int ignore_team );
	void					Event_SetEnemy( idEntity* ent );
	void					Event_ClearEnemy();
	void					Event_MuzzleFlash( const char* jointname );
	void					Event_CreateMissile( const char* jointname );
	void					Event_AttackMissile( const char* jointname );
	void					Event_FireMissileAtTarget( const char* jointname, const char* targetname );
	void					Event_LaunchMissile( const idVec3& muzzle, const idAngles& ang );
	void					Event_LaunchHomingMissile();
	void					Event_SetHomingMissileGoal();
	void					Event_LaunchProjectile( const char* entityDefName );
	void					Event_AttackMelee( const char* meleeDefName );
	void					Event_DirectDamage( idEntity* damageTarget, const char* damageDefName );
	void					Event_RadiusDamageFromJoint( const char* jointname, const char* damageDefName );
	void					Event_BeginAttack( const char* name );
	void					Event_EndAttack();
	void					Event_MeleeAttackToJoint( const char* jointname, const char* meleeDefName );
	void					Event_RandomPath();
	void					Event_CanBecomeSolid();
	void					Event_BecomeSolid();
	void					Event_BecomeNonSolid();
	void					Event_BecomeRagdoll();
	void					Event_StopRagdoll();
	void					Event_SetHealth( float newHealth );
	void					Event_GetHealth();
	void					Event_AllowDamage();
	void					Event_IgnoreDamage();
	void					Event_GetCurrentYaw();
	void					Event_TurnTo( float angle );
	void					Event_TurnToPos( const idVec3& pos );
	void					Event_TurnToEntity( idEntity* ent );
	void					Event_MoveStatus();
	void					Event_StopMove();
	void					Event_MoveToCover();
	void					Event_MoveToEnemy();
	void					Event_MoveToEnemyHeight();
	void					Event_MoveOutOfRange( idEntity* entity, float range );
	void					Event_MoveToAttackPosition( idEntity* entity, const char* attack_anim );
	void					Event_MoveToEntity( idEntity* ent );
	void					Event_MoveToPosition( const idVec3& pos );
	void					Event_SlideTo( const idVec3& pos, float time );
	void					Event_Wander();
	void					Event_FacingIdeal();
	void					Event_FaceEnemy();
	void					Event_FaceEntity( idEntity* ent );
	void					Event_WaitAction( const char* waitForState );
	void					Event_GetCombatNode();
	void					Event_EnemyInCombatCone( idEntity* ent, int use_current_enemy_location );
	void					Event_WaitMove();
	void					Event_GetJumpVelocity( const idVec3& pos, float speed, float max_height );
	void					Event_EntityInAttackCone( idEntity* ent );
	void					Event_CanSeeEntity( idEntity* ent );
	void					Event_SetTalkTarget( idEntity* target );
	void					Event_GetTalkTarget();
	void					Event_SetTalkState( int state );
	void					Event_EnemyRange();
	void					Event_EnemyRange2D();
	void					Event_GetEnemy();
	void					Event_GetEnemyPos();
	void					Event_GetEnemyEyePos();
	void					Event_PredictEnemyPos( float time );
	void					Event_CanHitEnemy();
	void					Event_CanHitEnemyFromAnim( const char* animname );
	void					Event_CanHitEnemyFromJoint( const char* jointname );
	void					Event_GetCombatKey( const char* key, float def );	//puncho - ai-combat (plans/ai-combat/07-DESIGN-c8-levers.md §C)
	void					Event_EnemyPositionValid();
	void					Event_ChargeAttack( const char* damageDef );
	void					Event_TestChargeAttack();
	void					Event_TestAnimMoveTowardEnemy( const char* animname );
	void					Event_TestAnimMove( const char* animname );
	void					Event_TestMoveToPosition( const idVec3& position );
	void					Event_TestMeleeAttack();
	void					Event_TestAnimAttack( const char* animname );
	void					Event_Shrivel( float shirvel_time );
	void					Event_Burn();
	void					Event_PreBurn();
	void					Event_ClearBurn();
	void					Event_SetSmokeVisibility( int num, int on );
	void					Event_NumSmokeEmitters();
	void					Event_StopThinking();
	void					Event_GetTurnDelta();
	void					Event_GetMoveType();
	void					Event_SetMoveType( int moveType );
	void					Event_SaveMove();
	void					Event_RestoreMove();
	void					Event_AllowMovement( float flag );
	void					Event_JumpFrame();
	void					Event_EnableClip();
	void					Event_DisableClip();
	void					Event_EnableGravity();
	void					Event_DisableGravity();
	void					Event_EnableAFPush();
	void					Event_DisableAFPush();
	void					Event_SetFlySpeed( float speed );
	void					Event_SetFlyOffset( int offset );
	void					Event_ClearFlyOffset();
	void					Event_GetClosestHiddenTarget( const char* type );
	void					Event_GetRandomTarget( const char* type );
	void					Event_TravelDistanceToPoint( const idVec3& pos );
	void					Event_TravelDistanceToEntity( idEntity* ent );
	void					Event_TravelDistanceBetweenPoints( const idVec3& source, const idVec3& dest );
	void					Event_TravelDistanceBetweenEntities( idEntity* source, idEntity* dest );
	void					Event_LookAtEntity( idEntity* ent, float duration );
	void					Event_LookAtEnemy( float duration );
	void					Event_SetJointMod( int allowJointMod );
	void					Event_ThrowMoveable();
	void					Event_ThrowAF();
	void					Event_SetAngles( idAngles const& ang );
	void					Event_GetAngles();
	void					Event_GetTrajectoryToPlayer();
	void					Event_RealKill();
	void					Event_Kill();
	void					Event_WakeOnFlashlight( int enable );
	void					Event_LocateEnemy();
	void					Event_KickObstacles( idEntity* kickEnt, float force );
	void					Event_GetObstacle();
	void					Event_PushPointIntoAAS( const idVec3& pos );
	void					Event_GetTurnRate();
	void					Event_SetTurnRate( float rate );
	void					Event_AnimTurn( float angles );
	void					Event_AllowHiddenMovement( int enable );
	void					Event_TriggerParticles( const char* jointName );
	void					Event_FindActorsInBounds( const idVec3& mins, const idVec3& maxs );
	void 					Event_CanReachPosition( const idVec3& pos );
	void 					Event_CanReachEntity( idEntity* ent );
	void					Event_CanReachEnemy();
	void					Event_GetReachableEntityPosition( idEntity* ent );
	void					Event_MoveToPositionDirect( const idVec3& pos );
	void					Event_AvoidObstacles( int ignore );
	void					Event_TriggerFX( const char* joint, const char* fx );

	void					Event_StartEmitter( const char* name, const char* joint, const char* particle );
	void					Event_GetEmitter( const char* name );
	void					Event_StopEmitter( const char* name );
};

class idCombatNode : public idEntity
{
public:
	CLASS_PROTOTYPE( idCombatNode );

	idCombatNode();

	void				Save( idSaveGame* savefile ) const;
	void				Restore( idRestoreGame* savefile );

	void				Spawn();
	bool				IsDisabled() const;
	bool				EntityInView( idActor* actor, const idVec3& pos );
	static void			DrawDebugInfo();

private:
	float				min_dist;
	float				max_dist;
	float				cone_dist;
	float				min_height;
	float				max_height;
	idVec3				cone_left;
	idVec3				cone_right;
	idVec3				offset;
	bool				disabled;

	void				Event_Activate( idEntity* activator );
	void				Event_MarkUsed();
};

#endif /* !__AI_H__ */
