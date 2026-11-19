//puncho - SA3-F (R-SA3-3 to R-SA3-11, R-SA3-27; D-49): the listener-rooted flood, the losses and the eight s_occlusion cvars
#include "precompiled.h"
#pragma hdrstop

#include "snd_local.h"	//puncho - SA3-E: the emitter targets and the world's hook-owned pointer
#include "snd_acoustics.h"
#include "snd_occlusion.h"

/*
===============================================================================

	Nothing in this file runs on a map with no bake: no idSoundOcclusion exists there. Its one cost on every map is the
	registration of the eight cvars below at start-up.

	The flood is a function of the listener's position and area, the portal states, the five loss cvars (a change of
	any is a dirty frame) and the largest range asked of it in this flood, whatever the order of the asks. Its orders
	are all by content: the roots in the order of the listener area's edges, a label's children in the order of its
	area's edges (SB3's order, GetPortal's), labels numbered in the order they are made, and heap ties broken by that
	number, so no two entries ever compare equal. No pointer value or hash order is read.

===============================================================================
*/

//puncho - R-SA3-9, R-SA3-10, R-SA3-27 and R-SA3-13's cutoff floor and muffle: read once per gated frame by BeginFrame,
// never by the flood or the helpers. None changes a bake; none is saved or sent
idCVar s_occlusionRoutes( "s_occlusionRoutes", "3", CVAR_SOUND | CVAR_INTEGER, "routes kept per sound on a baked map", 1, 3, idCmdSystem::ArgCompletion_Integer<1, 3> );
//puncho - SA3-E (DR-194): ranges are enforced on cvar sets, with no per-resolve checks
idCVar s_occlusionShutLow( "s_occlusionShutLow", "6", CVAR_SOUND | CVAR_FLOAT, "dB a shut portal with no door record takes off the low band (400 Hz) on a baked map", 0.0f, 60.0f );
idCVar s_occlusionShutMid( "s_occlusionShutMid", "12", CVAR_SOUND | CVAR_FLOAT, "dB a shut portal with no door record takes off the mid band (2.5 kHz) on a baked map", 0.0f, 60.0f );
idCVar s_occlusionShutHigh( "s_occlusionShutHigh", "24", CVAR_SOUND | CVAR_FLOAT, "dB a shut portal with no door record takes off the high band (15 kHz) on a baked map", 0.0f, 60.0f );
idCVar s_occlusionBendScale( "s_occlusionBendScale", "1.0", CVAR_SOUND | CVAR_FLOAT, "scale of the loss a route takes where it bends at a portal on a baked map", 0.0f, 4.0f );
idCVar s_occlusionMaxLossDb( "s_occlusionMaxLossDb", "60", CVAR_SOUND | CVAR_FLOAT, "largest loss in dB of any band of a route on a baked map", 0.0f, 120.0f );
idCVar s_occlusionMinCutoff( "s_occlusionMinCutoff", "250", CVAR_SOUND | CVAR_FLOAT, "lowest cutoff in Hz of the occlusion low-pass on a baked map", 20.0f, 6124.0f );
//puncho - SA3-E (DR-206): live fit control; changes no flood label
idCVar s_occlusionMuffle( "s_occlusionMuffle", "2", CVAR_SOUND | CVAR_FLOAT, "fraction of a route's mid-band loss the occlusion low-pass reproduces on a baked map", 0.0f, 2.0f );

//puncho - SA3-E (R-SA3-25, DR-193): natural alignment, indexed by the emitter's current index
struct sndOccLogSlot_t
{
	const idSoundEmitterLocal*	owner;
	sndOccLogVoice_t				record;
	uint32							frame;
	uint32							pad;
};
compile_time_assert( sizeof( sndOccLogSlot_t ) == 32 );

static const int	SND_OCC_MAX_HOPS = 10;			// today's MAX_PORTAL_TRACE_DEPTH (snd_world.cpp): a label of 10 hops has no child
static const int	SND_OCC_MAX_SETTLED = 3;		// R-SA3-7 step 3: settled labels per area
static const int	SND_OCC_NO_AREA = -2;			// D-49: an area no listener has (listener.area is -1 or a valid area)
static const int	SND_OCC_SHUT_BITS = PS_BLOCK_VIEW | PS_BLOCK_AIR;	// today's shut test; PS_BLOCK_LOCATION alone is open
static const float	SND_OCC_DOORWAY_NEAR = 8.0f;	// R-SA3-11, units: no loss this close to a shut opening with no door
static const float	SND_OCC_DOORWAY_RAMP = 24.0f;	// units over which the loss comes back in full
static const float	SND_OCC_MIN_SEGMENT = 1.0f;		// DR-183, units: a shorter segment has no direction of its own

// R-SA3-10: the bend table, inferred after the UTD curve of SA04.10; linear between rows, low / mid / high dB
static const int	SND_OCC_BEND_ROWS = 6;
static const float	sndOccBendDeg[ SND_OCC_BEND_ROWS ] = { 0.0f, 30.0f, 60.0f, 90.0f, 120.0f, 180.0f };
static const float	sndOccBendDb[ SND_OCC_BEND_ROWS ][ 3 ] =
{
	{ 0.0f, 0.0f, 0.0f },
	{ 0.5f, 1.5f, 3.0f },
	{ 1.5f, 4.0f, 7.0f },
	{ 3.0f, 7.0f, 11.0f },
	{ 4.5f, 9.0f, 14.0f },
	{ 6.0f, 12.0f, 18.0f }
};

