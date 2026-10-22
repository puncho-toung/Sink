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


#include "../Game_local.h"

/*
===============================================================================

	Crowd: the neighbour grid and separation steering
	(puncho, ai-avoidance, plans/ai-avoidance/00-PLAN.md §3.3, WP-A5)

	Every live actor goes into a flat grid of 64-unit cells once per tic, after the think loop, from the positions
	physics just produced; the next tic's thinks read it and nothing writes it until the next build. A monster's
	separation is a sum over the neighbours inside its ring, 1.2 ( r + r_j ): the push bends the heading TurnToward is
	given and scales the animation delta when it opposes the heading; an overlap deeper than 4 units is corrected by at
	most ai_sepCorrect units per tic through the move delta, so StepMove clips the correction against the world and the
	origin is never set directly (plan D4). Players and monsters without a move order count as infinite mass: they are
	never pushed and the one walking into them takes the whole push (R03.24).

===============================================================================
*/

const float SEP_DEAD_BAND		= 4.0f;		// an overlap this deep is left alone (P04.8's hysteresis)
const float SEP_BLEND			= 0.5f;		// the push against the desired direction
const float SEP_SMOOTH			= 0.7f;		// last tic's heading against this tic's
const float SEP_STEER_DIST		= 64.0f;	// how far ahead the bent heading is pointed

/*
============
AI_NeighborCell
============
*/
static ID_INLINE int AI_NeighborCell( float coord )
{
	return idMath::Ftoi( idMath::Floor( coord / AI_NEIGHBOR_CELL_SIZE ) );
}

/*
============
AI_NeighborBucket
============
*/
static ID_INLINE int AI_NeighborBucket( int cellX, int cellY )
{
	// unsigned: the products wrap by definition, a signed overflow would be undefined
	return ( int )( ( ( ( unsigned int )cellX * 73856093u ) ^ ( ( unsigned int )cellY * 19349663u ) ) & ( unsigned int )( AI_NEIGHBOR_CELLS - 1 ) );
}

/*
============
idGameLocal::BuildNeighborGrid

  Called after the think loop: every actor that is alive, shown and has a clip model, in spawn order so the chains are
  deterministic. The budget of overlap corrections is reset here, so it is per tic over every monster.
============
*/
void idGameLocal::BuildNeighborGrid()
{
	PROFILE_STATIC_DESC( descGrid, "Minions::Crowd::Grid", ::Optick::Category::AI );
	idAIProfileScope scopeGrid( aiStats.usGrid, descGrid );

	memset( aiNeighborCells, -1, sizeof( aiNeighborCells ) );
	numAINeighbors = 0;
	aiNeighborMaxRadius = 0.0f;

	for( idEntity* ent = spawnedEntities.Next(); ent != NULL; ent = ent->spawnNode.Next() )
	{
		if( !ent->IsType( idActor::Type ) || ent->IsHidden() || ent->health <= 0 )
		{
			continue;
		}
		const idPhysics* phys = ent->GetPhysics();
		const idClipModel* clipModel = phys->GetClipModel();
		if( clipModel == NULL || !clipModel->IsEnabled() )
		{
			continue;
		}
		if( numAINeighbors == MAX_AI_NEIGHBORS )
		{
			Warning( "BuildNeighborGrid: more than %d actors, the rest are no neighbours", MAX_AI_NEIGHBORS );
			break;
		}

		aiNeighbor_t& n = aiNeighbors[numAINeighbors];
		const idBounds& bounds = phys->GetBounds();
		n.origin = phys->GetOrigin();
		n.velocity = phys->GetLinearVelocity();
		n.radius = Max( Max( -bounds[0].x, bounds[1].x ), Max( -bounds[0].y, bounds[1].y ) );
		n.zMin = n.origin.z + bounds[0].z;
		n.zMax = n.origin.z + bounds[1].z;
		n.mass = phys->GetMass();
		n.entityNumber = ent->entityNumber;
		n.cellX = ( short )AI_NeighborCell( n.origin.x );
		n.cellY = ( short )AI_NeighborCell( n.origin.y );
		n.group = clipModel->GetGroup();
		n.flags = 0;
		n.pad = 0;
		if( ent->IsType( idPlayer::Type ) )
		{
			n.flags |= AIN_PLAYER;
		}
		else if( ent->IsType( idAI::Type ) && static_cast<const idAI*>( ent )->IsHoldingPosition() )
		{
			n.flags |= AIN_HOLDING;
		}

		const int bucket = AI_NeighborBucket( n.cellX, n.cellY );
		n.nextInCell = aiNeighborCells[bucket];
		aiNeighborCells[bucket] = numAINeighbors++;

		if( n.radius > aiNeighborMaxRadius )
		{
			aiNeighborMaxRadius = n.radius;
		}
	}

	aiNeighborTic = framenum;
	sepCorrectionsLeft = ai_sepBudget.GetInteger();
}

