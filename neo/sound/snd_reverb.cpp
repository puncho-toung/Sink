//puncho - SB4-L (R-SB4-1, R-SB4-5 step 2, R-SB4-8 to R-SB4-14, R-SB4-26, R-SB4-30, R-SB4-31, R-SB4-34): the reverb's map hooks, the arm and the probe lookup
#include "precompiled.h"
#pragma hdrstop

#include "snd_local.h"
#include "snd_acoustics.h"

/*
===============================================================================

	The lookup (R-SB4-8 to R-SB4-14) runs at most once per update of the playing world's gated arm, at W1: once armed,
	and only with the listener in an area the bake has. Every order in it is
	fixed by content: the flood settles entries, not areas (DR-184), popping offers by least path length, ties by area
	number, then portal index, then the order they were offered; an area settles at most two entries, through different
	portals; edges in the graph's order (SB3's, by area then GetPortal); probes read after the flood, areas in the order
	they were visited, cells x fastest. No hash table, pointer value or allocation order is read, and the blend's
	denominator is at least 0.05, so equal positions and door states give equal targets to the bit, run to run, at a cut
	or once every portal's openness has settled. Bounded per update by 16 areas, 32 settled entries and 864 cells and
	probes (C-SB66). On a map with no bake nothing here runs but the member's construction and destruction: Init's memset
	from the world's constructor, Detach from its destructor (a free of NULL and DetachReverb); W1 is inside the gated
	arm, and the hooks run only for a baked map.

===============================================================================
*/

// R-SB4-30, H1 (SB4-C's line) and R-SB4-14. SB4-Z defines the other three; none changes a bake, none is saved or sent
#if defined( USE_SOUND_MIXER )
//puncho - C4-R: the voice already includes its gain, this is the room input level.
idCVar s_acousticReverbSend( "s_acousticReverbSend", "0.35", CVAR_SOUND | CVAR_FLOAT, "room input gain" );
idCVar s_acousticReverbFadeMs( "s_acousticReverbFadeMs", "400", CVAR_SOUND | CVAR_FLOAT, "room fade in ms", 50.0f, 2000.0f );
idCVar s_acousticReverbSendDistance( "s_acousticReverbSendDistance", "0", CVAR_SOUND | CVAR_FLOAT, "room send distance share", 0.0f, 1.0f );
#else
idCVar s_acousticReverbSend( "s_acousticReverbSend", "1.0", CVAR_SOUND | CVAR_FLOAT, "level of the send to the reverb from every voice of a baked map that is not global" );
#endif
idCVar s_acousticReverbDwellMs( "s_acousticReverbDwellMs", "250", CVAR_SOUND | CVAR_INTEGER, "ms a new zone must stay the listener's before the reverb takes it" );
idCVar s_acousticReverbHysteresis( "s_acousticReverbHysteresis", "32", CVAR_SOUND | CVAR_FLOAT, "the reverb also takes a new zone once the listener is farther than this many units from every portal into the old one" );

extern idCVar s_smoothMaxDtMs;		// snd_world.cpp, R-E6-3: the clamp of every measured update time

static const int	MAX_FLOOD_AREAS = 16;				// R-SB4-10: the flood visits at most this many areas (a hit warns once per map)
static const float	REVERB_OPEN_RAMP_MS = 150.0f;		// R-SB4-10, R-SB4-17: a portal opens or shuts over this many ms
static const float	REVERB_OPEN_MIN = 0.001f;			// R-SB4-10: a portal less open than this carries no path
static const float	REVERB_ARRIVE = 1e-4f;				// R-SB4-17: arrive, in the value's unit
static const float	REVERB_FLOOR_WEIGHT = 0.05f;		// R-SB4-13: w0 at S = 0 (inferred; tuning, not output)
static const float	REVERB_FLOOR_SPAN = 0.25f;			// R-SB4-13: w0 falls to 0 as S reaches this
static const float	REVERB_DB_MIN = -100.0f;			// R-SB2-15, R-SB2-16: the bake's own clamp of earlyDb and lateDb
static const float	REVERB_DB_MAX = 40.0f;
static const float	REVERB_ENERGY_MIN = 1e-10f;			// 10^( REVERB_DB_MIN / 10 )
static const float	REVERB_ABSENT = -1.0f;				// R-SB4-12: the energies of a record the blend skips
static const float	REVERB_CELL_LIMIT = 268435456.0f;	// 2^28 cells, far past any map: every cell index and difference fits an int
static const float	SND_REVERB_DOMINANCE_EPS = 0.001f;	// DR-187: units; the float rounding of the equality case of Reverb_Dominated

