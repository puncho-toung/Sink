/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2015 Robert Beckebans

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

#ifndef __SAVEGAME_H__
#define __SAVEGAME_H__

/*

Save game related helper classes.

*/

#include "StreamTrace.h"	//puncho - P2b.1.10k: idSaveGame's idFile_StreamTrace counts every save's bytes

class idSaveGame
{
public:
	idSaveGame( idFile* savefile, idFile* stringFile, int inVersion );
	~idSaveGame();

	bool					Close();	//puncho - P2b.1.10k: false past MIN_SAVEGAME_SIZE_BYTES or MAX_SAVEGAME_STRING_TABLE_SIZE

	void					WriteDecls();

	void					AddObject( const idClass* obj );
	void					Resize( const int count )
	{
		objects.Resize( count );
	}
	void					WriteObjectList();

	void					Write( const void* buffer, int len );
	void					WriteInt( const int value );
	void					WriteJoint( const jointHandle_t value );
	void					WriteShort( const short value );
	void					WriteByte( const byte value );
	void					WriteSignedChar( const signed char value );
	void					WriteFloat( const float value );
	void					WriteBool( const bool value );
	void					WriteString( const char* string );
	void					WriteVec2( const idVec2& vec );
	void					WriteVec3( const idVec3& vec );
	void					WriteVec4( const idVec4& vec );
	void					WriteVec6( const idVec6& vec );
	void					WriteWinding( const idWinding& winding );
	void					WriteBounds( const idBounds& bounds );
	void					WriteMat3( const idMat3& mat );
	void					WriteAngles( const idAngles& angles );
	void					WriteObject( const idClass* obj );
	void					WriteStaticObject( const idClass& obj );
	//puncho start - saves (P1-m10b): an int a load may give back changed, so it may differ from save to save: a render
	// or PVS handle the Restore re-acquires, or one of two client fields idGameLocal::SaveGame writes. The bytes are
	// WriteInt's; field, a string literal, names it in the trace
	void					WriteRenderHandle( const int handle, const char* field );
	//puncho end
	void					WriteDict( const idDict* dict );
	void					WriteMaterial( const idMaterial* material );
	void					WriteSkin( const idDeclSkin* skin );
	void					WriteParticle( const idDeclParticle* particle );
	void					WriteFX( const idDeclFX* fx );
	void					WriteSoundShader( const idSoundShader* shader );
	void					WriteModelDef( const class idDeclModelDef* modelDef );
	void					WriteModel( const idRenderModel* model );
	void					WriteUserInterface( const idUserInterface* ui, bool unique );
	void					WriteRenderEntity( const renderEntity_t& renderEntity );
	void					WriteRenderLight( const renderLight_t& renderLight );
	void					WriteRenderEnvprobe( const renderEnvironmentProbe_t& renderEnvprobe ); // RB
	void					WriteRefSound( const refSound_t& refSound );
	void					WriteRenderView( const renderView_t& view );
	void					WriteUsercmd( const usercmd_t& usercmd );
	void					WriteContactInfo( const contactInfo_t& contactInfo );
	void					WriteTrace( const trace_t& trace );
	void					WriteTraceModel( const idTraceModel& trace );
	void					WriteClipModel( const class idClipModel* clipModel );
	void					WriteSoundCommands();

	void					WriteBuildNumber( const int value );

	int						GetBuildNumber() const
	{
		return version;
	}

	int						GetCurrentSaveSize() const
	{
		return file->Length();
	}

	//puncho start - P2b.1.10k: the bytes handed to the stream so far, uncompressed (GetCurrentSaveSize is the compressed
	// length of the blocks flushed so far), and those the objects' Save chains wrote, once Close ran them
	int						GetUncompressedSize() const
	{
		return counter.count;
	}
	int						GetObjectBytes() const
	{
		return objectBytes;
	}
	//puncho end

	//puncho start - stream-trace (P1-A1c, K1): the save driver's pseudo-scopes (<program>, <events>); nothing unless the
	// stream-trace checker armed this save
	void					BeginTraceScope( const char* name );
	void					EndTraceScope();
	//puncho end

private:
	idFile* 				file;
	idFile* 				stringFile;
	idCompressor* 			compressor;

