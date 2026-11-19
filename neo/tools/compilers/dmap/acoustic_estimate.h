//puncho - SB2-E (WP-SB2 R-SB2-15 to R-SB2-23, R-SB2-31, R-SB2-32): the bake's estimator, area defaults, zones
// and door losses. Pure arithmetic on arrays: no Steam Audio header, no file, no cvar, no engine state, and no
// print outside the self-test command.
#ifndef __ACOUSTIC_ESTIMATE_H__
#define __ACOUSTIC_ESTIMATE_H__

#include "../../../sound/snd_acousticfile.h"

/*
===============================================================================

	Stage B turns each probe's energy histogram (channel 0 of the three bands, bin i = delays [i * 10 ms,
	( i + 1 ) * 10 ms) after the direct sound) into one sndReverb_t and its flags, then builds the area defaults,
	the zones and the door losses from those records.

	The same input gives the same bits on every run of one build on one machine: double arithmetic, loops in
	ascending index, a fixed count of three refits with no convergence test, full tie-breaks in every sort, no
	hash table iterated, every output clamped and then quantised (R-SB2-18).

	What the caller (SB2-B) calls, in this order:
	1. AcousticAreaSums_Begin( sums, numAreas, numBins ), once the library has given the bin count.
	2. For every probe in MSH_PROBES order, as the bake finishes it: AcousticEstimateProbe for its own record and
	   flags, and AcousticAreaSums_AddProbe with the same histogram and statistics and the probe's area. The
	   histogram need not be kept after that: the memory is 24 * numBins bytes per area, not per probe.
	3. AcousticAreaDefaults (areas with probes), AcousticBuildZones, AcousticZoneRecords (zones, then areas with
	   no probe), AcousticDoorLoss per door; then AcousticAreaSums_Free.
	An area's mean is its sum (double, per band and bin, its probes added in probe order) divided by its probe
	count. A zone's sum is defined as the sum of its areas' sums taken in ascending area order (R-SB2-20 and
	R-SB2-22 as amended), divided by the zone's probe count.

	The probe count of an area is the count fed to its sum, never MSH_AREAS.probeCount: an area with no probe fed
	is an area with no probe in all three functions (E3R-1). Probes are fed in MSH_PROBES order; nothing here
	enforces it. The caller calls AcousticAreaSums_Free on every path once Begin has run; Begin on a live struct
	leaks its memory, and Free on a struct never begun and not zeroed is undefined.

	The caller validates its inputs before these calls and fails the bake with a message (E3-5): area indices,
	areaZone in [0, numZones), portal areas. Here they are debug asserts only; numBins <= 0 reads as no bins.

	Limits and choices, for the record:
	- Long decays under-read after the fixed three refits (8 s by about 11 %, 10 s by 17 %, 20 s by 42 %, all
	  with bit 4): a named limit of the method, not a fault (E3-9 a).
	- The working decay of a refit is capped at 20 s (r = 1 would divide by 0). A refit adds no tail when the
	  previous pass's raw decay is under 0.1 s (R-SB2-16 gives T = 0 there, so r = 0), nor in any refit of a
	  band whose first pass had no window point (E3-1, E3-9 b).
	- A two-point fallback with no usable bin, or with y_L >= 0, gives 0 s, which clamps to 0.1 s.
	- Bit 3 is set when the ceiling leaves no usable bin (iEnd 0); sparse bands are left out of the bit 3 test.
	- iEnd < 8: lateDb holds no bin, only the tail, and the tail covers bins iEnd..7 that earlyDb also counts.
	  iEnd < 5: the tail's mean is over iEnd bins. Such probes carry bit 3 (E3-10).
	- A cut at bins 1 to 3 leaves even a dense decay with no window point on its first pass, so no refit adds a
	  tail and lateDb reads -100 dB: with fewer than 8 usable bins and no window point there is no basis for a
	  tail, and bits 3 and 4 mark the probe (E3R-3, ruled).
	- A value that is not in [1e-30, FLT_MAX] (a NaN, a denormal, a negative, an infinity) reads as 0.

	The bodies are in acoustic_estimate.cpp, built only in the engine with USE_STEAMAUDIO_BAKE (rbdmap never
	bakes); without the option only the self-test command's stub remains.

===============================================================================
*/

