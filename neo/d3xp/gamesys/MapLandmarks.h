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
	}
	void ReadMapData( const idMapFile* map );
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
	void RemoveById( const char* id );
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
};
#endif
