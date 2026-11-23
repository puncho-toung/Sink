//puncho: WP-C2, deterministic room impulse responses with no engine state.
#ifndef __SND_IRBUILDER_H__
#define __SND_IRBUILDER_H__

#include <stdint.h>

//puncho - the director (DR-265): the 1.0 s cap of DR-253 is the engine's; a listening demo may be built at another
// cap to put it to the ear (WP-C2 R-C2-1)
#ifndef SND_IR_MAX_PARTITIONS_CAP
#define SND_IR_MAX_PARTITIONS_CAP	100
#endif

enum
{
	SND_IR_RATE = 44100,
	SND_IR_PARTITION = 441,
	SND_IR_MAX_PARTITIONS = SND_IR_MAX_PARTITIONS_CAP,
	SND_IR_EARLY_SAMPLES = 3528,
	SND_IR_TAPER_SAMPLES = 882
};
#define SND_IR_SILENT_DB ( -99.99f )

struct sndIRRecord_t
{
	float	decayTime[3];
	float	earlyDb;
	float	lateDb;
	float	preDelay;
	float	lateDelay;
};

// Two RBJ sections; each a row contains a1 and a2, with a0 normalized to one.
void SndIR_Butterworth4( bool highPass, double frequency, float b[2][3], float a[2][2] );

// One caller at a time; no lock is taken here.
// The calling thread must enable flush-to-zero and denormals-are-zero.
// A gap between parts lets the filters ring down through the denormal range.
class idSoundIRBuilder
{
public:
				idSoundIRBuilder();
	void		Begin( const sndIRRecord_t& record, unsigned int zoneSeed );
	int			NumPartitions() const { return numPartitions; }
	int			PartitionsDone() const { return partitionsDone; }
	bool		NextPartition( float* left, float* right );

private:
	sndIRRecord_t	record;
	double			decayStep[3];
	double			earlyAmplitude;
	double			lateAmplitude;
	float			sampleStep[3];
	float			filterB[8][3];
	float			filterA[8][2];
	float			filterState[2][8][2];
	uint32_t		noiseState[2][3];
	int				earlyStart;
	int				lateStart;
	int				numPartitions;
	int				partitionsDone;
};

#endif