// R-SB2-19: BAK_PROBES.flags (uint32; idLib's BIT() is 64-bit). Bit 0 is the leak gate's (SB2-B sets it); the
// estimator gives bits 1 to 5.
#define ACOUSTIC_PROBE_ESCAPED			0x01u	// bit 0: rays escaped from this probe (SB2-B)
#define ACOUSTIC_PROBE_EMPTY			0x02u	// bit 1: empty histogram, the dry record of R-SB2-17
#define ACOUSTIC_PROBE_ROOMSIZE_FALLBACK	0x04u	// bit 2: fewer than 64 segments, roomSize from preDelay
#define ACOUSTIC_PROBE_CEILING_CUT		0x08u	// bit 3: the bounce ceiling cut the window the fit would have used
#define ACOUSTIC_PROBE_TWO_POINT		0x10u	// bit 4: the decay fit fell back to its two-point value in some band
#define ACOUSTIC_PROBE_SPARSE			0x20u	// bit 5: a sparse band (R-SB2-32), its decayTime is 0.1 s

// IPLBakeProbeStats::ceilingPathMm when no ray used every bounce the ceiling allows
#define ACOUSTIC_CEILING_NONE			0xFFFFFFFFFFFFFFFFull

// One running sum per area, fed one probe at a time in probe order. Allocated by AcousticAreaSums_Begin
// (TAG_TOOLS), freed by AcousticAreaSums_Free.
struct acousticAreaSums_t
{
	double*				values;			// numAreas * SND_NUM_BANDS * numBins: area-major, then band, then bin
	uint64*				segments;		// numAreas: IPLBakeProbeStats::segments summed
	uint64*				segmentMm;		// numAreas: IPLBakeProbeStats::segmentMm summed
	uint64*				ceilingPathMm;	// numAreas: the shortest ceilingPathMm, ACOUSTIC_CEILING_NONE = none
	int*				probes;			// numAreas: probes added
	int					numAreas;
	int					numBins;		// EnergyFieldGetNumBins, never computed; <= 0 reads as 0
};

// 1. R-SB2-15, -31, -32, -16, -17, -18, -19. histogram = channel 0 of the three bands, band-major, numBins each.
// Fills reverb (clamped, then quantised) and returns flag bits 1 to 5; bit 0 is left 0.
uint32	AcousticEstimateProbe( const float* histogram, int numBins, uint32 segments, uint64 segmentMm, uint64 ceilingPathMm, sndReverb_t& reverb );

// the running sums of R-SB2-20 and R-SB2-22: zeroed for numAreas areas of numBins bins
void	AcousticAreaSums_Begin( acousticAreaSums_t& sums, int numAreas, int numBins );
// adds one probe of area area: histogram as for AcousticEstimateProbe, each value read as 0 outside [1e-30, FLT_MAX]
void	AcousticAreaSums_AddProbe( acousticAreaSums_t& sums, int area, const float* histogram, uint32 segments, uint64 segmentMm, uint64 ceilingPathMm );
void	AcousticAreaSums_Free( acousticAreaSums_t& sums );

// 2. R-SB2-20, areas with probes: the estimate of each area's mean (its sum divided by its probe count, no float
// round trip), its segments summed, its shortest ceiling path. Areas with no probe are left untouched here;
// AcousticZoneRecords fills them. areaReverb has sums.numAreas entries.
void	AcousticAreaDefaults( const acousticAreaSums_t& sums, sndReverb_t* areaReverb );

// 3. R-SB2-21. Joinable pairs: an MSH_PORTALS entry with door -1; never across a door portal. Similar merge
// (union spread under the limits), one pass-through merge, dense ids in ascending order of each zone's
// lowest area index. The area count and each area's probe count are the sums' (E3R-1); areaReverb must hold the
// defaults of every area with probes (AcousticAreaDefaults). Every comparison is of integer quantisation steps,
// no float is compared. Fills areaZone[sums.numAreas] and returns the zone count.
int		AcousticBuildZones( const acousticAreaSums_t& sums, const sndReverb_t* areaReverb, const sndPortal_t* portals, int numPortals, int* areaZone );

// 4. R-SB2-22 and the no-probe case of R-SB2-20. zoneReverb[numZones]: the estimate of each zone's mean (the sum
// of its areas' sums in ascending area order, divided by its probe count), segments summed, the shortest
// ceiling path; a zone with no probe takes the empty record. Then each area with no probe takes its zone's
// record. isolatedAreas is cleared and gets, ascending, every area with no probe whose zone has no probe (it
// took the empty record): SB2-B prints the warning.
void	AcousticZoneRecords( const acousticAreaSums_t& sums, const int* areaZone, int numZones, sndReverb_t* zoneReverb, sndReverb_t* areaReverb, idList<int>& isolatedAreas );

// 5. R-SB2-23. transmission = the door's plug material's three energy fractions; lossDb = clamp(
// -10 log10( max( t, 1e-6 ) ), 0, 60 ) dB, quantised to 0.01 dB. The rest of BAK_DOORS is SB2-B's.
void	AcousticDoorLoss( const float transmission[SND_NUM_BANDS], float lossDb[SND_NUM_BANDS] );

#endif /* !__ACOUSTIC_ESTIMATE_H__ */
