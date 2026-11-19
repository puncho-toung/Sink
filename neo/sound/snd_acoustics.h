//puncho - SB3 (R-SB3-30): the baked acoustics of one loaded map, as SB4 and SA3 read it
#ifndef __SND_ACOUSTICS_H__
#define __SND_ACOUSTICS_H__

#include "snd_acousticfile.h"

//puncho start - SB3-T (R-SB3-16, R-SB3-17, R-SB3-31): developer tools, off by default
extern idCVar s_acousticsShow;
extern idCVar s_acousticsLog;
void ListAcoustics_f( const idCmdArgs& args );
void AcousticsLog_Frame();
void AcousticsLog_Close();
//puncho end

/*
===============================================================================

	idSoundAcoustics is made by idSoundWorldLocal::LoadMapAcoustics when every check of the map's .sndbake has
	passed (step 10 of R-SB3-4) and deleted by FreeMapAcoustics or the world's destructor. Main thread only. Every
	pointer is valid from step 10 of the load to that delete; nothing is writable from outside SB3. The section
	pointers point into the step 1 block as read from the file: nothing is copied, converted or indexed but the
	portal graph.

===============================================================================
*/

struct sndPortalEdge_t					// 72 bytes, one per directed portal; static_assert
{
	const idWinding*  w;				// the render world's; lives until its next InitFromMap
	idPlane           plane;			// w->GetPlane(); orientation as the render world's winding gives it
	idVec3            center;			// w->GetCenter()
	idBounds          bounds;			// over w's points
	int32             otherArea;		// GetPortal().areas[1]
	int32             portal;			// double portal index = handle - 1
	int16             door;				// BAK_DOORS index or -1
	uint16            flags;			// 0 in step 1
};
struct sndPortalArea_t { int32 firstEdge; int32 numEdges; };   // 8 bytes
enum sndDoorUse_t { SND_DOOR_USED = 0, SND_DOOR_MISMATCH = 1, SND_DOOR_SHADOWED = 2 };

static_assert( sizeof( sndPortalEdge_t ) == 72, "sndPortalEdge_t is 72 B" );
static_assert( sizeof( sndPortalArea_t ) == 8, "sndPortalArea_t is 8 B" );

class idSoundAcoustics
{
public:
	const sndFileHeader_t&   GetHeader() const;
	const sndSettings_t&     GetSettings() const;               // GetHeader().settings
	int                      NumAreas() const;
	const sndBakeArea_t*     GetAreas() const;                  // NumAreas() records
	int                      NumCells() const;
	const int32*             GetCells() const;
	int                      NumProbes() const;
	const sndBakeProbe_t*    GetProbes() const;
	int                      NumZones() const;
	const sndReverb_t*       GetZones() const;
	int                      NumDoors() const;
	const sndBakeDoor_t*     GetDoors() const;
	const char*              GetDoorName( int door ) const;     // "" when out of range
	sndDoorUse_t             GetDoorUse( int door ) const;
	int                      NumEdges() const;                  // 2 x NumPortals()
	const sndPortalEdge_t*   GetEdges() const;
	const sndPortalArea_t*   GetAreaEdges() const;              // NumAreas() records
	int                      NumPortals() const;                // double portals
	const uint8*             GetPortalStates() const;           // NumPortals() blockingBits, this frame's read
	bool                     PortalStatesChanged() const;
	int                      MaxAreaEdges() const;              // k_max, with MaxAreaEdgesArea()
	int                      MaxAreaEdgesArea() const;
	bool                     IsArmed() const;
	const char*              GetMapName() const;
	uint64                   GetCompileId() const;
private:
	friend class idSoundWorldLocal;     // SB3's load, free, ListenerPass and tools
	byte*  block;  int blockBytes;     // step 1 block, Mem_Alloc16
	const sndBakeArea_t*  areas;  const int32* cells;  const sndBakeProbe_t* probes;
	const sndReverb_t*    zones;  const sndBakeDoor_t* doors;  const char* doorStrings;
	int    numAreas, numCells, numProbes, numZones, numDoors, doorStringBytes;
	byte*  graph;                       // one Mem_Alloc16: edges, area records, states, door uses
	sndPortalEdge_t* edges;  sndPortalArea_t* areaEdges;  uint8* states;  uint8* doorUse;
	int    numEdges, numPortals, maxAreaEdges, maxAreaEdgesArea;
	bool   statesChanged, armed;
	void*  mesh;                        //puncho - SB3-T (OP8-6): .sndmesh for bit 16, NULL until asked; refusal holds &acousticsMeshRefused, never freed
	idStrStatic< MAX_OSPATH > mapName;
};

