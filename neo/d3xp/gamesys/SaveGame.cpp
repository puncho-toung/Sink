/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

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

#include "precompiled.h"
#pragma hdrstop


#include "../Game_local.h"
#include "StreamTrace.h"	//puncho - stream-trace (P1-A1c)

/*
Save game related helper classes.

Save games are implemented in two classes, idSaveGame and idRestoreGame, that implement write/read functions for
common types.  They're passed in to each entity and object for them to archive themselves.  Each class
implements save/restore functions for it's own data.  When restoring, all the objects are instantiated,
then the restore function is called on each, superclass first, then subclasses.

Pointers are restored by saving out an object index for each unique object pointer and adding them to a list of
objects that are to be saved.  Restore instantiates all the objects in the list before calling the Restore function
on each object so that the pointers returned are valid.  No object's restore function should rely on any other objects
being fully instantiated until after the restore process is complete.  Post restore fixup should be done by posting
events with 0 delay.

The savegame header will have the Game Name, Version, Map Name, and Player Persistent Info.

Changes in version make savegames incompatible, and the game will start from the beginning of the level with
the player's persistent info.

Changes to classes that don't need to break compatibilty can use the build number as the savegame version.
Later versions are responsible for restoring from previous versions by ignoring any unused data and initializing
variables that weren't in previous versions with safe information.

At the head of the save game is enough information to restore the player to the beginning of the level should the
file be unloadable in some way (for example, due to script changes).
*/

//puncho start - stream-trace (P1-A1c, MP-A1 tier 1): an armed idStreamTrace (StreamTrace.h) records each primitive below
// once its bytes moved, behind one pointer test. A primitive that used to go through another (a string's table
// offset, an object index, a winding's point count) now writes or reads that int with file->WriteBig / ReadBig itself,
// the same bytes, so it is one entry. A struct writer opens its scope with EnterStruct and calls itself to run its
// body inside it; a class level's Save or Restore gets its scope in CallSave_r / CallRestore_r
//puncho end

/*
================
idSaveGame::idSaveGame()
================
*/
idSaveGame::idSaveGame( idFile* savefile, idFile* stringTableFile, int saveVersion )
{
	//compressor = idCompressor::AllocLZW();
	//compressor->Init( savefile, true, 8 );
	//file = compressor;
	file = savefile;
	stringFile = stringTableFile;
	version = saveVersion;

	// Put NULL at the start of the list so we can skip over it.
	objects.Clear();
	objects.Append( NULL );

	curStringTableOffset = 0;

	//puncho start - stream-trace (P1-A1c, K1): an armed trace sees every byte from here on
	streamTrace = idStreamTrace::armed;
	if( streamTrace != NULL )
	{
		file = streamTrace->Attach( savefile );
	}
	//puncho end
}

/*
================
idSaveGame::~idSaveGame()
================
*/
idSaveGame::~idSaveGame()
{
	//compressor->FinishCompress();
	//delete compressor;

	if( objects.Num() )
	{
		Close();
	}
}

/*
================
idSaveGame::Close
================
*/
void idSaveGame::Close()
{
	WriteSoundCommands();

	// read trace models
	BeginTraceScope( "<tracemodels>" );	//puncho - stream-trace (P1-A1c, K1)
	idClipModel::SaveTraceModels( this );
	EndTraceScope();	//puncho - stream-trace

	for( int i = 1; i < objects.Num(); i++ )
	{
		//puncho start - stream-trace (P1-A1c, K1): the object index of the scopes its Save chain opens
		if( streamTrace != NULL )
		{
			streamTrace->SetObject( i );
		}
		//puncho end
		CallSave_r( objects[ i ]->GetType(), objects[ i ] );
	}

	objects.Clear();

	// Save out the string table at the end of the file
	for( int i = 0; i < stringTable.Num(); ++i )
	{
		stringFile->WriteString( stringTable[i].string );
	}

	stringHash.Free();
	stringTable.Clear();

	if( file->Length() > MIN_SAVEGAME_SIZE_BYTES || stringFile->Length() > MAX_SAVEGAME_STRING_TABLE_SIZE )
	{
		idLib::FatalError( "OVERFLOWED SAVE GAME FILE BUFFER" );
	}

#ifdef ID_DEBUG_MEMORY
	idStr gameState = file->GetName();
	gameState.StripFileExtension();
	WriteGameState_f( idCmdArgs( va( "test %s_save", gameState.c_str() ), false ) );
#endif
}

/*
================
idSaveGame::WriteDecls
================
*/
void idSaveGame::WriteDecls()
{
	WriteInt( declManager->GetNumDeclTypes() );
	// Write out all loaded decls
	for( int t = 0; t < declManager->GetNumDeclTypes(); t++ )
	{
		for( int d = 0; d < declManager->GetNumDecls( ( declType_t )t ); d++ )
		{
			const idDecl* decl = declManager->DeclByIndex( ( declType_t )t, d, false );
			if( decl == NULL || decl->GetState() == DS_UNPARSED )
			{
				continue;
			}
			const char* declName = decl->GetName();
			if( declName[0] == 0 )
			{
				continue;
			}
			WriteString( declName );
		}
		WriteString( 0 );
	}
}

/*
================
idSaveGame::WriteObjectList
================
*/
void idSaveGame::WriteObjectList()
{
	WriteInt( objects.Num() - 1 );
	for( int i = 1; i < objects.Num(); i++ )
	{
		WriteString( objects[ i ]->GetClassname() );
	}
}

/*
================
idSaveGame::CallSave_r
================
*/
void idSaveGame::CallSave_r( const idTypeInfo* cls, const idClass* obj )
{
	if( cls->super )
	{
		CallSave_r( cls->super, obj );
		if( !cls->ownSave )	//puncho - OQ-B7: was cls->super->Save == cls->Save, which identical-code folding can fool
		{
			// don't call save on this inheritance level since the function was called in the super class
			return;
		}
	}

	//puncho start - stream-trace (P1-A1c, K1): each class level's Save is a scope, (object index, class name); a
	// WriteStaticObject reaches here too and nests inside its owner's
	if( streamTrace != NULL )
	{
		streamTrace->BeginScope( cls->classname );
		( obj->*cls->Save )( this );
		streamTrace->EndScope();
		return;
	}
	//puncho end

	( obj->*cls->Save )( this );
}

//puncho start - stream-trace (P1-A1c, K1)
/*
================
idSaveGame::BeginTraceScope
================
*/
void idSaveGame::BeginTraceScope( const char* name )
{
	if( streamTrace != NULL )
	{
		streamTrace->BeginScope( name );
	}
}

/*
================
idSaveGame::EndTraceScope
================
*/
void idSaveGame::EndTraceScope()
{
	if( streamTrace != NULL )
	{
		streamTrace->EndScope();
	}
}
//puncho end

/*
================
idSaveGame::AddObject
================
*/
void idSaveGame::AddObject( const idClass* obj )
{
	objects.AddUnique( obj );
}

/*
================
idSaveGame::Write
================
*/
void idSaveGame::Write( const void* buffer, int len )
{
	file->Write( buffer, len );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_RAW, len, 0 );
	}
	//puncho end
}