/*
============
idGameLocal::GatherNeighbors

  Every body whose flat centre distance to origin is under range, by walking the cells the range covers. A body that
  hashed into a walked bucket from another cell is skipped by its cell coordinates, so nothing is listed twice.
============
*/
int idGameLocal::GatherNeighbors( const idVec3& origin, float range, int* indices, int maxIndices ) const
{
	const int cellX0 = AI_NeighborCell( origin.x - range );
	const int cellX1 = AI_NeighborCell( origin.x + range );
	const int cellY0 = AI_NeighborCell( origin.y - range );
	const int cellY1 = AI_NeighborCell( origin.y + range );
	const float rangeSqr = range * range;
	int num = 0;

	for( int cellY = cellY0; cellY <= cellY1; cellY++ )
	{
		for( int cellX = cellX0; cellX <= cellX1; cellX++ )
		{
			for( int i = aiNeighborCells[AI_NeighborBucket( cellX, cellY )]; i != -1; i = aiNeighbors[i].nextInCell )
			{
				const aiNeighbor_t& n = aiNeighbors[i];
				if( n.cellX != cellX || n.cellY != cellY )
				{
					continue;
				}
				if( ( n.origin.ToVec2() - origin.ToVec2() ).LengthSqr() >= rangeSqr )
				{
					continue;
				}
				if( num == maxIndices )
				{
					return num;
				}
				indices[num++] = i;
			}
		}
	}
	return num;
}

/*
=====================
idAI::Separation

  Bends seekPos, the point TurnToward is about to be given, away from the neighbours inside this monster's ring, and
  leaves this tic's delta scale and overlap correction for AnimMove's move delta. The enemy and the goal entity are no
  neighbours: a monster must reach melee range. Nothing happens without the grid of the previous tic.
=====================
*/
void idAI::Separation( idVec3& seekPos )
{
	if( !ai_separation.GetBool() || !gameLocal.NeighborGridValid() )
	{
		sepLastDir.Zero();
		return;
	}

	const idVec3& origin = physicsObj.GetOrigin();
	const idBounds& bounds = physicsObj.GetBounds();
	const float radius = Max( Max( -bounds[0].x, bounds[1].x ), Max( -bounds[0].y, bounds[1].y ) );
	const float zMin = origin.z + bounds[0].z;
	const float zMax = origin.z + bounds[1].z;
	const float mass = physicsObj.GetMass();
	const idEntity* enemyEnt = enemy.GetEntity();
	const idEntity* goalEnt = move.goalEntity.GetEntity();
	const int enemyNum = ( enemyEnt != NULL ) ? enemyEnt->entityNumber : -1;
	const int goalNum = ( goalEnt != NULL ) ? goalEnt->entityNumber : -1;
	const float ring = ai_sepRadius.GetFloat();

	int indices[MAX_AI_NEIGHBORS];
	const int num = gameLocal.GatherNeighbors( origin, ring * ( radius + gameLocal.aiNeighborMaxRadius ), indices, MAX_AI_NEIGHBORS );

	idVec2 push( 0.0f, 0.0f );
	idVec2 correction( 0.0f, 0.0f );
	for( int i = 0; i < num; i++ )
	{
		const aiNeighbor_t& n = gameLocal.aiNeighbors[indices[i]];
		if( n.entityNumber == entityNumber || n.entityNumber == enemyNum || n.entityNumber == goalNum )
		{
			continue;
		}
		if( n.zMax < zMin || n.zMin > zMax )
		{
			continue;
		}
		idVec2 away = origin.ToVec2() - n.origin.ToVec2();
		const float dist = away.Length();
		const float ringDist = ring * ( radius + n.radius );
		if( dist >= ringDist )
		{
			continue;
		}
		gameLocal.aiStats.sepPairs++;
		if( dist > 1.0f )
		{
			away /= dist;
		}
		else
		{
			// on top of each other: step aside, the side by entity number so two such bodies part
			away.Set( -viewAxis[0].y, viewAxis[0].x );
			if( entityNumber & 1 )
			{
				away = -away;
			}
		}
		float weight = 1.0f - dist / ringDist;
		if( !( n.flags & ( AIN_HOLDING | AIN_PLAYER ) ) )
		{
			weight *= n.mass / ( mass + n.mass );
		}
		push += away * weight;

		const float overlap = ( radius + n.radius ) - dist;
		if( overlap > SEP_DEAD_BAND )
		{
			correction += away * ( overlap - SEP_DEAD_BAND );
		}
	}

	if( push.x != 0.0f || push.y != 0.0f )
	{
		idVec2 dir = seekPos.ToVec2() - origin.ToVec2();
		if( dir.Normalize() < 1.0f )
		{
			dir = viewAxis[0].ToVec2();
		}
		const float pushLen = push.Length();
		if( pushLen > 1.0f )
		{
			push /= pushLen;
		}
		idVec2 steer = dir * ( 1.0f - SEP_BLEND ) + push * SEP_BLEND;
		if( steer.Normalize() < 0.001f )
		{
			steer = dir;
		}
		if( sepLastDir.x != 0.0f || sepLastDir.y != 0.0f )
		{
			steer = sepLastDir.ToVec2() * SEP_SMOOTH + steer * ( 1.0f - SEP_SMOOTH );
			steer.Normalize();
		}
		sepLastDir.Set( steer.x, steer.y, 0.0f );
		seekPos = origin + sepLastDir * SEP_STEER_DIST;

		const float oppose = -( push * dir );
		if( oppose > 0.0f )
		{
			sepDeltaScale = Max( 0.0f, 1.0f - oppose );
		}
		gameLocal.aiStats.sepSteered++;
	}
	else
	{
		sepLastDir.Zero();
	}

	if( ( correction.x != 0.0f || correction.y != 0.0f ) && gameLocal.sepCorrectionsLeft > 0 )
	{
		const float maxCorrect = ai_sepCorrect.GetFloat();
		const float len = correction.Length();
		if( len > maxCorrect )
		{
			correction *= maxCorrect / len;
		}
		sepCorrection.Set( correction.x, correction.y, 0.0f );
		gameLocal.sepCorrectionsLeft--;
		gameLocal.aiStats.sepCorrections++;
	}
}