// R-SB4-34: a probe's or an area default's record as the blend reads it, 20 B
struct reverbBlendRecord_t
{
	float	lnDecay[ SND_NUM_BANDS ];	// ln decayTime[b]; b 0 low, 1 mid, 2 high
	float	earlyEnergy;				// 10^( earlyDb / 10 ); REVERB_ABSENT: an absent record
	float	lateEnergy;					// 10^( lateDb / 10 ); REVERB_ABSENT: an absent record
};
static_assert( sizeof( reverbBlendRecord_t ) == 20, "R-SB4-34: 20 B per record" );

// R-SB4-8: open[], one per double portal, 12 B (C-SB67)
struct reverbPortalOpen_t
{
	idSoundSmooth	open;				// 1 open, 0 shut; ramps over REVERB_OPEN_RAMP_MS, snaps on a cut, the arm or a visit after an update without one
	int32			lastVisit;			// the update (frame) in which the flood last stepped it
};
static_assert( sizeof( reverbPortalOpen_t ) == 12, "C-SB67: 12 B per double portal" );

// R-SB4-10: where a path from the listener reaches an area, how long it is and how open
struct reverbFloodEntry_t
{
	idVec3	q;							// L projected on the arrival portal's plane, clamped to its bounds; L itself for the root
	float	D;							// path length to q, units
	float	o;							// the openness of the portals along the path, multiplied
	int32	portal;						// the double portal the path came through; -1 for the root
};

// one area the flood visited in this lookup: the entries it settled, at most two, through different portals (DR-184)
struct reverbFloodArea_t
{
	int32				area;
	int32				numEntries;		// 1 or 2
	reverbFloodEntry_t	entry[ 2 ];		// in settle order, so D never decreases
};

// R-SB4-10 (DR-184): an entry offered to an area, waiting in the flood's frontier
struct reverbFloodOffer_t
{
	reverbFloodEntry_t	entry;
	int32				area;			// the area offered
	int32				order;			// the order it was pushed in (an offer dropped at the push takes none): the pop order's last key
};

// R-SB2-17's empty record: the blend's stand-in for an area default that is absent
static const sndReverb_t reverbEmptyRecord = { { 0.1f, 0.1f, 0.1f }, -100.0f, -100.0f, 0.0f, 0.0f, 1.0f };

/*
========================
Reverb_BlendRecord

R-SB4-12, R-SB4-34: a record in the blend's terms. A record is absent, marked so and false returned, when a field is not
finite or a decay time is 0 or less (its ln would be -inf or NaN). The load has checked every float finite but not bounded
(D-40), so the dB fields are held to the bake's own clamp: 10^( dB / 10 ) stays finite and above 0
========================
*/
static bool Reverb_BlendRecord( const sndReverb_t& r, reverbBlendRecord_t& out )
{
	bool present = SndFloatsFinite( &r, ( int )sizeof( r ) );
	for( int b = 0; b < SND_NUM_BANDS && present; b++ )
	{
		present = ( r.decayTime[b] > 0.0f );
	}
	if( !present )
	{
		for( int b = 0; b < SND_NUM_BANDS; b++ )
		{
			out.lnDecay[b] = 0.0f;
		}
		out.earlyEnergy = REVERB_ABSENT;
		out.lateEnergy = REVERB_ABSENT;
		return false;
	}
	for( int b = 0; b < SND_NUM_BANDS; b++ )
	{
		out.lnDecay[b] = idMath::Log( r.decayTime[b] );
	}
	out.earlyEnergy = idMath::Pow( 10.0f, idMath::ClampFloat( REVERB_DB_MIN, REVERB_DB_MAX, r.earlyDb ) / 10.0f );
	out.lateEnergy = idMath::Pow( 10.0f, idMath::ClampFloat( REVERB_DB_MIN, REVERB_DB_MAX, r.lateDb ) / 10.0f );
	return true;
}

/*
========================
Reverb_PortalPoint

R-SB4-10 step 3: L projected on the portal's plane, clamped to its bounds. Inside the portal it is L itself on the
plane, so path length is continuous as the listener crosses
========================
*/
static ID_INLINE idVec3 Reverb_PortalPoint( const sndPortalEdge_t& edge, const idVec3& L )
{
	idVec3 q = L - edge.plane.Normal() * edge.plane.Distance( L );
	q.Clamp( edge.bounds[0], edge.bounds[1] );
	return q;
}

/*
========================
Reverb_Cell

R-SB1b-8: the lattice cell of q, S and Sz as float, each coordinate held inside +-REVERB_CELL_LIMIT
========================
*/
static ID_INLINE void Reverb_Cell( const idVec3& q, float S, float Sz, int c[3] )
{
	c[0] = ( int )idMath::ClampFloat( -REVERB_CELL_LIMIT, REVERB_CELL_LIMIT, idMath::Floor( q.x / S ) );
	c[1] = ( int )idMath::ClampFloat( -REVERB_CELL_LIMIT, REVERB_CELL_LIMIT, idMath::Floor( q.y / S ) );
	c[2] = ( int )idMath::ClampFloat( -REVERB_CELL_LIMIT, REVERB_CELL_LIMIT, idMath::Floor( q.z / Sz ) );
}

