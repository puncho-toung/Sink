/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013 Robert Beckebans

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
//puncho - C3e-C: main-thread voice commands; sample views change only with a new generation.
#ifndef __MX_SOUNDVOICE_H__
#define __MX_SOUNDVOICE_H__

#include "../snd_voice.h"

class idSoundVoice_Mixer : public idSoundVoice_Base
{
public:
						idSoundVoice_Mixer();
	void				Create( const idSoundSample* leadinSample, const idSoundSample* loopingSample );
	void				Start( int offsetMS, int ssFlags );
	void				Stop();
	void				Pause();
	void				UnPause();
	bool				Update();
	float				GetAmplitude();
	void				SetReverbSend( float s );
	static bool			FormatsMatch( const idWaveFile::waveFmt_t& a, const idWaveFile::waveFmt_t& b );
	uint32				GetSampleRate() const { return leadinView.rate; }
	static int			GetSurroundLfe() { return dstLFE; }
#ifndef ID_RETAIL
	const sndVoiceCmd_t& GetCommand() const { return command; }
	bool				HasStarted() const { return started; }
	int					GetStartOffsetMS() const { return startOffsetMS; }
	float				GetLastOcclusion() const { return lastOcclusion; }
#endif

private:
	friend class idSoundHardware_Mixer;
	sndVoiceCmd_t		command;
	sndSampleView_t		leadinView;
	sndSampleView_t		loopView;
	int					slot;
	int					startOffsetMS;
	float				lastOcclusion;
	bool				noMeter;
	bool				started;
};

class idSoundVoice : public idSoundVoice_Mixer
{
};

#endif