/*
===============================================================================

	Flow field followers (plans/ai-avoidance/00-PLAN.md §3.5, WP-A6)

===============================================================================
*/

const int	FLOW_WALL_QUADS		= 32;		// wall quads of the area and the seams one tic looks at
const float	FLOW_WALL_MARGIN	= 8.0f;		// a wall closer than the radius plus this pushes

/*
=====================
idAI::FlowFollower

  Whether this move walks the flow field: a walker with an AAS and a goal that a horde may share (an enemy, an entity, a
  position); cover, attack and range moves are individual goals and keep the stock path. ai_flowField 2 puts every such
  walker on the field for the A/B; 1 only the followers by role or forced class; a forced full class never.
=====================
*/
bool idAI::FlowFollower() const
{
	const int mode = ai_flowField.GetInteger();
	if( mode == 0 || move.moveType != MOVETYPE_ANIM || aas == NULL )
	{
		return false;
	}
	if( move.moveCommand != MOVE_TO_ENEMY && move.moveCommand != MOVE_TO_ENTITY && move.moveCommand != MOVE_TO_POSITION )
	{
		return false;
	}
	const int avoid = EffectiveAvoidClass();
	if( avoid == AVOID_CLASS_FULL || avoid == AVOID_CLASS_OFF )
	{
		return false;
	}
	return mode == 2 || avoid == AVOID_CLASS_LIGHT || minionRole == MINION_ROLE_FOLLOWER;
}

/*
=====================
idAI::ReadSquadRole

  WP-A7 (plan §3.6): follower for a def that says minion_class trash, elite otherwise; the ai_squad marker named by the
  key 'squad' overrides the role when it gives one and sets the avoidance class. Derived from map data, never saved:
  read at spawn and after restore.
=====================
*/
void idAI::ReadSquadRole()
{
	minionRole = ( idStr::Icmp( spawnArgs.GetString( "minion_class" ), "trash" ) == 0 ) ? MINION_ROLE_FOLLOWER : MINION_ROLE_ELITE;
	avoidClass = AVOID_CLASS_BY_ROLE;
	aiSquad = -1;

	const char* squadName = spawnArgs.GetString( "squad" );
	if( squadName[0] == '\0' )
	{
		return;
	}
	aiSquad = ( short )gameLocal.FindAISquad( squadName );
	if( aiSquad < 0 )
	{
		gameLocal.Warning( "%s: squad '%s' has no ai_squad marker on this map", name.c_str(), squadName );
		return;
	}
	const aiSquadDef_t& squad = gameLocal.aiSquads[aiSquad];
	if( squad.role != AI_SQUAD_ROLE_UNSET )
	{
		minionRole = squad.role;
	}
	avoidClass = squad.avoid;
}

/*
=====================
idAI::EffectiveAvoidClass
=====================
*/
int idAI::EffectiveAvoidClass() const
{
	if( aiSquad >= 0 && gameLocal.aiSquads[aiSquad].pinned )
	{
		return gameLocal.aiSquads[aiSquad].avoid;
	}
	return avoidClass;
}

/*
===============================================================================

	The ai_squad markers (plan §3.6, WP-A7)

===============================================================================
*/

