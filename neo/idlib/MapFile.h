/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2015-2023 Robert Beckebans

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

#ifndef __MAPFILE_H__
#define __MAPFILE_H__
#include "gltfProperties.h"

/*
===============================================================================

	Reads or writes the contents of .map files into a standard internal
	format, which can then be moved into private formats for collision
	detection, map processing, or editor use.

	No validation (duplicate planes, null area brushes, etc) is performed.
	There are no limits to the number of any of the elements in maps.
	The order of entities, brushes, and sides is maintained.

===============================================================================
*/

const int OLD_MAP_VERSION					= 1;
// RB: added new map format
const int DOOM3_MAP_VERSION					= 2;
const int CURRENT_MAP_VERSION				= 3;
// RB end
const int DEFAULT_CURVE_SUBDIVISION			= 4;
const float DEFAULT_CURVE_MAX_ERROR			= 4.0f;
const float DEFAULT_CURVE_MAX_ERROR_CD		= 24.0f;
const float DEFAULT_CURVE_MAX_LENGTH		= -1.0f;
const float DEFAULT_CURVE_MAX_LENGTH_CD		= -1.0f;


class idMapPrimitive
{
public:
	// RB: new mesh primitive to work with Blender Ngons
	enum { TYPE_INVALID = -1, TYPE_BRUSH, TYPE_PATCH, TYPE_MESH };
	// RB end

	idDict					epairs;

	idMapPrimitive()
	{
		type = TYPE_INVALID;
	}
	virtual					~idMapPrimitive() { }
	int						GetType() const
	{
		return type;
	}

protected:
	int						type;
};


class idMapBrushSide
{
	friend class idMapBrush;

public:
	idMapBrushSide();
	~idMapBrushSide() { }

	const char* 			GetMaterial() const
	{
		return material;
	}
	void					SetMaterial( const char* p )
	{
		material = p;
	}
	const idPlane& 			GetPlane() const
	{
		return plane;
	}
	void					SetPlane( const idPlane& p )
	{
		plane = p;
	}
	void					SetTextureMatrix( const idVec3 mat[2] )
	{
		texMat[0] = mat[0];
		texMat[1] = mat[1];
	}
	void					GetTextureMatrix( idVec3& mat1, idVec3& mat2 )
	{
		mat1 = texMat[0];
		mat2 = texMat[1];
	}
	void					GetTextureVectors( idVec4 v[2] ) const;

	// RB: support Valve 220 projection by TrenchBroom
	enum ProjectionType
	{
		PROJECTION_BP		= 0,
		PROJECTION_VALVE220	= 1
	};

	ProjectionType			GetProjectionType() const
	{
		return projection;
	}

	const idVec2i&			GetTextureSize() const
	{
		return texSize;
	}

	void					ConvertToValve220Format( const idMat4& entityTransform, idStrList& textureCollections );
	// RB end

protected:
	idStr					material;
	idPlane					plane;
	idVec3					texMat[2];
	idVec3					origin;

public:
	// RB
	idVec3					planepts[ 3 ]; // for writing back original planepts
	ProjectionType			projection;
	idVec4					texValve[ 2 ]; // alternative texture coordinate mapping
	idVec2					texScale;
	idVec2i					texSize;

};

ID_INLINE idMapBrushSide::idMapBrushSide()
{
	plane.Zero();
	texMat[0].Zero();
	texMat[1].Zero();
	origin.Zero();

	projection = PROJECTION_BP;
	texValve[0].Zero();
	texValve[1].Zero();
	texScale[0] = 1.0f;
	texScale[1] = 1.0f;
	texSize[0] = 32;
	texSize[1] = 32;
}


class idMapBrush : public idMapPrimitive
{
public:
	idMapBrush()
	{
		type = TYPE_BRUSH;
		sides.Resize( 8, 4 );
	}
	~idMapBrush()
	{
		sides.DeleteContents( true );
	}
	static idMapBrush* 		Parse( idLexer& src, const idVec3& origin, bool newFormat = true, int version = CURRENT_MAP_VERSION );
	static idMapBrush* 		ParseQ3( idLexer& src, const idVec3& origin );
	static idMapBrush* 		ParseValve220( idLexer& src, const idVec3& origin ); // RB
	bool					Write( idFile* fp, int primitiveNum, const idVec3& origin ) const;
	bool					WriteValve220( idFile* fp, int primitiveNum, const idVec3& origin ) const; // RB

	// returns an origin brush with the size of (2, 2, 2) by default
	// so we can center the brush on a grid size of 1 in TrenchBroom
	static idMapBrush*		MakeOriginBrush( const idVec3& origin, const idVec3& scale = vec3_one );

	void					SetPlanePointsFromWindings( const idVec3& origin, int entityNum, int primitiveNum );