/*
========================
Reverb_OfferLess

R-SB4-10 step 2 (DR-184): by D, then area number, then portal (the root's -1 first), then the order offered. The last
key is unique, so the order is total: equal inputs pop the same offers in the same order
========================
*/
static ID_INLINE bool Reverb_OfferLess( const reverbFloodOffer_t& a, const reverbFloodOffer_t& b )
{
	if( a.entry.D != b.entry.D )
	{
		return a.entry.D < b.entry.D;
	}
	if( a.area != b.area )
	{
		return a.area < b.area;
	}
	if( a.entry.portal != b.entry.portal )
	{
		return a.entry.portal < b.entry.portal;
	}
	return a.order < b.order;
}

/*
========================
Reverb_Dominated

R-SB4-10 (DR-187): true when an entry the area already settled, { q, D, o }, has D + |q - q'| <= D' + eps and o >= o'
against the offer { q', D', o' }. By the triangle inequality such an offer is no nearer to any probe than the settled
entry, to within eps, and no more open, so its weight is never greater but by what eps allows (a dropped offer can weigh
a hair more); dropping it only frees the place. The arrival-portal rule (DR-186) is its cheap case. Named limit
(DR-188): the test is complete only when the two openness values are equal, which they are once every portal's
openness has settled, so an entry that never weighs more can hold a second place only during a door's 150 ms ramp
========================
*/
static ID_INLINE bool Reverb_Dominated( const reverbFloodArea_t& settled, const reverbFloodEntry_t& offer )
{
	for( int k = 0; k < settled.numEntries; k++ )
	{
		const reverbFloodEntry_t& e = settled.entry[k];
		if( e.o >= offer.o && e.D + ( e.q - offer.q ).Length() <= offer.D + SND_REVERB_DOMINANCE_EPS )
		{
			return true;
		}
	}
	return false;
}

/*
========================
idSoundReverb::Init

C-SB6m: the member of every sound world, zeroed by the world's constructor. Until a baked map's begin hook nothing
else of it runs but Detach, which the world's destructor calls whether or not the world ever had a bake
========================
*/
void idSoundReverb::Init( idSoundWorldLocal* soundWorld )
{
	memset( this, 0, sizeof( *this ) );		// plain members only: no constructor, no virtual
	world = soundWorld;
	area = -1;
	zone = -1;
	candidate = -1;
}

/*
========================
idSoundReverb::FreeTables

The one release of the per-map tables (R-SB4-26, R-SB4-31), for the end hook, the detach and a begin that finds them
held. Forgets the map without reading it: after the detach the object is already deleted (D-48)
========================
*/
void idSoundReverb::FreeTables()
{
	Mem_Free16( tables );
	tables = NULL;
	blend = NULL;
	portalOpen = NULL;
	floodSlot = NULL;
	flood = NULL;
	frontier = NULL;
	frontierCapacity = 0;
	acoustics = NULL;
#if defined( USE_SOUND_MIXER )
	mixerCutPending = false; //puncho - C4-R
#endif
	armPending = false;
	armed = false;
	armDeferred = false;
	armThisUpdate = false;
	walkPending = false;
}