/*
============
AI_SquadAvoidClass

  "full", "light" or "off" to avoidClass_t; -1 for anything else
============
*/
static int AI_SquadAvoidClass( const char* avoid )
{
	if( idStr::Icmp( avoid, "full" ) == 0 )
	{
		return AVOID_CLASS_FULL;
	}
	if( idStr::Icmp( avoid, "light" ) == 0 )
	{
		return AVOID_CLASS_LIGHT;
	}
	if( idStr::Icmp( avoid, "off" ) == 0 )
	{
		return AVOID_CLASS_OFF;
	}
	return -1;
}

/*
============
aiSquadLevers

  The combat levers of an ai_squad marker (plans/ai-combat/07-DESIGN-c8-levers.md §C): one row per marker key, the float
  it fills in aiSquadDef_t, its bit in overrides and what it accepts. ReadAISquads walks it per marker at map load and
  idAI::GetCombatKey per key at spawn, restore and script init; nothing reads it per tic.
============
*/
enum
{
	LEVER_ANY,				// any number: the ranges
	LEVER_POSITIVE,			// above 0: the accuracy scale and the rates
	LEVER_CHANCE,			// 0..1
	LEVER_SWITCH			// 0 or 1
};

static const char* const leverAccepts[] = { "any number", "above 0", "0..1", "0 or 1" };

struct aiSquadLever_t
{
	const char*		key;
	int				offset;		// of the float in aiSquadDef_t
	short			bit;		// AI_SQUAD_SET_*
	short			accepts;	// LEVER_*
};

static const aiSquadLever_t aiSquadLevers[] =
{
	{ "accuracy",			offsetof( aiSquadDef_t, accuracy ),			AI_SQUAD_SET_ACCURACY,			LEVER_POSITIVE },
	{ "attack_rate",		offsetof( aiSquadDef_t, attackRate ),		AI_SQUAD_SET_ATTACK_RATE,		LEVER_POSITIVE },
	{ "dodge_rate",			offsetof( aiSquadDef_t, dodgeRate ),		AI_SQUAD_SET_DODGE_RATE,		LEVER_POSITIVE },
	{ "dodge_chance",		offsetof( aiSquadDef_t, dodgeChance ),		AI_SQUAD_SET_DODGE_CHANCE,		LEVER_CHANCE },
	{ "range_min",			offsetof( aiSquadDef_t, rangeMin ),			AI_SQUAD_SET_RANGE_MIN,			LEVER_ANY },
	{ "range_max",			offsetof( aiSquadDef_t, rangeMax ),			AI_SQUAD_SET_RANGE_MAX,			LEVER_ANY },
	{ "kite_chance",		offsetof( aiSquadDef_t, kiteChance ),		AI_SQUAD_SET_KITE_CHANCE,		LEVER_CHANCE },
	{ "fire_near_friends",	offsetof( aiSquadDef_t, fireNearFriends ),	AI_SQUAD_SET_FIRE_NEAR_FRIENDS,	LEVER_SWITCH },
	{ "sniper",				offsetof( aiSquadDef_t, sniper ),			AI_SQUAD_SET_SNIPER,			LEVER_SWITCH }	//puncho - ai-combat (plans/ai-combat/08-PLAN-shooter-tokens.md §3.1)
};
static const int numSquadLevers = sizeof( aiSquadLevers ) / sizeof( aiSquadLevers[0] );

/*
============
AI_SquadLeverAccepts
============
*/
static bool AI_SquadLeverAccepts( int accepts, float value )
{
	switch( accepts )
	{
		case LEVER_POSITIVE:
			return value > 0.0f;
		case LEVER_CHANCE:
			return value >= 0.0f && value <= 1.0f;
		case LEVER_SWITCH:
			return value == 0.0f || value == 1.0f;
	}
	return true;
}