/*
================
idSaveGame::WriteInt
================
*/
void idSaveGame::WriteInt( const int value )
{
	file->WriteBig( value );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_INT, sizeof( value ), 0 );
	}
	//puncho end
}

/*
================
idSaveGame::WriteJoint
================
*/
void idSaveGame::WriteJoint( const jointHandle_t value )
{
	file->WriteBig( ( int& )value );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_JOINT, sizeof( int ), 0 );
	}
	//puncho end
}

/*
================
idSaveGame::WriteShort
================
*/
void idSaveGame::WriteShort( const short value )
{
	file->WriteBig( value );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_SHORT, sizeof( value ), 0 );
	}
	//puncho end
}

/*
================
idSaveGame::WriteByte
================
*/
void idSaveGame::WriteByte( const byte value )
{
	file->Write( &value, sizeof( value ) );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_BYTE, sizeof( value ), 0 );
	}
	//puncho end
}

/*
================
idSaveGame::WriteSignedChar
================
*/
void idSaveGame::WriteSignedChar( const signed char value )
{
	file->Write( &value, sizeof( value ) );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_SIGNEDCHAR, sizeof( value ), 0 );
	}
	//puncho end
}

/*
================
idSaveGame::WriteFloat
================
*/
void idSaveGame::WriteFloat( const float value )
{
	file->WriteBig( value );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_FLOAT, sizeof( value ), 0 );
	}
	//puncho end
}

/*
================
idSaveGame::WriteBool
================
*/
void idSaveGame::WriteBool( const bool value )
{
	file->WriteBool( value );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_BOOL, sizeof( value ), 0 );
	}
	//puncho end
}

/*
================
idSaveGame::WriteString
================
*/
void idSaveGame::WriteString( const char* string )
{
	//puncho start - stream-trace (P1-A1c, K1): one exit, so the string is one STRING primitive hashed by its text, never by
	// its table offset; the offset goes out through file->WriteBig as WriteInt sent it
	int offset = -1;
	if( string != NULL && *string != 0 )
	{
		// If we already have this string in our hash, write out of the offset in the table
		int hash = stringHash.GenerateKey( string );
		for( int i = stringHash.First( hash ); i != -1; i = stringHash.Next( i ) )
		{
			if( stringTable[i].string.Cmp( string ) == 0 )
			{
				offset = stringTable[i].offset;
				break;
			}
		}

		if( offset < 0 )
		{
			// Add the string to our hash, generate the index, and update our current table offset
			stringTableIndex_s& tableIndex = stringTable.Alloc();
			tableIndex.offset = curStringTableOffset;
			tableIndex.string = string;
			stringHash.Add( hash, stringTable.Num() - 1 );

			offset = curStringTableOffset;
			curStringTableOffset += ( strlen( string ) + 4 );
		}
	}

	file->WriteBig( offset );
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_STRING, sizeof( offset ), idStreamTrace::StringHash( string ) );
	}
	//puncho end
}

/*
================
idSaveGame::WriteVec2
================
*/
void idSaveGame::WriteVec2( const idVec2& vec )
{
	file->WriteBig( vec );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_VEC2, sizeof( vec ), 0 );
	}
	//puncho end
}

/*
================
idSaveGame::WriteVec3
================
*/
void idSaveGame::WriteVec3( const idVec3& vec )
{
	file->WriteBig( vec );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_VEC3, sizeof( vec ), 0 );
	}
	//puncho end
}

/*
================
idSaveGame::WriteVec4
================
*/
void idSaveGame::WriteVec4( const idVec4& vec )
{
	file->WriteBig( vec );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_VEC4, sizeof( vec ), 0 );
	}
	//puncho end
}

/*
================
idSaveGame::WriteVec6
================
*/
void idSaveGame::WriteVec6( const idVec6& vec )
{
	file->WriteBig( vec );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_VEC6, sizeof( vec ), 0 );
	}
	//puncho end
}

/*
================
idSaveGame::WriteBounds
================
*/
void idSaveGame::WriteBounds( const idBounds& bounds )
{
	file->WriteBig( bounds );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_BOUNDS, sizeof( bounds ), 0 );
	}
	//puncho end
}

/*
================
idSaveGame::WriteBounds
================
*/
void idSaveGame::WriteWinding( const idWinding& w )
{
	int i, num;
	num = w.GetNumPoints();
	file->WriteBig( num );
	for( i = 0; i < num; i++ )
	{
		idVec5 v = w[i];
		file->WriteBig( v );
	}
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_WINDING, sizeof( num ) + num * sizeof( idVec5 ), 0 );
	}
	//puncho end
}


/*
================
idSaveGame::WriteMat3
================
*/
void idSaveGame::WriteMat3( const idMat3& mat )
{
	file->WriteBig( mat );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_MAT3, sizeof( mat ), 0 );
	}
	//puncho end
}

/*
================
idSaveGame::WriteAngles
================
*/
void idSaveGame::WriteAngles( const idAngles& angles )
{
	file->WriteBig( angles );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_ANGLES, sizeof( angles ), 0 );
	}
	//puncho end
}

/*
================
idSaveGame::WriteObject
================
*/
void idSaveGame::WriteObject( const idClass* obj )
{
	int index;

	index = objects.FindIndex( obj );
	if( index < 0 )
	{
		gameLocal.DPrintf( "idSaveGame::WriteObject - WriteObject FindIndex failed\n" );

		// Use the NULL index
		index = 0;
	}

	//puncho start - stream-trace (P1-A1c): one OBJECT primitive; the index goes out as WriteInt sent it
	file->WriteBig( index );
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_OBJECT, sizeof( index ), 0 );
	}
	//puncho end
}

//puncho start - saves (P1-m10b)
/*
================
idSaveGame::WriteRenderHandle

WriteInt's bytes, traced as an SP_HANDLE named by field: the one entry the round trip's byte compare lets differ.
================
*/
void idSaveGame::WriteRenderHandle( const int handle, const char* field )
{
	file->WriteBig( handle );
	if( streamTrace != NULL )
	{
		streamTrace->Handle( field );
	}
}
//puncho end

/*
================
idSaveGame::WriteStaticObject
================
*/
void idSaveGame::WriteStaticObject( const idClass& obj )
{
	CallSave_r( obj.GetType(), &obj );
}

/*
================
idSaveGame::WriteDict
================
*/
void idSaveGame::WriteDict( const idDict* dict )
{
	int num;
	int i;
	const idKeyValue* kv;

	//puncho start - stream-trace (P1-A1c, K1): a struct scope, one entry in its owner however many keys it holds
	if( streamTrace != NULL && streamTrace->EnterStruct( "idDict" ) )
	{
		WriteDict( dict );
		streamTrace->EndScope();
		return;
	}
	//puncho end

	if( !dict )
	{
		WriteInt( -1 );
	}
	else
	{
		num = dict->GetNumKeyVals();
		WriteInt( num );
		for( i = 0; i < num; i++ )
		{
			kv = dict->GetKeyVal( i );
			WriteString( kv->GetKey() );
			WriteString( kv->GetValue() );
		}
	}
}