	int						GetNumSides() const
	{
		return sides.Num();
	}
	int						AddSide( idMapBrushSide* side )
	{
		return sides.Append( side );
	}
	idMapBrushSide* 		GetSide( int i ) const
	{
		return sides[i];
	}
	unsigned int			GetGeometryCRC() const;

	bool					IsOriginBrush() const;

protected:
	int						numSides;
	idList<idMapBrushSide*, TAG_IDLIB_LIST_MAP> sides;
};


class idMapPatch : public idMapPrimitive, public idSurface_Patch
{
public:
	idMapPatch();
	idMapPatch( int maxPatchWidth, int maxPatchHeight );
	~idMapPatch() { }
	static idMapPatch* 		Parse( idLexer& src, const idVec3& origin, bool patchDef3, int version );
	bool					Write( idFile* fp, int primitiveNum, const idVec3& origin ) const;
	const char* 			GetMaterial() const
	{
		return material;
	}
	void					SetMaterial( const char* p )
	{
		material = p;
	}
	int						GetHorzSubdivisions() const
	{
		return horzSubdivisions;
	}
	int						GetVertSubdivisions() const
	{
		return vertSubdivisions;
	}
	bool					GetExplicitlySubdivided() const
	{
		return explicitSubdivisions;
	}
	void					SetHorzSubdivisions( int n )
	{
		horzSubdivisions = n;
	}
	void					SetVertSubdivisions( int n )
	{
		vertSubdivisions = n;
	}
	void					SetExplicitlySubdivided( bool b )
	{
		explicitSubdivisions = b;
	}
	unsigned int			GetGeometryCRC() const;

protected:
	idStr					material;
	int						horzSubdivisions;
	int						vertSubdivisions;
	bool					explicitSubdivisions;
};

ID_INLINE idMapPatch::idMapPatch()
{
	type = TYPE_PATCH;
	horzSubdivisions = vertSubdivisions = 0;
	explicitSubdivisions = false;
	width = height = 0;
	maxWidth = maxHeight = 0;
	expanded = false;
}

ID_INLINE idMapPatch::idMapPatch( int maxPatchWidth, int maxPatchHeight )
{
	type = TYPE_PATCH;
	horzSubdivisions = vertSubdivisions = 0;
	explicitSubdivisions = false;
	width = height = 0;
	maxWidth = maxPatchWidth;
	maxHeight = maxPatchHeight;
	verts.SetNum( maxWidth * maxHeight );
	expanded = false;
}


// RB begin
class MapPolygon
{
	friend class MapPolygonMesh;

public:
	MapPolygon();
	MapPolygon( int numIndexes );
	~MapPolygon() { }

	const char* 			GetMaterial() const
	{
		return material;
	}

	void					SetMaterial( const char* p )
	{
		material = p;
	}

	void					AddIndex( int index )
	{
		indexes.Append( index );
	}

	void					SetIndexes( const idTempArray<int>& _indexes )
	{
		indexes.Resize( _indexes.Num() );

		for( unsigned int i = 0; i < _indexes.Num(); i++ )
		{
			indexes[i] = _indexes[i];
		}
	}

	const idList<int>&		GetIndexes() const
	{
		return indexes;
	}


protected:
	idStr					material;
	idList<int>				indexes;		// [3..n] references to vertices for each face
};

ID_INLINE MapPolygon::MapPolygon()
{
}

ID_INLINE MapPolygon::MapPolygon( int numIndexes )
{
	//indexes.AssureSize( 3 );
}


class MapPolygonMesh : public idMapPrimitive
{
public:
	MapPolygonMesh();
	~MapPolygonMesh()
	{
		//verts.DeleteContents();
		//polygons.DeleteContents( true );
	}

	void					ConvertFromBrush( const idMapBrush* brush, int entityNum, int primitiveNum );
	void					ConvertFromPatch( const idMapPatch* patch, int entityNum, int primitiveNum );
	static MapPolygonMesh*	ConvertFromMeshGltf( const gltfMesh_Primitive* prim, gltfData* _data, const idMat4& transform );
	static MapPolygonMesh*	Parse( idLexer& src, const idVec3& origin, float version = CURRENT_MAP_VERSION );
	bool					Write( idFile* fp, int primitiveNum, const idVec3& origin ) const;

	static MapPolygonMesh*	ParseJSON( idLexer& src );
	bool					WriteJSON( idFile* fp, int primitiveNum, const idVec3& origin ) const;



	int						GetNumVertices() const
	{
		return verts.Num();
	}

	int						AddVertex( const idDrawVert& v )
	{
		return verts.Append( v );
	}

	int						AddVertices( const idList<idDrawVert>& v )
	{
		return verts.Append( v );
	}