/*
========================
SndOcc_HeapLess

Ties go to the label made first, so the pop order is fixed by the labels alone.
========================
*/
static ID_INLINE bool SndOcc_HeapLess( const sndOccHeapEntry_t& a, const sndOccHeapEntry_t& b )
{
	return ( a.dist < b.dist ) || ( a.dist == b.dist && a.label < b.label );
}

/*
========================
SndOcc_HeapPush

The heap holds at most every label of the pool, so it never overflows.
========================
*/
static void SndOcc_HeapPush( sndOccHeapEntry_t* heap, int& numHeap, const float dist, const int label )
{
	assert( numHeap < SND_OCC_MAX_LABELS );
	sndOccHeapEntry_t entry;
	entry.dist = dist;
	entry.label = label;
	int i = numHeap++;
	while( i > 0 )
	{
		const int up = ( i - 1 ) >> 1;
		if( !SndOcc_HeapLess( entry, heap[up] ) )
		{
			break;
		}
		heap[i] = heap[up];
		i = up;
	}
	heap[i] = entry;
}

/*
========================
SndOcc_HeapPop

Removes and returns the label of least dist.
========================
*/
static int SndOcc_HeapPop( sndOccHeapEntry_t* heap, int& numHeap )
{
	assert( numHeap > 0 );
	const int top = heap[0].label;
	const sndOccHeapEntry_t last = heap[--numHeap];
	int i = 0;
	for( ;; )
	{
		int down = 2 * i + 1;
		if( down >= numHeap )
		{
			break;
		}
		if( down + 1 < numHeap && SndOcc_HeapLess( heap[down + 1], heap[down] ) )
		{
			down++;
		}
		if( !SndOcc_HeapLess( heap[down], last ) )
		{
			break;
		}
		heap[i] = heap[down];
		i = down;
	}
	heap[i] = last;		// with the heap now empty this writes the freed slot 0, which nothing reads
	return top;
}

/*
========================
SndOcc_SlideIntoWinding

The slide of idSoundWorldLocal::ResolveOrigin (snd_world.cpp), alone and unchanged: a point on q's plane outside one of
the winding's edges is moved onto that edge's line, edge by edge in the winding's order. The one slide the roots and
SlideOntoPortal share (D-49).
========================
*/
static void SndOcc_SlideIntoWinding( const sndPortalEdge_t& q, idVec3& point )
{
	const idWinding& w = *q.w;
	const int numPoints = w.GetNumPoints();
	for( int i = 0; i < numPoints; i++ )
	{
		const int j = ( i + 1 ) % numPoints;
		const idVec3 edgeDir = w[j].ToVec3() - w[i].ToVec3();
		idVec3 edgeNormal;
		edgeNormal.Cross( q.plane.Normal(), edgeDir );
		float d = edgeNormal * ( point - w[j].ToVec3() );
		if( d > 0.0f )
		{
			const float div = edgeNormal.Normalize();
			d /= div;
			point -= d * edgeNormal;
		}
	}
}

/*
========================
SndOcc_NearestOnWinding

R-SA3-7 step 4: the point of q's winding nearest p. The projection onto q's plane when it lies inside the winding, else
the nearest point of the winding's edges, the first edge kept on equal distances.
========================
*/
static idVec3 SndOcc_NearestOnWinding( const sndPortalEdge_t& q, const idVec3& p )
{
	const idWinding& w = *q.w;
	const idVec3 projected = p - q.plane.Distance( p ) * q.plane.Normal();
	if( w.PointInside( q.plane.Normal(), projected, 0.0f ) )
	{
		return projected;
	}
	const int numPoints = w.GetNumPoints();
	idVec3 nearest = w[0].ToVec3();
	float nearestSqr = ( p - nearest ).LengthSqr();
	for( int i = 0; i < numPoints; i++ )
	{
		const idVec3& a = w[i].ToVec3();
		const idVec3 edge = w[( i + 1 ) % numPoints].ToVec3() - a;
		const float edgeSqr = edge.LengthSqr();
		const float t = ( edgeSqr > 0.0f ) ? idMath::ClampFloat( 0.0f, 1.0f, ( ( p - a ) * edge ) / edgeSqr ) : 0.0f;
		const idVec3 onEdge = a + t * edge;
		const float onEdgeSqr = ( p - onEdge ).LengthSqr();
		if( onEdgeSqr < nearestSqr )
		{
			nearest = onEdge;
			nearestSqr = onEdgeSqr;
		}
	}
	return nearest;
}

//puncho start - SA3-E (R-SA3-23): the hooks own only this world's occlusion and emitter targets
/*
========================
idSoundOcclusion::OnMapAcousticsLoaded
========================
*/
void idSoundOcclusion::OnMapAcousticsLoaded( idSoundWorldLocal& world, const idSoundAcoustics& acoustics )
{
	assert( world.occlusion == NULL );
	idSoundOcclusion* made = new( TAG_AUDIO ) idSoundOcclusion;
	if( made == NULL )
	{
		common->FatalError( "sound occlusion: %s: no memory for the object", acoustics.GetMapName() );
		return;
	}
	made->Init( acoustics );
	world.occlusion = made;
}

/*
========================
idSoundOcclusion::OnMapAcousticsFreed
========================
*/
void idSoundOcclusion::OnMapAcousticsFreed( idSoundWorldLocal& world )
{
	for( int i = 0; i < world.emitters.Num(); i++ )
	{
		world.emitters[i]->portalLossDb = 0.0f;
		world.emitters[i]->portalOcclusion = 0.0f;
	}
	delete world.occlusion;
	world.occlusion = NULL;
}
//puncho end