/*
============
idGameLocal::ReadAISquads

  The role keys of every ai_squad entity of the map file: name, role ( leader | follower | elite ), avoid ( full | light |
  off ) and formation ( none | line | column ), and the combat levers of aiSquadLevers, each optional and loud when
  malformed. Read before the entities spawn, in
  LoadMap (a new game and a restore alike) and in LocalMapRestart; the table is not saved, only the pins of sys.aiSetAvoid
  are (by name; InitFromSaveGame re-applies them after this, audit M2).
============
*/
void idGameLocal::ReadAISquads()
{
	numAISquads = 0;
	memset( aiSquads, 0, sizeof( aiSquads ) );
	if( mapFile == NULL )
	{
		return;
	}

	for( int i = 1; i < mapFile->GetNumEntities(); i++ )
	{
		const idDict& args = mapFile->GetEntity( i )->epairs;
		if( idStr::Icmp( args.GetString( "classname" ), "ai_squad" ) != 0 )
		{
			continue;
		}
		const char* squadName = args.GetString( "name" );
		if( squadName[0] == '\0' )
		{
			Warning( "ai_squad without a name is ignored" );
			continue;
		}
		if( FindAISquad( squadName ) != -1 )
		{
			Warning( "ai_squad '%s' defined twice: the first one is used", squadName );
			continue;
		}
		if( numAISquads == MAX_AI_SQUADS )
		{
			Warning( "more than %d ai_squad markers: '%s' and the rest are ignored", MAX_AI_SQUADS, squadName );
			break;
		}

		aiSquadDef_t& squad = aiSquads[numAISquads++];
		idStr::Copynz( squad.name, squadName, sizeof( squad.name ) );
		squad.role = AI_SQUAD_ROLE_UNSET;
		squad.avoid = AVOID_CLASS_BY_ROLE;
		squad.formation = 0;
		squad.pinned = 0;
		squad.overrides = 0;

		const char* role = args.GetString( "role" );
		if( idStr::Icmp( role, "leader" ) == 0 )
		{
			squad.role = MINION_ROLE_LEADER;
		}
		else if( idStr::Icmp( role, "follower" ) == 0 )
		{
			squad.role = MINION_ROLE_FOLLOWER;
		}
		else if( idStr::Icmp( role, "elite" ) == 0 )
		{
			squad.role = MINION_ROLE_ELITE;
		}
		else if( role[0] != '\0' )
		{
			Warning( "ai_squad '%s': unknown role '%s' (leader, follower or elite): the members' defs decide", squadName, role );
		}

		const char* avoid = args.GetString( "avoid" );
		if( avoid[0] != '\0' )
		{
			const int avoidClass = AI_SquadAvoidClass( avoid );
			if( avoidClass < 0 )
			{
				Warning( "ai_squad '%s': unknown avoid '%s' (full, light or off): the role decides", squadName, avoid );
			}
			else
			{
				squad.avoid = ( byte )avoidClass;
			}
		}

		const char* formation = args.GetString( "formation" );
		if( idStr::Icmp( formation, "line" ) == 0 || idStr::Icmp( formation, "column" ) == 0 )
		{
			squad.formation = ( idStr::Icmp( formation, "line" ) == 0 ) ? 1 : 2;
			Warning( "ai_squad '%s': formation '%s' is read but not laid out yet (plan Q3: none); the members keep the loose blob", squadName, formation );
		}
		else if( formation[0] != '\0' && idStr::Icmp( formation, "none" ) != 0 )
		{
			Warning( "ai_squad '%s': unknown formation '%s' (none, line or column): none", squadName, formation );
		}

		// the combat levers (§C): one the marker does not give, or gives malformed, keeps its bit clear and the defs decide
		for( int j = 0; j < numSquadLevers; j++ )
		{
			const aiSquadLever_t& lever = aiSquadLevers[j];
			const char* value = args.GetString( lever.key );
			if( value[0] == '\0' )
			{
				continue;
			}
			if( !idStr::IsNumeric( value ) || strpbrk( value, "0123456789" ) == NULL )	//puncho - ai-combat (plans/ai-combat/07-REVIEW-c8.md m4): IsNumeric takes "-", "." and "-."
			{
				Warning( "ai_squad '%s': %s '%s' is not a number: the members' defs decide", squadName, lever.key, value );
				continue;
			}
			const float f = atof( value );
			if( !AI_SquadLeverAccepts( lever.accepts, f ) )
			{
				Warning( "ai_squad '%s': %s '%s' is out of range (%s): the members' defs decide", squadName, lever.key, value, leverAccepts[lever.accepts] );
				continue;
			}
			*( float* )( ( byte* )&squad + lever.offset ) = f;
			squad.overrides |= lever.bit;
		}
	}
}

/*
============
idGameLocal::FindAISquad
============
*/
int idGameLocal::FindAISquad( const char* name ) const
{
	for( int i = 0; i < numAISquads; i++ )
	{
		if( idStr::Icmp( aiSquads[i].name, name ) == 0 )
		{
			return i;
		}
	}
	return -1;
}

/*
============
idGameLocal::SetAISquadAvoid

  sys.aiSetAvoid( squad, class ): pins the class on the squad for the map; a script may pin, it never un-pins a stuck
  promotion (plan D6). Loud and false when the squad or the class is unknown. The pin is saved with the game and put back
  by name on restore (audit M2); a map restart drops it with the table.
============
*/
bool idGameLocal::SetAISquadAvoid( const char* name, const char* avoid )
{
	const int squad = FindAISquad( name );
	const int avoidClass = AI_SquadAvoidClass( avoid );
	if( squad < 0 )
	{
		Warning( "aiSetAvoid: no ai_squad '%s' on this map", name );
		return false;
	}
	if( avoidClass < 0 )
	{
		Warning( "aiSetAvoid: unknown class '%s' for squad '%s' (full, light or off)", avoid, name );
		return false;
	}
	aiSquads[squad].avoid = ( byte )avoidClass;
	aiSquads[squad].pinned = 1;
	return true;
}