/*
================
idSaveGame::WriteMaterial
================
*/
void idSaveGame::WriteMaterial( const idMaterial* material )
{
	if( !material )
	{
		WriteString( "" );
	}
	else
	{
		WriteString( material->GetName() );
	}
}

/*
================
idSaveGame::WriteSkin
================
*/
void idSaveGame::WriteSkin( const idDeclSkin* skin )
{
	if( !skin )
	{
		WriteString( "" );
	}
	else
	{
		WriteString( skin->GetName() );
	}
}

/*
================
idSaveGame::WriteParticle
================
*/
void idSaveGame::WriteParticle( const idDeclParticle* particle )
{
	if( !particle )
	{
		WriteString( "" );
	}
	else
	{
		WriteString( particle->GetName() );
	}
}

/*
================
idSaveGame::WriteFX
================
*/
void idSaveGame::WriteFX( const idDeclFX* fx )
{
	if( !fx )
	{
		WriteString( "" );
	}
	else
	{
		WriteString( fx->GetName() );
	}
}

/*
================
idSaveGame::WriteModelDef
================
*/
void idSaveGame::WriteModelDef( const idDeclModelDef* modelDef )
{
	if( !modelDef )
	{
		WriteString( "" );
	}
	else
	{
		WriteString( modelDef->GetName() );
	}
}

/*
================
idSaveGame::WriteSoundShader
================
*/
void idSaveGame::WriteSoundShader( const idSoundShader* shader )
{
	const char* name;

	if( !shader )
	{
		WriteString( "" );
	}
	else
	{
		name = shader->GetName();
		WriteString( name );
	}
}

/*
================
idSaveGame::WriteModel
================
*/
void idSaveGame::WriteModel( const idRenderModel* model )
{
	const char* name;

	if( !model )
	{
		WriteString( "" );
	}
	else
	{
		name = model->Name();
		WriteString( name );
	}
}

/*
================
idSaveGame::WriteUserInterface
================
*/
void idSaveGame::WriteUserInterface( const idUserInterface* ui, bool unique )
{
	const char* name;

	if( !ui )
	{
		WriteString( "" );
	}
	else
	{
		name = ui->Name();
		WriteString( name );
		WriteBool( unique );
		//puncho start - stream-trace (P1-A1c, K1): the GUI's state bypasses the primitives, one OPAQUE entry
		bool written;
		if( streamTrace != NULL )
		{
			const int start = streamTrace->Tell();
			written = ui->WriteToSaveGame( file );
			streamTrace->Opaque( start );
		}
		else
		{
			written = ui->WriteToSaveGame( file );
		}
		//puncho end
		if( written == false )	//puncho - stream-trace (P1-A1c)
		{
			gameLocal.Error( "idSaveGame::WriteUserInterface: ui failed to write properly\n" );
		}
	}
}

/*
================
idSaveGame::WriteRenderEntity
================
*/
void idSaveGame::WriteRenderEntity( const renderEntity_t& renderEntity )
{
	int i;

	//puncho start - stream-trace (P1-A1c, K3): a struct scope; the owner writes the entity's handle beside it with
	// WriteRenderHandle (P1-m10b)
	if( streamTrace != NULL && streamTrace->EnterStruct( "renderEntity_t" ) )
	{
		WriteRenderEntity( renderEntity );
		streamTrace->EndScope();
		return;
	}
	//puncho end

	WriteModel( renderEntity.hModel );

	WriteInt( renderEntity.entityNum );
	WriteInt( renderEntity.bodyId );

	WriteBounds( renderEntity.bounds );

	// callback is set by class's Restore function

	WriteInt( renderEntity.suppressSurfaceInViewID );
	WriteInt( renderEntity.suppressShadowInViewID );
	WriteInt( renderEntity.suppressShadowInLightID );
	WriteInt( renderEntity.allowSurfaceInViewID );

	WriteVec3( renderEntity.origin );
	WriteMat3( renderEntity.axis );

	WriteMaterial( renderEntity.customShader );
	WriteMaterial( renderEntity.referenceShader );
	WriteSkin( renderEntity.customSkin );

	if( renderEntity.referenceSound != NULL )
	{
		WriteInt( renderEntity.referenceSound->Index() );
	}
	else
	{
		WriteInt( 0 );
	}

	for( i = 0; i < MAX_ENTITY_SHADER_PARMS; i++ )
	{
		WriteFloat( renderEntity.shaderParms[ i ] );
	}

	for( i = 0; i < MAX_RENDERENTITY_GUI; i++ )
	{
		WriteUserInterface( renderEntity.gui[ i ], renderEntity.gui[ i ] ? renderEntity.gui[ i ]->IsUniqued() : false );
	}

	WriteFloat( renderEntity.modelDepthHack );

	WriteBool( renderEntity.noSelfShadow );
	WriteBool( renderEntity.noShadow );
	WriteBool( renderEntity.noDynamicInteractions );
	WriteBool( renderEntity.weaponDepthHack );

	WriteInt( renderEntity.forceUpdate );

	WriteInt( renderEntity.timeGroup );
	WriteInt( renderEntity.xrayIndex );
}

/*
================
idSaveGame::WriteRenderLight
================
*/
void idSaveGame::WriteRenderLight( const renderLight_t& renderLight )
{
	int i;

	//puncho start - stream-trace (P1-A1c, K3): a struct scope, as WriteRenderEntity's
	if( streamTrace != NULL && streamTrace->EnterStruct( "renderLight_t" ) )
	{
		WriteRenderLight( renderLight );
		streamTrace->EndScope();
		return;
	}
	//puncho end

	WriteMat3( renderLight.axis );
	WriteVec3( renderLight.origin );

	WriteInt( renderLight.suppressLightInViewID );
	WriteInt( renderLight.allowLightInViewID );
	WriteBool( renderLight.noShadows );
	WriteBool( renderLight.noSpecular );
	WriteBool( renderLight.pointLight );
	WriteBool( renderLight.parallel );

	WriteVec3( renderLight.lightRadius );
	WriteVec3( renderLight.lightCenter );

	WriteVec3( renderLight.target );
	WriteVec3( renderLight.right );
	WriteVec3( renderLight.up );
	WriteVec3( renderLight.start );
	WriteVec3( renderLight.end );

	// only idLight has a prelightModel and it's always based on the entityname, so we'll restore it there
	// WriteModel( renderLight.prelightModel );

	WriteInt( renderLight.lightId );

	WriteMaterial( renderLight.shader );

	for( i = 0; i < MAX_ENTITY_SHADER_PARMS; i++ )
	{
		WriteFloat( renderLight.shaderParms[ i ] );
	}

	if( renderLight.referenceSound != NULL )
	{
		WriteInt( renderLight.referenceSound->Index() );
	}
	else
	{
		WriteInt( 0 );
	}

	const mathLightData_t& m = renderLight.mathLight;
	WriteBool(m.enabled);
	WriteInt(m.type);
	WriteInt(m.falloff);
	WriteInt(m.pattern);
	WriteFloat(m.falloffExp);
	WriteFloat(m.innerRadius);
	WriteFloat(m.shapeExp);
	WriteFloat(m.penumbra);
	WriteFloat(m.patternStrength);
	WriteFloat(m.patternScale);
	WriteFloat(m.patternPhase);
	WriteBool(m.energyClamp);
}

