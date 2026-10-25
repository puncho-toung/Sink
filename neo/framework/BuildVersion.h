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

const int BUILD_NUMBER_SAVE_VERSION_BEFORE_SKIP_CINEMATIC       = 1400;
const int BUILD_NUMBER_SAVE_VERSION_CHANGE                      = 1401;
const int BUILD_NUMBER_SAVE_VERSION_SCRIPT_CHANGES1             = 1402;
const int BUILD_NUMBER_SAVE_VERSION_LIGHT_MODELTARGET_CHANGE    = 1403;
const int BUILD_NUMBER_SAVE_VERSION_MATH_LIGHTS                 = 1404;
const int BUILD_NUMBER_SAVE_VERSION_FUNC_DECAL                  = 1405;
const int BUILD_NUMBER_SAVE_VERSION_AI_SQUAD_PINS               = 1406;	// ai-avoidance (WP-A7, audit M2): the squad pins of sys.aiSetAvoid
const int BUILD_NUMBER_SAVE_VERSION_ENTITY_BITS_14              = 1407;		// 14-bit entity index: spawn ids in saves pack 18 + 14 bits (d3xp/Game_local.h)
const int BUILD_NUMBER_SAVE_VERSION_NET_CONTENT_CHECKSUM        = 1408;	// version rule (MP-A0, MD13): RELIABLE_LOADING_DONE carries idGame::GetContentDigests
const int BUILD_NUMBER_SAVE_VERSION_NET_REFUSAL_REASON          = 1409;	// version rule (MP-A0): OOB_GOODBYE_REFUSED tells a refused peer why (idLobby::RefusePeer)
const int BUILD_NUMBER_SAVE_VERSION_SLOT_GENERATION             = 1410;	// identity (P1-A2, K13): the save holds slotGen, the per-slot spawn-id generation, where spawnCount was; older saves are refused
const int BUILD_NUMBER_SAVE_VERSION_MOD_LAYERS                  = 1411;	// MP-A5 (K44): the save details hold the mod list and per-layer decl name counts and digests; older saves are refused
const int BUILD_NUMBER_SAVE_VERSION_NET_CONTENT_GLOBAL          = 1412;	// content check, global half (P1-A3a): RELIABLE_LOADING_DONE leads with the load id and adds MAP_ENTITIES, the decl digests hash decl text, implicit decls cross through GAME_RELIABLE_MESSAGE_DECL_REMAP
const int BUILD_NUMBER_SAVE_VERSION_NET_CONTENT_LAYERS          = 1413;	// content check, per-mod half (P1-A3b): the connect message carries the mod list; RELIABLE_LOADING_DONE carries four global digests and seven per content layer
const int BUILD_NUMBER_SAVE_VERSION_DAMAGABLE_SNAPSHOT          = 1414;	// P0-m10: idDamagable (func_damagable) is networkSync and writes its break to snapshots

const int BUILD_NUMBER = BUILD_NUMBER_SAVE_VERSION_DAMAGABLE_SNAPSHOT;
const int BUILD_NUMBER_MINOR = 0;