/*
========================
idSoundOcclusion::idSoundOcclusion
========================
*/
idSoundOcclusion::idSoundOcclusion()
{
	acoustics = NULL;
	block = NULL;
	labels = NULL;
	heap = NULL;
	slots = NULL;
	reverseEdge = NULL;
	numAreas = 0;
	numLabels = 0;
	numHeap = 0;
	stamp = 0;
	requested = 0.0f;
	lastListenerPos.Zero();
	lastListenerArea = SND_OCC_NO_AREA;
	capHit = false;
	capWarned = false;
	routes = 0;
	shutLossDb[0] = 0.0f;
	shutLossDb[1] = 0.0f;
	shutLossDb[2] = 0.0f;
	bendScale = 0.0f;
	maxLossDb = 0.0f;
	minCutoff = 0.0f;
	muffleScale = 0.0f;	//puncho - DR-206
	logSlots = NULL;		//puncho - SA3-E (R-SA3-25)
	numLogSlots = 0;
	logFrame = 0;
}

/*
========================
idSoundOcclusion::~idSoundOcclusion
========================
*/
idSoundOcclusion::~idSoundOcclusion()
{
	Mem_Free16( block );
	Mem_Free( logSlots );	//puncho - SA3-E (R-SA3-25)
}

/*
========================
idSoundOcclusion::Init

C-SB82: the 64 KB pool, the 8 KB heap, 12 bytes per area and 4 bytes per directed edge in one block, inside the begin
hook at map load.
========================
*/
void idSoundOcclusion::Init( const idSoundAcoustics& acousticsIn )
{
	assert( block == NULL );
	acoustics = &acousticsIn;
	numAreas = acousticsIn.NumAreas();
	const int numEdges = acousticsIn.NumEdges();
	const int numPortals = acousticsIn.NumPortals();
	const size_t labelBytes = SND_OCC_MAX_LABELS * sizeof( sndOccLabel_t );
	const size_t heapBytes = SND_OCC_MAX_LABELS * sizeof( sndOccHeapEntry_t );
	const size_t slotBytes = ( size_t )numAreas * sizeof( sndOccAreaSlot_t );
	// the build of the reverse-edge table holds one int32 per double portal for a moment, at the front of the block,
	// where the pool, the heap and the slots are written only after it: the block grows only when the portals'
	// int32s outnumber that room (more than 18,432 portals plus 3 per area)
	const size_t firstEdgeBytes = ( size_t )numPortals * sizeof( int32 );
	const size_t reverseOffset = Max( labelBytes + heapBytes + slotBytes, firstEdgeBytes );
	const size_t blockBytes = reverseOffset + ( size_t )numEdges * sizeof( int32 );
	block = ( byte* )Mem_Alloc16( blockBytes, TAG_AUDIO );
	if( block == NULL )
	{
		common->FatalError( "sound occlusion: %s: no memory for the flood (%d bytes)", acousticsIn.GetMapName(), ( int )blockBytes );
	}
	labels = ( sndOccLabel_t* )block;
	heap = ( sndOccHeapEntry_t* )( block + labelBytes );
	slots = ( sndOccAreaSlot_t* )( block + labelBytes + heapBytes );
	reverseEdge = ( int32* )( block + reverseOffset );

	// F6-1: reverseEdge[ e ] is the other directed edge of e's double portal, so PushLabel finds a label's exit edge
	// without a search. One pass over the edges: a portal's first edge waits in firstEdge, its second pairs with it.
	// SB3's graph gives every double portal exactly two edges (2 x NumPortals() edges, none a third on its portal); an
	// edge left at -1 is that promise broken, and PushLabel pushes no label across it
	assert( numEdges == 2 * numPortals );
	int32* firstEdge = ( int32* )block;
	for( int p = 0; p < numPortals; p++ )
	{
		firstEdge[p] = -1;
	}
	const sndPortalEdge_t* edges = acousticsIn.GetEdges();
	for( int e = 0; e < numEdges; e++ )
	{
		reverseEdge[e] = -1;
		const int portal = edges[e].portal;
		assert( portal >= 0 && portal < numPortals );
		const int first = firstEdge[portal];
		if( first == -1 )
		{
			firstEdge[portal] = e;
		}
		else if( reverseEdge[first] == -1 )
		{
			reverseEdge[first] = e;
			reverseEdge[e] = first;
		}
		else
		{
			assert( false );		// a third edge on one double portal
		}
	}

	// stamp 0 and count 0: every slot reads empty until the first flood. After the table: the slots may lie on firstEdge
	memset( slots, 0, slotBytes );
	stamp = 0;
	numLabels = 0;
	numHeap = 0;
	requested = 0.0f;
	capHit = false;
	capWarned = false;
	// D-49: the first BeginFrame of the map is dirty
	lastListenerPos.Zero();
	lastListenerArea = SND_OCC_NO_AREA;
}