/*
========================
idSoundReverb::OnMapAcousticsLoaded

The begin hook (R-SB4-1, R-SB4-5 step 2, R-SB4-34), at step 11 of LoadMapAcoustics, on the load screen. It runs after
the load's cover has ended, so nothing here may raise an Error (D-43): a failed allocation warns and leaves the map
dry. The draining cancel of step 2 is SB4-Z's; the zombies, the lists and the owner are the back end's, last
========================
*/
void idSoundReverb::OnMapAcousticsLoaded( const idSoundAcoustics& bake )
{
	FreeTables();		// none held when the end hook and the detach have run
	acoustics = &bake;	// kept to the end hook (DR-78)

	// nothing of the last map carries over
	for( int i = 0; i < REVERB_NUM_TARGETS; i++ )
	{
		target[i] = 0.0f;
	}
	area = -1;
	zone = -1;
	candidate = -1;
	dwellMs = 0.0f;
	zonePreDelay = 0.0f;
	zoneLateDelay = 0.0f;
	zoneRoomSize = 0.0f;
	areasVisited = 0;
	cellsRead = 0;
	probesWeighted = 0;
	weightSum = 0.0f;
	floorWeight = 0.0f;
	dtMs = 0.0f;
	held = false;
	lastUpdateUs = 0;
	frame = 0;
	areaCapWarned = false;

	// one block: the blend table (C-SB65), open[] (C-SB67), and the flood's scratch: a slot per area, a record per
	// visited area, and the frontier. At most 2 * MAX_FLOOD_AREAS entries settle and each offers at most one entry per
	// edge of its area, so with the root one lookup offers at most 1 + 2 * MAX_FLOOD_AREAS * MaxAreaEdges() (DR-184)
	const int numProbes = bake.NumProbes();
	const int numAreas = bake.NumAreas();
	const int numPortals = bake.NumPortals();
	const int capacity = 1 + 2 * MAX_FLOOD_AREAS * bake.MaxAreaEdges();
	const size_t blendBytes = ( size_t )( numProbes + numAreas ) * sizeof( reverbBlendRecord_t );
	const size_t openBytes = ( size_t )numPortals * sizeof( reverbPortalOpen_t );
	const size_t slotBytes = ( size_t )numAreas * sizeof( int );
	const size_t floodBytes = ( size_t )MAX_FLOOD_AREAS * sizeof( reverbFloodArea_t );
	const size_t frontierBytes = ( size_t )capacity * sizeof( reverbFloodOffer_t );
	tables = ( byte* )Mem_Alloc16( blendBytes + openBytes + slotBytes + floodBytes + frontierBytes, TAG_AUDIO );
	if( tables == NULL )
	{
		// armPending stays false: no update arms and no voice is walked; the back end still pairs the end hook
		common->Warning( "acoustics: %s no memory for the reverb's tables (%d bytes); the map plays dry", bake.GetMapName(), ( int )( blendBytes + openBytes + slotBytes + floodBytes + frontierBytes ) );
		soundSystemLocal.hardware.BeginReverbMap( this );
		return;
	}
	blend = ( reverbBlendRecord_t* )tables;
	portalOpen = ( reverbPortalOpen_t* )( tables + blendBytes );
	floodSlot = ( int* )( tables + blendBytes + openBytes );
	flood = ( reverbFloodArea_t* )( tables + blendBytes + openBytes + slotBytes );
	frontier = ( reverbFloodOffer_t* )( tables + blendBytes + openBytes + slotBytes + floodBytes );
	frontierCapacity = capacity;

	// R-SB4-34, R-SB4-12: the blend table in one pass, probes then area defaults. An absent probe is skipped by the blend;
	// an absent area default becomes the empty record, so the floor term always has a value. One warning per map
	const sndBakeProbe_t* probes = bake.GetProbes();
	const sndBakeArea_t* areas = bake.GetAreas();
	int absentProbes = 0;
	int absentDefaults = 0;
	for( int p = 0; p < numProbes; p++ )
	{
		if( !Reverb_BlendRecord( probes[p].reverb, blend[p] ) )
		{
			absentProbes++;
		}
	}
	for( int a = 0; a < numAreas; a++ )
	{
		if( !Reverb_BlendRecord( areas[a].defaultReverb, blend[numProbes + a] ) )
		{
			Reverb_BlendRecord( reverbEmptyRecord, blend[numProbes + a] );
			absentDefaults++;
		}
	}
	if( absentProbes + absentDefaults > 0 )
	{
		common->Warning( "acoustics: %s %d probe and %d area default reverb records have a field not finite or a decay time of 0 or less; such a probe is not used, such a default is the empty record",
						 bake.GetMapName(), absentProbes, absentDefaults );
	}

	// every portal snaps at its first step (valid false); no area is in a lookup
	memset( portalOpen, 0, openBytes );
	for( int a = 0; a < numAreas; a++ )
	{
		floodSlot[a] = -1;
	}

	// R-SB4-8, R-SB1b-9: the lattice, and Rmax = min( S, Sz ), every probe's radius. The load checked both in [32, 1024]
	const sndSettings_t& settings = bake.GetSettings();
	cellSize = ( float )settings.probeSpacing;
	cellSizeZ = ( float )settings.probeSpacingZ;
	rMax = ( float )Min( settings.probeSpacing, settings.probeSpacingZ );

	armPending = true;		// this world's next update while it plays arms, or refuses with section 9's warning (D-48)

	// R-SB4-5's hardware part (SEAMS section 3): the zombies, the send lists, the owner and the carrier flag
	soundSystemLocal.hardware.BeginReverbMap( this );
}

/*
========================
idSoundReverb::OnMapAcousticsFreed

The end hook (R-SB4-26), at map shutdown inside FreeMapAcoustics, the object still alive. soundCarried is SEAMS section
3's test, the gated world still the playing one: true for a load that carries a sound, and for a live s_acoustics 0
from ListenerPass inside the playing world's update (D-48), where the drain is intended
========================
*/
void idSoundReverb::OnMapAcousticsFreed()
{
	FreeTables();
	const bool soundCarried = ( soundSystemLocal.currentSoundWorld == world );
	soundSystemLocal.hardware.EndReverbMap( soundCarried );
}