// RB begin
void idSaveGame::WriteRenderEnvprobe( const renderEnvironmentProbe_t& renderEnvprobe )
{
	WriteVec3( renderEnvprobe.origin );

	WriteInt( renderEnvprobe.suppressEnvprobeInViewID );
	WriteInt( renderEnvprobe.allowEnvprobeInViewID );

	for( int i = 0; i < MAX_ENTITY_SHADER_PARMS; i++ )
	{
		WriteFloat( renderEnvprobe.shaderParms[ i ] );
	}
}
// Rb end

/*
================
idSaveGame::WriteRefSound
================
*/
void idSaveGame::WriteRefSound( const refSound_t& refSound )
{
	if( refSound.referenceSound )
	{
		WriteInt( refSound.referenceSound->Index() );
	}
	else
	{
		WriteInt( 0 );
	}
	WriteVec3( refSound.origin );
	WriteInt( refSound.listenerId );
	WriteSoundShader( refSound.shader );
	WriteFloat( refSound.diversity );
	WriteBool( refSound.waitfortrigger );

	WriteFloat( refSound.parms.minDistance );
	WriteFloat( refSound.parms.maxDistance );
	WriteFloat( refSound.parms.volume );
	WriteFloat( refSound.parms.shakes );
	WriteInt( refSound.parms.soundShaderFlags );
	WriteInt( refSound.parms.soundClass );
}

/*
================
idSaveGame::WriteRenderView
================
*/
void idSaveGame::WriteRenderView( const renderView_t& view )
{
	int i;

	WriteInt( view.viewID );
	WriteInt( 0 /* view.x */ );
	WriteInt( 0 /* view.y */ );
	WriteInt( 0 /* view.width */ );
	WriteInt( 0 /* view.height */ );

	WriteFloat( view.fov_x );
	WriteFloat( view.fov_y );
	WriteVec3( view.vieworg );
	WriteMat3( view.viewaxis );

	WriteBool( view.cramZNear );

	WriteInt( view.time[0] );

	for( i = 0; i < MAX_GLOBAL_SHADER_PARMS; i++ )
	{
		WriteFloat( view.shaderParms[ i ] );
	}
}

/*
===================
idSaveGame::WriteUsercmd
===================
*/
void idSaveGame::WriteUsercmd( const usercmd_t& usercmd )
{
	//puncho start - stream-trace (P1-A1c, K3): a struct scope; the save driver's dummy one is all zeros (usercmd_t's
	// constructor), so it compares like any other
	if( streamTrace != NULL && streamTrace->EnterStruct( "usercmd_t" ) )
	{
		WriteUsercmd( usercmd );
		streamTrace->EndScope();
		return;
	}
	//puncho end

	WriteByte( usercmd.buttons );
	WriteSignedChar( usercmd.forwardmove );
	WriteSignedChar( usercmd.rightmove );
	WriteShort( usercmd.angles[0] );
	WriteShort( usercmd.angles[1] );
	WriteShort( usercmd.angles[2] );
	WriteShort( usercmd.mx );
	WriteShort( usercmd.my );
	WriteByte( usercmd.impulse );
	WriteByte( usercmd.impulseSequence );
}

/*
===================
idSaveGame::WriteContactInfo
===================
*/
void idSaveGame::WriteContactInfo( const contactInfo_t& contactInfo )
{
	WriteInt( ( int )contactInfo.type );
	WriteVec3( contactInfo.point );
	WriteVec3( contactInfo.normal );
	WriteFloat( contactInfo.dist );
	WriteInt( contactInfo.contents );
	WriteMaterial( contactInfo.material );
	WriteInt( contactInfo.modelFeature );
	WriteInt( contactInfo.trmFeature );
	WriteInt( contactInfo.entityNum );
	WriteInt( contactInfo.id );
}

/*
===================
idSaveGame::WriteTrace
===================
*/
void idSaveGame::WriteTrace( const trace_t& trace )
{
	WriteFloat( trace.fraction );
	WriteVec3( trace.endpos );
	WriteMat3( trace.endAxis );
	WriteContactInfo( trace.c );
}

/*
 ===================
 idRestoreGame::WriteTraceModel
 ===================
 */
void idSaveGame::WriteTraceModel( const idTraceModel& trace )
{
	int j, k;

	WriteInt( ( int& )trace.type );
	WriteInt( trace.numVerts );
	for( j = 0; j < MAX_TRACEMODEL_VERTS; j++ )
	{
		WriteVec3( trace.verts[j] );
	}
	WriteInt( trace.numEdges );
	for( j = 0; j < ( MAX_TRACEMODEL_EDGES + 1 ); j++ )
	{
		WriteInt( trace.edges[j].v[0] );
		WriteInt( trace.edges[j].v[1] );
		WriteVec3( trace.edges[j].normal );
	}
	WriteInt( trace.numPolys );
	for( j = 0; j < MAX_TRACEMODEL_POLYS; j++ )
	{
		WriteVec3( trace.polys[j].normal );
		WriteFloat( trace.polys[j].dist );
		WriteBounds( trace.polys[j].bounds );
		WriteInt( trace.polys[j].numEdges );
		for( k = 0; k < MAX_TRACEMODEL_POLYEDGES; k++ )
		{
			WriteInt( trace.polys[j].edges[k] );
		}
	}
	WriteVec3( trace.offset );
	WriteBounds( trace.bounds );
	WriteBool( trace.isConvex );
	// padding win32 native structs
	char tmp[3];
	memset( tmp, 0, sizeof( tmp ) );
	file->Write( tmp, 3 );
	//puncho start - stream-trace (P1-A1c, K1): the padding bypasses the primitives, one OPAQUE entry
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_OPAQUE, sizeof( tmp ), 0 );
	}
	//puncho end
}

/*
===================
idSaveGame::WriteClipModel
===================
*/
void idSaveGame::WriteClipModel( const idClipModel* clipModel )
{
	if( clipModel != NULL )
	{
		WriteBool( true );
		//puncho start - stream-trace (P1-A1c, K3): idClipModel is no idClass, so its Save gets its scope here
		if( streamTrace != NULL )
		{
			streamTrace->BeginScope( "idClipModel" );
			clipModel->Save( this );
			streamTrace->EndScope();
		}
		else
		{
			clipModel->Save( this );
		}
		//puncho end
	}
	else
	{
		WriteBool( false );
	}
}

/*
===================
idSaveGame::WriteSoundCommands
===================
*/
void idSaveGame::WriteSoundCommands()
{
	//puncho start - stream-trace (P1-A1c, K1): the sound world bypasses the primitives, one OPAQUE entry in <sound>
	if( streamTrace != NULL )
	{
		streamTrace->BeginScope( "<sound>" );
		const int start = streamTrace->Tell();
		gameSoundWorld->WriteToSaveGame( file );
		streamTrace->Opaque( start );
		streamTrace->EndScope();
		return;
	}
	//puncho end

	gameSoundWorld->WriteToSaveGame( file );
}