/*
========================
idSoundOcclusion::BeginFrame

W1 of the gated world body, once per gated frame. It grows, stamps or frees the log scratch from logEmitters (DR-195,
DR-196) and reads the seven float cvars, taking a cvar's default when its value is not finite, before any compare or
store (DR-195). R-SA3-5: dirty when
a portal's state, the listener's position or area, or one of the five loss cvars (DR-183) changed; else reused as is.
Dirty: a new stamp makes every slot stale; the pool, heap and range are emptied and roots planted (R-SA3-7 step 1).
The flood itself runs in ExtendTo.
========================
*/
void idSoundOcclusion::BeginFrame( const idSoundAcoustics& acousticsIn, const idVec3& listenerPos, const int listenerArea, const int logEmitters )
{
	assert( &acousticsIn == acoustics );

	//puncho start - SA3-E (R-SA3-25, DR-195, DR-196): W1 passes this world's emitter count while the log is on, else 0;
	// 0 also when the world has no emitter. 0 frees the scratch; it is made again when a logged emitter appears. Grow
	// before the emitter loop, whose collapses can only shrink the list; stamp even a clean flood.
	if( logEmitters > 0 )
	{
		const int needed = ( logEmitters + 63 ) & ~63;
		if( needed > numLogSlots )
		{
			sndOccLogSlot_t* made = ( sndOccLogSlot_t* )Mem_Alloc( ( size_t )needed * sizeof( sndOccLogSlot_t ), TAG_AUDIO );
			if( made == NULL )
			{
				common->FatalError( "sound occlusion: %s: no memory for %d log slots", acousticsIn.GetMapName(), needed );
				return;
			}
			memset( made, 0, ( size_t )needed * sizeof( sndOccLogSlot_t ) );
			// No old record belongs to this update, so growth needs no copy.
			Mem_Free( logSlots );
			logSlots = made;
			numLogSlots = needed;
		}
		if( ++logFrame == 0 )
		{
			if( logSlots != NULL )
			{
				memset( logSlots, 0, ( size_t )numLogSlots * sizeof( sndOccLogSlot_t ) );
			}
			logFrame = 1;
		}
	}
	else if( logSlots != NULL )
	{
		Mem_Free( logSlots );
		logSlots = NULL;
		numLogSlots = 0;
		logFrame = 0;
	}
	//puncho end

	// the cvars, once per gated frame: the flood and SA3-E's helpers read these members only. The five the flood's losses
	// use are compared with the values the last flood used first: a change makes the frame dirty (DR-183), so a clean
	// flood always holds this frame's values. Routes, the cutoff and muffle do not dirty the flood.
	//puncho - SA3-E (DR-195): IsNAN also catches infinities; sanitize before dirty comparisons or cached stores.
	float shutLow = s_occlusionShutLow.GetFloat();
	if( IsNAN( shutLow ) )
	{
		shutLow = ( float )atof( s_occlusionShutLow.GetDefaultString() );
	}
	float shutMid = s_occlusionShutMid.GetFloat();
	if( IsNAN( shutMid ) )
	{
		shutMid = ( float )atof( s_occlusionShutMid.GetDefaultString() );
	}
	float shutHigh = s_occlusionShutHigh.GetFloat();
	if( IsNAN( shutHigh ) )
	{
		shutHigh = ( float )atof( s_occlusionShutHigh.GetDefaultString() );
	}
	float bend = s_occlusionBendScale.GetFloat();
	if( IsNAN( bend ) )
	{
		bend = ( float )atof( s_occlusionBendScale.GetDefaultString() );
	}
	float maxLoss = s_occlusionMaxLossDb.GetFloat();
	if( IsNAN( maxLoss ) )
	{
		maxLoss = ( float )atof( s_occlusionMaxLossDb.GetDefaultString() );
	}
	float cutoff = s_occlusionMinCutoff.GetFloat();
	if( IsNAN( cutoff ) )
	{
		cutoff = ( float )atof( s_occlusionMinCutoff.GetDefaultString() );
	}
	//puncho - DR-206: read even on a clean frame, so a live change reaches the next resolve
	float muffle = s_occlusionMuffle.GetFloat();
	if( IsNAN( muffle ) )
	{
		muffle = ( float )atof( s_occlusionMuffle.GetDefaultString() );
	}
	const bool lossCvarsChanged = ( shutLow != shutLossDb[0] || shutMid != shutLossDb[1] || shutHigh != shutLossDb[2] ||
									bend != bendScale || maxLoss != maxLossDb );
	shutLossDb[0] = shutLow;
	shutLossDb[1] = shutMid;
	shutLossDb[2] = shutHigh;
	bendScale = bend;
	maxLossDb = maxLoss;
	routes = s_occlusionRoutes.GetInteger();
	minCutoff = cutoff;
	muffleScale = muffle;	//puncho - DR-206: not a flood input

	// D-46: PortalStatesChanged is this update's, not a first-frame flag; D-49 makes the first frame dirty by the area
	if( !lossCvarsChanged && !acousticsIn.PortalStatesChanged() && listenerArea == lastListenerArea && listenerPos.Compare( lastListenerPos ) )
	{
		return;
	}
	lastListenerPos = listenerPos;
	lastListenerArea = listenerArea;

	if( ++stamp == 0 )
	{
		// the stamp wrapped: no slot may keep a stamp from 2^32 floods ago
		memset( slots, 0, ( size_t )numAreas * sizeof( sndOccAreaSlot_t ) );
		stamp = 1;
	}
	numLabels = 0;
	numHeap = 0;
	requested = 0.0f;
	capHit = false;

	// D-49: in no area (A-SB4-11's case) no root is planted, so every slot stays stale; the next frame with an area
	// differs in area and is dirty
	if( listenerArea < 0 || listenerArea >= numAreas )
	{
		return;
	}

	// DR-183: every piece of flood work is under one scope, the roots too
	SCOPED_PROFILE_EVENT( "SndOccFlood" );

	// R-SA3-7 step 1: for each edge q of the listener's area, a root label of the area q leads to
	const sndPortalEdge_t* edges = acoustics->GetEdges();
	const sndPortalArea_t& listenerEdges = acoustics->GetAreaEdges()[ listenerArea ];
	for( int e = listenerEdges.firstEdge, end = e + listenerEdges.numEdges; e < end && !capHit; e++ )
	{
		const sndPortalEdge_t& q = edges[e];
		sndOccLabel_t made;
		// the listener projected onto q's plane, then today's slide alone
		made.point = listenerPos - q.plane.Distance( listenerPos ) * q.plane.Normal();
		SndOcc_SlideIntoWinding( q, made.point );
		made.dist = ( made.point - listenerPos ).Length();
		PortalLoss( q, made.loss );
		const float scale = DoorwayScale( q, listenerPos, made.point );
		for( int b = 0; b < 3; b++ )
		{
			made.loss[b] = Min( made.loss[b] * scale, maxLossDb );
		}
		if( made.dist < SND_OCC_MIN_SEGMENT )
		{
			// DR-183: the listener on q's plane. The limit of "toward the listener" from L's side is q's normal: q is an
			// edge of L, and a directed portal's plane faces into the area that lists it (dmap writes area0 on the
			// positive side of the winding's plane, the load gives a2 the reversed winding, and the renderer's portal
			// flood skips a portal whose plane the viewer is behind)
			made.dirOut = q.plane.Normal();
		}
		else
		{
			made.dirOut = listenerPos - made.point;
			made.dirOut.Normalize();
		}
		made.lastPoint = made.point;
		made.area = q.otherArea;
		made.parent = -1;
		made.hops = 1;
		made.pad = 0;
		PushLabel( made, e );
	}
}

