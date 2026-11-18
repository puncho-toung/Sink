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

//puncho - R2 router: new file
#ifndef __AAS_ROUTER_H__
#define __AAS_ROUTER_H__

/*
===============================================================================

	The baked router, game side.

	A map may have router files beside an AAS file, one set per travel-flag mask
	("walk": TFL_WALK | TFL_AIR, "fly": TFL_WALK | TFL_AIR | TFL_FLY):
		<aas file name>.walk.nlw4	the baked lists									(needed)
		<aas file name>.walk.nld4	the door layer, when the map has wide doors		(optional)
		<aas file name>.walk.rtd	the door list: the areas that doors shut and open, as the files were
									baked with them. Text, nothing but area numbers.	(optional: no door)
		<aas file name>.fly.nlw4, <aas file name>.fly.nld4, <aas file name>.fly.rtd
	When a set loads, idAASLocal::RouteToGoalArea answers asks with that mask from it instead of flooding
	the routing cache. The files are baked by the standalone tool from the map the map compiler writes
	(aas/AASFile_router.h). The runtime is in libs/aasrouter/ and is the code that was tested standalone.

	Without a door layer the lists are asked through a table per goal area (libs/aasrouter/rt_goaltable.h):
	the game asks again at every area and every tic, and only the route that one ask fixes is proven, so the
	route of an ask is written out whole and its reaches are kept per area for the goal. aas_routerGoals says
	how many goals are kept (12 bytes per AAS area each). With a door layer every call is still a fresh ask.

	Exceptions. The runtime reports a refusal by throwing std::runtime_error with the refusal's text. Its
	headers are included by AAS_router.cpp and by nothing else, and no exception leaves a method of
	idAASRouter: a refusal is caught there, printed once, and switches that mask off, so the stock routing
	takes over. One refusal is the router's own: an area was shut that the door list does not name (the
	lists would still route across it).

	One thread. Like the routing cache, the router is asked and changed from the game thread only.

	Not like the stock routing: a goal area that is shut has no route (the stock flood still routes to a
	goal area that was shut after it was chosen).

===============================================================================
*/

class idAASRouterProfile;

class idAASRouter
{
public:
	idAASRouter();
	~idAASRouter();

	// Looks for router files beside the AAS file and loads the sets it finds. aasFileName is the name the
	// AAS file was loaded by (for example "maps/game/site3.aas48"). Returns the number of sets loaded.
	int						Init( idAASFile* file, const char* aasFileName );
	void					Shutdown();

	// Prints, per loaded set, whether it is on and how many asks it has answered (the command aasStats).
	void					PrintStats() const;

	// One ask. Returns false when no set serves this mask: the caller then routes the stock way.
	// Returns true when the router answered: found says whether a route exists under the current doors; if
	// it does and the areas differ, *reach is the route's first reachability and travelTime its time.
	bool					Route( int areaNum, int goalAreaNum, int travelFlags, bool& found, int& travelTime, idReachability** reach );

	// An area has been shut or opened (idAASLocal::DisableArea / EnableArea). Between BeginAreaChanges and
	// EndAreaChanges the changes are kept and made as ONE door change: a door of several areas shuts or
	// opens with one call of idAASLocal::SetAreaState, and a door change of the router costs by the call.
	void					BeginAreaChanges();
	void					SetAreaEnabled( int areaNum, bool enabled );
	void					EndAreaChanges();

private:
	idAASRouterProfile* 	profiles[2];	// walk, fly; NULL: not loaded
	idList<int, TAG_AAS>	changed;		// the areas of the door change that is being kept
	bool					changedEnabled;	// ... and what they have changed to
	int						keeping;		// BeginAreaChanges without its EndAreaChanges

	void					MakeAreaChanges();
};

#endif /* !__AAS_ROUTER_H__ */