	idList<const idClass*>	objects;
	int						version;

	idStreamTrace* 			streamTrace;	//puncho - stream-trace (P1-A1c): NULL unless the checker armed this save
	//puncho start - P2b.1.10k: always between the stream and its file, and under an armed trace's own counter: the
	// pipelined file's Length() is compressed and its Tell() asserts
	idFile_StreamTrace		counter;
	int						objectBytes;
#if !defined( ID_RETAIL )
	idList<byte>			testStream;		// g_testSaveOverflow stream: the stream's bytes so far, which size the plant
#endif
	//puncho end

	void					CallSave_r( const idTypeInfo* cls, const idClass* obj );

	struct stringTableIndex_s
	{
		idStr		string;
		int			offset;
	};

	idHashIndex						stringHash;
	idList< stringTableIndex_s >	stringTable;
	int								curStringTableOffset;

};

class idRestoreGame
{
public:
	idRestoreGame( idFile* savefile, idFile* stringTableFile, int saveVersion );
	~idRestoreGame();

	void					ReadDecls();

	void					CreateObjects();
	void					RestoreObjects();
	void					DeleteObjects();

	void					Error( VERIFY_FORMAT_STRING const char* fmt, ... );

	void					Read( void* buffer, int len );
	void					ReadInt( int& value );
	void					ReadJoint( jointHandle_t& value );
	void					ReadShort( short& value );
	void					ReadByte( byte& value );
	void					ReadSignedChar( signed char& value );
	void					ReadFloat( float& value );
	void					ReadBool( bool& value );
	void					ReadString( idStr& string );
	void					ReadVec2( idVec2& vec );
	void					ReadVec3( idVec3& vec );
	void					ReadVec4( idVec4& vec );
	void					ReadVec6( idVec6& vec );
	void					ReadWinding( idWinding& winding );
	void					ReadBounds( idBounds& bounds );
	void					ReadMat3( idMat3& mat );
	void					ReadAngles( idAngles& angles );
	void					ReadObject( idClass*& obj );
	void					ReadStaticObject( idClass& obj );
	void					ReadRenderHandle( int& handle, const char* field );	//puncho - saves (P1-m10b): WriteRenderHandle's int, read as ReadInt
	void					ReadDict( idDict* dict );
	void					ReadMaterial( const idMaterial*& material );
	void					ReadSkin( const idDeclSkin*& skin );
	void					ReadParticle( const idDeclParticle*& particle );
	void					ReadFX( const idDeclFX*& fx );
	void					ReadSoundShader( const idSoundShader*& shader );
	void					ReadModelDef( const idDeclModelDef*& modelDef );
	void					ReadModel( idRenderModel*& model );
	void					ReadUserInterface( idUserInterface*& ui );
	void					ReadRenderEntity( renderEntity_t& renderEntity );
	void					ReadRenderLight( renderLight_t& renderLight );
	void					ReadRenderEnvprobe( renderEnvironmentProbe_t& renderEnvprobe ); // RB
	void					ReadRefSound( refSound_t& refSound );
	void					ReadRenderView( renderView_t& view );
	void					ReadUsercmd( usercmd_t& usercmd );
	void					ReadContactInfo( contactInfo_t& contactInfo );
	void					ReadTrace( trace_t& trace );
	void					ReadTraceModel( idTraceModel& trace );
	void					ReadClipModel( idClipModel*& clipModel );
	void					ReadSoundCommands();

	//						Used to retrieve the saved game buildNumber from within class Restore methods
	int						GetBuildNumber() const
	{
		return version;
	}

	//puncho start - stream-trace (P1-A1c, K1): the restore driver's pseudo-scopes (<program>, <events>)
	void					BeginTraceScope( const char* name );
	void					EndTraceScope();
	//puncho end

private:
	idFile* 		file;
	idFile* 		stringFile;
	idList<idClass*, TAG_SAVEGAMES>		objects;
	int						version;
	int						stringTableOffset;

	idStreamTrace* 			streamTrace;	//puncho - stream-trace (P1-A1c): NULL unless the checker armed this restore

	void					CallRestore_r( const idTypeInfo* cls, idClass* obj );
};

#endif /* !__SAVEGAME_H__*/