/*
========================
idSoundOcclusion::ExtendTo

R-SA3-7 steps 2 to 6 and R-SA3-8, from E1: settles or discards every label nearer than range, until the pool is full.
Every child is pushed whatever its
dist (DR-183), so the heap keeps the ones beyond the range and a later, larger range resumes from it as the last call
left it; a settled label is final. Every call stops at a point of one sequence of pops and pushes, so what is settled
below a range does not depend on the order of the calls. At most SND_OCC_MAX_LABELS labels, each settled label
tests its area's edges once (at most SND_OCC_MAX_LABELS x k_max edge tests), at most 3 settled labels per area.
========================
*/
void idSoundOcclusion::ExtendTo( const float range )
{
	// the range requested so far is the largest asked for in this flood, kept across calls: a range at or below it is met
	if( range <= requested )
	{
		return;
	}
	requested = range;
	if( capHit || numHeap == 0 || heap[0].dist >= range )
	{
		return;
	}

	SCOPED_PROFILE_EVENT( "SndOccFlood" );

	const sndPortalEdge_t* edges = acoustics->GetEdges();
	const sndPortalArea_t* areaEdges = acoustics->GetAreaEdges();
	// step 5: stop at an empty heap, at a least dist at or past the range, or at a full pool
	while( !capHit && numHeap > 0 && heap[0].dist < range )
	{
		// step 2
		const int index = SndOcc_HeapPop( heap, numHeap );
		const sndOccLabel_t& label = labels[index];		// the pool never moves: PushLabel writes other indices

		// step 3: discard when the area holds 3 settled labels or one with the same exit edge; else settle
		sndOccAreaSlot_t& slot = slots[ label.area ];
		if( slot.stamp != stamp )
		{
			slot.stamp = stamp;
			slot.count = 0;
		}
		if( slot.count >= SND_OCC_MAX_SETTLED )
		{
			continue;
		}
		bool sameExit = false;
		for( int i = 0; i < slot.count && !sameExit; i++ )
		{
			sameExit = ( labels[ slot.label[i] ].exitEdge == label.exitEdge );
		}
		if( sameExit )
		{
			continue;
		}
		slot.label[ slot.count++ ] = ( int16 )index;

		// step 4: a child through each other edge of the area, below 10 hops
		if( label.hops >= SND_OCC_MAX_HOPS )
		{
			continue;
		}
		const sndPortalArea_t& from = areaEdges[ label.area ];
		for( int e = from.firstEdge, end = e + from.numEdges; e < end && !capHit; e++ )
		{
			if( e == label.exitEdge )
			{
				continue;		// the arrival edge
			}
			const sndPortalEdge_t& q = edges[e];

			// skip the listener's area, where a route is complete (DR-183; today's walk ends there), and an area already
			// on the parent chain: the chain of a label below 10 hops is at most 9 labels
			bool onChain = ( q.otherArea == lastListenerArea );
			for( int i = index, steps = 0; i != -1 && steps < SND_OCC_MAX_HOPS && !onChain; i = labels[i].parent, steps++ )
			{
				onChain = ( labels[i].area == q.otherArea );
			}
			if( onChain )
			{
				continue;
			}

			sndOccLabel_t made;
			made.point = SndOcc_NearestOnWinding( q, label.point );
			const float segment = ( made.point - label.point ).Length();
			made.dist = label.dist + segment;
			// the parent's loss, q's loss and the bend at the parent's point, each band clamped (R-SA3-27)
			PortalLoss( q, made.loss );
			for( int b = 0; b < 3; b++ )
			{
				made.loss[b] += label.loss[b];
			}
			if( segment < SND_OCC_MIN_SEGMENT )
			{
				// DR-183: two touching portals; the route goes straight through, no bend at the parent's point
				made.dirOut = label.dirOut;
			}
			else
			{
				made.dirOut = label.point - made.point;
				made.dirOut.Normalize();
				Bend( made.dirOut, label.dirOut, made.loss );
			}
			for( int b = 0; b < 3; b++ )
			{
				made.loss[b] = Min( made.loss[b], maxLossDb );
			}
			made.lastPoint = label.lastPoint;
			made.area = q.otherArea;
			made.parent = ( int16 )index;
			made.hops = ( uint8 )( label.hops + 1 );
			made.pad = 0;
			PushLabel( made, e );
		}
	}
}

