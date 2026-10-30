#ifndef __GAME_MAPLANDMARKS_H__
#define __GAME_MAPLANDMARKS_H__

struct landmark_t
{
	idStr id, kind;
	idVec3 origin;
	idMat3 axis;
	int flags;
	float radius;
	idStr link, sealedBy;
	int ordinal;
};

struct transitionVolume_t
{
	idBounds bounds;
	idStr landmark;
	int cap;
	bool carryAll;
	int ordinal;
};

//puncho - P2b.1.10j: the map-only kinds an entityDef names in "mapOnlyKind" (base/def/mapgraph.def): such an entity is
// never spawned, and the game reads its keys from the raw .map
enum mapOnlyKind_t
{
	MAPONLY_NONE,
	MAPONLY_LANDMARK,	// info_landmark: a doorway (landmarks)
	MAPONLY_VOLUME,		// trigger_transition: a carry volume (transitionVolumes)
	MAPONLY_LANDING		// info_landing: a party landing slot (idGameLocal's landing pick)
};

//puncho - c15b (P2b.1.10j): one party landing slot of a parsed map, classed once by ReadMapData for the landing pick
// (idGameLocal::SelectTransitionSpawn) and g_showTransition: its landmark and origin as the map spells them, its index
// the def's default where the map leaves it out
struct mapLanding_t
{
	idStr landmark;
	idVec3 origin;
	int index;
	int ordinal;		// the map entity it was read from; the rows are in map order
};

//puncho - P2b.1.10f: one dungeon_variant group of a parsed map: the group as its first entity spelled it, and the
// choices its entities name, sorted and unique by idStr::Icmp (idMapGraphDirector::InhibitSpawn picks one by index)
struct mapVariantGroup_t
{
	idStr group;
	idStrList choices;
};

//puncho - P2b.1.1b: the landmark and transition-volume tables of one parsed map. gameLocal.mapLandmarks holds the
// loaded map's: LoadMap reads them with each parse of the map, a restore's LoadMap too, and MapLoaded adds the town
// portal; derived per load, never saved. mapGraphCheck and a node map's fingerprint read other maps into an instance of
// their own.
class idMapLandmarks
{
public:
	void Clear()
	{
		landmarks.Clear();
		transitionVolumes.Clear();
		variantGroups.Clear();	//puncho - P2b.1.10f
		landings.Clear();		//puncho - c15b
	}
	void ReadMapData( const idMapFile* map );
	static mapOnlyKind_t MapOnlyKind( const char* classname, const idDict** defaults );	//puncho - P2b.1.10j
	const landmark_t* FindLandmark( const char* name ) const;
	const transitionVolume_t* FindTransitionVolume( const char* name ) const;
	int NumLandmarks() const
	{
		return landmarks.Num();
	}
	const landmark_t& GetLandmark( int i ) const
	{
		return landmarks[i];
	}
	int NumTransitionVolumes() const
	{
		return transitionVolumes.Num();
	}
	const transitionVolume_t& GetTransitionVolume( int i ) const
	{
		return transitionVolumes[i];
	}
	//puncho start - c15b
	int NumLandings() const
	{
		return landings.Num();
	}
	const mapLanding_t& Landing( int i ) const
	{
		return landings[i];
	}
	//puncho end
	void RemoveById( const char* id );
	const idStrList* FindVariantChoices( const char* group ) const;	//puncho - P2b.1.10f: NULL when no entity names the group
	void Add( const landmark_t& landmark )
	{
		landmarks.Append( landmark );
	}
	void AddVolume( const transitionVolume_t& volume )
	{
		transitionVolumes.Append( volume );
	}

private:
	idList<landmark_t, TAG_GAME> landmarks;
	idList<transitionVolume_t, TAG_GAME> transitionVolumes;
	idList<mapVariantGroup_t, TAG_GAME> variantGroups;	//puncho - P2b.1.10f
	idList<mapLanding_t, TAG_GAME> landings;			//puncho - c15b
};
#endif