/*
===============================================================================

	The combat levers (plans/ai-combat/07-DESIGN-c8-levers.md §C)

	A def sets the class, the ai_squad marker overrides the squad, ai_accuracyScale scales the game. Every lever is read
	once, at spawn, after restore or in the script's init, never per tic: the lookups below walk nine rows and a dict
	(puncho, ai-combat, plans/ai-combat/08-PLAN-shooter-tokens.md: the ninth row is sniper).

===============================================================================
*/

/*
=====================
idAI::GetCombatKey

  The squad's override when the marker set this key, else the def key as a float, else def. A key the table does not
  know goes straight to the def key.
=====================
*/
float idAI::GetCombatKey( const char* key, float def ) const
{
	if( aiSquad >= 0 )
	{
		const aiSquadDef_t& squad = gameLocal.aiSquads[aiSquad];
		for( int i = 0; i < numSquadLevers; i++ )
		{
			if( idStr::Icmp( aiSquadLevers[i].key, key ) != 0 )
			{
				continue;
			}
			if( squad.overrides & aiSquadLevers[i].bit )
			{
				return *( const float* )( ( const byte* )&squad + aiSquadLevers[i].offset );
			}
			break;
		}
	}
	float value;
	if( spawnArgs.GetFloat( key, "0", value ) )
	{
		return value;
	}
	return def;
}

/*
=====================
idAI::ReadCombatLevers

  The three levers the engine reads itself, with the scripts' precedence: fireNearFriends (the test mask of
  canHitEnemyFromAnim), accuracyScale (LaunchProjectile's spreads) and shooterSniper (canHitEnemyFromAnim without a
  shooter token, plans/ai-combat/08-PLAN-shooter-tokens.md). Derived from the def and the marker like the squad role,
  never saved: at spawn and after restore, after ReadSquadRole.
=====================
*/
void idAI::ReadCombatLevers()
{
	fireNearFriends = GetCombatKey( "fire_near_friends", 0.0f ) != 0.0f;	//puncho - ai-combat (plans/ai-combat/07-REVIEW-c8.md m3): non-zero is the stock mesh test; the only read
	accuracyScale = GetCombatKey( "accuracy", 1.0f );
	shooterSniper = GetCombatKey( "sniper", 0.0f ) != 0.0f;	//puncho - ai-combat (plans/ai-combat/08-PLAN-shooter-tokens.md): non-zero tests without a shooter token
}

/*
=====================
idAI::WallRepulsion

  A 2-D push away from every wall quad of the area (and of the seams the body's own box touches, WP-A2's cache) whose
  front edge is closer than the body's radius plus a margin; it bends the direction to seekPos and keeps the distance.
  Only the front side pushes: a body behind a wall edge is in another area's problem.
=====================
*/
void idAI::WallRepulsion( idVec3& seekPos )
{
	aasWallQuad_t quads[FLOW_WALL_QUADS];
	const idVec3& origin = physicsObj.GetOrigin();
	const idBounds& bounds = physicsObj.GetBounds();
	const float radius = Max( Max( -bounds[0].x, bounds[1].x ), Max( -bounds[0].y, bounds[1].y ) );
	const float reach = radius + FLOW_WALL_MARGIN;
	idBounds box( origin );
	box.ExpandSelf( reach );

	const int num = aas->GetWallQuads( flowAreaNum, box, TFL_WALK, 0.0f, quads, FLOW_WALL_QUADS );
	const idVec2 p = origin.ToVec2();
	idVec2 push( 0.0f, 0.0f );
	for( int i = 0; i < Min( num, FLOW_WALL_QUADS ); i++ )
	{
		// the front edge runs verts[1] -> verts[0]; the walkable side is where ( dir.y, -dir.x ) points
		const idVec2& a = quads[i].verts[1];
		const idVec2& b = quads[i].verts[0];
		idVec2 edge = b - a;
		const float edgeLen = edge.Normalize();
		if( edgeLen < 0.001f )
		{
			continue;
		}
		const idVec2 normal( edge.y, -edge.x );
		if( ( p - a ) * normal <= 0.0f )
		{
			continue;
		}
		const float along = idMath::ClampFloat( 0.0f, edgeLen, ( p - a ) * edge );
		idVec2 away = p - ( a + edge * along );
		const float dist = away.Length();
		if( dist >= reach || dist < 0.001f )
		{
			continue;
		}
		push += ( away / dist ) * ( 1.0f - dist / reach );
	}
	if( push.x == 0.0f && push.y == 0.0f )
	{
		return;
	}

	idVec2 dir = seekPos.ToVec2() - p;
	const float len = dir.Normalize();
	if( len < 1.0f )
	{
		return;
	}
	dir += push;
	if( dir.Normalize() < 0.001f )
	{
		return;
	}
	seekPos.x = p.x + dir.x * len;
	seekPos.y = p.y + dir.y * len;
}