//puncho start - SA3-E (R-SA3-12 to R-SA3-15, R-SA3-25; DR-193)
/*
========================
idSoundOcclusion::ResolveEmitter

E1, before UpdateVolume and the world's voice cut. No smoother or persistent route cache: only this update's targets.
========================
*/
void idSoundOcclusion::ResolveEmitter( idSoundEmitterLocal& emitter, const int soundInArea, const float maxDistance )
{
	const idSoundWorldLocal& world = *emitter.soundWorld;
	// Step 1: E6 leaves soundInArea at -1 when occlusion is disabled or every area retry misses. The arming test
	// comes first, so even a gated call made before LCUT_LOAD reads neither listener.area nor listener.pos.
	const bool direct = !acoustics->IsArmed() || soundInArea == -1 || soundInArea == world.listener.area;
	const float range = maxDistance * METERS_TO_DOOM;
	if( !direct )
	{
		// Step 2, before the emitter scope (D-50): SndOccFlood and SndOccEmitter never nest.
		ExtendTo( range );
	}
	// Step 5's direct defaults also give the no-route case a finite origin and no spectral loss.
	emitter.spatializedOrigin = emitter.origin;
	emitter.spatializedDistance = emitter.directDistance;
	emitter.portalLossDb = 0.0f;
	emitter.portalOcclusion = 0.0f;
	emitter.noPath = false;
	float combinedLoss[3] = { 0.0f, 0.0f, 0.0f };
	int numRoutes = 0;
	bool doorwayScaled = false;
	if( !direct )
	{
		SCOPED_PROFILE_EVENT( "SndOccEmitter" );	//puncho - DR-195: only after the direct cases and ExtendTo
		emitter.spatializedDistance = range * DOOM_TO_METERS;
		emitter.noPath = true;
		const sndOccLabel_t* areaLabels[3];
		const int count = Min( GetAreaLabels( soundInArea, areaLabels ), GetRoutes() );
		if( count > 0 )
		{
			struct sndOccRoute_t
			{
				idVec3	lastPoint;
				float	dist;
				float	loss[3];
				float	attenuation;
				float	weight;
			};
			sndOccRoute_t route[3];
			const sndPortalEdge_t* edges = acoustics->GetEdges();
			const float lossLimit = GetMaxLossDb();
			// Step 3: the settled-label order is fixed by SA3-F. Rejected routes do not reorder the survivors.
			for( int i = 0; i < count; i++ )
			{
				const sndOccLabel_t& l = *areaLabels[i];
				if( l.dist >= range )	//puncho - DR-195: a farther earlier emitter must not admit this label
				{
					continue;
				}
				idVec3 point = l.point;
				idVec3 lastPoint = l.lastPoint;
				idVec3 dirOut = l.dirOut;
				float remaining = l.dist;
				if( l.hops == 1 )
				{
					point = SlideOntoPortal( edges[ l.exitEdge ], emitter.origin, world.listener.pos );
					lastPoint = point;
					const idVec3 outgoing = world.listener.pos - point;
					remaining = outgoing.Length();
					if( remaining >= SND_OCC_MIN_SEGMENT )
					{
						dirOut = outgoing / remaining;
					}
				}
				idVec3 incoming = point - emitter.origin;
				const float segment = incoming.Length();
				const float distance = segment + remaining;
				if( distance >= range )
				{
					continue;
				}
				sndOccRoute_t& r = route[numRoutes++];
				r.lastPoint = lastPoint;
				r.dist = distance;
				memcpy( r.loss, l.loss, sizeof( r.loss ) );
				if( segment >= SND_OCC_MIN_SEGMENT )
				{
					incoming /= segment;
					Bend( incoming, dirOut, r.loss );
				}
				const float scale = DoorwayScale( edges[ l.exitEdge ], emitter.origin, point );
				if( scale < 1.0f )
				{
					float portalLoss[3];
					if( l.hops == 1 )	//puncho - DR-195: retain the root's listener-side scale
					{
						memcpy( portalLoss, l.loss, sizeof( portalLoss ) );
					}
					else
					{
						PortalLoss( edges[ l.exitEdge ], portalLoss );
					}
					for( int b = 0; b < 3; b++ )
					{
						r.loss[b] += portalLoss[b] * ( scale - 1.0f );
					}
					doorwayScaled = true;	// D-6: only the emitter-side scale, never the root's listener-side scale
				}
				for( int b = 0; b < 3; b++ )
				{
					r.loss[b] = Min( r.loss[b], lossLimit );
				}
			}
			if( numRoutes > 0 )
			{
				int best = 0;
				if( numRoutes == 1 )	//puncho - DR-195: one route needs neither a reference nor a power sum
				{
					for( int b = 0; b < 3; b++ )
					{
						combinedLoss[b] = Max( 0.0f, route[0].loss[b] );
					}
					emitter.spatializedOrigin = route[0].lastPoint;
				}
				else
				{
					// DR-193: a separate, occluded-only reference; the first channel wins a maxDistance tie. Update's
					// original range still bounds the flood and the path lengths, including a longer unoccluded channel.
					int reference = -1;
					for( int i = 0; i < emitter.channels.Num(); i++ )
					{
						const soundShaderParms_t& parms = emitter.channels[i]->parms;
						if( ( parms.soundShaderFlags & ( SSF_GLOBAL | SSF_NO_OCCLUSION ) ) != 0 )
						{
							continue;
						}
						if( reference == -1 || parms.maxDistance > emitter.channels[reference]->parms.maxDistance )
						{
							reference = i;
						}
					}
					assert( reference != -1 && emitter.emitterId != world.listener.id );
					const soundShaderParms_t& ref = emitter.channels[reference]->parms;
					// Step 4 (R-SA3-14, DR-193): the shared curve includes both ends and the -120 dB floor.
					for( int i = 0; i < numRoutes; i++ )
					{
						route[i].attenuation = SoundDistanceDB( route[i].dist * DOOM_TO_METERS, ref.minDistance, ref.maxDistance );
					}
					for( int i = 1; i < numRoutes; i++ )
					{
						if( route[i].attenuation - route[i].loss[0] > route[best].attenuation - route[best].loss[0] )
						{
							best = i;
						}
					}
					route[best].weight = 1.0f;
					//puncho - DR-203: the high band feeds only the log.
					const int numBands = ( logSlots != NULL ) ? 3 : 2;
					for( int b = 0; b < numBands; b++ )
					{
						float power = 1.0f;	//puncho - DR-195: the best route's term is exactly 1
						for( int i = 0; i < numRoutes; i++ )
						{
							if( i == best )
							{
								continue;
							}
							const float term = idMath::Pow( 10.0f, ( route[i].attenuation - route[best].attenuation +
																	route[best].loss[b] - route[i].loss[b] ) * 0.1f );
							power += term;
							if( b == 0 )
							{
								route[i].weight = term;
							}
						}
						// The best route contributes exactly 1, even if every other term underflows to 0: log never sees 0.
						combinedLoss[b] = Max( 0.0f, route[best].loss[b] - 10.0f * idMath::Log( power ) * 0.434294482f );
					}
					// V is last, after the band sums and their clamp. A point at the listener contributes no direction.
					idVec3 direction = vec3_zero;
					for( int i = 0; i < numRoutes; i++ )
					{
						idVec3 towardPortal = route[i].lastPoint - world.listener.pos;
						if( towardPortal.LengthSqr() > 0.0f )
						{
							towardPortal.Normalize();
							direction += route[i].weight * towardPortal;
						}
					}
					emitter.spatializedOrigin = route[best].lastPoint;
					if( direction.LengthSqr() >= 1.0e-6f )
					{
						direction.Normalize();
						emitter.spatializedOrigin = world.listener.pos + direction * ( route[best].lastPoint - world.listener.pos ).Length();
					}
				}
				//puncho - Step 5 (R-SA3-13, DR-201, DR-206): metres and broadband loss; muffle scales the mid-band
				// excess fitted at 2.5 kHz. No positive scaled excess keeps the zero target.
				emitter.spatializedDistance = route[best].dist * DOOM_TO_METERS;
				emitter.portalLossDb = combinedLoss[0];
				const float delta = GetMuffle() * ( combinedLoss[1] - combinedLoss[0] );
				if( delta > 0.0f )
				{
					const float excessPower = idMath::Pow( 10.0f, delta * 0.1f ) - 1.0f;
					if( excessPower > 0.0f )
					{
						//puncho - DR-203: Sqrt's positive branch, without another Sqrt call site.
						const float cutoff = Max( 2500.0f / ( excessPower * idMath::InvSqrt( excessPower ) ),
												  GetMinCutoff() );
						emitter.portalOcclusion = 1000.0f / cutoff;
					}
				}
				emitter.noPath = false;
			}
		}
	}

	// R-SA3-25: the only log writes are under this update's flag (a non-NULL array). A slot collapse after this
	// resolve changes the index, so GetLogVoice also checks the owner before handing anything to SB3-T.
	if( logSlots != NULL )
	{
		assert( emitter.index >= 0 && emitter.index < numLogSlots );
		sndOccLogSlot_t& slot = logSlots[emitter.index];
		slot.owner = &emitter;
		slot.record.numRoutes = ( uint8 )numRoutes;
		slot.record.flags = ( uint8 )( ( direct ? BIT( 0 ) : 0 ) | ( emitter.noPath ? BIT( 1 ) : 0 ) |
									  ( LabelCapHit() ? BIT( 2 ) : 0 ) | ( doorwayScaled ? BIT( 3 ) : 0 ) );
		slot.record.pad = 0;
		memcpy( slot.record.lossDb, combinedLoss, sizeof( combinedLoss ) );
		slot.frame = logFrame;
		slot.pad = 0;
	}
}

