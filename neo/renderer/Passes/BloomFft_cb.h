/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2014 Robert Beckebans

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

#pragma once

// The constants of the bloom's convolution ( WP-J24 ), one buffer written once a view by BloomPass and read by every
// pass of shaders/builtin/post/bloom_fft.cs.hlsl and by bloom_fft_compose.ps.hlsl. shaders/builtin/post/
// bloom_fft_cb.h has the same struct, member for member ( BloomPass_cb.h and bloom_cb.h are the model ).

#define BLOOM_FFT_WAVES 8
#define BLOOM_FFT_MAX 1024
#define BLOOM_FFT_THREADS 256
#define BLOOM_FFT_CLASSES 4		// WP-J27: BLOOM_STYLE_SLOTS

struct BloomFftConstants
{
	uint	field[4];		// x W, y H, z log2 W, w log2 H
	uint	image[4];		// x gw, y gh: the picture's cells; z, w the source level's rectangle ( texels )
	idVec4	source;			// x s, the source level's texels a cell spans ( the tent's half-width ); y 1 / s^2; zw 0
	idVec4	shape;			// x the reach R in cells; y the re-entry's factor ( 1 - w0 ) / w0; z k_s, the cells a screen pixel; w the arms' fall c ( spikes )
	idVec4	kernelA;		// x cells a view height; y the shape; z the spike arms n; w the rotation
	idVec4	kernelB;		// x the arms' length L; y their half-width at the lamp; z the spread; w the ratio
	idVec4	kernelC;		// x the ringing; y the glow's reach R_g; z the glare; w the cell's subsamples a side
	idVec4	norms;			// the terms' integrals: x the halo, y the arms, z the wide glow, w the glare
	idVec4	waves[BLOOM_FFT_WAVES];	// rgb the wavelength's share of each channel; w its pattern scale k
	uint	classes[4];		// WP-J27: x the classes the light is split into ( 1: none ); y the kernel slice PASS 0, 2 and 4 make; zw 0
	uint	classKernel[4];	// each class's kernel slice
	idVec4	classMask[BLOOM_FFT_CLASSES];	// each class's weights of the four slots' shares
};
compile_time_assert( sizeof( BloomFftConstants ) == 352 );