//puncho start - ai-combat (plans/ai-combat/08-PLAN-shooter-tokens.md, WP-T2/T3): the shooter tokens
/*
===============================================================================

	Shooter tokens (plans/ai-combat/08-PLAN-shooter-tokens.md)

	A monster tests a ranged shot only while it holds a token, so the traced tests per tic are bounded by the tokens, not
	by the crowd. One pool per player (the monsters attacking that player, ranked by their flat distance to it) and one
	per team for the monsters attacking monsters (ranked by their flat distance to their own enemy). Once per tic after
	the think loop the pass ranks every monster that asked lately, has an enemy in a capped pool and is not on its
	decision clock, and every pool keeps its best cap: a top-cap insertion under the total order ( key, entity number ),
	so the set does not depend on the walk order. A holder inside ai_shooterTokenHoldMs ranks first. A failed test puts
	the monster on the clock, so its token goes on at the next pass to the next monster whose line may be clear.

===============================================================================
*/

const int	MAX_SHOOTER_TEAMS	= 8;								// teams 0..7 each get a pool; a higher team shares the last one
const int	MAX_SHOOTER_POOLS	= MAX_CLIENTS + MAX_SHOOTER_TEAMS;	// one per player, then one per team
const int	MAX_SHOOTER_TOKENS	= 32;								// per pool: the upper bound of both cap cvars
const int	SHOOTER_ASK_MS		= 100;								// a monster that has not asked for this long wants no shot
const int	SHOOTER_LEASE_MS	= 1000;								// the same for a holder between a passed test and its attack

/*
============
AI_ShooterPoolCap

  The cap of a pool, 0 = no cap: ai_shooterTokensPlayer for a player's pool, ai_shooterTokensTeam for a team's
============
*/
static int AI_ShooterPoolCap( int pool )
{
	const int cap = ( pool < MAX_CLIENTS ) ? ai_shooterTokensPlayer.GetInteger() : ai_shooterTokensTeam.GetInteger();
	return idMath::ClampInt( 0, MAX_SHOOTER_TOKENS, cap );
}

/*
=====================
idAI::ShooterPool
=====================
*/
int idAI::ShooterPool() const
{
	const idActor* enemyEnt = enemy.GetEntity();
	if( enemyEnt == NULL )
	{
		return -1;
	}
	if( enemyEnt->IsType( idPlayer::Type ) && enemyEnt->entityNumber < MAX_CLIENTS )
	{
		return enemyEnt->entityNumber;
	}
	return MAX_CLIENTS + idMath::ClampInt( 0, MAX_SHOOTER_TEAMS - 1, team );
}

/*
=====================
idAI::ShooterMayTest

  The token gate of canHitEnemyFromAnim, after the touching test and before the decision clock. True when the tokens
  are off, for a sniper and for a monster whose pool has no cap; otherwise it records the ask (the pass ranks only
  monsters that asked) and says whether this monster holds a token of the last pass
=====================
*/
bool idAI::ShooterMayTest()
{
	if( !ai_shooterTokens.GetBool() )
	{
		return true;
	}
	if( shooterSniper )
	{
		gameLocal.aiStats.shooterSniperAsks++;
		return true;
	}
	const int pool = ShooterPool();
	assert( pool >= 0 );	// the caller returned on a NULL enemy
	if( AI_ShooterPoolCap( pool ) == 0 )
	{
		return true;
	}
	shooterAskTime = gameLocal.time;
	if( gameLocal.ShooterTokensValid() && shooterTokenGen == gameLocal.shooterTokenGen )
	{
		return true;
	}
	gameLocal.aiStats.shooterDenied++;
	return false;
}

struct shooterSlot_t
{
	float		key;			// flat distance squared to the enemy; -1 for a holder inside its hold
	int			entityNumber;	// the tie-break
};

struct shooterPool_t
{
	int				num;
	shooterSlot_t	slots[MAX_SHOOTER_TOKENS];	// ascending by AI_ShooterBefore
};

/*
============
AI_ShooterBefore
============
*/
static ID_INLINE bool AI_ShooterBefore( const shooterSlot_t& a, const shooterSlot_t& b )
{
	if( a.key != b.key )
	{
		return a.key < b.key;
	}
	return a.entityNumber < b.entityNumber;
}

/*
============
AI_ShooterInsert

  Keeps the best cap slots of a pool, sorted: an insertion into at most cap slots, no full sort; cap is at least 1
============
*/
static void AI_ShooterInsert( shooterPool_t& pool, int cap, const shooterSlot_t& slot )
{
	int i;
	if( pool.num < cap )
	{
		i = pool.num++;
	}
	else
	{
		if( !AI_ShooterBefore( slot, pool.slots[cap - 1] ) )
		{
			return;
		}
		i = cap - 1;	// the last one drops out
	}
	while( i > 0 && AI_ShooterBefore( slot, pool.slots[i - 1] ) )
	{
		pool.slots[i] = pool.slots[i - 1];
		i--;
	}
	pool.slots[i] = slot;
}