/*
========================
idSoundOcclusion::GetLogVoice
========================
*/
bool idSoundOcclusion::GetLogVoice( const idSoundEmitterLocal* emitter, sndOccLogVoice_t& out ) const
{
	if( logSlots == NULL || emitter == NULL || emitter->index < 0 || emitter->index >= numLogSlots )
	{
		return false;
	}
	const sndOccLogSlot_t& slot = logSlots[emitter->index];
	if( slot.frame != logFrame || slot.owner != emitter )
	{
		return false;
	}
	out = slot.record;
	return true;
}
//puncho end

/*
========================
idSoundOcclusion::PushLabel

Gives made its exit edge, puts it in the pool and pushes it. crossedEdge is the edge the flood crossed into made's area;
the exit edge is the other directed edge of its double portal, made's area's own edge back (F6-1: read from the table,
no search). The label that fills the pool stops the flood (R-SA3-7 steps 5 and 6).
========================
*/
void idSoundOcclusion::PushLabel( sndOccLabel_t& made, const int crossedEdge )
{
	assert( numLabels < SND_OCC_MAX_LABELS && made.area >= 0 && made.area < numAreas );
	assert( crossedEdge >= 0 && crossedEdge < acoustics->NumEdges() );
	made.exitEdge = reverseEdge[ crossedEdge ];
	if( made.exitEdge == -1 )
	{
		assert( false );		// SB3's graph gives every double portal one edge in each of its areas
		return;
	}
	// the exit edge is one of made's area's own edges (SB3 lists an area's edges in one run)
	assert( made.exitEdge >= acoustics->GetAreaEdges()[ made.area ].firstEdge &&
			made.exitEdge < acoustics->GetAreaEdges()[ made.area ].firstEdge + acoustics->GetAreaEdges()[ made.area ].numEdges );
	const int index = numLabels++;
	labels[index] = made;
	SndOcc_HeapPush( heap, numHeap, made.dist, index );
	if( numLabels == SND_OCC_MAX_LABELS )
	{
		capHit = true;
		if( !capWarned )
		{
			capWarned = true;
			common->DWarning( "sound occlusion: %s: the flood stopped at its %d labels; areas it did not reach are out of range", acoustics->GetMapName(), SND_OCC_MAX_LABELS );
		}
	}
}