	int						GetNumPolygons() const
	{
		return polygons.Num();
	}

	//int						AddPolygon( MapPolygon* face )
	//{
	//	return polygons.Append( face );
	//}

	const MapPolygon& 			GetFace( int i ) const
	{
		return polygons[i];
	}

	unsigned int			GetGeometryCRC() const;

	const idList<idDrawVert>&	GetDrawVerts() const
	{
		return verts;
	}

	bool					IsOpaque() const
	{
		return opaque;
	}

	bool					IsAreaportal() const;

	void					GetBounds( idBounds& bounds ) const;

private:
	void					SetContents();

	int						originalType;

protected:

	idList<idDrawVert>		verts;			// vertices can be shared between polygons
	idList<MapPolygon>		polygons;

	// derived data after parsing

	// material surface flags
	int						contents;
	bool					opaque;
};
// RB end




class idMapEntity
{
	friend class			idMapFile;

public:
	typedef idList<idMapEntity*, TAG_IDLIB_LIST_MAP>  EntityList;
	typedef idList<idMapEntity*, TAG_IDLIB_LIST_MAP>& EntityListRef;
	typedef idList<idMapEntity*, TAG_IDLIB_LIST_MAP>* EntityListPtr;

	idDict					epairs;
	idVec3					originOffset{ vec3_origin };

public:
	idMapEntity()
	{
		epairs.SetHashSize( 64 );
	}
	~idMapEntity()
	{
		primitives.DeleteContents( true );
	}
	// HVG check gltf scene for entities
	static int				GetEntities( gltfData* data, EntityListRef entities, int scene = 0 );
	static idMapEntity* 	Parse( idLexer& src, bool worldSpawn = false, int version = CURRENT_MAP_VERSION );
	bool					Write( idFile* fp, int entityNum, bool valve220 ) const;

	// HVG NOTE: this is not compatible with gltf (extra) json!
	// RB begin
	static idMapEntity* 	ParseJSON( idLexer& src );
	bool					WriteJSON( idFile* fp, int entityNum, int numEntities ) const;
	// RB end
	int						GetNumPrimitives() const
	{
		return primitives.Num();
	}
	idMapPrimitive* 		GetPrimitive( int i ) const
	{
		return primitives[i];
	}
	void					AddPrimitive( idMapPrimitive* p )
	{
		primitives.Append( p );
	}
	unsigned int			GetGeometryCRC() const;
	void					RemovePrimitiveData();

protected:
	void					CalculateBrushOrigin();

	idList<idMapPrimitive*, TAG_IDLIB_LIST_MAP>	primitives;
};


//puncho start - P1-m3: what one map patch entity did to the merged map, mechanism only; policy over the records, such
// as the use_aas advice of idGameLocal::SpawnMapEntities, is the caller's
enum mapPatchResult_t
{
	MAP_PATCH_MERGED,		// set keys on the map entity of its name
	MAP_PATCH_APPENDED,		// a new entity: none of its name, or it has none
	MAP_PATCH_SKIPPED		// a worldspawn patch, which a patch cannot address
};

struct mapPatchEntity_t
{
	int						entity;		// the merged map's entity index, -1 when skipped
	int						layer;		// the patch's layer rank, -1 for an os path
	idStr					file;		// the patch file
	int						line;		// the line the patch entity starts on
	mapPatchResult_t		result;
};
//puncho end

class idMapFile
{
public:
	idMapFile();
	~idMapFile()
	{
		entities.DeleteContents( true );
	}

	// filename does not require an extension
	// normally this will use a .reg file instead of a .map file if it exists,
	// which is what the game and dmap want, but the editor will want to always
	// load a .map file
	//puncho - MP-A5 (K41): unless ignoreExtraEnts, each layer's <map>_extra_ents.map (or .json) merges in, from the layer
	// that serves the map up; patchLayerLimit stops below that rank (-1 for every layer), which K41a's writers diff
	// against; layerEntitiesCRCs fills GetLayerEntitiesCRC, which only multiplayer's content check reads
	bool					Parse( const char* filename, bool ignoreRegion = false, bool osPath = false, bool ignoreExtraEnts = false, int patchLayerLimit = -1, bool layerEntitiesCRCs = false );
	bool					Write( const char* fileName, const char* ext, bool fromBasePath = true );

	// RB begin
	bool					WriteJSON( const char* fileName, const char* ext, bool fromBasePath = true );
	bool					WriteDiff( const idMapFile* other, const char* fileName, const char* ext, bool fromBasePath = true );
	bool					ConvertToPolygonMeshFormat();
	bool					ConvertToValve220Format( bool recalcPlanePoints );

	void					ClassifyEntitiesForTrenchBroom( idDict& classTypeOverview );