/*
=====================
idAI::AssignShooterTokens

  Once per tic after the think loop, while the merged player PVS is up (CanHitRetryMs reads the tier). A monster is ranked
  when it is alive, shown, not a sniper, has an enemy in a capped pool, asked within SHOOTER_ASK_MS (SHOOTER_LEASE_MS for a
  holder whose test said yes and that has not attacked since) and is not on its decision clock: a holder that failed is
  on the clock and lets its token go here, the yield. O(entities) plus an insertion into at most 32 slots per candidate;
  nothing allocated
=====================
*/
void idAI::AssignShooterTokens()
{
	PROFILE_STATIC_DESC( descTokens, "Minions::Combat::Tokens", ::Optick::Category::AI );
	idAIProfileScope scopeTokens( gameLocal.aiStats.usShooterTokens, descTokens );

	shooterPool_t	pools[MAX_SHOOTER_POOLS];
	int				caps[MAX_SHOOTER_POOLS];
	for( int p = 0; p < MAX_SHOOTER_POOLS; p++ )
	{
		pools[p].num = 0;
		caps[p] = AI_ShooterPoolCap( p );
	}

	const int now = gameLocal.time;
	const int gen = gameLocal.shooterTokenGen;		// the pass whose tokens this tic's thinks read
	const int holdMs = ai_shooterTokenHoldMs.GetInteger();
	int candidates = 0;
	int yields = 0;
	int expired = 0;

	for( idEntity* ent = gameLocal.spawnedEntities.Next(); ent != NULL; ent = ent->spawnNode.Next() )
	{
		if( !ent->IsType( idAI::Type ) )
		{
			continue;
		}
		idAI* ai = static_cast<idAI*>( ent );
		if( ai->health <= 0 || ai->IsHidden() || ai->shooterSniper )
		{
			continue;	// a token it held lapses: the new generation is not stamped on it
		}
		const int pool = ai->ShooterPool();
		if( pool < 0 || caps[pool] == 0 )
		{
			continue;
		}
		const bool held = ( ai->shooterTokenGen == gen );
		// a holder between a passed test and its attack may be winding up or turning (the mancubus waits 0.4 s before its
		// burst re-tests): it keeps the token for SHOOTER_LEASE_MS without asking; anyone else must have asked just now
		const bool attacking = held && ai->shooterPassTime > ai->shooterTokenTime && ai->shooterPassTime > ai->lastAttackTime;
		if( ai->shooterAskTime < 0 || now - ai->shooterAskTime > ( attacking ? SHOOTER_LEASE_MS : SHOOTER_ASK_MS ) )
		{
			expired += held ? 1 : 0;
			continue;
		}
		// the yield: a failed test put it on the decision clock, which would refuse its next test anyway
		if( ai->canHitFailTime >= 0 && now - ai->canHitFailTime < ai->CanHitRetryMs() )
		{
			yields += held ? 1 : 0;
			continue;
		}
		shooterSlot_t slot;
		slot.entityNumber = ai->entityNumber;
		if( held && now - ai->shooterTokenTime < holdMs )
		{
			slot.key = -1.0f;	// inside its hold: no challenger takes it
		}
		else
		{
			const idVec3& enemyOrigin = ai->enemy.GetEntity()->GetPhysics()->GetOrigin();
			slot.key = ( enemyOrigin.ToVec2() - ai->physicsObj.GetOrigin().ToVec2() ).LengthSqr();
		}
		candidates++;
		AI_ShooterInsert( pools[pool], caps[pool], slot );
	}

	// the new generation: the monsters in kept slots hold a token; every other one's generation is stale
	gameLocal.shooterTokenGen = gen + 1;
	gameLocal.shooterTokenTic = gameLocal.framenum;
	int holders = 0;
	int grants = 0;
	for( int p = 0; p < MAX_SHOOTER_POOLS; p++ )
	{
		for( int i = 0; i < pools[p].num; i++ )
		{
			idAI* ai = static_cast<idAI*>( gameLocal.entities[pools[p].slots[i].entityNumber] );
			if( ai->shooterTokenGen != gen )
			{
				ai->shooterTokenTime = now;		// a new holder: its hold starts here
				grants++;
			}
			ai->shooterTokenGen = gen + 1;
			holders++;
		}
	}
	gameLocal.aiStats.shooterCandidates += candidates;
	gameLocal.aiStats.shooterHolders += holders;
	gameLocal.aiStats.shooterGrants += grants;
	gameLocal.aiStats.shooterYields += yields;
	gameLocal.aiStats.shooterExpired += expired;
}
//puncho end