/*
========================
idSoundOcclusion::SlideOntoPortal

Today's rule of ResolveOrigin, for SA3-E's one-hop recompute (R-SA3-12): where the line from from through toward meets
q's plane, slid into q's winding. RayIntersection meets the whole line, behind from as well. It tests the direction
alone, so it rejects every line parallel to the plane, on the plane or off it (and from equal to toward); then the
answer is q's centre, not slid.
========================
*/
idVec3 idSoundOcclusion::SlideOntoPortal( const sndPortalEdge_t& q, const idVec3& from, const idVec3& toward )
{
	const idVec3 dir = toward - from;
	float scale;
	if( !q.plane.RayIntersection( from, dir, scale ) )
	{
		return q.center;
	}
	idVec3 point = from + scale * dir;
	SndOcc_SlideIntoWinding( q, point );
	return point;
}

/*
========================
idSoundOcclusion::PortalLoss

R-SA3-9, today's shut test. The edge's door is SB3's answer (R-SB3-27): set only where the record matches the portal,
and only the first record on it.
========================
*/
void idSoundOcclusion::PortalLoss( const sndPortalEdge_t& q, float out[3] ) const
{
	if( ( acoustics->GetPortalStates()[ q.portal ] & SND_OCC_SHUT_BITS ) == 0 )
	{
		out[0] = 0.0f;
		out[1] = 0.0f;
		out[2] = 0.0f;
		return;
	}
	if( q.door >= 0 )
	{
		const sndBakeDoor_t& door = acoustics->GetDoors()[ q.door ];
		out[0] = door.lossDb[0];
		out[1] = door.lossDb[1];
		out[2] = door.lossDb[2];
		return;
	}
	out[0] = shutLossDb[0];
	out[1] = shutLossDb[1];
	out[2] = shutLossDb[2];
}

/*
========================
idSoundOcclusion::Bend

R-SA3-10: theta is the angle between in and out, 0 degrees for a straight route.
========================
*/
void idSoundOcclusion::Bend( const idVec3& in, const idVec3& out, float loss[3] ) const
{
	// clamped before the arc cosine: two unit vectors' dot product can stray past 1 by rounding
	const float theta = RAD2DEG( idMath::ACos( idMath::ClampFloat( -1.0f, 1.0f, in * out ) ) );
	int row = 0;
	while( row < SND_OCC_BEND_ROWS - 2 && theta > sndOccBendDeg[ row + 1 ] )
	{
		row++;
	}
	const float f = idMath::ClampFloat( 0.0f, 1.0f, ( theta - sndOccBendDeg[row] ) / ( sndOccBendDeg[row + 1] - sndOccBendDeg[row] ) );
	for( int b = 0; b < 3; b++ )
	{
		loss[b] += bendScale * ( sndOccBendDb[row][b] + f * ( sndOccBendDb[row + 1][b] - sndOccBendDb[row][b] ) );
	}
}

/*
========================
idSoundOcclusion::DoorwayScale

R-SA3-11 (DR-51): a listener or a sound standing in a shut opening a player can walk through hears it fade in over
24 units. A door's loss is never reduced by anyone's distance to it.
========================
*/
float idSoundOcclusion::DoorwayScale( const sndPortalEdge_t& q, const idVec3& p, const idVec3& x ) const
{
	if( ( acoustics->GetPortalStates()[ q.portal ] & SND_OCC_SHUT_BITS ) == 0 || q.door >= 0 )
	{
		return 1.0f;
	}
	return idMath::ClampFloat( 0.0f, 1.0f, ( ( p - x ).Length() - SND_OCC_DOORWAY_NEAR ) / SND_OCC_DOORWAY_RAMP );
}
