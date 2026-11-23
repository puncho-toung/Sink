//puncho - SB4-L (R-SB4-1, R-SB4-8 to R-SB4-14, R-SB4-30, R-SB4-34; SEAMS section 3): the reverb of one sound world, its map hooks and the probe lookup
#ifndef __SND_REVERB_H__
#define __SND_REVERB_H__

/*
===============================================================================

	idSoundReverb is the member reverb of every idSoundWorldLocal. LoadMapAcoustics hands it a baked map (the begin
	hook, step 11) and FreeMapAcoustics takes it back (the end hook); the world's destructor detaches it. At most once per
	update of the gated arm (place W1, before the emitter loop), once armed, in the playing world and with the listener
	in an area the bake has, it turns the listener's place and the door states into the five targets of R-SB4-33 and a
	zone. SB4-Z smooths them and drives the two reverbs from Tick, which the XAudio2 back end
	(SB4-C) calls. On a map with no bake nothing here runs but the member's construction and destruction: Init zeroes
	it from the world's constructor, Detach (a free of NULL and DetachReverb) runs from its destructor, and it costs its
	memory (C-SB6m). Main thread only; nothing here is saved or sent.

	Included by snd_local.h after idSoundSmooth and before idSoundWorldLocal.

===============================================================================
*/

// SEAMS section 3: the frame the reverb hands the back end each tick. submix[0] is reverb A, submix[1] B; param[] holds the 24
// fields of XAUDIO2FX_REVERB_PARAMETERS in the SDK's declaration order, as floats (SB4-C rounds the integer fields).
// disabledMidTail: an effect was disabled while its tail rang, so A and B are made again at the next begin (R-SB4-5).
// releaseOwner: the back end clears its owner after applying the frame. The first line also opens R-SB4-33's block
static const int REVERB_LOG_PARAMS = 24;
struct reverbCarrierSubmix_t { int32 enabled; float busLevel; float returnGain; float param[ REVERB_LOG_PARAMS ]; };
struct reverbCarrierFrame_t { reverbCarrierSubmix_t submix[ 2 ]; bool disabledMidTail; bool releaseOwner; };
// SB4-Z: R-SB4-33's reverbLogSubmix_t and reverbLogFrame_t follow here

// R-SB4-30 and H1 (SB4-C's line): the send level of every voice of a gated world that is not SSF_GLOBAL. In snd_reverb.cpp
extern idCVar s_acousticReverbSend;
#if defined( USE_SOUND_MIXER )
extern idCVar s_acousticReverbFadeMs; //puncho - C4-R
extern idCVar s_acousticReverbSendDistance;
#endif

// R-SB4-33's target[]: ln T_mid, ln( T_low / T_mid ), ln( T_high / T_mid ), earlyDb, lateDb
static const int REVERB_NUM_TARGETS = 5;

class idSoundAcoustics;
class idSoundWorldLocal;
struct reverbBlendRecord_t;		// R-SB4-34's table, one per probe then one per area default; snd_reverb.cpp
struct reverbPortalOpen_t;		// open[], one per double portal (C-SB67); snd_reverb.cpp
struct reverbFloodArea_t;		// the flood's record of one visited area and the entries it settled; snd_reverb.cpp
struct reverbFloodOffer_t;		// an entry offered to an area, waiting in the flood's frontier; snd_reverb.cpp

class idSoundReverb
{
public:
	// no constructor: the world's constructor calls Init, which zeroes every member, then sets world and the -1s (C-SB6m)
	void					Init( idSoundWorldLocal* soundWorld );
	void					OnMapAcousticsLoaded( const idSoundAcoustics& acoustics );	// begin hook, LoadMapAcoustics step 11 (R-SB4-5)
	void					OnMapAcousticsFreed();		// end hook, FreeMapAcoustics (R-SB4-26)
	void					Detach();					// the world's destructor, after the bake object is deleted (R-SB4-31)
	void					Update();					// W1: the arm, the lookup, the zone, the arm walk
	void					Tick( int nowMs );			// the back end's update (R-SB4-6, R-SB4-7); SB4-Z fills it
	void					OnCarrierReacquired();		// the back end's Init after a device restart (R-SB4-32)

	// what SB4-Z and the log read. Written only by Update and the hooks; the last lookup's values stay while held
	float					target[ REVERB_NUM_TARGETS ];	// R-SB4-13, in R-SB4-33's order
	int						area;				// listener.area of the last update (a0); -1 none
	int						zone;				// R-SB4-14: Zc, a BAK_ZONES index; -1 until the arm
	int						candidate;			// BAK_AREAS[ a0 ].zone of the last lookup; -1 until the arm
	float					dwellMs;			// how long candidate has been the candidate without a break
	float					zonePreDelay;		// Zc's record, not blended (K-SB7): preDelay, s
	float					zoneLateDelay;		// lateDelay, s
	float					zoneRoomSize;		// roomSize, m
	int						areasVisited;		// R-SB4-33's counters of the last lookup
	int						cellsRead;
	int						probesWeighted;
	float					weightSum;			// S
	float					floorWeight;		// w0
	float					dtMs;				// ms since the last update, measured as Tick measures it (R-SB4-7)
	bool					armed;				// R-SB4-8: set by the first update after the begin hook unless section 9 refuses or the tables could not be allocated; false outside a map
	bool					armDeferred;		// R-SB4-9: armed in no area, the arm waits for the first valid one
	bool					armThisUpdate;		// the last update ran the arm: the load cut's, or the deferred one
	bool					held;				// R-SB4-9: the last update had no area (or one the bake lacks); no lookup, targets and zone kept

private:
#if defined( USE_SOUND_MIXER )
	friend class idSoundHardware_Mixer; //puncho - C4-R: main-thread caller takes and clears the cut once.
	bool mixerCutPending;
#endif
	void					FreeTables();		// the one release of the per-map tables; also forgets the map

	idSoundWorldLocal*			world;			// the world this is the member of
	const idSoundAcoustics*		acoustics;		// the begin hook's object, until the end hook or the detach; never read after
	byte*						tables;			// one Mem_Alloc16 per map: blend, portalOpen, floodSlot, flood, frontier
	reverbBlendRecord_t*		blend;			// NumProbes() records, then NumAreas() area defaults
	reverbPortalOpen_t*			portalOpen;		// NumPortals()
	int*						floodSlot;		// NumAreas(): an area's index in flood[] during a lookup, else -1
	reverbFloodArea_t*			flood;			// MAX_FLOOD_AREAS records, in the order the areas were visited; rebuilt by every lookup
	reverbFloodOffer_t*			frontier;		// frontierCapacity offers, a binary heap during a lookup (DR-184)
	uint64						lastUpdateUs;	// Sys_Microseconds() of the last update; 0 = none since the arm
	int							frontierCapacity;	// 1 + 2 * MAX_FLOOD_AREAS * MaxAreaEdges(): every offer one lookup can make
	int							frame;			// counts this map's armed updates of the playing world; a portal's last visit is one of these
	float						rMax;			// min( S, Sz ): every probe's radius and the flood's reach (R-SB4-8)
	float						cellSize;		// S
	float						cellSizeZ;		// Sz
	bool						armPending;		// the begin hook ran; the next update of the playing world arms or refuses (section 9)
	bool						walkPending;	// the next armed update of the playing world walks the voices (R-SB4-30)
	bool						areaCapWarned;	// the flood's area cap has warned on this map
};

#endif /* !__SND_REVERB_H__ */
