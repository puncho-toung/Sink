//puncho - SA3-F (R-SA3-3 to R-SA3-11, R-SA3-27): the listener-rooted flood over SB3's portal graph and its loss tables
#ifndef __SND_OCCLUSION_H__
#define __SND_OCCLUSION_H__

/*
===============================================================================

	idSoundOcclusion exists only while a baked map is loaded: the begin hook makes it after SB3 has built the portal
	graph, and the end hook deletes it. A map with no bake has none, so nothing here runs or is allocated there.

	Once per gated frame BeginFrame decides whether the flood is dirty (a portal state changed, the listener moved or
	its area changed, or one of the five loss cvars changed) and, only then, starts a new one from the listener's area.
	The flood is Dijkstra on path length (R-SA3-7): ExtendTo runs it on demand until every label nearer than the range
	asked for is settled or discarded, or the pool is full; a later, larger
	range resumes it and never redoes a settled label. A clean frame keeps every settled label; a request on it for a
	larger range than the flood's extends the frontier. Every order is fixed by content: edges in SB3's order, labels
	in the order they were made, heap ties by label index.

	Main thread only (W1 and E1 of the gated bodies); nothing here is saved or sent.

===============================================================================
*/

class idSoundAcoustics;
class idSoundWorldLocal;
class idSoundEmitterLocal;
struct sndPortalEdge_t;
struct sndOccLogSlot_t;		//puncho - SA3-E (DR-193): private 32-byte log scratch, defined in the source

static const int SND_OCC_MAX_LABELS = 1024;		// R-SA3-6: the label pool, 64 KB; the flood stops when it is full

// R-SA3-6: a route from an area toward the listener, leaving the area through one exit edge. 64 bytes
struct sndOccLabel_t
{
	idVec3		point;				// on the exit portal
	idVec3		dirOut;				// unit vector from point toward the next point, or the listener
	idVec3		lastPoint;			// on the listener-adjacent portal
	float		dist;				// path length in units from point to the listener
	float		loss[3];			// accumulated dB, low / mid / high, each clamped to s_occlusionMaxLossDb
	int32		area;
	int32		exitEdge;			// a GetEdges() index, one of area's own edges: edges[ exitEdge ].otherArea is the next
									// area toward the listener (the listener's area at a root). It is the edge today's
									// ResolveOrigin takes out of area, the same winding
	int16		parent;				// label index; -1 at a root
	uint8		hops;				// portals from point to the listener's area: 1 at a root
	uint8		pad;
};
compile_time_assert( sizeof( sndOccLabel_t ) == 64 );

// R-SA3-6: one per area. A slot whose stamp is not the current flood's is empty. 12 bytes
struct sndOccAreaSlot_t
{
	uint32		stamp;
	int16		label[3];			// the area's settled labels, in the order they settled (least dist first)
	uint8		count;
	uint8		pad;
};
compile_time_assert( sizeof( sndOccAreaSlot_t ) == 12 );

// one heap entry: a label made and not yet popped. 8 bytes
struct sndOccHeapEntry_t
{
	float		dist;
	int32		label;
};
compile_time_assert( sizeof( sndOccHeapEntry_t ) == 8 );

// R-SA3-25: what SA3 hands to the log for each voiced channel; SB3's V row reads it
struct sndOccLogVoice_t		// 16 bytes
{
	uint8	numRoutes;		// routes kept, 0..3; 0 when direct or unreachable
	uint8	flags;			// bit 0 direct, bit 1 unreachable, bit 2 the flood in use this frame stopped at the cap, bit 3 doorway scale < 1
	uint16	pad;
	float	lossDb[3];		// combined loss, low / mid / high
};
compile_time_assert( sizeof( sndOccLogVoice_t ) == 16 );

