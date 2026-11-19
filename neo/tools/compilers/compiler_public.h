/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 GPL Source Code (?Doom 3 Source Code?).

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#ifndef __COMPILER_PUBLIC_H__
#define __COMPILER_PUBLIC_H__

/*
===============================================================================

	Compilers for map, aas, etc. processing.

===============================================================================
*/

extern idCVar		dmap_verbose;

// map processing (also see SuperOptimizeOccluders in tr_local.h)
void Dmap_f( const idCmdArgs& args );
void PhononInfo_f( const idCmdArgs& args );	//puncho - SB0: loads phonon.dll, times load/context/unload, unloads (acoustic_phonon.cpp)
void AcousticEstimateSelfTest_f( const idCmdArgs& args );	//puncho - SB2-E: the estimator and zoning on synthetic histograms (acoustic_estimate.cpp)
void BakeAcoustics_f( const idCmdArgs& args );	//puncho - SB2-I: bakeAcoustics [fast] <map>, Stage B of the acoustic bake (acoustic_bake.cpp)
//puncho - TC1: checked compile entry points return after releasing owned data.
class idMapFile;
bool CM_CompileMap( const idMapFile* mapFile );
bool RunAAS( const idCmdArgs& args );
bool RunRouterMap( const idCmdArgs& args );	//puncho - R2 router

// AAS file compiler
void RunAAS_f( const idCmdArgs& args );
void RunAASDir_f( const idCmdArgs& args );
void RunReach_f( const idCmdArgs& args );

#endif	/* !__COMPILER_PUBLIC_H__ */