	// converts Wad texture names to valid Doom 3 materials and gives every entity a unique name
	bool					ConvertQuakeToDoom();
	void					FixDuplicatedNamesInGroupInstances();
	// RB end

	// get the number of entities in the map
	int						GetNumEntities() const
	{
		return entities.Num();
	}
	// get the specified entity
	idMapEntity* 			GetEntity( int i ) const
	{
		return entities[i];
	}
	// get the name without file extension
	const char* 			GetName() const
	{
		return name;
	}
	// get the file time
	ID_TIME_T					GetFileTime() const
	{
		return fileTime;
	}
	// get CRC for the map geometry
	// texture coordinates and entity key/value pairs are not taken into account
	unsigned int			GetGeometryCRC() const
	{
		return geometryCRC;
	}
	unsigned int			GetEntityCRC( int ordinal ) const;
	unsigned int			GetEntitiesCRC() const;	//puncho - content check (P1-A3a, K37): CRC of every entity's key/values, '\r' dropped
	//puncho - content check (P1-A3b, K38): one layer's part of the merged entities, the GetEntitiesCRC of the map file the
	// layer serves and of the layer's _extra_ents.map, each as parsed before the merge; 0 when Parse merged no patches
	unsigned int			GetLayerEntitiesCRC( int layer ) const;
	// returns true if the file on disk changed
	bool					NeedsReload();

	int						AddEntity( idMapEntity* mapentity );
	idMapEntity* 			FindEntity( const char* name ) const;
	idMapEntity*			FindEntityAtOrigin( const idVec3& org ) const; // RB
	void					RemoveEntity( idMapEntity* mapEnt );
	void					RemoveEntities( const char* classname );
	void					RemoveAllEntities();
	void					RemovePrimitiveData();
	bool					HasPrimitiveData()
	{
		return hasPrimitiveData;
	}

	bool					IsGLTF() const
	{
		return gltfFormat;
	}

	static void				AddMaterialToCollection( const char* material, idStrList& textureCollections );
	static void				WadTextureToMaterial( const char* material, idStr& matName );

	//puncho start - MP-A5 (K41a, K41b)
	// the in-engine writers of the level map: with one layer the map is written whole (a glTF map as a patch); with
	// layers, while a patch is in play the edits go into the write target's <map>_extra_ents.map as a diff against the
	// map merged through the layers below it, and otherwise the map, or a new map name, is written whole
	bool					WriteLevelEdits( const char* fileName );
	// a whole-map write bakes no patch in: it is refused while any layer other than exceptLayer holds a patch for
	// mapName, naming the layers
	static bool				RefuseWholeMapWrite( const char* mapName, int exceptLayer = -1 );
	// the last merge's log: each patch entity appended or skipped, with file:line (P1-m3: a member, no longer global)
	const idStrList&		GetPatchLog() const
	{
		return patchLog;
	}
	// the last merge's records, one per patch entity in merge order (P1-m3)
	const idList<mapPatchEntity_t>&	GetPatchEntities() const
	{
		return patchEntities;
	}
	//puncho end

protected:
	int						version;
	ID_TIME_T					fileTime;
	unsigned int			geometryCRC;
	idMapEntity::EntityList	entities;
	idStr					name;
	bool					hasPrimitiveData;
	bool					valve220Format;	// RB: for TrenchBroom support
	bool					gltfFormat;
	//puncho start - MP-A5 (K41, K41b)
	idStr					sourceName;			// the file Parse loaded, with its extension
	int						readLayer;			// a patch reads this layer's own copy; -1 reads the served copy
	idList<int>				entityLines;		// a patch's entity start lines, for the merge log
	idStrList				patchLog;			// P1-m3: the last merge's log, GetPatchLog
	idList<mapPatchEntity_t>	patchEntities;	// P1-m3: the last merge's records, GetPatchEntities
	//puncho end
	idList<unsigned int>	layerEntitiesCRC;	//puncho - content check (P1-A3b, K38): GetLayerEntitiesCRC per layer rank

private:
	void					SetGeometryCRC();
	bool					LoadLayerSource( idLexer& src, const char* fileName, idList<char>& text );	//puncho - MP-A5 (K41)
	void					MergePatch( const idMapFile& patch, int layer );						//puncho - MP-A5 (K41, K41b)
	const char*				GetUniqueEntityName( const char* classname ) const; // RB
};

ID_INLINE idMapFile::idMapFile()
{
	version = CURRENT_MAP_VERSION;
	fileTime = 0;
	geometryCRC = 0;
	entities.Resize( 1024, 256 );
	hasPrimitiveData = false;
	valve220Format = false;
	gltfFormat = false;
	readLayer = -1;		//puncho - MP-A5 (K41)
}

#endif /* !__MAPFILE_H__ */