class idSoundOcclusion
{
public:
	// creates world.occlusion (new TAG_AUDIO), allocates the label pool, the heap, one slot per area and the
	// reverse-edge table (Init)
	static void	OnMapAcousticsLoaded( idSoundWorldLocal& world, const idSoundAcoustics& acoustics );
	// zeroes portalLossDb and portalOcclusion on every emitter of world, deletes world.occlusion, sets it NULL
	static void	OnMapAcousticsFreed( idSoundWorldLocal& world );

							idSoundOcclusion();		// zeroes only
							~idSoundOcclusion();	// frees what Init allocated

	// C-SB82: the label pool, the heap, one slot per area and the reverse-edge table (F6-1: one int32 per directed edge)
	// in one TAG_AUDIO block; a fatal error when it fails. acoustics must outlive this object (the end hooks delete
	// it first)
	void					Init( const idSoundAcoustics& acoustics );

	//puncho - SA3-E (DR-195): logEmitters > 0 grows the log scratch in 64-slot steps and stamps this update,
	// even with a clean flood; 0 frees it. A failed log allocation is fatal.
	// Float NaNs or infinities use each cvar's default before comparisons and stores.
	// W1, once per gated frame: reads the eight cvars into members, then decides dirty (R-SA3-5; DR-183: a change of
	// one of the five loss cvars is dirty too). Dirty: a new stamp, the heap, the pool and the range emptied, the roots
	// planted (R-SA3-7 step 1) under SndOccFlood; none when listenerArea is -1. Clean: the flood is left as it is.
	// listenerArea is listener.area after PlaceListener. It does not run steps 2 to 6
	void					BeginFrame( const idSoundAcoustics& acoustics, const idVec3& listenerPos, int listenerArea, int logEmitters );
	// R-SA3-7 steps 2 to 6, R-SA3-8: settles or discards every label nearer than range (units), until the pool is
	// full; a range at or below the largest
	// asked for in this flood returns at once. A call with work opens SndOccFlood: SA3-E calls this before it opens its
	// own SndOccEmitter scope, so that the two scopes do not nest
	void					ExtendTo( float range );

	//puncho - SA3-E (R-SA3-12 to R-SA3-15): E1's five targets only, then a log slot when enabled. soundInArea is
	// E6's area result (-1 also disables occlusion); maxDistance is Update's range in metres, not the reference's
	void					ResolveEmitter( idSoundEmitterLocal& emitter, int soundInArea, float maxDistance );

	// the settled labels of area in the current flood (0 when its slot is stale or area is out of range), least dist first
	int						GetAreaLabels( int area, const sndOccLabel_t* out[3] ) const;
	const sndOccLabel_t&	Label( int index ) const;
	// the current flood stopped at SND_OCC_MAX_LABELS; it stays set while that flood is reused (the log's bit 2)
	bool					LabelCapHit() const;

	// today's rule (ResolveOrigin): where the line from from through toward meets q's plane, slid into q's winding;
	// q's centre, not slid, when the line is parallel to the plane, on it or off it. Reads no member
	static idVec3			SlideOntoPortal( const sndPortalEdge_t& q, const idVec3& from, const idVec3& toward );
	// R-SA3-9: 0 when q is open; a shut q gives its door record's three values, else the three shut cvars. Not clamped
	void					PortalLoss( const sndPortalEdge_t& q, float out[3] ) const;
	// R-SA3-10: adds the bend between the unit vectors in and out (0 degrees = straight) to loss. Not clamped
	void					Bend( const idVec3& in, const idVec3& out, float loss[3] ) const;
	// R-SA3-11: clamp( ( |p - x| - 8 ) / 24, 0, 1 ) for a shut q with no door record, else 1
	float					DoorwayScale( const sndPortalEdge_t& q, const idVec3& p, const idVec3& x ) const;

	// the cvars as BeginFrame read them this frame
	int						GetRoutes() const;			// s_occlusionRoutes, 1 to 3
	float					GetMinCutoff() const;		// s_occlusionMinCutoff, Hz
	float					GetMuffle() const;			//puncho - DR-206: s_occlusionMuffle, mid-band fraction, 0 to 2
	float					GetMaxLossDb() const;		// s_occlusionMaxLossDb: the clamp of every summed band (R-SA3-27)