/*
======================
idSaveGame::WriteBuildNumber
======================
*/
void idSaveGame::WriteBuildNumber( const int value )
{
	WriteInt( BUILD_NUMBER );
}

/***********************************************************************

	idRestoreGame

***********************************************************************/

/*
================
idRestoreGame::RestoreGame
================
*/
idRestoreGame::idRestoreGame( idFile* savefile, idFile* stringTableFile, int saveVersion )
{
	file = savefile;
	stringFile = stringTableFile;
	version = saveVersion;

	//puncho start - stream-trace (P1-A1c, K1): an armed trace sees every byte from here on
	streamTrace = idStreamTrace::armed;
	if( streamTrace != NULL )
	{
		file = streamTrace->Attach( savefile );
	}
	//puncho end
}

/*
================
idRestoreGame::~idRestoreGame()
================
*/
idRestoreGame::~idRestoreGame()
{
}

/*
================
idRestoreGame::ReadDecls
================
*/
void idRestoreGame::ReadDecls()
{
	idStr declName;
	int types = 15;
	if( GetBuildNumber() >= BUILD_NUMBER_SAVE_VERSION_WORLD_STORE )
	{
		ReadInt( types );
	}
	if( types < 0 || types > 256 )
	{
		Error( "Invalid saved decl type count" );
	}
	for( int t = 0; t < types; t++ )
	{
		while( true )
		{
			ReadString( declName );
			if( declName.IsEmpty() )
			{
				break;
			}
			if( t < declManager->GetNumDeclTypes() )
			{
				declManager->FindType( ( declType_t )t, declName );
			}
		}
	}
}

/*
================
void idRestoreGame::CreateObjects
================
*/
void idRestoreGame::CreateObjects()
{
	int i, num;
	idStr classname;
	idTypeInfo* type;

	ReadInt( num );

	// create all the objects
	objects.SetNum( num + 1 );
	memset( objects.Ptr(), 0, sizeof( objects[ 0 ] ) * objects.Num() );

	for( i = 1; i < objects.Num(); i++ )
	{
		ReadString( classname );
		type = idClass::GetClass( classname );
		if( type == NULL )
		{
			Error( "idRestoreGame::CreateObjects: Unknown class '%s'", classname.c_str() );
			return;
		}
		objects[ i ] = type->CreateInstance();

#ifdef ID_DEBUG_MEMORY
		InitTypeVariables( objects[i], type->classname, 0xce );
#endif
	}
}

/*
================
void idRestoreGame::RestoreObjects
================
*/
void idRestoreGame::RestoreObjects()
{
	int i;

	ReadSoundCommands();

	// read trace models
	BeginTraceScope( "<tracemodels>" );	//puncho - stream-trace (P1-A1c, K1)
	idClipModel::RestoreTraceModels( this );
	EndTraceScope();	//puncho - stream-trace

	// restore all the objects
	for( i = 1; i < objects.Num(); i++ )
	{
		//puncho start - stream-trace (P1-A1c, K1): the object index of the scopes its Restore chain opens
		if( streamTrace != NULL )
		{
			streamTrace->SetObject( i );
		}
		//puncho end
		CallRestore_r( objects[ i ]->GetType(), objects[ i ] );
	}

	// regenerate render entities and render lights because are not saved
	for( i = 1; i < objects.Num(); i++ )
	{
		if( objects[ i ]->IsType( idEntity::Type ) )
		{
			idEntity* ent = static_cast<idEntity*>( objects[ i ] );
			ent->UpdateVisuals();
			ent->Present();
		}
	}

#ifdef ID_DEBUG_MEMORY
	idStr gameState = file->GetName();
	gameState.StripFileExtension();
	WriteGameState_f( idCmdArgs( va( "test %s_restore", gameState.c_str() ), false ) );
	//CompareGameState_f( idCmdArgs( va( "test %s_save", gameState.c_str() ) ) );
	gameLocal.Error( "dumped game states" );
#endif
}

/*
====================
void idRestoreGame::DeleteObjects
====================
*/
void idRestoreGame::DeleteObjects()
{

	// Remove the NULL object before deleting
	objects.RemoveIndex( 0 );

	objects.DeleteContents( true );
}

/*
================
idRestoreGame::Error
================
*/
void idRestoreGame::Error( const char* fmt, ... )
{
	va_list	argptr;
	char	text[ 1024 ];

	va_start( argptr, fmt );
	idStr::vsnPrintf( text, sizeof( text ), fmt, argptr );
	va_end( argptr );

	objects.DeleteContents( true );

	gameLocal.Error( "%s", text );
}

/*
================
idRestoreGame::CallRestore_r
================
*/
void idRestoreGame::CallRestore_r( const idTypeInfo* cls, idClass* obj )
{
	if( cls->super )
	{
		CallRestore_r( cls->super, obj );
		if( !cls->ownRestore )	//puncho - OQ-B7: was cls->super->Restore == cls->Restore
		{
			// don't call save on this inheritance level since the function was called in the super class
			return;
		}
	}

	//puncho start - stream-trace (P1-A1c, K1): the scope CallSave_r opened for this class level
	if( streamTrace != NULL )
	{
		streamTrace->BeginScope( cls->classname );
		( obj->*cls->Restore )( this );
		streamTrace->EndScope();
		return;
	}
	//puncho end

	( obj->*cls->Restore )( this );
}

//puncho start - stream-trace (P1-A1c, K1)
/*
================
idRestoreGame::BeginTraceScope
================
*/
void idRestoreGame::BeginTraceScope( const char* name )
{
	if( streamTrace != NULL )
	{
		streamTrace->BeginScope( name );
	}
}

/*
================
idRestoreGame::EndTraceScope
================
*/
void idRestoreGame::EndTraceScope()
{
	if( streamTrace != NULL )
	{
		streamTrace->EndScope();
	}
}
//puncho end

/*
================
idRestoreGame::Read
================
*/
void idRestoreGame::Read( void* buffer, int len )
{
	file->Read( buffer, len );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_RAW, len, 0 );
	}
	//puncho end
}

/*
================
idRestoreGame::ReadInt
================
*/
void idRestoreGame::ReadInt( int& value )
{
	file->ReadBig( value );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_INT, sizeof( value ), 0 );
	}
	//puncho end
}

/*
================
idRestoreGame::ReadJoint
================
*/
void idRestoreGame::ReadJoint( jointHandle_t& value )
{
	file->ReadBig( ( int& )value );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_JOINT, sizeof( int ), 0 );
	}
	//puncho end
}

/*
================
idRestoreGame::ReadShort
================
*/
void idRestoreGame::ReadShort( short& value )
{
	file->ReadBig( value );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_SHORT, sizeof( value ), 0 );
	}
	//puncho end
}

