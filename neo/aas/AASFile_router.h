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
#ifndef __AASFILE_ROUTER_H__
#define __AASFILE_ROUTER_H__

/*
===============================================================================

	The router map: a loaded AAS file as the baked router (d3xp/ai/AAS_router.h) sees it for ONE
	travel-flag mask. Plain integer arrays, made the same way by the map compiler (which writes them
	to a file for the bake tool) and by the game at map load.

===============================================================================
*/
// one eligible reachability
struct aasRouterEdge_t
{
	int						from;			// area it leaves
	int						to;				// area it enters
	unsigned int			time;			// its time for the router (see idAASRouterMap::Build)
	int						ordinal;		// its position in the loaded reachability list of its area (0 = first)
};

class idAASRouterMap
{
public:
	int						numAreas;		// areas of the file; area 0 is a dummy
	int						travelFlags;	// the mask the map was built for
	idList<aasRouterEdge_t>	edges;			// the eligible reachabilities: area by area ascending, each area's in the order of its loaded list
	idList<int>				out;			// numAreas + 1 entries: edges[ out[a] ] .. edges[ out[a + 1] - 1 ] leave area a
	idList<int>				in;				// numAreas + 1 entries: rev[ in[a] ] .. rev[ in[a + 1] - 1 ] are the edges that enter area a
	idList<int>				rev;			// one entry per edge: edge numbers, grouped by the area they enter, ascending inside a group
	idList<byte>			enabled;		// per area: 1 = an area of this mask
	idList<byte>			mutableArea;	// per area: 1 = it can be shut and opened (cluster portal or obstacle contents)
	idList<unsigned int>	origin;			// per edge: the time from the centre of its area to its start point
	idList<idReachability*>	reach;			// per edge: the file's reachability

	idAASRouterMap();
	void					Clear();
	// Builds the map from a loaded file. Returns false and sets error when the file cannot be routed
	// (a time out of range, a reachability that names no area). mask: TFL_WALK | TFL_AIR, or with TFL_FLY.
	bool					Build( idAASFile* file, int mask, idStr& error );
	// Writes the map for the bake tool. The format is in AASFile_router.cpp. Returns false on a write error.
	bool					Write( idFile* f ) const;
};

#endif /* !__AASFILE_ROUTER_H__ */