	bool					GetLogVoice( const idSoundEmitterLocal* emitter, sndOccLogVoice_t& out ) const;

private:
	void					PushLabel( sndOccLabel_t& made, int crossedEdge );

	const idSoundAcoustics*	acoustics;			// Init's; read only while this object lives
	byte*					block;				// one Mem_Alloc16: labels, heap, slots, reverseEdge
	sndOccLabel_t*			labels;				// SND_OCC_MAX_LABELS, in the order they were made
	sndOccHeapEntry_t*		heap;				// SND_OCC_MAX_LABELS: every label made and not yet popped
	sndOccAreaSlot_t*		slots;				// numAreas
	int32*					reverseEdge;		// NumEdges(): the other directed edge of each edge's double portal; -1 none
	int						numAreas;
	int						numLabels;
	int						numHeap;
	uint32					stamp;				// the current flood's
	float					requested;			// the largest range asked for in this flood, kept across calls
	idVec3					lastListenerPos;	// the inputs of the current flood
	int						lastListenerArea;	// an area no listener has until the first BeginFrame (D-49)
	bool					capHit;				// the current flood stopped at SND_OCC_MAX_LABELS
	bool					capWarned;			// the cap has warned on this map
	// the cvars, read once per gated frame by BeginFrame; the flood and the helpers read only these
	int						routes;
	float					shutLossDb[3];
	float					bendScale;
	float					maxLossDb;
	float					minCutoff;
	float					muffleScale;		//puncho - DR-206: fills the 4-byte gap before logSlots on x64

	//puncho - SA3-E (R-SA3-25, DR-193): absent while logging is off, freed with the map's object
	sndOccLogSlot_t*			logSlots;
	int						numLogSlots;
	uint32					logFrame;			// this update, independent of the flood's stamp (a clean frame reuses it)
#if defined( USE_SOUND_MIXER )
	//puncho - W1-R: no constructor edit above the source's profile events.
	uint64					wallUpdateUs = 0;
	int						wallRays = 2;
	float					wallRadius = 48.0f;
	float					wallScale = 1.0f;
	bool					wallShow = false;
	int						wallCounts[3] = {}; // emitters, blocked centre rays, all point traces

	void					ResolveWalls( idSoundEmitterLocal& emitter );
	bool					WallRay( const idVec3& start, const idVec3& end, const idVec3& source,
								idRenderWorld* renderWorld, float loss[2], int& hits );
	template< bool draw >
	bool					TraceWallRay( const idVec3& start, const idVec3& end, const idVec3& source,
								idRenderWorld* renderWorld, float loss[2], int& hits );
public:
	static void				ListWallRays( const idSoundWorldLocal& world );
#endif
};

ID_INLINE int idSoundOcclusion::GetAreaLabels( int area, const sndOccLabel_t* out[3] ) const
{
	if( area < 0 || area >= numAreas || slots[area].stamp != stamp )
	{
		return 0;
	}
	const sndOccAreaSlot_t& slot = slots[area];
	for( int i = 0; i < slot.count; i++ )
	{
		out[i] = &labels[ slot.label[i] ];
	}
	return slot.count;
}

ID_INLINE const sndOccLabel_t& idSoundOcclusion::Label( int index ) const
{
	assert( index >= 0 && index < numLabels );
	return labels[index];
}

ID_INLINE bool idSoundOcclusion::LabelCapHit() const
{
	return capHit;
}

ID_INLINE int idSoundOcclusion::GetRoutes() const
{
	return routes;
}

ID_INLINE float idSoundOcclusion::GetMinCutoff() const
{
	return minCutoff;
}

ID_INLINE float idSoundOcclusion::GetMuffle() const
{
	return muffleScale;
}

ID_INLINE float idSoundOcclusion::GetMaxLossDb() const
{
	return maxLossDb;
}

#endif /* !__SND_OCCLUSION_H__ */