/*
================
idRestoreGame::ReadByte
================
*/
void idRestoreGame::ReadByte( byte& value )
{
	file->Read( &value, sizeof( value ) );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_BYTE, sizeof( value ), 0 );
	}
	//puncho end
}

/*
================
idRestoreGame::ReadSignedChar
================
*/
void idRestoreGame::ReadSignedChar( signed char& value )
{
	file->Read( &value, sizeof( value ) );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_SIGNEDCHAR, sizeof( value ), 0 );
	}
	//puncho end
}

/*
================
idRestoreGame::ReadFloat
================
*/
void idRestoreGame::ReadFloat( float& value )
{
	file->ReadBig( value );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_FLOAT, sizeof( value ), 0 );
	}
	//puncho end
}

/*
================
idRestoreGame::ReadBool
================
*/
void idRestoreGame::ReadBool( bool& value )
{
	file->ReadBig( value );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_BOOL, sizeof( value ), 0 );
	}
	//puncho end
}

/*
================
idRestoreGame::ReadString
================
*/
void idRestoreGame::ReadString( idStr& string )
{
	string.Empty();

	//puncho start - stream-trace (P1-A1c, K1): one exit, so the string is one STRING primitive hashed by the text it read;
	// the offset comes in through file->ReadBig as ReadInt took it
	int offset = -1;
	file->ReadBig( offset );

	if( offset >= 0 )
	{
		stringFile->Seek( offset, FS_SEEK_SET );
		stringFile->ReadString( string );
	}

	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_STRING, sizeof( offset ), idStreamTrace::StringHash( string.c_str() ) );
	}
	//puncho end
}

/*
================
idRestoreGame::ReadVec2
================
*/
void idRestoreGame::ReadVec2( idVec2& vec )
{
	file->ReadBig( vec );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_VEC2, sizeof( vec ), 0 );
	}
	//puncho end
}

/*
================
idRestoreGame::ReadVec3
================
*/
void idRestoreGame::ReadVec3( idVec3& vec )
{
	file->ReadBig( vec );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_VEC3, sizeof( vec ), 0 );
	}
	//puncho end
}

/*
================
idRestoreGame::ReadVec4
================
*/
void idRestoreGame::ReadVec4( idVec4& vec )
{
	file->ReadBig( vec );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_VEC4, sizeof( vec ), 0 );
	}
	//puncho end
}

/*
================
idRestoreGame::ReadVec6
================
*/
void idRestoreGame::ReadVec6( idVec6& vec )
{
	file->ReadBig( vec );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_VEC6, sizeof( vec ), 0 );
	}
	//puncho end
}

/*
================
idRestoreGame::ReadBounds
================
*/
void idRestoreGame::ReadBounds( idBounds& bounds )
{
	file->ReadBig( bounds );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_BOUNDS, sizeof( bounds ), 0 );
	}
	//puncho end
}

/*
================
idRestoreGame::ReadWinding
================
*/
void idRestoreGame::ReadWinding( idWinding& w )
{
	int i, num;
	file->ReadBig( num );	//puncho - stream-trace (P1-A1c): as ReadInt took it, so the winding is one primitive
	w.SetNumPoints( num );
	for( i = 0; i < num; i++ )
	{
		idVec5& v = w[i];
		file->ReadBig( v );
	}
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_WINDING, sizeof( num ) + num * sizeof( idVec5 ), 0 );
	}
	//puncho end
}

/*
================
idRestoreGame::ReadMat3
================
*/
void idRestoreGame::ReadMat3( idMat3& mat )
{
	file->ReadBig( mat );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_MAT3, sizeof( mat ), 0 );
	}
	//puncho end
}

/*
================
idRestoreGame::ReadAngles
================
*/
void idRestoreGame::ReadAngles( idAngles& angles )
{
	file->ReadBig( angles );
	//puncho start - stream-trace (P1-A1c)
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_ANGLES, sizeof( angles ), 0 );
	}
	//puncho end
}

/*
================
idRestoreGame::ReadObject
================
*/
void idRestoreGame::ReadObject( idClass*& obj )
{
	int index;

	//puncho start - stream-trace (P1-A1c): one OBJECT primitive; the index comes in as ReadInt took it
	file->ReadBig( index );
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_OBJECT, sizeof( index ), 0 );
	}
	//puncho end
	if( ( index < 0 ) || ( index >= objects.Num() ) )
	{
		Error( "idRestoreGame::ReadObject: invalid object index" );
	}
	obj = objects[ index ];
}

//puncho start - saves (P1-m10b)
/*
================
idRestoreGame::ReadRenderHandle

The int WriteRenderHandle wrote, traced under the same field name.
================
*/
void idRestoreGame::ReadRenderHandle( int& handle, const char* field )
{
	file->ReadBig( handle );
	if( streamTrace != NULL )
	{
		streamTrace->Handle( field );
	}
}
//puncho end

/*
================
idRestoreGame::ReadStaticObject
================
*/
void idRestoreGame::ReadStaticObject( idClass& obj )
{
	CallRestore_r( obj.GetType(), &obj );
}

/*
================
idRestoreGame::ReadDict
================
*/
void idRestoreGame::ReadDict( idDict* dict )
{
	int num;
	int i;
	idStr key;
	idStr value;

	//puncho start - stream-trace (P1-A1c, K1): the struct scope WriteDict opened
	if( streamTrace != NULL && streamTrace->EnterStruct( "idDict" ) )
	{
		ReadDict( dict );
		streamTrace->EndScope();
		return;
	}
	//puncho end

	ReadInt( num );

	if( num < 0 )
	{
		dict = NULL;
	}
	else
	{
		dict->Clear();
		for( i = 0; i < num; i++ )
		{
			ReadString( key );
			ReadString( value );
			dict->Set( key, value );
		}
	}
}

/*
================
idRestoreGame::ReadMaterial
================
*/
void idRestoreGame::ReadMaterial( const idMaterial*& material )
{
	idStr name;

	ReadString( name );
	if( !name.Length() )
	{
		material = NULL;
	}
	else
	{
		material = declManager->FindMaterial( name );
	}
}

/*
================
idRestoreGame::ReadSkin
================
*/
void idRestoreGame::ReadSkin( const idDeclSkin*& skin )
{
	idStr name;

	ReadString( name );
	if( !name.Length() )
	{
		skin = NULL;
	}
	else
	{
		skin = declManager->FindSkin( name );
	}
}

/*
================
idRestoreGame::ReadParticle
================
*/
void idRestoreGame::ReadParticle( const idDeclParticle*& particle )
{
	idStr name;

	ReadString( name );
	if( !name.Length() )
	{
		particle = NULL;
	}
	else
	{
		particle = static_cast<const idDeclParticle*>( declManager->FindType( DECL_PARTICLE, name ) );
	}
}

/*
================
idRestoreGame::ReadFX
================
*/
void idRestoreGame::ReadFX( const idDeclFX*& fx )
{
	idStr name;

	ReadString( name );
	if( !name.Length() )
	{
		fx = NULL;
	}
	else
	{
		fx = static_cast<const idDeclFX*>( declManager->FindType( DECL_FX, name ) );
	}
}