/*
========================
idSoundReverb::Detach

R-SB4-31, called by the world's destructor, never through FreeMapAcoustics, after the destructor has deleted the bake
object: reads nothing of it (D-48). By R-SB4-31 the back end acts only when this reverb owns the carrier
========================
*/
void idSoundReverb::Detach()
{
	FreeTables();
	soundSystemLocal.hardware.DetachReverb( this );
}

/*
========================
idSoundReverb::Update

W1, in the gated arm of the world's Update, before the emitter loop (R-SB4-8 to R-SB4-14, R-SB4-30). Declares nothing
in the world's frame: W1 holds only the call (D-46)
========================
*/
void idSoundReverb::Update()
{
	// R-SB4-9: nothing while this is not the playing world (SetPlayingSoundWorld updates the old world once more)
	if( soundSystemLocal.currentSoundWorld != world )
	{
		return;
	}
	if( armPending )
	{
		// R-SB4-8, D-48: the first update after the begin hook is the one SB3 armed in, the load's cut. A later LCUT_LOAD
		// with no new begin hook (a map restart) is a cut, not an arm
		armPending = false;
		if( world->renderWorld->NumAreas() != acoustics->NumAreas() )
		{
			// section 9; the load refuses this (R-SB3-7), so only a render world changed under a published bake reaches here
			common->Warning( "acoustics: %s has %d areas, the render world %d; the reverb stays dry", acoustics->GetMapName(), acoustics->NumAreas(), world->renderWorld->NumAreas() );
			return;
		}
		armed = true;
#if defined( USE_SOUND_MIXER )
		mixerCutPending = true; //puncho - C4-R: the arm also cuts in no area.
#endif
		armDeferred = true;		// taken below in the same update when the listener is in an area
		lastUpdateUs = 0;
	}
	if( !armed )
	{
		return;
	}

	// R-SB4-7's measure: ms since the last update that reached this line, clamped; 0 in the arm's update
	const uint64 nowUs = Sys_Microseconds();
	dtMs = 0.0f;
	if( lastUpdateUs != 0 )
	{
		dtMs = idMath::ClampFloat( 0.0f, s_smoothMaxDtMs.GetFloat(), ( float )( int64 )( nowUs - lastUpdateUs ) * 0.001f );
	}
	lastUpdateUs = nowUs;
	frame++;

	// R-SB4-9: SB4's own hold applies only in no area (E6+'s held area reads as any other). An area the bake does not
	// have, which the check above leaves to a render world changed after the arm, is held the same way. An arm made in no
	// area waits for the first valid one
	area = world->listener.area;
	held = ( area < 0 || area >= acoustics->NumAreas() );
	armThisUpdate = false;
	if( armDeferred && !held )
	{
		armDeferred = false;
		armThisUpdate = true;
		walkPending = true;
	}

	// R-SB4-30: the arm walk, on the arm (a deferred one too) and once after a reacquire. A voice started before the
	// arm, at spawn or by a pacifier render, gets SetReverbSend in this update, which sets the send only when the voice's
	// list holds the bus and returns at once otherwise (R-SB4-5); at most 48 deferred calls
	if( walkPending )
	{
		walkPending = false;
		const float send = s_acousticReverbSend.GetFloat();
		for( int e = 0; e < world->emitters.Num(); e++ )
		{
			const idSoundEmitterLocal* emitter = world->emitters[e];
			for( int c = 0; c < emitter->channels.Num(); c++ )
			{
				idSoundChannel* chan = emitter->channels[c];
				if( chan->hardwareVoice != NULL && ( chan->parms.soundShaderFlags & SSF_GLOBAL ) == 0 )
				{
					chan->hardwareVoice->SetReverbSend( send );
				}
			}
		}
	}

#if defined( USE_SOUND_MIXER )
	mixerCutPending |= world->listenerCut || armThisUpdate; //puncho - C4-R: before the no-area return.
#endif
	if( held )
	{
		return;		// R-SB4-9: no lookup; the last targets and zone stay
	}

	// a cut and the arm snap every reverb value (R-SB4-17, R-SB4-19): here the portals' openness and the zone; SB4-Z the rest
	const bool cut = world->listenerCut || armThisUpdate;
	const idVec3 L = world->listener.pos;			// units, whatever listener_t's comment says
	const sndBakeArea_t* areas = acoustics->GetAreas();
	const int32* cells = acoustics->GetCells();
	const sndBakeProbe_t* probes = acoustics->GetProbes();
	const sndPortalEdge_t* edges = acoustics->GetEdges();
	const sndPortalArea_t* areaEdges = acoustics->GetAreaEdges();
	const uint8* states = acoustics->GetPortalStates();

	// R-SB4-17: openness ramps over 150 ms, alpha 1; 0 when dtMs is 0
	soundSmoothParms_t openParms;
	openParms.tauMs = 0.0f;
	openParms.maxStep = dtMs / REVERB_OPEN_RAMP_MS;
	openParms.arrive = REVERB_ARRIVE;

	areasVisited = 0;
	cellsRead = 0;
	probesWeighted = 0;
	float S = 0.0f;
	float sum[ REVERB_NUM_TARGETS ] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };	// w-weighted: ln T low, mid, high, early and late energy

	// R-SB4-10 (DR-184): the flood settles entries, not areas. Offers wait in a binary heap in Reverb_OfferLess's order.
	// An area settles at most two entries, through different portals; its first makes it visited, and at most
	// MAX_FLOOD_AREAS are. Each settled entry is expanded at once by step 3. An offer already known at the push to be one
	// the pop would drop is not pushed; one that becomes droppable after its push is dropped at its pop. Every drop rule
	// only tightens as the flood runs, so the push's test removes only offers the pop would have dropped. The pops
	// are fewer, but the settlements and their order are the same; the frontier stays inside its capacity
	int numVisited = 0;
	int numOffers = 1;
	int nextOrder = 1;
	bool capHit = false;
	frontier[0].entry.q = L;		// step 1: the root, the listener's area, entry { L, 0, 1 }, no portal
	frontier[0].entry.D = 0.0f;
	frontier[0].entry.o = 1.0f;
	frontier[0].entry.portal = -1;
	frontier[0].area = area;
	frontier[0].order = 0;
	while( numOffers > 0 )
	{
		// step 2: pop the least offer; the last one sinks from the top
		const reverbFloodOffer_t popped = frontier[0];
		numOffers--;
		if( numOffers > 0 )
		{
			const reverbFloodOffer_t last = frontier[numOffers];
			int i = 0;
			for( ;; )
			{
				int child = 2 * i + 1;
				if( child >= numOffers )
				{
					break;
				}
				if( child + 1 < numOffers && Reverb_OfferLess( frontier[child + 1], frontier[child] ) )
				{
					child++;
				}
				if( !Reverb_OfferLess( frontier[child], last ) )
				{
					break;
				}
				frontier[i] = frontier[child];
				i = child;
			}
			frontier[i] = last;
		}

		// settle: a new area while fewer than MAX_FLOOD_AREAS are visited (else the cap is hit), or a second entry
		// through another portal than the first that the first does not dominate (DR-187)
		int slot = floodSlot[popped.area];
		if( slot == -1 )
		{
			if( numVisited == MAX_FLOOD_AREAS )
			{
				capHit = true;		// an area within Rmax is left out: the first 16 visited are used
				continue;
			}
			slot = numVisited++;
			floodSlot[popped.area] = slot;
			flood[slot].area = popped.area;
			flood[slot].numEntries = 0;
		}
		reverbFloodArea_t& settling = flood[slot];
		if( settling.numEntries == 2 || ( settling.numEntries == 1 && settling.entry[0].portal == popped.entry.portal ) || Reverb_Dominated( settling, popped.entry ) )
		{
			continue;
		}
		settling.entry[settling.numEntries++] = popped.entry;

		// R-SB4-10 step 3: each edge of the area but the arrival portal's offers the far area { q', D', e.o * o_pi } from
		// this entry. A portal's openness steps once per update, the first time the flood meets it
		const reverbFloodEntry_t& e = popped.entry;
		const sndPortalArea_t& range = areaEdges[popped.area];
		for( int j = range.firstEdge, end = range.firstEdge + range.numEdges; j < end; j++ )
		{
			const sndPortalEdge_t& edge = edges[j];
			assert( edge.portal >= 0 && edge.portal < acoustics->NumPortals() );
			// DR-186: nothing back through the portal this entry arrived by (the root's -1 matches no edge). That offer has
			// the same point q' and D' = e.D, both to float rounding (the reverse edge's plane is computed apart), so by the
			// triangle inequality it is never shorter to any probe than the entry it came from but by that rounding, and
			// never more open: it could only take a second place from a route that may weigh more. The portal's openness
			// already stepped this update, when the offer that settled this entry was made
			if( edge.portal == e.portal )
			{
				continue;
			}
			reverbPortalOpen_t& portal = portalOpen[edge.portal];
			if( portal.lastVisit != frame )
			{
				const float openTarget = ( ( states[edge.portal] & ( PS_BLOCK_VIEW | PS_BLOCK_AIR ) ) == 0 ) ? 1.0f : 0.0f;
				portal.open.Step( openTarget, 1.0f, openParms, cut || portal.lastVisit != frame - 1 );
				portal.lastVisit = frame;
			}
			const float openness = portal.open.value;
			if( openness < REVERB_OPEN_MIN )
			{
				continue;
			}
			const idVec3 q = Reverb_PortalPoint( edge, L );
			const float D = e.D + ( e.q - q ).Length();
			if( D >= rMax )
			{
				continue;
			}
			assert( edge.otherArea >= 0 && edge.otherArea < acoustics->NumAreas() );
			reverbFloodOffer_t offer;
			offer.entry.q = q;
			offer.entry.D = D;
			offer.entry.o = e.o * openness;
			offer.entry.portal = edge.portal;
			offer.area = edge.otherArea;
			const int otherSlot = floodSlot[edge.otherArea];
			if( otherSlot == -1 )
			{
				if( numVisited == MAX_FLOOD_AREAS )
				{
					capHit = true;
					continue;
				}
			}
			else if( flood[otherSlot].numEntries == 2 || flood[otherSlot].entry[0].portal == edge.portal || Reverb_Dominated( flood[otherSlot], offer.entry ) )
			{
				continue;		// the pop would drop it: settled entries are only added, so each rule still holds then
			}
			if( numOffers == frontierCapacity )
			{
				continue;		// cannot happen: the capacity holds every offer one lookup can make
			}
			// push: the new offer rises from the bottom
			offer.order = nextOrder++;
			int i = numOffers++;
			while( i > 0 )
			{
				const int parent = ( i - 1 ) / 2;
				if( !Reverb_OfferLess( offer, frontier[parent] ) )
				{
					break;
				}
				frontier[i] = frontier[parent];
				i = parent;
			}
			frontier[i] = offer;
		}
	}

	// R-SB4-11, R-SB4-12, after the flood (DR-184): each visited area in the order it was visited, its settled entries in
	// settle order
	for( int v = 0; v < numVisited; v++ )
	{
		const reverbFloodArea_t& visit = flood[v];

		// R-SB4-11: per entry, the 27 cells around cell( e.q ) inside the area's box, x fastest; the second entry skips
		// the cells the first one read, so each cell is read once per area and a probe counts once unless the file names
		// it in two cells (D-40 allows it). R-SB1b-9's arithmetic; the box is held to
		// the cell section by the load (dims and firstCell), cellMin is not, hence the int64 offsets
		const sndBakeArea_t& box = areas[visit.area];
		int c0[3] = { 0, 0, 0 };
		for( int k = 0; k < visit.numEntries; k++ )
		{
			int c[3];
			Reverb_Cell( visit.entry[k].q, cellSize, cellSizeZ, c );
			if( k == 0 )
			{
				c0[0] = c[0];
				c0[1] = c[1];
				c0[2] = c[2];
			}
			for( int dz = -1; dz <= 1; dz++ )
			{
				const int64 lz = ( int64 )c[2] + dz - box.cellMin[2];
				if( lz < 0 || lz >= box.dims[2] )
				{
					continue;
				}
				for( int dy = -1; dy <= 1; dy++ )
				{
					const int64 ly = ( int64 )c[1] + dy - box.cellMin[1];
					if( ly < 0 || ly >= box.dims[1] )
					{
						continue;
					}
					for( int dx = -1; dx <= 1; dx++ )
					{
						const int64 lx = ( int64 )c[0] + dx - box.cellMin[0];
						if( lx < 0 || lx >= box.dims[0] )
						{
							continue;
						}
						if( k == 1 )
						{
							const int ox = c[0] + dx - c0[0];
							const int oy = c[1] + dy - c0[1];
							const int oz = c[2] + dz - c0[2];
							if( ox >= -1 && ox <= 1 && oy >= -1 && oy <= 1 && oz >= -1 && oz <= 1 )
							{
								continue;
							}
						}
						const int cell = box.firstCell + ( int )lx + box.dims[0] * ( ( int )ly + box.dims[1] * ( int )lz );
						cellsRead++;
						const int p = cells[cell];
						if( p < 0 )
						{
							continue;
						}
						const reverbBlendRecord_t& record = blend[p];
						if( record.earlyEnergy < 0.0f )
						{
							continue;		// absent (R-SB4-12)
						}

						// R-SB4-12: w = o * ( 1 - ( d / r )^2 )^2 for d < r, d by the entry that gives the probe the most weight
						const sndBakeProbe_t& probe = probes[p];
						const idVec3 P( probe.position[0], probe.position[1], probe.position[2] );
						float w = 0.0f;
						for( int j = 0; j < visit.numEntries; j++ )
						{
							const reverbFloodEntry_t& e = visit.entry[j];
							const float d = e.D + ( e.q - P ).Length();
							if( d < probe.radius )		// so radius > 0 and d / radius < 1
							{
								const float t = d / probe.radius;
								const float u = 1.0f - t * t;
								w = Max( w, e.o * u * u );
							}
						}
						if( w <= 0.0f )
						{
							continue;
						}
						probesWeighted++;
						S += w;
						sum[0] += w * record.lnDecay[0];
						sum[1] += w * record.lnDecay[1];
						sum[2] += w * record.lnDecay[2];
						sum[3] += w * record.earlyEnergy;
						sum[4] += w * record.lateEnergy;
					}
				}
			}
		}
	}
	for( int v = 0; v < numVisited; v++ )
	{
		floodSlot[flood[v].area] = -1;
	}
	areasVisited = numVisited;
	if( capHit && !areaCapWarned )
	{
		areaCapWarned = true;
		common->Warning( "acoustics: %s more than %d areas within the reverb's reach of area %d; the first %d visited are used", acoustics->GetMapName(), MAX_FLOOD_AREAS, area, MAX_FLOOD_AREAS );
	}

	// R-SB4-13: x = ( sum w * x_i + w0 * x_d ) / ( S + w0 ), x_d the listener's area default. Every w >= 0, so S >= 0 and
	// S + w0 >= 0.05: never a zero denominator. Two log10 per update, each argument held at REVERB_ENERGY_MIN or above
	const reverbBlendRecord_t& areaDefault = blend[acoustics->NumProbes() + area];
	weightSum = S;
	floorWeight = REVERB_FLOOR_WEIGHT * Max( 0.0f, 1.0f - S / REVERB_FLOOR_SPAN );
	const float denominator = S + floorWeight;
	const float lnLow = ( sum[0] + floorWeight * areaDefault.lnDecay[0] ) / denominator;
	const float lnMid = ( sum[1] + floorWeight * areaDefault.lnDecay[1] ) / denominator;
	const float lnHigh = ( sum[2] + floorWeight * areaDefault.lnDecay[2] ) / denominator;
	const float early = ( sum[3] + floorWeight * areaDefault.earlyEnergy ) / denominator;
	const float late = ( sum[4] + floorWeight * areaDefault.lateEnergy ) / denominator;
	target[0] = lnMid;
	target[1] = lnLow - lnMid;
	target[2] = lnHigh - lnMid;
	target[3] = 10.0f * log10f( Max( early, REVERB_ENERGY_MIN ) );
	target[4] = 10.0f * log10f( Max( late, REVERB_ENERGY_MIN ) );

	// R-SB4-14: the candidate is the listener's area's zone. Zc takes it after it has been the candidate for the dwell
	// without a break, or once the listener is farther than the hysteresis from every edge of a0 into a Zc area (no such
	// edge: at once). A cut and the arm take it at once (R-SB4-19). preDelay, lateDelay and roomSize are the zone's
	const int areaZone = areas[area].zone;
	if( areaZone != candidate )
	{
		candidate = areaZone;
		dwellMs = 0.0f;
	}
	else
	{
		dwellMs += dtMs;
	}
	if( candidate != zone )
	{
		bool take = cut || dwellMs >= s_acousticReverbDwellMs.GetFloat();
		if( !take )
		{
			take = true;
			const float hysteresis = s_acousticReverbHysteresis.GetFloat();
			const sndPortalArea_t& range = areaEdges[area];
			for( int j = range.firstEdge, end = range.firstEdge + range.numEdges; j < end && take; j++ )
			{
				if( areas[edges[j].otherArea].zone == zone )
				{
					take = ( L - Reverb_PortalPoint( edges[j], L ) ).Length() > hysteresis;
				}
			}
		}
		if( take )
		{
			zone = candidate;
			const sndReverb_t& zoneRecord = acoustics->GetZones()[zone];
			zonePreDelay = zoneRecord.preDelay;
			zoneLateDelay = zoneRecord.lateDelay;
			zoneRoomSize = zoneRecord.roomSize;
		}
	}
}

/*
========================
idSoundReverb::Tick

R-SB4-6, R-SB4-7: the back end's update, every Render while this reverb owns the carrier. SB4-Z fills it; until then a
gated map is dry and a carried drain does not end
========================
*/
void idSoundReverb::Tick( int nowMs )
{
}

/*
========================
idSoundReverb::OnCarrierReacquired

R-SB4-32, at the end of the back end's Init after a device restart. SB4-L's line: the next armed update of the playing
world walks the voices again (R-SB4-30), as walkPending says. SB4-Z adds the submixes' reset
========================
*/
void idSoundReverb::OnCarrierReacquired()
{
	walkPending = true;
}