// the accessors are inline: SB4's and SA3's gated lines read them every frame
ID_INLINE const sndFileHeader_t& idSoundAcoustics::GetHeader() const
{
	return *( const sndFileHeader_t* )block;
}

ID_INLINE const sndSettings_t& idSoundAcoustics::GetSettings() const
{
	return GetHeader().settings;
}

ID_INLINE int idSoundAcoustics::NumAreas() const
{
	return numAreas;
}

ID_INLINE const sndBakeArea_t* idSoundAcoustics::GetAreas() const
{
	return areas;
}

ID_INLINE int idSoundAcoustics::NumCells() const
{
	return numCells;
}

ID_INLINE const int32* idSoundAcoustics::GetCells() const
{
	return cells;
}

ID_INLINE int idSoundAcoustics::NumProbes() const
{
	return numProbes;
}

ID_INLINE const sndBakeProbe_t* idSoundAcoustics::GetProbes() const
{
	return probes;
}

ID_INLINE int idSoundAcoustics::NumZones() const
{
	return numZones;
}

ID_INLINE const sndReverb_t* idSoundAcoustics::GetZones() const
{
	return zones;
}

ID_INLINE int idSoundAcoustics::NumDoors() const
{
	return numDoors;
}

ID_INLINE const sndBakeDoor_t* idSoundAcoustics::GetDoors() const
{
	return doors;
}

ID_INLINE const char* idSoundAcoustics::GetDoorName( int door ) const
{
	if( door < 0 || door >= numDoors )
	{
		return "";
	}
	return doorStrings + doors[door].nameOffset;		// checked against the string table at load, step 7
}

ID_INLINE sndDoorUse_t idSoundAcoustics::GetDoorUse( int door ) const
{
	assert( doorUse != NULL && door >= 0 && door < numDoors );
	return ( sndDoorUse_t )doorUse[door];
}

ID_INLINE int idSoundAcoustics::NumEdges() const
{
	return numEdges;
}

ID_INLINE const sndPortalEdge_t* idSoundAcoustics::GetEdges() const
{
	return edges;
}

ID_INLINE const sndPortalArea_t* idSoundAcoustics::GetAreaEdges() const
{
	return areaEdges;
}

ID_INLINE int idSoundAcoustics::NumPortals() const
{
	return numPortals;
}

ID_INLINE const uint8* idSoundAcoustics::GetPortalStates() const
{
	return states;
}

ID_INLINE bool idSoundAcoustics::PortalStatesChanged() const
{
	return statesChanged;
}

ID_INLINE int idSoundAcoustics::MaxAreaEdges() const
{
	return maxAreaEdges;
}

ID_INLINE int idSoundAcoustics::MaxAreaEdgesArea() const
{
	return maxAreaEdgesArea;
}

ID_INLINE bool idSoundAcoustics::IsArmed() const
{
	return armed;
}

ID_INLINE const char* idSoundAcoustics::GetMapName() const
{
	return mapName.c_str();
}

ID_INLINE uint64 idSoundAcoustics::GetCompileId() const
{
	return GetHeader().compileId;
}

#endif /* !__SND_ACOUSTICS_H__ */