/*
================
idRestoreGame::ReadSoundShader
================
*/
void idRestoreGame::ReadSoundShader( const idSoundShader*& shader )
{
	idStr name;

	ReadString( name );
	if( !name.Length() )
	{
		shader = NULL;
	}
	else
	{
		shader = declManager->FindSound( name );
	}
}

/*
================
idRestoreGame::ReadModelDef
================
*/
void idRestoreGame::ReadModelDef( const idDeclModelDef*& modelDef )
{
	idStr name;

	ReadString( name );
	if( !name.Length() )
	{
		modelDef = NULL;
	}
	else
	{
		modelDef = static_cast<const idDeclModelDef*>( declManager->FindType( DECL_MODELDEF, name, false ) );
	}
}

/*
================
idRestoreGame::ReadModel
================
*/
void idRestoreGame::ReadModel( idRenderModel*& model )
{
	idStr name;

	ReadString( name );
	if( !name.Length() )
	{
		model = NULL;
	}
	else
	{
		model = renderModelManager->FindModel( name );
	}
}

/*
================
idRestoreGame::ReadUserInterface
================
*/
void idRestoreGame::ReadUserInterface( idUserInterface*& ui )
{
	idStr name;

	ReadString( name );
	if( !name.Length() )
	{
		ui = NULL;
	}
	else
	{
		bool unique;
		ReadBool( unique );
		ui = uiManager->FindGui( name, true, unique );
		if( ui )
		{
			//puncho start - stream-trace (P1-A1c, K1): the GUI's state bypasses the primitives, one OPAQUE entry
			bool restored;
			if( streamTrace != NULL )
			{
				const int start = streamTrace->Tell();
				restored = ui->ReadFromSaveGame( file );
				streamTrace->Opaque( start );
			}
			else
			{
				restored = ui->ReadFromSaveGame( file );
			}
			//puncho end
			if( restored == false )	//puncho - stream-trace (P1-A1c)
			{
				Error( "idSaveGame::ReadUserInterface: ui failed to read properly\n" );
			}
			else
			{
				ui->StateChanged( gameLocal.time );
			}
		}
	}
}

/*
================
idRestoreGame::ReadRenderEntity
================
*/
void idRestoreGame::ReadRenderEntity( renderEntity_t& renderEntity )
{
	int i;
	int index;

	//puncho start - stream-trace (P1-A1c, K3): the struct scope WriteRenderEntity opened
	if( streamTrace != NULL && streamTrace->EnterStruct( "renderEntity_t" ) )
	{
		ReadRenderEntity( renderEntity );
		streamTrace->EndScope();
		return;
	}
	//puncho end

	renderEntity.maxVisDist = renderEntity.minVisPixels = 0.0f;
	renderEntity.cullShadow = 0;
	renderEntity.lodScale = 1.0f;
	ReadModel( renderEntity.hModel );

	ReadInt( renderEntity.entityNum );
	ReadInt( renderEntity.bodyId );

	ReadBounds( renderEntity.bounds );

	// callback is set by class's Restore function
	renderEntity.callback = NULL;
	renderEntity.callbackData = NULL;

	ReadInt( renderEntity.suppressSurfaceInViewID );
	ReadInt( renderEntity.suppressShadowInViewID );
	ReadInt( renderEntity.suppressShadowInLightID );
	ReadInt( renderEntity.allowSurfaceInViewID );

	ReadVec3( renderEntity.origin );
	ReadMat3( renderEntity.axis );

	ReadMaterial( renderEntity.customShader );
	ReadMaterial( renderEntity.referenceShader );
	ReadSkin( renderEntity.customSkin );

	ReadInt( index );
	renderEntity.referenceSound = gameSoundWorld->EmitterForIndex( index );

	for( i = 0; i < MAX_ENTITY_SHADER_PARMS; i++ )
	{
		ReadFloat( renderEntity.shaderParms[ i ] );
	}

	for( i = 0; i < MAX_RENDERENTITY_GUI; i++ )
	{
		ReadUserInterface( renderEntity.gui[ i ] );
	}

	// idEntity will restore "cameraTarget", which will be used in idEntity::Present to restore the remoteRenderView
	renderEntity.remoteRenderView = NULL;

	renderEntity.joints = NULL;
	renderEntity.numJoints = 0;

	ReadFloat( renderEntity.modelDepthHack );

	ReadBool( renderEntity.noSelfShadow );
	ReadBool( renderEntity.noShadow );
	ReadBool( renderEntity.noDynamicInteractions );
	ReadBool( renderEntity.weaponDepthHack );

	ReadInt( renderEntity.forceUpdate );

	ReadInt( renderEntity.timeGroup );
	ReadInt( renderEntity.xrayIndex );
}

/*
================
idRestoreGame::ReadRenderLight
================
*/
void idRestoreGame::ReadRenderLight( renderLight_t& renderLight )
{
	int index;
	int i;

	//puncho start - stream-trace (P1-A1c, K3): the struct scope WriteRenderLight opened
	if( streamTrace != NULL && streamTrace->EnterStruct( "renderLight_t" ) )
	{
		ReadRenderLight( renderLight );
		streamTrace->EndScope();
		return;
	}
	//puncho end

	renderLight.maxVisDist = renderLight.minVisPixels = 0.0f;
	ReadMat3( renderLight.axis );
	ReadVec3( renderLight.origin );

	ReadInt( renderLight.suppressLightInViewID );
	ReadInt( renderLight.allowLightInViewID );
	ReadBool( renderLight.noShadows );
	ReadBool( renderLight.noSpecular );
	ReadBool( renderLight.pointLight );
	ReadBool( renderLight.parallel );

	ReadVec3( renderLight.lightRadius );
	ReadVec3( renderLight.lightCenter );

	ReadVec3( renderLight.target );
	ReadVec3( renderLight.right );
	ReadVec3( renderLight.up );
	ReadVec3( renderLight.start );
	ReadVec3( renderLight.end );

	ReadInt( renderLight.lightId );

	ReadMaterial( renderLight.shader );

	for( i = 0; i < MAX_ENTITY_SHADER_PARMS; i++ )
	{
		ReadFloat( renderLight.shaderParms[ i ] );
	}

	ReadInt( index );
	renderLight.referenceSound = gameSoundWorld->EmitterForIndex( index );

	if (GetBuildNumber() >= BUILD_NUMBER_SAVE_VERSION_MATH_LIGHTS)
	{
		mathLightData_t& m = renderLight.mathLight;
		ReadBool(m.enabled);
		ReadInt(m.type);
		ReadInt(m.falloff);
		ReadInt(m.pattern);
		ReadFloat(m.falloffExp);
		ReadFloat(m.innerRadius);
		ReadFloat(m.shapeExp);
		ReadFloat(m.penumbra);
		ReadFloat(m.patternStrength);
		ReadFloat(m.patternScale);
		ReadFloat(m.patternPhase);
		ReadBool(m.energyClamp);
	}
	else
	{
		memset(&renderLight.mathLight, 0, sizeof(renderLight.mathLight));
	}
}

// RB begin
void idRestoreGame::ReadRenderEnvprobe( renderEnvironmentProbe_t& renderEnvprobe )
{
	ReadVec3( renderEnvprobe.origin );

	ReadInt( renderEnvprobe.suppressEnvprobeInViewID );
	ReadInt( renderEnvprobe.allowEnvprobeInViewID );

	for( int i = 0; i < MAX_ENTITY_SHADER_PARMS; i++ )
	{
		ReadFloat( renderEnvprobe.shaderParms[ i ] );
	}
}
// RB end

/*
================
idRestoreGame::ReadRefSound
================
*/
void idRestoreGame::ReadRefSound( refSound_t& refSound )
{
	int		index;
	ReadInt( index );

	refSound.referenceSound = gameSoundWorld->EmitterForIndex( index );
	ReadVec3( refSound.origin );
	ReadInt( refSound.listenerId );
	ReadSoundShader( refSound.shader );
	ReadFloat( refSound.diversity );
	ReadBool( refSound.waitfortrigger );

	ReadFloat( refSound.parms.minDistance );
	ReadFloat( refSound.parms.maxDistance );
	ReadFloat( refSound.parms.volume );
	ReadFloat( refSound.parms.shakes );
	ReadInt( refSound.parms.soundShaderFlags );
	ReadInt( refSound.parms.soundClass );
}

/*
================
idRestoreGame::ReadRenderView
================
*/
void idRestoreGame::ReadRenderView( renderView_t& view )
{
	int i;

	ReadInt( view.viewID );
	ReadInt( i /* view.x */ );
	ReadInt( i /* view.y */ );
	ReadInt( i /* view.width */ );
	ReadInt( i /* view.height */ );

	ReadFloat( view.fov_x );
	ReadFloat( view.fov_y );
	ReadVec3( view.vieworg );
	ReadMat3( view.viewaxis );

	ReadBool( view.cramZNear );

	ReadInt( view.time[0] );

	for( i = 0; i < MAX_GLOBAL_SHADER_PARMS; i++ )
	{
		ReadFloat( view.shaderParms[ i ] );
	}
}

/*
=================
idRestoreGame::ReadUsercmd
=================
*/
void idRestoreGame::ReadUsercmd( usercmd_t& usercmd )
{
	//puncho start - stream-trace (P1-A1c, K3): the struct scope WriteUsercmd opened
	if( streamTrace != NULL && streamTrace->EnterStruct( "usercmd_t" ) )
	{
		ReadUsercmd( usercmd );
		streamTrace->EndScope();
		return;
	}
	//puncho end

	ReadByte( usercmd.buttons );
	ReadSignedChar( usercmd.forwardmove );
	ReadSignedChar( usercmd.rightmove );
	ReadShort( usercmd.angles[0] );
	ReadShort( usercmd.angles[1] );
	ReadShort( usercmd.angles[2] );
	ReadShort( usercmd.mx );
	ReadShort( usercmd.my );
	ReadByte( usercmd.impulse );
	ReadByte( usercmd.impulseSequence );
}

/*
===================
idRestoreGame::ReadContactInfo
===================
*/
void idRestoreGame::ReadContactInfo( contactInfo_t& contactInfo )
{
	ReadInt( ( int& )contactInfo.type );
	ReadVec3( contactInfo.point );
	ReadVec3( contactInfo.normal );
	ReadFloat( contactInfo.dist );
	ReadInt( contactInfo.contents );
	ReadMaterial( contactInfo.material );
	ReadInt( contactInfo.modelFeature );
	ReadInt( contactInfo.trmFeature );
	ReadInt( contactInfo.entityNum );
	ReadInt( contactInfo.id );
}

/*
===================
idRestoreGame::ReadTrace
===================
*/
void idRestoreGame::ReadTrace( trace_t& trace )
{
	ReadFloat( trace.fraction );
	ReadVec3( trace.endpos );
	ReadMat3( trace.endAxis );
	ReadContactInfo( trace.c );
}

/*
 ===================
 idRestoreGame::ReadTraceModel
 ===================
 */
void idRestoreGame::ReadTraceModel( idTraceModel& trace )
{
	int j, k;

	ReadInt( ( int& )trace.type );
	ReadInt( trace.numVerts );
	for( j = 0; j < MAX_TRACEMODEL_VERTS; j++ )
	{
		ReadVec3( trace.verts[j] );
	}
	ReadInt( trace.numEdges );
	for( j = 0; j < ( MAX_TRACEMODEL_EDGES + 1 ); j++ )
	{
		ReadInt( trace.edges[j].v[0] );
		ReadInt( trace.edges[j].v[1] );
		ReadVec3( trace.edges[j].normal );
	}
	ReadInt( trace.numPolys );
	for( j = 0; j < MAX_TRACEMODEL_POLYS; j++ )
	{
		ReadVec3( trace.polys[j].normal );
		ReadFloat( trace.polys[j].dist );
		ReadBounds( trace.polys[j].bounds );
		ReadInt( trace.polys[j].numEdges );
		for( k = 0; k < MAX_TRACEMODEL_POLYEDGES; k++ )
		{
			ReadInt( trace.polys[j].edges[k] );
		}
	}
	ReadVec3( trace.offset );
	ReadBounds( trace.bounds );
	ReadBool( trace.isConvex );
	// padding win32 native structs
	char tmp[3];
	file->Read( tmp, 3 );
	//puncho start - stream-trace (P1-A1c, K1): the padding bypasses the primitives, one OPAQUE entry
	if( streamTrace != NULL )
	{
		streamTrace->Primitive( SP_OPAQUE, sizeof( tmp ), 0 );
	}
	//puncho end
}

/*
=====================
idRestoreGame::ReadClipModel
=====================
*/
void idRestoreGame::ReadClipModel( idClipModel*& clipModel )
{
	bool restoreClipModel;

	ReadBool( restoreClipModel );
	if( restoreClipModel )
	{
		clipModel = new( TAG_SAVEGAMES ) idClipModel();
		//puncho start - stream-trace (P1-A1c, K3): the scope WriteClipModel opened
		if( streamTrace != NULL )
		{
			streamTrace->BeginScope( "idClipModel" );
			clipModel->Restore( this );
			streamTrace->EndScope();
		}
		else
		{
			clipModel->Restore( this );
		}
		//puncho end
	}
	else
	{
		clipModel = NULL;
	}
}

/*
=====================
idRestoreGame::ReadSoundCommands
=====================
*/
void idRestoreGame::ReadSoundCommands()
{
	gameSoundWorld->StopAllSounds();

	//puncho start - stream-trace (P1-A1c, K1): the sound world bypasses the primitives, one OPAQUE entry in <sound>
	if( streamTrace != NULL )
	{
		streamTrace->BeginScope( "<sound>" );
		const int start = streamTrace->Tell();
		gameSoundWorld->ReadFromSaveGame( file );
		streamTrace->Opaque( start );
		streamTrace->EndScope();
		return;
	}
	//puncho end

	gameSoundWorld->ReadFromSaveGame( file );
}
